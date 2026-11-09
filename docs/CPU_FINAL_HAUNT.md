# The game thread on Final Haunt (2026-10-27)

The user saw the FPS counter dip to ~58 on Final Haunt and asked for the CPU's largest consumer to be found,
disassembled, reverse-engineered and replaced by native x64 code with identical behaviour.

## What the dips are

The user's own session (383 s, fullscreen 1080p, vsync) reported presentation p50 16.7 / p95 17.1 / p99 17.4 ms, 10
dropped packets, and the game thread behind its 60 Hz boundary at only 2 of 22,980 boundaries. But a third of its
frames were long: on those the guest itself computed 13.7-16.8 ms of a 16.67 ms frame ([frame-tail] in the run
report). So the game's own code nearly fills the frame on Final Haunt's heavy frames, and the counter's half-second
window shows the one that overshoots. Not a GPU limit.

## Where the guest time goes

tools/native_sample.py --stacks on the Final Haunt tape (`.build/recordings/final-haunt`, headless: the game thread
runs flat out, so its wall-time samples are its work), release build, 3 runs x 120 s of game time, 11,320 samples;
tools/stack_report.py charges them to the game's RTTI classes. Scripts: `.build/validation/cpu-fh/`.

| by owner (the innermost class method on the stack) | share |
|---|---|
| (no class method on the stack) | 40.0 % |
| Player::Behavior::Ground::vf5 (ground movement: Shadow and the Sonic NPC) | 10.7 % |
| PJS particles | 6.2 % |
| character collision (CharaColliGeneralBone::vf34, CharaColliGeneral, CharaColliBody) | ~12 % |
| Player::Shadow::ShadowMotion::vf1 | 3.6 % |
| MessageTask::vf1 | 3.4 % |

Inclusive, the ground behaviour's chain fn_80097658 (22 %) -> fn_80097DB4 -> fn_80021E24 -> fn_80083A38 ->
fn_8008331C (15 %) -> fn_800C5D20 (14 %) carries most of it. Self time (innermost guest frame):

| function | self | what |
|---|---|---|
| **0x803A1AFC** | **4.9 %** (4.3 % from fn_80099454, the ground behaviour) | Metrowerks `__dynamic_cast` |
| 0x8047FC08 | 3.8 % | RwMatrixMultiply (paired-single 4x4); 2.2 % once __dynamic_cast was native (below) |
| 0x80486340 | 2.3 % | a RenderWare standard-function wrapper (RwGlobals+0x70) |
| 0x80385950 | 2.1 % | SDK |

## 0x803A1AFC: __dynamic_cast, served natively (landed, default)

