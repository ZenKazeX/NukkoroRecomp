#pragma once

// The Vulkan renderer (docs/VULKAN_RENDERER.md): the same contract as render::Renderer (runtime/lift/render/renderer.h)
// - it owns the device, every GPU resource, the pipelines and the frame loop, and turns the game's FramePackets into
// frames - on Vulkan, for the Android port (and on Win32 for desktop testing).
//
// What it draws: per view a multisampled UNORM16 scene colour + D32 depth at the view's share of the output; the views
// drawn over earlier views (split screen's overlay) started from those views; the deformed meshes of the frame - delta
// morphs blended and skinned poses posed by the D3D12 renderer's compute shader (CSSkin) into this frame's region of the
// skin cache; the view's passes in the game's order - Scene passes (meshes with their environment / dual second pass,
// particle batches, immediate geometry in the world or on the view's canvas, sampling textures, snapshots or the
// history), GlareSources passes (drawn onto black over the scene's depth, tested not written), Glare passes (the glare
// chain of shaders/glare.hlsl; the composite folded into the view's resolve when only canvas draws follow it - B3a),
// Snapshot passes (the scene resolved where later draws sample it) and SaveHistory passes (resolved straight into the
// view's kept history - B1); the renderer's own deterministic resolve (PSResolve); and the compose of the views into
// the output (the swapchain image, pre-rotated on Android, or an image read back). The D3D12 renderer's shaders and
// constant blocks throughout (render/frame_math.h). Every pass is declared in the frame graph (frame_graph.h), which
// culls the unused ones, pools and aliases the targets and derives the barriers and attachment ops.
//
// Resources (textures, meshes) are made when asked, on the asking thread (the upload is submitted and the handle is
// usable at once); the D3D12 renderer's asynchronous resource worker is not ported yet. Handles, scopes and deferred
// releases follow render::Renderer exactly.

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "assets/geometry_import.h"
#include "assets/texture_import.h"
#include "render/frame.h"
#include "render/frame_math.h"
#include "render_vk/bindless.h"
#include "render_vk/device.h"
#include "render_vk/frame_graph.h"
#include "render_vk/gpu_timer.h"
#include "render_vk/frame_loop.h"
#include "render_vk/pipelines.h"
#include "render_vk/resources.h"
#include "render_vk/swapchain.h"

namespace shadow::render_vk {

using render::FramePacket;
using render::MeshHandle;
using render::Scope;
using render::TextureHandle;

struct RendererOptions {
    DeviceOptions device;
    std::uint32_t msaa_samples = 4;
    VkFormat scene_format = VK_FORMAT_R16G16B16A16_UNORM;  // R_DESIGN 2.5: no 8-bit banding in the scene
    std::uint32_t frames_in_flight = 2;
    std::uint32_t max_anisotropy = 16;
    std::uint32_t bindless_persistent = 16384;
    std::uint32_t bindless_transient_per_frame = 1024;
    VkDeviceSize upload_ring_bytes = 8u << 20;
    // The skin cache's starting region per frame slot (it grows to the frame's need, at least doubling).
    std::uint32_t skinned_vertices_per_frame = 16u << 10;
    // B3a: the glare composite folded into the view's resolve when only canvas draws follow it (false: always the
    // plain composite - the A/B of the draw checks).
    bool fold_glare = true;
    // V6 (docs/VULKAN_RENDERER.md 5.1): attachment-only graph textures in transient (tile) images, adjacent draw
    // passes on the same attachments merged into one rendering instance, GPU timestamps per rendering group. False:
    // the V2 plan (the A/B of the checks and the replay).
    bool transient_targets = true;
    bool merge_passes = true;
    bool gpu_timestamps = true;
    // The view's depth kept in tile memory: the passes after its last depth test do not take it, and - where the device
    // has VK_KHR_dynamic_rendering_local_read - a view's scene and glare sources passes up to its glare draw in ONE
    // rendering instance (two colour attachments, each draw's output routed to its own). False: as V6's first part.
    bool keep_depth_on_tile = true;
};

class Renderer {
public:
    Renderer() = default;
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    ~Renderer();

    bool create(const RendererOptions& options, std::string* error);
    void destroy();

