// SCAFFOLDING (R2): the glare's two copies as the boundaries of the native glare - deleted by R5: the render tree's lift owns the glare pass and its parameters
#include <algorithm>
#include <cmath>
#include <cstring>

#include "gekko/cpu.h"
#include "rw_driver.h"

// r2/SITES_PRESENT_EFFECTS.md 3.1, docs/effects-2026-09-21/BLOOM_SPEC.md 2.5-2.14 and 3.1, R2_DRIVER.md 4.6. The
// glare workers stay translated - their gate (G+0x08, E0, E1 | E2), the camera they fetch through the effect system,
// their state saves and restores - and draw nothing: their quads, clears and level copies are lowered no-ops. Two of
// their copies mark what the glare means:
//   copy #1 (PreGlare 0x80430D64, site 0x80430FAC): the finished scene is saved and the glare sources are drawn next,
//            onto black over the scene's depth;
//   copy #2 (PostGlare 0x80430904, site 0x80430B58): the glare-source image is taken; the glare of the sources is
//            added to the scene before anything else is drawn.
// Each copy runs only when its worker passed the gate, so the window is exactly the original's.

namespace gekko::scaffold {

namespace render = shadow::render;

namespace {

u32 glare_object(Cpu& c) {
    const u32 manager = c.mem_read32(layout::kEffectManager);
    return manager != 0u ? c.mem_read32(manager + layout::kEffectGlare) : 0u;
}

// The parameter block as the ladder 0x8042F258 uses it (BLOOM_SPEC 2.8, 2.13): gains truncated from the byte times
// 0.95 (the constant at 0x8056BCF0), except level B's cascade arm, which takes the byte as it is.
render::GlareParams glare_params(Cpu& c, u32 g) {
    render::GlareParams p;
    const u8 level_a = c.mem_read8(g + layout::kGlareEnable + 1u);
    const u8 level_b = c.mem_read8(g + layout::kGlareEnable + 2u);
    const u8 separate = c.mem_read8(g + layout::kGlareEnable + 3u);
    const u8 b_from_sources = c.mem_read8(g + layout::kGlareEnable + 4u);
    const u8 gain_a = c.mem_read8(g + layout::kGlareEnable + 5u);
    const u8 gain_b = c.mem_read8(g + layout::kGlareEnable + 6u);
    const float scale = 0.94999998807907104f;
    const auto scaled = [scale](u8 byte) {
        return static_cast<float>(static_cast<s32>(std::trunc(static_cast<float>(byte) * scale))) / 255.0f;
    };
    p.level_a = level_a != 0u;
    p.level_b = level_b != 0u;
    p.separate = separate != 0u;
    p.b_from_sources = b_from_sources != 0u;
    p.gain_a = scaled(gain_a);
    p.gain_b = b_from_sources == 0u && level_a != 0u ? static_cast<float>(gain_b) / 255.0f : scaled(gain_b);
    const auto spread = [&](u32 offset) {
        const u32 bits = c.mem_read32(g + offset);
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    };
    p.spread_a = spread(layout::kGlareSpreadA);
    p.spread_b = spread(layout::kGlareSpreadB);
    // The tap loops run `i < count` on signed counts: a negative count runs none.
    const auto taps = [&](u32 offset) {
        return static_cast<std::uint32_t>(std::clamp<s32>(static_cast<s32>(c.mem_read32(g + offset)), 0, 64));
    };
    p.taps_a[0] = taps(layout::kGlareTapsA);
    p.taps_a[1] = taps(layout::kGlareTapsA + 4u);
    p.taps_b[0] = taps(layout::kGlareTapsB);
    p.taps_b[1] = taps(layout::kGlareTapsB + 4u);
    return p;
}

}  // namespace

void RwDriver::glare_sources_begin(Cpu& c, u32 raster) {
    ++stats_.copies;
    const u32 g = glare_object(c);
    View* view = g != 0u ? view_for(c, c.mem_read32(g + layout::kGlareCamera)) : nullptr;
    if (view == nullptr || raster != c.mem_read32(g + layout::kGlareSceneRaster) || glare_window_) {
        ++stats_.glare_mismatches;
        return;
    }
    glare_window_ = true;
    glare_view_ = static_cast<std::size_t>(view - views_.data());
    if (pass_open_) {
        builder_.end_pass();
        pass_open_ = false;
    }
    ++stats_.glare_windows;
}

void RwDriver::glare_composite(Cpu& c, u32 raster) {
    ++stats_.copies;
    const u32 g = glare_object(c);
    if (!glare_window_ || g == 0u || raster != c.mem_read32(g + layout::kGlareSourceRaster)) {
        ++stats_.glare_mismatches;
        return;
    }
    if (pass_open_) {
        builder_.end_pass();
        pass_open_ = false;
    }
    builder_.glare(views_[glare_view_].id, glare_params(c, g));
    glare_window_ = false;
    ++stats_.glares;
}

}  // namespace gekko::scaffold
