// plume_d3d12.h brings in windows.h; keep its min/max macros out of std::min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "gekko/native_gpu.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <exception>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "gekko/camera.h"
#include "gekko/gx_texture_decode.h"
#include "gekko/render/gpu_transfer.h"
#include "gekko/render/native_gpu_lifetime.h"
#include "gekko/render/upload_arena.h"
#include "gekko/native_hud.h"
#include "gekko/native_failure_publication.h"
#include "gekko/profiling.h"
#include "gekko/rw_geometry_convert.h"

#if defined(SHADOW_ENABLE_PLUME)
#include "plume_d3d12.h"
#include "native_shader_ps.h"
#include "native_shader_ps_alpha.h"
#include "native_shader_ps_alpha_footprint.h"
#include "native_shader_ps_footprint.h"
#include "native_shader_vs.h"
#include <d3d12.h>
#endif

namespace gekko {

#if !defined(SHADOW_ENABLE_PLUME)

struct NativeGpu::Impl {};
NativeGpu::NativeGpu() : impl_(std::make_unique<Impl>()) {}
NativeGpu::~NativeGpu() = default;
bool NativeGpu::available() noexcept { return false; }
bool NativeGpu::create(const NativeGpuSettings&, std::string& error) {
    error = "plume (Direct3D 12) was not compiled in";
    return false;
}
void NativeGpu::destroy() {}
void NativeGpu::process(const native::FrameSegment&) {}
void NativeGpu::finish() {}
void NativeGpu::request_output_size(u32, u32, const DisplayLayout&, u32) {}
void NativeGpu::request_screenshot(const std::filesystem::path&, u32) {}
void NativeGpu::request_screenshot_at_tick(const std::filesystem::path&, u64) {}
NativeGpu::ScreenshotResult NativeGpu::screenshot_result() const { return {}; }
void* NativeGpu::device() const noexcept { return nullptr; }
bool NativeGpu::latest_output(OutputFrame&) const { return false; }
void* NativeGpu::present_queue() const noexcept { return nullptr; }
u64 NativeGpu::output_generation() const { return 0; }
bool NativeGpu::acquire_output(OutputFrame&) { return false; }
bool NativeGpu::acquire_output(u64, OutputFrame&) { return false; }
bool NativeGpu::output_produced_ns(u64, u64&) const { return false; }
void NativeGpu::release_output(u32, u64) {}
NativeGpu::Stats NativeGpu::stats() const { return {}; }
std::string NativeGpu::target_inventory() const { return {}; }
void NativeGpu::set_rejections(native::RejectionMailbox*) {}

#else

namespace {

using native::FrameSegment;
using native::Op;
static_assert(static_cast<u32>(Op::Count) < 64u,
              "constant dependency masks must cover every native opcode");

// plume's buffer, texture and graphics-pipeline factories return nullptr when
// the backend could not create the object (fixed in the backends, 2026-09-28).
// They used to return a live wrapper around a null native handle, which the
// renderer detected by casting to the D3D12 types; a null test is now the whole
// contract, on every backend.

constexpr u32 kSlots = 3;
constexpr u32 kBindlessCapacity = 16384;
constexpr u32 kTextureWhite = 0;
constexpr u32 kTextureMagenta = 1;
constexpr u32 kFirstDynamicTexture = 2;
// Immediate vertices + constants for one command-list slot. D3D12 upload
// resources consume 64 KiB allocation granules, so that is the useful minimum.
// Capacity grows from observed failed prefixes and never exceeds the old 8 MiB
// refusal boundary.
constexpr u64 kRingMinimumBytes = 64ull << 10;
constexpr u64 kRingMaximumBytes = 8ull << 20;
constexpr u32 kVertexStride = sizeof(shadow::render::RenderVertex);
constexpr u32 kUvOffset = offsetof(shadow::render::RenderVertex, uv);
constexpr u32 kUvSetBytes = sizeof(shadow::render::RenderVertex{}.uv[0]);
constexpr plume::RenderFormat kColorFormat = plume::RenderFormat::R8G8B8A8_UNORM;
constexpr plume::RenderFormat kDepthFormat = plume::RenderFormat::D32_FLOAT;
constexpr float kClearQuadDepth = 0.99999994f;

u64 now_ns() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

// Default-off fixed-storage CPU census. A disabled scope calls no clock;
// enabled timings include nested phase timing overhead and must not be summed.
struct CpuPhaseScope {
    NativeGpu::CpuPhaseCounter* counter = nullptr;
    u64 begin = 0;
    CpuPhaseScope(NativeGpu::Stats& stats, NativeGpu::CpuPhase phase,
                  bool enabled = true) noexcept {
        if (stats.cpu_profile_enabled && enabled) {
            counter = &stats.cpu_phases[static_cast<std::size_t>(phase)];
            begin = now_ns();
        }
    }
    void finish() noexcept {
        if (counter == nullptr) return;
        const u64 elapsed = now_ns() - begin;
        ++counter->calls;
        counter->ns += elapsed;
        counter->max_ns = std::max(counter->max_ns, elapsed);
        counter = nullptr;
    }
    ~CpuPhaseScope() { finish(); }
};

// ---- constant buffer layouts (must match native_shaders.hlsl) ----------------

struct DrawConstants {
    float pos_row[3][4];
    float nrm_row[3][4];
    float proj_a[4];
    float proj_b[4];
    float tex0_row[3][4];
    float tex1_row[3][4];
    u32 texgen0;
    u32 texgen1;
    u32 channel_count;
    u32 vertex_flags;
    u32 chan0_color;
    u32 chan0_alpha;
    u32 chan1_color;
    u32 chan1_alpha;
    float mat_color0[4];
    float amb_color0[4];
    float mat_color1[4];
    float amb_color1[4];
    float tev_reg0[4];
    float tev_reg1[4];
    float fog_color[4];
    float fog_params[4];
    u32 program;
    u32 alpha_compare;
    u32 texture0;
    u32 texture1;
    u32 sampler0;
    u32 sampler1;
    u32 fog_type;
    u32 blend_premultiply;
    float uv_transform0[4];
    float uv_transform1[4];
    float hud_affine[4];     // canvas x' = ax*x + bx, y' = ay*y + by
    float viewport_rect[4];  // guest viewport x, y, width, height
    u32 hud_enabled;
    // taps_x | taps_y << 8: native texels per guest texel of the resident
    // copy bound to map 0, used only by the footprint pixel-shader variants.
    u32 footprint_taps0;
    u32 pad2;
    u32 pad3;
};

// Exact semantic key for DrawConstants. Keeping the fields separate avoids the
// collisions the old XOR identity admitted and makes each non-GX input explicit.
struct DrawIdentity {
    u64 state_generation = 0;
    u64 texture_resolution_generation = 0;
    u32 content_aspect_bits = 0;
    u32 program = 0;
    u32 vertex_flags = 0;
    u32 premultiply = 0;
    u32 footprint_taps0 = 0;
    u32 hud_enabled = 0;
    std::array<u32, 4> hud_affine{};

    bool operator==(const DrawIdentity& other) const noexcept {
        return state_generation == other.state_generation &&
               texture_resolution_generation == other.texture_resolution_generation &&
               content_aspect_bits == other.content_aspect_bits && program == other.program &&
               vertex_flags == other.vertex_flags && premultiply == other.premultiply &&
               footprint_taps0 == other.footprint_taps0 && hud_enabled == other.hud_enabled &&
               hud_affine == other.hud_affine;
    }
};

u32 write_immediate_indices(u8 primitive, u32 count, u32* indices) noexcept {
    u32 written = 0;
    switch (primitive) {
        case 0:
        case 1:
            for (u32 q = 0; q + 3u < count; q += 4u) {
                indices[written++] = q;
                indices[written++] = q + 1u;
                indices[written++] = q + 2u;
                indices[written++] = q;
                indices[written++] = q + 2u;
                indices[written++] = q + 3u;
            }
            break;
        case 3:
            for (u32 i = 2; i < count; ++i) {
                indices[written++] = (i & 1u) == 0u ? i - 2u : i - 1u;
                indices[written++] = (i & 1u) == 0u ? i - 1u : i - 2u;
                indices[written++] = i;
            }
            break;
        case 4:
            for (u32 i = 2; i < count; ++i) {
                indices[written++] = 0u;
                indices[written++] = i - 1u;
                indices[written++] = i;
            }
            break;
        case 5:
            for (u32 i = 0; i + 1u < count; i += 2u) {
                indices[written++] = i;
                indices[written++] = i + 1u;
            }
            break;
        case 6:
            for (u32 i = 1; i < count; ++i) {
                indices[written++] = i - 1u;
                indices[written++] = i;
            }
            break;
        default:
            for (u32 i = 0; i < count; ++i) indices[written++] = i;
            break;
    }
    return written;
}

u64 immediate_index_count(u8 primitive, u32 count) noexcept {
    switch (primitive) {
        case 0:
        case 1: return static_cast<u64>(count / 4u) * 6u;
        case 2: return 0u;
        case 3:
        case 4: return count >= 3u ? static_cast<u64>(count - 2u) * 3u : 0u;
        case 5: return static_cast<u64>(count / 2u) * 2u;
        case 6: return count >= 2u ? static_cast<u64>(count - 1u) * 2u : 0u;
        default: return count;
    }
}

struct ImmediateIndexWriteContext {
    u8 primitive = 0;
    u32 vertices = 0;
};

void fill_immediate_indices(void* destination, u64, void* opaque) {
    const auto* context = static_cast<const ImmediateIndexWriteContext*>(opaque);
    write_immediate_indices(context->primitive, context->vertices,
                            static_cast<u32*>(destination));
}

struct LightConstants {
    float color[8][4];
    float position[8][4];
    float direction[8][4];
    float cos_attenuation[8][4];
    float dist_attenuation[8][4];
};

struct PackedVertexWriteContext {
    const NativeVertex* source = nullptr;
    std::size_t count = 0;
};

void fill_packed_vertices(void* destination, u64 bytes, void* opaque) {
    const auto* context = static_cast<const PackedVertexWriteContext*>(opaque);
    auto* packed = static_cast<shadow::render::RenderVertex*>(destination);
    for (std::size_t index = 0; index < context->count; ++index) {
        const NativeVertex& in = context->source[index];
        shadow::render::RenderVertex& out = packed[index];
        std::memcpy(out.position, in.pos, sizeof(out.position));
        std::memcpy(out.normal, in.nrm, sizeof(out.normal));
        std::memcpy(out.color0, in.rgba, sizeof(out.color0));
        std::memset(out.color1, 255, sizeof(out.color1));
        out.uv[0][0] = in.uv[0][0];
        out.uv[0][1] = in.uv[0][1];
        out.uv[1][0] = in.uv[1][0];
        out.uv[1][1] = in.uv[1][1];
    }
    if (context->count == 0u && bytes != 0u) std::memset(destination, 0, 1u);
}

// ---- GX state as replayed from the command stream ----------------------------

struct ChannelState {
    u32 enable = 0, ambient_source = 0, material_source = 0, light_mask = 0, diffuse = 0,
        attenuation = 2;
};

struct GxState {
    native::Viewport viewport{};
    native::Scissor scissor{};
    native::ScissorOffset scissor_offset{};
    native::Projection projection{};
    std::array<std::array<float, 12>, 128> position_matrix{};
    std::array<std::array<float, 9>, 128> normal_matrix{};
    std::array<std::array<float, 12>, 128> texture_matrix{};
    u32 current_matrix = 0;
    native::Blend blend{1, 4, 5, 0};
    native::DepthMode depth{1, 3, 1, 0};
    u32 depth_before_texture = 1;
    native::AlphaCompare alpha{7, 0, 0, 7, 0, {}};
    // Derived from `alpha` when it is applied, not per draw: true when some
    // alpha value fails the comparison. The default record {ALWAYS, AND,
    // ALWAYS} passes every value, hence false. Lives here so the
    // `state = GxState{}` of a savestate reset clears it with its source.
    bool alpha_test_active = false;
    u32 cull = 2;
    native::Fog fog{};
    u32 color_update = 1;
    u32 alpha_update = 1;
    u32 channel_count = 0;
    std::array<ChannelState, 4> channel{};  // COLOR0, COLOR1, ALPHA0, ALPHA1
    std::array<std::array<u8, 4>, 2> material{};
    std::array<std::array<u8, 4>, 2> ambient{};
    std::array<native::Light, 8> light{};
    u64 light_generation = 1;
    u32 texgen_count = 0;
    std::array<native::TexGen, 8> texgen{};
    std::array<std::array<s16, 4>, 4> tev_color{};
    std::array<native::BindTexture, 8> bind{};
    u64 serial = 1;
    // Advances only when a value consumed by DrawConstants actually changes.
    // Pipeline, scissor and light state have independent caches/owners.
    u64 draw_generation = 1;
    static_assert(static_cast<u32>(native::Op::Count) <= 64u);
    u64 draw_dirty_ops = 0;
    u64 draw_changed_ops = 0;

    GxState() {
        for (auto& m : position_matrix) m = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        for (auto& m : normal_matrix) m = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        for (auto& m : texture_matrix) m = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        viewport.width = 640;
        viewport.height = 480;
        viewport.far_z = 1;
        scissor.width = 640;
        scissor.height = 480;
        for (auto& c : material) c = {255, 255, 255, 255};
    }
};

struct PipelineKey {
    u8 topology = 0;     // 0 triangles, 1 lines, 2 points
    u8 alpha_test = 0;
    u8 footprint = 0;
    u8 blend_type = 0;
    u8 blend_src = 0;
    u8 blend_dst = 0;
    u8 depth_enable = 0;
    u8 depth_func = 0;
    u8 depth_write = 0;
    u8 cull = 0;
    u8 color_write = 0;
    u64 pack() const noexcept {
        return static_cast<u64>(topology) | static_cast<u64>(alpha_test) << 4 |
               static_cast<u64>(blend_type) << 8 | static_cast<u64>(blend_src) << 12 |
               static_cast<u64>(blend_dst) << 16 | static_cast<u64>(depth_enable) << 20 |
               static_cast<u64>(depth_func) << 24 | static_cast<u64>(depth_write) << 28 |
               static_cast<u64>(cull) << 32 | static_cast<u64>(color_write) << 36 |
               static_cast<u64>(footprint) << 40;
    }
};

plume::RenderComparisonFunction compare_function(u32 gx) {
    static constexpr plume::RenderComparisonFunction kMap[8] = {
        plume::RenderComparisonFunction::NEVER,         plume::RenderComparisonFunction::LESS,
        plume::RenderComparisonFunction::EQUAL,         plume::RenderComparisonFunction::LESS_EQUAL,
        plume::RenderComparisonFunction::GREATER,       plume::RenderComparisonFunction::NOT_EQUAL,
        plume::RenderComparisonFunction::GREATER_EQUAL, plume::RenderComparisonFunction::ALWAYS};
    return kMap[gx & 7u];
}

// GX blend factors. Slots 2/3 alias to destination colour in the source slot
// and source colour in the destination slot. The EFB is RGB8_Z24, so its alpha
// reads as 1: DSTALPHA is ONE and INVDSTALPHA is ZERO.
plume::RenderBlend blend_factor(u32 gx, bool source) {
    switch (gx & 7u) {
        case 0: return plume::RenderBlend::ZERO;
        case 1: return plume::RenderBlend::ONE;
        case 2: return source ? plume::RenderBlend::DEST_COLOR : plume::RenderBlend::SRC_COLOR;
        case 3: return source ? plume::RenderBlend::INV_DEST_COLOR : plume::RenderBlend::INV_SRC_COLOR;
        case 4: return plume::RenderBlend::SRC_ALPHA;
        case 5: return plume::RenderBlend::INV_SRC_ALPHA;
        case 6: return plume::RenderBlend::ONE;
        default: return plume::RenderBlend::ZERO;
    }
}

ID3D12Resource* d3d_texture(plume::RenderTexture* texture) {
    return texture != nullptr ? static_cast<plume::D3D12Texture*>(texture)->d3d : nullptr;
}

ID3D12Resource* d3d_buffer(plume::RenderBuffer* buffer) {
    return buffer != nullptr ? static_cast<plume::D3D12Buffer*>(buffer)->d3d : nullptr;
}

bool alpha_test_passes_everything(const native::AlphaCompare& a) {
    const auto compare = [](u32 function, u32 alpha, u32 reference) {
        switch (function & 7u) {
            case 0: return false;
            case 1: return alpha < reference;
            case 2: return alpha == reference;
            case 3: return alpha <= reference;
            case 4: return alpha > reference;
            case 5: return alpha != reference;
            case 6: return alpha >= reference;
            default: return true;
        }
    };
    for (u32 alpha = 0; alpha < 256u; ++alpha) {
        const bool x = compare(a.comp0, alpha, a.ref0);
        const bool y = compare(a.comp1, alpha, a.ref1);
        bool pass = false;
        switch (a.op & 3u) {
            case 0: pass = x && y; break;
            case 1: pass = x || y; break;
            case 2: pass = x != y; break;
            default: pass = x == y; break;
        }
        if (!pass) return false;
    }
    return true;
}

struct Deferred {
    std::unique_ptr<plume::RenderTexture> texture;
    std::unique_ptr<plume::RenderBuffer> buffer_a;
    std::unique_ptr<plume::RenderBuffer> buffer_b;
    u32 bindless = 0;
};

constexpr u64 kDrawConstantUploadBytes = 256u * ((sizeof(DrawConstants) + 255u) / 256u);

struct Slot {
    std::unique_ptr<plume::RenderCommandList> list;
    std::unique_ptr<plume::RenderCommandFence> fence;
    std::unique_ptr<plume::RenderQueryPool> timer;
    render::UploadArena ring;
    std::vector<Deferred> releases;
    // Superseded resident immediate-index patterns used by commands already
    // recorded in this slot. Cleared only after the slot fence retires.
    std::vector<std::unique_ptr<plume::RenderBuffer>> pattern_releases;
    // COPY-queue semaphores this slot's submission waits on; handed back to the
    // transfer queue when the slot retires (render::TransferQueue::retire).
    std::vector<plume::RenderCommandSemaphore*> transfer_waits;
    bool in_flight = false;
    bool timed = false;
    // Readback of a screenshot recorded into this frame.
    std::unique_ptr<plume::RenderBuffer> readback;
    u32 readback_width = 0, readback_height = 0, readback_row = 0;
    u32 readback_frame = 0;
    // A development-link request (NativeGpu::request_screenshot): its file; empty for --native-screenshot.
    std::filesystem::path readback_path;
    u64 readback_game_ticks = 0;
};

struct TextureEntry {
    u32 generation = 0;
    std::unique_ptr<plume::RenderTexture> texture;
    u32 bindless = 0;
    u32 width = 0, height = 0;
};

struct CameraTarget {
    std::unique_ptr<plume::RenderTexture> texture;
    u32 bindless = 0;
    u32 guest_width = 0, guest_height = 0;
    u32 native_width = 0, native_height = 0;
};

// A converted RenderWare instance, owned by its CONTENT - not by the guest
// cache entry that delivered it.
//
// The guest instances geometry into RenderWare's resource arena, a 256 KiB
// console heap that evicts least-recently-used entries whenever a new instance
// does not fit (RwResourcesAllocateResEntry 0x804818C8). Mirroring that entry
// lifetime on the GPU meant a conversion, two buffer creations, a COPY-queue
// submit, a cross-queue wait and a deferred release every time the guest
// re-instanced a model it had evicted - 7,392 times for 116 distinct contents
// on Death Ruins run-forward. An asset is created once per distinct content and
// stays resident while the device has room for it; guest entries bind to it.
struct GeometryAsset {
    u64 identity = 0;
    // The conversion's inputs, verbatim: every byte rw_convert_instance consumed
    // ([0, ConvertedGeometry::source_extent) of the normalised payload) and the
    // parameters that choose its vertex format and options. The converter reads
    // nothing else, so equal inputs convert to equal output; that equality is
    // what a hit is, not the identity hash.
    shadow::memory::Vector<u8, shadow::memory::Domain::Assets> source;
    u32 geometry_flags = 0;
    u32 world_sector = 0;
    u32 mesh_count = 0;
    u32 dynamic = 0;
    u32 matrix_palette = 0;
    std::array<u8, 24> vertex_format{};
    // Static assets own their vertices. Dynamic (CPU-skinned) assets own only
    // their indices: each draw's vertices are expanded from that frame's pose
    // into the frame's upload ring and bound from there.
    std::unique_ptr<plume::RenderBuffer> vertices;
    std::unique_ptr<plume::RenderBuffer> indices;
    u32 vertex_count = 0;
    u32 index_bytes = 0;
    shadow::memory::Vector<ConvertedMesh, shadow::memory::Domain::Assets> meshes;
    bool has_normals = false, has_colors = false;
    // Dynamic only: the converted template each frame's POS/NRM is expanded through.
    std::unique_ptr<ConvertedGeometry> cpu;
    u64 gpu_bytes = 0;
    u32 instances = 0;        // guest entries bound to this asset right now
    u32 last_used_frame = 0;  // eviction order among unreferenced assets
};

// One guest RwResEntry: an instance-cache slot bound to an asset.
struct GeometryEntry {
    u32 generation = 0;
    GeometryAsset* asset = nullptr;
    // Dynamic assets: this frame's expanded vertices, in the upload ring of the
    // list that recorded them (valid for this frame only; see draw_mesh).
    const plume::RenderBuffer* dynamic_buffer = nullptr;
    u64 dynamic_offset = 0;
    u32 dynamic_bytes = 0;
    bool dynamic_bound = false;
    u32 dynamic_frame = 0;
};

struct ImmediateIndexPattern {
    std::unique_ptr<plume::RenderBuffer> buffer;
    u32 index_capacity = 0;
    u32 bytes = 0;
};

struct OutputSlot {
    std::unique_ptr<plume::RenderTexture> texture;
    std::unique_ptr<plume::RenderCommandSemaphore> semaphore;
    u32 width = 0, height = 0;
    u32 guest_frame = 0;
    u64 produced_ns = 0;
    u64 generation = 0;
    // Presenter pins: a slot is never rewritten while a present that reads it
    // may still be queued. Guarded by output_mutex.
    u32 pins = 0;
};

}  // namespace

struct NativeGpu::Impl {
    NativeGpuSettings settings;
    // NativeGpu::request_screenshot: armed from any thread, served on the render thread.
    mutable std::mutex runtime_shot_mutex;
    std::filesystem::path runtime_shot_path;
    u32 runtime_shot_frame = 0;  // 0: none pending
    u64 runtime_shot_tick = 0;   // 0: none pending (request_screenshot_at_tick)
    u64 runtime_shots_written = 0;
    u32 runtime_shot_last_frame = 0;
    u64 runtime_shot_last_game_ticks = 0;
    std::filesystem::path runtime_shot_last_path;
    // The capture's reverse channel; see RejectionMailbox. Null is legal and
    // means refusals are counted but cannot be recovered from.
    native::RejectionMailbox* rejections = nullptr;
    std::unique_ptr<plume::RenderInterface> render_interface;
    std::unique_ptr<plume::RenderDevice> device;
    std::unique_ptr<plume::RenderCommandQueue> queue;

    std::unique_ptr<plume::RenderShader> vertex_shader;
    std::unique_ptr<plume::RenderShader> pixel_shader;
    std::unique_ptr<plume::RenderShader> pixel_shader_alpha;
    std::unique_ptr<plume::RenderShader> pixel_shader_footprint;
    std::unique_ptr<plume::RenderShader> pixel_shader_alpha_footprint;
    std::vector<std::unique_ptr<plume::RenderSampler>> samplers;
    std::unique_ptr<plume::RenderPipelineLayout> layout;
    std::unique_ptr<plume::RenderDescriptorSet> texture_set;
    std::unique_ptr<plume::RenderDescriptorSet> sampler_set;
    std::unordered_map<u64, std::unique_ptr<plume::RenderPipeline>> pipelines;

    std::unique_ptr<plume::RenderTexture> efb_color;
    std::unique_ptr<plume::RenderTexture> efb_depth;
    std::unique_ptr<plume::RenderFramebuffer> efb_framebuffer;
    // The one authoritative internal render scale. EfbGeometry carries it as
    // an INTEGER (RenderScale::factor), so it is isotropic by type and nothing
    // downstream can recompute a dimension from a literal or from the output
    // rectangle. The display aspect is applied at present time, not here.
    EfbGeometry efb{};
    u32 efb_width = 0, efb_height = 0;   // == efb.native_width()/native_height()
    // Texels of map 0 per guest pixel for the draw being prepared; see
    // measure_texel_density. Reset per draw, 0 when unmeasurable.
    float texel_density = 0.0f;
    // --no-effect-footprint: point-sample resident copies as before, so the
    // reconstruction can be A/B'd on one binary. Never an automatic fallback.
    bool effect_footprint = true;
    // --no-logical-pixel-snap: the raster-border snap below. Same option the
    // legacy raster takes, so there is one control, not two.
    bool logical_pixel_snap = true;

    std::unique_ptr<plume::RenderTexture> white_texture;
    std::unique_ptr<plume::RenderTexture> magenta_texture;
    std::vector<u32> free_bindless;
    u32 bindless_next = kFirstDynamicTexture;

    std::array<Slot, kSlots> slots;
    u32 slot_index = 0;
    Slot* slot = nullptr;
    u32 split_guard = 0;
    bool recording = false;
    // At most one prefix-growing resident pattern per GX primitive. Immediate
    // expansion indices are a pure function of (primitive, vertex_count), and
    // every larger pattern extends the smaller one. This replaces tens of
    // thousands of per-draw generations/uploads with eight bounded owners.
    std::array<ImmediateIndexPattern, 8> immediate_index_patterns;

    std::unordered_map<u32, TextureEntry> textures;
    std::unordered_map<u32, CameraTarget> targets;
    std::unordered_map<u32, GeometryEntry> geometry;
    // The persistent store: identity -> assets. More than one asset under an
    // identity only after a verified conflict (equal hash, different bytes).
    std::unordered_map<u64, std::vector<std::unique_ptr<GeometryAsset>>> geometry_assets;

    std::vector<std::unique_ptr<OutputSlot>> outputs;
    std::unique_ptr<plume::RenderCommandQueue> present_queue;
    // Uploads into resources no GPU work has touched yet, on the COPY queue,
    // through the RHI alone (render/gpu_transfer.h). Replaces hw::GpuRuntime,
    // a raw D3D12 copy queue beside plume with a second, pointer-keyed
    // resource-state table.
    render::TransferQueue transfer;
    bool transfer_live = false;
    std::vector<plume::RenderTexture*> pending_shader_read;
    std::vector<plume::RenderBuffer*> pending_buffer_read;
    // The one genuine cross-thread rendezvous in this file: the render thread
    // publishes under it in present(), the presenter thread acquires, releases
    // and reads the latest frame under it. Declared instrumented so the
    // contention is visible; every taker uses class template argument
    // deduction, so no call site changes and none may be written as
    // std::lock_guard<std::mutex>.
    mutable GEKKO_PROFILE_LOCKABLE(std::mutex, output_mutex);
    NativeGpu::OutputFrame latest{};
    u64 output_generation = 0;

    GxState state;
    bool retirement_failed = false;
    // Recording caches (reset per command list).
    const plume::RenderPipeline* bound_pipeline = nullptr;
    native::Viewport bound_viewport{};
    bool viewport_valid = false;
    native::Scissor bound_scissor{};
    bool scissor_valid = false;
    std::array<u8, sizeof(DrawConstants)> last_constants{};
    bool constants_valid = false;
    bool lights_valid = false;
    DrawIdentity last_draw_identity{};
    // One retained source snapshot; never owns an additional upload allocation.
    native::Projection last_constant_projection{};
    native::Fog last_constant_fog{};
    u64 texture_resolution_generation = 1;
    bool validate_constant_restoration = false;
    native::Projection cpu_last_perspective_key{};
    float cpu_last_perspective_aspect = 0;
    bool cpu_perspective_key_valid = false;
    plume::RenderBufferReference last_constants_ref{};
    u64 bound_light_generation = 0;
    const plume::RenderBuffer* bound_vertex_buffer = nullptr;
    u64 bound_vertex_offset = ~0ull;
    u32 bound_vertex_size = 0u;
    const plume::RenderBuffer* bound_index_buffer = nullptr;
    u64 bound_index_offset = ~0ull;
    u32 bound_index_size = 0u;
    u32 frame_ordinal = 0;
    u32 debug_copy_index = 0;

    // The live bloom parameter block, refreshed at every glare pass boundary.
    native::GlareWindow glare{};
    bool glare_seen = false;

    // Widescreen 2D layout for the draw being prepared (draw_immediate sets it).
    NativeHudLayout hud;
    bool draw_hud = false;
    HudAffine draw_affine{};

    // Output size requested by the window thread, applied at a present.
    std::mutex resize_mutex;
    bool resize_pending = false;
    u32 resize_width = 0, resize_height = 0;
    // 0 = auto (re-derive from the rectangle); non-zero pins an explicit
    // --internal-scale across every resize.
    u32 resize_internal_scale = 0;
    DisplayLayout resize_layout{};

    // Immediate vertices rewritten by the raster-border snap, and the window
    // positions and deltas it works from. Only filled when a draw actually
    // touches the raster border.
    shadow::memory::Vector<u8, shadow::memory::Domain::Scratch> immediate_scratch;
    shadow::memory::Vector<float, shadow::memory::Domain::Scratch> snap_scratch;
    shadow::memory::Vector<u8, shadow::memory::Domain::Scratch> rgba_scratch;
    ConvertedGeometry convert_scratch;

    mutable std::mutex stats_mutex;
    NativeGpu::Stats stats;

