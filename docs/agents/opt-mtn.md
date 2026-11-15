# opt-mtn: the Mtn animation library's key-frame hot path (2026-11-11)

Agent `opt-mtn`, branch `agent/opt-mtn` (cut from gc-removal 17dd6e8). Scope: 0x80411000-0x80422700. Names below are this
project's own (from the disassembly and data); evidence marks P (proven from the cited words / measured), I (inferred), U
(unknown). The external decomp was not used for any name or any code here.

## 1. RE findings: what the hot path is

The animation evaluation walks per-channel lists of "track" objects. A **track** is one animated scalar: a 0x40-byte state
that streams keys out of a packed, half-float-coded key stream, keeps a window of two keys (A = previous, B = next) and
interpolates between them with cubic Hermite. Three tracks form a vector (x, y, z), sampled by one of four "SampleVec3" callbacks.

### 1.1 Track state (P, from 0x804162AC / 0x80415300 / 0x80414B80)

| Off | Field |
|---|---|
| +0x00 s16 / +0x02 s16 | key index / key count |
| +0x04 u32 | flags: bit0 finished (value cached, held), bit3 flat segment (cached value valid) |
| +0x08 u32 | pointer into the packed key stream (next undecoded key) |
| +0x0C u32 | record pointer (set by 0x80414E88 = initTrackState; read only by 0x80414B80) |
| +0x10 f32 | cached value |
| +0x20 / +0x30 | key A / key B, each {f32 time, in-tangent, out-tangent, value} |

### 1.2 Packed key stream (P)
Per key, u16 words: header (low 14 bits = time as an integer converted to f32 through the 0x4330 magic-double trick; 0x8000 =
out-tangent repeats the in-tangent, so the key has no out word; 0x4000 = "short key" marker read only when walking backwards),
in-tangent, [out-tangent], value. Each coded value is a half-float-like word: 0 -> +0.0, else `sign | (((h & 0x7FFF) << 13) +
0x38000000)` (no denormal / signed-zero handling; 0x8000 decodes to -2^-15). Walking backwards steps `stream -= (bit 0x4000 ? 6
: 8)` bytes; TrackInit (reverse) starts at `record + size - s8(record+6) - (first header bit 0x4000 ? 6 : 8)`.

### 1.3 Sampling context 0x805E2380 (P)
+0x00..0x0C copy of the channel's 4 floats (set by 0x804167AC; +0x0C is the time being sampled), +0x08 doubles as the playback
rate sign for TrackInit, +0x14 / +0x18 / +0x1C the value / slope / second-derivative interpolators, set once by 0x804167D4
(called from 0x80421CAC) to 0x80416728 / 0x8041669C / 0x80416580. 0x804198F0 picks, per channel, the sampler triple by the sign
of channel +0xA8: forward (0x804162AC, 0x80416020, 0x80415D0C) or reverse (0x80415A30, 0x8041579C, 0x80415488) and stores them
in SDA [r13 + 0x5F78 / 0x5F7C / 0x5F80].

### 1.4 The functions

