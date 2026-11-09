# GPU fill: what the remaining frame dips are (2026-10-30)

The user's mission for 2026-10-30: find and fix the remaining frame dips, stalls, loading and CPU / GPU / RAM costs,
the hard way, no fake results. User rule of the same day: rendered tests run at **5K (5120x2880 render) fullscreen**.
Working folder: `.build/validation/perf-2026-10-30/` (run.ps1 = one replay run, headless or `-Rendered` 5K; analysis
scripts fw_report.py, gpu_report.py, gpu_cmp.py, capdiff.py).

## 1. The game thread is not the dip (measured)

New diagnostic: `GEKKO_FRAME_WORK=<csv>` (native_frame_limiter.cpp) logs host time and game ticks at every frame
limiter entry. Headless, the game thread never sleeps, so the time between two entries is that frame's whole
game-thread work (guest code, AX, render packet production).

| tape (headless, release) | frames | p50 | p99 | p99.9 | max (outside the stage start) |
|---|---|---|---|---|---|
| Westopolis, 90 s | 5,538 | 2.05 ms | 3.41 ms | 4.06 ms | 4.64 ms |
| Final Haunt, 330 s | 20,018 | 2.09 ms | 3.55 ms | 4.53 ms | 7.25 ms |

The only frames above 8 ms are frames 32-33, the stage hand-off (16.5 + 16.6 ms Westopolis, 17.3 + 19.5 ms Final
Haunt): one dropped frame at stage start, CPU work. Everything else has 4x headroom.

## 2. The displayed repeats are GPU-late frames

Present trace (GEKKO_PRESENT_TRACE + tools/present_trace_report.py), Westopolis replay, 90 s:
- 1440p windowed: 10 repeats = 8 gpu_late + 2 packet_late (the 2 at ticks 19/24, stage start); 13 drops.
- 5K fullscreen: 7 repeats = 4 gpu_late + 3 packet_late.
Every gpu_late frame is a frame whose `scene` pass took 8-14 ms. Content-driven: the same ticks are heavy at both
resolutions; the heaviest stretch is Westopolis ticks ~1170-1240 (game 19.5-20.7 s).

## 3. Two causes, both measured

**a) The driver keeps the GPU slow.** gpu_clock_sampler.py (NVML) over a 5K run: graphics clock p50 585 MHz (max
2505-3105), memory at half clock (5001 of 11201 MHz), P3, throttle reason "GPU idle" in every sample. The smoke frames
start at 930 MHz and only reach 1740 MHz ~0.4 s later. Every GPU time in an unlocked run is clock-state dependent
(the same scene total at 1440p and 5K; resolves cheaper at 5K).
Measurement fix (diagnostic only, not shipping): `SHADOW_R2_STABLE_POWER=1` calls ID3D12Device::SetStablePowerState
(developer mode): graphics 2205 MHz, memory 11201 MHz; repeated runs then agree within 0.02 ms (below).

**b) One particle effect fills the screen ~26 times.** Per-draw GPU timestamps (new: `GEKKO_GPU_TRACE_DRAWS=<csv>`
with GEKKO_GPU_TRACE; `GEKKO_GPU_TRACE_DRAWS_MIN_MS`, default 8) on the heavy frames: one effect, texture handle
0x010000F1 (a 32x32 smoke puff, 32.5 % of its texels alpha 0, alpha max 146), ~15 batches / ~740 particles per frame,
**11.1 ms of the scene pass** (unlocked clocks). SHADOW_R2_TRACE_FRAME=1182: batches of 40-60 quads 30-240 units wide
at ~100-200 units from the camera (one batch of 40 quads 241 units wide at 176 units: every quad covers the whole
screen), blend SrcAlpha/InvSrcAlpha, depth tested, no depth write, colour alpha 0x0D-0x97.
- MSAA is irrelevant: SHADOW_R2_MSAA=1 10.45 ms vs 4x 10.55 ms for the effect.
- So the cost is per pixel: ~118 Gpixel/s on the 40-quad batch.

## 4. Fix 1 (landed in source, not deployed): fragments of alpha exactly 0 are not drawn (kSkipClear)

For a draw that writes no depth and whose colour blend at source alpha 0 is ZERO x src + ONE x dst (source factor
SrcAlpha or Zero; destination InvSrcAlpha, One or DestAlpha, which this renderer draws as ONE), a fragment whose final
alpha is exactly 0 leaves the target bit for bit as it was (the alpha channel is always ZERO x src + ONE x dst). Every
draw shader now skips it when the CPU sets kSkipClear (frame_math.h; renderer.cpp skip_clear_flag; particles, mesh base
and dual passes, immediates). Exactly 0 only: the existing cut-off test (a <= cutoff) is not reused, because it would
also drop negative alphas, which do change the target. A/B on one binary: SHADOW_R2_SKIP_CLEAR=0 (non-shipping).

Proof of equality, Westopolis frames 1150-1249 (the smoke), 2560x1440 captures, diag build:
- MSAA off: 100 / 100 frames bit-identical.
- 4x MSAA: on vs off 98 / 100 identical, 6 pixels 1 step; two "off" runs differ from each other by 8 pixels 1 step
  (the known NVIDIA MSAA noise).

Cost, 5K fullscreen, locked clocks (SHADOW_R2_STABLE_POWER=1), runs alternated:

| arm | smoke frames: scene pass mean / max | whole run GPU p99 |
|---|---|---|
| off | 8.664 / 12.49, 8.667 / 12.49, 8.684 / 12.44 ms | 12.48, 12.37, 12.52 ms |
| on | 7.949 / 11.91, 7.959 / 11.51 ms | 11.33, 11.37 ms |

-0.71 ms (-8.3 %) on the smoke frames. 4x relabelling test (2026-10-30, the 5K side session's group_split.py: pixels constant inside each arm and
different between arms, against every other split of the same six runs): kSkipClear on x3 vs off x3, frames
1150-1249, SHADOW_R2_TARGET_ALIASING=0 in both: 15,295 varying pixel-frames (MSAA noise), arm split 0, all 9 other
splits 0. No arm-consistent difference. The test only sees pixels that are stable within an arm; the no-MSAA 100/100
identity above covers the rest. (An unlocked-clock A/B earlier showed -5 % inside clock noise.)

## 5. What limits the remaining smoke cost (locked clocks, diagnostic probes; they change the picture)

SHADOW_R2_PARTICLE_PROBE (non-shipping): 1 = no texture sample, 2 = no blending, 4 = no depth test. Smoke frames'
scene pass: baseline 9.21 ms; no texture 9.86; no blending 9.32; no depth test 11.59; all three 11.87. So neither
texturing nor blending limits it: the number of fragments does (no depth test = more hidden fragments shaded). Batch
time follows covered area, not particle count (batches of 50-112 particles cost ~0 when far away). An octagon trimmed
to the texture's non-zero support (bilinear footprint, 1 texel margin) would remove only 11 % of this texture's quad
area (19 % without margin), so geometric trimming is a small lever here.

## 6. The stage-start stall (CPU), attributed

New diagnostics: GEKKO_TASK_TIMES=<csv> (gameplay_scheduler.cpp: every task dispatch and continuation, steady clock)
and GEKKO_SELF_SAMPLE=<csv> (self_sampler.cpp: in-process stack sampler of the game thread, ~1,700 samples/s,
RtlVirtualUnwind; tools/self_sample_report.py names frames from the link map and selects game frames with
GEKKO_FRAME_WORK logs). The external tools/native_sample.py manages ~20 stack samples/s and cannot see a 16 ms window.
Westopolis stage start, 3 runs:
- frame 32 (17.3 ms, outside task executes): 27 % ButtonGlyphs::patch -> sharp_sheet -> resize_rgba (the sharp
  button-glyph sheets, computed on the game thread at their first read; once per sheet per session), 18.5 % guest
  fn_800CAE24, the rest asset copies (NativeFileApi::copy_asset) and guest code.
- frame 33 (16.4 ms): one execute of the resource task (vtable 0x8051D77C, execute 0x800122A0 -> 0x80012A30 on the
  manager singleton 0x8056FF94): 40 % the native PRS decoder (PrsEngine::run / prs_raw, 35 % exclusive).
  597 PRS expansions, 7.05 MB out, in the first 3 s of game time.
The loading screen itself (game seconds 1-3) costs 0.2-0.6 ms of game-thread work per frame: its length is the
game's own sequencing, not compute.

## 7. Stage-start fixes

**a) Sharp button-glyph art made off the game thread (landed, not deployed).** The scaled disc art of a sheet does
not depend on the controller family or the binding, so ButtonGlyphs::attach starts one worker (std::async) that
decodes and scales every sheet found; sharp_sheet uses it (waiting if still running) and pastes the family's cells as
before. No code here changes the FP environment (no MXCSR writes anywhere), so the worker's floats are the game
thread's. Proof: button_glyphs_tests' dump of all 164 sharp sheets (sheet x family x binding) before and after:
byte-identical (`diff -rq`). Westopolis stage start, 3 runs each, frame 32: 18.33 / 17.72 / 16.84 -> 13.99 / 13.15 /
13.60 ms (-4.0 ms mean; the sampler was running in both arms).

