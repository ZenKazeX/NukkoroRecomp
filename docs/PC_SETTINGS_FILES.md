# PC settings files

Shadow the Hedgehog on PC is a static recompilation of the PAL GameCube game made with GekkoForge. Its Options
screen is the game's own (retail): Sound (the music player), Vibration, System (speaker output: Stereo, Monaural,
Dolby Pro Logic II), Subtitle (the text language) and Load. Everything a PC adds on top - the window, the picture,
the volumes, the controller mapping, the keyboard - is set in four plain-text files.

**The Settings program** (`Settings.exe` beside the game, *Settings* in the Start menu) sets them for players
who would rather not edit files: pages for Display, Frame rate, Sound, Language, Controller buttons, Controller
options, Keyboard and mouse, and Saving. It shows what the files hold, writes only the keys the player changes (every
other line, comment and line ending stays, as when the game writes), creates a file only when something in it is
saved, refuses values the game would not accept, and has a Restore defaults button per page. It is laid out like
Windows 11's own Settings: the pages in a list on the left, each setting on a card with its explanation, switches for
on/off settings, and Windows' accent colour on the Save button. It reads the files again
before it writes, so a key the game wrote meanwhile (a window resized, Alt+Enter) is kept. `--folder <dir>` makes it
edit the files of another folder. Which control writes which key is listed in the last section of this page.

| File | Where | Read | Written by the game |
|---|---|---|---|
| `display.cfg` | beside the executable (`--display-cfg <file>` picks another) | at start | Alt+Enter and a window you resize: only the keys they change |
| `options.cfg` | beside `display.cfg` | at start | the Options screen's Vibration / System / Subtitle choices (the keys below marked "game option") and the first start's import from your save |
| `pad.cfg` | beside the executable (`--pad-cfg <file>` picks another) | at start | never (the Settings program writes it) |
| `controls.cfg` | beside the executable (`--controls-cfg <file>` picks another) | at start, and again when the game window comes back to the front | never (the Settings program writes it) |

All four are `key = value` lines; `#` starts a comment. Edit them while the game is closed: a value changed in a
file takes effect at the next start. When the game writes a file it changes only the lines of the keys it sets and
keeps every other line, comment and line ending as it was. Remove a key (or the whole file) to go back to its
default. The command line's display flags (`--width`, `--fullscreen`, `--msaa`, `--vsync` / `--no-vsync`,
`--render-rate`, `--frame-interpolation` / `--no-frame-interpolation`, `--draw-distance`, `--fps-counter`, ...) override
`display.cfg` for one run and are never saved.

## display.cfg

An unknown key or a bad value stops the game at start with the line number (`display.cfg line N: ...`).

| Key | Values | Default | What it does |
|---|---|---|---|
| `fullscreen` | `true` / `false` | `false` | Fullscreen (borderless at the desktop's size). Alt+Enter toggles it and saves it. |
| `exclusive` | `true` / `false` | `false` | With `fullscreen=true`: the monitor's own display mode `fullscreen_width` x `fullscreen_height` instead of borderless. Alt+Enter into an exclusive mode keeps it only when the monitor accepts the mode; otherwise (or with no answer within 15 s) the window goes back as it was and nothing is saved. |
| `fullscreen_width`, `fullscreen_height` | pixels, at least 160 x 120; `0` = the desktop's mode | `0` | The exclusive mode's size. |
| `width`, `height` (also `window_width`, `window_height`) | pixels, at least 160 x 120 | `1280`, `720` | The window's client size. Resizing the window saves the new size. |
| `aspect` | `auto`, `W:H` (`4:3`, `16:9`, `16:10`, `21:9`, `32:10`, `32:9`, ...) or a number such as `2.37` | `auto` | The picture's shape. `auto` = the window's or screen's own shape; a fixed ratio is letterboxed inside it. |
| `aspect_ratio` (also `custom_aspect`) | `W:H` or a number, 0.25 .. 8 | none | An older spelling of a fixed `aspect`. |
| `render_scale` | percent, 25 .. 200 | `100` | The 3D resolution per side of the picture (50 = half the pixels per side, 200 = supersampled). A value outside the range is clamped with a warning. |
| `msaa` | `off`, `2`, `4`, `8`, `16`, `32` (or `4x`) | `4` | Multisample anti-aliasing of the 3D scene. A count the GPU lacks is reported and the renderer keeps one it can do. |
| `anisotropy` | `off`, `2`, `4`, `8`, `16` | `16` | Anisotropic filtering of the game's filtered textures (`off` = the console's linear filter). |
| `vsync` | `true` / `false` | `true` | Waits for the monitor's refresh. |
| `render_rate` | `unlocked` or a rate in (0, 1000] Hz | `60` | Presented frames per second. The game itself always runs at its own rate. |
| `frame_interpolation` | `true` / `false` | `false` | Blends frames when presenting above the game's rate. |
| `draw_distance` | 1 .. 8 | `1` | Object draw distance; 1 = the original game. |
| `fps_counter` | `true` / `false` | `false` | Presented FPS and frame times, top right (Num * toggles it in the window). |
| `gameplay_aspect` | `follow`, `original` (4:3), `W:H` or a number | `follow` | The aspect the game logic's on-screen tests use; `follow` = the picture's. |
| `hud_scale`, `hud_inset` (also `hud_safe_zone`) | number > 0.05; pixels >= 0 | `1.0`, `0` | Read and accepted; they change nothing in this renderer. |
| `render_width`, `render_height`, `manual_width`, `manual_height`, `resolution_mode`, `auto_resolution` | - | - | Retired: accepted with a warning and ignored (the picture follows the window or screen and `render_scale`). |

