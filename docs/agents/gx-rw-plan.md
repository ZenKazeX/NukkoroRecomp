# gx-rw-plan: removing graphics.gx and graphics.rw-gamecube-driver (2026-11-07)

Analysis only (second `sdk-identify` task). No runtime, extract-list or planner input was changed. Files: this report and
`test_data/platform/gx_rw_frontier.csv` (all 160 frontier rows: cluster, name or hint, size, who enters it, up to three
callers or sources). Every number labelled *what_if* is a real generator run (`tools/platform/what_if.py`, lock
`generator`, `capped_run.ps1` at 10 %, 11-45 s wall and about 3 % of the machine each; release tree `.build/watch` of this
worktree, configured with `-DSHADOW_DEPLOY_DIR=`; baseline 751 pruned rows). Numbers from the planner
(`removal_plan.json`, 2026-11-07 10:26, snapshot) are graph predictions and are marked as such. Nothing here was run in the
game: no replay hit-counts exist for these rows yet (see Open problems), so "entered by" means "reachable", not "executed".

## What the two services are

| | graphics.gx | graphics.rw-gamecube-driver |
|---|---|---|
| compiled bodies / frontier rows | 292 / 64 | 383 / 96 |
| owning the frontier frees (planner) | 158 (2 outside the service) | 368 (79 outside the service) |
| entered by | runtime literal 40, RW code 157, game 5, game-unit 2 | data word 35, RW code 226, game 45, runtime literal 22, game-unit 3 |
| distinct entering functions | 39 RW functions, 5 game, 1 game-unit | 125 RW functions, 13 game, 1 game-unit |
| rows whose only entry is... | a runtime literal 25; RW code 22; game 1; game-unit 2 | a data word 23; RW code 45; a literal 5; game 6 |
| literals come from | rw_capture.cpp 30, native_gx_sync.cpp 7, gekko_core_tests.cpp 7 | rw_capture.cpp 21, measure_probes.cpp 1, native_empty_variadic_tests.cpp 1 |

## Findings that change the plan

