# opt-intrinsics: the host code that implements game logic (optimisation round 2, 2026-11-12)

Branch `agent/opt-intrinsics`, cut from `agent/opt-integrate` aa54d9b (contract: `docs/agents/OPT_2026-11-12.md`). Scope: the host
C++ behind the native owners - `shadow::math`, `CharaColliIntrinsic`, the matrix and vector intrinsics, the texture lookup. No
guest owner was added or changed, no generator emission was touched, no shared list (`extract/*`) was changed, nothing deployed.

## Method

- Base: the coordinator's exe (`wt-opt-integrate/.build/watch`, aa54d9b) copied to `.build/exes/base.exe`; candidate: this tree's
  `.build/watch` (same configuration: lifted renderer, reference exe, non-shipping, watch symbols), built once in full (28 min at 7 %).
- Profile: `tools/perf/sample_threads.py` with two local changes (explicit module load for dbghelp, per-line mapping of every sample,
  not only translated code), 22 s of the benchmark replay from game second 8: 13,858 samples of the game thread (base) and 13,612
  (candidate). Raw data stays in `.build/prof` (not committed).
- Benchmark: the contract's Westopolis replay (`run.sh`, unpaced headless, 316 game s, 1280x720, `--no-audio`), `wall_seconds` of the
  session; every run through `tools/agents/capped_run.ps1 -CpuPercent 7 -Lock benchmark`, A and B alternating.

## What the base profile said (game thread, share of samples)

| group | functions (self) | share |
|---|---|---|
| CharaColli bounding test | `distance_squared` 1.51, `update` compares (`execute`/lambda) ~0.9, `fp_compare` from them ~0.5, rest 0.4 | ~3.3 |
| 3x4 matrix leaves | `serve_point` 0.72, `serve_translate_by` 0.64, `serve_scale_columns` 0.40 | 1.76 |
| RwMatrixMultiply | `run_wide` 0.90, ordered `run` 0.40 | 1.30 |
| `shadow::math` matrix compose (called from generated code) | `compose_ordinary` 0.51 + `compose_affine_matrix` 0.27 | 0.78 |
| vec3 helpers | `detail::add/subtract/multiply/fused` 0.73 (+ the serve functions) | ~0.9 |
| MSL double math | `rem_pio2` 0.48, `sin` 0.30, `kernel_cos` 0.17, ... | ~1.3 |
| texture lookup | `RwDriver::texture_lookup` | 0.44 |
| world collision | `run_line` 0.79, `run_sphere` 0.17 | 0.96 |

## Findings (P = proven, I = inferred, U = unknown)

- P: every one of these routines ran the original `ppc::fps_*` sequence on FPR bit patterns (single -> double conversion with a NaN
  test, helper with tiny-operand and midpoint tests, double -> single conversion, per operand and per result). For operands the
  game really feeds them the answer is the plain host single-precision operation. This is the same argument that made round 1's
  "plain-float fast path with an exact fallback" work, here applied to the leaves that were left.
