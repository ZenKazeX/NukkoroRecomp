# 5K memory: render-target aliasing (2026-10-30, in progress)

## SESSION HANDOFF - resume here after a context loss (fork session "5K memory side", updated 2026-10-30 13:40)

STATUS 13:40: render-target aliasing DONE and validated (see "RESULT" at the end), delivered to H:\NukkoroGit.
~15:15: the main session reports a new user goal (every frame <= 10 ms, exact) and asked about trimming. Trimming was
NOT started (no go from this session's user); estimated only ~0.5-1 ms of the 9-12.5 ms locked-clock smoke frames.
Later (relayed by the main session): the user's newest directive is extreme 32:10 resolutions and a 1 ms complete GPU
frame (docs/OPTIMIZATION_GOAL.md "CURRENT GOAL"); the main session runs a 32:10 fullscreen ladder 3840x1200 ..
16384x5120 (no game processes from here meanwhile). New dev option --tape-output-free: a tape's inputs with the output
at the exact render size (PCM byte-identical across aspects). Render-target memory scales with the pixel count, so
the aliasing saving grows with it (view-sized RGBA16 shares the glare sources' memory at any size).
~15:25: the main session TOOK all smoke work (trimming and a tile-binned compute compositor it will try to prove
bit-equal). Its census (locked clocks, 5K fullscreen): Final Haunt max GPU 4.45 ms, 0 frames > 10 ms; Westopolis 20
frames > 10 ms, all smoke (ticks 1184-1208, max 12.83 ms) - the <= 10 ms goal needs ~-2.9 ms (~26 %) of the smoke's
fill. This session does NOT edit particles.hlsl or the particle path of renderer.cpp. Notes sent to it: exact ROP
UNORM16 rounding must be derived empirically; 4x coverage/fill rule/sample depth; prove at 1x + relabelling at 4x.

User instructions in force (2026-10-30, this session):
1. "Always test the game at 5K resolution in fullscreen mode. Treat 5K as the primary benchmark" - reduce CPU, GPU,
   RAM, VRAM through efficient native implementations without sacrificing visual quality, rendering accuracy or
   gameplay; measure before/after every optimization.
2. (10:05) Keep 5120x2880 fullscreen, 4x MSAA and the R16G16B16A16_UNORM scene format: no visual-quality reductions.
   Order: (a) eliminate the ~11 ms smoke overdraw by genuine GPU workload reduction, identical blending, depth and
   output; (b) complete the aliasing investigation: isolate the placed-resource pixel differences, keep committed
   resources wherever placement affects correctness, measure the real saving; (c) controlled 5K fullscreen A/B with
   GPU timing, RAM/VRAM, frame pacing, pixel comparisons; reconcile the 115 vs 173 MiB savings. No fake results, no
   ignored mismatches, no benchmark shortcuts; validate before shipping; coordinate builds with the other sessions.
3. "Always update your current session handoff to not forget anything after running out of context" - this block.
4. (10:3x) Delete pointless failed test files; keep contact with H:\NukkoroGit - "every major success has to be
   delivered there and fuse with the code there" (memory feedback-deliver-to-nukkorogit: file-by-file merge after
   validation, no tests, keep its own edits, public wording "static recompilation, work in progress"); create a fresh
   project map; delete temp files on C.
   Done 2026-10-30 ~10:50: removed the 492 GB runaway audio dump .build/validation/motion/s4b_wp/audio.s16be (a
   hung run of 10-29 with no budget), 31.6 GB of bulk outputs of finished work (prune.ps1 in the scratchpad: only
   folders with no file from today; text evidence kept), 3.9 GB of my analysed captures; %TEMP% on C: 50.4 GB of
   idle Claude session folders + 59.1 GB older than 3 days (C free 68 -> 180 GB; NVIDIA DXCache and pip cache
   kept). H: .build 548 -> ~20 GB. Map: docs/PROJECT_MAP.md.
   NukkoroGit delivery of THIS work: pending until the aliasing gates pass (36 runtime files differ today, many of
   them other sessions' unvalidated edits - deliver only validated changes).

Coordination (another session, "Performance optimization" local_e6ab04f3-68c4-4bb0-80f2-6f3e3da2b1c2, owns
docs/GPU_FILL.md, the smoke analysis and the kSkipClear fix):
- It asked (09:5x): no game processes, headless included, while it runs locked-clock 5K GPU A/B (~30 min). Wait for its
  "window over" message before any run. It also asked not to launch the diag exe while it relinks.
- 10:06 I proposed a split: it keeps the bound analysis of the remaining 7.9 ms smoke; I take particle trimming
  (fewer rasterized fragments of alpha-0 texels with an exactness proof). No reply yet - read notifications first.
- Builds: tell it before building release or diag; both trees are shared (.build/release, .build/diag).

State of the code (uncommitted, nothing deployed):
- runtime/lift/render/frame_graph.{h,cpp}: TexturePool Memory/placed textures, acquire(desc, frame, name, first,
  last), Activation (aliasing barrier + DiscardResource), diagnostics SHADOW_R2_TARGET_ALIASING (0 committed, 1/unset
  aliasing, 2 placed unshared) and SHADOW_R2_PLACED_FILTER (ss / msdepth / mscolour / ms).
- runtime/lift/render/resources.{h,cpp}: texture_resource_desc, allocate_target_memory, create_placed_texture_2d.
- renderer.h/.cpp pool_memories/pool_memory_bytes/pool_placed_bytes; src/gekko/lifted_render.cpp report line.
- runtime/tests/render_core_tests.cpp graph_aliases_memory_by_lifetime (passes; whole render_core_tests passes).
- Release exe with all of the above: .build/release (built ~09:50, build log .build/alias-release-build3.log).
- Main session's own uncommitted edits in the same tree: GEKKO_FRAME_WORK (native_frame_limiter), per-draw GPU timer,
  SHADOW_R2_STABLE_POWER, kSkipClear (particles.hlsl etc.).

11:15 coordination settled: the main session keeps the smoke / scene-pass GPU work (it measured the smoke as
fill-bound, GPU_FILL.md 3-5); this session owns the 5K memory side. Announce before BUILDING; do not run while it
measures GPU (it messages first); GPU-time comparisons use SHADOW_R2_STABLE_POWER=1; my runs go to
.build/validation/perf-2026-10-30/mem-5k/ (own run.ps1 copy there). The bisection runs from mem-5k/alias_bisect.ps1.
Added, NOT BUILT yet: TexturePool::memory_map() + Entry::name, renderer stats pool_memory_map (rebuilt when the pool
changes), report lines "[lifted-render]   shared memory <MiB>: <texture> WxH sN MiB (last frame n), ..." - the
evidence for the 115 vs 173 MiB reconciliation.

Next steps, in order:
1. After the window: rerun `.build/validation/perf-2026-10-30/alias_bisect.ps1` (delete the partial ab-g-base-1 first)
   and analyse with group_split.py (copy in the scratchpad; recreate if lost: counts pixels constant inside each arm
   and different between arms vs every other split of the same runs). Find which placed class (ss, msdepth,
   mscolour) moves pixels.
2. Policy: keep committed memory for any texture whose placement changes pixels (likely the whole-frame MSAA targets,
   which never share anyway); prove with 3+3 runs that aliased-with-policy vs committed has 0 arm-split pixels in
   the glare window (frames 1150-1169) and in the trail window (3010-3029), at 5120x2880.
3. Reconcile 115 vs 173 MiB: the report's "965 MiB for 1138 MiB" counts a superseded texture waiting out
   kKeepFrames (60 s) in placed_bytes; mode 2 (placed unshared) = 1080 MiB, so the true saving is 1080 - 965 = 115
   MiB. Prove by listing per-memory occupants at exit (add to the report) and by gpumem.csv maps in a 5K run.
4. Smoke trimming (if the split is agreed): see "Smoke" below.
5. Controlled 5K fullscreen A/B (run.ps1 -Rendered, SHADOW_R2_STABLE_POWER=1 for GPU timing, alternated arms, 3+
   runs each): GPU p50/p99, present_trace repeats, private commit, D3D12 local usage, pixel gate.
6. Full CTest (Release tests preset only), update this doc, NEXT_SESSION.md pointer, memory.

Smoke facts (from GPU_FILL.md + my reading): texture 0x010000F1 32x32 puff, 32.5 % texels alpha 0; ~15 batches /
~740 particles per heavy frame (Westopolis ticks ~1170-1240); one DrawInstanced(6, count) per batch
(renderer.cpp ~1700), PSParticle in shaders/particles.hlsl: colour x Sample(tex), cutoff discard, kSkipClear discard
of alpha exactly 0, fog, mark_glare_tile; blend SrcAlpha/InvSrcAlpha, depth test, no depth write; MSAA-independent
cost (per pixel). Samplers: point / linear / anisotropic (max 16) from descriptors.cpp; mips MinLOD 0..max. Trimming
exactness needs: every removed fragment provably samples alpha exactly 0 over the full filter footprint (bilinear
2x2, every mip the LOD can pick, anisotropic taps), and unchanged interpolated UVs for kept fragments under the new
triangulation (or a technique that keeps the triangles, e.g. a per-pixel test that is exact).

The user, 2026-10-30: "Always test the game at 5K resolution in fullscreen mode. Treat 5K as the primary benchmark"
(memory: feedback-5k-fullscreen-benchmark). This page is the memory side of that work. A parallel session works on
the scene pass's GPU time. Runner: `.build/validation/perf-2026-10-30/run.ps1 -Rendered` (5120x2880 fullscreen on the
1920x1080 60 Hz desktop, present + GPU + GPU-memory traces).

## Baseline at 5K (run 5k-wst1: diag exe of 09:0x, Westopolis replay, 90 s)

| | |
|---|---|
| GPU frame | p50 5.83, p90 6.92, p99 10.93, max 17.31 ms; 151 frames > 8 ms |
| displayed repeats | 7: 3 packet_late at stage start (ticks 16/21/27), 4 gpu_late (ticks 94, 1175, 1184, 1185; scene pass 14-16 ms) |
| process private commit | **~1.41 GiB for the whole run** (1041 MiB at 1 s, 1412 MiB from 4 s). The report's "private 186 MiB" is taken after the renderer is destroyed, and its "peak 1415.99" is the steady value, not a spike. |
| D3D12 local usage | 876 -> 1264 MiB; default-heap blocks 861 MiB at frame 1, 1228 MiB from ~3 s |

On this driver every default-heap byte is also process private commit (twenty-first increment), so at 5K the render
targets ARE the RAM problem. The D3D12MA map at 3 s (gpumem.csv.1.json):

| allocation | MiB | lifetime in a frame |
|---|---:|---|
| view colour, 4x R16G16B16A16_UNORM | 452 | scene .. final resolve |
| view depth, 4x D32 | 228 | scene .. final resolve |
| glare sources, 4x RGBA8 | 228 | first glare-sources pass .. its glare resolve (cleared every frame) |
| view (resolved), 1x RGBA16 | 115 | final resolve .. compose (also the mid-frame snapshots, same description) |
| shadow.view_history, 1x RGBA16 | 115 | persistent (motion trail) |
| output, 1x RGBA8 | 57.5 | glare resolve (sources resolved share it) .. present scale |
| everything else (asset textures 14.7, meshes, skin cache, glare levels, upload ring 16) | ~40 | |

The pool shared textures only of the SAME description; different descriptions with disjoint lifetimes each held
their own memory. The glare sources and the resolved view never live at the same time.

## The change

`TexturePool` (runtime/lift/render/frame_graph.{h,cpp}) places every render-target or depth texture of 1 MiB or
more (the existing own-allocation threshold; no unordered access) in shared render-target memory: an own heap per
memory (D3D12MA AllocateMemory, ALLOW_ONLY_RT_DS_TEXTURES, 4 MiB alignment so multisampled textures fit at offset
0), textures created there with CreateAliasingResource.
- Placement by the frame graph's kept-pass lifetimes: a resource goes into the LARGEST memory nothing else holds
  during its [first, last] kept passes - an existing placed texture of its description there, or a new one placed
  there - else into new memory of its own size. Same-description hand-overs (the twenty-first increment) still
  apply, now only when the memory is free in the new lifetime.
- Over frames textures move into the largest memory; the ones left unused are released after kKeepFrames (60 s) and
  their memory with the last of them.
- Activation: at every aliased resource's first kept pass, an aliasing barrier, a transition to RENDER_TARGET /
  DEPTH_WRITE when needed, and DiscardResource, before the pass's own transitions (FrameGraph::Activation). Valid
  because compile already refuses a graph-owned texture whose first use is not a clear or a whole overwrite, so no
  earlier contents are ever observed - including across frames.
- Report line: `texture pool: ... ; shared memory N blocks, X MiB for Y MiB of placed textures`.
- The Vulkan backend's pool (render_vk) is unchanged.

Expected at 5K: the resolved view inside the glare sources' memory, -115 MiB VRAM and -115 MiB private commit,
identical pixels. Not yet measured.

## Validation so far

- render_core_tests (WARP + D3D12 debug layer), new case `graph_aliases_memory_by_lifetime`: an 8-bit 1024x1024 and
  a 16-bit 1024x512 target with disjoint lifetimes share one memory, each is activated and reads back its own
  contents; the next frame reuses both with nothing created; overlapping lifetimes get their own memory; no debug
  layer messages. All render_core_tests pass.
- In the game (release exe 09:30, headless Westopolis replay, `--native-output 5120x2880`): the pool reports
  `shared memory 4 blocks, 965 MiB for 1138 MiB of placed textures` (the 1138 includes one superseded texture
  waiting out kKeepFrames). Placed but unshared (mode 2) needs 5 blocks, 1080 MiB: sharing saves 115 MiB at 5K.
- Same-binary switches (diagnostics): `SHADOW_R2_TARGET_ALIASING` = 0 committed textures as before, 2 placed
  textures each in its own memory, 1/unset aliasing; `SHADOW_R2_PLACED_FILTER` = ss / msdepth / mscolour / ms places
  only that class (attribution only).
- Captures, 20 frames at 5120x2880 each (scripts alias_caps.ps1, alias_modes.ps1; analysers in the fork's scratchpad:
  cmp_caps.py, group_split.py = pixels constant inside each arm and different between them, vs every other split of
  the same runs):
  - trail + distortion window (frames 3010-3029): on vs off, two runs each: **all 20 frames bit-identical**, also
    off vs off and on vs on.
  - glare window (frames 1150-1169), three runs per arm:

    | arms | pixels split by arm | the other splits of the same runs (noise) |
    |---|---:|---|
    | committed (0) vs aliased (1) | 28 | all 0 |
    | committed (0) vs placed, unshared (2) | 26 | all 0 |
    | placed, unshared (2) vs aliased (1) | **0** | 0-9 |

    **Sharing memory changes no pixel.** Placing the textures (instead of committed resources) moves ~1.4 pixels
    per frame by one step, all in one box (x 2122-2198, y 1006-1446 of 5120x2880); the same window also has the GPU's
    own run-to-run variance (92 px between two identical runs). Which placed texture does it: bisection running.
- Bisection (11:3x, release exe 10:42 incl. the main session's kSkipClear, mode 2 + SHADOW_R2_PLACED_FILTER, 3 runs
  per arm, mem-5k/alias_bisect.ps1):

  | placed class only | memory placed | arm-split pixels | other splits (noise) |
  |---|---:|---:|---|
  | single-sampled (view, output) | 172 MiB | 37 | 0, 0, 0, 0, 0, 5, 7, 12, 13 |
  | 4x depth | 228 MiB | 30 | 0, 0, 0, 0, 0, 0, 4, 9, 11 |
  | 4x colour (view colour, glare sources) | 680 MiB | 0 | 0, 0, 0, 0, 0, 4, 7, 9, 17 |

  Placing either of two unrelated targets moves pixels in the same smoke box; placing the 4x colour targets does not.
  Hypothesis (UNPROVEN): the GPU's own 4x MSAA run-to-run variance in that box has a distribution that depends on
  the targets' memory placement, not a computation difference. Decisive test running: SHADOW_R2_MSAA=1 (1x is
  bit-identical run to run per GPU_FILL.md), mode 0 vs 1, 3 runs each (mem-5k/alias_1x.ps1).