## options.cfg

A bad value is reported (`[options] options.cfg:N: bad value for <key> (default used)`) and that key takes its
default; an unknown key is reported and kept in the file. The game starts either way.

| Key | Values | Default | What it does |
|---|---|---|---|
| `master_volume` | 0 .. 100 | `100` | The whole output's volume, after the game's own mix. |
| `music_volume` | 0 .. 100 | `100` | The music's level, below the master. |
| `sfx_volume` | 0 .. 100 | `100` | The sound effects' level, below the master. |
| `voice_volume` | 0 .. 100 | `100` | The voices' level (dialogue, cutscenes, the characters' in-stage lines), below the master. |
| `subtitles` | `true` / `false` | `true` | Shows the subtitles of spoken lines. `false` hides only those texts; the lines still play. (The game's Subtitle item picks the text language; it has no on / off.) |
| `rumble_strength` | 25 .. 100 | `100` | How hard the controller rumbles, in percent of the original strength. Rumble on / off is the game's Vibration item (`vibration` below). |
| `invert_camera_x`, `invert_camera_y` | `true` / `false` | `false` | Inverts the camera stick's horizontal / vertical axis. |
| `language` | `0` Japanese, `1` English, `2` French, `3` Spanish, `4` German, `5` Italian | not set | Game option: the text language (the Options screen's Subtitle item). Applied from the first screen on, before the save loads. `6` (the Hungarian text of earlier builds) is read as English with a warning. |
| `speaker` | `0` Stereo, `1` Monaural, `2` Dolby Pro Logic II | not set | Game option: the Options screen's System item. |
| `vibration` | `true` / `false` | not set | Game option: the Options screen's Vibration item. |
| `voice` | `0` Japanese, `1` English | not set | Game option: the spoken language. The PAL game hides its voice item, so this file is the only place to set it; Japanese needs the Japanese disc's voice files (`PRS_VOICE_J.afs` and the `sng_*_J.adx` files) in a `voice_jp` folder of the game root; without them English plays. |
| `autosave` | `true` / `false` | not set | Game option: the game's autosave. `false` keeps the game from saving after stages and from Options. |

"Not set" means the game's own value: the first time a save loads, the game options it holds are taken from it and
written here (a key you already set wins), so the file holds what you play with. From then on this file is where
they live: they are applied over the save at every start and every Load, and a change on the Options screen is
written back here at once. Leaving the Options screen does not rewrite the save file; your next progress save carries
the choices too.

## pad.cfg

The gamepad's buttons. Values are XInput button masks: `0x0001` D-pad up, `0x0002` down, `0x0004` left, `0x0008`
right, `0x0010` Start, `0x0020` Back, `0x0040` / `0x0080` left / right stick press, `0x0100` / `0x0200` left / right
shoulder, `0x1000` A (south), `0x2000` B (east), `0x4000` X (west), `0x8000` Y (north); `0` = unbound. A value that is
not wholly a number, out of range, or an unknown key stops the game at start with the line number.

| Key | Values | Default | What it does |
|---|---|---|---|
| `a`, `b`, `x`, `y`, `start` | mask | A, B, X, Y, Start | The game's buttons of the same names. |
| `l`, `z` | mask | left shoulder, right shoulder | L and Z. |
| `r` | mask | `0` (the right trigger raises R) | R. |
| `dpad_up`, `dpad_down`, `dpad_left`, `dpad_right` | mask | the D-pad | The game's D-pad. |
| `trigger_threshold` | 0 .. 255 | `40` | Analog trigger travel beyond which a trigger also holds L / R. |
| `swap_sticks` | `0` / `1` | `0` | Movement on the right stick, camera on the left. |
| `button_style` | `auto`, `original`, `xbox`, `playstation`, `switch` | `auto` | Which controller's buttons the game's prompts show (`auto` follows the connected controller). `--button-style` overrides it. |

When the Settings program changes a button whose line ends in the button's name as a comment (`a = 0x1000   # A`, as
it writes a new file), the comment names the new button.

## controls.cfg

The keyboard and mouse: every action, the mouse speed and inversion, the pointer capture and the walk speed.
[controls/KEYBOARD_MOUSE_BINDINGS.md](controls/KEYBOARD_MOUSE_BINDINGS.md) lists every key and the defaults.

## What the removed PC Options menu could do that the files cannot

The PC builds before 2026-11-14 replaced the game's Options screen with a six-category menu (Display, Graphics,
Audio, Controls, Save Data, System). The game's own screen is back; every value that menu set is a key above. What is
gone:

