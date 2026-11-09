#pragma once

// The renderer (docs/lift-20261006/R_DESIGN.md 2, R2_DRIVER.md 2.2): owns the device, every GPU resource, the
// pipeline states and the frame loop, and turns FramePackets into frames. Resource creation and release are safe
// from any thread (the game thread loads while the render thread records); frames are rendered by one thread.
//
//   * Textures and meshes are created from importer data (assets/) inside a lifetime scope and named by
//     generational handles; releasing a scope retires its resources behind the GPU.
//   * A frame: per view, a multisampled scene colour + depth at the view's share of the output resolution; the
//     view's passes in order; a resolve; the views composed into the output - the window's back buffer, or an image
//     the caller reads back (tests, captures).
//   * Pipeline states are created on first use and cached by their key (the build-time enumeration of R2c replaces
//     first-use creation).
//
// Threads and locks (ES 2026-10-19, closing D11 of docs/mesh-re-20261007/INDEX.md): the game thread never waits on
// rendering. Two locks, never nested the other way round (render -> table):
//   * table_mutex_ - the handle tables, their free lists, the deferred releases and the resource statistics. Held
//     for bookkeeping only: nothing waits on the GPU or records a frame while holding it.
//   * render_mutex_ - what only the rendering thread uses (frame loop, swap chain, pipelines, pools, histories, skin
//     cache). The game thread never takes it, so the fence wait in FrameLoop::begin and the recording of a frame
//     no longer hold up a texture or mesh the game creates or releases.
// Recording reads the slots of the handles its packet names without the table lock: slot storage never moves (a
// table of fixed chunks), a slot is allocated before the packet that names it is handed over (the packet queue's lock
// orders the two), its resource is published by its job with the slot's state (Ready, release order), and its
// release takes effect only after that packet is finished (finish_packets).
// The resource worker's execution lock (resource_worker.h) is taken before the table lock when a job installs its
// resource; nothing that holds the table lock waits for a job. Full order: render -> worker execution -> table.

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

#include "assets/geometry_import.h"
#include "assets/texture_import.h"
#include "render/descriptors.h"
#include "render/frame.h"
#include "render/frame_graph.h"
#include "render/frame_loop.h"
#include "render/burst_ahead.h"
#include "render/gpu_device.h"
#include "render/gpu_memory.h"
#include "render/gpu_timer.h"
#include "render/jit_pacer.h"
#include "render/packet_stream.h"
#include "render/resource_worker.h"
#include "render/resources.h"
#include "render/skin_cache.h"
#include "render/swap_chain.h"

namespace shadow::render {

struct RendererOptions {
    DeviceOptions device;
    std::uint32_t msaa_samples = 4;
    DXGI_FORMAT scene_format = DXGI_FORMAT_R16G16B16A16_UNORM;  // R_DESIGN 2.5: no 8-bit banding in the scene
    std::uint32_t frames_in_flight = 2;
    std::uint32_t max_anisotropy = 16;
    // The skin cache's starting region per frame slot (G12: it grows to the frame's need, at least doubling).
    std::uint32_t skinned_vertices_per_frame = 16u << 10;
    // Diagnostics: the glare composite replaces the scene instead of adding to it (the picture is the glare alone),
    // and of the glare only its sources (1) or only its levels (2).
    bool debug_glare_only = false;
    std::uint32_t debug_glare_parts = 0;
    // Diagnostics: the glare sources bind the scene's depth through its writable view (no draw writes it).
    bool debug_writable_source_depth = false;
};

class Renderer {
public:
    Renderer() = default;
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    ~Renderer();

    bool create(const RendererOptions& options, std::string* error);
    void destroy();