| Address | Name (ours) | What it does | Ev | Self % (baseline) |
|---|---|---|---|---|
| 0x804162AC | TrackSample | value of a track at the context time: cached / A.value / interpolate / B.value / advance keys / hold / extrapolate | P | 1.23 |
| 0x80416728 | HermiteValue | cubic Hermite value (basis 2s^3-3s^2+1...) fused as the original; the entry in the body labelled fn_8041669C_e80416728 | P | 1.71 (with 0x8041669C) |
| 0x8041669C | HermiteSlope | its first derivative (constants 6, 3, 2): reached through ctx+0x18 | P | (above) |
| 0x80415300 | TrackSeekForward | advance keys while t >= B.time + eps | P | 0.31 |
| 0x80415170 | TrackSeekBackward | the mirror, stepping the stream back | P | ~0 |
| 0x80414B80 | TrackInit | reset to first (rate >= 0) or last (rate < 0) key, then seek | P | 0.48 |
| 0x80421358 / 0x804213FC / 0x8042148C / 0x80421518 | SampleVec3Reciprocal / SampleVec3 / SampleVec3At20 / SampleVec3Scaled | sample up to 3 tracks through [r13+0x5F78] into 3 floats; (1/x), plain, plain (tracks at +0x20), scaled by float at +0x10 when > 0 | P | 0.29 + 0.09 + 0.06 + 0.13 |
| 0x80416020 / 0x80415D0C / 0x80415A30 / 0x8041579C / 0x80415488 | slope / value+slope / reverse variants of the sampler | the same key walk, other outputs | P (read, not owned) | ~0 in the Westopolis profile |
| 0x804196F0 | NodeWalk (I) | recursive bone-tree walk: per node runs the node callbacks, the matrix-stack lock and a per-bone cache | I | 0.49 |
| 0x8041EC24 / 0x8041E8EC / 0x8041E5D0 / 0x8041F174 / 0x80420B9C | node callbacks (I) | compose rotations (fctiwz of angle / 16-bit turns, 0x8040C748 / 85C / 898 / 8D4 matrix-stack calls), normalise | I | 0.6 / 0.31 / 0.3 / 0.3 / 0.46 |
| 0x804121BC | MatrixOrthonormalize (I) | normalises the 3 columns with the game's sqrt (0x8000FEE4) | I | 0.41 |
| 0x80414368 / 0x80414290 | acquire / release clip node | allocator traffic: not ownable here (docs/resource-spine/motion.md) | P | 0.43 |

Not owned (reason): the node-walk family calls guest callbacks and the matrix-stack (0x8040C...) through pointers and holds the
lock object [r13+0x5F50]; owning it exactly needs those callees owned first, and each call is dominated by what it calls. The
acquire / release pair is the clip cache's allocator path.

## 2. Owners added (10 guest functions, one file)

`runtime/src/gekko/native_mtn_track.cpp` (+ `runtime/include/gekko/native_mtn_track.h`, the contract), installed from
`System` right after `vec_math::install`. All of them replace the translated body; the original bodies are pruned by the
generator (owner rows that emit only a thunk: 555 -> 564; the post-link gates still pass).

| Address | Owner | Notes |
|---|---|---|
| 0x804162AC | TrackSample | the whole key walk; the interpolator pointer in the context is served inline for 0x80416728 / 0x8041669C and called as guest code (guest_call) for any other value |
| 0x80416728 | HermiteValue | plain-float fast path (single rounding per step, fmaf for the fused steps), exact ppc::fps_* fallback |
| 0x8041669C | HermiteSlope | same |
| 0x80415300 | TrackSeekForward | |
| 0x80415170 | TrackSeekBackward | |
| 0x80414B80 | TrackInit | forward and reverse start, then the seek inline |
| 0x80421358 / 0x804213FC / 0x8042148C / 0x80421518 | SampleVec3Reciprocal / SampleVec3 / SampleVec3At20 / SampleVec3Scaled | call TrackSample inline when [r13+0x5F78] is 0x804162AC, else guest_call the pointer |

Why this shape: a transliteration of one leaf is not faster than the translated body (the translated code already uses
fps_* and keeps the register file in memory); the gain comes from (1) holding the 28-operation Hermite in host registers with
one cheap validity test instead of 28 full fps_* calls, and (2) the vector samplers calling the sampler (and the sampler the
interpolator) directly instead of 3 + 1 indirect guest dispatches per vector. First version (fps_* throughout) measured
-3.3 %; the Hermite fast path took it to -4.1 % (section 4).

