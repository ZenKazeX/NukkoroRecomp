#pragma once

// The frame graph in Vulkan (docs/VULKAN_RENDERER.md 3.6, stage V2): render/frame_graph.h's model and rules, on
// synchronization2 and dynamic rendering. That header is bound to D3D12 (resource states, CPU descriptor handles), so
// this is its counterpart rather than a shared core (hook H3 would split one out).
//
// A frame is a list of passes in the game's authored order. Each pass declares, when it is added, the resources it
// reads and writes and how. The graph then
//   * drops the passes whose results nothing uses (a pass with an outside effect, or one that writes an imported
//     resource - the swapchain image, a history, the skin cache - is always kept);
//   * gives every graph-owned texture a pooled image that matches its description, made once and reused across frames;
//     within a frame, a texture whose first use comes after another's last use takes over that one's image (a
//     snapshot shares the view's resolve target, the split-screen views share one set of scene targets);
//   * refuses a pass that reads a graph-owned texture before any pass wrote it;
//   * derives the barriers: per physical image (and imported buffer) from its last use to the next, one
//     vkCmdPipelineBarrier2 per pass boundary; a write that clears or overwrites starts from UNDEFINED; a read after a
//     read in the same layout needs none;
//   * turns Load into the attachment's load op (Clear -> CLEAR, Discard -> DONT_CARE, Keep -> LOAD) and stores an
//     attachment only when a later pass uses it (on a tiler, the depth and the multisampled colour after their last
//     pass are never written to memory);
//   * gives a texture that every kept pass uses only as an attachment a transient image (TRANSIENT_ATTACHMENT, in
//     lazily allocated memory where the GPU has it: tile memory only, unless a pass stores it) (V6);
//   * merges a mergeable pass into the one before it when both render to the same attachments in the same layouts,
//     the later one keeps their contents and needs no barrier but those between the two attachment writes (which
//     rasterization order covers): one rendering instance, so on a tiler the attachments stay in tile memory
//     between them (V6);
//   * records the kept passes in order, with a GPU timestamp after each rendering group when given a timer (V6).
// The graph never reorders passes: translucent and additive effects compose in the authored order, which is the
// visual intent. Planning happens in compile, so the plan (kept passes, barriers, aliases) can be inspected before
// anything is recorded.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "render_vk/device.h"
#include "render_vk/resources.h"

namespace shadow::render_vk {

enum class Access : std::uint8_t {
    RenderTarget,     // colour attachment (write)
    DepthWrite,       // depth attachment, tested and written (write)
    DepthRead,        // depth attachment, tested only - the read-only layout (read)
    ShaderRead,       // sampled by a shader (read)
    UnorderedAccess,  // storage read-write: compute (write)
    CopySource,       // (read)
    CopyDest,         // (write)
    VertexRead,       // a vertex stream (buffers; read)
};
const char* access_name(Access access) noexcept;
bool is_write(Access access) noexcept;
// The stage, access and layout of an image used so.
ImageUse image_use(Access access) noexcept;

// What a write does with the earlier contents.
enum class Load : std::uint8_t {
    Keep,     // the pass adds to the existing contents (blending, depth-tested draws)
    Clear,    // cleared to the use's clear value as the pass begins (the attachment's load op)
    Discard,  // the pass overwrites everything later passes read (full-screen passes, resolves, copies)
};

struct TextureDesc {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;  // colour, or a depth format
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    bool storage = false;  // also a storage image
    // Set by the graph, not by passes: every kept use is an attachment, so the image needs no other usage and can
    // live in lazily allocated (tile) memory.
    bool transient = false;
};
// The usage a pooled image of `desc` gets: one set per kind, so textures of one size and format pool and alias
// whatever passes use them (colour: attachment, sampled, copy source and destination; depth: attachment; transient:
// the attachment usage alone, plus TRANSIENT_ATTACHMENT).
VkImageUsageFlags pooled_usage(const TextureDesc& desc) noexcept;

// Render targets the graph owns, kept between frames (render/frame_graph.h TexturePool).
class TargetPool {
public:
    TargetPool() = default;
    TargetPool(const TargetPool&) = delete;
    TargetPool& operator=(const TargetPool&) = delete;
    ~TargetPool();

