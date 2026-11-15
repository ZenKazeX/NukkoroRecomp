# s1-mtn: Mtn animation library, stack-attributed map, no new owner (2026-11-12)

Branch s1/mtn (from s1/integrate f33908a), slot s1-slot-b. Deliverables: docs/re/mtn.md (map, inclusive costs), the stack
profiler tools (tools/perf/s1_*.py), claims line in s1-claims/mtn.txt. **No owner added; no native-owners / CMake / liveness
list touched.**

## What was measured
Stack-attributing profile of the base exe (700f72d), Westopolis headless replay, 18,699 samples of the game thread
(method and the PDB age trap in docs/re/mtn.md section 1; two earlier runs with wrong symbols were discarded).

| tree | inclusive % of game thread | unowned part |
|---|---|---|
| any Mtn frame | 14.13 | |
| MotionUpdate 0x80416EF4 (the only per-frame entry) | 11.62 | lifted Mtn bodies 2.26 (~25 functions, max 0.21), dispatch 0.73 |
| - bone pass 0x80419094 -> owned NodeWalk | 6.33 | self 0.19 |
| - 0x804198F0 sampler pick -> owned sample_vec3 | 2.69 | self 0.15 |
| - time advance 0x80419A9C | 0.84 | all |
| - weights/blend 0x80418618 | 0.78 | 0.22 self |
| - dispatcher 0x8041A33C | 0.60 | |
| unowned bone callbacks under NodeWalk (guest_call) | ~1.2 (0x80420B28 .34, 0x80420D10 .28, 0x804202E0 .16, 0x8041FDDC .14, 0x8041FA14 .13) | all |
| clip cache 0x804168A4 / 0x80413644 (allocator path, resource side) | 1.04 / 0.35 | all |
Callers (inclusive through MotionUpdate): enemy display common 0x801A4CF0 4.28, ShadowMotion sub5 0x800B4BEC 2.57,
0x8063ED4 1.82, Npc Sonic motion impl 1.51.

## Why no owner pays
* Ceiling: everything an owner of the whole MotionUpdate tree could remove is the unowned lifted bodies (2.26) plus the
  dispatch frames (0.73) = 3.0 % of the game thread. The 10 + 11 existing owners show what such a conversion yields: round 1
  took the replaced code from 11.5 % to 7.4 % (-36 %), round 3 of pose gave -1.9 % with overlapping ranges. -36 % of 3.0 is
  about **1.1 %** as an optimistic ceiling; the realistic piece (callbacks only, 1.2 % incl.) is about 0.4 %.
* That is below what the required 3 alternating rounds can resolve (earlier mtn blocks: spreads 1-2 % wide, pairs overlapping).
  The contract says no measured gain = no owner.
* The cost to own it is large and Tier E: 0x80418618 (2,684 B), 0x80419094 (1,628 B), 0x80419A9C (2,208 B), 0x804198F0, the
  0x8041A33C family and the callbacks, all with frame-local reads, guest callbacks and matrix-stack side effects (the pose
  round needed a 760,000-call test and found stale-stack preconditions). A first build is ~60 min at 7 %.
* The remaining big costs in the tree are not Mtn code: msl sin/cos (rem_pio2 0.26, sin 0.21), compose_disjoint 0.29,
  translate/scale fast 0.4, sqrt 0.11 belong to the math owners (other fields).
* "Unowned node between two owned ones": real for the bone callbacks (NodeWalk -> guest_call -> lifted callback -> owned
  math_affine owner), but each is <= 0.34 % inclusive.

## Gates
Not applicable (no owner, no source change): state.json 3a75268e85cd / PCM c64ee7883f980f73 are those of the base exe;
behaviour gate and benchmark rounds were not run. The sampler runs were on the unmodified base exe copy.

## Open questions / hints for others
* tools/perf/sample_threads.py cannot attribute inclusive cost; s1_sample_stacks.py can (also useful for the other S1 fields).
  PDBs: the exe embeds wt-opt-integrate's path, which was rebuilt (age 5); an age-4 PDB exists in wt-s2-rwgame/.build/exes.
