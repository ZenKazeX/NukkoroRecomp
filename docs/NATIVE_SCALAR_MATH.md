# Native scalar square root

## Recovered contract

The original DOL SHA-256 is `fde4fa6f81a60313b710161c196dc51c2260be62251ee02775d5eee06f9d55af`.
Its 548-byte routine at `0x803B2D2C` has SHA-256
`fd6cf7f6738e84f9b270ff49d03306802527bf3e1414b720c1c24c0235d0b005`.
The implementation extracts a double's exponent and fraction, normalizes
subnormals, and computes the square root one bit at a time in two integer loops.
The final remainder determines round-to-nearest-even. Its only direct external
entry is the call at `0x803B2FA4` in the scalar wrapper `0x803B2F98`; no data word
points into the body. The wrapper itself has four direct callers, at
`0x803AF788`, `0x803AF814`, `0x803AFA30`, and `0x803B0648`.

The scalar API takes one double and returns one double. The observable extra
output is the caller's math error state: the original stores 33 at SDA+0x5E98
for a negative nonzero argument or any nonfinite argument, including positive
infinity. Other inputs leave the prior error untouched. Signed zero survives.
NaNs retain sign/payload and acquire the quiet bit. Negative infinity produces
the default positive quiet NaN. Negative finite values return the double form
of the float NaN at `0x805EED78` (`0x7FFFFFFF`), namely
`0x7FFFFFFFE0000000`. These are recovered behavior, not assumptions about the
standard C library's errno policy.

Temporary integer/FP registers and the callee's private stack writes are not
outputs of this scalar API. The wrapper restores its stack and link register;
the numerical result is f1's scalar lane. No game time passes inside the call.
The user explicitly authorized preserving necessary math and behavior without
preserving obsolete internal console state. Native host arithmetic requires
round-to-nearest and denormal inputs enabled, as the existing runtime does.
Console FPU-disable traps and intermediate register contents are not part of
this native API.

## Implementation and removal

`runtime/lift/math/scalar.{h,cpp}` implements `shadow::math::square_root(double)`.
It classifies exceptional inputs by bits and executes the x64 scalar hardware
square-root instruction for finite nonnegative inputs. It returns a value and
an error indication; the caller owns error storage. It has no CPU object,
address tokens, compatibility memory, allocation, or reference-code fallback.
The existing lift-purity gate checks this directory without exemptions.

The recompiler's `scalar_square_root` lowering, listed in
`extract/lift-sites-math.txt`, passes the double to this native API and publishes
the value/error to the remaining translated caller. This marshalling belongs
to the generator and disappears with that caller. It contains no square-root
algorithm. The old 137-instruction translated body is pruned rather than intercepted by
an optional handler. The executable still embeds the original 5,773,024-byte
DOL as bootstrap data, including the raw bytes of this routine. Removing that
remaining image dependency is separate work; linked x64-body removal must not
be described as removal of every original code byte. The post-link gate verifies that the old function and
its interior entries have no map symbol and that the native function is linked.

`tools/lift/verify_native_math.py` pins the complete original implementation,
checks external direct branch edges and data pointers, and verifies pruning
and linked absence. Computed-address reachability remains the generator's
conservative analysis; this is not a proof about arbitrary runtime pointer
values. The generator already checks the listed call word and refuses stale
or unemitted sites.

## Validation

The independent reference test executes the original DOL words in the
instruction interpreter and compares the native value/error contract: 40,480
cases, 18,162,056 original instructions, covering every exponent, both signs,
fraction boundaries, random bit patterns, zeros, subnormals, infinities, NaN
payloads, and both NI modes. All passed before call-site integration.

`native_scalar_call_tests` separately exercises the actual generated wrapper's
marshalling, error publication, return linkage, stack ownership, game time and
nonvolatile registers. Keeping this separate lets the original-word oracle
remain excluded from production liveness without concealing a generated-code
dependency. `test_output_structure.py` verifies native call emission, removal
of a replaced callee and its exclusive child, and refusal of a stale site.

Whole-build, linked-size, gameplay and timing results belong in
`OPTIMIZATION_RESULTS.md`. The numeric reference result alone does not prove a
gameplay speedup or full-game correctness.

The oracle also runs the enclosing original math functions at `0x803AF678`,
`0x803AF8B4` and `0x803B0444` twice: once with the original scalar routine and
once replacing only that routine with the native value/error API. Across
1,203 pairs, all enclosing results match. The four call sites were exercised
100, 100, 200 and 401 times. A memory-access trace marks the original routine's
private writes and rejects any caller read after return before overwrite;
none occurred. Writes outside the enclosing stack/error contract are rejected.
This is evidence for the recovered lifetime contract on those inputs, not
exhaustive path coverage or automatic acceptance of a gameplay-state mismatch.

The subsequent hardware-watch run identifies the final writers of the other
two Final Haunt scratch ranges (`0x8060BE40` and `0x8060BE80`): both are the
original input spill at `0x803B2D30`, stack pointer + 8. The store's x64 RVA is
`38701060`, immediately before the watch exception at `38701064`, inside the
pinned control's `fn_803B2D2C_body`. It stores the byte-swapped double loaded
from f1. The last values exactly match the control snapshot. The watched run's
entire snapshot has the same SHA-256 as the earlier uninstrumented control
(`648d1d1490e75c3414e476e24f929d66d3fe12b19fba0aec30fd2f78764ff573`).
All 28 changed bytes are thus attributed to removed scalar-private storage;
the byte comparison still reports the actual differences. The watch proves
these last writers, while the separate enclosing-call access tests support
the recovered no-read-after-return lifetime contract. Neither is exhaustive
whole-game path coverage. See `NATIVE_DIAGNOSTICS.md` for scope and provenance.
