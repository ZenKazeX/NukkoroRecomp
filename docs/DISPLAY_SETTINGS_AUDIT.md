# Display settings audit (2026-11-05)

Read-only investigation of the display settings system of the lifted (release) build: Options menu -> settings
registry -> DisplaySettingsService / display.cfg / CLI -> display_geometry -> game output, camera, HUD -> renderer ->
present. No code was changed. Paths are relative to `runtime/` unless they start with `tools/` or `docs/`.

Evidence levels used below:
- **CONFIRMED (run)**: the decisive arithmetic was executed against the real headers (`gekko/display_geometry.h`,
  `render/view.h`) in a standalone probe, or observed in a real file / session report.
- **CONFIRMED (code)**: traced by hand through every function on the path; not executed in the game.
- **DESIGN**: not a defect of one function but of the model; behaviour is as the code intends.

The game was not launched: `.build/options-menu/RUN.lock` was held by another session ("title-trail session: tests
build + ctest") for the whole investigation. The probe was compiled with clang++ -std=c++20 against
`runtime/include` and `runtime/lift`; its source and output are reproduced in the appendix.

---

## 1. The system as it is

### 1.1 Settings and where they live

| Setting | Menu row | display.cfg key | CLI | Type in DisplaySettings | Consumer |
|---|---|---|---|---|---|
| Window mode | Display > Display mode (Windowed / Fullscreen = borderless / Exclusive) | `fullscreen`, `exclusive` | `--fullscreen --windowed --borderless --exclusive` | 2 bools | UI thread: `HostWindow::set_mode` |
| Display resolution | Display > Resolution | `width`, `height` (`window_width`, `window_height`) | `--width --height` | `window_width/height` | UI thread: windowed client size / exclusive mode; ignored in borderless |
| Aspect | Display > Screen ratio | `aspect` (+ `aspect_ratio`, `custom_aspect`) | `--aspect`, `--aspect-ratio` | `aspect` double, 0 = Auto | game thread: viewport, camera, HUD, logic (follow) |
| Render scale | Graphics > Render resolution | `render_scale` | `--render-scale` | `render_scale` percent | game thread: render size |
| Exact render size | Graphics > Render resolution ("exact" entry, only when in effect) | `render_width`, `render_height` | `--render-width --render-height` | 2 ints, 0 = by scale | game thread: render size, wins over the scale |
| MSAA | Graphics > Anti-aliasing | `msaa` | `--msaa` | int | renderer |
| Anisotropy | Graphics > Texture filtering | `anisotropy` | `--anisotropy` | int | renderer samplers |
| V-Sync, FPS counter | Display | `vsync`, `fps_counter` | `--vsync --no-vsync --fps-counter` | bools | UI thread |
| Gameplay aspect | none | `gameplay_aspect` | `--gameplay-aspect` | double, 0 = follow | game logic on-screen tests |
| Render rate, frame blending, draw distance | none (withheld) | `render_rate`, `frame_interpolation`, `draw_distance` | flags | | UI / game thread |
| HUD scale / inset | none | `hud_scale`, `hud_inset` | `--hud-scale --hud-inset` | DisplayConfig only | legacy renderer only (dead in the lifted build) |
| Framing | none | none | `--framing square|game` | Options | RwDriver |
| Headless output | none | none | `--native-output WxH` (only with `--native-screenshot`) | Options | game output in headless captures |

Precedence at start (`shadow_main.cpp resolve_display_config`, `main` around line 1655): defaults (1280x720 windowed,
Auto, 100 %) < display.cfg < CLI. `settings_aspect()` snaps a float ratio within 1e-5 of a named one to the exact
double. The result initialises `gekko::display_settings()`; from then on `DisplaySettingsService::apply` is the only
writer. `save()` writes only the keys changed since `initialize()`.

### 1.2 The geometry pipeline

```
 display.cfg + CLI --> DisplaySettings --(UI thread, kDisplayWindowChange)--> HostWindow::set_mode / create
                                |                                                 |
                                |                                     client area / monitor mode = OUTPUT
                                |                                                 | (pending_game_output)
                                +--(game thread, kDisplayGameChange, latch_game_output)--+
                                                                                         v
                        gekko::display_geometry(output, aspect, scale, exact)   [display_geometry.h]
                          viewport = fit_aspect(output, aspect)        (Auto: the whole output)
                          render   = exact ? exact : scale_size(viewport, scale)   (per-side clamp 16384)
                                                                                         |
   set_game_output(viewport)  -> RwDriver::set_output_aspect(viewport aspect)  -> camera window (raster_view,
                              -> Renderer::set_present_aspect(viewport aspect)     displayed_window), culling,
                              -> System::display().resize                         HUD canvas_fit, visible_screen,
                              -> logic aspect (follow: = viewport aspect)          enemy on-screen tests
   set_render_size(render)   -> render::render_size_for(render, viewport)  -> the renderer's targets
                                (special case: Auto + 100 % + not exact => 0x0 = "the back buffer's size")
                                                                                         |
   Renderer: views drawn in a box of packet.content_aspect inside the RENDER TARGET (4:3 during in-engine events,
             RwDriver::sample_event_framing) -> "present scale" maps the render target onto
             fit_aspect(back buffer, present_aspect) -> HUD overlay at picture pixels when scaled -> swap chain
```

There are therefore **three independent fit operations** (display_geometry's viewport, render_size_for's fit, the
presenter's own fit_aspect of the back buffer) and **two event-box operations** (content_aspect box in the render
target, output_aspect() = 4:3 for the camera). They agree in the common cases and disagree in the cases listed below.

### 1.3 The Options menu path

- `native_options_menu.cpp` calls `settings::registry()` only; `settings_display.cpp` provides the Display and
  Graphics categories over one `options_display::Model` each (`native_options_display.cpp`).
- `Model` stores **list indices** (`value[row]`), not values. Lists are rebuilt from other rows' values on every call
  (`resolutions()` from the mode, `scales()` from the viewport of resolution + ratio).
- `begin()` snapshots `display_settings().current()` into the model when a page opens or a tile is hovered.
- Display apply: a mode or resolution change starts the 15 s trial (`Registry::apply_pending` -> `TrialStarted`);
  aspect / vsync / fps apply at once and are saved. Graphics apply is always immediate and saved.
- Alt+Enter (`shadow_main.cpp` ~5414) bypasses the registry: `display_settings().apply` + `save()`.

---

## 2. Findings

Ordered by impact on what the player sees. Each: current behaviour / root cause / expected / recommended solution.

### F1. The 4:3 event box is nested inside the chosen picture (windowboxing) - CONFIRMED (run)

1. **Current.** During in-engine events the renderer boxes every view in a 4:3 rectangle *of the render target*
   (`renderer.cpp` ~1545, `box_w/box_h` computed from `output.width/height`, which is the render size). The render
   target is already the viewport of the Screen ratio. With the user's own setup (borderless 1920x1080, 32:10, 200 %)
   the viewport is 1920x600, the render 3840x1200, the event box 1600x1200, shown as **800x600 = 23 % of the screen**,
   black on all four sides. A 4:3 box on the physical screen would be 1440x1080 = 75 %.
2. **Root cause.** Two different "picture" concepts are layered without a rule: the Screen ratio's viewport
   (`display_geometry`) and the event's content aspect (`RwDriver::sample_event_framing` -> `FramePacket::content_aspect`
   in `scaffold/rw_driver/frames.cpp:272`). The event override is applied inside the result of the first instead of
   replacing it. `RwDriver::output_aspect()` returns 4:3 consistently, so the camera is right; only the placement is
   wrong.
