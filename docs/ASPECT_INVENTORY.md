# 4:3 aspect-ratio dependencies: inventory, replacements, validation (2026-10-27)

The user's request (verbatim, 2026-10-27): "Identify and reverse-engineer every remaining hardcoded 4:3 aspect-ratio
dependency across the entire game, including rendering, cameras, projection, FOV, culling, LOD, object positioning, UI,
effects, and gameplay logic. Replace all obsolete 4:3 constraints with lightweight, flexible, resolution-independent
native implementations supporting arbitrary aspect ratios. [...] Validate every replacement against the original
behavior, test multiple resolutions and aspect ratios, and investigate all discrepancies."

Out of scope by agreement: RenderWare's world-sector frustum collection (`fn_804626A8`, the BSP walk over `camera+0x94`
and the frustum box `camera+0x10C..0x120`), owned by the texture-flicker session.

Evidence levels as in NORTH_STAR_COMPACT.md: **PROVEN** (read in the disassembly or measured), **STRONGLY SUPPORTED**,
**STRUCTURAL MATCH**, **UNKNOWN**. Status: **REPLACED** (this change), **NATIVE** (replaced before this change, re-checked
here), **RETAINED** (a 4:3 value that is authored data or a decision, not a limitation - reason given), **NOT LIVE** (does
not run in the release build), **UNRELATED** (the number is not screen geometry).

## 1. Method

1. **Constant scan of the whole DOL** (scratch script, `aspect_scan.py`): every `lfs`/`lfd`/`psq_l`/`lwz` from r2 (SDA2
   0x805FA780), r13 (SDA 0x805EC500) or a `lis`-tracked base that loads 640, 480, 320, 240, 528, 264, 448, 224, 4/3,
   0.75, 1/640, 1/480, 16/9, 0.5625, 1.3195877 (single and double), and every `li`/`cmpwi`/`cmplwi` immediate 640, 480,
   320, 240, 528, 448, 456, 608. 179 sites in 70 functions. Each was disassembled and classified (sections 3-5).
2. **The screen system's own accessors**, which carry no constant: the viewport table 0x8058516C (640 x 480 rectangles
   and FOV scales), its accessors (fn_802731A4 FOV scale, fn_80272180 / fn_802732B4 player viewport, fn_80273270 /
   fn_80273288 / fn_802732A0 rect, fn_8000F850 / fn_8000F858 / fn_80063130 / fn_80063140 rect edges), the game's
   projections (fn_8000F654 to the canvas, fn_8000F57C to the camera's normalised coordinates, fn_8000F860 the camera
   window) and RenderWare's sphere test RwCameraFrustumTestSphere 0x80486718: every caller traced to what it decides.
3. **The port's own code**: every 4/3, 640, 480 and 0.75 in runtime/ (renderer, scaffold, game owners, UI, effects).

## 2. Summary

