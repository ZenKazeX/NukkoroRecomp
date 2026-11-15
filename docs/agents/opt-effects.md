# opt-effects: the Sonicteam effect system's particle update (2026-11-11)

Agent `opt-effects`, branch `agent/opt-effects`, cut from gc-removal 17dd6e8. Scope: the effect system 0x80422700-0x8043B000
(6.0 % of the game thread in the round's profile). Everything below is proven from `main.dol` (pinned, FNV-1a identity checked by
the test); the external reference decomp was used for orientation only (its names agree with ours where noted), nothing was copied
from it.

## 1. RE findings (P = what it does, I = inputs, U = users; addresses are `main.dol`, PAL GUPP8P)

Object model (offsets from the vtable layouts in .data, the call graph and what each function reads and writes):

| object | layout |
|---|---|
| particle system S (0x13C bytes) | +0x18 flags (bit 1: units are aged), +0xFC / +0x100 unit list head / tail, +0x108 definition flags (0x100: no age limit), +0x110 unit count, +0x114 gravity, +0x118 drag, +0x124 -> {+0x0C age limit}, +0x134 / +0x138 pool words (written into the global pool object, see below) |
| unit (0x7C bytes, vtable object) | +0x00 vtable (slot +8: deleting destructor), +0x04 position, +0x1C velocity (3 floats each), +0x4C flags (bit 0: finished), +0x50 age, +0x54 four 10.22 fixed-point colour ints, +0x64 prev, +0x68 next, +0x6C script cursor (0 = no script), +0x70 / +0x74 command list head / tail |
| command (vtable object) | slot +0x08 deleting destructor(this, 1), slot +0x0C `update(this, dt) -> 1 alive / 0 finished`; +0x04 data record (byte +2: end the unit when the command ends), +0x08 prev, +0x0C next, +0x10 started byte, +0x14 -> the unit's flags word, +0x18 -> the value the command moves (0 = spent), +0x1C elapsed, +0x20 duration, +0x24 rate (an int, a float or three floats by class) |