3. **Expected.** An event that is shown 4:3 is fitted into the *output* (the screen or window), like any other picture.
   Whether a fixed Screen ratio should still constrain events is a user decision, but nesting two letterboxes is never
   the intent.
4. **Solution.** Make the event aspect an input of the one geometry rule: while `event_framed_` is set, the game thread
   recomputes `display_geometry(output, 4/3, scale)` (viewport 1440x1080, render 2880x2160 at 200 %) and publishes it
   as the game output; `content_aspect` then becomes unnecessary for the whole-screen case (keep it only if a partial
   box is ever needed). This also stops rendering a 3840x1200 target of which 42 % is used. The switch must be latched
   at the same host service that sets `event_framed_`, so the camera and the target change on the same frame.

### F2. "Exact render size" is neither exact nor visible as such - CONFIRMED (run, and in the user's own display.cfg)

1. **Current.** `render_width x render_height` wins over the scale (`display_geometry`), but the renderer then fits it
   to the viewport's shape (`render_size_for`, `lifted_render.cpp apply_render_size`). The Graphics row still shows the
   asked size (`settings_display.cpp` view of `kRenderResolution`: `size_text(model_.exact...)`).
   - `../game/display.cfg` (the deployed game's file) has `render_width=720`, `render_height=540`, `fullscreen=true`,
     `aspect=auto`: the menu shows "720 x 540", the renderer renders **720x405**.
   - Launcher-style `--render-width 3840 --render-height 2160` with Screen ratio 32:10: menu "3840 x 2160", renderer
     **3840x1200**.
   - The session report's `geometry.render` shows the asked size, not the one rendered.
2. **Root cause.** Two owners of the render size: `display_geometry` (decides exact vs scale) and `render_size_for`
   (re-fits). The exact size predates the viewport model (RESOLUTION_AUDIT section 8) and was kept "for benchmarks and
   tapes" (section 11) without being reconciled with the aspect setting.
3. **Expected.** One function decides the render size, and what it decides is what the menu and the report show.
4. **Solution.** Remove the exact size from the player model:
   - display.cfg / Options: on load, convert `render_width/height` into a scale (`render_height / viewport.height`, the
     limiting side), log the conversion, and stop writing the keys (the Graphics row then never needs an "exact"
     entry).
   - CLI / benchmarks: keep `--render-width/--render-height` as a session-only override that **sets the viewport too**
     (aspect = W/H) or is refused when it disagrees with the aspect, so it is truly exact.
   - Report `render_size_for`'s output as `geometry.render`.

### F3. The launcher and the in-game menu use different models, and the launcher wins every launch - CONFIRMED (code)

1. **Current.** `tools/resolution_presets.py command_flags` always passes `--render-width W --render-height H` (plus
   `--width/--height` windowed, or `--fullscreen`). The CLI beats display.cfg. So in a launcher-started game the
   Graphics row is always the exact size (F2), and a render scale or mode chosen in the menu is saved to display.cfg
   and silently overridden on the next launch. The launcher offers no Screen ratio, no scale, no exclusive mode.
2. **Root cause.** CLI flags are treated as persistent preferences (they are re-applied each launch) while the menu
   writes a file the flags override. The launcher still implements the 2026-10-26 model ("one exact size = window and
   render size"); the runtime moved to the 2026-11-05 model (output + aspect + scale).
3. **Expected.** One source of truth for player preferences (display.cfg), written by both the menu and the launcher;
   CLI flags only for session overrides (tests, benchmarks), shown as such.
4. **Solution.** Launcher: write the chosen values into display.cfg (mode, size, aspect, scale) and pass no display
   flags. Runtime: remember which settings a CLI flag forced; the Options page shows them as "set by the command line"
   (disabled, or editable with a note that the change applies next launch only without the flag).

### F4. Display trial safety can be bypassed: unconfirmed modes reach display.cfg - CONFIRMED (code)

1. **Current.** While a trial runs, `Registry::save` refuses, but Alt+Enter calls `display_settings().save()` directly
   (`shadow_main.cpp` ~5420). `save()` writes every key changed since start, including the trial's unconfirmed
   resolution / mode. If the trial then expires, `trial_revert()` restores memory but never rewrites the file: the next
   launch starts in the unconfirmed (possibly black) mode the trial exists to prevent.
   Also: the Alt+Enter toggle is itself undone by the trial's revert (it copies all display fields of `before_trial_`).
2. **Root cause.** Persistence is reachable from two owners (Registry and the Alt+Enter handler), and the service saves
   "current" rather than "last confirmed".
3. **Expected.** display.cfg only ever holds confirmed values.
4. **Solution.** Route Alt+Enter through the Registry (as a Display change with its own trial, or refuse it during a
   trial), and give `DisplaySettingsService` a `confirmed` copy: `save()` writes `confirmed`, `apply()` of a trial does
   not update it, `confirm()` does.

### F5. The open Options page goes stale after Alt+Enter (and other out-of-band changes) - CONFIRMED (code)

1. **Current.** The page's model is taken at `begin()` (page open / tile hover). Alt+Enter changes the mode without
   calling `begin()`. The page keeps showing the old mode as the one in effect (`SettingView::applied` comes from
   `applied_`, taken at begin); pressing A applies the stale model and reverts the Alt+Enter (plus starts a trial).
2. **Root cause.** The model caches a copy of the service state and has no change notification.
3. **Expected.** The page reflects the settings in effect at every frame; pending edits are deltas over them.
4. **Solution.** Give the service a generation counter; providers re-take the applied state when it changes and keep
   only the rows the player has actually edited as pending.

### F6. Exclusive fullscreen fails silently and the setting keeps claiming it - CONFIRMED (code)

1. **Current.** `apply_display_mode` (`host_window.cpp`) refuses a size the monitor does not list and leaves the window
   borderless at the desktop size; the note goes only to stderr (`g_exclusive_note`). The Options page still shows
   "Exclusive / WxH", the trial can be confirmed (the picture is fine, just not what was chosen), and the unsupported
   mode is saved and refused again at every start.
   The menu itself offers such sizes: `Model::resolutions()` in exclusive mode lists the monitor's modes **plus** the
   size in effect and the chosen windowed size (`custom`, `windowed`), e.g. a 1765x993 work-area window size.
2. **Root cause.** The list mixes "the monitor can do" with "the player had"; the window owner has no way to report the
   actual mode back to the settings owner.
3. **Expected.** Exclusive lists only real modes; a refused mode is shown as refused (the trial dialog says so) and is
   not confirmable as if it worked.
4. **Solution.** In exclusive mode list only the monitor's modes (keep the chosen size for windowed separately, see R2);
   publish an `actual` window state (mode, size, exclusive active) from the UI thread next to `set_screen_size`, show
   it in the page, and fail the trial immediately when `exclusive_active()` is false after an exclusive apply.
   Related: exclusive lists the *primary* monitor's modes at startup (`main`, `o.primary`), the mode is set on the
   *window's* monitor; and the list comes from DXGI `GetDisplayModeList` while the switch uses `EnumDisplaySettingsEx`
   with a bit-depth filter - two enumerations that can disagree.

### F7. "Resolution" means three things, and in windowed mode it is often not the size you get - CONFIRMED (code + measured in RESOLUTION_AUDIT 11)

1. **Current.**
   - Windowed: the client size, but shrunk to the largest of its shape inside the work area
     (`windowed_client_size`). On a 1920x1080 desktop every 16:9 choice from 1920x1080 up (2560x1440, 3840x2160, ...)
     gives the same ~1765x993 client. The row keeps showing the chosen size.
   - Exclusive: the monitor mode.
   - Borderless: ignored (row disabled, value kept for the other modes).
   The help text says "Higher resolutions give a sharper image", which is false in windowed mode above the work area
   and in borderless.
2. **Root cause.** One field (`window_width/height`) carries the windowed client size, the exclusive mode and a memory
   for borderless; the window owner does not report back what it achieved.
3. **Expected.** Each mode has its own, honest size: windowed = window size (only sizes that fit offered, or the actual
   client shown next to the request), exclusive = monitor mode, borderless = the desktop (shown, not stored).
4. **Solution.** Split into `window_size` (windowed) and `fullscreen_mode` (exclusive) in DisplaySettings and
   display.cfg (keep reading `width/height` into both for compatibility). Windowed lists only presets that fit the
   work area (plus "maximised"); show "requested / actual" when they differ.

### F8. Windowed + a fixed Screen ratio is always windowboxed inside a window locked to another shape - DESIGN

1. **Current.** Since 2026-11-05 the aspect never shapes the window; the window is locked to the Resolution's shape
   (`set_content_aspect(window_width / window_height)`, WM_SIZING). Choosing 4:3 in a 1280x720 window gives a 960x720
   picture with 160 px bars in a window the player cannot reshape to fit it; the Resolution list no longer filters by
   ratio, so nothing guides the player to a matching size.
2. **Root cause.** Two settings both define the shape of a windowed picture (Resolution's W:H and Screen ratio); the
   decoupling rule chose the Resolution for the window and the ratio for the picture.
3. **Expected.** In a window, bars are never necessary: the window can take the picture's shape.
4. **Solution.** Windowed: the Resolution row is a *size budget* (or the window height), the client takes the
   viewport's shape (fit of the ratio inside the chosen size and the work area) - no bars, and dragging keeps that
   shape. Fullscreen keeps today's rule (bars are unavoidable there). This restores the pre-11 behaviour for windowed
   only, without re-coupling the render size to the window.

### F9. Values are stored as list indices over lists that move - CONFIRMED (run for the scale case)

1. **Current.** `Model::scales()` filters percentages whose render exceeds 16384 per side; `value_index` then picks the
   largest listed below the value in effect. With a 5120x1440 viewport and `render_scale=400` in effect the row shows
   **300 %**; applying any Graphics change (MSAA, AF) writes 300 to display.cfg. Similar index shifts happen when the
   "exact" entry (index 0) appears or disappears, and when a resolution list changes with the mode (handled today only
   because the chosen size is force-listed, which causes F6).
2. **Root cause.** The page model is index-based; values are reconstructed from indices by `Model::settings()`.
3. **Expected.** A value in effect is shown as itself and is never changed by an unrelated edit.
4. **Solution.** Keep values (`double scale`, `PixelSize size`, ...) in the model; lists are for stepping only (step
   moves to the next list entry above / below the current value; a value not in the list is shown as "custom"). Rows
   not edited by the player are copied from the settings in effect, never from the list.

### F10. The per-side 16384 clamp silently changes the shape and the reported size - CONFIRMED (run)

1. **Current.** `scale_size` clamps each side independently: 400 % of 5120x1440 -> 16384x5760 (aspect 2.84 instead of
   3.56); `render_size_for` then repairs it to 16384x4608 (an effective 320 %). The report says 16384x5760. The CLI and
   display.cfg accept 400 % regardless of the viewport; there is no memory budget check (400 % of a DSR 3840x2160
   output at 4x MSAA RGBA16 + D32 is about 15360x8640 x 4 samples x 12 bytes = 6.4 GB; not run).
2. **Root cause.** The limit is applied per side in one function and the aspect repaired in another.
3. **Expected.** The scale is clamped uniformly (the largest percentage at which both sides fit), reported as the
   effective scale, and refused or capped against the GPU budget with a visible reason.
4. **Solution.** `scale_size`: compute `s_max = min(16384 / w, 16384 / h)` and use `min(s, s_max)` on both sides;
   expose `effective_scale` in DisplayGeometry; add a VRAM estimate gate (scene targets dominate: RESOLUTION_AUDIT 9c)
   that caps the list and logs the cap.

### F11. Supersampling has three overlapping controls with different results - DESIGN

1. **Current.** The same 3840x2160-class image on a 1920x1080 monitor can be had by (a) borderless + 200 %, (b)
   exclusive DSR 3840x2160 + 100 %, (c) DSR desktop 3840x2160 + borderless + 100 %; and (b)/(c) plus 200 % gives
   7680-class renders. They differ in the downsampling filter (engine 2x2 box vs NVIDIA DSR smoothness), in where the
   HUD is drawn (1080 picture pixels vs 2160 then driver-downsampled), in input latency (exclusive mode switch), and
   "100 %" means a different pixel count depending on a driver setting outside the game. DSR modes are listed exactly
   like physical modes. The latest session report in `.build/options-menu/run` shows borderless 1920x1080 at 200 % ->
   3840x2160, i.e. path (a).
2. **Root cause.** The render scale is relative to the output, and the output may itself be virtual (DSR).
3. **Expected.** One control for "how many pixels the 3D is rendered with" and the output resolution reported as what
   it physically is.
4. **Solution.** Keep the render scale as the supersampling control. Detect virtual modes (DSR/VSR modes are larger
   than the monitor's preferred / native mode from `EnumDisplaySettings` / the EDID native size already read for
   `monitor_native`) and label them ("3840 x 2160 (DSR)"); show the effective render pixels ("3840 x 1200 rendered") on
   the Render resolution row so equal results look equal.

### F12. The Screen ratio is also a gameplay setting - DESIGN (documented in RESOLUTION_AUDIT 13)

1. **Current.** With `gameplay_aspect = follow` (default, not in the menu) the logic aspect = the viewport aspect: the
   Screen ratio changes enemy on-screen tests and ElecCoreWall's gated animation. During events `logic_aspect()`
   returns 4:3 even when `gameplay_aspect` is forced. The camera divergence in Lava Shelter at 46 s (section 13 Open)
   shows at least one more aspect-dependent path.
2. **Root cause.** A presentation setting is the default input of game logic.
3. **Expected.** The menu text for Screen ratio says only "the shape of the picture"; either that is true or the side
   effect is stated.
4. **Solution.** User decision. Options: (a) default `gameplay_aspect` to the picture as now but state it in the help;
   (b) a Gameplay-category row "Enemy view: follow screen / original 4:3"; (c) default to 4:3 logic for replay
   portability. The forced value should also win over the event 4:3 when the player forced it (or the doc should say
   why not).

### F13. CLI and display.cfg accept different grammars; dead and stale keys - CONFIRMED (code)

- `--aspect 32:10` / `--aspect 5:4` exit with "unknown aspect mode" (`parse_aspect_mode` only knows the old six names),
  while `aspect=32:10` in display.cfg and the menu accept them (`parse_display_file` falls back to W:H).
- `--render-width 2560` without a height is accepted and ignored: `display_geometry` needs both sides
  (`exact_render`), the "follows" rule then renders at the window's size. `view.h render_size_for`'s one-side branch
  and its header comment ("the launcher always gives both") are dead for the game. CONFIRMED (run): 1920x1080.
- `aspect_ratio=` after `aspect=21:9` is silently ignored (only switches Auto to Custom).
- `hud_scale`, `hud_inset` are parsed, written to the template, and do nothing in the lifted build; the startup
  WARNING claims they "disable the byte-identical 4:3 path", which is only true of the legacy renderer.
- The display.cfg template text (in both `../game/display.cfg` and the run copy) still says `# 0 = follow the window
  size` for render_width and lists `auto / 4:3 / 16:9 / 16:10 / 21:9 / 32:9 / custom` (no 5:4, 32:10).
- The headless output uses `width x height` even when the cfg says fullscreen (`headless_output_w`), and
  `--native-output` is honoured only together with `--native-screenshot`.

Solution: one parser for each value type shared by CLI and cfg (`parse_aspect` returning a double, as
`settings_aspect` already normalises); refuse a one-sided render size; drop the dead keys from the template and warn
"ignored by this build" when present; regenerate the template comments from the key table.

### F14. Named "21:9" is 2.333, real ultrawide panels are 2.37-2.39 - CONFIRMED (code)

1. **Current.** `kNamedAspects` 21:9 = 7/3. On a 3440x1440 (2.389) panel it gives a 3360x1440 viewport with 40 px bars
   each side; on 2560x1080 (2.370), 2520x1080. `display_modes.h aspect_family` calls those panels "21:9" (a band),
   so the Resolution list and the ratio list use the same name for different numbers.
2. **Expected.** Choosing the ratio a monitor is sold as fills that monitor.
3. **Solution.** Either name the ratio by the band (pick the output's exact ratio when the output is in the band) or
   rename the row entry "2.33:1" and add "Native" (= Auto). The simplest consistent rule: a named ratio within its
   family band of the output's own shape resolves to the output's exact shape.

### F15. Smaller inconsistencies - CONFIRMED (code) unless noted

- **Mode names differ between layers:** registry English reference `{"borderless","Borderless"}, {"exclusive",
  "Fullscreen"}` (`settings_display.cpp`), menu text table `word.borderless = "Fullscreen"`, `word.exclusive =
  "Exclusive"` (`options_menu_texts.inc`). The screen is right; the registry's reference wording is the opposite.
- **Ratio list order** is 4:3, 5:4, 16:9, 16:10, ... (not monotonic; `kNamedAspects` order) while the header comment
  documents 16:10 before 16:9.
- **Borderless after exclusive:** `set_screen_size` publishes `rcMonitor`, which in exclusive mode is the mode's size;
  opening the page in exclusive and switching to borderless shows the exclusive size as borderless's resolution until
  the page is reopened.
- **Auto/100 % uses a different code path** ("follows": render 0x0 = back buffer, taken at once by the UI thread)
  from every other combination (render latched at the game thread's next host service). Between a resize and the latch
  the camera aspect and the render target can differ for a frame (transient stretch; not observed, by reading).
- **HUD differs between 100 % and other scales:** at 100 % the HUD blends into the 16-bit scene, otherwise into the
  8-bit picture (documented in RESOLUTION_AUDIT 12).
- **Revert keeps the key dirty:** a trial revert or "changed and changed back" leaves the key in `changed_keys_`, so a
  later save persists a value that came from the CLI, contrary to the service's own contract.
- **Help texts promise more than the code does** ("Auto fills the screen" - not during events; "without changing ...
  the HUD" - not on the exact path; "Higher resolutions give a sharper image" - see F7).
- **PAL 640x528 canvas:** HUD anchor classification uses a 480-tall canvas constant (`hud_layout.h kHudCanvasHeight`;
  effect unverified, RESOLUTION_AUDIT 6).

---

## 3. How the settings actually depend on each other

| Changing ... | ... also changes | Visible to the player? |
|---|---|---|
| Display mode | output (desktop / mode / client), hence viewport, render size, Auto's shape, logic aspect | partly (Resolution row disabled in borderless) |
| Resolution (windowed) | window shape lock, Auto's shape, render size; capped by the work area | no (F7) |
| Resolution (exclusive) | monitor mode; refused sizes fall back silently | no (F6) |
| Screen ratio | viewport, camera, HUD placement, render size, **game logic** (follow), event box size | partly (F1, F12) |
| Render scale | render size; HUD path (overlay vs scene) | yes, except clamp (F10) |
| Exact render size | overrides the scale; re-fitted to the ratio | no (F2) |
| CLI flags | override cfg every launch | no (F3) |
| DSR (outside the game) | what Borderless's output and "100 %" mean | no (F11) |
| In-engine event | camera 4:3, box inside the viewport, logic 4:3 | yes, oddly (F1) |
| Tape `--replay-as-recorded`, `--native-output` | replace the output and drop the Screen ratio | logged only |

Circular / two-owner relations found:
- Render size: `display_geometry` (exact vs scale) and `render_size_for` (re-fit) (F2, F10).
- Picture placement: viewport offset in `display_geometry`, re-derived by the presenter's `fit_aspect(back buffer,
  present_aspect)`, plus the event box in the render target (F1).
- Window shape: Resolution's W:H (window lock) and Screen ratio (picture) (F8).
- Persistence: Registry (with trial) and Alt+Enter (without) (F4); CLI and display.cfg (F3).
- Displayed state: page model snapshot vs service state (F5); window owner's actual state never reported (F6, F7).

---

## 4. Redesign proposal

Goal: every setting has one responsibility, dependencies are one-directional and predictable, and what is shown is
what is rendered.

### 4.1 The model

```
  Player preferences (display.cfg, the only persistent store)
    window_mode        windowed | borderless | exclusive
    window_size        W x H              (windowed only: a size budget)
    fullscreen_mode    W x H @ Hz         (exclusive only: one of the monitor's real modes)
    picture_aspect     auto | ratio       (the picture's shape)
    render_scale       percent            (3D pixels relative to the picture)
    msaa, anisotropy, vsync, fps_counter, ...
    gameplay_aspect    follow | original | ratio   (game logic; a Gameplay row or documented default)

  Session overrides (CLI, tapes, captures): same fields, never saved, shown as overridden.

  One pure function, ONE owner of all derived sizes:
    resolve(preferences, overrides, monitor_state, game_state{event_4x3, two_players}) ->
      Achieved { window_mode, output WxH (physical, as the window owner reports it), virtual_mode flag,
                 picture rect in the output, picture aspect, render WxH, effective scale, logic aspect,
                 refusals[] (mode not available, scale capped, ...) }
```

Rules inside `resolve` (in this order, no other function re-fits):
1. Output: windowed = fit of `window_size` (and, if a fixed aspect, of the picture's shape) inside the work area;
   borderless = monitor; exclusive = `fullscreen_mode` if the monitor lists it, else borderless + refusal.
2. Picture aspect: event_4x3 ? 4:3 : picture_aspect (auto = output's). Named ratios inside a family band snap to the
   output's exact shape (F14).
3. Picture rect = fit(output, aspect) - for events too (F1).
4. Render = picture x effective_scale, where effective_scale = min(asked, the 16384 limit on both sides, the VRAM
   budget) (F10). A CLI exact size is an override of (aspect, scale) together, not a third mechanism (F2).
5. Logic aspect = override / gameplay_aspect / picture aspect.

The game thread consumes `Achieved` at host services; the renderer receives the picture rect and render size and does
no fitting of its own (the presenter's fit and `render_size_for` go away); the Options page and the session report
show `Achieved` next to the preferences.

### 4.2 Prioritised steps (each keeps existing behaviour where it is already right)

| P | Step | Fixes | Risk / check |
|---|---|---|---|
| 1 | Save only confirmed values; route Alt+Enter through the registry; refresh the page model on a settings generation change | F4, F5 | unit tests in options_display_tests / registry tests; no pixel change |
| 2 | Event box in the output: event aspect as a `display_geometry` input, latched with `event_framed_` | F1 | pixel A/B on an event at 4:3, 16:9, 32:10, 32:9; 2P intros unchanged (excluded already) |
| 3 | Window owner reports the achieved state (exclusive active, client size, monitor, virtual mode); exclusive lists only real modes; refused trials fail visibly | F6, F7 (display), F11 (labels) | manual: refused mode, DSR mode, Alt+Tab |
| 4 | Value-based page model; uniform scale clamp + effective scale + VRAM cap | F9, F10 | options_display_tests; view_boundary_tests for the clamp |
| 5 | Retire the exact render size from cfg/menu (convert to a scale on load); CLI exact = aspect + scale override; launcher writes display.cfg instead of flags | F2, F3 | benchmark scripts that pass --render-width must still get exactly that size; tapes unaffected (they carry logic, not render) |
| 6 | Split `window_size` / `fullscreen_mode`; windowed client takes the picture's shape | F7, F8 | read old `width/height` into both; windowed captures |
| 7 | One shared value parser for CLI and cfg; drop dead keys and stale template text; refuse one-sided render sizes | F13 | test_launcher, display parse tests |
| 8 | Ratio naming (band snap or honest names) and order; mode wording in the registry | F14, F15 | menu text review in six languages |
| 9 | Decide and state the gameplay-aspect policy (user decision); find the Lava Shelter camera path | F12 | replay_report on the existing v3 tapes |

### 4.3 What must not break

- 100 % + Auto must stay pixel-identical to today (the default identity gate of RESOLUTION_AUDIT 11/12).
- Replays: tapes record logic aspect, not render size; nothing above changes that. `--replay-as-recorded` remains a
  session override of the output.
- The menus' original 4:3 composition (centred canvas) and the per-cast anchoring are untouched: they consume the
  picture rect, which only changes during events (step 2).
- Benchmarks that ask for an exact size keep getting exactly it (step 5 makes that true for the first time when the
  aspect differs).

---

## Appendix: probe

Source: `geom_probe.cpp` in the investigating session's scratchpad (not kept in the repo), compiled against
`runtime/include/gekko/display_geometry.h` and `runtime/lift/render/view.h` as they were on 2026-11-05; the event box
and the menu's `value_index`/`fits` were re-typed from `renderer.cpp` (~1545) and `native_options_display.cpp`.

```
user setup: borderless 1920x1080, 32:10, 200%  output 1920x1080 viewport 1920x600+0+240  geometry.render 3840x1200 (exact=0)  renderer gets 3840x1200
same on a DSR 3840x2160 exclusive mode         output 3840x2160 viewport 3840x1200+0+480  geometry.render 7680x2400 (exact=0)  renderer gets 7680x2400
same at 100% on DSR 3840x2160                  output 3840x2160 viewport 3840x1200+0+480  geometry.render 3840x1200 (exact=0)  renderer gets 3840x1200
game/display.cfg: 1920x1080 Auto, exact 720x540 output 1920x1080 viewport 1920x1080+0+0  geometry.render 720x540 (exact=1)  renderer gets 720x405
launcher-style exact 3840x2160 + ratio 32:10   output 1920x1080 viewport 1920x600+0+240  geometry.render 3840x2160 (exact=1)  renderer gets 3840x1200
--render-width 2560 alone (no height)          output 1920x1080 viewport 1920x1080+0+0  geometry.render 1920x1080 (exact=0)  renderer gets 1920x1080
--render-scale 400 on a 5120x1440 desktop      output 5120x1440 viewport 5120x1440+0+0  geometry.render 16384x5760 (exact=0)  renderer gets 16384x4608
windowed 1280x720 client, ratio 4:3            output 1280x720 viewport 960x720+160+0  geometry.render 960x720 (exact=0)  renderer gets 960x720
event 4:3 box: render 3840x1200 -> box 1600x1200 -> on screen 800x600 (23.1% of 1920x1080); a 4:3 box on the whole screen would be 1440x1080 (75.0%)
event 4:3 box: render 1920x1080 -> box 1440x1080 -> on screen 1440x1080 (75.0% of 1920x1080); a 4:3 box on the whole screen would be 1440x1080 (75.0%)
menu: viewport 5120x1440, render_scale=400 in effect -> Graphics row shows 300%
menu: viewport 1920x600, render_scale=200 in effect -> Graphics row shows 200%
```

Not verified in the running game: every finding above (the lock was held throughout). The highest-value in-game
checks are F1 (any story event at 32:10), F4 (Alt+Enter during a resolution trial, then let it expire, then read
display.cfg) and F6 (exclusive at a size the monitor does not list).

---

## 5. First implementation (2026-11-05) - withdrawn

The first implementation pass had Auto/Manual game resolutions, preset lists, typed width/height, a separate exclusive
mode row and per-frame 4:3 framing of menus, movies and cutscenes. The user tried it and rejected it: too many settings,
and the picture jumped between 16:9, 4:3 and 32:10. Section 6 replaced it the same day. Of that pass only the
non-visible plumbing remains: confirmed vs provisional saving, Alt+Enter through the registry, page resync, the
value-based page model, exclusive refusal reporting, the shared aspect parser, and command-line overrides that are
never saved.

## 6. The display settings now (2026-11-05, user-approved design)

### 6.1 Four settings, two pages

| Page | Row | Values | Key |
|---|---|---|---|
| Display | Display mode | Windowed / Fullscreen (borderless at the desktop's size) | `fullscreen` |
| Display | Screen ratio | Auto, 4:3, 5:4, 16:10, 16:9, 21:9, 32:10, 32:9 (a custom ratio in display.cfg is listed) | `aspect` |
| Display | V-Sync | On / Off | `vsync` |
| Graphics | Resolution scale | 25, 33, 50, 67, 75, 100, 125, 150, 175, 200 % | `render_scale` |

**Not menu rows:**
- The window size is the window's own: you drag it, and the size is remembered (`width`, `height`).
- MSAA, filtering, the FPS counter and exclusive fullscreen (`exclusive`, `fullscreen_width/height`) are display.cfg
  or command-line keys for tests.
- There are no presets, no typed sizes and no exact render size. The old keys (`render_width`, `render_height`,
  `manual_*`, `resolution_mode`, `auto_resolution`) are ignored with a one-line warning.
- The launcher passes no display flag.

### 6.2 The rule (`gekko/display_geometry.h display_geometry`)

- **picture**: the largest rectangle of the ratio inside the window or screen, centred, with black bars outside. Auto
  means the whole window or screen. A ratio within 3 % of the output's own shape counts as that shape, so there are no
  hairline bars: 21:9 on a 3440x1440 panel fills it.
- **render**: picture x resolution scale, lowered by one factor on both sides only at the 16384 texture limit
  (reported), and by the VRAM cap in `LiftedRender`.
- **one shape for the whole game.** The picture has this shape from the first frame to the last: menus, movies,
  cutscenes and gameplay alike.
  - 4:3 content keeps its own box inside the picture, as the game composes it.
  - The per-frame 4:3 framing of the first pass is removed.
  - The settings are read from display.cfg before the window opens and applied at game tick 0, before the first frame.

### 6.3 Windowed

- **Auto**: the picture is the whole window, whatever shape it is dragged to.
- **Picking a fixed ratio on the page** shapes the window once (`window_for_ratio`): the same width and the ratio's
  height, smaller when the desktop cannot hold it. Picking it again (step away and back) shapes it again.
- **Afterwards the window is free**: a drag, maximise or restore gives exactly that size, and the picture is
  letterboxed or pillarboxed inside it.
  - The size is saved when the drag ends; never during a display trial.
  - A restart, or leaving fullscreen, restores that size exactly.
  - A size the desktop cannot hold opens as the largest of its shape that fits, and that real size is remembered.
- **The smallest client** is 320x240.

### 6.4 Verified

Unit tests:

| Test | Result | Notes |
|---|---|---|
| `view_boundary_tests` | 1741 checks, 0 failed | The rule, the snap, free-window letterboxing, window shaping, the texture cap, scale x picture independence over 7 sizes x 3 ratios x 8 scales, the aspect parser and labels. |
| `options_display_tests` | all passed | |
| `settings_registry_tests` | all passed | Four rows; fullscreen trial and rollback; ratio pick shapes a 1280x720 window to 1280x400; a dragged 1000x1000 window is kept by other changes and re-shaped to 1000x313 by re-picking 32:10; Auto keeps the window; CLI overrides not saved; Alt+Enter commit / verify / refusal. |
| `options_settings_tests` | 13 failures | The same 13 as the pre-change baseline (audio rows). |
| `options_config_tests` | 5 failures | The same 5 as the pre-change baseline (audio rows). |
| `tools/test_resolution_presets.py`, `tools/test_launcher.py` | 7 passed | |

In game (lifted build, windowed 1600x900, 32:10, 200 %):

| Check | Result |
|---|---|
| Startup | One game output (1600x500) and one render size (3200x1000), both from game tick 0. |
| 33 window captures over 100 s of boot screens and the intro movie | All content inside the 1600x500 band; the movie stays in its 4:3 box inside it; no shape change. |
| Dragging the window to 1000x1000 | Kept; picture 1000x313, render 2000x626; display.cfg saved width=1000, height=1000. |
| Restart | The window opens at the largest square that fits the desktop (993x993), the picture is correct from tick 0, and that size is remembered. |

Not run:
- the Options page in the game;
- gameplay and an in-engine cutscene;
- Alt+Enter;
- fullscreen at 32:10;
- the 5K baseline.