| id | where | what depends on 4:3 | status |
|---|---|---|---|
| G1 | fn_8019DA1C (vf5 of 12 enemy bases) | gameplay "is it on the screen" test against the 4:3 camera window | **REPLACED** |
| G2 | fn_80486718 callers (13) | sphere-in-frustum decisions | NATIVE (displayed frustum); 12 draw-only, 1 animation state (ElecCoreWall) |
| C1 | fn_801C2578, fn_801C28B0, fn_801C4AF8, fn_801B3584 | camera framing rules in the camera's own window | RETAINED (authored camera composition) |
| P1 | fn_8000F654 call sites (5) | world-to-canvas projection for 3D-anchored 2D | NATIVE (canvas_point) |
| P2 | gauge gate 0x800BF9D8, CSD helpers 0x8033BC6C / 0x8033BE80 | on-screen tests against the 640 x 480 rect | NATIVE (visible_screen_edge) |
| P3 | Weapon::LockonMark draw 0x800620D4 | projection + screen test | NATIVE (game owner) |
| P4 | Sprite3D functor 0x802B4A8C | projection, depth-only gate | no screen test: not a dependency |
| P5 | fn_80346908 | 2D placed at the player viewport's centre | canvas-authored, centre-anchored by the layout policy |
| R1 | camera sync fn_80271F9C -> fn_8000F860 | the renderer camera's window | NATIVE (camera_picture_window) |
| R2 | RwCameraFrustumTestSphere 0x80486718, frustum build 0x80485318 | culling cone | NATIVE (system.cpp); the BSP box is the flicker session's |
| R3 | fn_80272FA8 camera raster 640 x 480 + projection aspect 1.3195877 | the console raster | RETAINED (guest constant; nothing derives the picture from it) |
| R4 | RW ClearQuad 0x80499B14 (640 x 528 viewport/scissor/ortho) | console EFB clear | NOT LIVE (rw_camera_clear lift site) |
| U1 | lift/ui/hud_layout (kHudCanvasWidth/Height) | 2D layout rules measured in a fixed 640 x 480 | **REPLACED** (the bracket's own canvas) |
| U2 | frames.cpp kDisplayAspect, view.h kScreenAspect, camera.h kReferenceAspect | the console picture's 4:3 | RETAINED (the reference the content was authored for; every rule derives from it, nothing is limited to it) |
| E1 | game_owners.cpp lens flare share (640 x 480 literal) | the flare's view share | **REPLACED** (render::raster_view, the camera's rule) |
| E2 | screen passes (heat haze, refraction, trail, multiply) | screen copies on the 4:3 canvas | NATIVE (screens.cpp; full-screen covers widened) |
| E3 | WatchCamera::Monitor fn_80268660 (GUN Fortress security cameras) | noise sheet sized to the camera's authored window | **REPLACED** (lowered site 0x8026872C: the displayed window) |
| F1 | in-engine story events (EventManager byte r13+0x3E5D) | pictures boxed 4:3 | RETAINED: the user's decision 2026-10-26 (2P intros excluded) |
| M1 | movie blit 0x80036764 | 4:3 movie frame | NATIVE (whole, centred; prerendered content is 4:3) |
| S1 | OSFatal, VI, AX, Sofdec, UTF-8, GX register IDs | 640/480/240 as non-geometry or a dead crash screen | NOT LIVE / UNRELATED |

## 3. Gameplay, cameras and projection

### G1 - the enemies' shared on-screen test (REPLACED; PROVEN)

`fn_8019DA1C` is slot 5 of the vtables of EggPawnBase, BkSoldierBase, BkWingLargeBase, BkWingSmallBase, BkWormBase,
GunBeetleBase, GunBigfoot, EggPierrot, BkGiant, EggShadowAndroid, BkNinja and BkChaos (RTTI, tools/guest_classes.py).
It projects two world points - the enemy's own position (`fn_8017CA44(*(enemy+0x1C))`) and a second point read through
`fn_8006D83C` - with `fn_8000F57C(out, camera state, world, viewport 0)` and tests each:

    0x8019DB1C..0x8019DB60   x < -1 or x > 1 or y < -1 or y > 1 or z > 0  ->  "not on the screen"

and chooses the position it returns from the two verdicts (on screen: the point itself; off screen: a direction rotated
by one of two angles). What the returned position is used for by the enemies was not traced further (UNKNOWN); the test
itself is unambiguous. `fn_8000F57C` (decoded, 0x8000F57C..0x8000F650) is

    view = inverse(camera matrix built from the camera state) * world
    x = view.x / -(view.z * tan(fov.x / 2)) / scale.x ,  y = view.y / (view.z * tan(fov.y / 2)) / scale.y ,  z = view.z

with the FOV pair at camera object +0x24 (state +0x1C) and the viewport's FOV scale (`fn_802731A4`). So +-1 are the edges
of the camera's own window - the 4:3 picture the console displayed - whatever the output shows: on a 16:9 screen an
enemy in the side areas is "off the screen" while the player sees it.

