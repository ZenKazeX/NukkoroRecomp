# The lens flare - reverse engineered, and its visibility rewritten against the real viewport

Part of docs/boundary-20261007 (README.md). The user's course decision (docs/NORTH_STAR.md, last section, items 3-6):
the flare sat on the sun at 16:9 after the camera-window fix (DESIGN.md 8) but vanished when the sun left the central
4:3 area - "the visibility restriction is inside the flare / game code itself ... it requires a native reconstruction
of the flare visibility function". Evidence levels: **PROVEN** (read in disassembly or memory), **STRONGLY SUPPORTED**,
**STRUCTURAL MATCH**, **UNKNOWN**.

## 1. The chain (PROVEN)

| step | address | what |
|---|---|---|
| task | EffectDrawGlobalLensFlareTask vf1 0x801D2928 | first run: creates the flare (its resource through this+0x2C's vtable +0x1C, name 0x804D10CC), registers it with the effect manager (fn_80433198 -> fn_80432D68: the element table 0x8053DAC8, 7 elements); every run: fn_8043329C (build) then fn_80433270(manager, 1) (draw) - built and drawn in the SAME call. Its render level is in the Div group (renderer-re-20261007/01_FRAME.md): once per viewport - in 2P each view builds and draws its own flare with its own camera and raster. |
| manager | fn_800A35D8 = [r13+0x2DD8] = [0x805EF2D8] | the effect manager singleton; the global flare at +0x50 |
| build | fn_80432360 (functions.csv also has a mis-split row fn_8043240C inside it) | below, section 3 |
| draw | fn_80431E38 (mis-split row fn_80431E94 inside) | three Im2D strips inside its own camera bracket (section 4) |
| update | EffectLensFlarePJS update 0x80431AA4 -> fn_80433244 -> fn_80432FBC | the sun, enable and colour record (ORIGINAL.md 5) |

## 2. The objects (PROVEN: disassembly + a Glyphic Canyon savestate, run-lifted/gc-flare.gekkostate)

Flare (0x809E3270 in that state): +0x00 the manager; +0x04 u8 enabled; +0x05 u8 intensity (255); +0x06 u8 flash grey at
full strength (8); +0x08 RwV3d sun, world (5123, 7000, -19000 in Glyphic Canyon); +0x18 element table; +0x1C RwTexture*[]
(one atlas for all seven); +0x24 s32 count (7); +0x28 RwIm2DVertex[4 x count] (element i's quad at +0x60 i);
+0x2C RwIm2DVertex[4] the flash quad; +0x8C u8 glow hidden; +0x8D u8 ghosts hidden.

Element (0x24 bytes, static table 0x8053DAC8): +0x00 name string; +0x04 s16 height; +0x06 s16 width (canvas units);
+0x08 f32 position on the line from the view centre (0) through the sun (1); +0x0C..+0x18 the UV rectangle in the atlas;
+0x1C..+0x1F colour (the draw takes +0x1C, +0x1E, +0x1D, alpha +0x1F); +0x20 the draw-pass selector (part 06; the
draw compares it with its argument). Sizes are camera-raster pixels (= canvas units on the 640 x 480 raster). Part 06
(06_EFFECTS.md): the only writer of the flare's parameters is the type-9 element's ramp; only stg0201 / stg0402 carry
flare templates (9 templates on the disc); the dynamic_cast at 0x804323CC cannot fail after boot. Shipped values: element 0
(the glow) 128 x 128 at 1.0, alpha 255; ghosts 0.6 (128), 0.4, 0.2, -0.2, -0.4 (64), -0.6 (128), alpha 128.

The gate: fn_8042B620 = [0x805E25C8], the installed `Effect::System::EffectSystem`; the builder dynamic_casts it to
`Effect::RW3::EffectSystemRW3` (type_info 0x8056BD24, from 0x8056BD10). The one installed (by fn_801E258C at game init,
fn_8042B630) is an `Effect::PJS::EffectSystemPJS` (vtable 0x8053E93C; EffectSystem <- RW3 <- USA <- PJS); its GetCamera
(vtable +0x24, fn_80028730) returns the camera holder's RwCamera ([0x8056FF1C]).

## 3. The builder fn_80432360 (PROVEN, every instruction read; scratch/boundary-20261007/flare/fn_80432360.txt)

```c
void LensFlare::Build() {
    glow_hidden = ghosts_hidden = 1;
    EffectSystemRW3* sys = dynamic_cast<EffectSystemRW3*>(CurrentEffectSystem());
    if (!sys || !enabled || !vertices || count < 0) return;
    RwCamera* cam = sys->GetCamera();                     // the holder's RwCamera
    if (!cam) return;
    RwV3d s; RwV3dTransformPoint(&s, &sun, &cam->viewMatrix);   // fused single steps: pos + right x + up y + at z
    if (s.z < cam->nearPlane) return;                      // NaN passes (ordered compare)
    float W = cam->frameBuffer->width, H = ...->height;    // THE SCREEN = the camera's raster, read at run time
    float x = W * (s.x / s.z) - W / 2, y = H * (s.y / s.z) - H / 2;   // from the raster's centre (fused)
    float r = 0.5f * max(W, H);
    if (|x| <= r && |y| <= r) {                            // GHOSTS
        ghosts_hidden = 0;
        float k = max(0, (r - RwV2dLength(x, y)) / r) * (intensity / 255.0f);
        for (i = 1; i < count; ++i)                        // centre + element.position x (sun - centre), size w x h
            quad(i) = corners about (x * pos_i + W/2, y * pos_i + H/2), colour, alpha = (int)(k * alpha_i);
        flash colour = (int)(flash * k) grey, alpha 255;
    }
    if (x in [-0.5 W, 1.5 W] && y in [-0.5 H, 1.5 H]) {    // GLOW (x, y still from the centre)
        glow_hidden = 0;
        z = zNear + (far / s.z) * ((s.z - near) * (zFar - zNear) / (far - near));   // the sun's screen depth
        quad(0) = corners about (pos_0 * x + W/2, ...), z, colour + alpha of element 0 (no fade);
    }
    flash corners = (0, 0), (0, H), (W, 0), (W, H);        // the raster
}
```

Verified against memory (Glyphic Canyon, 60 s): the sun through the game camera and the authored window lands at
canvas (441.57, 51.00); the glow quad's centre in memory is (441.57, 51.00); ghost 1 (0.6) centre (392.94, 126.60), ghost 6
(-0.6) (247.06, 353.40) = centre + k (sun - centre); alpha 38 = (int)(0.2978 x 128) with |offset| 224.7, r = 320; the
flash (2, 2, 2) = (int)(8 x 0.2978).

## 4. The draw fn_80431E38 (PROVEN)

Its own RwCameraBeginUpdate / EndUpdate bracket; states SRCBLEND SRCALPHA, DESTBLEND ONE, VERTEXALPHA, no z write, no
fog, nearest filter. (1) Glow (if !glow_hidden and the element's texture slot differs from the call's argument):
ZTEST on, the atlas, the first strip (site 0x804320EC) - OCCLUSION is the depth test of this quad at the sun's depth,
separate from the off-screen rejection. (2) Ghosts (if !ghosts_hidden): ZTEST off, per element its texture, strip 0x80432194.
(3) Flash (if !ghosts_hidden and the argument is 0): a second bracket, SRCBLEND ONE / DESTBLEND ONE, untextured, the
raster-sized quad (site 0x80432260). In the lifted build sites 1 and 2 are Projected, 3 the layout policy's Fill (s21).

## 5. What the console assumed (the user's checklist, item 3)

| question | answer | evidence |
|---|---|---|
| sun -> camera space | RwV3dTransformPoint with the RwCamera view matrix (built at begin update from the frame and the view window) | PROVEN |
| -> projected screen space | x = W u - W/2, y = H v - H/2: canvas units from the CAMERA RASTER's centre | PROVEN |
| coordinate system of the test | raster canvas units, centre-relative (both tests) | PROVEN |
| bounds compared | ghosts: the square |x|, |y| <= 0.5 max(W, H); glow: x in [-0.5 W, 1.5 W], y in [-0.5 H, 1.5 H] | PROVEN |
| hard-coded 4:3? | NO literal: W, H are read from the camera's raster each frame (640 x 480 in 1P, a sub-raster in split screen). The "4:3 area" is the raster - the console's whole screen. The ghosts' square reaches the raster's left / right edges and 80 units beyond the top / bottom. | PROVEN |
| depends on | the camera raster and the camera's near plane, the view matrix (window); not CSD, not the EFB, not clip space | PROVEN |
| occlusion vs off-screen | separate: off-screen = the two tests above; occlusion = the glow quad's depth test against the scene (z = the sun's) | PROVEN |
| split screen | built and drawn once per VIEWPORT (the task's level is in the Div group, 01_FRAME.md), each time with the effect system's camera - the holder's RwCamera, synced to that viewport's camera and attached to its sub-raster: a flare per view, each tested against its own raster. 2P is always side by side (01_FRAME.md: only split modes 0 and 1 are written). Not run yet (no 2P route) | STRONGLY SUPPORTED (part 01's level census) / UNKNOWN at run time |
| EFB dimensions | not read | PROVEN |
| 640 x 480 / non-square pixels | none in the code; the canvas units are square because the 640 x 480 raster was shown 4:3 | STRONGLY SUPPORTED |
| margins / authored thresholds | the ghosts' radius 0.5 max(W, H) with a linear fade to 0 at it; the glow's half-screen margin (1.5 / -0.5) | PROVEN values |
| the glow's bounds | compared with CENTRE-relative x, y but symmetric about the raster's corner-relative centre (0.5 W): meant as half a screen beyond each edge in raster coordinates. Effect on the console: the glow vanished as soon as the sun crossed the left / top edge (popping half a visible glow) and lingered a screen beyond the right / bottom edge. A coordinate slip. | PROVEN (the compare) / STRONGLY SUPPORTED (the intent) |
| the length | RwV2dLength 0x80483440: fmuls, fmuls, fadds, then RenderWare's 4096-entry table square root ([RwGlobals + [0x805F2640]]: ~12-bit precision) | PROVEN |

## 6. The native rule (implemented: runtime/lift/effects/lens_flare.h + runtime/scaffold/rw_driver/lens_flare.cpp)

```
world sun -> the drawing camera (RenderWare view matrix: the camera carries the authored window, DESIGN.md 8)
          -> canvas units from the camera raster's centre
          -> THE SCREEN = what the output shows of this camera's raster:
             render/view.h visible_canvas(raster W x H, aspect W/H, view aspect = output aspect x the raster's share
             of the 640 x 480 screen) - per camera viewport, any output aspect
          -> ghosts + flash: the console's field (square of half the RASTER's larger side, linear fade to 0 at it) laid
             over that screen per axis: the sun's offset measured in raster units of each shown axis (x W / shown W,
             y H / shown H), then the console's square and fade (section 10; until 2026-10-19 the square was half the
             SHOWN larger side on both axes)
          -> glow: half a screen beyond each edge of that screen (the bounds' intent)
          -> occlusion: unchanged (the glow quad's depth test at the sun's depth)
          -> the game's draw (glow + ghosts Projected, flash Fill)
```

- At the raster's own aspect (4:3 output; `--framing game` changes nothing here) the screen IS the raster, both scales
  are exactly 1: positions, alphas, the flash and the ghosts' visibility are the console's bit for bit (lens_flare_tests:
  200,000 random suns; the 320 x 480 split-screen raster: 20,000 more).
- 16:9: the ghosts stay while the sun is anywhere in the picture and fade to exactly 0 at its real left / right edge
  (the field stretched sideways by 853.33 / 640: 0 at 426.67 instead of 320); 21:9 / 32:9 likewise. Above and below
  the picture nothing changes against the console: the field reaches 80 units past the top / bottom edge and fades as
  strongly as the console's (section 10). A portrait output stretches the field vertically instead (4/3 of the shown
  half-height) and keeps the console's reach sideways (the picture's side edges).
- Split screen: each camera's screen is its own viewport's share of the output (a 320 x 480 half on 16:9 shows 426.67 x
  480 units; the flare never tests the whole window): the half's console field (r = 240: 80 units past each side,
  0 at the top / bottom edges) stretched sideways by 426.67 / 320. The game builds and draws the flare once per
  viewport with that viewport's camera (its level is in the Div group), so View 0 -> camera 0 -> flare 0 and View 1 -> camera 1 -> flare 1
  already hold with the native builder; to be confirmed on a 2P route.
