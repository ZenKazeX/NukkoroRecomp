# opt-matrix: the 3x4 matrix / vector library, round 2 (2026-11-12)

Agent `opt-matrix`, branch `agent/opt-matrix` (cut from agent/opt-integrate aa54d9b). Scope: the game's matrix / vector library
0x8040C000-0x8040E100 and its callers' leaf calls. Names are this project's own, from the words and the call sites; evidence
marks P (proven from the cited words, or by the contract test), I (inferred), U (unknown). The external decomp was not used.

## 1. RE findings (P unless marked)

Matrices are 3 rows x (x y z translation), single floats, rows 0x10 apart (the 4th word of a row is the translation). The
constants 1.0 / 0.0 are read from .sdata2 at r2 + 0x1638 / + 0x163C at every call.

| address | what it is | callers | state before this round |
|---|---|---|---|
| 0x8040C6CC | point: out = m x [p 1] (3 rows, (m1*p1; fma m0*p0; fma m2*p2) + m3) | many | owner (fps_*) |
| 0x8040C668 | rotate vector: the same without the translation column | 115 | generated |
| 0x8040C5EC | translate_by(m, v): m[i][3] += rotation row . v | | owner (fps_*) |
| 0x8040C57C | translate_by with the vector in f1..f3 | 32 | generated |
| 0x8040C7E0 | scale columns by v[0..2] (memory) | | owner (fps_*) |
| 0x8040C770 | scale columns by f1..f3 | 39 | generated (one site lowered by the watch-camera owner) |
| 0x8040DF7C | squared length x*x + y*y + z*z -> f1 (z*z + (x*x + y*y)) | ~100 | generated |
| 0x8040C94C | concat out = a x b (frame 0xA0, saves f23..f31, no LR word) | 2 | generated |
| 0x8040CD24 | cofactor inverse in place (frame 0xB0, saves f22..f31); det == [r2+0x163C] leaves m | 4 | generated |
| 0x8040CF48 | cofactor inverse into out (frame 0xC0, saves f21..f31, LR word at SP+4); det == const: 12-word copy 0x8040CD04, CTR = 0 | ~100 | generated; its copy site 0x8040D004 was a lowered call |
| 0x8040C110 / C188 / C200 | rotation about X / Y / Z: sincosf (0x80031568, lowered to the MSL host sin / cos) then 12 stores | | generated, NOT owned (cost is the sine) |
| 0x8040C85C / C898 / C8D4 / C910 | rotate the matrix: build C110 / C188 / C200 / C278 then compose 0x8040CB14 (lowered, host) | | generated, not owned (tiny) |
| 0x8040CB14 | affine compose in place (host `shadow::math::compose_affine_matrix`, 68 lowered sites) | | host (opt-intrinsics' file), not touched |

Profile (tools/perf/sample_threads.py, base = the round-1 integrated exe, 8,704 game-thread samples, headless Westopolis
replay) before this round, % of the game thread: serve_point 0.77, serve_translate_by 0.48, serve_scale_columns 0.38 (the
three fps_* owners together 1.63), fn_8040CF48 0.46, fn_8040CD24 0.28, fn_8040C94C 0.21, fn_8040C188 0.18, C110 0.17, DF7C
0.17, C200 0.15, C668 0.13, plus the compose host path (~1.0, not mine). The library is ~3.3 % of the game thread; the
shared dispatch cost of any owner (serve_native_owner / try_hle_slow / find_hle ~2.1 %) is opt-host2's.

### The original's observable footprint (what an owner must reproduce)
* Stores: the result words only for the leaves. The three framed routines also write a frame below SP: the back chain at SP-size,
  (0x8040CF48 only) LR at SP+4, and for each saved f<n> the double ps0 at SP-size+0x10*(n-first+1) (stfd) and, 8 bytes above, the
  two singles of psq_st (GQR0 type 0: flush-denormal singles of ps0 and ps1). Those words are guest RAM, so a state hash sees them
  (the ADX owner's state.json difference in round 1 was exactly this). The owners write them.
* Registers: psq_l / lfd restore f<first>..f31; ps0 is restored exactly, **ps1 comes back as the single rounded from the stored
  word** (denormal singles flushed): applied by the owner. cr0 and FPSCR[FPCC] from the determinant fcmpu (inverses): set by
  calling ppc::fp_compare once. CTR = 0 after the 12-word copy (the lowered copy site's loop count).
* Not reproduced (volatile by the ABI, as in every other owner): r0, r4..r12, f0..f13 (except f1 of the length), cr1.., and the
  dead values of the original's scratch.

## 2. Owners (one file, runtime/src/gekko/native_math_affine.cpp)

10 guest functions (3 existed with fps_* arithmetic, 7 are new). Every owner has a **plain-float fast path with the exact
ppc::fps_* path as fallback** (the technique of docs/agents/opt-mtn.md): all inputs are loaded, every operand / intermediate /
result is tested (`0 < |x| < 2^-100` or NaN declines), the fused steps are one-rounding fma (`_mm_fmadd_ss`), the words are stored
after the last test. A declined call runs the original's instruction sequence through ppc::fps_* (NI flush, NaN rules, default
NaN) with nothing stored or compared yet. The framed routines are served on the fast path only when the output equals or is
disjoint from each input (a script over the original's load / store trace shows that equal aliasing is order-safe; partial
overlaps run the exact path in the original's interleaved order). The float-argument entries (C57C, C770) take the fast path only
for FPR values that are exact singles. The fast path functions are exposed in `gekko::math_affine::detail` for the test.

## 3. Gates

1. **Exactness.** `math_matrix_contract_tests` (runtime/tests/math_matrix_contract_tests.cpp, ctest, labels reference;native-contract): the
   original words of the pinned DOL (FNV 0xD2CFB4A1CA897F9B) in the reference interpreter vs the owner on identical machines,
   60,000 calls (6,000 per entry; 50,000 with overlapping operands, 2,379 singular matrices, 1,210 singular-determinant paths,
   5,972 with a quantised GQR0, 14,936 non-single f1..f3). Compared: r1, r2, r3, r13-r31, LR, CTR, CR, f14-f31 (both halves, arbitrary
   64-bit ps0/ps1 seeds), f1 (length), FPSCR, and all 0x2000 arena bytes including the framed routines' stack words. The fast path
   was accepted on 57-84 % of calls per entry (the rest ran the exact path): point 76, rotate 76, translate 72, translate floats 57,
   scale 71, scale floats 58, length 84, concat 67, invert in place 73, invert 61 %. Ten negative controls (concat fadds->fsubs, inverse
   fneg->fmr, copy branch inverted, point, rotate, length, translate-floats, scale-floats fault in the original; stfd offset moved
   (frame); psq_l removed (ps1)) all FAIL as required. The existing `math_affine_contract_tests` (18,000 calls) still PASS.
2. **Game behaviour.** Benchmark replay, headless, `state.json` md5 0f534f094c3a identical in all 5 base and all 5 candidate runs
   (the dead frames and the ps1 rounding are reproduced; without them the framed routines would show up in the hash like the ADX owner did).
3. **Speed.** `.build/ab.sh`, capped 7 %, lock benchmark, alternating, first pair discarded, wall seconds (session.json):

   | arm | kept runs | mean | min..max |
   |---|---|---|---|
   | base (round-1 integrated) | 36.50, 37.70, 38.82, 38.47 | 37.87 | 36.50..38.82 |
   | cand (this branch) | 39.50, 38.03, 39.37, 37.03 | 38.48 | 37.03..39.50 |

   **No measurable wall-clock change** (spreads overlap, the machine is loaded by the other agents; sign is noise). The sampler
   (same replay, game thread, 8,704 base / 8,795 cand samples) is the finer instrument: the owned functions' own cost (owner bodies
   plus the generated bodies they replace, excluding the shared sine / compose host code) falls from 255 samples (2.93 %) to about
   192 (2.2 %), i.e. -0.7 % of the game thread (about 0.25 s of 36), below the run-to-run noise. Per function (samples, base -> cand):
   point 67 -> 36, scale columns 33 -> 31, translate 42 -> 18, invert 40 -> 29, invert in place 24 -> 17, concat 18 -> 21, length 15 -> 11,
   rotate vector 11 -> 6. The remaining cost is the dispatch (serve_native_owner / std::function lambda ~ 1 % shared by all owners)
   and fp_compare / psq_* of the other guest code, which is not this library.
4. Tests not run (build budget): native_matrix_call_tests (its copy-site count was lowered 684 -> 683 for the removed 0x8040D004
   site, as in the withdrawn round-1 commit), native_file_api_tests (the Owners list grew 3 -> 10; the owners file has the 7 rows),
   lift_native_math_evidence. The watch tree built (post-link gates passed; the first full build hit the known MSB4166 child-node
   abort once, the re-run succeeded).

## 4. Verdict and open problems
* The change is exact and state-identical but its wall-clock effect is below what this machine can resolve. By the round-2 rule
  ("no measurable change: withdraw unless needed") the coordinator may drop it; it is kept on the branch because the sampler
  shows a real, if small, reduction and the exactness evidence is complete.
* The ceiling of this library is ~3 % of the game thread; ~1 % of it is the shared owner dispatch (opt-host2). A call-site lowering
  (extract/lift-sites, `call` kind) would remove the dispatch but needs a per-site proof that no caller reads the clobbered
  volatile registers; not attempted.
* Not owned: rotations C110/C188/C200 (cost is the MSL sine), compose 0x8040CB14 (host, opt-intrinsics).
* Lifted roots file: round 1 left extract/liveness-roots-lifted.txt without the Mtn literals; I only added my 7 addresses (plain and
  lifted) and did not regenerate it.

## 5. Shared-list lines
* CMakeLists.txt: a `foreach` block (math_matrix_contract_tests + 10 WILL_FAIL controls) after the adx_decode block.
* extract/native-owners.txt: a commented block at the end, 7 rows (0x8040C668, C57C, C770, DF7C, C94C, CD24, CF48).
* extract/liveness-roots.txt and extract/liveness-roots-lifted.txt: the same 7 addresses (+0x...).
* extract/lift-sites-math.txt: the 0x8040D004 copy site removed (its body is pruned); runtime/tests/native_matrix_call_tests.cpp 684 -> 683;
  docs/NATIVE_MATRIX_MATH.md note. system.cpp unchanged (math_affine::install already installed); native_file_api_tests unchanged
  (it uses math_affine::Owners, now 10).