Replacement: the two call sites (0x8019DB0C, 0x8019DB74) are lowered (extract/lift-sites-r2.txt) to
`RwDriver::displayed_screen_point` (runtime/scaffold/rw_driver/projection.cpp). It computes the same coordinates from the
camera object's world-to-view rows (+0x3C, the matrix fn_8000F654 projects with) against the window of the picture the
renderer draws and the frustum culls (`render::displayed_window` of the viewport's canvas and the output's aspect, square
pixels or `--framing game`). +-1 is then the edge of the displayed screen on every aspect; with `--framing game` at 4:3
the window is the game's own and the test is the original's. No constant is involved: the window follows the viewport
rectangle, the output aspect and the camera's FOV. A counter compares every verdict with the original formula
(section 6). `fn_8000F57C` itself is not replaced: its other callers are camera framing rules (C1).

### G2 - RwCameraFrustumTestSphere callers (NATIVE; PROVEN)

The frustum test 0x80486718 is already native (`System::native_frustum_test`, the displayed frustum). All 13 callers were
traced to what the result gates. 12 gate only `RpClumpRender` 0x80457C14 or the atomic render callback, plus re-posing
the clump's own render frame: HintRing 0x80109DDC, item box 0x8010C5F8, 0x80112C70, ScatteringRing 0x8011A268,
CircusPole 0x80130C0C, MorphDoor 0x801439B0, StretchGrass 0x8014C1C0, 0x802309F4, 0x80238E1C, ElecRollHexa 0x802662A4,
EggLastMechFeverRing 0x80358A54 and the world-sector atomic loop 0x804603E0. One also writes object state: ElecCoreWall
0x80264C64 sets its animation state 1 -> 3 (`0x80264F68 li r0,3; stw r0,0x30(r30)`) only inside the visible branch
(`0x80264CFC beq 0x802651B8`). Its readers inside the class are the draw and the per-frame update 0x8026533C (an
animation clock); the collision update 0x802652C4 does not read it. With the displayed frustum the wall's animation
advances when it is on the displayed screen - the meaning of the test. STRONGLY SUPPORTED that nothing outside the class
reads the state (readers outside the class not exhaustively traced).

### C1 - camera framing rules (RETAINED: authored camera composition; PROVEN)

`fn_801C2578`, `fn_801C28B0`, `fn_801C4AF8` (camera controllers) and `fn_801B3584` call `fn_8000F57C` to place the target
in the camera's window and then clamp it to the limits at controller +0x120 / +0x124 (`0x801C26CC..0x801C2770`) or fit
the camera distance to it (`0x801B3780..0x801B37E4`), turning the camera by `atan(tan(fov/2) * limit)`
(`0x801C2784..0x801C279C`). These are how the camera composes the authored 4:3 picture: they move the camera, they are not
a screen test. Measuring them on the displayed picture would make the camera's motion depend on the output - a 32:9
camera would let the player drift far to the sides before turning. Hor+ keeps the composition and adds view around it
(NORTH_STAR_COMPACT: "Authored camera composition, where proven, is kept as camera semantics"). Retained unchanged.

### C2 - the projection aspect 1.3195877 (RETAINED: camera authoring rule; PROVEN)

The `.sdata` float 0x805EBA30 (= 640 / 485, never written) is read only by `fn_8000F41C` (vFOV = 2 atan(tan(hFOV/2) /
1.3195877); used by the FOV setter fn_8000D988 and the camera keyframe sampler fn_8000FBFC), `fn_8000F2E8` (a diagonal FOV
of 140 deg split into hFOV / vFOV; fn_800A605C) and the camera system's constructor `fn_80272FA8` (default view window
{0.5 * 1.3195877, 0.5}; the 640 x 480 camera and Z raster sizes). The authored data are horizontal FOVs; this rule turns
them into the vertical extent of the authored picture. The renderer keeps that vertical extent and derives the horizontal
one from the displayed aspect (render/view.h `displayed_window`), so the constant never limits the picture: it defines
the authored framing, as the 4:3 reference does. Replacing it with the output aspect would crop the authored picture
vertically on wide screens (Vert-). Retained. The global vertical FOV factor 15/14 at 0x805F1238 (fn_802732D0) and the
split-screen viewport scales {0.5, 1.0} are applied alike by the camera sync and by every native consumer of the window
(`viewport_fov_scale`).

