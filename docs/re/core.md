# S1 core: the shared gameplay core (2026-11-12)

Field `core` (branch `s1/core`, cut from e6b3a35 = agent/opt-integrate 700f72d + the S1 contract). Names below are this
project's own, from the PAL disassembly (`.build/dolrecon/pal-v30/out/asm`), the call graph and the runtime owners already in
the tree. Evidence marks: **P** proven from cited words or a run, **I** inferred, **U** unknown. "Reference agrees" lines mean the
external reference was only used to check a boundary; nothing here is copied from it (the reference does not name any of the
functions below: it labels them by address, exactly like our tables).

Profile used: `wt-s1-integrate/.build/s1/profile-base-700f72d.json` (base 700f72d, 11,230 game-thread samples, 10-32 s of the
Westopolis replay). In it the functions of 0x80009000-0x80050000 plus 0x800C0000-0x800CC000 take **15.4 %** of the game thread by
self time; no single one is above 0.85 %. The profile is flat: the core is many small things, not one hot loop.

Coordinator split (2026-11-12): the model-matrix sync group 0x80015000-0x80016000 (0x80015354 / 0x80015740 / 0x800158C4) goes to the enemy agent; core did no work there beyond reading it. The range-wide counted-pointer rows in 0x80015000-0x80016C00 below are map only.

## 1. Map of 0x80009000-0x80050000 (2011 functions) and 0x800C0000-0x800CB400

Callers per subsystem come from the call graph joined with the S2 ownership table (hint table; its names are not used).

| Range | What it is | P/I |
|---|---|---|
| 0x800091A4-0x800091EC | Vec3 helpers: 0x800091D0 and 0x800091EC are the **same body** (copy 3 floats r4 -> r3), 905 / 778 callers; 0x800091A4 a thunk to the matrix library's 0x8040DDF8 | P (asm) |
| 0x80009370-0x80009EA0 | **counted-handle library** (boost-style `shared_ptr` / `weak_ptr`): control block = {vtable, use +4, weak +8; vtable +0xC dispose, +0x10 destroy}; 0x8000945C release (use--, on zero dispose, weak--, on zero destroy), 0x80009408 handle dtor (release + optional delete), 0x800097E0 copy (use++), 0x80009E4C assign, 0x800094C8 / 0x80009500 control-block ctor/dtor (437 callers each) | P (asm + vtable) |
| 0x80012600-0x800127F0, 0x800122D4 | resource-name helpers and singleton accessors (`fn_800122D4` caches the instance in SDA r13+0x...; lazy ctor `static_8056FF94__instance`) | I |
| 0x80014E04-0x80014E34 | **word bit helpers**: 0x80014E04 / 0x80014E10 store 0 (two aliases), 0x80014E1C set bit r4, 0x80014E34 test bit r4 (431 callers, mostly stage objects); the layout scan calls the test with bit 0 for the "restricted" mode | P |
| 0x80015000-0x80016C00 | small value classes and the **intrusive counted pointer** (count at +8, vtable slot +0x10 releases): 0x80017454 dtor, 0x80017930 / 0x8001AEE8 copy (aliases), 0x80016838 weak -> strong lock (throws the bad_weak_ptr exception at zero), 0x80016914 add-ref-lock | P (asm) |
| 0x8001C4F8-0x8001C574 | variadic **debug-print stubs** (spill the argument registers, return 0): 0x8001C574, 0x8000D374 (332 / 309 callers); 0x8001C564 a spill-r3 stub. These are the empty-variadic shapes of `docs/EMPTY_VARIADIC_CONTRACT.md` | P |
| 0x8001E000-0x80036000 | **shape-intersection library** ("Range": Sphere, Capsule, Cylinder, Box, Triangle ... classes with pairwise tests), section 3 | P/I |
| 0x80036000-0x80050000 | character table accessors (0x8003A568: `[table + 0x588 + 4*i]`; 0x8003AA58: slot-used test), task base ctor/dtor 0x8004F014 / 0x8004EF18 (**S2 events own the Task tree**), singleton accessors, front-end / NPC glue | I |
| 0x800C0BB0-0x800C7900 | **CharaColli** (character collision bodies, general bodies, bones, manager task): already served by the host `CharaColliIntrinsic` (tests, query loops, manager pass 0x800C7068); map only, section 4 | P |
| 0x800C986C-0x800CB400 | **set layout manager**: the per-frame scan and its range leaves; section 2 | P |