    // ---- resources ----------------------------------------------------------------------------------------------
    // Made before returning (any thread): an empty handle and `error` when it could not be made. A handle's lifetime
    // ends with the last packet that names it: a release names that packet's tick (kNoPacket: none) and takes effect
    // once finish_packets has passed it (render/renderer.h).
    static constexpr std::uint64_t kNoPacket = ~std::uint64_t{0};
    TextureHandle create_texture(Scope scope, const assets::TextureImage& image, std::string* error);
    MeshHandle create_mesh(Scope scope, const assets::MeshData& mesh, std::string* error);
    void release(TextureHandle texture, std::uint64_t named_through = kNoPacket);
    void release(MeshHandle mesh, std::uint64_t named_through = kNoPacket);
    void finish_packets(std::uint64_t tick);
    void release_scope(Scope scope);

    // ---- output -----------------------------------------------------------------------------------------------
    bool attach_window(const NativeWindow& window, std::uint32_t width, std::uint32_t height, std::string* error);
    bool resize_window(std::uint32_t width, std::uint32_t height, std::string* error);
    // The window's surface goes away (Android: the activity left the screen): the swapchain with it; the device, every
    // resource and the histories stay, and attach_window on the next surface continues.
    void detach_window();
    bool has_window() const noexcept { return has_window_; }
    // The swapchain's picture size and pre-rotation (logs, tools).
    std::uint32_t window_width() const noexcept { return swapchain_.width(); }
    std::uint32_t window_height() const noexcept { return swapchain_.height(); }
    std::uint32_t window_quarter_turns() const noexcept { return swapchain_.quarter_turns(); }
    // Renders the packet into the swapchain's next image and presents it. A swapchain that went out of date is
    // recreated and the frame is skipped (true).
    bool render(const FramePacket& packet, PresentMode mode, std::string* error);
    bool reading(const FramePacket& packet) const noexcept {
        return reading_.load(std::memory_order_acquire) == &packet;
    }
    // Renders the packet at width x height and returns its pixels (RGBA8, rows top first).
    bool render_to_image(const FramePacket& packet, std::uint32_t width, std::uint32_t height,
                         std::vector<std::uint8_t>* rgba, std::string* error, bool commit_history = true);
    void invalidate_histories();
    // Diagnostics (the effect checks' A/B): the glare composite folded into the resolve (B3a) or always plain.
    void set_fold_glare(bool on) noexcept { options_.fold_glare = on; }

    // ---- introspection ------------------------------------------------------------------------------------------
    enum class SkipReason : std::uint8_t {
        ParticleBatch, ParticlePipeline, ParticleUpload, ImmediateInvalid, ImmediatePipeline, ImmediateImage,
        ImmediateUpload, MeshMissing, SkinMissing, MeshPipeline, MeshUpload,
        Count
    };
    static const char* skip_reason_name(SkipReason reason) noexcept;
    static constexpr std::size_t kSkipReasons = static_cast<std::size_t>(SkipReason::Count);