* The clip cache path 0x804168A4 -> 0x80412868 -> 0x80414368 (1.04 % incl.) is resource-side (re-engine/S3), not animation.
* If anyone later wants the 1 % ceiling: batch the callbacks (0x80420B28, 0x80420D10, 0x804202E0, 0x8041FDDC, 0x8041FA14) into
  NodeWalk's callback switch (as the five owned callbacks already are), where they would call math_affine natively.

## Round 4: the five NodeWalk callbacks folded into the owned walk (window-ready, off by default)

Commit: see git log of s1/mtn (round 4). No timing run was made (contract amendment 4); gains are for the main session's combined A/B.

**What it is.** In `native_mtn_pose.cpp` the owned NodeWalk executes 0x80420B28 (scale of the stack top by the first bone vector,
skipped at exactly 1,1,1), 0x80420D10 (translate by it), 0x8041FA14 / 0x804202E0 / 0x8041FDDC (the "bone bank" passes: scale lengths,
translation, rotation over the persistent per-bone matrices at 0x805E23A0 + 0x30 * position; the fifth matrix at +0xC0 is the base
accumulator) natively, instead of entering their guest bodies through `guest_call`. They are not installed as owners (the guest
bodies stay for any other caller). 0x8041FA14 is the existing ScaleLengths pass with another tail (new `VecKind::ScaleBank`); the
other two are new functions `fold_translation_bank` / `fold_rotation_bank` (the helper 0x80412438 with r3 = 0 is inlined as
`copy_3x3_to_top`). Same primitives as the existing passes (ppc::fps_*, msl sin/cos in the caller's NI mode, math_affine owners,
compose). Hand-back: before any write the fold checks bone indices 0..3 up to the first -1, a first entry that is a root (else the
original reads a stale stack local) and, for the rotation / translation banks, a non-null base matrix; otherwise the guest body runs.

**Switches (all off by default).** Run time: `GEKKO_MTN_FOLD=1` enables (`=0` disables); `GEKKO_MTN_FOLD_STATS=1` or an explicit
`GEKKO_MTN_FOLD=1` prints the hit counts to stderr at exit. Build: CMake option `SHADOW_MTN_FOLD` (OFF) defines
`GEKKO_MTN_FOLD_DEFAULT=1` for native_mtn_pose.cpp only (`-DSHADOW_MTN_FOLD=ON`); the env var still overrides. One exe serves
both arms. API: `mtn_pose::fold_enabled()`, `set_fold_enabled()`, `fold_hits(kind)`, `fold_handed_back(kind)`.

**Deterministic hit counts (Westopolis tape, 316 game s; identical in 4 runs):** scale callback 4,961,207; translate callback
2,611,021; scale bank 36,570; translation bank 36,570; rotation bank 36,570; handed back 0 each. About 7.6 M callback executions
leave `guest_call` per run (the two small callbacks dominate by count, the three banks by cost per call).

**Gates.**
* `mtn_pose_contract_tests` (ctest label reference;native-contract), default size, seed 0xA11CE5ED: PASS **1,060,000 calls, 0
  differences** (second seed 0x1234ABCD: PASS). New cases: each folded body against the original body at its address
  (FoldScaleCallback, FoldTranslateCallback, FoldScaleBank, FoldTranslationBank, FoldRotationBank, N each), then NodeWalk with the fold
  on and the five callbacks (+ RotationPass + a guest callback) as list entries (N/2, the walk requires every kind to run: fold counts
  in the test output). Every ORIGINAL instruction of the five bodies executed (29/29, 19/19, 242/242, 280/280, 321/321), all earlier
  ranges still 100 %. Compared after each call: arena, pass scratch + weights (0x805E2300-0x805E2700), caller frame, r1/r2/r13,
  r14-r31, f14-f31, FPSCR except FPCC, both NI modes. Negative controls, all caught: rotation bank with the wrong FPSCR[NI] mode
  20000/20000, translation bank one ulp off 20000/20000, scale bank stale flag 20000/20000, skipped scale callback 9853/20000
  (the rest hit exactly 1,1,1), plus the three earlier controls.
* Westopolis `state.json` md5 **3a75268e85cd** and PCM **c64ee7883f980f73**: identical to the base for fold off and fold on
  (3 runs each before, 2 runs after the last source edit).
