#include "render/renderer.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <array>
#include <cmath>
#include <cstring>
#include <unordered_map>

// shadow_render's include root is runtime/lift only; the schedule offset is shared with the game thread and the audio
// device thread (runtime/include): header-only atomics, one object program-wide (inline function statics).
#include "../../include/gekko/display_geometry.h"
#include "../../include/gekko/schedule_offset.h"
#include "render/frame_math.h"
#include "render/gpu_timer.h"
#include "render/particle_fill.h"
#include "render/present_trace.h"
#include "render/init_trace.h"
#include "render/packet_stream.h"
#include "render/profile.h"
#include "render/pipelines.h"
#include "render/snapshot_reads.h"

#include "render_ps_base.h"
#include "render_ps_dual.h"
#include "render_ps_environment.h"
#include "render_cs_glare_columns.h"
#include "render_cs_glare_rows.h"
#include "render_cs_glare_tiles_clear.h"
#include "render_ps_base_tiles.h"
#include "render_ps_dual_tiles.h"
#include "render_ps_environment_tiles.h"
#include "render_ps_immediate_tiles.h"
#include "render_ps_particle_tiles.h"
#include "render_vs_glare_tiles.h"
#include "render_ps_glare_blur.h"
#include "render_ps_glare_box.h"
#include "render_ps_glare_capture.h"
#include "render_ps_depth_bits.h"
#include "render_ps_glare_composite.h"
#include "render_ps_resolve_glare.h"
#include "render_ps_glare_merge.h"
#include "render_ps_particle.h"
#include "render_ps_resolve.h"
#include "render_ps_scale.h"
#include "render_ps_texture.h"
#include "render_ps_immediate.h"
#include "render_vs_fullscreen.h"
#include "render_vs_immediate.h"
#include "render_vs_mesh.h"
#include "render_vs_particle.h"

namespace shadow::render {

// The packet stream (render/packet_stream.h, GEKKO_PACKET_RECORD): what this renderer is asked, recorded for another
// backend to replay. Compiled out in shipping builds; otherwise one null-pointer check.
#if defined(GEKKO_SHIPPING)
#define SHADOW_RECORD_PACKETS(call) \
    do {                            \
    } while (false)
#else
#define SHADOW_RECORD_PACKETS(call) \
    do {                            \
        if (recorder_ != nullptr)   \
            recorder_->call;        \
    } while (false)
#endif

namespace {

// frame_math.h's view-pixel rectangle as D3D12's scissor.
D3D12_RECT to_rect(const PixelBox& box) {
    return D3D12_RECT{static_cast<LONG>(box.left), static_cast<LONG>(box.top), static_cast<LONG>(box.right),
                      static_cast<LONG>(box.bottom)};
}

static_assert(sizeof(PositionVertex) == 24, "PositionVertex layout");
static_assert(sizeof(AttributeVertex) == 20, "AttributeVertex layout");

static_assert(sizeof(DrawConstants) == 256, "one draw block");

static_assert(sizeof(ParticleConstants) <= 256, "one batch block");
static_assert(sizeof(ImmediateConstants) <= 256, "one immediate block");

D3D_PRIMITIVE_TOPOLOGY to_d3d(Topology topology) {
    switch (topology) {
    case Topology::Lines: return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
    case Topology::LineStrip: return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
    case Topology::Triangles: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    case Topology::TriangleStrip: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    }
    return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
}

// A draw's skin-cache element when the frame has no skinned vertices for it.
constexpr std::uint32_t kNotSkinned = 0xFFFFFFFFu;
static_assert(sizeof(Float3x4) == sizeof(BoneMatrix), "a palette entry is a Float3x4");

bool fail(std::string* error, const std::string& what) {
    if (error != nullptr)
        *error = what;
    return false;
}

// The console's colour target had no alpha channel, so destination-alpha factors read 1 (RENDERING_ORIGINAL.md 4.1);
// the scene target's alpha stays 1 and those factors are resolved here to keep that meaning.
D3D12_BLEND to_d3d(Blend blend) {
    switch (blend) {
    case Blend::Zero: return D3D12_BLEND_ZERO;
    case Blend::One: return D3D12_BLEND_ONE;
    case Blend::SrcColor: return D3D12_BLEND_SRC_COLOR;
    case Blend::InvSrcColor: return D3D12_BLEND_INV_SRC_COLOR;
    case Blend::SrcAlpha: return D3D12_BLEND_SRC_ALPHA;
    case Blend::InvSrcAlpha: return D3D12_BLEND_INV_SRC_ALPHA;
    case Blend::DestAlpha: return D3D12_BLEND_ONE;
    case Blend::InvDestAlpha: return D3D12_BLEND_ZERO;
    case Blend::DestColor: return D3D12_BLEND_DEST_COLOR;
    case Blend::InvDestColor: return D3D12_BLEND_INV_DEST_COLOR;
    }
    return D3D12_BLEND_ONE;
}

// kSkipClear (frame_math.h, docs/GPU_FILL.md): a fragment of alpha exactly 0 leaves the target as it was when the draw
// writes no depth and its colour blend at source alpha 0 is ZERO x src + ONE x dst: source factor SrcAlpha or Zero,
// destination factor InvSrcAlpha, One or DestAlpha (drawn as ONE, to_d3d). The alpha channel is always
// ZERO x src + ONE x dst. Diagnostic builds: SHADOW_R2_SKIP_CLEAR=0 draws every fragment (A/B on one binary).
bool skip_clear_enabled() {
#if defined(GEKKO_SHIPPING)
    return true;
#else
    static const bool on = [] {
        const char* v = std::getenv("SHADOW_R2_SKIP_CLEAR");
        return v == nullptr || v[0] != '0';
    }();
    return on;
#endif
}
// Not done, measured (Codex review 2026-11-05; docs/PERF_60FPS_2026-11-04.md): an R, G, B-only write mask on the
// blended draws (their alpha blend ZERO x src + ONE x dst keeps the alpha anyway) gave identical pixels, audio and state,
// and no GPU time - 5K smoke frames paired at stable clocks, +0.006 ms mean, inside run-to-run noise.
// The full-resolution HUD under a scaled render size (record: hud_deferred). Diagnostic builds:
// SHADOW_R2_HUD_OVERLAY=0 keeps the 2D in the scaled scene (A/B on one binary).
bool hud_overlay_enabled() {
#if defined(GEKKO_SHIPPING)
    return true;
#else
    static const bool on = [] {
        const char* v = std::getenv("SHADOW_R2_HUD_OVERLAY");
        return v == nullptr || v[0] != '0';
    }();
    return on;
#endif
}
// Diagnostics only (GPU_FILL.md): SHADOW_R2_PARTICLE_PROBE = bits, changing the picture to find what limits particle
// fill: 1 = no texture sample, 2 = no blending (written opaque), 4 = no depth test; meshes 8 = no blending, 16 = no
// texture sample; 32 = immediates no blending; 64 = no mesh extra passes. Never in a shipping build.
std::uint32_t particle_probe() {
#if defined(GEKKO_SHIPPING)
    return 0u;
#else
    static const std::uint32_t probe = [] {
        const char* v = std::getenv("SHADOW_R2_PARTICLE_PROBE");
        return v != nullptr ? static_cast<std::uint32_t>(std::strtoul(v, nullptr, 10)) : 0u;
    }();
    return probe;
#endif
}
// The exact opaque write (docs/GPU_FILL.md section 19). With the blend unit enabled, this GPU coalesces two fragments
// of one draw that cover complementary samples of one pixel and stores their mean in every sample (section 18): not
// the specified per-sample blend, and dependent on scheduling. A base pass whose blend provably reduces to writing
// the source colour - every fragment's alpha exactly 1 under SrcAlpha / InvSrcAlpha, or the factors One / Zero - is
// drawn with the blend unit off and only R, G, B written (the scene's alpha is ZERO x src + ONE x dst: unchanged):
// the specified result in every sample, independent of scheduling. SHADOW_R2_EXACT_OPAQUE=0 (non-shipping) draws
// them through the blend unit as before; SHADOW_R2_OPAQUE_CHECK=1 makes such a draw output magenta wherever a
// fragment's alpha is not exactly 1 (the proof's check).
bool exact_opaque_enabled() {
#if defined(GEKKO_SHIPPING)
    return true;
#else
    static const bool on = [] {
        const char* v = std::getenv("SHADOW_R2_EXACT_OPAQUE");
        return v == nullptr || v[0] != '0';
    }();
    return on;
#endif
}
// SHADOW_R2_OPAQUE_CHECK: 1 = the fragment alpha, 2 = the interpolated vertex alpha, 3 = the filtered texel alpha.
std::uint32_t opaque_check_enabled() {
#if defined(GEKKO_SHIPPING)
    return 0u;
#else
    static const std::uint32_t bit = [] {
        const char* v = std::getenv("SHADOW_R2_OPAQUE_CHECK");
        return v == nullptr ? 0u : v[0] == '1' ? kOpaqueCheck : v[0] == '2' ? 256u : v[0] == '3' ? 512u : 0u;
    }();
    return bit;
#endif
}
// The exact alpha (docs/GPU_FILL.md section 19): when every vertex alpha and every texel alpha of a base pass is 255,
// its fragment alpha is the material constant's exactly (kExactAlpha), not the interpolated / filtered 1 - e.
// SHADOW_R2_EXACT_ALPHA=0 (non-shipping) keeps the interpolated value.
bool exact_alpha_enabled() {
#if defined(GEKKO_SHIPPING)
    return true;
#else
    static const bool on = [] {
        const char* v = std::getenv("SHADOW_R2_EXACT_ALPHA");
        return v == nullptr || v[0] != '0';
    }();
    return on;
#endif
}
// The blend writes the source colour when the fragment alpha is 1 (to_d3d: DestAlpha is ONE, InvDestAlpha ZERO).
bool identity_when_opaque(const Recipe& recipe) {
    const bool src_one = recipe.src == Blend::One || recipe.src == Blend::DestAlpha || recipe.src == Blend::SrcAlpha;
    const bool dst_zero = recipe.dst == Blend::Zero || recipe.dst == Blend::InvDestAlpha || recipe.dst == Blend::InvSrcAlpha;
    return src_one && dst_zero;
}
// Without any alpha source the blend is the identity whatever the alpha.
bool identity_always(const Recipe& recipe) {
    return (recipe.src == Blend::One || recipe.src == Blend::DestAlpha) &&
           (recipe.dst == Blend::Zero || recipe.dst == Blend::InvDestAlpha);
}
// Diagnostics (SHADOW_R2_DUMP_PARTICLES=<directory>, SHADOW_R2_DUMP_PARTICLES_TICKS=<first>,<last>; non-shipping):
// every particle draw of the frames in the tick range appended to <directory>/particles_<tick>.bin, in draw order -
// the real game's particle input for tools/exact/ (docs/EXACT_RENDER_SPEC.md). Record: 'PDRW', tick (u64), pass
// (0 scene, 1 glare sources), draw index, target width / height / samples, recipe (src, dst, depth test, depth
// write, cull, cutoff, fog, fog start, fog end, fog colour), batch (right xyz, up xyz, colour rgba, particle colours,
// texture handle, sampler filter / u / v), the pass view-projection (row-major 4x4), count, then count 44-byte
// records (frame.h Particle).
void dump_particle_draw(std::uint64_t tick, bool glare_sources, std::uint32_t d, const TextureDesc& target,
                        const Recipe& recipe, const ParticleBatch& batch, const Particle* records, const Mat4& vp) {
#if defined(GEKKO_SHIPPING)
    (void)tick, (void)glare_sources, (void)d, (void)target, (void)recipe, (void)batch, (void)records, (void)vp;
#else
    static const char* dir = std::getenv("SHADOW_R2_DUMP_PARTICLES");
    if (dir == nullptr || dir[0] == 0)
        return;
    static const std::pair<std::uint64_t, std::uint64_t> range = [] {
        unsigned long long a = 0, b = ~0ull;
        if (const char* v = std::getenv("SHADOW_R2_DUMP_PARTICLES_TICKS"))
            std::sscanf(v, "%llu,%llu", &a, &b);
        return std::make_pair(static_cast<std::uint64_t>(a), static_cast<std::uint64_t>(b));
    }();
    if (tick < range.first || tick > range.second)
        return;
    char name[64];
    std::snprintf(name, sizeof(name), "/particles_%06llu.bin", static_cast<unsigned long long>(tick));
    std::FILE* out = std::fopen((std::string(dir) + name).c_str(), "ab");
    if (out == nullptr)
        return;
    const std::uint32_t magic = 0x57524450u;  // 'PDRW'
    std::fwrite(&magic, 4, 1, out);
    std::fwrite(&tick, 8, 1, out);
    const std::uint32_t head[11] = {glare_sources ? 1u : 0u, d, target.width, target.height, target.samples,
                                    static_cast<std::uint32_t>(recipe.src), static_cast<std::uint32_t>(recipe.dst),
                                    recipe.depth_test ? 1u : 0u, recipe.depth_write ? 1u : 0u,
                                    static_cast<std::uint32_t>(recipe.cull), recipe.fog ? 1u : 0u};
    std::fwrite(head, sizeof(head), 1, out);
    const float fog[3] = {recipe.alpha_cutoff, recipe.fog_start, recipe.fog_end};
    std::fwrite(fog, sizeof(fog), 1, out);
    const std::uint32_t fog_colour = packed(recipe.fog_colour);
    std::fwrite(&fog_colour, 4, 1, out);
    const float axes[10] = {batch.right.x, batch.right.y, batch.right.z, batch.up.x, batch.up.y, batch.up.z,
                            batch.colour.x, batch.colour.y, batch.colour.z, batch.colour.w};
    std::fwrite(axes, sizeof(axes), 1, out);
    const std::uint32_t tail[5] = {batch.particle_colours ? 1u : 0u, batch.texture.value,
                                   static_cast<std::uint32_t>(batch.sampler.filter), static_cast<std::uint32_t>(batch.sampler.u),
                                   static_cast<std::uint32_t>(batch.sampler.v)};
    std::fwrite(tail, sizeof(tail), 1, out);
    std::fwrite(vp.m, sizeof(vp.m), 1, out);
    std::fwrite(&batch.count, 4, 1, out);
    std::fwrite(records, sizeof(Particle), batch.count, out);
    std::fclose(out);
#endif
}
// Diagnostics (SHADOW_R2_IDENTITY_BLEND_OFF=1, non-shipping): a One/Zero recipe is drawn with the blend unit
// disabled (a plain write) instead of an enabled identity blend (GPU_FILL.md section 18: on 4x MSAA edge pixels the
// enabled blend unit is not exact and depends on the draw's scheduling).
bool identity_blend_off() {
#if defined(GEKKO_SHIPPING)
    return false;
#else
    static const bool on = [] {
        const char* v = std::getenv("SHADOW_R2_IDENTITY_BLEND_OFF");
        return v != nullptr && v[0] == '1';
    }();
    return on;
#endif
}
// Diagnostics (SHADOW_R2_DEPTH_S8=1, non-shipping): the scene depth as D32_FLOAT_S8X24_UINT - the same 32-bit float
// depth in another storage format (GPU_FILL.md section 16: does 4x depth storage decide depth ties?).
DXGI_FORMAT scene_depth_format() {
#if defined(GEKKO_SHIPPING)
    return DXGI_FORMAT_D32_FLOAT;
#else
    static const DXGI_FORMAT format = [] {
        const char* v = std::getenv("SHADOW_R2_DEPTH_S8");
        return v != nullptr && v[0] == '1' ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D32_FLOAT;
    }();
    return format;
#endif
}
std::uint32_t depth_bits_sample() {
#if defined(GEKKO_SHIPPING)
    return 0u;
#else
    static const std::uint32_t n = [] {
        const char* v = std::getenv("SHADOW_R2_DEPTH_BITS");
        return v != nullptr && v[0] >= '1' && v[0] <= '4' ? static_cast<std::uint32_t>(v[0] - '0') : 0u;
    }();
    return n;
#endif
}
// The direct resolve: the frame's one full-output view resolves straight into the 8-bit output and the compose is
// skipped (see the resolve in Renderer::record). SHADOW_R2_DIRECT_RESOLVE: unset = when the render size is the
// output's (measured 2026-11-04, 1080p: GPU p50 3.59 -> 2.92 ms; rendering at 2160p into a 1080p window it gained
// nothing, so a scaled frame keeps the compose); 1 = also when scaling; 0 = never. A shader emulation of the 16-bit
// step was not exact (2026-11-04: 88-216 pixels per 1080p frame one step high - the GPU's float -> UNORM16 store is not
// round-to-nearest, D3D allows 0.6 ULP); the resolve now classifies each mean against the device's MEASURED
// thresholds of the whole R16 store -> compose -> 8-bit chain (unorm_calibration.cpp), so it writes the byte the
// compose path would. A failed calibration leaves the compose path running.
int direct_resolve_mode() {
    static const int mode = [] {
        const char* v = std::getenv("SHADOW_R2_DIRECT_RESOLVE");
        return v == nullptr || v[0] == '\0' ? -1 : v[0] == '0' ? 0 : 1;
    }();
    return mode;
}
bool direct_resolve() { return direct_resolve_mode() != 0; }
// SHADOW_R2_FOLD_UNDER=0 (A/B): the view under an overlay is resolved into its own texture and copied into the overlay
// ("views under") and the compose, as before the fold.
std::atomic<int> g_fold_under_forced{-1};  // set_fold_under_override
bool fold_under() {
    static const bool on = [] {
        const char* v = std::getenv("SHADOW_R2_FOLD_UNDER");
        return !(v != nullptr && v[0] == '0');
    }();
    const int forced = g_fold_under_forced.load(std::memory_order_relaxed);
    return forced < 0 ? on : forced != 0;
}
std::uint32_t diagnostic_draw_limit() {
#if defined(GEKKO_SHIPPING)
    return 0xFFFFFFFFu;
#else
    static const std::uint32_t limit = [] {
        const char* v = std::getenv("SHADOW_R2_DRAW_LIMIT");
        return v != nullptr && v[0] != 0 ? static_cast<std::uint32_t>(std::strtoul(v, nullptr, 10)) : 0xFFFFFFFFu;
    }();
    return limit;
#endif
}
// SHADOW_R2_SCENE_TILES_PASS=<name> (diagnostics): tile only the draw passes of that name ("scene", "glare sources").
// SHADOW_R2_SCENE_TILES_ORDINAL=k (diagnostics): tile only the k-th pass of that name in a frame (0 = the first).
std::uint32_t scene_tiles(const char* pass_name, std::uint32_t ordinal) {
#if defined(GEKKO_SHIPPING)
    (void)pass_name;
    (void)ordinal;
    return 1u;
#else
    static const char* only_ordinal = std::getenv("SHADOW_R2_SCENE_TILES_ORDINAL");
    if (only_ordinal != nullptr && only_ordinal[0] != 0 && std::strtoul(only_ordinal, nullptr, 10) != ordinal)
        return 1u;
    static const std::uint32_t grid = [] {
        const char* v = std::getenv("SHADOW_R2_SCENE_TILES");
        const unsigned long n = v != nullptr ? std::strtoul(v, nullptr, 10) : 1ul;
        return static_cast<std::uint32_t>(n >= 1ul && n <= 8ul ? n : 1ul);
    }();
    static const char* only = std::getenv("SHADOW_R2_SCENE_TILES_PASS");
    if (only != nullptr && only[0] != 0 && std::strcmp(only, pass_name) != 0) return 1u;
    return grid;
#endif
}
std::uint32_t skip_clear_flag(const Recipe& recipe) {
    const bool source_zero = recipe.src == Blend::SrcAlpha || recipe.src == Blend::Zero;
    const bool destination_kept =
        recipe.dst == Blend::InvSrcAlpha || recipe.dst == Blend::One || recipe.dst == Blend::DestAlpha;
    return skip_clear_enabled() && !recipe.depth_write && source_zero && destination_kept ? kSkipClear : 0u;
}

// The glare composite folded into the view's resolve (B3a): on, except diagnostic builds with SHADOW_R2_FOLD_GLARE=0
// (the plain composite, for A/B on one binary).
bool fold_glare_composite() {
#if defined(GEKKO_SHIPPING)
    return true;
#else
    static const bool on = [] {
        const char* v = std::getenv("SHADOW_R2_FOLD_GLARE");
        return v == nullptr || v[0] != '0';
    }();
    return on;
#endif
}

// The glare capture as compute (GLARE_OPTIMIZATION.md G1): on, except diagnostic builds with SHADOW_R2_GLARE_COMPUTE=0
// (the resolve + PSGlareCapture passes, for A/B on one binary).
bool glare_compute() {
#if defined(GEKKO_SHIPPING)
    return true;
#else
    static const bool on = [] {
        const char* v = std::getenv("SHADOW_R2_GLARE_COMPUTE");
        return v == nullptr || v[0] != '0';
    }();
    return on;
#endif
}

// The glare sources' tiles (GLARE_OPTIMIZATION.md G2): on, except diagnostic builds with SHADOW_R2_GLARE_TILES=0 (the
// sources resolved and read whole, for A/B on one binary).
bool glare_tiles() {
#if defined(GEKKO_SHIPPING)
    return true;
#else
    static const bool on = [] {
        const char* v = std::getenv("SHADOW_R2_GLARE_TILES");
        return v == nullptr || v[0] != '0';
    }();
    return on;
#endif
}

// Diagnostics (B3a): SHADOW_R2_RESOLVE_LOW_BYTE makes the views' final resolves show the low byte of each 16-bit
// value, so an 8-bit capture compares the picture at full precision: 1 the resolved mean, 2..5 sample 0..3 alone.
std::uint32_t resolve_low_byte() {
#if defined(GEKKO_SHIPPING)
    return 0u;
#else
    static const std::uint32_t on = [] {
        const char* v = std::getenv("SHADOW_R2_RESOLVE_LOW_BYTE");
        return v != nullptr && v[0] >= '1' && v[0] <= '5' ? static_cast<std::uint32_t>(v[0] - '0') : 0u;
    }();
    return on;
#endif
}
std::uint32_t handle_value(std::uint32_t slot, std::uint8_t generation) {
    return (static_cast<std::uint32_t>(generation) << 24) | (slot + 1);
}
}  // namespace

Renderer::~Renderer() {
    destroy();
}

bool Renderer::create(const RendererOptions& options, std::string* error) {
    destroy();
    options_ = options;
    if (!device_.create(options.device, error) || !heap_.create(device_, 65536, 4096, options.frames_in_flight, error) ||
        !samplers_.create(device_, options.max_anisotropy, error) || !pool_.create(device_, heap_, 64, 32, error) ||
        // A history per view; the game draws at most a handful of views (split screen and its overlay).
        !history_rtvs_.create(device_, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 32, error) ||
        // F-M1: 2 MiB per frame slot (frames use at most about 0.35 MiB; more spills, never fails).
        // G16: 512 KiB per frame slot (BENCH-FH max 349 KB / frame, Lava 277 KB); F-M1's spill chunks cover any larger frame.
        !frames_.create(device_, heap_, options.frames_in_flight, 512u << 10, error) ||
        !uploads_.create(device_, 16ull << 20, error) /* F-M2: 64 -> 16 MiB (HW_REPORT P / R; ring waits counted) */ || !create_root_signature(device_, &root_, error) ||
        !skin_cache_.create(device_, heap_, options.skinned_vertices_per_frame, options.frames_in_flight, root_.Get(),
                            error))
        return false;
    // MSAA support: a count is offered only when the GPU takes it, at quality 0, for the scene's colour format and its
    // depth format both (CheckFeatureSupport, D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS).
    msaa_supported_mask_ = 1u;
    for (std::uint32_t bit = 1; bit <= 5; ++bit) {
        const std::uint32_t count = 1u << bit;
        bool ok = true;
        for (const DXGI_FORMAT format : {options.scene_format, scene_depth_format()}) {
            D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS q{};
            q.Format = format;
            q.SampleCount = count;
            ok = ok && SUCCEEDED(device_.d3d()->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &q,
                                                                    sizeof(q))) &&
                 q.NumQualityLevels > 0;
        }
        if (ok)
            msaa_supported_mask_ |= 1u << bit;
    }
    {
        // The count asked at start; one the GPU lacks is refused for the largest it has below it (reported).
        std::uint32_t samples = std::max<std::uint32_t>(options.msaa_samples, 1u);
        if (!msaa_supported(samples)) {
            std::uint32_t fallback = 1u;
            for (std::uint32_t c = 1u; c < samples; c <<= 1u)
                if (msaa_supported(c))
                    fallback = c;
            std::lock_guard<std::mutex> lock(msaa_note_mutex_);
            msaa_note_ = std::to_string(samples) + "x MSAA is not supported by this GPU for the scene's formats; " +
                         std::to_string(fallback) + "x in use";
            samples = fallback;
        }
        msaa_ = samples;
        msaa_active_.store(samples, std::memory_order_release);
        anisotropy_active_.store(samplers_.anisotropy(), std::memory_order_release);
    }
    // GPU trace (measurement only): created only when the environment asks for it.
    const std::string gpu_trace = GpuTimer::requested_path();
    if (!gpu_trace.empty()) {
        gpu_timer_ = std::make_unique<GpuTimer>();
        if (!gpu_timer_->create(device_, options.frames_in_flight, gpu_trace, error)) {
            gpu_timer_.reset();
            return false;
        }
    }
    // GPU memory trace (measurement only): created only when the environment asks for it.
    if (gpu_memory::enabled())
        memory_trace_ = std::make_unique<GpuMemoryTrace>();
    uploads_published_.store(0, std::memory_order_relaxed);
    uploads_visible_ = 0;
#if !defined(GEKKO_SHIPPING)
    recorder_ = PacketRecorder::from_environment();
#endif
    worker_.start();
    created_ = true;
    return true;
}