Reverse-engineered (612 bytes; the full contract is in runtime/include/gekko/dynamic_cast_intrinsic.h): the object's
vtable gives its RTTI record and the offset to the complete object; the target is found by comparing class-name
strings byte by byte against the record and each base entry (plain entries, and repeated bases resolved through the
source type's path); a failed reference cast throws bad_cast. 268 call sites, all with constant type arguments.

The answer depends only on constant data (the vtable's two words and the type_info records, all in the DOL), so
runtime/src/gekko/dynamic_cast_intrinsic.cpp does the same walk natively once per (vtable, vptr offset, target,
source) and keeps it, re-walking if the vtable's words ever read differently. A served call leaves what the guest body
leaves on its non-throwing paths: r3, r0 = the return address, r31 and r1 unchanged, and its prologue's three stack
words. A failing reference cast declines to the guest body, which throws. Hook: System (0x803A1AFC, "Metrowerks
__dynamic_cast"); A/B: `--no-dynamic-cast-intrinsic`.

Measured on the Final Haunt tape, 120 s of game time, headless, release build, arms alternated:

| | guest body | native |
|---|---|---|
| wall time (game thread flat out) | 23.5 / 23.5 / 23.5 s | 22.5 / 22.5 s (-4.3 %; a cold first run 26.1 s) |
| casts | 2,359,915 | the same, 135 distinct (walked once each), 0 declined |
| spawn tests / native dispatches | 8,597,281 / 4,795,409 | identical |
| 120 captured frames | - | 119 identical, 1 pixel 1 step (the known NVIDIA noise) |

## 0x8047FC08: RwMatrixMultiply, served natively (landed, default; no measurable speed-up)

The guest body (254 bytes, paired single; disassembly and contract in runtime/include/gekko/matrix_multiply_intrinsic.h)
is out = a x b over rows right/up/at/pos, every product a ps_muls0/1 and every sum a ps_madds0/1, out.flags =
a.flags & b.flags, the padding words untouched. Its second pass computes the z column in ps0 and throwaway sums
(b.*.z, 1.0) in ps1, which stay in f8-f13.

runtime/src/gekko/matrix_multiply_intrinsic.cpp serves it two ways:
- **four lanes at a time** (AVX2): the 48 half-operations as 12 fused multiply-adds on doubles, then rounded to single,
  which is what each ppc_ops helper computes on its fast path. It is taken only after proving per lane that no
  operation could leave that path (tiny operand or result, NaN, fused double on a single-precision midpoint). The
  fast path changes no FPSCR bit, so the order of the operations is then not observable. Inputs are read before
  anything is written, and an output overlapping an input or the stack frame goes to the ordered body.
- **the ordered body**: the guest instruction sequence with the register file in host registers, the same helper
  per half in the same order, and loads and stores in the same order. It is exact for every input, FPSCR included.
  It is also the path on hosts without AVX2.

Either way a call leaves what the guest body leaves: the output, the stack frame's words, r0, r4, f0-f13, and f30/f31
restored through their paired-single save. A/B: `--no-matrix-multiply-intrinsic`. `--verify-matrix-multiply` runs
every served call also through the translated guest body from the same state and compares all registers and the
bytes of the output, the inputs and the stack frame.

Measured on the Final Haunt tape, 120 s of game time, headless, release build:

| | guest body | native |
|---|---|---|
| calls checked against the guest body (`--verify-matrix-multiply`) | - | 4,507,612, **0 differed** |
| four lanes at a time / ordered | - | 3,418,745 / 1,088,867 (all off the fast path, none overlapping) |
| spawn tests / native dispatches | 8,597,281 / 4,795,409 | identical |
| wall time, arms alternated | 25.1* / 22.49 / 22.52 s | 22.52 / 22.55 / 22.50 s |
| game-thread samples in the function | ~82 of 3,685 (69 body + its share of psq_l/psq_st) | ~69 of 3,735 |
| 120 captured frames | - | 118 identical, 3 pixels 1 step (the known NVIDIA noise) |

(*a cold first run.) The function was 2.2 % of the game thread once `__dynamic_cast` had gone native, not the 3.8 %
of the first profile. The native version takes ~15 % less of it, about 0.3 % of the game thread, which is below the
wall-time noise. Why so little:
- The translated body was already close to native code: flat memory, inline helpers, hardware FMA (/arch:AVX2).
- Its cost is moving the three matrices (24 loads, 21 stores per call), not the arithmetic.
- A quarter of the calls must stay ordered. The throwaway ps1 sums such as `pos.x + 1.0` often land exactly on a
  single-precision tie, where the guest's helper takes its slow path and updates FPSCR.

## The ground behaviour's chain: the CharaColli broad phase, served natively where it rejects (landed, default)

With both fixes above in, the chain is 13.4 % of the game thread (fn_8008331C -> fn_80097658). Mapped by RTTI and
call counts (a temporary counting build, 120 s of the tape):
- fn_80097658 (Player::Behavior::Ground) casts rays (fn_80097DB4 -> fn_80021E24 -> **0x800C5D20**, 57,714 queries)
  and sphere sweeps (fn_80098ABC -> fn_80021934 -> **0x800C51C8**, 31,187 queries). Each first tests the static
  world meshes (fn_80021678 / fn_80021480), then **every** character-collision object in the world: the
  manager's groups hold lists of boost::weak_ptr; each is locked and its test virtual called (vf 0x60 ray, vf 0x6C
  sweep). About 103 objects per ray and 79 per sweep: **8.4 million tests, of which 3,242 hit**.
- Each test (CharaColliBody 0x800C15A0 / 0x800C1108, CharaColliGeneral 0x800C31EC / 0x800C2854) first re-syncs the
  object from its owner (vf 0x88: two weak_ptr locks, compare-and-copy of position, rotation and a word, a dirty
  byte; 20.8 million weak_ptr copies in all), then rejects it at the bounding sphere: the squared distance from
  the centre to the ray / sweep segment (0x8003C688, through RenderWare's RwV3d helpers) against radius^2.
- A Body ray first calls the ground behaviour's filter fn_80099454 (a mask test, an exclusion list, a
  `__dynamic_cast` and a team check on the cast object's owner).
- Split of the 13.4 %: the two query loops 3.8 %, the per-test updates 3.9 %, the tests 2.9 %, the world meshes 1.6 %.

runtime/src/gekko/chara_colli_intrinsic.cpp serves the four test virtuals (contract in
runtime/include/gekko/chara_colli_intrinsic.h): the update, the filter and the bounding test with the guest's
ppc_ops helper for every floating-point operation in the guest's order and the guest's single conversion at every
stored float, so FPSCR and every byte written are the guest's. Every reference count the guest's locks touch is
incremented and released again in the same call, so a served test writes none (it declines when a count could
wrap or reach zero). A rejected test returns r3 = 0 natively; a test inside the sphere (the narrow phase), another
class's vtable (GeneralBone re-derives its bones), another filter, MSR[FP] clear or a quantising GQR0 declines
with nothing changed. A served test leaves the ABI-visible state the guest body leaves: r3, the object's writes,
FPSCR, f31 through its paired-single save, the LR save word at r1 + 4, every other non-volatile register and every
byte at or above r1. Volatile registers and the dead stack below r1 are not reproduced: the tests have no direct
call site in the DOL (vtables only), and both query loops read only r3 after the call.
`--no-chara-colli-intrinsic`: A/B. `--verify-chara-colli`: every test also runs through the translated guest body
(its published transfers driven until it returns) and is compared on that contract, every 1024th on every committed
byte of guest memory outside the stack below r1; a declined test must change nothing.

Measured on the Final Haunt tape, 120 s of game time, headless, release build:

| | guest bodies | native |
|---|---|---|
| tests checked against the guest body (`--verify-chara-colli`) | - | 8,430,465, **0 differed** (5,417 on all memory) |
| served natively / declined | - | 5,606,757 / 2,823,708 (79,432 inside the sphere, 681,986 another filter, 2,062,290 another vtable) |
| ground filters computed natively | - | 2,466,386 |
| spawn tests / native dispatches / frames | 8,597,281 / 4,795,409 / 7,160 | identical |
| wall time, arms alternated | 22.51 / 22.53 / 22.54 s | 21.56 / 21.53 / 21.54 s (**-4.4 %**) |
| the chain's share of game-thread samples | 13.4 % | 10.8 % |
| 120 captured frames (from frame 1200, every 40th) | - | **120 identical** |

### The plain classes' updates

Of the 2.06 million tests declined for their vtable, 1.26 million were the plain CharaColliBody / CharaColliGeneral
(vtables 0x80523868 / 0x805239E8), whose update vf 0x88 (0x800C0C9C / 0x800C1D20) is a bare `blr`; they are served
now. The rest are CharaColliGeneralBone (below).

## The query loops 0x800C5D20 (ray) and 0x800C51C8 (sweep), served natively (landed, default)

Both loops are transcribed onto the CPU's own register file and stack frame (chara_colli_intrinsic.h has the
contract): the prologue's frame (back chain, stmw area, the caller's LR save word), every loop register (r20-r31 /
r14-r31) and every frame word the guest would hold, the group loop and its group-by-mask bit matrix, the list walk,
the weak_ptr copies and lock, the object's enable byte (+0x2A), the sweep's exclusion list, the result
initialisers. A weak_ptr copy that is released with no guest code in between changes no count, so none is written;
the copy held across the test is counted and released as the guest does. The four native tests are called directly.
Any other test virtual (a declined test, vf 0x68, GeneralBone) is a nested guest call with the guest's own return
address in LR. A query is handed back to the guest body, the machine left at the guest instruction it stands at with
every register and frame word as the guest's would be, when a test hits (the guest builds the result objects), an
entry has expired (the guest erases it), a release would reach zero, a count could wrap, or a nested call is cut
short by an interrupt (the guest resumes inside the callee and returns into its own loop). `--no-chara-colli-loops`:
A/B (the tests stay native). Under `--verify-chara-colli` every query served to its end is replayed by the guest body
from the same state, every test in that replay by its guest body too, and compared on the contract registers and
every committed byte of guest memory outside the stack below the caller's r1.

Measured on the Final Haunt tape, 120 s of game time, headless, release build:

| | loops in the guest | native loops |
|---|---|---|
| queries checked against the guest body, all memory | - | 87,630, **0 differed** (and 1,565,825 tests, 0 differed) |
| queries served to the end / handed back | - | 87,630 / 1,271 (all at a hit; 0 expired, 0 count, 0 cut short) |
| tests served natively | 6,714,763 of 8,430,465 (with the plain classes) | the same; 842,428 nested guest test calls |
| spawn tests / frames | 8,597,281 / 7,160 | identical |
| wall time, arms alternated | 21.55 / 21.52 / 21.58 s | 20.54 / 20.53 / 20.51 s (**-4.8 %**) |
| 120 captured frames vs everything in the guest | - | **120 identical** |

Against the all-guest chain (22.53 s) the collision broad phase is now **-8.9 %** of the game thread's time.

## CharaColliGeneralBone's update: a recorded run reused when its inputs are the same (landed, default)

Profiled with the loops native, GeneralBone's update (vf 0x88, 0x800C3D6C) was **8.5 % of the game thread**: every
test of a bone object (from the two loops, a per-frame manager pass and an enemy query) re-poses all its child
shapes on their bones. Reverse-engineered:
- 0x800C409C (the owner sync, as for GeneralCharInfo); vf 0x8C: when dirty, every child re-derived from the owner.
- vf 0x90 (0x800C3E00): the child list (sentinel this+0x64) walked with the bone pointer array at this+0x6C; each
  child with a bone is re-posed by its vf 0x54. Children on the tape: Range::RangeSphere (centre = bone x local
  position), Range::RangeCapsule and RangeCylinder: bone x translation(local) (0x8040C084, 0x8040C94C), the Euler
  rotation of +0x40 in the order +0x4C (0x8001E070: per non-zero angle an axis rotation through MSL sin / cos and
  0x8040CB14), the centre its translation column, +0x80 its inverse (0x8040CF48, a general 3x4 inverse), the axis
  ends and direction from the half length +0x38 (0x8002F5BC / 0x8003286C). this+0x29 = 1.
- vf 0x84 (0x800C3C3C): this+8 = the largest |child centre - this+0x40| + child radius over the enabled children.

The update is a deterministic function of what it reads, so it is not recomputed when nothing it reads changed
(runtime/src/gekko/chara_colli_intrinsic.cpp, `bone_inputs` has the complete read and write sets): its inputs -
r2, the FPSCR control bits, the object's vtable slots and centre, the list and bone array, every bone matrix, every
child's vtable, slots, local position, angles, order, half length, radius and enable byte (and its centre when it
has no bone) - are gathered as words. A test whose inputs equal the object's recorded run gets that run's output
words written, FPCC set as the run left it, and the FPRs the run's callees save with psq_st rounded as their
restores leave them (f21-f31 when a capsule or cylinder is re-posed, else f30-f31). A run is recorded only when it
changed no FPSCR bit but FPCC (whatever it raised was already set: the game runs with FX/VX/VXVC set since an early
ordered compare of a NaN), and reused only while those exception bits are still set. A test with no matching run
calls the guest update nested, the guest test's prologue built first and the test's own return address as its link,
records it, and continues natively. A dirty object declines; a bone test inside its sphere is handed to the guest test
at its vf 0x88 return, the update's writes standing.

