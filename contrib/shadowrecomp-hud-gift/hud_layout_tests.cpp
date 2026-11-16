// Standalone checks for hud_layout + hud_anchor_affine. Build: c++ -std=c++17 hud_layout.cpp hud_layout_tests.cpp
// Canvas coordinates below are measured from Shadow the Hedgehog's own HUD (GameCube build, 640 x 480 canvas).
#include "hud_anchor_affine.h"
#include "hud_layout.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace shadow::ui;

static int failures = 0;
static void require(bool ok, const char* what) {
    if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}

int main() {
    // Per-draw classification (ungrouped draws).
    require(classify_hud_anchor({20, 16, 90, 70}, true) == HudAnchor::TopLeft, "lives stay top-left");
    require(classify_hud_anchor({250, 8, 390, 48}, true) == HudAnchor::Top, "time stays top-centre");
    require(classify_hud_anchor({12, 390, 170, 468}, true) == HudAnchor::BottomLeft, "hero meter bottom-left");
    require(classify_hud_anchor({470, 390, 628, 468}, true) == HudAnchor::BottomRight, "dark meter bottom-right");
    require(classify_hud_anchor({0, 0, 640, 480}, true) == HudAnchor::Canvas, "textured full-screen stays 4:3");
    require(classify_hud_anchor({0, 0, 640, 480}, false) == HudAnchor::Fill, "untextured full-screen fills (fades)");

    // Coverage is per axis: a fade curtain drawn as 64-wide columns, bands of any height.
    require(spans_canvas_width({0, 0, 640, 40}), "a full-width band 40 tall covers horizontally");
    require(!spans_canvas_width({0, 0, 64, 480}), "one curtain column is not full width");
    require(spans_canvas_height({0, 0, 64, 480}), "but it does span the height");
    require(!spans_canvas_width({26.5f, 100, 614.5f, 380}), "an inset dialog 92% wide is content, not a backdrop");

    // Group anchors: a widget wholly in one half belongs to that side, however close to the middle.
    require(classify_group_anchor({197.5f, 40, 229.5f, 80}, HudAnchor::Center, false) == HudAnchor::Left,
            "meter icon capping the Hero gauge stays with it");
    require(classify_group_anchor({415.5f, 40, 447.5f, 80}, HudAnchor::Center, false) == HudAnchor::Right,
            "and the Dark gauge's");
    require(classify_group_anchor({24, 60, 620, 420}, HudAnchor::Center, false) == HudAnchor::Center,
            "a menu across the frame stays a centred safe area");

    // Text runs report one alignment point: thirds, no hysteresis.
    auto point = [](float x) { return HudBounds{x, 0, x, 0}; };
    for (float x : {54.f, 78.f, 84.f, 120.f})
        require(classify_group_anchor(point(x), HudAnchor::Center, false) == HudAnchor::Left, "left HUD text");
    require(classify_group_anchor(point(320), HudAnchor::Center, false) == HudAnchor::Center, "timer centred");
    for (float x : {477.f, 520.f, 560.f})
        require(classify_group_anchor(point(x), HudAnchor::Center, false) == HudAnchor::Right, "right HUD text");

    // Hysteresis for widgets that slide in: change side only once decisively there.
    require(classify_group_anchor({30, 0, 300, 40}, HudAnchor::Center, true) == HudAnchor::Center,
            "drifting into the left half is not enough");
    require(classify_group_anchor({30, 0, 250, 40}, HudAnchor::Center, true) == HudAnchor::Left,
            "a decisive move is");
    require(classify_group_anchor({120, 200, 520, 280}, HudAnchor::Left, true) == HudAnchor::Center,
            "a centred card does not inherit an edge anchor");

    // Per-cast anchoring: one CSD project drawing two panels (mission objective window: counters at X 24 and 545).
    {
        HudLayout layout;
        for (int frame = 0; frame < 2; ++frame) {
            layout.on_group(HudGroupKind::CsdWidgetPerCast, 9u, 1u, 0.0f);
            layout.on_scene(0u, 1u);
            layout.layout_draw({10, 60, 200, 110}, true);
            layout.on_scene(0u, 2u);
            layout.layout_draw({440, 60, 630, 110}, true);
            layout.end_frame();
        }
        layout.on_group(HudGroupKind::CsdWidgetPerCast, 9u, 1u, 0.0f);
        layout.on_scene(0u, 1u);
        require(layout.layout_draw({10, 60, 200, 110}, true).anchor == HudAnchor::Left, "left panel goes left");
        layout.on_scene(0u, 2u);
        require(layout.layout_draw({440, 60, 630, 110}, true).anchor == HudAnchor::Right, "right panel goes right");
        layout.end_frame();
    }
    // A widget's invisible (alpha 0) part does not decide where it goes.
    {
        HudLayout layout;
        for (int frame = 0; frame < 2; ++frame) {
            layout.on_group(HudGroupKind::CsdWidget, 3u, 7u, 0.0f);
            layout.layout_draw({150, 150, 500, 330}, true, /*shapes_anchor=*/false);  // hidden big panel
            layout.layout_draw({500, 40, 620, 90}, true);                              // visible small panel
            layout.end_frame();
        }
        layout.on_group(HudGroupKind::CsdWidget, 3u, 7u, 0.0f);
        require(layout.layout_draw({500, 40, 620, 90}, true).anchor == HudAnchor::Right, "hidden draws do not pull");
        layout.end_frame();
    }
    // A fade curtain of ten columns fills the whole output; the loading graphic on top is not a column.
    {
        HudLayout layout;
        layout.on_group(HudGroupKind::ScreenCover, 5u, 0u, 0.0f);
        require(layout.layout_draw({0, 0, 64, 480}, false).anchor == HudAnchor::Fill, "curtain column fills");
        require(layout.layout_draw({576, 0, 640, 480}, false).anchor == HudAnchor::Fill, "last column fills");
        layout.end_frame();
    }
    // A sprite zoomed past the canvas edges (title logo after-images) stays with its widget.
    {
        HudLayout layout;
        layout.on_group(HudGroupKind::CsdWidget, 7u, 0u, 320.0f);
        require(layout.layout_draw({31, 127, 543, 255}, true).anchor == HudAnchor::Center, "wordmark centred");
        const auto d = layout.layout_draw({-125, 95, 699, 287}, true);
        require(d.anchor == HudAnchor::Center && d.beyond_canvas, "zoomed after-image stays with it");
        layout.end_frame();
    }
    // Split screen: draws inside one player's viewport anchor inside that viewport.
    {
        HudLayout layout;
        const HudLayout::Region regions[2] = {{0, 0, 320, 480}, {320, 0, 640, 480}};
        layout.set_regions(regions, 2, 640, 480);
        layout.on_group(HudGroupKind::Text, 11u, 0u, 520.0f);
        const auto d = layout.layout_draw({500, 400, 540, 420}, true);
        require(d.region == 1 && d.anchor == HudAnchor::Right, "2P counter on its own window's side");
        layout.end_frame();
    }

    // Placement at 16:9: left edge stays at x=0, right edge stays at x=640, centre keeps 320, art is squeezed by 0.75.
    {
        HudPlacementSettings s;
        s.output_aspect = 16.0f / 9.0f;
        const HudAffine l = hud_affine_for_anchor(HudAnchor::BottomLeft, s);
        const HudAffine r = hud_affine_for_anchor(HudAnchor::BottomRight, s);
        const HudAffine c = hud_affine_for_anchor(HudAnchor::Center, s);
        auto near = [](float a, float b) { return std::fabs(a - b) < 1e-3f; };
        require(near(l.ax, 0.75f) && near(l.bx, 0.0f), "left anchor pinned to the left edge");
        require(near(r.ax * 640 + r.bx, 640.0f), "right anchor pinned to the right edge");
        require(near(c.ax * 320 + c.bx, 320.0f), "centre stays centred");
        const HudAffine f = hud_affine_for_anchor(HudAnchor::Fill, s);
        require(f.ax == 1 && f.bx == 0, "fill is identity (covers the stretched output)");
        s.output_aspect = 4.0f / 3.0f;
        require(hud_affine_for_anchor(HudAnchor::Left, s).ax == 1.0f, "4:3 is byte-identical");
    }

    std::printf(failures ? "%d FAILED\n" : "all hud_layout checks passed\n", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