- 1x test (11:5x, mem-5k/alias_1x.ps1, SHADOW_R2_MSAA=1, mode 0 vs 1, 3 runs each, frames 1150-1169 at 5120x2880):
  **all 6 runs bit-identical in all 20 frames** (0 varying pixels anywhere; aliasing at 1x: 6 blocks, 517 MiB for
  690 MiB of placed textures). So placement and sharing change no computation. The 4x difference is GPU-internal:
  with 4x MSAA, placing the single-sampled targets or the 4x depth makes the GPU's MSAA path give a different but
  reproducible 1-step result on ~1.4 px/frame in the smoke-edge box where the same GPU also varies between identical
  runs (92 px between two committed runs). Mechanism inside the driver/GPU: not identified (PLAUSIBLE: compression
  state differs between committed and placed resources). Same class as B3a's 1-LSB MSAA edge collapse, which the user
  accepted then. NOT self-accepted here: the saving (-115 MiB) needs exactly the classes that show it (view in the
  glare sources' memory), so the choice goes to the user (asked 11:5x).
- The user (11:5x) chose "Investigate further first" (not accept, not drop). Cause candidates: the per-frame
  DiscardResource of every placed texture (activation) vs the committed path's plain Clear; the heap kind (RT/DS-only
  vs all). New diagnostics (built 12:0x): SHADOW_R2_ACTIVATE=changed (activate only when another texture used the
  memory since this one; Memory::gpu_last_user, committed in execute in GPU order), SHADOW_R2_TARGET_HEAP=all.
  Running mem-5k/alias_cause.ps1: arms c (committed), pA (placed unshared + activate changed), pH (placed unshared,
  all-heap), sA (shared + activate changed), 3 runs each.
