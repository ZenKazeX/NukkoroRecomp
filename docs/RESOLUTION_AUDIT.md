# Resolution audit of the R2 renderer (2026-10-26)

Scope: the default build (CMakePresets "release", `SHADOW_RENDER_LIFTED=ON`). Read from the source, with the decisive
lines checked by hand; nothing here is measured yet (measurements: OPTIMIZATION_RESULTS.md, when the validation runs).
Paths are relative to `runtime/`.

## 1. Which renderer, which machinery

- The legacy renderer (EFB emulation at 640x528, `--internal-scale` integer multiples, `native_renderer.cpp`,
  `native_gpu.cpp`, `bloom*.cpp`, `native_hud.cpp`, ...) is excluded from the release build (`CMakeLists.txt` lifted
  file list) and every use in `src/shadow_main.cpp` sits under `#if !defined(SHADOW_RENDER_LIFTED)`. It is built only by
  the "tests" preset for its own tests. `internal_scale` is still computed for a log line that says it is legacy-only.
- So the default game has **no EFB, no integer multiplier and no 640x480 full-scene intermediate**.

## 2. From settings to the scene targets

1. `display.cfg` (`src/gekko/display.cpp`) and the CLI (`--render-width/--render-height`, `src/shadow_main.cpp`
   `resolve_display_config`) give `render_width` / `render_height` (0 = follow the window).
2. The game's output size is the window client area (windowed), `--native-output` (headless captures) or the input
   tape's recorded output (`set_game_output` / `latch_game_output` in `shadow_main.cpp`).
3. `LiftedRender::set_output_aspect` (`src/gekko/lifted_render.cpp:330`) sets the render size:
   - render height given: `h = render_height`, `w = round(render_height * output_w / output_h)` - **the requested width
     is ignored**;
   - only a width given: `w = render_width`, `h = round(render_width * output_h / output_w)`.
   No clamp (other than >= 1), no rounding to multiples, no relation to 640x480.
4. `Renderer::record` (`lift/render/renderer.cpp`, per view): "view colour" W x H R16G16B16A16 4x MSAA, "view depth"
   W x H D32 4x MSAA, "view" resolved W x H, where W x H is the view's rectangle of the render size (the main view is
   the whole render size). Viewport and scissor are that rectangle; the projection uses its pixel aspect.

Consequences for the brief's table (window = output):

| Asked | Output | Scene target today | Brief wants |
| --- | --- | --- | --- |
| 640x480 | 4:3 window | 640x480 | 640x480 |
| 1280x720, 1920x1080, 2560x1440, 3840x2160 | exact 16:9 (fullscreen 1920x1080) | as asked | as asked |
| 2560x1440 | windowed 1920x1080 client (title bar: e.g. 1920x1057) | ~2616x1440 (width follows the client) | 2560x1440 |
| 2560x1600 | 16:9 display | 2844x1600, then fitted to 1080p | 2560x1600 |
| 3440x1440 | 16:9 display | 2560x1440 | 3440x1440 |

## 3. Every target in a frame (render size W x H)

| Target | Size | Note |
| --- | --- | --- |
| view colour / view depth / view (resolved) | W x H (4x MSAA, resolved 1x) | the 3D scene is rasterized here |
| glare sources (+ resolved) | W x H | |
| glare capture, blur, box, merge levels | A: 128 rows x even(96 x view aspect), B: half of A | intentionally fixed: the original glare chain's level sizes (BLOOM_SPEC 6.1/6.4); blur widths are in level texels, so the glare looks the same at every resolution. Not a scene target. |
| scene snapshot (heat haze, refraction) | W x H, only the read rectangle resolved | full resolution |
| history (motion trail) | W x H, persistent per view | full resolution; the console's 320x240 trail downsample is not reproduced |
| views under (split screen) | W x H | 1:1 |
| output (compose) | render size | views copied 1:1, point sampled |
| present | back buffer | **the only full-frame resampling**: when the render size differs from the back buffer, PSScale fits it (2x2 box at exactly 2:1, Catmull-Rom bicubic otherwise) with bars |

No full-scene intermediate is smaller than the render size. The present scale is a downsample when the render size is
larger than the window (supersampling), an upsample only when the player picks a render size smaller than the window.

