#pragma once

// The new native bloom ("glare") system: its sizing rule, its gain chain, its
// tap-set generator, and a CPU reference of the whole pass chain.
//
// This header is the single authority for the bloom's mathematics. The GPU
// implementation renders the same formulas; the CPU reference here is what
// proves it, pass by pass, without a device. Nothing in this file depends on
// the renderer, on Direct3D, on the guest, or on the internal render scale.
//
// Specification: docs/effects-2026-09-21/BLOOM_SPEC.md. The behavioural facts
// that shaped it, each measured from the PAL DOL:
//
//   * There is NO luminance threshold and NO bright-pass. The original's only
//     fragment arithmetic is one TEV stage - GX_MODULATE when a texture is
//     bound, GX_PASSCLR when not (guest 0x804914EC, 0x80491574, 0x804915DC).
//     What blooms is decided by RENDER-LIST MEMBERSHIP, upstream of this file.
//   * The parameters are compiled-in constants, not authored stage data: the
//     ScreenEffect constructor installs them inline at 0x80431828-0x80431878
//     when its parameter argument is NULL, and no .one archive on the disc
//     carries a GLARE record. They are still read every frame, because a
//     temporal channel exists in code that can lerp them.
//   * The 8-bit saturation IS the look. Level A's total gain is ~1.0588 and
//     level B's ~4.7059, so level B clips once the glare source exceeds about
//     51/255. Everything here is unsigned 8-bit with saturating adds for that
//     reason; a float chain without an explicit clamp changes the appearance.
//   * Taps per half are `extra + 1`, not `extra`: each half emits an
//     unconditional centre tap before its loop (guest 0x80430450, 0x804306B8).
//   * The blur border is ZERO, not clamp-to-edge. The original shortens the
//     quad rather than clamping the UV, so a destination texel whose source
//     index falls outside the level receives nothing. This is what makes the
//     halo fade at the frame edge instead of smearing, and it is visible.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "gekko/types.h"

namespace gekko::bloom {

// The guest's parameter block G+0xE0..0xFC, host-side. Field names follow the
// specification's table; the shipped values on every stage measured are
// enable/a/b = 1, separate = 0, b_from_source = 0, intensity 32/48, spreads
// 1.0/1.0, extra taps 2/2 and 4/4.
struct Parameters {
    bool enable = true;
    bool enable_a = true;
    bool enable_b = true;
    // +0xE3: composite A and B separately instead of merging B into A first.
    bool separate = false;
    // +0xE4: level B captures the full-resolution source instead of cascading
    // from level A. This also selects which capture gain level B uses.
    bool b_from_source = false;
    u8 intensity_a = 0x20;
    u8 intensity_b = 0x30;
    float spread_a = 1.0f;
    float spread_b = 1.0f;
    std::int32_t taps_ah = 2;
    std::int32_t taps_av = 2;
    std::int32_t taps_bh = 4;
    std::int32_t taps_bv = 4;
};

// The guest's own gate, from the two entry points' early returns.
constexpr bool gate_open(const Parameters& p) noexcept {
    return p.enable && (p.enable_a || p.enable_b);
}

// ---------------------------------------------------------------------------
// Sizing (specification rule R1)
// ---------------------------------------------------------------------------
//
// The original's 128x128 and 64x64 are a deliberate low-pass, not a memory
// limit, so the new system keeps the kernel's ANGULAR size rather than its
// texel count. A level-A texel is H/96 wide by H/128 tall at every aspect,
// which reproduces exactly 128x128 at 4:3 and preserves the original's
// 1.3333:1 screen-space anisotropy everywhere else.
//
// Dimensions are forced even so that level B is exactly half of level A and
// the cascade's 2x2 box is exact.
struct LevelSizes {
    u32 a_width = 0;
    u32 a_height = 0;
    u32 b_width = 0;
    u32 b_height = 0;

