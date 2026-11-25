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

From the repository root (Visual Studio 2026, a CMake that knows it: 4.2 or newer, Python 3):

```powershell
cmake --preset release -DSHADOW_RECOMP_DOL=D:/Dumps/Shadow-PAL/sys/main.dol
cmake --build --preset release -- /nodeReuse:false
```

The output is `.build/release/bin/Release/ShadowTheHedgehog.exe`. Copy it next to a `files/` folder, or run it with
`--game-data <extracted disc folder>`. `ShadowTheHedgehog.exe --help` lists all options.

Notes:

- Developer builds use 2 parallel jobs (`jobs` in the build preset, `SHADOW_RECOMP_COMPILE_JOBS`). The recompiled code
  is large; more jobs mostly add heat. The setup program picks up to 12 from the PC's processors and free memory.
- If you leave out `SHADOW_RECOMP_DOL`, the preset uses `game/sys/main.dol` inside this repository. `game/` is git-ignored.
- `host-tools` builds `ppcrecomp` alone. `android-arm64` and `android-x86_64` cross-build the Vulkan backend. They
  expect the NDK at the paths in `CMakePresets.json`; see the comments in `CMakeLists.txt`.
- Builds never deploy anywhere: `SHADOW_DEPLOY_DIR` is empty.

## Prebuilt parts

A release package can carry, in `prebuilt/`, the parts of the build that contain no game code, compiled once by the
release step. The setup program passes `-DSHADOW_PREBUILT_DIR=<package>/prebuilt`, and the build then compiles only the
code that comes from your disc. Your PC still builds, from your own `main.dol`:

- `ppcrecomp`'s output: every recompiled shard, the function table and everything else under `.build/release/generated/`;
- `shadow_recomp.lib`, compiled from that output;
- `ShadowTheHedgehog.exe`, which links all of it and embeds your `sys/main.dol`, `sys/boot.bin` and `sys/bi2.bin`.

What is prebuilt, all from this repository's own sources (`CMakeLists.txt` names them in `SHADOW_PREBUILT_LIBRARIES`
and `SHADOW_PREBUILT_PROGRAMS`, the only list):

| Part | Built from |
| --- | --- |
| `shadow_runtime` (with `shadow_runtime_common`), `shadow_render`, `shadow_render_lifted`, `shadow_rwim`, `shadow_d3d12ma`, `shadow_lift`, `shadow_assets`, `shadow_mods`, `shadow_motion`, `shadow_native_execution`, `shadow_native_memory`, `shadow_native_gameplay` | `runtime/`, `thirdparty/plume/contrib/D3D12MemoryAllocator`; `shadow_render` includes the renderer's shaders, compiled by DXC from `runtime/lift/render/shaders`; `shadow_mods` is the mod loader (`runtime/mods`) |
| `ppcrecomp.exe` | `tools/ppcrecomp/main.cpp` (the recompiler itself; it reads your DOL only when it runs on your PC) |
| `ShadowPadConfig.exe` | `tools/padconfig/`, `runtime/lift/peripheral/gamepad_binding.cpp` |
| `ShadowModManager.exe` | `tools/mod_manager/mod_manager.cpp`, `runtime/mods` |

Some sources of this repository carry data taken from the game, and the scan finds their objects: they are never
prebuilt. `SHADOW_PREBUILT_LOCAL_SOURCES` lists them (`native_audio_mixer.cpp`: the AX compressor's gain table;
`button_glyphs.cpp`: palettes and texel blocks matching the game's button sheets; `native_boot.cpp` and `os_exception_entry.cpp`: the 38
PowerPC words of the SDK's exception vector, `gekko/os_exception_vector.h`; `runtime/mods/afs.cpp`: the names of the
memory-card banner and icon files it checks, which the compiler lays out in the order of the disc's file table). The release step leaves their objects out of the
archives and your build compiles them into the same targets. `shadow_msl` (fdlibm, whose constant tables are the
game's C library's byte for byte) is not prebuilt at all.

**How they are made.** `dev/Package-Release.ps1` unpacks the source package it is about to publish, configures it with
the release preset and an empty `SHADOW_RECOMP_DOL` plus `SHADOW_PREBUILT_EXPORT=ON` (CMake refuses the export when a
DOL is given, so that tree has no game input at all), builds exactly the listed targets and writes
`prebuilt/<set>/prebuilt.json`: the key below, every file's size and SHA-256, and the toolchain's versions. It then runs
`dev/scan_game_code.py` over every prebuilt file, and over the mod manager's `mods-data/` (installed as it is), against
the PAL disc: every 32-byte window of `sys/` and of the whole `files/` tree, at every offset, is looked up among the
prebuilt files, taken as they are and byte-swapped in 16- and 32-bit units (the console is big-endian). Counting
and identity tables, and runs that occur as they are in the C runtime libraries the toolchain links into every
program, are listed apart. Any other shared run of 64 bytes or more,
256 shared bytes in one file, a defined translated function (`fn_8xxxxxxx`) in a symbol table or a path of the
release machine's user stops the release. The prebuilt parts are compiled in the folder the setup unpacks the
package into (`%PUBLIC%\NukkoroRecomp\<version>`), because MSVC derives the names of anonymous namespaces (which also
appear in RTTI names) from each source file's full path.

**When they are used.** At configure time CMake computes the key of the player's tree, one line each: the compiler
and its exact version, the platform toolset, the Windows SDK, the generator and platform, `CMAKE_CXX_FLAGS`,
`CMAKE_CXX_FLAGS_RELEASE`, `CMAKE_RC_FLAGS`, the build options that change compiled code (`SHADOW_X64_ARCH`,
`SHADOW_SHIPPING`, `SHADOW_RENDER_LIFTED`, `SHADOW_ENABLE_PLUME`, `SHADOW_ENABLE_TRACY`, `SHADOW_WATCH_SYMBOLS`,
`SHADOW_BUILD_TESTS`, `SHADOW_BUILD_REFERENCE`) and a SHA-256 over every file of `runtime/`, `rwcore/`, `rpworld/`,
`tools/`, `thirdparty/`, `CMakeLists.txt` and `CMakePresets.json`. A prebuilt set is used only when every line is
equal and every file has its recorded SHA-256; the configure output says which set, or why none fits. Otherwise the
tree builds everything, exactly as without `prebuilt/`. An exact compiler match is required, not only a compatible
one: MSVC can link objects of an older compiler, but then the game would contain code that compiler generated rather
than the code the player's compiler generates from the same source, and the project keeps the game's behaviour
bit-exact. With an exact match the prebuilt objects are the ones the player's compiler would produce (the release
validation compares them object by object).