    struct Stats {
        std::uint64_t frames = 0;
        std::uint64_t draws = 0;          // last frame: draw calls issued
        std::uint64_t skipped_draws = 0;  // last frame
        std::uint64_t skipped_draws_total = 0;
        std::array<std::uint64_t, kSkipReasons> skipped_by_reason{};  // all frames
        std::uint64_t particles = 0;      // last frame
        std::uint64_t immediates = 0;     // last frame
        std::uint64_t glares = 0;            // last frame: glare composites
        std::uint64_t glares_folded = 0;     // last frame: of those, folded into the resolve (B3a)
        std::uint64_t folded_spans = 0;      // last frame: the folded composites' tile spans drawn plainly
        std::uint64_t snapshots = 0;         // last frame: snapshots resolved
        std::uint64_t history_saves = 0;     // last frame: histories saved
        std::uint64_t skin_jobs = 0;         // last frame: skinned poses
        std::uint64_t morph_jobs = 0;        // last frame: delta-morph shapes (a skinned one counts in both)
        std::uint64_t skinned_vertices = 0;  // last frame
        std::uint64_t pairs = 0;             // last frame: scene + glare sources pairs (one rendering instance each)
        std::uint64_t skin_overflows = 0;    // all frames: deformed draws the skin cache had no room for
        std::uint64_t views_over_views = 0;  // all frames
        std::uint64_t image_misses = 0;      // all frames: immediate draws of a snapshot / history that does not exist
        std::size_t pool_images = 0;
        VkDeviceSize pool_bytes = 0;         // now: the pooled targets (texels x texel size)
        std::uint64_t pool_created = 0;      // all frames: pooled targets made
        FrameGraph::Stats graph;             // last frame: passes declared / kept / culled, textures, aliases, barriers
        // The latest frame the GPU finished that the timer read (render: a frame or two behind; render_to_image: this
        // frame): GPU time per rendering group and their sum. Empty without timestamps.
        std::vector<GpuTimer::Span> gpu_passes;
        double gpu_ms = 0.0;
        std::uint64_t gpu_frames = 0;  // all frames: frames whose times were read
        std::size_t pipelines = 0;
        std::size_t textures = 0;  // now
        std::size_t meshes = 0;
        std::uint64_t deferred_releases = 0;  // all frames
        std::size_t pending_releases = 0;     // now
    };
    Stats stats() const;
    Device& device() noexcept { return device_; }
    std::vector<std::string> take_debug_errors() { return device_.take_validation_messages(); }
    VkSampleCountFlagBits scene_samples() const noexcept { return samples_; }
    // The scene colour format in use: the asked one, or the fallback when the device cannot blend into it as a
    // multisampled colour attachment (docs/VULKAN_RENDERER.md 5.3).
    VkFormat scene_format() const noexcept { return options_.scene_format; }
    const RendererOptions& options() const noexcept { return options_; }

private:
    struct Output {
        Image* image = nullptr;  // the swapchain image or the capture image
        std::uint32_t width = 0, height = 0;  // the picture's (the image's sides swapped when turned odd times)
        std::uint32_t quarter_turns = 0;      // the surface's pre-rotation (swapchain.h)
        bool present = false;                 // left ready for the present
        Buffer* readback = nullptr;           // the picture copied here (RGBA8, rows top first)
    };
    // A texture or mesh slot: generational handles (24-bit slot + 1, 8-bit generation; render/frame.h).
    struct TextureSlot {
        Image image;  // image.srv: its persistent bindless index
        Scope scope = Scope::Session;
        std::uint8_t generation = 1;
        bool live = false;
    };
    struct MeshSlot {
        // The three streams in ONE buffer (render/renderer.cpp G13): positions + normals at 0, colour + UVs at
        // attributes_offset, the 32-bit indices at indices_offset.
        Buffer streams;
        VkDeviceSize attributes_offset = 0;
        VkDeviceSize indices_offset = 0;
        std::uint32_t vertex_count = 0;
        // A skinned or delta-morphed mesh keeps its base vertices with their bone weights (SkinVertex, 44 bytes) and
        // its delta targets (MorphDelta, target-major) for the deformation pass; bindless storage-buffer indices.
        Buffer skin;
        std::uint32_t skin_srv = kNoDescriptor;
        bool skinned = false;
        Buffer morph;
        std::uint32_t morph_srv = kNoDescriptor;
        std::uint32_t morph_targets = 0;
        std::vector<assets::MeshData::Submesh> submeshes;
        Scope scope = Scope::Session;
        std::uint8_t generation = 1;
        bool live = false;
    };
    struct PendingRelease {
        bool texture = false;
        std::uint32_t handle = 0;
        std::uint64_t named_through = 0;
    };
    // Per view while a frame is recorded: its graph textures and its rectangle in the output.
    struct ViewTargets {
        ResourceId colour = kNoResource;    // multisampled scene colour
        ResourceId depth = kNoResource;
        ResourceId resolved = kNoResource;  // the view's picture
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        std::uint32_t width = 0, height = 0;
    };
    // A view's history (render/renderer.h History, B1): its scene colour as the view last saved it, kept across
    // frames at the view's size, a colour attachment a save resolves straight into.
    struct History {
        Image image;
        bool valid = false;
    };
    // What a frame's passes share: the light blocks (ring offsets), the deformed vertices of each draw (skin-cache
    // element and pose origin, filled by the deformation pass), and the frame's counters.
    static constexpr std::uint32_t kNotDeformed = 0xFFFFFFFFu;
    struct FrameDraws {
        std::vector<std::uint32_t> light_blocks;
        std::uint32_t no_lights = 0;
        std::vector<std::uint32_t> deformed;               // per draw: first skin-cache element, kNotDeformed
        std::vector<std::array<float, 3>> deformed_origin;  // per draw: the pose's origin (skinned)
        std::uint64_t draws = 0, skipped = 0, particles = 0, immediates = 0;
        std::uint64_t glares = 0, glares_folded = 0, folded_spans = 0, snapshots_made = 0, history_saves = 0;
        std::uint64_t skin_jobs = 0, morph_jobs = 0, skinned_vertices = 0;
        std::uint64_t pairs = 0;
    };
    // Where a draw pass draws: the view's scene colour, or its glare sources (onto black, over the scene's depth
    // tested and not written), or both as a pair - and the images its draws may sample besides textures.
    struct DrawTarget {
        ResourceId colour = kNoResource;
        ResourceId depth = kNoResource;    // none: after the view's last depth test
        ResourceId sources = kNoResource;  // a pair: the glare sources beside the scene colour (`colour`)
        bool glare_sources = false;
        ResourceId history = kNoResource;  // the view's history when it holds a picture
        std::vector<ResourceId> snapshots;  // per snapshot id: the snapshots made before this pass
    };
    // A view's glare composite folded into its final resolve (B3a): the inputs, and the tile spans that got the
    // plain composite before the canvas draws after it (re-resolved plainly).
    struct FoldedGlare {
        bool active = false;
        ResourceId sources = kNoResource;
        ResourceId first = kNoResource;
        ResourceId second = kNoResource;
        std::vector<render::PixelBox> spans;
    };

