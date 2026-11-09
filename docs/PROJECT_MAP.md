# Project map (2026-10-30, fresh)

A one-page picture of the whole workspace as it is today. Direction: docs/NORTH_STAR_COMPACT.md (the user's verbatim
docs/NORTH_STAR.md governs). Working rules: AGENTS.md. Live handoffs: docs/NEXT_SESSION.md (top lines point to the
current sessions' handoffs).

## What the project is

Shadow the Hedgehog (GameCube, PAL GUPP8P) as a PC program: a static recompiler translates the game's DOL to C++ at
build time; a runtime replaces console mechanisms with native PC code one subsystem at a time (renderer, audio, files,
timing, memory, math, collision, ...). Most game code still runs translated: it is a **static recompilation, work in
progress, not a native port yet** (public wording rule).

## Workspace on H:

| Path | Size | What |
|---|---:|---|
| `H:\ShadowTheHedgehogDiskGC_Extract\source\` | 24.5 GB (all but ~0.3 GB is `.build/`) | the ONLY maintained source tree |
| `H:\ShadowTheHedgehogDiskGC_Extract\game\` | 4.1 GB | playable folder: `files/` + `sys/` (disc), deployed `ShadowTheHedgehog.exe` (2026-10-20, fa1d49a6...), comparator `ShadowTheHedgehog.reference.exe`, dated test builds `ShadowTheHedgehog.<topic>-<date>.exe`, the user's saves/cards/cfg (never touched by tests) |
| `H:\ShadowTheHedgehogDiskGC_Extract\RW37SDK\` | 0.5 GB | RenderWare 3.7 SDK, research only |
| `H:\ShadowTheHedgehogDiskGC_Extract\EXTRACTED_DISKS\` | 2.6 GB | disc extraction |
| `H:\NukkoroGit\` | 1.4 GB | **public GitHub-ready copy** (no game code/data; Install.cmd builds from the user's own PAL disc; Build.cmd for developers; no tests). Every validated major success is merged there file by file (memory: feedback-deliver-to-nukkorogit). No `.git` inside yet. |

## source/ layout

| Folder | Contents |
|---|---|
| `runtime/src/gekko/` (93 files) | the runtime core: System, CPU / recompiled-code glue, native owners of game functions (frame limiter, file API, CRI device, RtGCNFS, disc-error, SE manager, collision CharaColli, dynamic_cast, matrix multiply, ...), host window, input, audio runtime, options tile, button glyphs, menus |
| `runtime/lift/render/` (45) | native D3D12 renderer (R2): frame graph + texture pool, resources (D3D12MA), renderer.cpp, shaders (mesh, particles, immediate, glare, skinning, utility), GPU timer / memory / present traces |
| `runtime/lift/render_vk/` (39) | Vulkan backend (Android / portability), its own frame graph |
| `runtime/lift/{audio,assets,math,motion,object,peripheral,ui,effects,rwim}` | native audio (AX mixer, reverb), asset pipeline, exact MSL / matrix / scalar math, motion store, object range, pad/card, UI |
| `runtime/scaffold/rw_driver/` (16) | the lowered RenderWare driver feeding R2 (far objects / draw distance, projection) - SCAFFOLDING |
| `runtime/tests/` (105) | C++ regression and oracle tests (CTest, `tests` preset, Release only) |
| `tools/ppcrecomp/` | the static recompiler + its checks (liveness roots, jump tables, GX sites, disc map) |
| `tools/` | launcher, comparator (PySide6), stage_control, present_trace_report, native_sample, memory_map_sampler, gpu_clock_sampler, dolphin_dump, ... |
| `extract/` | function tables, lift-site lists, native-owners.txt, liveness-roots.txt (generated - regenerate, never hand-edit) |
| `test_data/` | fixtures (liveness census, renderer-re, se-manager, core-audit) |
| `docs/` (43 top-level + distant-rendering/, effects/, tools/, boundary-20261007/) | direction, plans, results, subsystem maps |

## Builds (.build/)

| Tree | Use |
|---|---|
| `.build/release` | shipping-config game; `cmake --build --preset release` |
| `.build/diag` | `-DSHADOW_SHIPPING=OFF`: present / GPU / memory traces, verification flags |
| `.build/tests` | CTest preset (`ctest --preset tests`; never Debug - abort dialogs) |
| `.build/android-arm64`, `vk-*` | Vulkan / Android cross builds |
| `.build/recordings/` | input tapes + initial persistence: `westopolis-cpu-spike` (the user's test route, stg0100), `final-haunt` (stg0604), `lethal-highway-sfx`, `user-westopolis-ramp`, `westopolis-replay` |
| `.build/validation/<topic>/` | per-topic evidence (logs, json, csv, reports). Bulk captures of finished work are pruned (2026-10-30: 492 GB runaway audio dump + 31.6 GB of finished captures/exes removed). |

## Status by area (2026-10-30)

| Area | State | Doc |
|---|---|---|
| Renderer | native D3D12 R2, exact WxH at any aspect, 4x MSAA, R16G16B16A16_UNORM scene; 5K (5120x2880) fullscreen is the primary benchmark (user) | RESOLUTION_AUDIT.md, VULKAN_RENDERER.md |
| GPU dips | the remaining displayed repeats are GPU-late smoke frames (one effect fills the screen ~26x, ~11 ms) on a GPU the driver keeps at ~585 MHz; exact zero-alpha skip landed (-8.3 % on those frames) | GPU_FILL.md |
| 5K memory | ~1.41 GiB private at 5K, almost all render targets; render-target aliasing -115 MiB, placed-vs-committed pixel bisection pending | RENDER_TARGET_ALIASING_5K.md |
| CPU | game thread headless p99 ~3.5 ms per frame (not the dip); CharaColli native (-23 %), MSL math native, dynamic_cast native | CPU_FINAL_HAUNT.md, NATIVE_MSL_MATH.md |
| Timing | one time base (exact 1/60 s frames, retrace, audio passes) | FRAME_PACING.md |
| Disc | DVD model eliminated in 5 cuts; cut 6 (delete the 31-owner contract) next | DISC_SYSTEM_MAP.md |
| ARAM | retired, native stores; savestate v24 | ARAM.md |
| Sound | native SE manager (phase 2), reverb heard; phases 3-4 next | GCAX_SOUND_MAP.md, SE_MANAGER_MAP.md |
| Draw distance | far-object stand-ins; default 1 (out of the first release) | DRAW_DISTANCE.md, distant-rendering/ |
| Effects | inventory, layer switches | EFFECTS_INVENTORY.md, effects/ |
| Menus / UI | original 4:3 menus, Options Display tile, PC button glyphs | OPTIONS_DISPLAY_TILE.md, BUTTON_GLYPHS.md |
| Public copy | NukkoroGit: installer + dev build, not yet carrying the 2026-10-29/30 work (36 runtime files differ) | (H:\NukkoroGit\README.md) |

Nothing from 2026-10-26..30 is committed or deployed to game/ShadowTheHedgehog.exe; test builds sit in game/ under
their own names.

## Sessions working in parallel (2026-10-30)

Several Claude sessions edit this one tree: "Performance optimization" (GPU_FILL.md), the 5K memory fork
(RENDER_TARGET_ALIASING_5K.md), far-object proxies, effects scouting, options textures, NukkoroGit packaging. Before
building release/diag or running the game, tell the others (shared trees, shared GPU for timing runs).

## Housekeeping rules

- Bulky test outputs (captures, dumps, exe copies, savestates) are deleted when a topic is finished; text evidence
  stays. Every run passes a budget (`--budget-ms`): the 492 GB dump came from a run that hung without stopping.
- Temp on C: Claude session scratchpads live in %TEMP%\claude; finished sessions' folders are removed.
