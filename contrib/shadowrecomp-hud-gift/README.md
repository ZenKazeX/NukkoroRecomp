# HUD alignment for Shadow the Hedgehog — a gift from Nukkoro Recompiled

Hi FoxWorkshop team! We are [Nukkoro Recompiled](https://github.com/ZenKazeX/NukkoroRecomp), a static recompilation
of the **GameCube** version of Shadow the Hedgehog. Both ports draw the same 640 x 480 CSD HUD, so we wanted to share
the 2D layout policy we use to keep it aligned on widescreen and ultrawide outputs. Take whatever is useful.

**No license, no conditions.** Use it, change it, ship it, no credit needed. Made for everyone who loves Shadow.

Your hand-tuned CSD path table already does more than ours in many places. This package is meant to sit **under** it:
an automatic layer for whatever the table leaves to `CSD_ALIGN_AUTO_H`, plus a few edge cases we measured.

## What's in the box

| file | what |
|---|---|
| `hud_layout.h` / `hud_layout.cpp` | The layout policy, fully commented. Standard C++17 only: no guest memory, no renderer. A cleaned-up version of what ships in Nukkoro, checked to give identical decisions (200k+ randomized draws). |
| `hud_anchor_affine.h` | Turns an anchor into `x' = ax*x + bx` (Hor+ squeeze, edge pinning, HUD scale, safe-area inset). |
| `hud_layout_tests.cpp` | 40+ checks using canvas coordinates measured from the game. `c++ -std=c++17 hud_layout.cpp hud_layout_tests.cpp && ./a.out` |

## The ideas, and the bugs each one fixes

1. **Classify the whole widget, not each quad.** `ResolveAnchorX` with `CSD_ALIGN_AUTO_H` applies the centroid-thirds
   rule to each primitive. A widget's pieces then land in different thirds and come apart. Two examples: the gauge's
   meter icon at x 197.5..229.5 sits half a unit from the left third, and a long bar's centre falls in the middle third
   while its caps don't. `HudLayout` collects the bounds of everything drawn between "group open" and "group close"
   (one CSD project render) and decides **once** per group.
2. **Halves before thirds for extents.** A group that lies wholly in one half of the canvas belongs to that side,
   however close to the middle it reaches (`classify_group_anchor`). Thirds are kept only for text runs, which report
   one alignment point rather than an extent. The HUD's text X values (54, 78, 84, 120 / 320 / 477, 520, 560) clear
   both thirds easily.
3. **Remember anchors across frames, with hysteresis.** Anchors are kept per (draw site, instance). A widget changes
   side only after its centre has moved a quarter of the canvas past the middle. Without this, entrance animations that
   slide across a boundary snap halfway through.
4. **Per-cast anchoring for projects that draw several panels.** The mission objective window is one project that
   draws two panels, with counters at X = 24 and X = 545. Taken as one unit, the project straddles the middle and goes
   to Center while its counters go Left and Right, so the counters leave their panels. `CsdWidgetPerCast` anchors each
   cast on its own. A cast that straddles the middle still falls back to Center, so the change is safe.
5. **Invisible draws don't vote.** Every frame, the Egg Dealer slot display draws a hidden large panel at vertex
   alpha 0 across the middle. Counting it held the visible small panel in the centre. `layout_draw(..., shapes_anchor = false)`
   makes the draw follow its group's anchor without widening the group's bounds.
6. **Coverage means reaching both edges, decided per axis.** The fade curtain is ten 64 x 480 columns, so no single
   quad looks full-screen, and the outer strips of a 16:9 frame stayed unfaded. The `ScreenCover` group makes every
   column fill. On the other side, the autosave dialog is 588 units wide (92% of the canvas) but inset on both sides.
   It is content, not a backdrop, and must not be stretched away from its own 4-unit side borders.
7. **Textured full-canvas draws stay a centred 4:3 canvas. Untextured ones fill.** Menus and backgrounds keep their
   original composition, while fades and letterbox bands cover the whole output.
8. **Zoomed sprites past the edges stay with their widget.** The title's after-images of the SHADOW wordmark scale
   past both canvas sides (for example -125..699). They are content larger than the screen, not bands, so they keep
   the wordmark's anchor and are clipped at the view edge (`Decision::beyond_canvas`).
9. **Split screen.** `set_regions` gives each player's viewport, and HUD draws inside one viewport are anchored inside
   it. Text there uses halves of the region, because the 2P ammo counters at x 120 / 520 would fall in the middle
   thirds. Anything drawn across the split, such as the 2P timer, keeps the whole canvas.
10. **The canvas is a parameter.** Every rule takes the canvas it measures in (for example 640 x 528 PAL, or a
    640 x 240 split view), not a hard-coded 640 x 480.

## Hooking it into ShadowRecomp (sketch)

```cpp
static shadow::ui::HudLayout g_hudLayout;

// Project render begins (where g_sceneModifier is resolved):
g_hudLayout.on_group(shadow::ui::HudGroupKind::CsdWidget, /*site*/ projectHash, /*instance*/ projectPtr, 0.0f);
// Each cast (RenderCsdCastMidAsmHook):
g_hudLayout.on_scene(castNodeIndex, castPtr);
// Draw(): only when the table left the quad on CSD_ALIGN_AUTO_H
shadow::ui::HudBounds b = boundsOf(vertexData, CSDVertexCount, stride);
bool visible = anyVertexAlpha(vertexData) > 0;
auto d = g_hudLayout.layout_draw(b, /*textured*/ true, visible);
if (d.laid_out) {
    auto a = shadow::ui::hud_affine_for_anchor(d.anchor, {g_aspectRatio, hudScale});
    // x = a.ax * x + a.bx; y = a.ay * y + a.by  (canvas units, before your 640x480 -> viewport mapping)
}
// HUD text (BeginHudNumber): on_group(HudGroupKind::Text, site, 0, x) before the glyphs.
// Once per frame, after the 2D pass:
g_hudLayout.end_frame();
```

You could also leave your own math alone and use only `classify_group_anchor` (ideas 1–3) to choose between
`frame.Left` / centre / `frame.Right` in `ResolveAnchorX`.

Happy 21st birthday, Shadow — from one recomp to another. 🦔