    // ---- resources ------------------------------------------------------------------------------------------
    // Asking for a resource returns its handle at once; making it is a job for the resource worker
    // (resource_worker.h): the texture's decode and mip chain, the mesh's vertex streams, the GPU resources and their
    // uploads. Handles are allocated by the asking thread in the order of the asks - the values are those the
    // synchronous path gave. A packet that names a handle draws its resource: before recording a draw of one still
    // being made, the rendering thread waits for it (running the queued jobs itself), so the first frame that uses a
    // resource shows it, as on the console. A resource the job fails to make (a GPU allocation failure) draws as a
    // stale handle does - skipped - and is counted.
    //
    // A texture from a stored record: `texels` is the caller's own copy of the stored bytes (the record's view is
    // replaced by it), so the caller's memory may be reused as soon as this returns.
    struct TextureSource {
        assets::TextureRecord record;
        std::vector<std::uint8_t> texels;
        assets::ImportOptions options;
    };
    TextureHandle request_texture(Scope scope, TextureSource source, std::string* error);
    MeshHandle request_mesh(Scope scope, assets::MeshData mesh, std::string* error);
    // The same, made before returning (tools, tests): an empty handle and `error` when it could not be made.
    TextureHandle create_texture(Scope scope, const assets::TextureImage& image, std::string* error);
    MeshHandle create_mesh(Scope scope, const assets::MeshData& mesh, std::string* error);
    // A handle's lifetime ends with the last packet that names it. The game builds packets ahead of the frames that
    // record them (a window records the newest packet after its back buffer is free), so a release names the last
    // packet tick that may still use the handle - normally the packet being built - and the handle stays valid until
    // every packet up to that tick has been recorded or discarded (finish_packets). kNoPacket: no packet names it.
    static constexpr std::uint64_t kNoPacket = ~std::uint64_t{0};
    void release(TextureHandle texture, std::uint64_t named_through = kNoPacket);
    void release(MeshHandle mesh, std::uint64_t named_through = kNoPacket);
    // Every packet with a tick up to `tick` has been recorded or will never be: their releases take effect. Called by
    // whoever hands packets over (after recording one, or after the headless path saw it).
    void finish_packets(std::uint64_t tick);
    // Releases every resource of the scope (stage end, event end).
    void release_scope(Scope scope);

