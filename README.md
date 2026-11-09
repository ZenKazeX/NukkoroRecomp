# Nukkoro Recompiled

An unofficial PC version of **Shadow the Hedgehog** (GameCube, PAL), built from your own copy of the game.

> **This is not a native port.** It is a static recompilation made with GekkoForge, and it is still under development.
> Expect bugs and unfinished features.

## How it works

When you install it, GekkoForge translates the game's executable from your disc into C++ and compiles it for Windows.
The result runs on a runtime that replaces the console's systems with PC ones, one part at a time. Much of the GameCube
architecture is still emulated, which is why this is not a native port.

The repository does not contain the game's executable or disc files. You provide those from your own legally dumped
disc. The install contains code built from your disc, so please do not share it.

## What you need

- Windows 10 or 11, 64-bit, with a Direct3D 12 graphics card
- Shadow the Hedgehog for GameCube, the **PAL** release (game ID **GUPP8P**), extracted with [Dolphin](https://dolphin-emu.org/)
- [Visual Studio 2026](https://visualstudio.microsoft.com/downloads/) with the *Desktop development with C++* workload
- [CMake](https://cmake.org/download/) 3.25 or newer
- [Python 3](https://www.python.org/downloads/)
- The Microsoft Edge WebView2 Runtime (included in Windows 11; on Windows 10 the setup offers the download)

## Install

1. In Dolphin, right-click the game and choose *Properties*. On the *Filesystem* tab, right-click the disc at the top and
   choose *Extract Entire Disc...*.
2. Run `Install.cmd`.
3. Choose the extracted disc folder, the install location and the shortcuts you want, then start the install.

The first build takes about 10 to 30 minutes. Afterwards the extracted disc folder is no longer needed. Running the setup
again on the same folder updates the game and keeps your saves.

To install from a console instead:

```powershell
.\Install.cmd -DiscPath "D:\Dumps\Shadow PAL" -InstallPath "D:\Games\Shadow the Hedgehog Recompiled"
```

## Controller

Run `ShadowPadConfig.exe` in the install folder, or use the *Controller Buttons* shortcut in the Start menu. Click a
button on the picture, press the button you want on your controller, then Save. The game uses the new mapping the next
time it starts.

By default the face buttons follow their position on a modern controller. The GameCube pad's B is on the left button
(X on an Xbox controller) and its X is on the right button (B). Keyboard keys cannot be changed yet.

## Saves

Your settings and saves stay in the install folder. Existing GameCube saves from the PAL, North American and Japanese
releases can be imported with `--memory-card <file>` the first time you start the game.

## Uninstall

Run `Uninstall.cmd` in the install folder, or use the uninstall shortcut in the Start menu. It removes only what the
setup installed and keeps your saves and settings. `Uninstall.cmd -RemoveSaves` deletes those as well.

## Known limitations

- The game is still partly emulated, and some features are unfinished.
- Only the PAL release can be installed.
- The Hungarian language is switched off for now.

## For developers

`Build.cmd` builds from the repository after each edit. The first run asks for the extracted disc folder.

```powershell
.\Build.cmd                             # build
.\Build.cmd -Run --boot-stage stg0100   # build, then start the game
```

See [docs/BUILDING.md](docs/BUILDING.md) for presets, tests and what is and is not in this repository, and
[docs/RELEASE_VALIDATION.md](docs/RELEASE_VALIDATION.md) for what was tested in this release.

## Legal

This is an unofficial fan project and is not affiliated with or endorsed by SEGA or Nintendo. Shadow the Hedgehog is a
trademark of SEGA. You must own the game to use this project.
