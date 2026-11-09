# Parallel agents: GameCube SDK removal (2026-11-07)

The user asked for several agent sessions, each in its own isolated worktree, removing the remaining GameCube SDK
code with native C++ while preserving the original behaviour exactly. This file is the shared contract. The main
session (in `H:/ShadowTheHedgehogDiskGC_Extract/wt-gc-removal`, branch `gc-removal`) integrates every branch.

Read first: `AGENTS.md`, `docs/NORTH_STAR_COMPACT.md`, `docs/GC_REMOVAL_PLAN.md` (mechanism, gates, and the worked
example "Step 2 done 2026-11-07"), `docs/NEXT_SESSION.md` (top section).

## 1. Isolation

- Each agent works ONLY in its own worktree `H:/ShadowTheHedgehogDiskGC_Extract/wt-agent-<name>` on branch
  `agent/<name>`, cut from `gc-removal` at the snapshot commit. Never edit another worktree, `wt-gc-removal`,
  `source/`, `game/`, `EXTRACTED_DISKS/` or `research/`. Read them freely.
- Shared read-only inputs: `../game` (game data, `sys/main.dol`), `../source/.build/dolrecon` (dolrecon outputs:
  platform manifest, skeleton, names), `../source/.build/recordings/westopolis-cpu-spike` (the replay tape),
  `../research/sources/repos` (Dolphin, hwtests: reference only, never copied into shipped code).
- Commit your work on your own branch when a step passes its gates (`git -c core.autocrlf=false`, byte-preserving).
  Never push, never merge, never rebase other branches. The main session merges.
- Shared list files (`extract/native-owners.txt`, `extract/wait-points.txt`, `extract/liveness-roots.txt`,
  `CMakeLists.txt`, `runtime/src/gekko/system.cpp`): edit them in your worktree, but keep your edits to your own
  subsystem's section (append a commented block, do not reorder or reformat other lines) so the merge is mechanical.
  Line endings: every file keeps exactly the endings it has in `gc-removal` (most files, including all `extract/*.txt`
  lists, CMakeLists.txt and system.cpp, are LF; a few sources are stored CRLF; corrected 2026-11-07 - an earlier note
  here said the lists were CRLF, which was wrong). Scripts that rewrite a file must preserve its endings, and
  `git -c core.autocrlf=false` commits exactly the bytes on disk. Before committing, check that
  `git diff --stat --ignore-cr-at-eol` and `git diff --stat` show the same line counts; if they differ you are
  committing line-ending changes. Mixed endings turn every merge into a whole-file conflict.
  Every owner (any new runtime literal) makes `extract/liveness-roots-lifted.txt` stale: regenerate it in your branch
  with the `native_liveness_roots_lifted` test's exact arguments (`ctest -R native_liveness_roots_lifted -N -V`
  prints them; `liveness_roots.py --out` to a new file, then copy it over) and check that the test passes.
  Do NOT change `SHADOW_GC_REMOVAL_RATCHET`: report your measured body counts; the main session lowers it after merging.

## 2. CPU limit (enforced) and locks