* Behaviour gate (`tools/behaviour_run.sh`, combined layout, fold on) vs `wt-s1-integrate/.build/s1/base-combined-1` AND `-2`:
  **PASS, no field differs in any bit over 6,320 rows, 2,231 events identical.**
* `native_liveness_roots` and `native_liveness_roots_lifted` pass; no shared list changed (the five addresses are value-only
  constants tagged `liveness-roots: value-only`; no string literal carries an address, the checker reads those as roots).
* CRLF kept (`native_mtn_pose.cpp/.h`), the test and CMakeLists stay LF.

**Dead frames.** The folded callbacks' own frames (saved registers, link slots, psq spills) are not written, as for the five already
owned callbacks; the locals the bodies read back are written at the original addresses. state.json (whole RAM image hash) is
identical on the Westopolis replay; Final Haunt was not run (round 2 saw dead-stack words differ there).

**Expected effect (unmeasured).** The five callbacks were ~1.2 % of the game thread inclusive in the round-3 stack profile
(guest_call 0.27 % self, serve_native_owner share, lifted bodies); the fold removes the dispatch and the lifted bodies, the
arithmetic remains. Order of 0.4-0.8 % ceiling: below the noise floor, so this is for the combined window.

**Files.** `runtime/src/gekko/native_mtn_pose.cpp`, `runtime/include/gekko/native_mtn_pose.h`, `runtime/tests/mtn_pose_contract_tests.cpp`,
`CMakeLists.txt` (option SHADOW_MTN_FOLD, 6 lines after the SHADOW_SHIPPING block). Build: `.build/watch` exe md5 7e6e50235430a71e711b985062bee00f.

## Round 5: the MotionUpdate tree owner (window-ready, off by default)

Input: a post-fold stack profile (`tools/perf/s1_sample_stacks.py`, my exe with the fold on, 5,194 samples, about +-0.3 % on a 1 % row): any Mtn
frame 9.72 %, MotionUpdate 0x80416EF4 7.74 % inclusive; unowned lifted Mtn bodies 1.44 % + dispatch 0.19 % under it. Net (unowned lifted + dispatch
leaf) per direct phase of MotionUpdate: 0x8041A33C subtree 0.62 (0x80414E88 0.10, A33C 0.08, 0x8000543C 0.06, allocator 0x80414368 0.06, BDE4, C74C),
0x80419094 0.58 (0x80419094 0.13, 0x804215E0 0.06, game-side post callbacks 0.10), 0x80419A9C 0.37 (self 0.17, C01C 0.08, BF6C 0.06), 0x80418618
0.25 (self 0.17), 0x804198F0 0.17, 0x8041AFAC 0.12. No single body is above 0.17 %.

**What is owned.** An HLE hook (not an owner row: the guest body stays and runs whenever the switch is off) on `0x80416EF4 MotionUpdate(motion)` runs the
whole function natively: push, identity top, clear the flag bits, per active channel the guest phase 0x8041BB8C (called with r3 = motion, r4 = the channel
index, as the original leaves r4), then the guest phases 0x8041AFAC / 0x8041A33C / 0x80419A9C, the NATIVE sampler pick 0x804198F0 (sampler triple by
the sign of the channel rate with the NaN compare flags of the two fcmpo, the 4-float sampling context incl. the fctiwz / magic-double conversion, the two
channel callback lists; the four vec3 samplers are served directly through `mtn_track::serve_*`, any other callback by guest_call), the NATIVE bone pass
0x80419094 (root chain, per-channel local matrices with the three flag-dependent paths, the blended bank at 0x805E23A0, the channel weights, then the owned
NodeWalk) with the helpers 0x80412438 (r3 != 0), 0x80412520 (r4 = 0) and 0x804215E0 (matrix lerp: MatrixBlend + translation lerp) inlined, and the guest
phase 0x80418618, pop. The five guest phases are entered exactly where the original enters them. Files: `native_mtn_pose.cpp/.h` (CRLF kept), the contract
test, CMakeLists.txt, both liveness lists (6 lines each: 0x80416EF4, 0x80418618, 0x80419A9C, 0x8041A33C, 0x8041AFAC, 0x8041BB8C become runtime literals).

