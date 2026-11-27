<p align="center">
    <img src="docs/assets/logo.png" alt="Nukkoro Recompiled" width="512"/>
</p>

---

A static recompilation of **Shadow the Hedgehog** (GameCube, PAL) for PC, made with GekkoForge and built from your own
copy of the game. It is still under development, so expect bugs and unfinished features.

## Screenshots

<p align="center">
    <img src="docs/assets/screenshots/1-sky-ruins.jpg" alt="Shadow flying above floating ruins in a blue sky" width="49%"/>
    <img src="docs/assets/screenshots/2-research-lab.jpg" alt="Shadow in a green-lit research lab" width="49%"/>
    <img src="docs/assets/screenshots/3-castle-courtyard.jpg" alt="Shadow running across a castle courtyard" width="49%"/>
    <img src="docs/assets/screenshots/4-purple-lake.jpg" alt="Shadow on a hover platform over a glowing purple lake" width="49%"/>
</p>

## How it works

When you install it, GekkoForge translates the game's executable from your disc into C++ and compiles it for Windows.
Parts of the game have been replaced with PC code, one part at a time, and the rest is still static recompiled code.

You provide the game's executable and files from your own legally dumped disc. The install contains code built from
your disc, so please keep it to yourself.

## What you need

- Windows 10 or 11, 64-bit, with a Direct3D 12 graphics card
- Shadow the Hedgehog for GameCube, the **PAL** release (game ID **GUPP8P**), extracted with [Dolphin](https://dolphin-emu.org/)
- Optional, for Japanese voices: the **Japanese** release (game ID **GUPJ8P**), extracted into a different folder (see
  [Japanese voices](#japanese-voices))
- [Visual Studio 2026](https://visualstudio.microsoft.com/downloads/) with the *Desktop development with C++* workload
- CMake 4.2 or newer (older versions do not know Visual Studio 2026). The Visual Studio workload above installs one (*C++ CMake tools for Windows*) and the setup uses it; a separate [CMake](https://cmake.org/download/) install works too
- [Python 3](https://www.python.org/downloads/)
- The Microsoft Edge WebView2 Runtime (included in Windows 11; on Windows 10 the setup offers to download and install it)

You do not have to install any of these tools yourself. When Visual Studio's build tools, CMake or Python is missing, the setup installs it as the first step of the install: it downloads the vendor's own installer from the vendor's address (Microsoft, python.org), checks that the file is signed by that vendor and runs it without questions. Windows asks once for permission; choose Yes. The Components page says beforehand what will be installed. From the console, `Install.cmd -DiscPath ... -InstallPath ... -GetTools` does the same.

## Install

1. In Dolphin, right-click the game and choose *Properties*. On the *Filesystem* tab, right-click the disc at the top and
   choose *Extract Entire Disc...*. [Extracting the disc](#extracting-the-disc) shows this with pictures.
2. Download `NukkoroRecomp-<version>-Setup.exe` from [Releases](https://github.com/ZenKazeX/NukkoroRecomp/releases) and run
   it. Windows may warn about an unknown publisher, because the file is unsigned. Choose *More info*, then *Run anyway*.
3. Choose the extracted disc folder, the install location and the shortcuts you want, then start the install. On the
   *Japanese Voices* page you can also choose an extracted Japanese disc, or leave it empty.

The first build takes about 5 to 20 minutes. Afterwards the extracted disc folder is no longer needed, and you can delete
the build files in `C:\Users\Public\NukkoroRecomp`. Running the setup again on the same folder updates the game and keeps
your saves. The game is installed as *Shadow the Hedgehog* in the Start menu, with *Settings* and *Mod Manager* beside
it; an update removes the shortcuts and programs of earlier versions, which had other names (*Shadow the Hedgehog
Recompiled*, `ShadowPadConfig.exe`, `ShadowModManager.exe`).

From a copy of this repository, run `Install.cmd` instead. To install from a console:

```powershell
.\Install.cmd -DiscPath "D:\Dumps\Shadow PAL" -InstallPath "D:\Games\Shadow the Hedgehog"
```

### Japanese voices

The PAL disc has the Japanese menu and subtitle text, but not the Japanese voices of the events (cutscenes) and dialogue.
Those are only on the Japanese disc. For the events to play in Japanese:

1. Extract your Japanese disc (GUPJ8P) with Dolphin the same way, into a **different folder** than the PAL disc.
2. Choose that folder on the setup's *Japanese Voices* page, and keep *Speak Japanese* ticked.

The setup checks the disc and copies only its voice files (about 470 MB) into the `voice_jp` folder of the install
folder; nothing else from the Japanese disc is used. To switch the spoken language later, set `voice` in `options.cfg`
(`0` Japanese, `1` English). To add the voices to an existing install, run the setup again with the Japanese disc
chosen; your saves and settings are kept. From a console:

```powershell
.\Install.cmd -DiscPath "D:\Dumps\Shadow PAL" -JapaneseDiscPath "D:\Dumps\Shadow JP" -InstallPath "D:\Games\Shadow the Hedgehog"
```

### Extracting the disc

Follow the red numbers in each picture. Right-click means the right mouse button, and every other click is the left one.

1. In Dolphin's game list, right-click the European version of the game, which has the blue flag with yellow stars. Then
   click *Properties*.

   <img src="docs/assets/install/dolphin-1-properties.png" alt="Dolphin's game list with the right-click menu open and Properties marked" width="720"/>

2. On the *Info* page, check that the *Name* line ends with `Revision 0` and the *Game ID* line says `GUPP8P`. The setup
   refuses any other version.

   <img src="docs/assets/install/dolphin-2-check-version.png" alt="Dolphin's Properties window with Revision 0 and Game ID GUPP8P marked" width="720"/>

3. Click *Filesystem*, right-click the *Disc* line at the very top of the list, then click *Extract Entire Disc...*.

   <img src="docs/assets/install/dolphin-3-extract.png" alt="Dolphin's Filesystem page with the Disc line and Extract Entire Disc marked" width="720"/>

4. Pick a drive, click *New folder*, type a name and press Enter. Double-click the new folder to go inside it, then click
   *Select Folder*. The picture hides the drives and folders, and you will see your own.

   <img src="docs/assets/install/dolphin-4-choose-folder.png" alt="The folder window with the drive area, New folder and Select Folder marked" width="720"/>

5. When the extraction has finished, check that the folder contains `sys` and `files`. This is the folder the setup asks
   for.

## Controller

Run `Settings.exe` in the install folder, or use the *Settings* shortcut in the Start menu, and open the
*Controller buttons* page. Click a button on the picture, press the button you want on your controller, then Save. The
game uses the new mapping the next time it starts. The [Settings tutorial](docs/SETTINGS_TUTORIAL.md#controller-buttons)
shows this with pictures, and the [Settings guide](docs/SETTINGS_GUIDE.md#controller-buttons) step by step.

By default the face buttons follow their position on a modern controller. The GameCube pad's B is on the left button
(X on an Xbox controller) and its X is on the right button (B).

The game can also be played with keyboard and mouse. The keys and the mouse are set on the Settings program's
*Keyboard and mouse* page (or in `controls.cfg` in the install folder;
[docs/controls/KEYBOARD_MOUSE_BINDINGS.md](docs/controls/KEYBOARD_MOUSE_BINDINGS.md) lists the defaults and every
setting).

## Settings

The Options screen is the game's own. Everything a PC adds - the window and full screen, resolution, picture shape,
frame rate and V-Sync, volumes, spoken language and subtitles, rumble, camera inversion, controller buttons, keyboard
and mouse, autosave - is in the *Settings* program (`Settings.exe` in the install folder, *Settings* in the
Start menu). The [Settings tutorial](docs/SETTINGS_TUTORIAL.md) walks through every page with pictures, and the
[Settings guide](docs/SETTINGS_GUIDE.md) explains every page and every setting. The game reads the
settings when it starts, so a change takes effect the next time you start it. The
program writes `display.cfg`, `options.cfg`, `pad.cfg` and `controls.cfg` in the install folder;
[docs/PC_SETTINGS_FILES.md](docs/PC_SETTINGS_FILES.md) describes every line. Alt+Enter switches between a window and
fullscreen.

The Settings program and the Mod Manager can be zoomed (up to 200 %) and themed (light, dark, or Shadow: black and red):
use the *Zoom* and *Theme* lists (under the list of pages in Settings, bottom right in the Mod Manager), or hold Ctrl and
press + or - to zoom. Both programs
share the choice. Zoom makes the text larger, never the window: the programs rearrange themselves to fit.

## Mods

The game loads mods from the `mods` folder in the install folder. Mods that replace the game's files (models, stages,
textures, text, sounds, movies) and Dolphin texture packs work. Mod support is experimental; the
[Mod Manager tutorial](docs/MOD_MANAGER_TUTORIAL.md) shows the steps with pictures, and the
[Mods guide](docs/MODS_GUIDE.md) explains every step and lists the known limitations.

1. Run `ModManager.exe` in the install folder, or use the *Mod Manager* shortcut in the Start menu.
2. Drop a downloaded mod (`.zip`, `.7z`, `.rar`, or a folder) on the window, or click *Install...*. The manager puts
   each file where the game expects it and says what it left out.
3. Tick the mods you want. The list's order is the priority: when two mods change the same file, the higher one wins,
   and the manager shows every such conflict.
4. Click *Play*, or start the game as usual.

Code patches (Gecko and Action Replay codes, patched `main.dol` files) are recognised and kept with the mod, but
installing a mod or switching it on does not apply them (the manager's *Code...* window is a developer feature that
needs the game's source tree). Programs and save files in a download are never installed. To play without mods, switch off *Load
mods* in the manager, or start the game with `--no-mods`. `ModManager.exe help` lists the command-line use, and
[docs/mods/DESIGN.md](docs/mods/DESIGN.md) describes the folder layout and the rules.

## Saves

Your settings and saves stay in the install folder. Existing GameCube saves from the PAL, North American and Japanese
releases can be imported with `--memory-card <file>` the first time you start the game.

## Uninstall

Open Windows Settings > Apps > Installed apps, choose *Shadow the Hedgehog* and *Uninstall*, or use *Uninstall Shadow
the Hedgehog* in the Start menu, or run `Uninstall.exe` in the install folder. The uninstaller looks like the setup and
goes in three steps: what will be removed, what happens to your data, and the uninstall itself. It removes only what
the setup installed: the game, Settings and the Mod Manager, the Japanese voices, the shortcuts and the Installed apps
entry. Your saves, settings, mods and snapshots are kept unless you tick them, each with its size shown, and files the
setup did not put in the folder always stay.

From a console, `Uninstall.cmd` in the install folder does the same without the window (`Uninstall.cmd -RemoveSaves`
deletes your saves, settings, mods and snapshots as well; `-RemoveData saves,mods` only those named).

## Known limitations

- The game is still partly static recompiled code, and some features are unfinished.
- Only the PAL release can be installed.
- Mods made for the North American release may show its stage layouts or fonts; the manager points these out.

## For developers

`Build.cmd` builds from the repository after each edit. The first run asks for the extracted disc folder.

```powershell
.\Build.cmd                             # build
.\Build.cmd -Run --boot-stage stg0100   # build, then start the game
```

See [docs/BUILDING.md](docs/BUILDING.md) for presets, tests and what is and is not in this repository, and
[docs/RELEASE_VALIDATION.md](docs/RELEASE_VALIDATION.md) for what was tested in this release.

## Legal

Unofficial fan project, unaffiliated with SEGA and Nintendo. Shadow the Hedgehog is a trademark of SEGA. You must own
the game to use this project.
