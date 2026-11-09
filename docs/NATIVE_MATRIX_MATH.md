# Native affine matrix operations

## Recovered contract

The original routine at `0x8040CD04..0x8040CD24` copies twelve 32-bit
matrix elements, in increasing element order. Its complete 32-byte body has
SHA-256 `949be76368af4f56469cd79b3c4ca0f058309b6098a4bc4649b7d31b069a4faa`.
The input and output each cover 48 bytes. Self-copy is permitted; partially
overlapping spans retain the original forward element-copy behavior. In
particular, a destination inside a later part of the source can propagate an
earlier element through subsequent elements. An unconditional memmove would
change this behavior.

There are two direct call sites: `0x8040CB44` in matrix multiplication copies
the left operand to temporary storage, and `0x8040D004` in matrix inversion
copies a matrix in the singular-determinant branch. The evidence gate checks
all direct entries and rejects data pointers into the retired body. Recovering
and replacing the enclosing multiply/inverse algorithms is separate work.

All element representations survive, including signed zeros, subnormals,
infinities and NaN payloads. The original word oracle confirms that its
floating-point load/store sequence preserves these bits. Temporary registers
and the CTR loop counter are internal implementation state, not matrix outputs.
The call performs no allocation and advances no game time.

## Implementation and removal

`runtime/lift/math/matrix.{h,cpp}` provides
`shadow::math::copy_affine_matrix(void*, const void*)`. It copies a fixed
48-byte object representation with normal host pointers. Disjoint and
backward-overlapping spans use memmove; forward overlap uses twelve ordered
four-byte transfers. Unaligned host storage is supported. There are no guest
addresses, CPU objects, byte-order conversions, or reference-code fallback in
this API.

The generator validates complete source and destination spans, preserves memory
observation and journalling, and initially emitted the native call at both sites
in `extract/lift-sites-math.txt`. This boundary is temporary scaffolding until
the enclosing callers are ported. Invalid spans stop execution without a
partial write. The old routine is pruned from generation and linking. Its raw
PowerPC words remain in the original DOL bootstrap resource, as documented for
the scalar replacement.

`tools/lift/verify_native_math.py` pins both math bodies, direct edges, absent
data pointers, pruning, linked absence and native API presence. Generator
structure tests also reject stale call words and check removal of exclusive
callee dependencies.

## Evidence

The independent original-word oracle passed 45,056 aligned cases across eleven
overlap distances, with random and exceptional floating-point representations.
It checks the entire surrounding buffer, caller stack, return and game time.
Another 516 cases compare unaligned native spans with an independent ordered
byte-copy model. The actual generated-boundary test covers both sites,
overlapping spans, access observation, journal rollback, nonvolatile state and
invalid-span refusal. Build, suite and gameplay results are recorded in
`OPTIMIZATION_RESULTS.md`; an isolated oracle pass is not a gameplay speed claim.

The oracle also executes both enclosing original algorithms in 1,024 paired
calls, replacing only the copy in the second arm and leaving its former
temporary registers untouched. Each copy site is exercised 512 times, including
self-aliasing operands and the singular-inverse branch. Complete output buffers,
initialized stack storage and return values match; both calls restore the stack
and preserve game time. This checks the recovered caller contract on these
inputs rather than assuming every volatile register is irrelevant.

## Affine composition

The composition replacement covers `0x8040CB14..0x8040CD04` (496 bytes, SHA-256
`c18e10dde4c1a98a61a1da12e39ae0747581bd77e3c3d8cc1902b5de355d837e`).
It composes a row-major 3x4 affine matrix in place: r3 points to the left/output
matrix and r4 to the right matrix. The left matrix is first copied into its
96-byte stack frame. For each output row i and column j, the recovered order is
`t = round_float(left[i,1] * right[1,j])`, then
`t = fused_float(left[i,0] * right[0,j] + t)`, then
`t = fused_float(left[i,2] * right[2,j] + t)`. Translation column j=3 additionally
adds `left[i,3]` with a separate single-precision rounding. Changing this order
or replacing fused operations with separate multiply/add is not justified.
The operation has no allocation, callback, clock advance or object cleanup.
Its output is the twelve left-hand matrix elements; its scratch registers and
96-byte private frame are not part of that output. The old r3 return happens to
point into the discarded private frame, rather than denoting a matrix result.