void Renderer::destroy() {
    if (!created_)
        return;
    // The jobs still queued run now (they take the table lock to install what they make), then the worker is gone.
    worker_.stop();
    jit_.stop();  // its watcher waits on the direct queue's fence: gone before the device
    burst_.stop();  // the same
    std::lock_guard<std::mutex> render_lock(render_mutex_);
    std::lock_guard<std::mutex> table_lock(table_mutex_);
    if (memory_trace_ != nullptr) {  // the last row and map, everything still allocated
        memory_trace_->finish(device_, memory_counters());
        memory_trace_.reset();
    }
    recorder_.reset();  // the stream closed (render/packet_stream.h)
    uploads_.destroy();
    uploads_visible_ = 0;
    device_.wait_idle();
    if (gpu_timer_ != nullptr) {  // every frame is complete now: its rows, then the file
        gpu_timer_->finish();
        gpu_timer_->destroy(device_);
        gpu_timer_.reset();
    }
    swap_chain_.destroy();
    has_window_ = false;
    for (std::uint32_t i = 0; i < textures_.size(); ++i) {
        TextureSlot& slot = textures_[i];
        if (slot.live && slot.state.load(std::memory_order_acquire) == kSlotReady)
            retire(device_, slot.texture, &heap_);
    }
    for (std::uint32_t i = 0; i < meshes_.size(); ++i) {
        MeshSlot& slot = meshes_[i];
        if (slot.live && slot.state.load(std::memory_order_acquire) == kSlotReady) {
            retire(device_, slot.streams);
            retire(device_, slot.skin, heap_);
            retire(device_, slot.morph, heap_);
        }
    }
    // The direct resolve's table (unorm_calibration.cpp): its D3D12MA allocation must go before the allocator does.
    if (unorm_table_)
        retire(device_, unorm_table_, &heap_);
    unorm_table_format_ = DXGI_FORMAT_UNKNOWN;
    unorm_calibration_tried_ = false;
    pending_releases_.clear();
    packets_finished_ = false;
    finished_through_ = 0;
    for (History& history : histories_) {
        if (history.texture)
            retire(device_, history.texture, &heap_);
        if (history.rtv.ptr != 0)
            history_rtvs_.free(history.rtv);
    }
    histories_.clear();
    textures_.clear();
    meshes_.clear();
    free_textures_.clear();
    free_meshes_.clear();
    pipelines_.clear();
    screen_pipelines_.clear();
    compute_pipelines_.clear();
    skin_cache_.destroy();
    pool_.destroy();
    frames_.destroy();
    root_.Reset();
    device_.wait_idle();
    device_.destroy();
    created_ = false;
}

// ---- resources -----------------------------------------------------------------------------------------------------
//
// Asking (any thread, normally the game's): a slot under the table lock - the free list first, then a new one, exactly
// as the synchronous path chose them, so a handle's value does not depend on when its resource gets made - and the job
// that makes it, queued with its ticket in the slot. Making (the resource worker, or a thread waiting for it): the
// decode / conversion, the GPU resource and its upload, outside every renderer lock; then the upload list is submitted
// and the resource installed under the table lock with its state. Releasing a slot whose job has not run yet frees the
// slot at once (deterministic reuse); the job then finds the generation moved on and retires what it made.

bool Renderer::allocate_texture_slot(Scope scope, std::uint32_t* slot, std::string* error) {
    if (!free_textures_.empty()) {
        *slot = free_textures_.back();
        free_textures_.pop_back();
    } else if (!textures_.grow(slot)) {
        return fail(error, "texture handles exhausted");
    }
    TextureSlot& s = textures_[*slot];
    s.scope = scope;
    s.live = true;
    s.failure.clear();
    s.state.store(kSlotPending, std::memory_order_release);
    ++stats_.textures;
    ++stats_.textures_requested;
    return true;
}

bool Renderer::allocate_mesh_slot(Scope scope, std::uint32_t* slot, std::string* error) {
    if (!free_meshes_.empty()) {
        *slot = free_meshes_.back();
        free_meshes_.pop_back();
    } else if (!meshes_.grow(slot)) {
        return fail(error, "mesh handles exhausted");
    }
    MeshSlot& s = meshes_[*slot];
    s.scope = scope;
    s.live = true;
    s.failure.clear();
    s.state.store(kSlotPending, std::memory_order_release);
    ++stats_.meshes;
    ++stats_.meshes_requested;
    return true;
}

void Renderer::publish_uploads() {
    const std::uint64_t fence = uploads_.flush();
    std::uint64_t seen = uploads_published_.load(std::memory_order_relaxed);
    while (fence > seen && !uploads_published_.compare_exchange_weak(seen, fence, std::memory_order_acq_rel)) {
    }
}

bool Renderer::make_texture(const assets::TextureImage& image, GpuTexture* out, std::string* error) {
    GEKKO_PROFILE_ZONE("res/make-texture");
    GEKKO_PROFILE_VALUE(image.rgba.size());
    GpuTexture texture;
    TextureInfo info;
    info.width = image.width;
    info.height = image.height;
    info.mip_levels = static_cast<std::uint16_t>(image.mip_levels);
    info.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    info.name = L"shadow.asset_texture";
    if (!create_texture_2d(device_, info, &texture, error) ||
        !create_texture_srv(device_, heap_, texture, DXGI_FORMAT_UNKNOWN, error)) {
        retire(device_, texture, &heap_);
        return false;
    }
    texture.opaque = true;
    for (std::size_t i = 3; i < image.rgba.size(); i += 4)
        if (image.rgba[i] != 0xFF) {
            texture.opaque = false;
            break;
        }
    std::vector<SubresourceData> levels(image.mip_levels);
    for (std::uint32_t l = 0; l < image.mip_levels; ++l)
        levels[l] = {image.rgba.data() + image.level_offsets[l], std::uint64_t{image.level_width(l)} * 4};
    if (!uploads_.upload_texture(texture.resource.Get(), 0, image.mip_levels, levels.data(), error)) {
        uploads_.flush();
        retire(device_, texture, &heap_);
        return false;
    }
    *out = std::move(texture);
    return true;
}

bool Renderer::make_mesh(const assets::MeshData& mesh, MeshResources* out, bool* morphed_out, std::string* error) {
    GEKKO_PROFILE_ZONE("res/make-mesh");
    GEKKO_PROFILE_VALUE(mesh.vertex_count);
    if (mesh.vertex_count == 0 || mesh.indices.empty())
        return fail(error, "empty mesh");
    std::vector<PositionVertex> positions(mesh.vertex_count);
    std::vector<AttributeVertex> attributes(mesh.vertex_count);
    for (std::uint32_t v = 0; v < mesh.vertex_count; ++v) {
        PositionVertex& p = positions[v];
        std::memcpy(p.position, &mesh.positions[std::size_t{v} * 3], sizeof(p.position));
        for (int k = 0; k < 3; ++k)
            p.normal[k] = mesh.normals.empty() ? 0.0f : mesh.normals[std::size_t{v} * 3 + k];
        AttributeVertex& a = attributes[v];
        // No colour array: the vertex colour channel is black (the console's channel without a colour source;
        // R2_MATERIAL_CENSUS.md 4.4).
        a.colour = mesh.colours.empty()
                       ? 0xFF000000u
                       : (mesh.colours[std::size_t{v} * 4] | (mesh.colours[std::size_t{v} * 4 + 1] << 8) |
                          (mesh.colours[std::size_t{v} * 4 + 2] << 16) |
                          (static_cast<std::uint32_t>(mesh.colours[std::size_t{v} * 4 + 3]) << 24));
        for (int k = 0; k < 2; ++k) {
            a.uv0[k] = mesh.uv0.empty() ? 0.0f : mesh.uv0[std::size_t{v} * 2 + k];
            a.uv1[k] = mesh.uv1.empty() ? 0.0f : mesh.uv1[std::size_t{v} * 2 + k];
        }
    }
    MeshResources created;
    created.opaque_colours = true;
    for (const AttributeVertex& a : attributes)
        if ((a.colour >> 24) != 0xFFu) {
            created.opaque_colours = false;
            break;
        }
    const std::uint64_t pbytes = positions.size() * sizeof(PositionVertex);
    const std::uint64_t abytes = attributes.size() * sizeof(AttributeVertex);
    const std::uint64_t ibytes = mesh.indices.size() * sizeof(std::uint32_t);
    const auto give_back = [&] {
        uploads_.flush();  // what is recorded goes first: the retirements' marks must come after it
        retire(device_, created.streams);
        retire(device_, created.skin, heap_);
        retire(device_, created.morph, heap_);
    };
    // One buffer, the streams at 256-byte boundaries (G13).
    created.attributes_offset = align_up(pbytes, 256);
    created.indices_offset = align_up(created.attributes_offset + abytes, 256);
    created.indices_bytes = ibytes;
    if (!create_buffer(device_, created.indices_offset + ibytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE,
                       D3D12_RESOURCE_STATE_COMMON, L"shadow.mesh_streams", &created.streams, error)) {
        give_back();
        return false;
    }
    // A skinned mesh also keeps its bind pose with the weights: the skinning pass poses it each frame. A delta-morphed
    // mesh keeps its base vertices the same way plus its targets: the pass blends each draw's shape from them.
    const bool skinned = mesh.skin_indices.size() >= std::size_t{mesh.vertex_count} * 4 &&
                         mesh.skin_weights.size() >= std::size_t{mesh.vertex_count} * 4;
    const std::size_t vc = mesh.vertex_count;
    bool morphed = !mesh.delta_targets.empty() && mesh.delta_targets.size() <= kMaxMorphTargets;
    for (const assets::MeshData::DeltaTarget& t : mesh.delta_targets) {
        if ((!t.positions.empty() && t.positions.size() != vc * 3) || (!t.normals.empty() && t.normals.size() != vc * 3))
            morphed = false;
    }
    std::vector<SkinVertex> skin;
    if (skinned || morphed) {
        skin.resize(mesh.vertex_count);
        for (std::uint32_t v = 0; v < mesh.vertex_count; ++v) {
            SkinVertex& s = skin[v];
            std::memcpy(s.position, positions[v].position, sizeof(s.position));
            std::memcpy(s.normal, positions[v].normal, sizeof(s.normal));
            std::memset(s.weights, 0, sizeof(s.weights));
            s.indices = 0;
            if (!skinned)
                continue;
            std::memcpy(s.weights, &mesh.skin_weights[std::size_t{v} * 4], sizeof(s.weights));
            for (int k = 0; k < 4; ++k)
                s.indices |= static_cast<std::uint32_t>(mesh.skin_indices[std::size_t{v} * 4 + k]) << (8 * k);
        }
    }
    std::vector<MorphDelta> deltas;
    if (morphed) {
        deltas.resize(mesh.delta_targets.size() * vc);
        for (std::size_t t = 0; t < mesh.delta_targets.size(); ++t) {
            const assets::MeshData::DeltaTarget& target = mesh.delta_targets[t];
            for (std::size_t v = 0; v < vc; ++v) {
                MorphDelta& d = deltas[t * vc + v];
                for (int k = 0; k < 3; ++k) {
                    d.position[k] = target.positions.empty() ? 0.0f : target.positions[v * 3 + k];
                    d.normal[k] = target.normals.empty() ? 0.0f : target.normals[v * 3 + k];
                }
            }
        }
    }
    if (!uploads_.upload_buffer(created.streams.resource.Get(), 0, positions.data(), pbytes, error) ||
        !uploads_.upload_buffer(created.streams.resource.Get(), created.attributes_offset, attributes.data(), abytes,
                                error) ||
        !uploads_.upload_buffer(created.streams.resource.Get(), created.indices_offset, mesh.indices.data(), ibytes,
                                error) ||
        ((skinned || morphed) && !create_skin_source(device_, heap_, uploads_, skin, &created.skin, error)) ||
        (morphed && !create_morph_source(device_, heap_, uploads_, deltas,
                                         static_cast<std::uint32_t>(mesh.delta_targets.size()), &created.morph,
                                         error))) {
        give_back();
        return false;
    }
    created.skinned = skinned;
    created.vertex_count = mesh.vertex_count;
    created.submeshes = mesh.submeshes;
    *out = std::move(created);
    *morphed_out = morphed;
    return true;
}

void Renderer::install_texture(std::uint32_t slot, std::uint8_t generation, bool made, GpuTexture texture,
                               const std::string& failure) {
    std::lock_guard<std::mutex> lock(table_mutex_);
    TextureSlot& s = textures_[slot];
    if (!s.live || s.generation != generation) {
        // Released while it was being made: the slot went back to the free list then; what was made goes now.
        if (made)
            retire(device_, texture, &heap_);
        return;
    }
    if (made) {
        s.texture = std::move(texture);
        s.state.store(kSlotReady, std::memory_order_release);
    } else {
        s.failure = failure;
        ++stats_.resources_failed;
        s.state.store(kSlotFailed, std::memory_order_release);
    }
}

void Renderer::install_mesh(std::uint32_t slot, std::uint8_t generation, bool made, MeshResources resources,
                            bool morphed, const std::string& failure) {
    std::lock_guard<std::mutex> lock(table_mutex_);
    MeshSlot& s = meshes_[slot];
    if (!s.live || s.generation != generation) {
        if (made) {
            retire(device_, resources.streams);
            retire(device_, resources.skin, heap_);
            retire(device_, resources.morph, heap_);
        }
        return;
    }
    if (made) {
        static_cast<MeshResources&>(s) = std::move(resources);
        if (s.skinned)
            ++stats_.skinned_meshes;
        if (morphed)
            ++stats_.morphed_meshes;
        s.state.store(kSlotReady, std::memory_order_release);
    } else {
        s.failure = failure;
        ++stats_.resources_failed;
        s.state.store(kSlotFailed, std::memory_order_release);
    }
}

TextureHandle Renderer::request_texture(Scope scope, TextureSource source, std::string* error) {
    std::lock_guard<std::mutex> lock(table_mutex_);
    std::uint32_t slot = 0;
    if (!allocate_texture_slot(scope, &slot, error))
        return {};
    TextureSlot& s = textures_[slot];
    const std::uint8_t generation = s.generation;
    SHADOW_RECORD_PACKETS(texture_source(scope, handle_value(slot, generation), source.record, source.texels,
                                         source.options));
    s.ticket = worker_.submit([this, slot, generation, source = std::move(source)]() mutable {
        // The importer reads the caller's copy of the stored bytes; the decode and the mip chain are the same code
        // the synchronous path ran, so the texels are the same bits.
        source.record.texels = assets::ByteView{source.texels.data(), source.texels.size()};
        assets::TextureImage image;
        GpuTexture texture;
        std::string failure;
        const bool made = assets::import_texture(source.record, source.options, &image, &failure) &&
                          make_texture(image, &texture, &failure);
        // Diagnostics: SHADOW_R2_DUMP_TEXTURE=<directory> writes every imported texture's levels as
        // <handle hex>.rgba (u32 width, u32 height, u32 levels, then the level bytes as imported).
        if (const char* dump = std::getenv("SHADOW_R2_DUMP_TEXTURE"); made && dump != nullptr && dump[0] != 0) {
            char name[32];
            std::snprintf(name, sizeof(name), "/%08X.rgba", handle_value(slot, generation));
            if (std::FILE* out = std::fopen((std::string(dump) + name).c_str(), "wb")) {
                const std::uint32_t header[3] = {image.width, image.height, image.mip_levels};
                std::fwrite(header, sizeof(header), 1, out);
                std::fwrite(image.rgba.data(), 1, image.rgba.size(), out);
                std::fclose(out);
            }
        }
        if (made)
            publish_uploads();
        install_texture(slot, generation, made, std::move(texture), failure);
    });
    return TextureHandle{handle_value(slot, generation)};
}

MeshHandle Renderer::request_mesh(Scope scope, assets::MeshData mesh, std::string* error) {
    if (mesh.vertex_count == 0 || mesh.indices.empty()) {
        fail(error, "empty mesh");
        return {};
    }
    std::lock_guard<std::mutex> lock(table_mutex_);
    std::uint32_t slot = 0;
    if (!allocate_mesh_slot(scope, &slot, error))
        return {};
    MeshSlot& s = meshes_[slot];
    const std::uint8_t generation = s.generation;
    SHADOW_RECORD_PACKETS(mesh(scope, handle_value(slot, generation), mesh));
    s.ticket = worker_.submit([this, slot, generation, mesh = std::move(mesh)]() {
        MeshResources resources;
        bool morphed = false;
        std::string failure;
        const bool made = make_mesh(mesh, &resources, &morphed, &failure);
        if (made)
            publish_uploads();
        install_mesh(slot, generation, made, std::move(resources), morphed, failure);
    });
    return MeshHandle{handle_value(slot, generation)};
}

TextureHandle Renderer::create_texture(Scope scope, const assets::TextureImage& image, std::string* error) {
    std::uint32_t slot = 0;
    std::uint8_t generation = 0;
    std::uint64_t ticket = 0;
    {
        std::lock_guard<std::mutex> lock(table_mutex_);
        if (!allocate_texture_slot(scope, &slot, error))
            return {};
        TextureSlot& s = textures_[slot];
        generation = s.generation;
        ticket = s.ticket = worker_.submit([this, slot, generation, image]() {
            GpuTexture texture;
            std::string failure;
            const bool made = make_texture(image, &texture, &failure);
            if (made)
                publish_uploads();
            install_texture(slot, generation, made, std::move(texture), failure);
        });
    }
    worker_.wait(ticket);
    const TextureHandle handle{handle_value(slot, generation)};
    std::lock_guard<std::mutex> lock(table_mutex_);
    TextureSlot* s = texture_slot(handle);
    if (s != nullptr && s->state.load(std::memory_order_acquire) == kSlotReady) {
        SHADOW_RECORD_PACKETS(texture_image(scope, handle.value, image));
        return handle;
    }
    if (s != nullptr) {
        fail(error, s->failure);
        release_locked(handle);
    }
    return {};
}

MeshHandle Renderer::create_mesh(Scope scope, const assets::MeshData& mesh, std::string* error) {
    if (mesh.vertex_count == 0 || mesh.indices.empty()) {
        fail(error, "empty mesh");
        return {};
    }
    std::uint32_t slot = 0;
    std::uint8_t generation = 0;
    std::uint64_t ticket = 0;
    {
        std::lock_guard<std::mutex> lock(table_mutex_);
        if (!allocate_mesh_slot(scope, &slot, error))
            return {};
        MeshSlot& s = meshes_[slot];
        generation = s.generation;
        ticket = s.ticket = worker_.submit([this, slot, generation, &mesh]() {
            // The caller waits for this job below, so its mesh outlives it.
            MeshResources resources;
            bool morphed = false;
            std::string failure;
            const bool made = make_mesh(mesh, &resources, &morphed, &failure);
            if (made)
                publish_uploads();
            install_mesh(slot, generation, made, std::move(resources), morphed, failure);
        });
    }
    worker_.wait(ticket);
    const MeshHandle handle{handle_value(slot, generation)};
    std::lock_guard<std::mutex> lock(table_mutex_);
    MeshSlot* s = mesh_slot(handle);
    if (s != nullptr && s->state.load(std::memory_order_acquire) == kSlotReady) {
        SHADOW_RECORD_PACKETS(mesh(scope, handle.value, mesh));
        return handle;
    }
    if (s != nullptr) {
        fail(error, s->failure);
        release_locked(handle);
    }
    return {};
}

Renderer::TextureSlot* Renderer::texture_slot(TextureHandle handle) {
    if (!handle)
        return nullptr;
    const std::uint32_t slot = (handle.value & 0xFFFFFFu) - 1;
    if (slot >= textures_.size())
        return nullptr;
    TextureSlot& s = textures_[slot];
    return s.live && s.generation == (handle.value >> 24) ? &s : nullptr;
}

Renderer::MeshSlot* Renderer::mesh_slot(MeshHandle handle) {
    if (!handle)
        return nullptr;
    const std::uint32_t slot = (handle.value & 0xFFFFFFu) - 1;
    if (slot >= meshes_.size())
        return nullptr;
    MeshSlot& s = meshes_[slot];
    return s.live && s.generation == (handle.value >> 24) ? &s : nullptr;
}

const Renderer::TextureSlot* Renderer::drawable_texture(TextureHandle handle) {
    const TextureSlot* s = texture_slot(handle);
    if (s == nullptr)
        return nullptr;
    std::uint8_t state = s->state.load(std::memory_order_acquire);
    if (state == kSlotPending) {
        ++render_stats_.resolve_waits;
        worker_.wait(s->ticket);
        state = s->state.load(std::memory_order_acquire);
    }
    return state == kSlotReady ? s : nullptr;
}

const Renderer::MeshSlot* Renderer::drawable_mesh(MeshHandle handle) {
    const MeshSlot* s = mesh_slot(handle);
    if (s == nullptr)
        return nullptr;
    std::uint8_t state = s->state.load(std::memory_order_acquire);
    if (state == kSlotPending) {
        ++render_stats_.resolve_waits;
        worker_.wait(s->ticket);
        state = s->state.load(std::memory_order_acquire);
    }
    return state == kSlotReady ? s : nullptr;
}

void Renderer::release(TextureHandle handle, std::uint64_t named_through) {
    std::lock_guard<std::mutex> lock(table_mutex_);
    SHADOW_RECORD_PACKETS(release(true, handle.value, named_through));
    if (named_through != kNoPacket && !(packets_finished_ && named_through <= finished_through_) &&
        texture_slot(handle) != nullptr) {
        pending_releases_.push_back(PendingRelease{true, handle.value, named_through});
        ++stats_.deferred_releases;
        stats_.pending_releases = pending_releases_.size();
        return;
    }
    release_locked(handle);
}

void Renderer::finish_packets(std::uint64_t tick) {
    std::lock_guard<std::mutex> lock(table_mutex_);
    SHADOW_RECORD_PACKETS(finish_packets(tick));
    if (!packets_finished_ || tick > finished_through_)
        finished_through_ = tick;
    packets_finished_ = true;
    std::size_t kept = 0;
    for (const PendingRelease& p : pending_releases_) {
        if (p.named_through > finished_through_) {
            pending_releases_[kept++] = p;
            continue;
        }
        if (p.texture)
            release_locked(TextureHandle{p.handle});
        else
            release_locked(MeshHandle{p.handle});
    }
    pending_releases_.resize(kept);
    stats_.pending_releases = kept;
}

void Renderer::release_locked(TextureHandle handle) {
    TextureSlot* s = texture_slot(handle);
    if (s == nullptr)
        return;
    // A made texture's upload was submitted before it became Ready, so the retirement's mark covers it. One still
    // being made is left to its job (install_texture), which finds this generation gone.
    if (s->state.load(std::memory_order_acquire) == kSlotReady)
        retire(device_, s->texture, &heap_);
    s->state.store(kSlotEmpty, std::memory_order_release);
    s->live = false;
    s->generation = static_cast<std::uint8_t>(s->generation == 255 ? 1 : s->generation + 1);
    free_textures_.push_back((handle.value & 0xFFFFFFu) - 1);
    --stats_.textures;
}

void Renderer::release(MeshHandle handle, std::uint64_t named_through) {
    std::lock_guard<std::mutex> lock(table_mutex_);
    SHADOW_RECORD_PACKETS(release(false, handle.value, named_through));
    if (named_through != kNoPacket && !(packets_finished_ && named_through <= finished_through_) &&
        mesh_slot(handle) != nullptr) {
        pending_releases_.push_back(PendingRelease{false, handle.value, named_through});
        ++stats_.deferred_releases;
        stats_.pending_releases = pending_releases_.size();
        return;
    }
    release_locked(handle);
}

void Renderer::release_locked(MeshHandle handle) {
    MeshSlot* s = mesh_slot(handle);
    if (s == nullptr)
        return;
    if (s->state.load(std::memory_order_acquire) == kSlotReady) {
        retire(device_, s->streams);
        if (s->skinned)
            --stats_.skinned_meshes;
        if (s->morph.targets != 0u)
            --stats_.morphed_meshes;
        retire(device_, s->skin, heap_);
        retire(device_, s->morph, heap_);
    }
    s->state.store(kSlotEmpty, std::memory_order_release);
    s->skinned = false;
    s->submeshes.clear();
    s->live = false;
    s->generation = static_cast<std::uint8_t>(s->generation == 255 ? 1 : s->generation + 1);
    free_meshes_.push_back((handle.value & 0xFFFFFFu) - 1);
    --stats_.meshes;
}

void Renderer::release_scope(Scope scope) {
    std::lock_guard<std::mutex> lock(table_mutex_);
    SHADOW_RECORD_PACKETS(release_scope(scope));
    for (std::uint32_t i = 0; i < textures_.size(); ++i) {
        if (textures_[i].live && textures_[i].scope == scope)
            release_locked(TextureHandle{handle_value(i, textures_[i].generation)});
    }
    for (std::uint32_t i = 0; i < meshes_.size(); ++i) {
        if (meshes_[i].live && meshes_[i].scope == scope)
            release_locked(MeshHandle{handle_value(i, meshes_[i].generation)});
    }
}

// ---- pipelines -----------------------------------------------------------------------------------------------------