## 4. Copies the game makes (EFB-copy equivalents)

Glare copies only mark the glare window (the glare is native at view size). Screen copies for heat haze, refraction
and the motion trail become view-sized snapshots / history saves; guest UVs become canvas points and are mapped to
fractions of the view (`canvas_image_rows`), so they sample correctly at any resolution. Other camera-texture copies
are not implemented in R2 (counted, "not drawn yet").

## 5. HUD / 2D

The canvas is the guest raster (640x480 or 640x528, shown 4:3). Anchored canvas draws are placed in a box of the
canvas aspect inside the view (height-filled on a wider view), Fill stretches, and each draw is scissored in physical
pixels; the 2D is rasterized at the render size. `hud_scale` / `hud_inset` (display.cfg) have no effect in R2 (only the
legacy HUD read them). The display.cfg `aspect` setting is not read by R2: the view aspect is the output's.

## 6. Every hardcoded size on the default path

All are logical (compatibility) coordinates except the glare levels:
- `scaffold/rw_driver/frames.cpp` kDisplayAspect 4/3, canvas = guest raster size; `projection.cpp` 640x480 viewport
  table, kAuthoredScreenAspect 4/3, kSplitScale 1.25, kMarkHalfSize 32 (canvas units); `rw_driver.h` / `game/rw_api.h`
  default ScreenRect 640x480; `lift/render/view.h` kScreenAspect 4/3; `lift/ui/hud_layout.h` kHudCanvasWidth/Height
  640/480 (anchor classification; uses 480 even on a 640x528 canvas - effect not verified);
  `game/game_owners.cpp` and `lift/effects/lens_flare.h` 640x480 lens-flare share; `screens.cpp` half-resolution copy
  rectangles (which views a copy covers).
- Physical but intentional: glare levels 128 / 64 rows (`renderer.cpp`, `shaders/glare.hlsl`).
- Defaults: captures 1280x720 unless `--native-output` (or `SHADOW_R2_CAPTURE_AT_RENDER_SIZE=1`).

## 7. Gaps against the brief

1. A requested width is ignored when a height is given; a windowed client that is not exactly the requested aspect
   changes the width; 16:10 and 21:9 requests on a 16:9 output are not rendered at the requested size.
2. No limit check: a size above D3D12's 16384 would fail at texture creation instead of being refused clearly.
3. No test covers `set_render_size`, the aspect rule, the present scale or the glare level sizes.
4. No way to show the actual dimensions of the bound render targets and viewports (the GPU memory trace names
   allocations without sizes; no PIX or RenderDoc is installed on this machine).
5. `hud_scale` / `hud_inset` / `aspect` are parsed but ignored by R2 (not resolution bugs; listed for completeness).

## 8. Changes made (2026-10-26)

- **Exact render size** (`lift/render/view.h` `render_size_for`, `src/gekko/lifted_render.cpp`, `src/shadow_main.cpp`):
  when a width and a height are given, the game-visible output is that size (`exact_render_size` in shadow_main), so
  the 3D scene, depth, resolve and output targets are exactly the asked pixels and the camera, culling and HUD take its
  actual aspect (width / height). A window or screen of another shape shows the picture fitted with black bars (the
  existing present-scale pass; 1:1 when the sizes match). An input tape's recorded output of another aspect gets the
  largest rectangle of its aspect inside the asked size (2560x1440 inside 2560x1600) - never a size larger than asked,
  never an invented size such as 2844x1600. Only one side given: that side, the other following the output (CLI only;
  the launcher always gives both).
- **Limits**: a render size above 16384 (D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION) is refused at startup.
- **Display detection** (`lift/render/display_modes.h/.cpp`, `--list-displays`): DXGI adapters and outputs, the
  desktop's real size and refresh (EnumDisplaySettings), the monitor's name, every supported mode, and the established
  resolution presets grouped by aspect family (16:9, 16:10, 21:9, 32:10, 32:9, 4:3, 5:4); `aspect_family` names any size
  by its actual ratio (3440x1440 is "21:9" at 2.389:1; 1366x768 is a near-16:9). Rendering always uses width / height.
- **Resolution state** in the session report (`resolution`): physical render size, its family and exactness, the asked
  size, the logical game canvas, the game output, the window mode and window, the primary monitor, its native mode and
  refresh.
