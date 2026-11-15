# opt-mtn2: the Mtn pose pass (2026-11-12)

Agent `opt-mtn2`, branch `agent/opt-mtn2` (cut from aa54d9b, round 2). Scope: the Mtn residue 0x80411000-0x80422700 not owned by round 1
(docs/agents/opt-mtn.md). Names are this project's own, from the disassembly and data; evidence marks P (proven from the cited
words / measured), I (inferred), U (unknown). The external decomp was not used for any name or code.

## 1. Profile that picked the targets

Base exe (the round-1 integrated build), headless unpaced Westopolis replay, `tools/perf/sample_threads.py` with line mapping, game thread,
15,700 samples. Self time of the translated bodies in the range: 0x80420B9C 0.68 %, 0x80414368 0.59 %, **0x804196F0 0.54 %**, 0x8041EC24
0.52 %, 0x8041E8EC 0.39 %, 0x8042193C 0.32 %, 0x804121BC 0.28 %, 0x8041E5D0 0.27 %, 0x8041F174 0.21 %, 0x80421774 0.18 %, 0x80421BB0 0.11 %
(7.7 % of the game thread in the whole range, of which the owned set below is about 3.5 %).

## 2. RE findings

Matrix stack (P): the object at `[r13 + 0x5F50]` is `{+0 base, +8 index}` with 0x30-byte entries; top = base + 0x30 * index
(0x80412750), push 0x8040D254 (index + 1, copy the old top up), pop 0x8040D294 (index - 1 while > 0). 0x8040C748 copies the static
identity at 0x805E17E0 into its r3; rotations 0x8040C85C / C898 / C8D4 (Z / Y / X, r3 = matrix, f1 = radians) build a rotation and compose.

| Address | Name | What it does | Ev |
|---|---|---|---|
| 0x804196F0 | NodeWalk(r3 motion, r4 node) | recursive. Node +8 list of callbacks `{fn, next, args at +8}`, +0x1C post callback (3-word struct in the frame: stack top, node +0x20, `[[node +0xC]]`), +0x10 matrix receiving a copy of the top, +0x24 child, +0x28 sibling (non-zero: push first, pop after the subtree). Channel weights: for each channel index of the motion table (+0x24, count +0x20) with a non-zero record (node +0x18 + 32 * idx), `weights[idx] = fmadds(table, rec[2], rec[3])`, the old value saved at frame + 0x14 + 4 * idx and restored after the child. Motion +0x14 bit 1 ends the walk after the node | P |
| 0x8041EC24 | RotationPass(args) | args `{+0 list {count, indices*, parents* at +0xC}, +4 base matrix, +8 pairs {source*, flags*}}`. Orders the bones (parent before child with the flag arrays at 0x805E23A0 + 0x1E0 / 0x1F0 / 0x200), quantises each bone's three angles to 16-bit turns (angle * 65536 / 2pi, fctiwz), builds the Euler matrix (identity, Z, Y, X) on the stack top, stores it per bone (0x805E23A0 + 0xF0 + 0x30 * idx) and blends it into its parent's accumulator with MatrixBlend (weight at 0x805E23A0 + 0x210 + 4 * idx = the channel weight table 0x805E25B0) when the flag word's bit 0 is set, else copies. Push at the start, pop and `top = top * accumulator` at the end | P |
| 0x8041E5D0 / 0x8041E8EC | ScalePass / ScaleLengthsPass | the same ordering for scale vectors (0x805E23A0 + 0x1B0 + 0xC * idx): `dst = (1 - w) * base + w * source`; the base of the first bone is the matrix's scale vector (+0x30) or, for the second, the column lengths of the matrix (0x80412378); `scale_columns(top, accumulator)` at the end unless it is exactly (1, 1, 1) | P |
| 0x8041F174 | TranslationPass | the same for translations; the first base is the matrix's translation column (+0xC / +0x1C / +0x2C); `translate_by(top, accumulator)` always | P |
| 0x80420B9C | EulerBone | one bone's angles (indices[0] of the list) applied to the stack top, Z then Y then X | P |
| 0x8042167C | MatrixBlend(out, a, b, t) | quaternions of a and b (0x8042193C), slerp (0x80421774), back to a matrix (0x80421BB0), columns normalised (0x804121BC) | P |
| 0x80421774 | QuatSlerp | shortest arc (dot < 0 negates the weight), acosf / sinf of the game's MSL, linear weights for dot >= 0.9999 | P |
| 0x8042193C / 0x80421BB0 | MatrixToQuat / QuatToMatrix | the usual branch on the trace and the largest diagonal element; 2x2 products with fmsubs / fmadds | P |
| 0x804121BC | MatrixNormalize(dst, src) | copies src to dst (12 words, forward), unit columns when longer than 2^-23; a 0 argument is the stack top (through frame locals) | P |