ID3D12PipelineState* Renderer::mesh_pipeline(Program program, const Recipe& recipe, DXGI_FORMAT colour,
                                             std::uint32_t samples, std::string* error, bool lines, bool tiles,
                                             bool plain, bool no_depth) {
    const PipelineKey key{static_cast<std::uint8_t>(program), static_cast<std::uint8_t>(recipe.src),
                          static_cast<std::uint8_t>(recipe.dst), static_cast<std::uint8_t>(recipe.cull),
                          recipe.depth_test, recipe.depth_write, colour, samples, lines, tiles, plain, no_depth};
    if (const auto found = pipelines_.find(key); found != pipelines_.end())
        return found->second.Get();
    static const D3D12_INPUT_ELEMENT_DESC mesh_inputs[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 1, 4, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 1, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    // One Particle per instance (frame.h); the vertex function expands the quad.
    static const D3D12_INPUT_ELEMENT_DESC particle_inputs[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"SIZE", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 20, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"ROTATION", 0, DXGI_FORMAT_R32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 28, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 36, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
    };
    GraphicsPipelineInfo info;
    info.vs = {kRenderVSMesh, sizeof(kRenderVSMesh)};
    info.inputs = mesh_inputs;
    info.input_count = 5;
    switch (program) {
    case Program::Base:
        info.ps = tiles ? D3D12_SHADER_BYTECODE{kRenderPSBaseTiles, sizeof(kRenderPSBaseTiles)}
                        : D3D12_SHADER_BYTECODE{kRenderPSBase, sizeof(kRenderPSBase)};
        break;
    case Program::Environment:
        info.ps = tiles ? D3D12_SHADER_BYTECODE{kRenderPSEnvironmentTiles, sizeof(kRenderPSEnvironmentTiles)}
                        : D3D12_SHADER_BYTECODE{kRenderPSEnvironment, sizeof(kRenderPSEnvironment)};
        break;
    case Program::Dual:
        info.ps = tiles ? D3D12_SHADER_BYTECODE{kRenderPSDualTiles, sizeof(kRenderPSDualTiles)}
                        : D3D12_SHADER_BYTECODE{kRenderPSDual, sizeof(kRenderPSDual)};
        break;
    case Program::Particles:
        info.vs = {kRenderVSParticle, sizeof(kRenderVSParticle)};
        info.ps = tiles ? D3D12_SHADER_BYTECODE{kRenderPSParticleTiles, sizeof(kRenderPSParticleTiles)}
                        : D3D12_SHADER_BYTECODE{kRenderPSParticle, sizeof(kRenderPSParticle)};
        info.inputs = particle_inputs;
        info.input_count = 6;
        break;
    case Program::Immediate: {
        static const D3D12_INPUT_ELEMENT_DESC immediate_inputs[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };
        info.vs = {kRenderVSImmediate, sizeof(kRenderVSImmediate)};
        info.ps = tiles ? D3D12_SHADER_BYTECODE{kRenderPSImmediateTiles, sizeof(kRenderPSImmediateTiles)}
                        : D3D12_SHADER_BYTECODE{kRenderPSImmediate, sizeof(kRenderPSImmediate)};
        info.inputs = immediate_inputs;
        info.input_count = 3;
        if (lines)
            info.topology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        break;
    }
    }
    info.color_format = colour;
    info.depth_format = scene_depth_format();
    info.samples = samples;
    info.depth.DepthEnable = TRUE;
    // Depth is reversed (frame_math.h kFarDepth): nearer is greater.
    // Diagnostics: GEKKO_DEPTH_TEST=less resolves depth ties the other way (the earlier surface keeps the pixel). A
    // capture of the same deterministic frame with and without it differs exactly where two surfaces overlap at equal
    // depth - the map of coplanar overlap (docs/tools/REPLAY_MARKERS.md "Overlap").
    static const bool strict_depth = [] {
        const char* v = std::getenv("GEKKO_DEPTH_TEST");
        return v != nullptr && std::strcmp(v, "less") == 0;
    }();
    // A mesh's own extra passes (environment, dual) lie on its base pass by design and keep GREATER_EQUAL.
    info.depth.DepthFunc = !recipe.depth_test                       ? D3D12_COMPARISON_FUNC_ALWAYS
                           : strict_depth && program == Program::Base ? D3D12_COMPARISON_FUNC_GREATER
                                                                      : D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    info.depth.DepthWriteMask = recipe.depth_write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    if (no_depth) {  // the HUD overlay: drawn onto the presented picture, after every 3D draw of its view
        info.depth_format = DXGI_FORMAT_UNKNOWN;
        info.depth.DepthEnable = FALSE;
        info.depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    }
    D3D12_RENDER_TARGET_BLEND_DESC& b = info.blend.RenderTarget[0];
    b.BlendEnable = identity_blend_off() && recipe.src == Blend::One && recipe.dst == Blend::Zero ? FALSE : TRUE;
    b.SrcBlend = to_d3d(recipe.src);
    b.DestBlend = to_d3d(recipe.dst);
    b.BlendOp = D3D12_BLEND_OP_ADD;
    b.SrcBlendAlpha = D3D12_BLEND_ZERO;  // the scene's alpha stays 1 (see to_d3d)
    b.DestBlendAlpha = D3D12_BLEND_ONE;
    b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    if (plain) {
        b.BlendEnable = FALSE;
        b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN |
                                  D3D12_COLOR_WRITE_ENABLE_BLUE;
    }
    // Measured on the disc's worlds (R2_DRIVER.md 2.5): the faces the game keeps are counter-clockwise on screen.
    info.raster.FrontCounterClockwise = TRUE;
    info.raster.CullMode = recipe.cull == Cull::Back    ? D3D12_CULL_MODE_BACK
                           : recipe.cull == Cull::Front ? D3D12_CULL_MODE_FRONT
                                                        : D3D12_CULL_MODE_NONE;
    info.name = L"shadow.mesh";
    Com<ID3D12PipelineState> pipeline;
    if (!create_graphics_pipeline(device_, root_.Get(), info, &pipeline, error))
        return nullptr;
    ID3D12PipelineState* raw = pipeline.Get();
    pipelines_.emplace(key, std::move(pipeline));
    render_stats_.pipelines = pipelines_.size();
    return raw;
}

ID3D12PipelineState* Renderer::screen_pipeline(Screen screen, DXGI_FORMAT colour, std::uint32_t samples,
                                               std::string* error) {
    const auto key = std::make_tuple(screen, colour, samples);
    if (const auto found = screen_pipelines_.find(key); found != screen_pipelines_.end())
        return found->second.Get();
    GraphicsPipelineInfo info;
    info.vs = {kRenderVSFullscreen, sizeof(kRenderVSFullscreen)};
    switch (screen) {
    case Screen::GlareCapture: info.ps = {kRenderPSGlareCapture, sizeof(kRenderPSGlareCapture)}; break;
    case Screen::GlareBlur: info.ps = {kRenderPSGlareBlur, sizeof(kRenderPSGlareBlur)}; break;
    case Screen::GlareBox: info.ps = {kRenderPSGlareBox, sizeof(kRenderPSGlareBox)}; break;
    case Screen::GlareMerge: info.ps = {kRenderPSGlareMerge, sizeof(kRenderPSGlareMerge)}; break;
    case Screen::Resolve: info.ps = {kRenderPSResolve, sizeof(kRenderPSResolve)}; break;
    case Screen::DepthBits: info.ps = {kRenderPSDepthBits, sizeof(kRenderPSDepthBits)}; break;
    case Screen::Under: info.ps = {kRenderPSTexture, sizeof(kRenderPSTexture)}; break;  // the compose's copy
    case Screen::Scale: info.ps = {kRenderPSScale, sizeof(kRenderPSScale)}; break;      // G10's present scale
    case Screen::ResolveGlare: info.ps = {kRenderPSResolveGlare, sizeof(kRenderPSResolveGlare)}; break;
    case Screen::ResolveTiles:
        info.vs = {kRenderVSGlareTiles, sizeof(kRenderVSGlareTiles)};
        info.ps = {kRenderPSResolve, sizeof(kRenderPSResolve)};
        break;
    case Screen::GlareComposite: {
        info.ps = {kRenderPSGlareComposite, sizeof(kRenderPSGlareComposite)};
        // Added to the scene, saturating at white; the scene's alpha stays.
        D3D12_RENDER_TARGET_BLEND_DESC& b = info.blend.RenderTarget[0];
        b.BlendEnable = TRUE;
        b.SrcBlend = D3D12_BLEND_ONE;
        b.DestBlend = options_.debug_glare_only ? D3D12_BLEND_ZERO : D3D12_BLEND_ONE;
        b.BlendOp = D3D12_BLEND_OP_ADD;
        b.SrcBlendAlpha = D3D12_BLEND_ZERO;
        b.DestBlendAlpha = D3D12_BLEND_ONE;
        b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        break;
    }
    }
    info.color_format = colour;
    info.samples = samples;
    info.name = L"shadow.glare";
    Com<ID3D12PipelineState> pipeline;
    if (!create_graphics_pipeline(device_, root_.Get(), info, &pipeline, error))
        return nullptr;
    ID3D12PipelineState* raw = pipeline.Get();
    screen_pipelines_.emplace(key, std::move(pipeline));
    return raw;
}

ID3D12PipelineState* Renderer::compute_pipeline(Compute compute, std::string* error) {
    if (const auto found = compute_pipelines_.find(compute); found != compute_pipelines_.end())
        return found->second.Get();
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root_.Get();
    switch (compute) {
    case Compute::GlareRows: desc.CS = {kRenderCSGlareRows, sizeof(kRenderCSGlareRows)}; break;
    case Compute::GlareColumns: desc.CS = {kRenderCSGlareColumns, sizeof(kRenderCSGlareColumns)}; break;
    case Compute::GlareTilesClear: desc.CS = {kRenderCSGlareTilesClear, sizeof(kRenderCSGlareTilesClear)}; break;
    }
    Com<ID3D12PipelineState> pipeline;
    const HRESULT hr = device_.d3d()->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline));
    if (FAILED(hr)) {
        if (error != nullptr)
            *error = "CreateComputePipelineState (glare): " + hresult_text(hr);
        return nullptr;
    }
    pipeline->SetName(L"shadow.glare.compute");
    ID3D12PipelineState* raw = pipeline.Get();
    compute_pipelines_.emplace(compute, std::move(pipeline));
    return raw;
}

// ---- frames ----------------------------------------------------------------------------------------------------------

bool Renderer::record(ID3D12GraphicsCommandList* list, std::uint32_t slot, const FramePacket& packet,
                      const Output& output, D3D12_RESOURCE_STATES* output_state_after, GpuBuffer* capture,
                      D3D12_PLACED_SUBRESOURCE_FOOTPRINT* capture_layout, std::string* error, bool commit_history) {
    GEKKO_PROFILE_ZONE("gpu/record");
    // The direct resolve needs this device's measured conversion table first (once; unorm_calibration.cpp).
    if (direct_resolve())
        ensure_unorm_calibration(output.format);
    const std::uint64_t frame_number = frames_.frame_number();
    FrameGraph& graph = frame_graph_;  // C4: kept across frames
    graph.reset(frame_number);
    // C4: nothing of this frame outlives record - the passes' closures (they refer to the packet and to this
    // function's tables) are dropped on every return, as the local graph's were; the vectors keep their capacity.
    struct GraphEnd {
        FrameGraph& graph;
        std::uint64_t frame;
        ~GraphEnd() { graph.reset(frame); }
    } graph_end{graph, frame_number};
    TextureDesc output_desc;
    output_desc.width = output.width;
    output_desc.height = output.height;
    output_desc.format = output.format;
    output_desc.clear_color = {packet.background.x, packet.background.y, packet.background.z, 1.0f};
    ResourceId out_id;
    // G10: rendered at width x height into a graph texture, then scaled into the imported resource (present_id).
    // A capture (no resource) can be scaled the same way into a graph texture the capture then reads (the proof
    // hook SHADOW_R2_CAPTURE_AT_RENDER_SIZE: the presented path's own pass, read back).
    const bool scaled = output.present_width != 0u && output.present_height != 0u &&
                        (output.present_width != output.width || output.present_height != output.height);
    ResourceId present_id = kNoResource;
    if (scaled) {
        TextureDesc present_desc = output_desc;
        present_desc.width = output.present_width;
        present_desc.height = output.present_height;
        present_desc.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};  // the bars around a fitted picture
        if (output.resource != nullptr) {
            ImportedTexture imported;
            imported.resource = output.resource;
            imported.state = output.state;
            imported.final_state = D3D12_RESOURCE_STATE_PRESENT;
            imported.rtv = output.rtv;
            imported.desc = present_desc;
            present_id = graph.import_texture("present", imported);
        } else {
            present_id = graph.create_texture("present", present_desc);
        }
        out_id = graph.create_texture("output", output_desc);
    } else if (output.resource != nullptr) {
        ImportedTexture imported;
        imported.resource = output.resource;
        imported.state = output.state;
        imported.final_state = D3D12_RESOURCE_STATE_PRESENT;
        imported.rtv = output.rtv;
        imported.desc = output_desc;
        out_id = graph.import_texture("output", imported);
    } else {
        out_id = graph.create_texture("output", output_desc);
    }

    // The frame's light sets, written once each into the frame ring; draws without a set read the empty one.
    std::vector<D3D12_GPU_VIRTUAL_ADDRESS> light_blocks(packet.light_sets.size(), 0);
    for (std::size_t i = 0; i < packet.light_sets.size(); ++i) {
        const UploadAllocation block = frames_.ring().allocate(sizeof(LightConstants), 256);
        if (!block)
            return fail(error, "frame ring full (light sets)");
        LightConstants constants;
        fill_light_constants(packet.light_sets[i], &constants);
        std::memcpy(block.cpu, &constants, sizeof(constants));
        light_blocks[i] = block.gpu;
    }
    D3D12_GPU_VIRTUAL_ADDRESS no_lights = 0;
    {
        const UploadAllocation block = frames_.ring().allocate(sizeof(LightConstants), 256);
        if (!block)
            return fail(error, "frame ring full (light sets)");
        const LightConstants empty{};
        std::memcpy(block.cpu, &empty, sizeof(empty));
        no_lights = block.gpu;
    }

    // Deformed draws (R_DESIGN.md 2.4): one job per (mesh, palette, morph weights) into this slot's region of the skin
    // cache - the mesh's delta targets blended with the draw's weights, then posed by its palette when skinned; every
    // draw of that combination (its submeshes and second passes) reads the job's output as its stream 0. A draw with
    // a palette but no skinned vertices is not drawn (its bind pose at the origin is not the picture); morph weights
    // on a mesh without targets leave its base shape.
    struct SkinJob {
        const SkinSource* source;
        D3D12_GPU_VIRTUAL_ADDRESS palette;
        std::uint32_t element;
        const MorphSource* morph;
        float weights[kMaxMorphTargets];
        bool skin;
    };
    // G12: the frame's distinct deformation jobs (the same keys as below) fit in the cache before it is used.
    {
        std::set<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>> keys;
        std::uint64_t needed = 0;
        for (const DrawRecord& draw : packet.draws) {
            if (draw.bones.count == 0u && draw.morph.count == 0u)
                continue;
            const MeshSlot* mesh = drawable_mesh(draw.mesh);
            if (mesh == nullptr || mesh->skin.vertex_count == 0u)
                continue;
            const bool morph = mesh->morph.targets != 0u && draw.morph.count != 0u;
            if (draw.bones.count == 0u && !morph)
                continue;
            if (keys.emplace(draw.mesh.value, draw.bones.count != 0u ? draw.bones.first : ~0u,
                             morph ? draw.morph.first : ~0u).second)
                needed += mesh->skin.vertex_count;
        }
        std::string grow_error;
        if (needed > skin_cache_.vertices_per_frame() &&
            !skin_cache_.reserve(static_cast<std::uint32_t>(std::min<std::uint64_t>(needed, 0x7FFFFFFFu)),
                                 &grow_error))
            std::fprintf(stderr, "[lifted-render] skin cache could not grow to %llu vertices: %s\n",
                         static_cast<unsigned long long>(needed), grow_error.c_str());
    }
    skin_cache_.begin_frame(slot);
    std::vector<SkinJob> skin_jobs;
    std::vector<std::uint32_t> skin_element(packet.draws.size(), kNotSkinned);
    // A skinned pose is written relative to its own origin (the first posed bone's translation), never in world space:
    // a character tens of thousands of units out would have every vertex rounded to that distance's float step, a
    // different rounding each frame; the draw puts the origin back through its world matrix (composed in double).
    std::vector<std::array<float, 3>> skin_origin(packet.draws.size(), std::array<float, 3>{0.0f, 0.0f, 0.0f});
    std::map<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>, std::array<float, 3>> origin_of;
    std::map<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>, std::uint32_t> job_of;  // -> element
    std::uint64_t skinned_vertices = 0, morph_jobs = 0;
    D3D12_GPU_VIRTUAL_ADDRESS no_palette = 0;  // the palette block a morph-only job binds (never read)
    for (std::size_t d = 0; d < packet.draws.size(); ++d) {
        const DrawRecord& draw = packet.draws[d];
        if (draw.bones.count == 0u && draw.morph.count == 0u)
            continue;
        const MeshSlot* mesh = drawable_mesh(draw.mesh);
        const bool morph = mesh != nullptr && mesh->morph.targets != 0u && draw.morph.count != 0u &&
                           std::size_t{draw.morph.first} + draw.morph.count <= packet.morph_weights.size();
        if (draw.bones.count == 0u && !morph)
            continue;
        const auto key = std::make_tuple(draw.mesh.value, draw.bones.count != 0u ? draw.bones.first : ~0u,
                                         morph ? draw.morph.first : ~0u);
        if (const auto found = job_of.find(key); found != job_of.end()) {
            skin_element[d] = found->second;
            skin_origin[d] = origin_of[key];
            continue;
        }
        std::array<float, 3> origin{0.0f, 0.0f, 0.0f};
        std::uint32_t element = kNotSkinned;
        const bool skin = draw.bones.count != 0u;
        const bool palette_ok = mesh != nullptr && mesh->skinned && draw.bones.count <= kMaxPaletteBones &&
                                std::size_t{draw.bones.first} + draw.bones.count <= packet.bones.size();
        if (mesh != nullptr && mesh->skin.vertex_count != 0u && (skin ? palette_ok : morph)) {
            element = skin_cache_.allocate(mesh->skin.vertex_count);
            if (element == kNotSkinned) {
                ++render_stats_.skin_overflows;
            } else {
                SkinJob job{&mesh->skin, 0, element, morph ? &mesh->morph : nullptr, {}, skin};
                if (skin) {
                    // The whole palette block the shader declares: entries past the skin's bones read zero, not
                    // whatever follows in the ring.
                    const UploadAllocation palette =
                        frames_.ring().allocate(kMaxPaletteBones * sizeof(BoneMatrix), 256);
                    if (!palette)
                        return fail(error, "frame ring full (skin palettes)");
                    const std::size_t used = std::size_t{draw.bones.count} * sizeof(BoneMatrix);
                    std::vector<Float3x4> rebased(draw.bones.count);
                    std::memcpy(rebased.data(), &packet.bones[draw.bones.first], used);
                    // Unused entries are all zero (the game's palette leaves them so) and stay zero.
                    const auto posed = [](const Float3x4& b) {
                        for (const auto& row : b.m)
                            for (const float value : row)
                                if (value != 0.0f)
                                    return true;
                        return false;
                    };
                    for (const Float3x4& b : rebased)
                        if (posed(b)) {
                            origin = {b.m[0][3], b.m[1][3], b.m[2][3]};
                            break;
                        }
                    for (Float3x4& b : rebased)
                        if (posed(b))
                            for (int row = 0; row < 3; ++row)
                                b.m[row][3] -= origin[row];
                    std::memcpy(palette.cpu, rebased.data(), used);
                    std::memset(static_cast<std::uint8_t*>(palette.cpu) + used, 0,
                                kMaxPaletteBones * sizeof(BoneMatrix) - used);
                    job.palette = palette.gpu;
                    skinned_vertices += mesh->skin.vertex_count;
                } else {
                    if (no_palette == 0) {
                        const UploadAllocation palette =
                            frames_.ring().allocate(kMaxPaletteBones * sizeof(BoneMatrix), 256);
                        if (!palette)
                            return fail(error, "frame ring full (skin palettes)");
                        std::memset(palette.cpu, 0, kMaxPaletteBones * sizeof(BoneMatrix));
                        no_palette = palette.gpu;
                    }
                    job.palette = no_palette;
                }
                if (morph) {
                    // Targets past the draw's weights blend with weight 0.
                    for (std::uint32_t t = 0; t < mesh->morph.targets && t < kMaxMorphTargets; ++t)
                        job.weights[t] = t < draw.morph.count ? packet.morph_weights[draw.morph.first + t] : 0.0f;
                    ++morph_jobs;
                }
                skin_jobs.push_back(job);
            }
        }
        job_of.emplace(key, element);
        origin_of.emplace(key, origin);
        skin_element[d] = element;
        skin_origin[d] = origin;
    }
    const bool skinning = !skin_jobs.empty();
    ResourceId skin_id = kNoResource;
    if (skinning) {
        ImportedBuffer cache;
        cache.resource = skin_cache_.buffer();
        cache.state = D3D12_RESOURCE_STATE_COMMON;
        cache.final_state = D3D12_RESOURCE_STATE_COMMON;
        skin_id = graph.import_buffer("skin cache", cache);
        graph.add_pass(
            "skin",
            [&](PassBuilder& p) { p.write(skin_id, Access::UnorderedAccess, Load::Keep); },
            [this, &skin_jobs](PassContext& c) {
                for (const SkinJob& job : skin_jobs)
                    skin_cache_.dispatch(c.list(), *job.source, job.palette, job.element, job.morph, job.weights,
                                         job.skin);
            });
    }

    // Scene snapshots (numbered in pass order; a graph texture each, made by the view's Snapshot pass) and the views
    // whose history this frame saves or draws (their history is imported into the graph).
    std::vector<SnapshotId> pass_snapshot(packet.passes.size(), ~SnapshotId{0});
    std::vector<std::uint8_t> uses_history(packet.views.size(), 0);
    SnapshotId snapshot_count = 0;
    for (std::size_t p = 0; p < packet.passes.size(); ++p) {
        const PassRecord& pass = packet.passes[p];
        if (pass.kind == PassKind::Snapshot)
            pass_snapshot[p] = snapshot_count++;
        if (pass.view >= packet.views.size())
            continue;
        if (pass.kind == PassKind::SaveHistory)
            uses_history[pass.view] = 1;
        for (std::uint32_t d = pass.first_draw; d < pass.first_draw + pass.draw_count && d < packet.draws.size(); ++d) {
            const std::uint32_t im = packet.draws[d].immediate;
            if (im < packet.immediates.size() && packet.immediates[im].source == ImageSource::History)
                uses_history[pass.view] = 1;
        }
    }
    std::vector<ResourceId> snapshot_ids(snapshot_count, kNoResource);
    // What each snapshot's readers sample of it (snapshot_reads.h): its resolve makes only that. A snapshot whose reads
    // cannot be bounded - a draw outside the canvas, one in another view, lines - resolves its whole view, as before.
    // The box the views are shown in: the whole output, or a centred box of the packet's content aspect (black
    // outside it - the compose pass clears the output), whole pixels.
    float box_x = 0.0f, box_y = 0.0f;
    float box_w = static_cast<float>(output.width), box_h = static_cast<float>(output.height);
    if (packet.content_aspect > 0.0f) {
        if (box_w > std::round(box_h * packet.content_aspect)) {
            box_w = std::round(box_h * packet.content_aspect);
            box_x = std::floor((static_cast<float>(output.width) - box_w) * 0.5f);
        } else if (box_h > std::round(box_w / packet.content_aspect)) {
            box_h = std::round(box_w / packet.content_aspect);
            box_y = std::floor((static_cast<float>(output.height) - box_h) * 0.5f);
        }
    }
    const auto view_size = [&](std::size_t v, std::uint32_t* w, std::uint32_t* h) {
        const ViewRecord& view = packet.views[v];
        const float x0 = std::round(view.rect.x * box_w), y0 = std::round(view.rect.y * box_h);
        const float x1 = std::round((view.rect.x + view.rect.width) * box_w);
        const float y1 = std::round((view.rect.y + view.rect.height) * box_h);
        *w = static_cast<std::uint32_t>(std::max(1.0f, x1 - x0));
        *h = static_cast<std::uint32_t>(std::max(1.0f, y1 - y0));
    };
    // Full-resolution HUD (docs/RESOLUTION_AUDIT.md section 12): when the render size is scaled into a picture of
    // another size (a render scale other than 100 %), each view's trailing 2D - the canvas draws after its last 3D draw
    // and after its last glare / snapshot / history step, sampling no screen copy - is left out of the scaled scene and
    // drawn after the present scale at the picture's own pixels, in the same order (the "hud overlay" pass). At 100 %
    // (and in captures) nothing moves: the picture is the one the scene path always made.
    std::vector<std::uint8_t> hud_deferred;
    bool hud_overlay = false;
    gekko::PixelSize hud_place{};
    if (scaled && output.resource != nullptr && hud_overlay_enabled()) {
        const double picture_aspect = output.picture_aspect;
        hud_place = gekko::fit_aspect({output.present_width, output.present_height},
                                      picture_aspect > 0.0 ? picture_aspect
                                                           : static_cast<double>(output.width) / output.height);
        if (hud_place.width != output.width || hud_place.height != output.height) {
            hud_deferred.assign(packet.draws.size(), std::uint8_t{0});
            for (std::size_t v = 0; v < packet.views.size(); ++v) {
                bool open = true;
                for (std::size_t p = packet.passes.size(); open && p-- > 0;) {
                    const PassRecord& pass = packet.passes[p];
                    if (pass.view != v)
                        continue;
                    if (pass.kind != PassKind::Scene) {
                        open = false;
                        break;
                    }
                    for (std::uint32_t k = pass.draw_count; open && k-- > 0;) {
                        const std::uint32_t d = pass.first_draw + k;
                        if (d >= packet.draws.size()) {
                            open = false;
                            break;
                        }
                        const DrawRecord& draw = packet.draws[d];
                        const ImmediateDraw* im =
                            draw.immediate < packet.immediates.size() ? &packet.immediates[draw.immediate] : nullptr;
                        if (draw.particles != kNoParticles || im == nullptr || im->space != Space::Canvas ||
                            im->source != ImageSource::Texture) {
                            open = false;
                            break;
                        }
                        hud_deferred[d] = 1u;
                        hud_overlay = true;
                    }
                }
            }
        }
    }
    std::vector<TexelRect> snapshot_region(snapshot_count);
    std::vector<std::uint8_t> snapshot_whole(snapshot_count, 0);