    // ---- output -----------------------------------------------------------------------------------------------
    bool attach_window(HWND window, std::uint32_t width, std::uint32_t height, std::string* error);
    bool resize_window(std::uint32_t width, std::uint32_t height, std::string* error);
    // Takes the swap chain's frame-latency credit now (waits for it) so that the caller can choose the packet only
    // once a back buffer may be queued: the newest packet at that moment, not one taken a refresh earlier
    // (docs/TIMING_ARCHITECTURE.md finding 3). The credit is held until the next render(); without it render() waits.
    // False: the wait ended without a credit (timed out or failed); the frame is still recorded and presented, and the
    // caller must not treat now as the credit's time.
    bool acquire_frame();
    // SHADOW_R2_JIT (render/jit_pacer.h): after acquire_frame, before the caller picks its packet - waits until the
    // frame should start so that it is done shortly before the vblank it is shown at. VSync presents only; without
    // the switch, a window or a usable vblank grid it returns at once. Returns the planned start (steady ns), 0 when
    // none was planned: the phase lock measures its hold against it.
    // fill_mpx: the packet's estimated particle fill (particle_fill_mpx), used by SHADOW_R2_JIT_ASSIST=predict.
    std::int64_t jit_wait(bool vsync, double fill_mpx = 0.0);
    // The packet's estimated particle fill in megapixels at the render size (render/particle_fill.h; CPU only).
    double particle_fill_mpx(const FramePacket& packet) const;
    // SHADOW_R2_JIT_ASSIST=predict: the fill of the packet actually recorded, when it is not the one jit_wait was given
    // (a newer packet pushed during the wait) - the pacer's model learns from the frame it measures (rendering thread).
    void set_jit_fill_mpx(double fill_mpx) noexcept { jit_fill_mpx_ = fill_mpx; }
    // SHADOW_R2_JIT: the measured refresh period (rendering thread).
    std::int64_t jit_period_ns() const noexcept { return jit_.period_ns(); }
    // render/burst_ahead.h: true for a few frames after the frames-in-flight mode switched - their holds measure the
    // switch, not the game's phase, and the phase lock leaves them out (rendering thread).
    bool burst_settling() const noexcept { return burst_settle_ > 0u; }
    // Renders the packet into the window's next back buffer and presents it. credit_ns given: eager start
    // (render/burst_ahead.h) - recorded and submitted before the swap chain's credit is taken; *credit_ns = when it came.
    bool render(const FramePacket& packet, PresentMode mode, std::string* error, std::int64_t* credit_ns = nullptr);
    // How much earlier than the phase lock's schedule packets come now (burst lead), for the hold the lock measures.
    std::int64_t hold_bias_ns() const noexcept;
    // C3 (HW_REPORT S): true while render / render_to_image reads `packet` (any thread). A packet handed back to the
    // frame builder (FrameBuilder::recycle, its vectors refilled by the next frame) must not be one read here.
    bool reading(const FramePacket& packet) const noexcept {
        return reading_.load(std::memory_order_acquire) == &packet;
    }
    // G10 (HW_REPORT N.2): the size frames are rendered at (any thread; 0 x 0: the window's size, today's path). A
    // size other than the back buffer's renders the frame at that size and scales it into the back buffer (a 2 x 2
    // box at exactly 2:1, a bicubic otherwise, fitted with black bars when the aspects differ). The caller keeps its
    // aspect the game-visible output's (LiftedRender). Captures (render_to_image) keep their own size.
    void set_render_size(std::uint32_t width, std::uint32_t height) noexcept {
        render_size_.store((std::uint64_t{width} << 32) | height, std::memory_order_release);
    }
    // The picture's exact aspect (the game's viewport, gekko/display_geometry.h): where a scaled render size is placed
    // in the back buffer - the largest rectangle of it, centred, bars outside. 0: the render size's own aspect.
    void set_present_aspect(double aspect) noexcept { present_aspect_.store(aspect, std::memory_order_release); }
    // Image quality, any thread, taken at the start of the next frame (no swap chain is touched):
    // MSAA - the sample count of the scene's colour and depth targets (1 = single-sample rendering, no resolve work).
    //   A count the GPU does not support for both formats is refused: the count in use stays, msaa_note() says why.
    // Anisotropic filtering - the level of the game's filtered textures (1 = Off: the console's linear filter; 2..16);
    //   the sampler table is rewritten with the GPU idle. Screen copies, effects and point textures keep theirs.
    void set_msaa_samples(std::uint32_t samples) noexcept { msaa_request_.store(samples, std::memory_order_release); }
    void set_anisotropy(std::uint32_t level) noexcept { anisotropy_request_.store(level, std::memory_order_release); }
    std::uint32_t msaa_samples() const noexcept { return msaa_active_.load(std::memory_order_acquire); }
    std::uint32_t anisotropy() const noexcept { return anisotropy_active_.load(std::memory_order_acquire); }
    // Bit n set: 2^n samples work for the scene colour and depth formats (bit 0: single-sample, always).
    std::uint32_t msaa_supported_mask() const noexcept { return msaa_supported_mask_; }
    bool msaa_supported(std::uint32_t samples) const noexcept;
    std::string msaa_note() const;
    // G10's proof hook: captures rendered at the render size and scaled into their own size by the same pass.
    void set_capture_at_render_size(bool on) noexcept { capture_at_render_size_.store(on, std::memory_order_release); }
    // Renders the packet at width x height and returns its pixels (RGBA8, rows top first); with `rgba` null nothing is
    // read back (a frame rendered only for what it leaves behind: the views' histories). `commit_history` false: a
    // capture of a frame that is ALSO presented (a windowed capture, rendered just before the present) - the frame's
    // own later draws see the history it saves, but the views' kept histories stay as they were, so the present that
    // follows renders the frame exactly as it would have without the capture.
    bool render_to_image(const FramePacket& packet, std::uint32_t width, std::uint32_t height,
                         std::vector<std::uint8_t>* rgba, std::string* error, bool commit_history = true);
    // Every view's kept history becomes invalid (drawn as missing until the view saves one again): a restored
    // savestate's frames do not continue this process's pictures.
    void invalidate_histories();

