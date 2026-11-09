# The Options menu's Display tile (2026-10-28)

The user approved a PC "Display" tile for the Options menu, designed in `tools/options_menu_sim`. This is how it lives in
the game. Code: `runtime/include/gekko/native_options_display.h` (game thread), `runtime/src/gekko/options_display_overlay.*`
(the page's drawing, UI thread), `runtime/include/gekko/display_settings.h` (applying and saving).

## The original screen (main.dol, PAL GUPP8P)

- `AdvReal::Option::Phase`, vtable `0x805589E8`, phase + 4 = impl. setup `0x802B81E8` -> `0x802BA004` -> `0x802B9F2C` ->
  `0x802B833C(impl)` (CSD `csdFiles/Options`, casts); update `0x802B81C4` -> `0x802B8D88`.
- Six tiles, a 3 x 2 grid at `0x804FF7A8` (item = row * 2 + column). Item table `0x80558940`, stride 0x14:
  {motion, help message, "Null_box", value message ids, value count}. 0 Sound, 1 Vibe, 2 System, 3 Subtitle, 4 Load,
  5 Audio (voice language, `icn_audio`).
- The constructor `0x802BA13C` sets impl + 0x26 = 1: setup hides item 5's cast (`0x8036BBC0(cast, 1)`) and the grid
  never moves onto it, so the PAL screen shows five tiles. impl + 0x24 does the same for item 3 (0 on PAL).
- Cursor global `0x80585528` = {row, column}; + 0x08 + 4 * item = the page's first shown row, + 0x20 + 4 * item = the
  item's value, the copy taken when the page opens at + 0x38.. (`0x802B9330`). B restores it (`0x802B99F0`).
- impl + 0 = the screen state (`0x802B9FD0(impl, state, 1)`), impl + 0x1E = the page draws text, impl + 0x34 = frames
  to wait after a move (the page sets 2).
- The page handler `0x802B964C(impl)`: up and down through `0x802B810C(3 / 4, 4)` (a repeating press), A and B through
  `0x802B8168(1 / 2, 4)` (an edge). A on items 1 and 5 runs `0x802B8828` (any setting changed?) and `0x802B85D8` (save).
- The rows: `0x802B8BC4(impl, item, focused)` draws the text nodes text1.. (`0x805589B8`) by message id
  (`0x802BF228` -> `0x80049BE8`), selected colour through `0x802BF108`. Callers `0x802B8F7C`, `0x802B9088`.
- Menu sounds: `0x801D59AC(GetSoundManager(), 0x1E002 cursor / 0x1E003 decide / 0x1E004 back, 0, 0)`.
- The button queries: `0x802B810C` / `0x802B8168` (button, 4) pass the fade gate (`0x801178FC` -> `0x802E5D88`: the fade
  level at + 0x18 equals `K(-0x590)`), map the button (identity table `0x804FF6F8`) to three pad masks
  (`*(r13 + 0x59E4)`, `0x80560788` from `0x80357220`) and ask the input object (`0x8005185C`, r13 + 0x2D9C):
  - `0x8034A608(input, port, mask, 0xC, 1)` (repeat): port 4 = the active one (input + 0x11C, both ports when it is
    2 or more); true when the port's edge word (input + 0x30 + port * 0x28) holds the whole mask, its d-pad part
    (0x3C0) or its stick part (0x3C0000), or a held counter (s16 at input + port * 16 + slot * 2, masks `0x80512730`)
    is at least 13 frames;
  - `0x80357068(button, 4)` (edge): the same edge test on the merged word (input + 0xD0); for A it also notes the port
    that pressed it (r13 + 0x59E0).

## Native implementation

- `0x802B833C` (entry, declines): impl + 0x26 cleared, so the tile is shown and the grid reaches it.
- `0x802B964C` (entry, declines): notes that the page has the cursor this frame.
- `0x802B8BC4` for item 5: the game's voice-language rows are not drawn; the Display rows are published to the overlay.
- `0x802B810C` called from the page handler's "up" (return address `0x802B96D4`) for item 5: the Display rows' input
  (up / down move the cursor, left / right change the row, with the game's sounds and its 2-frame wait) read from the
  game's input state by the same rules; the call answers 0, as does "down" (`0x802B9754`).
- `0x802B8168` called from the page handler's "A" (`0x802B97F4`) for item 5: applies every changed row through
  `display_settings()` and saves display.cfg, decide sound; answers 0, so the voice language's save never runs and
  cursor + 0x34 is never written. B is the game's own: it restores the saved copy and closes the page (pending rows are
  dropped).
- Art: cells 46 (normal) and 52 (selected) of Options2 (512 x 512 RGB5A3) in every `csdFiles/Options_<lang>.txd` hold
  the TV tile (`tools/options_menu_sim/emit_display_tile_inc.py` -> `runtime/src/gekko/options_display_tile.inc`).
- Help line: message 0 of every `FONTS/ADVERTISE/Advertise_<lang>.fnt` (the voice-language help, shown only for item 5)
  reads "Change the screen and picture settings." in that language (texts from `tools/options_menu_sim/pc_strings.js`),
  refused at mount if a glyph is missing from the language's sheets.
- Drawing: the front-end's glyph sheets (`Advertise_<lang>.txd` + `.met`, luminance as alpha), on the menu's 640 x 480
  canvas kept 4:3 and centred, in the page's text slots (x 399, y 140 + 40 n): "label  value", the cursor row in the game's
  selected colour (table 0x80558AE8: colour 2 = FFFF00, rows colour 0 = white; times 1.0 with the cursor in the
  page, 0.5 without, as 0x802B8BC4 passes 0x802BF108) with "< value >". The original marks a selection only by that yellow; the fontnull "highlight"
  strip was tried as a bar and removed (user, 2026-10-28: overcooked).
- Language: the JP disc's own, else the console's (SRAM byte 0x16 of the IPL settings: EN, GE, FR, SP, IT).

## Rows

Display mode, Screen ratio, Resolution (the launcher's presets), Render resolution (a percentage of the resolution),
V-Sync, FPS counter.

**Current rules (2026-11-05, docs/RESOLUTION_AUDIT.md section 11).** The rows are four independent settings:
- Display mode: Windowed / Fullscreen (borderless) / Exclusive.
- Screen ratio: the picture's aspect, boxed with bars; it no longer filters or changes the resolution list.
- Resolution:
  - windowed: the presets plus the size in effect;
  - borderless: the desktop's size;
  - exclusive: the monitor's own modes.
- Render resolution: the 3D render scale, a percentage per side of the picture.

A row change never resets another row. The two paragraphs below are the history they replace.

Screen ratio (fixed 2026-10-30): Auto lists every resolution, and at 100 % the render size follows the window. A chosen
ratio lists that family's sizes and gives the picture an exact render size of that shape (resolution x percentage),
which the renderer boxes on a window or monitor of another shape (4:3 on a 16:9 monitor: bars at the sides, fullscreen
included). The page reads the ratio back from the exact render size. Before the fix the row only filtered the list and
wrote nothing: the page reopened on Auto, and fullscreen ignored it.

Fullscreen resolution and 100 % (fixed 2026-11-05):
- **What was wrong:** borderless fullscreen always shows the picture at the monitor's own size, but the page still
  offered the windowed sizes and took the percentage of them.
  - "2560 x 1440, 100%" on a 1920 x 1080 monitor rendered 2560 x 1440, shrunk into the screen: 133 % of what it shows.
  - With a 1280 x 960 window saved, 75 % rendered 960 x 720, cut to 960 x 540: half the screen's width.
- **Now:**
  - Fullscreen has one resolution: the monitor the window is on (the UI loop publishes it, `set_screen_size`).
    - Auto: the whole screen.
    - A ratio: the largest rectangle of it on the screen (4:3 on 1920 x 1080: 1440 x 1080, boxed).
  - 100 % renders that 1:1, and the percentages are of it. The windowed size is kept for the way back to windowed.
  - A size saved under the old meaning is listed as what it is of the screen (2560 x 1440 shows as 133%) and kept
    exactly until another percentage is chosen.
- **Evidence:**
  - options_display_tests: new fullscreen cases.
  - Real runs print the page's view on the "Options Display tile" report line:
    - windowed 1280 x 960: 1280 x 960, 100%;
    - fullscreen with 1280 x 960 saved: 1920 x 1080, 100%;
    - the old 2560 x 1440: 133%.
- **Windowed is unchanged:** 100 % is the asked window size. When it does not fit the work area the client is smaller,
  so Auto at 100 % renders the client (1920 x 1080 asked on this desktop: 1920 x 1061), while a chosen ratio's exact
  size can be a few percent larger than the client.

Draw distance (x1 Original .. x8 Maximum) is withheld from the first release (user 2026-10-30: early alpha). The model
keeps the row; display.cfg's `draw_distance` and `--draw-distance` still set it, and the default is now 1 (the original).

Not offered: Frame rate and Frame blending. The release present loop shows every game frame as it arrives and applies
neither (`display_settings.h`, the service's report of 2026-10-28), and a row must change what it names. They come back
when the present loop paces and blends.

## Deviation

The PAL screen has five tiles; this one has six. The voice-language page the sixth slot once held is not reachable on
PAL retail either (impl + 0x26), so nothing of the original is lost.