- **Changing a value while the game runs.** The menu applied volumes, rumble, camera inversion, subtitles, the voice
  language and the picture settings at once. The files are read at start, so such a change now needs a restart.
  Still live in the game: Alt+Enter (fullscreen), resizing the window, and the game's own Vibration, System and
  Subtitle items.
- **The 15-second confirmation of a display change.** It existed because the menu could pick a mode the monitor might
  not show. A file edit is not tried out; if a `display.cfg` makes the screen unusable, close the game and fix or
  delete the file. Alt+Enter into an exclusive mode is still checked and rolled back as described above.
- **Restore defaults / Restore all defaults.** The Settings program has Restore defaults on each page; in the files,
  remove the keys (or the files) instead.
- **The build information row.** The session report and the console output carry the build.
- **Hungarian text (language 6).** Withdrawn with the menu (the user's decision of 2026-11-14); `language=6` reads
  as English.

## What the Settings program writes

| Page | Control | File | Key | Values written |
|---|---|---|---|---|
| Display | Screen mode: Window / Full screen / Exclusive full screen | `display.cfg` | `fullscreen`, `exclusive` | `false` / `true`+`false` / `true`+`true` |
| Display | Window size (a list, or typed as `W x H`, at least 320 x 240) | `display.cfg` | `width`, `height` (also `window_width`, `window_height` where the file spells them so) | pixels |
| Display | Full screen resolution (exclusive only; the monitor's own modes) | `display.cfg` | `fullscreen_width`, `fullscreen_height` | pixels; `0` = the desktop's |
| Display | Picture shape | `display.cfg` | `aspect` (an older `aspect_ratio` / `custom_aspect` line is replaced) | `auto`, `4:3`, `5:4`, `16:10`, `16:9`, `21:9`, `32:10`, `32:9` |
| Display | Resolution scale | `display.cfg` | `render_scale` | 25 .. 200 |
| Display | Edge smoothing (anti-aliasing) | `display.cfg` | `msaa` | `off`, 2, 4, 8, 16, 32 |
| Display | Texture sharpness (anisotropic filtering) | `display.cfg` | `anisotropy` | `off`, 2, 4, 8, 16 |
| Display | Draw distance | `display.cfg` | `draw_distance` | 1 .. 8 |
| Frame rate | V-Sync | `display.cfg` | `vsync` | `true` / `false` |
| Frame rate | Frame rate | `display.cfg` | `render_rate` | 30, 60, 75, 90, 120, 144, 165, 240, 360, `unlocked` |
| Frame rate | Smooth motion (frame interpolation) | `display.cfg` | `frame_interpolation` | `true` / `false` |
| Frame rate | Frame counter | `display.cfg` | `fps_counter` | `true` / `false` |
| Sound | Master volume, Music, Sound effects, Voices | `options.cfg` | `master_volume`, `music_volume`, `sfx_volume`, `voice_volume` | 0 .. 100 |
| Sound | Speaker output | `options.cfg` | `speaker` | 0, 1, 2; "Not chosen yet" removes the line |
| Language | Spoken language | `options.cfg` | `voice` | 0 Japanese, 1 English; "Not chosen yet" removes the line |
| Language | Subtitles | `options.cfg` | `subtitles` | `true` / `false` |
| Language | Text language | `options.cfg` | `language` | 0 .. 5; "Not chosen yet" removes the line |
| Controller buttons | the twelve game buttons, trigger press, button prompts, stick swap | `pad.cfg` | every key of pad.cfg | as above |
| Controller options | Vibration | `options.cfg` | `vibration` | `true` / `false`; "Not chosen yet" removes the line |
| Controller options | Rumble strength | `options.cfg` | `rumble_strength` | 25 .. 100 |
| Controller options | Invert camera left and right / up and down | `options.cfg` | `invert_camera_x`, `invert_camera_y` | `true` / `false` |
| Keyboard and mouse | the 22 actions, three keys or mouse buttons each | `controls.cfg` | the action keys | binds, or `none` |
| Keyboard and mouse | Mouse speed, left and right / up and down | `controls.cfg` | `mouse_sensitivity_x`, `mouse_sensitivity_y` | 0.01 .. 50 |
| Keyboard and mouse | Invert mouse, Keep the pointer in the game, Walk speed | `controls.cfg` | `mouse_invert_x`, `mouse_invert_y`, `mouse_capture`, `walk_deflection` | 0 / 1; 1 .. 100 |
| Saving | Autosave | `options.cfg` | `autosave` | `true` / `false`; "Not chosen yet" removes the line |

Not offered, on purpose: `gameplay_aspect` (a test setting for the game logic, not a picture setting), `hud_scale` /
`hud_inset` (accepted but they change nothing in this renderer) and the retired render-size keys. A value the file
holds that is not in a list (a frame rate of 100, a ratio of 2.37) is shown as "(set in the file)" and kept unless
the player picks another.

## appearance.cfg: the programs' zoom and theme

The Settings program and the Mod Manager share one more file beside them, `appearance.cfg`. It holds how the two
programs look; the game does not read it. Both programs have two lists for it (the Settings program under its list
of pages, the Mod Manager at the right end of its bottom bar):

| Control | Key | Values |
|---|---|---|
| Zoom: 100 % .. 200 %, everything larger (also Ctrl and + / -, Ctrl and 0 for 100 %, Ctrl and the mouse wheel) | `zoom` | 100, 125, 150, 175, 200 |
| Theme: same as Windows / light / dark / Shadow | `theme` | `system` (Windows' app mode: Settings > Personalisation > Colours), `light`, `dark`, `shadow` (near black with Shadow's red instead of Windows' accent colour) |

A change is used at once and saved at once (no Save needed); the other program uses it the next time it opens. The
file is created only when one of the two is changed, and like the other files only the changed line is rewritten.
With Windows' high contrast mode on, both programs use the high contrast colours whatever the file says.

Zoom never changes a window's size: both programs lay everything out again for the room the window has (and again
when the window is resized). Words wrap instead of being cut off, and nothing scrolls sideways. In the Settings
program a setting's control moves under its name when the page is narrow, the navigation list shows only its icons,
the keyboard page's two cards go one under the other, and the page scrolls up and down. The Controller buttons page
has a scale of its own: its list and the controller pictures stay side by side and grow with the zoom or shrink to
fit the window together, like a picture (in a window too narrow for that, the pictures go under the list at the
zoom's size). In the Mod Manager the buttons wrap into more rows and show only their icons (their names as tooltips)
when their words would take more than two rows, the selected mod's card goes under the list in a narrow window and
scrolls when it is taller than its room, and the bottom bar wraps. Both programs check this with `--check-layout
<report.txt>` (Settings also takes `--folder <dir>`): every zoom in the start-up, smallest and a wide window size, each
control whose text does not fit and any zoom that changed the window's size reported, exit code 1 when there is one.
The uninstaller keeps the file unless `-RemoveSaves` is given.
