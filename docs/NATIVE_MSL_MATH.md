# The game's MSL math library, native (2026-10-29)

Item 1 of NATIVE_MATH_METHOD.md section 4. The code is `runtime/lift/math/msl.{h,cpp}` (`shadow::math::msl`); the
oracle is `runtime/tests/native_msl_tests.cpp`.

## Recovered contract

The game links Metrowerks MSL, whose double routines are fdlibm compiled for the Gekko. The compiler fused every
multiply-add it could (fmadd / fmsub / fnmsub, one rounding). The float wrappers are the double routine followed by
`frsp`, and the 2-instruction pass-throughs add nothing.

| routine | original | notes |
|---|---|---|
| sin / cos | 0x803B2AFC / 0x803B2594 | kernels 0x803B1F5C / 0x803B1014 |
| x mod pi/2 | `__ieee754_rem_pio2` 0x803B0C74 | large arguments: `__kernel_rem_pio2` 0x803B1108, prec 2 from its one call site, two_over_pi 0x80515398, PIo2 0x80515530 |
| scalbn / floor | 0x803B283C / 0x803B2668 | scalbn is MSL's: NaN, infinity and zero return x itself, not x + x |
| atan / atan2 | 0x803B2210 / 0x803AFAEC | atan2's wrapper 0x803B2C8C |
| acos | 0x803AF678 | wrapper 0x803B2C4C. The domain error returns `__float_nan` (0x805EED78, 0x7FFFFFFF) as a double and writes no errno |
| sinf / cosf / atan2f / acosf | 0x8000FFE8 / 0x8000F3B0 / 0x80016260 / 0x801B2150 | the double routine, then frsp |
| pow | `__ieee754_pow` 0x803B0444 (wrapper 0x803B2D0C) | added 2026-11-01 for RenderWare's RwImageSetGamma (rwcore/src/image.cpp). MSL's variant: x < 0 with a non-integer y returns `__float_nan` as a double and writes errno = EDOM (33, at 0x805F2398; the port has no errno); \|y\| > 2^31 with \|x\| within an ulp of 1 starts from x - 1 where fdlibm has \|x\| - 1; the overflow / underflow results of \|y\| > 2^31 are the stored infinity / zero. y = 0.5 is the software sqrt 0x803B2D2C, exact like std::sqrt. NI reaches it only through a denormal operand of a multiply (x scaled by 2^53, a denormal y's products) |

NaN rules are the Gekko's, not the host's:
- the first NaN operand is quieted with its sign and payload;
- an invalid operation gives 0x7FF8000000000000 (x64 gives 0xFFF8000000000000);
- so `sin(inf)` is the positive default NaN.

The old pose-math transcription returned x64's negative NaN there. It also threw "FAIL_CLOSED" for |x| > 2^19 pi/2.

**FPSCR[NI].** OSInit 0x80370E68 calls PPCSetFpNonIEEEMode 0x80370BE8 (`mtfsb1 29`) unconditionally, so the game
runs in non-IEEE mode: a denormal operand reads as zero, and a result whose exact value is below the normal range
becomes zero. The oracle shows how far the mode reaches:
- sin, cos, atan, acos and the reduction give the same bits in both modes;
- atan2's y / x depends on it (a tiny quotient flushes to zero, which atan then keeps);
- so does scalbn on denormal values (the library's own calls never pass one);
- so does the frsp of the float wrappers.

These functions take a `SubnormalMode`, as the rest of `lift/math` does.

**FPSCR status bits are not part of the contract.** Census of main.dol's text: the FPSCR readers are `mffs` at
0x80370B68 (PPCMffs), 0x80372ADC (OSSaveFPUContext) and 0x8039DD58 (MetroTRK's register access), and there are no
`mcrfs` and no record-form FP instructions (Rc = 1). The PPCMffs callers are OSInit 0x80373378 and the exception
path 0x80373590, which mask the enables and write the value back. No game code reads FPRF, FX, FI, FR or the sticky
exception bits. The library returns values only.

## Implementation

`msl.cpp` is the original algorithm in the compiled operation order. It has:
- fused operations through `_mm_fmadd_sd` (FMA builds) or `std::fma`;
- NaN-exact helpers only at the sites a NaN or infinity reaches;
- `math::detail::divide` (`floating_point.h`) for atan2's mode-dependent quotient: operand flush, and tininess on the
  exact quotient (on the 2^-1022 boundary, |a| 2^1022 < |b| decides).

It has no CPU object, guest addresses, CRT transcendental, allocation or fallback. `lift_purity` passes. The tables
are compiled in, and the test compares them with main.dol word for word.

## Evidence (2026-10-29, release `tests` tree)

- `native_msl_tests` (CTest label `native-contract`): 1,802,920 calls of the original words in the interpreter,
  228,985,509 instructions, **0 differences**. It runs:
  - both NI modes;
  - every routine above, including rem_pio2's stored y0 / y1 and n, and the kernels on their own;
  - inputs: every branch threshold ±1, zeros, denormals, infinities, quiet and signalling NaNs with payloads,
    multiples of pi/2 ±3 ulps up to 3000, every exponent from -1074 to 1023, random bit patterns and large reductions
    up to 2^1059;
  - pairs for atan2, including quotients that are tiny relative to x.

  Each call also checks that the original wrote only its own frames, the caller's LR save word and its output, and
  that it restored the stack, game time and FPSCR's control byte. The run found two defects of the first draft
  before it passed, both in scalbn under NI: a denormal operand, and the exact tininess of the final scaling.
- pow (2026-11-01): the same test now also runs pow and its wrapper on pairs in both NI modes - every special
  against every special, the image gamma tables' operands, integer powers of negative bases, the special bases to
  odd / even / fractional powers, \|y\| > 2^31 near 1, the overflow and underflow edges, bases near 1, four pairs
  found by search where the log step's fused multiply-add decides the last bit, random bit patterns: 2,577,288
  original calls in all (397 million instructions), 0 differences. Controls (`.build/dolrecon/pow_mutations.json`):
  fdlibm's \|x\| - 1, the log step unfused, the odd-power sign dropped - each fails (the last two only after the
  special-base pairs and the searched pairs were added).
- Pose A/B (`tools/motion/motion_pose_check.cpp`): 105 clips × 150 frames of BKSOLDIER, BKWINGLARGE, BKLARVA,
  BKNINJA, GUNSOLDIER and BKGIANT, old build (`.build/diag`, the transcribed sin/cos/acos) against the new one. Every
  bone matrix is byte-identical.

## The cut: the game's own calls (2026-10-29, built and gated)

- `tan`, `__kernel_tan`, `tanf` and `atanf` were added to the library, with the same oracle: 2,151,480 original
  calls, 0 differences, both NI modes.
- Every direct call to the MSL entries from code that survives is a lowered site in `extract/lift-sites-math.txt`:
  265 sites with 13 `msl_*` kinds (tools/ppcrecomp, `LiftKind::Msl*`). Eight earlier rows sat in code the generator
  no longer emits (fn_8000F654, and the natively owned PosPan around 0x801D96A0) and were removed; the generator
  refuses a site it cannot place.
  - The kinds are sin, cos, tan, atan, acos, atan2, their float wrappers (frsp in the caller's FPSCR[NI] mode),
    and sincos 0x80031568, which stores frsp(sin) to `*r3` and frsp(cos) to `*r4` and leaves f1 = cos.
  - By count: 81 atan2f, 76 cosf, 57 sinf, 14 atanf, 14 tanf, 12 sincos, 6 acosf, 7 cos, 4 sin, 1 atan2, 1 acos.
- The 21 bodies (wrappers, the double routines, `__ieee754_rem_pio2`, `__kernel_rem_pio2`, the sin / cos / tan
  kernels) are pinned in `tools/lift/verify_native_math.py`: body hash, incoming edges, no data pointers. They
  must leave the generated code and the link.
- `scalbn`, `floor` and `copysign` keep other callers (other MSL routines, game code) and stay.
- Measured share on Westopolis: the translated trig was ~21% of the bone-hierarchy walk 804196F0 (6.1% of the game
  thread).
- Gate result (diag build 13:00): all 21 contracts pass, so the bodies are pruned and absent from the map. The first
  attempt failed on `sin`: test fixtures used 0x80010000-0x80031000 as scratch addresses, the liveness census rooted
  every 0x8xxxxxxx literal in runtime sources, and one landed in sinf. The fixtures moved to free MEM1 (0x80F10000..,
  0x80600000, 0x8060F000) and `extract/liveness-roots.txt` followed.

### Same-tree A/B: is the gameplay the same?

Two executables from one tree, differing only in the msl rows of the site list (on: 265 sites; off: none, the
translated bodies run). Westopolis, `--boot-stage stg0100` plus the recorded tape, headless.

- Each arm is deterministic: repeated runs give 0 differing state blocks.
- The game-memory journal (every complete memcpy and PRS expansion with the hash of what it wrote): 109,356 lines
  in both arms, 0 differing lines.
- The frame at 40 s (frame 2359, tick 1,620,000,000): byte-identical PNG (sha256 1387078...C19A0).
- Savestates at 2, 10, 20, 30 and 40 s, compared byte by byte with `tools/state_delta.py` (nothing excluded): 83,
  298, 30, 162 and 182 bytes differ, and the count does not grow. Every differing byte falls into one of three
  classes, with no byte left unexplained:
  - **Dead stack.** Inside a guest thread's stack, below that thread's r1 (the architected r1 for the current
    thread). These are frames the translated MSL routines wrote and the native ones do not.
  - **Saved volatile FPRs.** The current thread's OSContext fpr / psf slots for f5-f10, and the same registers in
    the architected state. They are caller-clobbered: after a call, compiled code writes them before it reads them.
  - **The 4th word of a 16-byte row,** whose other 12 bytes are identical in both arms. These are RwMatrix pad slots
    (for example two orthonormal matrices at 0x809F2F50 and 0x809F2F90, flags 3, pad2 at +0x2C). A struct copy from
    a stack temporary carries stack residue into the pad, so the residue differs with the stack contents.
- What this proves: on this replay, none of these differences reaches any other byte of game state, any copied or
  decompressed data, or the picture. It is not a proof for every stage; a reader of an RwMatrix pad slot elsewhere
  would show up as a propagating difference in the same comparison.
- Cost: 6 alternating rounds of 40 s of game time, timed by `guest_run_wall_seconds` (the game thread's run
  time). On: median 3.937 s, range 3.918-3.939 s. Off: median 4.094 s, range 4.033-4.179 s. That is -3.8% of the
  whole game thread, and the two ranges do not overlap.
- The comparison scripts are in `.build/validation/msl-bench/`: `classify.py`, `threads.py`, `dump.py` and
  `time_ab.ps1`.

## Users and what remains

- `runtime/lift/motion/pose_math.cpp`: its sin, cos and acos now call the library. About 190 lines of duplicated
  transcription are gone, and so is its fail-closed large-argument path.
- `gekko/native_se_math.cpp` (sound positional math) and `gekko/chara_colli_bone.cpp` call the library. The user
  approved the rule change on 2026-10-29, so their comparisons check FPSCR's control byte (`same_fpscr`), not its
  status bits; the census above shows nothing reads them. The second copy and `msl_trig_ported` are deleted, and so
  is CharaColli's decline for angles above 2^19 pi/2.
- The pose-math float wrappers round with IEEE `float()`, the behaviour before this change. Under NI that differs
  only for a denormal float angle (frsp then flushes). Pose math has no mode input today; it is open with the motion
  code's own mode question.
- The game's sqrt 0x8000FF04 (frsqrte + Newton) stays where it is. Its frsqrte is the runtime's exact 1/sqrt model
  (ppc_ops.h), a separate question.
