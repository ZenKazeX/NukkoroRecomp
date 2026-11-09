#pragma once

// The frame graph (docs/lift-20261006/R_DESIGN.md 2.5).
//
// A frame is a list of passes in the game's authored order. Each pass declares, when it is added, the resources
// it reads and writes and how. The graph then
//   * drops the passes whose results nothing uses (a pass with an outside effect, or one that writes an imported
//     resource such as the back buffer, is always kept);
//   * gives every graph-owned texture a pooled resource that matches its description, created once and reused
//     across frames (recreated only when a description changes, e.g. on a resolution change);
//   * refuses a pass that reads a graph-owned texture before any pass wrote it;
//   * works out the resource-state transitions between passes;
//   * records the passes into the frame's command list, transitions and clears first.
// The graph never reorders passes: translucent and additive effects compose in the authored order, which is the
// visual intent.

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "render/descriptors.h"
#include "render/gpu_device.h"
#include "render/resources.h"

namespace shadow::render {

enum class Access : std::uint8_t {
    RenderTarget,     // colour output (write)
    DepthWrite,       // depth test and write (write)
    DepthRead,        // depth test only (read)
    ShaderRead,       // sampled or loaded by any shader stage (read)
    UnorderedAccess,  // shader read-write (write)
    CopySource,       // (read)
    CopyDest,         // (write)
    ResolveSource,    // multisample resolve input (read)
    ResolveDest,      // multisample resolve output (write)
    VertexRead,       // read as a vertex stream (read)
    IndexRead,        // read as an index stream (read)
};

// What a write does with the earlier contents.
enum class Load : std::uint8_t {
    Keep,     // the pass adds to the existing contents (blending, partial copies, depth-tested draws)
    Clear,    // the graph clears to the texture's clear value before the pass
    Discard,  // the pass overwrites every texel (full-screen passes, whole-resource resolves and copies)
};

struct TextureDesc {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;  // colour format, or the depth format (D32_FLOAT, D24_UNORM_S8_UINT)
    std::uint32_t samples = 1;
    bool unordered_access = false;
    std::array<float, 4> clear_color{0.0f, 0.0f, 0.0f, 0.0f};
    float clear_depth = 0.0f;  // reversed depth: the far end (frame_math.h kFarDepth)
    std::uint8_t clear_stencil = 0;

    bool operator==(const TextureDesc& o) const noexcept {
        return width == o.width && height == o.height && format == o.format && samples == o.samples &&
               unordered_access == o.unordered_access && clear_color == o.clear_color &&
               clear_depth == o.clear_depth && clear_stencil == o.clear_stencil;
    }
    bool operator!=(const TextureDesc& o) const noexcept { return !(*this == o); }
};

bool is_depth_format(DXGI_FORMAT format) noexcept;

using ResourceId = std::uint32_t;
inline constexpr ResourceId kNoResource = 0xFFFFFFFFu;

// Render targets the graph owns, kept between frames.
class TexturePool {
public:
    TexturePool() = default;
    TexturePool(const TexturePool&) = delete;
    TexturePool& operator=(const TexturePool&) = delete;
    ~TexturePool();

    bool create(Device& device, BindlessHeap& heap, std::uint32_t rtv_capacity, std::uint32_t dsv_capacity,
                std::string* error);
    void destroy();

    // Render-target memory shared by placed textures whose lifetimes in a frame never overlap (memory aliasing). The
    // frame graph's lifetimes are its kept-pass intervals; `busy` lists the intervals this frame's resources hold.
    struct Memory {
        Com<D3D12MA::Allocation> allocation;
        std::uint64_t size = 0;
        std::uint64_t frame = ~0ull;  // the frame `busy` belongs to
        struct Busy {
            std::uint32_t first, last;  // kept-pass interval, inclusive
            const void* holder;         // the Entry holding it
        };
        std::vector<Busy> busy;
        // The texture whose resource last used this memory in the GPU's order (set when a frame is recorded).
        const void* gpu_last_user = nullptr;
        // True when [first, last] meets an interval another entry holds this frame.
        bool conflicts(std::uint64_t at_frame, std::uint32_t first, std::uint32_t last, const void* holder) const;
        void hold(std::uint64_t at_frame, std::uint32_t first, std::uint32_t last, const void* holder);
    };

