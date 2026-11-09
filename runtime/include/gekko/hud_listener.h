#pragma once

// The 2D layer's identity announcements (System's recovered observation hooks; docs/2D_LAYOUT_ARCHITECTURE.md 2):
// which logical group - a glyph run, a CSD widget, the fade's tiling - and which authored cast the screen-space draws
// that follow belong to. Whatever lays the 2D out for the real output listens: the old renderer's capture, or in the
// SHADOW_RENDER_LIFTED build the R2 driver layer (runtime/scaffold/rw_driver). Both calls come on the game thread,
// in draw order. Goes with the hooks in R4, when the 2D layer's lift carries its anchors as data.

#include "gekko/gx.h"
#include "gekko/types.h"

namespace gekko {

class HudListener {
public:
    virtual void on_hud_group(HudGroupKind kind, u32 group_id, float canvas_x, u32 instance) = 0;
    virtual void on_hud_scene(u32 node, u32 cast) = 0;

protected:
    ~HudListener() = default;
};

}  // namespace gekko