### P1-P5 - 2D attached to world points

- **P1** `fn_8000F654` (world point to canvas): all five call sites lowered to `world_to_screen` (canvas_point of the
  authored picture). NATIVE, unchanged.
- **P2** the enemy gauge gate 0x800BF9D8 and the 3D-anchored CSD helpers 0x8033BC6C / 0x8033BE80 read the viewport
  rectangle's edges for their on-screen tests: lowered to `visible_screen_*` (the displayed screen). NATIVE, unchanged.
- **P3** Weapon::LockonMark's draw 0x800620D4: a native game owner (projection and displayed-screen test). NATIVE.
- **P4** the Sprite3D functor 0x802B4A8C projects with fn_8000F654 and tests only the depth against its near / far
  (`0x802B4B30..0x802B4B48`): no screen rectangle. Not a dependency.
- **P5** `fn_80346908` places a quad at (viewport.x + 0.5 viewport.w, viewport.y + 372) (0.5 / 372 at 0x805FB348 /
  0x805FB358): a canvas-authored, centre-anchored draw laid out by the 2D layout policy. Not a dependency.

## 4. Rendering, UI and effects

### R1-R3 - camera window and culling (NATIVE)

The camera sync's window step (fn_80271F9C -> fn_8000F860) is lowered to `camera_picture_window` (the authored window of
the raster's canvas). The frustum build 0x80485318 and the sphere test 0x80486718 are native against
`render::displayed_window` (System::native_frustum_build / native_frustum_test). RenderWare's BSP frustum box
(fn_804626A8) was the flicker session's to check; its finding (2026-10-27): `_rwCameraSync` 0x804862DC computes the box at
camera+0x10C from the natively widened corners, so sector collection is already as wide as the displayed frustum. NATIVE. The 640 x 480 raster sizes written by fn_80272FA8 are the
guest's canvas; the renderer takes no picture size or aspect from them, only the canvas the 2D is authored in.

### R4 - RenderWare's ClearQuad 0x80499B14 (NOT LIVE in the release build; PROVEN)

GXSetViewport / GXSetScissor 640 x 528 and an ortho 2/639, -2/527 for a full-EFB clear. Reached only through
RwCameraClear's dispatch 0x8048682C, which the release build lowers to `rw_camera_clear` (lift-sites-r2.txt; it records
the colour). It still runs in the non-lifted Android build, where the GX path clears the whole EFB.

### U1 - the 2D layout policy's fixed canvas (REPLACED; PROVEN)

`lift/ui/hud_layout` classified every canvas draw against `kHudCanvasWidth` x `kHudCanvasHeight` = 640 x 480: anchor
thirds, "spans the canvas" (reaches both edges / 90 % of the height), edge art (a quarter of the height), off-canvas. The
canvas a draw is authored in is the bracket's camera raster - 640 x 528 in PAL 50 Hz mode, a player's 640 x 240 view in
split screen - and RESOLUTION_AUDIT.md section 6 listed the 480 as "effect not verified". `HudLayout` now takes the canvas
from the bracket (`set_regions(..., bracket.width, bracket.height)`, already called before every Im2D draw) and every
rule measures that canvas; the free classifiers take it as parameters (640 x 480 by default, for the legacy renderer).
640 x 480 brackets are classified exactly as before. Tests: gekko_core_tests `test_display_hud_anchors_and_scale` (a
240-row view, a 528-row canvas, a 320-wide bracket).

### U2 - the 4:3 reference (RETAINED: the authored aspect, not a limit)

`camera.h kReferenceAspect`, `render/view.h kScreenAspect`, `frames.cpp kDisplayAspect`, `projection.cpp
kAuthoredScreenAspect`: the console showed its 640 x 480 / 640 x 528 picture on a 4:3 television, so the canvas's units
are (4/3) / (640/480) wide. This defines the authored canvas and maps it into any view (canvas_fit, region_fit,
visible_canvas, raster_view, displayed_window); view_boundary_tests exercise these at 4:3 .. 32:9 and narrower. Nothing is
clamped or boxed to it except F1.