    void attach(Device& device) noexcept { device_ = &device; }
    // An image matching `desc` that no other resource of this frame holds (made when there is none).
    Image* acquire(const TextureDesc& desc, std::uint64_t frame, const char* name, std::string* error);
    static bool compatible(const TextureDesc& a, const TextureDesc& b) noexcept {
        return a.width == b.width && a.height == b.height && a.format == b.format && a.samples == b.samples &&
               a.storage == b.storage && a.transient == b.transient;
    }
    // How long an unused image stays pooled (render/frame_graph.h TexturePool::kKeepFrames: sixty seconds at 60 Hz -
    // an intermittent pass returns to its targets; a size no longer asked for is released a minute later).
    static constexpr std::uint64_t kKeepFrames = 3600;
    // Ends a frame: images stay pooled; one unused for `keep_frames` frames is released (after the GPU is done).
    void end_frame(std::uint64_t frame, std::uint64_t keep_frames = kKeepFrames);
    // Releases every pooled image now (the GPU must be idle).
    void destroy();
    std::size_t size() const noexcept { return entries_.size(); }
    std::uint64_t created() const noexcept { return created_; }    // images made, all frames
    std::uint64_t released() const noexcept { return released_; }  // images released unused, all frames
    VkDeviceSize bytes() const noexcept;                            // of the pooled images (estimate: texels x size)

private:
    struct Entry {
        Image image;
        TextureDesc desc;
        std::uint64_t last_frame = 0;
        bool in_use = false;
    };
    Device* device_ = nullptr;
    std::vector<std::unique_ptr<Entry>> entries_;  // entries never move: a frame holds Image pointers into them
    std::uint64_t created_ = 0, released_ = 0;
};

using ResourceId = std::uint32_t;
inline constexpr ResourceId kNoResource = 0xFFFFFFFFu;

class FrameGraph;

class PassBuilder {
public:
    void read(ResourceId resource, Access access);
    // `clear`: the value a Load::Clear clears to.
    void write(ResourceId resource, Access access, Load load = Load::Keep, const VkClearValue& clear = {});
    // Keeps the pass even when nothing in the graph reads its results (a readback the CPU consumes).
    void side_effect();
    // The pass records exactly one rendering instance, over all its attachments and their whole extent, through
    // PassContext::begin_rendering / end_rendering: the graph may continue the previous pass's instance instead (V6).
    void mergeable();

private:
    friend class FrameGraph;
    PassBuilder(FrameGraph& graph, std::uint32_t pass) : graph_(graph), pass_(pass) {}
    FrameGraph& graph_;
    std::uint32_t pass_;
};

class PassContext {
public:
    VkCommandBuffer cmd() const noexcept { return cmd_; }
    Image& image(ResourceId id) const;
    VkBuffer buffer(ResourceId id) const;
    // The rendering attachment of a texture this pass writes (RenderTarget, DepthWrite) or tests (DepthRead): its
    // view and layout, the load op from the declared Load (with its clear value), the store op from whether a later
    // pass uses it (for a pass that opens a merged instance: whether a pass after the instance uses it).
    VkRenderingAttachmentInfo attachment(ResourceId id) const;
    // A mergeable pass's rendering instance: begun, or continued when the graph merged this pass into the previous
    // one (the info must name the same attachments and area); ended, or left open for the next pass merged into it.
    bool begin_rendering(const VkRenderingInfo& info, std::string* error);
    void end_rendering();

private:
    friend class FrameGraph;
    PassContext(FrameGraph& graph, VkCommandBuffer cmd, std::uint32_t pass) : graph_(graph), cmd_(cmd), pass_(pass) {}
    FrameGraph& graph_;
    VkCommandBuffer cmd_;
    std::uint32_t pass_;
};

class GpuTimer;

class FrameGraph {
public:
    // Records the pass; false (with the reason) stops the frame.
    using Execute = std::function<bool(PassContext&, std::string* error)>;

    // Starts a new frame's graph. `frame` numbers frames for the pool's reuse policy.
    void reset(std::uint64_t frame);

    ResourceId create_texture(const char* name, const TextureDesc& desc);
    // An image the graph does not own (the swapchain image, a capture, a history): it starts from the use the image
    // remembers (Image::state) and, with `final_use`, is left in that use after the last pass (the present).
    ResourceId import_image(const char* name, Image& image, const ImageUse* final_use = nullptr);
    // A buffer the graph does not own, last used as `initial` (earlier frames) and left ready for `final`.
    ResourceId import_buffer(const char* name, VkBuffer buffer, Access initial, Access final);

    template <class Setup>
    void add_pass(const char* name, Setup&& setup, Execute execute) {
        const auto index = static_cast<std::uint32_t>(passes_.size());
        passes_.push_back(Pass{name, {}, false, false, std::move(execute), {}, {}});
        PassBuilder builder(*this, index);
        setup(builder);
    }

    // V6 planning switches (both on by default; off: the V2 plan, for A/B and the checks).
    void set_transient(bool on) noexcept { transient_ = on; }
    void set_merging(bool on) noexcept { merging_ = on; }