### Leaf families (what the 15 % is made of)

| Family | Functions | Self % (base profile) | Verdict |
|---|---|---|---|
| counted handles | 0x80009408 .62, 0x8000945C .53, 0x80028D60 .39 (handle dtor wrapper), 0x80017454 .45, 0x800097E0 .26, 0x80017930 .17, 0x80016838 .28, 0x80028DB0 .26 (weak lock), 0x80009E4C .12, 0x80028D20 .12, 0x80028EA0 .11, 0x80016914 .09 | ~3.4 | the biggest cluster; every function is tiny and called from everywhere (CharaColli lock/release pairs, range tests, shape objects). Owned as leaves they would not pay (dispatch >= body); the saving is the call overhead of the lifted code, which is the generator's field (leaf inlining), see section 6 |
| Vec3 copy / bit helpers | 0x800091EC .30, 0x800091D0 .22, 0x80014E34 .23 | ~0.75 | same |
| layout scan | 0x800CAC08 .45, 0x800CA8D8 .28, 0x80014E34 (part of .23) | ~0.9 | one whole-loop owner, section 2 |
| shape sweeps | 0x8001EE0C .57, 0x800221A8 .47, 0x80021678 .25, 0x8002EC10 .20, 0x80023B28 .10, 0x80021934 / 0x80021E24 / 0x80021CA4 .15 / .13 / .12 | ~2.0 | float-heavy sweep code; section 3 |
| character slot tests | 0x8003AA58 .34 (called 2x168 per bullet sweep from 0x80021934) | .34 | section 3 |
| CharaColli manager pass body | 0x800C7068 .85 | .85 | host intrinsic territory (opt), not touched |

## 2. The set layout manager (0x800C986C-0x800CB400)  [claimed: s1-claims/core.txt]

Documented before in `docs/DRAW_DISTANCE.md` (range contract, flags, the whole spawn / leave / respawn cycle) and owned in part
by `NativeObjectRange` (the four range leaves 0x800C9D20 / 0x800C9D54 / 0x800CA200 / 0x800CA970). New here (P, from the asm of
0x800CAC08, 0x800CAA2C, 0x800CA8D8, 0x800CAE24):

| Address | Name (ours) | Contract |
|---|---|---|
| 0x800CAC08 | LayoutScan1 | `(manager r3, point r4)`: the one-viewport per-frame walk of the 0x800 slots; returns nothing |
| 0x800CAA2C | LayoutScan2 | the same with two points (two-player mode, +0xB2 of the stage global); the benchmark tape never runs it |
| 0x800CA8D8 | SpawnTest | `(manager, slot, point) -> bool`: flags 0x2 / 0x20000 -> 0; range test; in range -> clear 0x80040, call factory create ([factory + 0xC], r3 = slot), return 1 |
| 0x800CAE24 | GroupRelink | `(manager, group byte)`: clears the group's list head ([manager + 0x1C + 4*group]) and walks **all 0x800 slots** relinking those whose record flag 1 is set and whose group byte (+0x22) matches: a doubly linked list through slot +4 (prev) / +8 (next), head's +4 = tail |
| 0x800CA970 | InRange | owned (NativeObjectRange) |