**Switches (off by default).** Env `GEKKO_MTN_TREE=1` enables (`=0` disables); `GEKKO_MTN_TREE_STATS=1` (or an explicit `=1`) prints the hit counts at
exit; CMake option `SHADOW_MTN_TREE` (OFF) defines `GEKKO_MTN_TREE_DEFAULT=1` for native_mtn_pose.cpp. When off the hook declines (counted) and the
guest body runs. API: `mtn_pose::tree_enabled()`, `set_tree_enabled()`, `tree_hits(kind)`. Hand-back: the native path has no input it has to refuse
(it reads every pointer where the original reads it and keeps the original's operation order, so even stale-stack cases are not entered: every frame local
is written before it is read for any input); the only hand-back is the switch.

**Deterministic hit counts (Westopolis 316 game s, identical in 3 runs):** motion updates 297,405; channels through the sampler pick 277,507; bone passes
297,405; declined 0. (Fold counts unchanged: 4,961,207 / 2,611,021 / 36,570 x 3.)

**Gates.**
* `mtn_pose_contract_tests`, default size, seed 0xA11CE5ED: PASS **1,180,200 calls, 0 differences, 0 skipped**; seed 0x1234ABCD PASS. New: SamplerPick,
  BonePass and MotionUpdate each against the original DOL body at its address (N/2 each), MotionUpdate with a null motion; for the whole update the five
  guest phases are replaced on BOTH machines by a stub that logs r3 (and r4 for the channel phase, a tag for the others) into the arena, so call order,
  arguments and counts are compared too; fixtures: motion + 4 channels (0x130 bytes) + channel table + callback lists (stub / the four vec3 samplers /
  none) + node chain, random flags, floats incl. NaN / -0 / overflow in the rate and the converted word. Compared: the arena (now 16 KiB), the scratch
  and weight tables, the sampling context, the sampler pointers [r13 + 0x5F78..80] (new in the comparison), caller frame, r1/r2/r13, r14-r31, f14-f31,
  FPSCR except FPCC, both NI modes. Every original instruction of MotionUpdate 61/61, SamplerPick 107/107, BonePass 407/407, MatrixLerp 39/39;
  0x80412520 23/30 (the r4 != 0 path is never taken by the bone pass). Negative controls caught 20000/20000 x 3: wrong NI mode in the bone pass,
  a flipped sampling-context word after the pick, a missed matrix stack pop in the update; all earlier controls unchanged. (Earlier fixtures had 1,000
  skipped cases from overlapping list entries; entries are now 0x100 apart: 0 skipped.)
* Westopolis `state.json` **3a75268e85cd**, PCM **c64ee7883f980f73**: identical to the base with the tree off and on (fold on in both).
* Behaviour gate (combined layout, tree + fold on) vs `base-combined-1` and `-2`: **PASS, no field differs in any bit, 6,320 rows, 2,231 events identical.**
* Final Haunt (stg0604, 120000 ms, tape final-haunt/input.tape, initial/ card+SRAM+cfg, `--headless --no-xinput`, same exe, fold on, tree off vs on):
  PCM identical (fa2abc800651039e), ticks / VI / render counts identical, every block of every region identical EXCEPT one 64 KiB block of region 0
  (0x80600000-0x8060FFFF; state.json md5 a01510ed564c vs 19d8b932f5f4): 31 sixteen-byte lines between 0x8060BAA0 and 0x8060BD87, saved registers, link
  words and matrix scratch of the original frames of 0x80419094 / 0x804198F0 / 0x804215E0 (hex dump compared, `--dump 0x80600000 0x10000`). The current
  stack pointer at exit is r1 = 0x8060C4C8 in both runs, above every differing word: dead stack below SP, the same classification as round 2 (the
  frames the original wrote and the owner does not). Classified, not replicated; the Westopolis image is identical.
* `native_liveness_roots` + `native_liveness_roots_lifted` pass with the regenerated lists.