    // Culls, assigns pooled images, validates and plans the barriers and merges. False (with the reason) on an
    // invalid graph or when an image cannot be made; nothing is recorded then.
    bool compile(TargetPool& pool, std::string* error);
    // Records the kept passes into `cmd`, each after its barriers, then the imported images' closing barriers; the
    // images remember their last use (Image::state) for the next frame. With `timer` (begun for this frame), a
    // timestamp after each rendering group, named after its passes.
    bool execute(VkCommandBuffer cmd, std::string* error, GpuTimer* timer = nullptr);

    // ---- introspection (checks, stats) -------------------------------------------------------------------------
    struct Barrier {
        std::uint32_t pass;  // pass_count() for the closing barriers of imported images
        ResourceId resource;
        ImageUse before;     // for a buffer: stage and access, no layout
        ImageUse after;
        bool discard;        // the earlier contents are not kept (old layout UNDEFINED)
    };
    struct Stats {
        std::uint32_t passes = 0;    // declared
        std::uint32_t kept = 0;
        std::uint32_t culled = 0;
        std::uint32_t textures = 0;  // graph-owned textures the kept passes use
        std::uint32_t images = 0;    // distinct pooled images they were given
        std::uint32_t aliased = 0;   // textures that took over an earlier texture's image
        std::uint32_t barriers = 0;  // image and buffer barriers planned
        std::uint32_t barrier_calls = 0;  // vkCmdPipelineBarrier2 calls (one per pass boundary that needs any)
        std::uint32_t transient = 0;      // textures given transient images
        std::uint32_t merged = 0;         // passes recorded inside the previous pass's rendering instance
        // Attachments a rendering instance stores at its end (a later pass uses them; on a tiler, a write to memory)
        // and, of those, depth attachments: 0 means every depth stayed in tile memory.
        std::uint32_t stores = 0;
        std::uint32_t depth_stores = 0;
    };
    const Stats& stats() const noexcept { return stats_; }
    std::vector<std::string> kept_passes() const;
    std::vector<std::string> culled_passes() const;
    // The kept passes grouped as recorded: a merged pass joins the group of the one before ("a + b").
    std::vector<std::string> groups() const;
    const std::vector<Barrier>& barriers() const noexcept { return barriers_; }
    // The physical image a texture got (null before compile, for a culled-only texture, or a buffer).
    const Image* image_of(ResourceId id) const noexcept;
    std::size_t pass_count() const noexcept { return passes_.size(); }

private:
    friend class PassBuilder;
    friend class PassContext;

    struct Use {
        ResourceId resource;
        Access access;
        Load load;
        bool write;
        VkClearValue clear;
    };
    struct Planned {  // a pass's barriers, as recorded
        std::vector<VkImageMemoryBarrier2> images;
        std::vector<VkBufferMemoryBarrier2> buffers;
    };
    struct Pass {
        std::string name;
        std::vector<Use> uses;
        bool side_effect;
        bool kept;
        Execute execute;
        Planned planned;
        std::vector<std::uint8_t> stored;  // per use: a later kept pass uses the resource (attachment store op)
        bool mergeable = false;
        bool continues = false;        // recorded inside the previous kept pass's rendering instance
        bool leaves_open = false;      // the next kept pass continues this one's instance
        std::uint32_t group_last = 0;  // the last pass of the instance this pass opens or continues
    };
    enum class Kind : std::uint8_t { Created, ImportedImage, ImportedBuffer };
    struct Resource {
        std::string name;
        Kind kind;
        TextureDesc desc;
        Image* image = nullptr;           // created (after compile) or imported image
        VkBuffer buffer = VK_NULL_HANDLE;  // imported buffer
        ImageUse buffer_state;             // imported buffer: its use at the frame's start (no layout)
        ImageUse buffer_final;
        bool has_final = false;
        ImageUse final_use;  // imported image
        bool used = false;   // by a kept pass
        bool aliased = false;
    };

    void add_use(std::uint32_t pass, ResourceId resource, Access access, Load load, bool write,
                 const VkClearValue& clear);
    bool fail(std::string* error, const std::string& message);

    std::uint64_t frame_ = 0;
    std::vector<Resource> resources_;
    std::vector<Pass> passes_;
    std::vector<Barrier> barriers_;
    Planned closing_;
    // The last use of every physical image after the frame (written back to Image::state by execute).
    std::vector<std::pair<Image*, ImageUse>> end_states_;
    Stats stats_;
    bool compiled_ = false;
    std::string build_error_;
    bool transient_ = true;
    bool merging_ = true;
    // execute: the rendering instance open, for a continuing pass's check.
    VkRect2D open_area_{};
    std::vector<VkImageView> open_views_;
    bool open_ = false;
};

}  // namespace shadow::render_vk
