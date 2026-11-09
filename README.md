# Shadow the Hedgehog: static recompilation for PC (work in progress)

This project builds Shadow the Hedgehog (GameCube, PAL) for Windows from your own disc. At build time a static
recompiler translates the game's executable into C++. That code runs on a runtime in which console mechanisms are
being replaced with PC implementations one part at a time. Much of the GameCube architecture is still emulated, so this
is not a native port yet; it is a work in progress.

**This repository contains no game code or data.** You need your own legally dumped **PAL** disc (game ID **GUPP8P**).
The installer builds the game from your disc, so do not share the installed game: it contains code built from that disc.

## Requirements

- Windows 10/11 x64 with a Direct3D 12 graphics card
- [Visual Studio 2026](https://visualstudio.microsoft.com/downloads/) with the *Desktop development with C++* workload
- [CMake](https://cmake.org/download/) 3.25 or newer, and [Python 3](https://www.python.org/downloads/)
- [Dolphin](https://dolphin-emu.org/), to extract your disc
- The Microsoft Edge WebView2 Runtime for the setup window (part of Windows 11; on Windows 10 the setup offers the download)

## Install

1. **Extract the disc with Dolphin.** In the game list, right-click Shadow the Hedgehog and choose *Properties*.
   On the *Filesystem* tab, right-click the disc (the top entry) and choose *Extract Entire Disc...*.
   Dolphin writes `sys/` and `files/` into the folder you pick.
2. **Run `Install.cmd`.** It builds the small setup program on first use (a few seconds), then opens the setup window.
3. **Welcome:** choose the extracted disc folder with **Browse...**. The setup checks that it is the unmodified PAL release.
4. **Install Location:** choose where to install with **Browse...**, and whether to start the game afterwards.
5. **Components:** choose the desktop and Start menu shortcuts. The page also checks that the build tools are installed.
6. **Install:** the game is built from your disc (the first build takes 10-30 minutes). The game and a copy of the disc's
   `files/` folder are then copied to the install folder.

To remove the game, run `Uninstall.cmd` in the install folder or use the Start menu's uninstall shortcut. It removes only
what setup installed and keeps your saves and settings; `Uninstall.cmd -RemoveSaves` deletes those too.

Settings, saves and the memory card are kept in the install folder. After installing, the extracted disc folder is no
longer needed. If you run the setup again on the same folder, it updates the game and keeps your saves.

The setup window's artwork, music and icon are in `installer/art/`; see [installer/ART_PACK.md](installer/ART_PACK.md).
The same icon is compiled into the game. If a file is missing, the window hides that layer, and without any art it uses
a plain dark theme.

To install from the console instead, run:

```powershell
.\Install.cmd -DiscPath "D:\Dumps\Shadow PAL" -InstallPath "D:\Games\Shadow the Hedgehog Recompiled"
```

## For developers

`Build.cmd` builds from the repository after each edit. The first run asks for the extracted disc folder and remembers
it; later runs only rebuild what changed.

```powershell
.\Build.cmd                                   # build
.\Build.cmd -Run --boot-stage stg0100         # build, then start the game (other options go to the game)
```

The game is `.build\release\bin\Release\ShadowTheHedgehog.exe`. See [docs/BUILDING.md](docs/BUILDING.md) for all options.

## More

- [docs/BUILDING.md](docs/BUILDING.md) covers building by hand, the CMake presets, and what is and is not in this repository.
- `installer/` holds the setup window (`src/main.cpp` hosts `web/` in WebView2), its PowerShell backend
  (`Backend.ps1`, `InstallCore.psm1`) and the console installer (`Install.ps1`).