Manager layout (P): +0x08 s32 live count (zeroed by the scan, ++ per live slot); +0x14 factory table (0 until the stage layout is
loaded); +0x18 pointer to the 0x800 slots; +0x1C + 4*g group list heads (g = record byte +0x22; up to +0x41C); +0x428 u32 mode (bit 0:
"restricted", only records flagged 0x8000 are scanned).
Slot (0x20 bytes, P): +0x00 record pointer, +0x04 prev, +0x08 next, +0x0C the live object (set by its constructor), +0x10 s32 factory index
(negative: none). Record (0x2C bytes, the layout file's entry; P for the used fields): +0x00 f32x3 position, +0x18 flags, +0x1C saved flags,
+0x22 group byte, +0x23 range byte (distance 100 * byte).
Flags (from the asm; agrees with `DRAW_DISTANCE.md`): 0x1 may spawn, 0x2 live, 0x40 out of range while destroyed, 0x800 never leaves,
0x8000 scanned while restricted, 0x10000 respawn gate, 0x20000 held, 0x80000 / 0x80800 always in range, 0x800000 restore flag.
Respawn masks: with 0x40 the saved flags | 1 minus 0x40 (keeping the old bits of 0x208), with 0x800000 saved flags | 1 minus 0x20040
(0xFFFDFFBF; `DRAW_DISTANCE.md` writes 0x10000: the mask in the asm clears 0x20000 and 0x40).

Per-slot decision of LayoutScan1 (P): `restricted && !(f & 0x8000)` skip; `f & 2` count; `f & 1`: if type >= 0, SpawnTest; else if
saved & 1: `f & 0x40` restore + relink, `f & 0x800000` restore + relink, `f & 0x10000` InRange: out of range -> `f |= 0x40`.
A frame with nobody to create, restore or mark changes nothing but the count: that is nearly every frame.

Owner `s1_core::serve_layout_scan` (runtime/src/gekko/native_s1_core.cpp): scans without writing; on a quiet frame writes the count and the
frames of the original's prologue and of its last SpawnTest call (state.json hashes stale stack words); on a frame with a spawn, a
restore or the 0x40 mark it **declines before writing** and the original body runs the whole frame (the scan only rewrites the count from
zero, so the hand-back is exact). Not in `native-owners.txt`: the translated body stays for the hand-back. Contract test
`s1_core_layout_scan_tests`. Results: docs/agents/s1-core.md.

## 3. The shape library and the sweeps (0x8001E000-0x80036000)

Call tree of the dynamic-object sweeps (P from the call graph; names ours):

```
bullets / thrown objects / boss shots / vehicles / camera (15 callers)  ->  0x80021934 SweepCharacters(world, sphere, ...)
   for slot in 0..0xA8: 0x8003AA58 SlotUsed(table, slot, mode)  ->  0x8003A568 CharacterAt  ->  [char + 0xB8] body non-null
       -> 0x80021480 SweepOne -> 0x8001EE0C SweptSphereVsBody (3280 B) -> 0x800221A8 (6392 B) / 0x80024AC8 (3832 B) triangle sweeps
   then (flag 2) 0x800C51C8 sphere query of the CharaColli world (host-served loop)
other callers: 0x80021678 line pick (host NativeWorldCollision), 0x800215C0 sphere query (host NativeWorldCollision),
               0x80021CA4 / 0x80021E24 / 0x80021FE4 enemy and attack queries (22 callers) -> 0x800C615C (host-served)
```

* The world line pick (0x80021678) and sphere query (0x800215C0) and the CharaColli query loops (0x800C5D20 / 0x800C51C8 / 0x800C615C)
  are served by host code already (`NativeWorldCollision`, `CharaColliIntrinsic`); they are not touched.
* What stays guest and is hot: the sweep of one body (0x8001EE0C / 0x800221A8 / 0x80024AC8 / 0x80023B28 / 0x80025E2C) and the shape
  library behind it. These are float-heavy (fmadds chains, _rwInvSqrt, the matrix library) and each is one 3-6 KB function: a Tier E
  owner has to reproduce every fps_* step, so the plain-float fast path would be the whole gain, about 3x on ~1.1 % of the game
  thread. Not attempted in this round (section 5).
* Anatomy of 0x8001EE0C SweptSphereVsWorld (P/I, asm read end to end): `(world r3, query r4, start r5, motion r6, ..., f1 = radius)`; builds the
  AABB of the swept sphere (min/max per axis chosen by the sign of the motion component, +/-radius, a 0.001 margin kept in f31),
  then walks the RenderWare world's collision BSP with an explicit stack in the frame (node = {axis +0, children +8/+0xC, planes
  +0x10/+0x14}; a negative axis marks a leaf sector, the sector's collision plugin is fetched at r13-relative `lbl_805F24C8`),
  asks RenderWare for the sector's triangle tree (0x80453C54 / 0x80453C9C, the collision plugin accessors), and per leaf runs the
  per-triangle sweep 0x800221A8 (or the box variant 0x80024AC8): plane distance, edge tests, `_rwInvSqrt` normalisation, closest-hit
  bookkeeping, hit objects built as `Range::RangeTriangle` C++ objects (ctor call, counted-handle copies 0x80028DB0, a `__dynamic_cast`
  through the exception-library stub 0x8001EE04). The frame is 0x470 bytes. This is the game's own swept version of the world query that
  `NativeWorldCollision` serves for the plain line / sphere picks, but its hits are C++ objects, not records.
  Why no Tier E owner in this round: the loop mixes ~1,500 instructions of exact single-precision control flow with nested guest calls
  that allocate and construct (RangeTriangle ctor, handle copies, RenderWare accessors), which cannot be called from an owner when
  they may block, and which a hand-back design would have to re-enter mid-function (no mid-function entry points exist for it). The
  tractable owner is the whole function after the C++ object construction is understood and the world BSP is mirrored natively as
  `NativeWorldCollision` already does for the line / sphere picks - a multi-session item, recorded in the open questions.