- No 4:3 constant, no fake region, no clamp: the only inputs are the camera's raster and the output's aspect.

Classified differences from the console (all intended):
1. Ghosts shown with the sun in a wider picture's side bands (the point of the change); counted at run time
   ("past the raster's own square").
2. The glow's edge bands: kept while the sun is up to half a screen past the left / top edge (the console dropped it at
   the edge while half of it was still on screen), dropped beyond half a screen past the right / bottom edge (where the
   console kept a glow wholly off screen). 35,004 of 200,000 random suns in the test fall in these bands; only those
   within the glow's half-size (64 units) of the left / top edge look different.
3. The distance is the true length; RwV2dLength's 12-bit table could move a ghost alpha or the flash grey by 1 where
   k x alpha lies within ~2.5e-4 of an integer.

## 6a. Which stages have a flare (PROVEN for the first 30 s of each stage)

scratch/boundary-20261007/scripts/flare_stage_survey.py booted all 38 bootable stages for 30 s of game time and read
the global flare out of a savestate: ENABLED in Glyphic Canyon (stg0201, sun (5123, 7000, -19000)) and Sky Troops
(stg0402, sun (5123, 4000, -19000)) only; every other stage keeps the default (enabled 0, sun (0, 1000, 0)). The enable
and the sun come from a stage's EffectLensFlarePJS object (section 1); a stage could still enable it further in.
Westopolis has none (also a full camera circle there over 60 s: 3463 builds, never enabled) - the capture matrix's sun
cases are therefore taken in Glyphic Canyon (the sun ahead from the start).