Not owned: 0x80414368 / 0x80414290 (the clip cache's allocator path: see docs/agents/opt-mtn.md), 0x80412868, 0x80419A9C, 0x80418618 and the time-advance
chain (each 0.2 % or less).

## 3. Owners

`runtime/src/gekko/native_mtn_pose.cpp` (+ `runtime/include/gekko/native_mtn_pose.h`), installed right after `mtn_track::install`. Eleven owners:
0x804196F0, 0x8041EC24, 0x8041E5D0, 0x8041E8EC, 0x8041F174, 0x80420B9C, 0x8042167C, 0x80421774, 0x8042193C, 0x80421BB0, 0x804121BC.
NodeWalk calls the five callbacks as C++ when the list entry is one of them and by `guest_call` otherwise (a different function pointer, the
post callback). Callees outside the range: the rotations 0x8040C85C / C898 / C8D4 by `guest_call` (unchanged guest code, not owned);
0x8040C5EC / 0x8040C7E0 by calling their native owners (`math_affine::serve_*`); the retired composition 0x8040CB14 through
`shadow::math::compose_affine_matrix` exactly as the generated lift-site boundary does; sqrt through `se_math::detail::guest_sqrt`, acosf / sinf through
`shadow::math::msl` in the caller's FPSCR[NI] mode. The matrix-stack leaves (D198 copy, D254, D294, 12750, 0x800091D0, 0x8040C748's copy) are
a few guest-memory words each and are inlined, not hooked: no owner is installed outside 0x80411000-0x80422700.

Frames. An owner has no frame, but the original's locals that are read back live at the same guest addresses (r1 is lowered by the original's
frame size while guest functions are called), so a local the original reads without having written (an accumulator of an empty list) reads the
same stale word the original would read. The dead frames (saved registers, link slots, FPR spills, copies nothing reads) are not written.

Preconditions (documented, not enforced): channel indices of the walk are at most 10 (the original saves into its 0x40-byte frame
at +0x14 + 4 * idx and clobbers its own saved registers beyond that); a bone list has a root that is processed (otherwise the original composes an
uninitialised stack local: undefined data); a channel in the table has a record (otherwise the original restores a stale frame word into the weight
table). All three hold for any data the game builds; the contract test generates only such data (`MTN_POSE_STALE=1` keeps the others).

FPSCR: no compare result code is kept (as round 1); fctiwz does not set FPSCR[FI / XX / VXCVI] sticky bits (the translated code does not either: the
interpreter and `ppc::fctiw` agree, and the test compares FPSCR except FPCC).

## 4. Gates