    // ---- introspection --------------------------------------------------------------------------------------------
    // Why a draw was left out. Every skip site in render_frame counts exactly one reason; the first skips of a run
    // are also logged (frame, reason, draw index).
    enum class SkipReason : std::uint8_t {
        ParticleBatch,      // a particle draw whose batch is missing, empty or outside the packet's records
        ParticlePipeline,   // no pipeline state for the particle recipe
        ParticleUpload,     // the frame's upload ring had no room for the records or constants
        ImmediateInvalid,   // an immediate draw that is missing, empty or outside the packet's vertices/indices
        ImmediatePipeline,  // no pipeline state for the immediate recipe
        ImmediateImage,     // its snapshot or history does not exist this frame
        ImmediateUpload,    // the upload ring had no room for its vertices, indices or constants
        MeshMissing,        // a stale mesh handle or a submesh it does not have
        SkinMissing,        // a skinned draw the skin cache holds no vertices for
        MeshPipeline,       // no pipeline state for a mesh program
        MeshUpload,         // the upload ring had no room for the draw constants
        Count
    };
    static const char* skip_reason_name(SkipReason reason) noexcept;
    static constexpr std::size_t kSkipReasons = static_cast<std::size_t>(SkipReason::Count);

    struct Stats {
        std::uint64_t frames = 0;
        std::uint64_t draws = 0;          // last frame
        std::uint64_t skipped_draws = 0;  // last frame: a stale handle or submesh, an unusable palette
        std::uint64_t skin_jobs = 0;      // last frame: meshes skinned
        std::uint64_t skinned_vertices = 0;
        std::uint64_t morph_jobs = 0;     // last frame: delta-morphed shapes blended (a skinned one counts in both)
        std::uint64_t skipped_draws_total = 0;  // all frames: draws not drawn (a stale handle, an unusable palette)
        std::array<std::uint64_t, kSkipReasons> skipped_by_reason{};  // all frames, per SkipReason
        std::size_t pool_textures = 0;    // now: render targets the frame graph keeps pooled
        std::uint64_t pool_created = 0;   // all frames: pooled render targets created
        std::uint64_t pool_released = 0;  // all frames: pooled render targets released after going unused
        std::uint64_t pool_memories = 0;        // shared render-target memories live (TexturePool::Memory)
        std::uint64_t pool_memory_bytes = 0;    // their bytes
        std::uint64_t pool_placed_bytes = 0;    // what the textures placed in them would take each on its own
        std::string pool_memory_map;            // TexturePool::memory_map (rebuilt when the pool changes)
        std::uint64_t deferred_releases = 0;    // all frames: releases that waited for packets naming the handle
        std::size_t pending_releases = 0;       // now: releases still waiting
        std::uint64_t particle_batches = 0;  // last frame
        std::uint64_t particles = 0;
        std::uint64_t glares = 0;            // last frame: glare composites
        std::uint64_t immediates = 0;        // last frame: immediate draws
        std::uint64_t snapshots = 0;         // last frame: scene snapshots taken
        std::uint64_t history_saves = 0;     // last frame: views whose history was saved
        std::uint64_t glare_composites_folded = 0;  // all frames: glare composites folded into the view's resolve
        std::uint64_t folded_glare_spans = 0;       // all frames: the tile rows those composites were still drawn on
        std::uint64_t image_misses = 0;      // all frames: draws of a snapshot or history that did not exist
        std::uint64_t views_over_views = 0;  // all frames: views started from the views under them (split screen)
        std::uint64_t views_folded = 0;      // all frames: of those, the view under resolved straight into them
        std::uint64_t skin_overflows = 0;  // all frames: skinned draws the cache had no room for
        std::size_t pipelines = 0;
        std::size_t textures = 0;
        std::size_t meshes = 0;
        std::size_t skinned_meshes = 0;
        std::size_t morphed_meshes = 0;
        std::uint64_t textures_requested = 0;  // all asks (request_texture + create_texture)
        std::uint64_t meshes_requested = 0;
        std::uint64_t resources_failed = 0;    // jobs that could not make their resource
        std::uint64_t resolve_waits = 0;       // all frames: draws whose resource was still being made when recorded
    };
    Stats stats() const {
        std::lock_guard<std::mutex> lock(table_mutex_);
        return stats_;
    }
    ResourceWorker::Stats worker_stats() const { return worker_.stats(); }
    Device& device() noexcept { return device_; }
    std::vector<std::string> take_debug_errors() { return device_.take_debug_errors(); }

private:
    // Slots that never move once made: `kChunk` slots per chunk, chunks made as the table grows. Readers index any
    // slot below size() without a lock (see the header comment); growth and every write happen under the table lock.
    // 2^24 - 1 slots at most: a handle keeps its slot + 1 in 24 bits.
    template <class Slot>
    class SlotTable {
    public:
        static constexpr std::uint32_t kChunkBits = 10;
        static constexpr std::uint32_t kChunk = 1u << kChunkBits;
        static constexpr std::uint32_t kMaxChunks = 1u << (24 - kChunkBits);
        static constexpr std::uint32_t kMaxSlots = kChunk * kMaxChunks - 1u;