    struct Entry {
        GpuTexture texture;
        TextureDesc desc;
        // Set for a texture placed in shared render-target memory; empty for one with its own memory. A placed
        // texture is activated (aliasing barrier, then discard) at the first pass of every resource it backs.
        std::shared_ptr<Memory> memory;
        std::string name;  // the resource it was created for (report only; later holders may be others)
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
        D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
        D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
        D3D12_CPU_DESCRIPTOR_HANDLE dsv_read_only{};  // depth (and stencil) tested, not written
        std::uint32_t uav = kNoDescriptor;
        std::uint64_t last_frame = 0;
        bool in_use = false;
        // Render targets in heaps that are not zeroed must be initialised (discard, clear or copy) before any
        // other use; the graph discards a new texture once, before its first pass.
        bool initialized = false;
    };

    // A texture matching `desc` that no other resource of this frame holds (created when there is none). Matching is
    // by what the texture IS - size, format, samples, unordered access - not by the clear values a pass asks for: the
    // graph clears with the resource's own requested values whatever the pooled texture's optimized clear value is
    // (HW_REPORT M, G-R1: a background colour that changes every frame no longer makes a new set of view targets
    // every frame). Pooled textures are created with one fixed optimized clear value per kind (kPoolClear*).
    Entry* acquire(const TextureDesc& desc, std::uint64_t frame, const char* name, std::string* error);
    // The same for a resource used by kept passes [first, last] of the frame. A render-target or depth texture of
    // 1 MiB or more (no unordered access) is placed in shared render-target memory: in the largest memory that no
    // other resource holds during [first, last] - an existing placed texture there, or a new one placed there - or,
    // with none, in new memory of its own size. Memory then serves textures of any description at disjoint times
    // (at 5120x2880 the 4x glare sources and the resolved view, 228 + 115 MiB, take 228 MiB), and textures move into
    // the largest memory over frames; the ones left unused are released after `kKeepFrames`, their memory with the
    // last of them.
    Entry* acquire(const TextureDesc& desc, std::uint64_t frame, const char* name, std::uint32_t first,
                   std::uint32_t last, std::string* error);
    // Whether a texture of this description is placed in shared render-target memory.
    bool aliasable(const TextureDesc& desc) const;
    static bool pool_compatible(const TextureDesc& a, const TextureDesc& b) noexcept {
        return a.width == b.width && a.height == b.height && a.format == b.format && a.samples == b.samples &&
               a.unordered_access == b.unordered_access;
    }
    // How long an unused texture stays pooled. Releasing early saves nothing on this platform: a released render
    // target's memory stays in the process's commit for about 46 s on the measured RTX 4080 driver (OPTIMIZATION_RESULTS
    // "Phase A", A5), so a target recreated within that time - the 60 MiB glare-sources target after the glare passes
    // pause for a few frames - costs a second copy plus the creation. Sixty seconds at 60 Hz keeps every target an
    // intermittent pass returns to; a target whose size or format is no longer asked for (a changed render size) is
    // still released, one minute later.
    static constexpr std::uint64_t kKeepFrames = 3600;
    // Ends a frame: textures stay pooled, and a texture unused for `keep_frames` frames is released.
    void end_frame(std::uint64_t frame, std::uint64_t keep_frames = kKeepFrames);
    // Releases every pooled texture (after the GPU is done with it).
    void clear();
    std::size_t size() const noexcept { return entries_.size(); }
    std::uint64_t created() const noexcept { return created_; }    // textures made, all frames
    std::uint64_t released() const noexcept { return released_; }  // textures released unused, all frames
    std::size_t memories() const noexcept { return memories_.size(); }  // shared render-target memories live
    std::uint64_t memory_bytes() const noexcept;                         // their bytes
    std::uint64_t placed_bytes() const noexcept;  // the bytes their textures would take each in its own memory
    // Every shared memory and the textures placed in it, one line per memory: "<MiB> MiB: <name> <w>x<h> s<samples>
    // <MiB> (last frame <n>), ...". For the run report; built on demand.
    std::string memory_map() const;

private:
    void release(Entry& entry);
    // Creates a texture for `desc` in `memory` (placed), or with its own memory when `memory` is null.
    Entry* create_entry(const TextureDesc& desc, std::uint64_t frame, const char* name,
                        std::shared_ptr<Memory> memory, std::string* error);
    std::uint64_t texture_bytes(const TextureDesc& desc) const;