## 7. Validation

Unit: runtime/tests/lens_flare_tests.cpp (CTest lens_flare_tests): 471,306 checks PASS (quick_test.cmd). Runtime
comparisons: section 9 (filled as they are made). Since 2026-10-19 (section 10): 492,668 checks PASS
(scratch/lf-20261019/tools/quick_test.cmd, the CMake target's /arch:AVX2 /fp:precise); the same test compiled against
the previous header fails 1,056 of them (the vertical field at 16:10 .. 32:9, the Sky Troops rows, portrait and split).

## 8. Next (Stage 2): the flare as a native render pass

The game keeps the data (enabled, intensity, flash, sun, the element table, the atlas); the renderer draws the flare per
VIEW from it - the sun projected with each view's camera, the same rule over each view's shown screen, the glow
depth-tested in the view's depth - and the guest draw fn_80431E38 with its Im2D strips is deleted. Deleted with it:
this builder's write-back of guest vertices.

## 9. Runtime evidence (build-lifted-7 / -8, 2026-10-07)

- **Against the original code** (the boundary-3 exe runs the console's builder translated; same boot route, Glyphic
  Canyon frame 1600, the sun ahead inside the raster): **1280 x 960 PIXEL-IDENTICAL** (SHA-256 68203189... both);
  **`--framing game` 1280 x 960 PIXEL-IDENTICAL** (4cb59fd0... both). At 1920 x 1080 the frames differ by the rule
  (both framings): the fade is measured over the wider screen (below).
- **Smoke** (Westopolis probe route, 45 s): 2564 builds served by the native owner; the sun never in front of that
  route's camera; every other boundary / 2D number identical to before (2066 projections, 0 window mismatches,
  149,292 canvas draws).