        SlotTable() : chunks_(new std::unique_ptr<Slot[]>[kMaxChunks]) {}
        std::uint32_t size() const noexcept { return size_.load(std::memory_order_acquire); }
        Slot& operator[](std::uint32_t i) noexcept { return chunks_[i >> kChunkBits][i & (kChunk - 1u)]; }
        const Slot& operator[](std::uint32_t i) const noexcept { return chunks_[i >> kChunkBits][i & (kChunk - 1u)]; }
        // A new slot at the end; false when the 24-bit handle space is used up.
        bool grow(std::uint32_t* index) {
            const std::uint32_t n = size_.load(std::memory_order_relaxed);
            if (n >= kMaxSlots)
                return false;
            if ((n & (kChunk - 1u)) == 0u)
                chunks_[n >> kChunkBits].reset(new Slot[kChunk]);
            *index = n;
            size_.store(n + 1u, std::memory_order_release);
            return true;
        }
        void clear() {
            const std::uint32_t chunks = (size_.load(std::memory_order_relaxed) + kChunk - 1u) >> kChunkBits;
            for (std::uint32_t c = 0; c < chunks; ++c)
                chunks_[c].reset();
            size_.store(0, std::memory_order_release);
        }

    private:
        std::unique_ptr<std::unique_ptr<Slot[]>[]> chunks_;
        std::atomic<std::uint32_t> size_{0};
    };