**How they are linked.** Each prebuilt library keeps its CMake target and therefore its usage requirements (include
folders, definitions, link order). The target compiles one empty file and its archive step merges the prebuilt archive
into it (`lib.exe` accepts a `.lib` as input; `STATIC_LIBRARY_OPTIONS`), so the executable's link line is the same,
item for item, as a full build's. `ppcrecomp` becomes an imported executable, as in cross builds, and
`ShadowPadConfig` and `ShadowModManager` copies of the prebuilt programs (the manager's `mods-data/` is copied beside
it from `test_data/mods` either way). The renderer's shaders are not compiled, since only
`shadow_render` uses them. The build-time string the runtime shows (`__DATE__ __TIME__` in `system.cpp`) is then the
release step's. `prebuilt/` may hold several sets, one per toolchain (run the release step with each); CMake takes the
first whose key matches.

To add a part (it must take no input from the disc or from `extract/`): build it as a target, add its name to
`SHADOW_PREBUILT_LIBRARIES` (a static library) or `SHADOW_PREBUILT_PROGRAMS` (a program the game does not link; give
it the substitution `ShadowPadConfig` has), and, if the install should copy it, to `$script:InstalledPrograms` in
`installer/InstallCore.psm1`. Folders copied into the install as they are go in `$script:InstalledFolders` there.

## What is in this repository

| Folder | Contents |
| --- | --- |
| `runtime/` | Runtime: renderer, audio, input, storage, timing, and the subsystems rewritten so far |
| `tools/ppcrecomp/` | The static recompiler and the gates the build runs |
| `tools/` | Other build-time generators and checks, plus the stage launcher, comparator and diagnostic scripts |
| `extract/` | Function table, site lists and disc catalogue that drive the recompiler (addresses only, no code) |
| `thirdparty/` | plume (with D3D12MemoryAllocator and Vulkan headers), stb, Tracy; each keeps its own license |
| `dev/` | `Build.ps1`, the developer build behind `Build.cmd`; `Package-Release.ps1`, the release package with its prebuilt parts; `scan_game_code.py`, `compare_libs.py` and `time_install.py`, the checks and timing the release validation runs |
| `installer/` | The setup window (Win32 + WebView2; its SDK is downloaded from NuGet at configure time), its art pack (`installer/art/`, also the game's icon; see `installer/ART_PACK.md`), its PowerShell backend, and the console installer |

Not included:

- **The regression test suite** (`runtime/tests`, `test_data/`). It needs memory images and translated code taken from
  the game, so the `tests` preset is removed and `SHADOW_BUILD_TESTS=ON` stops configuration with an explanation.
- **Disassembly, strings and symbol dumps from the DOL**. The build does not use them.
- **Research material and session notes.**
- **Tables that quote the game's texts**: the save messages behind `tools/save_text` (`save_originals.json`,
  `pc_save_<lang>.json`; the generated `runtime/src/gekko/pc_save_texts.inc` is here), the mod manager's full region map
  (`test_data/mods/region_diff.json` keeps only the fields the manager reads; `tools/mods/map_region_diff.py` makes the
  rest from both discs), and the voice-line map (`tools/mods/map_voice_lines.py`).

## Regression tests

Two presets build the tests. Both need the PAL `main.dol`, and the test inputs that cannot be published (RAM images of
the game) come from the folder named by `SHADOW_PRIVATE_TEST_DATA`. Tests that need those inputs are not usable without them.

```
cmake --preset tests -DSHADOW_RECOMP_DOL=<disc>/sys/main.dol -DSHADOW_PRIVATE_TEST_DATA=<folder>
cmake --build .build/tests --config Release --target ALL_BUILD ShadowTheHedgehogReference
ctest --test-dir .build/tests -C Release -j 4
```

`shipping-tests` runs the same suite against the Shipping configuration. Tests of code Shipping does not compile (the
older renderer, the removed loader modes) cannot build or pass there; docs/RELEASE_VALIDATION.md lists them.

Every configure with a preset must repeat `-DSHADOW_RECOMP_DOL`, because the preset's value otherwise replaces the cached one.

`powershell -File installer/tests/Test-Uninstall.ps1` tests the uninstaller on fake installs under `.build/installer-test`.