* Shape classes (P: RTTI/vtable names in `names.json`; ours from them): Range::RangeSphere (vtable 0x8051DE14), RangeCapsule (0x8051DC88),
  RangeCylinder (0x8051DD04), RangeTriangle, plus Box and the pair tests 0x8002B830 (5.3 KB), 0x8002A704 (2.9 KB), 0x80029ED0 ... The
  classes' ctor / dtor / getter virtuals sit at 0x80028F64-0x80035E2C, with 0x80028DB0 (handle copy of the shape) in nearly every one.

## 4. CharaColli (0x800C0BB0-0x800C7900): map only

Already host-served: the four test virtuals (0x800C15A0 / 0x800C1108 / 0x800C31EC / 0x800C2854), the query loops, the manager pass
fn_800C7068 (with its object update calls). Remaining guest cost in this range per the profile: the pass body 0x800C7068 (.85 %),
0x800C3C98 (.1 %), 0x800C3E00. Per the contract these belong to opt work; recorded for completeness.

## 5. Owner decisions

| Candidate | Cost (profile) | Decision |
|---|---|---|
| LayoutScan1 0x800CAC08 (+ SpawnTest inline) | ~0.9 % | built, see report |
| counted-handle family | ~3.4 % in leaves | not owned: leaf owners do not pay (round-2 finding); needs generator-level leaf inlining |
| SweptSphereVsBody 0x8001EE0C + 0x800221A8 | ~1.0 % | open: Tier E float transliteration, large; fallback fps_* path needed for NaN / denormals |
| SweepCharacters 0x80021934 slot loop | ~0.5 % | open |

## 6. Open questions

1. Generator-level inlining of the counted-handle leaves (0x80009408 / 0x8000945C / 0x80017454 / 0x800097E0 / 0x80016838 and the Vec3
   copies): ~4 % of the game thread sits in function-call overhead of 10-20 instruction bodies. Not mine (generator emission).
2. Whether the one-player scan declines often in the benchmark (spawn frames): the report counts served vs declined calls.
3. Exact owner of the sweep pair 0x8001EE0C / 0x800221A8 (needs a captured-input harness for the contract test: the sweep objects are
   built by 0x80021480 on the stack).

## 7. Round 4: the swept-sphere chain, purity and callee facts (static, from the asm and the call graph)

Self cost of the whole chain in the base profile (0x8001EE0C, 0x800221A8, 0x80021934/80021480, 0x80023B28, 0x800169F8, 0x80020F48,
0x80027374, 0x80025E2C and the small object helpers): **203 of 11,230 samples = 1.81 %**; 0x8001EE0C .57, 0x800221A8 .47, 0x80021934 .15,
0x80023B28 .10, the rest <= .08 each. 0x80024AC8 (the box variant) never appears on the tape.