| function | P / I / U |
|---|---|
| 0x80427E2C particle system Update(dt) | P: the per-frame entry (flag tests, ages the system, calls the steps below). I: r3 system, f1 dt. U: the particle class's vtable slot (0x8056B58C); the two renderer-side subclasses' updates call it as the base update (0x804341E4, 0x80434B80). Not owned (0.05 %). |
| **0x80428044 unit step(dt)** (0x2F4 bytes, 1.48 % self) | P: four passes over the unit list: (1) age units and flag the ones past `limit = 0.0001f + S[0x124][0xC]` (only when S[0x108] & 0x100 is clear; the flag tested per unit is the SYSTEM's S[0x18] & 2), (2) per unit not finished: the script step 0x80429224, then every command's update; a command that returns 0 is unlinked and deleted (vtable slot +8), (3) unlink and delete the finished units, (4) integrate: position += velocity * dt, the drag blend (|v| > 0.0001f: v *= (|v| - |v| * drag) / |v|, or 0 when |v| * drag > |v|), the gravity pull velocity.y -= dt * S[0x114] (fused). Between the passes it writes S[0x134] / S[0x138] into the pool global's object (word +4 of the object the pointer at 0x805E25C8 names; the original returns the old word, nobody reads it). I: r3 S, f1 dt (callers: the three `bl` sites in 0x80427E2C). U: only 0x80427E2C. |
| 0x80429224 script step | P: runs a unit's command script (a byte-coded list at +0x6C): allocates command objects from a pool and appends them to the unit's command list, ends the unit when a script ends. **Does nothing when the cursor +0x6C is 0** (its first instructions); allocation heavy, not float code. I: r3 unit. U: 0x80428044 only. Not owned (0.16 %). |
| **0x80423F20 alpha ramp command update** (0x130 bytes, 0.64 %) | P: `*target += int(rate * dt)` on a 10.22 fixed-point alpha (the colour bytes 0x80434824 emits are the ints >> 22), clamped to [0x3FFFFF, 0x3FC00000]; below the lower clamp the target is released (command spent), above the upper the rate stops. int -> double through the 2^52+2^31 bias (fsubs to single, fmuls dt, fctiwz). I: r3 command, f1 dt. U: vtable slot +0x0C of the alpha classes (data words 0x8056B2E8, 0x8056B8B4, 0x8056B920). Reference: `EffCmdMatAlphaExec::Update`. |
| **0x80425B20 scalar command update** (0xB0, 0.31 %) | P: `*target += rate * dt` as one fused fmadds. U: ten vtables (0x8056B408, ...668, ...694, ...6C0, ...6EC, ...848, ...874, ...AA8, ...AD4, ...B00). Reference: `EffCmdSingleExec::Update`. |
| **0x80425544 vector command update** (0xD8, 0.28 %) | P: `target[0..2] += rate[0..2] * dt` (Scale into a frame temporary, then AddTo; both 0x8040D helpers are owned natively, the owner inlines them with the original's operand roles). U: vtables 0x8056B0F0, ...11C, ...148, ...3C0. Reference: `EffCmdVector3Exec::Update`. |
| **0x80424958 2D scale command update** (0xD8, 0.19 %) | P: byte-for-byte the same code as 0x80425544 (the reference names it `EffCmd2DSclExec`). U: vtable 0x8056B330. |
| 0x80434824 particle renderer vf (0x348, 0.60 %) | P: the vertex fill of the particle tank (the repository's earlier docs call it the PTank fill; the object at S[0x13C] is the RenderWare particle tank): locks five vertex streams of it (the attribute lock 0x80449458 x5, stream flags 1, 2, 4, 0x20, 0x80: positions, colour bytes, scale pairs, rotation, uv rectangle), writes one record per unit (positions optionally transformed by the matrix at S+0x54 through 0x8040C6CC; colour = the four fixed-point ints >> 22 as bytes; the rotation float is wrapped into [-a, a] by repeated +-2a in place), unlocks (0x804495E8), sets the stream's dirty bit and vertex count, then DRAWS: render-state get / set through the function pointers at +0x24 / +0x20 of the object at *(0x805F265C), and the tank's render callback at +0x48 of S[0x13C]'s target. Feeds the renderer: the vertex streams are guest memory the R2 renderer reads. Not owned: it ends in the RW draw, which an owner would have to call back into (see section 5). U: vtables 0x8056BF34, 0x8051DABC, 0x8053D9CC. |
| 0x8043531C shimmer renderer vf (0xE48, 0.56 %) | P: the same job for the shimmer (heat-haze) effect: much larger, per-unit geometry. Not analysed beyond its role. U: 0x8056C038, 0x8051D9BC, 0x8053D738. |
| 0x8042B610 / 0x8042B620 | P: swap the word at +4 of an object / return the global pool pointer (0x805E25C8). Two instructions each; inlined by the owner. |

## 2. Owners added

`runtime/src/gekko/native_effect_update.cpp` (+ `include/gekko/native_effect_update.h`), installed by `effect_update::install` after
`vec_math::install` in `System`:

| address | owner |
|---|---|
| 0x80423F20 | alpha ramp command update |
| 0x80425B20 | scalar command update |
| 0x80425544 | vector command update |
| 0x80424958 | 2D scale command update (same code as 0x80425544) |
| 0x80428044 | unit step |

Design:
- Every float operation is the original's, in its operand roles and order, written with the CPU's own `ppc::fps_*` helpers (single
  results, NI policy, one rounding for the fused forms, NaN rules), every store the original's `stfs` word, every load / store in
  the original's order (so a target that aliases the command's own fields behaves as in the original).
- The unit step walks the lists natively. It calls the game only where the game's own code is the contract: the script step
  0x80429224 (only when a unit has a script cursor: the function does nothing without one), a command class other than the three
  owned update functions (through the vtable; the three are inlined and never leave C++), and the deleting destructors.
  Nested calls go through `gekko::guest_call` (the existing seam: reference build = nested interpreter, production = the
  translated function). A callee that does not return stops the run with an ExecutionFault, never a silent cut.
- `vec_math::serve_length` (already proven) computes |v| for the drag step.
- Not kept (ABI-dead or console scratch): f0..f13, r0, r3..r12, ctr, cr, the original's frame words, FPSCR FPCC, and an
  MSR[FP]-off check (never off). FPSCR sticky bits set by the owner's arithmetic and unordered compares are kept; those set by
  `serve_length` are not (its own documented limit).

## 3. Gate evidence

### 3.1 Exactness: `effect_update_contract_tests` (CTest, labels reference;native-contract)

Original instructions from the pinned DOL (FNV-1a 0xD2CFB4A1CA897F9B checked) run in the reference interpreter on one machine; the
owner runs on a second machine with the same DOL and the same random memory image. Result of the final run:
`PASS effect update contract: 312000 calls compared (controls caught: NI 824, dt 4743, perturbed alpha 2863, unfused 393)`
(job CPU 18 s, 2.4 % of the machine under the 10 % cap).

- Calls: AlphaRamp 60,000; Scalar 60,000; Vector 60,000; Vector2D (0x80424958) 60,000; UnitStep 20,000 (random systems of 0..5
  units with 0..4 commands each in all five classes: the three owned update functions, the 2D alias, and a stub class the owner
  must reach through the game's own function); UnitStepNoScripts 20,000 (every unit without a script cursor and the ORIGINAL
  script step 0x80429224 running on both machines: proves the "does nothing without a cursor" shortcut the owner takes).
  Both FPSCR[NI] modes alternate. Float fields: ordinary values, +-0, denormals, the 2^-126 boundary, infinities, NaNs with
  payloads, huge and tiny normals. dt is always a value a single holds. Command targets alias the command's own +0x18 / +0x1C /
  +0x24 words and the unit's flags word in 1 of 4 commands; alpha targets sit at and around both clamps; rates include 0 and
  the +-2^31 edges.
- The script step, the destructors and the unknown update class are stubs installed on BOTH machines (they log tag, object, f1 and
  the pool word, and mutate memory from a hash of the object and the log). So compared are: every word of the 10 KiB arena
  including the call log (which function, which arguments, in which order, with which pool word installed), the pool global,
  r1, r14..r31, f14..f31, an update's r3 result, and the FPSCR control byte (NI / RN / enables). Not compared: FPSCR sticky bits
  and FPCC (documented in the header), f0..f13, r0, r3..r12.
- Callee-saved FPRs: the original saves / restores f30 / f31 with `psq_st` / `psq_l` (a single-precision round trip of ps1);
  the test seeds ps1 with values a single holds. A ps1 lane holding a non-single double would come back rounded from the
  original and untouched from the owner; the game's ps1 lanes of f30 / f31 are singles (lfs fills both lanes).
- Negative controls (the same comparison, wrong natives): FPSCR[NI] flipped around the unit step (caught 824 of 8000: only calls
  with denormal operands differ, as they must), dt off by 2^-29 relative (4743), a perturbed alpha result (2863), the scalar
  update with multiply and add rounded separately (393 of 8000).
- Mutation sweep (scratch script, not committed): 18 single-line mutations of `native_effect_update.cpp` (upper clamp `>` to
  `>=`, lower clamp, bias operands swapped, fctiw rounding instead of truncation, unfused scalar, duration compare, age flag
  read from the unit instead of the system, age limit without the epsilon, count not decremented, gravity sign, drag compare
  swapped, drag epsilon, pool words swapped, destructor flag 0, tail pointer not relinked, finished unit not skipped in the
  script pass, update result tested on the whole word, vector lane order). Each mutant was rebuilt and run: all 18 FAIL. One
  survived the first version of the test (pool words swapped: nothing observed the pool word between the two swaps); the stubs
  now log it and that mutant is caught.
- Other CTests (tests tree): `native_file_api_tests` PASS (owner list == extract/native-owners.txt, includes the 5 rows),
  `native_liveness_roots` and `native_liveness_roots_lifted` PASS. `lift_purity` fails exactly as at the snapshot (baseline list).

### 3.2 Game behaviour: Westopolis replay, `state.json` identity

Harness: `.build/bench/run.sh` (a copy of the round's `run.sh`: `--boot-stage stg0100 --headless --no-audio`, the Westopolis
tape, 316 game seconds, 1280x720 native output), lifted `.build/watch` tree, `-DSHADOW_DEPLOY_DIR=` (nothing deployed).
`state.json` holds the game ticks, the VI / render frame counts and a hash of EVERY 64 KiB block of guest memory at the end of
the replay, so an identical md5 means identical guest RAM, not just identical counters. 20 runs (10 base, 10 candidate; one
earlier ad-hoc pair gave the same value): **every `state.json` md5 = 3a75268e...** (12,798,000,000 ticks, 9,477 VI frames,
18,920 render frames). So A/A and B/B are identical as well as A/B. (The round's profile quotes
ed5d7b6b: that is gc-removal plus uncommitted work; this branch is cut from 17dd6e8, so its base arm has its own value, and it
equals the candidate's.) PCM: audio is not touched and the run has `--no-audio`.
Owner hits in the replay (`HLE hooks ... hits`, identical in every candidate run): unit step 341,679; alpha ramp 5,810 and
vector 6,208 as standalone calls (the command updates are otherwise inlined into the unit step, whose guest calls - the script
step, the other command classes, the deleting destructors - run through `guest_call`, 0.15 % of the thread in the profile).

### 3.3 Speed

Setup: base = `.build/bench/base.exe` (this branch before the owners, built first under the cap), candidate = `cand1.exe`
(this branch at commit 2c724ee). `.build/bench/ab.sh` / `ab2.sh` alternate base / candidate (the order flips each pair from
pair 5 on); every run goes through `capped_run.ps1 -Lock benchmark`, 316 game seconds. Metric: `host.guest_run_cpu_seconds` of
`session.json` (the game thread's CPU time; the process wall is within 0.3 s of it). The machine was shared with the other
agents' builds and benchmarks the whole time (up to seven `benchmark` lock waiters), so timings drift by +-3 s between
neighbouring runs.

| pair | base cpu s | candidate cpu s |
|---:|---:|---:|
| 1 (discarded) | 35.78 | 35.67 |
| 2 | 35.59 | 36.05 |
| 3 | 37.58 | 33.78 |
| 4 | 34.48 | 34.39 |
| 5 | 38.84 | 39.41 |
| 6 | 40.94 | 37.84 |
| 7 | 34.70 | 38.47 |
| 8 | 34.78 | 39.48 |
| 9 | 35.23 | 33.33 |
| 10 | 34.52 | 33.23 |
| mean, pairs 2-10 | 36.30 | 36.22 |
| median | 35.23 | 36.05 |
| min / max | 34.48 / 40.94 | 33.23 / 39.48 |

**Result: no measurable change in wall / CPU time** (the means differ by 0.08 s, the spread is 6.5 s; the lowest runs favour the
candidate by about 1 s, the median does not). The wall clock cannot resolve the effect size the profile below predicts on this
machine; the main session should repeat on a quiet machine.

What does resolve it is the sampler (`tools/perf/sample_threads.py`, game thread, the round's method), as a share of the game
thread's samples:

| | translated code in 0x80422700-0x8043B000 | owners (this work) | `guest_call` |
|---|---:|---:|---:|
| round's baseline profile (11,522 samples) | 5.89 % (679) | - | 0.30 % |
| candidate (12,659 samples, 20 s window) | 1.99 % (252) | 2.58 % (vector 0.89, unit step 0.68, run_commands 0.42, scalar 0.31, alpha 0.28) | 0.15 % |

The effect range cost about 5.9 % (plus the Vec3 helper owners it called, about 0.5 %) and costs about 4.7 % now: roughly
1.5 % of the game thread (about 0.5 s of 36). The owners are not much faster per call than the translated code, which is already
plain C++ with the same arithmetic helpers: a hot-cache microbenchmark of the owners
(`effect_update_contract_tests main.dol bench`) gives 8.2 ns (alpha), 8.7 ns (scalar) and 15.4 ns (vector) per call. The cost is
in the number of calls (a unit step walks tens of units with a few commands each; about 10^7 command updates in the replay),
not in call overhead.

## 4. The exact shared-list lines changed

- `extract/native-owners.txt`: appended one commented block, 5 rows: `0x80423F20`, `0x80425B20`, `0x80425544`, `0x80424958`, `0x80428044`.
- `extract/liveness-roots.txt` and `extract/liveness-roots-lifted.txt`: regenerated with `liveness_roots.py` (the
  `native_liveness_roots*` test arguments); 9 lines added to each: the five owner addresses, `0x80429224` (the script step the
  owner calls) and the constants `0x8051B848`, `0x8051BC54`, `0x8051BC58`. The four return addresses the owner hands
  `guest_call` are marked `// liveness-roots: value-only` (they root nothing).
- `CMakeLists.txt`: `native_effect_update.cpp` after `native_vec_math.cpp`; the `effect_update_contract_tests` target after
  `vec_math_contract_tests`.
- `runtime/include/gekko/system.h`: `#include "gekko/native_effect_update.h"`; `runtime/src/gekko/system.cpp`:
  `effect_update::install(cpu_);` after `vec_math::install(cpu_);` (system.cpp has mixed line endings; the inserted lines use the
  CRLF of the line before them).
- `runtime/tests/native_file_api_tests.cpp`: `add_owner_addresses<effect_update::Owners>`.
- `SHADOW_GC_REMOVAL_RATCHET` not touched. The owners prune no SDK body (they are game code).

## 5. Open problems and what was left alone

- **0x80434824 (particle renderer, 0.48-0.60 %) and 0x8043531C (shimmer renderer, 0.56 %) are not owned.** Both write the locked
  vertex streams (guest memory the R2 renderer reads) and then END in the RenderWare tank's render callback
  (+0x48 of the tank, with the render-state get / set pointers at +0x24 / +0x20 of the object at *(0x805F265C)); an owner would have to run that draw through a nested `guest_call`, whose
  documented limit (the callee must not yield) cannot be proven for the draw. The per-unit fill loops are plain float / byte code
  and would be exact to write; the proof obligation is the draw. Left as a decision for the main session.
- 0x80429224 (script step, 0.08-0.16 %): allocation and constructor code, not owned. Other command classes (0x80425088 pattern
  animation 0.08 %, 0x80424324 material colour 0.05 %) are not owned; the unit step reaches them through `guest_call` (0.15 %).
- `serve_unit_step` takes `vec_math::serve_length` for |v|; that owner (already in this tree) does not keep FPSCR sticky bits, so
  those are not compared. The original passes f1 (dt or the age limit) into the script step; the script step does not read f1 in
  its own instructions and the replay state is identical, but the callees of the script step were not proven not to read it.
- A callee of a nested `guest_call` that yields (or leaves to the dispatcher) stops the run with an ExecutionFault by design (the
  same seam as the RenderWare owners); none occurred in the replays.
- The snapshot's other test failures (`lift_purity` and the rest of the baseline list) are unchanged by this work.
- The wall-clock gain is below this machine's noise (section 3.3): do not quote "faster" without a repeat on a quiet machine.

## 6. Commands

    # trees (this worktree; deployment disabled)
    python -B tools/platform/regen_manifest.py --no-names      # the names patch no longer applies to the shared copy: totals unchanged
    cmake -S . -B .build/watch -G "Visual Studio 18 2026" -A x64 -DSHADOW_RENDER_LIFTED=ON -DSHADOW_BUILD_REFERENCE=ON ... -DSHADOW_DEPLOY_DIR=
    cmake --preset tests ... -DSHADOW_DEPLOY_DIR=
    # all through tools/agents/capped_run.ps1 at 10 %
    cmake --build .build/tests --config Release --target effect_update_contract_tests -- -m:2 -v:m
    .build/tests/Release/effect_update_contract_tests.exe H:/ShadowTheHedgehogDiskGC_Extract/game/sys/main.dol
    bash .build/bench/ab2.sh <first> <last> <candidate exe>      # alternating replay, lock benchmark

Build under the cap: base watch build 1,769 s wall (7.7 % of the machine); the candidate's first build hit the known
`C1090 PDB API call failed` (recomp_table.cpp), the second (incremental) run succeeded (151 s); tests-tree target 278 s.