    // ---- profiling -----------------------------------------------------------
    //
    // The DIRECT queue's GPU timestamp context. The legacy raster has its own
    // (plume_gx_raster.cpp) for its own queues; this path had none, so nothing
    // the device did here was ever timed by the device itself. Declared
    // unconditionally because the macro is `void*` without Tracy, and null
    // until create() reaches the context - every use is guarded on that.
    GEKKO_PROFILE_GPU_CONTEXT_TYPE tracy_gpu = nullptr;
    // Previous-frame values of the cumulative counters that publish_frame_plots
    // republishes as per-frame deltas, and the flags that make the one-time
    // work one-time. `plot_baseline` is what stops the first frame after a
    // mid-run attach from publishing the whole session as one frame's work.
    bool plots_configured = false;
    bool plot_baseline = false;
    bool last_error_reported = false;
    u64 plot_draws = 0, plot_immediate_draws = 0, plot_mesh_draws = 0, plot_triangles = 0;
    u64 plot_clears = 0, plot_copies = 0, plot_barriers = 0, plot_barriers_elided = 0;
    u64 plot_constant_uploads = 0, plot_constant_reuses = 0, plot_dynamic_updates = 0;
    u64 plot_footprint_draws = 0, plot_border_snap_draws = 0;
    u64 plot_textures_created = 0, plot_geometry_created = 0, plot_upload_bytes = 0;

    // ---- creation ------------------------------------------------------------

    bool create(std::string& error) {
        const char* validate_constants = std::getenv("GEKKO_NATIVE_CONSTANT_VALIDATE");
        validate_constant_restoration = validate_constants != nullptr &&
                                       std::strcmp(validate_constants, "1") == 0;
        const char* cpu_profile = std::getenv("GEKKO_NATIVE_CPU_PROFILE");
        stats.cpu_profile_enabled = cpu_profile != nullptr && std::strcmp(cpu_profile, "1") == 0;
        if (stats.cpu_profile_enabled) {
            // Report clock-pair cost; do not subtract it from phase timings.
            for (u32 i = 0; i < 4096u; ++i) {
                const u64 begin = now_ns();
                stats.cpu_profile_clock_pair_ns += now_ns() - begin;
                ++stats.cpu_profile_clock_pair_calls;
            }
        }
        auto d3d12 = std::make_unique<plume::D3D12Interface>();
        if (!d3d12->isValid()) {
            error = "Direct3D 12 interface unavailable";
            return false;
        }
        render_interface = std::move(d3d12);
        device = render_interface->createDevice();
        if (device == nullptr) {
            error = "no Direct3D 12 device";
            return false;
        }
        queue = device->createCommandQueue(plume::RenderCommandListType::DIRECT);
        if (queue == nullptr) {
            error = "no DIRECT queue";
            return false;
        }
        vertex_shader = device->createShader(kNativeVSMain, sizeof(kNativeVSMain), "VSMain",
                                             plume::RenderShaderFormat::DXIL);
        pixel_shader = device->createShader(kNativePSMain, sizeof(kNativePSMain), "PSMain",
                                            plume::RenderShaderFormat::DXIL);
        pixel_shader_alpha = device->createShader(kNativePSMainAlphaTest,
                                                  sizeof(kNativePSMainAlphaTest),
                                                  "PSMainAlphaTest",
                                                  plume::RenderShaderFormat::DXIL);
        pixel_shader_footprint =
            device->createShader(kNativePSMainFootprint, sizeof(kNativePSMainFootprint),
                                 "PSMainFootprint", plume::RenderShaderFormat::DXIL);
        pixel_shader_alpha_footprint = device->createShader(
            kNativePSMainAlphaTestFootprint, sizeof(kNativePSMainAlphaTestFootprint),
            "PSMainAlphaTestFootprint", plume::RenderShaderFormat::DXIL);
        if (!vertex_shader || !pixel_shader || !pixel_shader_alpha ||
            !pixel_shader_footprint || !pixel_shader_alpha_footprint) {
            error = "shader creation failed";
            return false;
        }
        // 36 samplers: min {near, linear} x mag {near, linear} x wrap_s x wrap_t.
        static constexpr plume::RenderTextureAddressMode kWrap[3] = {
            plume::RenderTextureAddressMode::CLAMP, plume::RenderTextureAddressMode::WRAP,
            plume::RenderTextureAddressMode::MIRROR};
        for (u32 min = 0; min < 2u; ++min) {
            for (u32 mag = 0; mag < 2u; ++mag) {
                for (u32 s = 0; s < 3u; ++s) {
                    for (u32 t = 0; t < 3u; ++t) {
                        plume::RenderSamplerDesc desc;
                        desc.minFilter = min != 0u ? plume::RenderFilter::LINEAR : plume::RenderFilter::NEAREST;
                        desc.magFilter = mag != 0u ? plume::RenderFilter::LINEAR : plume::RenderFilter::NEAREST;
                        desc.mipmapMode = plume::RenderMipmapMode::NEAREST;
                        desc.addressU = kWrap[s];
                        desc.addressV = kWrap[t];
                        desc.addressW = plume::RenderTextureAddressMode::CLAMP;
                        desc.maxLOD = 0.0f;
                        samplers.push_back(device->createSampler(desc));
                    }
                }
            }
        }
        const plume::RenderDescriptorRange texture_ranges[] = {
            plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::TEXTURE, 0, kBindlessCapacity)};
        const plume::RenderDescriptorSetDesc texture_set_desc(texture_ranges, 1, true, kBindlessCapacity);
        const plume::RenderDescriptorRange sampler_ranges[] = {
            plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::SAMPLER, 0, 36)};
        const plume::RenderDescriptorSetDesc sampler_set_desc(sampler_ranges, 1);
        const plume::RenderDescriptorSetDesc set_descs[] = {texture_set_desc, sampler_set_desc};
        const plume::RenderRootDescriptorDesc root_descs[] = {
            plume::RenderRootDescriptorDesc(0, 2, plume::RenderRootDescriptorType::CONSTANT_BUFFER),
            plume::RenderRootDescriptorDesc(1, 2, plume::RenderRootDescriptorType::CONSTANT_BUFFER)};
        plume::RenderPipelineLayoutDesc layout_desc;
        layout_desc.descriptorSetDescs = set_descs;
        layout_desc.descriptorSetDescsCount = 2;
        layout_desc.rootDescriptorDescs = root_descs;
        layout_desc.rootDescriptorDescsCount = 2;
        layout_desc.allowInputLayout = true;
        layout = device->createPipelineLayout(layout_desc);
        texture_set = device->createDescriptorSet(texture_set_desc);
        sampler_set = device->createDescriptorSet(sampler_set_desc);
        if (!layout || !texture_set || !sampler_set) {
            error = "pipeline layout / descriptor sets";
            return false;
        }
        for (u32 index = 0; index < 36u; ++index) {
            sampler_set->setSampler(index, samplers[index].get());
        }

        effect_footprint = settings.effect_footprint;
        logical_pixel_snap = settings.logical_pixel_snap;
        if (!create_efb(resolve_internal_scale(settings.internal_scale, settings.hud_layout))) {
            error = "EFB targets";
            return false;
        }
        stats.output_width = settings.output_width;
        stats.output_height = settings.output_height;

        for (Slot& s : slots) {
            s.list = queue->createCommandList();
            s.fence = device->createCommandFence();
            s.timer = device->createQueryPool(2);
            if (!s.list || !s.fence) {
                error = "command lists";
                return false;
            }
            render::UploadArena::Config ring_config;
            ring_config.minimum_chunk_bytes = kRingMinimumBytes;
            ring_config.max_bytes = kRingMaximumBytes;
            ring_config.vertex = true;
            ring_config.index = true;
            ring_config.constant = true;
            if (!s.ring.create(*device, ring_config, error)) {
                error = "upload ring";
                return false;
            }
        }
        present_queue = device->createCommandQueue(plume::RenderCommandListType::DIRECT);

        {
            render::TransferQueue::Config transfer_config;
            transfer_config.staging_bytes = kRingMaximumBytes;
            transfer_config.batches = 2;
            std::string transfer_error;
            transfer_live = transfer.create(*device, transfer_config, transfer_error);
            if (!transfer_live && stats.last_error.empty()) {
                stats.last_error = "transfer queue: " + transfer_error;
            }
        }

        // Device timestamps for the DIRECT queue. Created here, on the caller's
        // thread and before the render thread exists, so every list recorded
        // later already has a context to write into. The name is guarded
        // because TracyD3D12ContextName dereferences the handle without
        // checking it.
        tracy_gpu = GEKKO_PROFILE_GPU_CONTEXT(static_cast<plume::D3D12Device*>(device.get())->d3d,
                                              static_cast<plume::D3D12CommandQueue*>(queue.get())->d3d);
        if (tracy_gpu != nullptr) {
            GEKKO_PROFILE_GPU_CONTEXT_NAME(tracy_gpu, "native direct", 13);
        }