Measured on the Final Haunt tape, 120 s of game time, headless, release build:

| | |
|---|---|
| bone updates from a recorded run / run by the guest (recorded) / dirty | 759,116 / 35,376 (35,376) / 4,990 |
| queries checked against the guest body on all memory (`--verify-chara-colli`) | 83,409, **0 differed** |
| tests checked against the guest body | 1,857,981 (20,859 on all memory), **0 differed** |
| spawn tests / frames | identical to the all-guest run |
| wall time: all guest / all native, arms alternated | 22.56 / 23.55 / 23.57 s vs 19.52 / 19.50 / 19.54 s |
| wall time: native loops without the bone runs (earlier build) | 20.52 s, so the bone runs are about **-4.9 %** |
| 120 captured frames vs all guest | identical (two all-guest captures differ from each other on 18 pixels by 1 step, the known NVIDIA noise; the native capture equals one of them) |

The collision broad phase is now **-13 %** of the game thread against all guest (22.5 -> 19.5 s).

### The update computed natively (landed, default): runtime/src/gekko/chara_colli_bone.cpp

What is not reused is now computed by native code, dirty updates included; the guest update is kept only as the
fallback for what the native contract excludes (a child class other than the three Range shapes, more than 32
children, a sine / cosine argument above 2^19 pi/2, where MSL's __kernel_rem_pio2 is not ported) and as the oracle.
The mathematics, recovered (the 3x4 matrix routines lifted symbolically from the disassembly: every stored word as an
expression of the loaded words, operand order and rounding included):

| guest | what it is | native |
|---|---|---|
| 0x8040C084 | [I \| v] | `translation` |
| 0x8040C94C, 0x8040CB14 | a x b (3x4 affine), one fused multiply-add per term: a[i][2] b[2][j] + (a[i][0] b[0][j] + a[i][1] b[1][j]) (+ a[i][3]) | `mul34` |
| 0x8040C6CC / 0x8040C668 | m x [p 1] / m's rotation x v | `point` / `rotate` |
| 0x8001E070, 0x8040C110 / C188 / C200, 0x80031568 | Euler rotation: per non-zero angle (order 0: z y x, 1: z x y, 2: y x z) m = m x R_axis, R from MSL sin / cos in double rounded to single | `euler`, `rotation`, `sincos` (MSL cos ported alongside the existing sin) |
| 0x8040CF48 | cofactor inverse adj(m) / det(m); det 0: a copy | `inverse` |
| 0x8040DFA0, 0x8040DEC4 | length through the game's sqrt; normalize (zero vector below 1e-6) | `length`, `normalize` |
| 0x8002F664 / 0x8003291C / 0x800354A0 | the re-pose on a bone (vf 0x54) | `repose` |
| 0x8002F774 / 0x80032A2C / 0x80035500 | the dirty re-derivation from the owner (vf 0x50) | `rederive` |
| 0x8002F5BC / 0x8003286C | centre, inverse, axis ends and direction | `finish_oriented` |
| 0x800C3C3C | the bounding radius | in `update` |

In this runtime an arithmetic helper changes no FPSCR bit (the status and exception bits of arithmetic are not
modelled; fp_arith_slow takes FPSCR by value), so each operation is a pure function of its operand bits and the
native code computes the dataflow freely: one helper per guest operation on the same operands in the same operand
order (NaN propagation), the guest's single conversion at every store. The compares - the only FPSCR writers - run in
the guest's order. The guest's machinery that is gone: the call and stack frames of ~25 functions per child, the
11-FPR save / restore of the inverse and multiply, the stack copies between them, the virtual calls for the local
position, angles and order.

Verified on the Final Haunt tape (`--verify-chara-colli`: every native update also run by the guest update from the
same state and compared on every word and byte the update writes, FPSCR and f14-f31, every 16th on all memory; every
64th also on changed inputs - NaN / infinite / denormal / -0 / signalling / large angles, a zero or NaN axis, rotation
orders 1, 2, 7 and -1, NaN positions and radii, a singular or NaN bone matrix, dirty states - each run by both from
the same changed state and put back after, the guest reference with MSR[EE] clear so no interrupt cuts it short):

| | |
|---|---|
| native updates checked against the guest's | 40,366 (35,376 clean, 4,990 dirty; 2,523 on all memory), **0 differed** |
| changed-input cases (a 30 s run) | 3,891, **0 differed** |
| updates run by the guest | 0 |
| queries / tests against the guest body, all memory | 83,268 / 1,868,648, **0 differed** |
| spawn tests / frames | identical; 120 captured frames identical (two all-guest captures differ from each other on 19 pixels) |

Wall time, arms alternated:

| | guest update | native update |
|---|---|---|
| every update computed (`--chara-colli-bone-no-memo`) | 20.49 / 20.53 / 20.52 / 20.54 / 20.55 s | 19.44 / 20.50 / 19.54 / 19.54 / 19.53 s (**-4.9 %**) |
| with the recorded runs (default; `--chara-colli-bone-guest` for the guest's) | 19.51 / 19.53 / 19.51 s | 19.51 / 19.49 / 19.55 s |
| all guest (`--no-chara-colli-intrinsic`) | 22.58 / 23.57 / 22.54 s | - |

The native computation alone now saves what the recorded runs saved: an update computed natively costs about what
the recorded run's input check costs, so on this tape the two together are no faster than either. Both are kept; the
recorded runs need the native code no more.

## The other Body test filters (landed, default; no measurable speed-up)

0.84 million Body tests carried a filter other than the ground behaviour's, nearly all from the enemies' query
fn_800C615C (call site 0x800C62E4). A census of the declines found five filter functions; every one is integer
logic over the test's pair (+8 the object, +0xC the shape's mask, +0x10 the filter's argument) and now runs natively
(`run_filter`):

| filter | tests | returns 1 (test the object) when |
|---|---|---|
| 0x801A658C | 378,331 | mask & 0x1AB3 != 0 |
| 0x801C80A4 | 167,863 | mask & 0x50008 == 0 |
| 0x8005B6C8 | 149,697 | mask & 0xC0018 == 0, unless the object `__dynamic_cast`s (to r13 - 0x7978, from r13 - 0x7980) to one whose owner (its weak_ptr locked and released) has +0x54 == arg[0] != -1 |
| 0x801C80C0 | 87,741 | mask & 0x100108 == 0 |
| 0x801C0910 | 52,586 | the object is neither arg[1] nor arg[3], and mask & arg[0] != 0 |

Tests served natively: 8,342,539 of 8,430,465 (99.0 %; 570 with an unported filter, 87,356 inside the sphere). Under
`--verify-chara-colli`: 1,868,648 tests (21,731 on all memory), 83,268 queries on all memory, 40,366 bone updates and
15,183 changed-input bone cases, all **0 differed**. Wall time unchanged (19.53 / 19.51 / 19.53 s; all guest 23.56 /
22.58 / 22.57 s): these filters are a few instructions, and declining them to the guest body cost little - the cost
around them is the enemies' query loop itself, still guest code. 120 frames identical to all guest (two captures of
the same native build differ from each other on 15 pixels by 1 step, the known noise).

## The enemies' query 0x800C615C, served natively (landed, default; no measurable speed-up)

The enemies' ray query (from 0x80021FE4: fn_801A6458, fn_801CC870, ...) is the ground ray loop without its segment
copy and its stop at a hit on the segment's start: frame 0xE0, r17-r31, the segment used in place, vf 0x60 per object
with or without a result and filter, a hit building the result objects in the guest; it returns the list argument.
Transcribed the same way (`run_enemy`), with the same hand-backs (a hit at 0x800C62E4 / 0x800C641C, an expired entry
at 0x800C6228, a release to zero at 0x800C650C, a count at 0x800C61F4, a cut-short nested call where it stands).

| | |
|---|---|
| queries on the tape (all three loops) | 114,010; 106,182 served to the end, 1,417 handed back at a hit, 6,411 at a cut-short nested call |
| queries checked against the guest body on all memory | 106,182, **0 differed** (and 481,942 tests, 40,366 bone updates, 15,183 changed-input cases: 0 differed) |
| wall time, arms alternated | 19.52 / 19.53 s (a 20.71 s outlier), unchanged; `--no-chara-colli-loops` 20.61 / 21.56 / 20.56 s |
| frames | 120 identical (two native captures differ from each other on 6 pixels) |

Profiled before, the query was 3.4 % of the game thread inclusive, nearly all of it the tests and bone updates it
calls - already native. Its own guest loop was too small to measure.

## Re-profile with everything above native (2026-10-28)

tools/native_sample.py --stacks, three runs x 120 s of the tape, 7,198 game-thread samples, tools/stack_report.py.
The top-level tasks (each called by the native scheduler), inclusive:

| chain | share | what it is | what is known |
|---|---|---|---|
| fn_8004620C -> fn_800471BC | 10.9 % | the render traversal: RenderWare world / clump rendering (fn_8048652C -> fn_80461B6C, fn_80486504 -> fn_80474EEC, fn_80460FAC -> fn_80463538) feeding the native renderer's capture | includes the native far-object proxies (FarObjects::collect / model_for, ~1.3 %) and RwMatrixMultiply |
| fn_800C949C -> fn_800C7068 | 9.9 % | the CharaColli manager's per-frame pass: every collision object's vf 0x7C update and the pairwise tests between them | its updates are the code already ported: GeneralBone 0x800C3DD4 (2.5 %, reaching the bone re-pose through guest code), Body 0x800C1788 -> 0x800C1A74 (1.0 %), General 0x800C3594 -> 0x800C409C (0.7 %); the rest is its own loop, weak_ptr copies and locks (~3 %) and the pair tests |
| Player::Behavior::Ground (fn_8008331C) | 6.4 % | the ground chain, what is left in guest code around the native collision | |
| fn_804196F0 (recursive), fn_8041EC24 | 6.7 % | a hierarchy walk that calls MSL sin / cos (rem_pio2, the kernels) and the game's sqrt per node; reached from ShadowMotion and others through fn_80416EF4 -> fn_80419094 | not yet named (likely the animation hierarchy) |
| Effect::PJS::EffectTask (fn_801D2318 -> fn_804262E4) | 6.2 % | the PJS particles' update | |
| fn_8015AD54 -> fn_800E5280 | 5.3 % | a map task (functions.csv tags it "map"): fn_80163394 -> fn_80416EF4, fn_800E4AD8 | |
| fn_801A6F08 -> fn_801A7280 | 5.0 % | the shared enemy update (fn_801A1370 -> fn_802CDD7C, fn_8018DE04, fn_8018D204 -> fn_801A5B6C; the enemy query's caller) | |
| MessageTask (fn_801FB5AC) | 4.1 % | | |
| fn_80385950 | 3.8 % | the SDK (audio callback path: the native reverb, AX) | native already |

Innermost host code: 4.4 % unattributed, the native collision's distance and compares ~4 %, RwMatrixMultiply 2.1 %,
psq_l / psq_st 1.9 %, the native reverb ~1.8 %.

## Measurement correction: the game thread's own time

The process wall time `ab.ps1` measured moves in ~1 s steps (every run ends on x.5 s: the exit waits on a periodic
tick), so a change under ~1 s could read as nothing or as a full second. From here on the measurement is the run
report's `guest_run_wall_seconds` (the game thread's own wall time; ab.ps1 prints it). Re-measured with it, the three
query loops together (`--no-chara-colli-loops`) are 18.40 -> 17.16 s, **-6.7 %**, not the -4.8 % the steps showed; the
"no measurable speed-up" of the enemy query and the filters above was measured with the coarse timer and is not
established either way.

## The manager pass's per-object updates (vf 0x7C), served natively (landed, default)

The manager's per-frame pass fn_800C949C -> fn_800C7068 (9.9 % of the game thread) is a pairwise loop: for each
active object A (+0x2A enabled, +0x2B enabled last frame) and each later object B of a group the bit matrix pairs
with A's, it calls A's vf 0x7C once and B's for every pair (2.6 million calls in 120 s), looks B up in A's contact
list (A+0xC, a weak_ptr lock per entry), and when either moved (+0x29) compares |cA - cB|^2 with (rA + rB + k)^2
(vf 0x38 radius, vf 0x14 centre, k = r2 - 0x6660), running the narrow phase vf 0x80 inside it; contacts and their
events go through 0x80026AAC, 0x800C77D8, 0x800C78E0, 0x800C6A4C. After A, +0x29 = 0 and +0x2B = +0x2A.

vf 0x7C is the per-object update: GeneralBone 0x800C3DD4 = vf 0x88 (the bone update); Body 0x800C1788 / General
0x800C3594 = vf 0x88 (the owner sync, or the plain classes' blr), then vf 0x8C (0x800C1714 / 0x800C34D8: when dirty,
Body's shape / General's children re-derived from the owner, +0x29 = 1, +0x28 = 0). All three are now served by the
native code above (`run_manage`; chara_colli_bone's `rederive_shape` / `rederive_children`); the FPRs the guest's
callees round are tracked per path (the inverse f21, the bone multiply f23, a rotation or the radius f30, none for a
sphere re-derived without one). `--no-chara-colli-manage`: A/B.

| | |
|---|---|
| vf 0x7C calls on the tape | 2,603,170: 232,786 bone, 1,464,278 Body, 889,821 General served (27,624 dirty re-derivations), 16,285 declined |
| checked against the guest's vf 0x7C (`--verify-chara-colli`: contract registers, FPSCR, the object's and its shapes' bytes; every 256th all memory) | 2,586,885, **0 differed** (queries 106,182, tests 481,942, bone updates 40,524, changed-input cases 15,112: 0 differed) |
| bone updates reused | 991,744 (the pass reaches the memo too) |
| game thread, arms alternated (`guest_run_wall_seconds`) | 17.13 / 17.21 / 17.16 / 17.74 / 17.49 s -> 16.44 / 16.62 / 16.47 / 16.44 / 16.72 s (median **-4.3 %**) |
| all guest (`--no-chara-colli-intrinsic`) | 21.43 / 21.38 s: everything native is **-23 %** of the game thread |
| frames | 120 identical (two native captures differ from each other on 19 pixels) |

## Next candidates

- **The rest of the manager pass fn_800C7068**: the pair loop itself (its weak_ptr locks and contact-list walks, the
  2.6 million vf 0x7C dispatches, the radius / centre virtuals and the distance test), transcribed as the query loops
  were, the contact and event helpers and the narrow phase as nested guest calls.
- The animation hierarchy walk fn_804196F0 (6.7 %): name it first (RW37SDK / the Heroes cross-reference).
- The render traversal (10.9 %): RenderWare's own code; overlaps the native renderer's capture.
- 0x80021FE4 (5.0 % inclusive): the enemies' ray wrapper around the query, and its static-world mesh tests.
- The per-frame manager pass (fn_800C7068 -> GeneralBone vf 0x7C, 0x800C3DD4) and the enemy query (fn_800C615C),
  which reach the bone update through guest code.
- 40 % of the samples have no class method on the stack (RenderWare, the SDK, the shared enemy update
  fn_801A6F08 -> fn_801A7280): naming them is RE work.

## Runtime helpers and the effect engine, net (2026-10-30, Phase VII)

Measured with GEKKO_SELF_SAMPLE (50 us, stacks) on the Westopolis replay (21,613 game-thread samples) and
tools/sample_subtree.py (exclusive inside a subtree: shares add up without double counting). self_sample_report.py
--select takes a LIST of frames, not a range.

Top exclusive host frames, whole run: ppc::fp_compare 2.2 %, Cpu::find_hle 1.5 % (+ try_hle_slow 0.7 %),
ppc::psq_load + psq_store 1.8 %, recomp::call_indirect 1.1 %, the native CharaColli distance 1.2 %, MatrixMultiply
intrinsic 2.1 %, audio (fused / reverb / mixer) ~3.8 %. Top guest self: 0x804421D8 2.0 %, 0x8041669C 1.6 %,
0x80428044 1.4 % (particle step), 0x8043531C 1.1 % (shimmer draw), 0x804162AC 1.0 %, 0x8043C3F0 1.0 % (not yet named).
The particle engine (effect update 8.51 % + draw walk 4.80 %) is mapped in docs/effects/particles.md section 7.

FPSCR: the core models it exactly (ppc_ops.h fpscr_raise, every fps_* helper); a native owner stays bit-exact on
FPSCR by calling those helpers in the guest's order (as the CharaColli intrinsic does) - no unmodelled bits needed.

| change | gates (state report, audio PCM, frame-3000 capture; both tapes) | game thread, arms alternated x3 | verdict |
|---|---|---|---|
| find_hle: the direct-mapped HLE slot also holds its entry index (one load instead of the hash lookup when the slot names exactly the address; shared / verification slots keep the map) - cpu.h hle_slot_entry_, cpu.cpp find_hle / rebuild_hle_slots | byte-identical | Westopolis 10.594 -> 10.575 s (noise, ranges overlap); Final Haunt 11.740 -> 11.656 s (-0.7 %, ranges disjoint) | KEPT (in source, not committed) |
| ppc::fp_compare forced inline (8,679 generated sites) | byte-identical | Westopolis 10.585 -> 10.511 s (-0.7 %); Final Haunt 11.637 -> 11.674 s (+0.3 %, overlapping) | REVERTED: not demonstrably faster; the .build/diag binary still contains it until the next rebuild |

Next by measured net cost: the animation hierarchy (fn_80416EF4 -> fn_80419094 -> fn_804196F0 / fn_8041EC24 and the
0x8041xxxx self-cost leaves 0x8041669C, 0x804162AC) - re-sample with a binary whose map is kept beside the samples
(the diag map was overwritten by a rebuild before the subtree could be named); the per-particle affector family and
the particle step (native owners through the ppc_ops helpers); the shimmer draw 0x8043531C (1.9 %); psq_load /
psq_store and call_indirect (runtime helpers, exact by construction).