Every build, test, generator run and game run goes through the capped runner. It puts the command and all its children
in a Job Object with a hard CPU-rate cap (10 % of the whole machine by default) and holds named cross-session locks:

    powershell -NoProfile -ExecutionPolicy Bypass -File tools/agents/capped_run.ps1 `
        -Lock <lock>[,<lock>] -Command "<command line>" [-WorkingDirectory <dir>]

It prints the job's measured CPU share at the end; quote it in your report. Never run builds, CTests, the generator or
the game outside it. Never raise `-CpuPercent` above 10.

Locks (take only what the operation really conflicts on; independent work in your own worktree needs none):

| lock | hold it for |
|---|---|
| `game-window` | any WINDOWED game run (one window and one audio device stream at a time). Headless runs need no lock. Before a windowed run, check that no `ShadowTheHedgehog*.exe` the user started is running (`tasklist`); if one is, the user is playing: run headless or wait, never stop it. |
| `generator` | `tools/platform/what_if.py`, `removal_plan.py` and other ppcrecomp generator runs (heavy, and they write `.build/platform/what_if/_baseline` style caches) |

Builds and CTests in your own `.build/` need no lock (separate trees, capped CPU).

## 3. Your build trees (in your worktree; deployment disabled)

The planner and `what_if` classify rows by service from the dolrecon manifest, and the shared `../source/.build/dolrecon` one has no
identified SDK names (101 shipping bodies show as `sdk:unidentified`). Build your own first, once and after any change to
`test_data/platform/*names*`: `python -B tools/platform/regen_manifest.py` (writes `<worktree>/.build/dolrecon`, never the shared copy), and give
every tree `-DSHADOW_DOLRECON_DIR=<worktree>/.build/dolrecon` (the CMake default already is that path; a tree configured earlier keeps the old
cache value: reconfigure it). Totals do not change, only the per-service split.

A new tree copies its exe over the user's game unless `SHADOW_DEPLOY_DIR` is empty: always pass `-DSHADOW_DEPLOY_DIR=`
and check `grep -c copy_if_different .build/watch/ShadowTheHedgehog.vcxproj` is 0 before the first build.

    # game tree (lifted renderer, reference exe, symbols for the guest watch)
    cmake -S . -B .build/watch -G "Visual Studio 18 2026" -A x64 -DSHADOW_RENDER_LIFTED=ON -DSHADOW_BUILD_REFERENCE=ON
        -DSHADOW_SHIPPING=OFF -DSHADOW_BUILD_TESTS=OFF -DSHADOW_ENABLE_PLUME=OFF -DSHADOW_WATCH_SYMBOLS=ON
        -DSHADOW_RECOMP_SHARDS=48 -DSHADOW_RECOMP_COMPILE_JOBS=2 -DSHADOW_DEPLOY_DIR=
        -DSHADOW_RECOMP_DOL=H:/ShadowTheHedgehogDiskGC_Extract/game/sys/main.dol
        -DSHADOW_DOLRECON_DIR=<worktree>/.build/dolrecon
    # tests tree (without SHADOW_RECOMP_DOL no generator outputs exist)
    cmake --preset tests -DSHADOW_RECOMP_COMPILE_JOBS=2 -DSHADOW_DEPLOY_DIR=
        -DSHADOW_RECOMP_DOL=H:/ShadowTheHedgehogDiskGC_Extract/game/sys/main.dol
        -DSHADOW_DOLRECON_DIR=<worktree>/.build/dolrecon
        -DSHADOW_DISC_IMAGE=H:/ShadowTheHedgehogDiskGC_Extract/wt-gc-removal/.build/platform/GUPP8P.gcm
    # builds: MSBuild switches with a dash in Git Bash (-m:2 -v:m), never /m
    cmake --build .build/watch --config Release --target ShadowTheHedgehog -- -m:2 -v:m
    cmake --build .build/tests --config Release -- -m:2 -v:m
    # Known: a FULL rebuild of .build/watch (SHADOW_WATCH_SYMBOLS=ON) under the 10 % cap repeatedly hits
    # `error C1090: PDB API call failed` (0x6 / 0x6BA) in a few recomp_NNN.cpp shards (seen 3 times in a row on
    # 2026-11-07). Re-running the same build (incremental) has succeeded every time. Re-run once; report it if the
    # second run fails too. Root cause not investigated yet.
    # CTest: Release only (Debug pops abort dialogs)
    ctest -C Release -R <pattern> --output-on-failure

Baseline failures at the snapshot (not yours to fix unless in your subsystem; never weaken them): difftest_f1b,
native_liveness_roots, native_file_api_tests (CRI owners missing from the test's runtime list), render_core_tests,
ax_pb_tests (segfault at start-up), lift_sites_reference_tests, options_settings_tests, options_config_tests,
lift_purity, render_determinism, f1c_canary_longrun, native_wait_points, platform_runtime_coupling,
native_host_isa_gate; disc_address_map_GUPE8P is flaky. Report any NEW failure you cause and fix it.

## 4. Gates for every replacement (docs/GC_REMOVAL_PLAN.md "Gates")

1. Measure first: `tools/platform/what_if.py --name <x> --own 0x... --release-dir .build/watch
   --dolrecon-dir .build/dolrecon` (lock `generator`): rows freed, revived, refusals, stale listed sites.
2. Contract test against the original body in the reference interpreter (pinned DOL), every documented output equal,
   with boundary / failure / negative cases and at least one negative control that the test catches.
3. No surviving reader of retired state: `--watch-guest <ranges> --watch-guest-from-tick <tick> --headless` on the
   Westopolis replay. The harness to copy into your own `.build/ab-jobs/` is
   `H:/ShadowTheHedgehogDiskGC_Extract/wt-gc-removal/.build/ab-jobs/run.sh` (read it; set `EXE=` to your exe): it
   runs `--boot-stage stg0100 --input-tape-play <tape>` with copies of the recording's `initial/` card, SRAM,
   display.cfg and pad.cfg and writes audio.pcm, state.txt (`--report-peek` adds guest ranges). Every access attributed.
4. Game behaviour: your baseline exe (build `.build/watch` at the snapshot BEFORE your first change and copy the exe
   to `.build/ab-jobs/base.exe`) vs candidate, alternating, at least 2 runs each, 90 s: PCM hash (the snapshot's
   reference is 015445f2...), game ticks, VI and render frames identical; A/A and B/B memory identical; every differing A/B guest word classified with evidence.
   Never replicate stack or register garbage to make memory match; classify it.
5. Performance: only from alternating repeated runs of the same binary pair (>= 3 each, first discarded), with the
   capped runner's CPU numbers; say "no measurable change" when the spread overlaps. No absolute FPS claims.

Never fake a result, never cut a gate short, never weaken a test. If a gate fails, stop, find the cause, and report it.

## 5. Report

Write `docs/agents/<name>.md` in your worktree and commit it: what you analysed (functions, addresses, names with the
evidence for each name), what you replaced (owners, files), what the generator freed (rows, bodies; measured), the
removed dependencies, each gate's evidence (commands, numbers), performance evidence, open problems, and exactly
which shared list lines you changed. End your session's work with a short summary pointing at that file.

## 6. Assignments (non-overlapping)

| name | scope | primary files |
|---|---|---|
| `axfx-effects` | The three SDK effects the sound start-up (0x8043F9A0) initializes but never registers as aux callbacks: init 0x80388FF8 / shutdown 0x80389064 (context GCAX - 0x1FC), init 0x80389464 / shutdown 0x803896AC (GCAX + 0x138), init 0x80389FF4 / shutdown 0x8038A03C (GCAX + 0x1D4). Identify each (names from evidence), then own them as step 2 owned ReverbHi's. | new `runtime/src/gekko/native_axfx_effects.cpp` / `runtime/include/gekko/native_axfx_effects.h`, a new contract test |
| `card-saves` | Plan step 3: saves.card (6 frontier rows, game save code 0x8029A7D8..0x8029B494 and 0x801E258C) behind a Saves contract; needs a replay that saves and loads (record one only with the user, windowed, lock `game-window`). | `runtime/src/gekko/native_card*`, save store files, new tests |
| `native-boot` | Plan step 4: native boot - `__start` 0x80003154 -> DBInit 0x80370BF0 / InitMetroTRK 0x803A0000 -> OSInit 0x80370E68 -> constructors 0x803796CC -> main 0x800510C0 (os.core, debug.trk, cpu.config). | boot / system start-up code, new tests |
| `cri-threads` | NEXT_SESSION 2b: the CRI thread bridge's upcall becomes a plain native call at the retrace point now that the vsync server never waits for an interrupt; then ADX exec (CRI step 3). Includes the `platform_runtime_coupling` guard uses at `native_cri.cpp` (classify them truthfully, do not game the classifier). | `runtime/src/gekko/native_cri.cpp`, `native_adx_*`, `docs/CRI_SERVERS.md` |
| `vi` (session gc-native-boot, worktree `wt-agent-vi`, branch `agent/vi`) | The graphics.vi service (removal plan row 10: about 6 frontier rows). Inventory what `runtime/src/gekko/native_video.cpp` already owns, measure the rest with what_if, own the remaining entries behind the existing video contract. Must not touch boot (native-boot) or interrupt dispatch beyond VI's own handler. | `runtime/src/gekko/native_video*`, new tests |
| `os-other` (session gc-axfx-effects, worktree `wt-agent-os-other`, branch `agent/os-other`) | The os.other service (removal plan: about 20 frontier rows, 45 bodies). First list its rows and their entries; any row that the boot chain (native-boot), the CRI servers (cri-threads) or VI (vi) enters goes back to the main session instead of being owned here. Own the rest behind native contracts. | new `runtime/src/gekko/native_os_*` files, new tests |
| `os-font` (session gc-sdk-identify, worktree `wt-agent-os-font`, branch `agent/os-font`) | os.font (named by sdk-identify): OSGetFontEncode 0x80375150, OSInitFont, OSGetFontTexture - 3 frontier rows, 10 bodies freed by the graph prediction; the stale sites 0x80375178 / 0x80375DDC / 0x8037600C leave the site lists in the same change. | new `runtime/src/gekko/native_os_font*`, new tests |
| `audio-ax` (session gc-saves-card, worktree `wt-agent-audio-ax`, branch `agent/audio-ax`) | audio.ax core (named by sdk-identify: 44 frontier rows, 104 bodies): AX voice / aux / command list / output and MIX. NOT the AXFX ReverbStd / Chorus / Delay init-shutdown (axfx-effects), NOT anything the CRI ADX path enters (cri-threads: message the main session for any shared row). The 20 statics entered only by `measure_probes.cpp` / test literals need those literals moved in the same change. Large: split into steps, each through all gates. | `runtime/src/gekko/native_ax*`, `runtime/lift/audio/*`, new tests |
| `sdk-identify` (second task) | Analysis: a removal plan for graphics.gx + graphics.rw-gamecube-driver (the two largest services), per frontier row: who enters it, what the native renderer (R2) already serves, what RenderWare reconstruction (docs/RENDERWARE_RECONSTRUCTION.md) must own first; ordered steps with what_if numbers. No runtime change. | `docs/agents/gx-rw-plan.md` |
| `sdk-identify` | The 61 `sdk:unidentified` frontier rows (removal plan): name each from evidence (call graph, strings, SDK signatures, RW 3.7 SDK, Dolphin as reference) with PROVEN / INFERRED / UNKNOWN per name, and the service each belongs to. Analysis only: no runtime change; output a proposed manifest patch file in your worktree (never write `../source/.build`). | `docs/agents/sdk-identify.md`, `test_data/platform/unidentified_names.csv` |
