#include "gekko/native_hud.h"

#include "gekko/profiling.h"

namespace gekko {

// LEGACY RENDERER ONLY (native_hud.h). The policy is shadow::ui::HudLayout (NE-RW2c).

void NativeHudLayout::end_frame() noexcept {
    // Called from Op::Present, so this is the one point that runs exactly once per displayed frame - which is why the
    // HUD plots are published from here rather than from layout_draw, where a zone would be a third of what it
    // measures.
    GEKKO_PROFILE_ZONE("gpu/hud frame end");
    policy_.end_frame();
    GEKKO_PROFILE_ONLY(if (GEKKO_PROFILE_CONNECTED()) {
        const shadow::ui::HudLayout::Stats& s = policy_.stats();
        GEKKO_PROFILE_PLOT("gpu/hud laid out", static_cast<double>(s.laid_out));
        GEKKO_PROFILE_PLOT("gpu/hud off canvas", static_cast<double>(s.off_canvas));
        GEKKO_PROFILE_PLOT("gpu/hud group memory", static_cast<double>(policy_.group_memory_size()));
        GEKKO_PROFILE_PLOT("gpu/hud cast memory", static_cast<double>(policy_.cast_memory_size()));
        GEKKO_PROFILE_PLOT("gpu/hud group memory capped", static_cast<double>(s.group_memory_capped));
        GEKKO_PROFILE_PLOT("gpu/hud groups open at frame end", static_cast<double>(s.groups_open_at_frame_end));
    })
}

NativeHudLayout::Decision NativeHudLayout::layout_draw(const HudBounds& bounds, bool textured,
                                                       bool shapes_anchor) noexcept {
    const shadow::ui::HudLayout::Decision d = policy_.layout_draw(bounds, textured, shapes_anchor);
    Decision decision;
    decision.laid_out = d.laid_out;
    decision.anchor = d.anchor;
    decision.region = d.region;
    if (d.laid_out)
        decision.affine = hud_affine_for_anchor(d.anchor, layout_);
    return decision;
}

}  // namespace gekko