Exactness: `mtn_pose_contract_tests` (ctest label `reference;native-contract`, `MTN_POSE_N` scales the count; default 40000 per entry). Original DOL
instructions in the reference interpreter vs the owner on a second machine; compared after each call: the whole 8 KiB arena (matrix stack,
argument structures, matrices, vectors), the DOL .bss pass scratch and channel-weight tables (0x805E2300-0x805E2700), the caller's frame above r1
(except the link slot), r1 / r2 / r13, r14-r31 and f14-f31 (both halves), FPSCR except FPCC, r3 where the original returns 1 (QuatSlerp). The junk under r1 is
random and identical on both machines. Run: N=5000 -> **147,387 calls, 0 differences** (113 skipped: the original did not finish within 3M steps on cyclic
random data); a second run with N=1500 (86,163 calls, 0 differences) after the last scale-pass generator fix reached **every original
instruction of all eleven bodies** (NodeWalk 128/128, RotationPass 340/340, TranslationPass 191/191, ScalePass 199/199, ScaleLengthsPass 206/206,
EulerBone 93/93, QuatSlerp 114/114, MatrixToQuat 157/157, QuatToMatrix 52/52, MatrixNormalize 111/111, MatrixBlend all). Negative controls caught: wrong
FPSCR[NI] 20000/20000, one-ulp-off matrix 20000/20000, stale flag 19950/20000 (rest had the bit). Constants compared with the DOL words. NodeWalk trees:
depth <= 3, mixed owned / guest callbacks, post callbacks, copies, weights, siblings with push/pop. Test-design finding: a first run showed mismatches that
were all reads of uninitialised stack locals (empty bone lists, channels without records) - undefined data, see the preconditions in section 3.

Game behaviour: `state.json` (game ticks, VI / render counts, 64 KiB block hashes of all guest RAM) of 10 runs (base 1-5, candidate 1-5, headless Westopolis,
316 game s) has the SAME md5 (0f534f094c3a...) - the dead stack words match too, so no classification is needed.

## 5. Benchmark