**Expected ceiling (unmeasured).** The two native phases cost, in the post-fold profile, 0x804198F0 0.17 % net + 0x80419094 about 0.2 % Mtn net (+0.1 % of
game-side callbacks and matrix leaves it now calls directly): about 0.4 % of the game thread removed from lifted bodies and dispatch; the fold removed about
1.2 % inclusive for a measured -1.3 %, so the expectation is -0.3 to -0.5 % (below one window round's spread, expect to need the 5-round median). Remaining
unowned phases of MotionUpdate for a further round: 0x8041A33C tree 0.62 net (play state: 0x80414E88, A33C, BDE4, C74C, ADAC / C1C0 / C30C), 0x80419A9C
tree 0.37, 0x80418618 0.25, 0x8041AFAC 0.12: about 1.4 % net, 11 KB of code, partly non-Mtn (allocator 0x80414368, 0x8000543C).

## Round 6: the remaining MotionUpdate phases (two window candidates, off by default)

Merged s1/integrate (970aaae). No timing run. Input: a 10,261-sample stack profile of my exe (fold + tree on, `tools/perf/s1_sample_stacks.py`, +-0.15 % on a
0.5 % row). MotionUpdate is 11.25 % inclusive; the unowned phases under it (net = lifted body + dispatch + the leaf helpers they call, % of the game thread):
0x8041A33C tree **0.75** (0x80414E88 0.17, 0x8000543C 0.07, 0x8041BDE4 0.06, 0x8041C30C 0.06, allocator 0x80414368 0.06, 0x80412E3C 0.06: play-state and
clip-window code, half of it resource-side), 0x80418618 **0.46** (self 0.23, 0x8000543C memset 0.07, 0x80412438 0.05, 0x800091D0 0.03, dispatch 0.04),
0x80419A9C **0.42** (self 0.12, 0x8041C01C 0.13 and 0x8041BF6C 0.03 reached through the guest callee 0x8041BF20, dispatch 0.07), 0x8041AFAC 0.11.
The A33C tree (6 KB over twelve functions, recursion and the clip allocator) was not attempted: its Mtn-only net is about 0.45 % for the most code and risk;
0x8041AFAC alone (0.11 %) is below any window's resolution.

Both candidates are HLE hooks (no owner rows; the guest body stays and runs whenever the switch is off) with hand-written native bodies built from the
existing exact primitives (ppc::fps_*, math_affine / vec_math serve functions, the matrix-stack leaves, compare helpers that keep the fcmpo NaN bits and the
cror eq/gt/lt sequences). The native MotionUpdate (tree) calls them directly; with the tree off the hook intercepts the guest MotionUpdate's call.

### Candidate D: 0x80418618 (pose rebuild, the last phase)
Hand-back before any write: a bone index above 3 in the channel table (the pass scratch has four slots), a partner index above 3, or a first active channel
whose partner entry is not -1 (the original then reads an unwritten frame local, fr+8, in its third loop). Native: two-part delta rebuild from the base
matrix, root chain inversion (math_affine invert owner), per-channel compose, the partner blend (kind 1: weighted translations, the time warp) and the delta
propagation (kind 0: ordered tree pass over the partner table with 0x804125CC inlined as `rotate_by_top`), the per-channel reset (guest 0x8041DDBC entered
only when flag 0x2000 is set), the 12-byte clear of 0x8000540C inlined. Switch: env `GEKKO_MTN_PHASE_D=1` (`=0` disables), `GEKKO_MTN_PHASE_D_STATS=1` (or an
explicit `=1`) prints `[mtn_phase_d] folded / handed back / channels`; CMake option `SHADOW_MTN_PHASE_D` (OFF). API `phase_d_enabled / set_phase_d_enabled /
phase_d_hits`. Expected ceiling: **about 0.4 %** of the game thread (0.46 net minus what stays: the guest reset call, the invert and matrix leaves are
already native).

### Candidate C: 0x80419A9C (time advance)
Hand-back before any write: an active channel with a null parameter block. Native: parameter clamp (kinds other than 0/1), kind 0 timed parameter with the
finish branch (flag clears, guest 0x8041DC68 / 0x80414290 / 0x8041A6BC entered where the original enters them, the secondary table removal and the channel
table compaction, the outer index reset to 0 the original leaves in r31), kind 1 partner follow (flag copy and the time warp), the playback time advance with
the ping-pong wrap loops (the guest helper 0x8041ABF8 evaluated at both ends, Vec3Sub through vec_math) and the stop rules; 0x8000A980 (fabs, frsp) inlined;
0x8041BF20 entered for a channel that moved. Switch: `GEKKO_MTN_PHASE_C` / `GEKKO_MTN_PHASE_C_STATS`, CMake `SHADOW_MTN_PHASE_C` (OFF). Expected ceiling:
**about 0.25 %** (the callee 0x8041C01C / 0x8041BF6C remain guest): below the 0.5 % line alone, worth timing only together with D (combined about 0.6 %).

### Hit counts (Westopolis 316 game s, tree + fold on; identical in every run)
D: 297,405 phase runs, 0 handed back, 311,412 channels visited. C: 297,405 phase runs, 0 handed back, 277,714 channels visited.

### Gates (same exe, 4 arms: base = tree + fold only, D, C, D+C)
* `mtn_pose_contract_tests`, default size, seed 0xA11CE5ED: PASS **1,377,100 calls, 0 differences** (second seed 0x1234ABCD: PASS). New cases: PhaseD and PhaseC each
  against the original DOL body at its address (the guest callees 0x8041DDBC / 0x8041BF20 / 0x8041DC68 / 0x80414290 / 0x8041A6BC replaced on both machines by logging stubs,
  0x8041ABF8 by a stub that logs r3 / r6 and writes a vector derived from r5, so arguments and call order are compared), MotionUpdate with the native phase called
  from the native shell (original runs the real body), the hand-back cases. Every original instruction executed: PhaseD 670/671 (the missed word is a `b`
  padding), PhaseC 550/552 (two padding `b` words), 0x804125CC 46/46. Negative controls: PhaseD wrong NI 20000/20000, one-ulp-off 20000/20000, missing flag reset
  20000/20000; PhaseC wrong NI 19957/20000, one-ulp-off time 19954/20000, skipped phase 14647/20000 (the rest had nothing to do). Hand-back: 2000 + 1000 states
  (index above 3, a partner on the first active channel, a null parameter block) declined with nothing written; switch off declines.
  Note: 6,100 of 1.4 M calls were skipped because the ORIGINAL did not finish in 3 M interpreter steps (a random frame time or time value that makes the
  original wrap loop run for millions of iterations); the owner has the same loops, so degenerate times are as slow there as in the original.
* Westopolis state.json **3a75268e85cd**, PCM **c64ee7883f980f73**: identical in all four arms.
* Behaviour gate (combined layout) vs base-combined-1 and -2: PASS in all four arms.
* Final Haunt (stg0604, 120000 ms, same exe, tree + fold on): PCM fa2abc800651039e and state.json 19d8b932f5f4 **identical in all four arms** (D and C change no
  dead-stack word versus the tree-only arm: the frames they skip are overwritten later, unlike the tree's own).
* liveness: 6 new runtime literals in both lists (0x80414290, 0x8041A6BC, 0x8041ABF8, 0x8041BF20, 0x8041DC68, 0x8041DDBC); `native_liveness_roots` and `_lifted` pass.

## Harness-only port (branch s1/mtn-harness, base agent/opt-integrate 4ea5050)

`mtn_pose_contract_tests` failed on the merged tree because the test depended on the build's switch defaults (native arm ran the native phases while the original arm stubbed them)
and because the fixture overlapped (channel parameter blocks 0x20 apart overwrote the callback list, so the original called into a float and the calls were skipped). This branch
carries only the harness fix, with no candidate B (no B chains, B code or B switch). Ported from s1/mtn:

* fb39aaf: `SwitchGuard` (records the build defaults of fold, tree, C and D, pins them OFF, chains that need a switch set it themselves, restores on every exit), per-chain time
  table, bounded exotic timing words in the random fixtures.
* f9c28e0 / 1ebb0bb / a4deacc: the fixture spacing fix (parameter blocks 0x40 apart, lists at kArena+0x2C00), the fixed huge-time set for PhaseC and MotionUpdate(phaseC)
  (terminating cases with a 400 M budget; non-terminating loops frozen at caps 1/3/50 via the test cap hook), 2^26 as the absorbing value, ctest TIMEOUT 3600.
* 9d051b6: the phase C family runs the original with a 4e9-step budget (others 3 M) because a follow channel can warp the time to ~1e7, which the original wraps back one span
  at a time (up to ~6e6 terminating iterations); env-gated `MTN_POSE_SKIPDUMP` prints the inputs of any call that still does not finish.
* The wrap-loop cap hook in `native_mtn_pose.cpp` exists only under `GEKKO_MTN_TEST_HOOKS`, defined only on the `mtn_pose_contract_tests` target (which compiles the owner file
  itself); without the macro `wrap_cap_hit()` is constexpr false. The only non-test change against 4ea5050 is that guarded hook (`git diff 4ea5050 -- runtime/src`).
* Object scan (checked by the coordinator with dumpbin): in the library object `wrap_cap_hit` is a 3-byte COMDAT with no reference; `g_wrap_cap` and `set_wrap_loop_cap` exist
  only in the test object.
## Round 7: the 0x8041A33C clip start phase and its bind tree (one candidate, off by default)

Merged agent/opt-integrate 0f54a54 and s1/integrate ef7eaac first (D and C are default ON there; my watch tree is configured with fold, tree, D, C ON and
the new option OFF). No timing run. Input: the 10,261-sample stack profile of round 6 (tree + fold on): 0x8041A33C is 0.82 % inclusive, 0x8041AFAC 0.19 %.

### What the tree is (P from the disassembly, counts from the Westopolis replay)
0x8041A33C is the clip start phase of MotionUpdate: per channel with a start request (flag 1 of +0x94) it runs 0x8041ADAC (start the clip: release the old
clip handle, reset the channel, 0x8041C1C0 bind, three 0x80412E3C queries), derives the state flags with 0x8041BDE4 (range clamp of the channel's four limit
floats, returns two bits), and calls the guest phases 0x8041DEC4 / 0x8041ABF8 / 0x8041A934, then 0x8041A508 or 0x8041A6BC. The bind (0x8041C1C0) resets the
track state arrays of the channel slot in the node tree (0x8041C74C), looks the clip up (guest), and binds the key records of the clip's node tree
(0x8041C30C, recursive over children at +0x48 / +0x4C, finding the matching tree node with the recursive search 0x8041E498, taking each key record from
the guest cursor 0x8041352C and initialising a track state with 0x80414E88, 15 flag-driven groups). Replay counts: 297,405 phase runs but only **5,008 clip
starts** (105,126 node binds, 771,104 node-search visits): the phase cost is the per-channel scan (A33C + BDE4 on every run), the clip start is rare but heavy.

### Owned natively (hand-written, `native_mtn_pose.cpp`; `mtn_track::state_init` added to native_mtn_track)
0x8041A33C, 0x8041BDE4 (all fcmpo / cror branches, NaN exception bits kept), 0x8041ADAC, 0x8041C1C0, 0x8041C74C, 0x8041C30C, 0x8041E498, 0x80414E88 (= record store
+ the existing exact TrackInit), plus inline: memset / memcpy of Mtn-owned memory (0x8000540C / 0x800054F4, pure byte fills and copies), 0x8000A980 (fabs then
frsp through `ppc::fps_round`), the sampling-context copy 0x804167AC. Guest calls kept exactly where the original makes them: clip cache 0x80414368 /
0x80414290, query 0x80412E3C, lookup 0x80413644, handle 0x8041342C, info 0x8041174C, record cursor 0x8041352C, and the phases 0x8041DEC4, 0x8041ABF8,
0x8041A934, 0x8041A508, 0x8041A6BC. Frames: each native level takes the original's frame size (0x30 / 0x20 / recursive 0x30 per bind level) so the guest callees'
frames land where the original's do. HLE hook on 0x8041A33C (declines when off); the native MotionUpdate calls the phase directly. Hand-back before any write: an
active channel (flag 4) or a start request without a parameter block. Recursion depth is capped at 200 (cyclic node data would overflow the guest stack in the original).
Switch `GEKKO_MTN_PHASE_B=1` (`=0` disables), `GEKKO_MTN_PHASE_B_STATS=1` (or explicit `=1`) prints `[mtn_phase_b] runs / handed back / clips started / nodes bound /
searches`; CMake option `SHADOW_MTN_PHASE_B` (OFF). API `phase_b_enabled / set_phase_b_enabled / phase_b_hits`.

### Net costs (percent of the game thread, 10,261 samples, +-0.15)
* Mtn-only net inside the tree, now native: 0x80414E88 0.17, 0x8041BDE4 0.06, 0x8041C30C 0.06, 0x8041A33C 0.05, 0x8041E498 0.05, 0x8041C74C 0.04, 0x8041ADAC 0.01, plus the
  inlined memset 0x8000543C 0.07 and memcpy = about **0.5 %**.
* **Resource-side, kept as guest calls (for a resource-side owner):** clip cache acquire 0x80414368 0.06 (+ the memory-system lambda it reaches 0.03-0.04, MoveEngine 0.01),
  query 0x80412E3C 0.06 self / 0.13 inclusive, record cursor 0x8041352C 0.06, lookup 0x80413644 0.02-0.04, handle / info 0x8041342C / 0x8041174C about 0.05 together =
  about **0.35 %**. All of it is called per clip start (5,008 times per replay) or per record (35 cursor calls per start): a native cursor (stateful SDA cursor over
  the clip database lists at [r13 + 0x5F58], [r13 + 0x5F68], [r13 + 0x5F6C]) and the lookup chain would be the natural next owner; they were not reimplemented here.
* 0x8041AFAC (0.19 % inclusive, 0.11 net, recursion over the bone tree with the owned rotation / translation fold callbacks below it) was not built: below any window's
  resolution on its own and entangled with the already-native NodeWalk callbacks.

### Expected ceiling (unmeasured): **about 0.45 %**, expect -0.4 to -0.6 % at the 5-round median (D + C measured -0.8 % for 0.67 % net).

### Gates (one exe; base = default set: fold, tree, D, C, plus everything merged from agent/opt-integrate; candidate = + GEKKO_MTN_PHASE_B=1)
* `mtn_pose_contract_tests` (see the numbers below): StateInit, FindNode, ResetTracks, BindNodes, ClipBind, RangeFlags, PhaseB and MotionUpdate with the native
  phase, against the original DOL bodies on a second machine; the clip database / allocator callees are replaced on BOTH machines by logging stubs (return tables
  for the lookups, key records, a clip info block with an embedded node tree, an obj-side node tree with children / siblings / grandchildren, four chained lists of
  entry blocks per node in a second 32 KiB region that is compared as well), so arguments and call order are compared too. Every original instruction executed
  (PhaseB 115/115, ClipStart 128/128, ClipBind 83/83, BindNodes 272/272, ResetTracks 98/98, FindNode 78/78, RangeFlags 79/79; StateInit 180/186, the six misses are `b` paddings).
* Westopolis state.json **3a75268e85cd**, PCM **c64ee7883f980f73**: identical, base and candidate. Behaviour gate (combined layout) vs base-combined-1 and -2: PASS both arms.
* Final Haunt (stg0604, 120000 ms, same exe): PCM fa2abc800651039e, state.json 19d8b932f5f4 **identical in both arms** (no difference from the current default set).
* Liveness lists regenerated: 9 new runtime literals (0x8041174C, 0x80412E3C, 0x8041342C, 0x8041352C, 0x80413644, 0x80414368, 0x8041A508, 0x8041A934, 0x8041DEC4) in both lists; the
  lists pass.

### Round 7 contract-test status (honest)
Default-size seed 0xA11CE5ED, first full run: 1,578,150 calls compared, 1 mismatch, all in `MotionUpdate(phaseB)`, where 19,999 of 20,000 calls were skipped (the original did not
finish) and the one compared call differed in the matrix stack index. Cause found in the fixture, not the owner: the obj-side list blocks' first word (read by the original NodeWalk
as a callback function pointer) was random. Fixed in the test (word cleared); a focused re-run after the fix (`MotionUpdate(phaseB)` 4000 calls: 2,699 compared, 1,301 skipped
for the usual unbounded wrap loops, 0 differences) passes. Second seed 0x1234ABCD, full default size: PASS, 0 differences. **A full default-size re-run of seed 0xA11CE5ED after the fixture fix was
started (`.build/r7/full1b.out`) but had not finished when this report was written** (a quiet window stops further runs): the coordinator should treat the seed-1 full run as pending.