| Function | Instr | Purity (P, asm) |
|---|---|---|
| 0x800221A8 SweepSphereTriangleList | 1598 | **pure**: no bctrl, no allocation; writes only through its out pointers (hit point / normal / t / flags at r26, r28, r29, r30, r31) and its own frame; calls 0x80023B28 (x9), 0x80035E2C, 0x80024AA8, 0x8001E9BC, 0x80027374, 0x800266D4, 0x80023AA0-AF0, `_rwInvSqrt`, 0x80483350, 0x8001EDEC/EDFC/EE04 |
| 0x80023B28 | 650 | pure (calls 0x800245C8 x5, 0x80024570 x4, 0x80025E2C, 0x80483350, 0x80024550) |
| 0x80024AC8 | 958 | pure (box variant) |
| 0x8001EE0C SweptSphereVsWorld | 820 | **not pure**: an optional guest filter callback (r22, `bctrl`), RenderWare collision-plugin accessors 0x80453C54 / 0x80453C9C, and the hit recorders 0x8001FADC / 0x8001FBD8 / 0x8001FC88 / 0x8001FF1C / 0x8001FFA4 / 0x800267B0 / 0x80026518 / 0x800270CC which allocate from the game's range-object pools (singletons 0x80571D40 / 0x80571D9C through 0x800268B0 / 0x800271A8) and a deleting destructor 0x80026FB4 for the previous hit; none of them has a `bctrl`, none blocks |

Why the exact owner was not built in round 4 (written down as the contract allows):
1. **Gain ceiling.** The chain is 1.8 % self; the translated code already runs every float op through `ppc::fps_*`, whose fast path is a
   host double operation, so an owner can only remove the register-file traffic and the checks (about 2/3 of 1.8 % at best, 1.2 %), and
   the S1 noise floor (65-70 % at the 7 % cap, docs/agents/s1-noise-floor.md) hides that in every measurement the project can make
   before the quiet window.
2. **Proof cost.** A Tier E owner of the pure part (0x800221A8 / 0x80023B28 / 0x80024AC8, ~3,200 instructions, 20+ fused-multiply-add
   chains, `fcmpo` ladders with NaN exception bits, `_rwInvSqrt` and 0x80483350 callees) needs the mtn-style two-path evaluation (plain
   float with a NaN / tiny-magnitude escape to `fps_*` for the *whole* function) and a contract test whose inputs are triangle lists and
   sphere sweeps in guest memory (random + degenerate + captured); the walk 0x8001EE0C adds the RenderWare world BSP fixture, a guest
   filter callback and pool allocations whose observable effects (RangeTriangle objects, handle counts) must be reproduced word for word.
   No part of that is a transliteration a reviewer can check by eye; it needs a generator for the fast-path variant.
3. **No safe partial cut.** The only function in the chain that can be served stand-alone is the pure triangle sweep; its callers pass
   it pointers into the 0x8001EE0C frame, so serving it alone saves ~0.4 % at the price of an owner dispatch per triangle batch.

Recommended instead (not done): emit, in the generator, a policy-templated body for **pure float leaves** (no bctrl, no allocation,
out-pointer writes only): `Fast` policy = host float ops plus an accumulated "NaN / |x| < 2^-100" flag, `Exact` policy = the existing `fps_*`
ops, run Fast and re-run Exact (buffering the out writes) when the flag is set. That makes 0x800221A8, 0x80023B28 and 0x80024AC8 (and the
Mtn / matrix leaves) exact by construction and measurable in the quiet window; the in-game verifier of NativeWorldCollision
(`GEKKO_VERIFY_WORLD_COLLISION`, write journal) is the model for proving them on the Westopolis replay.

## 8. The counted-handle family (round 6, static from the asm + the pinned DOL; P = proven by the cited words)

Correction to section 1: 0x80017454 / 0x80017930 / 0x8001AEE8 are NOT an "intrusive counted pointer": the count they touch is the
**weak count at +8 of the same control block** (P: 0x80017454 decrements [pi + 8] and calls vtable slot +0x10 at zero).