    // A frame's own texture (pooled by the graph).
    ResourceId frame_texture(const char* name, std::uint32_t width, std::uint32_t height, VkFormat format,
                             VkSampleCountFlagBits samples);
    // The image's index in the bindless set for this frame (a transient index written with its sampled view).
    std::uint32_t transient_srv(const Image& image);
    // A Root block (16 x 32-bit, cbuffer b0) for the next draw; binds set 0 with it.
    bool bind_root(VkCommandBuffer cmd, const std::uint32_t (&root)[16], std::string* error);
    // A constant block of `bytes` in the frame ring at the uniform alignment; its offset, or ~0u when the ring is full.
    std::uint32_t ring_block(const void* data, std::size_t bytes);
    // Declares the frame's passes in the graph, compiles it and records it into `cmd` (renderer.cpp).
    // `commit_history` false: the views' kept histories are read but never written (render_to_image).
    bool record(VkCommandBuffer cmd, const FramePacket& packet, const Output& output, bool commit_history,
                std::string* error);
    // One pass of the packet in a draw pass: its record (null: none - the scene's clear), and whether its draws are
    // glare sources.
    struct DrawStep {
        const render::PassRecord* pass = nullptr;
        bool sources = false;
    };
    // One draw pass of view `v`: one rendering instance (the graph may continue the previous one) over `target`, in
    // which each step's draws are recorded in order (renderer_draws.cpp) - one Scene or GlareSources pass, or a pair's
    // run of them.
    bool record_draw_pass(PassContext& ctx, const FramePacket& packet, const std::vector<DrawStep>& steps,
                          std::size_t v, const ViewTargets& t, const DrawTarget& target, FrameDraws& frame,
                          std::string* error);
    void record_draws(PassContext& ctx, const FramePacket& packet, const render::PassRecord& record, std::size_t v,
                      const ViewTargets& t, const DrawTarget& target, bool sources, FrameDraws& frame);
    void skip(SkipReason reason, FrameDraws& frame);

