# Agent report: opt-adx (optimization round 2026-11-11, branch agent/opt-adx, cut from gc-removal 17dd6e8)

Scope: the "CRI 3.9 %" bucket of the main session's profile (docs/agents/OPT_2026-11-11.md). Result: **one owner shipped
(the ADX stereo block decoder 0x803C5B2C), three more built, proven exact, measured without gain and withdrawn.** No wall-clock
change is measurable on the benchmark; the decoder's own cost fell from 0.48 % to 0.14 % of the game thread's samples.

## 1. RE findings: what the hot "CRI" functions are

The 3.9 % was a loose address bucket. Re-summing the main session's samples (`samp-2026-11-11/samples.json`, 11,522 game-thread
samples) by region: ADX/CRI 0x803A0000-0x803D0000 is **1.34 %**, libcsd (Chao::CSD 2D UI, 0x8035-0x8036) 2.07 %, the game's
own 3x4 matrix / vector library 0x8040C000-0x8040E100 2.13 %. Only one hot function is CRI.

| address | % game thread | what it is (evidence) |
|---|---|---|
| 0x803C5B2C | 0.48 | **ADX 4-bit ADPCM decoder, two channels to two separate 16-bit buffers** (words, below). Reference agrees (ADX_DecodeSte4AsSte). |
| 0x8040CF48 | 0.29 | 3x4 affine matrix inverse into a second matrix (3x3 cofactor expansion, determinant compare against the 0.0 constant at r2+0x163C; zero determinant copies the matrix through 0x8040CD04). Game code, not CRI. |
| 0x8040CD24 | 0.13 | the same inverse in place (zero determinant: untouched). |
| 0x8040C94C | 0.23 | 3x4 affine matrix concatenation out = a x b (rotation rows and translation column). |
| 0x8040C188 | 0.21 | builds a rotation about Y: calls sincosf 0x80031568, stores [c 0 s 0; 0 1 0 0; -s 0 c 0]. Siblings 0x8040C110 / 0x8040C200 (X / Z). |
| 0x8040DF7C | 0.21 | Vec3 squared length, ((x*x + y*y) + z*z) with fmuls / fadds, 9 words. |

### 0x803C5B2C in detail (read from the words; the reference's source was read for orientation only, nothing copied)
- Entry: only `bl` site 0x803C5B00 inside the wrapper 0x803C5AC8, which calls 0x803C5B2C when the word at 0x805B8FB8 is zero and the
  sibling 0x803C5D8C otherwise (a different, not owned variant); the wrapper is entered from 0x803C4D50.
- Arguments: r3 source (block pair 0), r4 = n samples per channel, r5 / r7 PCM out channel 0 / 1, r6 / r8 history {s16 h1, s16 h2},
  r9 / r10 prediction coefficients (s16, 12-bit shift); stack of the caller: +8 key pointer, +0xE s16 key multiplier, +0x12 s16 key
  addend. Return r3 = n, or 2 * pairs done when a scale word has the stop bit 0x8000 (histories are then NOT stored).
- Format: a pair is 0x24 bytes, channel 0 block (18 bytes: scale word + 16 data bytes) then channel 1 block; one pair gives 32
  samples per channel. scale = ((word ^ key) & 0x1FFF) + 1, then key = (key * mult + add) & 0x7FFF, stored after each channel; high
  nibble = byte >> 4, low nibble from the word table at 0x80516B18 (sign-extended nibble) indexed by (byte << 2) & 0x3C;
  sample = clamp16(nibble * scale + ((c1 * h1 + c2 * h2) >> 12)) with 32-bit wrapping mullw / add.
- Observable leftovers kept: CTR (0 once a pair ran), **XER[CA] (the last srawi; found by the state A/B below: saved thread
  contexts hold it)**.

## 2. Owners

