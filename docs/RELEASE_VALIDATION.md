# Version 0.0.4 (branch `release-0.0.4`, 2026-11-25)

Built on the mod loader update below (never published on its own), with the development tree's source brought from
1c9ac0be to 217f71b9 and two setup fixes. Checked on one PC: MSVC 19.52.37029, Windows SDK 10.0.26100, the CMake that
Visual Studio 2026 installs (4.3.1), Python 3.12, the PAL disc GUPP8P as the player's disc.

## What changed for the player

- **The setup no longer fails at step 1 of 5 with an older CMake** (issue #7: `Could not create named generator Visual
  Studio 18 2026`). The prerequisite check accepted any CMake from 3.25 on, but only CMake 4.2 and newer know Visual
  Studio 2026. The setup now asks each CMake it finds whether it knows that generator: the one Visual Studio 2026
  installs first, then those on PATH, then a default CMake install. When none does, it says which CMake it found and
  what to install, before anything is built.
- **A separate CMake install is no longer needed.** Visual Studio's own CMake is not on PATH outside its command
  prompt; the setup finds it in the Visual Studio it detected.
- **The game needs less processor time.** The same recorded input through the release executable of the mod loader
  update and through this one, interleaved, 4 rounds (8 runs each), headless, whole-process CPU time: Westopolis
  24.41 s -> 20.44 s (-16.3 %; ranges 24.20..24.58 and 20.39..20.61), Final Haunt 9.33 s -> 8.08 s (-13.4 %;
  9.22..9.42 and 7.95..8.30). The v0.0.3 executable needed 33.36 s for the Westopolis input on the same PC.
- From the development tree, besides the speed work: the fourth shoe jet (the original fills three of four), and a
  picture rate independent of the game's 60 Hz (`--frame-interpolation`, off unless asked for).
- Everything of the mod loader update: the mod loader and manager, the optional Japanese voices, the prebuilt parts.

## Source

`f42192b` takes the development tree's 430 commits after 1c9ac0be for `runtime/`, `extract/`, `tools/`, `test_data/`
and the CMake files (applied as one three-way patch: no conflict). Left out on purpose: the conversion waves (held out
of the development image too), the "fast owners" and the lifted sweep closure (no gain measured there, off by default,
and they need source trees this repository does not carry), research tools, the recovered and lifted trees, session
notes. The fast exact sine / cosine header lives in `runtime/lift/math/fast_trig.h` here. The liveness roots are
regenerated for this source. `lifted_render.{h,cpp}` are the development tree's 0458bae4 with the trail-cause counters
of its 08d2c0a7 placed correctly (that commit's own copy does not compile; repaired there as 9c124756).

## Results

| Check | Result |
|---|---|
| `tests` preset, all of ctest except `mtn_pose_contract_tests` (303 tests) | 296 passed in the full run; three more were fixed and pass (`lift_purity`: a guest address in the new header's comment; `native_host_isa_gate`: `fp_single_round_trip_tests` now links the host gate; `native_runtime_ratchet`: needed the game image's map in the configuration). The suite was not run again as a whole after those three fixes. Not passed, as before and not from this change: `shipping_platform_selection` (needs the reference executable, not built in this configuration), `f1c_host_stack_integrity`, `f1c_host_stack_integrity_notail`, `f1c_host_stack_game` (need the Python module `pefile`, not installed on this PC) |
| The development tree's tape gate on this repository's `release` executable (Shipping, MSVC) | 12 of 12 equal its references: Westopolis state `6d9d956eec70`, audio `c64ee7883f980f73`, behaviour trace, ten screenshots; Final Haunt state `46e94ea3142c`, audio `fa2abc800651039e` |
| Against the mod loader update's executable, run as that section ran it | final states and audio differ, as they must: the development tree changed behaviour on purpose in between (the fourth shoe jet among others; its gate pins the retail jets with `SHADOW_RETAIL_SHOE_JETS=1`). The gate row above is the comparison that counts |
| `dev/Package-Release.ps1 -Version 0.0.4` | package 54.3 MB, one prebuilt set `msvc-19.52.37029.0-sdk10.0.26100.0` (15 parts) |
| Scan of the prebuilt files and mod data against the disc | passed: no shared run of 64 bytes or more (longest real run 52), no translated function defined or referenced, no personal path |
| Install from the package: unpacked where the setup unpacks it, `installer/Install.ps1` in a shell with **no CMake on PATH**, fresh target folder | exit 0 in 433 s (438 s in an earlier run of the same kind); the prebuilt set was used; `install-info.json` records the package's commit with 0 modified files |
| The development tree's tape gate on the **installed** executable | 12 of 12 |
| The prerequisite check, three cases (a stand-in for a CMake 3.31 that lists Visual Studio 2022 as its newest generator) | old CMake first on PATH and Visual Studio's present: Visual Studio's is used and put first on PATH; only the old one: refused, with its version, its path and what to install; none on PATH: Visual Studio's is used |
| `installer/tests/Test-Uninstall.ps1` | 29 checks, 0 failed |

## Not tested

- The setup window driven by hand with this package (the install above went through `Install.ps1`, the same module
  the window's backend calls); another PC, another compiler or a clean Windows install. The player of issue #7 has not
  confirmed the fix: it was reproduced only with the stand-in CMake.
- `mtn_pose_contract_tests`, the `shipping-tests` preset, the strict comparison against the original code.
- Play by a person: nobody has played from this build. The picture-rate mode was checked as numbers in the development
  tree, not looked at.
- Mods and the Japanese voices with this source (unchanged code paths, validated in the section below).
## Later the same day: the setup, tested the way a player uses it

The results above went through `installer/Install.ps1`. Installing through the call the setup window makes, from
folders with awkward names, and with tools reported missing found more; each was fixed and the rounds repeated.
Measured and tested on the same PC. The numbers are from the packages of commits `2c00a2c` .. `a91adc2`; the package
of the last commit of this branch is checked again by the same scripts and its results are in the release notes.

**Fixed**

| Failure | Cause | Now |
|---|---|---|
| The setup window's install never started without a Japanese disc | the window passed a lone `-` as "none"; `powershell.exe -File` ends with exit code 2 and no output on that argument | `none` |
| The build failed at its very end for a disc folder with non-ASCII letters (`RC2135: file not found`) | the resource script named the disc's system files by their path; `rc.exe` reads the script in the ANSI code page | the three files are copied into the build tree and embedded from there (same bytes) |
| Visual Studio 2026 from the preview channel was not found | `vswhere` leaves prereleases out unless asked | asked for |
| `Install.cmd` (the zip) wanted a CMake on PATH and took any | the same cause as issue #7 | it uses the CMake the installer picks |
| Updating while the game runs failed at the copy step | the running program cannot be replaced | the folder check names the program to close |
| A folder the player cannot write in (Program Files) failed when the install started | not checked | refused at the folder check, in plain words |
| The build drive running full failed somewhere in the build | not checked | checked before the build |
| An older Python 3 failed in the middle of the build | any Python 3 was accepted | 3.10 or newer, else a current one is installed |
| Visual Studio's C++ tools without a Windows SDK failed at configure | not checked | counts as missing, the installer adds the SDK |
| A failed step said only its exit code | - | the first error line of that step and a hint for the known causes |
| A folder unpacked by another build of the same version was reused | the marker had no build identity | it names the build |
| About 1.3 GB of build files stayed under `%PUBLIC%\NukkoroRecomp` for good, and so did earlier versions' folders | never removed | removed after a successful install (only in a folder the setup exe unpacked) |

**New: the setup installs what the PC lacks.** With Visual Studio's C++ tools, CMake or Python missing, the Components
page says so in one card, Next stays open, and the install starts with a step that downloads the vendor's installer
(Microsoft, python.org), refuses a file not signed by that vendor, and runs it unattended (Visual Studio `--passive`
with the compiler, the Windows SDK 10.0.26100 and CMake named; Python `/passive` for the current user). Windows asks
once for permission. The WebView2 Runtime is installed the same way before the window opens.

**Shorter install.** From nothing unpacked to the finished install: 435 s -> 360 s (unpack 0.9, configure 10.8,
translate and compile 345.9, copies and finish 3.0). Three shards hold one very large function each; they compile
first now, and `verify_native_math.py` reads its inputs once instead of once per contract (53 s -> 2.2 s, same output,
same refusal of five broken inputs). The length is now set by `fn_803E1D58` alone: 164 s in the compiler by itself,
about 290 s beside eleven other compilers. More parallel compilers (20 instead of 12) changed nothing.

**Results**

| Check | Result |
|---|---|
| The setup exe unpacks itself (`--extract-only`) | exit 0; the tree equals the zip plus the setup window program |
| Install through the window's backend call, no CMake on PATH, disc folder and install folder with spaces, accented letters, brackets and an ampersand | exit 0 in about 355 s |
| Tape gate on that install | 12 of 12 |
| The installed game started in a window from its own folder, no arguments | window open and responding; it reaches the game's first screen (the 60 Hz question) |
| The packaged setup window's self-test with `--install`: every page, then Install, to "Installation Complete" | passed |
| The same with Python reported missing and a signed stand-in program served locally as "the Python installer" | the window lets the player go on, installs the stand-in as the step before the build, checks again, builds; passed |
| Tape gate on the game the window installed | 12 of 12 |
| The three real downloads (`vs_community.exe`, `python-3.13.9-amd64.exe`, `MicrosoftEdgeWebview2Setup.exe`), downloaded and signature-checked, not run | all valid, signed by Microsoft Corporation / Python Software Foundation |
| `installer/tests/Test-InstallCore.ps1` | all checks pass (failure causes, folder checks, the backend's command line, the tool step with a stand-in, the removal of build files) |
| `installer/tests/Test-Uninstall.ps1` | all checks pass |
| All of ctest except `mtn_pose_contract_tests`, one run | 299 of 303; the four others need Python `pefile` (3) or the reference executable (1), as above |

**Not tested.** The real unattended installs of Visual Studio and Python (this PC has both; the step is proven with
the stand-in, the component names are the ones installed here); the WebView2 install on a Windows that lacks it; an
older Python or a missing Windows SDK on a real PC (the checks are exercised only where this PC allows); a second PC;
the setup window driven by a person; play by a person.
# Mod loader update (branch `release-modloader`, 2026-11-15)

Not released yet. The game source is the development tree's `release/integrate` at 1c9ac0be (510 commits after the
17dd6e8 this repository was first made from), merged with this repository's own work. Checked on the same PC and
toolchain as the section below (MSVC 19.50.35730, Windows SDK 10.0.26100, CMake 4.2.3), the PAL disc as the player's
disc. Two packages were made with `dev/Package-Release.ps1`: `ml3` at commit `6cc83a0` (the full comparison, the
regression suites and the install times below) and `ml4` at `cac2a8f`, the last source commit (the scan, the prebuilt
list, and a fast install with its replay and mod check). Between them: a test's disc path, one comment in
`gamepad_binding.h`, and the data files that quoted the game's texts (below).

## What changed for the player

- The mod loader (`runtime/mods`, library `shadow_mods`) is in the game, and `ShadowModManager.exe` with its maps of
  the game (`mods-data/`) is installed beside it, with a *Mod Manager* Start menu shortcut.
- The Options menu is the game's own again; the window, the picture and the volumes are in `display.cfg` and
  `options.cfg` (docs/PC_SETTINGS_FILES.md). The Resolution row this repository added is gone, as is the Hungarian
  language.
- Keyboard and mouse controls, set in `controls.cfg`.
- The uninstaller removes the manager and `mods-data/`, and keeps `mods/` and `controls.cfg` with the saves and
  settings.

## Prebuilt parts

One set, `prebuilt/msvc-19.50.35730.0-sdk10.0.26100.0/` (`ml4`); package 53.4 MB with it:

| File | Bytes | SHA-256 (first 16) |
|---|---|---|
| `shadow_native_execution.lib` | 357,036 | `10710c1cd2c878f3` |
| `shadow_native_memory.lib` | 907,796 | `db24a651f91afa3c` |
| `shadow_native_gameplay.lib` | 717,240 | `f8b894e273d135fa` |
| `shadow_lift.lib` | 1,575,798 | `cb2058cdbc6dbde2` |
| `shadow_assets.lib` | 2,443,482 | `34f485a9be4e29f4` |
| `shadow_mods.lib` (new) | 10,791,800 | `7aec5d2cba154d5f` |
| `shadow_motion.lib` | 765,434 | `ac7567ef6a5ec36e` |
| `shadow_d3d12ma.lib` | 621,268 | `43502516cc3dc12a` |
| `shadow_render.lib` | 11,354,938 | `bb90d32cd41caa6c` |
| `shadow_rwim.lib` | 286,634 | `f2eaed74853bd647` |
| `shadow_render_lifted.lib` | 10,503,360 | `62bc5599f042c3bc` |
| `shadow_runtime.lib` | 64,118,996 | `38ca4e926fccac3a` |
| `ppcrecomp.exe` | 757,248 | `02c86a09a20989f6` |
| `ShadowPadConfig.exe` | 1,259,008 | `c933357b22830369` |
| `ShadowModManager.exe` (new) | 996,352 | `cc03e577531c862d` |

`runtime/mods/afs.cpp` joined the local sources: the first scan found a 70-byte run of the disc's file table in
`shadow_mods.lib` (the memory-card banner and icon file names it checks, which the compiler pools in the disc's
order). The release step now builds the local sources in its tree like a full build and removes their objects from
the copied archives (`lib.exe /REMOVE`), so the prebuilt manager, which links `shadow_mods`, is the program a full build
makes.

## Scan

`dev/scan_game_code.py` (`ml4`) over the 16 prebuilt files and the three `mods-data` files (19 files, 108,481,598
bytes) against all of `sys/` and `files/` (3,485 files): **passed**. Longest real shared run 52 bytes; no file with 256
real shared bytes; no defined `fn_8xxxxxxx`; no personal path.

| Shared run | Bytes | What it is |
|---|---|---|
| `ppcrecomp.exe`: ASCII case/identity tables | 384, 258, 256 | toolchain (as before) |
| `shadow_runtime.lib`: the SDK's machine-check and DVDOpen message texts | 52, 43 | as before |
| `shadow_runtime.lib` (`native_message_system.cpp`): three rows of the subtitle window's layout constants, 12 floats | 48 | game data, under the limits |
| `ShadowModManager.exe`: memory-card icon file names (`afs.cpp`) | 48 | file names, under the limits |
| `file_contexts.json`: one file-name format string of the executable | 40 | under the limits |
| `ShadowPadConfig.exe`: UTF-16 "Shadow the Hedgehog ..." | 41, 39, 39, 35 | as before |

**The data files the source package carries.** The byte scan compares UTF-8 JSON and Markdown as they are, while the
game keeps its texts in UTF-16. So every data file new in this update (`test_data/mods`, `tools/mods/*.json`,
`tools/save_text`, `docs/mods`) was scanned again with its strings re-encoded as UTF-16LE, Shift-JIS and Latin-1. That
found the game's own texts, which are no longer in the repository:

| File | Found | Now |
|---|---|---|
| `test_data/mods/region_diff.json` (installed as `mods-data`) | the save and memory-card messages of every language and the disc banner's texts, quoted where the two regions differ (runs up to 165 bytes) | keeps only the per-path fields the manager reads: MD5 sums, kind, risk, summary, advice, stage title. The manager's region notes work from these |
| `tools/save_text`: `save_originals.json`, `pc_save_<lang>.json`, `review_2026-11-05.py` | the save messages (runs up to 601 bytes) | not kept; the generated `runtime/src/gekko/pc_save_texts.inc` stays. These tables were in the earlier releases under `tools/options_menu_sim/save_text` |
| `docs/mods/map/VOICE_LINES.md` | two quoted voice lines | replaced by a description |

What remains: file names (lists of disc file names in `file_contexts.json` and `reloaded_features.json`, which match
the executable's string table only when re-joined with NULs) and phrases under 50 bytes such as the game's title.
Unused research records of the mod work (`voice_lines.json`, `format_*.json`, `region_ingame_proof.json`,
`codemods/site_classes.json`) are not kept.

## Builds and the game

| Check | Result |
|---|---|
| Fast install of `ml3` and of `ml4` (prebuilt parts used: "Prebuilt game-code-free parts: ...msvc-19.50.35730.0-sdk10.0.26100.0") | exit 0 |
| Full install of `ml3` without `prebuilt/` | exit 0 |
| `ShadowTheHedgehog.exe`, `ShadowModManager.exe`, `ShadowPadConfig.exe`: fast vs full (same folder) | 4 bytes differ in each: the PE and debug-directory timestamps |
| `mods-data/` fast vs full | identical |

Headless Westopolis replay (the gates' invocation with `--audio-dump`, the installed exe with its own game files),
12,798,000,000 ticks, 9,477 VI frames, 18,920 frames:

| Executable | State | PCM |
|---|---|---|
| `ml3` fast install | `f1e9e48ae923` | `c64ee7883f980f73` |
| `ml3` full install | `f1e9e48ae923` | `c64ee7883f980f73` |
| `ml4` fast install | `f1e9e48ae923` | `c64ee7883f980f73` |
| The development tree at 1c9ac0be, built with this repository's `release` preset (Shipping, lifted, plume off) | `f1e9e48ae923` | `c64ee7883f980f73` |

The development tree's own gate gives state `5c27c8fa6c7d` with the same PCM: that is its `watch` build (not Shipping,
`SHADOW_WATCH_SYMBOLS`, reference runner on). Built in the same Shipping configuration, the development tree's source
gives exactly this repository's state, so the merge changed nothing the replay sees; the state report differs between
the two configurations, not between the two trees.

## Mod loader and manager, installed

| Check | Result |
|---|---|
| `ShadowModManager.exe help`, `list` (installed folder) | exit 0; "mods on, folder <install>\mods, 0 installed" |
| The window (`--snapshot`) | opens; list, details, Install..., Load mods, Play |
| `install cream.rar` (a GameBanana download: a loose `shadow.one`) | "1 file ... shadow.one placed at character/shadow.one"; installed switched off |
| `enable cream`, `plan` | `character/shadow.one` from `mods/cream/files/` |
| The game on the Westopolis tape, capture at tick 1.2e9 | "Mods: 1 active from <install>\mods (1 files, ...)"; `--mods-report` applies the same file the plan lists; Cream is the player in the capture |
| State and capture, with the mod / with `--no-mods` | state `1333820e1e25` / `3542c3ba0f50`, capture `f0c6259b61f4` / `5ff3bdaae89d` |

Done with `ml3` and again with `ml4` (its reduced `region_diff.json`): the same hashes. `tools/mods/modtest.py` needs `--asset-trace`, which the Shipping executable refuses, so its scenarios were not run
against the installed game; the check above is the same cream-model scenario without the read proof.

## Uninstall

| Check | Result |
|---|---|
| `Uninstall.ps1 -Yes` (fast install, one mod installed) | removed 3,486 items: the three programs, `mods-data/` (3 files), the game files; `mods/` kept and reported |
| `Uninstall.ps1 -Yes -RemoveSaves` (full install, one mod installed) | the folder is gone |
| `installer/tests/Test-Uninstall.ps1` | all checks pass |

## Regression tests

| Preset | Passed | Not passed |
|---|---|---|
| `tests` | 274 of 275 | `lift_purity`, then fixed (below) and passing |
| `shipping-tests` | 265 of 272 | 7, all known: they cannot apply to Shipping |

`lift_purity` refused this repository's own comment in `gamepad_binding.h` ("GameCube"): the development tree's check
now covers that folder. The comment was reworded and the test passes; the suite was not run again for that comment.
`adx_decode_contract_tests`, new from the development tree, looked for the disc at `../game/files`; it now uses the disc
beside the configured main.dol and passes. `difftest_f1b`, the development tree's one known failure, passes here
(this repository's regenerated fingerprints).

Not passed in `shipping-tests`, each for the reason the earlier release recorded: `native_matrix_call_tests`,
`gekko_core_tests`, `native_failure_publication_tests`, `lift_sites_reference_tests` (code Shipping does not compile:
not built), `native_expand_jobs_tests` ("the native expand jobs did not install"), `guest_watch_tests` (non-shipping
only), `compiler_body_tests` ("the SelectThread idle loop head has no generated entry"). The earlier list's four
control-row tests (`platform_gc_removal_plan`, `platform_runtime_coupling`, `platform_retired_readers_*`) are no
longer registered in a Shipping tree.

## Install time

`dev/time_install.py`, one run each, 12 jobs, 60% CPU cap, unpacked into `%PUBLIC%\NukkoroRecomp\ml3`:

| Phase (seconds) | Fast | Full |
|---|---|---|
| Unpack and checks | 1.6 | 1.6 |
| Configure | 10.3 | 3.9 |
| Libraries, `ppcrecomp`, shaders | 7.7 | 41.3 |
| Shard translation | 12.7 | 13.2 |
| Waiting for `shadow_render_lifted` | 0.3 | 24.6 |
| Shard compile | 230.0 | 229.6 |
| Link | 13.1 | 13.0 |
| Post-link checks | 72.5 | 72.3 |
| `ShadowPadConfig` + `ShadowModManager` | (copies) | 8.1 |
| Copy and finish | 2.5 | 2.5 |
| **Total** | **351.6** | **410.1** |

The `ml4` fast install took 504.1 s with every phase 1.3 to 1.8 times longer (shard compile 300.1 s, post-link
checks 128.9 s): the machine was busier then; it is not a comparison of the two packages.

## Not tested

- The setup window with this package; another PC or compiler; a clean Windows install.
- The manager's window driven by hand (`tools/mods/gui_test.ps1` was not run); installing more than one mod, texture
  packs and AFS mods against the installed game.
- Final Haunt with the installed executable; gameplay beyond the Westopolis replay.

# Release validation: v0.0.1

Recorded 2026-11-09. The release's commit is the one tagged `v0.0.1`; each install records it in `install-info.json`.

## Reproducing the build

Install from a clean checkout with `Install.cmd`. Each install writes `install-info.json`, which records the commit, the
number of modified files and the settings read from the build's CMake cache.

| Setting | Value |
|---|---|
| Preset, configuration | `release`, Release |
| `SHADOW_SHIPPING` | ON |
| `SHADOW_RENDER_LIFTED` | ON |
| `SHADOW_ENABLE_PLUME` | OFF |
| `SHADOW_ENABLE_TRACY`, `SHADOW_BUILD_TESTS`, `SHADOW_BUILD_REFERENCE` | OFF |
| `SHADOW_RECOMP_SHARDS` | 48 |
| Generator | Visual Studio 18 2026 |
| Disc | GUPP8P, `main.dol` SHA-1 `6ff04994371e93d6ba1015644f4e7467713b9363` |

## Source compared with the development tree

Against the development tree this release was taken from, `rwcore` and `rpworld` are identical. The product source differs in these files:

- The Resolution row: `native_options_display.{h,cpp}`, `settings_display.cpp`, `display_settings.h`, `options_menu_texts.inc`.
- `native_vi_wait.cpp` (return addresses computed from the row's address, same values) and `unorm_calibration.cpp` (clears
  the target before its first use).
- `native_hungarian_palsel.cpp`, a stub: its generated picture is not published. `tools/hungarian/make_palsel.py` makes it.
- Comments only: six files in `runtime/lift`, six in `runtime/scaffold/rw_driver`.
- The default controller layout, in `runtime/lift/peripheral/gamepad_binding.h`: the GameCube pad's B on the left face button, its X on the right.

## Results

### Regression tests

| Build | Passed | Not passed |
|---|---|---|
| `tests` preset | 237 of 238, the other fixed below | none after the fix |
| `shipping-tests` preset | 224 of 235 | 11, listed below |

`shipping_platform_selection` failed in the `tests` run because of an empty argument in the test's own setup. It was then
fixed and passes in both trees; the other tests were not re-run for that one-line change.

These 11 cannot apply to the Shipping configuration. All pass in the `tests` preset:
`native_matrix_call_tests`, `gekko_core_tests`, `native_failure_publication_tests`, `lift_sites_reference_tests` (they
use code Shipping does not compile), `native_expand_jobs_tests` (loader modes Shipping removes), `guest_watch_tests`
(non-shipping only), `compiler_body_tests` (its helper and audio checks pass; its last check needs an idle loop Shipping
does not have), and `platform_gc_removal_plan`, `platform_runtime_coupling`, `platform_retired_readers_context`,
`platform_retired_readers_reverb` (their control rows are no longer compiled in Shipping).

`dolrecon_lift_tests` is not in either count: it needs generated code that cannot be published. Run from the development
tree it fails with last-digit floating-point differences. That code is compiled only into that test, not into the game.
**This failure is unresolved.**

### Strict comparison against the original code

Reference build, 30 s per stage:

| Stage | Segments matched | Mismatched |
|---|---|---|
| Westopolis | 10,193,336 | 0 |
| Final Haunt | 11,168,459 | 0 |
| Lethal Highway | 9,925,857 | 0 |

Each stage matched 169 more segments than before the Resolution row (reverting that commit restores 10,193,167 for
Westopolis). The menu's fonts grew with its texts (English 116,982 to 117,166 bytes), which fits a little more guest
work at boot, but that link was not proved.

### The installed Shipping executable

Installed by `installer/Install.ps1` into a folder inside this repository's build tree, and run from there with its own
game files:

| Check | Result |
|---|---|
| Replay determinism, Westopolis tape, twice | Audio `46a70a1051347069` both times |
| Run length | 3,645,000,000 ticks, 2,697 fields, 5,360 frames |
| Five rendered frames | All identical to the reference images |
| First launch, headless and in a real window | Exit 0 |
| Create a save through the game's dialog, reload it in a new process | Passed |
| PAL save | All 8,028 record bytes reach game memory |
| Converted USA and Japanese saves | Same, except the one byte PC settings override |

### Installer and uninstaller

| Check | Result |
|---|---|
| Build and install from an empty build folder | Passed |
| Update over an existing install | Save unchanged |
| `Uninstall.cmd` | Removes the install and its shortcuts; keeps saves, settings and an unrelated file |
| Reinstall over kept saves | The game loads them |
| `Uninstall.cmd -RemoveSaves` | Folder gone |
| `installer/tests/Test-Uninstall.ps1` | All checks pass |
| Setup backend (prerequisites, disc, target) | Passed |

### Controller configurator

| Check | Result |
|---|---|
| `pad_config_tests`, against the game's own parser | Passed |
| Every button chosen by clicking the real window's pictures | All 12 correct |
| Save in the installed configurator, then the installed game reads it | Passed |
| Uninstall removes it and keeps `pad.cfg` | Passed |

## Not tested

- Choosing a button by pressing it on a real controller in the configurator, and the pressed highlight.

- A person clicking through the setup window. Its own scripted run went through every page and a complete install.
- Shortcuts in the real Desktop and Start menu. Their code ran with folders inside this repository, as the work was
  limited to it.
- Another machine, or a clean Windows install.
- Add/Remove Programs: no entry existed at the time of this round, by design; removal was through `Uninstall.cmd` or the
  Start menu shortcut. (Since release 0.0.4 the setup writes a per-user Installed apps entry and installs an uninstall
  window, `Uninstall.exe`; that round is recorded in the 0.0.4 release notes.)
- The new Resolution row by script in the real menu; its logic has automated tests, and the owner tried it in the game.
- How the game feels to play, beyond the owner's own sessions.

## Known limitations

- Input tapes recorded before stage loading became faster arrive late. The check tape above is unaffected.
- The Hungarian language is switched off in this release. Its code, texts and tools remain.
  Every launch prints a warning that the Hungarian 50/60 Hz picture was not applied; it is harmless.
- `rwcore` and `rpworld` hold reconstructed RenderWare code. Check its licensing before publishing this repository.
- Two floating-point test mutants were removed and two difftest fingerprints regenerated after the model was corrected:
  estimate instructions do not flush denormals.

# Installer: prebuilt game-code-free parts (branch `installer-prebuilt`, 2026-11-14)

Not released yet. Measured and checked on one PC (i9-13900K, 32 threads, 64 GB; Visual Studio 2026 Build Tools,
MSVC 19.50.35730, Windows SDK 10.0.26100; CMake 4.2.3), with the PAL disc as the player's disc (read only).

## What is prebuilt

`dev/Package-Release.ps1 -Version pb4` (commit `4a6c8ce`), configured with no main.dol, produced one set,
`prebuilt/msvc-19.50.35730.0-sdk10.0.26100.0/`, 15.9 MB compressed in the package (package 37.7 MB without it, 53.6 MB
with it; setup exe 54.4 MB):

| File | Bytes | SHA-256 (first 16) |
|---|---|---|
| `shadow_native_execution.lib` | 357,036 | `c3c51e302003a37e` |
| `shadow_native_memory.lib` | 907,796 | `d509264e3fc03a2d` |
| `shadow_native_gameplay.lib` | 720,236 | `1dbbd4247460e170` |
| `shadow_lift.lib` | 1,127,436 | `2468756400a51172` |
| `shadow_assets.lib` | 2,443,482 | `3d9b0362cda5b928` |
| `shadow_motion.lib` | 765,434 | `bdc7a43a9740a6fe` |
| `shadow_d3d12ma.lib` | 621,268 | `bf91b5e9526e6111` |
| `shadow_render.lib` | 11,350,354 | `eeefd8e65884fea6` |
| `shadow_rwim.lib` | 286,634 | `81e2312deb13f46e` |
| `shadow_render_lifted.lib` | 11,068,850 | `7032a47f4c02bbf5` |
| `shadow_runtime.lib` | 56,690,712 | `622c26e197d8a413` |
| `ppcrecomp.exe` | 702,464 | `64f16000cd8013c5` |
| `ShadowPadConfig.exe` | 1,259,008 | `a03a066bd0a85115` |
| `prebuilt.json` | | the key, these hashes, the toolchain |

Never prebuilt: everything `ppcrecomp` generates from the player's main.dol, `shadow_recomp.lib`, `ShadowTheHedgehog.exe`
(it embeds `sys/main.dol`, `boot.bin`, `bi2.bin`), the host ISA gate, `shadow_msl`, and the four sources in
`SHADOW_PREBUILT_LOCAL_SOURCES`.

## No game code, no disc data

`dev/scan_game_code.py` over the 14 files against all of `sys/` and `files/` (3,485 files, 1,362,605,808 bytes; each
prebuilt file indexed as is and byte-swapped in 16/32-bit units, 19.3 million windows): **passed**.

| Shared run | Bytes | Class |
|---|---|---|
| `ppcrecomp.exe`: three ASCII case/identity tables (main.dol and apploader have the game C library's copies) | 384, 258, 256 | toolchain: found verbatim in the SDK's `libucrt.lib` |
| `shadow_runtime.lib` (`native_os_machine_check.cpp`): `Machine check received\nHID2 = 0x%x   SRR1 = 0x%x\n` | 52 | real, under the limits: the SDK's message text, repeated by the runtime |
| `shadow_runtime.lib` (`native_file_api.cpp`): `DVDOpen(): file '%s' was not found under` (twice, overlapping) | 43, 34 | real, under the limits: SDK message text |
| `ShadowPadConfig.exe`: UTF-16 "Shadow the Hedgehog ..." (also in the game's font files) | 41, 39, 39, 35 | real, under the limits: the product's name |

No defined `fn_8xxxxxxx` symbol; nine string literals name guest functions (diagnostic messages); no path of the
release machine's user.

The first scans (before the sources were kept local) found, and the release step now leaves out: the AX compressor's
gain table (`native_audio_mixer.cpp`, from main.dol 0x80563420), button-sheet palettes and texels
(`button_glyphs.cpp`), the SDK exception vector's 38 PowerPC words (`gekko/os_exception_vector.h`, compiled by
`native_boot.cpp` and `os_exception_entry.cpp`) and fdlibm's constant tables (`shadow_msl`). **These are in the
repository's published source and in every source package already; prebuilding only stops repeating them in compiled
form.**

## The prebuilt objects are the player's compiler's objects

`dev/compare_libs.py` (every object, `.debug$S` / `.chks64` / header timestamp aside, which the linker drops):

- Prebuilt archives against the archives a full build of the same package makes in the same folder
  (`%PUBLIC%\NukkoroRecomp\pb4`): every object identical except `system.obj` (its `__DATE__ __TIME__`); the full build
  has the four local sources' objects, which the prebuilt archives do not.
- Against `main`'s package built with a main.dol in another folder: identical once MSVC's anonymous-namespace hashes
  (`?A0x...`, derived from the source path) are set aside. A main.dol in the tree changes none of these objects.
- `ppcrecomp.exe` and `ShadowPadConfig.exe` against the full build's: only the PE and debug-directory timestamps differ.
  The prebuilt `ppcrecomp` generated byte-identical sources (`recomp_table.cpp`, 48 shards, `recomp_decls.h`) in every
  install below.

## The installed game

Every install below was a fresh unpack of the package into a new folder, installed by `installer/Install.ps1` with the
player's disc. Executables compared section by section:

| Pair | `.text`, `.pdata`, `.rsrc`, `.reloc` | Bytes that differ |
|---|---|---|
| Fast vs full, same folder (`%PUBLIC%\NukkoroRecomp\pb4`) | identical | 9: PE timestamp, debug-directory timestamp, the build-time string |
| Fast vs `main` (other folders) | identical | 488: those, plus 478 in RTTI names (`?A0x` path hash) |
| `main` vs `main` (two folders) | identical | 490: the same kinds |

Headless Westopolis replay (the `run.sh` invocation of the gates, plus `--audio-dump`; the Shipping exe has no
`--behaviour-trace`), 12,798,000,000 ticks, 9,477 VI frames, 18,920 frames each:

| Install | State | PCM |
|---|---|---|
| `main` package, two installs | `93630df0b4e2` | `c64ee7883f980f73` |
| Fast, two folders | `93630df0b4e2` | `c64ee7883f980f73` |
| Full build of the new package, two folders | `93630df0b4e2` | `c64ee7883f980f73` |

`state.json` files are byte-identical.

## Install time

`dev/time_install.py`: fresh folder, the package unpacked as the setup does, `Install.ps1`, each log line stamped as it
appears. 12 compile jobs in every run (`Get-InstallJobs` picks 12 on this PC; `main` was given `-Jobs 12`). Runs one at
a time under a 60% CPU cap, holding the `benchmark` lock, alternated `main` / fast / `main` / fast. Same disk (H:).
The disc's files were in the OS file cache for every run after the first.

| Phase (seconds) | `main` 1 | `main` 2 | fast 1 | fast 2 | full build, new package |
|---|---|---|---|---|---|
| Unpack and checks | 1.3 | 1.2 | 1.6 | 1.6 | 1.6 |
| Configure | 3.6 | 3.7 | 9.1 | 9.2 | 3.8 |
| Libraries, `ppcrecomp`, shaders (until the translation starts) | 142.4 | 136.1 | 6.2 | 6.1 | 29.5 |
| Shard translation (`ppcrecomp` runs) | 13.9 | 17.3 | 11.5 | 10.9 | 12.6 |
| Waiting for `shadow_render_lifted` | 31.8 | 27.4 | 0.2 | 0.3 | 31.5 |
| Shard compile | 246.6 | 241.0 | 240.1 | 237.3 | 242.7 |
| Link | 11.7 | 11.1 | 11.3 | 11.7 | 11.4 |
| Post-link checks | 69.5 | 68.5 | 68.1 | 69.4 | 69.7 |
| `ShadowPadConfig` | 4.6 | 4.5 | (copy) | (copy) | 4.5 |
| Copy and finish | 2.5 | 2.4 | 2.3 | 2.6 | 2.5 |
| **Total** | **528.0** | **513.3** | **350.9** | **349.5** | **409.9** |

Configure is 5.5 s longer with the prebuilt parts: it hashes the 1,492 source files and the 88 MB of prebuilt files.
The full build of the new package (`/MP` on `shadow_runtime_common`) is what a player whose compiler does not match
gets. The post-link checks still run (69 s); they are not skipped. In the same folder on C:, fast 357.5 s, full 414.9 s.

## Not tested

- Another PC, another compiler version (the fallback was checked by configuring with a changed key and a changed file:
  both build everything), a clean Windows install.
- The setup window's own scripted run with the new package; the setup exe was built and unpacked the package (14
  prebuilt files present).
- Gameplay beyond the Westopolis replay.

# Release 0.0.4, the last day (2026-11-26)

What was added on the day of the release, how it was tested, and what was found. The round that ran on the released
package itself is summarised in the release notes; the rounds below ran on the commits before it, each a full package
build from that commit (`dev/Package-Release.ps1`, with the content scan).

## What a round is

1. The tests tree at the commit and the whole ctest suite.
2. The package, the setup window and the single-file setup program from the commit.
3. Two installs from the package: through the call the setup window makes (folders with spaces, accented letters,
   brackets and an ampersand; a Python under a non-ASCII path), and through the setup window's self-test with Python
   reported missing and a stand-in installer. Each installed game runs the development tree's tape gate (two recorded
   stages: state, audio, behaviour trace, ten screenshots; 12 checks).
4. The installer's and the uninstaller's own tests, and the zip's `Install.cmd` route.
5. `Settings.exe --check-layout` and `ModManager.exe --check-layout`.
6. Mod scenarios (`tools/mods/modtest.py`).
7. Player scenarios: cancel in the build and Retry (gated), an update over an install holding a player's files, an
   install over a running game, the uninstaller.

## Results

| Commit | What it added | Round |
|---|---|---|
| 44fdd98 | guides, Settings save fix, development keys and console off | full: all passed |
| 3d79baf | stick pictures, camera mouse lists | full: all passed |
| ea6553d | stick pictures | package, installs, layout, mods: all passed |
| 1407d5c | the uninstall window and the Installed apps entry | full: all passed; uninstall window and console uninstaller on the real installs |
| f69f411 | the error message, the mods guide, the harness fix | full: all passed but one screenshot hash, see below |

ctest was 301 of 305 in every round; the same four cannot run on the test PC (three need the Python module `pefile`,
one a reference executable that is not built).

## Found on the day

- **Development keys in the player's game.** N and M (savestate), F5 (snapshot), F8 and F9 (markers), F6 and the
  keypad (effect layers) answered in the installed game. Off under `GEKKO_SHIPPING` now. Checked on the installed game
  against the previous build as a control: the control wrote a savestate and a snapshot, the new build nothing.
- **A console window beside the game.** The player's build lets go of a console Windows opened for it alone. Checked
  the same way (the control holds a console, the new build none).
- **Settings: Save blocked by an untouched value.** Read in the code; no test exercises it.
- **Mods with real downloads.** 17 files from GameBanana's Shadow the Hedgehog section. 15 are loadable mods: each
  alone plays the whole Westopolis tape on the player's executable (the gate's runner, `run_tape.ps1`), as do the
  seven- and eight-mod halves, the 14 without the stage mod, and the stage mod with each of eight others. All 15
  together stop at tick 2,002,050,000 with a 2-byte load from address 4: the motion bank's clip allocation had
  returned 0 (28 times; 0 in every other run). The game's own heap is exhausted; the console has the same heap. Three
  texture packs: 5 textures drawn from packs on the player's executable. The save download is refused by name. One
  archive arrived cut off (a download fault here, shown by its checksum) and was fetched again.
- **The game vanished without a word on an error.** It now shows one message; with mods on it says how many are on.
  The first version was never seen (a message box started after the window has closed is ended by the pending
  WM_QUIT); it runs on its own thread now.
- **The mod harness skipped every picture check.** It read `cap.png`; the tests build writes `cap.ppm`.
- **One screenshot hash differed once.** In the round on f69f411 the gate on the cancel-and-retry build differed in
  one of ten screenshots: one pixel, one step in one channel. The same executable passed 12 of 12 when the gate was
  run again. Not reproduced, cause unknown.
- **The cancel scenario counted no processes.** Its filter matched command lines, and a compiler's command line is a
  response file. It counts by parentage now.

## Not tested

A clean Windows or a second PC; Windows 10; the real unattended installs of Visual Studio and Python (a stand-in is
used); the Settings program by hand with a controller; the mouse-direction settings in play; mods in other stages, in
the menus and in long play; Shadow the Hedgehog Reloaded; play by a person from the released build.

## More game memory for runs with mods (2026-11-27)

The heap exhaustion above is removed for mod runs. `GuestMemory::extend_legacy_heap` maps the game's heap range up to
0x90000000 (256 MiB of game memory) when a mod replaces game files; the boot block's memory size, the BI2 address and
the arena limit follow (`gekko/game_memory_end.h`), and the ten places that compared a pointer with the console's
0x81800000 compare with the run's end. On the development build: the 15 mods together play the whole Westopolis tape
with 0 failed clip allocations at 64 MiB and at 256 MiB, at the same CPU time; a run without mods and a run with a
texture pack alone keep the console's range and an identical state hash. The player's build and the gate: see the
release notes. Not shown: that behaviour with a mod is the same in the larger memory as in the console's (state
hashes differ by the memory size words and the heap's free tail); that is piece W1 of `docs/memory/REWRITE_PLAN.md`.

## The main menu's EXIT entry (2026-11-27, for 0.0.5)

Branch `release-0.0.5`: the published 0.0.4 (b367453) plus three commits (the entry and its Yes / No pop-up; each
language's own word; the drawn kanji and the clearer Q). Design and findings: `docs/re/main-menu-exit/README.md`.

MEASURED on commit fed4de6, one PC (Windows 11, Visual Studio 2026):

- Unit tests 302 of 306 (the new `main_menu_exit_tests` passes; the other four cannot run on this PC, as before).
- Package, two full installs (one through the setup window) and the build made by cancel and retry: each reproduces the
  reference states, audio and screenshots of the two recorded stages, 12 of 12. Installer tests 57 of 57, uninstaller
  tests, the zip route, update, install over a running game (refused), the 13 mod scenarios and the layout checks pass.
- The installed game in a real window, started the way a shortcut starts it, driven by a recorded controller tape:
  EXIT then Yes closes the game by itself after 54 s with exit code 0 and no message box (also with a music mod and a
  texture pack on, and with `--no-mods`); EXIT then B leaves the game running, and it then closes normally by its
  window's close button. N, F5, F6 and F8 still do nothing and no console opens.
- Unattended runs in the six text languages show the entry (EXIT, QUITTER, BEENDEN, ESCI, SALIR, 終了) and the question
  in the game's blue pop-up; pictures were looked at for each.

Not checked: pressing the buttons by hand on a real controller; a texture pack that replaces the title's `TitleParts`
picture (it would hide the word); the entry with every button-prompt style side by side (seen with the Xbox style
only); other PCs. 終了 is drawn slightly lighter than the katakana beside it. The Q, the B and both kanji are new
artwork in the menu's style; the C is the English sheet's own.