    // A slot's resource: being made (Pending, by the job `ticket`), made (Ready) or not makeable (Failed). `live`: the
    // handle of this generation is current (asked for, not released). The job writes the resource and then the state
    // (release); a reader that sees Ready sees the resource.
    enum : std::uint8_t { kSlotEmpty = 0, kSlotPending = 1, kSlotReady = 2, kSlotFailed = 3 };
    struct TextureSlot {
        GpuTexture texture;
        Scope scope = Scope::Session;
        std::uint8_t generation = 1;
        bool live = false;
        std::atomic<std::uint8_t> state{kSlotEmpty};
        std::uint64_t ticket = 0;
        std::string failure;  // why the job could not make it (Failed)
    };
    struct MeshResources {
        // G13 (HW_REPORT P / R): the mesh's three streams in ONE buffer - one allocation instead of three, each of
        // which took at least the 64 KiB buffer placement: stream 0 position + normal at 0 (a skinned or morphed draw
        // reads it from the skin cache), stream 1 colour + UV0 + UV1 at attributes_offset, the 32-bit indices at
        // indices_offset.
        GpuBuffer streams;
        std::uint64_t attributes_offset = 0;
        std::uint64_t indices_offset = 0;
        std::uint64_t indices_bytes = 0;
        SkinSource skin;       // the base vertices the deformation pass reads; vertex_count 0: neither skinned nor morphed
        bool skinned = false;  // the base vertices carry bone weights
        MorphSource morph;     // delta targets; targets 0: not morphed
        std::uint32_t vertex_count = 0;
        std::vector<assets::MeshData::Submesh> submeshes;
        bool opaque_colours = false;  // every vertex colour's alpha is 255 (or no colours: the channel is opaque black)
    };
    struct MeshSlot : MeshResources {
        Scope scope = Scope::Session;
        std::uint8_t generation = 1;
        bool live = false;
        std::atomic<std::uint8_t> state{kSlotEmpty};
        std::uint64_t ticket = 0;
        std::string failure;
    };
    // Which shader pair: a mesh's base pass, its environment reflection, its second texture; a particle batch;
    // immediate geometry.
    enum class Program : std::uint8_t { Base, Environment, Dual, Particles, Immediate };
    // The full-screen passes of the glare (shaders/glare.hlsl; Composite adds into the scene) and the resolve of a
    // multisampled target (shaders/utility.hlsl); Under: an earlier view's picture copied into a view that draws over
    // the views before it (FramePacket ViewRecord::over_views).
    // ResolveGlare: the view's resolve with the glare composite folded in (B3a).
    // ResolveTiles: the glare sources' resolve on their marked tiles (VSGlareTiles + PSResolve, G2).
    enum class Screen : std::uint8_t {
        GlareCapture, GlareBlur, GlareBox, GlareMerge, GlareComposite, Resolve, DepthBits, Under, Scale, ResolveGlare, ResolveTiles
    };
    // The glare capture's compute steps (shaders/glare.hlsl CSGlareRows / CSGlareColumns, GLARE_OPTIMIZATION.md G1).
    enum class Compute : std::uint8_t { GlareRows, GlareColumns, GlareTilesClear };
    struct PipelineKey {
        std::uint8_t program, src, dst, cull;
        bool depth_test, depth_write;
        DXGI_FORMAT colour;
        std::uint32_t samples;
        bool lines;
        bool tiles;  // the GLARE_TILES pixel shader (glare_tiles.hlsli)
        bool plain;  // the blend unit off, colour channels only written (a blend proven to be the identity)
        bool no_depth;  // no depth target at all (the full-resolution HUD overlay draws onto the presented picture)
        bool operator<(const PipelineKey& o) const {
            return std::tie(program, src, dst, cull, depth_test, depth_write, colour, samples, lines, tiles, plain,
                            no_depth) <
                   std::tie(o.program, o.src, o.dst, o.cull, o.depth_test, o.depth_write, o.colour, o.samples,
                            o.lines, o.tiles, o.plain, o.no_depth);
        }
    };
    // A view's history (R2_DRIVER.md 4.9): its scene colour as the view last saved it, kept across frames; sized to
    // the view, recreated (and invalid until the next save) when the view's size changes.
    struct History {
        GpuTexture texture;
        D3D12_CPU_DESCRIPTOR_HANDLE rtv{};  // history_rtvs_: a save resolves straight into it
        bool initialised = false;           // its first frame discards it (FrameGraph discard_first)
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
        std::uint32_t width = 0, height = 0;
        bool valid = false;
    };
    struct Output {
        ID3D12Resource* resource = nullptr;  // imported (back buffer) or null (graph-owned capture image)
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
        D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
        DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
        std::uint32_t width = 0, height = 0;
        // G10: the imported resource's own size when the frame is rendered at width x height and scaled into it
        // (0: the resource is width x height, nothing is scaled).
        std::uint32_t present_width = 0, present_height = 0;
        // The shape the frame is placed in the resource with (0: width / height): the game's picture, the same on
        // every frame.
        double picture_aspect = 0.0;
    };