- P: the preconditions under which the exact route (`fps_add/sub/mul/div/fused`) returns exactly the host float result are
  (a) every operand a zero or a normal finite single, (b) every result a zero or a normal finite single of magnitude >= 2^-100 (so
  no NaN, no overflow, no approach to the non-IEEE flush boundary 2^-126). add / sub / mul / div on singles through a double are
  innocuous (53 >= 2 * 24 + 2); the fused form is the once-rounded result (the exact route's double fma + rounding + midpoint
  repair is exactly that). The exact route changes no FPSCR bit there. `opt_intrinsics_diff_tests` proves it: 30 million random,
  edge and constructed trials per run, of which 27.3 million are accepted by the check; each accepted trial equals `fps_*` bit for
  bit with FPSCR[NI] both ways and sticky bits set, including "midpoint trap" triples (an exact single midpoint plus a perturbation
  far below the double's precision).
- P: `RwMatrixMultiply` (5.39 M calls in the benchmark): 27.5 % of the calls (1,480,351) left the old four-lane double fast path
  because of its single-precision *midpoint* test, not because of tiny or NaN values: the old design computed the fused step in
  double (4 lanes of `_mm256_fmadd_pd`), converted to single, and routed any double result that is exactly a single midpoint to the
  ordered body. A hardware single-precision fma has no double-rounding case, so it needs no such test. The new body is 99.9 %
  wide (5,380,805 of 5,385,484; the remaining 4,679 have a tiny or NaN value).
- P: `CharaColli` `update` (vf 0x88) made seven `fp_compare` calls per bounding test, each a read-modify-write of CR and FPSCR; only
  the last compare executed is observable.
- P: the DynamicCast memo is an `unordered_map` probed once per `cast`/`serve`; the key set is tiny (a few hundred entries).
- I: `compose_affine_matrix` (0.78 %) is bound by its checks (18 six-op range masks per call) and by what the generated marshalling
  does around it, not by the arithmetic; a store-forwarding stall after the caller's 32-bit stores was suspected from the per-line
  profile and measured: no difference (below).
- U: why `RwDriver::texture_lookup` (0.44 %) costs what it does: six dependent guest loads into scattered rasters and one hash probe;
  it looks cache-bound, not compute-bound. Not changed.

## Changes (files)

1. `runtime/include/gekko/fast_single.h` (new): the plain-float layer. `fast::Check` (collect "an operand/result left the ordinary
   range" while computing), `fast::fma`, `compare_flags` / `put_compare`, `LazyCompare` (fcmp cr0 with the CR / FPCC write done once).
2. `runtime/include/gekko/chara_colli_geometry.h` (new, code moved out of `chara_colli_intrinsic.cpp`): the RwV3d helpers and the
   0x8003C688 squared distance: `distance_squared_exact` (the unchanged original sequence), `distance_squared_fast`,
   `distance_squared` (fast, else exact). The fast route writes CR0 / FPSCR[FPCC] as the last compare the exact route would have made.
   `chara_colli_intrinsic.cpp`: `update()` uses `LazyCompare` (NaN words still take `ppc::fp_compare` in place).
3. `runtime/src/gekko/native_math_affine.cpp`: `serve_point` / `serve_translate_by` / `serve_scale_columns` read everything, compute in
   float, check, and store; the old bodies are kept as `serve_*_exact` (the fallback and the test's reference).
4. `runtime/src/gekko/native_vec_math.cpp`: the same for Sub, Add, Scale, SubFrom, AddTo, Dot, Cross, Length; old bodies `serve_*_exact`.
5. `runtime/src/gekko/native_rw_matrix.cpp`: `RwMatrixScale` (ops 1, 2) and `RwMatrixTranslate` (op 1) fast route with the old body as
   the fallback.
6. `runtime/src/gekko/matrix_multiply_intrinsic.cpp` / `.h`: `run_wide` rewritten in single precision, one output row per 4-lane
   vector (6 fused steps per 8 elements gone to 3 per row), inputs checked once for tiny values with a running unsigned minimum of
   `|bits| - 1`; the ordered body `run` and the whole semantics unchanged (it is the reference of the differential test; `run`,
   `run_wide`, `execute` are public for it).
7. `runtime/lift/math/matrix.cpp`: `compose_affine_matrix` gets a disjoint-spans route (`compose_disjoint`: loads both operands, no
   copies; Preserve mode checks only for a NaN output - every operation is the IEEE one; Flush mode keeps the old range rule with a
   cheaper running min / max); overlapping spans and every non-ordinary case run the old code unchanged.
8. `runtime/src/gekko/dynamic_cast_intrinsic.{h,cpp}`: a 256-slot direct-mapped front for the memo (same entries, same revalidation
   of the vtable's two words).
9. Tests: `runtime/tests/opt_intrinsics_diff_tests.cpp` (new), `CMakeLists.txt` (its target, next to `native_gameplay_tests`).

## Exactness evidence

- `opt_intrinsics_diff_tests` (new; ctest `opt_intrinsics_diff_tests`, ~20 s): old implementation vs new, bit for bit, random + edge +
  NaN / infinity / denormal / boundary-of-range inputs, FPSCR[NI] on and off and random sticky bits:
  primitives (30 M trials), `LazyCompare` vs the `fp_compare` sequence (CR, FPSCR, EQ answers, with NaN words), `distance_squared`
  (3 M calls incl. degenerate segments, centre on / past the segment ends, +-0 and random bound words; result bits, CR, FPSCR),
  `compose_affine_matrix` (3 M calls, both modes, against the scalar contract), `RwMatrixMultiply` `execute` vs ordered `run`
  (200 k calls: all GPR / FPR / CR / FPSCR / 1 KiB arena, outputs overlapping inputs and the stack frame), `math_affine` x3 and
  `vec_math` x8 against their `*_exact` bodies (every register, the arena, aliased operands), `DynamicCastIntrinsic` front cache vs a
  fresh instance while the vtable words change. 8 negative controls (a perturbed fast result in each family, a lost compare write) are
  caught. Coverage is asserted (>= 25 % of primitive trials per op on the fast route, > 1/3 of `distance_squared` calls, > 1/2 of
  matrix multiplies wide).
- The existing oracles pass unchanged: `math_affine_contract_tests`, `vec_math_contract_tests`, `rw_matrix_contract_tests`,
  `native_matrix_compose_tests` (the original words in the reference interpreter from the pinned DOL).
  `native_matrix_call_tests` needs the tests tree's generated recomp outputs, whose generation stops at this base with
  "0x80416338 branches into the interior of a native owner at 0x80416570" (round 1's Mtn owners in a non-lifted tree; not mine, not
  run).
- End to end: `--verify-matrix-multiply --verify-chara-colli` on the replay (40 game seconds): 639,121 RwMatrixMultiply calls all run
  through the guest body too, 0 differed; 24,548 CharaColli bounding tests checked, 0 differed.
- Game identity: `state.json` md5 identical to the base in every benchmark run of both arms (0f534f094c3a, 8 of 8 in the first
  alternation, see below). No audio code touched, so no PCM check.
- FPSCR rule: no owner leaves any FPSCR bit different from the original: the fast routes run only where the exact route changes none,
  and the compare-writing ones (`distance_squared`, `update`) write FPCC exactly as the last executed compare did.

## Benchmark (wall_seconds, replay 316 game s, 7 % cap, lock `benchmark`; other agents were building, so the whole machine drifts)

Two alternations of base / candidate, 4 rounds each, the first round of each alternation discarded (cold caches) but listed:

| alternation | arm | runs (s) | kept mean | kept min-max |
|---|---|---|---|---|
| 1 (machine loaded by builds) | base | (40.81) 41.71 41.09 41.31 | 41.37 | 41.09-41.71 |
| 1 | candidate | (38.12) 40.79 38.41 38.08 | 39.09 | 38.08-40.79 |
| 2 (lighter load) | base | (38.36) 38.10 38.32 37.86 | 38.09 | 37.86-38.32 |
| 2 | candidate | (36.77) 36.77 36.13 36.36 | 36.42 | 36.13-36.77 |

Candidate faster in 8 of 8 pairs: -5.5 % (alternation 1, one slow outlier 40.79 inside), -4.4 % (alternation 2); best-to-best
-3.2 % / -4.6 %. The profile accounts for about 3 points of the thread; the rest of the measured difference is likely the load
drift and cache effects, so the main session should repeat it before believing more than "about 3-4 %". `state.json` md5 identical
in all 16 runs (0f534f094c3a). Absolute times are higher than the contract's 34-35 s because other agents were building.

Profile (game thread, share of samples): the groups above fell from 16.1 % to 13.0 % of the thread (`distance_squared` 1.51 -> inlined
0.4, `fp_compare` 1.60 -> 0.98, `run_wide`+`run` 1.30 -> 0.45, `serve_point` 0.72 -> 0.37, `serve_translate_by` 0.64 -> 0.22,
`compose_*` 0.78 -> 0.68, `detail::add/sub/mul/fused` 0.73 -> 0.03).

Microbenchmark (ns per call new / old, memory included): point 16 / 29, translate_by 16 / 29, cross 8 / 19, length 5 / 19,
distance_squared 22 / 77-90.

## Rejected / not done

- `compose_affine_matrix` with scalar lane loads against the 16-byte loads (suspected store-forwarding stall after the generated
  marshalling's 32-bit stores): 14.8 vs 15.0 ns in a microbenchmark that mimics the marshalling; dropped.
- Relaxing the Flush-mode intermediate checks of `compose`: a tiny intermediate that cancels to a zero changes the sign of that zero
  (the original flushes the operand first); every intermediate stays checked in Flush mode.
- MSL `rem_pio2` / `sin`: already host double code (one `fctiwz` and three fused steps in the medium range); nothing exact to remove.
- `RwDriver::texture_lookup`: dependent guest loads, cache-bound; a cache keyed by the texture would have to repeat the same loads to
  validate itself.
- The world-collision sector walk (`run_line` 0.9 %, line 683 = the kd-tree node fetch): pointer chasing; a restructure would change
  the mirror's layout, not just its arithmetic.
- The remaining `fp_compare` (0.98 %) is the generated code's, and `std::function` / `serve_native_owner` dispatch is opt-host2's.

## Open problems

- `extract/liveness-roots-lifted.txt` is stale at the base itself (16 unlisted addresses: 0x80414B80, 0x80415170, 0x80415300,
  0x804162AC, 0x80416338, 0x8041669C, 0x80421358.. and 0x80001234 / 0x805E2380): the round-1 merge did not regenerate it. Not touched
  here (this branch adds no root literal; the plain `liveness-roots.txt` check passes); the coordinator regenerates it once at integration.
- The tests tree cannot generate the recomp outputs at this base (message above), so tests that need `shadow_recomp` do not build there.

## Shared-list lines changed

None in `extract/*`. `CMakeLists.txt`: one block (target and ctest row `opt_intrinsics_diff_tests`) inserted before `native_gameplay_tests`.