| Address | Our name | Contract (P) | Self % (base profile) |
|---|---|---|---|
| 0x8000945C | sp_counted_base release | use = [pi+4] - 1 stored; zero: vtable +0xC (dispose), then [pi+8] - 1 stored, zero: vtable +0x10 (destroy). Frame 0x10: back chain, LR, r31 | .53 |
| 0x80009408 | shared_count dtor(this, flag) | pi = [this]; pi != 0: release(pi); `extsh flag > 0`: operator delete(this). Frame 0x10 (stmw r30) | .62 |
| 0x800097E0 | shared_count copy(dst, src) | [dst] = [src]; pi != 0: [pi+4]++. Leaf | .26 |
| 0x80009E4C | shared_count assign(this, rhs) | rhs.pi != lhs.pi: rhs.pi != 0: use++; lhs.pi != 0: release(lhs.pi); [this] = rhs.pi. Frame 0x10 (stmw r30) | .12 |
| 0x80017454 | weak_count dtor(this, flag) | pi = [this]; pi != 0: weak = [pi+8] - 1 stored, zero: vtable +0x10; flag > 0: operator delete | .45 |
| 0x80017930, 0x8001AEE8 | weak_count copy (identical bodies) | [dst] = [src]; pi != 0: [pi+8]++. Leaf | .17 + .04 |
| 0x80016914 | add_ref_lock(pi) | use == 0: constructs and throws bad_weak_ptr (0x801EC5C4); else [pi+4]++. Frame 0x20 | .09 |
| 0x80016838 | shared_count(weak_count) ctor(dst, src) | [dst] = [src]; pi != 0 ? add_ref_lock(pi) : throws bad_weak_ptr. Frame 0x20 | .28 |
| 0x80028DB0 | weak_ptr::lock(out, weak) of one type | 0x80028EA0 expired? (= [[weak+4] + 4] == 0 through the leaf 0x800169AC): empty temp (0x80028E90) else temp = shared_ptr(weak) (0x80028E4C: 0x80016838 on weak+4, px = [weak]); out = copy of temp (0x80028D20); temp destroyed (0x80028D60). Frame 0x30 (stmw r28) | .26 + 0.11 (0x80028EA0) + .08 (0x80028E4C) |
| 0x80028D60 and 131 clones | shared_ptr dtor wrapper(this, flag) | `this != 0`: shared_count dtor(this + 4, -1), then operator delete(this) for flag > 0 | .39 (+ .13 0x800521EC, ...) |
| 0x80028D20 and 233 clones | shared_ptr copy ctor(dst, src) | [dst] = [src]; shared_count copy(dst+4, src+4); returns dst | .12 (+ .05 0x800521AC, ...) |
| 0x8000D6E8 and 117 clones | shared_ptr assign(dst, src) | [dst] = [src]; shared_count assign(dst+4, src+4); returns dst | .07 (0x800A4068) |