| owner | file | state |
|---|---|---|
| 0x803C5B2C ADX stereo 4-bit block decoder | runtime/src/gekko/native_adx_decode.cpp / .h | **shipped in this branch** |
| 0x8040C94C concat, 0x8040CD24 and 0x8040CF48 inverses | commit 655bc25 (native_math_affine + math_matrix_contract_tests) | built, exact, **withdrawn** by the next commit |

Withdrawn because the sampler on the candidate (tools/perf/sample_threads.py, 12,670 samples) shows the owners cost the same as the
generated bodies: concat 0.25 % (body was 0.23), invert 0.32 % (0.29), invert-in-place 0.14 % (0.13), plus the dispatch cost of an
owner call (serve_native_owner / find_hle / try_hle_slow). The arithmetic is dominated by the same ppc::fps_* helpers in both; the
owner removes only the dead psq_st / psq_l register saves, which the dispatch cost eats. A leaf owner is a net loss when the body
is arithmetic-heavy, which matches native-math's finding for the vector leaves; 0x8040DF7C / 0x8040C188 (9 and 30 words) were therefore
not attempted. The decoder wins because its body is a 600-instruction integer loop that the owner turns into plain C++ integer code.

## 3. Overlap with agent/cri-adx-exec (commit f00a503, read before choosing)
That branch plans ADX **exec** ownership (adxt_ExecHndl 0x803C3F4C, vint 0x803B7810, a 399-function closure; steps 3b-3d: ADXSTM
getters, rna accessors, state 1/2 paths, then ExecHndl / vint). 0x803C5B2C is a leaf **below** it (called via 0x803C5AC8 from
0x803C4D50, not an edge of ExecHndl's listed direct callees), in the closure's decode family. This branch owns only that kernel;
the exec branch's closure map counts it among the 399 and would simply stop reaching its body once the owner exists. No file is
shared: native_adx_decode.* is new, native_cri.cpp and every native_adx_* of that branch are untouched. The sibling decoder
0x803C5D8C and the rest of the decode family (0x803A71B4 .. 0x803A8164, 0x803BBCD8, ...) were not hot in the benchmark and are left to it.

## 4. Gates
1. **Exactness (contract).** `adx_decode_contract_tests` (runtime/tests/adx_decode_contract_tests.cpp, ctest `adx_decode_contract_tests`):
   the original words of the pinned DOL (FNV 0xD2CFB4A1CA897F9B) run in the reference interpreter vs the native on identical
   machines. 3,000 fuzz calls (1,647 with overlapping buffers: source under an output, both outputs equal, a history under an output,
   the key inside a buffer; 580 stopped by a scale stop bit; 951 with n <= 1 / negative / INT_MIN / INT_MAX; 192 with saturation;
   extreme coefficients and histories; random keys) + **768 calls over real frames of 24 of the game's own .adx files** (two windows each,
   history carried; native PCM hash 7db13f4a5d665d2c). Compared after every call: r1, r3, r13-r31, LR, CTR, CR2-4, XER, f0-f31, FPSCR and
   every byte of the 0x2000-byte arena plus the stack arguments. Not compared: r0, r4-r12, cr0/1/5-7 (volatile) and the original's
   save frame below SP (stmw / lmw), which the owner does not write. Negative controls (ctest, WILL_FAIL): the original's prediction shift
   12 -> 11 (caught at fuzz iteration 0), key mask 0x7FFF -> 0xFFFF (caught at iteration 35), one nibble-table word changed (iteration 0).
   The matrix owners passed 15,000 calls (953 on the zero-determinant path, f14-f31 and the CR included) with three controls before withdrawal.