    // Effects (renderer_effects.cpp): each declares its graph passes; the graph records them.
    // Grows the skin cache to the frame's distinct deformation jobs (before the graph imports it).
    void size_skin_cache(const FramePacket& packet);
    // The frame's deformation jobs (skinning, delta morphs) recorded into the skin cache; per draw its element.
    bool record_deformations(VkCommandBuffer cmd, const FramePacket& packet, FrameDraws& frame, std::string* error);
    // `from` (a multisampled scene or glare-sources texture; single-sampled at 1x) resolved into `into` by PSResolve
    // (a copy when single-sampled); `region`: only those texels. `done` runs once it is recorded.
    void add_resolve(const char* name, ResourceId from, ResourceId into, const render::PixelBox* region,
                     std::function<void()> done = {});
    // One full-screen pass: `screen` writes all of `out` from `inputs` (sampled) with the root constants `root`,
    // whose first inputs.size() entries become the inputs' bindless indices.
    void add_screen_pass(const char* name, Screen screen, ResourceId out, std::vector<ResourceId> inputs,
                         const std::array<std::uint32_t, 16>& root);
    // The view's glare (glare.hlsl) from its multisampled sources at pass `glare_pass`: the levels, then the composite
    // into the scene - or folded into the resolve (`folded`).
    void add_glare(const FramePacket& packet, std::size_t v, std::size_t glare_pass, const render::GlareParams& g,
                   const ViewTargets& t, ResourceId sources_ms, FoldedGlare& folded, FrameDraws& frame);
    // The view's final resolve with a folded glare composite (PSResolveGlare, then the spans plainly).
    void add_folded_resolve(const ViewTargets& t, const FoldedGlare& folded);
    bool reserve_skin_cache(std::uint32_t vertices_per_frame, std::string* error);

    // Resources (renderer_resources.cpp). The handle's slot, or null for a stale handle (under the table lock).
    TextureSlot* texture_slot(TextureHandle handle);
    MeshSlot* mesh_slot(MeshHandle handle);
    // Its bindless index (kNoDescriptor: none, stale or released). The rendering thread.
    std::uint32_t texture_srv(TextureHandle handle);
    void release_locked(TextureHandle texture);
    void release_locked(MeshHandle mesh);
    // Records `fill` into a one-time command buffer and submits it (the upload of a new resource); the staging buffer
    // and the command buffer are released once the GPU has passed it.
    bool upload(VkDeviceSize staging_bytes, const std::function<void(void* staging)>& write,
                const std::function<void(VkCommandBuffer cmd, VkBuffer staging)>& fill, std::string* error);

    RendererOptions options_;
    VkSampleCountFlagBits samples_ = VK_SAMPLE_COUNT_1_BIT;
    Device device_;
    Bindless bindless_;
    FrameLoop frames_;
    Pipelines pipelines_;
    Swapchain swapchain_;
    bool has_window_ = false;
    FrameGraph graph_;
    TargetPool targets_;
    GpuTimer timer_;
    std::uint64_t frame_number_ = 0;
    Image capture_;
    Buffer readback_;
    std::atomic<const FramePacket*> reading_{nullptr};
    Stats render_stats_;  // the rendering thread's frame figures
    std::vector<History> histories_;  // by view index
    // The skin cache: frames_in_flight regions of skin_per_frame_ SkinnedVertex (24 bytes), a storage buffer the
    // deformation pass writes and the draws read as their stream 0.
    Buffer skin_cache_;
    std::uint32_t skin_cache_srv_ = kNoDescriptor;
    std::uint32_t skin_per_frame_ = 0;

    // Resources: the handle tables, their free lists and the deferred releases, under table_mutex_ (never held across a
    // GPU wait). Uploads have their own command pool and lock.
    mutable std::mutex table_mutex_;
    std::vector<TextureSlot> textures_;
    std::vector<std::uint32_t> free_textures_;
    std::vector<MeshSlot> meshes_;
    std::vector<std::uint32_t> free_meshes_;
    std::vector<PendingRelease> pending_releases_;
    bool packets_finished_ = false;
    std::uint64_t finished_through_ = 0;
    std::uint64_t deferred_releases_ = 0;
    std::mutex upload_mutex_;
    VkCommandPool upload_pool_ = VK_NULL_HANDLE;

    Stats stats_;  // published per frame (stats_mutex_)
    mutable std::mutex stats_mutex_;
    std::mutex render_mutex_;
    bool created_ = false;
};

}  // namespace shadow::render_vk