        // Fixed bindless entries: white (unbound / NULL raster) and magenta
        // (missing content - a visible coverage error, never silent).
        white_texture = create_solid_texture(255, 255, 255, 255);
        magenta_texture = create_solid_texture(255, 0, 255, 255);
        texture_set->setTexture(kTextureWhite, white_texture.get(), plume::RenderTextureLayout::SHADER_READ);
        texture_set->setTexture(kTextureMagenta, magenta_texture.get(), plume::RenderTextureLayout::SHADER_READ);
        return true;
    }

    // The EFB backing for the guest's 640x480 raster inside its 640x528 frame
    // buffer, at internal scale `scale`. Only called with no command list in
    // flight or recording.
    //
    // The output rectangle is deliberately NOT an input: it used to select both
    // dimensions independently (width/640 and height/480), which folded the
    // display aspect into the pixel grid and produced two disagreeing
    // fractional factors. The presenter fits 640S x 480S into whatever the
    // window is (display.cpp fit_content_rect), which is where an aspect
    // belongs.
    // Nothing here writes the live EFB description until the device has
    // actually produced the textures. The dimensions are what every clamp in
    // copy(), clear() and present() is bounded by, and what the exit report
    // states; advancing them and then failing would leave the renderer
    // clamping against a target that does not exist and reporting an EFB the
    // device refused.
    bool create_efb(u32 scale) {
        // Colour + depth at 640S x 528S: ~40 MiB in two resources at scale 3,
        // the largest VRAM item the renderer owns and the only one that grows
        // with the square of the internal scale.
        GEKKO_PROFILE_ZONE("gpu/efb-create");
        const EfbGeometry wanted{RenderScale{std::clamp(scale, 1u, kMaxInternalScale)}};
        const u32 width = wanted.native_width();
        const u32 height = wanted.native_height();
        GEKKO_PROFILE_VALUE(width);
        const plume::RenderClearValue color_clear =
            plume::RenderClearValue::Color(plume::RenderColor(0.0f, 0.0f, 0.0f, 1.0f), kColorFormat);
        const plume::RenderClearValue depth_clear =
            plume::RenderClearValue::Depth(plume::RenderDepth(1.0f), kDepthFormat);
        efb_framebuffer.reset();
        // The previous pair dies at the assignment below, so the free is
        // announced first: the allocator reuses addresses, and a new target
        // landing on one Tracy still believes is live reads as a double
        // allocation. The EFB never passes through the deferred-release path,
        // which is why it needs its own pool and its own free sites.
        GEKKO_PROFILE_ONLY(
            if (d3d_texture(efb_color.get()) != nullptr) GEKKO_PROFILE_FREE(efb_color.get(), "gpu/efb");
            if (d3d_texture(efb_depth.get()) != nullptr) GEKKO_PROFILE_FREE(efb_depth.get(), "gpu/efb");
        )
        efb_color = device->createTexture(plume::RenderTextureDesc::ColorTarget(
            width, height, kColorFormat, plume::RenderMultisampling(), &color_clear));
        efb_depth = device->createTexture(plume::RenderTextureDesc::DepthTarget(
            width, height, kDepthFormat, plume::RenderMultisampling(), &depth_clear));
        if (efb_color == nullptr || efb_depth == nullptr) {
            efb_color.reset();
            efb_depth.reset();
            return false;
        }
        GEKKO_PROFILE_ALLOC(efb_color.get(), static_cast<u64>(width) * height * 4u, "gpu/efb");
        GEKKO_PROFILE_ALLOC(efb_depth.get(), static_cast<u64>(width) * height * 4u, "gpu/efb");
        efb = wanted;
        efb_width = width;
        efb_height = height;
        const plume::RenderTexture* color_attachment = efb_color.get();
        plume::RenderFramebufferDesc framebuffer_desc(&color_attachment, 1, efb_depth.get());
        efb_framebuffer = device->createFramebuffer(framebuffer_desc);
        if (efb_framebuffer == nullptr) return false;
        stats.internal_scale = efb.scale.factor;
        stats.efb_width = efb_width;
        stats.efb_height = efb_height;
        return true;
    }

    // The internal scale the settings ask for: explicit when non-zero,
    // otherwise the smallest integer that does not magnify the guest raster
    // inside the fitted presentation rectangle. One function, so the startup
    // path and every later resize cannot disagree.
    u32 resolve_internal_scale(u32 requested, const DisplayLayout& layout) const {
        if (requested != 0u) return std::clamp(requested, 1u, kMaxInternalScale);
        return recommended_internal_scale(layout, kMaxInternalScale);
    }

    // Applies a pending output rectangle between two frames (after a present's
    // submit, before the next list begins).
    //
    // A resize changes two independent things. The Hor+ content aspect and the
    // 2D layout are properties of the OUTPUT rectangle and always follow it.
    // The EFB is a property of the INTERNAL SCALE and is rebuilt only when that
    // integer changes - so dragging a window edge across a few dozen pixels no
    // longer reallocates every render target. Camera textures follow lazily:
    // the next copy into each recreates it at the new scale, and until then
    // draws sample the old one through normalised coordinates.
    void apply_output_resize() {
        u32 width = 0, height = 0, requested_scale = 0;
        DisplayLayout layout{};
        {
            std::lock_guard lock(resize_mutex);
            if (!resize_pending) return;
            resize_pending = false;
            width = resize_width;
            height = resize_height;
            layout = resize_layout;
            requested_scale = resize_internal_scale;
        }
        hud.set_layout(layout);
        settings.hud_layout = layout;
        settings.content_aspect = render_content_aspect(layout);
        settings.output_width = width;
        settings.output_height = height;
        stats.output_width = width;
        stats.output_height = height;
        ++stats.output_resizes;
        const u32 scale = resolve_internal_scale(requested_scale, layout);
        settings.internal_scale = requested_scale;
        if (scale == efb.scale.factor) return;
        // AFTER the scale test on purpose: apply_output_resize runs once per
        // present and returns above almost always. What is left - three full
        // fence waits plus a mid-session reallocation of every render target -
        // is the one guaranteed multi-frame stall in the renderer, and today it
        // is indistinguishable from a slow frame.
        GEKKO_PROFILE_ZONE("gpu/efb-rebuild");
        // Every list that could reference the old EFB completes first.
        for (Slot& s : slots) wait_slot(s);
        const u32 old_scale = efb.scale.factor;
        if (!create_efb(scale)) {
            // Keep rendering at the previous scale rather than lose the device.
            // If even that fails the renderer has no EFB at all, which is a
            // lost device, not a resolution problem: say so instead of letting
            // the next list dereference a null target.
            ++stats.efb_resize_refused;
            stats.last_error = "EFB resize failed";
            if (!create_efb(old_scale)) {
                stats.last_error = "EFB lost: neither the new nor the previous internal scale "
                                   "could be allocated";
                ++stats.target_allocation_failures;
            }
            return;
        }
        ++stats.efb_rebuilds;
        GEKKO_PROFILE_MESSAGE_LITERAL("gpu/efb rebuilt");
    }

    std::unique_ptr<plume::RenderTexture> create_solid_texture(u8 r, u8 g, u8 b, u8 a) {
        auto texture = device->createTexture(plume::RenderTextureDesc::Texture2D(1, 1, 1, kColorFormat));
        auto upload = device->createBuffer(plume::RenderBufferDesc::UploadBuffer(4));
        if (texture == nullptr || upload == nullptr) return {};
        auto* bytes = static_cast<u8*>(upload->map());
        if (bytes == nullptr) return {};
        bytes[0] = r; bytes[1] = g; bytes[2] = b; bytes[3] = a;
        upload->unmap();
        auto list = queue->createCommandList();
        list->begin();
        list->barriers(plume::RenderBarrierStage::COPY,
                       plume::RenderTextureBarrier(texture.get(), plume::RenderTextureLayout::COPY_DEST));
        list->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(texture.get()),
                                plume::RenderTextureCopyLocation::PlacedFootprint(upload.get(), kColorFormat, 1, 1, 1, 1));
        list->barriers(plume::RenderBarrierStage::GRAPHICS,
                       plume::RenderTextureBarrier(texture.get(), plume::RenderTextureLayout::SHADER_READ));
        list->end();
        auto fence = device->createCommandFence();
        const plume::RenderCommandList* lists[] = {list.get()};
        queue->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, fence.get());
        const auto retired = plume::waitForRetirement([&](u64 timeout) {
            return queue->waitForCommandFenceFor(fence.get(), timeout);
        });
        if (retired.outcome != plume::RenderFenceWait::SIGNALED) {
            const plume::FenceRetirementError error(retired.outcome, "native startup texture upload");
            if (retired.outcome != plume::RenderFenceWait::DEVICE_LOST) {
                // These locals own submitted upload bytes. Do not unwind them
                // while an unknown/healthy device can still consume the copy.
                std::fprintf(stderr, "[native-gpu] fatal startup before upload release: %s\n", error.what());
                std::fflush(stderr);
                std::terminate();
            }
            throw error;
        }
        return texture;
    }

    u32 allocate_bindless() {
        if (!free_bindless.empty()) {
            const u32 index = free_bindless.back();
            free_bindless.pop_back();
            return index;
        }
        if (bindless_next >= kBindlessCapacity) {
            // Descriptor-heap exhaustion is a silent refusal with NO counter of
            // any kind: every texture and camera target from here on is pinned
            // to the magenta sentinel and nothing in Stats or the exit report
            // would say why. Bounded by resource arrivals, and only after the
            // ceiling is reached, so it cannot flood. Read it against the
            // gpu/bindless-next plot.
            GEKKO_PROFILE_MESSAGE_LITERAL("gpu/bindless exhausted");
            return kTextureMagenta;
        }
        const u32 index = bindless_next++;
        stats.bindless_high_water = std::max<u64>(stats.bindless_high_water, index + 1u);
        return index;
    }

    // ---- frame / command list --------------------------------------------------

    // Before any draw, clear, copy or present: transitions what the COPY queue
    // wrote for reading. The COPY batch itself is executed once, immediately
    // before the GRAPHICS list is (submit_list), and the list waits on it: a
    // list cannot start before its own submission, so executing copies any
    // earlier buys nothing and costs a submission and a semaphore per
    // upload-then-draw interleave.
    void flush_resource_uploads() {
        CpuPhaseScope cpu_phase(stats, NativeGpu::CpuPhase::ResourceUpload);
        if (pending_shader_read.empty() && pending_buffer_read.empty()) return;
        GEKKO_PROFILE_ZONE("gpu/flush-uploads");
        for (plume::RenderTexture* texture : pending_shader_read) {
            ensure_texture_layout(texture, plume::RenderTextureLayout::SHADER_READ,
                                  plume::RenderBarrierStage::GRAPHICS);
        }
        pending_shader_read.clear();
        for (plume::RenderBuffer* buffer : pending_buffer_read) {
            slot->list->barriers(plume::RenderBarrierStage::GRAPHICS,
                                 plume::RenderBufferBarrier(buffer, plume::RenderBufferAccess::READ));
            ++stats.barrier_requests;
        }
        pending_buffer_read.clear();
    }

    // ONE state authority. The RHI backend tracks every resource's state in the
    // resource object itself and records nothing when a transition is already
    // satisfied. The table this replaced was keyed on raw ID3D12Resource
    // pointers, so a resource the allocator placed at a freed address inherited
    // the dead one's state and a required barrier could be elided; it needed a
    // forget() call at every death to stay sound, and missed the EFB rebuild.
    // State that lives in the object dies with it.
    void ensure_texture_layout(plume::RenderTexture* texture, plume::RenderTextureLayout wanted,
                               plume::RenderBarrierStages stage) {
        if (texture == nullptr) return;
        slot->list->barriers(stage, plume::RenderTextureBarrier(texture, wanted));
        ++stats.barrier_requests;
    }

    // Completion is judged by the RHI's bounded fence wait. The D3D12 backend
    // judges it by the fence value alone: its auto-reset completion event can
    // carry a stale signal from a submission whose completion was observed
    // another way, and a bare wait on it used to return while the GPU was still
    // executing - after which complete_slot freed deferred resources and
    // begin_list rewrote the upload ring under live GPU work. That hazard now
    // lives, and is fixed, in the backend.
    void wait_slot(Slot& s) {
        CpuPhaseScope cpu_phase(stats, NativeGpu::CpuPhase::FenceWait);
        if (!s.in_flight) return;
        const auto initial = queue->waitForCommandFenceFor(s.fence.get(), 0u);
        if (initial == plume::RenderFenceWait::SIGNALED) {
            ++stats.slot_fence_already_complete;
            complete_slot(s);
            return;
        }
        // SLOW PATH ONLY: the one place the render thread blocks on the GPU. It
        // is resolution-gated - at 640x528 the fast path above always wins.
        GEKKO_PROFILE_ZONE_COLOR("gpu/fence-wait", 0xC03030);
        const u64 begin = now_ns();
        // Reuse the sampled result for the helper's initial nonblocking poll.
        const auto retired = plume::waitForRetirement([&](u64 timeout) {
            return timeout == 0u ? initial : queue->waitForCommandFenceFor(s.fence.get(), timeout);
        });
        const auto result = retired.outcome;
        stats.fence_wait_ns += now_ns() - begin;
        if (result != plume::RenderFenceWait::SIGNALED) {
            // The slot is NOT completed: it stays in flight so nothing recycles
            // resources the GPU may still own, and a slot left in flight never
            // reclaims its deferred resources - the frames after this one are a
            // different program from the frames before it.
            ++stats.fence_wait_failures;
            const plume::FenceRetirementError error(result, "native GRAPHICS slot");
            native::publishFailureBeforeDiagnostics([&] { retirement_failed = true; }, [&] {
                std::fprintf(stderr, "[native-gpu] slot fence did not retire (%s); the slot stays in flight\n",
                             error.what());
                std::fflush(stderr);
                GEKKO_PROFILE_MESSAGE_LITERAL("gpu/fence wait failed: the slot stays in flight");
                stats.last_error = error.what();
            });
            throw error;
        }
        ++stats.fence_waits;
        plume::completeRetirement(retired, "native GRAPHICS slot", [&] { complete_slot(s); });
    }

    void complete_slot(Slot& s) {
        // Once per begin_list that finds an in-flight slot: in_flight is set
        // only in submit_list and cleared only here, and begin_list waits the
        // next slot of a three-slot rotation once a frame. ~60/s.
        GEKKO_PROFILE_ZONE("gpu/slot-complete");
        s.in_flight = false;
        if (s.timed && s.timer != nullptr) {
            s.timer->queryResults();
            const uint64_t* results = s.timer->getResults();
            if (results != nullptr && results[1] >= results[0]) {
                const u64 ns = results[1] - results[0];
                // The renderer already measures this and then averages it into
                // one exit number. As a plot it says WHICH frames the 16.76 ms
                // maximum belonged to, beside the zones that explain them.
                GEKKO_PROFILE_PLOT("gpu/gpu-frame-ms", static_cast<double>(ns) / 1.0e6);
                stats.gpu_frame_ns += ns;
                stats.gpu_frame_ns_max = std::max(stats.gpu_frame_ns_max, ns);
                ++stats.gpu_frames_timed;
            }
            s.timed = false;
        }
        if (s.readback != nullptr) {
            write_screenshot(s);
            s.readback.reset();
        }
        for (Deferred& d : s.releases) {
            if (d.bindless >= kFirstDynamicTexture) {
                texture_set->setTexture(d.bindless, white_texture.get(), plume::RenderTextureLayout::SHADER_READ);
                free_bindless.push_back(d.bindless);
            }
            // THE ONE PLACE EVERY DEFERRED RESOURCE DIES, so it is the one
            // place the pools can be balanced. The handle tests are required,
            // not defensive: present() deliberately KEEPS an output texture the
            // device refused - it only counts the failure - and that same
            // pointer reaches here without ever having been announced.
            // buffer_a is the vertex buffer and buffer_b the index buffer at
            // all three sites that push a Deferred.
            GEKKO_PROFILE_ONLY(
                if (d3d_texture(d.texture.get()) != nullptr) GEKKO_PROFILE_FREE(d.texture.get(), "gpu/texture");
                if (d3d_buffer(d.buffer_a.get()) != nullptr) GEKKO_PROFILE_FREE(d.buffer_a.get(), "gpu/geometry-vertex");
                if (d3d_buffer(d.buffer_b.get()) != nullptr) GEKKO_PROFILE_FREE(d.buffer_b.get(), "gpu/geometry-index");
            )
            ++stats.deferred_releases;
        }
        s.releases.clear();
        GEKKO_PROFILE_ONLY(
            for (const auto& buffer : s.pattern_releases) {
                if (d3d_buffer(buffer.get()) != nullptr) {
                    GEKKO_PROFILE_FREE(buffer.get(), "gpu/immediate-index-pattern");
                }
            }
        )
        s.pattern_releases.clear();
        // The submission that waited on these has retired: they may be signalled again.
        if (transfer_live) transfer.retire(s.transfer_waits);
        s.transfer_waits.clear();
    }

    void begin_list() {
        CpuPhaseScope cpu_phase(stats, NativeGpu::CpuPhase::ListBegin);
        // The frame's opening cost in one place: the split guard's wait over
        // all three slots, this slot's own wait, the ring rewind, the timestamp
        // pool reset and the whole of bind_frame_state.
        GEKKO_PROFILE_ZONE("gpu/begin-list");
        slot = &slots[slot_index];
        if (split_guard != 0u) {
            // A list split lets a later list read ring memory staged on an
            // earlier one. Before any ring is reset again, every list that
            // could still reference it must have completed.
            for (Slot& other : slots) wait_slot(other);
            --split_guard;
        }
        wait_slot(*slot);
        slot->ring.reset();
        slot->list->begin();
        recording = true;
        bound_pipeline = nullptr;
        viewport_valid = false;
        scissor_valid = false;
        constants_valid = false;
        lights_valid = false;
        bound_light_generation = 0;
        bound_vertex_buffer = nullptr;
        bound_index_buffer = nullptr;
        bound_vertex_size = 0u;
        bound_index_size = 0u;
        if (slot->timer != nullptr) {
            slot->list->resetQueryPool(slot->timer.get(), 0, 2);
            slot->list->writeTimestamp(slot->timer.get(), 0);
            slot->timed = true;
        }
        ensure_texture_layout(efb_color.get(), plume::RenderTextureLayout::COLOR_WRITE,
                              plume::RenderBarrierStage::GRAPHICS);
        ensure_texture_layout(efb_depth.get(), plume::RenderTextureLayout::DEPTH_WRITE,
                              plume::RenderBarrierStage::GRAPHICS);
        bind_frame_state();
    }

    void bind_frame_state() {
        slot->list->setFramebuffer(efb_framebuffer.get());
        slot->list->setGraphicsPipelineLayout(layout.get());
        slot->list->setGraphicsDescriptorSet(texture_set.get(), 0);
        slot->list->setGraphicsDescriptorSet(sampler_set.get(), 1);
        upload_lights(true);
        bound_pipeline = nullptr;
        viewport_valid = false;
        scissor_valid = false;
        constants_valid = false;
        last_draw_identity = {};
        bound_vertex_buffer = nullptr;
        bound_index_buffer = nullptr;
        bound_vertex_size = 0u;
        bound_index_size = 0u;
    }

    // Submits the list. `present` marks a frame boundary.
    void submit_list(plume::RenderCommandSemaphore* signal) {
        CpuPhaseScope cpu_phase(stats, NativeGpu::CpuPhase::ListSubmit);
        // The only hand-off to the driver in this file.
        GEKKO_PROFILE_ZONE("gpu/submit");
        if (!recording) return;
        if (slot->timed) slot->list->writeTimestamp(slot->timer.get(), 1);
        // Read before the list closes and before the next begin_list rewinds
        // it: this is the list's high-water mark against the hard 8 MiB bound
        // that a ring split exists to enforce.
        GEKKO_PROFILE_PLOT("gpu/ring-used", static_cast<double>(slot->ring.used_bytes()));
        stats.ring_peak_bytes = std::max(stats.ring_peak_bytes, slot->ring.used_bytes());
        {
            CpuPhaseScope cpu_close(stats, NativeGpu::CpuPhase::ListClose);
            slot->list->end();
        }
        stats.barriers += slot->list->transitionsRecorded;
        slot->list->transitionsRecorded = 0;
        // Copies recorded since the last flush run before this list: nothing it
        // reads may still be in flight on the COPY queue, and nothing released
        // while it runs can die before the copy that wrote it.
        if (transfer_live) {
            CpuPhaseScope cpu_transfer(stats, NativeGpu::CpuPhase::TransferSubmit);
            transfer.submit(slot->transfer_waits);
        }
        stats.graphics_waits_on_copy += slot->transfer_waits.size();
        const plume::RenderCommandList* lists[] = {slot->list.get()};
        plume::RenderCommandSemaphore* signals[] = {signal};
        {
        CpuPhaseScope cpu_queue(stats, NativeGpu::CpuPhase::QueueSubmit);
        queue->executeCommandLists(lists, 1,
                                   slot->transfer_waits.empty() ? nullptr : slot->transfer_waits.data(),
                                   static_cast<u32>(slot->transfer_waits.size()),
                                   signal != nullptr ? signals : nullptr, signal != nullptr ? 1u : 0u,
                                   slot->fence.get());
        }
        slot->in_flight = true;
        recording = false;
        slot_index = (slot_index + 1u) % kSlots;
    }

    // Allocation inside the current slot's ring. A frame that outgrows it is
    // split: the list is submitted, the next slot takes over, and the ring is
    // grown for the next use. Counted: a steady-state split is a sizing bug.
    plume::RenderBufferReference allocate(u64 bytes, u64 alignment, u8*& out,
                                          NativeGpu::RingUploadOwner owner) {
        stats.ring_largest_request_bytes = std::max(stats.ring_largest_request_bytes, bytes);
        if (bytes > kRingMaximumBytes) {
            ++stats.ring_bound_failures;
            ++stats.ring_fail_allocate_oversize;
            out = nullptr;
            return plume::RenderBufferReference();
        }
        auto allocation = slot->ring.allocate(bytes, alignment);
        if (!allocation) {
            // Splitting cannot repair a device allocation failure on an empty
            // retired arena. Recursing through begin_list here would cycle
            // through slots forever while repeatedly failing the same request.
            if (slot->ring.used_bytes() == 0u) {
                ++stats.ring_bound_failures;
                ++stats.ring_fail_grow;
                out = nullptr;
                return plume::RenderBufferReference();
            }
            ++stats.ring_flushes;
            // A split submits and re-begins mid-frame and then drains every
            // slot at the next begin_list, so without a mark it reads as an
            // unexplained long frame. This is the one message here with no
            // structural bound - allocate() runs ~500 times a frame - so if
            // ring_flushes ever becomes non-zero in steady state, drop the
            // message and keep the plot.
            GEKKO_PROFILE_MESSAGE_LITERAL("gpu/ring split (allocate)");
            split_guard = kSlots;
            submit_list(nullptr);
            begin_list();
            allocation = slot->ring.allocate(bytes, alignment);
            if (!allocation) {
                ++stats.ring_bound_failures;
                ++stats.ring_fail_grow;
                out = nullptr;
                return plume::RenderBufferReference();
            }
        }
        out = allocation.data;
        stats.upload_bytes += bytes;
        const std::size_t owner_index = static_cast<std::size_t>(owner);
        ++stats.ring_owner_allocations[owner_index];
        stats.ring_owner_bytes[owner_index] += bytes;
        return plume::RenderBufferReference(allocation.buffer, allocation.offset);
    }

    // Guarantees that `bytes` of upload space are available on the current list
    // before any binding for a draw or upload is recorded, so a ring split can
    // never separate a draw from its constants.
    void reserve(u64 bytes) {
        CpuPhaseScope cpu_phase(stats, NativeGpu::CpuPhase::UploadReserve);
        if (slot->ring.used_bytes() + bytes + 512u <= slot->ring.max_bytes()) return;
        ++stats.ring_flushes;
        GEKKO_PROFILE_MESSAGE_LITERAL("gpu/ring split (reserve)");
        split_guard = kSlots;
        submit_list(nullptr);
        begin_list();
        if (bytes + 4096u > slot->ring.max_bytes()) {
            ++stats.ring_bound_failures;
            ++stats.ring_fail_reserve_oversize;
        }
    }

    // ---- resources -------------------------------------------------------------

    void release_texture_entry(TextureEntry& entry) {
        render::appendRetainedOwner(slot->releases, [&](Deferred& retained) noexcept {
            retained.texture = std::move(entry.texture);
            retained.bindless = entry.bindless;
        });
    }

    // `fresh`: no GPU work has used this texture yet. Only then may the COPY
    // queue write it - it cannot order itself behind GRAPHICS work still
    // reading the old texels. A rewrite goes on the GRAPHICS list at the point
    // the guest changed the texture, which is also where its draws expect it.
    bool upload_rgba_into(plume::RenderTexture* texture, u32 width, u32 height, bool fresh,
                          const u8* rgba) {
        GEKKO_PROFILE_ZONE("gpu/texture-upload");
        GEKKO_PROFILE_VALUE(static_cast<u64>(width) * height * 4u);
        if (fresh && transfer_live &&
            transfer.upload_texture_rgba8(*texture, width, height, rgba)) {
            pending_shader_read.push_back(texture);
            return true;
        }
        // Row pitch and placement as every backend accepts them. A pitch of
        // width * 4 (what this path used before) is invalid on D3D12 for any
        // texture narrower than 64 texels.
        const u64 row_bytes = static_cast<u64>(width) * 4u;
        const u64 pitch = (row_bytes + render::kStagingRowAlignment - 1u) &
                          ~(render::kStagingRowAlignment - 1u);
        const u64 bytes = pitch * height;
        reserve(bytes + render::kStagingPlacementAlignment);
        u8* staging = nullptr;
        const auto ref = allocate(bytes, render::kStagingPlacementAlignment, staging,
                                  NativeGpu::RingUploadOwner::Texture);
        if (staging == nullptr) return false;
        for (u32 y = 0; y < height; ++y) {
            std::memcpy(staging + pitch * y, rgba + row_bytes * y,
                        static_cast<std::size_t>(row_bytes));
        }
        ensure_texture_layout(texture, plume::RenderTextureLayout::COPY_DEST,
                              plume::RenderBarrierStage::COPY);
        {
            // SAFE because the single allocate() above has already run: nothing
            // inside this scope can split the command list, and a zone whose
            // destructor resolves its query into a list that was already closed
            // records an unpaired timestamp.
            GEKKO_PROFILE_GPU_ZONE(tracy_gpu,
                                   static_cast<plume::D3D12CommandList*>(slot->list.get())->d3d,
                                   "gpu/gpu-texture-upload");
            slot->list->copyTextureRegion(
                plume::RenderTextureCopyLocation::Subresource(texture),
                plume::RenderTextureCopyLocation::PlacedFootprint(ref.ref, kColorFormat, width,
                                                                  height, 1, static_cast<u32>(pitch / 4u),
                                                                  ref.offset));
        }
        ensure_texture_layout(texture, plume::RenderTextureLayout::SHADER_READ,
                              plume::RenderBarrierStage::GRAPHICS);
        bound_vertex_buffer = nullptr;
        bound_index_buffer = nullptr;
        bound_vertex_size = 0u;
        bound_index_size = 0u;
        return true;
    }

    void texture_resolution_changed() {
        if (++texture_resolution_generation == 0u) constants_valid = false;
    }

    void on_texture_content(const FrameSegment& segment, const native::TextureContent& content) {
        CpuPhaseScope cpu_phase(stats, NativeGpu::CpuPhase::TextureUpload);
        TextureEntry& entry = textures[content.texture.address];
        if (entry.texture != nullptr && entry.generation == content.texture.generation) {
            return;
        }
        // texture_index consumes descriptor/generation ownership outside GX
        // state. Even a failed arrival may change the missing-resource result.
        texture_resolution_changed();
        // OPENED AFTER THE GENERATION EARLY-OUT: what is left is an actual
        // texture arrival - decode, create, upload and bindless publication -
        // and textures_created and texture_bytes carry no time at all.
        GEKKO_PROFILE_ZONE("gpu/texture-content");
        const u32 width = content.width, height = content.height;
        GEKKO_PROFILE_VALUE(width * height);
        const std::size_t rgba_bytes = static_cast<std::size_t>(width) * height * 4u;
        const u8* rgba = nullptr;
        // Native asset payloads arrive already decoded to linear RGBA8
        // (kTextureNativeLinear, hash-verified against the guest raster at
        // capture time); every other payload is GX texels decoded here.
        bool decoded = false;
        {
            // Splits CPU texel decode from the device create and upload that
            // follow inside the same arrival. The extent is already on the
            // enclosing zone, so the format is the useful second dimension.
            GEKKO_PROFILE_ZONE("gpu/texture-decode");
            GEKKO_PROFILE_VALUE(content.gx_format);
            if ((content.flags & native::kTextureNativeLinear) != 0u &&
                content.gx_format == kGxTexFmtRGBA8 &&
                content.texels.size >= rgba_bytes) {
                ++stats.native_linear_texture_payloads;
                stats.native_linear_texture_bytes += rgba_bytes;
                // The immutable segment outlives this consume call. Both COPY
                // and GRAPHICS paths copy synchronously into their fence-owned
                // mapped staging; no GPU command keeps this source pointer.
                rgba = segment.payload_at(content.texels);
                ++stats.native_linear_texture_direct_payloads;
                stats.native_linear_texture_direct_bytes += rgba_bytes;
                decoded = true;
            } else {
                rgba_scratch.resize(rgba_bytes);
                decoded = gx_decode_texture(
                    content.gx_format, width, height, segment.payload_at(content.texels),
                    content.texels.size,
                    content.palette.size != 0u ? segment.payload_at(content.palette) : nullptr,
                    content.tlut_format, content.tlut_entries, rgba_scratch.data());
                rgba = rgba_scratch.data();
            }
        }
        if (!decoded) {
            ++stats.texture_decode_failures;
            if (entry.texture == nullptr) {
                // Nothing existed; magenta is the honest answer.
                ++stats.texture_pinned_missing_new;
                entry.bindless = kTextureMagenta;
            } else {
                // A working, resident texture is retained rather than
                // replaced. Counted separately because these two outcomes are
                // opposite and used to share one number.
                ++stats.texture_pinned_missing_reuse;
            }
            reject(native::ResourceKind::Texture, content.texture.address,
                   content.texture.generation, native::RejectReason::TextureUpload);
            return;
        }
        const bool reuse = entry.texture != nullptr && entry.width == width && entry.height == height;
        if (reuse) {
            if (!upload_rgba_into(entry.texture.get(), width, height, false, rgba)) {
                // DO NOT TOUCH entry.bindless HERE. It is the descriptor index
                // of a texture that still EXISTS and is still resident - only
                // its new texels failed to arrive. Overwriting it with the
                // magenta sentinel destroys the real index (leaking it, since
                // nothing returns it to free_bindless) and pins the object
                // magenta even after a successful retry, because the retry
                // re-uploads and commits the generation but has no way to
                // recover an index it no longer knows.
                //
                // Leaving the descriptor alone keeps the PREVIOUS texels
                // visible for one frame - stale, but present and correct in
                // extent - and withholding the generation commit makes the
                // capture re-send. That is strictly better than magenta and
                // strictly better than a permanent loss.
                ++stats.texture_pinned_missing_reuse;
                reject(native::ResourceKind::Texture, content.texture.address,
                       content.texture.generation, native::RejectReason::TextureUpload);
                return;
            }
            entry.generation = content.texture.generation;
            return;
        }
        if (entry.texture != nullptr) {
            release_texture_entry(entry);
            // NOTE the asymmetry this pairs with: the matching increment is at
            // the very END of this path, so the two failure returns below sit
            // BETWEEN them. Without this being deliberate, a refused texture
            // would decrement textures_live and never increment it, and the
            // count would drift down by one per failure for the rest of the
            // session. It is correct as written - the old texture really is
            // gone - but the failure paths must not be read as "live" either,
            // which is what texture_pinned_missing_new records.
            --stats.textures_live;
        }
        entry = TextureEntry{};
        entry.texture = device->createTexture(plume::RenderTextureDesc::Texture2D(width, height, 1, kColorFormat));
        if (entry.texture == nullptr) {
            entry.texture.reset();
            entry.bindless = kTextureMagenta;
            ++stats.texture_pinned_missing_new;
            reject(native::ResourceKind::Texture, content.texture.address,
                   content.texture.generation, native::RejectReason::TextureCreate);
            return;
        }
        // Announced only once the device has actually produced it; the matching
        // free is in complete_slot, which every death site for a texture -
        // release_texture_entry, on_texture_release, the camera-target rebuild,
        // present's output resize and the ResetState arm - routes through.
        GEKKO_PROFILE_ALLOC(entry.texture.get(), rgba_bytes, "gpu/texture");
        // Record the extent NOW, while the texture that has it is alive. The
        // create path wipes the entry, so an exit between here and the commit
        // below used to leave a fully created WxH texture behind an entry
        // reporting 0x0 - and the next content then failed the `reuse` test
        // against its own dimensions and rebuilt a texture that was already
        // correct.
        entry.width = width;
        entry.height = height;
        if (!upload_rgba_into(entry.texture.get(), width, height, true, rgba)) {
            entry.bindless = kTextureMagenta;
            ++stats.texture_pinned_missing_new;
            reject(native::ResourceKind::Texture, content.texture.address,
                   content.texture.generation, native::RejectReason::TextureUpload);
            return;
        }
        // Committed only now, with a live texture behind it.
        entry.generation = content.texture.generation;
        entry.bindless = allocate_bindless();
        texture_set->setTexture(entry.bindless, entry.texture.get(), plume::RenderTextureLayout::SHADER_READ);
        entry.width = width;
        entry.height = height;
        ++stats.textures_live;
        ++stats.textures_created;
        stats.texture_bytes += rgba_bytes;
    }

    void on_texture_release(const native::ResourceKey& key) {
        auto it = textures.find(key.address);
        if (it == textures.end()) return;
        // See on_geometry_release: `>` not `!=`, for the same reason.
        if (it->second.generation > key.generation) return;
        texture_resolution_changed();
        if (it->second.texture != nullptr) {
            release_texture_entry(it->second);
            --stats.textures_live;
        }
        textures.erase(it);
        // A released raster address may have been a camera target.
        auto target = targets.find(key.address);
        if (target != targets.end()) {
            render::appendRetainedOwner(slot->releases, [&](Deferred& retained) noexcept {
                retained.texture = std::move(target->second.texture);
                retained.bindless = target->second.bindless;
            });
            targets.erase(target);
        }
    }

    CameraTarget& camera_target(u32 address, u32 guest_width, u32 guest_height) {
        CameraTarget& target = targets[address];
        // One integer factor on both axes: the two levels of the glare ladder
        // can no longer disagree about their vertical scale the way independent
        // lround(guest * 2.102) and lround(guest * 2.109) did.
        const u32 native_width = std::max<u32>(1u, efb.scale.apply(guest_width));
        const u32 native_height = std::max<u32>(1u, efb.scale.apply(guest_height));
        if (target.texture == nullptr || target.native_width != native_width ||
            target.native_height != native_height) {
            texture_resolution_changed();
            if (target.texture != nullptr) {
                render::appendRetainedOwner(slot->releases, [&](Deferred& retained) noexcept {
                    retained.texture = std::move(target.texture);
                    retained.bindless = target.bindless;
                });
            }
            target.texture = device->createTexture(
                plume::RenderTextureDesc::Texture2D(native_width, native_height, 1, kColorFormat));
            if (target.texture == nullptr) {
                // Out of memory or a lost device. Leave the target empty and
                // keep its dimensions unset so the next frame retries, rather
                // than transition and publish a descriptor for a null handle.
                target.texture.reset();
                target.native_width = 0;
                target.native_height = 0;
                ++stats.target_allocation_failures;
                return target;
            }
            // Same pool as the decoded textures, because Deferred carries no
            // kind tag and both die through the same release path.
            GEKKO_PROFILE_ALLOC(target.texture.get(),
                                static_cast<u64>(native_width) * native_height * 4u, "gpu/texture");
            target.bindless = allocate_bindless();
            target.guest_width = guest_width;
            target.guest_height = guest_height;
            target.native_width = native_width;
            target.native_height = native_height;
            ensure_texture_layout(target.texture.get(), plume::RenderTextureLayout::SHADER_READ,
                                  plume::RenderBarrierStage::GRAPHICS);
            texture_set->setTexture(target.bindless, target.texture.get(), plume::RenderTextureLayout::SHADER_READ);
            ++stats.camera_targets;
        }
        return target;
    }

    // The resident asset whose conversion inputs equal this content's, or null.
    // Equality is established byte for byte over everything the conversion
    // consumed; the identity only narrows the search.
    GeometryAsset* find_geometry_asset(const native::GeometryContent& content, const u8* blob) {
        CpuPhaseScope cpu_phase(stats, NativeGpu::CpuPhase::GeometryLookup);
        auto it = geometry_assets.find(content.identity);
        if (it == geometry_assets.end()) return nullptr;
        for (const auto& asset : it->second) {
            if (asset->geometry_flags == content.geometry_flags &&
                asset->world_sector == content.world_sector &&
                asset->mesh_count == content.mesh_count && asset->dynamic == content.dynamic &&
                asset->matrix_palette == content.matrix_palette &&
                asset->vertex_format == content.vertex_format &&
                asset->source.size() <= content.blob.size &&
                std::memcmp(asset->source.data(), blob, asset->source.size()) == 0) {
                return asset.get();
            }
        }
        ++stats.geometry_identity_conflicts;
        return nullptr;
    }

    void unbind_geometry(GeometryEntry& entry) {
        if (entry.asset == nullptr) return;
        --entry.asset->instances;
        entry.asset->last_used_frame = frame_ordinal;
        entry.asset = nullptr;
    }

    // Converts the content and creates its GPU buffers. Null on refusal, with
    // the refusal already reported to the capture; the store is unchanged.
    GeometryAsset* create_geometry_asset(const native::GeometryContent& content, const u8* blob) {
        CpuPhaseScope cpu_phase(stats, NativeGpu::CpuPhase::GeometryConvert);
        // CONVERT BEFORE ALLOCATING ANYTHING: a conversion failure must leave the
        // store exactly as it was found.
        const std::size_t header_size = 12u + 8u * load_be32(blob + 8u) + 8u * content.mesh_count;
        ConvertedGeometry& converted = convert_scratch;
        {
            // The RenderWare display-list walk: the CPU-heavy half of an
            // arrival and the half that can fail.
            GEKKO_PROFILE_ZONE("gpu/geometry-convert");
            const GxVertexFormatDesc format =
                rw_vertex_format(content.vertex_format.data(), content.geometry_flags,
                                 content.matrix_palette != 0u);
            RwConvertOptions options;
            options.dynamic_pos_nrm = content.dynamic != 0u;
            std::string error;
            // Base 0: the capture made every embedded pointer relative to blob byte 0.
            if (header_size > content.blob.size ||
                !rw_convert_instance(blob, header_size, blob, content.blob.size, 0u, format,
                                     converted, &error, options)) {
                ++stats.geometry_convert_failures;
                if (stats.last_error.empty()) stats.last_error = "geometry: " + error;
                note_drop("content:convert-failed", content.geometry.address,
                          content.geometry.generation, 0u, content.mesh_count);
                reject(native::ResourceKind::Geometry, content.geometry.address,
                       content.geometry.generation, native::RejectReason::ConvertFailed);
                // A refused display list is a reverse-engineering problem, and it
                // cannot be solved from an error string. Write the exact bytes the
                // parser choked on, plus the format and flags it was given, so the
                // desync can be decoded offline against the GX command encoding.
                if (!settings.geometry_failure_dump.empty()) {
                    char name[64];
                    std::snprintf(name, sizeof(name), "geometry-%08X-gen%u.bin", content.geometry.address,
                                  content.geometry.generation);
                    const auto path = settings.geometry_failure_dump / name;
                    std::ofstream out(path, std::ios::binary | std::ios::trunc);
                    if (out) {
                        // A small header so the blob is self-describing offline.
                        const u32 head[6] = {content.geometry.address, content.geometry.generation,
                                             content.geometry_flags,  content.world_sector,
                                             content.mesh_count,      content.dynamic};
                        out.write(reinterpret_cast<const char*>(head), sizeof(head));
                        out.write(reinterpret_cast<const char*>(content.vertex_format.data()),
                                  static_cast<std::streamsize>(content.vertex_format.size()));
                        // Pointers in the blob are relative to its byte 0.
                        const u32 base = 0u;
                        out.write(reinterpret_cast<const char*>(&base), sizeof(base));
                        const u32 size = content.blob.size;
                        out.write(reinterpret_cast<const char*>(&size), sizeof(size));
                        out.write(reinterpret_cast<const char*>(blob), static_cast<std::streamsize>(size));
                    }
                }
                return nullptr;
            }
        }
        stats.geometry_foreign_commands += converted.stats.foreign_commands;

        auto asset = std::make_unique<GeometryAsset>();
        asset->identity = content.identity;
        asset->geometry_flags = content.geometry_flags;
        asset->world_sector = content.world_sector;
        asset->mesh_count = content.mesh_count;
        asset->dynamic = content.dynamic;
        asset->matrix_palette = content.matrix_palette;
        asset->vertex_format = content.vertex_format;
        // source_extent is bounded by the blob by construction (every resolve
        // lies inside the one span passed above).
        asset->source.assign(blob, blob + std::min<std::size_t>(converted.source_extent,
                                                                 content.blob.size));
        asset->meshes = converted.meshes;
        asset->has_normals = converted.has_normals;
        asset->has_colors = converted.has_colors;
        asset->vertex_count = static_cast<u32>(converted.vertices.size());
        const bool dynamic = content.dynamic != 0u;
        const u64 index_bytes = std::max<u64>(1u, converted.indices.size()) * sizeof(u32);
        u64 vertex_bytes = 0;
        PackedVertexWriteContext packed_vertices{converted.vertices.data(), converted.vertices.size()};
        if (!dynamic) {
            vertex_bytes = std::max<u64>(1u, converted.vertices.size()) * sizeof(shadow::render::RenderVertex);
            asset->vertices = device->createBuffer(
                plume::RenderBufferDesc::VertexBuffer(vertex_bytes, plume::RenderHeapType::DEFAULT));
        }
        asset->indices = device->createBuffer(
            plume::RenderBufferDesc::IndexBuffer(index_bytes, plume::RenderHeapType::DEFAULT));
        if ((!dynamic && asset->vertices == nullptr) || asset->indices == nullptr) {
            ++stats.geometry_drop_buffer_create;
            note_drop("content:buffer-create", content.geometry.address,
                      content.geometry.generation, 0u, content.mesh_count);
            reject(native::ResourceKind::Geometry, content.geometry.address,
                   content.geometry.generation, native::RejectReason::BufferCreate);
            return nullptr;
        }
        GEKKO_PROFILE_ONLY(
            if (asset->vertices != nullptr) GEKKO_PROFILE_ALLOC(asset->vertices.get(), vertex_bytes, "gpu/geometry-vertex");
            GEKKO_PROFILE_ALLOC(asset->indices.get(), index_bytes, "gpu/geometry-index");
        )
        // Fresh buffers: the COPY queue may write them. Each buffer is staged
        // independently, so a refusal of one (an empty index list is refused as
        // a zero-byte upload) cannot leave the other half-recorded.
        bool vertices_staged = dynamic;
        bool indices_staged = false;
        if (transfer_live) {
            if (!dynamic && transfer.upload_buffer_generated(
                                *asset->vertices, 0, vertex_bytes,
                                render::TransferQueue::UploadOwner::GeometryVertex,
                                fill_packed_vertices, &packed_vertices)) {
                vertices_staged = true;
                pending_buffer_read.push_back(asset->vertices.get());
                stats.cpu_static_geometry_copy_bytes_avoided += vertex_bytes;
            }
            if (transfer.upload_buffer(*asset->indices, 0, converted.indices.data(),
                                       converted.indices.size() * sizeof(u32),
                                       render::TransferQueue::UploadOwner::GeometryIndex)) {
                indices_staged = true;
                pending_buffer_read.push_back(asset->indices.get());
            }
        }
        if (!vertices_staged || !indices_staged) {
            // The GRAPHICS list stages what the COPY queue did not take, with the
            // transitions a copy on this queue requires: COPY_DEST before, and a
            // read state before the first draw (flush_resource_uploads).
            reserve((vertices_staged ? 0u : vertex_bytes) + (indices_staged ? 0u : index_bytes) + 64u);
            u8* staging = nullptr;
            if (!vertices_staged) {
                auto vref = allocate(vertex_bytes, 16u, staging,
                                     NativeGpu::RingUploadOwner::GeometryVertex);
                if (staging == nullptr) {
                    ++stats.geometry_drop_vertex_staging;
                    note_drop("content:vertex-staging", content.geometry.address,
                              content.geometry.generation, 0u, static_cast<u32>(vertex_bytes));
                    reject(native::ResourceKind::Geometry, content.geometry.address,
                           content.geometry.generation, native::RejectReason::VertexStaging);
                    defer_asset_buffers(*asset);
                    return nullptr;
                }
                fill_packed_vertices(staging, vertex_bytes, &packed_vertices);
                slot->list->barriers(plume::RenderBarrierStage::COPY,
                                     plume::RenderBufferBarrier(asset->vertices.get(),
                                                                plume::RenderBufferAccess::WRITE));
                ++stats.barrier_requests;
                {
                    // TWO ZONES: the index allocate() below can split the list.
                    GEKKO_PROFILE_GPU_ZONE(tracy_gpu,
                                           static_cast<plume::D3D12CommandList*>(slot->list.get())->d3d,
                                           "gpu/gpu-geometry-vertex-upload");
                    slot->list->copyBufferRegion(plume::RenderBufferReference(asset->vertices.get()), vref,
                                                 vertex_bytes);
                }
                pending_buffer_read.push_back(asset->vertices.get());
            }
            if (!indices_staged) {
                auto iref = allocate(index_bytes, 16u, staging,
                                     NativeGpu::RingUploadOwner::GeometryIndex);
                if (staging == nullptr) {
                    ++stats.geometry_drop_index_staging;
                    note_drop("content:index-staging", content.geometry.address,
                              content.geometry.generation, 0u, static_cast<u32>(index_bytes));
                    reject(native::ResourceKind::Geometry, content.geometry.address,
                           content.geometry.generation, native::RejectReason::IndexStaging);
                    defer_asset_buffers(*asset);
                    return nullptr;
                }
                std::memset(staging, 0, index_bytes);
                std::memcpy(staging, converted.indices.data(), converted.indices.size() * sizeof(u32));
                slot->list->barriers(plume::RenderBarrierStage::COPY,
                                     plume::RenderBufferBarrier(asset->indices.get(),
                                                                plume::RenderBufferAccess::WRITE));
                ++stats.barrier_requests;
                {
                    GEKKO_PROFILE_GPU_ZONE(tracy_gpu,
                                           static_cast<plume::D3D12CommandList*>(slot->list.get())->d3d,
                                           "gpu/gpu-geometry-index-upload");
                    slot->list->copyBufferRegion(plume::RenderBufferReference(asset->indices.get()), iref,
                                                 index_bytes);
                }
                pending_buffer_read.push_back(asset->indices.get());
            }
        }
        asset->index_bytes = static_cast<u32>(index_bytes);
        if (dynamic) asset->cpu = std::make_unique<ConvertedGeometry>(converted);
        asset->gpu_bytes = vertex_bytes + index_bytes;
        asset->last_used_frame = frame_ordinal;
        ++stats.geometry_created;
        stats.geometry_asset_bytes += asset->gpu_bytes;
        stats.geometry_source_bytes += asset->source.size();
        ++stats.geometry_assets_resident;
        bound_vertex_buffer = nullptr;
        bound_index_buffer = nullptr;
        bound_vertex_size = 0u;
        bound_index_size = 0u;
        GeometryAsset* const raw = asset.get();
        geometry_assets[content.identity].push_back(std::move(asset));
        return raw;
    }

    // Queues an asset's buffers for destruction behind the current frame's fence.
    void defer_asset_buffers(GeometryAsset& asset) {
        if (asset.vertices == nullptr && asset.indices == nullptr) return;
        render::appendRetainedOwner(slot->releases, [&](Deferred& retained) noexcept {
            retained.buffer_a = std::move(asset.vertices);
            retained.buffer_b = std::move(asset.indices);
        });
    }

    void on_geometry_content(const FrameSegment& segment, const native::GeometryContent& content) {
        GEKKO_PROFILE_ZONE("gpu/geometry-content");
        GEKKO_PROFILE_VALUE(content.mesh_count);
        auto existing = geometry.find(content.geometry.address);
        if (existing != geometry.end() && existing->second.generation == content.geometry.generation) {
            return;
        }
        // Test-only; see inject_reject. Before any mutation, so an injected
        // refusal exercises the transactional property the real failures rely on.
        if (inject_reject(content.geometry.address)) {
            ++stats.geometry_drop_buffer_create;
            note_drop("content:injected-reject", content.geometry.address,
                      content.geometry.generation, 0u, content.mesh_count);
            reject(native::ResourceKind::Geometry, content.geometry.address,
                   content.geometry.generation, native::RejectReason::BufferCreate);
            return;
        }
        const u8* blob = segment.payload_at(content.blob);
        GeometryAsset* asset = find_geometry_asset(content, blob);
        if (asset != nullptr) {
            ++stats.geometry_asset_hits;
        } else {
            asset = create_geometry_asset(content, blob);
            // Refused and reported. The entry keeps whatever it held: an older
            // generation reports an honest mismatch at the draw, and the
            // capture re-sends.
            if (asset == nullptr) return;
        }
        // THE COMMIT: the entry names this generation only with an asset behind it.
        GeometryEntry& entry = geometry[content.geometry.address];
        if (entry.asset == nullptr) ++stats.geometry_live;
        unbind_geometry(entry);
        entry.asset = asset;
        ++asset->instances;
        asset->last_used_frame = frame_ordinal;
        entry.generation = content.geometry.generation;
        entry.dynamic_buffer = nullptr;
        entry.dynamic_bound = false;
        entry.dynamic_frame = 0;
    }

    // Unreferenced assets stay resident: the guest re-instances evicted models
    // constantly, and an asset costs its content, not a GPU round trip, to
    // keep. They are released only when the device reports it is over its
    // memory budget, least recently used first. `GEKKO_GEOMETRY_UNREFERENCED_LIMIT`
    // (bytes; TEST-ONLY) caps them instead, so the release path can be proven:
    // 0 releases every unreferenced asset at every present, which reproduces
    // the old per-entry lifetime and must draw the same pixels.
    static u64 unreferenced_limit_override() {
        static const u64 limit = [] {
            const char* value = std::getenv("GEKKO_GEOMETRY_UNREFERENCED_LIMIT");
            return value != nullptr ? std::strtoull(value, nullptr, 10) : ~0ull;
        }();
        return limit;
    }

    void trim_geometry_assets() {
        const u64 override_limit = unreferenced_limit_override();
        u64 excess = 0;
        if (override_limit != ~0ull) {
            u64 unreferenced = 0;
            for (const auto& [identity, assets] : geometry_assets) {
                for (const auto& asset : assets) {
                    if (asset->instances == 0u) unreferenced += asset->gpu_bytes;
                }
            }
            if (unreferenced <= override_limit) return;
            excess = unreferenced - override_limit;
        } else {
            update_budget();
            if (stats.vram_budget_bytes == 0u || stats.vram_usage_bytes <= stats.vram_budget_bytes) return;
            excess = stats.vram_usage_bytes - stats.vram_budget_bytes;
        }
        GEKKO_PROFILE_ZONE("gpu/geometry-trim");
        std::vector<std::pair<u32, GeometryAsset*>> candidates;
        for (const auto& [identity, assets] : geometry_assets) {
            for (const auto& asset : assets) {
                if (asset->instances == 0u) candidates.emplace_back(asset->last_used_frame, asset.get());
            }
        }
        std::sort(candidates.begin(), candidates.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        u64 freed = 0;
        std::unordered_set<const GeometryAsset*> doomed;
        for (const auto& [frame, asset] : candidates) {
            if (freed >= excess) break;
            freed += asset->gpu_bytes;
            doomed.insert(asset);
        }
        for (auto it = geometry_assets.begin(); it != geometry_assets.end();) {
            auto& assets = it->second;
            for (auto a = assets.begin(); a != assets.end();) {
                if (doomed.count(a->get()) == 0u) {
                    ++a;
                    continue;
                }
                stats.geometry_asset_bytes -= (*a)->gpu_bytes;
                stats.geometry_source_bytes -= (*a)->source.size();
                --stats.geometry_assets_resident;
                ++stats.geometry_assets_evicted;
                GEKKO_PROFILE_ONLY(
                    if ((*a)->vertices != nullptr) GEKKO_PROFILE_FREE((*a)->vertices.get(), "gpu/geometry-vertex");
                    GEKKO_PROFILE_FREE((*a)->indices.get(), "gpu/geometry-index");
                )
                defer_asset_buffers(**a);
                a = assets.erase(a);
            }
            it = assets.empty() ? geometry_assets.erase(it) : std::next(it);
        }
    }

    // FAULT INJECTION, TEST-ONLY, OFF UNLESS THE ENVIRONMENT SAYS OTHERWISE.
    //
    // The recovery path this class now has is unreachable on every route the
    // project can run: all five gates report zero rejections, because nothing
    // legitimately fails on them. An untested recovery path is not a recovery
    // path. This makes the failure reachable on demand so the contract can be
    // proven, and proven bounded.
    //
    //   GEKKO_REJECT_GEOMETRY=once    refuse each address the first time only;
    //                                 the retry must then succeed and the
    //                                 picture must come back IDENTICAL.
    //   GEKKO_REJECT_GEOMETRY=always  refuse forever; the retry budget must
    //                                 stop it and say so.
    //
    // Read once. Costs one already-initialised static load when unset.
    enum class InjectMode { Off, Once, Always };
    static InjectMode inject_mode() {
        static const InjectMode mode = [] {
            const char* value = std::getenv("GEKKO_REJECT_GEOMETRY");
            if (value == nullptr) return InjectMode::Off;
            if (std::strcmp(value, "once") == 0) return InjectMode::Once;
            if (std::strcmp(value, "always") == 0) return InjectMode::Always;
            return InjectMode::Off;
        }();
        return mode;
    }
    std::unordered_set<u32> injected_once;

    // Returns true when this content should be treated as refused.
    bool inject_reject(u32 address) {
        switch (inject_mode()) {
            case InjectMode::Off: return false;
            case InjectMode::Always: return true;
            case InjectMode::Once: return injected_once.insert(address).second;
        }
        return false;
    }

    // TELL THE CAPTURE WE DID NOT TAKE IT. Without this the capture's latch
    // stands, the content is never re-sent, and the object is gone for the
    // rest of its generation - which for static world geometry is the stage.
    void reject(native::ResourceKind kind, u32 address, u32 generation,
                native::RejectReason reason) {
        ++stats.rejections_posted;
        // A refusal handed back to the capture IS the recovery contract firing:
        // the difference between an object re-sent and an object gone for the
        // stage. Explicitly bounded, because GEKKO_REJECT_GEOMETRY=always would
        // otherwise flood the timeline; the totals stay on the plots.
        GEKKO_PROFILE_ONLY(if (stats.rejections_posted <= 16u && GEKKO_PROFILE_CONNECTED()) {
            char message[128];
            const int written = std::snprintf(
                message, sizeof(message), "gpu/reject %s 0x%08X gen %u: %s",
                kind == native::ResourceKind::Geometry ? "geometry" : "texture", address,
                generation, native::reject_reason_name(reason));
            if (written > 0) GEKKO_PROFILE_MESSAGE(message, static_cast<std::size_t>(written));
        })
        if (kind == native::ResourceKind::Geometry) {
            ++stats.rejections_geometry;
        } else {
            ++stats.rejections_texture;
        }
        if (rejections == nullptr) {
            // Still counted. A refusal with nowhere to go is exactly the old
            // behaviour, and saying so is the difference between a known
            // limitation and a silent one.
            ++stats.rejections_undeliverable;
            return;
        }
        native::ResourceRejection out;
        out.address = address;
        out.generation = generation;
        out.kind = kind;
        out.reason = reason;
        rejections->post(out);
    }

    void note_drop(const char* cause, u32 address, u32 wanted, u32 held, u32 detail) {
        if (stats.geometry_drops_seen < NativeGpu::Stats::kGeometryDropRecords) {
            stats.geometry_drops[stats.geometry_drops_seen] =
                NativeGpu::Stats::GeometryDrop{cause, address, wanted, held, detail, frame_ordinal};
            // Inside the existing bound, which is what makes this safe: 24 per
            // session, at the single choke point every immediate, mesh,
            // dynamic and geometry drop passes through. A plot says a drop
            // happened in frame N; this says which cause and which object, on
            // the timeline beside the draw zone that lost it. Without the
            // bound, the Gun Fortress case (810 drops per frame) would be
            // 48k messages a second.
            GEKKO_PROFILE_ONLY(if (GEKKO_PROFILE_CONNECTED()) {
                char message[128];
                const int written = std::snprintf(message, sizeof(message),
                                                  "gpu/drop %s 0x%08X want %u held %u detail %u",
                                                  cause, address, wanted, held, detail);
                if (written > 0) GEKKO_PROFILE_MESSAGE(message, static_cast<std::size_t>(written));
            })
        }
        ++stats.geometry_drops_seen;
        note_drop_object(cause, address, wanted, held);
    }

    // THE PREFIX RING ABOVE CANNOT ANSWER THE QUESTION THAT MATTERS. It holds
    // the first 24 drops, so a total of 1,538 is equally consistent with 17
    // objects lost forever and 1,538 objects lost for one frame each - and
    // those need completely different fixes. The 2026-09 Gun Fortress
    // investigation had to reach that answer by arithmetic on two unrelated
    // totals, because nothing in the renderer held it.
    //
    // Keyed on object address, with the frame span. A permanent loss is a few
    // addresses whose last_frame is the final frame; a transient is many
    // addresses with small counts. Linear scan over at most 256 entries, on a
    // path that only runs when something has already gone wrong.
    void note_drop_object(const char* cause, u32 address, u32 wanted, u32 held) {
        auto& used = stats.geometry_drop_objects_used;
        for (u32 i = 0; i < used; ++i) {
            auto& row = stats.geometry_drop_objects[i];
            if (row.address != address) continue;
            row.last_cause = cause;
            row.last_frame = frame_ordinal;
            row.wanted = wanted;
            row.held = held;
            ++row.count;
            return;
        }
        if (used >= NativeGpu::Stats::kGeometryDropObjects) {
            // Say so rather than truncating silently - a table that quietly
            // stops recording is the failure mode this replaces.
            ++stats.geometry_drop_objects_overflow;
            return;
        }
        auto& row = stats.geometry_drop_objects[used++];
        row.address = address;
        row.first_cause = cause;
        row.last_cause = cause;
        row.first_frame = frame_ordinal;
        row.last_frame = frame_ordinal;
        row.wanted = wanted;
        row.held = held;
        row.count = 1;
    }

    void on_geometry_dynamic(const FrameSegment& segment, const native::GeometryDynamic& dynamic) {
        CpuPhaseScope cpu_phase(stats, NativeGpu::CpuPhase::GeometryDynamic);
        // 5 per frame, ~300/s: the only per-frame CPU work that scales with the
        // number of characters on screen. Every skinned vertex is re-expanded,
        // repacked and staged on the GRAPHICS list here, every frame, and
        // dynamic_updates carries no cost for any of it.
        GEKKO_PROFILE_ZONE("gpu/geometry-dynamic");
        auto it = geometry.find(dynamic.geometry.address);
        // Each of these used to be one silent `return`. A dropped update leaves
        // the entry unbound, and draw_mesh then drops every mesh that
        // references it - so an invisible failure here surfaces only as a
        // number of disappeared draws, with no way back to the cause.
        if (it == geometry.end() || it->second.generation != dynamic.geometry.generation) {
            ++stats.dynamic_drop_unknown;
            note_drop("dynamic:unknown", dynamic.geometry.address, dynamic.geometry.generation,
                      it == geometry.end() ? 0u : it->second.generation, 0u);
            return;
        }
        if (it->second.asset->cpu == nullptr) {
            ++stats.dynamic_drop_no_cpu;
            note_drop("dynamic:no-cpu", dynamic.geometry.address, dynamic.geometry.generation,
                      it->second.generation, 0u);
            return;
        }
        GeometryEntry& entry = it->second;
        const ConvertedGeometry& cpu = *entry.asset->cpu;
        const std::size_t vertices = cpu.vertices.size();
        GEKKO_PROFILE_VALUE(vertices);
        const u64 bytes = std::max<std::size_t>(1u, vertices) * sizeof(shadow::render::RenderVertex);
        // Allocate the fence-owned destination before packing. This pose used
        // to be built into a temporary packed vector and then memcpy'd wholesale into the
        // upload ring every update. The mapped arena is already the final CPU
        // destination and remains owned by this slot until its fence retires.
        reserve(bytes + 16u);
        u8* staging = nullptr;
        const auto ref = allocate(bytes, 16u, staging,
                                  NativeGpu::RingUploadOwner::DynamicVertex);
        if (staging == nullptr) {
            ++stats.dynamic_drop_staging;
            note_drop("dynamic:staging", dynamic.geometry.address, dynamic.geometry.generation,
                      it->second.generation, static_cast<u32>(bytes));
            return;
        }
        static constexpr RwPackedStaticFields static_fields{
            offsetof(shadow::render::RenderVertex, color0), offsetof(shadow::render::RenderVertex, color1),
            kUvOffset, kUvOffset + kUvSetBytes};
        if (!rw_expand_dynamic_pos_nrm_strided(
                cpu, segment.payload_at(dynamic.positions), dynamic.positions.size,
                dynamic.normals.size != 0u ? segment.payload_at(dynamic.normals) : nullptr,
                dynamic.normals.size, staging, static_cast<std::size_t>(bytes),
                sizeof(shadow::render::RenderVertex), offsetof(shadow::render::RenderVertex, position),
                offsetof(shadow::render::RenderVertex, normal), &static_fields)) {
            ++stats.dynamic_drop_expand;
            note_drop("dynamic:expand", dynamic.geometry.address, dynamic.geometry.generation,
                      it->second.generation, static_cast<u32>(vertices));
            return;
        }
        if (vertices == 0u) std::memset(staging, 0, static_cast<std::size_t>(bytes));
        // The pose is drawn straight from the frame's upload ring, like every
        // immediate draw. The asset is shared by every guest entry holding this
        // content, so a pose must never be written into it.
        stats.cpu_staging_copy_bytes_avoided += bytes;
        stats.cpu_dynamic_scratch_bytes_avoided += vertices * 6u * sizeof(float);
        // The ring memory stays valid until this list's slot is recycled, and
        // a list split before the draw is covered by split_guard exactly as for
        // immediate draws.
        entry.dynamic_buffer = ref.ref;
        entry.dynamic_offset = ref.offset;
        entry.dynamic_bytes = static_cast<u32>(bytes);
        entry.dynamic_bound = true;
        entry.dynamic_frame = frame_ordinal;
        ++stats.dynamic_updates;
    }

    void on_geometry_release(const native::ResourceKey& key) {
        auto it = geometry.find(key.address);
        if (it == geometry.end()) return;
        // `>` NOT `!=`. The capture names the generation IT assigned; we may
        // hold an OLDER one, because a refused content leaves this entry on
        // its previous generation while the capture has already moved on. An
        // exact-match test then ignored the release entirely and leaked both
        // buffers along with the map entry. Anything at or older than the
        // generation being released is dead; only strictly NEWER content must
        // be protected from a stale release.
        if (it->second.generation > key.generation) return;
        // The guest freed its cache entry. The asset outlives it: the guest
        // re-instances evicted content constantly (see GeometryAsset).
        unbind_geometry(it->second);
        geometry.erase(it);
        --stats.geometry_live;
    }

    // ---- state application -------------------------------------------------------

    void apply_state(const native::CommandHeader& header, const u8* record) {
        CpuPhaseScope cpu_phase(stats, NativeGpu::CpuPhase::StateApply);
        ++state.serial;
        const u32 op_index = static_cast<u32>(header.op);
        if (op_index < 64u) state.draw_dirty_ops |= 1ull << op_index;
        bool draw_changed = false;
        const auto assign_draw = [&](auto& target, const auto& value) {
            if (std::memcmp(&target, &value, sizeof(target)) == 0) return;
            target = value;
            draw_changed = true;
        };
        switch (header.op) {
            case Op::Viewport:
                assign_draw(state.viewport, FrameSegment::read<native::Viewport>(record));
                break;
            case Op::Scissor: state.scissor = FrameSegment::read<native::Scissor>(record); break;
            case Op::ScissorOffset: {
                // Consumed, not dropped. GX subtracts this origin from every
                // vertex coordinate before the scissor test; the guest passes
                // (0, 0) on Westopolis, Gun Fortress, Lava Shelter and Final
                // Haunt - the BP register reads 0x02ACAB, which is
                // (0 + 342) / 2 on both axes - so it is identity here and the
                // renderer has nothing to apply. A non-zero origin would mean
                // tiled rendering into a virtual frame buffer, which this
                // renderer cannot express; it is refused loudly with its value
                // rather than quietly ignored.
                state.scissor_offset = FrameSegment::read<native::ScissorOffset>(record);
                if (state.scissor_offset.x != 0 || state.scissor_offset.y != 0) {
                    ++stats.scissor_offsets_unsupported;
                    if (stats.last_error.empty()) {
                        char message[160];
                        std::snprintf(message, sizeof(message),
                                      "GXSetScissorBoxOffset(%d, %d) is not implemented: the "
                                      "renderer has no virtual frame buffer to tile into",
                                      state.scissor_offset.x, state.scissor_offset.y);
                        stats.last_error = message;
                    }
                }
                break;
            }
            case Op::Projection:
                assign_draw(state.projection, FrameSegment::read<native::Projection>(record));
                break;
            case Op::PositionMatrix: {
                const auto m = FrameSegment::read<native::MatrixLoad>(record);
                if (m.id < 128u) {
                    auto& target = state.position_matrix[m.id];
                    if (std::memcmp(target.data(), m.m.data(), 12u * sizeof(float)) != 0) {
                        std::memcpy(target.data(), m.m.data(), 12u * sizeof(float));
                        draw_changed = true;
                    }
                }
                break;
            }
            case Op::NormalMatrix: {
                const auto m = FrameSegment::read<native::MatrixLoad>(record);
                if (m.id < 128u) {
                    auto& target = state.normal_matrix[m.id];
                    if (std::memcmp(target.data(), m.m.data(), 9u * sizeof(float)) != 0) {
                        std::memcpy(target.data(), m.m.data(), 9u * sizeof(float));
                        draw_changed = true;
                    }
                }
                break;
            }
            case Op::TextureMatrix: {
                const auto m = FrameSegment::read<native::MatrixLoad>(record);
                if (m.id < 128u) {
                    auto target = state.texture_matrix[m.id];
                    if (m.type == 1u) {
                        std::memcpy(target.data(), m.m.data(), 8u * sizeof(float));
                        target[8] = 0; target[9] = 0; target[10] = 1; target[11] = 0;
                    } else {
                        std::memcpy(target.data(), m.m.data(), 12u * sizeof(float));
                    }
                    assign_draw(state.texture_matrix[m.id], target);
                }
                break;
            }
            case Op::CurrentMatrix: {
                const u32 value = FrameSegment::read<native::Scalar>(record).value;
                assign_draw(state.current_matrix, value);
                break;
            }
            case Op::Blend:
                assign_draw(state.blend, FrameSegment::read<native::Blend>(record));
                break;
            case Op::DepthMode: state.depth = FrameSegment::read<native::DepthMode>(record); break;
            case Op::DepthBeforeTexture: state.depth_before_texture = FrameSegment::read<native::Scalar>(record).value; break;
            case Op::AlphaCompare: {
                const auto alpha = FrameSegment::read<native::AlphaCompare>(record);
                assign_draw(state.alpha, alpha);
                state.alpha_test_active = !alpha_test_passes_everything(state.alpha);
                break;
            }
            case Op::Cull: state.cull = FrameSegment::read<native::Scalar>(record).value; break;
            case Op::Fog:
                assign_draw(state.fog, FrameSegment::read<native::Fog>(record));
                break;
            case Op::ColorUpdate: state.color_update = FrameSegment::read<native::Scalar>(record).value; break;
            case Op::AlphaUpdate: state.alpha_update = FrameSegment::read<native::Scalar>(record).value; break;
            case Op::ChannelCount: {
                const u32 value = FrameSegment::read<native::Scalar>(record).value;
                assign_draw(state.channel_count, value);
                break;
            }
            case Op::ChannelControl: {
                const auto c = FrameSegment::read<native::ChannelControl>(record);
                ChannelState value;
                value.enable = c.enable;
                value.ambient_source = c.ambient_source;
                value.material_source = c.material_source;
                value.light_mask = c.light_mask;
                value.diffuse = c.diffuse;
                value.attenuation = c.attenuation;
                switch (c.channel) {
                    case 0: assign_draw(state.channel[0], value); break;  // COLOR0
                    case 1: assign_draw(state.channel[1], value); break;  // COLOR1
                    case 2: assign_draw(state.channel[2], value); break;  // ALPHA0
                    case 3: assign_draw(state.channel[3], value); break;  // ALPHA1
                    case 4:  // COLOR0A0
                        assign_draw(state.channel[0], value);
                        assign_draw(state.channel[2], value);
                        break;
                    case 5:  // COLOR1A1
                        assign_draw(state.channel[1], value);
                        assign_draw(state.channel[3], value);
                        break;
                    default: break;
                }
                break;
            }
            case Op::MaterialColor:
            case Op::AmbientColor: {
                const auto c = FrameSegment::read<native::ChannelColor>(record);
                auto& table = header.op == Op::MaterialColor ? state.material : state.ambient;
                const u32 channel = c.channel;
                const u32 index = (channel == 1u || channel == 3u || channel == 5u) ? 1u : 0u;
                const bool rgb = channel == 0u || channel == 1u || channel >= 4u;
                const bool a = channel == 2u || channel == 3u || channel >= 4u;
                const auto before = table[index];
                if (rgb) { table[index][0] = c.rgba[0]; table[index][1] = c.rgba[1]; table[index][2] = c.rgba[2]; }
                if (a) table[index][3] = c.rgba[3];
                if (table[index] != before) draw_changed = true;
                break;
            }
            case Op::Light: {
                const auto l = FrameSegment::read<native::Light>(record);
                bool light_changed = false;
                for (u32 index = 0; index < 8u; ++index) {
                    if (((l.mask >> index) & 1u) == 0u) continue;
                    if (std::memcmp(&state.light[index], &l, sizeof(l)) != 0) {
                        state.light[index] = l;
                        light_changed = true;
                    }
                }
                if (light_changed) ++state.light_generation;
                break;
            }
            case Op::TexGenCount: {
                const u32 value = FrameSegment::read<native::Scalar>(record).value;
                assign_draw(state.texgen_count, value);
                break;
            }
            case Op::TexGen: {
                const auto t = FrameSegment::read<native::TexGen>(record);
                if (t.coord < 8u) assign_draw(state.texgen[t.coord], t);
                break;
            }
            case Op::TevRegisterColor: {
                const auto t = FrameSegment::read<native::TevColor>(record);
                if (t.index < 4u) assign_draw(state.tev_color[t.index], t.rgba);
                break;
            }
            case Op::BindTexture: {
                const auto b = FrameSegment::read<native::BindTexture>(record);
                if (b.map < 8u) assign_draw(state.bind[b.map], b);
                break;
            }
            default:
                // A GX STATE CHANNEL THE CAPTURE EMITS AND NOTHING CONSUMES.
                // Op::DestinationAlpha and Op::TevKonstColor are both emitted
                // by rw_capture and appear nowhere in this file, so they were
                // swallowed here indistinguishably from an op that genuinely
                // has no native meaning. Op::TevRegisterColor by contrast IS
                // handled above, so this is two specific channels rather than
                // a whole family.
                //
                // Counted, not fixed: dest-alpha and TEV konstants are
                // appearance, and what they should DO natively is a decision
                // about the look, not a mechanical port.
                ++stats.state_ops_unconsumed;
                if (stats.state_op_unconsumed_first == 0u) {
                    stats.state_op_unconsumed_first = static_cast<u32>(header.op);
                }
                break;
        }
        if (draw_changed) {
            ++state.draw_generation;
            if (op_index < 64u) state.draw_changed_ops |= 1ull << op_index;
            ++stats.constant_generation_changes[op_index];
        }
    }

    bool upload_lights(bool force) {
        if (!force && lights_valid && bound_light_generation == state.light_generation) return true;
        CpuPhaseScope cpu_phase(stats, NativeGpu::CpuPhase::LightUpload);
        LightConstants lights{};
        for (u32 index = 0; index < 8u; ++index) {
            const native::Light& l = state.light[index];
            for (u32 c = 0; c < 4u; ++c) lights.color[index][c] = l.rgba[c] / 255.0f;
            for (u32 c = 0; c < 3u; ++c) {
                lights.position[index][c] = l.position[c];
                lights.direction[index][c] = l.direction[c];
                lights.cos_attenuation[index][c] = l.cos_attenuation[c];
                lights.dist_attenuation[index][c] = l.dist_attenuation[c];
            }
        }
        u8* out = nullptr;
        const auto ref = allocate(256u * ((sizeof(LightConstants) + 255u) / 256u), 256u, out,
                                  NativeGpu::RingUploadOwner::LightConstants);
        if (out == nullptr || ref.ref == nullptr) {
            lights_valid = false;
            return false;
        }
        std::memcpy(out, &lights, sizeof(lights));
        slot->list->setGraphicsRootDescriptor(ref, 1);
        bound_light_generation = state.light_generation;
        lights_valid = true;
        return true;
    }

    u32 texture_index(u32 map, float transform[4], bool& missing) {
        CpuPhaseScope cpu_phase(stats, NativeGpu::CpuPhase::DescriptorLookup);
        transform[0] = 1.0f; transform[1] = 1.0f; transform[2] = 0.0f; transform[3] = 0.0f;
        const native::BindTexture& b = state.bind[map];
        if (b.texture.address == 0u) return kTextureWhite;
        if ((b.flags & native::kTextureCameraTarget) != 0u) {
            auto it = targets.find(b.texture.address);
            if (it == targets.end() || it->second.texture == nullptr) {
                // A camera texture sampled before any copy: the guest's zero-filled
                // pixels. The magenta marker would be wrong here.
                return kTextureWhite;
            }
            // Texel (x, y) of the bound (sub-)raster is texel (x, y) of the parent.
            transform[0] = it->second.guest_width != 0u ? static_cast<float>(b.width) / it->second.guest_width : 1.0f;
            transform[1] = it->second.guest_height != 0u ? static_cast<float>(b.height) / it->second.guest_height : 1.0f;
            return it->second.bindless;
        }
        auto it = textures.find(b.texture.address);
        if (it == textures.end() || it->second.generation != b.texture.generation) {
            missing = true;
            return kTextureMagenta;
        }
        return it->second.bindless;
    }

    // Texels of map 0 per guest pixel for the draw about to be issued, or 0
    // when it cannot be measured. This is the quantity the legacy raster
    // computes per triangle (render_geometry.h texel_footprint) and the only
    // one that separates a kernel tap from a magnifying composite.
    //
    // The guest viewport and scissor CANNOT stand in for it: across a whole
    // Final Haunt route the guest sets each of them exactly twice, both at
    // 640x480. The glare ladder bounds its passes with the quad's own
    // coordinates and then copies a 128x128 region out of the EFB, so the
    // destination extent lives in the vertices and nowhere else.
    //
    // Measured from the bounding boxes: a ratio, so it does not care that an
    // ortho 2D quad is in guest screen units while a projected one is not - a
    // draw whose positions are not screen-like simply yields a density that
    // fails both thresholds, which is the conservative answer.
    void measure_texel_density(const u8* vertices, u32 count, u32 attributes) {
        texel_density = 0.0f;
        if (vertices == nullptr || count < 3u) return;
        if ((attributes & native::kImmediateUv0) == 0u) return;
        float min_x = 1e30f, max_x = -1e30f, min_y = 1e30f, max_y = -1e30f;
        float min_u = 1e30f, max_u = -1e30f, min_v = 1e30f, max_v = -1e30f;
        for (u32 i = 0; i < count; ++i) {
            shadow::render::RenderVertex v{};
            std::memcpy(&v, vertices + std::size_t(i) * sizeof(shadow::render::RenderVertex),
                        sizeof(v));
            min_x = std::min(min_x, v.position[0]); max_x = std::max(max_x, v.position[0]);
            min_y = std::min(min_y, v.position[1]); max_y = std::max(max_y, v.position[1]);
            min_u = std::min(min_u, v.uv[0][0]); max_u = std::max(max_u, v.uv[0][0]);
            min_v = std::min(min_v, v.uv[0][1]); max_v = std::max(max_v, v.uv[0][1]);
        }
        const float screen_w = max_x - min_x, screen_h = max_y - min_y;
        const float uv_w = max_u - min_u, uv_h = max_v - min_v;
        if (!(screen_w > 0.0f) || !(screen_h > 0.0f) || !(uv_w > 0.0f) || !(uv_h > 0.0f)) return;
        const native::BindTexture& b = state.bind[0];
        const float texels_w = uv_w * static_cast<float>(std::max<u32>(1u, b.width));
        const float texels_h = uv_h * static_cast<float>(std::max<u32>(1u, b.height));
        texel_density = std::sqrt((texels_w * texels_h) / (screen_w * screen_h));
    }

    // Guest-texel footprint for a resident EFB copy bound to `map`, or 0 when
    // this draw must not reconstruct.
    //
    // The rule is the legacy GX raster's, restated against what the native
    // renderer knows (renderer.h effect_footprint_target and
    // gx_raster_shader.h UseTextureFootprint):
    //   * only a camera target - a GPU-resident EFB copy - qualifies, and only
    //     when the guest samples it with LINEAR magnification. A point-sampled
    //     copy is the motion-history case and keeps its native sub-texels.
    //   * a REDUCED copy (smaller than the guest raster in both axes: the
    //     glare ladder's 128x128 and 64x64 levels) reconstructs from 0.75
    //     guest texels per pixel, because a 1:1 draw of it IS a kernel tap.
    //   * a FULL-raster copy (the scene save, the glare capture) reconstructs
    //     only when MINIFIED - threshold 1.5 - so its identity add-back stays a
    //     lossless native sample.
    //   * a magnifying draw - the bloom composite stretching 128x128 over the
    //     frame - never reconstructs; it keeps native bilinear and stays smooth.
    // Texels per pixel is measured from the bound guest extent against the
    // guest viewport, which is the same quantity the legacy path carries per
    // triangle. At internal scale 1 the taps are 1x1 and this is identity.
    u32 footprint_taps(u32 map) const {
        const native::BindTexture& b = state.bind[map];
        if (!effect_footprint) return 0u;
        if (b.texture.address == 0u || (b.flags & native::kTextureCameraTarget) == 0u) return 0u;
        if (b.mag_filter != 1u) return 0u;  // GX_LINEAR magnification only
        const auto it = targets.find(b.texture.address);
        if (it == targets.end() || it->second.texture == nullptr) return 0u;
        const CameraTarget& target = it->second;
        if (target.guest_width == 0u || target.guest_height == 0u) return 0u;
        const float texels_per_pixel = texel_density;
        if (!(texels_per_pixel > 0.0f)) return 0u;
        const bool sub_raster = b.width < 640u && b.height < 480u;
        if (texels_per_pixel < (sub_raster ? 0.75f : 1.5f)) return 0u;
        const auto taps = [](u32 native, u32 guest) -> u32 {
            if (guest == 0u) return 1u;
            const long rounded = std::lround(static_cast<double>(native) / guest);
            return static_cast<u32>(std::clamp<long>(rounded, 1, 16));
        };
        const u32 taps_x = taps(target.native_width, target.guest_width);
        const u32 taps_y = taps(target.native_height, target.guest_height);
        if (taps_x <= 1u && taps_y <= 1u) return 0u;
        return taps_x | (taps_y << 8);
    }

    u32 sampler_index(u32 map) const {
        const native::BindTexture& b = state.bind[map];
        const u32 min_linear = (b.min_filter == 1u || b.min_filter == 3u || b.min_filter == 5u) ? 1u : 0u;
        const u32 mag_linear = b.mag_filter == 1u ? 1u : 0u;
        const auto wrap = [](u32 w) -> u32 { return w == 1u ? 1u : (w == 2u ? 2u : 0u); };
        return ((min_linear * 2u + mag_linear) * 3u + wrap(b.wrap_s)) * 3u + wrap(b.wrap_t);
    }

    void fill_texgen(u32 coord, u32& packed, float rows[3][4]) {
        packed = 0;
        for (u32 r = 0; r < 3u; ++r) for (u32 c = 0; c < 4u; ++c) rows[r][c] = (r == c) ? 1.0f : 0.0f;
        if (coord >= state.texgen_count) return;
        const native::TexGen& t = state.texgen[coord];
        packed = static_cast<u32>(t.function) | static_cast<u32>(t.source) << 8 | 1u << 16;
        if (t.matrix != 60u && t.matrix < 128u) {
            const auto& m = state.texture_matrix[t.matrix];
            for (u32 r = 0; r < 3u; ++r) for (u32 c = 0; c < 4u; ++c) rows[r][c] = m[r * 4u + c];
        }
    }

    static u32 pack_channel(const ChannelState& c) {
        return (c.enable & 1u) | ((c.ambient_source & 1u) << 1) | ((c.material_source & 1u) << 2) |
               ((c.diffuse & 3u) << 4) | ((c.attenuation & 3u) << 6) | ((c.light_mask & 255u) << 8);
    }

    // ---- draws -------------------------------------------------------------------

    const plume::RenderPipeline* pipeline_for(const PipelineKey& key) {
        CpuPhaseScope cpu_phase(stats, NativeGpu::CpuPhase::PipelineLookup);
        const u64 packed = key.pack();
        auto it = pipelines.find(packed);
        if (it != pipelines.end()) {
            ++stats.pso_hits;
            return it->second.get();
        }
        ++stats.pso_misses;
        // MISS BRANCH ONLY, and it must stay that way: pipeline_for is called
        // once per draw (17.4k/s) and the hit path above is a hash lookup. The
        // key space collapses to 13-18 pipelines for a whole run, each costing
        // ~2 ms the first time its blend/cull/alpha combination appears - a
        // real multi-millisecond hitch with nothing to attribute it to.
        GEKKO_PROFILE_ZONE("gpu/pso-create");
        const u64 begin = now_ns();
        static const plume::RenderInputSlot kSlot(0, kVertexStride);
        static const plume::RenderInputElement kElements[] = {
            plume::RenderInputElement("POSITION", 0, 0, plume::RenderFormat::R32G32B32_FLOAT, 0, offsetof(shadow::render::RenderVertex, position)),
            plume::RenderInputElement("NORMAL", 0, 1, plume::RenderFormat::R32G32B32_FLOAT, 0, offsetof(shadow::render::RenderVertex, normal)),
            plume::RenderInputElement("COLOR", 0, 2, plume::RenderFormat::R8G8B8A8_UNORM, 0, offsetof(shadow::render::RenderVertex, color0)),
            plume::RenderInputElement("COLOR", 1, 3, plume::RenderFormat::R8G8B8A8_UNORM, 0, offsetof(shadow::render::RenderVertex, color1)),
            plume::RenderInputElement("TEXCOORD", 0, 4, plume::RenderFormat::R32G32_FLOAT, 0, kUvOffset),
            plume::RenderInputElement("TEXCOORD", 1, 5, plume::RenderFormat::R32G32_FLOAT, 0, kUvOffset + kUvSetBytes),
        };
        plume::RenderGraphicsPipelineDesc desc;
        desc.pipelineLayout = layout.get();
        desc.vertexShader = vertex_shader.get();
        desc.pixelShader =
            key.footprint != 0u
                ? (key.alpha_test != 0u ? pixel_shader_alpha_footprint.get()
                                        : pixel_shader_footprint.get())
                : (key.alpha_test != 0u ? pixel_shader_alpha.get() : pixel_shader.get());
        desc.primitiveTopology = key.topology == 0u ? plume::RenderPrimitiveTopology::TRIANGLE_LIST
                                 : key.topology == 1u ? plume::RenderPrimitiveTopology::LINE_LIST
                                                       : plume::RenderPrimitiveTopology::POINT_LIST;
        desc.depthClipEnabled = true;
        desc.depthEnabled = key.depth_enable != 0u;
        desc.depthWriteEnabled = key.depth_enable != 0u && key.depth_write != 0u;
        desc.depthFunction = compare_function(key.depth_func);
        desc.depthTargetFormat = kDepthFormat;
        desc.cullMode = key.cull == 1u ? plume::RenderCullMode::FRONT
                        : key.cull == 2u ? plume::RenderCullMode::BACK
                                          : plume::RenderCullMode::NONE;
        desc.frontFace = plume::RenderFrontFace::CLOCKWISE;
        desc.renderTargetFormat[0] = kColorFormat;
        desc.renderTargetCount = 1;
        plume::RenderBlendDesc blend;
        blend.renderTargetWriteMask = key.color_write != 0u
            ? static_cast<u8>(static_cast<u8>(plume::RenderColorWriteEnable::RED) |
                              static_cast<u8>(plume::RenderColorWriteEnable::GREEN) |
                              static_cast<u8>(plume::RenderColorWriteEnable::BLUE))
            : static_cast<u8>(0u);
        if (key.blend_type == 1u || key.blend_type == 3u) {  // BLEND (LOGIC approximated as BLEND)
            blend.blendEnabled = true;
            blend.srcBlend = blend_factor(key.blend_src, true);
            blend.dstBlend = blend_factor(key.blend_dst, false);
            blend.blendOp = plume::RenderBlendOperation::ADD;
        } else if (key.blend_type == 2u) {  // SUBTRACT: dst - src
            blend.blendEnabled = true;
            blend.srcBlend = plume::RenderBlend::ONE;
            blend.dstBlend = plume::RenderBlend::ONE;
            blend.blendOp = plume::RenderBlendOperation::REV_SUBTRACT;
        } else {
            blend.blendEnabled = false;
            blend.srcBlend = plume::RenderBlend::ONE;
            blend.dstBlend = plume::RenderBlend::ZERO;
            blend.blendOp = plume::RenderBlendOperation::ADD;
        }
        blend.srcBlendAlpha = plume::RenderBlend::ONE;
        blend.dstBlendAlpha = plume::RenderBlend::ZERO;
        blend.blendOpAlpha = plume::RenderBlendOperation::ADD;
        desc.renderTargetBlend[0] = blend;
        desc.inputSlots = &kSlot;
        desc.inputSlotsCount = 1;
        desc.inputElements = kElements;
        desc.inputElementsCount = 6;
        auto pipeline = device->createGraphicsPipeline(desc);
        stats.pipeline_create_ns += now_ns() - begin;
        ++stats.pipelines;
        // A failed PSO is null and is cached as null under its key, so every
        // draw of that key is refused in prepare_draw (skipped_null_pipeline)
        // rather than bound as SetPipelineState(nullptr).
        if (pipeline == nullptr) ++stats.pipeline_create_failures;
        const plume::RenderPipeline* raw = pipeline.get();
        pipelines.emplace(packed, std::move(pipeline));
        return raw;
    }

    // Returns false when the draw can produce no pixels.
    // `modulate_table` is the operand order of a MODULATE stage: false = the
    // GXSetTevOp(GX_MODULATE) preset (b = texture, c = rasterised colour),
    // true = the RwGC one-stage table (b = rasterised, c = texture). The
    // Flipper lerp is asymmetric in b and c, so the order is part of the
    // program (native_shaders.hlsl tev_lerp).
    bool prepare_draw(u32 topology, native::MaterialProgram program, u32 vertex_flags,
                      bool modulate_table = false) {
        CpuPhaseScope cpu_prepare(stats, NativeGpu::CpuPhase::PrepareDraw);
        if (state.cull == 3u && topology == 0u) {
            // GX_CULL_ALL: the game intentionally drawing nothing. Faithful
            // emulation, and it must stay at whatever value it reaches. It
            // shared one counter with the PSO failure below, which is the
            // opposite thing - a draw the renderer LOST. `skipped 0`
            // exonerated nothing at any other value.
            ++stats.skipped_draws;
            ++stats.skipped_cull_all;
            return false;
        }
        PipelineKey key;
        key.topology = static_cast<u8>(topology);
        const bool test = state.alpha_test_active;
        key.alpha_test = test ? 1u : 0u;
        // The footprint variants are a different pixel shader, so the decision
        // has to be in the key or the pipeline would keep the ordinary one and
        // the reconstruction would never run.
        const bool samples_map0 = program == native::MaterialProgram::Modulate ||
                                  program == native::MaterialProgram::DefaultTx2;
        const u32 footprint0 = samples_map0 ? footprint_taps(0) : 0u;
        key.footprint = footprint0 != 0u ? 1u : 0u;
        if (key.footprint != 0u) ++stats.footprint_draws;
        if (test && state.depth_before_texture != 0u) ++stats.depth_before_texture_with_alpha_test;
        // GX_BM_NONE 0, BLEND 1, LOGIC 2, SUBTRACT 3 -> key 0 none, 1 blend, 2 subtract, 3 logic.
        switch (state.blend.type) {
            case 1: key.blend_type = 1u; break;
            case 2: key.blend_type = 3u; ++stats.logic_op_draws; break;
            case 3: key.blend_type = 2u; break;
            default: key.blend_type = 0u; break;
        }
        key.blend_src = state.blend.source;
        key.blend_dst = state.blend.destination;
        // (INV)SRCALPHA as the source factor is applied in the shader with GX's
        // truncation and the pipeline blends ONE (see native_shaders.hlsl finish).
        u32 premultiply = 0;
        if (key.blend_type == 1u && (key.blend_src == 4u || key.blend_src == 5u)) {
            premultiply = key.blend_src == 4u ? 1u : 2u;
            key.blend_src = 1u;
        }
        key.depth_enable = state.depth.enable;
        key.depth_func = state.depth.function;
        key.depth_write = state.depth.update;
        key.cull = topology == 0u ? static_cast<u8>(state.cull & 3u) : 0u;
        key.color_write = state.color_update != 0u ? 1u : 0u;
        const plume::RenderPipeline* pipeline = pipeline_for(key);
        if (pipeline == nullptr) {
            // The device refused this pipeline (pipeline_create_failures): the
            // draw is lost, and counted as lost rather than as a GX_CULL_ALL.
            ++stats.skipped_draws;
            ++stats.skipped_null_pipeline;
            return false;
        }
        if (pipeline != bound_pipeline) {
            slot->list->setPipeline(pipeline);
            bound_pipeline = pipeline;
            if (stats.cpu_profile_enabled) ++stats.cpu_draw_state_emits[0];
        }
        DrawIdentity identity{};
        identity.state_generation = state.draw_generation;
        identity.texture_resolution_generation = texture_resolution_generation;
        std::memcpy(&identity.content_aspect_bits, &settings.content_aspect,
                    sizeof(identity.content_aspect_bits));
        identity.program = static_cast<u32>(program) | (modulate_table ? 0x100u : 0u);
        identity.vertex_flags = vertex_flags;
        identity.premultiply = premultiply;
        identity.footprint_taps0 = footprint0;
        if (draw_hud) {
            // All FOUR affine coefficients. `by` was missing, and the early-out
            // below returns before DrawConstants is rebuilt, so two draws that
            // differed only in their vertical anchor bucket collided and the
            // second silently reused the first's vertical offset. Unreachable
            // at the defaults (hud_scale 1 and hud_inset 0 make every `by`
            // zero) and reachable with either of them set.
            //
            // The coefficients are hashed through their bit patterns, not
            // through a float-to-u32 cast: bx and by are routinely negative,
            // and converting a negative float to an unsigned integer is
            // undefined behaviour, not a wrap.
            const auto bits = [](float value) {
                u32 raw = 0;
                std::memcpy(&raw, &value, sizeof(raw));
                return static_cast<u64>(raw);
            };
            identity.hud_enabled = 1u;
            identity.hud_affine = {static_cast<u32>(bits(draw_affine.ax)),
                                   static_cast<u32>(bits(draw_affine.bx)),
                                   static_cast<u32>(bits(draw_affine.ay)),
                                   static_cast<u32>(bits(draw_affine.by))};
        }
        const native::Viewport& v = state.viewport;
        if (!viewport_valid || std::memcmp(&v, &bound_viewport, sizeof(v)) != 0) {
            const float f = static_cast<float>(efb.scale.factor);
            const plume::RenderViewport viewport(v.x * f, v.y * f, v.width * f,
                                                 v.height * f, std::clamp(v.near_z, 0.0f, 1.0f),
                                                 std::clamp(v.far_z, 0.0f, 1.0f));
            slot->list->setViewports(&viewport, 1);
            if (stats.cpu_profile_enabled) ++stats.cpu_draw_state_emits[1];
            bound_viewport = v;
            viewport_valid = true;
        }
        native::Scissor s = state.scissor;
        if (draw_hud) {
            // The guest's scissor moves with the group, so a centred composition
            // keeps masking the art the guest parked just outside the 4:3 frame.
            s32 left = static_cast<s32>(std::floor(apply_hud_x(draw_affine, static_cast<float>(s.x))));
            s32 right = static_cast<s32>(std::floor(apply_hud_x(draw_affine, static_cast<float>(s.x + s.width - 1))));
            s32 top = static_cast<s32>(std::floor(apply_hud_y(draw_affine, static_cast<float>(s.y))));
            s32 bottom = static_cast<s32>(std::floor(apply_hud_y(draw_affine, static_cast<float>(s.y + s.height - 1))));
            if (left > right) std::swap(left, right);
            if (top > bottom) std::swap(top, bottom);
            left = std::max(0, left - 1);
            top = std::max(0, top - 1);
            right = std::min(639, right + 1);
            bottom = std::min(479, bottom + 1);
            s.x = left;
            s.y = top;
            s.width = right - left + 1;
            s.height = bottom - top + 1;
        }
        if (!scissor_valid || std::memcmp(&s, &bound_scissor, sizeof(s)) != 0) {
            // Exact: an integer scale maps a guest pixel boundary onto a native
            // pixel boundary, so no scissor edge lands mid-pixel and no two
            // adjacent guest regions can round onto the same native column.
            const s32 f = static_cast<s32>(efb.scale.factor);
            const s32 left = s.x * f;
            const s32 top = s.y * f;
            const s32 right = (s.x + s.width) * f;
            const s32 bottom = (s.y + s.height) * f;
            const plume::RenderRect rect(std::max(0, left), std::max(0, top),
                                         std::min<s32>(right, static_cast<s32>(efb_width)),
                                         std::min<s32>(bottom, static_cast<s32>(efb_height)));
            slot->list->setScissors(&rect, 1);
            if (stats.cpu_profile_enabled) ++stats.cpu_draw_state_emits[2];
            bound_scissor = s;
            scissor_valid = true;
        }

        if (constants_valid && identity == last_draw_identity) {
            ++stats.constant_reuses;
            ++stats.constant_identity_reuses;
            state.draw_dirty_ops = 0;
            state.draw_changed_ops = 0;
            return upload_lights(false);
        }
        // Ignore the generation itself when attributing non-state inputs.
        DrawIdentity previous_non_state = last_draw_identity;
        previous_non_state.state_generation = identity.state_generation;
        const bool non_state_changed = !(previous_non_state == identity);
        constexpr u64 restorable_ops = (1ull << static_cast<u32>(Op::Projection)) |
                                      (1ull << static_cast<u32>(Op::Fog));
        const bool restored_sources = constants_valid && !non_state_changed &&
            state.draw_changed_ops != 0u &&
            (state.draw_changed_ops & ~restorable_ops) == 0u &&
            std::memcmp(&state.projection, &last_constant_projection,
                        sizeof(state.projection)) == 0 &&
            std::memcmp(&state.fog, &last_constant_fog, sizeof(state.fog)) == 0;
        last_draw_identity = identity;
        if (restored_sources && !validate_constant_restoration) {
            // A->B->A state transitions were applied in order; no draw consumed
            // B. Every other constant input and the bound upload remain valid.
            ++stats.constant_reuses;
            ++stats.constant_restored_reuses;
            state.draw_dirty_ops = 0;
            state.draw_changed_ops = 0;
            return upload_lights(false);
        }

        ++stats.constant_materializations;
        CpuPhaseScope cpu_constants(stats, NativeGpu::CpuPhase::Constants);
        const bool sample_constants = stats.cpu_profile_enabled &&
            stats.constant_materializations % NativeGpu::kCpuConstantSampleStride == 0u;
        CpuPhaseScope cpu_sample_transform(stats,
            NativeGpu::CpuPhase::SampleConstantsTransformProjection, sample_constants);
        DrawConstants c{};
        const u32 matrix = state.current_matrix < 128u ? state.current_matrix : 0u;
        const auto& pm = state.position_matrix[matrix];
        for (u32 r = 0; r < 3u; ++r) for (u32 col = 0; col < 4u; ++col) c.pos_row[r][col] = pm[r * 4u + col];
        const auto& nm = state.normal_matrix[matrix];
        for (u32 r = 0; r < 3u; ++r) for (u32 col = 0; col < 3u; ++col) c.nrm_row[r][col] = nm[r * 3u + col];
        const native::Projection& p = state.projection;
        c.proj_a[0] = static_cast<float>(p.type);
        c.proj_a[1] = p.p[0];
        c.proj_a[2] = p.p[1];
        c.proj_a[3] = p.p[2];
        c.proj_b[0] = p.p[3];
        c.proj_b[1] = p.p[4];
        c.proj_b[2] = p.p[5];
        if (stats.cpu_profile_enabled && p.type == 0u) {
            if (cpu_perspective_key_valid &&
                std::memcmp(&p, &cpu_last_perspective_key, sizeof(p)) == 0 &&
                std::memcmp(&settings.content_aspect, &cpu_last_perspective_aspect,
                            sizeof(settings.content_aspect)) == 0) {
                ++stats.cpu_projection_key_hits;
            } else {
                ++stats.cpu_projection_key_misses;
                cpu_last_perspective_key = p;
                cpu_last_perspective_aspect = settings.content_aspect;
                cpu_perspective_key_valid = true;
            }
        }
        // Widescreen goes through the canonical camera model (gekko/camera.h),
        // which is the single authority for the renderable area. The GX block
        // is DECODED into physical frustum extents, the aspect policy changes
        // those extents, and the matrix is re-encoded from them - so the
        // culling volume that gekko::camera::view_frustum() derives from the
        // same extents describes exactly this projection. Nothing here patches
        // a coefficient.
        if (p.type == 0u) {
            CpuPhaseScope cpu_projection(stats, NativeGpu::CpuPhase::ProjectionPolicy);
            camera::CameraPolicy policy;
            policy.target_aspect = settings.content_aspect;
            // Derived from content_aspect - the same input this path has
            // always used - through the ONE Coverage implementation. Not read
            // from a stored field: a denormalised copy is only correct after a
            // resize has assigned it, and before that it reads 1.0, which
            // leaves the projection un-widened while the presenter still fits
            // to the target aspect. That is a horizontally stretched image.
            const float widen =
                camera::Coverage::from_aspect(settings.content_aspect).extent_scale;
            // A neutral policy leaves the camera untouched, so the coefficients
            // are already correct. Pass them through rather than round-tripping
            // through the decoder: a float decode/encode pair is not guaranteed
            // bit-exact, and 4:3 byte-identity is a hard gate.
            if (widen != 1.0f) {
                if (stats.cpu_profile_enabled) ++stats.cpu_projection_policy_non_neutral;
                const float block[6] = {p.p[0], p.p[1], p.p[2], p.p[3], p.p[4], p.p[5]};
                camera::Camera decoded;
                if (camera::decode::gx_projection(p.type, block, camera::Transform{}, decoded)) {
                    const camera::Projection out =
                        camera::projection_matrix(camera::apply_policy(decoded, policy));
                    c.proj_a[1] = out.m00;
                    c.proj_a[2] = out.m02;
                    c.proj_a[3] = out.m11;
                    c.proj_b[0] = out.m12;
                    c.proj_b[1] = out.m22;
                    c.proj_b[2] = out.m23;
                }
            }
        }
        cpu_sample_transform.finish();
        CpuPhaseScope cpu_sample_texgen(stats, NativeGpu::CpuPhase::SampleConstantsTexgen,
                                      sample_constants);
        fill_texgen(0, c.texgen0, c.tex0_row);
        fill_texgen(1, c.texgen1, c.tex1_row);
        cpu_sample_texgen.finish();
        CpuPhaseScope cpu_sample_colors(stats,
            NativeGpu::CpuPhase::SampleConstantsColorConversion, sample_constants);
        c.channel_count = state.channel_count;
        c.vertex_flags = vertex_flags;
        c.chan0_color = pack_channel(state.channel[0]);
        c.chan0_alpha = pack_channel(state.channel[2]);
        c.chan1_color = pack_channel(state.channel[1]);
        c.chan1_alpha = pack_channel(state.channel[3]);
        for (u32 i = 0; i < 4u; ++i) {
            c.mat_color0[i] = state.material[0][i] / 255.0f;
            c.amb_color0[i] = state.ambient[0][i] / 255.0f;
            c.mat_color1[i] = state.material[1][i] / 255.0f;
            c.amb_color1[i] = state.ambient[1][i] / 255.0f;
            c.tev_reg0[i] = state.tev_color[1][i] / 255.0f;
            c.tev_reg1[i] = state.tev_color[2][i] / 255.0f;
            c.fog_color[i] = state.fog.rgba[i] / 255.0f;
        }
        cpu_sample_colors.finish();
        CpuPhaseScope cpu_sample_fog(stats,
            NativeGpu::CpuPhase::SampleConstantsFogCoefficients, sample_constants);
        // GX fog coefficients (GXSetFog): perspective A = far*near/((far-near)(end-start)),
        // B = far/(far-near), C = start/(end-start); orthographic A = (far-near)/(end-start),
        // C = (start-near)/(end-start).
        const native::Fog& f = state.fog;
        c.fog_type = f.type;
        const float range = f.end - f.start;
        const float depth_range = f.far_z - f.near_z;
        if (range != 0.0f && depth_range != 0.0f) {
            if ((f.type & 8u) != 0u) {
                c.fog_params[0] = depth_range / range;
                c.fog_params[2] = (f.start - f.near_z) / range;
            } else {
                c.fog_params[0] = f.far_z * f.near_z / (depth_range * range);
                c.fog_params[1] = f.far_z / depth_range;
                c.fog_params[2] = f.start / range;
            }
        } else {
            c.fog_type = 0u;
        }
        cpu_sample_fog.finish();
        CpuPhaseScope cpu_sample_textures(stats,
            NativeGpu::CpuPhase::SampleConstantsResolvedTexturesAlpha, sample_constants);
        c.program = identity.program;
        c.blend_premultiply = premultiply;
        if (draw_hud) {
            c.hud_enabled = 1u;
            c.hud_affine[0] = draw_affine.ax;
            c.hud_affine[1] = draw_affine.bx;
            c.hud_affine[2] = draw_affine.ay;
            c.hud_affine[3] = draw_affine.by;
            c.viewport_rect[0] = v.x;
            c.viewport_rect[1] = v.y;
            c.viewport_rect[2] = v.width;
            c.viewport_rect[3] = v.height;
        }
        c.alpha_compare = static_cast<u32>(state.alpha.comp0 & 7u) | static_cast<u32>(state.alpha.ref0) << 3 |
                          static_cast<u32>(state.alpha.op & 3u) << 11 | static_cast<u32>(state.alpha.comp1 & 7u) << 13 |
                          static_cast<u32>(state.alpha.ref1) << 16;
        bool missing = false;
        if (program == native::MaterialProgram::Modulate || program == native::MaterialProgram::DefaultTx2) {
            c.texture0 = texture_index(0, c.uv_transform0, missing);
            c.sampler0 = sampler_index(0);
        } else {
            c.uv_transform0[0] = c.uv_transform0[1] = 1.0f;
        }
        c.uv_transform1[0] = c.uv_transform1[1] = 1.0f;
        if (missing) ++stats.missing_textures;
        c.footprint_taps0 = footprint0;
        cpu_sample_textures.finish();
        CpuPhaseScope cpu_sample_compare(stats,
            NativeGpu::CpuPhase::SampleConstantsCompareUploadAttribution, sample_constants);

        const u64 dirty_ops = state.draw_dirty_ops;
        const u64 changed_ops = state.draw_changed_ops;
        state.draw_dirty_ops = 0;
        state.draw_changed_ops = 0;
        if (restored_sources) {
            ++stats.constant_restore_checks;
            if (std::memcmp(&c, last_constants.data(), sizeof(c)) != 0) {
                ++stats.constant_restore_mismatches;
                stats.last_error = "restored constant source failed byte-equivalence check";
                constants_valid = false;
                return false;
            }
        }
        if (std::memcmp(&c, last_constants.data(), sizeof(c)) != 0 || !constants_valid) {
            u8* out = nullptr;
            last_constants_ref =
                allocate(kDrawConstantUploadBytes, 256u, out,
                         NativeGpu::RingUploadOwner::DrawConstants);
            if (out == nullptr || last_constants_ref.ref == nullptr) {
                constants_valid = false;
                return false;
            }
            std::memcpy(out, &c, sizeof(c));
            std::memcpy(last_constants.data(), &c, sizeof(c));
            slot->list->setGraphicsRootDescriptor(last_constants_ref, 0);
            constants_valid = true;
            ++stats.constant_uploads;
        } else {
            ++stats.constant_reuses;
            ++stats.constant_rebuild_reuses;
            if (non_state_changed) ++stats.constant_rebuild_non_state_changed;
            if (changed_ops == 0u) ++stats.constant_rebuild_no_changed_state;
            const bool single_changed_op = changed_ops != 0u &&
                                           (changed_ops & (changed_ops - 1u)) == 0u;
            // Classify raw restoration versus distinct sources producing
            // identical bytes; neither changes the semantic key.
            if ((changed_ops & (1ull << static_cast<u32>(Op::Projection))) != 0u) {
                if (std::memcmp(&state.projection, &last_constant_projection,
                                sizeof(state.projection)) == 0) {
                    ++stats.constant_rebuild_projection_source_restored;
                } else {
                    ++stats.constant_rebuild_projection_source_distinct;
                }
            }
            if ((changed_ops & (1ull << static_cast<u32>(Op::Fog))) != 0u) {
                if (std::memcmp(&state.fog, &last_constant_fog, sizeof(state.fog)) == 0) {
                    ++stats.constant_rebuild_fog_source_restored;
                } else {
                    ++stats.constant_rebuild_fog_source_distinct;
                }
            }
            if (dirty_ops == 0u) {
                ++stats.constant_rebuild_no_state;
            } else {
                for (u32 op = 0; op < static_cast<u32>(native::Op::Count); ++op) {
                    if ((dirty_ops & (1ull << op)) != 0u) {
                        ++stats.constant_rebuild_dirty_ops[op];
                    }
                    if ((changed_ops & (1ull << op)) != 0u) {
                        ++stats.constant_rebuild_changed_ops[op];
                        if (single_changed_op && !non_state_changed) {
                            ++stats.constant_rebuild_single_changed_ops[op];
                        }
                    }
                }
            }
        }
        last_constant_projection = state.projection;
        last_constant_fog = state.fog;
        cpu_sample_compare.finish();
        cpu_constants.finish();
        return upload_lights(false);
    }

    // SIZE IS PART OF THE KEY, and leaving it out was a real rendering bug.
    //
    // The view these build carries an extent, so two draws that share a buffer
    // and an offset but differ in size need two different bindings. That used
    // to be unreachable: every geometry got a freshly created buffer, so a
    // pointer identified exactly one geometry and (buffer, offset) was
    // sufficient - by accident of the allocator, not by design.
    //
    // Once buffers are pooled, a recycled pointer is handed to a DIFFERENT
    // geometry with a different vertex or index count. The cache then matched,
    // the bind was skipped, and the GPU kept the previous geometry's extent:
    // stale descriptor, wrong range, torn geometry. For indices it is worse,
    // because drawIndexedInstanced reads through that stale view.
    //
    // Note this was invisible to draw-count comparison - the number of draws is
    // identical either way. Only the pixels differ.
    void bind_vertices(const plume::RenderBuffer* buffer, u64 offset, u32 size) {
        CpuPhaseScope cpu_phase(stats, NativeGpu::CpuPhase::BindVertexIndex);
        if (stats.cpu_profile_enabled) ++stats.cpu_bind_attempts[0];
        if (buffer == bound_vertex_buffer && offset == bound_vertex_offset &&
            size == bound_vertex_size) {
            if (stats.cpu_profile_enabled) ++stats.cpu_bind_reuses[0];
            return;
        }
        CpuPhaseScope cpu_emit(stats, NativeGpu::CpuPhase::VertexBindEmit);
        static const plume::RenderInputSlot kSlot(0, kVertexStride);
        const plume::RenderVertexBufferView view(plume::RenderBufferReference(buffer, offset), size);
        slot->list->setVertexBuffers(0, &view, 1, &kSlot);
        bound_vertex_buffer = buffer;
        bound_vertex_offset = offset;
        bound_vertex_size = size;
    }

    void bind_indices(const plume::RenderBuffer* buffer, u64 offset, u32 size) {
        CpuPhaseScope cpu_phase(stats, NativeGpu::CpuPhase::BindVertexIndex);
        if (stats.cpu_profile_enabled) ++stats.cpu_bind_attempts[1];
        if (buffer == bound_index_buffer && offset == bound_index_offset &&
            size == bound_index_size) {
            if (stats.cpu_profile_enabled) ++stats.cpu_bind_reuses[1];
            return;
        }
        CpuPhaseScope cpu_emit(stats, NativeGpu::CpuPhase::IndexBindEmit);
        const plume::RenderIndexBufferView view(plume::RenderBufferReference(buffer, offset), size,
                                                plume::RenderFormat::R32_UINT);
        slot->list->setIndexBuffer(&view);
        bound_index_buffer = buffer;
        bound_index_offset = offset;
        bound_index_size = size;
    }

    bool resident_immediate_indices(u8 primitive, u32 vertex_count, u32 index_count,
                                    plume::RenderBufferReference& reference) {
        if (!transfer_live || primitive >= immediate_index_patterns.size() || index_count == 0u) {
            return false;
        }
        ImmediateIndexPattern& pattern = immediate_index_patterns[primitive];
        const u32 bytes = index_count * sizeof(u32);
        if (pattern.buffer != nullptr && pattern.index_capacity >= index_count) {
            reference = plume::RenderBufferReference(pattern.buffer.get());
            ++stats.immediate_index_pattern_hits;
            stats.immediate_index_upload_bytes_avoided += bytes;
            stats.immediate_index_generation_bytes_avoided += bytes;
            return true;
        }

        // Grow by demand class instead of by every new count. This is the same
        // bounded retained-page principle as the upload arenas: a new maximum
        // causes O(log N), not O(N), allocations and COPY submissions.
        // The measured shipping route peaks at 200 quad and 137 strip
        // vertices. Starting at the next demand class (256) avoids allocating
        // and submitting a ladder of tiny default-heap resources during the
        // first frames. It remains a floor, not a GameCube limit: larger
        // workloads continue doubling up to the same upload/resource bound.
        u32 vertex_capacity = 256u;
        while (vertex_capacity < vertex_count && vertex_capacity <= 0x7FFFFFFFu) {
            vertex_capacity *= 2u;
        }
        if (vertex_capacity < vertex_count) vertex_capacity = vertex_count;
        const u64 capacity_indices64 = immediate_index_count(primitive, vertex_capacity);
        const u64 capacity_bytes64 = capacity_indices64 * sizeof(u32);
        if (capacity_indices64 > 0xFFFFFFFFull || capacity_bytes64 > kRingMaximumBytes) {
            return false;
        }
        const u32 capacity_indices = static_cast<u32>(capacity_indices64);
        const u32 capacity_bytes = static_cast<u32>(capacity_bytes64);
        auto grown = device->createBuffer(plume::RenderBufferDesc::IndexBuffer(
            capacity_bytes, plume::RenderHeapType::DEFAULT));
        if (grown == nullptr) {
            ++stats.immediate_index_pattern_create_failures;
            return false;
        }
        ImmediateIndexWriteContext context{primitive, vertex_capacity};
        if (!transfer.upload_buffer_generated(
                *grown, 0, capacity_bytes, render::TransferQueue::UploadOwner::ImmediateIndex,
                fill_immediate_indices, &context)) {
            ++stats.immediate_index_pattern_upload_failures;
            return false;
        }
        GEKKO_PROFILE_ONLY(
            GEKKO_PROFILE_ALLOC(grown.get(), capacity_bytes, "gpu/immediate-index-pattern");
        )
        pending_buffer_read.push_back(grown.get());
        if (pattern.buffer != nullptr) {
            slot->pattern_releases.push_back(std::move(pattern.buffer));
        }
        stats.immediate_index_pattern_bytes -= pattern.bytes;
        pattern.buffer = std::move(grown);
        pattern.index_capacity = capacity_indices;
        pattern.bytes = capacity_bytes;
        stats.immediate_index_pattern_bytes += capacity_bytes;
        ++stats.immediate_index_pattern_growths;
        reference = plume::RenderBufferReference(pattern.buffer.get());
        return true;
    }

    // The legacy widescreen policy (Renderer::apply_hud_layout) applied to an
    // immediate draw. Gates, in its order: orthographic projection; drawn into
    // the presented picture (after the frame's last camera-texture copy); not
    // sampling a camera texture (blur/glare passes carry their aspect in the
    // captured image); a 4:3 layout lays out nothing; the guest's scissor
    // covers the canvas (effect downsamples draw into a corner).
    void layout_hud(const FrameSegment& segment, u32 sequence, const native::DrawImmediate& draw,
                    const u8* vertices) {
        CpuPhaseScope cpu_layout(stats, NativeGpu::CpuPhase::ImmediateLayout);
        draw_hud = false;
        // WORLD-ANCHORED UI is already in final presentation space: the native
        // world-anchor projection put it there, through the same canonical
        // camera the world geometry uses. Applying the HUD affine on top would
        // be the second camera-coverage transform this architecture exists to
        // remove. Explicit metadata, checked before anything else - never a
        // projection-type heuristic.
        if (draw.anchor == native::DrawAnchorKind::WorldAnchored) {
            ++stats.hud_skip_world_anchored;
            return;
        }
        if (state.projection.type != 1u) return;
        if (layout_is_native(hud.layout())) return;
        const u32 last_copy = segment.last_copy_sequence();
        if (last_copy != FrameSegment::kNoCopy && sequence < last_copy) {
            ++stats.hud_skip_before_copy;
            return;
        }
        const bool textured = draw.program == native::MaterialProgram::Modulate ||
                              draw.program == native::MaterialProgram::DefaultTx2;
        if (textured && (state.bind[0].flags & native::kTextureCameraTarget) != 0u) {
            ++stats.hud_skip_effect_texture;
            return;
        }
        const native::Viewport& vp = state.viewport;
        if (!(vp.width > 0.0f) || vp.height == 0.0f) return;
        if (static_cast<float>(state.scissor.width) < kVirtualHudWidth * 0.9f ||
            static_cast<float>(state.scissor.height) < kVirtualHudHeight * 0.9f) {
            ++stats.hud_skip_small_scissor;
            return;
        }
        const u32 matrix = state.current_matrix < 128u ? state.current_matrix : 0u;
        const auto& m = state.position_matrix[matrix];
        const auto& p = state.projection.p;
        HudBounds bounds{1.0e9f, 1.0e9f, -1.0e9f, -1.0e9f};
        for (u32 i = 0; i < draw.vertex_count; ++i) {
            float position[3];
            std::memcpy(position, vertices + i * kVertexStride, sizeof(position));
            const float ex = m[0] * position[0] + m[1] * position[1] + m[2] * position[2] + m[3];
            const float ey = m[4] * position[0] + m[5] * position[1] + m[6] * position[2] + m[7];
            const float sx = vp.x + 0.5f * vp.width + (p[0] * ex + p[1]) * 0.5f * vp.width;
            const float sy = vp.y + 0.5f * vp.height - (p[2] * ey + p[3]) * 0.5f * vp.height;
            bounds.x0 = std::min(bounds.x0, sx);
            bounds.y0 = std::min(bounds.y0, sy);
            bounds.x1 = std::max(bounds.x1, sx);
            bounds.y1 = std::max(bounds.y1, sy);
        }
        const NativeHudLayout::Decision decision = hud.layout_draw(bounds, textured);
        if (!decision.laid_out) {
            ++stats.hud_off_canvas;
            return;
        }
        ++stats.hud_laid_out;
        const HudAffine& a = decision.affine;
        if (a.ax == 1.0f && a.bx == 0.0f && a.ay == 1.0f && a.by == 0.0f) return;
        ++stats.hud_remapped;
        draw_hud = true;
        draw_affine = a;
    }

    // ---- raster-border snap ---------------------------------------------
    //
    // GX takes ONE sample per pixel, at its centre, so the guest authors
    // screen-aligned 2D geometry in pixel-CENTRE coordinates: this game's
    // full-screen effect quads span [0.5, 640.5] x [0.5, 480.5]. At internal
    // scale 1 that rectangle contains every pixel centre i+0.5 for i in
    // 0..639, so the console covers the whole screen. Scaled into a native
    // target it does not: every native sub-sample whose guest centre falls
    // below 0.5 lies outside it, which is floor(S/2) of the S sub-samples in
    // guest pixel 0. Measured on the native renderer at S=3: native column 0
    // and native row 0 read exactly (0,0,0) on every scanline, holding the
    // clear, while column 1 holds normal content. The high edges lose nothing
    // because 640.5 already overhangs 640.
    //
    // This is the same conversion the legacy GX raster performs
    // (renderer.cpp, `logical_pixel_snap_`), restated for a renderer whose
    // vertices are still in model space when the CPU sees them:
    //
    //   * restricted to the raster border on purpose. A blanket snap would
    //     quantise the shimmer pass, which submits a distorted orthographic
    //     screen mesh whose interior vertices carry real sub-pixel position.
    //   * gated on scale > 1, so scale 1 - every byte-identity gate - is
    //     untouched, and on orthographic projection, so no world geometry
    //     moves. A perspective edge clipped at the frustum plane lands on
    //     window 0.0, not on the half-integer lattice, and must not be moved.
    //   * moving a vertex without moving its attributes re-maps the
    //     primitive: the same texel would land at a different screen position,
    //     shifting the sampled source window by up to half a guest texel. On
    //     the effect ladder that is a per-pass error inside a feedback loop,
    //     so it accumulates. The UVs are carried along the primitive's own
    //     screen-space affine gradients instead, which leaves the
    //     texture-to-screen mapping exactly where it was and moves only the
    //     coverage boundary - the one thing this snap is for.
    //
    // The window position is computed with exactly the algebra VSMain uses
    // (native_shaders.hlsl: eye space from the position matrix, the
    // orthographic projection branch, then the widescreen HUD remap), so a
    // snapped vertex lands where the shader will put it and not a pixel
    // elsewhere.
    struct BorderSnapMap {
        // window_x = ax . position + cx,  window_y = ay . position + cy
        float ax[3] = {0.0f, 0.0f, 0.0f};
        float ay[3] = {0.0f, 0.0f, 0.0f};
        float cx = 0.0f, cy = 0.0f;
        float low_x = 0.0f, high_x = 0.0f, low_y = 0.0f, high_y = 0.0f;
        // Minimum-norm right inverse of the 2x3 map, so a window-space delta
        // becomes the smallest model-space delta that produces it.
        float inverse[2][3]{};
        bool usable = false;
    };

    BorderSnapMap border_snap_map() const {
        BorderSnapMap map;
        if (efb.scale.factor <= 1u) return map;
        if (!logical_pixel_snap) return map;
        if (state.projection.type != 1u) return map;
        const native::Viewport& vp = state.viewport;
        if (!(vp.width > 0.0f) || !(vp.height > 0.0f)) return map;
        const u32 matrix = state.current_matrix < 128u ? state.current_matrix : 0u;
        const auto& m = state.position_matrix[matrix];
        const auto& p = state.projection.p;
        // VSMain: clip.x = p[0]*ex + p[1]; window_x = vp.x + 0.5*vp.w*(clip.x + 1)
        //         clip.y = p[2]*ey + p[3]; window_y = vp.y + 0.5*vp.h*(1 - clip.y)
        const float kx = 0.5f * vp.width * p[0];
        const float ky = -0.5f * vp.height * p[2];
        float bias_x = vp.x + 0.5f * vp.width * (p[1] + 1.0f);
        float bias_y = vp.y + 0.5f * vp.height * (1.0f - p[3]);
        float sx_scale = 1.0f, sy_scale = 1.0f;
        if (draw_hud) {
            // The HUD remap is applied in window space after the projection,
            // so the border test has to see the remapped position.
            sx_scale = draw_affine.ax;
            sy_scale = draw_affine.ay;
            bias_x = draw_affine.ax * bias_x + draw_affine.bx;
            bias_y = draw_affine.ay * bias_y + draw_affine.by;
        }
        for (u32 i = 0; i < 3u; ++i) {
            map.ax[i] = sx_scale * kx * m[i];
            map.ay[i] = sy_scale * ky * m[4u + i];
        }
        map.cx = sx_scale * kx * m[3] + bias_x;
        map.cy = sy_scale * ky * m[7] + bias_y;
        map.low_x = vp.x;
        map.high_x = vp.x + vp.width;
        map.low_y = vp.y;
        map.high_y = vp.y + vp.height;
        if (draw_hud) {
            map.low_x = apply_hud_x(draw_affine, map.low_x);
            map.high_x = apply_hud_x(draw_affine, map.high_x);
            map.low_y = apply_hud_y(draw_affine, map.low_y);
            map.high_y = apply_hud_y(draw_affine, map.high_y);
            if (map.low_x > map.high_x) std::swap(map.low_x, map.high_x);
            if (map.low_y > map.high_y) std::swap(map.low_y, map.high_y);
        }
        // Minimum-norm solution of [ax; ay] . d = (dx, dy): d = A^T (A A^T)^-1.
        const float gxx = map.ax[0] * map.ax[0] + map.ax[1] * map.ax[1] + map.ax[2] * map.ax[2];
        const float gyy = map.ay[0] * map.ay[0] + map.ay[1] * map.ay[1] + map.ay[2] * map.ay[2];
        const float gxy = map.ax[0] * map.ay[0] + map.ax[1] * map.ay[1] + map.ax[2] * map.ay[2];
        const float det = gxx * gyy - gxy * gxy;
        // Refuse rather than guess: a position matrix whose first two rows are
        // parallel or degenerate has no unique smallest displacement, and any
        // choice would move the draw somewhere the guest did not put it.
        if (!(std::fabs(det) > 1.0e-12f * std::max(1.0f, gxx * gyy)) ||
            !std::isfinite(det)) {
            return map;
        }
        const float inv_xx = gyy / det, inv_yy = gxx / det, inv_xy = -gxy / det;
        for (u32 i = 0; i < 3u; ++i) {
            map.inverse[0][i] = map.ax[i] * inv_xx + map.ay[i] * inv_xy;
            map.inverse[1][i] = map.ax[i] * inv_xy + map.ay[i] * inv_yy;
        }
        map.usable = true;
        return map;
    }

    // Returns the vertex payload to upload: `vertices` unchanged when nothing
    // was on the border, otherwise a snapped copy in immediate_scratch.
    const u8* snap_raster_border(const native::DrawImmediate& draw, const u8* vertices,
                                 u32 topology) {
        CpuPhaseScope cpu_layout(stats, NativeGpu::CpuPhase::ImmediateLayout);
        if (topology != 0u) return vertices;  // lines and points carry no area
        const u32 count = draw.vertex_count;
        if (count < 3u) return vertices;
        const BorderSnapMap map = border_snap_map();
        if (efb.scale.factor > 1u && logical_pixel_snap && state.projection.type == 1u &&
            !map.usable) {
            ++stats.border_snap_refused;
            return vertices;
        }
        if (!map.usable) return vertices;
        const auto snap_edge = [](float value, float low, float high) {
            if (value > low && value <= low + 0.5f) return low;
            if (value < high && value >= high - 0.5f) return high;
            return value;
        };
        snap_scratch.clear();
        snap_scratch.resize(static_cast<std::size_t>(count) * 4u);  // sx, sy, dx, dy
        bool moved = false;
        for (u32 i = 0; i < count; ++i) {
            float position[3];
            std::memcpy(position, vertices + std::size_t(i) * kVertexStride, sizeof(position));
            const float sx = map.ax[0] * position[0] + map.ax[1] * position[1] +
                             map.ax[2] * position[2] + map.cx;
            const float sy = map.ay[0] * position[0] + map.ay[1] * position[1] +
                             map.ay[2] * position[2] + map.cy;
            const float dx = snap_edge(sx, map.low_x, map.high_x) - sx;
            const float dy = snap_edge(sy, map.low_y, map.high_y) - sy;
            snap_scratch[i * 4u] = sx;
            snap_scratch[i * 4u + 1u] = sy;
            snap_scratch[i * 4u + 2u] = dx;
            snap_scratch[i * 4u + 3u] = dy;
            if (dx != 0.0f || dy != 0.0f) moved = true;
        }
        if (!moved) return vertices;

        // OPENED HERE, NOT AT THE TOP. The projection loop above runs on every
        // immediate draw (8.8k/s) and belongs inside gpu/draw-immediate; what
        // only a border draw pays for is the rewrite below - a minimum-norm
        // inverse per vertex plus triangle_of's linear index scan, which is
        // quadratic in the batch. Zero at internal scale 1, so headless gates
        // see none of it; that is a property of the path, not a defect.
        GEKKO_PROFILE_ZONE("gpu/border-snap");
        const std::size_t bytes = std::size_t(count) * kVertexStride;
        immediate_scratch.clear();
        immediate_scratch.resize(bytes);
        std::memcpy(immediate_scratch.data(), vertices, bytes);
        u8* out = immediate_scratch.data();

        // Per-vertex UV correction along the screen-space gradients of the
        // FIRST triangle that contains the vertex. A quad's two triangles
        // share one gradient, so this is the primitive's own affine; a mesh
        // whose triangles genuinely disagree only ever snaps on the border,
        // where one of them has to be picked and the nearest one is it.
        const bool has_uv = (draw.attributes & native::kImmediateUv0) != 0u;
        const auto triangle_of = [&](u32 vertex, u32 tri[3]) -> bool {
            switch (draw.primitive) {
                case 0:
                case 1: {  // QUADS
                    const u32 base = (vertex / 4u) * 4u;
                    if (base + 3u >= count) return false;
                    if (vertex == base + 3u) {
                        tri[0] = base; tri[1] = base + 2u; tri[2] = base + 3u;
                    } else {
                        tri[0] = base; tri[1] = base + 1u; tri[2] = base + 2u;
                    }
                    return true;
                }
                case 2: {  // TRIANGLES
                    const u32 base = (vertex / 3u) * 3u;
                    if (base + 2u >= count) return false;
                    tri[0] = base; tri[1] = base + 1u; tri[2] = base + 2u;
                    return true;
                }
                case 3: {  // TRIANGLESTRIP
                    if (count < 3u) return false;
                    const u32 end = std::max<u32>(2u, vertex);
                    if (end >= count) return false;
                    if ((end & 1u) == 0u) {
                        tri[0] = end - 2u; tri[1] = end - 1u; tri[2] = end;
                    } else {
                        tri[0] = end - 1u; tri[1] = end - 2u; tri[2] = end;
                    }
                    return true;
                }
                case 4: {  // TRIANGLEFAN
                    if (count < 3u) return false;
                    const u32 end = std::max<u32>(2u, vertex);
                    if (end >= count) return false;
                    tri[0] = 0u; tri[1] = end - 1u; tri[2] = end;
                    return true;
                }
                default: return false;
            }
        };
        for (u32 i = 0; i < count; ++i) {
            const float dx = snap_scratch[i * 4u + 2u];
            const float dy = snap_scratch[i * 4u + 3u];
            if (dx == 0.0f && dy == 0.0f) continue;
            u8* vertex_out = out + std::size_t(i) * kVertexStride;
            float position[3];
            std::memcpy(position, vertex_out, sizeof(position));
            for (u32 c = 0; c < 3u; ++c) {
                position[c] += map.inverse[0][c] * dx + map.inverse[1][c] * dy;
            }
            std::memcpy(vertex_out, position, sizeof(position));
            if (!has_uv) continue;
            u32 tri[3];
            if (!triangle_of(i, tri)) continue;
            const float x0 = snap_scratch[tri[0] * 4u], y0 = snap_scratch[tri[0] * 4u + 1u];
            const float x1 = snap_scratch[tri[1] * 4u], y1 = snap_scratch[tri[1] * 4u + 1u];
            const float x2 = snap_scratch[tri[2] * 4u], y2 = snap_scratch[tri[2] * 4u + 1u];
            const float denominator = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);
            if (!std::isfinite(denominator) || denominator == 0.0f) continue;
            float uv[2][2];
            std::memcpy(uv, vertex_out + kUvOffset, sizeof(uv));
            float uv0[2][2], uv1[2][2], uv2[2][2];
            std::memcpy(uv0, out + std::size_t(tri[0]) * kVertexStride + kUvOffset, sizeof(uv0));
            std::memcpy(uv1, out + std::size_t(tri[1]) * kVertexStride + kUvOffset, sizeof(uv1));
            std::memcpy(uv2, out + std::size_t(tri[2]) * kVertexStride + kUvOffset, sizeof(uv2));
            for (u32 map_index = 0; map_index < 2u; ++map_index) {
                for (u32 c = 0; c < 2u; ++c) {
                    const float a0 = uv0[map_index][c], a1 = uv1[map_index][c],
                                a2 = uv2[map_index][c];
                    const float da_dx =
                        ((a1 - a0) * (y2 - y0) - (a2 - a0) * (y1 - y0)) / denominator;
                    const float da_dy =
                        ((a2 - a0) * (x1 - x0) - (a1 - a0) * (x2 - x0)) / denominator;
                    uv[map_index][c] += da_dx * dx + da_dy * dy;
                }
            }
            std::memcpy(vertex_out + kUvOffset, uv, sizeof(uv));
        }
        ++stats.border_snap_draws;
        return immediate_scratch.data();
    }

    void draw_immediate(const FrameSegment& segment, u32 sequence, const native::DrawImmediate& draw) {
        CpuPhaseScope cpu_phase(stats, NativeGpu::CpuPhase::ImmediateDraw);
        // 146 per frame, ~8.8k/s. Together with gpu/draw-mesh this is the
        // dominant event source in the subsystem; if a paired build-tracy
        // measurement ever shows the profiler distorting the frame, these two
        // are the first to drop - the per-frame draw plots survive them.
        GEKKO_PROFILE_ZONE("gpu/draw-immediate");
        const u32 count = draw.vertex_count;
        if (count == 0u) return;
        const auto* vertices = segment.payload_at(draw.vertices);
        u32 topology = 0;
        switch (draw.primitive) {
            case 0:  // QUADS
            case 1:  // QUADS_2
                break;
            case 2:  // TRIANGLES: payload is already a list; draw without an index buffer.
                break;
            case 3:  // TRIANGLESTRIP
                break;
            case 4:  // TRIANGLEFAN
                break;
            case 5:  // LINES
                topology = 1;
                ++stats.line_draws;
                break;
            case 6:  // LINESTRIP
                topology = 1;
                ++stats.line_draws;
                break;
            default:  // POINTS
                topology = 2;
                ++stats.point_draws;
                break;
        }
        const u64 index_count64 = immediate_index_count(draw.primitive, count);
        const u64 vertex_bytes64 = static_cast<u64>(count) * kVertexStride;
        const u64 index_bytes64 = index_count64 * sizeof(u32);
        if (index_count64 > 0xFFFFFFFFull || vertex_bytes64 > kRingMaximumBytes ||
            index_bytes64 > kRingMaximumBytes) {
            ++stats.ring_bound_failures;
            if (vertex_bytes64 > kRingMaximumBytes) {
                ++stats.immediate_drop_vertex_ring;
                note_drop("immediate:vertex-bound", 0u, draw.primitive, count,
                          static_cast<u32>(std::min<u64>(vertex_bytes64, 0xFFFFFFFFull)));
            } else {
                ++stats.immediate_drop_index_ring;
                note_drop("immediate:index-bound", 0u, draw.primitive, count,
                          static_cast<u32>(std::min<u64>(index_bytes64, 0xFFFFFFFFull)));
            }
            return;
        }
        const u32 index_count = static_cast<u32>(index_count64);
        const bool indexed = index_count != 0u;
        const std::size_t primitive_index = std::min<std::size_t>(draw.primitive, 7u);
        ++stats.immediate_primitive_draws[primitive_index];
        stats.immediate_max_vertices[primitive_index] =
            std::max<u64>(stats.immediate_max_vertices[primitive_index], count);
        if (indexed) {
            ++stats.immediate_indexed_draws[primitive_index];
            stats.immediate_index_bytes[primitive_index] +=
                static_cast<u64>(index_count) * sizeof(u32);
        }
        const u32 list_vertices = count - (count % 3u);
        // A degenerate or non-TRIANGLES immediate batch: QUADS with fewer than
        // 4 vertices, a strip or fan with fewer than 3. The whole batch - a HUD
        // quad, a glyph run, a particle batch - vanishes here with no trace.
        if (!indexed && (draw.primitive != 2 || list_vertices < 3u)) {
            ++stats.immediate_drop_degenerate;
            note_drop("immediate:degenerate", 0u, draw.primitive, count, list_vertices);
            return;
        }
        u32 flags = 0;
        if ((draw.attributes & native::kImmediateNormal) != 0u) flags |= 1u;
        if ((draw.attributes & native::kImmediateColor0) != 0u) flags |= 2u;
        if ((draw.attributes & native::kImmediateColor1) != 0u) flags |= 4u;
        if (draw.program == native::MaterialProgram::Unsupported) ++stats.unsupported_program_draws;
        const u32 vertex_bytes = static_cast<u32>(vertex_bytes64);
        const u32 index_bytes = static_cast<u32>(index_bytes64);
        plume::RenderBufferReference resident_iref;
        const bool resident_indices =
            indexed && resident_immediate_indices(draw.primitive, count, index_count, resident_iref);
        // Includes a newly-created resident pattern in the same COPY batch as
        // any texture/geometry arrivals preceding this draw, then records the
        // exact GRAPHICS read dependency once.
        flush_resource_uploads();
        reserve(vertex_bytes + (resident_indices ? 0u : index_bytes) + 2048u);
        layout_hud(segment, sequence, draw, vertices);
        measure_texel_density(vertices, count, draw.attributes);
        // Strictly AFTER layout_hud: the HUD classifier takes its bounds from
        // the UNSNAPPED guest window positions and compares them against fixed
        // canvas thresholds, so a snap applied first would re-anchor elements.
        // It needs draw_hud/draw_affine, so it is also before they are reset.
        const u8* upload_vertices = snap_raster_border(draw, vertices, topology);
        const bool prepared = prepare_draw(topology, draw.program, flags);
        draw_hud = false;
        if (!prepared) {
            // prepare_draw has already attributed WHY (cull-all vs no
            // pipeline); this counts the loss on the immediate lane, which had
            // no total of its own at all.
            ++stats.immediate_drop_no_pipeline;
            return;
        }
        CpuPhaseScope cpu_pack(stats, NativeGpu::CpuPhase::ImmediatePackUpload);
        u8* out = nullptr;
        const auto vref =
            allocate(vertex_bytes, 16u, out, NativeGpu::RingUploadOwner::ImmediateVertex);
        if (out == nullptr) {
            ++stats.immediate_drop_vertex_ring;
            note_drop("immediate:vertex-ring", 0u, draw.primitive, count, vertex_bytes);
            return;
        }
        std::memcpy(out, upload_vertices, vertex_bytes);  // Canonical RenderVertex layout
        plume::RenderBufferReference iref = resident_iref;
        if (indexed) {
            if (!resident_indices) {
                iref = allocate(index_bytes, 16u, out,
                                NativeGpu::RingUploadOwner::ImmediateIndex);
                if (out == nullptr) {
                    ++stats.immediate_drop_index_ring;
                    note_drop("immediate:index-ring", 0u, draw.primitive, count, index_bytes);
                    return;
                }
                const u32 written =
                    write_immediate_indices(draw.primitive, count, reinterpret_cast<u32*>(out));
                if (written != index_count) {
                    ++stats.immediate_drop_index_ring;
                    note_drop("immediate:index-count", 0u, draw.primitive, index_count, written);
                    return;
                }
            }
            stats.cpu_index_staging_copy_bytes_avoided += index_bytes;
        }
        cpu_pack.finish();
        bind_vertices(vref.ref, vref.offset, vertex_bytes);
        if (indexed) {
            bind_indices(iref.ref, iref.offset, index_bytes);
            {
                CpuPhaseScope cpu_emit(stats, NativeGpu::CpuPhase::DrawEmit);
                slot->list->drawIndexedInstanced(index_count, 1, 0, 0, 0);
            }
            // topology 0 is TRIANGLE_LIST; lines and points are not triangles.
            if (topology == 0u) stats.triangles += index_count / 3u;
        } else {
            {
                CpuPhaseScope cpu_emit(stats, NativeGpu::CpuPhase::DrawEmit);
                slot->list->drawInstanced(list_vertices, 1, 0, 0);
            }
            if (topology == 0u) stats.triangles += list_vertices / 3u;
        }
        ++stats.draws;
        ++stats.immediate_draws;
    }

    void draw_mesh(const native::DrawMesh& draw) {
        CpuPhaseScope cpu_phase(stats, NativeGpu::CpuPhase::MeshDraw);
        // 145 per frame, ~8.7k/s. The other half of the draw lane: five
        // generation/index guards, prepare_draw, two binds and the draw.
        GEKKO_PROFILE_ZONE("gpu/draw-mesh");
        flush_resource_uploads();
        CpuPhaseScope cpu_lookup(stats, NativeGpu::CpuPhase::GeometryLookup);
        auto it = geometry.find(draw.geometry.address);
        if (it == geometry.end()) {
            ++stats.missing_geometry;
            ++stats.missing_geometry_unknown;
            note_drop("draw:unknown", draw.geometry.address, draw.geometry.generation, 0u,
                      draw.mesh_index);
            return;
        }
        if (it->second.generation != draw.geometry.generation) {
            ++stats.missing_geometry;
            ++stats.missing_geometry_generation;
            note_drop("draw:generation", draw.geometry.address, draw.geometry.generation,
                      it->second.generation, draw.mesh_index);
            return;
        }
        if (draw.mesh_index >= it->second.asset->meshes.size()) {
            ++stats.missing_geometry;
            ++stats.missing_geometry_mesh_index;
            note_drop("draw:mesh-index", draw.geometry.address, draw.geometry.generation,
                      it->second.generation, draw.mesh_index);
            return;
        }
        GeometryEntry& entry = it->second;
        const GeometryAsset& asset = *entry.asset;
        const ConvertedMesh& mesh = asset.meshes[draw.mesh_index];
        // A whole mesh whose display list converted to zero triangles. This
        // was the last uninstrumented exit in draw_mesh, sitting between four
        // attributed causes above it and one below - so a mesh lost here was
        // the one disappearance the drop ledger could not see.
        if (mesh.index_count == 0u) {
            ++stats.mesh_drop_zero_indices;
            note_drop("draw:zero-indices", draw.geometry.address, draw.geometry.generation,
                      entry.generation, draw.mesh_index);
            return;
        }
        cpu_lookup.finish();
        u32 flags = 0;
        if (asset.has_normals) flags |= 1u;
        if (asset.has_colors) flags |= 2u;
        if (draw.program == native::MaterialProgram::Unsupported) ++stats.unsupported_program_draws;
        reserve(2048u);
        // The default-object pass (RwGC decision table 0x80466624, reached from
        // the default and skin render callbacks) programs its one-stage
        // MODULATE through the table at 0x804669A8: b = RASC, c = TEXC. Every
        // other textured mesh pass (MatFX) uses the GXSetTevOp preset.
        const bool modulate_table = draw.program == native::MaterialProgram::Modulate &&
                                    (draw.producer == native::DrawProducer::DefaultMesh ||
                                     draw.producer == native::DrawProducer::SkinMesh);
        if (!prepare_draw(0, draw.program, flags, modulate_table)) {
            ++stats.mesh_drop_no_pipeline;
            return;
        }
        if (asset.cpu != nullptr) {
            if (!entry.dynamic_bound || entry.dynamic_frame != frame_ordinal) {
                ++stats.missing_geometry;
                ++stats.missing_geometry_dynamic_stale;
                note_drop("draw:dynamic-stale", draw.geometry.address, draw.geometry.generation,
                          entry.dynamic_frame, draw.mesh_index);
                return;
            }
            bind_vertices(entry.dynamic_buffer, entry.dynamic_offset, entry.dynamic_bytes);
        } else {
            bind_vertices(asset.vertices.get(), 0, asset.vertex_count * kVertexStride);
        }
        bind_indices(asset.indices.get(), 0, asset.index_bytes);
        {
            CpuPhaseScope cpu_emit(stats, NativeGpu::CpuPhase::DrawEmit);
            slot->list->drawIndexedInstanced(mesh.index_count, 1, mesh.index_offset, 0, 0);
        }
        // prepare_draw(0, ...) above: a converted mesh is always TRIANGLE_LIST.
        stats.triangles += mesh.index_count / 3u;
        ++stats.draws;
        ++stats.mesh_draws;
    }

    void clear(const native::Clear& c) {
        flush_resource_uploads();
        const s32 f = static_cast<s32>(efb.scale.factor);
        const s32 left = std::max<s32>(0, c.x * f);
        const s32 top = std::max<s32>(0, c.y * f);
        const s32 right = std::min<s32>(static_cast<s32>(efb_width), (c.x + c.width) * f);
        const s32 bottom = std::min<s32>(static_cast<s32>(efb_height), (c.y + c.height) * f);
        if (right <= left || bottom <= top) return;
        // After the early-out, so it matches stats.clears exactly - that
        // counter sits after it too. flush_resource_uploads above has its own
        // zone and is deliberately not included.
        GEKKO_PROFILE_ZONE("gpu/clear");
        const plume::RenderRect rect(left, top, right, bottom);
        {
            // At scale 3 a full-raster clear is 1920x1584 of fill and its cost
            // grows with the square of the scale - the exact question
            // --internal-scale raises. Nothing in here can split the list.
            GEKKO_PROFILE_GPU_ZONE(tracy_gpu,
                                   static_cast<plume::D3D12CommandList*>(slot->list.get())->d3d,
                                   "gpu/gpu-clear");
            if ((c.mode & 1u) != 0u) {
                slot->list->clearColor(0, plume::RenderColor(c.rgba[0] / 255.0f, c.rgba[1] / 255.0f, c.rgba[2] / 255.0f, 1.0f), &rect, 1);
            }
            if ((c.mode & 2u) != 0u) {
                slot->list->clearDepthStencil(true, false, kClearQuadDepth, 0, &rect, 1);
            }
        }
        ++stats.clears;
    }

    void copy(const native::CopyTexture& c) {
        CpuPhaseScope cpu_phase(stats, NativeGpu::CpuPhase::Copy);
        flush_resource_uploads();
        if (c.half_resolution != 0u) {
            // GXSetTexCopyDst's mipmap argument. The console box-filters 2x1
            // on the way out; this renderer's copy path is a 1:1 region copy
            // and cannot express that, so the copy is refused LOUDLY with the
            // guest raster it was bound for. Measured zero on Final Haunt,
            // Gun Fortress, Westopolis and Lava Shelter, so this is a latent
            // path - but a copy that silently does nothing is exactly what
            // AGENTS.md rules out, and it would look like a dead effect.
            ++stats.half_resolution_copies_unsupported;
            if (stats.last_error.empty()) {
                char message[160];
                std::snprintf(message, sizeof(message),
                              "half-resolution EFB copy into raster 0x%08X (%ux%u) refused: the "
                              "2x box GXSetTexCopyDst asks for is not implemented",
                              c.target.address, c.target_width, c.target_height);
                stats.last_error = message;
            }
            return;
        }
        // EFB copies ARE the glare ladder and the scene save: 9 per frame,
        // each four layout transitions plus a framebuffer rebind that
        // invalidates the viewport and scissor caches.
        GEKKO_PROFILE_ZONE("gpu/efb-copy");
        CameraTarget& target = camera_target(c.target.address, c.target_width, c.target_height);
        if (target.texture == nullptr) {
            // camera_target already counted the allocation failure; count the
            // copy that could not be serviced because of it separately, so a
            // report cannot show copies and targets agreeing when they do not.
            ++stats.copies_missing_target;
            return;
        }
        // Integer throughout. With two fractional factors a source rectangle
        // and its destination origin rounded independently, so a copy could
        // land one native pixel off its own extent; at an integer scale the
        // source, the destination and the target's own size are all exactly
        // `factor` times the guest numbers the guest's UVs were computed from.
        const s32 f = static_cast<s32>(efb.scale.factor);
        const s32 sx0 = c.source_x * f;
        const s32 sy0 = c.source_y * f;
        const s32 sx1 = (c.source_x + c.source_width) * f;
        const s32 sy1 = (c.source_y + c.source_height) * f;
        const s32 dx = c.destination_x * f;
        const s32 dy = c.destination_y * f;
        // destination_x/y are signed (native_frame.h). A negative origin makes
        // `native_width - dx` LARGER than the target, so the clamp below would
        // pass it and plume would receive a ~4-billion destination origin.
        // Reject it here instead, with a counter: the guest never produces one
        // in single player, and split screen must not find out the hard way.
        if (dx < 0 || dy < 0 || sx0 < 0 || sy0 < 0) {
            ++stats.copies_rejected_geometry;
            return;
        }
        s32 width = std::min(sx1 - sx0, static_cast<s32>(target.native_width) - dx);
        s32 height = std::min(sy1 - sy0, static_cast<s32>(target.native_height) - dy);
        width = std::min(width, static_cast<s32>(efb_width) - sx0);
        height = std::min(height, static_cast<s32>(efb_height) - sy0);
        if (width <= 0 || height <= 0) {
            ++stats.copies_rejected_geometry;
            return;
        }
        GEKKO_PROFILE_VALUE(width * height);
        {
            // The barrier-heaviest operation in the frame and the glare
            // ladder's device cost. ensure_texture_layout cannot split the
            // list, so the scope is safe.
            GEKKO_PROFILE_GPU_ZONE(tracy_gpu,
                                   static_cast<plume::D3D12CommandList*>(slot->list.get())->d3d,
                                   "gpu/gpu-efb-copy");
            ensure_texture_layout(efb_color.get(), plume::RenderTextureLayout::COPY_SOURCE,
                                  plume::RenderBarrierStage::COPY);
            ensure_texture_layout(target.texture.get(), plume::RenderTextureLayout::COPY_DEST,
                                  plume::RenderBarrierStage::COPY);
            const plume::RenderBox box(sx0, sy0, sx0 + width, sy0 + height);
            slot->list->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(target.texture.get()),
                                          plume::RenderTextureCopyLocation::Subresource(efb_color.get()),
                                          static_cast<u32>(dx), static_cast<u32>(dy), 0, &box);
            ensure_texture_layout(target.texture.get(), plume::RenderTextureLayout::SHADER_READ,
                                  plume::RenderBarrierStage::GRAPHICS);
            ensure_texture_layout(efb_color.get(), plume::RenderTextureLayout::COLOR_WRITE,
                                  plume::RenderBarrierStage::GRAPHICS);
        }
        // A copy re-establishes nothing on the list, but plume resets the
        // framebuffer binding when barriers change the attachment's state.
        slot->list->setFramebuffer(efb_framebuffer.get());
        viewport_valid = false;
        scissor_valid = false;
        ++stats.copies;
    }

    void present(const native::Present& p, u64 produced_ns) {
        CpuPhaseScope cpu_phase(stats, NativeGpu::CpuPhase::Present);
        // Output-slot selection (a scan under output_mutex), the resolve copy,
        // the optional screenshot readback allocation, four barriers, the
        // submit and the publication. publish_latency_ns says how long guest
        // present to publication took but never which part of this it was.
        //
        // The zone covers the WHOLE function rather than stopping after the
        // submit: region_width, region_height and the chosen slot are declared
        // here and read by the publication block below, so a scope that ended
        // at submit_list could not compile. The profiler work that used to sit
        // at the end of this function - the timestamp collect and the frame
        // plots - lives in the caller instead, so it is not charged to the
        // frame it reports on.
        GEKKO_PROFILE_ZONE("gpu/present");
        flush_resource_uploads();
        // After the flush: an asset released here has no copy still to record,
        // and its buffers die behind this frame's fence like every other release.
        trim_geometry_assets();
        const u32 region_width = efb_width;
        const u32 guest_height = p.efb_height != 0u ? p.efb_height : kGuestRasterHeight;
        const u32 region_height = std::min<u32>(efb_height, efb.scale.apply(guest_height));
        // An output slot nobody is reading: not pinned by the presenter and not
        // one of the two newest published frames (the presenter may be about to
        // acquire either: the newest, or the older of a same-interval pair).
        OutputSlot* chosen = nullptr;
        {
            std::lock_guard lock(output_mutex);
            u64 pins = 0;
            for (auto& candidate : outputs) {
                pins += candidate->pins;
                if (chosen == nullptr && candidate->pins == 0u &&
                    candidate->generation + 2u <= output_generation) {
                    chosen = candidate.get();
                }
            }
            stats.output_pins_peak = std::max(stats.output_pins_peak, pins);
            if (chosen == nullptr) {
                outputs.push_back(std::make_unique<OutputSlot>());
                chosen = outputs.back().get();
                ++stats.output_slots_created;
                chosen->semaphore = device->createCommandSemaphore();
                // The GPU-to-presenter ready signal for every frame that ever
                // lands on this slot. It had no null check, unlike every other
                // device resource in this file, so a refusal here became a
                // null handed to submit_list rather than an observable fault.
                if (chosen->semaphore == nullptr) ++stats.present_output_semaphore_failures;
            }
        }
        OutputSlot& output = *chosen;
        if (output.texture == nullptr || output.width != region_width || output.height != region_height) {
            if (output.texture != nullptr) {
                // Unpinned, but an earlier list may still be writing it.
                render::appendRetainedOwner(slot->releases, [&](Deferred& retained) noexcept {
                    retained.texture = std::move(output.texture);
                });
            }
            output.texture = device->createTexture(
                plume::RenderTextureDesc::Texture2D(region_width, region_height, 1, kColorFormat));
            // THE ENTIRE PRESENTED FRAME. create_efb and on_texture_content
            // both test usable() on what the device returns; this one did not,
            // so a refusal here lost the picture with nothing to read
            // afterwards that would say why.
            if (output.texture == nullptr) ++stats.present_output_texture_failures;
            // Announced only when the device produced one. An unusable output
            // texture is deliberately KEPT here, and that same pointer later
            // reaches complete_slot - which is why the free there tests the
            // handle rather than the wrapper.
            GEKKO_PROFILE_ONLY(
                if (output.texture != nullptr) {
                    GEKKO_PROFILE_ALLOC(output.texture.get(),
                                        static_cast<u64>(region_width) * region_height * 4u,
                                        "gpu/texture");
                }
            )
            output.width = region_width;
            output.height = region_height;
        }
        ensure_texture_layout(efb_color.get(), plume::RenderTextureLayout::COPY_SOURCE,
                              plume::RenderBarrierStage::COPY);
        ensure_texture_layout(output.texture.get(), plume::RenderTextureLayout::COPY_DEST,
                              plume::RenderBarrierStage::COPY);
        const plume::RenderBox box(0, 0, static_cast<s32>(region_width), static_cast<s32>(region_height));
        {
            // The largest single GPU transfer per frame. The scope MUST close
            // before submit_list: the zone's destructor ends its query and
            // resolves it into this list, and a list that has already been
            // closed and executed cannot take either.
            GEKKO_PROFILE_GPU_ZONE(tracy_gpu,
                                   static_cast<plume::D3D12CommandList*>(slot->list.get())->d3d,
                                   "gpu/gpu-present-resolve");
            slot->list->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(output.texture.get()),
                                          plume::RenderTextureCopyLocation::Subresource(efb_color.get()), 0, 0, 0, &box);
        }
        std::filesystem::path runtime_shot;
        {
            std::lock_guard<std::mutex> shot_lock(runtime_shot_mutex);
            if ((runtime_shot_frame != 0u && p.frame >= runtime_shot_frame) ||
                (runtime_shot_tick != 0u && p.game_ticks >= runtime_shot_tick)) {
                runtime_shot = runtime_shot_path;
                runtime_shot_frame = 0u;
                runtime_shot_tick = 0u;
            }
        }
        const bool screenshot = !runtime_shot.empty() ||
                                !settings.screenshot_path.empty() && settings.screenshot_frame != 0u &&
                                p.frame >= settings.screenshot_frame &&
                                (settings.screenshot_stride == 0u
                                     ? p.frame == settings.screenshot_frame
                                     : ((p.frame - settings.screenshot_frame) % settings.screenshot_stride) == 0u &&
                                           (p.frame - settings.screenshot_frame) / settings.screenshot_stride <
                                               settings.screenshot_count);
        if (screenshot) {
            const u32 row = region_width;
            slot->readback = device->createBuffer(
                plume::RenderBufferDesc::ReadbackBuffer(static_cast<u64>(row) * region_height * 4u));
            if (slot->readback != nullptr) {
                auto destination = plume::RenderTextureCopyLocation::PlacedFootprint(
                    slot->readback.get(), kColorFormat, region_width, region_height, 1, row);
                slot->list->copyTextureRegion(destination,
                                              plume::RenderTextureCopyLocation::Subresource(efb_color.get()), 0, 0, 0, &box);
                slot->readback_width = region_width;
                slot->readback_height = region_height;
                slot->readback_row = row;
                slot->readback_frame = p.frame;
                slot->readback_path = runtime_shot;
                slot->readback_game_ticks = p.game_ticks;
            }
        }
        ensure_texture_layout(output.texture.get(), plume::RenderTextureLayout::SHADER_READ,
                              plume::RenderBarrierStage::GRAPHICS);
        ensure_texture_layout(efb_color.get(), plume::RenderTextureLayout::COLOR_WRITE,
                              plume::RenderBarrierStage::GRAPHICS);
        Slot* const runtime_shot_slot = !runtime_shot.empty() && slot->readback != nullptr ? slot : nullptr;
        submit_list(output.semaphore.get());
        // A run-time request (the development link) is written now: the game may be paused after this frame.
        if (runtime_shot_slot != nullptr) wait_slot(*runtime_shot_slot);
        {
            std::lock_guard lock(output_mutex);
            output.generation = ++output_generation;
            output.guest_frame = p.frame;
            output.produced_ns = produced_ns;
            latest.texture = output.texture.get();
            latest.ready_semaphore = output.semaphore.get();
            latest.width = region_width;
            latest.height = region_height;
            latest.generation = output.generation;
            latest.guest_frame = p.frame;
        }
        ++stats.frames;
    }

    // Diagnostic readback of the EFB at a point inside the frame.
    void debug_dump_efb(const char* tag) {
        // Splits the list, submits, blocks on wait_slot and writes a full-EFB
        // PPM per copy. Zero unless --copy-dump names this frame. A CPU zone is
        // safe here even though submit_list and begin_list run inside it; no
        // GPU zone may ever be opened in this function, for exactly that
        // reason.
        GEKKO_PROFILE_ZONE("gpu/debug-dump-efb");
        const u32 width = efb_width, height = efb_height;
        auto readback = device->createBuffer(
            plume::RenderBufferDesc::ReadbackBuffer(static_cast<u64>(width) * height * 4u));
        if (readback == nullptr) return;
        slot->list->barriers(plume::RenderBarrierStage::COPY,
                             plume::RenderTextureBarrier(efb_color.get(), plume::RenderTextureLayout::COPY_SOURCE));
        auto destination = plume::RenderTextureCopyLocation::PlacedFootprint(readback.get(), kColorFormat,
                                                                             width, height, 1, width);
        slot->list->copyTextureRegion(destination, plume::RenderTextureCopyLocation::Subresource(efb_color.get()));
        slot->list->barriers(plume::RenderBarrierStage::GRAPHICS,
                             plume::RenderTextureBarrier(efb_color.get(), plume::RenderTextureLayout::COLOR_WRITE));
        Slot* dumped = slot;
        submit_list(nullptr);
        wait_slot(*dumped);
        auto* pixels = static_cast<const u8*>(readback->map());
        if (pixels != nullptr) {
            char name[128];
            std::snprintf(name, sizeof(name), "copy%03u%s.ppm", debug_copy_index++, tag);
            std::ofstream out(settings.copy_dump_directory / name, std::ios::binary);
            out << "P6\n" << width << " " << height << "\n255\n";
            std::vector<u8> row(static_cast<std::size_t>(width) * 3u);
            for (u32 y = 0; y < height; ++y) {
                for (u32 x = 0; x < width; ++x) {
                    row[x * 3u] = pixels[(static_cast<std::size_t>(y) * width + x) * 4u];
                    row[x * 3u + 1u] = pixels[(static_cast<std::size_t>(y) * width + x) * 4u + 1u];
                    row[x * 3u + 2u] = pixels[(static_cast<std::size_t>(y) * width + x) * 4u + 2u];
                }
                out.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(row.size()));
            }
            readback->unmap();
        }
        begin_list();
    }

    bool dumping_copies() const {
        return !settings.copy_dump_directory.empty() && settings.copy_dump_frame != 0u &&
               frame_ordinal + 1u == settings.copy_dump_frame;
    }

    void write_screenshot(Slot& s) {
        // A synchronous map, a per-row repack and an ofstream inside
        // complete_slot, ON THE RENDER THREAD: ~2.7 MB of file I/O in the
        // middle of a frame at 1280x720. Bounded by --native-screenshot-count.
        GEKKO_PROFILE_ZONE("gpu/screenshot-write");
        auto* pixels = static_cast<const u8*>(s.readback->map());
        if (pixels == nullptr) return;
        std::filesystem::path path = settings.screenshot_path;
        const bool runtime = !s.readback_path.empty();
        if (runtime) {
            path = s.readback_path;
        } else if (settings.screenshot_stride != 0u) {
            char suffix[32];
            std::snprintf(suffix, sizeof(suffix), "_%06u", s.readback_frame);
            path.replace_filename(path.stem().string() + suffix + path.extension().string());
        }
        std::ofstream out(path, std::ios::binary);
        out << "P6\n" << s.readback_width << " " << s.readback_height << "\n255\n";
        std::vector<u8> row(static_cast<std::size_t>(s.readback_width) * 3u);
        for (u32 y = 0; y < s.readback_height; ++y) {
            const u8* line = pixels + static_cast<std::size_t>(y) * s.readback_row * 4u;
            for (u32 x = 0; x < s.readback_width; ++x) {
                row[x * 3u] = line[x * 4u];
                row[x * 3u + 1u] = line[x * 4u + 1u];
                row[x * 3u + 2u] = line[x * 4u + 2u];
            }
            out.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(row.size()));
        }
        s.readback->unmap();
        ++stats.screenshots;
        if (runtime) {
            out.close();
            std::lock_guard<std::mutex> shot_lock(runtime_shot_mutex);
            ++runtime_shots_written;
            runtime_shot_last_frame = s.readback_frame;
            runtime_shot_last_game_ticks = s.readback_game_ticks;
            runtime_shot_last_path = s.readback_path;
            s.readback_path.clear();
        }
    }

    void process(const FrameSegment& segment) {
        // This is the render thread; create() and destroy() run on the caller.
        // finish() runs on the SHUTDOWN thread, so gpu/flush-uploads,
        // gpu/submit, gpu/fence-wait and gpu/slot-complete also appear on an
        // unnamed thread at exit - that is correct, not a bug.
        // The thread is NOT named here. NativeRenderer::run() creates it and
        // names it "native-render" once at entry, and it is the only caller of
        // process(). Naming it again from here would win - tracy::SetThreadName
        // keeps the last setter - and replace the owner's accurate name with a
        // second one for the same thread.
        const u64 begin = now_ns();
        std::lock_guard stats_lock(stats_mutex);
        // One segment is one guest frame, structurally: rw_capture has a single
        // close_segment() call site and it always follows the single
        // emit(Op::Present). record_ns_max says a segment took 7.84 ms; this
        // says WHICH one, and how much work was in it.
        GEKKO_PROFILE_ZONE("gpu/segment");
        GEKKO_PROFILE_VALUE(segment.command_count());
        GEKKO_PROFILE_PLOT("gpu/commands", static_cast<double>(segment.command_count()));
        if (!recording) begin_list();
        segment.for_each([&](const native::CommandHeader& header, const u8* record) {
            switch (header.op) {
                case Op::TextureContent:
                    on_texture_content(segment, FrameSegment::read<native::TextureContent>(record));
                    break;
                case Op::ReleaseTexture:
                    on_texture_release(FrameSegment::read<native::ResourceKey>(record));
                    break;
                case Op::GeometryContent:
                    on_geometry_content(segment, FrameSegment::read<native::GeometryContent>(record));
                    break;
                case Op::GeometryDynamic:
                    on_geometry_dynamic(segment, FrameSegment::read<native::GeometryDynamic>(record));
                    break;
                case Op::ReleaseGeometry:
                    on_geometry_release(FrameSegment::read<native::ResourceKey>(record));
                    break;
                case Op::DrawImmediate:
                    draw_immediate(segment, header.sequence, FrameSegment::read<native::DrawImmediate>(record));
                    break;
                case Op::HudGroup: {
                    const auto mark = FrameSegment::read<native::HudGroup>(record);
                    hud.on_group(static_cast<HudGroupKind>(mark.kind), mark.group_id, mark.instance, mark.canvas_x);
                    ++stats.hud_groups;
                    break;
                }
                case Op::HudScene: {
                    const auto mark = FrameSegment::read<native::HudScene>(record);
                    hud.on_scene(mark.node, mark.cast);
                    ++stats.hud_scenes;
                    break;
                }
                case Op::GlareWindow: {
                    // The bloom pass boundary. Nothing renders differently yet:
                    // this records the guest's own gate and its live parameter
                    // block so the new bloom system can be built against
                    // measured values rather than against the constants the
                    // specification derived from the executable.
                    const auto window = FrameSegment::read<native::GlareWindow>(record);
                    const bool changed =
                        !glare_seen ||
                        std::memcmp(&window.enable, &glare.enable,
                                    sizeof(native::GlareWindow) -
                                        offsetof(native::GlareWindow, enable)) != 0;
                    if (changed) ++stats.glare_parameter_changes;
                    glare = window;
                    glare_seen = true;
                    ++stats.glare_windows;
                    if (window.gate != 0u) ++stats.glare_windows_gated_on;
                    stats.glare_parameters = window;
                    break;
                }
                case Op::DrawMesh:
                    draw_mesh(FrameSegment::read<native::DrawMesh>(record));
                    break;
                case Op::Clear:
                    clear(FrameSegment::read<native::Clear>(record));
                    break;
                case Op::CopyTexture:
                    copy(FrameSegment::read<native::CopyTexture>(record));
                    if (dumping_copies()) debug_dump_efb("");
                    break;
                case Op::Present:
                    if (dumping_copies()) debug_dump_efb("-xfb");
                    present(FrameSegment::read<native::Present>(record), segment.close_ns());
                    // PROFILER WORK, DELIBERATELY OUTSIDE gpu/present. The
                    // collect drains the device's timestamp ring and the plots
                    // read the D3D12MA budget and the device layer's counters;
                    // none of that may be charged to the frame it reports on.
                    // Collect() dereferences the context unconditionally, so
                    // the null guard is required.
                    if (tracy_gpu != nullptr) GEKKO_PROFILE_GPU_COLLECT(tracy_gpu);
                    publish_frame_plots();
                    GEKKO_PROFILE_FRAME("gpu/frame");
                    if (segment.close_ns() != 0u) {
                        const u64 latency = now_ns() - segment.close_ns();
                        GEKKO_PROFILE_PLOT("gpu/publish-latency-ms",
                                           static_cast<double>(latency) / 1.0e6);
                        stats.publish_latency_ns += latency;
                        stats.publish_latency_ns_max = std::max(stats.publish_latency_ns_max, latency);
                        ++stats.publish_latency_samples;
                        ++stats.publish_latency_histogram[std::min<u64>(latency / 500000u, 63u)];
                    }
                    hud.end_frame();
                    apply_output_resize();
                    ++frame_ordinal;
                    begin_list();
                    break;
                case Op::ResetState:
                    // Savestate load: GX state restarts (the capture replays the
                    // saved values next) and every texture/geometry entry belongs
                    // to the discarded timeline; each re-uploads at its next use.
                    state = GxState{};
                    constants_valid = false;
                    lights_valid = false;
                    texture_resolution_changed();
                    for (auto& [address, entry] : textures) {
                        if (entry.texture != nullptr) {
                            release_texture_entry(entry);
                            --stats.textures_live;
                        }
                    }
                    textures.clear();
                    // The guest's instance-cache entries go with the timeline;
                    // the assets do not belong to any timeline - they are
                    // content, proven equal byte for byte when rebound.
                    for (auto& [address, entry] : geometry) {
                        unbind_geometry(entry);
                        --stats.geometry_live;
                    }
                    geometry.clear();
                    // `targets` is the THIRD guest-address-keyed resource map
                    // and it was not cleared here, so every camera render
                    // target survived a savestate load that discarded the
                    // timeline they belonged to. A later raster at a recycled
                    // guest address then found a resident target holding the
                    // PREVIOUS timeline's pixels, and the VRAM was never
                    // reclaimed. Released through the same deferred path as
                    // everything else so the GPU is not reading them.
                    for (auto& [address, target] : targets) {
                        if (target.texture == nullptr) continue;
                        render::appendRetainedOwner(slot->releases, [&](Deferred& retained) noexcept {
                            retained.texture = std::move(target.texture);
                            retained.bindless = target.bindless;
                        });
                        --stats.camera_targets;
                    }
                    targets.clear();
                    ++stats.state_resets;
                    // A savestate load discards every texture, geometry and
                    // camera target at once; the frames after it are a
                    // different resource population from the frames before.
                    GEKKO_PROFILE_MESSAGE_LITERAL("gpu/state reset: every resource discarded");
                    break;
                case Op::CameraBegin:
                case Op::CameraEnd:
                    break;
                default:
                    apply_state(header, record);
                    break;
            }
        });
        const u64 ns = now_ns() - begin;
        GEKKO_PROFILE_PLOT("gpu/record-ms", static_cast<double>(ns) / 1.0e6);
        stats.record_ns += ns;
        stats.record_ns_max = std::max(stats.record_ns_max, ns);
    }

    void finish() {
        std::lock_guard stats_lock(stats_mutex);
        // An aborted command list must never be submitted after a failed wait.
        // Retry only retirement of submitted work at explicit shutdown, keeping
        // all still-owned resources intact until that retirement succeeds.
        if (!retirement_failed) flush_resource_uploads();
        if (recording && !retirement_failed) {
            // Close the open list so its resource work completes; nothing presents.
            submit_list(nullptr);
        }
        for (Slot& s : slots) wait_slot(s);
    }

    // ---- per-frame plots -------------------------------------------------------
    //
    // EVERYTHING THE RENDERER ALREADY COUNTS, REPUBLISHED ONCE PER PRESENT AS A
    // QUANTITY OVER TIME. Two kinds of number live here and neither can be a
    // zone. The work quantities are far too hot - constant uploads alone are
    // 12k/s and state ops ~64k/s - and the refusal counters are the ones that
    // matter most, yet a single exit total can only ever pose them as an
    // arithmetic puzzle. Step-drawn, so a refusal line steps AT the frame it
    // happened in and can be read against the zones on the same timeline: that
    // is how the Gun Fortress case - 810 of 810 dropped draws from one poisoned
    // address - would have been found in one look instead of by arithmetic on
    // two unrelated totals.
    //
    // NOTHING IN HERE RUNS UNLESS A PROFILER IS ATTACHED. The D3D12MA budget
    // query and GpuRuntime::stats() - which takes the device layer's mutex and
    // returns a struct holding a std::string - both cost something, and neither
    // may be charged to the shipped binary or to an unattached Tracy build.
    //
    // Called from the Op::Present arm with stats_mutex held, and it takes
    // output_mutex for the output-slot gauges. That is the order present()
    // already establishes (stats_mutex, then output_mutex); nothing anywhere
    // takes the two the other way round.
    void publish_frame_plots() {
        if (!GEKKO_PROFILE_CONNECTED()) return;
        if (!plots_configured) {
            plots_configured = true;
            configure_plots();
        }
        // The first present after an attach records the baseline and publishes
        // no deltas. Without it the whole session's accumulated counts would be
        // published as one frame's work, which is not a spike anyone can act on.
        const bool publish = plot_baseline;
        plot_baseline = true;
        const auto delta = [publish](const char* name, u64 now, u64& previous) {
            if (publish) GEKKO_PROFILE_PLOT(name, static_cast<double>(now - previous));
            previous = now;
        };

        // ---- work done in this frame (deltas of cumulative counters) ----
        delta("gpu/draws", stats.draws, plot_draws);
        delta("gpu/immediate-draws", stats.immediate_draws, plot_immediate_draws);
        delta("gpu/mesh-draws", stats.mesh_draws, plot_mesh_draws);
        delta("gpu/triangles", stats.triangles, plot_triangles);
        delta("gpu/clears", stats.clears, plot_clears);
        delta("gpu/efb-copies", stats.copies, plot_copies);
        // The renderer's own barrier counts. stats() later REPLACES them with
        // the device's on the grounds that they are the same events seen twice,
        // so these deltas are the right numbers to publish here.
        delta("gpu/barriers", stats.barriers, plot_barriers);
        delta("gpu/barrier-requests", stats.barrier_requests, plot_barriers_elided);
        delta("gpu/constant-uploads", stats.constant_uploads, plot_constant_uploads);
        delta("gpu/constant-reuses", stats.constant_reuses, plot_constant_reuses);
        delta("gpu/dynamic-updates", stats.dynamic_updates, plot_dynamic_updates);
        delta("gpu/footprint-draws", stats.footprint_draws, plot_footprint_draws);
        delta("gpu/border-snap-draws", stats.border_snap_draws, plot_border_snap_draws);
        // CREATED, not just live. The churn defect is precisely the gap between
        // the two - 6,765 created against 46 live - and a live count alone is
        // flat and says nothing.
        delta("gpu/textures-created", stats.textures_created, plot_textures_created);
        delta("gpu/geometry-created", stats.geometry_created, plot_geometry_created);
        delta("gpu/upload-bytes", stats.upload_bytes, plot_upload_bytes);

        // ---- pool occupancy ----
        GEKKO_PROFILE_PLOT("gpu/textures-live", static_cast<double>(stats.textures_live));
        GEKKO_PROFILE_PLOT("gpu/texture-bytes", static_cast<double>(stats.texture_bytes));
        GEKKO_PROFILE_PLOT("gpu/geometry-live", static_cast<double>(stats.geometry_live));
        GEKKO_PROFILE_PLOT("gpu/camera-targets", static_cast<double>(stats.camera_targets));
        GEKKO_PROFILE_PLOT("gpu/bindless-high-water", static_cast<double>(stats.bindless_high_water));
        GEKKO_PROFILE_PLOT("gpu/bindless-free", static_cast<double>(free_bindless.size()));
        // The approach to kBindlessCapacity, which has no counter of any kind:
        // past it every texture and camera target is pinned to the magenta
        // sentinel. Pair with the gpu/bindless exhausted message.
        GEKKO_PROFILE_PLOT("gpu/bindless-next", static_cast<double>(bindless_next));
        GEKKO_PROFILE_PLOT("gpu/pipelines", static_cast<double>(pipelines.size()));
        GEKKO_PROFILE_PLOT("gpu/pso-misses", static_cast<double>(stats.pso_misses));
        // The release backlog waiting on this slot's fence.
        GEKKO_PROFILE_PLOT("gpu/deferred-pending",
                           static_cast<double>(slot != nullptr ? slot->releases.size() : 0u));
        // The glare ladder's resident VRAM, which the single gpu/texture pool
        // folds in with everything else. At most five entries.
        u64 target_bytes = 0;
        for (const auto& entry : targets) {
            if (entry.second.texture == nullptr) continue;
            target_bytes += static_cast<u64>(entry.second.native_width) *
                            entry.second.native_height * 4u;
        }
        GEKKO_PROFILE_PLOT("gpu/camera-target-bytes", static_cast<double>(target_bytes));

        // THE OUTPUT SLOTS CAN ONLY GROW. present() pushes a new one whenever
        // no candidate is both unpinned and older than generation-2, and
        // nothing ever removes one; each is a full output texture (3.7 MiB at
        // 1280x720). No counter, report line or other plot would show it.
        {
            std::lock_guard lock(output_mutex);
            u64 pinned = 0;
            for (const auto& candidate : outputs) {
                if (candidate->pins != 0u) ++pinned;
            }
            GEKKO_PROFILE_PLOT("gpu/output-slots", static_cast<double>(outputs.size()));
            GEKKO_PROFILE_PLOT("gpu/output-slots-pinned", static_cast<double>(pinned));
        }

        // VRAM AGAINST BUDGET OVER TIME DOES NOT EXIST ANYWHERE TODAY:
        // update_budget() runs only from stats(), which is reached once per
        // session, after the renderer has stopped. This is the number that
        // decides whether internal scale 3 and above is affordable.
        update_budget();
        GEKKO_PROFILE_PLOT("gpu/vram-used", static_cast<double>(stats.vram_usage_bytes));
        GEKKO_PROFILE_PLOT("gpu/vram-budget", static_cast<double>(stats.vram_budget_bytes));

        // ---- every refusal and drop, cumulative and step-drawn ----
        GEKKO_PROFILE_PLOT("gpu/ring-bound-failures", static_cast<double>(stats.ring_bound_failures));
        GEKKO_PROFILE_PLOT("gpu/ring-fail-allocate-oversize", static_cast<double>(stats.ring_fail_allocate_oversize));
        GEKKO_PROFILE_PLOT("gpu/ring-fail-reserve-oversize", static_cast<double>(stats.ring_fail_reserve_oversize));
        GEKKO_PROFILE_PLOT("gpu/ring-flushes", static_cast<double>(stats.ring_flushes));
        GEKKO_PROFILE_PLOT("gpu/missing-geometry-unknown", static_cast<double>(stats.missing_geometry_unknown));
        GEKKO_PROFILE_PLOT("gpu/missing-geometry-generation", static_cast<double>(stats.missing_geometry_generation));
        GEKKO_PROFILE_PLOT("gpu/missing-geometry-mesh-index", static_cast<double>(stats.missing_geometry_mesh_index));
        GEKKO_PROFILE_PLOT("gpu/missing-geometry-dynamic-stale", static_cast<double>(stats.missing_geometry_dynamic_stale));
        GEKKO_PROFILE_PLOT("gpu/immediate-drop-degenerate", static_cast<double>(stats.immediate_drop_degenerate));
        GEKKO_PROFILE_PLOT("gpu/immediate-drop-no-pipeline", static_cast<double>(stats.immediate_drop_no_pipeline));
        GEKKO_PROFILE_PLOT("gpu/immediate-drop-vertex-ring", static_cast<double>(stats.immediate_drop_vertex_ring));
        GEKKO_PROFILE_PLOT("gpu/immediate-drop-index-ring", static_cast<double>(stats.immediate_drop_index_ring));
        GEKKO_PROFILE_PLOT("gpu/mesh-drop-zero-indices", static_cast<double>(stats.mesh_drop_zero_indices));
        GEKKO_PROFILE_PLOT("gpu/mesh-drop-no-pipeline", static_cast<double>(stats.mesh_drop_no_pipeline));
        GEKKO_PROFILE_PLOT("gpu/dynamic-drop-unknown", static_cast<double>(stats.dynamic_drop_unknown));
        GEKKO_PROFILE_PLOT("gpu/dynamic-drop-no-cpu", static_cast<double>(stats.dynamic_drop_no_cpu));
        GEKKO_PROFILE_PLOT("gpu/dynamic-drop-expand", static_cast<double>(stats.dynamic_drop_expand));
        GEKKO_PROFILE_PLOT("gpu/dynamic-drop-staging", static_cast<double>(stats.dynamic_drop_staging));
        GEKKO_PROFILE_PLOT("gpu/geometry-drop-buffer-create", static_cast<double>(stats.geometry_drop_buffer_create));
        GEKKO_PROFILE_PLOT("gpu/geometry-drop-vertex-staging", static_cast<double>(stats.geometry_drop_vertex_staging));
        GEKKO_PROFILE_PLOT("gpu/geometry-drop-index-staging", static_cast<double>(stats.geometry_drop_index_staging));
        GEKKO_PROFILE_PLOT("gpu/geometry-convert-failures", static_cast<double>(stats.geometry_convert_failures));
        GEKKO_PROFILE_PLOT("gpu/geometry-drops-seen", static_cast<double>(stats.geometry_drops_seen));
        GEKKO_PROFILE_PLOT("gpu/geometry-drop-objects-overflow", static_cast<double>(stats.geometry_drop_objects_overflow));
        GEKKO_PROFILE_PLOT("gpu/texture-decode-failures", static_cast<double>(stats.texture_decode_failures));
        GEKKO_PROFILE_PLOT("gpu/texture-pinned-missing-new", static_cast<double>(stats.texture_pinned_missing_new));
        GEKKO_PROFILE_PLOT("gpu/texture-pinned-missing-reuse", static_cast<double>(stats.texture_pinned_missing_reuse));
        GEKKO_PROFILE_PLOT("gpu/missing-textures", static_cast<double>(stats.missing_textures));
        GEKKO_PROFILE_PLOT("gpu/copies-rejected-geometry", static_cast<double>(stats.copies_rejected_geometry));
        GEKKO_PROFILE_PLOT("gpu/copies-missing-target", static_cast<double>(stats.copies_missing_target));
        GEKKO_PROFILE_PLOT("gpu/half-resolution-copies-unsupported", static_cast<double>(stats.half_resolution_copies_unsupported));
        GEKKO_PROFILE_PLOT("gpu/scissor-offsets-unsupported", static_cast<double>(stats.scissor_offsets_unsupported));
        GEKKO_PROFILE_PLOT("gpu/state-ops-unconsumed", static_cast<double>(stats.state_ops_unconsumed));
        GEKKO_PROFILE_PLOT("gpu/target-allocation-failures", static_cast<double>(stats.target_allocation_failures));
        GEKKO_PROFILE_PLOT("gpu/pipeline-create-failures", static_cast<double>(stats.pipeline_create_failures));
        GEKKO_PROFILE_PLOT("gpu/present-output-texture-failures", static_cast<double>(stats.present_output_texture_failures));
        GEKKO_PROFILE_PLOT("gpu/present-output-semaphore-failures", static_cast<double>(stats.present_output_semaphore_failures));
        GEKKO_PROFILE_PLOT("gpu/efb-resize-refused", static_cast<double>(stats.efb_resize_refused));
        GEKKO_PROFILE_PLOT("gpu/fence-wait-failures", static_cast<double>(stats.fence_wait_failures));
        GEKKO_PROFILE_PLOT("gpu/border-snap-refused", static_cast<double>(stats.border_snap_refused));
        GEKKO_PROFILE_PLOT("gpu/rejections-posted", static_cast<double>(stats.rejections_posted));
        GEKKO_PROFILE_PLOT("gpu/rejections-geometry", static_cast<double>(stats.rejections_geometry));
        GEKKO_PROFILE_PLOT("gpu/rejections-texture", static_cast<double>(stats.rejections_texture));
        GEKKO_PROFILE_PLOT("gpu/rejections-undeliverable", static_cast<double>(stats.rejections_undeliverable));
        // 2D elements that were genuinely dropped - 5.2 per frame on the
        // measured route - and the three classifier skips beside them. A
        // dropped HUD element is a refusal like any other.
        GEKKO_PROFILE_PLOT("gpu/hud-off-canvas", static_cast<double>(stats.hud_off_canvas));
        GEKKO_PROFILE_PLOT("gpu/hud-skip-before-copy", static_cast<double>(stats.hud_skip_before_copy));
        GEKKO_PROFILE_PLOT("gpu/hud-skip-effect-texture", static_cast<double>(stats.hud_skip_effect_texture));
        GEKKO_PROFILE_PLOT("gpu/hud-skip-small-scissor", static_cast<double>(stats.hud_skip_small_scissor));

        // ---- the transfer queue ----
        if (transfer_live) {
            const render::TransferQueue::Stats t = transfer.stats();
            GEKKO_PROFILE_PLOT("gpu/copy-queue-submits", static_cast<double>(t.submits));
            GEKKO_PROFILE_PLOT("gpu/copy-queue-bytes", static_cast<double>(t.bytes));
            GEKKO_PROFILE_PLOT("gpu/copy-queue-batch-waits", static_cast<double>(t.batch_waits));
            GEKKO_PROFILE_PLOT("gpu/copy-queue-refused", static_cast<double>(t.refused_oversize +
                                                                            t.refused_arguments));
        }

        // stats.last_error is the only channel in this subsystem that carries
        // WHY something failed, and it is read exactly once, at exit, by which
        // time the frame it belonged to is unrecoverable. Once per session, on
        // the timeline at the frame it happened.
        if (!last_error_reported && !stats.last_error.empty()) {
            last_error_reported = true;
            GEKKO_PROFILE_MESSAGE(stats.last_error.data(), stats.last_error.size());
        }
    }

    // How each plot is DRAWN. Without this Tracy interpolates every staircase
    // and renders byte counts as raw numbers, which is exactly the misreading
    // the step and memory choices were made to avoid. One-time, inside the
    // connected guard, so it configures on first attach.
    void configure_plots() {
        // Counts and refusals: staircases, because that is what a counter does.
        GEKKO_PROFILE_PLOT_CONFIG("gpu/commands", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/draws", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/immediate-draws", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/mesh-draws", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/triangles", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/clears", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/efb-copies", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/barriers", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/barriers-elided", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/constant-uploads", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/constant-reuses", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/dynamic-updates", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/footprint-draws", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/border-snap-draws", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/textures-created", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/geometry-created", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/textures-live", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/geometry-live", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/camera-targets", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/bindless-high-water", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/bindless-free", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/bindless-next", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/pipelines", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/pso-misses", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/deferred-pending", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/output-slots", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/output-slots-pinned", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/ring-bound-failures", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/ring-fail-allocate-oversize", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/ring-fail-reserve-oversize", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/ring-flushes", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/missing-geometry-unknown", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/missing-geometry-generation", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/missing-geometry-no-vertices", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/missing-geometry-mesh-index", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/missing-geometry-dynamic-stale", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/immediate-drop-degenerate", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/immediate-drop-no-pipeline", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/immediate-drop-vertex-ring", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/immediate-drop-index-ring", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/mesh-drop-zero-indices", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/mesh-drop-no-pipeline", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/dynamic-drop-unknown", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/dynamic-drop-no-cpu", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/dynamic-drop-expand", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/dynamic-drop-staging", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/geometry-drop-buffer-create", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/geometry-drop-vertex-staging", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/geometry-drop-index-staging", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/geometry-convert-failures", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/geometry-drops-seen", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/geometry-drop-objects-overflow", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/texture-decode-failures", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/texture-pinned-missing-new", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/texture-pinned-missing-reuse", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/missing-textures", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/copies-rejected-geometry", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/copies-missing-target", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/half-resolution-copies-unsupported", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/scissor-offsets-unsupported", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/state-ops-unconsumed", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/target-allocation-failures", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/pipeline-create-failures", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/present-output-texture-failures", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/present-output-semaphore-failures", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/efb-resize-refused", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/fence-wait-failures", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/border-snap-refused", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/rejections-posted", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/rejections-geometry", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/rejections-texture", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/rejections-undeliverable", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/hud-off-canvas", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/hud-skip-before-copy", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/hud-skip-effect-texture", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/hud-skip-small-scissor", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/device-upload-failures", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/device-upload-stalls", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/device-upload-allocs", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/device-texture-refused", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/device-buffer-refused", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/device-copy-open-failures", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/device-copy-close-failures", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/device-barriers-unknown", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/device-resources-tracked", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/device-fence-cpu-waits", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/device-fence-cpu-wait-ns", GEKKO_PLOT_NUMBER, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/copy-queue-submits", GEKKO_PLOT_NUMBER, true, false, 0);
        // Bytes, so Tracy shows KiB/MiB rather than a raw integer.
        GEKKO_PROFILE_PLOT_CONFIG("gpu/texture-bytes", GEKKO_PLOT_MEMORY, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/camera-target-bytes", GEKKO_PLOT_MEMORY, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/ring-used", GEKKO_PLOT_MEMORY, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/upload-bytes", GEKKO_PLOT_MEMORY, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/vram-used", GEKKO_PLOT_MEMORY, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/vram-budget", GEKKO_PLOT_MEMORY, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/device-upload-bytes", GEKKO_PLOT_MEMORY, true, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/copy-queue-bytes", GEKKO_PLOT_MEMORY, true, false, 0);
        // Durations are samples, not staircases: interpolating them is right.
        GEKKO_PROFILE_PLOT_CONFIG("gpu/gpu-frame-ms", GEKKO_PLOT_NUMBER, false, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/record-ms", GEKKO_PLOT_NUMBER, false, false, 0);
        GEKKO_PROFILE_PLOT_CONFIG("gpu/publish-latency-ms", GEKKO_PLOT_NUMBER, false, false, 0);
    }

    void update_budget() {
        plume::RenderMemoryBudget budget;
        if (device == nullptr || !device->getMemoryBudget(budget)) return;
        stats.vram_usage_bytes = budget.usageBytes;
        stats.vram_budget_bytes = budget.budgetBytes;
    }
};

NativeGpu::NativeGpu() : impl_(std::make_unique<Impl>()) {}
NativeGpu::~NativeGpu() { destroy(); }
bool NativeGpu::available() noexcept { return true; }

bool NativeGpu::create(const NativeGpuSettings& settings, std::string& error) {
    impl_->settings = settings;
    impl_->hud.set_layout(settings.hud_layout);
    impl_->resize_width = settings.output_width;
    impl_->resize_height = settings.output_height;
    impl_->resize_internal_scale = settings.internal_scale;
    impl_->resize_layout = settings.hud_layout;
    try { return impl_->create(error); }
    catch (const plume::FenceRetirementError& failure) {
        native::publishFailureBeforeDiagnostics([&] { impl_->retirement_failed = true; }, [&] {
            error = failure.what();
        });
        return false;
    }
}

void NativeGpu::destroy() {
    if (!impl_ || impl_->device == nullptr) return;
    try {
        impl_->finish();
    } catch (const plume::FenceRetirementError& error) {
        if (error.outcome() != plume::RenderFenceWait::DEVICE_LOST) {
            // Releasing members below would free GPU-owned upload/resource
            // bytes. Process teardown is the only safe bounded failure policy
            // when the device is healthy/unknown and its fence did not retire.
            std::fprintf(stderr, "[native-gpu] fatal teardown before resource release: %s\n", error.what());
            std::fflush(stderr);
            std::terminate();
        }
        std::fprintf(stderr, "[native-gpu] removed device: discard resources without retirement\n");
    }
    // AFTER finish() and BEFORE the queue and device are released.
    // ~D3D12QueueCtx drains the queries still outstanding and complains if the
    // client is still pushing any, so every list must have retired first; it
    // also holds a reference to the queue it was created from. Null-guarded
    // because the destroy macro dereferences the context unconditionally.
    if (impl_->tracy_gpu != nullptr) {
        GEKKO_PROFILE_GPU_DESTROY(impl_->tracy_gpu);
        impl_->tracy_gpu = nullptr;
    }
    impl_->transfer.destroy();
    impl_->transfer_live = false;
    impl_->pipelines.clear();
    impl_->textures.clear();
    impl_->targets.clear();
    impl_->geometry.clear();
    impl_->geometry_assets.clear();
    for (auto& s : impl_->slots) {
        s.ring.destroy();
        s = Slot{};
    }
    impl_->outputs.clear();
    impl_->present_queue.reset();
    impl_->efb_framebuffer.reset();
    GEKKO_PROFILE_ONLY(
        if (d3d_texture(impl_->efb_color.get()) != nullptr) GEKKO_PROFILE_FREE(impl_->efb_color.get(), "gpu/efb");
        if (d3d_texture(impl_->efb_depth.get()) != nullptr) GEKKO_PROFILE_FREE(impl_->efb_depth.get(), "gpu/efb");
    )
    impl_->efb_color.reset();
    impl_->efb_depth.reset();
    impl_->white_texture.reset();
    impl_->magenta_texture.reset();
    impl_->texture_set.reset();
    impl_->sampler_set.reset();
    impl_->layout.reset();
    impl_->samplers.clear();
    impl_->vertex_shader.reset();
    impl_->pixel_shader.reset();
    impl_->pixel_shader_alpha.reset();
    impl_->pixel_shader_footprint.reset();
    impl_->pixel_shader_alpha_footprint.reset();
    impl_->queue.reset();
    impl_->device.reset();
    impl_->render_interface.reset();
}

void NativeGpu::process(const native::FrameSegment& segment) {
    try { impl_->process(segment); }
    catch (const plume::FenceRetirementError& failure) {
        std::lock_guard lock(impl_->stats_mutex);
        native::publishFailureBeforeDiagnostics([&] { impl_->retirement_failed = true; }, [&] {
            impl_->stats.last_error = failure.what();
        });
        throw;
    }
}
void NativeGpu::finish() { impl_->finish(); }

void NativeGpu::request_screenshot(const std::filesystem::path& path, u32 frame) {
    std::lock_guard<std::mutex> lock(impl_->runtime_shot_mutex);
    impl_->runtime_shot_path = path;
    impl_->runtime_shot_frame = frame != 0u ? frame : 1u;
    impl_->runtime_shot_tick = 0u;
}

void NativeGpu::request_screenshot_at_tick(const std::filesystem::path& path, u64 game_tick) {
    std::lock_guard<std::mutex> lock(impl_->runtime_shot_mutex);
    impl_->runtime_shot_path = path;
    impl_->runtime_shot_frame = 0u;
    impl_->runtime_shot_tick = game_tick != 0u ? game_tick : 1u;
}

NativeGpu::ScreenshotResult NativeGpu::screenshot_result() const {
    std::lock_guard<std::mutex> lock(impl_->runtime_shot_mutex);
    ScreenshotResult result;
    result.written = impl_->runtime_shots_written;
    result.frame = impl_->runtime_shot_last_frame;
    result.game_ticks = impl_->runtime_shot_last_game_ticks;
    result.path = impl_->runtime_shot_last_path;
    result.pending = impl_->runtime_shot_frame != 0u || impl_->runtime_shot_tick != 0u;
    return result;
}

void NativeGpu::request_output_size(u32 width, u32 height, const DisplayLayout& layout,
                                    u32 internal_scale) {
    if (width == 0u || height == 0u) return;
    std::lock_guard lock(impl_->resize_mutex);
    const bool same_size = width == impl_->resize_width && height == impl_->resize_height &&
                           internal_scale == impl_->resize_internal_scale;
    const DisplayLayout& last = impl_->resize_layout;
    if (same_size && last.aspect == layout.aspect && last.hud_scale == layout.hud_scale &&
        last.hud_inset == layout.hud_inset && last.output_width == layout.output_width &&
        last.output_height == layout.output_height) {
        return;
    }
    impl_->resize_width = width;
    impl_->resize_height = height;
    impl_->resize_internal_scale = internal_scale;
    impl_->resize_layout = layout;
    impl_->resize_pending = true;
}
void* NativeGpu::device() const noexcept { return impl_->device.get(); }

bool NativeGpu::latest_output(OutputFrame& frame) const {
    std::lock_guard lock(impl_->output_mutex);
    if (impl_->latest.texture == nullptr) return false;
    frame = impl_->latest;
    return true;
}

void* NativeGpu::present_queue() const noexcept { return impl_->present_queue.get(); }

u64 NativeGpu::output_generation() const {
    std::lock_guard lock(impl_->output_mutex);
    return impl_->output_generation;
}

bool NativeGpu::acquire_output(OutputFrame& frame) {
    std::lock_guard lock(impl_->output_mutex);
    for (u32 index = 0; index < impl_->outputs.size(); ++index) {
        OutputSlot& slot = *impl_->outputs[index];
        if (slot.generation != 0u && slot.generation == impl_->output_generation) {
            ++slot.pins;
            frame = impl_->latest;
            frame.slot = index;
            return true;
        }
    }
    return false;
}

bool NativeGpu::acquire_output(u64 generation, OutputFrame& frame) {
    std::lock_guard lock(impl_->output_mutex);
    if (generation == 0u) return false;
    for (u32 index = 0; index < impl_->outputs.size(); ++index) {
        OutputSlot& slot = *impl_->outputs[index];
        if (slot.generation != generation || slot.texture == nullptr) continue;
        ++slot.pins;
        frame.texture = slot.texture.get();
        frame.ready_semaphore = slot.semaphore.get();
        frame.width = slot.width;
        frame.height = slot.height;
        frame.generation = slot.generation;
        frame.guest_frame = slot.guest_frame;
        frame.slot = index;
        return true;
    }
    return false;
}

bool NativeGpu::output_produced_ns(u64 generation, u64& produced_ns) const {
    std::lock_guard lock(impl_->output_mutex);
    for (const auto& slot : impl_->outputs) {
        if (slot->generation == generation && slot->produced_ns != 0u) {
            produced_ns = slot->produced_ns;
            return true;
        }
    }
    return false;
}

void NativeGpu::release_output(u32 slot, u64 generation) {
    std::lock_guard lock(impl_->output_mutex);
    if (slot >= impl_->outputs.size()) return;
    OutputSlot& output = *impl_->outputs[slot];
    if (output.pins != 0u) --output.pins;
    (void)generation;
}

std::string NativeGpu::target_inventory() const {
    std::lock_guard lock(impl_->stats_mutex);
    const Impl& impl = *impl_;
    std::string out;
    char row[256];
    // The propagation table. One integer scale drives every line below it; the
    // output rectangle is stated because the presenter fits the raster into it,
    // not because anything here was sized from it.
    const ContentRect fitted = fit_content_rect(static_cast<int>(impl.settings.output_width),
                                                static_cast<int>(impl.settings.output_height),
                                                impl.settings.content_aspect);
    std::snprintf(row, sizeof(row),
                  "[render-targets] output %ux%u  fitted %dx%d at %d,%d  aspect %.4f\n",
                  impl.settings.output_width, impl.settings.output_height, fitted.width,
                  fitted.height, fitted.x, fitted.y, impl.settings.content_aspect);
    out += row;
    std::snprintf(row, sizeof(row),
                  "[render-targets] internal scale %ux (isotropic)  efb %ux%u  raster %ux%u\n",
                  impl.efb.scale.factor, impl.efb_width, impl.efb_height,
                  impl.efb.native_raster().width, impl.efb.native_raster().height);
    out += row;
    std::uint64_t scaled_bytes = 0, guest_bytes = 0;
    for (const auto& [address, target] : impl.targets) {
        if (target.texture == nullptr) continue;
        const std::uint64_t native = std::uint64_t(target.native_width) * target.native_height * 4u;
        const std::uint64_t guest = std::uint64_t(target.guest_width) * target.guest_height * 4u;
        scaled_bytes += native;
        guest_bytes += guest;
        std::snprintf(row, sizeof(row),
                      "[render-targets]   0x%08X guest %ux%u -> native %ux%u  x%.3f,%.3f  %.3f MiB"
                      " (guest size would be %.3f MiB)%s\n",
                      address, target.guest_width, target.guest_height, target.native_width,
                      target.native_height,
                      target.guest_width != 0u ? double(target.native_width) / target.guest_width : 0.0,
                      target.guest_height != 0u ? double(target.native_height) / target.guest_height : 0.0,
                      double(native) / (1024.0 * 1024.0), double(guest) / (1024.0 * 1024.0),
                      (target.guest_width < kGuestRasterWidth &&
                       target.guest_height < kGuestRasterHeight)
                          ? "  sub-raster"
                          : "");
        out += row;
    }
    const std::uint64_t efb = std::uint64_t(impl.efb_width) * impl.efb_height * 8u;  // colour + depth
    std::snprintf(row, sizeof(row),
                  "[render-targets] totals: efb %.3f MiB + targets %.3f MiB = %.3f MiB"
                  "; the same targets at guest resolution would be %.3f MiB\n",
                  double(efb) / (1024.0 * 1024.0), double(scaled_bytes) / (1024.0 * 1024.0),
                  double(efb + scaled_bytes) / (1024.0 * 1024.0),
                  double(guest_bytes) / (1024.0 * 1024.0));
    out += row;
    return out;
}

void NativeGpu::set_rejections(native::RejectionMailbox* mailbox) {
    if (impl_ != nullptr) impl_->rejections = mailbox;
}

NativeGpu::Stats NativeGpu::stats() const {
    std::lock_guard lock(impl_->stats_mutex);
    impl_->update_budget();
    NativeGpu::Stats out = impl_->stats;
    if (impl_->queue != nullptr) {
        const auto queue_stats = static_cast<const plume::D3D12CommandQueue*>(
            impl_->queue.get())->getSubmissionCpuStats();
        static_assert(std::tuple_size_v<decltype(queue_stats.stages)> ==
                      std::tuple_size_v<decltype(out.queue_cpu_phases)>);
        out.queue_cpu_profile_enabled = queue_stats.enabled;
        for (std::size_t i = 0; i < out.queue_cpu_phases.size(); ++i) {
            const auto& source = queue_stats.stages[i];
            out.queue_cpu_phases[i] = {source.calls, source.ns, source.max_ns,
                                       source.failures, source.last_failure_hresult};
        }
    }
    out.ring_retained_bytes = 0;
    out.ring_growths = 0;
    out.ring_allocation_failures = 0;
    out.ring_current_bytes = 0;
    out.ring_resets = 0;
    out.ring_chunk_switches = 0;
    for (const auto& slot : impl_->slots) {
        const render::UploadArena::Stats arena = slot.ring.stats();
        out.ring_retained_bytes += arena.retained_bytes;
        out.ring_growths += arena.growths;
        out.ring_allocation_failures += arena.allocation_failures;
        out.ring_current_bytes += arena.used_bytes;
        out.ring_resets += arena.resets;
        out.ring_chunk_switches += arena.chunk_switches;
    }
    out.geometry_assets_unreferenced = 0;
    for (const auto& [identity, assets] : impl_->geometry_assets) {
        for (const auto& asset : assets) {
            if (asset->instances == 0u) ++out.geometry_assets_unreferenced;
        }
    }
    out.fence_already_complete = out.slot_fence_already_complete;
    {
        std::lock_guard output_lock(impl_->output_mutex);
        out.output_slots = impl_->outputs.size();
        out.output_pins = 0;
        out.output_pool_bytes = 0;
        for (const auto& output : impl_->outputs) {
            out.output_pins += output->pins;
            if (output->texture != nullptr) {
                out.output_pool_bytes +=
                    static_cast<u64>(output->width) * output->height * 4u;
            }
        }
        out.output_pins_peak = std::max(out.output_pins_peak, out.output_pins);
    }
    if (impl_->transfer_live) {
        const render::TransferQueue::Stats t = impl_->transfer.stats();
        out.copy_queue_submits = t.submits;
        out.copy_queue_uploads = t.buffer_uploads + t.texture_uploads;
        out.copy_queue_generated_uploads = t.generated_buffer_uploads;
        out.copy_queue_generated_bytes = t.generated_buffer_bytes;
        out.copy_queue_bytes = t.bytes;
        out.copy_queue_staging_bytes = t.staging_bytes;
        out.copy_queue_retained_bytes = t.retained_bytes;
        out.copy_queue_peak_batch_bytes = t.peak_batch_bytes;
        out.copy_queue_largest_request_bytes = t.largest_request_bytes;
        out.copy_queue_growths = t.growths;
        out.copy_queue_allocation_failures = t.allocation_failures;
        out.copy_queue_current_bytes = t.current_bytes;
        out.copy_queue_resets = t.resets;
        out.copy_queue_chunk_switches = t.chunk_switches;
        for (std::size_t index = 0;
             index < static_cast<std::size_t>(render::TransferQueue::UploadOwner::Count);
             ++index) {
            out.copy_queue_owner_uploads[index] = t.owner_uploads[index];
            out.copy_queue_owner_bytes[index] = t.owner_bytes[index];
            out.copy_queue_owner_staging_bytes[index] = t.owner_staging_bytes[index];
        }
        out.copy_queue_early_submits = t.early_submits;
        out.copy_queue_batch_waits = t.batch_waits;
        out.copy_queue_batch_wait_ns = t.batch_wait_ns;
        out.copy_queue_refused_oversize = t.refused_oversize;
        out.copy_queue_refused_arguments = t.refused_arguments;
        out.copy_queue_semaphores = t.semaphores;
        out.copy_queue_semaphore_failures = t.semaphore_failures;
    }
    return out;
}

#endif

}  // namespace gekko