- Result (12:2x): vs committed, arm-split pixels pA 41, pH 39, sA 32 - neither the discard policy nor the heap kind
  removes it. Per-pixel value patterns over all 12 runs (time order c pA pH sA x3): 32 pixel-frames follow exactly
  `A B B B A B B B A B B B` = the 3 committed runs one value, all 9 placed runs (every variant) the other: a
  DETERMINISTIC placed-vs-committed effect inside the driver/GPU at 4x. The rest are single-run deviations (e.g. 33
  pixel-frames where only c-2 differs) = noise. Next: SHADOW_R2_PLACED_FILTER=view (only the 1x RGBA16 view class)
  and viewglare (view + 4x colour, exactly what the 115 MiB saving needs); if one of them shows no effect, keep
  everything else committed (output RGBA8 and the 4x depth) and the saving survives.
- Coordination: the main session runs its kSkipClear 3+3 gate now (diag exe, SHADOW_R2_TARGET_ALIASING=0); wait for
  its "done" before running. group_split.py is copied to perf-2026-10-30/ and mem-5k/.
- View-class test (12:4x, mem-5k/alias_view.ps1): placed view class only (1 block, 115 MiB) and the SHARED view +
  4x colour configuration (2 blocks, 680 MiB for 795 MiB of textures = -115 MiB) vs committed: **arm split 0** both
  (noise-only relabellings 0-5 and 0-29). With the bisection (ss = view + output: 37; view alone: 0) the
  layout-dependent 4x change comes from placing the 8-bit 1x output class and/or the 4x depth, neither of which can
  share memory in the game's frames.