### Exactness argument (what the tests prove, and what they rely on)
* The translated code's `fps_*` arithmetic never writes FPSCR (`fp_arith_slow` takes the FPSCR by value): each operation is a
  pure value function. For operands and results that are not NaN and not tiny it is the correctly rounded single result, so the
  same operations in the same order on host floats are equal; the fused steps use one rounding (`_mm_fmadd_ss`), which is also
  what the original's midpoint slow path computes (directed test below). Any NaN or any nonzero magnitude below 2^-100 in any
  operand or intermediate (a superset of the original's 2^-125 slow-path trigger) redoes the whole evaluation through
  `ppc::fps_*` in the original's operand roles, i.e. the exact path the translated code takes (NI flush, NaN priority, default NaN).
* Compares (`fcmpo` / `fcmpu`) are evaluated on the values with the original's NaN exception bits (`fp_compare_unordered`);
  CR0 and the FPCC result code of the last compare are not kept (nothing reads them; the next compare overwrites them).
* Constants (eps 0x38D1B717, 0, 1, 2, 3, 6) are the .sdata2 words at r2+0x16D0..0x1700 of the pinned DOL.
* Not kept: f0..f12, r0..r12, CR0, the original's stack scratch (it writes below r1 and its own LR slot only).
* Caveat shared with the other owners (vec_math): the f1 given to an interpolator owner is a single value (an `lfs` result), as
  at every caller; a double that is not a single takes the exact path.

## 3. Gates

### 3.1 Exactness (contract test `mtn_track_contract_tests`, ctest label `reference;native-contract`)
Original instructions from the pinned DOL (hash-checked) run in the reference interpreter on one machine, the owner on a second
machine with the same DOL and memory; compared after every call: the whole 0x400-byte arena (track state, keys, packed stream,
record), the sampling context, f1 (where it is a return value), r1 / r2 / r13 and FPSCR except FPCC.
* 1,640,000 call pairs: HermiteValue and HermiteSlope 2 x 120,000 random (every float class: zeros, denormals, the 2^-126 edge,
  tiny normals around the fast path's bound, infinities, quiet / signalling NaNs with payloads, huge, random bits; both FPSCR[NI]
  modes) + 2 x 120,000 "ordinary" inputs (the fast path's own territory); TrackSample 360,000; TrackSeekForward /
  TrackSeekBackward 120,000 each; TrackInit 240,000 (rate sign, byte 6, first-header bit 0x4000 varied); the four vector
  samplers 60,000 each (sampler pointer sometimes 0x80416020, a guest function the owner does not serve, so guest_call runs).
  Track states: index and count at and beyond their bounds (incl. negative), flags random, flat segments, packed streams of
  structured and random half words, interpolator pointer 0x80416728 / 0x8041669C / 0x804167AC (a guest function), context time on
  and one ulp around the eps window edges. **0 differences.**
* 400,000 directed fused-multiply-add triples (67,323 of them exactly on a single-precision midpoint) equal `ppc::fps_madd`.
* Coverage of the ORIGINAL bodies by those runs (every instruction executed): TrackSample 179 / 181, HermiteSlope 35 / 35,
  HermiteValue 33 / 33, SeekForward 96 / 98, SeekBackward 98 / 100, TrackInit 178 / 184, SampleVec3Reciprocal 41 / 41,
  SampleVec3 35 / 36, SampleVec3At20 35 / 35, SampleVec3Scaled 50 / 50. Every miss is an unreachable `b` padding word the
  compiler left (0x80416350, 0x80416410, 0x80415400, 0x8041547C, 0x8041526C, 0x804152F4, 0x80414C44, 0x80414C90, 0x80414CC8,
  0x80414D88, 0x80414DD4, 0x80414E10, 0x80421488).
* Negative controls, all caught by the same comparison: the wrong FPSCR[NI] mode (20,000 of 20,000 calls), a result one ulp off
  (20,000 / 20,000), a stale flat flag (15,431 / 20,000; the rest already had the bit), a wrong stream pointer after a seek
  (20,000 / 20,000).
* Test history, for honesty: a first draft compared f1 for the seek / init / vector entries; the original clobbers f1 there
  (scratch), so it is compared only where it is a return value (sampler, interpolators). A second draft overwrote a track's
  index / count with the scale field of the 0x80421518 object layout (a test bug, fixed in the test). The owners passed their
  first run once the test was right.

### 3.2 Game behaviour
Benchmark replay (Westopolis tape, `--boot-stage stg0100`, 316 game s, headless, no audio, 1280x720): `state.json` (game ticks,
VI / render frame counts and the 64 KiB block hashes of every guest memory region, i.e. the whole RAM image) has the same md5
(3a75268e85cd...) in every run of base and of every candidate build (55 runs): A/A and A/B identical, so no guest memory
word differs. Final Haunt replay (`--boot-stage stg0604`, the recorded tape, 120 game s): base and candidate `state.json` md5
3e09038e24f2... are identical. The track state lives in guest memory throughout and the owners write exactly the words the
originals write, so there is no retired state and no `--watch-guest` question to answer. No audio code is touched; the runs are
`--no-audio`, so no PCM hash was taken.

### 3.3 Other checks
* `native_liveness_roots_lifted` passes after regenerating `extract/liveness-roots-lifted.txt` (one row changed:
  `0x80416728 census` became `0x80416728`, because the address is now a runtime literal).
* Builds: `.build/watch` (lifted renderer, reference exe, symbols) built with 0 errors on the first try, base and candidates; no
  C1090 PDB error occurred (full build 1,780 s wall at 7.8 % machine CPU, full candidate rebuild 1,406 s at 8.5 %).
* The owner list in `extract/native-owners.txt` equals `mtn_track::Owners` (checked by script); `native_file_api_tests` has the line
  added but was not built or run (a baseline failure for CRI owners; building it was out of the CPU budget).

## 4. Benchmark

Method: `.build/ab.sh` (copy of the coordinator's run.sh per arm), alternating base / candidate, lock `benchmark`, every run through
the capped runner at 10 %, 316 game s headless, first pair discarded, numbers = `session.json` host.guest_run_cpu_seconds (wall in
the raw files is within 0.3 s of it). Base = `.build/watch` at 17dd6e8 without this branch's owners. Other agents' load changes the
absolute level between blocks (34.8 s vs 39.6 s for the same base exe), so compare inside a block only.

| Block | Candidate | Base mean (min-max) | Candidate mean (min-max) | Change | Rounds used |
|---|---|---|---|---|---|
| 1 | cand1: owners, fps_* throughout (commit 10a2323) | 36.06 s (35.23-36.80) | 34.86 s (34.34-35.78) | -3.3 %, spreads overlap | 4 of 5 |
| 2 | cand2: + Hermite fast path (8f87225) | 34.83 s (34.70-34.97) | 33.41 s (33.34-33.48) | **-4.1 %**, disjoint | 4 of 5 |
| 3 | cand3: + eps-compare fast path (42f0bce) | 39.70 s (39.38-40.28) | 37.73 s (37.06-38.25) | -4.9 % under load | 4 of 5 |
| 4 | cand2 as base vs cand3 | 33.87 s (33.44-34.19) | 33.84 s (33.34-34.36) | **no measurable change** -> reverted (3a96509) | 4 of 5 |
| 5 | **final = HEAD 3a96509** (same source as cand2) | 39.58 s (39.39-39.64) | 38.01 s (37.58-38.56) | **-4.0 %**, disjoint | 5 of 6 |

State identity: every run in every block, base and candidate, produced the same `state.json` (md5 3a75268e85cd...), 55 files
(base, every candidate, the A/A pairs) checked at the end.

Profile (tools/perf/sample_threads.py, game thread, ~12,600 samples, same replay): translated Mtn code 11.53 % -> 7.39 % of the
game thread; the owners cost hermite_value_fast 0.61 %, track_sample 0.94 %, seek_forward 0.24 %, the four vector samplers 0.56 %,
track_init 0.15 %, hermite_value 0.07 % (about 2.6 % in total, plus their share of `Cpu::serve_native_owner` 1.2 %, which is
shared with every other owner). The first (fps_* only) version had hermite_value alone at 2.16 %: the translated body was not
slow because of the register file, the fps_* sequence itself is the cost.

## 5. What was left and why

* No owner for the rest of the 7.4 % residue; the biggest are 0x8041EC24 0.61 %, 0x804196F0 0.59 %, 0x80420B9C 0.55 %,
  0x804121BC 0.36 %, 0x80412868 0.33 %, 0x80414368 0.32 %, 0x8041F174 / 0x8041E5D0 / 0x8041E8EC 0.28 % each. The bone-tree
  walk and the three rotation-composition callbacks (0x8041EC24 family) are dominated by guest callbacks, the matrix-stack lock
  and the 0x8040C* matrix-stack calls; an exact owner needs those owned first. The quaternion chain behind 0x8042167C
  (0x8042193C matrix to quaternion, 0x80421774 slerp with the game's acos / sinf, 0x80421BB0 quaternion to matrix, 0x804121BC
  normalise, total about 0.8 %) is pure float math but needs the game's sqrt / sinf / acos bit-exactly: the sqrt and the MSL
  sine owners of agent/native-math are not in this tree, so it was not attempted. acquire / release (0x80414368 /
  0x80414290) are the clip cache's allocator path and stay guest code (docs/resource-spine/motion.md).
* The reverse and slope sampler variants (0x80416020, 0x80415D0C, 0x80415A30, 0x8041579C, 0x80415488) and the second-derivative
  interpolator 0x80416580 are not owned (cold in the Westopolis profile; a vector sampler reaches them through guest_call, which
  the contract test runs). Two of them, 0x80415D0C and 0x80416580, use a different register convention (r3 / r4 out pointers, r5
  the track state; r5 / r6 the keys) than the sampler at [r13+0x5F78].
* A second fast path (plain-float eps-window compares in the sampler and seeks, 42f0bce) passed the same contract test and gave no
  measurable gain; it was reverted instead of kept.
* `native_file_api_tests` was not built; the full `ctest` set was not run (CPU budget); only `mtn_track_contract_tests` and
  `native_liveness_roots_lifted` were.

## 6. Shared-list lines changed (exact)

* `CMakeLists.txt`: `runtime/src/gekko/native_mtn_track.cpp` after `runtime/src/gekko/native_vec_math.cpp` in the runtime source
  list; the `mtn_track_contract_tests` executable / `add_test` block (TIMEOUT 1800, labels `reference;native-contract`) after
  the `vec_math_contract_tests` block.
* `extract/native-owners.txt`: a commented block appended at the end, 10 rows: 0x804162AC, 0x80416728, 0x8041669C, 0x80415300,
  0x80415170, 0x80414B80, 0x80421358, 0x804213FC, 0x8042148C, 0x80421518.
* `extract/liveness-roots-lifted.txt`: regenerated with the `native_liveness_roots_lifted` test's arguments; one line:
  `0x80416728 census` -> `0x80416728`.
* `runtime/include/gekko/system.h`: `#include "gekko/native_mtn_track.h"`; `runtime/src/gekko/system.cpp` (CRLF file, CRLF kept):
  `mtn_track::install(cpu_);` after `vec_math::install(cpu_);`; `runtime/tests/native_file_api_tests.cpp`:
  `add_owner_addresses<mtn_track::Owners>` after the vec_math line.
* Not changed: `SHADOW_GC_REMOVAL_RATCHET`, wait-points, liveness-roots.txt, native-owners-r2.txt. Measured owner rows: the generator log says "native owners: 555 rows emit only a thunk" for the base and 564 after this
  branch (+9 rows for 10 owners; inferred: 0x80416728 is an entry inside the generated body of 0x8041669C, see fn_8041669C_e80416728) (the main session lowers the ratchet after merging).
