# Replacing console systems with exact x64 math: the short method (2026-10-29)

Asked for by the user 2026-10-29: "find the way to replace systems with identical behavior lightweight x64 PC math.
No cheating, no fake results. Cut the obsolete systems out and replace them with brand-new ones written from
scratch." This page is the method. It is drawn from the replacements that already passed it: square root
(NATIVE_SCALAR_MATH.md), the matrix copy and multiply (NATIVE_MATRIX_MATH.md) and the CharaColli code
(CPU_FINAL_HAUNT.md). Above it stands NORTH_STAR_COMPACT.md.

## 1. What "identical" means

The same output bits for every input class, not values that are close:
- signed zeros, subnormals, infinities and NaN payloads;
- the same side effects: error words, the bytes the original writes, aliasing behaviour;
- no game time passing inside the call.

What is not kept: the console's working state. That means temporary registers, stack scratch, FPSCR bits nobody reads,
CTR, and paired-single lanes as storage. A tolerance, a field left out of a comparison, or a rerun until a gate
passes is a fake result.

## 2. Why plain x64 math can be bit-identical

The game's math is IEEE-754 arithmetic in a fixed order. x64 SSE2 and FMA3, and ARM64, round the same way. So the
algorithm, written in C++ with the original's operation order, gives the original's bits. The mapping:

| Gekko | x64 / C++ | trap |
|---|---|---|
| `fadd` / `fmul` / `fdiv` (double) | the same double operation | none |
| `fadds` / `fmuls` / `fdivs` | compute in double, round once to float | correct: a double holds the exact product or sum of two floats |
| `fmadd(s)` / `fmsub(s)` / `fnmsub(s)` | `std::fma` / `_mm_fmadd_*` (one rounding) | never write `a*b+c`: separate multiply and add round twice |
| `frsp` | `float(x)` | |
| `fctiwz` | truncate and saturate; NaN gives INT_MIN | a C cast is undefined out of range |
| `frsqrte` / `fres` | the Gekko's exact estimate, from a table or bit rule | a "close" `1/sqrt` is wrong; prove it on every input class |
| denormal (non-IEEE) mode | `SubnormalMode::Flush` in `lift/math/floating_point.h` | the zero must keep its sign |
| `psq_l` / `psq_st` | integer conversion times `2^-scale` from the GQR, saturating | the scale and type come from the GQR at the call |

Compiler rules: `/fp:precise` (or `/fp:strict` for contract tests), never `/fp:fast`, and no implicit contraction.
Do not call the CRT's `std::sin`, `std::atan2` or `std::pow`. They are not fdlibm and differ in the last bit. The
game's own MSL/fdlibm algorithm, written in C++, is the lightweight replacement. Use x87 nowhere.

## 3. The loop, one routine or system at a time

1. **Pin.**
   - Hash the original body.
   - List every direct caller.
   - Prove that no data word points into the body.
   - The evidence gate lives in `tools/lift/verify_native_math.py`.
2. **Recover the contract.** Inputs, outputs, side stores, aliasing, error states and preconditions, written into a
   `docs/NATIVE_*.md` page with addresses. Name the routine first (RW37SDK, Heroes PC cross-reference, RTTI).
3. **Write it from scratch** in `runtime/lift/<area>/`.
   - Use normal host pointers and values.
   - It must not contain a CPU object, guest addresses, GuestPtr, a fallback to the old code, or a call back into
     translated code.
   - `lift_purity` checks this.
4. **Oracle.**
   - Run the original PowerPC words and the new function side by side.
   - Use random inputs plus every branch threshold (the high words), the exceptional values and the overlap
     distances.
   - Compare whole buffers, stack, return and game time.
   - Then run the paired-caller test: the enclosing original algorithm with only this call swapped.
5. **Gameplay gate.**
   - Replay the Westopolis tape (`--boot-stage stg0100`) and Final Haunt.
   - Use a `--verify-*` mode that compares every call against the guest: 0 differences on millions of calls.
   - The same frames must come out.
   - Byte-identical self-determinism.
6. **Cut.**
   - The generator emits the native call at every site.
   - The old body is pruned from generation and linking.
   - A post-link gate proves its symbol is gone and the native one is present.
   - Delete the A/B switch once the gate has passed on all tapes.
7. **Measure** with `guest_run_wall_seconds`, arms alternated, never the process wall time. Record "no measurable
   gain" honestly.
8. **Document and hand off.** Update the doc page, `OPTIMIZATION_RESULTS.md` and `NEXT_SESSION.md`.

A path the new code does not cover yet fails closed with a named error. It never falls back to the old code, and it
counts as unfinished work, not as done.

## 4. What is still the old system, in order (from the 2026-10-28 Final Haunt profile)

1. **One MSL math library.**
   - sin and cos are written twice today:
     - `runtime/lift/motion/pose_math.cpp`;
     - `runtime/include/gekko/native_se_math.h`, which uses the CPU's FP model and FPSCR, a console structure.
   - Neither copy ports `__kernel_rem_pio2` (both fail closed above 2^19 pi/2).
   - Move them into one `runtime/lift/math/msl.{h,cpp}` with plain double/float values:
     - sin, cos, atan and atan2 (0x803B2AFC, 0x803B1F5C, 0x803B1014, 0x803B0C74, 0x803B2210, 0x803AFAEC);
     - the full `__kernel_rem_pio2`;
     - the remaining fdlibm entries the game calls, found by caller census.
   - The two users then call the library instead of their copies.
   - **Status 2026-10-29:** the library is written and proven against the original words (0 differences in 1.8
     million calls). Pose math uses it. The sound and CharaColli copy waits on the user's FPSCR-status decision. See
     NATIVE_MSL_MATH.md.
2. **The hierarchy walk fn_804196F0 / fn_8041EC24 (6.7 % of the game thread).** It runs sin, cos and sqrt per node,
   probably the animation hierarchy. Name it, then port it on top of item 1.
3. **psq_l / psq_st (36 site groups, 1.9 %).** A generator lowering into the quantization rule of section 2, with
   the GQR value proven per site.
4. **The RenderWare vector and matrix family** around the native RwMatrixMultiply: transform points and vectors,
   normalize, invert, quaternion to matrix.
5. **The CharaColli manager's pair loop fn_800C7068 (9.9 %)**: weak_ptr locks, contact lists, the distance test.
6. **PJS particles (6.2 %)** and **the shared enemy update fn_801A6F08 (5.0 %)**: name them first.

Not math, but the same method applies, and other sessions own them: ARAM (`docs/ARAM.md`), draw distance
(`docs/DRAW_DISTANCE.md`).

## 5. Done means

The four questions of NORTH_STAR_COMPACT.md all answered yes, every gate in section 3 passed and recorded, and the
old body gone from the link. Nothing is called native while a fallback to translated code remains.
