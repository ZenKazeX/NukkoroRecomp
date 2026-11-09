#pragma once

// LEGACY RENDERER ONLY (G5 keeps it out of the lifted build): the legacy GX/EFB replay renderer's view of the game's
// 2D layout policy. NE-RW2c moved the policy itself to runtime/lift/ui/hud_layout.h (shadow::ui::HudLayout, plain
// values); this keeps the legacy renderer's canvas affine on top of it (display.h hud_affine_for_anchor) and the
// Tracy plots it published once per displayed frame. Deleted with the legacy renderer.

#include "gekko/display.h"
#include "gekko/gx.h"
#include "gekko/types.h"
#include "ui/hud_layout.h"

namespace gekko {

class NativeHudLayout {
public:
    void set_layout(const DisplayLayout& layout) noexcept { layout_ = layout; }
    const DisplayLayout& layout() const noexcept { return layout_; }

    void on_group(HudGroupKind kind, u32 group_id, u32 instance, float canvas_x) noexcept {
        policy_.on_group(kind, group_id, instance, canvas_x);
    }
    void on_scene(u32 node, u32 cast) noexcept { policy_.on_scene(node, cast); }
    void end_frame() noexcept;

    // The policy's decision plus the legacy renderer's canvas affine for its anchor (identity when not laid out).
    struct Decision {
        HudAffine affine{};
        bool laid_out = false;
        HudAnchor anchor = HudAnchor::Center;
        int region = -1;
    };
    Decision layout_draw(const HudBounds& bounds, bool textured, bool shapes_anchor = true) noexcept;

    const shadow::ui::HudLayout::Stats& stats() const noexcept { return policy_.stats(); }

private:
    shadow::ui::HudLayout policy_;
    DisplayLayout layout_{};
};

}  // namespace gekko
