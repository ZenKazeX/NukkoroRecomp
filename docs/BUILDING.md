# Building by hand

## How the build uses your disc

The build reads your disc's `sys/main.dol` at configure time.

1. `tools/ppcrecomp/disc_map.py identify` checks the DOL's SHA-1 against `extract/discs.json`. Only the PAL release
   GUPP8P builds. NTSC-U (GUPE8P) and NTSC-J (GUPJ8P) have address maps in `extract/address-maps/`, but the runtime
   cannot use them yet.
2. `ppcrecomp` (built first) translates the DOL into C++ under `.build/release/generated/`. It is driven by the
   function table and site lists in `extract/`.
3. The executable embeds `sys/main.dol`, `sys/boot.bin` and `sys/bi2.bin`. At runtime it needs only the disc's `files/`
   tree, which it finds next to itself, in the working directory, or through `--game-data <folder>`.

Everything generated stays in `.build/`. The executable contains code and data from your disc: do not redistribute it.

## The quick way: Build.cmd

`Build.cmd` (script: `dev/Build.ps1`) wraps the commands below. It checks the tools, validates the disc, configures
only when needed, builds with 2 jobs, and can start the result.

| Command | Does |
| --- | --- |
| `Build.cmd` | Build. The first run asks for the disc folder and remembers it in `.build\dev-disc.txt` |
| `Build.cmd -Run [game options]` | Build, then start the game with `--game-data <disc folder>`; unknown options go to the game, e.g. `-Run --boot-stage stg0100 --fullscreen` |
| `Build.cmd -Disc <folder>` | Use and remember another disc folder (configures again) |
| `Build.cmd -Target <name>` | Build one target, e.g. `ppcrecomp` |
| `Build.cmd -Reconfigure` | Run CMake's configure step again |
| `Build.cmd -Clean` | Delete `.build\release` and build from scratch |
| `Build.cmd -Jobs <n>` | Parallel compile jobs (default 2) |

A game started with `-Run` keeps its settings, saves and memory card next to the built exe, apart from any installed
copy. The script refuses to build while that exe is running, because the linker cannot replace it.

## Commands

From the repository root (Visual Studio 2026, CMake 3.25+, Python 3):

```powershell
cmake --preset release -DSHADOW_RECOMP_DOL=D:/Dumps/Shadow-PAL/sys/main.dol
cmake --build --preset release -- /nodeReuse:false
```

The output is `.build/release/bin/Release/ShadowTheHedgehog.exe`. Copy it next to a `files/` folder, or run it with
`--game-data <extracted disc folder>`. `ShadowTheHedgehog.exe --help` lists all options.

Notes:

- Builds use 2 parallel jobs (`jobs` in the build preset, `SHADOW_RECOMP_COMPILE_JOBS`). The recompiled code is large;
  more jobs mostly add heat.
- If you leave out `SHADOW_RECOMP_DOL`, the preset uses `game/sys/main.dol` inside this repository. `game/` is git-ignored.
- `host-tools` builds `ppcrecomp` alone. `android-arm64` and `android-x86_64` cross-build the Vulkan backend. They
  expect the NDK at the paths in `CMakePresets.json`; see the comments in `CMakeLists.txt`.
- Builds never deploy anywhere: `SHADOW_DEPLOY_DIR` is empty.

## What is in this repository

| Folder | Contents |
| --- | --- |
| `runtime/` | Runtime: renderer, audio, input, storage, timing, and the subsystems rewritten so far |
| `tools/ppcrecomp/` | The static recompiler and the gates the build runs |
| `tools/` | Other build-time generators and checks, plus the stage launcher, comparator and diagnostic scripts |
| `extract/` | Function table, site lists and disc catalogue that drive the recompiler (addresses only, no code) |
| `thirdparty/` | plume (with D3D12MemoryAllocator and Vulkan headers), stb, Tracy; each keeps its own license |
| `dev/` | `Build.ps1`, the developer build behind `Build.cmd` |
| `installer/` | The setup window (Win32 + WebView2; its SDK is downloaded from NuGet at configure time), its art pack (`installer/art/`, also the game's icon; see `installer/ART_PACK.md`), its PowerShell backend, and the console installer |

Not included:

- **The regression test suite** (`runtime/tests`, `test_data/`). It needs memory images and translated code taken from
  the game, so the `tests` preset is removed and `SHADOW_BUILD_TESTS=ON` stops configuration with an explanation.
- **Disassembly, strings and symbol dumps from the DOL**. The build does not use them.
- **Research material and session notes.**
- **`tools/options_menu_sim/generated/`**: the Options menu simulator's textures and data, extracted from the disc.
  `python tools/options_menu_sim/build_sim_data.py` rebuilds them from your own copy.
