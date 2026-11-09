# Options menu simulator

A standalone, browser-based simulation of the Options screen (AdvReal::Option::Phase). It needs no game
runtime. It draws the real screen from the disc's layout, motions, textures, glyph sheets and strings.

## Run

```
python build_sim_data.py                       # once, or after changing the generator; reads game/files only
python make_display_icon.py                    # the Display tile, built from the game's own tiles
python -m http.server 8765 --directory .       # then open http://localhost:8765/
```

In the Claude desktop app the preview config `options-menu-sim` in `.claude/launch.json` does the second
step. The page must be served: browsers block the scripts when it is opened as a `file://` page.

Controls: arrows move, Enter/Z = A, Esc/X = B, C = Y (exit, as the GameCube build labels it). A standard
gamepad also works; the page only reads it and never sends rumble.

## Files

| File | What it is |
|---|---|
| `build_sim_data.py` | Reads `csdFiles/Options.gncp`, `Options_<lang>.txd` and `fonts/Advertise/*` and writes `generated/`. Reuses `../assets/gncp_read.py` and `../assets/txd_extract.py`. |
| `options.js` | **The menu content.** Categories (icons) and their rows. Edit this to add settings. |
| `sim.js` | A small CSD player (casts, cells, motions), the glyph text renderer and the menu logic. |
| `index.html` | The page: 640x480 canvas, language picker, "PC settings" toggle, current settings. `OPTIONS_SIM.step(n)` advances frames by hand. |

## Adding a setting

A setting that belongs to an existing category is a new row in that category in `options.js`:

```js
{ id: 'vsync', label: 'V-Sync', values: ['on', 'off'], value: 0 }
```

New subjects get their own category in `extraCategories`, like the example **Display** category
(Resolution, Screen ratio). An extra category borrows a grid slot the PAL screen leaves empty
(`cast`/`motion`: the voice-language slot, bottom right). Its tile is `art: { normal, selected }`, two
82 x 82 images in `generated/tex`; Display's are made by `make_display_icon.py` from the game's tiles (clean steel
taken from the six real tiles and rebuilt as a clean brushed plate, an old CRT television cut in the same
engraved style, grey and red). `art: 'placeholder'`
draws a labelled stand-in instead. Extra categories show when "PC settings" is ticked or the URL ends in `#extras`.

`values` and `label` may be keys of the game's message table (`on`, `off`, `stereo`, `lang_en`,
`track_611`... see `STRING_IDS` in `build_sim_data.py`) or literal text. A category with one row behaves like
the game: the panel lists the values and the current one is yellow. With two or more rows the panel lists
`label  value`, and left/right changes the value. The panel shows 7 rows and scrolls beyond that.

Display's rows are the display settings the port accepts today (display mode, screen ratio as a filter of the
resolution presets, resolution, render resolution in percent of the resolution, frame rate, V-Sync, frame blending, draw distance, FPS counter).
A row may carry `cli`, the game's own flag for its value; with PC settings on, the sidebar shows the resulting
game command line. `values` (and `value`) may be functions of the current settings, so a list can follow another
row (resolutions follow the ratio, render sizes follow the resolution).

The only free grid slot is the sixth one. A second extra category needs a layout decision first: a new
slot, or a page of the grid.

## What is from the disc and what is inferred

From the disc: every cast position, rotation, scale and pattern, all motions (MenuOpen/MenuClose, BGOpen/
BGClose, Open/Select/Close, the Loop fire and lines, the per-icon poses), the four layout textures per
language, the Advertise glyph sheets and metrics, and every string shown, in all six languages. The button
glyphs use pattern index 1 of each button cast, which is the GameCube set (index 0 = PS2, 2 = Xbox,
3 = PS2 alternate). So "exit" is the Y button, as authored.

Checked against the Dolphin reference `source/.build/validation/menu-ref/dolphin-options-sound-test.png`
(retail PAL): five tiles (the voice-language tile is hidden), header text position, the Sound test list
(I AM .. WESTOPOLIS, current entry yellow, no bar while the cursor is on the grid), the text origin
(3 px right of and 5 px above each text slot) and 7 visible rows.

Inferred (not checked against the running game):
- the panel flow: A opens the list, A picks, B backs out, and the red bar while the cursor is in the panel;
- the order of the subtitle language list;
- glyph spacing (ink width + 1 px on the 12 px Latin cells);
- Sound test and Load are stubs: they show a message only.

The background plate ends at y = 451 (the authored scale is 0.94), so the bottom strip stays black, as in
the layout data.
