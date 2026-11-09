# Setup art pack

The setup window's character art, logo, backgrounds, music and icon are in `installer/art/`, with the layout below.
Shadow the Hedgehog and its characters are trademarks of Sega; the images are publicly available promotional artwork.
Without the pack the window still works: it shows a dark gradient theme with a text logo, and it has no music and no
custom icon.

To change the art, replace files in `installer/art/`, then run `Install.cmd`. The icon is compiled into the setup
program and into the game, so both need a rebuild. `Install.cmd` rebuilds the setup every time, and the install
rebuilds the game. A different icon file can be given with `-DSHADOW_GAME_ICON=<file.ico>`.

| File | Shown as |
| --- | --- |
| `art/shadow.png` | Shadow, right side |
| `art/eggman.png` | Eggman, sidebar (faded at the bottom) |
| `art/commander.png` | G.U.N. Commander, sidebar bottom |
| `art/dooms_eye.png` | Doom's Eye, top right |
| `branding/logo.png` | "Shadow the Hedgehog" logo (560 x 118 in the layout) |
| `branding/emblem.png` | Shadow emblem next to the logo |
| `branding/recompiled.png` | "RECOMPILED" wordmark (the source sheet; the page crops it) |
| `window/main_background.png` | Main background, stretched to the window |
| `window/sidebar_background.svg` | Sidebar background, stretched to the window height |
| `audio/music.mp3` | Looping music (starts at 25% volume; the window shows a toggle and slider) |
| `app.ico` | Icon of `NukkoroSetup.exe` and of the game, `ShadowTheHedgehog.exe` (file, window, taskbar, shortcuts). One file with 16, 20, 24, 32, 40, 48, 64, 96, 128 and 256 px images |

Any file may be missing; the page hides that layer. Positions come from the approved design (1265 x 648 window) and
are set in `installer/web/setup.css`.