Layouts (P): control block {+0 vtable, +4 use, +8 weak}; vtable +0xC dispose, +0x10 destroy (the weak count includes one for the strong
group, so release's second decrement is that one); shared_ptr / weak_ptr {+0 px, +4 pi}; shared_count / weak_count = the word pi.
The clones (132 dtor wrappers, 234 copy constructors, 118 assignments) were found by exact instruction match of the 0x80028D60, 0x80028D20 and
0x8000D6E8 bodies over the DOL text (the `bl` words compared by absolute target: 0x80009408 / the operator-delete stub / 0x800097E0 /
0x80009E4C); `.build/s1/scan_clones.py`. No lock-free or interlocked pattern exists: every count update is a plain lwz / stw pair (single game
thread; P: no lwarx / stwcx. in any member).

Observability (P): `--watch <addr>` and `--context-hook <addr>` are `Cpu::install_hle` hooks on the address (shadow_main.cpp 2472-2500); a hook
installed on an address REPLACES an owner's hook there (install_hle keeps one entry per address), so a watch on an owner's own entry is
honoured by construction; a watch on a chain member the owner inlines is not, so the owner asks `has_hle` / `hle_name` of every inlined member
once and hands the whole call back when a hook that is not its own covers one (`foreign()` in native_s1_shptr.cpp).

State.json (P): the translated chain writes dead stack words at every level (frames of 0x10 / 0x20 / 0x30 bytes below r1, the saved r28..r31, the
nested return addresses); the owner reproduces them in the translated order, because state.json hashes them (docs/agents/opt-host2.md).

Owner: `s1_shptr` (docs/agents/s1-core.md "Round 6"). Hot clones hooked: 16 dtor wrappers, 12 copy constructors, 3 assignments and the 10
shared members above (41 entries). Hand-back (nothing written): last strong release, weak count reaching zero, operator delete, add_ref_lock of an
expired block, null block where the translated code throws, a foreign hook on a chain member.

## 9. Round 7: the profile of the kept build and the ranking of the unowned gameplay call trees (2026-11-13)

**Method.** Build 956f57dd43f5 (agent/opt-integrate 0f54a54 + s1/integrate merged; options ON: SHADOW_S1_CORE_SCAN, SHADOW_S1_SHPTR, SHADOW_MTN_FOLD,
SHADOW_MTN_TREE, SHADOW_MTN_PHASE_D, SHADOW_MTN_PHASE_C; the shptr counters confirm the owners ran: 26.4 M dtor-wrapper calls etc.), the Westopolis tape
headless, unpaced, at the 7 % cap; the stack sampler `tools/perf/s1_sample_stacks.py` (host stack walk of the busiest thread at 1 kHz, 32 s from game
second ~6): **10,855 samples**. Two views, both in this tree: `tools/s1_core/rank_trees.py` (greedy disjoint trees: the function with the largest net
takes all samples whose stacks contain it, the next is ranked on what is left) and `tools/s1_core/rank_closure.py` (per function inclusive, net,
guest-leaf self and the static byte size of the call closure).
* inclusive % = samples with the function anywhere on the stack; **net % = inclusive minus the samples whose frames below it are an owned native, an
  out-of-scope subsystem (RW driver, audio, CSD, effects, resource, Mtn) or the dispatch machinery** (that time stays when the guest code above it is owned);
  guest-leaf % = samples whose innermost guest frame is the function (host helpers called by it included).
* The tape is the Westopolis stage (one player, few enemies): rows for bosses / vehicles / the camera of other stages are under-weighted.

**What the profile says first.** The player's per-frame update (0x800A8160, 14.8 % inclusive) funnels into three subtrees: the ground / idle / fly
behaviours (0x8008331C 4.7 %), the contact group 0x80097658 (10.2 %, of which 0x80098ABC 6.2 %) and the target search 0x800B6CC0 (1.8 %). Inside the
contact group **one function, 0x80021934, is 5.4 % of the whole game thread**: it is the entry into the character sweep (0x80021480 -> 0x8001EE0C ->
0x800221A8 ...) that round 5 mapped.

| # | Address | Identity (ours) | Incl % | Net % | Owner verdict |
|---|---|---|---|---|---|
| 1 | 0x80021934 | CharSweep: the 168-slot character loop + the CharaColli query call | 5.52 | 3.18 | **built this round** (`s1_chars`): exact (integer / pointer code, leaves inlined, callees called as translated); ceiling ~0.85 % (guest-leaf .22 + 0x8003AA58 .52 + 0x8003A568 + two guest calls per slot); the 3.18 net is mostly the sweep tree below |
| 2 | 0x80021480 | SweepOne tree: 0x80021480 -> 0x8001EE0C (SweptSphereVsWorld, 54 KB closure) -> 0x800221A8 / 0x80023B28 / 0x80025E2C / 0x800245C8 | 2.67 | 2.48 | exact owner possible (round 5 proved the translated bodies exact; the lifted copy was 2.7 % SLOWER); a hand-written plain-float body with the `ppc::fps_*` ops on locals is the next big candidate, ceiling ~1.2 % (guest-leaf 8001EE0C .75 + 800221A8 .50 + their ppc:: helper time .7), RE-heavy (~3,000 instructions) |
| 3 | 0x80097DB4 + 0x800988A8 + 0x800985F0 | contact group residue after 0x80021934: ground contact, wall contact, ring pickup helpers | 4.0 + 1.3 + .8 | .89 + .6 + .4 | player-field code, no single hot loop; ceiling < .5 % each |
| 4 | 0x8008331C | Ground behaviour update (the sonic-like ground state machine) | 4.71 | 2.80 | not ownable as a tree (a state machine calling the contact group and Mtn); children are rows 1-3 |
| 5 | 0x80072638 / 0x800AFD28 | per-character executers (NPC SonicType, ShadowExecuter) | 4.4 / 7.8 | 2.3 / 4.9 | task entries (S2 events own the Task tree); all their cost is in 0x800A8160 |
| 6 | 0x8015AD54 | set-object exec base (gadget update gate) | 3.67 | 2.00 | objects field; 153 classes; ceiling unclear; per-class bodies are small |
| 7 | 0x8009F9A4 | Player::Controller update (input -> action mapping) | 2.60 | 1.84 | self .14: the cost is in children (0x8009F668 .41, 0x800ABFC0 .41); no loop |
| 8 | 0x80086D84 | Idle behaviour update | 2.69 | 1.53 | state machine, children shared with row 4 |
| 9 | 0x800B6CC0 | PlayerTarget search (target lists, lock-on) | 1.82 | 1.38 | weapons / player; round 6's filters were withdrawn (below noise); inclusive tree .. 0x800B76B0 / 0x800B6E38 |
| 10 | 0x801A5B6C / 0x801A1090 | EnemyDispCommon display helpers | 3.42 / 2.10 | 1.35 / 1.04 | enemy field; display-side; many small bodies |
| 11 | 0x80197D64 / 0x8017D030 | TEnemySetBase vf11 (enemy set update) | 2.10 / 2.08 | 1.11 / 1.03 | enemy field; no Westopolis weight in bosses |
| 12 | 0x801D0B0C | PJSCamera post task (camera update chain 0x8000D170, 0x80270A0C) | 1.34 | .95 | camera field; Mtn-free; ceiling ~.4 % |
| 13 | 0x8001FC28 + clones | weak_count holder dtor wrapper (37 clones; 0x80017454 under it .64) | .77 | .77 | **exact, cheap**: a 2-level chain like the shptr wrappers (which paid -3.7 %); hand-written, same contract machinery; ceiling ~.7 % (guest leaf .30 + 0x80017454 .45); built as the second candidate (`SHPTR_WEAKWRAP`) |
| 14 | 0x80021E24 / 0x80021FE4 / 0x80021678 | the other query entries (ray, enemy ray, line pick) | 3.8 / 3.0 / 2.2 | .64 / .72 / .86 | their cost is the already-owned native collision under them; guest-leaf .2-.3 % each |
| 15 | 0x800158C4 / 0x80015740 / 0x800159AC | model-matrix sync (enemy / MotionImpl) | 1.57 / .86 / 1.17 | .63 / .44 / .49 | tried in round 5 (ModelMtxSync, 2/5 faster): withdrawn |

Choice: row 1 is the highest-ranked exact tree whose own guest ceiling is above 0.5 % (the larger tree, row 2, is the follow-up that must be
assigned a whole round); row 13 is added as a cheap second candidate. Other S1 rows are task-entry wrappers or state machines whose cost sits in
children already listed.

## 10. Round 8 note: why the hand-written sweep 0x8001EE0C did not beat the translated one (window 4: median +0.4 %, withdrawn)
Evidence is the contract-test and whole-run counters (not a new profile; no timing was made in this round). The note is a cost model, to be confirmed by a
sampler run if the sweep is ever revisited.
* **What the owner saves.** The translated body costs the dispatcher's per-instruction work for about 700 executed guest instructions per sweep (the box
  arithmetic, the BSP stacks, the pair-tree walk) plus the ppc:: helper time per float op. Whole run: 267,029 sweeps, 267,029 leaves, about 1.41 M
  candidate triangles (the stats line printed these under the wrong labels before the format fix), i.e. about 5 candidate triangles per sweep.
* **What it adds.** Every candidate triangle crosses the guest boundary at least three times (plugin accessors 0x80453C54 and 0x80453C9C, 0x800221A8 or
  the callback, so roughly 4.2 M+ `call_guest` calls per tape), and each hit adds the 15-call hit chain. Each `call_guest` writes back r1, r14-r31, the
  argument registers, the stack-argument words, f29-f31 (psq_store) and LR, does the table lookup, then re-checks `exiting()` / `pc != link` after the call.
  The translated body pays none of this: it keeps those values in the Cpu already and calls with a direct `bl`.
* **Balance.** The walk's own instructions are a small share of the tree: the callees dominate, and they stay guest. Saving ~700 instructions per sweep
  against ~15-20 boundary crossings per sweep (each tens of ns) is about break-even, which matches +0.4 % +/- 1.7 %.
* **If revisited.** A whole-chain owner is needed (0x800221A8, 0x80023B28 and the plugin accessors native as well) so that the crossings disappear; the
  hit chain would need the same. Owning only the outer walk cannot win. No third sweep attempt is planned.