    Device* device_ = nullptr;
    BindlessHeap* heap_ = nullptr;
    CpuDescriptorPool rtvs_;
    CpuDescriptorPool dsvs_;
    std::vector<std::unique_ptr<Entry>> entries_;
    std::vector<std::shared_ptr<Memory>> memories_;
    std::uint64_t created_ = 0, released_ = 0;
    std::uint64_t last_frame_ = 0;  // the frame end_frame last ended (reports: which textures are in use)
};

struct ImportedTexture {
    ID3D12Resource* resource = nullptr;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;        // its state when the frame starts
    D3D12_RESOURCE_STATES final_state = D3D12_RESOURCE_STATE_COMMON;  // the state the graph leaves it in
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
    D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
    std::uint32_t srv = kNoDescriptor;
    TextureDesc desc;  // size, format and samples of the resource
    // A render-target texture never written yet: its first render-target write declared Load::Discard initialises it
    // with DiscardResource after that pass's barriers, as D3D12 requires before any other use.
    bool discard_first = false;
};

struct ImportedBuffer {
    ID3D12Resource* resource = nullptr;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES final_state = D3D12_RESOURCE_STATE_COMMON;
};

class FrameGraph;

class PassBuilder {
public:
    void read(ResourceId resource, Access access);
    void write(ResourceId resource, Access access, Load load = Load::Keep);
    // Keeps the pass even when nothing in the graph reads its results (e.g. a readback the CPU consumes).
    void side_effect();

private:
    friend class FrameGraph;
    PassBuilder(FrameGraph& graph, std::uint32_t pass) : graph_(graph), pass_(pass) {}
    FrameGraph& graph_;
    std::uint32_t pass_;
};

class PassContext {
public:
    ID3D12GraphicsCommandList* list() const noexcept { return list_; }
    ID3D12Resource* resource(ResourceId id) const;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv(ResourceId id) const;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv(ResourceId id) const;
    // The read-only view of a graph-owned depth texture (Access::DepthRead).
    D3D12_CPU_DESCRIPTOR_HANDLE dsv_read_only(ResourceId id) const;
    std::uint32_t srv(ResourceId id) const;  // bindless index
    std::uint32_t uav(ResourceId id) const;  // bindless index
    const TextureDesc& desc(ResourceId id) const;
    // Viewport and scissor covering the whole texture.
    void full_viewport(ResourceId id) const;
    // Sets a viewport and scissor (every pass sets them through here or full_viewport, so a target trace sees them).
    void set_viewport(const D3D12_VIEWPORT& viewport, const D3D12_RECT& scissor) const;

private:
    friend class FrameGraph;
    PassContext(const FrameGraph& graph, ID3D12GraphicsCommandList* list) : graph_(graph), list_(list) {}
    const FrameGraph& graph_;
    ID3D12GraphicsCommandList* list_;
};

// Optional observer of the kept passes as they are recorded: GPU timestamps around each pass (gpu_timer.h,
// GEKKO_GPU_TRACE). It records into the same list; nothing is called when none is given.
class PassMarks {
public:
    virtual ~PassMarks() = default;
    virtual void pass_begin(ID3D12GraphicsCommandList* list, const std::string& name) = 0;
    virtual void pass_end(ID3D12GraphicsCommandList* list) = 0;
};

class FrameGraph {
public:
    using Execute = std::function<void(PassContext&)>;

    // Starts a new frame's graph. `frame` numbers frames for the pool's reuse policy.
    void reset(std::uint64_t frame);

    ResourceId create_texture(const char* name, const TextureDesc& desc);
    ResourceId import_texture(const char* name, const ImportedTexture& texture);
    ResourceId import_buffer(const char* name, const ImportedBuffer& buffer);

    template <class Setup>
    void add_pass(const char* name, Setup&& setup, Execute execute) {
        const auto index = static_cast<std::uint32_t>(passes_.size());
        passes_.push_back(Pass{name, {}, false, false, std::move(execute)});
        PassBuilder builder(*this, index);
        setup(builder);
    }