### E1 - the lens flare's view share (REPLACED; PROVEN)

`GameOwners::lens_flare_build` took the camera raster's share of "the game's 640 x 480 screen" with the literal
`(w * 480) / (h * 640)`, while the camera window and the frustum take the share of the raster's actual top-level raster
(`render::raster_view`). In 60 Hz mode both are 640 x 480 and the results are bit-identical (share exactly 1; canvas
aspect 4.0f/3.0f == 640.0f/480.0f; a 640 x 240 split view gives 2.6666667f both ways). With a 640 x 528 top-level raster
the literal disagreed with the camera. The flare now reads the top-level raster and uses `raster_view`.

### E2 - screen passes (NATIVE; re-checked)

Heat haze, refraction patch, motion trail and screen multiply (screens.cpp) map the guest's screen-copy UVs to canvas
points of the view and lay full-screen grids over the whole view (`Anchor::Fill`; a centred grid spanning the canvas
width is widened). The glare chain's 128 / 64-row levels are resolution-independent by design (RESOLUTION_AUDIT 3).

### E3 - the watch camera's noise sheet (REPLACED, 2026-11-05)

GUN Fortress (stg0600) has nine `WatchCamera::SetWatchCamera` objects (layout type 0x1770, only in that stage). When
one sees Shadow it takes the view (CameraModeUser), and its `Monitor` (fn_80268660) draws BS0600CAMERANOISE.DFF over
the picture:
- The model is a unit quad (corners +-0.5). Its texture `s0501_sa029` is 2 x 128, with both columns equal: one red
  scanline on transparency, tiled about 5.9 times down the quad and scrolled by the model's UV animation.