1. **`rw_capture.cpp` pins 22 frontier rows (30 + 21 literals) although the shipping tree does not compile it.** CMake removes
   `rw_capture.cpp` (and `native_renderer.cpp`, `native_gpu.cpp`, `bloom*.cpp`, `native_immediate.cpp`) from
   `shadow_runtime_common` when `SHADOW_RENDER_LIFTED` is ON (CMakeLists.txt, the block after the common sources). The
   generator's liveness roots are one shared file (`extract/liveness-roots.txt`) built from every `0x8xxxxxxx` literal in all
   runtime sources, so the lifted build keeps alive every GX state setter, every Gcn raster/camera entry and the three
   pipeline return points (PTank 0x8044A1B8, default mesh 0x80463BB4, MatFX 0x804A4228) that only the legacy capture names.
   *what_if* `s1-drop-rw-capture` (drop the roots only that file names; the count is in the table): **17 additional rows freed**
   (11 gx, 6 rw), revives 0. The non-lifted tree (`SHADOW_RENDER_LIFTED=OFF`, the R2 gate's baseline) still needs those
   roots, so the fix is per-build root files, not deleting the literals.
2. **Tests pin almost nothing.** `gekko_core_tests.cpp` names 7 GX rows, but *what_if* `x-drop-tests` (drop the roots only
   `runtime/tests/` names) frees **1** row; with `rw_capture.cpp` as well (`x-drop-capture-and-tests`) **18**. The planner's
   prediction is confirmed: owning all 64 gx rows frees 158 and all 96 driver rows 368, exactly its numbers (`s-gxAll`,
   `s-rwAll`), so the planner can be trusted to size the later steps.
3. **The GX leaf API is called by RenderWare pipeline code, not by the game.** 39 RW functions call the 64 rows: the world
   pipeline callbacks (0x80463964, 0x80463C80), skinning (0x8044EEC4, 0x80449D54), `SubmitNoLightNode` 0x804A0B68, RpMatFX
   (0x804A3F74, 0x804A4358, 0x804A4E00, 0x804A55CC, 0x804A7334, 0x804A7AAC) and the 2D toolkits. R2 already replaces the
   *execution* of these (the six `RxPipelineExecute` sites call `rw_pipeline_execute_*`), but the callbacks stay live through
   node-definition tables and the roots of finding 1; they go only when RenderWare's pipeline objects are native.
4. **35 of the 96 driver rows are data words**: the RwDevice standard-function tables (`GcnSystem` fills them): raster,
   image, texture, camera, render-state and native-texture entries. R2 lowered the draw-facing dispatches (434 render-state
   sets, 133 gets, 50 Im2D, 3 camera, texture LOD, camera-texture copies) but, in its own words, *kept translated* `fpSystem`,
   the raster/texture/image standard functions and the native-texture read (header of `extract/lift-sites-r2.txt`). Those are
   what the RenderWare reconstruction (`rwcore`: raster, image, texture, camera, resources) must provide before the table rows
   can go.

## Clusters (rows in `gx_rw_frontier.csv`)

| cluster | rows | what | entered by | native R2 / runtime already serves | RenderWare must own first |
|---|---:|---|---|---|---|
| **gxA** state API | 26 | GXSetCullMode / BlendMode / ZMode / ZCompLoc / AlphaCompare / Fog / ColorUpdate / AlphaUpdate / DstAlpha, SetViewport(+Jitter) / Scissor(+BoxOffset), SetProjection(v), LoadPosMtxImm / NrmMtxImm / TexMtxImm, SetCurrentMtx, SetNumChans / ChanCtrl / ChanMatColor / ChanAmbColor, LoadLightObjImm, SetNumTexGens, SetTexCoordGen2 | 20 rows: only `rw_capture.cpp` literals; 6 also RW pipeline code (SubmitNoLight, MatFX, skin, world nodes); `GXSetBlendMode` also 4 game effect functions | draw state is kept natively by `scaffold/rw_driver/render_states.cpp` (the lowered `rw_render_state_set/get` sites); the frame, view, matrices and lights reach the renderer as values (`projection.cpp`, `frames.cpp`) | nothing for the 20 literal-only rows (finding 1); the RW-code callers are the pipeline callbacks of finding 3 |
| **gxB** TEV / indirect | 19 | GXSetTevOp / ColorIn / ColorOp / Order / KColor / KColorSel / Color, SetNumTevStages, SetNumIndStages, SetTevIndirect / DirectI, SetIndTexCoordScale / Order | RW code only (matfx 0x804A4E00 and multitexture/matfx 0x804A7334, 0x804A7AAC; skin/world nodes for TevOp, Order, NumTevStages) | combiner program is decided by RenderWare semantics in the scaffold (`material_probe.cpp`, `pipelines.cpp`), not by TEV registers | RpMatFX effects (env map, bump, dual pass, UV transform) and RpMultiTexture natively reconstructed (0x804A1000..0x804AB000, 0 % today) |
| **gxC** vertex / lists / misc | 8 | GXBegin, GXCallDisplayList, GXInvalidateVtxCache, GXClearVtxDesc (signature) and three unnamed vertex-state helpers (0x8039530C, 0x80395790, 0x80395CF4), 0x80396428 | RW code (world/skin nodes, resource-entry destructors) and one tests literal | immediate vertices go through the lowered gx-sites (`vertex`, 1,487 of 1,527 pipe stores) and `rwim`/`rw_im2d_*`; retained geometry is read from RwResEntry data | world-sector and geometry native instance data (0x80465000 block: `GamerCube*` pipelines, `*Native` stream callbacks) |
| **gxD** init / sync / misc | 11 | GXInit (entered by a `native_gx_sync.cpp` literal inside it), GXSetMisc (game boot 0x80051028), 0x803964E8 (signature: GXDrawDone; tests literal), six continuation rows inside native owners (`native_gx_sync.cpp` literals 0x8039A754, 0x8039AE4C..0x8039AE7C: 0x8039A648, 0x8039A700, 0x8039AE44..0x8039AE74), two unnamed functions only the game's `ScreenTest` calls (0x80396EAC, 0x803970E4) | literals of `native_gx_sync.cpp` and tests, game boot, game-unit | `native_gx_sync.cpp` already owns the FIFO, break-points, token/finish interrupts, peeks/pokes (18 owners in `native-owners.txt`) | nothing from RenderWare; the boot sequence (native-boot agent) and the ScreenTest debug code |
| **rwR1** device surface | 19 | Im2D render triangle / line / primitive / indexed (`GcnIm2DRender*`), camera clear / begin / end (`GcnCamera*`), `GcnRasterShowRaster`, render-state get/set table entries, camera-texture copy 0x80496208, 0x804987C8 alpha mode, 0x80499E58 | data words (device table) + `rw_capture.cpp` / `measure_probes.cpp` literals; 0x804960F0/0x804960FC from engine open | lowered: Im2D primitive (50+1 sites), camera begin/end/clear, render-state set/get, alpha mode, camera-texture copy, present (`rw_im2d_*`, `rw_camera_*`, `rw_screen_*`, `render_submit_frame`) | the device registration (`RwEngineOpen` / `_rwDeviceGetHandle` / `RwDevice` standard-function table creation) in `rwcore` (rwcore_lifecycle, camera, frame ported; the GameCube device not) |
| **rwR2** raster / texture / image table | 26 | `GcnRaster{Create,Destroy,Lock,Unlock,LockPalette,UnlockPalette,SubRaster,Clear,ClearRect,SetContext,Render*,GetMipLevels,SetImage}`, `GcnImage*`, `GcnRGBToPixel`, `GcnPixelToRGB`, `GcnTextureSetRaster`, `GcnNativeTexture{GetSize,Write,Read}`, `GcnSystem` | data words only (23); the native-texture read has one lowered LOD site | `scaffold/rw_driver/textures.cpp` (LOD get/set, native-texture-loaded); texture decode in `gx_texture_decode` | rwcore raster/image/texture natives (rwcore_raster is not yet in the test list: images, resources, resample, palquant, texture are) and the PC native-texture reader (docs/renderware-pc-driver/NATIVE_TEXTURE_FORMAT.md) |
| **rwR3** game-entered | 10 | `rtpitexdCreateImageFromRaster` and five siblings (the game's texture-dictionary image export, from 0x8040FB60 / 0x8040FD70), `RwIm3DTransform` 0x8049B87C, `RwIm3DRenderPrimitive` 0x8049BB48, `RwIm3DRenderIndexedPrimitive` 0x8049B9AC (named in `rw_driver.h` and `lift-sites-r2.txt`) and the sibling 0x8049B95C, called by 11 game effect functions and Rt2d | game code | their pipeline executions are lowered (`rw_pipeline_execute_im3d_transform/render`), plus the `rw_trail_*`, `rw_shimmer_draw`, `rw_refraction_draw` scaffolds; the Im3D entry points themselves stay translated because game code calls them directly | `rwim` Im3D / Rt2d reconstructed natively; the game's rtpitexd caller is game code (reconstruct with the game) |
| **rwR4** world-sector pipeline | 22 | `GamerCubeWorldSectorAllInOnePipelineInit`, node definition getter, pipeline node bodies 0x8046521C / 0x80465438, `*GeometryNative` / `*WorldSectorNative` stream callbacks, 0x80466D2C..0x80468C18 (unnamed block: the rpworld 0x80469000 'identify first' range) | RW code (world open, plugin attach, 0x8046BD0C.. render callbacks), one data word | `rw_pipeline_execute_sector` and `rw_geometry_instance_release` lowered; instance data read by the scaffold | rpworld reconstruction: world, world sector, geometry stream and the GameCube instance format; identify the unnamed 0x80466D2C..0x80468C18 functions |
| **rwR5** plugin / node registry | 19 | plugin attach / construct / destruct callbacks (0x8049BCE0..0x8049C198), pipeline node definitions and init (0x8049C3A0..0x8049CE40, `ImmInstance`/`SubmitNoLight` init), `RwPalQuantTerm` 0x80491448 | RW code only (engine plugin registration, `RwEngineOpen`) | none | rwcore plugin registry (ported 99 %), engine init, and the RxPipeline node/cluster core (0 %) |

## Ordered steps, each with *what_if* numbers

"Additional rows freed" are rows beyond the owned ones; every run uses the release tree's exact generator arguments and
leaves `extract/` untouched. Steps 2-10 are cumulative (each includes the earlier ones and the dropped roots); the
second table gives each cluster alone.

| step | run | owned rows | roots dropped | additional rows freed | of which (by service) | generator | listed sites to retire |
|---|---|---:|---:|---:|---|---|---:|
| 1 | `s1-drop-rw-capture` | 0 | 83 | 17 | gx 11, rw 6, other 0 | no | 37 |
| 2 | `c02-cum-gxA` | 26 | 83 | 43 (+26) | gx 37, rw 6, other 0 | no | 156 |
| 3 | `c03-cum-gxB` | 45 | 83 | 43 (+0) | gx 37, rw 6, other 0 | no | 216 |
| 4 | `c04-cum-gxC` | 53 | 83 | 80 (+37) | gx 74, rw 6, other 0 | no | 255 |
| 5 | `c05-cum-gxD` | 64 | 83 | 176 (+96) | gx 168, rw 6, other 2 | refuses: stale site 0x80393FF4 | 571 |
| 6 | `c06-cum-rwR1` | 83 | 83 | 248 (+72) | gx 192, rw 54, other 2 | refuses: stale site 0x80393FF4 | 1352 |
| 7 | `c07-cum-rwR3` | 93 | 83 | 252 (+4) | gx 192, rw 56, other 4 | refuses: stale site 0x80393FF4 | 1356 |
| 8 | `c08-cum-rwR5` | 112 | 83 | 279 (+27) | gx 192, rw 71, other 16 | refuses: stale site 0x80393FF4 | 1356 |
| 9 | `c09-cum-rwR4` | 134 | 83 | 400 (+121) | gx 208, rw 175, other 17 | refuses: stale site 0x80393FF4 | 1365 |
| 10 | `c10-cum-rwR2` | 160 | 83 | 581 (+181) | gx 245, rw 294, other 42 | refuses: stale site 0x80393FF4 | 1447 |

Each cluster alone (no roots dropped):

| cluster | rows | owned alone: additional rows freed | of which | generator |
|---|---:|---:|---|---|
| gxA state API | 26 | 26 | gx 26, rw 0, other 0 | no |
| gxB TEV / indirect | 19 | 0 | gx 0, rw 0, other 0 | no |
| gxC vertex / display list | 8 | 37 | gx 37, rw 0, other 0 | no |
| gxD init / sync / misc | 11 | 95 | gx 93, rw 0, other 2 | refuses: stale site 0x80393FF4 |
| rwR1 device surface | 19 | 71 | gx 19, rw 52, other 0 | refuses |
| rwR2 raster / texture / image table | 26 | 144 | gx 2, rw 117, other 25 | refuses |
| rwR3 game-entered | 10 | 3 | gx 0, rw 1, other 2 | refuses |
| rwR4 world-sector pipeline | 22 | 119 | gx 14, rw 104, other 1 | refuses |
| rwR5 plugin / node registry | 19 | 27 | gx 0, rw 15, other 12 | no |
| all 64 graphics.gx rows | 64 | 158 | gx 156, rw 0, other 2 | refuses: stale site 0x80393FF4 |
| all 96 graphics.rw-gamecube-driver rows | 96 | 368 | gx 39, rw 289, other 40 | refuses |
| `x-drop-tests`: drop the roots only `runtime/tests/` names (no owners) | 0 | 1 | gx 1 | no |
| `x-drop-capture-and-tests`: also the `rw_capture.cpp` roots (no owners) | 0 | 18 | gx 12, rw 6 | no |

Reading the tables: a refusal is the generator rejecting a stale site list *after* deciding liveness, so the freed count is
valid but the step cannot be landed until the listed sites leave `extract/*sites*.txt` (and `liveness-roots.txt`) in the same
change, as in step 2 of docs/GC_REMOVAL_PLAN.md (the reverb owners).

1. **Per-build liveness roots (no new code beyond the roots tool).** Generate `extract/liveness-roots.txt` for the lifted
   build without `rw_capture.cpp` (and the other files CMake removes from `shadow_runtime_common`), keep the current file for
   the non-lifted R2 baseline. Contract: `native_liveness_roots` per variant (it refuses a stale file); `platform_runtime_coupling`
   unchanged. Gate: the lifted `.build/watch` exe PCM/tick/frame identity vs the pre-step exe (plan gate 4), because the pruned
   rows must be unreachable. Frees: row 1 of the table.
2. **Tests stop pinning shipping code** (small: 1 row, `x-drop-tests`). Tests that never enter generated code declare
   `interpreter-only` (the tool verifies they do not include `gekko/recomp.h`); `gekko_core_tests.cpp` calls GX rows, so it
   keeps its literals in the tests tree's roots only.
3. **gxA, the GX state API as native owners** writing into the scaffold's state (what the removed observers did). Per row:
   a contract test against the original body in the reference interpreter (every documented output: the `__GXData` shadow
   fields, the BP/XF payload the function would emit, return value), boundary values, one negative control; then the
   retired-reader gate: `retired_readers.py` / `--watch-guest` over the `__GXData` fields the bodies wrote, on the Westopolis
   replay. Expected tests: `gx_state_contract_tests` (new), `native_gx_sites_reference_tests` (existing, stale-site count
   shrinks), `lift_sites_reference_tests`. Prerequisite: none from RenderWare; the 6 RW-code callers keep working because the
   owner has the same ABI.
4. **gxB, TEV and indirect state.** Same shape as 3; only RpMatFX / multitexture / skin / world nodes call them, so the
   owner is a no-op sink unless the combiner is derived from the TEV registers (it is not: `material_probe.cpp`).
   Expected test: `gx_tev_contract_tests`. Measured effect small until gxC (see table).
5. **gxC, vertex path.** `GXBegin` and the vertex-format setters: tie to the immediate builder; test = the gx-sites
   `vertex` family contract already in `native_gx_sites_reference_tests` plus `rw_geometry_instance_contract_tests` (new) for
   `GXCallDisplayList`. Needs the retained-geometry reader in `scaffold/rw_driver/pipelines.cpp` to cover display lists.
6. **gxD, init / sync / misc.** `GXInit` and `GXSetMisc` belong to native boot (the `native-boot` agent: `__start` ... main);
   `GXDrawDone` and the continuation rows go with `native_gx_sync.cpp` (`native_gx_sync_reference_tests`). The ScreenTest
   rows are debug code of the game: own them or leave them until the game side is reconstructed.
7. **rwR1, the device surface.** Needs the RwDevice registration native (rwcore: engine open, `_rwDeviceGetHandle`); then the
   Im2D, camera and render-state table rows become unreferenced. Tests: `rwcore_lifecycle_tests`, `rwcore_camera_native_tests`,
   `camera_tests`, `rwim_tests`, and a new `rw_device_table_contract_tests` (each table slot of the original `GcnSystem`
   compared with the reconstruction's registration).
8. **rwR3 + rwR5, game-entered helpers and the plugin / node registry.** rwR3 follows `rwim` (Im3D / Rt2d); rwR5 follows the
   RxPipeline core and plugin attach; tests `rwcore_plugin_tests`, `rwcore_pluginstream_tests` plus new node-definition tests.
9. **rwR4, the world-sector pipeline** with the rpworld geometry/sector reconstruction and the unnamed 0x80466D2C block.
10. **rwR2, raster / texture / image table** last: it needs the rwcore raster natives and the PC native-texture reader.
    Tests `rwcore_texture_native_tests`, `rwcore_image_native_tests`, `render_core_tests`, and the texture corpus check of
    NATIVE_TEXTURE_FORMAT.md (148 textures, 509 levels).

Each step also needs the plan's gates 3-5 (retired readers, A/B game behaviour on the Westopolis replay, performance), and
removes its rows from `extract/gx-sites.txt` / `lift-sites*.txt` / `platform-sites.txt` in the same change.

## Open problems

* Execution counts now exist (step 0 below) but only for the Westopolis replay: 0 hits there is not proof of never.
* The planner's prediction and *what_if* agree by construction on liveness, not on behaviour; the retired-reader and A/B gates
  remain the proof.
* Names of the unnamed GX rows rest on the Dolphin signature table (`functions-dolphin-sigs.csv`, a shape matcher) and the
  project's own `rw_capture.cpp` constants; the same-shape collision warning of docs/agents/sdk-identify.md applies
  (e.g. `GXSetNumTexGens` appears for 0x80397C80, which `rw_capture.cpp` calls `GXSetNumChans`).
* `rwcore` has no raster/Im2D/GameCube-device module yet, and 0x80466D2C..0x80468C18 (rpworld's unnamed block) is unidentified.

## Step 0 - replay hit counts (measured 2026-11-07)

What ran: the existing entry probes (`--measure r5probes`, `extra=` addresses; observe-only, `runtime/src/gekko/measure_probes.cpp`)
on the Westopolis replay, headless, 90 game seconds (`--boot-stage stg0100 --input-tape-play input.tape`, the recording's
`initial/` card, SRAM, display.cfg, pad.cfg), exe = the main session's lifted exe built at the 99fe8c3 source
(sha256 dd615d79b3d6684e...), no runtime change. The probe takes 32 addresses per run, so the 160 rows ran as 5 batches plus one
extra batch for the interior rows, each through `tools/agents/capped_run.ps1` at 10 % (about 12 s of CPU each, 3 % of the
machine). Every run reached the same end state (3,645,000,000 ticks, 2,697 VI frames); the control `RasterShowRaster` (0x80495CBC)
counted 5,360 = the render frame count; batch 0 repeated gave identical hit counts, first and last frame. Per-row result:
`hits`, `first_vi`, `last_vi`, `vi_frames`, `top_callers`, `census_note` in `test_data/platform/gx_rw_frontier.csv`.

| | rows | executed | executed only during start-up (last VI <= 14) | executed past VI 14 |
|---|---:|---:|---:|---:|
| graphics.gx | 64 | 41 | 39 | 2 |
| graphics.rw-gamecube-driver | 96 | 38 | 23 | 15 |
| all | 160 | 79 | 62 | 17 |

Per cluster (executed / rows): gxA 23/26, gxB 9/19, gxC 4/8, gxD 5/11, rwR1 2/19, rwR2 11/26, rwR3 4/10, rwR4 4/22, rwR5 17/19.

What this changes in the plan:

* "Reachable" is not "executed", and the difference is large: 81 of the 160 rows have 0 hits in this replay (10 of 19 gxB rows,
  17 of 19 rwR1 rows, 18 of 22 rwR4 rows, ...).
* The GX state API (gxA, gxB) is not a per-frame cost: every executed row there runs within the first 14 VI frames (1-16 calls each)
  and never again, called from the RenderWare pipeline-setup callbacks (0x80394xxx) and the init path. The only steady-state GX rows
  are two vertex rows: 0x80396428 (5,360 hits, once per render frame, from 0x80495E08, the show-raster path) and 0x80395D38
  GXInvalidateVtxCache (29 hits, from `readWorldSectorNative`, until VI 2285).
* The hot driver rows number about a dozen: `GcnSystem` 0x80494EDC (11,135), `GcnRasterSubRaster` 0x804981B8 (12,761), 0x8049B7DC (5,453),
  `RasterShowRaster` 0x80495CBC (5,360), the raster life cycle (RasterCreate and RasterUnlockPaletteEnd 371 each, RasterDestroy 79,
  GcnNativeTextureRead 348), and the game-entered helpers 0x8049B87C / 0x8049B95C / 0x8049B9AC (about 300,000 each, from 0x804722xx and
  the game units), 0x8049BB48 (22,501) and 0x8049C198 (176,459, from 0x804863F0). These are what a native owner has to be fast
  at; everything else is a start-up one-shot.
* Seven rows have no translated entry (interior labels of larger translated functions): 0x8039A700 (inside 0x8039A648),
  0x80499F10 (inside 0x80499E58) and 0x8049A304 / 0x8049A364 / 0x8049A3B4 / 0x8049A41C / 0x8049A480 (inside 0x8049A124). The planner
  counts them as rows, the generator does not emit them as functions. Their `hits` are the enclosing function's entry count
  (1 for 0x8039A648, 0 for the other two), so an owner for them is the enclosing function's owner.
* Limit: one stage and no menus, movies or cutscenes. A row with 0 hits here is "not executed in this replay", not "never executed":
  before an owner replaces or drops it, its contract test, not this census, is the evidence.

## Step 1 - per-build liveness roots (measured 2026-11-07)

Problem (section "Findings"): `extract/liveness-roots.txt` is generated from every `0x8xxxxxxx` literal in `runtime/`; the lifted
(shipping) tree does not compile `rw_capture.cpp` (nor the other eight legacy-renderer sources CMake removes), yet their literals
rooted guest bodies in it.

What changed:

* `tools/ppcrecomp/liveness_roots.py`: `--without <runtime source>` (repeatable) leaves that file's literals out and names the
  omitted files in the output header; a path that does not exist is an error (a renamed source cannot silently root again);
  `--base <roots file>` lists only roots that file also lists. `tools/test_liveness_roots.py`: four new tests (a shared literal stays,
  header and `--check` refuse a different list, missing file refused, `--base` against a stale full file); 14 tests pass.
* `CMakeLists.txt`: one list `SHADOW_LIFTED_UNCOMPILED_SOURCES` feeds both the `list(REMOVE_ITEM ...)` of the lifted tree and the
  roots choice: `SHADOW_RENDER_LIFTED` uses the new `extract/liveness-roots-lifted.txt`; every other tree (the R2 baseline, the tests
  preset) keeps `extract/liveness-roots.txt` and so still roots on `rw_capture.cpp`. New test `native_liveness_roots_lifted`
  (full scan minus the list, restricted to the roots the full file lists) refuses a lifted file that does not match the list.
* `extract/liveness-roots-lifted.txt`: 1,513 roots = the full file minus 83. All nine omitted sources together own exactly those
  83 exclusive roots (rw_capture.cpp alone owns the same 83; the other eight add none).

Why `--base`: regenerating the full `extract/liveness-roots.txt` as `native_liveness_roots` demands is currently not possible. The
file is 453 roots behind the sources (225 from `native_one.cpp`, 47 `native_sj.cpp`, 38 `native_expand_jobs.cpp`, 36 `system.cpp`, ...),
and a what_if run adding all 453 is refused by the generator ("0x803CF05C branches into the interior of a native owner at
0x803CF24C"). That is the baseline `native_liveness_roots` failure the coordination doc lists; this change neither causes nor fixes
it. The lifted file is derived from the committed full file, so it neither hides nor inherits that staleness; when the full file is
regenerated the lifted test fails until its file is regenerated too. Follow-up for whoever takes that failure: find which of the 453
addresses are values, not entries, and mark them `// liveness-roots: value-only`.

Measured result (real builds, not predictions): `recomp_pruned.csv` of this change's lifted build against the main session's lifted
tree (`wt-gc-removal/.build/watch/generated`):

* newly pruned (not emitted): 17 rows. graphics.gx 11: 0x80396568, 0x80396C50, 0x80396D00, 0x803975AC, 0x80397D6C, 0x80397D8C,
  0x80397D98, 0x80397DA4, 0x80397DB0, 0x8039868C, 0x8039A0B4. graphics.rw-gamecube-driver 6: 0x80496208, 0x804962E4, 0x8049630C,
  0x80496330, 0x80496358, 0x80496380.
* revived by this change: 0. (That main tree was built at a later head and prunes 5 rows this build does not, 0x80380170, 0x80380178,
  0x80380180, 0x803A225C, 0x803A2280: the VI owners merged after 99fe8c3, not a difference of this change.)

That is exactly the 17 rows (11 GX, 6 RW) the plan predicted. The shipping ratchet (963) can drop by 17 once the main session
measures it; `SHADOW_GC_REMOVAL_RATCHET` was not changed.

Gates (section 4 of docs/AGENT_COORDINATION.md; every run through `capped_run.ps1` at 10 %):

1. Both trees build: `.build/watch` (lifted; the first attempt hit a transient compiler `C1090` PDB error in two shards, the
   incremental rerun finished and produced the exe) and `.build/tests` (preset `tests`, not lifted, all targets).
2. CTests in the tests tree: `native_liveness_roots_lifted` PASS (new), `platform_gc_removal_plan` PASS, `platform_runtime_coupling`
   PASS, `native_liveness_roots` FAIL exactly as at baseline (stale by 453, see above). Full suite: 195 tests, 179 passed, 16 failed. Thirteen are on the coordination doc's baseline list (render_core_tests, options_settings_tests, options_config_tests, native_file_api_tests, ax_pb_tests, lift_sites_reference_tests, difftest_f1b, f1c_canary_longrun, lift_purity, render_determinism, native_liveness_roots, native_wait_points, native_host_isa_gate; platform_runtime_coupling passes). Not on that list: `shipping_platform_selection` (the tests tree does not build the `ShadowTheHedgehog` exe the test launches: a missing file, not roots), `native_msl_tests` (timed out at 600 s under the 10 % cap with -j2; passes alone with a 3000 s limit) and `dolrecon_lift_tests` (timed out at 600 s and again at 3000 s under the cap; it lifts every function, reads neither roots file, and I did not run it on the unchanged tree, so whether it passes at baseline is not established).
3. Westopolis A/B, headless, 90 game seconds, alternating baseline / candidate twice (baseline = the main session's lifted exe at
   the 99fe8c3 source, candidate = this change's lifted exe, `SHADOW_NATIVE_JOBS` unset): PCM hash 015445f28cd10698... in all four
   runs (the snapshot's reference 015445f2...), 3,645,000,000 ticks, 2,697 VI frames and 5,360 render frames in all four. Guest
   memory (every 64 KiB block of every region): B/B 0 differing blocks, C/C 0, B/C 0 (all pairings). No differing guest word to
   classify.
4. Performance: not claimed (the change removes unreachable code from the image; no repeated-pair timing run was made).

Not covered: windowed gameplay, audio device output and GPU output (headless runs); stages other than Westopolis; the non-lifted
tree was built and its tests ran, but its game was not replayed (its roots file is unchanged).

Files changed: `CMakeLists.txt` (list, roots choice, test), `tools/ppcrecomp/liveness_roots.py`, `tools/test_liveness_roots.py`,
`extract/liveness-roots-lifted.txt` (new), `test_data/platform/gx_rw_frontier.csv` (census columns), this doc. No shared list line
was changed.

## Step 1b - the stale liveness roots, made honest (measured 2026-11-07)

Task: make `native_liveness_roots` pass without loosening any check. Merged gc-removal at 987321f first (its `CMakeLists.txt` and the two
roots lists are CRLF; the conflict was only line endings, resolved by taking its file and re-applying the three CMake edits as CRLF; both
roots lists stay CRLF).

**(a) What is the branch at 0x803CF05C?** `fn_803cef10` (0x803CEF10..0x803CF260, 848 bytes) is the native owner "ADX output: one
handle" (`extract/native-owners.txt`, `runtime/src/gekko/native_adx_output.cpp`). Its original body contains `0x803CF05C: beq 0x803cf24c`
(the loop exit of the chunk loop) and `0x803CF070: b 0x803cf070` (the original's hang on unequal chunk lengths). The generator refused
because a *root inside that body* made a live row out of the owner's old code, and that row's `beq` entered the interior of an
owner. The roots in question were runtime literals that are not entries at all:

* `runtime/src/gekko/native_adx_output.cpp:81` and `:178`: the text of two error messages, "the original hangs at 0x803CF494" and
  "...0x803CF070". String literals count as code to the scanner. Value, never entered.
* `runtime/tests/lift_sites_reference_tests.cpp:59`: `cpu.find_lift_site(0x803CF0D0u)` / `(0x803CF21Cu)`, lookups of table entries.
  Value, never entered.

Fix in the right place: each of the four literals now sits alone on its line with `// liveness-roots: value-only` (the scanner's own
declared mechanism; the test splits its one `check` into two named `const bool`s). The generator, the scanner and the check
are unchanged. No owner entry was missing: the five other roots inside owner bodies (0x80372524, 0x803725B0, 0x80376224, 0x803CA9F4,
0x803CC0D0) are declared `entry:` lines of `native-owners.txt` with runtime thunks, and the generator accepts them.

**(b) The 453.** `test_data/platform/liveness_roots_missing.csv` lists each address with its placement (function start / interior of
a function / inside an owner / between functions / data), the shape of its use in the source, the files naming it and a verdict. The
verdicts are structural (placement in `functions.csv` and `native-owners.txt`, the shape of the source line), not a line-by-line reading
of 453 call sites; the generator's acceptance is the arbiter:

| verdict | rows |
|---|---:|
| data address (>= the data sections, tables, test sentinels): roots nothing | 108 |
| guest function the runtime hooks or tables (real entry) | 98 |
| guest continuation / call-site address the runtime enters or resumes at (return-address constants, resume sites) | 62 |
| owner start: the thunk the runtime dispatches through | 62 |
| guest code address named by the runtime, no more specific shape (call-site / hook sites inside functions) | 114 |
| declared owner entry (`entry:` in native-owners.txt) | 5 |
| value-only (marked now; inside the ADX output owner's body) | 4 |

Result: 449 real roots are added, 4 are marked value-only. `what_if` (`--root` all 449, `--release-dir .build/watch`, lock `generator`):
no generator refusal, 0 rows freed, 7 revived: 0x803CAAA4 (SJMEM_Error), 0x803CAF08, 0x803CB178, 0x803CB378 (SJRBF Unget/Put/GetChunk),
0x803CBC44 (SJRBF_Error), 0x803CBF98 (SJ error report), 0x803CF260 (ADX output: silence transfer completion). All seven are native
owner starts: rooting them emits the thunk the runtime dispatches through, no SDK body. They were absent only because the list was stale.

Regenerated with `liveness_roots.py` (output converted to CRLF, nothing else): `extract/liveness-roots.txt` 2,056 roots (+449, none removed),
`extract/liveness-roots-lifted.txt` 1,973 (= the full file minus the 83 `rw_capture.cpp` roots).

Gates:

1. Both trees build with the new lists (`.build/watch` lifted exe, `.build/tests` all targets; capped runner at 10 %).
2. Tests tree: `native_liveness_roots` PASS (was the baseline failure), `native_liveness_roots_lifted` PASS, `platform_gc_removal_plan` PASS,
   `platform_runtime_coupling` PASS, `tools/test_liveness_roots.py` 14 pass. Not mine and unchanged: `native_wait_points` (stale list) and
   `lift_sites_reference_tests` still fail (its "four stream-ring sites are generated" assertion is false because those sites sit inside
   the ADX owner since 2026-11-03, `extract/lift-sites-native.txt`; the second failure line is the site-count total).
3. Westopolis A/B, headless, 90 game seconds, alternating twice: baseline = the same branch built with the previous lists, candidate =
   with the new lists, `SHADOW_NATIVE_JOBS` unset: PCM hash 015445f28cd10698... in all four runs, 3,645,000,000 ticks, 2,697 VI frames,
   5,360 render frames in all four; guest memory 0 differing 64 KiB blocks in every pairing (baseline/baseline, candidate/candidate,
   baseline/candidate). The two executables differ (sha256 6383eabc... vs 732f5033...), so the runs compared two real builds.
4. Performance: not claimed.

Not covered: windowed gameplay, audio device output, GPU output; other stages. The earlier `dolrecon_lift_tests` timeout is the known one
(docs/GC_REMOVAL_PLAN.md "dolrecon_lift_tests timeout"), unchanged here.

Files changed: `extract/liveness-roots.txt`, `extract/liveness-roots-lifted.txt` (both regenerated, CRLF), `runtime/src/gekko/native_adx_output.cpp`
(2 lines, comment markers), `runtime/tests/lift_sites_reference_tests.cpp` (one check split into two named bools),
`test_data/platform/liveness_roots_missing.csv` (new), this doc. `CMakeLists.txt` is the step-1 change re-applied on gc-removal's CRLF file.

## Queue item 2 - the identified names applied to the manifest (measured 2026-11-07)

What exists now (commit "dolrecon: identified SDK names applied in a private manifest"):

* `tools/dolrecon/platform_deps.py`: the 29 exact-name `LIBRARY` rules of `test_data/platform/unidentified_names.platform_deps.patch`
  (statics whose names carry no library prefix, e.g. `DMAErrorHandler`, `GetFontCode`, `__ReverbSTDCreate`, `SamplingHandler`; and
  `SPEC2_MakeStatus`, which the audio `SP` rule misread). `tools/test_dolrecon_evidence.py` and `test_dolrecon_callgraph.py`: 105 tests pass.
* `tools/platform/regen_manifest.py`: the command. It builds a private `<worktree>/.build/dolrecon` from the shared
  `../source/.build/dolrecon` (read only; copies `pal-v30/{config,out/asm}`, `main.dol`, `names.json`, `callgraph.json`, `receivers.json`
  and `skeleton/`), applies `unidentified_names.symbols.patch` (162 renames) to the copied `symbols.txt`, and runs
  `platform_deps` with `extract/native-owners*.txt`. Run it again to rebuild from scratch; `--no-names` builds the control.

```
python -B tools/platform/regen_manifest.py
cmake -S . -B .build/watch -DSHADOW_DOLRECON_DIR=<worktree>/.build/dolrecon      # same for .build/tests
```

(`SHADOW_DOLRECON_DIR` already defaults to `<worktree>/.build/dolrecon`; the trees here had been configured against the shared copy.)

Two effects, kept apart:

1. Regenerating by itself (control, no renames): the shared manifest predates later changes to `platform_deps.py`, so merely
   regenerating moves entry counts: `engine.renderware` 820 -> 1,094, `host.compiler-runtime` 37 -> 49, `middleware.cri` 1,180 ->
   1,178, `host.c-library` 116 -> 118, `debug.trk` 186 -> 185, `sdk:unidentified` 211 -> 209. Not caused by the names; whoever adopts
   this should know the renderware count changes.
2. The names: `sdk:unidentified` entries 209 -> 9 (`audio.ax` 6 -> 99, `saves.card` 45 -> 84, `input.pad` 1 -> 14, `audio.ai` 14 -> 19,
   `memory.aram` 0 -> 10, `memory.aram-queue` 0 -> 5, `os.font` 0 -> 10, `graphics.vi` 17 -> 23, `bus.exi` 18 -> 22, `input.si` 21 -> 24,
   `files.dvd` 92 -> 94, `os.debug` 1 -> 4, `audio.dsp` 14 -> 15, and a few single rows).

The removal plan with real service names (same live graph of the lifted tree, `removal_plan.py --release-dir .build/watch`, once with the
shared manifest, once with the private one; platform live bodies 912 -> 911, frontier rows 229 -> 229, so the ratchet is untouched):

| service | live bodies | frontier rows | freed if frontier owned |
|---|---:|---:|---:|
| sdk:unidentified | 103 -> 1 | 43 -> 0 | 58 -> 0 |
| audio.ax | 0 -> 86 | 0 -> 38 | 0 -> 51 |
| os.font | 0 -> 10 | 0 -> 3 | 0 -> 7 |
| os.interrupts | 11 -> 12 | 0 | 0 |
| graphics.vi | 1 -> 2 | 1 -> 1 | 0 |
| os.debug | 1 -> 2 | 1 -> 2 | 0 |
| audio.ai, input.pad | 0 -> 1 each | 0 / 1 | 0 |

(The shared manifest's `saves.card` and the other services above are unchanged in the lifted tree: the CARD library is no longer compiled.)
Gates: tests tree reconfigured against the private directory; `platform_gc_removal_plan`, `platform_runtime_coupling`,
`native_liveness_roots`, `native_liveness_roots_lifted` pass. Not done: no game run (analysis tooling only); the planner's
`--control-own` positive control ran inside the ctest. The skeleton (`classes/`, `functions.csv`) was copied, not regenerated; layers
do not depend on SDK names.

## Queue item 3 - rwR2 inventory: what `rwcore` and the native-texture reader must provide (2026-11-07)

Scope: the 26 `graphics.rw-gamecube-driver` rows of cluster rwR2 (`test_data/platform/gx_rw_frontier.csv`, hit counts from the
Westopolis census above) that axfx-effects will implement later. Analysis only; nothing here was built or run beyond the
census and read-only disassembly (`tools/doldis.py`).

### How the rows are entered, and the slot map

All but two of the 26 rows are function pointers in the GameCube driver's standard-function registration table (guest
`0x8051D400`, 29 pairs of {slot id, function}; two pairs are empty, slots 0 and 22 are never registered), which `RwEngineInit` copies into
`RwEngineInstance` (`rwcore/include/rw/core/engine.h`: `rwSTANDARDNUMOFSTANDARD` = 29; the PC D3D9 driver registers slots 25-28 the
same way, `docs/renderware-pc-driver/NATIVE_TEXTURE_FORMAT.md` line 40). The slot numbers below are read from that table, not guessed:
1 camera begin, 2 RGBTOPIXEL, 3 PIXELTORGB, 4 RASTERCREATE, 5 RASTERDESTROY, 6 IMAGEGETRASTER, 7 RASTERSETIMAGE, 8 TEXTURESETRASTER,
9 IMAGEFINDRASTERFORMAT, 10 camera end, 11 SETRASTERCONTEXT, 12 RASTERSUBRASTER, 13 RASTERCLEARRECT, 14 RASTERCLEAR, 15 RASTERLOCK,
16 RASTERUNLOCK, 17 RASTERRENDER, 18 RASTERRENDERSCALED, 19 RASTERRENDERFAST, 20 RASTERSHOWRASTER (rwR1), 21 CAMERACLEAR (rwR1),
22 empty, 23 RASTERLOCKPALETTE, 24 RASTERUNLOCKPALETTE, 25 NATIVETEXTUREGETSIZE, 26 NATIVETEXTUREREAD, 27 NATIVETEXTUREWRITE,
28 RASTERGETMIPLEVELS. Slots 4, 5, 8, 15, 16, 26, 28 agree with the constants in `rwcore/src/raster.cpp`; 23 is identified from its body
(`0x804972B8` takes (out, raster, flags), tests the raster type, sets private flags 0x08 / 0x10 from the lock flags and returns
`raster->palette` through `out`).

Two entries are corrections to names used elsewhere: `0x804972B8` is RASTERLOCKPALETTE (the frontier CSV and `rw_capture.cpp` call it
UnlockEnd), and `0x8049A508` is RASTERCLEARRECT (Dolphin's `SidelineExplodableManager:` is a shape collision). The other two rows
are not slots: `0x80497458` (a helper of RasterCreate) and `0x8049B7DC` (a draw-sync fence check).

### Row by row

`hits` = entries on the Westopolis replay (90 game seconds); `last VI` = the last video frame in which it ran.

| address | name (manifest / hint) | slot | RenderWare role | hits (last VI) | rwcore host device today | what has to be provided |
|---|---|---|---|---:|---|---|
| 0x80497CA8 | GX/RasterCreate | 4 | RASTERCREATE | 371 (1626) | host: 8888/888/default only, CPU pixels, no palettes, no DXT | every format the game creates (the GX formats it reads: 8888, 565, 5A3, I4/I8/IA4/IA8/CI4/CI8 via the raster format word), palette rasters, mip chain, camera / Z / camera-texture types without CPU storage, DONTALLOCATE |
| 0x80498058 | GX/RasterDestroy | 5 | RASTERDESTROY | 79 (1751) | host: frees CPU pixels | release of the renderer texture the raster owns (R2 scaffold: `release(handle)`), palette block, the fence wait (row 0x8049B7DC) before freeing |
| 0x8049819C | GcnTextureSetRaster | 8 | TEXTURESETRASTER | 26 (1751) | host: sets the raster, nothing else | texture-to-raster binding that keeps the raster's renderer handle reachable from `texture->raster->parent` (the lookup `texture_of` uses) |
| 0x804981B8 | GcnRasterSubRaster | 12 | RASTERSUBRASTER | 12761 (2697) | not supported | parent/offset/size sub-raster: shares the parent's pixels and renderer texture, offset into it. HOT (12,761 hits, every frame) |
| 0x80496CA4 | GcnRasterLock | 15 | RASTERLOCK | 3 (14) | host: CPU lock with RenderWare semantics | lock for read / write on a raster whose texels the renderer owns (copy-in/copy-out or mapped upload), mip level, stride; wait on the fence before a write |
| 0x80497104 | GX/RasterUnlock | 16 | RASTERUNLOCK | 3 (14) | host: CPU unlock | unlock that re-uploads the changed level to the renderer texture (what 0x80497458 does for palette rasters today) |
| 0x804966D8 | GcnRasterGetMipLevels | 28 | RASTERGETMIPLEVELS | 0 | host: derives from the format word | the same, for the mip counts the GX format stores (`LevelsMinusOne` 0xFF = derive) |
| 0x804972B8 | GX/RasterUnlockEnd | 23 | RASTERLOCKPALETTE (see names) | 0 | not supported | palette lock: returns the palette pointer for read/write intent (the original sets raster private flags 0x08/0x10 and returns raster->palette); palette rasters only, otherwise the E_RW error |
| 0x80497388 | GX/RasterUnlockPalette | 24 | RASTERUNLOCKPALETTE | 0 | not supported | palette unlock: re-uploads the palette to the renderer texture |
| 0x80497458 | GX/RasterUnlockPaletteEnd | - | helper of RasterCreate (not a slot) | 371 (1626) | - | the palette/CLUT finalisation RasterCreate calls (371 hits = RasterCreate's 371). Folds into the Create owner; no separate entry |
| 0x8049A57C | GcnNativeTextureGetSize | 25 | NATIVETEXTUREGETSIZE | 0 | not supported | size of the native chunk a texture would write (needed only by the writer; 0 hits) |
| 0x8049A604 | GcnNativeTextureWrite | 27 | NATIVETEXTUREWRITE | 0 | not supported | write of the native chunk (the game never writes a dictionary: 0 hits); needed for round-trip tests of the reader |
| 0x8049A8DC | GcnNativeTextureRead | 26 | NATIVETEXTUREREAD | 348 (1626) | host: reads only the converter's invented 'HOST' chunk (to be retired) | the GameCube native chunk the disc streams hold, byte for byte (layout below), producing RwRaster + RwTexture + the renderer texture. 348 reads in the replay |
| 0x80492BFC | GcnRGBToPixel | 2 | RGBTOPIXEL | 0 | not supported | colour to a pixel value of each raster format (pure function; tests against the interpreter per format) |
| 0x80492F10 | GcnPixelToRGB | 3 | PIXELTORGB | 0 | not supported | the inverse; the same tests |
| 0x80494614 | GcnRasterSetImage | 7 | RASTERSETIMAGE | 1 (14) | not supported | image to raster conversion per format (runs once, VI 14, from RwRasterSetFromImage 0x8048AA9C) |
| 0x80493A40 | GcnImageGetRaster | 6 | IMAGEGETRASTER | 0 | not supported | raster to image conversion per format |
| 0x804948D8 | GcnImageFindRasterFormat | 9 | IMAGEFINDRASTERFORMAT | 0 | not supported | the raster format an image needs (pure) |
| 0x8049A570 | GcnRasterSetContext | 11 | SETRASTERCONTEXT | 0 | not supported | 12-byte setter of the device's current raster; no hits |
| 0x8049A508 | SidelineExplodableManager::GetNumExplodables(void) | 13 | RASTERCLEARRECT | 0 | not supported | clear a rectangle (Dolphin's signature name 'SidelineExplodableManager:' is a same-shape collision); no hits |
| 0x8049A52C | GcnRasterClear | 14 | RASTERCLEAR | 0 | not supported | clear the raster; no hits |
| 0x80499A9C | GcnRasterRender | 17 | RASTERRENDER | 0 | not supported | 2D blit of a raster at x,y; no hits (the game draws through Im2D) |
| 0x80499AEC | GcnRasterRenderScaled | 18 | RASTERRENDERSCALED | 0 | not supported | scaled blit; no hits |
| 0x80499AC4 | GcnRasterRenderFast | 19 | RASTERRENDERFAST | 0 | not supported | fast blit; no hits |
| 0x80494EDC | GcnSystem | (device system) | GcnSystem, the device's system function (data word 0x8056F510) | 11135 (2697) | host: `hostDeviceSystem` | the device request switch: open/close/start/stop, standard-function export, video modes, fog/zbuffer/texture formats, camera-texture support. HOT (11,135 hits). Overlaps rwR1 |
| 0x8049B7DC | fn_8049B7DC | - | GX draw-sync token check (helper) | 5453 (2697) | - | reads the GX draw-sync token (0x803964DC), compares two 16-bit counters (r13+0x6220 current, r13+0x2AF0 last waited) against the 0xE000 wrap: 'has the GPU finished with this data'. Becomes a renderer fence query (frame index / fence value). 5,453 calls, 5,357 from 0x80494B4C |

### The GameCube native texture chunk (row 0x8049A8DC), read from the original body

What `NATIVETEXTUREREAD` does, instruction by instruction (`0x8049A8DC..0x8049AB70`), so a native reader can be written and tested
against it in the reference interpreter:

1. `RwStreamFindChunk(stream, 1 /*struct*/, &length, &version)` (0x8047DFA0); the version must lie in `[0x35000, 0x37002]`.
2. Read `0x58` bytes: `+0x00` platform id, must be `6`; `+0x04` filter and addressing word; `+0x08..0x10` three LOD words;
   `+0x14` a float (the LOD bias); `+0x18` texture name (32 bytes); `+0x38` mask name (32 bytes).
3. Read `0x10` bytes: `+0x00` raster format word (the original ORs in `0x80`, rwRASTERDONTALLOCATE); `+0x04` u16 width; `+0x06` u16 height;
   `+0x08` u8 depth; `+0x09` a u8 handed to the level-storage allocator (0x80497A70; by its use the mip level count, not verified); `+0x0A` u8 GX texture format; `+0x0B` u8 TLUT format; `+0x0C` u32 has-alpha flag.
   `RwRasterCreate(width, height, depth, format | 0x80)` (0x8048AEC4); the GC extension (at the raster plugin offset, `0x805F2700` word)
   gets GX format (+0x0C), TLUT format (+0x10), has-alpha (+0x14 = flag != 0).
4. Allocate the level storage for the mip count (0x80497A70); a failure destroys the raster.
5. When the raster format is paletted (format bits 0x2000 / 0x4000): read `2 << depth` bytes of 16-bit TLUT entries into the extension's
   palette (+0x20) and flush the data cache range.
6. Read a u32 byte count, then that many GX-tiled texel bytes (every level) into the extension's pixels (+0x1C), flush, then a GX call (0x803987B8, a texture-cache invalidate by its place in the sequence). The byte count is checked against what the stream returned.
7. texture creation from the raster (0x8048DD2C, RwTextureCreate by its call shape); NULL destroys the raster. Filter and addressing are copied into texture `+0x50` in three
   masked stores, then the name and mask-name setters (0x8048DEA4, 0x8048DF50), then the LOD call (0x8049B568: bias float and the three words).
8. `*out = texture`, return 1.

The R2 scaffold already runs behind step 7: the LOD call is lowered to `RwDriver::native_texture_loaded`, which hands the GX texels and
TLUT to the shared importer (`assets::TextureRecord`, the decoder that walks the disc) and writes the renderer's texture handle into the
extension's `+0x2C`. That scaffold is "deleted by R6": the owner of rows 4-9 and 26 replaces it. Steps 5-6 are where a native reader
can skip the guest-memory round trip: stream bytes straight into the importer, never into the guest raster.

The spec for the PC chunk (`NATIVE_TEXTURE_FORMAT.md`, checked against the 148-texture corpus) is the reader the *PC* assets need; the
disc streams the game loads are the GameCube chunk above. Both are native-texture readers behind slot 26 and can coexist keyed by the
platform id word (6 here); only the GameCube chunk is on the critical path of the game, and the PC chunk reader is needed for the new
assets. Do not conflate them.

### The guest-visible state a native owner must keep valid

The translated game and RenderWare code (still guest code until step 5) reads rasters and textures by fixed offsets, so a native owner
cannot hold a 64-bit object instead:

* `RwRaster` (guest layout, `rwcore.h:1365`): parent +0x00, cpPixels +0x04, palette +0x08, width +0x0C, height +0x10, depth +0x14,
  stride +0x18, offsetX +0x1C, offsetY +0x1E, cType +0x20, cFlags +0x21, privateFlags +0x22, cFormat +0x23, originalWidth +0x28,
  originalHeight +0x2C. The scaffold's lowered sites read `kRasterParent..kRasterType` (`runtime/scaffold/rw_driver/rw_driver.h`).
* The GameCube extension (plugin offset word at `0x805F2700`): GX format +0x0C, TLUT format +0x10, flags +0x14 (bit 0 alpha),
  pixels +0x1C, palette +0x20, native texture handle +0x2C (read only by the lowered bind), levels-minus-one +0x32.
* `RwTexture`: raster +0x00, name +0x10, mask +0x30, filter/addressing +0x50, refcount +0x54.

So the first deliverable of an owner is a guest-layout mirror of those fields written through guest memory (as `rwcore` already does for
other objects it shares with translated code), with the renderer handle kept native. Until the game itself is native a raster has two
faces; nothing in this inventory removes that.

### Order (each step through the section-4 gates; expected `what_if` from the cluster table above)

1. **Pure functions** (no state): slots 2, 3, 9 (and the image exchange 6, 7): per-format tables in `rwcore`, contract tests against the
   original bodies for every raster format word the game can produce. Hits are 0-1, so no replay risk; frees the data-word-only rows.
2. **The native texture reader** (slot 26 plus the extension fields): GameCube chunk reader as above, test = interpreter reading real
   `.txd` streams from the disc against the native reader (every field of the raster, extension, texture; texel and TLUT bytes equal),
   negative cases (wrong platform id, version out of range, short reads, bad palette size), and a negative control. Then the PC chunk reader
   against the existing corpus checker. Replaces the 'HOST' stand-in in `rwcore/src/raster.cpp`.
3. **Raster life cycle** (slots 4, 5, 8, 28 + the helper 0x80497458): Create/Destroy/SetRaster/GetMipLevels with the renderer texture,
   TLUT finalisation, mip chain; tests across type (normal, texture, Z, camera, camera texture), format, depth, DONTALLOCATE.
4. **Lock / unlock / palette lock** (slots 15, 16, 23, 24): intent flags, level, stride, re-upload on unlock; tests with writes read back.
5. **Sub-raster and the fence** (slot 12 and 0x8049B7DC): the two hot rows (12,761 and 5,453 calls). A sub-raster is a view; the fence
   becomes a frame/fence value of the renderer. Measure with the capped A/B (performance claims only from repeated alternating pairs).
6. **The rest** (slots 11, 13, 14, 17-19, 25, 27, GcnSystem): the zero-hit rasters ops and the writer. `GcnSystem` is shared with rwR1.
   The `what_if` for the whole cluster alone is in the cumulative table above (rwR2 step); the cluster is entered by data words, so owning
   all 26 frees rows only after the table words stop being data (owners have no body, but the table entries still name them: the existing
   native-owner mechanism covers that).

Open problems: (1) no GameCube `.txd` corpus is checked in for slot 26's test; the Westopolis run reads 348 textures that can be collected
(the disc's texture-dictionary streams via `--asset-trace`, or the guest raster contents via the watch/peek options) before step 2; how is not settled here. (2) Whether the game ever lock/unlocks a camera-texture
raster is unmeasured (the census saw 3 Lock/Unlock calls); the camera-texture copies are lowered sites (`lift-sites-r2.txt`). (3) The
platform id word `6` and the 0x58/0x10 headers are read from the original code only; no document states them: step 2's tests are their
evidence.

## rwR2 implementation, step 1a - RGBTOPIXEL and PIXELTORGB are native owners (2026-11-07)

Assignment (main session): implement rwR2 following the order above, natively, keeping the guest-visible raster / extension / texture fields valid.
Step 1a is the two stateless colour conversions (slots 2 and 3). The image exchange (slots 6, 7, 9) is step 1b, below.

What changed: `runtime/include/gekko/native_rw_pixel.h`, `runtime/src/gekko/native_rw_pixel.cpp` (`gekko::rw_pixel`: the pure conversions
`rgb_to_pixel` / `pixel_to_rgb`, the two guest-call servers and `install`), registered in `system.cpp` (next to the os_settings owners),
`CMakeLists.txt` (source list, `rw_pixel_contract_tests`), `extract/native-owners.txt` (a commented block, two rows),
`runtime/tests/native_file_api_tests.cpp` (the owner list of the runtime), and both roots lists regenerated with the scanner (LF).

What the original does (read from `0x80492BFC` / `0x80492F10`, written into the header): a switch on `rasterFormat & 0x0F00`: default and
565 (0x200), the GX RGB5A3 pair (0x100 / 0x300), 555 (0xA00), 8888 (0x500), 888 (0x600). Luminance (0x400) and the unlisted values raise a
RenderWare error and store an uninitialised stack word. Deliberate difference: the owner stops the run with a diagnostic there instead of
reporting the RwError; the game never reaches it (0 calls on the replay, and its rasters use only the listed formats).

Gates:

1. Contract test `rw_pixel_contract_tests` (original DOL body in the reference interpreter vs the owner): 439,740 calls over every listed format
   (with stray bits around the field), a 20 x 20 x 5 x 5 colour grid for RGBTOPIXEL and 450+ pixel words (every bit, bit pairs, pseudo-random)
   for PIXELTORGB; result register and every stored byte of the scratch area equal; 54 error-format calls refused without writing; a negative
   control (a green bit dropped) is caught. 7 s.
2. No retired state: both functions are stateless (they read the arguments and write the output), so there is no guest watch to run.
3. Westopolis A/B, default config, alternating 2 + 2 (baseline = the same branch built without the change, `base3.exe`; candidate `cand3.exe`):
   PCM hash 015445f2 in all four, 3,645,000,000 ticks, 2,697 VI frames, 5,360 render frames; guest memory 0 differing 64 KiB blocks in every
   pairing (B/B, C/C, B/C x2). Nothing to classify.
4. PIXEL GATE: five `--capture-at-tick` frames (ticks 4.0e8, 1.2e9, 2.0e9, 2.8e9, 3.5e9, frames 552 .. 5145, `SHADOW_R2_MSAA=1`), baseline and
   candidate PNGs byte-identical (sha256 d948841c, 49e7aa7a, 24f6623e, 2e359121, 64cad03f...). (Repeated `--capture-at-tick` flags keep only the
   last, so each frame is its own run.)
5. Performance: not measured. The replay never calls either function (0 hits), so there is nothing to time; the hot rows (SubRaster, GcnSystem,
   the fence check) are not touched by this step.
6. Build and tests: lifted tree and tests tree build; `rw_pixel_contract_tests`, `native_liveness_roots`, `native_liveness_roots_lifted`,
   `platform_gc_removal_plan`, `platform_runtime_coupling` pass. `native_file_api_tests` (its owner list does not include the CRI owners; neither of
   this step's addresses is among the rows it names), `lift_sites_reference_tests` and `native_wait_points` fail as before.

Measured: tests-tree platform-SDK bodies 877 (ratchet) -> 874, frontier 221; the lifted tree's ratchet is the main session's to lower.
Not covered: windowed gameplay, audio device output, GPU output on hardware other than this machine; stages other than Westopolis.

## rwR2 implementation, step 1b - IMAGEFINDRASTERFORMAT is a native owner (2026-11-07)

`0x804948D8` (standard slot 9) joins the step-1a owners in `native_rw_pixel.cpp`. Read from the original: it stores `cType = flags & 7` and
`depth = 0`, then by raster type 0 / 4 / 5 sets width and height to the image's clamped to 0x400 (rounded down to a power of two when
`flags & 0x8000`) and `cFormat = (scan | flags & 0x9000) >> 8`; types 1 / 2 copy the image size and clear `cFormat`; types 3, 6, 7 raise an
RwError (the owner stops). The scan (0x804947AC) reads each pixel's alpha (palette alpha for depth 4 / 8, the fourth byte otherwise): 0xFF is
opaque, 0..0x0F marks the image cut-out (0x100), any other alpha ends the scan with 0x300 (with the palette bit 0x4000 / 0x2000 for depth 4 / 8);
all opaque is 0x200, plus the palette bit.

Gates: contract test (same file, original body in the reference interpreter): 16,896 calls equal (result register and all 0x40 bytes of the raster)
over depths 4 / 8 / 16 / 32, widths up to 0x401 (the 0x400 clamp), heights, six alpha populations, flag extras (power-of-two bit, stray bits) and
the types; 4,608 error-type calls refused after the stores the original makes first; the sweep reaches every answer of the scan (948 opaque,
768 cut-out, 2,892 translucent). Westopolis A/B 2 + 2 (baseline = the step-1a build): PCM 015445f2, 2,697 / 5,360 frames, 0 differing memory
blocks in every pairing. PIXEL GATE: 5 frames byte-identical. Tests tree: platform bodies 874 -> 872, frontier 221 -> 220;
`rw_pixel_contract_tests`, `native_liveness_roots(+_lifted)`, `platform_gc_removal_plan`, `platform_runtime_coupling` pass;
`native_file_api_tests` fails as at baseline (the CRI owners).

Slots 6 (IMAGEGETRASTER) and 7 (RASTERSETIMAGE) move later in the order: they lock the raster through slot 15 / 16 and the palette through 23 / 24
and dispatch into per-format converters (0x80493C38, 0x80493E90, 0x804940EC, 0x80494380), so they are done after the lock / unlock owners exist.

## Queue item 4 - the main session's SDK identities, verified and applied (2026-11-07)

Source: the main session's lists (docs/SEMANTIC_PUZZLE_ENGINE.md sections 11-12b, 16 + 28 identities). Each was re-read from the disassembly
(`tools/doldis.py`) before being graded; the table with grade and evidence per row is `test_data/platform/sdk_anchor_names.csv` (41 renames),
applied by `tools/platform/regen_manifest.py` after the earlier renames (`--anchor-names`, a row is skipped and reported when the symbol's current
name is not the recorded one).

Result of the check: every identity is confirmed. PROVEN (the code matches the SDK source sequence, register ids, field offsets and call
structure): `__DBExceptionDestinationAux`, `OSSaveContext`, `__GXSetVCD`, `GXSetNumTexGens`, `GXDrawDone`, `GXClearBoundingBox`, `GXSetNumChans`,
`GXInvalidateTexAll`, `GXSetProjection`, `GXSetProjectionv`, `__GXSetViewport`, `GXSetClipMode`, `__GXSetMatrixIndex`, `GXSetDispCopyYScale`,
`GXSetCopyClear`, `GXCopyDisp`, `GXCopyTex`, `GXSetZTexture`, `GXSetFieldMask`, and the thread / context set (`OSGetCurrentThread`,
`OSIsThreadTerminated`, `OSGetThreadPriority`, `OSYieldThread`, `OSCreateThread`, `OSExitThread`, `OSResumeThread`, `OSSuspendThread`, `OSSleepThread`,
`OSWakeupThread`, `OSSetThreadPriority`, `__OSUnlockAllMutex`, `UnsetRun`, `__OSGetEffectivePriority`, `SetEffectivePriority`, `OSInitContext`,
`OSGetStackPointer`, `OSGetConsoleType`, `DefaultSwitchThreadCallback`). INFERRED (role certain, only part of the body traced): `GXSetGPMetric`
(head and registers), `GXSetFogRangeAdj`, `OSCancelThread`. Settled: 0x8039A0B4 takes (matrix, type) and is `GXSetProjection`; 0x8039A158 takes a pointer
whose first float is the type and is `GXSetProjectionv`. `GXCopyDisp` and `GXCopyTex` differ in the copy-control bit 14 (1 and 0) and their register
shadow sets (+0x1E0.. and +0x1F0..). Not applied: 0x8037829C already carries the SDK label `__OSSystemCallVectorStart`; the three MetroTRK
extended-register blocks (0x8039F67C, 0x8039F6FC, 0x8039F7EC; mfspr 0x3A0.. / 0x3D0.. with stmw / lmw into the TRK context at r2+0x240 / 0x2B8) have no
symbol to rename (they are split pieces); the role is INFERRED and `functions.csv`'s `OSGetTime` for 0x8039F6FC is wrong.

Services: the regexes filed six of the names wrongly, so four exact-name rules were added to `tools/dolrecon/platform_deps.py`
(`__DBExceptionDestination(Aux)` -> os.debug, `UnsetRun` / `SetEffectivePriority` / `__OSUnlockAllMutex` / `__OSGetEffectivePriority` /
`DefaultSwitchThreadCallback` -> os.threads, `OSGetStackPointer` -> os.core).

Before / after on the same live graph (`removal_plan.py --reuse-graph`, the merged lists of gc-removal d582a25; names-only manifest vs +these identities):

| tree | platform bodies | frontier | moved between services |
|---|---:|---:|---|
| tests (not lifted) | 765 -> 765 | 208 -> 208 | os.other 40 -> 36 bodies, os.threads 25 -> 27, os.core 17 -> 19, os.debug 2 -> 4, sdk:unidentified 2 -> 0 |
| lifted (shipping) | 515 -> 515 | 165 -> 165 | the same |

The body count does not move (as the main session said: the planner already classifies pieces by address range); only the attribution does.
The GX identities do not change a count either: those rows were already graphics.gx by link order. Split-piece attachment was not done (low priority,
per the correction). Note for whoever reads the totals: the 872 -> 765 drop seen when this plan was first regenerated is the merge, not the names
(the live graph is rebuilt from the merged owner lists each run).

## rwR2 step 2 groundwork - the GameCube native texture chunk checked against the whole disc (2026-11-07)

`tools/rw/gcn_txd_corpus.py` indexes every `.txd` under `../game/files` and checks the layout read from the original body (section "The GameCube
native texture chunk") field by field; the index is `test_data/rw/gcn_txd_corpus.csv` (one row per texture: file, name, mask name, filter word, LOD words
and bias, raster format word, size, depth, levels, GX and TLUT format, alpha flag, palette and texel byte counts).

Result: 701 dictionaries, 1,735 textures, 0 problems. Every texture has platform id 6; the struct chunk's size equals `0x58 + 0x10 + palette + 4 + texel
bytes` exactly (the palette is `2 << depth` bytes when the raster format word has 0x2000 / 0x4000); the texel byte count equals the GX-tiled size of the
level chain (32-byte tiles, 8x8 for 4-bit, 8x4 for 8-bit, 4x4 for 16-bit). The header fields therefore are what the instructions say, including
`+0x09` = the level count (not minus one): it is 1 in all 1,735.

What the disc actually uses (this scopes the native reader): GX formats 4 (RGB565, 1 texture), 5 (RGB5A3, 12), 8 (C4, 1,138) and 9 (C8, 584) only;
TLUT format 1 or 2 (RGB565 / RGB5A3) for the palettised ones, 0xFF otherwise; raster format words 0x204, 0x304, 0x4104, 0x4204, 0x2104, 0x2204, 0x2304;
depths 4 / 8 / 16; no mipmaps (levels = 1 everywhere). The reader's hard cases (more levels, other GX formats, RGBA8, CMPR) are not exercised by any
disc dictionary; they can be refused with a diagnostic, as the PIXEL formats of step 1a are.

Not done: the oracle run of the original body on each texture (it needs the RenderWare engine opened in the reference interpreter, or recorded inputs
and outputs from the live run) and the native reader itself; the owner has to orchestrate guest calls (RwStreamFindChunk 0x8047DFA0, RwStreamRead 0x80481D9C,
RwRasterCreate 0x8048AEC4, the level allocator 0x80497A70, RwTextureCreate 0x8048DD2C and the name setters) with the importer taking the texels directly,
which also retires the R2 scaffold's read of the guest raster (`build_texture`). That is the next piece of work.

## rwR2 step 3 - the native raster -> image conversion, slot 6 (2026-11-08, agent sdk-identify)

Owner: `rw_image::install` (`runtime/src/gekko/native_rw_image.{h,cpp}`), row `0x80493A40` GcnImageGetRaster (standard function 6, data word
`0x8051D43C`). The row was chosen because the per-row `what_if` shows it frees 26 platform rows by itself (the dispatcher plus its three converters
`0x80493118` / `0x8049336C` / `0x80493660` and the 22 interior pieces and pixel functions they call; the three jump tables
`0x8056F1F4 / 0x8056F2AC / 0x8056F320` stay in `extract/jump-tables.txt`, which is generated and checked by `native_jump_tables`: the generator then
treats their words as edges of the pruned converters and frees the 20 case arms. Correction of commit 1a08287, which had deleted those three lines: the table
words became liveness roots again and only 6 of the 26 rows were pruned; with the lines restored the generated build prunes 26 of 26). The other zero-hit rows of the table above free 0 on their own (their callees stay
reachable from other rows); RasterLock (17) and RasterUnlock (9) are the raster life-cycle owner's rows.

What it does (read from the original body, section header of `native_rw_image.h`): lock the raster for reading unless it is locked (`RwRasterLock`
`0x8048AFC0`), lock the palette for reading when the format has a palette bit and it is not locked (`0x8048ACE4`), convert by image depth 4 / 8 / 32, unlock
what it locked (`0x8048ABD8`, `0x8048AB90`), r3 = 1. The four lock functions stay the game's own, called through the guest-call seam (RW core; none blocks).
The per-format pixel expansions are `rw_pixel::pixel_to_rgb` (the same arithmetic as the original's six pixel functions, which the contract checks).

Stated differences: the original reports an RwError for a format that is not 1555 / 565 / 4444 / 8888 / 888 / 555, for a depth pair that is not
(image 4: raster 4), (image 8: raster 4 or 8), (image 32: raster 4 / 8 / 16 / 32), and for any other image depth, then carries on with a null pixel
function; the owner stops the run (ExecutionFault with a diagnostic) with nothing written. The game does not reach them (0 calls on the Westopolis replay;
the only caller of the standard function is RwImageSetFromRaster `0x8048A9F8`, which has four call sites in three RenderWare library functions
(`0x80472BCC`, `0x8048D684`, `0x804A2A58`); two have no direct caller and the third is called once, from `0x804A6608`. This is a static reading of direct calls only,
not a proof that nothing live reaches them, which is why the owner converts instead of stopping).

Evidence:
* (a) stub oracle - `rw_image_contract_tests`: the original body in the reference interpreter against the owner, the four lock / unlock functions replaced by
  recording stubs in both runs. Every legal pair of image depth x raster depth x the six pixel formats x six sizes (odd widths, strided rows) x three lock
  states (already locked / raster unlocked / raster and palette unlocked); compared: r3, every byte of the scene (image pixels and palette, the raster, the
  sentinel fill around them), the order and arguments of the lock calls, r1 and lr. Plus the refusals (formats 0x000 0x400 0x700 0x800 0x900 0xB00, image
  depths 0 1 16 24 64, the illegal depth pairs): an ExecutionFault and nothing written.
* (b) live evidence: none for this row. It has 0 hits on the replay, so no running game exercises it; this is a static + contract step, as the never-entered
  rows of os-font were. No recorded replay was built.
* Result: 756 legal scenes (432 image depth 32, 108 image depth 4, 216 image depth 8; 504 of them with lock / unlock calls) equal to the original; 191 refusals.
  Negative control: with the 16-bit raster branch's pixel format changed (`format ^ 0x100`) the same test fails ("converted bytes differ from the original's"); restored,
  it passes.

## The five unnamed RW driver rows, named from their instructions (2026-11-08, agent sdk-identify)

`removal_plan` (tests tree, 5e8e695) lists 91 RW-driver frontier rows with no owner; five carry no name in `test_data/platform/gx_rw_frontier.csv`. Read from the DOL
(`tools/doldis.py --func`), grade INFERRED (the instructions and the callees' Dolphin signatures; no contract test, because naming them does not need an owner and none
of the five frees a row alone):

| address | what the instructions do | callees (Dolphin signature) | entered by |
|---|---|---|---|
| 0x80498F74 | `(a, b, c, d, e)`: unless the block at 0x805D?42D0+0x3C equals 1 it calls `GXSetAlphaCompare` with the five values saved at +0x40/+0x4C/+0x48/+0x44/+0x4D, then saves the new five and sets +0x4E = 1: a save-and-apply-previous wrapper for the alpha-compare state | GXSetAlphaCompare 0x80399778 | game fn_80046D80 |
| 0x8049B568 | builds the GX texture object of a texture's raster: palettised formats (format byte & 0x6000) go through `GXInitTexObjCI` 0x80398080, others through `GXInitTexObj` 0x80397E34, with the wrap/filter words looked up in the tables at 0x8056FC90 / 0x8056FCC8 indexed by the texture's filter word (+0x50), then the LOD call with the float in f1 and the byte at +0x32 | GXInitTexObj, GXInitTexObjCI | game fn_80038BB0 |
| 0x8049B734 | `(texture, float* bias, int* a, int* b, int* c)`: reads the GX texture object back: when its flag word has the bit that marks "no sampler state" it answers bias = the float at sdata 0x1D18, a = b = 1, c = 0; otherwise the four GX getters 0x8039824C (LOD bias float), 0x8039828C, 0x80398298, 0x803982AC. The inverse of 0x8049B568's LOD call | GXGetTexObj* getters | game fn_80038BB0 |
| 0x8049C6BC | `lwz r3, 0x6244(r13); blr`: returns the engine's current-state pointer (vi.md step 6; owned by `install_native_rw_world_owners`) | - | runtime literal |
| 0x8049C6C4 | `(object, arg, flag)`: with the flag and a non-zero field at +0x18 of the r13+0x6244 state it calls 0x8049FD4C; records (object, arg, state) in a static block at 0x805D?4320 with its +8 set to 1, clears object+0x10, calls the object's virtual `object->[8]->[0]->[4](block + 0x10)`; a null answer clears block+8; if object+0x10 is above 1 it is set to 2 and the (empty) 0x8049C19C runs; clears the block; returns the object on success, 0 otherwise: a re-entrancy-guarded virtual call | 0x8049C19C (`blr`) | RW code 0x80456F04 |