    constexpr bool valid() const noexcept {
        return a_width >= 2u && a_height >= 2u && b_width >= 1u && b_height >= 1u;
    }
};

// `quality` is a multiplier on the level resolution; 1 is the console.
LevelSizes level_sizes(u32 width, u32 height, u32 quality = 1u) noexcept;

// ---------------------------------------------------------------------------
// The gain chain
// ---------------------------------------------------------------------------
//
// Level A's capture multiplies the authored intensity by 0.94999998807907104
// and TRUNCATES to an integer before dividing by 255 (guest fmuls 0x8042F2CC,
// fctiwz 0x8042F2D0). The cascade arm of level B does NOT apply that 0.95: it
// stores the raw byte (lbz 0x8042F690 straight to stb). The two direct-from-
// source arms do apply it. Reproducing that asymmetry is deliberate.
float capture_gain(u8 intensity, bool apply_legacy_scale) noexcept;

inline float capture_gain_a(const Parameters& p) noexcept {
    return capture_gain(p.intensity_a, true);
}
inline float capture_gain_b(const Parameters& p) noexcept {
    // Cascade (the shipped configuration) takes the raw byte.
    return capture_gain(p.intensity_b, p.b_from_source);
}

// Taps per half, including the unconditional centre tap.
constexpr std::int32_t taps_per_half(std::int32_t extra) noexcept { return extra < 0 ? 1 : extra + 1; }

// The total gain a level applies to its source, which is what makes level B
// clip. Both halves are unweighted sums, so the gain multiplies.
float level_gain(float capture, std::int32_t extra_h, std::int32_t extra_v) noexcept;

// ---------------------------------------------------------------------------
// The tap-set generator
// ---------------------------------------------------------------------------
//
// TapSet(extra, spread) = { 0 } then, for i in [0, extra):
//     offset_i = (floor(i / 2) + 1) * sign_i * spread,   sign alternating from +
//
// so extra = 2 gives { 0, +s, -s } and extra = 4 gives { 0, +s, -s, +2s, -2s }.
// At an ODD `extra` the set is asymmetric - { 0, +s, -s, +2s } for 3 - and that
// asymmetry is reproduced on purpose. Offsets are in texels of THIS LEVEL's own
// render target, never in guest units and never scaled by the internal scale.
void tap_offsets(std::int32_t extra, float spread, std::vector<float>& out);

// ---------------------------------------------------------------------------
// CPU reference of the pass chain
// ---------------------------------------------------------------------------

// Straight 8-bit RGBA, row-major, no padding.
struct Image {
    u32 width = 0;
    u32 height = 0;
    std::vector<u8> rgba;

    Image() = default;
    Image(u32 w, u32 h) : width(w), height(h), rgba(std::size_t(w) * h * 4u, 0u) {}

    u8* at(u32 x, u32 y) noexcept { return rgba.data() + (std::size_t(y) * width + x) * 4u; }
    const u8* at(u32 x, u32 y) const noexcept {
        return rgba.data() + (std::size_t(y) * width + x) * 4u;
    }
};

// How a level is captured from its source.
enum class Capture {
    // The exact continuous box over the destination texel's source footprint,
    // with fractional weights at the partial end texels. This is the one place
    // the new system deliberately changes pixels: the box and the original's
    // single probe have the SAME first moment, so this changes variance, not
    // brightness, and it is what removes the sampling lattice.
    Box,
    // Parity arm: the original's single bilinear tap at the footprint centre.
    Probe,
};

Image downsample(const Image& src, u32 dst_width, u32 dst_height, float gain, Capture capture);

// One separable half. `horizontal` selects the axis. Unweighted, unnormalised,
// saturating, ZERO border.
Image blur_axis(const Image& src, const std::vector<float>& taps, bool horizontal);

// LA + BilinearUp2x(LB), saturating. The upsample samples LB at
// uv = (k + 0.5) / LA_extent, the half-texel-correct convention, which places
// level B's contribution on level A's texel exactly - the alignment the
// original bought with a half-texel displacement of the whole bloom.
Image merge_up2x(const Image& level_a, const Image& level_b);

// scene = saturate(scene + glare_source + magnify(level)). `glare_source` is
// sampled 1:1 and must match the scene's dimensions.
void composite(Image& scene, const Image& glare_source, const Image& level);

// The whole chain, from the glare source to the finished level that P9 will
// magnify. Returns an empty image when the gate is closed.
Image run(const Image& glare_source, const Parameters& parameters, u32 quality = 1u,
          Capture capture = Capture::Box);

}  // namespace gekko::bloom