- POLICY (in source 12:5x): TexturePool::aliasable places only multisampled colour and single-sampled
  R16G16B16A16_UNORM; depth and every other class (8-bit 1x, untested) stay committed. SHADOW_R2_TARGET_ALIASING=3 =
  every class (old behaviour, diagnostics). render_core_tests updated (two RGBA16 targets share; 8-bit 1x and 4x
  depth not placed, 4x colour placed): all pass.
- 115 vs 173 MiB RECONCILED (map-full2, every class placed): blocks 452 (view colour) + 228 (view depth + a stale
  57.5 MiB "glare sources resolved" texture created at frame 1, never used again) + 228 (glare sources + scene
  snapshot/view 115) + 57.5 (glare sources resolved) = 965 MiB; textures 1138 MiB. 1138 - 965 = 173 = 115 real
  sharing + 57.5 stale texture lying inside the depth's memory (no memory of its own). Placed-unshared = 1080 MiB, so
  the true saving is 1080 - 965 = **115 MiB**; with the policy 680 MiB for 795 MiB (-115 MiB, the same). The report
  now counts only textures in use and marks the others "(unused since frame n)".
- Gate 1 running (mem-5k/gate_policy.ps1): policy vs committed, 3+3, frames 1150 and 3010 windows. Then gate 2
  (mem-5k/ab_5k.ps1, 5K FULLSCREEN, diag exe, SHADOW_R2_STABLE_POWER=1, 3+3 alternating, analyser ab_5k_report.py).