- **Sun sweep at 16:9** (scratch/boundary-20261007/glyphic-flare-sweep.uitest: the camera stick, a new `cstick` verb;
  SHADOW_FLARE_TRACE): 3411 builds with the sun in front; 1245 of them showed ghosts with the sun outside the raster's
  own square (where the console hid them); the sun in every position class - centre (frame 2729, x 3.1), near the old
  border (2293, x 282.6; 2765, x -281.0), past it (2690, x 320.8; 2769, x -322.1; 1237 frames in the band), at the
  16:9 edge (2081, x -400.9; held at x 399 from 3120).
- **The matrix** (scratch/boundary-20261007/scripts/flare_matrix.py: the same exe and input twice per frame, the
  console's screen rule SHADOW_FLARE_CONSOLE_SCREEN=1 vs the native rule; sheet captures/flare-rule-sheet-a.png):
  2690 (sun 0.8 units past the right border): the native rule keeps the ghost chain the console dropped (the console
  kept the glow on that side); 2769 (past the left border): the native rule keeps the ghosts AND the glow - the
  console dropped both (the user's report "hides it when the sun leaves the central 4:3 area"); 2081 / 3120 (at the
  16:9 edge): identical - the fade is 0 there in both and the glow is occluded; 2729 / 2293 / 2765 (inside the old
  area): both show the flare, the native one stronger - fade 0.571 vs 0.42 at 2729, the flash grey 4 vs 3 (the
  whole frame +1), two overlapping ghosts up to +39.
- **Classified difference (intended): the fade radius follows the screen.** The console's rule fades the ghosts and
  the flash to 0 at half the screen's larger side (320 on its 640 x 480 raster). Over a 16:9 screen that is 426.67: the
  same sun offset is a smaller fraction of the wider picture, so the flare is stronger for a central sun and reaches 0
  exactly at the real edge (no pop). The alternative - keeping the 4:3 strength - needs a 320 (a 4:3 constant) in the
  formula, which the user's decision rules out ("Do NOT smuggle 4:3 assumptions back in through constants"); a 4:3
  output (either framing) is the console's, pixel for pixel.
  **SUPERSEDED for the vertical axis on 2026-10-19 (section 10).** Horizontally it stands (stronger for a sun inside
  the wider picture, 0 at the real side edge). Vertically, "half the shown larger side on both axes" also stretched
  the field above and below a picture that had not grown taller - the cause of the blink at the start of Sky Troops.

## 10. The vertical reach on wider outputs (2026-10-19; findings-20261019-LF/LF_REPORT.md)

**The defect (the user: "an annoying blink in the lens flare effect at the start of sky troops").** At 16:9 the whole
screen went white for 4 frames as the flare came on in stg0402 (render frames 293-296 of `--boot-stage stg0402`;
frame mean 198 -> 236, 45 % of the pixels white; none at 4:3 output, none at 16:9 with SHADOW_FLARE_CONSOLE_SCREEN=1).
The chain, layer by layer:
1. Game data (console behaviour, kept): the stage's first EffectLensFlare element ramps the flash byte from the LF
   ctor's default 0x20 to the authored 0x08 over its 10-frame life (renderer-re-20261007/06_EFFECTS.md 3.1.5); the sun
   enters the view just above the picture (sy -359 .. -241 canvas units from the centre; the picture's top edge is
   -240). The sun track was identical at 4:3 and 16:9.
2. This builder (the first divergence): the field's radius was half the SHOWN larger side on both axes, 426.67 at
   16:9, so a sun above the picture got fade 0.26-0.34 where the console's gives 0.02-0.12, and the flash grey
   (byte x fade) reached 5, 5, 5, 5 in frames 293-296 (console: 0, 1, 1, 1).
3. The glare (console-equal, kept): every flare draw is a glare source (section 4, BLOOM_SPEC 2.6), the full-screen
   flash too. The level-A capture q8(grey x 30/255) is 0 for a grey of 4 and 1 for 5; the uniform 1 becomes 9 after
   the tap sum, level B 2 x 25 = 50, so about +59 is added to every pixel. Stripping the glare pass or the immediate
   draws removed the white-out; frames 293-296 are exactly the frames with a flash grey of 5.

**What the console's rule means and how it is kept.** The console lays a field over its picture: the square of half
the raster's larger side, fading linearly with the distance from the centre - on 640 x 480 it reaches the left / right
edges and 80 units past the top / bottom ones. The 10-07 decision wanted the field to follow a wider picture into its
side bands and reach 0 at the real side edges, with no 4:3 constant. Only the horizontal was in question; taking half
the shown larger side on both axes also pushed the field 186.67 units above and below a picture that is no taller, and
raised the fade at its top edge from 0.25 to 0.44. The rule now lays the console's field over the shown screen axis by
axis: the sun's offset is measured in raster units of each shown axis (x W / shown W, y H / shown H), then the
console's square and fade apply.
- 4:3 (either framing) and the console's own split-screen rasters at 4:3: the scales are exactly 1 - the console's
  arithmetic bit for bit (200,000 + 20,000 random suns).
- 16:9 / 16:10 / 21:9 / 32:9: horizontally as on 10-07 (0 at the real side edge); vertically the console's (for a
  sun on the vertical axis the console's ghosts, alphas and flash at every aspect, tested 4:3 .. 32:9).
- Portrait: stretched vertically (4/3 of the shown half-height), the console's reach sideways.
- Split screen on 16:9: the half's console field (r = 240) stretched sideways by 426.67 / 320.
- Inputs: the camera's raster and the shown screen only (no 4:3 constant). Unchanged: the glow's band, the occlusion,
  the positions, the draw, the glare.
- The Sky Troops start at 16:9 (the trace rows, the implied flash bytes): flash grey <= 2 instead of 5, under the
  glare's first level at 16:10, 16:9 and 21:9 (lens_flare_tests section 8; the previous rule fails it).
- Visible consequence (accepted by the integrator for the user, 2026-10-19): on a wider output the flare is weaker than
  before while the sun is above or below the picture - as strong as on the console.

Runtime validation (2026-10-19; build-lf c0d077a9 from the stage scratch/lf-20261019/src-lf1 vs the candidate exe
3679f2dd; details and data in findings-20261019-LF/LF_REPORT.md "Phase B results"):
- stg0402 frames 280-360, every frame, flash grey at or above the glare's first level:

  | output | before (max) | after (max) |
  |---|---|---|
  | 16:9 | frames 293-296 (5) - the reported blink | none (2) |
  | 21:9 | frames 291-298 (9) | none (3) |
  | 16:10 | none (4) | none (2) |
  | 4:3 | none (2) | none (2) |

  4:3 on WARP: 81 of 81 frames byte-identical before vs after.
- Sweep, 38 stages at 16:9: the flare is enabled only in stg0201 / stg0402.
  - stg0402, 60 s, frames with ghosts shown: before 2,973; after 566; the console's rule 548. After = the console's plus
    the 18 side-band frames.
  - stg0201, start: flash 5 on frames 153-155 (before: up to 8 on 151-157). All under the loading card: on WARP they are
    byte-identical to a run that never reaches the glare level. The first picture is frame 181.
- D3D12 debug layer: 0 messages.
- Default windowed run: the native owner serves 0x80432360, and the trace shows the per-axis fades at the output's 16:9.
- CTest (build-lf-n): 84 / 86 - EV's two known environment failures (#15, #75) - lens_flare_tests PASS.
- R2 gate: PASS on all four routes, candidate states equal to the candidate exe's (westopolis 750fca55, final-haunt
  6baab7df, story 38a0dda3, lava a3ee1d6e).
- Note: the build was made from FLARE.md 60b0e516 (this line added afterwards; docs are not a build input).