    // Culls, assigns pooled textures, validates and plans the transitions. False (with the reason) on an
    // invalid graph; nothing is recorded then.
    bool compile(TexturePool& pool, std::string* error);
    // Records the kept passes. The bindless heaps and the root signature are bound first (heaps before the root
    // signature, as directly indexed heaps require). `marks`, when given, is told where each kept pass begins and
    // ends (its barriers and clears included).
    void execute(ID3D12GraphicsCommandList* list, const BindlessHeap& heap, const SamplerHeap& samplers,
                 ID3D12RootSignature* root_signature, PassMarks* marks = nullptr);

    // Final state of an imported resource after execute (what the caller records for the next frame).
    D3D12_RESOURCE_STATES state_after(ResourceId id) const;

    // ---- introspection (tests, captures) ------------------------------------------------------------------
    struct Transition {
        std::uint32_t pass;  // passes_.size() for the closing transitions of imported resources
        ResourceId resource;
        D3D12_RESOURCE_STATES before;
        D3D12_RESOURCE_STATES after;
        bool uav_barrier;
    };
    // A resource backed by a texture in shared render-target memory, made the memory's active resource at its first
    // kept pass: an aliasing barrier (with a transition to `discard_state` when it was left in another state), then
    // DiscardResource, before that pass's own transitions. Its first use overwrites it whole or clears it (compile
    // checks that), so the discarded contents are never observed.
    struct Activation {
        std::uint32_t pass;
        ResourceId resource;
        D3D12_RESOURCE_STATES before;         // the texture's state when the frame reaches the pass
        D3D12_RESOURCE_STATES discard_state;  // RENDER_TARGET or DEPTH_WRITE, what DiscardResource needs
    };
    std::vector<std::string> kept_passes() const;
    const std::vector<Transition>& transitions() const noexcept { return transitions_; }
    const std::vector<Activation>& activations() const noexcept { return activations_; }
    // Target trace (docs/RESOLUTION_AUDIT.md: resolution evidence): while set, execute appends one line per kept pass,
    // one per render or depth target it binds - the size, samples and format the D3D12 resource reports
    // (ID3D12Resource::GetDesc) - and one per viewport/scissor it sets. Null: nothing is recorded.
    void set_target_trace(std::vector<std::string>* lines) noexcept { trace_ = lines; }
    std::size_t pass_count() const noexcept { return passes_.size(); }

private:
    friend class PassBuilder;
    friend class PassContext;

    struct Use {
        ResourceId resource;
        Access access;
        Load load;
        bool write;
    };
    struct Pass {
        std::string name;
        std::vector<Use> uses;
        bool side_effect;
        bool kept;
        Execute execute;
    };
    enum class Kind : std::uint8_t { Created, ImportedTexture, ImportedBuffer };
    struct Resource {
        std::string name;
        Kind kind;
        TextureDesc desc;
        ID3D12Resource* d3d = nullptr;
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES final_state = D3D12_RESOURCE_STATE_COMMON;
        D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
        D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
        D3D12_CPU_DESCRIPTOR_HANDLE dsv_read_only{};
        std::uint32_t srv = kNoDescriptor;
        std::uint32_t uav = kNoDescriptor;
        TexturePool::Entry* pooled = nullptr;
        bool discard_first = false;  // ImportedTexture::discard_first
        D3D12_RESOURCE_STATES end_state = D3D12_RESOURCE_STATE_COMMON;  // planned state after the frame
        bool used = false;                                                  // by a kept pass
    };

    void add_use(std::uint32_t pass, ResourceId resource, Access access, Load load, bool write);
    bool fail(std::string* error, const std::string& message);

    std::uint64_t frame_ = 0;
    std::vector<Resource> resources_;
    std::vector<Pass> passes_;
    std::vector<Transition> transitions_;
    std::vector<Activation> activations_;
    std::vector<std::pair<TexturePool::Memory*, const void*>> memory_users_;  // compile -> execute
    bool compiled_ = false;
    std::string build_error_;
    std::vector<std::string>* trace_ = nullptr;
};

D3D12_RESOURCE_STATES state_for(Access access) noexcept;

}  // namespace shadow::render