- **Launcher** (`tools/launcher.py`, `tools/resolution_presets.py`): Display mode (Windowed / Borderless fullscreen),
  Aspect ratio (Auto, 4:3, 5:4, 16:9, 16:10, 21:9, 32:10, 32:9) filtering the Resolution list (Native = the monitor's
  desktop, the presets of the family, the monitor's own other modes, Custom validated to 160x120..16384). The chosen
  size is both the window/fullscreen picture and the exact render size. The "Internal scale" multiplier and the ignored
  aspect flag are gone; a build that predates --render-width gets the window choice only.
- **Evidence tooling**: `GEKKO_RENDER_TARGET_TRACE=<file>` (diagnostic builds) writes, for one frame
  (`GEKKO_RENDER_TARGET_TRACE_FRAME=<n>` or `capture`), every kept pass, the render/depth/resolve/copy targets it binds as
  the D3D12 resources report them (`ID3D12Resource::GetDesc`) and every viewport and scissor it sets
  (`PassContext::set_viewport`, now the only way passes set them).
- **Tests**: view_boundary_tests (the size rule over every listed size; never larger than asked), render_core_tests
  (`graph_target_trace` on a 333x207 4x target), test_resolution_presets (the launcher's table equals the engine's,
  classification, choices, validation, flags).

## 9. Evidence (headless, `.build/validation/resolution/headless`)

Westopolis (stg0100, no input tape so the output takes each size), capture of the first rendered gameplay frame at
the asked size with the target trace of that frame:

| Asked | Capture | Scene colour / depth | view, output, glare sources | Smaller targets (by design) |
| --- | --- | --- | --- | --- |
| 1920x1080, 2560x1440, 3840x2160 (16:9) | exact | exact, 4x | exact | glare levels 170x128 / 85x64 |
| 1920x1200, 2560x1600 (16:10) | exact | exact, 4x | exact | 154x128 / 77x64 |
| 3440x1440 (21:9) | exact | exact, 4x | exact | 230x128 / 115x64 |
| 3840x1200 (32:10) | exact | exact, 4x | exact | 308x128 / 154x64 |
| 5120x1440 (32:9) | exact | exact, 4x | exact | 342x128 / 171x64 |
| 640x480 (4:3) | exact | exact, 4x | exact | 128x128 / 64x64 |
| 1000x777 (custom) | exact | exact, 4x | exact | 124x128 / 62x64 |

Every viewport of the scene, resolve and compose passes is the full asked size. The pictures: 4:3 keeps the original
framing; wider shapes extend it sideways at the same vertical field of view (Hor+), nothing stretched; the HUD keeps
its proportions, anchored to the edges and centre.

Game behaviour: every run reached the same game tick (1,620,651,375) and frame count (1,196). The guest state is
byte-identical between sizes of the same aspect (1920x1080 = 2560x1440 = 3840x2160; 1920x1200 = 2560x1600): the
resolution changes nothing in the game. Between aspects 4-5 of 385 64 KiB guest-memory blocks differ - the game's
aspect-dependent state (its camera window and screen tests see the wider picture: HW_REPORT J); not attributed further.

## 9b. Bars, windowed and borderless fullscreen (rendered, `.build/validation/resolution/fullscreen`, `windowed`)

User request: no black bars. A picture of another shape than its window or screen can only be shown with bars (or
stretched, cropped, or rendered at a size other than the one chosen - all excluded). So:
- **Windowed** (`HostWindow::set_content_aspect`, `src/gekko/host_window.cpp`, called by shadow_main for an exact render
  size): the window's client takes the picture's shape - the largest of that aspect inside the asked window size and
  the desktop's work area - and keeps it while its edges are dragged (WM_SIZING). Before: a 1920x1080 window on this
  1080p desktop came out 1920x1061 (title bar + taskbar), so even a 1920x1080 picture was rescaled with bars, and
  3440x1440 had 129-row bars. After: 3440x1440 windowed -> a 1904x797 client, no bars (screen capture measured).
- **Borderless fullscreen** (user's choice): the launcher offers only sizes of exactly the monitor's shape (on a
  1920x1080 screen: 1280x720 ... 7680x4320, supersampled down), so fullscreen never has bars; other shapes stay
  available windowed. A custom fullscreen size of another shape is refused. (The runtime itself still letterboxes such
  a size if given on the command line.)

Screen captures of the game window's client area 15 s into each run (pure-black edge rows/columns counted):

| Run | Client | Bars |
| --- | --- | --- |
| fullscreen 1920x1080 / 2560x1440 / 3840x2160 | 1920x1080 | none |
| fullscreen 3440x1440 (21:9 on 16:9, now not offered) | 1920x1080 | 138 rows top and bottom |
| fullscreen 5120x1440 (32:9 on 16:9, now not offered) | 1920x1080 | 270 rows top and bottom |
| windowed 3440x1440 | 1904x797 | none |

## 9c. Cost per resolution (windowed, Westopolis stg0100 without input, 60 s, diagnostic build)

| Render size | GPU frame p50 / p95 / p99 (ms) | scene pass p50 | D3D12 default heap max | local usage max |
| --- | --- | --- | --- | --- |
| 1920x1080 | 5.68 / 9.21 / 11.79 | 3.80 | 188 MiB | 236 MiB |
| 2560x1440 | 4.14 / 9.86 / 13.00 | 2.61 | 329 MiB | 371 MiB |
| 3440x1440 | 4.92 / 10.02 / 11.46 | 2.73 | 434 MiB | 476 MiB |
| 5120x1440 | 7.37 / 15.20 / 17.32 | 3.52 | 631 MiB | 675 MiB |
| 3840x2160 | 7.05 / 9.86 / 12.03 | 2.84 | 690 MiB | 737 MiB |

Memory grows with the pixel count as expected (multisampled RGBA16 colour + depth dominate). GPU times do not scale
cleanly: the RTX 4080 runs this light load in its low-power states and clocks vary with load (Phase A, A1), so one
run per size measures clock state as much as work; the 1080p run being slower than 1440p is that, not the work.

## 9d. Presentation follow-ups (2026-10-26, user reports)

- **Video-mode switch blank.** The PAL 50/60 Hz prompt is redrawn once at 480 lines after the 528-line screen; that
  frame popped visibly. `RwDriver::submit_frame` replaces the first packet after a top-level raster size change with
  an empty black packet (`mode_switch_blanks`, reported as "video-mode switches shown black"). Verified: frame 26
  black, count 1.
- **Edge art. REVERTED 2026-10-28.** `classify_edge_art` sent tall art flush with a canvas side (the results' RESULT
  lettering, the title's "ultimate life form" column, the Options menu's OPTIONS lettering and right panel) to that
  side of a wide screen, which pulled the menus apart. The user asked for the menus in their original form: the rule
  and the function are removed; that art stays in the centred 4:3 canvas with the rest of its menu.
- **Backgrounds: cover. REVERTED 2026-10-28.** A textured draw over the whole canvas took `Anchor::Cover` (scaled up to
  cover the view, overflow cut). It now takes `HudAnchor::Canvas`: the centred 4:3 canvas with the menu on it, as the
  original shows it (Dolphin reference `.build/validation/menu-ref/dolphin-options-sound-test.png`, pillarboxed).
  Fades, bands and curtain columns still Fill. `render::Anchor::Cover` stays in the renderer; the layout no longer
  picks it. Unit tests updated to the original behaviour (gekko_core_tests passes); not yet seen in the running game.
- **Pupils cut by the head (results screen, both characters).** Not a texture: the pupils are separate 20-vertex
  atomics drawn just in front of the head, and stages sit ~25,000 units out where a float step is 0.002 units. Skins
  were posed into world space on the GPU and draw matrices composed in float, so the head's surface landed a different
  rounding from the pupils each frame. Now every skin pose is written relative to its first posed bone (the draw's world
  matrix restores the origin) and `multiply` accumulates in double and rounds once. Lethal Highway results frames
  16500-16515: the cut/"drip" on 6 of 16 frames before, none after (`results/before-eyes`, `eyes_before_after.png`).
- **Real-time cutscenes 4:3.** EventManager's constructor (0x80173EF4) sets the small-data byte r13+0x3E5D while an
  in-engine event holds the frame interval; its destructor clears it. While it is set and the game is not in a
  two-player match (system object 0x8057E760 + 0xB2 clear; the 2P intros keep the output's shape, user 2026-10-26), the
  driver's `output_aspect()` is 4:3 and the packet's `content_aspect` boxes every view in a centred 4:3 rectangle with
  black outside (renderer view loop). event0800 (a 2P intro, before the 2P rule): 448 framed frames, 4:3 box with bars
  at 16:9 and 32:9; the one unframed frame between its two scenes is black (the game's own fade). After the rule: 0
  framed frames. No story event could be captured: `--boot-event` of story events (1001, 2101, 3001, 4001, 6001)
  faults in the story-progress lookup at 0x801784A0 (pre-existing, cold boot has no story record), `--boot-story` goes
  from the opening movie straight into Westopolis, and `--boot-flashback` plays the Maria flashback movie.

## 10. Limitations

- Exclusive fullscreen is not implemented: borderless fullscreen uses the desktop's mode and refresh, so the launcher
  shows the refresh rate instead of offering a choice. Fullscreen opens on the primary display, and offers only sizes of
  its shape (21:9 / 32:9 on a 16:9 monitor: windowed only).
- Per-resolution GPU timings are single runs under varying GPU clocks (see 9c).
- A windowed client is rounded to whole pixels: a residual mismatch of at most one pixel can remain.
- No third-party GPU capture (PIX / RenderDoc are not installed; the user chose the in-engine trace).
- The glare chain's levels are fixed 128 / 64 rows (the original glare's sizes) at every resolution.
- Prerendered movies were not checked at every aspect.
- hud_scale / hud_inset (display.cfg) still have no effect in R2.
- The 4:3 cutscene box is verified on a two-player intro only (before excluding them); a story cutscene has not been
  captured (9d).
## 11. Four independent settings (2026-11-05)

The rule is `runtime/include/gekko/display_geometry.h`, used by the settings, the game output and the present pass:
- **Window mode:** windowed, borderless fullscreen or exclusive fullscreen.
- **Display resolution:** the windowed client size, or the exclusive display mode. Borderless shows at the desktop's
  size.
- **Aspect:** Auto (the output's shape), 4:3, 5:4, 16:9, 16:10, 21:9, 32:10, 32:9, or any `W:H` / number in
  display.cfg.
- **Render scale:** a percentage per side of the picture, 10..400 %.

How they combine:
- output + aspect -> **viewport**: the largest rectangle of the aspect inside the output, centred, with bars outside.
  The viewport is the game's output: the camera, culling and HUD take its exact aspect.
- viewport x scale -> **render size**: each side rounded half away from zero, at least 1, at most 16384.
- The present pass maps the render size onto the viewport. This is the only resampling: 1:1, a 2x2 box at exactly
  2:1, a bicubic otherwise.
- An exact `render_width` x `render_height` (CLI, benchmarks) still wins over the scale. An input tape's recorded
  output is the viewport during a replay.

Decoupled (each was a coupling found in the audit):
- A render size no longer becomes the game's output, and no longer shapes or moves the window. Render, aspect and
  scale changes are the display settings' game group only.
- The window takes only the display resolution's shape: exactly that client when it fits the work area, else the
  largest of that shape inside it. The setting is kept and the actual output is reported.
- The `aspect` key is now honoured (it was parsed and ignored).
- New: the `exclusive` and `render_scale` keys, and `--exclusive`, `--borderless`, `--render-scale`.
- Exclusive fullscreen (`host_window.cpp`): `ChangeDisplaySettingsEx(CDS_FULLSCREEN)` on the window's monitor, in its
  current depth and refresh when the mode has it.
  - A mode the monitor does not list is refused: logged, the window stays borderless at the desktop's size, and the
    setting is untouched.
  - The desktop's mode comes back on leaving exclusive, on Alt+Tab (the game minimises, and the mode returns on
    activation), and when the window is destroyed.
- Alt+Enter switches windowed <-> fullscreen through the display settings (saved; nothing else changes).
- The process is per-monitor DPI aware (v2). A windowed client keeps its pixel size across a DPI change.
- Options page (`native_options_display.cpp`): four rows held by value.
  - The ratio no longer filters or changes the resolution list.
  - A resolution or mode change keeps the ratio and the scale.
  - Exclusive lists the monitor's own modes, borderless its desktop size.
  - An exact render size, custom ratio or custom scale in effect is listed and kept.
- The session report's `resolution` has `settings` (the four values) and `geometry` (output, viewport and offset,
  viewport aspect, render size, render pixels / viewport pixels).

Validation (Westopolis boot, frame 300 render-target trace + session report; `.build/perf/decouple/`):

| Mode | Display | Aspect | Scale | Output | Viewport | 3D render (4x MSAA) | Pixels | Present |
|---|---|---|---|---|---|---|---|---|
| windowed | 1280x960 | Auto | 100 | 1280x960 | 1280x960 | 1280x960 | 1.00 | 1:1 (no scale pass) |
| windowed | 1920x1080 | Auto | 75 | 1765x993 (work area) | 1765x993 | 1324x745 | 0.563 | into 1765x993 |
| windowed | 1280x720 | 4:3 | 100 | 1280x720 | 960x720 +160+0 | 960x720 | 1.00 | pillarbox 160 px |
| windowed | 1920x1080 | 21:9 | 100 | 1765x993 | 1765x756 +0+118 | 1765x756 | 1.00 | letterbox |
| windowed | 854x480 | Auto | 100 | 854x480 | 854x480 | 854x480 | 1.00 | 1:1 |
| borderless | (desktop) | 16:9 | 50 | 1920x1080 | 1920x1080 | 960x540 | 0.25 | into 1920x1080 |
| borderless | (desktop) | 32:10 | 100 | 1920x1080 | 1920x600 +0+240 | 1920x600 | 1.00 | letterbox |
| borderless | (desktop) | Auto | 266.67 | 1920x1080 | 1920x1080 | 5120x2880 | 7.11 | down to 1920x1080 |
| exclusive | 800x600 | 4:3 | 50 | 800x600 (mode set, 60 Hz) | 800x600 | 400x300 | 0.25 | into 800x600 |
| exclusive | 800x600 | 16:9 | 50 | 800x600 (mode set) | 800x450 +0+75 | 400x225 | 0.25 | letterbox |
| exclusive | 3840x2160 | Auto | 100 | 3840x2160 (DSR mode, 60 Hz) | 3840x2160 | 3840x2160 | 1.00 | 1:1 |
| exclusive | 3840x2160 | Auto | 50 | 3840x2160 | 3840x2160 | 1920x1080 | 0.25 | into 3840x2160 |
| exclusive | 3840x2160 | Auto | 200 | 3840x2160 | 3840x2160 | 7680x4320 | 4.00 | down to 3840x2160 |
| exclusive | 2560x1440 | Auto | 100 | 1920x1080: refused, "lists no 2560x1440 mode" | 1920x1080 | 1920x1080 | 1.00 | 1:1 |

Other checks:
- The desktop was back at 1920x1080 @ 60 Hz after the exclusive runs.
- Default path (100 %, Auto) against the previous build, headless 15 s / smoke / 25 s: 0 pixels differ, audio and
  state identical.
- Tests: options_display_tests all passed; view_boundary_tests 1295 / 0 failed (new geometry cases); gekko_core_tests
  95 / 0.

Not done or not verified:
- **The scale still applies to the 2D too.** HUD, menus and fades are drawn into the same scaled target as the 3D,
  so below 100 % they are as soft as the scene.
  - A 3D-only scale needs the trailing canvas draws of each view moved to a full-resolution overlay after the present
    scale.
  - They are interleaved with the scene's passes (glare, snapshots, depth), so that is a renderer change with its own
    pixel checks. At 100 % the current path would stay.
- **Not tested by hand:** live Options changes in every order, Alt+Enter, Alt+Tab out of exclusive, monitor and DPI
  changes. This display is at 100 % scaling.
- **The projection uses the render target's pixel aspect:** a scaled render size's rounding gives at most 0.02 %
  anisotropy. The game-side aspect (culling, HUD tests) is the viewport's exact one.
- **Other monitors:** exclusive lists the primary monitor's modes. On another monitor the mode is still set on the
  window's monitor, or refused if that monitor lacks it.
- **854x480 now renders 854x480:** a size within rounding of the output's shape is taken as asked, where before it
  was fitted to 853x480.

## 12. MSAA, anisotropic filtering, full-resolution HUD (2026-11-05)

Six independent settings now: window mode, display resolution, aspect, render scale (section 11), plus MSAA and
anisotropic filtering. Each is saved in display.cfg (`msaa`, `anisotropy`), has a CLI flag (`--msaa`, `--anisotropy`)
and an Options row. Changing one changes nothing else.

**MSAA** (`render/renderer.cpp`):
- The scene's colour and depth targets take the sample count. 1 = single-sample: no multisampled target is allocated
  and the resolve is a copy.
- The pipelines and the target pool were already keyed by sample count, and the resolve shaders read it from a
  constant.
- The count is a live request (`set_msaa_samples`), taken at the start of the next frame under the render lock. No
  swap chain change; targets of the old count age out of the pool.
- Support is checked once with `CheckFeatureSupport(MULTISAMPLE_QUALITY_LEVELS)` for the scene format
  (R16G16B16A16_UNORM) and the depth format both. This RTX 4080: 1, 2, 4, 8 (16x refused).
- A count the GPU lacks is refused and reported (`msaa_note`, the session report, a startup warning). The renderer keeps
  the largest supported count below it. The setting stays as asked, and the Options page shows it as "16x (n/a)".

**Anisotropic filtering** (`render/descriptors.cpp`):
- The 9 Filter::Anisotropic slots of the sampler table are the game's filtered textures (RW filter modes other than
  nearest).
- Off = D3D12_FILTER_MIN_MAG_MIP_LINEAR, the console's own linear filter. 2..16 = D3D12_FILTER_ANISOTROPIC with that
  MaxAnisotropy (the D3D12 hardware limit is 16).
- Point and linear slots (screen copies, effects, point textures) never change.
- Live changes rewrite the slots with the direct queue idle (`wait_idle`), once per change.
- The default stays 16x, the existing behaviour.

**Full-resolution HUD** (`render/renderer.cpp`, `hud_deferred` / "hud overlay"):
- Applies when the render size is scaled into a picture of another size (a scale other than 100 %).
- The draws moved: each view's trailing 2D - the canvas draws after its last 3D draw and its last glare / snapshot /
  history step, sampling no screen copy (the HUD, menus, fades).
- They leave the scaled scene and are drawn after the present scale, onto the picture at its own pixels: the same
  order, anchors, scissors and canvas regions, with a depth-less pipeline (`no_depth`).
- At 100 % (and in captures) nothing moves. `SHADOW_R2_HUD_OVERLAY=0` keeps the old path (A/B).

**Verification**

Targets (frame 300 trace + session report, `.build/perf/quality/`):
- MSAA 1 / 2 / 8: 'view colour' and 'view depth' carry samples 1 / 2 / 8; MSAA 16: refused, 8 in use.
- AF Off: anisotropy_active 1.
- The brief's example - exclusive 800x600, 4:3, 50 %, 8x, 16x:
  - the monitor set to 800x600 @ 60 Hz;
  - 'view colour' and 'view depth' 400x300 with 8 samples;
  - 'present' 800x600; the "hud overlay" pass's viewport 800x600;
  - the desktop back to 1920x1080 afterwards.

Pixels (headless 15 s capture against the default):
- AF Off: 142,395 pixels differ, on textures seen at a steep angle (the far floor, rings, character detail).
- AF 8x: 2,440 pixels differ.
- MSAA Off: 54,037 pixels differ, on geometry edges.
- In all three, game state and audio are identical. Difference maps: `.build/perf/af-ident/af_msaa_diff.png`.

Default identity: 4x, AF 16, 100 % against the build before today's resolution work, at 15 s, the smoke and 25 s: 0
pixels differ, audio and state identical.

HUD (windowed 1280x720, PrintWindow of the presented picture, `.build/perf/hud/hud_compare.png`):
- At 50 % the gauge, digits and ring counter are as sharp as at 100 % (blurred before).
- At 200 % they match 100 % (not drawn at 200 %).

Cost (fullscreen 1920x1080 Westopolis replay, 45 s, boost on; GPU after 15 s; `.build/perf/quality-perf/`):

| Setting | 3D render | GPU p50 / p95 ms | VRAM peak | Repeats | Latch -> vblank p50 / p99 |
|---|---|---|---|---|---|
| MSAA Off | 1920x1080 x1 | 0.50 / 1.16 | 146 MiB | 0 | 22.1 / 25.6 |
| MSAA 2x | 1920x1080 x2 | 0.54 / 1.27 | 169 MiB | 0 | 22.1 / 25.6 |
| MSAA 4x (default) | 1920x1080 x4 | 0.65 / 1.55 | 224 MiB | 0 | 22.1 / 25.6 |
| MSAA 8x | 1920x1080 x8 | 1.01 / 2.44 | 352 MiB | 0 | 22.1 / 25.6 |
| AF Off (4x) | 1920x1080 x4 | 0.65 / 1.55 | 224 MiB | 0 | 22.1 / 25.6 |
| Scale 50 % (4x) | 960x540 x4 | 0.38 / 0.63 | 116 MiB | 0 | 22.1 / 25.6 |
| Scale 200 % (4x) | 3840x2160 x4 | 2.17 / 5.20 | 681 MiB | 0 | 22.1 / 25.6 |

Tests: options_display_tests (new MSAA / AF rows, unsupported count shown and kept), view_boundary_tests, and
gekko_core_tests all pass.

**Not verified:**
- Changing MSAA, AF or the scale live through the Options page in the running game. The apply path runs only on a
  change; startup values were tested.
- Alt+Enter, Alt+Tab out of exclusive, and monitor / DPI changes.
- HUD overlay in menus, fades, split screen and cutscene boxes (Westopolis gameplay only).
- MSAA 16x and above (this GPU refuses them).
- **A precision difference at non-100 % scales:** the overlay blends onto the 8-bit picture where the scene path blends
  onto the 16-bit scene, so a HUD pixel over the scene can round differently. 100 % is unaffected.

## 13. Gameplay aspect and resolution-free replays (2026-11-05)

**What reaches the game.** A read-only trace of every consumer of the game output found that only its aspect reaches
the simulation:
- **Gameplay:**
  - the enemy AI's on-screen test (`displayed_screen_point`, 12 enemy classes);
  - ElecCoreWall's visibility-gated animation (`native_frustum_test`, caller returning to 0x80264CF8).
- **Drawing and HUD only** (written into guest memory but read only there): the frustum planes / sector array, the
  enemy gauge and CSD positions, the lens flare.
- **Never reach game state:** the render size, render scale, MSAA, filtering and window size.

**The split.**
- Those two tests take a logic aspect: `RwDriver::logic_aspect()` and `System::set_logic_aspect`.
- Live play keeps the picture's (`gameplay_aspect = follow`, the default). It can also be set to `original` (4:3) or
  any ratio (display.cfg `gameplay_aspect`, `--gameplay-aspect`).
- A replay feeds the recorded one. Tape version 3 adds logic / meta / setting / check records (docs/tools/RECORDER.md);
  a version 2 tape's output w/h is its logic.

**Verified:**
- **Westopolis, 40 s:** a v3 tape recorded at 1920x1080, replayed at 3840x1200 and 1280x960.
  - 39 / 39 checkpoints matched; the audio was identical.
  - The end state differed in 8-11 of 510 presentation-written blocks.
- **Lava Shelter, 178 s:** a v3 tape recorded at its own 3840x1200, replayed at 1920x1080.
  - Stage, RNG, player and mission matched at all 177 checkpoints; the audio was byte-identical.
  - The camera group differed at 4 checkpoints from 46.0 s.
  - The end state differed in 91 of 499 blocks.
- **The control** (logic following the 16:9 picture, the behaviour before the split) gave exactly the same result,
  bit-identical end state on both routes. On these two routes the enemy test never flipped a verdict between the
  aspects, so the split changed nothing measurable.
- **What this means:** the camera difference comes from another aspect-dependent path that the trace did not find, a
  real lead.
- Live default (`follow`): pixels / audio / state against the build before, at 15 s / smoke / 25 s (below).

**Open:**
- Find what moves the camera frame with the picture aspect in Lava Shelter around 46 s. Candidates: an event camera
  framed from `output_aspect()`, or a camera that reads the RwCamera's view window or frustum.
- Whether those 4 transient camera differences ever reach gameplay: on this route they did not (all other groups and
  the audio stayed identical through the end).