- The sheet's position is the camera position plus 1.1 x the normalised direction from it to the point
  fn_800101F8 returns (I: the camera's target). Its axes are the camera's matrix.
- Its matrix is scaled by fn_8040C770(m, 2 x (window.x x 1.1), 2 x (window.y x 1.1), 1): K(-0x19B0) = 1.1, K(-0x19AC)
  = 2. The window is the RwCamera's (+0x68 / +0x6C), so at 1.1 in front of the eye the quad's edges are the window's
  edges.

The defect:
- The RwCamera carries the authored picture's window (R1), and the renderer widens it at draw time.
- So the noise covered only the authored 4:3 part of a wider view, leaving bare side bands at 16:9, 21:9 and 32:9.
- On a narrower view (5:4, 1:1) it left bare top and bottom bands.

The replacement:
- The call at 0x8026872C is a lowered site (`watch_camera_sheet_scale`, extract/lift-sites-r2.txt), served by
  `GameOwners::watch_camera_sheet_scale` (runtime/game/game_owners.cpp).
- It sizes the sheet with the same arithmetic in the same order (runtime/lift/effects/watch_camera_sheet.h), on the
  window the view displays: `raster_view` + `displayed_window`, the rule the world is drawn and culled with.
- It then scales the matrix as fn_8040C770 does.
- At the authored aspect the size is the game's, bit for bit (view_boundary_tests, plus the
  `watch_sheet_game_mismatches` counter, which compares the game's own f1 / f2 with the reconstruction).
- The texture is constant across the sheet's width, so the wider sheet shows the same scanlines with no seam.

Diagnostics:
- `SHADOW_WATCH_SHEET_GAME=1` keeps the console's size, for an A/B on the same frame.
- `SHADOW_WATCH_SHEET_TRACE=<n>` prints the first n scales with frame and game tick.

The rain box (EFFECTS_INVENTORY P11) has the same dependency and is still open.

### F1 - in-engine story events boxed 4:3 (RETAINED: the user's decision)

`RwDriver::output_aspect()` returns 4/3 while EventManager's byte r13+0x3E5D is set and no two-player match runs, and the
frame packet's `content_aspect` boxes the views (projection.cpp `sample_event_framing`, frames.cpp). This deliberate 4:3
limit was introduced on the user's request on 2026-10-26 ("2P mode cutscenes don't count"). It is the one remaining place
where the output is forced to 4:3. The user confirmed it on 2026-10-27: "Keep the story cutscenes boxed at 4:3 as decided on
2026-10-26." (Removing it would be the single `event_framed_` test.)

### M1 - prerendered movies (NATIVE)

The movie blit 0x80036764 is lowered to `rw_movie_frame` and drawn `Anchor::Center`: the 4:3 film stays whole and
undistorted (pillarboxed). The content is 4:3 video; widening it would mean cropping or stretching it.

## 5. Not screen geometry, or not running (UNRELATED / NOT LIVE; PROVEN)

| site / function | value | meaning |
|---|---|---|
| fn_8000EFC0 (camera shake) | 0.75 f/d | phase breakpoint of the shake's triangle wave |
| fn_8006261C, fn_8007AE10, fn_80086AC0, Jump::vf2, fn_80092FE0, fn_800D3AC4 / 3CEC, fn_80151038, fn_801A84A8, fn_80288EBC, fn_801B0F04, fn_8021B244, BlackDoom / DevilDoom vf1, EggShadowAndroid HomingJump | 0.75 | ramps, an exponent, speed scales, quarter bins, a matrix scale, 135 deg, motion tuples, curves, HP > 75 % phase tests, enemy fields |
| fn_8034304C | 0.75 | a fade duration |
| fn_8034E90C | 4/3 | a duration (80 frames / 60) in a timing table |
| fn_80132764, fn_801A0854, fn_801C3168 | 240.0 | degrees per second, angles, a speed clamp |
| HeavyDog ATK_BARRIER vf1, fn_8036A714, fn_80360224, allocator wrappers, fn_8004F284, fn_80205010, fn_802BAFF0, fn_8020E80C | li 240/320/456/480/640 | a message id, `__LINE__`, object sizes, memset sizes, struct offsets, a resource id |
| fn_802EA568 | 480.0 | an enemy tuning value |
| fn_800490B0 | li 640/480 | RsGlobal maximum size default, overwritten from the video mode (no renderer reader) |
| fn_80045F48 | li 640 | a {0, 340, 640, 120} band with no reader |
| fn_800424D0 / fn_80362A3C | 640.0 / 480.0 | the CSD UI canvas (Chao::CSD screen space): canvas-authored; its draws go through the 2D layout policy |
| FadeLogo constructor 0x80044344 | 480.0 | the fade curtain, 10 columns of 64 x 480: laid over the whole view (layout policy ScreenCover) |
| fn_80201620, fn_802E51B0, fn_80343E74 | 320 / 640 | canvas-authored text and panels centred on x = 320: centre-anchored |
| fn_802BF228 / fn_802BF3C0 | 528.0 / video height | message-text placement normalised from a CSD node's canvas position (fn_8036BCE0 -> fn_80362A00): canvas to canvas |
| OSFatal 0x803746B0 / 0x80374888 / 0x803745B4 | 640 x 480 | the DVD fatal screen: unreachable (DVDSetAutoFatalMessaging is a native owner) |
| VIInit 0x8037E824 | 640 | native owner; guest VI bookkeeping only |
| AX 0x80384E14 .. 0x80389464 | 0x280 | 640-byte audio frames |
| interrupts 0x80376198 / 0x8037620C, GXSetFog 0x80399A0C, GX metric 0x8039A824, Sofdec 0x803D2C54 .. 0x804006C8, UTF-8 0x80472BCC / 0x804735CC | 0xF0 / 0x1E0 / 0x210 ... | masks, register ids, table bounds, memset sizes, UTF-8 lead bytes |

No LOD system exists (camera.h, DRAW_DISTANCE.md); no distance or detail decision in the scan depends on the aspect.

## 6. Validation (2026-10-27, what ran)

Builds: `.build/diag` (lifted renderer, diagnostics; the lift-site change regenerated and rebuilt the recomp table),
and the `tests` preset with `ctest --preset tests`: 121 of 121 passed (gekko_core_tests includes the new canvas cases).
The `release` preset was not rebuilt; its next build regenerates the recomp table for the two new lift sites. Control: the diag executable of 14:06 the same day,
built before these changes (it also lacks other sessions' later changes, so control-vs-candidate differences are
attributed separately below). Runs: headless, Westopolis (stg0100), the user's Westopolis gameplay tape
(`.build/recordings/westopolis-cpu-spike`, copies of its initial/ card, SRAM and configs), savestate and capture at a
fixed game tick. Aspects: 640x480 (4:3), 1920x1080 (16:9), 2560x1080 (21:9, 2.37:1), 3840x1080 (32:9), plus 640x480
with `--framing game`. Outputs: `.build/validation/aspect/` (captures and logs kept; the savestates were deleted after the comparisons).

**G1, one binary, two measures.** The diagnostic switch `SHADOW_GAME_WINDOW_SCREEN_TEST=1` makes the native function
write the coordinates in the game's own window (the original formula) instead of the displayed picture's; everything
else is identical.

| run | calls | on the displayed screen | verdict differs from the 4:3 window (gained / lost) | state A (game window) vs B (displayed) |
|---|---|---|---|---|
| tape 0 .. 4.0e9 ticks, 16:9 | 458 | 396 | 65 / 0 | 4 bytes differ (below) |
| savestate 3.5e9 -> 4.0e9, 4:3 `--framing game` | 580 | 503 | 0 / 0 | identical |
| same, 4:3 square pixels | 580 | 510 | 7 / 0 | identical |
| same, 16:9 | 580 | 519 | 16 / 0 | identical |
| same, 21:9 | 580 | 524 | 21 / 0 | identical |
| same, 32:9 | 580 | 526 | 23 / 0 | identical |

- **Native == original.** The tape run with the switch (native code, game-window measure) and the control (the original
  translated `fn_8000F57C`) end in byte-identical savestates (whole file, sha256 `e37a0419...` both). So the native
  projection - camera +0x3C rows, the game's tan, the viewport scale - reproduces the original exactly here, and the
  other sessions' changes between the two builds do not touch game state on this route.
- **At 4:3 with the game's framing the two measures never disagree** (0 of 580), as the window is the same.
- **Wider pictures only ever gain** "on screen" verdicts (7 -> 16 -> 21 -> 23 with the aspect); none is lost, because the
  displayed window contains the game's. 4:3 square pixels gains 7: that picture is already 17 % wider than the game's
  window (authored x 0.7228 vs game x 0.6158, `[boundary]` trace), which is what the port has always displayed at 4:3.
- **Effect on game state:** over the 4.0e9-tick tape the only difference is 4 bytes - the low mantissa bits of two
  floats of magnitude 1e-16 at 0x81556FA0 / 0x81556FA4, in an object whose secondary vtable 0x80532700 is set by the
  constructor fn_8018CCEC (the enemy code range). In the half-second segments at every aspect the gained verdicts left no
  trace in memory. The verdicts change the position fn_8019DA1C returns; this route shows that position does not, here,
  change what the enemies do beyond that residue. What the enemies use it for was not traced (UNKNOWN; section 3).
- The tape replays only at its recorded 16:9 output (`shadow_main` latches it), so the multi-aspect rows start from the
  tape's savestate at 3.5e9 and continue without input.

**Pictures.** Control and candidate captures at tick 3.0e9 from the 2.9e9 savestate are pixel-identical at all four
aspects (U1 and E1 change nothing on a 640 x 480 single-player canvas, as derived in section 4). The 4.0e9 captures
(`aspects-4e9.png`): the same vertical framing at 4:3 .. 32:9, the picture extended sideways, the HUD on its edges and
centre, nothing stretched or clipped. The 16:9 tape frame at 3.0e9 shows a Black Arms soldier inside the side area that
the original window treats as off screen.

**Not verified in game:** U1 and E1 on a 640 x 528 canvas (PAL 50 Hz). `--boot-stage` refuses a 50 Hz preselect and the
cold-boot route renders no frames headless (the renderer drops all 742 before the stage path), so only the unit tests
cover the 528-row canvas. Split screen (the 640 x 240 views) was not run.