`.build/ab.sh` (the coordinator's run.sh per arm), alternating, lock `benchmark`, `capped_run -CpuPercent 7`, headless unpaced, 316 game s, numbers = session.json
guest_run_cpu_seconds, first pair discarded. Machine load drifted during the block (36.3 -> 33.3 s for the same base), so compare within pairs.

| round | base | candidate | change |
|---|---|---|---|
| 1 (discarded) | 36.34 | 35.53 | -2.2 % |
| 2 | 35.84 | 34.94 | -2.5 % |
| 3 | 35.19 | 33.16 | -5.8 % |
| 4 | 32.77 | 31.88 | -2.7 % |
| 5 | 33.28 | 32.02 | -3.8 % |
| mean of 2-5 | 34.27 (32.77-35.84) | 33.00 (31.88-34.94) | **-3.7 %** |

The candidate is lower in all five pairs, but the ranges overlap because of the load drift: read the claim as "about -3 % to -4 %, paired", not as a disjoint-range result.

## 6. Open problems / what is left

* The rotations 0x8040C85C / C898 / C8D4 (+ the sincos and compose they call) still run as guest code through `guest_call`: up to three per bone per frame. They are
  the matrix library's (opt-matrix); when they have owners a direct C++ call replaces `guest_call` here.
* Not owned: 0x80414368 / 0x80414290 (allocator path), 0x80412868, the time-advance chain 0x80419A9C / 0x8041BF20 / 0x8041C01C (0.2 % or less each).
* No plain-float fast path (round 1's Hermite idea): everything uses `ppc::fps_*`, exact by construction; the gain is the removed frames, register-file traffic and dispatch.
* `native_file_api_tests` was not built; `lift_native_math_evidence` (verify_native_math.py) was not run - it may list the 13 removed lowered call sites.

## 7. Shared-list lines changed

* `CMakeLists.txt`: `runtime/src/gekko/native_mtn_pose.cpp` after `native_mtn_track.cpp`; the `mtn_pose_contract_tests` block after `mtn_track_contract_tests`.
* `extract/native-owners.txt`: a commented block at the end, 11 rows: 0x804196F0, 0x8041EC24, 0x8041E5D0, 0x8041E8EC, 0x8041F174, 0x80420B9C, 0x8042167C,
  0x80421774, 0x8042193C, 0x80421BB0, 0x804121BC.
* `extract/lift-sites-math.txt`: removed 13 lowered sites inside owned bodies: 0x804121E8, 0x80412208, 0x80412228, 0x80412360, 0x80419830, 0x8041EF44,
  0x8041F0C4, 0x8041F11C, 0x8041F15C, 0x80421848, 0x80421850, 0x80421864, 0x80421870 (the generator reports them stale otherwise).
* `extract/liveness-roots.txt`, `extract/liveness-roots-lifted.txt`: regenerated (after cherry-picking 7381ba7); link addresses are named constants tagged value-only.
* `runtime/include/gekko/system.h` include, `runtime/src/gekko/system.cpp` (CRLF kept): `mtn_pose::install(cpu_);` after `mtn_track::install`;
  `runtime/tests/native_file_api_tests.cpp`: `add_owner_addresses<mtn_pose::Owners>` after the mtn_track line.
* Generator: native owner rows 564 -> 581 (log: "native owners: 581 rows emit only a thunk"); `SHADOW_GC_REMOVAL_RATCHET` untouched.

## 8. Coordinator re-check (default size failed) - root cause and fixes

* Default run (N=40000, seed 0xA11CE5ED) failed 2 of 759,403 NodeWalk calls. Reproduced; with bone indices limited to 0..3 NodeWalk has 0 mismatches in 60,000
  cases, with indices up to 5 it fails often. Cause: the generator's 10 % "index 5" case. The pass scratch is four slots by construction (the static
  initialiser 0x804216F4 builds `Matrix[4]` at 0x805E23A0 + 0xF0 and `Vec3[4]` at + 0x1B0; the flag arrays are four words), so index >= 4 makes the
  original overwrite its own flag arrays, and a root-less list then makes it compose an unwritten stack local. That is the documented stale-stack
  precondition leaking into data the game cannot produce; it was not an owner bug. Indices are now 0..3 (`MTN_POSE_MAXIDX` keeps the old shape).
  Reproducing the stale read is not possible (it is the dead writes of earlier callees, which the owner does not make); instead the owner now says so once on stderr
  (`[mtn_pose] precondition not met: ...`) for: a bone index above 3, a pass whose accumulator is never written, a channel above 3, a table channel without a record.
* The real replays: Westopolis and Final Haunt print only the last kind (a table channel without a record: the restore reads a frame slot that holds the previous
  walk's saved value, which the owner writes identically), never the first three. State of the weight table and everything else is unchanged (below).
* MatrixBlend 30/62: my coverage range was wrong. 0x804216F4 is a different function (the scratch static initialiser, no caller), outside 0x8042167C-0x804216F0. Range fixed: 30/30.
* New: guest calls raise r1 by 0x10 first, so a callee's frame lands where the original's `bl` puts it (guest_call lowers by 0x10 itself).
* Default size, seed 0xA11CE5ED and a second seed (`MTN_POSE_SEED=0x1234ABCD`): both **PASS, 760,000 calls, 0 differences, 0 skipped**, controls caught 20000 x 3, all bodies 100 % covered.
* Westopolis state.json: 8 of 8 runs (base 1-4, cand 1-4) identical (md5 0f534f09...). Timing (cpu s, round 1 discarded): base 32.16 / 32.17 / 32.05, cand 31.41 / 31.72 / 31.27 = **-2.1 %**
  (all pairs lower; the earlier -3.7 % block had load drift and is superseded).
* Final Haunt (stg0604, 120 game s): `state.json` differs in exactly ONE 64 KiB block of the image (0x80600000-0x8060FFFF), 85 words between 0x8060B9D8 and 0x8060BC1F: frame debris
  of the pose pass (base: the original's dead frames of MatrixBlend/slerp; cand: the rotation callees' frames). The current thread's saved context (base) has r1 = 0x8060C4C8, above
  every differing word, so they are dead stack (below the sleeping thread's SP); every other block, the tick/VI/render counts and the weight table agree. Classified, not replicated.