    void release_locked(TextureHandle texture);
    void release_locked(MeshHandle mesh);
    // Orders the direct queue (on the GPU) after every upload the jobs have submitted so far. The rendering thread,
    // before it submits a frame.
    void make_uploads_visible();
    // The slot a current handle names, whatever its state (bookkeeping, under the table lock).
    TextureSlot* texture_slot(TextureHandle handle);
    MeshSlot* mesh_slot(MeshHandle handle);
    // The rendering thread: the slot of a handle whose resource is made, waiting for its job first; null for a stale
    // handle or a resource that could not be made.
    const TextureSlot* drawable_texture(TextureHandle handle);
    const MeshSlot* drawable_mesh(MeshHandle handle);
    // Under the table lock: a slot for a new handle (the free list first, as always); false when none is left.
    bool allocate_texture_slot(Scope scope, std::uint32_t* slot, std::string* error);
    bool allocate_mesh_slot(Scope scope, std::uint32_t* slot, std::string* error);
    // Jobs (the resource worker's execution lock: the upload stream is theirs).
    bool make_texture(const assets::TextureImage& image, GpuTexture* out, std::string* error);
    bool make_mesh(const assets::MeshData& mesh, MeshResources* out, bool* morphed, std::string* error);
    void install_texture(std::uint32_t slot, std::uint8_t generation, bool made, GpuTexture texture,
                         const std::string& failure);
    void install_mesh(std::uint32_t slot, std::uint8_t generation, bool made, MeshResources resources, bool morphed,
                      const std::string& failure);
    // Submits the open upload list and publishes its copy fence (a job's last step before its resource is Ready).
    void publish_uploads();
    // `lines`: the pipeline draws line primitives (immediate geometry only). `tiles`: its pixel shader marks the glare
    // sources' tiles (the glare-sources pass, G2).
    ID3D12PipelineState* mesh_pipeline(Program program, const Recipe& recipe, DXGI_FORMAT colour, std::uint32_t samples,
                                       std::string* error, bool lines = false, bool tiles = false, bool plain = false,
                                       bool no_depth = false);
    ID3D12PipelineState* screen_pipeline(Screen screen, DXGI_FORMAT colour, std::uint32_t samples, std::string* error);
    ID3D12PipelineState* compute_pipeline(Compute compute, std::string* error);
    // Records the frame into `output` in frame slot `slot`; the caller submits. `capture` receives the readback copy
    // when not null.
    // `commit_history` false: the views' kept histories are read but never written (render_to_image).
    bool record(ID3D12GraphicsCommandList* list, std::uint32_t slot, const FramePacket& packet, const Output& output,
                D3D12_RESOURCE_STATES* output_state_after,
                GpuBuffer* capture, D3D12_PLACED_SUBRESOURCE_FOOTPRINT* capture_layout, std::string* error,
                bool commit_history = true);
    // GPU trace (GEKKO_GPU_TRACE, gpu_timer.h): the frame-end mark with the frame's figures. Only with gpu_timer_.
    void end_gpu_frame(ID3D12GraphicsCommandList* list);
    // GPU memory trace (GEKKO_GPU_MEMORY, gpu_memory.h): the renderer's counters for a row. Only with memory_trace_.
    GpuMemoryTrace::Counters memory_counters() const;