2. **Game behaviour** (benchmark replay, 316,000 ms, base.exe = gc-removal 17dd6e8 lifted watch tree, cand.exe = + owner):
   - game ticks 12,798,000,000, VI frames 9,477, render frames 18,920 identical.
   - **PCM (audio on, headless, `--audio-dump`), 2 runs per arm alternating: md5 prefix c64ee7883f980f73, 40,375,680 bytes, identical in all four.**
   - state.json memory digest: A/A identical (base md5 3a75268e85cd in every base run), B/B identical (0f534f094c3a); 96 of 97 64 KiB blocks of the
     image region identical; **block 91 differs in 14 words, 0x805B3220..0x805B325C**: word 0 = the back chain (0x805B3260) and 13 words
     = the stmw r19..r31 image, exactly the frame `stwu r1,-0x40` / `stmw r19,0xc(r1)` of 0x803C5B2C below its caller's SP 0x805B3260.
     Dead stack the owner does not write (never replicated: classified). A first candidate also differed in block 88: XER saved in the
     thread context at 0x805874DC (0x20000000 vs 0): that was a real difference (the original's srawi sets CA); the owner now sets it
     as the original and block 88 matches.
   - Guest watch: not run; the owner retires no state (it writes only what the original writes, plus no stack).
3. **Speed.** Benchmark `run.sh` (wt-gc-removal's samp-2026-11-11) under capped_run at 10 %, lock `benchmark`, alternating
   base/cand, 5 rounds, first discarded, wall ms of one 316 s game replay:

   | arm | kept runs (ms) | mean | min..max |
   |---|---|---|---|
   | base | 36419, 36891, 37133, 37147 | 36898 | 36419..37147 |
   | cand (decoder only) | 36973, 36694, 36790, 36696 | 36788 | 36694..36973 |

   -0.3 %, spreads overlap: **no measurable change**. An earlier candidate with the three matrix owners too: base 35953 / 35910 / 36597 /
   35836 (mean 36074), cand 35664 / 36026 / 36336 / 35335 (35840), also overlapping. Per-function (sampler on cand): serve_decode_ste4
   18 of 12,670 samples = 0.14 % against 0.48 % for fn_803C5B2C_body in the base profile (about 0.3 % of the game thread; below the noise).
4. **Tests run (Release, capped):** adx_decode_contract_tests and its three controls, math_affine_contract_tests, native_matrix_call_tests,
   native_file_api_tests, native_file_api_reference_tests (108 s), lift_sites_reference_tests, native_liveness_roots, native_liveness_roots_lifted:
   11 of 11 pass. First build under the cap of the watch tree: 1,778 s wall, no PDB C1090.

## 5. Shared-list lines changed (vs 17dd6e8)
- CMakeLists.txt: `runtime/src/gekko/native_adx_decode.cpp` after native_math_affine.cpp; a `foreach` block after math_affine_contract_tests
  adding adx_decode_contract_tests (+ three WILL_FAIL control tests).
- extract/native-owners.txt: `0x803C5B2C ADX stereo 4-bit block decoder` (with a comment line) after the math_affine block.
- runtime/src/gekko/system.cpp: `adx_decode::install(cpu_);` after `math_affine::install(cpu_);`; runtime/include/gekko/system.h: the include.
- runtime/tests/native_file_api_tests.cpp: `add_owner_addresses<adx_decode::Owners>(installed, total);`.
- extract/liveness-roots.txt `+0x803C5B2C +0x80516B18`; extract/liveness-roots-lifted.txt `+0x80516B18` (regenerated with the tests' arguments).
- Ratchet not touched. Body count: the owner prunes the body of 0x803C5B2C (one row, not re-measured with what_if).

## 6. Open problems / not done
- The decoder sibling 0x803C5D8C (word 0x805B8FB8 != 0) is unowned; it was not executed in the benchmark.
- ADX/CRI is only 1.34 % of the game thread; the rest of the family is cri-adx-exec's. libcsd (2.07 %, Chao::CSD DrawScene /
  CScene update) and the matrix library (2.13 %) are the larger untouched pieces; for the matrix library a faster owner needs host
  arithmetic instead of the exact fps_* helpers (FPSCR sticky-bit semantics decide whether that is allowed).
- Leaf owners of 10-30 word bodies are a net loss through dispatch cost (measured here and by native-math).