- GATE 1 PASSED (13:1x, release exe of 13:0x): policy reports `shared memory 2 blocks, 680 MiB for 795 MiB` (view
  colour alone; glare sources + view/snapshot). Trail window (3010-3029): arm split 0, every relabelling 0 (2
  varying pixel-frames in all). Smoke window (1150-1169): arm split 18 inside the null (other relabellings up to
  31); pooled with the view-class test (same placement, unchanged rendering code) 6 committed vs 6 placed runs: **arm
  split 0**, all 461 other relabellings >= 0 (max 13). Plus 1x: bit-identical (earlier, every class placed).
- GATE 2 first pass (13:2x-13:4x, mem-5k/ab5k-{c,p}-{1,2,3}, 5K fullscreen, diag exe, locked clocks; analyser
  ab_5k_report.py): GPU equal (median p50 3.11 vs 3.09, p99 7.39 vs 7.35, max 12.51 vs 12.53 ms; repeats 3 vs 3,
  drops 3 vs 3, all gpu_late; present p99 17.2 both). Memory equal over most of the run (private steady 1423 vs 1427
  MiB) - FINDING: the view's first texture is made at frame 1 in memory of its own (no glare yet); from frame 146
  the view lives in the glare sources' memory, but the old texture waited out kKeepFrames (60 s). At 62.7 s the
  policy arm dropped default heap 1236 -> 1121, local usage 1268 -> 1153, private 1430 -> 1315 MiB (-115 each, at
  once: freed placed heaps leave commit immediately).