The original loads all nine right linear elements before the first output
store, but loads the three right translation elements after that store. Full
self-aliasing is safe; some partial overlaps can overwrite a translation value
before its load. A value-copy implementation must not silently assume all
partial overlaps have the same behavior. `compose_affine_matrix` preserves this
load/store ordering for overlapping float spans. Its normal finite path uses
packed multiplication and fused additions with separate translation addition.
Exceptional values use explicit NaN selection, signed-zero and subnormal rules.
Host rounding is nearest with DAZ/FTZ disabled; the explicit Flush policy zeros
subnormal operands and exact tiny results before rounding. A two-sum residual
resolves the exceptional case immediately below the normal boundary.

There are 68 direct external call sites and one data reference at `0x800076F4`:
the words are `(0x8040CB14, 0x1F0, 0x800057D4)`, a function/length/descriptor
record consistent with exception metadata. Its actual consumer/retirement
contract must be proved before modifying liveness; it is not an arbitrary
unused pointer to erase. The pinned entry list and data words are in
`.build/optimization/matrix-compose-contract.json`.

The native value implementation is now integrated into all 68 generated direct
call sites. Its independent oracle passes 29,700 original-word comparisons
(5,227,200 instructions), including both subnormal policies, exceptional float
representations, self/partial overlap, and exact results immediately below the
normal boundary. It uses packed x64 FMA for ordinary finite values and explicit
scalar edge handling. The release is 24,064 bytes smaller than the preceding copy-only build;
`OPTIMIZATION_RESULTS.md` records the current gameplay checks and limitations. Another 256 paired
executions of four enclosing transform routines produce identical matrices and
never read removed private stack storage before overwrite. This is bounded
caller evidence, not exhaustive execution of every gameplay path.

The metadata consumer has now been located in the original DOL: initialization
at `0x803A2520` registers `0x80008D14` through `0x803A3954` in the fragment record
at `0x805A61F0`. Lookup at `0x803A371C..0x803A3920` reads that registration,
checks the code interval, and binary-searches twelve-byte index records. It uses
the first word as a range comparison and the third as a descriptor address,
not as a call target. The descriptor for composition contains no cleanup body.
The independent oracle executes original registration and lookup for the first,
interior and final instructions, obtaining the exact descriptor with no cleanup
selected. The evidence gate pins those three original consumer bodies, the
registration record, sorted index and complete descriptor. It exempts only the
single verified range key from executable liveness, rejecting any additional
pointer. The original data bytes remain unchanged.

The common generated boundary validates two complete aligned 48-byte float
spans, reconstructs overlap using resolved host storage, converts object
representations and calls the pure native value API. Writes retain access
observation and journal rollback. Alignment is a typed-float input requirement;
invalid or incomplete spans stop without partial writes. The boundary contains
no matrix arithmetic and is marked for removal when the remaining callers own
native matrices. The former internal copy site disappears with its enclosing
routine; the inverse's copy site remains. The structural gate rejects every
translated entry inside the retired composition range, including interior
entries. The embedded original DOL still contains the reference instruction
bytes; removing translated production code does not claim otherwise.


A later gameplay caller does read an old matrix spill after reusing its stack
slot as an unwritten collision output. The no-hit path at 0x800979E8 computes a
temporary direction before checking the negative query result. Two bounded
hardware-watch runs identify the old and new stale words and reproduce each
uninstrumented snapshot exactly. The original-instruction regression executes
the suffix through 0x80097BA8 for both captured stale vectors over 32 bounded
radius/motion cases: the same discard branch is reached without writes outside
the local frame, while the real exception-status difference remains. This
supports discarding obsolete spill stores on this observed path; it is not a
proof for every possible collision input or all later consumers. The four
transform-only caller tests above have a narrower no-read lifetime contract.


All 289 remaining matrix-related stack bytes now have final-writer attribution
from 21 sequential hardware-watch batches (81 aligned words). Every selected run
produces a fresh snapshot exactly equal to the uninstrumented control. Thirty
words are matrix prologue stores, 38 are the temporary matrix copy, and 13 are
paired floating-point saves in five original frames. Helper stores are identified
by exact linked writer, unique guest return link and matching store footprint;
the parent routine is strongly supported without a captured host call stack.
The report in `matrix-stack-watch/attribution.json` pins every summary and word.
This attributes final stored values; the separate bounded caller-read and
collision-discard tests are still required, and strict snapshot equality remains
false. No byte exclusions or fake legacy register/stack writes were introduced.