    RendererOptions options_;
    // C4 (HW_REPORT S): the frame graph is kept (reset each frame) so its pass / resource / transition vectors keep
    // their capacity instead of growing from nothing every frame.
    FrameGraph frame_graph_;
    Device device_;
    BindlessHeap heap_;
    SamplerHeap samplers_;
    TexturePool pool_;
    FrameLoop frames_;
    UploadStream uploads_;                             // the jobs' (resource worker execution lock)
    std::atomic<std::uint64_t> uploads_published_{0};  // copy fence after which every Ready resource is uploaded
    std::uint64_t uploads_visible_ = 0;                // the rendering thread's: the direct queue waits up to here
    Com<ID3D12RootSignature> root_;
    SkinCache skin_cache_;
    SwapChain swap_chain_;
    bool has_window_ = false;
    D3D12_RESOURCE_STATES back_buffer_states_[4] = {};
    SlotTable<TextureSlot> textures_;
    std::vector<std::uint32_t> free_textures_;
    SlotTable<MeshSlot> meshes_;
    std::vector<std::uint32_t> free_meshes_;
    std::map<PipelineKey, Com<ID3D12PipelineState>> pipelines_;
    std::map<std::tuple<Screen, DXGI_FORMAT, std::uint32_t>, Com<ID3D12PipelineState>> screen_pipelines_;
    std::map<Compute, Com<ID3D12PipelineState>> compute_pipelines_;
    std::vector<History> histories_;  // by view index
    CpuDescriptorPool history_rtvs_;   // the histories' render-target views
    // Releases waiting for the packets that may name their handle (release / finish_packets).
    struct PendingRelease {
        bool texture = false;
        std::uint32_t handle = 0;
        std::uint64_t named_through = 0;
    };
    std::vector<PendingRelease> pending_releases_;
    bool packets_finished_ = false;          // finish_packets was called at least once
    std::atomic<const FramePacket*> reading_{nullptr};  // C3: the packet render / render_to_image reads (reading())
    std::uint64_t finished_through_ = 0;     // every packet up to this tick is recorded or discarded
    Stats stats_;          // under table_mutex_: the resource counts here, the frame figures published per frame
    Stats render_stats_;   // the rendering thread's own frame figures (render_mutex_), copied into stats_ per frame
    // Copies the rendering thread's frame figures into stats_ (takes the table lock briefly).
    void publish_render_stats();
    ResourceWorker worker_;
    std::unique_ptr<GpuTimer> gpu_timer_;  // null unless GEKKO_GPU_TRACE is set (the rendering thread's)
    std::unique_ptr<GpuMemoryTrace> memory_trace_;  // null unless GEKKO_GPU_MEMORY is set (the rendering thread's)
    // The packet stream (render/packet_stream.h): null unless GEKKO_PACKET_RECORD is set; never made in shipping
    // builds (the hooks compile out there). Records what this renderer is asked, for another backend to replay.
    std::unique_ptr<PacketRecorder> recorder_;
    std::atomic<std::uint64_t> render_size_{0};      // G10: (width << 32) | height; 0: the window's size
    std::atomic<double> present_aspect_{0.0};         // the picture's aspect for the present scale; 0: the render's
    // Image quality (set_msaa_samples / set_anisotropy): requests from any thread, applied by apply_quality() under
    // render_mutex_ at the start of a frame; msaa_ is the count the frame being recorded uses.
    void apply_quality();
    std::atomic<std::uint32_t> msaa_request_{0};
    std::atomic<std::uint32_t> anisotropy_request_{0};
    std::atomic<std::uint32_t> msaa_active_{4};
    std::atomic<std::uint32_t> anisotropy_active_{16};
    std::uint32_t msaa_ = 4;
    std::uint32_t msaa_supported_mask_ = 1u;
    mutable std::mutex msaa_note_mutex_;
    std::string msaa_note_;
    std::atomic<bool> capture_at_render_size_{false};  // G10's proof hook (captures scaled like the present)
    bool created_ = false;
    // Resources are created and released on the game thread while frames record on the render thread
    // (R2_DRIVER.md 4.2). See the header comment: the table lock is bookkeeping only; the render lock is the
    // rendering thread's and is never taken by the game thread.
    mutable std::mutex table_mutex_;
    std::mutex render_mutex_;
    bool frame_acquired_ = false;  // render_mutex_: acquire_frame() took the latency credit; render() has not yet
    // SHADOW_R2_JIT (render/jit_pacer.h): the rendering thread's; started on the first jit_wait with the switch on.
    JitPacer jit_;
    // The direct resolve (SHADOW_R2_DIRECT_RESOLVE, unorm_calibration.cpp): this device's measured thresholds of the
    // R16 store -> compose -> 8-bit chain, R32_UINT 256 x 4 (channel row, byte column), made once before the first
    // frame that could use it. Without a table (the calibration failed or never ran) the compose path runs.
    bool calibrate_unorm_resolve(DXGI_FORMAT output_format, std::string* why);
    void ensure_unorm_calibration(DXGI_FORMAT output_format);
    GpuTexture unorm_table_;
    DXGI_FORMAT unorm_table_format_ = DXGI_FORMAT_UNKNOWN;
    bool unorm_calibration_tried_ = false;
    std::int64_t jit_start_ns_ = 0;  // the start jit_wait returned for the frame render() is about to record
    double jit_fill_mpx_ = 0.0;      // that frame's estimated particle fill (for the predictive model)
    // Render-ahead in GPU bursts (render/burst_ahead.h; the rendering thread's, under render_mutex_ except burst_).
    void apply_burst_mode();       // before a credit wait: follow what the spans ask for
    bool take_credit();            // the credit wait, plus a credit owed by leaving the mode; false: none taken
    void leave_burst_mode_unmeasured();  // a new or resized swap chain: back to one frame in flight
    BurstAhead burst_;
    bool burst_ahead_ = false;       // two frames in flight now
    std::uint32_t burst_repay_ = 0;  // credits to take on top of the next frame's (leaving the mode)
    std::uint32_t burst_settle_ = 0; // frames whose hold the phase lock should not sample (a switch just happened)
    // Credits owed: frames presented after a wait that took none. Each such present returns a credit later; it is taken
    // back without waiting as soon as it is there, so the pipeline does not stay a frame deeper (take_credit).
    std::uint32_t credit_debt_ = 0;
};

// Diagnostics (SHADOW_R2_FOLD_CHECK, lifted_render.cpp): 0 / 1 force the fold of a view under its overlay off / on for
// the renders that follow; -1 follows SHADOW_R2_FOLD_UNDER again.
void set_fold_under_override(int forced);

}  // namespace shadow::render