- FIX (in source 13:5x): TexturePool::end_frame releases a placed texture alone in its memory that was unused this
  frame while its description served from larger memory (superseded: acquire always takes the largest free memory).
  render_core_tests graph_releases_superseded_placements (16 MiB view alone, then a 32 MiB texture before it: the
  view moves, old texture + memory released that frame, contents correct): all pass.
- POST-FIX GATES (14:0x-14:3x): pixels gf-g (smoke window, 3+3): arm split 0 (others 0-13). 5K fullscreen
  ab5kf-{c,p}-{1..3} (diag, locked clocks, Westopolis 90 s):

  | median (range) | committed | policy |
  |---|---:|---:|
  | private commit, steady 10-85 s | 1422.6 (1421.3-1424.1) MiB | **1313.6** (1312.1-1313.6) MiB |
  | private commit, max | 1424.1 | 1426.7 (a ~2 s transient at the glare's first frame) |
  | D3D12 local usage, max | 1264.0 MiB | **1154.0** MiB |
  | default-heap blocks, max | 1237.5 MiB | **1122.5** MiB |
  | GPU frame p50 / mean | 3.10 / 3.09 ms | 3.11 / 3.10 ms |
  | GPU frame p99 | 7.24, 7.24, 7.24 | 7.28, 7.31, 7.32 (+0.04..0.08) |
  | GPU max / frames > 12 ms | 12.39 / 3 | 12.56 / 6 |
  | displayed repeats / drops (90 s) | 3 / 3 | 3 / 3 (all gpu_late) |

  Per pass: scene +0.003, glare sources/resolve/rows +0.001 each (ms per frame). The saving is now in place from
  3.1 s (default heap 861 -> 1113, never both copies in D3D12MA). The consistent p99 shift is NOT accepted silently:
  testing whether the placed 4x scene colour (alone in its memory, never shares) costs GPU time - mem-5k/ab_5k_gv.ps1
  (filter glareview = 4x RGBA8 glare sources + 1x RGBA16 view placed, scene colour committed) vs committed, 3+3.
- Scene-colour test (14:5x, ab5kg): only glare sources + view placed (scene colour committed) vs committed: p99
  7.52/7.35/7.31 vs 7.28/7.26/7.33, means 3.089/3.094/3.083 vs 3.092/3.102/3.095. Pooling every committed run of
  the day, p99 spans 7.24-7.43 ms; the policy arm's 7.28-7.32 lies inside it and the means differ by <= 0.01 ms.
  The "consistent +0.06" came from one tight committed triple (7.24 x3): NO measurable GPU-time effect. The tested
  policy is kept (scene colour placed; memory equal either way: 1152 vs 1154 MiB local).
- FINAL HAUNT GATE PASSED (mem-5k/gate_fh.ps1, frames 3000-3019 at 5120x2880, 3+3): same layout (680 for 795),
  arm split 0, every relabelling 0 (4 varying pixel-frames in all), render failures 0.
- FULL CTEST (tests preset, Release, -j 2): 130 / 131. The one failure, f1c_host_stack_integrity_notail (32 loops
  "UNBOUNDED": painted host stack 2472 B after 250k iterations, 2160 B after 1M - a one-off 312 B deeper call early,
  beyond the 64 B slack), is NOT this work: with every file of this work reverted the rebuild recompiled nothing (the
  driver's chain shadow_runtime_common -> shadow_runtime -> f1c_stack_driver_notail.exe contains none of these files;
  lifted_render.cpp is in shadow_render_lifted) and the test failed identically; the failing exe was linked 10:46 from
  the main session's build, which GPU_FILL.md reports 131/131 on. render_core_tests pass after the restore.
  Resolved as environmental (13:5x): the main session ran the same 10:46 exe alone twice: 156/156 bounded. In this
  session's processes (no GEKKO_/SHADOW_ variables inherited, nothing concurrent) it fails with a varying set: a fresh
  --work dir gave 4/156 (xrec_direct_t, eof_fall_t), with the extra 312 B in the 1M measurement instead of the 250k
  one - a one-off asynchronous call on the driver thread at random moments, most likely injected by this session's
  command sandbox. The binary is fine. The main session's full tests build + ctest on the tree with this work and
  its PRS serve-ahead (~15:00): **131 / 131 passed**.
- DELIVERED to H:\NukkoroGit (13:3x, scratchpad deliver_nukkoro.py; backups of the previous NukkoroGit files in the
  scratchpad nukkoro_backup): frame_graph.{h,cpp}, resources.{h,cpp}, renderer.h, lifted_render.cpp whole (those
  copies differed only by this work) + only the stats_.pool_* hunk of renderer.cpp (the rest of source's renderer.cpp
  is other sessions' unvalidated work). NukkoroGit Build.cmd: success (1:35). Its exe at 5K headless: `shared memory 2
  blocks, 680 MiB for 795 MiB`, render failures 0. The NukkoroGit session was told.

## RESULT (2026-10-30, validated)

At 5120x2880 fullscreen, 4x MSAA, R16G16B16A16_UNORM (no quality change): **-115 MiB D3D12 default heap, -110 MiB
D3D12 local usage, -109 MiB process private commit (steady)**, from 3 s into the stage (the glare's first frame).
Pixels: identical at 1x; at 4x no pixel consistent with the arm on Westopolis (smoke and trail windows) and Final
Haunt (3+3 runs each, permutation test against every relabelling). GPU time: no measurable effect (means within 0.01
ms, p99 inside the committed runs' spread). Displayed repeats and drops unchanged (3 / 3 per 90 s, all gpu_late - the
smoke frames, the main session's work). Peak private unchanged (a ~2 s transient when the view moves into the glare
memory). Remaining 5K memory: view colour 452 (4x, whole frame), depth 228, glare sources + view 228, output 57.5,
history 115 - each alive across the others, so no further sharing exists in these frames; only format changes (the
user's decision: none) would shrink them.

Open: smoke trimming (taken over from the main session; design in the block at the top, not started);
f1c_host_stack_integrity_notail (main session).

## Not done, user decisions

- The scene's alpha is constant 1 (docs/effects/screen.md), so a quarter of the 452 MiB view colour stores a
  constant. Any smaller format (R10G10B10A2, R11G11B10F) changes pixels: a quality decision for the user, not taken.
- 4x MSAA on top of a 5120x2880 render shown on a 1080p screen is 64 samples per displayed pixel; the sample count is
  the user's quality setting and is not changed here.