#if !defined(GEKKO_SHIPPING)
    // Diagnostics (A/B in one traced binary): SHADOW_R2_WHOLE_SNAPSHOTS=1 resolves every snapshot's whole view again.
    static const bool whole_snapshots = [] {
        const char* v = std::getenv("SHADOW_R2_WHOLE_SNAPSHOTS");
        return v != nullptr && v[0] == '1';
    }();
    if (whole_snapshots)
        std::fill(snapshot_whole.begin(), snapshot_whole.end(), std::uint8_t{1});
#endif
    {
        std::vector<std::size_t> snapshot_view(snapshot_count, packet.views.size());
        for (std::size_t p = 0; p < packet.passes.size(); ++p)
            if (pass_snapshot[p] < snapshot_count)
                snapshot_view[pass_snapshot[p]] = packet.passes[p].view;
        for (const PassRecord& pass : packet.passes) {
            if (pass.view >= packet.views.size())
                continue;
            std::uint32_t width = 0, height = 0;
            view_size(pass.view, &width, &height);
            const Canvas& canvas = packet.views[pass.view].canvas;
            for (std::uint32_t d = pass.first_draw; d < pass.first_draw + pass.draw_count && d < packet.draws.size();
                 ++d) {
                const std::uint32_t i = packet.draws[d].immediate;
                if (i >= packet.immediates.size())
                    continue;
                const ImmediateDraw& im = packet.immediates[i];
                if (im.source != ImageSource::Snapshot || im.snapshot >= snapshot_count ||
                    snapshot_whole[im.snapshot] != 0u)
                    continue;
                bool bounded = im.space == Space::Canvas && snapshot_view[im.snapshot] == pass.view &&
                               std::size_t{im.first_vertex} + im.vertex_count <= packet.immediate_vertices.size() &&
                               std::size_t{im.first_index} + im.index_count <= packet.immediate_indices.size();
                if (bounded) {
                    // The draw's placement exactly as its pass records it.
                    Mat4 placement;
                    if (im.region != kNoRegion && im.region < packet.canvas_regions.size()) {
                        PixelBox unused;
                        region_projection(canvas, packet.canvas_regions[im.region], im.anchor,
                                          static_cast<float>(width), static_cast<float>(height), &placement, &unused);
                    } else {
                        placement = canvas_projection(canvas, im.anchor, static_cast<float>(width),
                                                      static_cast<float>(height));
                    }
                    const Mat4 clip = multiply(placement, from3x4(packet.draws[d].world));
                    const float position[3][4] = {{clip.m[0][0], clip.m[0][1], clip.m[0][2], clip.m[0][3]},
                                                  {clip.m[1][0], clip.m[1][1], clip.m[1][2], clip.m[1][3]},
                                                  {clip.m[3][0], clip.m[3][1], clip.m[3][2], clip.m[3][3]}};
                    const auto rows = canvas_image_rows(placement);
                    const float uv[2][4] = {{rows[0][0], rows[0][1], rows[0][2], rows[0][3]},
                                            {rows[1][0], rows[1][1], rows[1][2], rows[1][3]}};
                    bounded = grow_sampled_texels(
                        im.topology, &packet.immediate_vertices[im.first_vertex], im.vertex_count,
                        im.index_count != 0u ? &packet.immediate_indices[im.first_index] : nullptr, im.index_count,
                        position, uv, width, height, &snapshot_region[im.snapshot]);
                }
                if (!bounded)
                    snapshot_whole[im.snapshot] = 1;
            }
        }
    }
    // C4: a draw pass reads the snapshot table as it stood when the pass was added (the snapshots filled so far),
    // through this view instead of its own copy of the vector (a heap copy per pass before).
    std::vector<std::uint32_t> snapshot_order(snapshot_count, 0u);  // the fill number of each snapshot (0: not yet)
    std::uint32_t snapshot_fills = 0;
    struct SnapshotTable {
        const std::vector<ResourceId>* ids;
        const std::vector<std::uint32_t>* order;
        std::uint32_t seen;
        std::size_t size() const noexcept { return ids->size(); }
        ResourceId operator[](std::size_t i) const noexcept {
            return (*order)[i] != 0u && (*order)[i] <= seen ? (*ids)[i] : kNoResource;
        }
    };
    if (histories_.size() < packet.views.size())
        histories_.resize(packet.views.size());
    std::vector<ResourceId> history_ids(packet.views.size(), kNoResource);
    // The views' kept histories as imported (history_ids moves to the frame's own copy when a capture does not commit
    // its save; the kept texture's state after the frame is tracked under this id).
    std::vector<ResourceId> kept_history_ids(packet.views.size(), kNoResource);
    std::vector<std::uint8_t> history_saved(packet.views.size(), 0);
    std::uint64_t snapshots = 0;

    struct ViewTargets {
        ResourceId colour = kNoResource, depth = kNoResource, resolved = kNoResource;
        D3D12_VIEWPORT viewport;
        D3D12_RECT scissor;
        Mat4 view;
        Mat4 view_projection;
    };
    std::vector<ViewTargets> views(packet.views.size());
    // A view whose picture reaches the screen only through the overlay drawn over it - the next view, over_views, the
    // same rectangle, no other view under it - is resolved straight into that overlay's multisampled colour: the
    // resolve writes into every sample the value the "views under" copy would (the resolved texel, point-sampled),
    // and the compose leaves it out (the overlay covers it with the same opaque copy). Both copies of its picture go.
    std::vector<std::size_t> fold_into(packet.views.size(), SIZE_MAX);
    if (fold_under() && msaa_ > 1u && depth_bits_sample() == 0u) {
        const auto rect_of = [&](std::size_t v) {
            const ViewRect& r = packet.views[v].rect;
            return std::array<float, 4>{box_x + std::round(r.x * box_w), box_y + std::round(r.y * box_h),
                                        box_x + std::round((r.x + r.width) * box_w),
                                        box_y + std::round((r.y + r.height) * box_h)};
        };
        for (std::size_t w = 1; w < packet.views.size(); ++w) {
            if (!packet.views[w].over_views || fold_into[w - 1] != SIZE_MAX)
                continue;
            const std::array<float, 4> over = rect_of(w);
            bool alone = rect_of(w - 1) == over;
            for (std::size_t u = 0; alone && u + 1 < w; ++u) {
                const std::array<float, 4> r = rect_of(u);
                alone = !(r[0] < over[2] && r[2] > over[0] && r[1] < over[3] && r[3] > over[1]);
            }
            for (std::size_t x = w + 1; alone && x < packet.views.size(); ++x) {  // no later overlay reads it either
                const std::array<float, 4> r = rect_of(x);
                alone = !(packet.views[x].over_views && r[0] < over[2] && r[2] > over[0] && r[1] < over[3] &&
                          r[3] > over[1]);
            }
            if (alone)
                fold_into[w - 1] = w;
        }
    }
    // Views that end in a resolved picture of their own (the rest are folded into an overlay's colour). With exactly one,
    // covering the whole output, the compose is a plain point-sampled copy of it - so that view resolves straight into
    // the output instead (below, "direct resolve").
    const std::size_t unfolded_views =
        static_cast<std::size_t>(std::count(fold_into.begin(), fold_into.end(), SIZE_MAX));
    bool output_resolved = false;
    std::uint64_t draws = 0, skipped = 0, particles = 0, glares = 0, immediates = 0;
    // A draw left out: counted by reason; the first ones of a run are logged so a rare skip can be found.
    const auto skip = [&](SkipReason reason, std::uint32_t draw_index) {
        ++skipped;
        const std::uint64_t total = ++render_stats_.skipped_by_reason[static_cast<std::size_t>(reason)];
        if (render_stats_.skipped_draws_total + skipped <= 16)
            std::fprintf(stderr, "[lifted-render] skipped draw: frame %llu draw %u reason %s (%llu of this reason)\n",
                         static_cast<unsigned long long>(render_stats_.frames + 1), draw_index,
                         skip_reason_name(reason), static_cast<unsigned long long>(total));
    };
    for (std::size_t v = 0; v < packet.views.size(); ++v) {
        const ViewRecord& view = packet.views[v];
        // The view's depth mapping for this frame: its first perspective pass's near and far (add_draw_pass).
        bool view_depth_set = false;
        float view_depth_near = 1.0f, view_depth_far = 10000.0f;
        for (const PassRecord& p : packet.passes) {
            if (p.view == v && p.has_camera && !p.camera.parallel) {
                view_depth_near = p.camera.near_plane;
                view_depth_far = p.camera.far_plane;
                view_depth_set = true;
                break;
            }
        }
        const float x0 = box_x + std::round(view.rect.x * box_w);
        const float y0 = box_y + std::round(view.rect.y * box_h);
        const float x1 = box_x + std::round((view.rect.x + view.rect.width) * box_w);
        const float y1 = box_y + std::round((view.rect.y + view.rect.height) * box_h);
        const auto width = static_cast<std::uint32_t>(std::max(1.0f, x1 - x0));
        const auto height = static_cast<std::uint32_t>(std::max(1.0f, y1 - y0));
        ViewTargets& t = views[v];
        TextureDesc colour;
        colour.width = width;
        colour.height = height;
        colour.format = options_.scene_format;
        colour.samples = msaa_;
        colour.clear_color = {packet.background.x, packet.background.y, packet.background.z, 1.0f};
        TextureDesc depth = colour;
        depth.format = scene_depth_format();
        TextureDesc resolved = colour;
        resolved.samples = 1;
        if (t.colour == kNoResource)  // an overlay's colour is made by the view folded into it
            t.colour = graph.create_texture("view colour", colour);
        t.depth = graph.create_texture("view depth", depth);
        t.viewport = D3D12_VIEWPORT{x0, y0, x1 - x0, y1 - y0, 0.0f, 1.0f};
        t.scissor = D3D12_RECT{static_cast<LONG>(x0), static_cast<LONG>(y0), static_cast<LONG>(x1), static_cast<LONG>(y1)};
        t.view = view_of(view.camera);
        const float view_canvas_aspect =
            canvas_aspect_of(view.canvas, static_cast<float>(width), static_cast<float>(height));
        t.view_projection = multiply(
            projection_of(view.camera, static_cast<float>(width) / height, view_canvas_aspect), t.view);

        // The view's history, when this frame saves or draws it: kept across frames at the view's size (a new size
        // makes a new, invalid one).
        bool history_valid = false;  // as the passes are walked: drawable from here
        if (uses_history[v] != 0u) {
            History& h = histories_[v];
            if (!h.texture || h.width != width || h.height != height) {
                if (h.texture)
                    retire(device_, h.texture, &heap_);
                if (h.rtv.ptr != 0)
                    history_rtvs_.free(h.rtv);
                h = History{};
                TextureInfo info;
                info.width = width;
                info.height = height;
                info.format = options_.scene_format;
                info.flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
                info.initial_state = D3D12_RESOURCE_STATE_RENDER_TARGET;  // discarded on first use (discard_first)
                info.name = L"shadow.view_history";
                if (!create_texture_2d(device_, info, &h.texture, error) ||
                    !create_texture_srv(device_, heap_, h.texture, options_.scene_format, error))
                    return false;
                h.rtv = history_rtvs_.allocate();
                if (h.rtv.ptr == 0)
                    return fail(error, "no render-target view left for a view history");
                device_.d3d()->CreateRenderTargetView(h.texture.resource.Get(), nullptr, h.rtv);
                h.width = width;
                h.height = height;
                h.state = D3D12_RESOURCE_STATE_RENDER_TARGET;
                h.initialised = false;
            }
            ImportedTexture imported;
            imported.resource = h.texture.resource.Get();
            imported.state = h.state;
            imported.final_state = state_for(Access::ShaderRead);
            imported.srv = h.texture.srv;
            imported.rtv = h.rtv;
            imported.desc = resolved;
            imported.discard_first = !h.initialised;
            history_ids[v] = graph.import_texture("view history", imported);
            kept_history_ids[v] = history_ids[v];
            history_valid = h.valid;
        }

        bool first = true;  // the view's scene colour and depth are not cleared yet
        // A view that draws over the views before it (the full-screen overlay of split screen) starts from the picture
        // those views made where it covers them - the one picture the game drew both players' viewports and then its
        // HUD and fades into - on the background elsewhere; its depth starts cleared.
        bool colour_ready = false;
        if (view.over_views && v > 0 && fold_into[v - 1] == v) {
            colour_ready = true;  // the view under it resolved into its colour
            ++render_stats_.views_over_views;
            ++render_stats_.views_folded;
        } else if (view.over_views && v > 0) {
            ID3D12PipelineState* under_pipeline = screen_pipeline(Screen::Under, colour.format, colour.samples, error);
            if (under_pipeline == nullptr)
                return false;
            std::vector<std::size_t> under;
            for (std::size_t u = 0; u < v; ++u) {
                const D3D12_VIEWPORT& w = views[u].viewport;
                if (w.TopLeftX < x1 && w.TopLeftX + w.Width > x0 && w.TopLeftY < y1 && w.TopLeftY + w.Height > y0)
                    under.push_back(u);
            }
            const ResourceId target = t.colour;
            graph.add_pass(
                "views under",
                [&](PassBuilder& p) {
                    for (const std::size_t u : under)
                        p.read(views[u].resolved, Access::ShaderRead);
                    p.write(target, Access::RenderTarget, Load::Clear);
                },
                [&views, under, under_pipeline, target, x0, y0, width, height](PassContext& c) {
                    ID3D12GraphicsCommandList* cl = c.list();
                    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = c.rtv(target);
                    cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
                    cl->SetPipelineState(under_pipeline);
                    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                    const D3D12_RECT whole{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
                    for (const std::size_t u : under) {
                        const D3D12_VIEWPORT& w = views[u].viewport;
                        const D3D12_VIEWPORT at{w.TopLeftX - x0, w.TopLeftY - y0, w.Width, w.Height, 0.0f, 1.0f};
                        c.set_viewport(at, whole);
                        const std::uint32_t constants[2] = {c.srv(views[u].resolved),
                                                            sampler_index(Filter::Point, Address::Clamp, Address::Clamp)};
                        cl->SetGraphicsRoot32BitConstants(kRootConstants, 2, constants, 4);
                        cl->DrawInstanced(3, 1, 0, 0);
                    }
                });
            colour_ready = true;
            ++render_stats_.views_over_views;
        }
        const float aspect = static_cast<float>(width) / static_cast<float>(height);
        // A multisampled target resolved into `into` (single-sampled, render target) by the renderer's own resolve
        // (PSResolve: the mean of the samples, the same bits in every process - the driver's resolve was seen to differ
        // in the last bit between processes, R2_DRIVER.md 4.9); a single-sampled one is copied.
        // `region` (a snapshot's, snapshot_reads.h): only those texels are made - the only ones its readers read.
        // `into_samples`: the target's sample count (a view folded under an overlay resolves into its multisampled
        // colour; the pixel shader's one value lands in every sample).
        auto add_resolve = [&](const char* name, ResourceId from, const TextureDesc& from_desc, ResourceId into,
                               const TexelRect* region = nullptr, std::uint32_t into_samples = 1u,
                               DXGI_FORMAT into_format = DXGI_FORMAT_UNKNOWN, std::uint32_t store16 = 0u,
                               std::uint32_t unorm_table = kNoDescriptor) {
            const bool part = region != nullptr;
            const TexelRect rect = part ? *region : TexelRect{};
            if (from_desc.samples <= 1) {
                graph.add_pass(
                    name,
                    [&](PassBuilder& p) {
                        p.read(from, Access::CopySource);
                        p.write(into, Access::CopyDest, Load::Discard);
                    },
                    [from, into, part, rect](PassContext& c) {
                        if (!part) {
                            c.list()->CopyResource(c.resource(into), c.resource(from));
                            return;
                        }
                        D3D12_TEXTURE_COPY_LOCATION dst{c.resource(into), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
                        D3D12_TEXTURE_COPY_LOCATION src{c.resource(from), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
                        const D3D12_BOX box{static_cast<UINT>(rect.left), static_cast<UINT>(rect.top), 0,
                                            static_cast<UINT>(rect.right), static_cast<UINT>(rect.bottom), 1};
                        c.list()->CopyTextureRegion(&dst, box.left, box.top, 0, &src, &box);
                    });
                return;
            }
            ID3D12PipelineState* pipeline = screen_pipeline(
                Screen::Resolve, into_format != DXGI_FORMAT_UNKNOWN ? into_format : from_desc.format, into_samples,
                nullptr);
            const std::uint32_t samples = from_desc.samples;
            const std::uint32_t low_byte = std::strcmp(name, "resolve") == 0 ? resolve_low_byte() : 0u;
            graph.add_pass(
                name,
                [&](PassBuilder& p) {
                    p.read(from, Access::ShaderRead);
                    p.write(into, Access::RenderTarget, Load::Discard);
                },
                [pipeline, from, into, samples, part, rect, low_byte, store16, unorm_table](PassContext& c) {
                    if (pipeline == nullptr)
                        return;
                    ID3D12GraphicsCommandList* cl = c.list();
                    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = c.rtv(into);
                    cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
                    c.full_viewport(into);
                    if (part) {
                        const D3D12_RECT scissor{rect.left, rect.top, rect.right, rect.bottom};
                        cl->RSSetScissorRects(1, &scissor);
                    }
                    cl->SetPipelineState(pipeline);
                    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                    std::uint32_t root[16] = {};
                    root[4] = c.srv(from);  // root1.x: the multisampled source
                    root[5] = samples;      // root1.y
                    root[12] = low_byte;    // root3.x: diagnostics only
                    root[13] = store16;     // root3.y: straight into the 8-bit output (direct resolve)
                    root[14] = unorm_table; // root3.z: the measured thresholds that direct resolve classifies against
                    cl->SetGraphicsRoot32BitConstants(kRootConstants, 16, root, 0);
                    cl->DrawInstanced(3, 1, 0, 0);
                });
        };
        // A pass's draws into `target`: the scene (depth written as the draws ask), or the glare sources (the scene's
        // depth tested, never written; `glare_tiles`: the tile marks its pixel shaders set, G2).
        std::uint32_t draw_pass_ordinal[2] = {0u, 0u};  // diagnostics: scene / glare-sources passes recorded so far
        auto add_draw_pass = [&](const PassRecord* pass, const char* name, ResourceId target,
                                 const TextureDesc& target_desc, Load target_load, bool glare_sources,
                                 ResourceId glare_tiles = kNoResource) {
            const std::uint32_t pass_ordinal = draw_pass_ordinal[glare_sources ? 1 : 0]++;
            const Load depth_load = first ? Load::Clear : Load::Keep;
            first = false;
            // The pass's own camera, or the view's.
            const Camera& camera = pass != nullptr && pass->has_camera ? pass->camera : view.camera;
            const Mat4 pass_view = view_of(camera);
            // One depth mapping per view and frame. The game re-reads its camera at every bracket, and while it eases
            // its far plane (after a load: ~1.8 units a frame in Final Haunt) the frame's first bracket still carries
            // the previous frame's far plane. Near and far place nothing on screen - they only map eye depth into the
            // depth buffer - so every perspective pass of the view maps depth with the view's first one: a surface the
            // game draws twice (a glow shell over its model, tested greater-or-equal against the depth the model wrote)
            // gets the same depth in both passes instead of one rounding either way of it each frame.
            Camera depth_camera = camera;
            if (!camera.parallel && view_depth_set) {
                depth_camera.near_plane = view_depth_near;
                depth_camera.far_plane = view_depth_far;
            }
            const Mat4 pass_view_projection =
                multiply(projection_of(depth_camera, aspect, view_canvas_aspect), pass_view);
            // The view's canvas for each anchor (canvas draws), and the canvas's own rectangle there: a canvas draw is
            // clipped to it as the console's raster edge clipped the 2D, so art parked outside the canvas stays
            // hidden on a wider view - except a Projected one, which marks the world, and a Continued one, a pattern the
            // layout continued to the view's edges: those are clipped to the view.
            std::array<Mat4, kAnchorCount> canvas_placement;
            std::array<D3D12_RECT, kAnchorCount> canvas_scissor;
            for (std::size_t a = 0; a < canvas_placement.size(); ++a) {
                const Mat4 p = canvas_projection(view.canvas, static_cast<Anchor>(a), static_cast<float>(width),
                                                 static_cast<float>(height));
                canvas_placement[a] = p;
                canvas_scissor[a] = to_rect(shadow::render::canvas_scissor(
                    view.canvas, static_cast<Anchor>(a), p, static_cast<float>(width), static_cast<float>(height)));
            }
            const D3D12_RECT full_scissor{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
            // The images the pass's draws sample besides textures: the frame's snapshots taken so far, the view's
            // history when it holds a picture.
            const SnapshotTable pass_snapshots{&snapshot_ids, &snapshot_order, snapshot_fills};
            const ResourceId history = history_valid ? history_ids[v] : kNoResource;
            // The glare sources test the scene's depth without writing it: a read-only depth view (diagnostics:
            // debug_writable_source_depth binds the writable one, every source draw still writing no depth).
            const bool read_only_depth = glare_sources && !options_.debug_writable_source_depth;
            graph.add_pass(
                name,
                [&](PassBuilder& p) {
                    p.write(target, Access::RenderTarget, target_load);
                    if (glare_tiles != kNoResource)
                        p.write(glare_tiles, Access::UnorderedAccess, Load::Keep);
                    if (read_only_depth)
                        p.read(t.depth, Access::DepthRead);
                    else
                        p.write(t.depth, Access::DepthWrite, depth_load);
                    if (skinning)
                        p.read(skin_id, Access::VertexRead);
                    if (pass == nullptr)
                        return;
                    std::vector<ResourceId> images;
                    for (std::uint32_t d = pass->first_draw; d < pass->first_draw + pass->draw_count; ++d) {
                        const std::uint32_t im = packet.draws[d].immediate;
                        if (im >= packet.immediates.size())
                            continue;
                        const ImmediateDraw& draw = packet.immediates[im];
                        ResourceId image = kNoResource;
                        if (draw.source == ImageSource::Snapshot && draw.snapshot < pass_snapshots.size())
                            image = pass_snapshots[draw.snapshot];
                        else if (draw.source == ImageSource::History)
                            image = history;
                        if (image != kNoResource && std::find(images.begin(), images.end(), image) == images.end())
                            images.push_back(image);
                    }
                    for (const ResourceId image : images)
                        p.read(image, Access::ShaderRead);
                },
                [this, pass, &packet, &t, &draws, &skip, &particles, &immediates, &hud_deferred, target, target_desc,
                 glare_sources,
                 glare_tiles, read_only_depth, &light_blocks, no_lights, pass_view, pass_view_projection, canvas_placement,
                 canvas_scissor, full_scissor, pass_snapshots, history, &skin_element, &skin_origin,
                 pass_grid = scene_tiles(name, pass_ordinal)](PassContext& c) {
                    if (pass == nullptr)
                        return;
                    const TextureDesc& colour = target_desc;
                    ID3D12GraphicsCommandList* cl = c.list();
                    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = c.rtv(target);
                    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = read_only_depth ? c.dsv_read_only(t.depth) : c.dsv(t.depth);
                    cl->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
                    c.full_viewport(target);
                    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                    // G2: the tile variants' b0 (glare_tiles.hlsli); the draw shaders read nothing else from b0.
                    const bool tiles = glare_tiles != kNoResource;
                    if (tiles) {
                        std::uint32_t root[16] = {};
                        root[0] = c.uav(glare_tiles);
                        cl->SetGraphicsRoot32BitConstants(kRootConstants, 16, root, 0);
                    }
                    ID3D12PipelineState* bound = nullptr;
                    // GEKKO_GPU_TRACE_DRAWS (gpu_timer.h): a timestamp either side of every draw, whichever way the
                    // draw leaves the loop body. Measurement only; nothing is recorded without it.
                    GpuTimer* const draw_timer =
                        gpu_timer_ != nullptr && gpu_timer_->draw_detail() ? gpu_timer_.get() : nullptr;
                    // Diagnostics (SHADOW_R2_SCENE_TILES=n, non-shipping): the pass's draws recorded once per tile of an
                    // n x n grid, every scissor intersected with the tile - a measurement of how the pass's cost
                    // depends on its working set (GPU_FILL.md section 12).
                    const std::uint32_t grid = pass_grid;
                    for (std::uint32_t tile_index = 0; tile_index < grid * grid; ++tile_index) {
                    // SHADOW_R2_SCENE_TILE_ALIGN=a (diagnostics): tile sides rounded up to a multiple of a pixels.
                    static const LONG align = [] {
                        const char* v = std::getenv("SHADOW_R2_SCENE_TILE_ALIGN");
                        const long a = v != nullptr ? std::strtol(v, nullptr, 10) : 2L;
                        return a >= 2L ? a : 2L;
                    }();
                    const LONG tw = ((static_cast<LONG>(target_desc.width) + static_cast<LONG>(grid) - 1) / static_cast<LONG>(grid) + align - 1) / align * align;
                    const LONG th = ((static_cast<LONG>(target_desc.height) + static_cast<LONG>(grid) - 1) / static_cast<LONG>(grid) + align - 1) / align * align;
                    const D3D12_RECT tile{static_cast<LONG>(tile_index % grid) * tw, static_cast<LONG>(tile_index / grid) * th,
                                          std::min<LONG>(static_cast<LONG>(tile_index % grid + 1) * tw, static_cast<LONG>(target_desc.width)),
                                          std::min<LONG>(static_cast<LONG>(tile_index / grid + 1) * th, static_cast<LONG>(target_desc.height))};
                    const auto clip = [&tile](const D3D12_RECT& r) {
                        D3D12_RECT o{std::max(r.left, tile.left), std::max(r.top, tile.top), std::min(r.right, tile.right),
                                     std::min(r.bottom, tile.bottom)};
                        if (o.right < o.left) o.right = o.left;
                        if (o.bottom < o.top) o.bottom = o.top;
                        return o;
                    };
                    const D3D12_RECT tile_full = clip(full_scissor);
                    std::array<D3D12_RECT, kAnchorCount> tile_canvas;
                    for (std::size_t a = 0; a < tile_canvas.size(); ++a) tile_canvas[a] = clip(canvas_scissor[a]);
                    cl->RSSetScissorRects(1, &tile_full);
                    // Diagnostics (SHADOW_R2_DRAW_LIMIT=n, non-shipping): each draw pass issues only its first n draws.
                    const std::uint32_t draw_limit = diagnostic_draw_limit();
                    for (std::uint32_t d = pass->first_draw;
                         d < pass->first_draw + std::min(pass->draw_count, draw_limit); ++d) {
                        // The view's trailing 2D under a scaled render size: drawn by the hud overlay instead.
                        if (!hud_deferred.empty() && hud_deferred[d] != 0u)
                            continue;
                        const DrawRecord& draw = packet.draws[d];
                        struct DrawSpanGuard {
                            GpuTimer* timer;
                            ID3D12GraphicsCommandList* list;
                            GpuTimer::DrawNote note;
                            ~DrawSpanGuard() {
                                if (timer != nullptr)
                                    timer->draw_end(list, note);
                            }
                        } draw_span{draw_timer, cl, {}};
                        if (draw_timer != nullptr) {
                            GpuTimer::DrawNote& n = draw_span.note;
                            n.draw = d;
                            n.c = static_cast<std::uint32_t>(draw.recipe.src) |
                                  static_cast<std::uint32_t>(draw.recipe.dst) << 4 |
                                  (draw.recipe.depth_write ? 1u : 0u) << 8 | (draw.recipe.depth_test ? 1u : 0u) << 9 |
                                  static_cast<std::uint32_t>(draw.material.extra.kind) << 10 |
                                  static_cast<std::uint32_t>(draw.layer) << 20;  // the effect layer (cost audit)
                            if (draw.particles != kNoParticles) {
                                n.kind = 1;
                                if (draw.particles < packet.particle_batches.size()) {
                                    n.a = packet.particle_batches[draw.particles].count;
                                    n.b = packet.particle_batches[draw.particles].texture.value;
                                }
                            } else if (draw.immediate != kNoImmediate) {
                                n.kind = 2;
                                if (draw.immediate < packet.immediates.size()) {
                                    const ImmediateDraw& im = packet.immediates[draw.immediate];
                                    n.a = im.vertex_count;
                                    n.b = im.index_count;
                                    n.c |= static_cast<std::uint32_t>(im.source) << 12;
                                }
                            } else {
                                n.a = draw.mesh.value;
                                const MeshSlot* mesh = drawable_mesh(draw.mesh);
                                if (mesh != nullptr && draw.submesh < mesh->submeshes.size())
                                    n.b = mesh->submeshes[draw.submesh].index_count;
                            }
                            draw_timer->draw_begin(cl);
                        }
                        Recipe base_recipe = draw.recipe;
                        if (glare_sources)
                            base_recipe.depth_write = false;
                        // SHADOW_R2_DRAW_LIMIT's bisection: what the last issued draw of each pass is.
                        if (draw_limit != 0xFFFFFFFFu && d + 1u == pass->first_draw + std::min(pass->draw_count, draw_limit)) {
                            const MeshSlot* m = draw.particles == kNoParticles && draw.immediate == kNoImmediate
                                                    ? drawable_mesh(draw.mesh) : nullptr;
                            std::fprintf(stderr,
                                         "[draw-limit] frame %llu pass %s draw %u: kind %s mesh %08X sub %u idx %u "
                                         "verts %u blend %u/%u zw %d zt %d cull %u cutoff %.3f tex %08X flags %X extra %u "
                                         "bones %u lights %u imm-src %u\n",
                                         static_cast<unsigned long long>(packet.tick), glare_sources ? "glare" : "scene",
                                         d - pass->first_draw,
                                         draw.particles != kNoParticles ? "particles"
                                         : draw.immediate != kNoImmediate ? "immediate" : "mesh",
                                         draw.mesh.value, draw.submesh,
                                         m != nullptr && draw.submesh < m->submeshes.size() ? m->submeshes[draw.submesh].index_count : 0u,
                                         m != nullptr ? m->vertex_count : 0u, static_cast<unsigned>(base_recipe.src),
                                         static_cast<unsigned>(base_recipe.dst), base_recipe.depth_write ? 1 : 0,
                                         base_recipe.depth_test ? 1 : 0, static_cast<unsigned>(base_recipe.cull),
                                         base_recipe.alpha_cutoff, draw.material.texture.value, flags_of(draw.material),
                                         static_cast<unsigned>(draw.material.extra.kind), draw.bones.count,
                                         static_cast<unsigned>(draw.lights),
                                         draw.immediate != kNoImmediate && draw.immediate < packet.immediates.size()
                                             ? static_cast<unsigned>(packet.immediates[draw.immediate].source) : 0u);
                        }
                        // Meshes (same diagnostic): 8 = no blending (written opaque), 16 = no texture sample.
                        if (draw.particles == kNoParticles && draw.immediate == kNoImmediate &&
                            (particle_probe() & 8u) != 0u) {
                            base_recipe.src = Blend::One;
                            base_recipe.dst = Blend::Zero;
                        }
                        if (draw.particles != kNoParticles && particle_probe() != 0u) {
                            if ((particle_probe() & 2u) != 0u) {
                                base_recipe.src = Blend::One;
                                base_recipe.dst = Blend::Zero;
                            }
                            if ((particle_probe() & 4u) != 0u)
                                base_recipe.depth_test = false;
                        }
                        if (draw.particles != kNoParticles) {
                            // A particle batch: its records as a per-instance stream, six vertices per particle.
                            const ParticleBatch* batch = draw.particles < packet.particle_batches.size()
                                                             ? &packet.particle_batches[draw.particles]
                                                             : nullptr;
                            ID3D12PipelineState* pipeline =
                                mesh_pipeline(Program::Particles, base_recipe, colour.format, colour.samples, nullptr,
                                              false, tiles);
                            if (batch == nullptr || batch->count == 0 ||
                                std::size_t{batch->first} + batch->count > packet.particles.size() || pipeline == nullptr) {
                                skip(pipeline == nullptr ? SkipReason::ParticlePipeline : SkipReason::ParticleBatch, d);
                                continue;
                            }
                            const std::uint64_t bytes = std::uint64_t{batch->count} * sizeof(Particle);
                            const UploadAllocation records = frames_.ring().allocate(bytes, 16);
                            const UploadAllocation block = frames_.ring().allocate(sizeof(ParticleConstants), 256);
                            if (!records || !block) {
                                skip(SkipReason::ParticleUpload, d);
                                continue;
                            }
                            std::memcpy(records.cpu, &packet.particles[batch->first], bytes);
                            ParticleConstants constants{};
                            store_column_major(pass_view_projection, constants.view_projection);
                            constants.right[0] = batch->right.x;
                            constants.right[1] = batch->right.y;
                            constants.right[2] = batch->right.z;
                            constants.up[0] = batch->up.x;
                            constants.up[1] = batch->up.y;
                            constants.up[2] = batch->up.z;
                            constants.colour[0] = batch->colour.x;
                            constants.colour[1] = batch->colour.y;
                            constants.colour[2] = batch->colour.z;
                            constants.colour[3] = batch->colour.w;
                            const TextureSlot* texture = drawable_texture(batch->texture);
                            constants.texture = texture != nullptr && (particle_probe() & 1u) == 0u
                                                    ? texture->texture.srv
                                                    : kNoDescriptor;
                            constants.sampler =
                                sampler_index(batch->sampler.filter, batch->sampler.u, batch->sampler.v);
                            constants.flags =
                                (batch->particle_colours ? kParticleColours : 0u) | skip_clear_flag(base_recipe);
                            constants.cutoff = draw.recipe.alpha_cutoff > 0.0f ? draw.recipe.alpha_cutoff : -1.0f;
                            constants.fog_enabled = draw.recipe.fog ? 1u : 0u;
                            constants.fog_start = draw.recipe.fog_start;
                            constants.fog_end = draw.recipe.fog_end;
                            constants.fog_colour = packed(draw.recipe.fog_colour);
                            std::memcpy(block.cpu, &constants, sizeof(constants));
                            dump_particle_draw(packet.tick, glare_sources, d - pass->first_draw, colour, base_recipe,
                                               *batch, &packet.particles[batch->first], pass_view_projection);
                            if (pipeline != bound) {
                                cl->SetPipelineState(pipeline);
                                bound = pipeline;
                            }
                            const D3D12_VERTEX_BUFFER_VIEW stream{records.gpu, static_cast<UINT>(bytes),
                                                                  static_cast<UINT>(sizeof(Particle))};
                            cl->IASetVertexBuffers(0, 1, &stream);
                            cl->SetGraphicsRootConstantBufferView(kRootFrameConstants, block.gpu);
                            cl->DrawInstanced(6, batch->count, 0, 0);
                            ++draws;
                            particles += batch->count;
                            continue;
                        }
                        if (draw.immediate != kNoImmediate && (particle_probe() & 32u) != 0u) {
                            base_recipe.src = Blend::One;  // diagnostics: immediates unblended
                            base_recipe.dst = Blend::Zero;
                        }
                        if (draw.immediate != kNoImmediate) {
                            // Immediate geometry: its vertices (and indices) streamed through the frame ring.
                            const ImmediateDraw* im = draw.immediate < packet.immediates.size()
                                                          ? &packet.immediates[draw.immediate]
                                                          : nullptr;
                            const bool lines = im != nullptr && (im->topology == Topology::Lines ||
                                                                 im->topology == Topology::LineStrip);
                            ID3D12PipelineState* pipeline = mesh_pipeline(Program::Immediate, base_recipe,
                                                                          colour.format, colour.samples, nullptr, lines,
                                                                          tiles);
                            if (im == nullptr || pipeline == nullptr || im->vertex_count == 0 ||
                                std::size_t{im->first_vertex} + im->vertex_count > packet.immediate_vertices.size() ||
                                std::size_t{im->first_index} + im->index_count > packet.immediate_indices.size()) {
                                skip(im != nullptr && pipeline == nullptr ? SkipReason::ImmediatePipeline
                                                                          : SkipReason::ImmediateInvalid, d);
                                continue;
                            }
                            // The image it samples: a texture, a snapshot taken earlier in the frame, the view's
                            // history; a snapshot or history that does not exist leaves the draw out (counted).
                            std::uint32_t image = kNoDescriptor;
                            if (im->source == ImageSource::Texture) {
                                const TextureSlot* texture = drawable_texture(im->texture);
                                image = texture != nullptr ? texture->texture.srv : kNoDescriptor;
                            } else {
                                const ResourceId source = im->source == ImageSource::History ? history
                                                          : im->snapshot < pass_snapshots.size()
                                                              ? pass_snapshots[im->snapshot]
                                                              : kNoResource;
                                if (source == kNoResource) {
                                    skip(SkipReason::ImmediateImage, d);
                                    ++render_stats_.image_misses;
                                    continue;
                                }
                                image = c.srv(source);
                            }
                            const std::uint64_t vertex_bytes = std::uint64_t{im->vertex_count} * sizeof(ImmediateVertex);
                            const std::uint64_t index_bytes = std::uint64_t{im->index_count} * sizeof(std::uint32_t);
                            const UploadAllocation vertices = frames_.ring().allocate(vertex_bytes, 16);
                            const UploadAllocation indices =
                                im->index_count != 0 ? frames_.ring().allocate(index_bytes, 16) : UploadAllocation{};
                            const UploadAllocation block = frames_.ring().allocate(sizeof(ImmediateConstants), 256);
                            if (!vertices || !block || (im->index_count != 0 && !indices)) {
                                skip(SkipReason::ImmediateUpload, d);
                                continue;
                            }
                            std::memcpy(vertices.cpu, &packet.immediate_vertices[im->first_vertex], vertex_bytes);
                            if (im->index_count != 0)
                                std::memcpy(indices.cpu, &packet.immediate_indices[im->first_index], index_bytes);
                            ImmediateConstants constants{};
                            // A canvas draw laid out in a region of the canvas (a player's viewport in split
                            // screen) is placed and clipped in that region; any other by its anchor alone.
                            Mat4 region_placement;
                            PixelBox region_box;
                            const bool in_region = im->space == Space::Canvas && im->region != kNoRegion &&
                                                   im->region < packet.canvas_regions.size();
                            if (in_region)
                                region_projection(packet.views[pass->view].canvas, packet.canvas_regions[im->region],
                                                  im->anchor, static_cast<float>(colour.width),
                                                  static_cast<float>(colour.height), &region_placement,
                                                  &region_box);
                            const D3D12_RECT region_scissor = to_rect(region_box);
                            const Mat4& placement = in_region ? region_placement
                                                    : im->space == Space::Canvas
                                                        ? canvas_placement[static_cast<std::size_t>(im->anchor)]
                                                        : pass_view_projection;
                            store_column_major(multiply(placement, from3x4(draw.world)),
                                               constants.world_view_projection);
                            // Texture coordinates: as given, or - a canvas draw of a screen image - canvas points
                            // placed like the positions.
                            constants.uv_row[0][0] = 1.0f;
                            constants.uv_row[1][1] = 1.0f;
                            if (im->space == Space::Canvas && im->source != ImageSource::Texture) {
                                const auto rows = canvas_image_rows(placement);
                                for (int r = 0; r < 2; ++r)
                                    for (int k = 0; k < 4; ++k)
                                        constants.uv_row[r][k] = rows[r][k];
                            }
                            constants.texture = image;
                            constants.sampler = sampler_index(im->sampler.filter, im->sampler.u, im->sampler.v);
                            constants.cutoff = base_recipe.alpha_cutoff > 0.0f ? base_recipe.alpha_cutoff : -1.0f;
                            constants.flags = skip_clear_flag(base_recipe);
                            constants.fog_enabled = base_recipe.fog ? 1u : 0u;
                            constants.fog_start = base_recipe.fog_start;
                            constants.fog_end = base_recipe.fog_end;
                            constants.fog_colour = packed(base_recipe.fog_colour);
                            std::memcpy(block.cpu, &constants, sizeof(constants));
                            if (pipeline != bound) {
                                cl->SetPipelineState(pipeline);
                                bound = pipeline;
                            }
                            cl->IASetPrimitiveTopology(to_d3d(im->topology));
                            const D3D12_VERTEX_BUFFER_VIEW stream{vertices.gpu, static_cast<UINT>(vertex_bytes),
                                                                  static_cast<UINT>(sizeof(ImmediateVertex))};
                            cl->IASetVertexBuffers(0, 1, &stream);
                            cl->SetGraphicsRootConstantBufferView(kRootFrameConstants, block.gpu);
                            const bool on_canvas = im->space == Space::Canvas;
                            const D3D12_RECT tile_region = clip(region_scissor);
                            if (on_canvas)
                                cl->RSSetScissorRects(1, in_region ? &tile_region
                                                                   : &tile_canvas[static_cast<std::size_t>(im->anchor)]);
                            if (im->index_count != 0) {
                                const D3D12_INDEX_BUFFER_VIEW ibv{indices.gpu, static_cast<UINT>(index_bytes),
                                                                  DXGI_FORMAT_R32_UINT};
                                cl->IASetIndexBuffer(&ibv);
                                cl->DrawIndexedInstanced(im->index_count, 1, 0, 0, 0);
                            } else {
                                cl->DrawInstanced(im->vertex_count, 1, 0, 0);
                            }
                            if (on_canvas)
                                cl->RSSetScissorRects(1, &tile_full);
                            cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                            ++draws;
                            ++immediates;
                            continue;
                        }
                        const MeshSlot* mesh = drawable_mesh(draw.mesh);
                        if (mesh == nullptr || draw.submesh >= mesh->submeshes.size()) {
                            skip(SkipReason::MeshMissing, d);
                            continue;
                        }
                        const assets::MeshData::Submesh& sub = mesh->submeshes[draw.submesh];
                        if (sub.index_count == 0)
                            continue;
                        const std::uint32_t skinned = skin_element[d];
                        if (draw.bones.count != 0u && skinned == kNotSkinned) {
                            skip(SkipReason::SkinMissing, d);
                            continue;
                        }
                        const Material& material = draw.material;
                        DrawConstants constants{};
                        // Skinned positions are world space less the pose's origin, which the world matrix puts back;
                        // a morphed shape alone is still in model space.
                        Float3x4 world3 = draw.world;
                        if (skinned != kNotSkinned && draw.bones.count != 0u) {
                            world3 = Float3x4{};
                            for (int row = 0; row < 3; ++row)
                                world3.m[row][3] = skin_origin[d][row];
                        }
                        const Mat4 world = from3x4(world3);
                        const Mat4 wvp = multiply(pass_view_projection, world);
                        const Mat4 world_view = multiply(pass_view, world);
                        for (int col = 0; col < 4; ++col)
                            for (int row = 0; row < 4; ++row)
                                constants.world_view_projection[col * 4 + row] = wvp.m[row][col];
                        for (int row = 0; row < 3; ++row)
                            for (int col = 0; col < 4; ++col) {
                                constants.world_row[row][col] = world3.m[row][col];
                                constants.view_row[row][col] = col < 3 ? world_view.m[row][col] : 0.0f;
                            }
                        constants.colour[0] = material.colour.x;
                        constants.colour[1] = material.colour.y;
                        constants.colour[2] = material.colour.z;
                        constants.colour[3] = material.colour.w;
                        for (int row = 0; row < 2; ++row)
                            for (int col = 0; col < 3; ++col)
                                constants.uv_row[row][col] = material.uv_transform[row][col];
                        const TextureSlot* texture = drawable_texture(material.texture);
                        constants.texture =
                            texture != nullptr && (particle_probe() & 16u) == 0u ? texture->texture.srv : kNoDescriptor;
                        constants.sampler = sampler_index(material.sampler.filter, material.sampler.u, material.sampler.v);
                        constants.flags = flags_of(material) | skip_clear_flag(base_recipe);
                        constants.uv_set = 0;
                        constants.cutoff = draw.recipe.alpha_cutoff > 0.0f ? draw.recipe.alpha_cutoff : -1.0f;
                        constants.ka = material.ka;
                        constants.coefficient = 0.0f;
                        constants.fog_enabled = draw.recipe.fog ? 1u : 0u;
                        constants.fog_start = draw.recipe.fog_start;
                        constants.fog_end = draw.recipe.fog_end;
                        constants.fog_colour = packed(draw.recipe.fog_colour);
                        constants.dissolve = material.dissolve;
                        const D3D12_GPU_VIRTUAL_ADDRESS lights =
                            draw.lights < light_blocks.size() ? light_blocks[draw.lights] : no_lights;

                        static_assert(sizeof(SkinnedVertex) == sizeof(PositionVertex), "stream 0 from the skin cache");
                        D3D12_VERTEX_BUFFER_VIEW streams[2] = {
                            {skinned != kNotSkinned ? skin_cache_.element_address(skinned) : mesh->streams.gpu(),
                             static_cast<UINT>(mesh->vertex_count * sizeof(PositionVertex)),
                             static_cast<UINT>(sizeof(PositionVertex))},
                            {mesh->streams.gpu() + mesh->attributes_offset,
                             static_cast<UINT>(mesh->vertex_count * sizeof(AttributeVertex)),
                             static_cast<UINT>(sizeof(AttributeVertex))}};
                        const D3D12_INDEX_BUFFER_VIEW ibv{mesh->streams.gpu() + mesh->indices_offset,
                                                          static_cast<UINT>(mesh->indices_bytes), DXGI_FORMAT_R32_UINT};
                        cl->IASetVertexBuffers(0, 2, streams);
                        cl->IASetIndexBuffer(&ibv);
                        cl->SetGraphicsRootConstantBufferView(kRootPassConstants, lights);

                        auto issue = [&](Program program, const Recipe& recipe, const DrawConstants& block_data,
                                         bool plain = false) {
                            ID3D12PipelineState* pipeline = mesh_pipeline(program, recipe, colour.format, colour.samples,
                                                                          nullptr, false, tiles, plain);
                            const UploadAllocation block = frames_.ring().allocate(sizeof(DrawConstants), 256);
                            if (pipeline == nullptr || !block) {
                                skip(pipeline == nullptr ? SkipReason::MeshPipeline : SkipReason::MeshUpload, d);
                                return;
                            }
                            if (pipeline != bound) {
                                cl->SetPipelineState(pipeline);
                                bound = pipeline;
                            }
                            std::memcpy(block.cpu, &block_data, sizeof(block_data));
                            cl->SetGraphicsRootConstantBufferView(kRootFrameConstants, block.gpu);
                            cl->DrawIndexedInstanced(sub.index_count, 1, sub.first_index, 0, 0);
                            ++draws;
                        };
                        // The exact opaque write: the base pass's alpha is exactly 1 in every fragment when the
                        // material colour's alpha (M) is 1 or absent, every vertex alpha (P) is 255 and the texture is
                        // absent or opaque (alpha = C0.a x v.a x texel alpha, shaders/mesh.hlsl).
                        bool plain = false;
                        const bool alpha_is_c0 = (!material.vertex_colours || mesh->opaque_colours) &&
                                                 (constants.texture == kNoDescriptor || texture->texture.opaque);
                        if (alpha_is_c0 && exact_alpha_enabled())
                            constants.flags |= kExactAlpha;
                        if (exact_opaque_enabled()) {
                            const bool alpha_one = alpha_is_c0 && exact_alpha_enabled() &&
                                                   (!material.modulate || material.colour.w == 1.0f);
                            plain = identity_always(base_recipe) || (alpha_one && identity_when_opaque(base_recipe));
                            if (plain && opaque_check_enabled() != 0u)
                                constants.flags |= opaque_check_enabled();
                        }
                        if (plain)
                            draw_span.note.c |= 1u << 16;  // the per-draw trace: drawn by the exact opaque write
                        const ExtraPass& extra = material.extra;
                        const TextureSlot* extra_texture = drawable_texture(extra.texture);
                        if (extra.kind == ExtraPass::Kind::BumpEnvironment && extra_texture != nullptr &&
                            (particle_probe() & 64u) == 0u)
                            fill_bump_environment(extra, extra_texture->texture.srv,
                                                  sampler_index(extra.sampler.filter, extra.sampler.u, extra.sampler.v),
                                                  &constants);
                        issue(Program::Base, base_recipe, constants, plain);
                        constants.flags &= ~(kOpaqueCheck | 256u | 512u | kExactAlpha);

                        // The second draw of the same mesh: same depth test, no depth writes, its own blend.
                        if ((extra.kind == ExtraPass::Kind::Environment || extra.kind == ExtraPass::Kind::Dual) &&
                            extra_texture != nullptr && (particle_probe() & 64u) == 0u) {
                            Recipe recipe = base_recipe;
                            recipe.depth_write = false;
                            DrawConstants second = constants;
                            second.texture = extra_texture->texture.srv;
                            second.sampler = sampler_index(extra.sampler.filter, extra.sampler.u, extra.sampler.v);
                            if (extra.kind == ExtraPass::Kind::Environment) {
                                recipe.src = Blend::One;
                                recipe.dst = Blend::One;
                                second.coefficient = extra.coefficient;
                                second.cutoff = -1.0f;
                                issue(Program::Environment, recipe, second);
                            } else {
                                recipe.src = extra.src;
                                recipe.dst = extra.dst;
                                second.flags = (constants.flags & ~kSkipClear) | skip_clear_flag(recipe);
                                second.uv_set = extra.second_uv_set ? 1u : 0u;
                                for (int row = 0; row < 2; ++row)
                                    for (int col = 0; col < 3; ++col)
                                        second.uv_row[row][col] = extra.uv_transform[row][col];
                                second.cutoff = extra.alpha_cutoff > 0.0f ? extra.alpha_cutoff : -1.0f;
                                issue(Program::Dual, recipe, second);
                            }
                        }
                    }
                    }

                });
        };
        auto add_scene_pass = [&](const PassRecord* pass) {
            add_draw_pass(pass, "scene", t.colour, colour, first && !colour_ready ? Load::Clear : Load::Keep, false);
        };

        // The glare (shaders/glare.hlsl; BLOOM_SPEC 6.1, 6.4): one full-screen pass per step into a new graph texture.
        TextureDesc sources_desc = colour;
        sources_desc.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sources_desc.clear_color = {0.0f, 0.0f, 0.0f, 0.0f};
        const auto even = [](double x) { return static_cast<std::uint32_t>(std::max(2L, 2L * std::lround(x / 2.0))); };
        TextureDesc level_a;
        level_a.width = even(96.0 * width / height);
        level_a.height = 128;
        level_a.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        TextureDesc level_b = level_a;
        level_b.width = level_a.width / 2;
        level_b.height = level_a.height / 2;
        const std::uint32_t linear_clamp = sampler_index(Filter::Linear, Address::Clamp, Address::Clamp);
        // One step: `screen` writes `out` from `inputs` (read as shader resources) with root constants `root`.
        auto add_screen_pass = [&](const char* name, Screen screen, const TextureDesc& out_desc,
                                   std::initializer_list<ResourceId> inputs, auto fill_root) {
            const ResourceId out = graph.create_texture(name, out_desc);
            const std::vector<ResourceId> reads(inputs);
            ID3D12PipelineState* pipeline = screen_pipeline(screen, out_desc.format, out_desc.samples, nullptr);
            graph.add_pass(
                name,
                [&](PassBuilder& p) {
                    for (const ResourceId input : reads)
                        p.read(input, Access::ShaderRead);
                    p.write(out, Access::RenderTarget, Load::Discard);
                },
                [pipeline, out, reads, fill_root](PassContext& c) {
                    if (pipeline == nullptr)
                        return;
                    ID3D12GraphicsCommandList* cl = c.list();
                    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = c.rtv(out);
                    cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
                    c.full_viewport(out);
                    cl->SetPipelineState(pipeline);
                    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                    std::uint32_t root[16] = {};
                    fill_root(c, reads, root);
                    cl->SetGraphicsRoot32BitConstants(kRootConstants, 16, root, 0);
                    cl->DrawInstanced(3, 1, 0, 0);
                });
            return out;
        };
        // The root constants are filled when the graph executes, after this view's locals are gone: every value a
        // filler uses is captured as a copy.
        const auto float_bits = [](float f) {
            std::uint32_t u;
            std::memcpy(&u, &f, sizeof(u));
            return u;
        };
        // A level captured from `source` (full size): box-downsampled to `level` times `gain`.
        auto capture_level = [&](ResourceId source, const TextureDesc& level, float gain) {
            const std::uint32_t sizes[4] = {width, height, level.width, level.height};
            const std::uint32_t gain_bits = float_bits(gain);
            return add_screen_pass("glare capture", Screen::GlareCapture, level, {source},
                                   [sizes0 = sizes[0], sizes1 = sizes[1], sizes2 = sizes[2], sizes3 = sizes[3],
                                    gain_bits](PassContext& c, const std::vector<ResourceId>& in, std::uint32_t* r) {
                                       r[0] = c.srv(in[0]);
                                       r[4] = sizes0;
                                       r[5] = sizes1;
                                       r[6] = sizes2;
                                       r[7] = sizes3;
                                       r[8] = gain_bits;
                                   });
        };
        // Level A captured by compute (GLARE_OPTIMIZATION.md G1): CSGlareRows sums each resolved source row's share of
        // every level column, CSGlareColumns the rows' shares into the level - capture_level's integral in exact
        // integer arithmetic, read by neighbouring threads from neighbouring pixels. Its bounds
        // (compute_capture_fits): a group's 256 threads hold its columns' span, and the integer sum stays below 2^32.
        auto capture_level_compute = [&](ResourceId resolved, const TextureDesc& level, float gain, ResourceId tiles) {
            TextureDesc partial_desc;
            partial_desc.width = level.width;
            partial_desc.height = height;
            partial_desc.format = DXGI_FORMAT_R32G32B32A32_UINT;
            partial_desc.unordered_access = true;
            const ResourceId partial = graph.create_texture("glare row sums", partial_desc);
            TextureDesc level_desc = level;
            level_desc.unordered_access = true;
            const ResourceId out = graph.create_texture("glare capture", level_desc);
            ID3D12PipelineState* rows = compute_pipeline(Compute::GlareRows, nullptr);
            ID3D12PipelineState* columns = compute_pipeline(Compute::GlareColumns, nullptr);
            const std::uint32_t w = width, h = height, lw = level.width, lh = level.height;
            const std::uint32_t gain_bits = float_bits(gain);
            // The level columns whose span (with a partial pixel at each end) fits a CSGlareRows group's 256 threads.
            const std::uint32_t per_group = std::max(1u, 254u * lw / w);
            graph.add_pass(
                "glare rows",
                [&](PassBuilder& p) {
                    p.read(resolved, Access::ShaderRead);
                    if (tiles != kNoResource)
                        p.read(tiles, Access::ShaderRead);
                    p.write(partial, Access::UnorderedAccess, Load::Discard);
                },
                [rows, resolved, tiles, partial, w, h, lw, lh, per_group](PassContext& c) {
                    if (rows == nullptr)
                        return;
                    ID3D12GraphicsCommandList* cl = c.list();
                    cl->SetPipelineState(rows);
                    std::uint32_t root[16] = {};
                    root[0] = c.srv(resolved);
                    root[1] = tiles != kNoResource ? c.srv(tiles) : 0xFFFFFFFFu;
                    root[2] = c.uav(partial);
                    root[4] = w;
                    root[5] = h;
                    root[6] = lw;
                    root[7] = lh;
                    root[8] = per_group;
                    cl->SetComputeRoot32BitConstants(kRootConstants, 16, root, 0);
                    cl->Dispatch((lw + per_group - 1) / per_group, (h + 15) / 16, 1);  // CSGlareRows: 16 rows a group
                });
            graph.add_pass(
                "glare columns",
                [&](PassBuilder& p) {
                    p.read(partial, Access::ShaderRead);
                    p.write(out, Access::UnorderedAccess, Load::Discard);
                },
                [columns, partial, out, w, h, lw, lh, gain_bits](PassContext& c) {
                    if (columns == nullptr)
                        return;
                    ID3D12GraphicsCommandList* cl = c.list();
                    cl->SetPipelineState(columns);
                    std::uint32_t root[16] = {};
                    root[0] = c.srv(partial);
                    root[1] = c.uav(out);
                    root[4] = w;
                    root[5] = h;
                    root[6] = lw;
                    root[7] = lh;
                    root[8] = gain_bits;
                    cl->SetComputeRoot32BitConstants(kRootConstants, 16, root, 0);
                    cl->Dispatch((lw + 7) / 8, (lh + 7) / 8, 1);
                });
            return out;
        };
        // One half of a blur: the sum of 1 + taps taps along an axis (no taps: the half does not run).
        auto blur = [&](ResourceId level_id, const TextureDesc& level, std::uint32_t taps, float spread,
                        std::uint32_t axis) {
            if (taps == 0)
                return level_id;
            const std::uint32_t level_width = level.width, level_height = level.height;
            const std::uint32_t spread_bits = float_bits(spread);
            return add_screen_pass(axis == 0 ? "glare blur x" : "glare blur y", Screen::GlareBlur, level, {level_id},
                                   [level_width, level_height, taps, spread_bits, axis](
                                       PassContext& c, const std::vector<ResourceId>& in, std::uint32_t* r) {
                                       r[0] = c.srv(in[0]);
                                       r[4] = level_width;
                                       r[5] = level_height;
                                       r[8] = taps;
                                       r[9] = spread_bits;
                                       r[10] = axis;
                                   });
        };
        // The glare composite folded into the view's resolve (B3a, OPTIMIZATION_RESULTS.md thirtieth increment): when
        // nothing but canvas draws follows the view's glare, the composite's ONE / ONE blend into every sample of the
        // multisampled scene is applied only where those draws can land (`spans`: disjoint pixel rows of 16-pixel
        // tiles, the blend before the draws as the game ordered it), and the view's resolve adds the glare per sample
        // everywhere else with the blend's own arithmetic (glare.hlsl PSResolveGlare). The same pixels, without
        // reading and writing every sample of the scene a second time.
        struct FoldedGlare {
            ResourceId sources = kNoResource, first = kNoResource, second = kNoResource;
            bool active = false;
            std::vector<D3D12_RECT> spans;
        } folded;
        // The pixel rows of tiles the view's draws after pass `glare_pass` can touch, or false when one of those passes
        // is anything but canvas draws (then the composite is drawn as it is).
        auto later_canvas_spans = [&](std::size_t glare_pass, std::vector<D3D12_RECT>* spans) {
            constexpr std::uint32_t kTile = 16;
            const std::uint32_t tiles_x = (width + kTile - 1) / kTile, tiles_y = (height + kTile - 1) / kTile;
            std::vector<std::uint8_t> marked(std::size_t{tiles_x} * tiles_y, 0);
            for (std::size_t q = glare_pass + 1; q < packet.passes.size(); ++q) {
                const PassRecord& later = packet.passes[q];
                if (later.view != v)
                    continue;
                if (later.kind != PassKind::Scene)
                    return false;
                for (std::uint32_t d = later.first_draw; d < later.first_draw + later.draw_count; ++d) {
                    if (d >= packet.draws.size())
                        return false;
                    const DrawRecord& draw = packet.draws[d];
                    if (draw.immediate >= packet.immediates.size())
                        return false;
                    const ImmediateDraw& im = packet.immediates[draw.immediate];
                    if (im.space != Space::Canvas ||
                        std::size_t{im.first_vertex} + im.vertex_count > packet.immediate_vertices.size())
                        return false;
                    Mat4 placement;
                    PixelBox unused;
                    if (im.region != kNoRegion && im.region < packet.canvas_regions.size())
                        region_projection(view.canvas, packet.canvas_regions[im.region], im.anchor,
                                          static_cast<float>(width), static_cast<float>(height), &placement, &unused);
                    else
                        placement = canvas_projection(view.canvas, im.anchor, static_cast<float>(width),
                                                      static_cast<float>(height));
                    const Mat4 m = multiply(placement, from3x4(draw.world));
                    double x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30;
                    for (std::uint32_t k = 0; k < im.vertex_count; ++k) {
                        const Float3& p = packet.immediate_vertices[im.first_vertex + k].position;
                        double c[4];
                        for (int r = 0; r < 4; ++r)
                            c[r] = static_cast<double>(m.m[r][0]) * p.x + static_cast<double>(m.m[r][1]) * p.y +
                                   static_cast<double>(m.m[r][2]) * p.z + m.m[r][3];
                        if (!(c[3] > 0.0) || !std::isfinite(c[0]) || !std::isfinite(c[1]))
                            return false;
                        const double px = (c[0] / c[3] + 1.0) * 0.5 * width;
                        const double py = (1.0 - c[1] / c[3]) * 0.5 * height;
                        x0 = std::min(x0, px);
                        x1 = std::max(x1, px);
                        y0 = std::min(y0, py);
                        y1 = std::max(y1, py);
                    }
                    if (im.vertex_count == 0)
                        continue;
                    // A pixel's samples lie inside it: one pixel of margin on every side covers every sample a
                    // primitive inside these bounds can reach (and a line's width).
                    // Clamped before the conversion: a far vertex (a fade quad's) is outside long's range, and the
                    // converted bound wrapped and skipped the draw's tiles. Within the view nothing changes.
                    const auto clamp_x = [&](double v) { return std::clamp(v, -2.0, static_cast<double>(width) + 2.0); };
                    const auto clamp_y = [&](double v) { return std::clamp(v, -2.0, static_cast<double>(height) + 2.0); };
                    const long left = std::max(0L, static_cast<long>(std::floor(clamp_x(x0))) - 1);
                    const long top = std::max(0L, static_cast<long>(std::floor(clamp_y(y0))) - 1);
                    const long right = std::min(static_cast<long>(width), static_cast<long>(std::ceil(clamp_x(x1))) + 1);
                    const long bottom = std::min(static_cast<long>(height), static_cast<long>(std::ceil(clamp_y(y1))) + 1);
                    if (left >= right || top >= bottom)
                        continue;
                    for (long ty = top / kTile; ty <= (bottom - 1) / static_cast<long>(kTile); ++ty)
                        for (long tx = left / kTile; tx <= (right - 1) / static_cast<long>(kTile); ++tx)
                            marked[static_cast<std::size_t>(ty) * tiles_x + static_cast<std::size_t>(tx)] = 1;
                }
            }
            for (std::uint32_t ty = 0; ty < tiles_y; ++ty)
                for (std::uint32_t tx = 0; tx < tiles_x;) {
                    if (marked[std::size_t{ty} * tiles_x + tx] == 0) {
                        ++tx;
                        continue;
                    }
                    std::uint32_t end = tx;
                    while (end < tiles_x && marked[std::size_t{ty} * tiles_x + end] != 0)
                        ++end;
                    spans->push_back(D3D12_RECT{static_cast<LONG>(tx * kTile), static_cast<LONG>(ty * kTile),
                                                static_cast<LONG>(std::min(end * kTile, width)),
                                                static_cast<LONG>(std::min((ty + 1) * kTile, height))});
                    tx = end;
                }
            return true;
        };
        // G2 (GLARE_OPTIMIZATION.md): the glare sources' tile marks - one per 16 x 16 tile, cleared here before the
        // view's first glare-sources pass, set by the pass's pixel shaders (glare_tiles.hlsli).
        const std::uint32_t tile_columns = (width + 15) / 16, tile_rows = (height + 15) / 16;
        auto add_tiles_clear = [&]() {
            TextureDesc tiles_desc;
            tiles_desc.width = tile_columns;
            tiles_desc.height = tile_rows;
            tiles_desc.format = DXGI_FORMAT_R32_UINT;
            tiles_desc.unordered_access = true;
            const ResourceId tiles = graph.create_texture("glare tiles", tiles_desc);
            ID3D12PipelineState* clear = compute_pipeline(Compute::GlareTilesClear, nullptr);
            graph.add_pass(
                "glare tiles clear", [&](PassBuilder& p) { p.write(tiles, Access::UnorderedAccess, Load::Discard); },
                [clear, tiles, tile_columns, tile_rows](PassContext& c) {
                    if (clear == nullptr)
                        return;
                    ID3D12GraphicsCommandList* cl = c.list();
                    cl->SetPipelineState(clear);
                    std::uint32_t root[16] = {};
                    root[0] = c.uav(tiles);
                    root[1] = tile_columns;
                    root[2] = tile_rows;
                    cl->SetComputeRoot32BitConstants(kRootConstants, 16, root, 0);
                    cl->Dispatch((tile_columns + 7) / 8, (tile_rows + 7) / 8, 1);
                });
            return tiles;
        };
        // The sources resolved on their marked tiles only (VSGlareTiles + PSResolve): the target is cleared to 0, which
        // is what the full resolve writes on a tile no source pixel reached (it holds the sources' clear, black).
        auto add_tile_resolve = [&](ResourceId from, const TextureDesc& from_desc, ResourceId tiles, ResourceId into) {
            ID3D12PipelineState* pipeline = screen_pipeline(Screen::ResolveTiles, from_desc.format, 1, nullptr);
            const std::uint32_t samples = from_desc.samples, w = width, h = height;
            graph.add_pass(
                "glare resolve",
                [&](PassBuilder& p) {
                    p.read(from, Access::ShaderRead);
                    p.read(tiles, Access::ShaderRead);
                    p.write(into, Access::RenderTarget, Load::Clear);
                },
                [pipeline, from, tiles, into, samples, w, h, tile_columns, tile_rows](PassContext& c) {
                    if (pipeline == nullptr)
                        return;
                    ID3D12GraphicsCommandList* cl = c.list();
                    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = c.rtv(into);
                    cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
                    c.full_viewport(into);
                    cl->SetPipelineState(pipeline);
                    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                    std::uint32_t root[16] = {};
                    root[0] = c.srv(tiles);   // root0: VSGlareTiles
                    root[1] = tile_columns;
                    root[2] = w;
                    root[3] = h;
                    root[4] = c.srv(from);    // root1: PSResolve
                    root[5] = samples;
                    cl->SetGraphicsRoot32BitConstants(kRootConstants, 16, root, 0);
                    cl->DrawInstanced(6, tile_columns * tile_rows, 0, 0);
                });
        };
        auto add_glare = [&](const GlareParams& g, ResourceId sources_ms, std::size_t glare_pass, ResourceId tiles) {
            // The sources, resolved: the captures read them, the composite adds them.
            TextureDesc single = sources_desc;
            single.samples = 1;
            const ResourceId sources = graph.create_texture("glare sources resolved", single);
            if (tiles != kNoResource)
                add_tile_resolve(sources_ms, sources_desc, tiles, sources);
            else
                add_resolve("glare resolve", sources_ms, sources_desc, sources);
            // G1: level A by the exact compute capture, inside its bounds.
            const bool compute_capture_fits =
                width <= 254u * level_a.width && std::uint64_t{width} * height * 255u < (std::uint64_t{1} << 32);
            const bool compute = compute_capture_fits && glare_compute();
            ResourceId a = kNoResource, b = kNoResource;
            if (g.level_a) {
                a = compute ? capture_level_compute(sources, level_a, g.gain_a, tiles)
                            : capture_level(sources, level_a, g.gain_a);
                a = blur(a, level_a, g.taps_a[0], g.spread_a, 0);
                a = blur(a, level_a, g.taps_a[1], g.spread_a, 1);
            }
            if (g.level_b) {
                if (a != kNoResource && !g.b_from_sources) {
                    const std::uint32_t a_width = level_a.width, a_height = level_a.height;
                    const std::uint32_t gain_bits = float_bits(g.gain_b);
                    b = add_screen_pass("glare box", Screen::GlareBox, level_b, {a},
                                        [a_width, a_height, gain_bits](PassContext& c, const std::vector<ResourceId>& in,
                                                                       std::uint32_t* r) {
                                            r[0] = c.srv(in[0]);
                                            r[4] = a_width;
                                            r[5] = a_height;
                                            r[8] = gain_bits;
                                        });
                } else {
                    b = capture_level(sources, level_b, g.gain_b);
                }
                b = blur(b, level_b, g.taps_b[0], g.spread_b, 0);
                b = blur(b, level_b, g.taps_b[1], g.spread_b, 1);
            }
            ResourceId first_level = a, second_level = b;
            if (a != kNoResource && b != kNoResource && !g.separate) {
                const std::uint32_t a_width = level_a.width, a_height = level_a.height;
                first_level = add_screen_pass(
                    "glare merge", Screen::GlareMerge, level_a, {a, b},
                    [a_width, a_height, linear_clamp](PassContext& c, const std::vector<ResourceId>& in,
                                                      std::uint32_t* r) {
                        r[0] = c.srv(in[0]);
                        r[1] = c.srv(in[1]);
                        r[2] = linear_clamp;
                        r[4] = a_width;
                        r[5] = a_height;
                    });
                second_level = kNoResource;
            } else if (a == kNoResource) {
                first_level = b;
                second_level = kNoResource;
            }
            // The composite: the scene gets the sources and the level(s), saturating - folded into the view's
            // resolve when only canvas draws follow (the diagnostics' glare parts keep the plain composite).
            std::vector<D3D12_RECT> spans;
            // 4x and above only: at 2x the composite's read-modify-write of two samples costs less than the longer
            // resolve (Final Haunt 2560x1440: 1.192 ms plain, 1.248 ms folded; at 4x 3.809 -> 2.481 ms).
            const bool fold = colour.samples >= 4 && !folded.active && options_.debug_glare_parts == 0u &&
                              !options_.debug_glare_only && fold_glare_composite() &&
                              later_canvas_spans(glare_pass, &spans);
            if (fold) {
                folded = FoldedGlare{sources, first_level, second_level, true, spans};
                ++render_stats_.glare_composites_folded;
                render_stats_.folded_glare_spans += spans.size();
                if (spans.empty()) {
                    ++glares;
                    return;
                }
            }
            ID3D12PipelineState* pipeline =
                screen_pipeline(Screen::GlareComposite, colour.format, colour.samples, nullptr);
            graph.add_pass(
                "glare composite",
                [&](PassBuilder& p) {
                    p.read(sources, Access::ShaderRead);
                    if (first_level != kNoResource)
                        p.read(first_level, Access::ShaderRead);
                    if (second_level != kNoResource)
                        p.read(second_level, Access::ShaderRead);
                    p.write(t.colour, Access::RenderTarget, Load::Keep);
                },
                [pipeline, &t, sources, first_level, second_level, linear_clamp, width, height, fold, spans,
                 glare_parts = options_.debug_glare_parts](PassContext& c) {
                    if (pipeline == nullptr)
                        return;
                    ID3D12GraphicsCommandList* cl = c.list();
                    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = c.rtv(t.colour);
                    cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
                    c.full_viewport(t.colour);
                    cl->SetPipelineState(pipeline);
                    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                    std::uint32_t root[16] = {};
                    root[0] = c.srv(sources);
                    root[1] = first_level != kNoResource ? c.srv(first_level) : kNoDescriptor;
                    root[2] = second_level != kNoResource ? c.srv(second_level) : kNoDescriptor;
                    root[3] = linear_clamp;
                    root[4] = width;
                    root[5] = height;
                    // Diagnostics (debug_glare_parts): 1 the sources alone, 2 the levels alone.
                    if (glare_parts == 1u)
                        root[1] = root[2] = kNoDescriptor;
                    else if (glare_parts == 2u)
                        root[0] = kNoDescriptor;
                    cl->SetGraphicsRoot32BitConstants(kRootConstants, 16, root, 0);
                    if (!fold) {
                        cl->DrawInstanced(3, 1, 0, 0);
                        return;
                    }
                    for (const D3D12_RECT& span : spans) {
                        cl->RSSetScissorRects(1, &span);
                        cl->DrawInstanced(3, 1, 0, 0);
                    }
                });
            ++glares;
        };

        // The view's passes in the game's order. Glare sources go to a black target over the scene's depth until the
        // view's next Glare pass adds their glare to the scene.
        ResourceId glare_sources = kNoResource;
        ResourceId glare_source_tiles = kNoResource;  // G2: their tile marks (kNoResource: resolved and read whole)
        // G11 (HW_REPORT P / R): the view's snapshots share one texture. Each snapshot is read only by the draws after
        // it in the view (a draw samples its view's latest snapshot), so the next snapshot can overwrite the same
        // texture once those draws are recorded - the graph orders the reads before the next resolve. One copy per
        // view instead of one per snapshot (2.95 per frame on Lava, up to 11): the pixels are the same.
        ResourceId snapshot_texture = kNoResource;
        for (const PassRecord& pass : packet.passes) {
            if (pass.view != v)
                continue;
            switch (pass.kind) {
            case PassKind::Scene:
                add_scene_pass(&pass);
                break;
            case PassKind::GlareSources: {
                if (first)
                    add_scene_pass(nullptr);  // the scene's depth exists before it is tested
                const bool fresh = glare_sources == kNoResource;
                if (fresh) {
                    glare_sources = graph.create_texture("glare sources", sources_desc);
                    if (sources_desc.samples > 1 && glare_tiles())
                        glare_source_tiles = add_tiles_clear();
                }
                add_draw_pass(&pass, "glare sources", glare_sources, sources_desc, fresh ? Load::Clear : Load::Keep,
                              true, glare_source_tiles);
                break;
            }
            case PassKind::Glare:
                if (glare_sources != kNoResource) {
                    add_glare(pass.glare, glare_sources, static_cast<std::size_t>(&pass - packet.passes.data()),
                              glare_source_tiles);
                    glare_sources = kNoResource;
                    glare_source_tiles = kNoResource;
                }
                break;
            case PassKind::Snapshot:
            case PassKind::SaveHistory: {
                // The scene colour as it is here, resolved into a new snapshot texture or into the view's history.
                const bool save = pass.kind == PassKind::SaveHistory;
                const SnapshotId id = pass_snapshot[static_cast<std::size_t>(&pass - packet.passes.data())];
                if (save ? history_ids[v] == kNoResource : id >= snapshot_ids.size())
                    break;
                if (first)
                    add_scene_pass(nullptr);  // the scene exists before it is copied
                // A snapshot is resolved into its own graph texture; a history save straight into the kept history
                // (a capture that is also presented resolves into a frame texture: the kept one is the present's).
                ResourceId resolved_id = kNoResource;
                if (save) {
                    resolved_id = commit_history ? history_ids[v] : graph.create_texture("history resolve", resolved);
                } else {
                    if (snapshot_texture == kNoResource)
                        snapshot_texture = graph.create_texture("scene snapshot", resolved);
                    resolved_id = snapshot_texture;
                }
                // A snapshot makes what its readers sample (none: a corner texel, the pass kept for the graph's order).
                TexelRect region;
                const bool part = !save && snapshot_whole[id] == 0u;
                if (part && snapshot_region[id].empty())
                    region = TexelRect{0, 0, 1, 1};
                else if (part)
                    region = snapshot_region[id];
                add_resolve(save ? "history resolve" : "snapshot", t.colour, colour, resolved_id,
                            part ? &region : nullptr);
                if (save && !commit_history) {
                    // A capture that is also presented: the frame's later draws read the picture it saves (its own
                    // resolve), the kept history is left for the present to save.
                    history_ids[v] = resolved_id;
                    history_valid = true;
                    break;
                }
                if (save) {
                    history_valid = true;
                    history_saved[v] = 1;
                } else {
                    snapshot_ids[id] = resolved_id;
                    snapshot_order[id] = ++snapshot_fills;
                    ++snapshots;
                }
                break;
            }
            }
        }
        if (first)
            add_scene_pass(nullptr);  // a view without passes still shows the background
        // Folded under an overlay: resolved into the overlay's colour (every sample), made here, ahead of its view.
        const bool fold = fold_into[v] != SIZE_MAX;
        const std::uint32_t into_samples = fold ? colour.samples : 1u;
        // Direct resolve (experiment, SHADOW_R2_DIRECT_RESOLVE=1 only - not bit-exact, see direct_resolve): the frame's
        // one unfolded view covers the whole output, so the compose only copies its 16-bit resolve, texel for texel
        // (point sampling, same size, no blend), into the 8-bit output. The experiment writes the output from the
        // resolve itself through an emulated 16-bit step (root3.y, PSResolve / PSResolveGlare), saving the 8 B/px
        // texture write, its read and the output clear. Nothing else reads the view's resolve: snapshots and history
        // resolve the scene colour themselves, captures read the output.
        const bool direct = !fold && unfolded_views == 1u && direct_resolve() &&
                            (direct_resolve_mode() == 1 || !scaled) && colour.samples > 1u &&
                            depth_bits_sample() == 0u && resolve_low_byte() == 0u && width == output.width &&
                            height == output.height && t.viewport.TopLeftX == 0.0f && t.viewport.TopLeftY == 0.0f &&
                            (output.format == DXGI_FORMAT_R8G8B8A8_UNORM || output.format == DXGI_FORMAT_B8G8R8A8_UNORM) &&
                            // The emulated step is the 16-bit store's; and with no compose after it, a resolve
                            // pipeline that failed to build would leave the whole output undefined.
                            resolved.format == DXGI_FORMAT_R16G16B16A16_UNORM &&
                            unorm_table_format_ == output.format && unorm_table_.srv != kNoDescriptor &&
                            screen_pipeline(Screen::Resolve, output.format, 1u, nullptr) != nullptr &&
                            (!folded.active || screen_pipeline(Screen::ResolveGlare, output.format, 1u, nullptr) != nullptr);
        if (fold)
            views[fold_into[v]].colour = graph.create_texture("view colour", colour);
        else if (!direct)
            t.resolved = graph.create_texture("view", resolved);
        output_resolved = output_resolved || direct;
        const ResourceId resolve_into = fold ? views[fold_into[v]].colour : direct ? out_id : t.resolved;
        const DXGI_FORMAT into_format = direct ? output.format : resolved.format;
        const std::uint32_t store16 = direct ? 1u : 0u;
        const std::uint32_t unorm_table = direct ? unorm_table_.srv : kNoDescriptor;
        if (folded.active) {
            ID3D12PipelineState* fold_pipeline =
                screen_pipeline(Screen::ResolveGlare, into_format, into_samples, nullptr);
            ID3D12PipelineState* plain_pipeline = screen_pipeline(Screen::Resolve, into_format, into_samples, nullptr);
            const ResourceId from = t.colour, into = resolve_into;
            const FoldedGlare g = folded;
            const std::uint32_t samples = colour.samples;
            graph.add_pass(
                "resolve",
                [&](PassBuilder& p) {
                    p.read(from, Access::ShaderRead);
                    p.read(g.sources, Access::ShaderRead);
                    if (g.first != kNoResource)
                        p.read(g.first, Access::ShaderRead);
                    if (g.second != kNoResource)
                        p.read(g.second, Access::ShaderRead);
                    p.write(into, Access::RenderTarget, Load::Discard);
                },
                [fold_pipeline, plain_pipeline, from, into, g, samples, linear_clamp, width, height,
                 store16, unorm_table](PassContext& c) {
                    if (fold_pipeline == nullptr || plain_pipeline == nullptr)
                        return;
                    ID3D12GraphicsCommandList* cl = c.list();
                    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = c.rtv(into);
                    cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
                    c.full_viewport(into);
                    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                    std::uint32_t root[16] = {};
                    root[0] = c.srv(g.sources);
                    root[1] = g.first != kNoResource ? c.srv(g.first) : kNoDescriptor;
                    root[2] = g.second != kNoResource ? c.srv(g.second) : kNoDescriptor;
                    root[3] = linear_clamp;
                    root[4] = c.srv(from);
                    root[5] = samples;
                    root[8] = width;
                    root[9] = height;
                    root[12] = resolve_low_byte();
                    root[13] = store16;
                    root[14] = unorm_table;
                    cl->SetGraphicsRoot32BitConstants(kRootConstants, 16, root, 0);
                    cl->SetPipelineState(fold_pipeline);
                    cl->DrawInstanced(3, 1, 0, 0);
                    // The spans got the composite before the draws after it: their samples are resolved as they are.
                    cl->SetPipelineState(plain_pipeline);
                    for (const D3D12_RECT& span : g.spans) {
                        cl->RSSetScissorRects(1, &span);
                        cl->DrawInstanced(3, 1, 0, 0);
                    }
                });
        } else {
            add_resolve("resolve", t.colour, colour, resolve_into, nullptr, into_samples, into_format, store16,
                        unorm_table);
        }
        // Diagnostics (SHADOW_R2_DEPTH_BITS=1..4, non-shipping): the view shows the bits of stored depth sample n-1
        // instead of its picture (GPU_FILL.md section 17).
        if (const std::uint32_t depth_sample = depth_bits_sample(); depth_sample != 0u && colour.samples > 1u) {
            ID3D12PipelineState* bits = screen_pipeline(Screen::DepthBits, resolved.format, 1, nullptr);
            const ResourceId depth = t.depth, into = t.resolved;
            graph.add_pass(
                "depth bits",
                [&](PassBuilder& p) {
                    p.read(depth, Access::ShaderRead);
                    p.write(into, Access::RenderTarget, Load::Keep);
                },
                [bits, depth, into, depth_sample](PassContext& c) {
                    if (bits == nullptr)
                        return;
                    ID3D12GraphicsCommandList* cl = c.list();
                    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = c.rtv(into);
                    cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
                    c.full_viewport(into);
                    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                    std::uint32_t root[16] = {};
                    root[4] = c.srv(depth);
                    root[5] = depth_sample - 1u;
                    cl->SetGraphicsRoot32BitConstants(kRootConstants, 16, root, 0);
                    cl->SetPipelineState(bits);
                    cl->DrawInstanced(3, 1, 0, 0);
                });
        }
    }

    ID3D12PipelineState* compose = output_resolved ? nullptr : screen_pipeline(Screen::Under, output.format, 1, error);
    if (!output_resolved && compose == nullptr)
        return false;
    if (!output_resolved) {
        graph.add_pass(
            "compose",
            [&](PassBuilder& p) {
                for (const ViewTargets& t : views)
                    if (t.resolved != kNoResource)  // a folded view is in its overlay's picture
                        p.read(t.resolved, Access::ShaderRead);
                // A view covering the whole output overwrites every pixel with an opaque copy (no blend), so the clear
            // under it is never seen: the output is discarded instead of cleared (exact; bars and split screens
            // still clear).
            bool covered = false;
            for (const ViewTargets& t : views)
                covered = covered || (t.resolved != kNoResource && t.scissor.left <= 0 && t.scissor.top <= 0 &&
                                      t.scissor.right >= static_cast<LONG>(output.width) &&
                                      t.scissor.bottom >= static_cast<LONG>(output.height));
            p.write(out_id, Access::RenderTarget, covered ? Load::Discard : Load::Clear);
            },
            [&views, compose, out_id](PassContext& c) {
                ID3D12GraphicsCommandList* cl = c.list();
                const D3D12_CPU_DESCRIPTOR_HANDLE rtv = c.rtv(out_id);
                cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
                cl->SetPipelineState(compose);
                cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                for (const ViewTargets& t : views) {
                    if (t.resolved == kNoResource)
                        continue;
                    c.set_viewport(t.viewport, t.scissor);
                    const std::uint32_t constants[2] = {c.srv(t.resolved),
                        sampler_index(Filter::Point, Address::Clamp, Address::Clamp)};
                    cl->SetGraphicsRoot32BitConstants(kRootConstants, 2, constants, 4);
                    cl->DrawInstanced(3, 1, 0, 0);
                }
            });
    }
    if (scaled) {
        ID3D12PipelineState* scale = screen_pipeline(Screen::Scale, output.format, 1, error);
        if (scale == nullptr)
            return false;
        // The picture placed in the back buffer (gekko/display_geometry.h): the largest rectangle of the picture's
        // aspect - the game's viewport, exact; else the render size's own - centred, black bars outside. The render
        // size is mapped onto it whole (the only resampling: 1:1, a 2x2 box at exactly 2:1, a bicubic otherwise).
        const double picture_aspect = output.picture_aspect;
        const gekko::PixelSize place = gekko::fit_aspect(
            {output.present_width, output.present_height},
            picture_aspect > 0.0 ? picture_aspect : static_cast<double>(output.width) / output.height);
        const float vw = static_cast<float>(place.width), vh = static_cast<float>(place.height);
        const D3D12_VIEWPORT viewport{static_cast<float>((output.present_width - place.width) / 2u),
                                      static_cast<float>((output.present_height - place.height) / 2u), vw, vh, 0.0f,
                                      1.0f};
        const D3D12_RECT scissor{static_cast<LONG>(viewport.TopLeftX), static_cast<LONG>(viewport.TopLeftY),
                                 static_cast<LONG>(viewport.TopLeftX + vw), static_cast<LONG>(viewport.TopLeftY + vh)};
        const bool box = output.width == 2u * place.width && output.height == 2u * place.height;
        const std::uint32_t source_w = output.width, source_h = output.height;
        graph.add_pass(
            "present scale",
            [&](PassBuilder& p) {
                p.read(out_id, Access::ShaderRead);
                p.write(present_id, Access::RenderTarget, Load::Clear);
            },
            [scale, out_id, present_id, viewport, scissor, box, source_w, source_h](PassContext& c) {
                ID3D12GraphicsCommandList* cl = c.list();
                const D3D12_CPU_DESCRIPTOR_HANDLE rtv = c.rtv(present_id);
                cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
                cl->SetPipelineState(scale);
                cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                c.set_viewport(viewport, scissor);
                const std::uint32_t source[2] = {c.srv(out_id), box ? 1u : 0u};
                const std::uint32_t size[2] = {source_w, source_h};
                cl->SetGraphicsRoot32BitConstants(kRootConstants, 2, source, 4);
                cl->SetGraphicsRoot32BitConstants(kRootConstants, 2, size, 8);
                cl->DrawInstanced(3, 1, 0, 0);
            });
    }
    if (hud_overlay && scaled) {
        // The views' trailing 2D at the picture's own pixels (see hud_deferred): each view's rectangle in the back
        // buffer is its rectangle of the render size mapped onto the placed picture, and its canvas is laid out there
        // exactly as the scene lays it out on the view's target - the same anchors, scissors and draw order, without
        // the render scale.
        struct HudView {
            std::size_t view;
            D3D12_VIEWPORT viewport;
            D3D12_RECT rect;
        };
        std::vector<HudView> hud_views;
        const double sx = static_cast<double>(hud_place.width) / output.width;
        const double sy = static_cast<double>(hud_place.height) / output.height;
        const double ox = static_cast<double>((output.present_width - hud_place.width) / 2u);
        const double oy = static_cast<double>((output.present_height - hud_place.height) / 2u);
        for (std::size_t v = 0; v < packet.views.size(); ++v) {
            bool any = false;
            for (const PassRecord& pass : packet.passes)
                for (std::uint32_t d = pass.first_draw; pass.view == v && !any && d < pass.first_draw + pass.draw_count &&
                                                        d < hud_deferred.size();
                     ++d)
                    any = hud_deferred[d] != 0u;
            if (!any)
                continue;
            const ViewRecord& view = packet.views[v];
            const double x0 = box_x + std::round(view.rect.x * box_w), y0 = box_y + std::round(view.rect.y * box_h);
            const double x1 = box_x + std::round((view.rect.x + view.rect.width) * box_w);
            const double y1 = box_y + std::round((view.rect.y + view.rect.height) * box_h);
            const LONG l = static_cast<LONG>(std::lround(ox + x0 * sx)), t = static_cast<LONG>(std::lround(oy + y0 * sy));
            const LONG r = static_cast<LONG>(std::lround(ox + x1 * sx)), b = static_cast<LONG>(std::lround(oy + y1 * sy));
            if (r <= l || b <= t)
                continue;
            hud_views.push_back({v,
                                 D3D12_VIEWPORT{static_cast<float>(l), static_cast<float>(t), static_cast<float>(r - l),
                                                static_cast<float>(b - t), 0.0f, 1.0f},
                                 D3D12_RECT{l, t, r, b}});
        }
        const DXGI_FORMAT present_format = output.format;
        graph.add_pass(
            "hud overlay", [&](PassBuilder& p) { p.write(present_id, Access::RenderTarget, Load::Keep); },
            [this, &packet, &hud_deferred, hud_views, present_id, present_format](PassContext& c) {
                ID3D12GraphicsCommandList* cl = c.list();
                const D3D12_CPU_DESCRIPTOR_HANDLE rtv = c.rtv(present_id);
                cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
                ID3D12PipelineState* bound = nullptr;
                for (const HudView& hv : hud_views) {
                    const ViewRecord& view = packet.views[hv.view];
                    const float w = hv.viewport.Width, h = hv.viewport.Height;
                    std::array<Mat4, kAnchorCount> placement;
                    std::array<D3D12_RECT, kAnchorCount> scissor;
                    for (std::size_t a = 0; a < placement.size(); ++a) {
                        placement[a] = canvas_projection(view.canvas, static_cast<Anchor>(a), w, h);
                        const D3D12_RECT local = to_rect(
                            shadow::render::canvas_scissor(view.canvas, static_cast<Anchor>(a), placement[a], w, h));
                        scissor[a] = D3D12_RECT{std::max(hv.rect.left, hv.rect.left + local.left),
                                                std::max(hv.rect.top, hv.rect.top + local.top),
                                                std::min(hv.rect.right, hv.rect.left + local.right),
                                                std::min(hv.rect.bottom, hv.rect.top + local.bottom)};
                        if (scissor[a].right < scissor[a].left) scissor[a].right = scissor[a].left;
                        if (scissor[a].bottom < scissor[a].top) scissor[a].bottom = scissor[a].top;
                    }
                    c.set_viewport(hv.viewport, hv.rect);
                    for (const PassRecord& pass : packet.passes) {
                        if (pass.view != hv.view)
                            continue;
                        for (std::uint32_t d = pass.first_draw;
                             d < pass.first_draw + pass.draw_count && d < hud_deferred.size(); ++d) {
                            if (hud_deferred[d] == 0u)
                                continue;
                            const DrawRecord& draw = packet.draws[d];
                            const ImmediateDraw& im = packet.immediates[draw.immediate];
                            const bool lines = im.topology == Topology::Lines || im.topology == Topology::LineStrip;
                            ID3D12PipelineState* pipeline = mesh_pipeline(Program::Immediate, draw.recipe, present_format,
                                                                          1u, nullptr, lines, false, false, true);
                            if (pipeline == nullptr || im.vertex_count == 0 ||
                                std::size_t{im.first_vertex} + im.vertex_count > packet.immediate_vertices.size() ||
                                std::size_t{im.first_index} + im.index_count > packet.immediate_indices.size())
                                continue;
                            const TextureSlot* texture = drawable_texture(im.texture);
                            const std::uint32_t image = texture != nullptr ? texture->texture.srv : kNoDescriptor;
                            const std::uint64_t vertex_bytes = std::uint64_t{im.vertex_count} * sizeof(ImmediateVertex);
                            const std::uint64_t index_bytes = std::uint64_t{im.index_count} * sizeof(std::uint32_t);
                            const UploadAllocation vertices = frames_.ring().allocate(vertex_bytes, 16);
                            const UploadAllocation indices =
                                im.index_count != 0 ? frames_.ring().allocate(index_bytes, 16) : UploadAllocation{};
                            const UploadAllocation block = frames_.ring().allocate(sizeof(ImmediateConstants), 256);
                            if (!vertices || !block || (im.index_count != 0 && !indices))
                                continue;
                            std::memcpy(vertices.cpu, &packet.immediate_vertices[im.first_vertex], vertex_bytes);
                            if (im.index_count != 0)
                                std::memcpy(indices.cpu, &packet.immediate_indices[im.first_index], index_bytes);
                            // A draw laid out in a region of the canvas (split screen) is placed and clipped there.
                            Mat4 region_placement;
                            PixelBox region_box;
                            const bool in_region = im.region != kNoRegion && im.region < packet.canvas_regions.size();
                            if (in_region)
                                region_projection(view.canvas, packet.canvas_regions[im.region], im.anchor, w, h,
                                                  &region_placement, &region_box);
                            const Mat4& place =
                                in_region ? region_placement : placement[static_cast<std::size_t>(im.anchor)];
                            ImmediateConstants constants{};
                            store_column_major(multiply(place, from3x4(draw.world)), constants.world_view_projection);
                            constants.uv_row[0][0] = 1.0f;
                            constants.uv_row[1][1] = 1.0f;
                            constants.texture = image;
                            constants.sampler = sampler_index(im.sampler.filter, im.sampler.u, im.sampler.v);
                            constants.cutoff = draw.recipe.alpha_cutoff > 0.0f ? draw.recipe.alpha_cutoff : -1.0f;
                            constants.flags = skip_clear_flag(draw.recipe);
                            constants.fog_enabled = draw.recipe.fog ? 1u : 0u;
                            constants.fog_start = draw.recipe.fog_start;
                            constants.fog_end = draw.recipe.fog_end;
                            constants.fog_colour = packed(draw.recipe.fog_colour);
                            std::memcpy(block.cpu, &constants, sizeof(constants));
                            if (pipeline != bound) {
                                cl->SetPipelineState(pipeline);
                                bound = pipeline;
                            }
                            cl->IASetPrimitiveTopology(to_d3d(im.topology));
                            const D3D12_VERTEX_BUFFER_VIEW stream{vertices.gpu, static_cast<UINT>(vertex_bytes),
                                                                  static_cast<UINT>(sizeof(ImmediateVertex))};
                            cl->IASetVertexBuffers(0, 1, &stream);
                            cl->SetGraphicsRootConstantBufferView(kRootFrameConstants, block.gpu);
                            D3D12_RECT clip = scissor[static_cast<std::size_t>(im.anchor)];
                            if (in_region) {
                                const D3D12_RECT local = to_rect(region_box);
                                clip = D3D12_RECT{std::max(hv.rect.left, hv.rect.left + local.left),
                                                  std::max(hv.rect.top, hv.rect.top + local.top),
                                                  std::min(hv.rect.right, hv.rect.left + local.right),
                                                  std::min(hv.rect.bottom, hv.rect.top + local.bottom)};
                                if (clip.right < clip.left) clip.right = clip.left;
                                if (clip.bottom < clip.top) clip.bottom = clip.top;
                            }
                            cl->RSSetScissorRects(1, &clip);
                            if (im.index_count != 0) {
                                const D3D12_INDEX_BUFFER_VIEW ibv{indices.gpu, static_cast<UINT>(index_bytes),
                                                                  DXGI_FORMAT_R32_UINT};
                                cl->IASetIndexBuffer(&ibv);
                                cl->DrawIndexedInstanced(im.index_count, 1, 0, 0, 0);
                            } else {
                                cl->DrawInstanced(im.vertex_count, 1, 0, 0);
                            }
                        }
                    }
                }
                cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            });
    }
    if (capture != nullptr) {
        ImportedBuffer imported;
        imported.resource = capture->resource.Get();
        imported.state = D3D12_RESOURCE_STATE_COPY_DEST;
        imported.final_state = D3D12_RESOURCE_STATE_COPY_DEST;
        const ResourceId capture_id = graph.import_buffer("capture", imported);
        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout = *capture_layout;
        ID3D12Resource* destination = capture->resource.Get();
        graph.add_pass(
            "capture",
            [&](PassBuilder& p) {
                p.read(scaled ? present_id : out_id, Access::CopySource);
                p.write(capture_id, Access::CopyDest, Load::Discard);
                p.side_effect();
            },
            [layout, destination, captured = scaled ? present_id : out_id](PassContext& c) {
                D3D12_TEXTURE_COPY_LOCATION to{};
                to.pResource = destination;
                to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                to.PlacedFootprint = layout;
                D3D12_TEXTURE_COPY_LOCATION from{};
                from.pResource = c.resource(captured);
                from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                c.list()->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            });
    }
    if (!graph.compile(pool_, error))
        return false;
#if !defined(GEKKO_SHIPPING)
    // Resolution evidence (docs/RESOLUTION_AUDIT.md): GEKKO_RENDER_TARGET_TRACE=<file> writes, for the first frame the
    // renderer records at or after number GEKKO_RENDER_TARGET_TRACE_FRAME (default 600; renderer frames - a headless
    // run renders only its captures, plus frames that only keep the motion-trail history), every kept pass with the
    // targets it binds as D3D12 reports them and the viewports it sets. Once per run.
    static const char* const target_trace_path = std::getenv("GEKKO_RENDER_TARGET_TRACE");
    // GEKKO_RENDER_TARGET_TRACE_FRAME=capture: the first frame recorded for a capture (a headless run's picture).
    static const char* const target_trace_when = std::getenv("GEKKO_RENDER_TARGET_TRACE_FRAME");
    static const bool target_trace_capture = target_trace_when != nullptr && std::strcmp(target_trace_when, "capture") == 0;
    static const std::uint64_t target_trace_frame =
        target_trace_when != nullptr && target_trace_when[0] != '\0' && !target_trace_capture
            ? std::strtoull(target_trace_when, nullptr, 10)
            : 600ull;
    static bool target_trace_written = false;
    std::vector<std::string> target_trace;
    const bool tracing = target_trace_path != nullptr && !target_trace_written &&
                         (target_trace_capture ? capture != nullptr : frame_number >= target_trace_frame);
    if (tracing)
        target_trace_written = true;
    if (tracing)
        graph.set_target_trace(&target_trace);
#endif
    graph.execute(list, heap_, samplers_, root_.Get(), gpu_timer_.get());
#if !defined(GEKKO_SHIPPING)
    if (tracing) {
        graph.set_target_trace(nullptr);
        if (FILE* out = std::fopen(target_trace_path, "w")) {
            std::fprintf(out, "# shadow render target trace 1: frame %llu, output %ux%u, presented %ux%u, %zu views\n",
                         static_cast<unsigned long long>(frame_number), output.width, output.height,
                         output.present_width, output.present_height, packet.views.size());
            for (const std::string& line : target_trace)
                std::fprintf(out, "%s\n", line.c_str());
            std::fclose(out);
        }
    }
#endif
    if (gpu_timer_ != nullptr)
        gpu_timer_->note_barriers(graph.transitions().size());
    if (output_state_after != nullptr)
        *output_state_after = graph.state_after(scaled ? present_id : out_id);
    std::uint64_t history_saves = 0;
    for (std::size_t v = 0; v < kept_history_ids.size(); ++v) {
        if (kept_history_ids[v] == kNoResource)
            continue;
        histories_[v].state = graph.state_after(kept_history_ids[v]);
        if (history_saved[v] != 0u) {
            histories_[v].valid = true;
            histories_[v].initialised = true;  // its discard and first write are recorded
            ++history_saves;
        }
    }
    render_stats_.snapshots = snapshots;
    render_stats_.history_saves = history_saves;
    render_stats_.draws = draws;
    render_stats_.skipped_draws = skipped;
    render_stats_.skipped_draws_total += skipped;
    render_stats_.skin_jobs = static_cast<std::uint64_t>(
        std::count_if(skin_jobs.begin(), skin_jobs.end(), [](const SkinJob& job) { return job.skin; }));
    render_stats_.morph_jobs = morph_jobs;
    render_stats_.skinned_vertices = skinned_vertices;
    render_stats_.particle_batches = packet.particle_batches.size();
    render_stats_.particles = particles;
    render_stats_.glares = glares;
    render_stats_.immediates = immediates;
    ++render_stats_.frames;
    return true;
}

void Renderer::end_gpu_frame(ID3D12GraphicsCommandList* list) {
    GpuTimer::Counters counters;
    counters.draws = render_stats_.draws;
    counters.immediates = render_stats_.immediates;
    counters.particles = render_stats_.particles;
    counters.upload_bytes = frames_.ring().used();
    counters.upload_overflows = frames_.ring().overflow_count();
    counters.transient_descriptors = heap_.transient_used();
    counters.persistent_descriptors = heap_.persistent_in_use();
    const Device::Budget budget = device_.local_budget();
    counters.vram_usage = budget.usage;
    counters.vram_budget = budget.budget;
    gpu_timer_->end_frame(list, counters);
}

GpuMemoryTrace::Counters Renderer::memory_counters() const {
    GpuMemoryTrace::Counters c;
    c.frame = frames_.frame_number();
    const UploadStream::Counts u = uploads_.counts();
    c.copy_allocators_created = u.allocators_created;
    c.copy_allocators_spare = u.allocators_spare;
    c.copy_submissions_in_flight = u.in_flight;
    c.upload_submissions = u.submissions;
    c.upload_staged_bytes = u.staged_bytes;
    c.upload_ring_waits = u.ring_waits;
    c.upload_dedicated = u.dedicated;
    c.retired_pending = device_.retired_count();
    c.persistent_descriptors = heap_.persistent_in_use();
    return c;
}

// How long an unused pooled render target is kept (TexturePool::kKeepFrames). Diagnostics only (A/B in one traced
// binary): SHADOW_R2_POOL_KEEP_FRAMES=<n> overrides it (3 = the policy before 2026-10-26).
static std::uint64_t pool_keep_frames() {
#if !defined(GEKKO_SHIPPING)
    static const std::uint64_t keep = [] {
        const char* v = std::getenv("SHADOW_R2_POOL_KEEP_FRAMES");
        return v != nullptr && v[0] != '\0' ? std::strtoull(v, nullptr, 10) : TexturePool::kKeepFrames;
    }();
    return keep;
#else
    return TexturePool::kKeepFrames;
#endif
}
const char* Renderer::skip_reason_name(SkipReason reason) noexcept {
    switch (reason) {
        case SkipReason::ParticleBatch: return "particle-batch";
        case SkipReason::ParticlePipeline: return "particle-pipeline";
        case SkipReason::ParticleUpload: return "particle-upload";
        case SkipReason::ImmediateInvalid: return "immediate-invalid";
        case SkipReason::ImmediatePipeline: return "immediate-pipeline";
        case SkipReason::ImmediateImage: return "immediate-image";
        case SkipReason::ImmediateUpload: return "immediate-upload";
        case SkipReason::MeshMissing: return "mesh-missing";
        case SkipReason::SkinMissing: return "skin-missing";
        case SkipReason::MeshPipeline: return "mesh-pipeline";
        case SkipReason::MeshUpload: return "mesh-upload";
        case SkipReason::Count: break;
    }
    return "?";
}

void Renderer::publish_render_stats() {
    const Stats& r = render_stats_;
    std::lock_guard<std::mutex> lock(table_mutex_);
    stats_.frames = r.frames;
    stats_.draws = r.draws;
    stats_.skipped_draws = r.skipped_draws;
    stats_.skipped_by_reason = r.skipped_by_reason;
    // The pool's memory description changes when a texture is created or released, and which textures are in use
    // changes slowly: rebuilt then and once a second.
    const bool pool_changed = stats_.pool_created != pool_.created() || stats_.pool_released != pool_.released() ||
                              stats_.pool_memories != pool_.memories() || frames_.frame_number() % 60u == 0u;
    stats_.pool_textures = pool_.size();
    stats_.pool_created = pool_.created();
    stats_.pool_released = pool_.released();
    stats_.pool_memories = pool_.memories();
    if (pool_changed) {
        stats_.pool_memory_bytes = pool_.memory_bytes();
        stats_.pool_placed_bytes = pool_.placed_bytes();
        stats_.pool_memory_map = pool_.memory_map();
    }
    stats_.skin_jobs = r.skin_jobs;
    stats_.skinned_vertices = r.skinned_vertices;
    stats_.morph_jobs = r.morph_jobs;
    stats_.skipped_draws_total = r.skipped_draws_total;
    stats_.particle_batches = r.particle_batches;
    stats_.particles = r.particles;
    stats_.glares = r.glares;
    stats_.immediates = r.immediates;
    stats_.snapshots = r.snapshots;
    stats_.history_saves = r.history_saves;
    stats_.glare_composites_folded = r.glare_composites_folded;
    stats_.folded_glare_spans = r.folded_glare_spans;
    stats_.image_misses = r.image_misses;
    stats_.views_over_views = r.views_over_views;
    stats_.views_folded = r.views_folded;
    stats_.skin_overflows = r.skin_overflows;
    stats_.pipelines = r.pipelines;
    stats_.resolve_waits = r.resolve_waits;
}

bool Renderer::attach_window(HWND window, std::uint32_t width, std::uint32_t height, std::string* error) {
    std::lock_guard<std::mutex> lock(render_mutex_);
    SwapChainOptions options;
    if (!swap_chain_.create(device_, window, width, height, options, error))
        return false;
    for (D3D12_RESOURCE_STATES& state : back_buffer_states_)
        state = D3D12_RESOURCE_STATE_PRESENT;
    has_window_ = true;
    frame_acquired_ = false;  // a new swap chain, a new waitable
    credit_debt_ = 0;         // nothing owed to it
    leave_burst_mode_unmeasured();  // created with one frame in flight
    return true;
}

void Renderer::apply_burst_mode() {
    if (!burst_.running())
        return;
    const bool want = burst_.want_ahead();
    if (want == burst_ahead_)
        return;
    const bool ahead_mode = BurstAhead::mode() == BurstAhead::Mode::Ahead;
    const std::int64_t lead = BurstAhead::burst_lead_ns();
    if (!want) {
        // Leaving gives the lead's audio back (gekko/schedule_offset.h): only once the device queue holds it plus 12 ms
        // (the device period is 10 ms; a 5 ms margin left 5 ms and one underrun while the phase lock was still moving
        // the game later, 2026-11-05), else stay (the trim refills the queue against its neutral target).
        const std::int64_t queued = gekko::schedule_offset::audio_queued_ns().load(std::memory_order_relaxed);
        if (queued >= 0 && queued < lead + 12'000'000)
            return;
    }
    if (want) {
        // Render-ahead: two frames in flight, DXGI hands the extra credit back at once. Both: the game runs `lead` early.
        if (ahead_mode)
            swap_chain_.set_max_latency(2);
        gekko::schedule_offset::desired_lead_ns().store(lead, std::memory_order_release);
    } else {
        // Render-ahead: one frame in flight again; DXGI takes no credit back, so this frame takes one more while the
        // game skips one frame of wall time and the second queued frame is shown.
        if (ahead_mode) {
            swap_chain_.set_max_latency(1);
            ++burst_repay_;
        }
        gekko::schedule_offset::desired_lead_ns().store(0, std::memory_order_release);
    }
    burst_ahead_ = want;
    burst_settle_ = 8;
    present_trace::record(present_trace::Ev::BurstAhead, want ? 1u : 0u,
                          static_cast<std::uint64_t>(burst_.deciding_span_ns()));
}

std::int64_t Renderer::hold_bias_ns() const noexcept {
    // Burst lead: packets come `lead` earlier than the phase lock's schedule while it lasts; render-ahead's frame is
    // absorbed by the swap chain's queue instead (the credit comes at the flip), so it biases nothing.
    return (burst_ahead_ && BurstAhead::mode() == BurstAhead::Mode::Lead) ? BurstAhead::burst_lead_ns() : 0;
}

bool Renderer::take_credit() {
    // Every present must be matched by one credit taken. A wait that took none (timed out or failed) is still followed
    // by the present, as before - the frame is not dropped - but that present returns a credit later which no wait
    // takes, and the pipeline would stay one frame deeper from then on (more input latency, the same picture). It is
    // owed instead, and taken back without waiting once it is there (Codex review 2026-11-05: the result was ignored).
    const auto take_one = [this] {
        if (swap_chain_.wait_for_frame(1000))
            return true;
        credit_debt_ = std::min<std::uint32_t>(credit_debt_ + 1u, 3u);  // at most the swap chain's buffers
        if (static std::atomic<bool> reported{false}; !reported.exchange(true))
            std::fprintf(stderr, "[render] swap chain credit wait ended without a credit (timeout or failure); owed\n");
        return false;
    };
    const bool took = take_one();
    for (; burst_repay_ > 0u; --burst_repay_)
        take_one();
    while (took && credit_debt_ > 0u && swap_chain_.try_take_credit())
        --credit_debt_;
    return took;
}

bool Renderer::msaa_supported(std::uint32_t samples) const noexcept {
    if (samples == 0u || (samples & (samples - 1u)) != 0u || samples > 32u)
        return false;
    std::uint32_t bit = 0;
    while ((1u << bit) < samples) ++bit;
    return (msaa_supported_mask_ & (1u << bit)) != 0u;
}

std::string Renderer::msaa_note() const {
    std::lock_guard<std::mutex> lock(msaa_note_mutex_);
    return msaa_note_;
}

void Renderer::apply_quality() {
    // MSAA: the next frame's targets and pipelines take the count (the target pool and the pipeline cache are keyed by
    // it; targets of the old count age out of the pool). The swap chain is untouched.
    if (const std::uint32_t asked = msaa_request_.exchange(0u, std::memory_order_acq_rel); asked != 0u && asked != msaa_) {
        std::lock_guard<std::mutex> lock(msaa_note_mutex_);
        if (msaa_supported(asked)) {
            msaa_ = asked;
            msaa_active_.store(asked, std::memory_order_release);
            msaa_note_.clear();
            std::fprintf(stderr, "[render] MSAA %ux\n", asked);
        } else {
            msaa_note_ = std::to_string(asked) + "x MSAA is not supported by this GPU for the scene's formats; " +
                         std::to_string(msaa_) + "x stays in use";
            std::fprintf(stderr, "[render] %s\n", msaa_note_.c_str());
        }
    }
    // Anisotropic filtering: the sampler table is shared by every frame in flight, so it is rewritten with the GPU idle
    // (a settings change, not a per-frame cost).
    if (const std::uint32_t asked = anisotropy_request_.exchange(0u, std::memory_order_acq_rel);
        asked != 0u && asked != samplers_.anisotropy()) {
        device_.direct().wait_idle();
        samplers_.set_anisotropy(device_, asked);
        anisotropy_active_.store(samplers_.anisotropy(), std::memory_order_release);
        std::fprintf(stderr, "[render] anisotropic filtering %s\n",
                     asked <= 1u ? "off (the original linear filter)" : (std::to_string(asked) + "x").c_str());
    }
}

void Renderer::leave_burst_mode_unmeasured() {
    // A new swap chain starts at latency 1; a resized one keeps its latency, and whether ResizeBuffers refills the
    // credits is not documented - either way no credit is repaid here (at worst one refresh of extra latency until the
    // next switch, never a stall).
    if (burst_ahead_) {
        swap_chain_.set_max_latency(1);
        gekko::schedule_offset::desired_lead_ns().store(0, std::memory_order_release);
        present_trace::record(present_trace::Ev::BurstAhead, 2u, 0u);
    }
    burst_ahead_ = false;
    burst_repay_ = 0;
    burst_settle_ = 8;
    burst_.force_calm();
}

bool Renderer::acquire_frame() {
    std::lock_guard<std::mutex> lock(render_mutex_);
    if (!has_window_ || frame_acquired_)
        return true;
    GEKKO_PROFILE_ZONE("gpu/swap-wait");
    apply_burst_mode();
    const bool took = take_credit();
    frame_acquired_ = true;  // render() does not wait again either way (as before); a missing credit is owed
    return took;
}

std::int64_t Renderer::jit_wait(bool vsync, double fill_mpx) {
    // The rendering thread's own state; no lock held while sleeping (resize takes the render lock on this thread
    // only, so nothing can change the swap chain meanwhile).
    jit_start_ns_ = 0;
    jit_fill_mpx_ = fill_mpx;
    if (!vsync || !has_window_ || !JitPacer::requested())
        return 0;
    if (!jit_.running())
        jit_.start(device_.direct().fence());
    const JitPacer::Start start = jit_.wait_for_start(fill_mpx);
    jit_start_ns_ = start.start;
    return start.target;
}

double Renderer::particle_fill_mpx(const FramePacket& packet) const {
    // The render size the frame will be recorded at (G10), else the window's.
    const std::uint64_t size = render_size_.load(std::memory_order_acquire);
    std::uint32_t w = static_cast<std::uint32_t>(size >> 32), h = static_cast<std::uint32_t>(size);
    if (w == 0u || h == 0u) {
        w = swap_chain_.width();
        h = swap_chain_.height();
    }
    return estimate_particle_fill(packet, w, h).pixels / 1e6;
}

bool Renderer::resize_window(std::uint32_t width, std::uint32_t height, std::string* error) {
    std::lock_guard<std::mutex> lock(render_mutex_);
    if (!has_window_)
        return fail(error, "no window attached");
    // A repeated size (WM_SIZE on restore / maximise without a change) keeps everything: clearing the pool here
    // discarded every placed target - hundreds of MiB at 5K - only for the swap chain to return unchanged.
    if (std::max<std::uint32_t>(width, 1) == swap_chain_.width() && std::max<std::uint32_t>(height, 1) == swap_chain_.height())
        return true;
    jit_.reset_grid();  // a new swap chain size: its statistics start again
    leave_burst_mode_unmeasured();
    pool_.clear();
    // Whether ResizeBuffers refills the latency semaphore is not documented: wait for a fresh credit after it (at
    // worst one refresh at a resize).
    frame_acquired_ = false;
    return swap_chain_.resize(width, height, error);
}

void Renderer::make_uploads_visible() {
    // The jobs submit their uploads themselves before their resources become Ready; the direct queue is ordered after
    // the latest of them on the GPU - the CPU never waits here and takes no lock.
    const std::uint64_t uploaded = uploads_published_.load(std::memory_order_acquire);
    if (uploaded > uploads_visible_) {
        device_.direct().wait_for(device_.copy(), uploaded);
        uploads_visible_ = uploaded;
    }
}

// The swap chain belongs to the render thread alone. Nothing here holds the table lock across a wait: the back
// buffer wait, the frame slot's fence wait (frames_.begin) and the recording hold only the render lock, which the
// game thread never takes (D11).
bool Renderer::render(const FramePacket& packet, PresentMode mode, std::string* error, std::int64_t* credit_ns) {
    GEKKO_PROFILE_ZONE("gpu/render");
    // Eager start (render/burst_ahead.h): credit_ns given - record and submit now, take the credit only before the
    // present (credit_ns: when it came, steady ns). The back buffer recorded into is the one neither shown nor queued.
    const bool eager = credit_ns != nullptr;
    // SHADOW_R2_JIT: a frame jit_wait did not start (no wait asked) spans from here.
    const std::int64_t frame_start_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
    SHADOW_RECORD_PACKETS(frame(packet, 0, 0, false, true));
    // C3: the packet this call reads (reading()), until it returns.
    struct ReadEnd {
        std::atomic<const FramePacket*>& slot;
        const FramePacket* before;
        ~ReadEnd() { slot.store(before, std::memory_order_release); }
    } read_end{reading_, reading_.exchange(&packet, std::memory_order_acq_rel)};
    std::lock_guard<std::mutex> render_lock(render_mutex_);
    if (!has_window_)
        return fail(error, "no window attached");
    apply_quality();  // MSAA / anisotropic filtering changes, from this frame
    if (!frame_acquired_ && !eager) {
        GEKKO_PROFILE_ZONE("gpu/swap-wait");
        apply_burst_mode();
        take_credit();
    }
    frame_acquired_ = false;
    if (burst_settle_ > 0u)
        --burst_settle_;
    const std::uint32_t index = swap_chain_.current();
    // No back buffer: the last resize failed after releasing them (SwapChain::resize). Recording into a null output
    // would silently draw nothing (a null resource is the off-screen capture path) and present a frozen window.
    if (swap_chain_.back_buffer(index) == nullptr)
        return fail(error, "no back buffer: the last swap chain resize failed");
    D3D12_RESOURCE_STATES after = D3D12_RESOURCE_STATE_PRESENT;
    {
        const FrameLoop::Frame frame = [this] {
            GEKKO_PROFILE_ZONE("gpu/slot-wait");
            return frames_.begin();
        }();
        if (gpu_timer_ != nullptr)
            gpu_timer_->begin_frame(frame.list, frame.slot, frame.number, packet.tick, packet.game_ticks, false);
        Output output;
        output.resource = swap_chain_.back_buffer(index);
        output.state = back_buffer_states_[index];
        output.rtv = swap_chain_.rtv(index);
        output.format = swap_chain_.format();
        output.width = swap_chain_.width();
        output.height = swap_chain_.height();
        // G10: a render size other than the window's renders there and scales into the back buffer.
        const std::uint64_t size = render_size_.load(std::memory_order_acquire);
        const auto render_w = static_cast<std::uint32_t>(size >> 32), render_h = static_cast<std::uint32_t>(size);
        // The picture keeps one shape on every frame (gekko/display_geometry.h): a frame's own 4:3 content (an in-engine
        // cutscene) is boxed inside it, never given another shape of its own.
        output.picture_aspect = present_aspect_.load(std::memory_order_acquire);
        if (render_w != 0u && render_h != 0u && (render_w != output.width || render_h != output.height)) {
            output.present_width = output.width;
            output.present_height = output.height;
            output.width = render_w;
            output.height = render_h;
        }
        // Diagnostics (particle-trim study, docs/PERF_60FPS_2026-11-04.md): SHADOW_R2_PARTICLE_AUDIT=<csv> writes one row
        // per particle draw - the state that decides whether a texel outside a crop could still change the picture.
        if (static std::FILE* audit = [] {
                const char* path = std::getenv("SHADOW_R2_PARTICLE_AUDIT");
                std::FILE* f = (path != nullptr && path[0] != 0) ? std::fopen(path, "w") : nullptr;
                if (f != nullptr)
                    std::fputs("tick,texture,src,dst,depth_test,depth_write,fog,cutoff,filter,address_u,address_v,"
                               "particle_colours,count,u0min,u0max,v0min,v0max,u1min,u1max,v1min,v1max,size_min,size_max,"
                               "rotated\n",
                               f);
                return f;
            }();
            audit != nullptr) {
            for (const DrawRecord& draw : packet.draws) {
                if (draw.particles == kNoParticles || draw.particles >= packet.particle_batches.size()) continue;
                const ParticleBatch& b = packet.particle_batches[draw.particles];
                float u0[2] = {1e9f, -1e9f}, v0[2] = {1e9f, -1e9f}, u1[2] = {1e9f, -1e9f}, v1[2] = {1e9f, -1e9f};
                float smin = 1e9f, smax = 0.0f;
                std::uint32_t rotated = 0;
                const std::uint32_t last = std::min<std::uint32_t>(b.first + b.count, static_cast<std::uint32_t>(packet.particles.size()));
                for (std::uint32_t i = b.first; i < last; ++i) {
                    const Particle& p = packet.particles[i];
                    u0[0] = std::min(u0[0], p.uv0[0]); u0[1] = std::max(u0[1], p.uv0[0]);
                    v0[0] = std::min(v0[0], p.uv0[1]); v0[1] = std::max(v0[1], p.uv0[1]);
                    u1[0] = std::min(u1[0], p.uv1[0]); u1[1] = std::max(u1[1], p.uv1[0]);
                    v1[0] = std::min(v1[0], p.uv1[1]); v1[1] = std::max(v1[1], p.uv1[1]);
                    smin = std::min({smin, p.size[0], p.size[1]});
                    smax = std::max({smax, p.size[0], p.size[1]});
                    rotated += p.rotation != 0.0f ? 1u : 0u;
                }
                const Recipe& r = draw.recipe;
                std::fprintf(audit, "%llu,%u,%d,%d,%d,%d,%d,%g,%d,%d,%d,%d,%u,%g,%g,%g,%g,%g,%g,%g,%g,%g,%g,%u\n",
                             static_cast<unsigned long long>(packet.tick), b.texture.value, static_cast<int>(r.src),
                             static_cast<int>(r.dst), r.depth_test ? 1 : 0, r.depth_write ? 1 : 0, r.fog ? 1 : 0,
                             r.alpha_cutoff, static_cast<int>(b.sampler.filter), static_cast<int>(b.sampler.u),
                             static_cast<int>(b.sampler.v), b.particle_colours ? 1 : 0, b.count, u0[0], u0[1], v0[0],
                             v0[1], u1[0], u1[1], v1[0], v1[1], smin, smax, rotated);
            }
        }
        if (present_trace::enabled()) {
            // Burst-prediction study (render/particle_fill.h): what the CPU knows about this packet before it is
            // recorded. Measurement only; a few thousand multiply-adds.
            const ParticleFill fill = estimate_particle_fill(packet, output.width, output.height);
            present_trace::record(present_trace::Ev::FrameFeatures, packet.tick, fill.particles,
                                  static_cast<std::uint64_t>(fill.pixels), packet.draws.size());
        }
        bool ok = false;
        {
            init_trace::Scope trace("record", L"frame", packet.tick);
            ok = record(frame.list, frame.slot, packet, output, &after, nullptr, nullptr, error);
        }
        GEKKO_PROFILE_PLOT("gpu/upload-bytes", static_cast<std::int64_t>(frames_.ring().used()));
        if (gpu_timer_ != nullptr)
            end_gpu_frame(frame.list);
        make_uploads_visible();  // after recording: it may have waited for resources still being made
        {
            GEKKO_PROFILE_ZONE("gpu/submit");
            init_trace::Scope trace("submit", L"frame", packet.tick);
            const std::uint64_t ends_at = frames_.submit();
            if (jit_.running())
                jit_.note_submitted(ends_at, jit_start_ns_ != 0 ? jit_start_ns_ : frame_start_ns, jit_fill_mpx_);
            // Render-ahead in bursts: VSync presents only, and not with the JIT pacer (which owns the frame's start).
            if (mode == PresentMode::VSync && BurstAhead::requested() && !JitPacer::requested()) {
                if (!burst_.running())
                    burst_.start(device_.direct().fence());
                burst_.note_submitted(ends_at, frame_start_ns);
            }
        }
        if (gpu_timer_ != nullptr)
            gpu_timer_->on_submitted();
        if (memory_trace_ != nullptr)
            memory_trace_->tick(device_, memory_counters());
        pool_.end_frame(frame.number, pool_keep_frames());
        publish_render_stats();
        if (!ok)
            return false;
    }
    back_buffer_states_[index] = after;
    if (eager) {
        GEKKO_PROFILE_ZONE("gpu/swap-wait");
        apply_burst_mode();
        // Only a credit actually taken has a time: 0 tells the caller not to publish it (last credit, hold sample).
        *credit_ns = take_credit() ? std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count()
                                   : 0;
    }
    bool presented = false;
    {
        GEKKO_PROFILE_ZONE("gpu/present");
        presented = swap_chain_.present(mode, error);
    }
    if (jit_.running()) {
        // The vblank grid the next jit_wait aims at: DXGI's last shown vblank. None (composed, occluded): no wait.
        std::uint64_t refresh = 0;
        std::int64_t vblank_ns = 0;
        if (mode == PresentMode::VSync && swap_chain_.last_vblank(refresh, vblank_ns))
            jit_.note_vblank(refresh, vblank_ns);
        else
            jit_.reset_grid();
    }
    jit_start_ns_ = 0;
    if (gpu_timer_ != nullptr)
        gpu_timer_->on_presented();
    return presented;
}

bool Renderer::render_to_image(const FramePacket& packet, std::uint32_t width, std::uint32_t height,
                               std::vector<std::uint8_t>* rgba, std::string* error, bool commit_history) {
    GEKKO_PROFILE_ZONE("gpu/render-to-image");
    SHADOW_RECORD_PACKETS(frame(packet, width, height, rgba != nullptr, commit_history));
    // C3: the packet this call reads (reading()), until it returns.
    struct ReadEnd {
        std::atomic<const FramePacket*>& slot;
        const FramePacket* before;
        ~ReadEnd() { slot.store(before, std::memory_order_release); }
    } read_end{reading_, reading_.exchange(&packet, std::memory_order_acq_rel)};
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    UINT rows = 0;
    UINT64 row_bytes = 0, total = 0;
    device_.d3d()->GetCopyableFootprints(&desc, 0, 1, 0, &layout, &rows, &row_bytes, &total);
    std::lock_guard<std::mutex> render_lock(render_mutex_);
    apply_quality();  // MSAA / anisotropic filtering changes, from this frame
    GpuBuffer capture;
    if (rgba != nullptr && !create_buffer(device_, total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                                          D3D12_RESOURCE_STATE_COPY_DEST, L"shadow.capture", &capture, error))
        return false;
    const FrameLoop::Frame frame = frames_.begin();
    if (gpu_timer_ != nullptr)
        gpu_timer_->begin_frame(frame.list, frame.slot, frame.number, packet.tick, packet.game_ticks, true);
    Output output;
    output.width = width;
    output.height = height;
    output.picture_aspect = present_aspect_.load(std::memory_order_acquire);
    // G10's proof hook (SHADOW_R2_CAPTURE_AT_RENDER_SIZE, set_capture_at_render_size): the capture is rendered at the
    // render size and scaled into width x height by the presented path's own pass.
    if (capture_at_render_size_.load(std::memory_order_acquire)) {
        const std::uint64_t size = render_size_.load(std::memory_order_acquire);
        const auto render_w = static_cast<std::uint32_t>(size >> 32), render_h = static_cast<std::uint32_t>(size);
        if (render_w != 0u && render_h != 0u && (render_w != width || render_h != height)) {
            output.present_width = width;
            output.present_height = height;
            output.width = render_w;
            output.height = render_h;
        }
    }
    const bool ok = record(frame.list, frame.slot, packet, output, nullptr, rgba != nullptr ? &capture : nullptr,
                           rgba != nullptr ? &layout : nullptr, error, commit_history);
    if (gpu_timer_ != nullptr)
        end_gpu_frame(frame.list);
    make_uploads_visible();
    const std::uint64_t submitted = frames_.submit();
    if (gpu_timer_ != nullptr)
        gpu_timer_->on_submitted();
    if (memory_trace_ != nullptr)
        memory_trace_->tick(device_, memory_counters());
    device_.direct().wait(submitted);
    pool_.end_frame(frame.number, pool_keep_frames());
    publish_render_stats();
    if (rgba == nullptr)
        return ok;
    if (!ok) {
        retire(device_, capture);
        return false;
    }
    rgba->resize(std::size_t{width} * height * 4);
    void* mapped = nullptr;
    const D3D12_RANGE everything{0, static_cast<SIZE_T>(total)};
    if (FAILED(capture.resource->Map(0, &everything, &mapped))) {
        retire(device_, capture);
        return fail(error, "could not map the capture");
    }
    gpu_memory::note_map(L"shadow.capture", mapped, total);
    for (std::uint32_t y = 0; y < height; ++y)
        std::memcpy(rgba->data() + std::size_t{y} * width * 4,
                    static_cast<const std::uint8_t*>(mapped) + layout.Offset + std::size_t{y} * layout.Footprint.RowPitch,
                    std::size_t{width} * 4);
    const D3D12_RANGE nothing{0, 0};
    gpu_memory::note_unmap(mapped);
    capture.resource->Unmap(0, &nothing);
    retire(device_, capture);
    return true;
}

void Renderer::invalidate_histories() {
    std::lock_guard<std::mutex> lock(render_mutex_);
    SHADOW_RECORD_PACKETS(invalidate_histories());
    for (History& history : histories_)
        history.valid = false;
}

void set_fold_under_override(int forced) {
    g_fold_under_forced.store(forced, std::memory_order_relaxed);
}

}  // namespace shadow::render
