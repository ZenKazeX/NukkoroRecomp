#pragma once

// From an anchor to a placement: the per-axis affine x' = ax * x + bx, y' = ay * y + by, in the game's 640 x 480
// canvas, for a canvas that is stretched over the whole output (as the game's 2D is). Standalone: depends only on
// hud_layout.h.
//
// The idea: with the canvas stretched to a wider output, 2D art is squeezed back by the same Hor+ factor the 3D
// projection gets (s = (4/3) / aspect, never above 1), then pushed to the edge (or kept in the middle) its anchor
// names. Center is exactly the transform a world-projected 2D element needs, because x' = 320 + s * (x - 320) is
// the canvas form of scaling clip-space x by s.

// No license, no conditions: use, change and share this however you like.

#include "hud_layout.h"

#include <algorithm>

namespace shadow::ui {

struct HudAffine {
    float ax = 1.0f, bx = 0.0f;
    float ay = 1.0f, by = 0.0f;
};

struct HudPlacementSettings {
    float output_aspect = 4.0f / 3.0f;  // the aspect the 3D is rendered for (width / height)
    float hud_scale = 1.0f;             // user HUD scale, 1 = original size
    float inset_x = 0.0f;               // safe-area inset from each anchored edge, canvas units
    float inset_y = 0.0f;
};

inline float hud_horplus_scale(float aspect) noexcept {
    // Hor+ only widens: at and below 4:3 the canvas is left untouched (a narrower output is pillarboxed instead).
    return aspect > (4.0f / 3.0f) ? (4.0f / 3.0f) / aspect : 1.0f;
}

inline HudAffine hud_affine_for_anchor(HudAnchor anchor, const HudPlacementSettings& s,
                                       float canvas_width = kHudCanvasWidth,
                                       float canvas_height = kHudCanvasHeight) noexcept {
    HudAffine a;
    // Fades and bands cover the output; Cover is a renderer's own scale-to-cover path.
    if (anchor == HudAnchor::Fill || anchor == HudAnchor::Cover) return a;

    const float sx = hud_horplus_scale(s.output_aspect) * s.hud_scale;
    const float sy = s.hud_scale;
    int h = 1, v = 1;
    switch (anchor) {
        case HudAnchor::TopLeft: h = 0; v = 0; break;
        case HudAnchor::Top: v = 0; break;
        case HudAnchor::TopRight: h = 2; v = 0; break;
        case HudAnchor::Left: h = 0; break;
        case HudAnchor::Right: h = 2; break;
        case HudAnchor::BottomLeft: h = 0; v = 2; break;
        case HudAnchor::Bottom: v = 2; break;
        case HudAnchor::BottomRight: h = 2; v = 2; break;
        default: break;  // Center, Canvas: the centred 4:3 safe area
    }
    a.ax = sx;
    a.bx = h == 0 ? s.inset_x : h == 2 ? canvas_width - s.inset_x - canvas_width * sx : canvas_width * 0.5f * (1.0f - sx);
    a.ay = sy;
    a.by = v == 0 ? s.inset_y : v == 2 ? canvas_height - s.inset_y - canvas_height * sy : canvas_height * 0.5f * (1.0f - sy);
    return a;
}

}  // namespace shadow::ui