**b) PRS match copy: tried, no gain, reverted.** Replacing the per-match memcpy / byte loop in prs_raw with in-bounds
overlapping wide copies passed game_memory_owner_tests (15,109 disc members = 330 MB equal to the host codec, 513
register-exact against the interpreter) but the owner's decode time did not move: 0.563 / 0.560 s before, 0.572 /
0.529 s after (the decoder already runs ~590 MB/s; the copy is not its cost). Reverted: no evidence, no change.

**c) The layout manager's link-group lists, native (landed in source, not deployed).** fn_800CAEAC rebuilt the
lists of all 256 link groups by calling fn_800CAE24 per group, each walking all 2,048 spawn slots (524,288 visits,
~3 ms, 18.5 % of frame 32). NativeLinkGroups (include/gekko/native_link_groups.h has the disassembly and contract)
serves it with one pass; when an entry's fields or the array word could lie on a word the walk writes (proved per
call by range), it runs the original's 256 x 2,048 walk natively in the original's order instead, so it is exact for
every input. Registers left as the original leaves them (r0 = LR, r3, r4 = 0xFF, r5-r8 of the last visit, CTR 0,
CR0 = EQ|SO) and the frame's four words. fn_800CAE24 (one group; 2 callers) stays guest code.
native_link_groups_tests: 12 layouts x 2 seeds against the original words in the reference interpreter (realistic,
shared entries, the last visit's four outcomes, three aliasing layouts): 0 differences in r0-r8, r30, r31, r1, LR, PC,
CTR, CR, XER, the stack and every byte of a 3 MiB arena; 18 calls by the single pass, 6 by the walk. A deliberate
mutation (one write dropped) is caught. extract/native-owners.txt and liveness-roots.txt (+0x800CAEAC, regenerated
with the tool) updated.

## 8. Gates of the landed changes (release build, 2026-10-30)

Control = the previous release (game/ShadowTheHedgehog.result-fix-2026-10-30.exe, SHA 142ac98b...), candidate =
.build/release with kSkipClear, the glyph worker and NativeLinkGroups; headless, 90 s, isolated persistence
(perf-2026-10-30/gate.ps1):
- Westopolis replay: --state-report (every 64 KiB block hash of guest memory at the end) identical, --audio-dump PCM
  byte-identical, frame-3000 capture at 2560x1440 identical; 0x800CAEAC served 3 times.
- Final Haunt tape: the same three identical.
- Stage start (Westopolis, frame log only): frame 32 16.51 -> 8.92 / 8.71 / 8.83 ms; frame 33 16.61 -> 16.15 / 16.45
  / 16.27 ms (unchanged: PRS and resource loading).

- Full suite (tests preset, Release): 131 / 131 passed (incl. native_link_groups_tests, native_liveness_roots,
  native_file_api_tests' owner list, game_memory_owner_tests, button_glyphs_tests).
- 5K fullscreen, Westopolis 90 s, diag build of the same source, present trace, clocks NOT locked (one run each):
  before 7 repeats (3 packet_late, 4 gpu_late), 7 drops; after 6 repeats (0 packet_late, 6 gpu_late), 4 drops,
  0 render failures. The packet_late repeats (stage start) are gone; the gpu_late count moves with the driver's clock
  state (section 3a) and one unlocked run each cannot rank it - the locked-clock A/B of section 4 is the GPU claim.

Note: the binaries of these gates and of the locked-clock A/B (section 4) also contain the other session's
render-target aliasing (docs/RENDER_TARGET_ALIASING_5K.md, default on after 09:30; SHADOW_R2_TARGET_ALIASING=0 for
committed targets). The locked-clock A/B used one binary for both arms (only SHADOW_R2_SKIP_CLEAR differed), so it is
unaffected; the headless gates were identical with it included.

f1c_host_stack_integrity_notail (2026-10-30 afternoon): failed in the 5K side session's suite run (32/156 loops
"UNBOUNDED": a one-off 312 B deeper host stack in one of the N / 4N measurements), and failed there alone too; with a
fresh work dir there 4/156, other loops, the extra 312 B in the other measurement. The same 10:46 driver passes in
this session every time: twice by the exact command (fresh work dirs; depths identical run to run) and once by
`ctest --preset tests -R f1c_host_stack_integrity_notail`. No GEKKO_* / SHADOW_* variables in either environment.
Random loops and either measurement point to something asynchronous on the driver thread in processes that session
launches (its command sandbox is the suspect), not to code. Not proven which mechanism; the test is sensitive to
injected thread activity.

## 9. Stage start frame 33: PRS members decoded ahead of use (landed in source, not deployed)

Attribution at 5K fullscreen (self sampler, 3 runs): frame 33 = 17.7-18.5 ms; PRS decode 37 %, RenderWare stream code
(fn_804586E8, fn_80013C30 / fn_800149F4, fn_80456F68) most of the rest. Headless it also showed 19 % of mapped-file
opens (FileProvider::mapped_slot); that is a headless artefact: the low-priority pre-opener has had ~7 ms of CPU by
then, while at 5K fullscreen the opens are gone (the pre-opener finishes in time).

Two exact decoder micro-changes were tried and reverted for no gain (game_memory_owner_tests' owner decode time over
all 330 MB of disc members): wide match copies (0.563 / 0.560 -> 0.572 / 0.529 s) and whole literal runs per flag
byte (-> 0.600 / 0.582 / 0.576 s). The decoder runs ~1.7 ns per output byte, ~20 cycles a token: the literal / match
branch, inherent to the format.

What landed instead (gekko/prs_ahead.h): when the file API delivers part of a ".one" archive, its member table is
parsed once (mapped file) and every PRS member whose stream starts in the delivered part is queued to two
below-normal workers, which decode it with the owner's own token decoder (prs_decode_ahead, game_memory.cpp). The
expand call looks the decoded stream up by CONTENT (FNV-1a of its first 16 bytes) - RenderWare's file system reads an
unaligned piece through a 32-byte bounce buffer (0x80574DA0) and memmoves it into place (0x8047BF74), so the first
version, keyed by address, served 8 of 597 calls and the version before it (placement = address - offset) queued
191,441 decodes and made the frame 60 ms. GameMemory::serve_prs_ahead serves a call only when no hand-back is due, no
journal is open, the guest's stream bytes memcmp-equal the decoded stream, source and output are host memory and do
not overlap; it writes the output and the register file PrsEngine publishes at the terminator (r0, r3, r4, r7-r10, CR0,
CA; r5 / r6 / CTR only when a match ran, else the caller's). Anything else decodes as before, so the game sees the
same bytes and registers whichever path ran. GEKKO_PRS_AHEAD=0 (non-shipping) turns it off.

Proof:
- game_memory_owner_tests: every PRS member of every ONE archive on the disc decoded ahead and served on a third
  machine, compared with the owner (all 32 GPRs, CR, XER, CTR, LR/PC/MSR, the output, the 64 bytes after it, the source
  block): 15,041 members qualify (68 do not: a copy from before the output or a stream ending within reach of the
  file end), 14,885 served equal, 156 deliberately changed streams refused and decoded equal. PASS.
- Westopolis and Final Haunt replays (90 s, headless, vs the previous release): state report (every 64 KiB guest
  block), audio PCM and capture identical. Served 569 / 625 expand calls; 42 / 62 not decoded yet (decoded normally).
- Stage start, Westopolis: headless frame 33 16.61 -> 13.70 / 13.37 / 13.60 ms; 5K fullscreen frame 33 17.7-18.5 ->
  13.6-14.0 ms, frame 32 9.9-10.7 -> 9.1-9.3 ms (the "before" 5K runs had the sampler attached). Both stage-start
  frames are now inside 16.7 ms.
- Full suite (tests preset, Release, this session): 131 / 131 passed.

## 10. Delivered to H:\NukkoroGit (2026-10-30)

Rule feedback-deliver-to-nukkorogit; the NukkoroGit session confirmed no edits in flight. Copied whole (their
NukkoroGit diff was this session's only): renderer.cpp, frame_math.h, gpu_timer.*, gpu_device.cpp, the particle /
mesh / immediate shaders, native_frame_limiter.*, button_glyphs.*, game_memory.*, native_file_api.*,
gameplay_scheduler.cpp, extract/native-owners.txt, tools/native_sample.py; new: self_sampler.*, native_link_groups.*,
prs_ahead.*, tools/self_sample_report.py. Hunk-merged (other sessions' undelivered work left out: --boot-cutscene,
goal_stage, the draw-distance default, menu backgrounds, 0x8025D810, test targets; NukkoroGit's icon block and tests
guard kept): shadow_main.cpp, system.h/.cpp, CMakeLists.txt (3 source lines), liveness-roots.txt (+0x800CAEAC). Tests
and internal docs not delivered (NukkoroGit ships no runtime/tests). dev\Build.ps1 -Jobs 4: succeeded in 07:48. Its
exe on the headless Westopolis gate: state report identical to the source candidate and to the previous release; PRS
ahead served 570, link groups 3.

## 11. The user's goal (2026-10-30 afternoon): every frame 10 ms or below, exact

Where it stands (measured):
- CPU, headless game-thread work per frame (both tapes): every frame <= 7.3 ms except the stage start. Stage start
  at 5K fullscreen: frame 32 8.6-9.9 ms, frame 33 12.7-13.75 ms (7 runs; with PRS ahead queuing every member of an
  archive from its first read, 4 workers: not-ready expand calls 47 -> 6-8; before that 13.6-14.2 ms). Gate:
  Westopolis state report and capture identical.
- GPU at 5K fullscreen, locked clocks (SHADOW_R2_STABLE_POWER=1), diag build of current source, 90 s each:
  Final Haunt max 4.45 ms (0 frames > 10 ms); Westopolis p99 7.25, max 12.83 ms, 20 frames > 10 ms, all the smoke
  (ticks 1184-1208). The displayed repeats left (ticks 13-27, 94; Final Haunt 13-23) are the boot screens before
  gameplay. With the driver's own clocks (585 MHz typical) GPU times are ~2x these; that clock policy is the driver's.
- Frame 33 after PRS ahead (5K sampler, 4 runs): PRS 20 % (members read in the same frame they are expanded),
  RenderWare stream deserialization ~32 % spread over fn_804586E8 / fn_8045AA18 / fn_80456F68 (RwStreamRead
  fn_8047DFA0 6 %, the chunk reader fn_80481D9C 8 %, nothing larger), ResourceOneFile fn_800149F4 -> fn_80013C30
  20 %. No single owner: below 10 ms needs the RenderWare stream readers native (geometry, materials, texture
  dictionaries) with original-word oracles.
- Smoke: needs ~-2.9 ms (~26 % of its fill) on the worst frame; exact trimming gives ~11 %.

## 12. Goal changed (user, 2026-10-30): 3 ms and below. The scene pass in screen tiles

Census against 3 ms (locked clocks, 5K fullscreen): GPU Westopolis 62 % of frames over 3 ms (p50 3.13), Final Haunt
9 % (p50 2.34); the scene pass is 2.31 ms of Westopolis' 3.16 ms mean frame. CPU (headless game-thread work): ~5-6 %
of frames over 3 ms (Westopolis 303 / 5,538, Final Haunt 1,166 / 20,018) plus the stage start.

What limits the typical scene pass (locked clocks, ordinary frames, ticks 300-1700 without the smoke):
- per draw: meshes 1.41 ms of 1.66 (particles 0.18, immediates 0.07); every mesh draws SrcAlpha/InvSrcAlpha + z write;
- probes (SHADOW_R2_PARTICLE_PROBE 8 = meshes unblended, 16 = meshes untextured): 1.786 / 1.778 -> 1.772 / 1.780
  and 1.701 / 1.710 ms: neither blending nor texturing limits it;
- render size: 2560x1440 GPU mean 0.505 ms (scene 0.281) vs 5120x2880 2.662 ms (scene 1.785): 5.3x / 6.4x for 4x
  the pixels. Super-linear: at 5K the 4x targets (depth 236 MB, colour 472 MB) are far beyond the 64 MB L2.

Prototype (diagnostic, SHADOW_R2_SCENE_TILES=n, non-shipping; renderer.cpp scene_tiles): each draw pass recorded once
per tile of an n x n grid, every scissor (full, canvas, region) intersected with the tile. Per pixel the work and
its order are unchanged. Locked clocks, Westopolis 30 s, typical frames:

| grid | scene ms | GPU mean | GPU p50 | GPU p99 | smoke scene / max frame |
|---|---|---|---|---|---|
| 1x1 | 1.806, 1.807 | 2.661, 2.674 | 2.509, 2.521 | | 7.93 / 12.3-12.4 |
| 2x2 | 1.179, 1.186 | 2.101, 2.114 | 2.009, 2.019 | | 7.15-7.27 / 11.7-11.8 |
| 3x3 | 0.820, 0.824 | 1.608, 1.636 | 1.564, 1.597 | | 6.92-6.96 / 11.47 |
| 4x4 | 0.821 | 1.599 | 1.564 | 2.661 | 6.96 / 11.60 |
| 5x5 | 0.860 | 1.650 | 1.622 | 2.678 | 6.95 / 11.32 |
| 6x6 | 0.931 | 1.721 | 1.669 | 2.938 | 7.12 / 11.64 |
| 8x8 | 1.125 | 1.940 | 1.839 | 3.175 | 7.21 / 11.87 |

Exactness:
- MSAA off (SHADOW_R2_MSAA=1), tiles 4 vs 1: 100 / 100 frames bit-identical at 2560x1440 (frames 1150-1249), 20 / 20
  at 5120x2880 (1180-1199).
- 4x MSAA (default), 3 runs per arm, group_split.py: NOT identical. 2560x1440 100 frames: 2,210 pixel-frames constant
  in each arm and different between them, every other split 0; 5120x2880 20 frames: 321, others 0. Steps: 2,207 x 1,
  3 x 2. Not at tile edges (1-4 % within 2 px of one), uniform in x and y mod 8. The same kind of effect the 5K
  session found for placing the 4x depth (RENDER_TARGET_ALIASING_5K.md): how the GPU's 4x memory blocks are filled
  changes its rounding on a few pixels. Not landed as a default: a 1-step change on ~22 px per frame at 1440p is a
  picture change; the user decides.

## 13. Goal changed again (user, 2026-10-30 evening): extreme 32:10 resolution, 1 ms complete GPU frame

The directive is in docs/OPTIMIZATION_GOAL.md ("CURRENT GOAL"). State at the change:
- The 3 ms work above (section 12): the tiled scene pass is a diagnostic only (SHADOW_R2_SCENE_TILES, default 1,
  shipping ignores it). The user did not accept the 4x 1-step difference; under the new directive every deterministic
  mismatch is investigated, so its cause must be found (the 4x memory-block fill order is the hypothesis, not proven).
- 32:10 on the recorded tapes: every tape (Westopolis, Final Haunt, Lethal Highway, the user's ramp) is 16:9 and its
  output records pin the game's output - asking 5120x1600 rendered 2844x1600 (16:9; session report "family 16:9,
  family_exact false"). The game's output shape is game-visible (camera window, screen tests), so a 32:10 benchmark
  needs either a new 32:10 recording or proof that these inputs replay the same gameplay at 32:10.
- New development option (shadow_main.cpp): `--tape-output-free` - the tape's inputs play, its output records do not
  decide the output, which then follows the exact render size. Next: run the Westopolis tape at 16:9 and at 32:10 with
  it and compare the 64 KiB guest blocks over the run (the audit found 4-5 of 385 blocks differ between aspects
  without a tape: the camera window); only if gameplay is shown identical does the tape count as 32:10 real play.
- Resolution ladder to test (all exact 32:10; D3D12 caps a texture side at 16384): 3840x1200, 5120x1600, 6400x2000,
  7680x2400, 10240x3200, 12800x4000, 16384x5120. Each: full 90 s replays, fullscreen (letterboxed on the 1920x1080
  monitor: presentation scaling only), present + GPU traces, GPU memory, render failures / device removal.

32:10 replay validity (done): with --tape-output-free the tape's inputs play and the output follows the exact render
size. 90 s headless, 2560x1440 vs 5120x1600: Westopolis PCM byte-identical, SE manager counters identical (660
requests, 659 started, 8,436 kept alive, 649 expired, 28 rings), 15 of 510 guest 64 KiB blocks differ; Final Haunt
PCM byte-identical, 7 of 500 blocks differ. Same-aspect sizes (3840x1200 vs 5120x1600): 0 blocks differ; the free
16:9 replay vs the normal one: 0 differ. Every sound event (rings, hits, enemies, voices) at the same tick means the
same gameplay; the differing blocks are the game's aspect state (camera window, screen tests). So these tapes are
real 32:10 gameplay with --tape-output-free (scripts: perf-2026-10-30/aspect.ps1, ladder.ps1).

## 14. The 32:10 resolution ladder (2026-10-30)

Westopolis replay, --tape-output-free, borderless fullscreen on the 1920x1080 60 Hz monitor (the 32:10 picture is
letterboxed: presentation scaling only; render sizes as reported by the renderer and the session report), diag build
of current source, present + GPU traces (perf-2026-10-30/ladder.ps1, ladder_report.py). Frames from tick 300.

Stability, driver clocks (as a player runs it), 90 s each - every size: exit 0, 0 render failures, no device removal:

| render (32:10) | GPU p50 / p90 / p99 / max ms | VRAM max | private peak | repeats (gpu_late) | dropped packets |
|---|---|---|---|---|---|
| 3840x1200 | 6.56 / 14.25 / 17.44 / 20.67 | 418 MiB | 583 MiB | 142 | 425 |
| 5120x1600 | 5.51 / 6.71 / 8.97 / 14.51 | 679 MiB | 905 MiB | 3 (boot) | 2 |
| 6400x2000 | 5.56 / 6.36 / 7.83 / 17.11 | 1,028 MiB | 1,292 MiB | 6 | 4 |
| 7680x2400 | 5.77 / 6.63 / 8.49 / 16.97 | 1,428 MiB | 1,733 MiB | 3 | 3 |
| 10240x3200 | 5.78 / 8.77 / 10.81 / 20.95 | 2,444 MiB | 2,860 MiB | 7 | 17 |
| 12800x4000 | 9.09 / 14.14 / 17.61 / 52.60 | 3,820 MiB | 3,992 MiB | 48 | 166 |
| 16384x5120 | 14.70 / 20.23 / 27.91 / 114.93 | 6,150 MiB | 6,449 MiB | 115 | 1,453 |

16384 is D3D12's limit for a texture side (D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION; the renderer refuses more), so
16384x5120 is the largest 32:10 size a single render target can have; it runs without failure but does not sustain
60 Hz. With the driver's clocks the smallest size is the SLOWEST (3840x1200, 142 repeats): the lighter the load,
the lower the clock the driver picks; the GPU times above are clock-state dependent.

GPU work, locked clocks (SHADOW_R2_STABLE_POWER=1, 2205 MHz), 45 s each:

| render | GPU p50 / p90 / p99 / max ms | VRAM | repeats / drops |
|---|---|---|---|
| 3840x1200 | 0.668 / 0.801 / 2.804 / 3.724 | 418 MiB | 3 / 2 |
| 5120x1600 | 1.592 / 1.841 / 4.694 / 6.231 | 679 MiB | 2 / 1 |
| 7680x2400 | 4.050 / 4.697 / 10.575 / 13.600 | 1,427 MiB | 3 / 3 |
| 10240x3200 | 5.405 / 9.033 / 14.391 / 25.046 | 2,443 MiB | 7 / 24 |
| 12800x4000 | 9.093 / 15.608 / 25.261 / 59.220 | 3,820 MiB | 52 / 185 |
| 16384x5120 | 13.507 / 21.229 / 36.455 / 124.228 | 6,150 MiB | 53 / 710 |

VRAM includes the 5K session's render-target aliasing (the resolved view lives in the glare sources' memory; at
16384x5120 that saves ~640 MiB against committed targets; the other large targets are alive at the same time).

3840x1200 meets 1 ms at p50 and p90 (work); its p99 / max are the smoke. Cost grows faster than pixels (3840x1200 ->
5120x1600: 1.78x pixels, 2.38x time), the cache effect of section 12.

Picture at the maximum: headless capture at 16384x5120 (cap3.ps1: tape inputs, output = render size), Westopolis
frame 3000: correct 32:10 Hor+ framing, HUD anchored at the edges, no corruption in a 2048x640 crop at native pixels
(perf-2026-10-30/max-16384x5120-preview.png, max-16384x5120-crop-1to1.png).

## 15. Phase II (user, 2026-10-30 evening)

Directive: docs/OPTIMIZATION_GOAL.md "CURRENT GOAL, PHASE II". Order of work (highest measured impact first): (1)
the 4x tiled-scene difference (the 40 % GPU lever at high resolution), (2) smoke, (3) the driver clock behaviour,
(4) CPU and the native RenderWare loaders. Commits: once the suite and gates pass, as separate milestones.

## 16. The 4x tiled-scene difference: controlled experiments (Phase II item B, in progress)

Method throughout: Westopolis frames 1150-1249 at 2560x1440 (headless captures, cap2.ps1), 3 runs per arm,
group_split.py (pixels constant within each arm and different between them, against every other split of the same
six runs = the noise baseline; every baseline below was 0). Tiled = SHADOW_R2_SCENE_TILES=4. New diagnostics in
renderer.cpp (non-shipping): SHADOW_R2_SCENE_TILES_PASS=<pass>, SHADOW_R2_SCENE_TILE_ALIGN=<px>,
SHADOW_R2_DRAW_LIMIT=<n> (each draw pass issues its first n draws; logs the last one per pass as [draw-limit]),
SHADOW_R2_DEPTH_S8=1 (scene depth as D32_FLOAT_S8X24), SHADOW_R2_PARTICLE_PROBE 8 / 16 (meshes unblended /
untextured); scripts draw_bisect.py, splitmap.py.

| experiment | arm split (pixel-frames) | reading |
|---|---|---|
| tiled vs untiled, all draw passes | 2,201 (2,198 x 1 step, 3 x 2) | the difference to explain |
| tile only the scene pass | 1,702 | both passes contribute ... |
| tile only the glare-sources pass | 555 | ... and that pass writes NO depth (read-only depth view) |
| tiles aligned to 256 px vs untiled | 15,794 (15,282 in frame 1158) | partition-dependent; frame 1158's blob is the heat haze next to the fire spreading small differences through its snapshot |
| aligned 256 vs default tiling | 17,507 | two tilings differ from each other too |
| position of the differences | 0 % within 4 px of a tile edge; uniform in x, y mod 8 | not tile edges, not block alignment |
| bisection of the draw count (frame 1150) | first at 29 draws: 5 px (28 draws: 0) | draw 28 of the scene pass: mesh 0x01000028 sub 13 (69 indices) after sub 12 (a 6-index quad), SrcA/InvSrcA + z write; the glare pass's draw 28: an additive mesh, depth-tested |
| depth as D32_FLOAT_S8X24, tiled vs untiled | 2,164 | the same with another depth storage format |
| D32_FLOAT_S8X24 vs D32_FLOAT, both untiled | 15,306 | the depth storage format alone changes the picture |
| GEKKO_DEPTH_TEST=less map (coplanar ties) vs the mismatches | ties: 10 pixel-frames; mismatches inside or within 2 px: 0 | not depth ties |
| meshes unblended in both arms | 4,443 | not mesh blending |
| meshes and particles unblended in both arms | 14,876 | not blending |
| edge-ness of the mismatch pixels (60 frames) | gradient median 1.0 (random 0.3); 5 % strong edges (random 2 %) | smooth regions, not MSAA edge pixels |

Excluded so far: tile-edge effects, block alignment, depth ties, one depth format's compression, blending, edge
coverage. Remaining candidates (not yet tested): the 4x targets' storage state as seen by later SHADER READS (the
resolve / glare capture / snapshot passes Load() samples), texture LOD derivatives, the glare tile marks. Next: a
per-sample dump of the 4x colour and depth after the scene pass (compute Load of every sample to a readback buffer)
to find whether the stored samples differ (render side) or only what later passes read (read side).
Not enabled: SHADOW_R2_SCENE_TILES stays a diagnostic until this is explained.

## 17. Phase III item 1: the tiled 4x difference at the sample level (in progress)

Directive: docs/OPTIMIZATION_GOAL.md "CURRENT GOAL, PHASE III". Tools added (renderer.cpp / utility.hlsl, all
non-shipping): SHADOW_R2_DEPTH_BITS=1..4 (the view shows stored 4x depth sample n-1's low 24 float bits; shader
PSDepthBits), the existing SHADOW_R2_RESOLVE_LOW_BYTE (1 = the 16-bit resolved mean's low byte, 2..5 = colour sample
0..3 alone), SHADOW_R2_DRAW_LIMIT with its [draw-limit] description, draw_bisect.py (honours the environment).

| experiment (frame 1150 unless stated; 2-3 runs per arm, arm split vs every other split = 0) | result |
|---|---|
| 16-bit resolved mean, tiled vs untiled, 40 frames | 125,924 pixel-frames differ (8-bit showed 2,201: most differences are below the 8-bit step) |
| 16-bit colour sample 0 alone, 40 frames | 150,563: the STORED colour samples differ (render side, not the later reads) |
| 1x MSAA, 16-bit mean, tiled vs untiled, 40 frames | 0: the partition matters only at 4x |
| bisection at 16-bit (sample 0) | first divergence when draw 6 is included (6 draws: 0 px, 7 draws: 4 px): draw 6 = mesh 0x01000002 sub 3 (126 indices), SrcA/InvSrcA + z write |
| the 4 pixels | (1280,1257) (1281,1258) (1281,1292) (1280,1293): diagonal pairs; every one has differing samples (partially covered edge pixels; 0.21 % of the frame's pixels are such) |
| stored depth, samples 0-3 (low 24 bits), 64 draws | 0 differing pixels (the dump holds 3.37 M distinct values): depth, depth compression and depth ties are NOT the cause; the same triangles win the same samples |
| draw 6 with mesh blending off (probe 8) | the same 4 pixels: not blending |
| draw 6 with mesh texturing off (probe 16) | still 6 pixels differ: not texture sampling |
| centroid interpolation in every draw shader, 16-bit mean, 40 frames | 197,004 differ: not extrapolation outside the triangle |
| (withdrawn) scissor right edge pulled in by 1 px, untiled | confounded: it removes a column of the scene, and the glare capture (sums every source row) and the heat haze spread that everywhere - it changed 11,323 pixel-frames even at 1x; the switch was removed |

What is established: at 4x MSAA, with identical coverage and depth, the colour the GPU writes to some partially
covered edge pixels depends on how the pass is partitioned (scissor tiles); at 1x it does not. Blending, texturing,
depth and extrapolation are excluded. Not yet known: the hardware mechanism (candidates: how the rasterizer
distributes a draw's work over its units when a scissor splits it, and with it which triangle's shading reaches a
sample of an edge pixel when two triangles of one draw both cover it). Next: a standalone D3D12 reproduction (an
interpolated-colour mesh into a 4x target, full scissor vs quadrants, samples read back) to isolate it outside the
game, and attribute-interpolation variants there. SHADOW_R2_SCENE_TILES stays off.

## 18. ROOT CAUSE of the tiled 4x difference: the GPU's blend unit on 4x edge pixels (proven, 2026-10-30)

In the game (section 17, continued): tiling only one scene pass at a time, at the bisected point (7 draws, frame
1150, 16-bit sample 0): pass #0 (the world meshes) 4 px, passes #1-#3 0 px; tiling only the glare-sources pass 0 px.
New diagnostic: SHADOW_R2_SCENE_TILES_ORDINAL=k (tile only the k-th scene / glare-sources pass of a frame).

Standalone reproduction (tools/gpu_repro/msaa_partition.cpp, built with cl from a VS 2026 developer environment into
.build/gpu_repro): a 96 x 96 grid of jittered edge-sharing triangles (18,432) with interpolated colours into a
1024x640 R16G16B16A16_UNORM target + D32 (GREATER_EQUAL, writes on), one draw under a full scissor vs the same draw per
tile of a 4 x 4 scissor grid; every sample read back by a compute shader. RTX 4080:

| variant | whole vs identical resubmission | whole vs tiled | blend unit on vs off (RGB) |
|---|---|---|---|
| 4x, blend unit OFF (also flat colour, isolated triangles, 2x2 tiles, huge perspective triangles) | 0 | 0 | - |
| 4x, blend ON One/Zero (an identity: must equal a plain write) | 24-70 samples | 242-286 samples in 61-72 px, all edge pixels | 3,452 samples |
| 4x, blend ON SrcAlpha/InvSrcAlpha | 136 | 860 (216 px, 215 edge) | - |
| 4x, One/Zero, huge perspective triangles | 0 | 7,565 (2,112 px, all edge) | 1,808 |
| 1x, any blend | 0 | 0 | 0 |

Conclusion: with the blend unit enabled on a 4x MSAA target, the colour this GPU stores in partially covered (edge)
pixels is not the mathematical blend - an identity blend does not return the source - and it depends on how the
draw's work is scheduled (scissor partition, even an identical resubmission). With the blend unit off the result is
exact and partition-invariant; at 1x the blend is exact. Tiling does not introduce an error: it changes the scheduling
the blend unit's edge-pixel result depends on. Every earlier exclusion stands except "not blending": the game's probe
8 set One/Zero factors but kept the blend unit enabled, which is exactly the inexact case.

Consequence for an exact tiled renderer (proposal, not implemented): (1) draw with the blend unit OFF every draw whose
blend is provably the identity on every fragment - factors One/Zero, or SrcAlpha/InvSrcAlpha (or SrcAlpha/One with a
zero destination factor contribution...) when the fragment alpha is provably 1 (texture with no alpha below 255,
material and vertex alpha 1, no fade): exact, partition-invariant, and cheaper (no destination read); this changes
today's output only on edge pixels, where today's value is the blend unit's inexact one; (2) keep genuinely
transparent draws (alpha < 1) untiled in their order (a pass split into tiled and untiled segments), since their
edge-pixel result depends on scheduling; (3) re-measure the gain and the equality (tiled vs untiled under the same
rules). Needs the user's acceptance of (1)'s change on edge pixels relative to today's output.

In-game confirmation (2026-10-30 18:20-18:45, Westopolis frames 1150-1189, 2560x1440, 16-bit colour sample 0
(SHADOW_R2_RESOLVE_LOW_BYTE=2), 2 runs per arm, group_split.py, every other split 0). New non-shipping switches:
SHADOW_R2_IDENTITY_BLEND_OFF=1 (a One/Zero recipe gets BlendEnable FALSE), SHADOW_R2_PARTICLE_PROBE bits 32
(immediates One/Zero) and 64 (no mesh extra passes). Probe 42 = meshes + particles + immediates One/Zero.

| arms (both arms identical except SHADOW_R2_SCENE_TILES=4) | tiled vs untiled, pixel-frames |
|---|---|
| reference: normal blending (section 17) | 150,563 |
| probe 42 + extra passes off (106), blend unit ON (One/Zero factors) | 19,563 |
| probe 42, blend unit OFF, extra passes (additive One/One) still drawn | 2,763 (box 1035,584-1533,1245) |
| probe 106, blend unit OFF | **0** (0 varying pixel-frames at all) |

So in the game itself: with the blend unit off, the tiled 4x scene is bit-exact against the untiled one; every
remaining difference comes from draws drawn with the blend unit enabled (identity or genuine). The 4x tiled
mismatch is fully explained. Exact unlock path: blend unit off for provably-identity draws (opaque base passes),
and genuinely blended draws (alpha < 1, additive extra passes, particles) either untiled or accepted as the GPU's
scheduling-dependent edge result (it already differs between identical untiled submissions in the repro). Decision
for the user. Captures: .build/validation/perf-2026-10-30/ib-*, ix-*, ic-*.

## 19. Exact-pixel architecture, part 1: the mechanism dissected (2026-10-30, directive "Exact-Pixel Rendering Architecture Redesign")

Directive: docs/OPTIMIZATION_GOAL.md "EXACT-PIXEL ARCHITECTURE". Tool: tools/gpu_repro/msaa_partition.cpp (build:
.build/gpu_repro/build.cmd), new switches --format unorm16|unorm8|float16|float32|rgb10a2, --mask-rgb, --no-depth,
--per-sample, --ps psc|psd|psz|pse|psr|psu, --uav, --split n, --shuffle, --gap n, --time n. Every run reads back every
sample (RGBA, 16-bit) and compares blend-on to the same draw with the blend unit off (the specified result).

WHAT the blend unit stores (1024x640, 4x, 18,432 edge-sharing triangles, blend One/Zero = an identity):

| measurement | result |
|---|---|
| where blend-on differs from the plain write | only edge pixels (3,460 samples in 868 pixels; full pixels 0) |
| what it stores there | ALL FOUR samples one value = the MEAN of the plain samples (868 of 868 pixels, +-1 LSB); e.g. plain 36b8,36b8,3634,3634 -> 3676 x4 |
| the pixel's samples before: common leading bits | no threshold (collapsed pixels with 0..15 common bits): not a colour tolerance |
| format | unorm16 868 px, float16 31,897, rgb10a2 65,845, unorm8 123,909 (of ~47k-178k edge pixels), **float32 0** (also 0 run-to-run, 0 tiled) |
| write mask RGB, no depth test, per-sample shading (SV_SampleIndex), SV_Coverage output, discard, SV_Depth output, [earlydepthstencil], UAV-capable target, UAV side effect | unchanged (858-886 px): not the shader, not depth, not compression flags |
| ROV in the pixel shader (unorm8) | 569 px instead of 122,268: fewer, not none |
| triangles of the grid drawn as TWO draws by parity (every neighbour in the other draw) | **0** (unorm16 and unorm8: blend-on == plain, tiled == whole, run-to-run 0) |
| one draw, triangles shuffled (neighbours far apart in the stream) | untiled 1,523 samples (from 463k, unorm8); tiled 91,684 |
| isolated vertices (no index sharing); degenerate triangles between | still collapses: not vertex reuse |

MODEL (all measurements agree): with the blend unit enabled, this GPU coalesces fragments of the SAME draw that
cover disjoint samples of one pixel and are in flight together into one fragment whose colour is their coverage
mean, written to every sample. Which fragments meet depends on timing: the stream order (shuffle), the scissor
partition (tiles), even an identical resubmission (24-180 samples). Different draws never coalesce. Blend unit off:
never. float32 targets: never.

The in-game reference is therefore not a fixed image. Westopolis 1150-1189, 2560x1440, untiled, normal rendering, 6
identical runs (3 driver clocks, 3 locked): 192 pixel-frames vary between runs; the clock split explains 27 of them,
other splits 1-5 (run-to-run variation independent of the clocks). No renderer can be bit-identical to a reference
that differs from itself; the reproducible target has to be the SPECIFIED per-sample result, which the blend-off
path produces.

## 20. Exact-pixel architecture, part 2: the first differing operations fixed, the blend arithmetic modelled (2026-10-30)

A. COALESCING IS TIMING, NOT THE DRAW. One draw per triangle (18,432 direct draws, or one ExecuteIndirect with
18,432 records) still varies between identical submissions (720 / 412 samples, SrcA/InvSrcA): back-to-back draws
coalesce too. The parity split was clean because neighbours were thousands of triangles apart in time. Any
fixed-function blended path therefore stays scheduling-dependent on this GPU; only the blend unit OFF (or a
programmable blend) is exact. Per-triangle draws cost: whole 0.069 ms (same as one draw) but tiled 0.47-0.75 ms.

B. THE EXACT OPAQUE WRITE (renderer.cpp, default ON, SHADOW_R2_EXACT_OPAQUE=0 for A/B): a mesh base pass whose blend
reduces to the identity is drawn with the blend unit off and RGB-only writes (the scene's alpha is ZERO x src + ONE x
dst, so masking alpha keeps it). Proof data per asset: GpuTexture::opaque (every texel of every level alpha 255,
computed in make_texture), MeshResources::opaque_colours (every vertex alpha 255, make_mesh). Recipes: One/Zero (any
alpha) or SrcAlpha/InvSrcAlpha (and DestAlpha / InvDestAlpha, drawn as ONE / ZERO) when the alpha is exactly 1.

C. FIRST DIFFERING OPERATION FOUND AND FIXED: the fragment alpha of "opaque" draws was NOT exactly 1.
SHADOW_R2_OPAQUE_CHECK=1|2|3 (magenta where the fragment / interpolated vertex / filtered texel alpha != 1): the
interpolated vertex alpha missed 1.0 in practically every scene sample (64.98 M of 40 frames' pixel-frames: the
hardware's interpolation of a constant 1 gives 1 - e), the filtered texel alpha of all-255 textures in 20,895
(filter weights). The game's math is alpha = C0.a x v.a x t.a, exactly C0.a when every vertex and texel alpha is 255;
the reference's 1 - e alpha made SrcA/InvSrcA write s*a + d*(1-a) instead of s. Fix (kExactAlpha, default ON,
SHADOW_R2_EXACT_ALPHA=0): such a base pass takes its alpha from the material constant. After it: the check adds 0
magenta pixels (79 = 79, the scene's own colour, in both arms) - every plain-written fragment has alpha exactly 1.
In-game: exact opaque write vs the same frames through the blend unit (both exact alpha): 133,207 pixel-frames
(sample 0, 40 frames), each arm 3 runs with 0 run-to-run variation - the coalescing the plain write removes.

D. THE BLEND UNIT'S ARITHMETIC (R16G16B16A16_UNORM), MODELLED EXACTLY: tools/gpu_repro/blend_arith.cpp (1x - no
coalescing - random destination, the exact source captured in an R32G32B32A32_FLOAT target, every channel compared;
BLEND_ARITH_DUMP=<file> dumps s, a, d, r). Fitted model, 0 mismatches in 5,898,240 channels (3 x 1,966,080):

    q16(f)  = (floor(clamp(f, 0, 1) * 2^20) * 65535 + 2^19 - 1) >> 20      // 20 fraction bits, x*65535 ties DOWN
    S = q16(s), A = q16(a), D = stored unorm16
    SrcAlpha / InvSrcAlpha:  r = min((S*A + D*(65535 - A) + 32767) / 65535, 65535)   // ONE rounding of the sum
    One / One:               r = min(S + D, 65535)
    SrcAlpha / One:          r = min((S*A + 32767) / 65535 + D, 65535)

Rejected on the way (mismatches out of 1.97 M): fp32 s*a + d*(1-a) 280,888; fp64 280,889; s, a unorm16-rounded first
47,861; per-product rounding 490,503; ties-to-even conversion 4. This makes a programmable blend that reproduces the
blend unit bit for bit (without its coalescing) possible: the next prototype.

## 21. Exact-pixel architecture, part 3: programmable blending prototyped (2026-10-30)

tools/gpu_repro/exact_blend.cpp (build .build/gpu_repro/build_exact.cmd): L full-screen layers of jittered
edge-sharing triangles, random colours and alphas, SrcAlpha / InvSrcAlpha over a cleared 4x R16G16B16A16 target.
Ground truth: every layer alone with the blend unit off into an R32G32B32A32_FLOAT 4x target (the exact per-sample
source; float32 never coalesces), then the section 20 D integer model per sample on the CPU, in layer order.
ROV path: the pixel shader blends itself (q16 and the one-rounding blend in 32-bit integer HLSL - the q16 split
x = 16h + l verified equal to the 64-bit form for all 2^20 + 1 inputs) into a RasterizerOrderedStructuredBuffer
holding the 4 samples of each pixel; SV_Coverage picks the samples.

| 4x, SrcA/InvSrcA, samples differing from the model | whole | identical resubmission | tiled 4x4 |
|---|---|---|---|
| fixed function, 1024x640, 8 layers x 8,192 triangles | 155,132 | 155,169 | 158,890 |
| fixed function, 2560x1600, 8 layers x 512 (large) triangles | 1,230,770 | 1,230,770 | 1,242,882 |
| ROV linear / ROV 8x8 blocks / ROV blocks + software compression, both sizes | **0** | **0** | **0** |

| GPU ms (median) | fixed whole | fixed tiled | ROV linear | ROV 8x8 | ROV 8x8 + compression |
|---|---|---|---|---|---|
| 1024x640, small triangles | 0.40 | 0.41 | 1.01 / 1.18 tiled | 1.01 / 1.18 | 0.87 / 1.09 |
| 2560x1600, large triangles (fill-bound, like smoke) | 0.65 | 0.49 | 7.19 / 5.14 | 7.16 / 5.15 | 7.35 / 2.62 |

Software compression = sample 0's alpha bits (the scene's alpha never changes) flag a uniform pixel: a fully covered
fragment over it reads and writes 8 bytes. Verdict: the programmable blend IS the exact, deterministic,
partition-invariant blend (0 differences everywhere), but 4-11x slower than the blend unit where fill dominates: it
fails acceptance criterion 2 as a replacement for the blended draws.

Every blend-enabled fixed-function path coalesces (msaa_partition, unorm8, ~120k pixels each): One/Zero, dual-source
SRC1_ALPHA / INV_SRC1_ALPHA with o1.a = 1, BLEND_FACTOR (1,1,1,1) / ZERO, SrcAlpha / InvSrcAlpha with alpha forced to
1. On this GPU an exact colour write through the ROP exists only with the blend unit off.

## 22. Exact-pixel architecture, part 4: where the cost is, what is exact, the fork (2026-10-30)

Benchmark of the landed exact opaque write (+ exact alpha), Westopolis replay, 5K fullscreen, locked clocks
(SHADOW_R2_STABLE_POWER=1), 2 runs per arm, same diag binary, 5,356 frames each:

| arm | frame GPU p50 / p95 / p99 / max (ms) | scene mean (ms) | resolve |
|---|---|---|---|
| SHADOW_R2_EXACT_OPAQUE=0 (blend unit) | 3.11 / 3.99 / 7.25 / 12.38 and 3.10 / 3.94 / 7.24 / 12.31 | 0.848 / 0.844 | 0.289 / 0.285 |
| exact opaque write (default) | 3.10 / 3.96 / 7.27 / 12.78 and 3.11 / 3.97 / 7.28 / 12.78 | 0.848 / 0.848 | 0.286 / 0.287 |

Cost-neutral (within run-to-run spread). CTest (tests preset, Release): 131 / 131 passed (that preset builds without
the lifted renderer: the rendering proof is the capture gates of sections 19-20, not CTest).

Where the GPU time is (GEKKO_GPU_TRACE_DRAWS at 5K, locked; the trace's c bit 16 now marks exact-opaque draws;
.build/validation/perf-2026-10-30/draw_classes.py): scene particles 90.0 % of the traced draw time (1,195 draws),
exact-opaque meshes 9.1 % (12,296 draws), immediates 0.1 %, blended meshes 0.1 %, glare-source particles 0.7 %.
Heaviest frames (1203-1206): ~12 ms, of which particles ~11.0 ms, opaque meshes 0.87 ms.

So exactness AND speed are decided by the particle renderer:
- fixed-function blending: fast, but coalesces (inexact, timing-dependent) - no state avoids it (sections 19, 21);
- ROV programmable blend: exact everywhere, 4-11x slower for fill-heavy content (section 21);
- a tile-local COMPUTE particle renderer (a tile's 4x samples on chip through all layers, API order) would remove
  the 26-layer DRAM round trips that tiling showed cost ~40 %; but to equal the hardware-shaded reference it must
  reproduce the hardware's attribute interpolation bit for bit. Fit (tools/gpu_repro/blend_arith.cpp
  BLEND_ARITH_VERTS dump, 1x, w = 1, 16.8 snapped vertices, D3D top-left rule, 655,360 pixels, 2,621,440 channels):
  plane form c0 + b1 (c1 - c0) + b2 (c2 - c0) fp32 2,048,976 differ; weights form 2,025,049; fma 2,092,576; exact
  double 2,112,314; at best 77 % within 1 ulp, max 3,505 ulp. The interpolator is not an IEEE formula: matching it
  needs its own reverse engineering (open, no guarantee).

Reference status: the old reference varies between identical runs (section 19: 192 pixel-frames over 6 runs). With
the exact opaque write + exact alpha, 3 + 3 identical runs varied 0 (sections 19-20), but particles still go through
the blend unit, whose coalescing is timing-dependent by construction.

## 23. RTX 4080 attribute interpolator: reverse-engineering evidence (2026-10-30; STOPPED by directive, kept as evidence)

Directive change (user, same night): no NVIDIA-specific renderer; a vendor-independent exact specification instead
(docs/OPTIMIZATION_GOAL.md top). What was recovered before the stop, as evidence only:

Oracle: tools/gpu_repro/interp_oracle.cpp + interp_oracle.hlsl (DXC SM 6.1; every fragment appends RAW BITS of
SV_Position, SV_Barycentrics (perspective and linear), perspective / linear / centroid attributes, coarse / fine
derivatives, GetAttributeAtVertex, coverage); fits in tools/gpu_repro/interp_fit.py (stages load, attr, shift, bary,
basis, order, lsq, plane, plane2, lp, range, anchor, lattice, nice, nicemiss, grain, nice2). Build:
.build/gpu_repro/build_oracle.cmd.

| finding | evidence |
|---|---|
| GetAttributeAtVertex(k) = input vertex k | 100 % of 149,966 fragments |
| perspective = linear attribute when w = 1 | 100 % |
| SV_Barycentrics = the same interpolator applied to unit attributes (1,0,0) / (0,1,0) / (0,0,1) | 100 % |
| a constant attribute comes back exactly | 100 % (the form is difference-based, not sum of weights: sums of weights are 1 in only 13 %) |
| attribute is NOT any fp32 formula of the exposed barycentrics | best 44.98 %, errors to 122 ulp |
| ONE plane per triangle, evaluated at high precision, rounded once | residual vs a fitted plane 0.30 ulp rms (pure single rounding = 0.289) |
| the final rounding is TRUNCATION | LP: one real plane fits every pixel of 58/58 triangles with truncation, 18/58 with round-to-nearest, 10/58 round-up |
| coefficients carry <= 24 mantissa bits; the plane does not pass exactly through any vertex value | LP feasible intervals (relative width ~1e-7) |
| frame anchored to a 4-pixel grid + absolute-magnitude setup effects | integer shifts: multiples of 4 keep 82-91 % of bits, other shifts 9-19 %, >= 64 px 61-67 % |
| negative results land one ulp toward zero even when exactly representable (internal one-LSB bias, then truncation) | designed exact triangles: all 1,618 negative pixels, never a positive one; with this output rule the exact case matches 99.986-99.994 % |
| not yet recovered: how inexact coefficients are rounded at setup, and the anchor | non-power-of-two legs: best 78-80 % (A, B rd / rne) |

Conclusion for the new architecture: the hardware interpolator is a fixed-point / extended-precision unit with its
own setup, anchor and asymmetric output rule - nothing a portable specification may depend on. A vendor-independent
renderer must compute coverage, interpolation, texture filtering and blending itself, in explicitly specified
integer arithmetic (section 24).

## 24. Universal exact architecture, increment 1: specification v0, CPU reference, portable GPU kernel (2026-10-30)

Specification: docs/EXACT_RENDER_SPEC.md v0 (particle pass): CPU setup (16.8 snapping, exact 128-bit rational
plane coefficients rounded once, ties to even), integer coverage on the D3D / Vulkan standard 4x pattern with the
top-left rule, integer attribute evaluation at the pixel centre, integer bilinear filtering (8-bit subtexel), integer
colour and the one-rounding unorm16 blend. No GPU float, interpolator, rasterizer, sampler or blend unit decides a bit.
Code: tools/exact/exact_particles.h (portable C++20 reference, header-only), exact_particles.hlsl (SM 6.0 compute,
64-bit integers: D3D12 Int64ShaderOps / Vulkan shaderInt64), exact_particles_test.cpp (scene, binning, validation,
timing; build .build/gpu_repro/build_exact_spec.cmd), div65535_proof.cpp.

Kernel design: one thread owns one pixel's 4 samples in registers and walks its 16 x 16 tile's triangle list in draw
order: no ROV, no atomics, one framebuffer read and one write per pass. Exactness-preserving optimisations, each
re-validated: binner proves a tile fully covered (sample-box corners strictly inside every edge: per-sample tests
skipped) or empty (strictly outside one edge: entry dropped); register compression (one blend while the 4 samples are
equal and fully covered; re-compressed after full blends); floor(x / 65535) = (x + 1 + (x >> 16)) >> 16, proven for
all 4,294,868,993 inputs used with no 32-bit overflow; power-of-two wrap / mirror by masks (two's complement i & (n-1)
= non-negative i mod n for every i), 32-bit texel indices; 64 triangle records staged in groupshared memory.

Validation (samples = R, G, B of every one of the 4 samples of every pixel):

| scene | CPU ref vs RTX 4080 | CPU ref vs WARP (Microsoft Basic Render Driver, a CPU D3D12) | non-triviality |
|---|---|---|---|
| 640x400, 60 smoke particles (alpha) | 0 | 0 | all 1,024,000 samples changed, 4,395 edge pixels |
| 640x400, 400 sparks (additive) | 0 | 0 | 312,973 changed, 4,371 edge pixels |
| 3840x1200, 400 smoke particles | 0 (18,432,000 samples) | not run (time) | all changed, 100,296 edge pixels |
| 5120x1600, 400 smoke particles | 0 (32,768,000 samples) | not run | all changed, 134,580 edge pixels |

Validated implementations so far: the CPU reference, D3D12 on the RTX 4080, D3D12 WARP. NOT validated: AMD, Intel,
Vulkan, any mobile GPU (none available here; no claim).

Performance (RTX 4080, driver clocks, median of 9-15; "hardware path" = the same triangles through rasterizer +
sampler + blend unit, the old renderer's way - its output is NOT the specification's):

| step | 3840x1200 exact kernel | 5120x1600 exact kernel | hardware path |
|---|---|---|---|
| first kernel | 13.05 ms | 22.06 ms | 1.34 / 2.38 ms |
| + tile full / empty proofs, register compression, fast /65535 | 7.37 | 11.77 | 1.33 / 2.38 |
| + mask addressing, 32-bit texel indices, groupshared staging | 2.86 (untextured 2.35) | 5.11 | 1.33 / 2.38 |

Tile entries at 3840x1200: 772,519 (654,552 full, 117,967 partial). Status: exact everywhere tested, still ~2.1x
slower than the hardware path: fails the performance gate so far. Next optimisations (each must stay sample-exact):
per-triangle constants hoisted into the staged record (untextured colour, cutoff), a packed record (112 -> ~64
bytes), tile-relative 64x32 attribute evaluation, partial-tile edge functions as tile base + small offsets, locked-
clock A/B, occupancy (register) tuning, GPU binning (the CPU binner is outside these timings). Then the particle
pass in the game: the clip-position setup (sin / cos, matrix) specified on the CPU, mip selection, fog, depth test
against the mesh pass (section 7 of the specification).

## 25. Phase IV, increment 1: the real game's particle pass through the exact renderer (2026-10-30)

Data path (reverse engineered): the game's particle systems fill RenderWare PTank (RpPTank) clusters - position,
size, rotation, two texture corners, per-particle RwRGBA - which RwDriver::particle_batch (rw_driver/pipelines.cpp)
reads from guest memory into 44-byte frame.h Particle records; the batch carries the camera's right / up axes, the
material texture / sampler and the recipe. Upstream (the game's emitters and simulation) is not yet traced.

Capture: SHADOW_R2_DUMP_PARTICLES=<dir> (+ _TICKS=a,b), renderer.cpp dump_particle_draw, non-shipping: every particle
draw in draw order with recipe, batch, pass view-projection, target size and records; with SHADOW_R2_DUMP_TEXTURE the
mip chains. Full replays at 5K: Westopolis 5,260 frames (157 MB), Final Haunt 5,144 (160 MB).
tools/exact/particle_dump.py (survey, heavy).

What the game draws (every 5th frame of both tapes: 57,056 draws, 1.25 M particles): all textured, all per-particle
colours, all fogged, all depth-tested, all wrap / wrap, all mapped to the anisotropic sampler; SrcA/InvSrcA 39,514,
SrcA/One 11,050, One/One 6,492; alpha-tested (cutoff 0.784, depth write) ~1,000 draws; rotation in 77 %; <= 150
particles per draw; Final Haunt draws most particles into the glare-sources pass. Densest frames: Westopolis ticks
1202-1208, ~88 screens of projected overdraw, 1,574 particles in 36 draws (the 11 ms GPU frames); the next densest
frame anywhere is ~3 screens. Tick 1207: smoke texture 010000F1 (32x32, 6 levels, 32.5 % of texels alpha 0); one
draw of 40 particles at colour alpha 3-4 / 255 covers 25.6 screens; one draw has colour alpha 0 with SrcA/One (adds
exactly nothing); camera-facing quads -> one depth, one fog distance, one mip level per particle.

Specification v1 (docs/EXACT_RENDER_SPEC.md section 9; tools/exact/exact_render.h, exact_render.hlsl,
exact_frame_test.cpp, build .build/gpu_repro/build_exact_frame.cmd): the particle setup from the records on the CPU
(portable sin / cos from basic operations, double projection, cull outside 0 <= z/w <= 1, per-particle depth = fp32
of z / w, fog weight 16.16 from w), exact LOD (integer log2 by squaring) and trilinear filtering on the real mip
chains, fog lerp in 16.16, blends SrcA/InvSrcA, SrcA/One, One/One, per-sample depth test (GREATER_EQUAL as integer
compare of non-negative float bits) and depth write for alpha-tested draws.

Validation on real frames (colour = every RGB sample, depth = every depth sample):

| frame | size | CPU ref vs RTX 4080 | CPU ref vs WARP | non-triviality |
|---|---|---|---|---|
| Westopolis tick 1039 (scene) | 960x540 | 0 / 0 | 0 / 0 | 13,089 samples changed |
| tick 1207 dense smoke (scene) | 960x540 | 0 / 0 | 0 / 0 | all 2,073,600 changed |
| tick 1207 (glare-sources pass) | 960x540 | 0 / 0 | 0 / 0 | 17,117 changed |
| tick 1207 dense smoke (scene) | 5120x2880 | 0 / 0 (58,982,400 samples) | not run | all changed, 54,055 edge pixels |

Inputs not yet from the game: the colour and depth the pass starts from (clear colour, far depth: every particle
passes its depth test - more work than the game's frame, never less).

Performance, tick 1207 scene particles (886 particles -> 1,328 triangles after the near / far cull):

| size | exact kernel (driver clocks) | exact kernel (locked) | CPU setup | CPU binning | game's hardware particle time |
|---|---|---|---|---|---|
| 5120x2880 | 21.82 ms | 27.57 ms | 1.6 ms | 120.6 ms | 11.10 ms (locked, GPU trace dc-5k) |
| 3840x1200 | 7.02 ms | - | 1.6 ms | 37.2 ms | not measured yet |
| 5120x1600 | 12.29 ms | - | 1.6 ms | 67.3 ms | not measured yet |

Locked-clock attribution (ablation variants, cost attribution only): no texture 20.35 ms, bilinear only 27.86 ms, no
fog 25.69 ms -> texture ~7 ms, fog ~2 ms, the per-layer base loop ~20 ms (1.47 G pixel-iterations; every full-screen
quad has an entry in every one of the 57,600 tiles: 5.75 M entries, 97 % full tiles).

Verdict: exact on real gameplay data on every tested implementation; 2.5x SLOWER than the hardware path on the
densest real frame, and the CPU binning (120 ms) is unusable. Not integrated. Causes to attack next, in order:
(1) binning: per-tile lists of per-triangle entries for screen-covering quads -> GPU binning and a range
representation (a full-tile run of consecutive triangles as one entry); (2) the base loop: per-iteration LDS reads of
the 128-byte record (one broadcast load per pixel-iteration), several pixels per thread, a packed full-tile record,
64-bit attribute evaluation tile-relative; (3) the real starting colour / depth (capture), which can only reduce work.

## 26. Phase VI, increment 1: particle engine cost at runtime, GPU binning (2026-10-30)

Static dissection of the whole PJS particle engine already exists: docs/effects/particles.md (2026-10-27; manager /
element classes with vtables, banks, time base, emission shapes, the Euler step with per-update drag, the affector
bytecode, MT19937, buckets in launch order, the PTank fill 0x80434824, the PTank callback 0x80449A74, flaws F1-F14).
This increment adds the runtime COST of that engine (not yet the runtime verification of each rule).

Game-thread attribution, Westopolis headless replay (GEKKO_SELF_SAMPLE every 50 us + GEKKO_FRAME_WORK; 21,613
samples, inclusive): EffectTask 0x801D2318 / manager update 0x804262E4 8.5 %, element update 0x80427E2C 6.7 %
(EffectParticlePJS 0x800282AC / 0x80434B6C), particle step 0x80428044 5.7 % (1.4 % exclusive), draw walk 0x804261C0
4.8 % (PTank fill 0x80434824 2.4 %, RwDriver::particle_batch 0.6 %, lock 0x80449458 0.1 %), affector bytecode
0x80429224 0.6 %, colour key 0x80424324 0.1 %, MT19937 0x8040FB60 0.1 %. The particle engine is ~13 % of the game
thread (~0.27 ms of the ~2.1 ms median): a native owner is worth it, but must be bit-exact with the translated code
(paired-single maths, the shared MT19937 stream - flaw F5 -, the affector bytecode); not started. (The per-frame
window selection of self_sample_report.py returned only 4-5 samples for frames 1150-1260 - frame numbering of the
two logs to be reconciled; the whole-run shares above are unaffected.)

GPU binning (exact_render.hlsl classify + MASK_BINNING; exact_frame_test.cpp --gpu-bin): one thread per (tile, 32
triangles) runs the same full / empty proof as the CPU binner (bounding-box reject first) and writes two mask words;
the render kernel walks each tile's mask words in order (bit index = triangle index = draw order), staging the set
triangles in groupshared memory. No sort, no lists, no CPU binning.

| tick 1207 scene particles, 5120x2880 4x, locked clocks | exact? | GPU | CPU |
|---|---|---|---|
| CPU-built tile lists | 0 / 0 differing samples | 27.58 ms | setup 1.5 ms + binning 124.4 ms |
| GPU mask binning (classification included) | 0 / 0 (58,982,400 samples) | 35.33 ms | setup 1.5 ms |
| game's hardware particles (same frame, locked, GPU trace) | (old renderer, scheduling-dependent) | 11.10 ms | - |

Small sizes (960x540): GPU binning exact on RTX 4080 and WARP for ticks 1207 and 1039 (0 / 0 everywhere).

Reading: the binning problem is solved (the CPU cost is gone, exactly), but the exact kernel is 3.2x slower than the
hardware on the densest real frame. The gap is structural: every one of the 1.47 G pixel-steps does in ALU what the
hardware does in fixed-function units (filtering, blending, depth). Rejected on the way: keeping the blend unit for
the blend while the shader supplies exact S - it reintroduces vendor dependence and the in-draw coalescing breaks
exactness at every quad diagonal (section 19). Candidate gains not yet measured: several pixels per thread (shared
staging, constants and texel fetches), per-tile texel reuse (the smoke texture is 32x32 magnified ~100x: a 16x16
tile's four bilinear texels are almost always the same), 64x32 instead of 64x64 multiplies, 32-mask staging rounds
merged to 64, uniform-depth compression. Expected order of magnitude ~2x - near the hardware, not clearly below.

## 27. Lava Shelter mid-stage drop = the driver's P5 memory clock (2026-10-31)

The user's recorded run (.build/recordings/user-lava-shelter-20261031-064451; stg0602, 3840x1200 fullscreen, release
build): 163-172 s into the run 31-55 fps, 104 frames missed (telemetry drop 4 -> 108), game speed 1.000 throughout (the
game thread kept time; the GPU side was late). GPU memory trace flat (~514 MiB of a 16 GiB budget).

Replays of the tape (perf-2026-10-30/lava_replay.ps1, lava-r2 and lava-r3-stable; present/GPU/per-draw traces + NVML
sampler): the same game moment, the same pass list (same draws, so the same gameplay), and both times the NVIDIA driver
moves to P5 for exactly 162-171 s: memory clock 810 MHz (P3 5001, P0 11201). The full-frame copy passes depend on
memory bandwidth and slow 3-6x:

| ms/frame | 150-161 s (P3, 5001 MHz) | 163-171 s (P5, 810 MHz) |
|---|---|---|
| resolve | 0.58-0.66 | 2.09-2.46 |
| views under | 0.38-0.57 | 1.43-1.72 |
| compose | 0.29-0.30 | 1.38-1.46 |
| scene | 2.9-3.1 | 2.5-3.4 |
| frame GPU median | 5.0 | 7.9-9.0 (max 11.5) |

The replays raised the graphics clock to 1.0-1.26 GHz within a second and stayed under 16.7 ms; the user's run did not
(no sampler there; presents averaged 31 ms in the first second of the drop). Why the driver picks P5 at that moment is
its own heuristic (throttle reason "GPU idle" throughout); the frame's work did not grow. At 5K the GPU is busy enough
that the driver never leaves the high states, which is why the user sees fewer drops at 5K. SHADOW_R2_STABLE_POWER=1
had no effect in lava-r3-stable because the release preset is a shipping build (SHADOW_SHIPPING=ON compiles the switch
out); use the diag build for locked-clock A/B.
Options: the per-program NVIDIA power setting ("Prefer maximum performance", the user's setting) or fewer full-frame
copies at the end of the frame (resolve -> views under -> resolve -> compose copy the frame ~4 times; exact change,
needs the pixel gates).
Where the copies come from: the user's display.cfg has fps_counter=true, and the FPS counter (effect_layer_overlay.cpp)
is drawn as a second view over the game (over_views): the game view resolved into its own texture, copied into the
overlay's 4x colour ("views under"), the overlay resolved again, and both views copied by the compose.

The fold (2026-10-31, renderer.cpp, SHADOW_R2_FOLD_UNDER=0 for the old path): a view whose picture reaches the screen
only through the overlay over it (next view, over_views, same rectangle, no other view under or over it) is resolved
straight into the overlay's multisampled colour - the resolve writes its one value into every sample, the value the
copy wrote (unorm16 -> float -> unorm16 is the identity) - and the compose leaves it out (the overlay covers it with an
opaque copy). Stats line: "views under an overlay resolved into it".
- Exactness (diag build, SHADOW_R2_FOLD_CHECK=1: each captured frame rendered with the overlay folded and copied):
  79 frames of the Lava tape at 3840x1200, 78 byte-identical; frame 150 had 1 byte (1 step), and the repeat shows why:
  the FIRST render in the process gives 50 at that pixel whichever path it takes (copy first: the copy has 50, both
  folded renders 49), every later render 49 - a first-render effect of this process, not the fold.
- Cost, fullscreen replays of the tape (one run per arm, driver clocks; frames binned by the memory clock at their GPU
  end; this time the driver spent most of both runs in P5):

| tail passes ms/frame | fold off | fold on |
|---|---|---|
| P5 (810 MHz): resolve + views under + compose + present scale | 5.10 | 3.16 |
| P5 frame GPU p50 / p99 | 7.89 / 15.97 | 6.84 / 14.90 |
| P5 frames over 16.6 ms | 53 of 7,608 | 27 of 6,704 |
| P3 (5001 MHz) tail / p50 | 1.53 / 4.85 | 1.14 / 4.20 |
| frames dropped (telemetry) | 105 | 59 |

The fold does not remove P5 or every late frame; the rest is scene work at P5 (see POWER_MANAGEMENT_PLAN.md).

Side issue: twice the user's fullscreen session (XInput on) ended without the clean shutdown on WM_CLOSE (no exit
lines, no tape, no present/GPU traces); two controlled closes did not reproduce it. The tape is now saved every 10 s
while recording (shadow_main.cpp, autosave on a worker; proven by a forced kill).

## Open (next steps, in order)
- Frame 33 after PRS ahead (~13.6 ms at 5K): the RenderWare stream code (fn_804586E8 and callers) and the 42-62
  expand calls per stage start whose member was not decoded yet.
- GPU clock state (3a): the driver holds the GPU at ~585 MHz under this load; the smoke frames are late because the
  clock ramps only after them. Requesting more (power profile / NVAPI application setting) is the user's decision.
- Smoke fill (3b): ALL smoke work, particle trimming included, belongs to this (performance) session since 15:25 (the 5K side session handed it over; its exactness notes are in docs/RENDER_TARGET_ALIASING_5K.md "Smoke facts"). Remaining options change pixels (lower-resolution particle pass, R11G11B10F scene) or save little
  exactly (corner trimming ~11 % of this texture's area): user decisions / measured trade-offs.
- Commit and deploy: nothing committed or deployed.
