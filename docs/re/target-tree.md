# PlayerTarget search tree 0x800B6CC0 -> 0x800B76B0 -> 0x800B6E38: map and owner verdict (S1 `target`, round 10, 2026-11-14)

Branch `s1/target` (from agent/opt-integrate 19f6041). Mapping only: no owner code. Names are this project's own (the earlier
tables in `tools/re/s1_names/` and `docs/re/target-search.md`); every claim is read from the pinned PAL DOL (`tools/doldis.py`), the
dolrecon call graph, the core agent's stack samples of the tip exe (`wt-s1-core/.build/prof/stacks-tip.json`, 9,682 samples) and a
call census of the tip exe (`tip-f462fa4c.exe`, headless Westopolis replay, `GEKKO_CALL_CENSUS_COUNTS`, 267 hooked addresses; the
census run was made before the latency window). The external decomp was not used. Evidence: P = read from the words or counted,
I = inferred, U = unknown.

## 1. Verdict in six lines

1. **What it is:** the per-frame target selection of the player (lock-on / homing candidate). One call per game tick (18,873 per run,
   every frame, called from the ShadowExecuter update 0x800AFD28 -> 0x800A8160). 0x800B6E38 is 1.28 % inclusive of the game thread
   (124 of 9,682 samples); 0x800B6CC0 1.41 %.
2. **The same query layer (0x801E7000-0x801EB800) serves a second chain**, `NpcTarget` 0x802072CC (25,763 calls per run, 1.25 %
   inclusive, called from the same 0x800A8160). Together the query layer is on the stack in **2.47 %** of the samples (239).
3. **Exactness:** Tier E is feasible: the tree is integer / pointer code plus ordinary float ops whose helpers are already native
   (sqrt, Vec3Normalize, MSL sin / cos), and its only virtual calls are into one closed class (`Target`, vtable 0x8053ECB0, no
   subclass in the RTTI) and, in 21 % of the frames, the player's `ShadowWeapon` holder. Everything else is the original's own frames
   (stack words) and heap order. The proof pattern of round 5 (store-for-store frame model, hand-back before any write) carries over;
   the work is large (about 110 distinct functions, ~12 float chains, a heap sequence).
4. **Guest crossings left in a whole-chain owner of 0x800B6E38:** the allocator only: **4 `operator new` + 4 `operator delete` per
   frame** (query, its vector buffer, the copy's vector buffer, the shared-pointer control block), plus **2-3 virtual calls into the
   weapon holder in the 21 % lock-on frames**. About **8.6 crossings per frame on average**, against ~12,000 guest instructions of
   work per frame that become native stores and float ops. sweep2 lost with 15-20 crossings per ~700 instructions.
5. **Ceiling:** 0x800B6E38 incl. 1.28 % -> **about 0.9 % of game-thread CPU** (0.6-1.1 %); a layered owner of the query layer that also
   serves the NpcTarget chain reaches **about 1.6 % (1.0-2.0 %)** of the 2.47 %.
6. **Verdict:** exact: **yes (Tier E, hand-back before any write)**; worth building: **yes, as a layered owner** (query layer first,
   then the PlayerTarget selector on top of it), because the round-5 filter owners failed only on size (0.25 % covered) and the
   crossing count is now known to be small. The cost is the size of the frame model (section 7).

## 2. Call profile per frame (census, P)

Direct call sites inside 0x800B6E38 (return-address counts, 18,873 frames; "x" = per frame):

| callee | calls | per frame | note |
|---|---|---|---|
| 0x801E7C40 CreateFromRegistry | 18,873 | 1 | allocates 2 blocks, copies the registry (about 11.8 targets), prune pass (class bit 0) |
| 0x800B7288 (shared_ptr wrap of the query) | 18,873 | 1 | allocates the 0x14-byte control block |
| 0x801E82C4 sphere filter | 18,873 + 3 x 4,039 | 1.64 | the 3 extra only in the lock-on frames (21.4 %) |
| 0x801E8158 same-target filter | 18,873 | 1 | |
| 0x801E84C0 view-cone filter | 18,873 | 1 | |
| 0x801E83D0 flag filter | 18,873 + 4,039 | 1.21 | mask 0x2000 (and 0x800 in lock-on frames) |
| 0x801E8528 range filter | 4,039 | 0.21 | lock-on frames only |
| 0x801E8A68 copy of the query | 18,873 | 1 | allocates the copy's vector buffer |
| 0x801E879C nearest | 2 x 18,873 + 4,039 | 2.21 | |
| 0x8007975C (player-basis transform) | 18,873 (+ 4,039 x 1 at 0x800B70CC...) | 1.2 | pure float |
| 0x800B73C8 AimSmooth | 18,873 | 1 | the float chain (section 5) |
| 0x800B759C, 0x800610FC, 0x8006119C, 0x802068F8, 0x800601EC, 0x8005FEDC / 0x8005FF1C | 1-3 each | | handle glue |

Candidates per query: the registry holds **N = 11.8 targets** on average (594,041 list steps / 46,264 factory calls, P). Flag-predicate
chains run 7.0 per filter call (round 5: 534,962 predicate calls / 76,066 calls). Estimated work per frame: ~30 predicate chains
(~230-500 guest instructions each) + the factory / copy / dtor (~3,000) = **about 12,000 guest instructions per frame** (I, from sizes
x counts). The measured cost is 23 microseconds per frame (1.28 % of 1.8 ms), consistent with ~2 ns per translated instruction.

Lock-on frames: 4,039 of 18,873 (21.4 %): the branch at 0x800B704C taken when the player holds a weapon object ([player+0x238] != 0) and
the player flag bit 0x41 at [player+0xA8] is set (0x8006CEA0).

## 3. Per-frame control flow of 0x800B6E38 (P for the call order, I for the data flow between the calls)

```
f_o  = this (PlayerTarget data object);  p = [this+0x5C] (the player);  q = [this+0x58]
v114 = (0, 0, c)                          ; local vec3 via 0x800091EC           (float const r2-0x6970 / -0x696C)
0x80206E58(q)  ; inlined leaf             ; 0x8040E0FC(v114) rotate about Y by f1
v114 = 0x800B759C(this)                   ; target origin (section 5)
0x800610FC([this+0x44])                   ; reset the previous-target handle
sp   = 0x800B7288(0x801E7C40(registry))   ; Q = shared_ptr<TargetSearchList>
0x801E82C4(Q, p, v114, 0.0)               ; sphere filter around the origin
0x802068F8 -> 0x801E8158(Q, handle)       ; remove the entry equal to the current selection
0x801E84C0(Q, p)                          ; view cone
nearest = 0x801E879C(Q, p)  -> assign to [this+0x4C] (0x8006119C)
clear bits 1 and 0 of [this+0]            ; 0x800613A4
v = scale(v114, c2); aim = 0x8040DE20(p+..., v)  -> [this+0x14] (0x800091D0)
R = copy of Q (0x801E8A68); 0x801E83D0(R, 0x2000); nearest2 = 0x801E879C(R, p)
lock = lock(nearest2) (0x800578A0 / 0x80057810 / 0x80057850)
if lock: position via Target::slot2 (vtable+8) -> [this+0x14]; set bit 1 of [this]
free R (0x801E8974, delete)
if [p+0x238] (weapon holder, 21 % of frames) and flag 0x41 of [p+0xA8]:
    holder vf at [[holder+0x18]+0x20] (ShadowWeapon vf6 0x8023B4FC); filters 0x801E83D0(Q,0x800), 0x801E82C4 x3
    (three offsets from 0x8007975C), holder vf at +0x24 (vf7 0x8023B4BC -> weapon slot 5), 0x801E8528(Q, f), 0x801E879C
    -> lock -> Target::slot2 -> [this+0x14]; clear bit 0 of [this]
aim = 0x800B73C8(this, v)  -> [this+0x20] (0x800091D0)
free Q (0x800601EC)
```
The reads are from 0x800B6E38-0x800B7244; the exact order of the calls is the call-site order in the disassembly.

## 4. Memory writes of the tree (P unless marked)

* **Stack:** every function frame (return address, saved r28-r31 / f29-f31 via stmw / stfd + psq_st, locals). These are the dead
  words state.json hashes; the round-5 owner modelled them store for store and the contract test proved the model.
* **PlayerTarget data object (`this` = [o+0x18] of the entry, r29 in 0x800B6E38):** [this+0x14] vec3 (aim position; 0x800B6F78 from
  the sphere centre math, 0x800B7018 from the locked target), [this+0x20] vec3 (0x800B7224, from AimSmooth), [this+0x2C] vec3 and
  [this+0x38..0x40] three floats (AimSmooth 0x800B7514 / 0x800B7540-0x800B7550), [this+0x44] handle (reset by 0x800610FC, assigned
  by 0x800B7248, which never runs on the tape), [this+0x4C] handle (assigned by 0x8006119C from the nearest result, every frame),
  [this+0] flag word (bits 0 and 1: 0x800613A4 clears, 0x80014E1C sets); in the entry 0x800B76B0: [o+4] and [o+0x54] floats (timers),
  [o+0x14] vec3 (copies from [o+0x20] or [o+8]), flag bit 1.
* **Heap (guest `operator new` / `delete`, 0x803A1380 / 0x803A1334):** per frame the query object (0x14 B, 0x801E7C5C), its vector
  buffer (0x800D19C0 -> 0x8001BC70, 4 x capacity), the copy's vector buffer (0x801EA340 -> 0x8001BC70), the control block of the
  shared pointer (0x14 B, 0x800B7358); the matching deletes are 0x801E89B8 (query dtor), 0x800198D8 (vector buffers), 0x8006138C
  (control-block dispose). Sizes depend on N; the order is the order of the calls in section 3.
* **Use counts of the targets / query (shared state):** every lock +1 / release -1; the net change per frame is 0 on live targets
  (round 5, P). The control block of the query is created with use = weak = 1 and destroyed (use 0 -> dispose -> weak 0 -> destroy).
* **Globals:** none written per frame. First-use initialisation of the registry singleton (0x80060180: r13+0x2DA0 / r13+0x2E59, 0x801E7F68
  enabled byte) happens once per run (the stores execute once, P).
* **SDA / constants read:** r13+0x4608 / 0x4618 / 0x4628 / 0x462C (bound-functor policy bytes), r13+0x31A4 (debug byte in the control
  block wrapper), r13-0x68F8 / -0x68F4 (float limits in 0x800B76B0), r2 float constants (0.0 and the vec3 axis constants).

## 5. Float operations (exactness concerns, P)

All single-precision; the originals use fsubs / fmuls / fadds / fdivs / fmadds / fmsubs and a few fcmpu. In the tree:
* Vec3Sub 0x8040DDC4 (3 fsubs), Vec3Add 0x8040DE20 / Sub 0x8040DE54, Vec3Scale 0x8040DDF8 / 0x8040DE88, dot 0x8040DFE0 (fmuls + 2 fmadds),
  cross 0x8040E008 (fmuls + fmsubs), squared length 0x8040DF7C, length 0x8040DFA0 (-> sqrt wrapper 0x8000FEE4 -> 0x8000FF04, owned natively),
  Vec3Normalize 0x8040DEC4 (owned natively) and normalise-in-place 0x8040DF38.
* Rotations 0x8040E0FC (Y) / 0x8040E168 (X) / 0x8040E094 (Z): sin / cos through 0x80031568 (MSL sin / cos, native), then fmuls / fmadds;
  matrices 0x8040C110 / C188 / C200 (rotation builders), 0x8040C85C / C898 / C8D4 (scales), 0x8040C6CC (matrix x vector), 0x8040D198 (identity),
  0x8040CF48 (inverse), 0x804107B0 / 0x80410DDC (angle / slerp helpers, AimSmooth on ~20 % of frames), 0x8000F3B0 (trig).
* The sphere predicate 0x801E90F0 compares a dot product to f1 (`fcmpo`, branch on the result bits); the view-cone predicate 0x801E9324
  normalises and dots; the nearest scan compares squared distances with `fcmpo` (strict less, first wins, a null owner = a large constant).
* **Concerns for the `ppc::fps_*` rules:** fused multiply-add rounding (fmadds / fmsubs: single rounding of the exact product-sum, not
  fmaf of a double), NaN / denormal inputs to fcmpo, signed zero in the dot / cross, the fres-free normalise (division by the length).
  All helpers that carry them are already exact natives (vec_math, math_affine, msl) and are called by the original through the same
  addresses; a whole-chain owner would call those natives directly (no crossing) or hand back when an input is not finite.

## 6. Virtual calls and what stays guest

| call | where | classes | stays guest? |
|---|---|---|---|
| Target slot 2 position (0x801E76FC: copies [this+8] to out) | predicates 0x801E90F0 / 0x801E9138 / 0x801E9324, 0x801E879C, 0x800B7004, 0x800B71D4 | **Target only** (vtable 0x8053ECB0; no RTTI subclass) | no: devirtualise behind a vtable-word guard, inline copy |
| ShadowWeapon vf6 / vf7 (0x8023B4FC / 0x8023B4BC) at [[p+0x238]+0x18]+0x20 / +0x24 | 0x800B705C, 0x800B7140 | the only `Player::WeaponBase` subclass is ShadowWeapon (vtable 0x80548C44); vf7 calls the held weapon's slot 5 (0x80051BF0 forwarding or an override such as ChaosSpear 0x802F14E4) | **yes**, 2-3 per lock-on frame (21 %), or hand back when the holder / weapon class is not the expected ones |
| sp_counted_base dispose / destroy ([vtable+0xC] / +0x10) | 0x8000945C when a count reaches 0 | the query's `sp_counted_base_impl<TargetSearchList*, checked_deleter>` (vtable 0x8051FE84) at the end of the frame | devirtualise (vtable guard), the delete inside stays an allocator crossing |
| checked_deleter body 0x80060110 | delete of the query | same | same |
| functor calls through bctrl (0x801E9964 etc.) | predicates 0x801E926C, 92CC, 9498, 906C, 90F0, 9138, 9324, 9430, 94E8 | a closed set of 9 predicate functions | no: direct native code |
| `operator new` 0x803A1380 / `delete` 0x803A1334 | section 4 | MSL / Sonicteam pool allocator (guest, not owned) | **yes**, 4 + 4 per frame |
| 0x80206F34 (PlayerTargetBase update, 520 B, bctrl inside) | called by 0x800B6CC0, not by 0x800B6E38 | outside the tree | not in scope (0.06 % incl) |

**Crossings per call of an owner at 0x800B6E38 (per frame): 8 allocator calls + 0.21 x 3 weapon-holder calls = 8.6.** The allocator calls
are about 100-300 guest instructions each (0x803A3E14 / 0x803A473C / 0x803A4894 pool code), 4 of the 9,682 samples (3 %) of the tree's
124. An owner at 0x800B76B0 would add two reads of the microsecond clock (0x80051BBC -> 0x80051414, native frame limiter owns the divide)
whose results the function drops (their stack words must still be written).

## 7. Owner verdict and ceiling

* **Tier E exact: yes.** Required guards (hand-back before any write): every registry entry is a live `Target` (vtable word, use > 0,
  pn / px non-null, in RAM), vector counts bounded, no aliasing with the stack window, the registry singleton already built, the
  weapon holder either absent or a ShadowWeapon whose held weapon's slot 5 is a known body (else hand back), finite inputs to the float
  chains. The exact residue (stack words, heap order, handle counts) follows the round-5 model, extended to ~110 frame types.
* **Why it can pay now:** the per-frame guest work is ~12,000 instructions against ~8.6 crossings; the measured share is 1.28 % (28 % of
  the tree's samples, 0.36 % of the game thread, have `enter_slow` / `finish_transfers` on the stack: dispatch / hook overhead, because
  the s1_shptr owners hook the handle leaves of this very tree; 14 samples are inside the s1_shptr host code). Replacing the tree by one native body removes that overhead too.
* **Ceiling:** 0x800B6E38 chain: 1.28 % x (1 - ~25 % native remainder and allocator) = **about 0.9 %** (range 0.6-1.1). Layered with the
  NpcTarget chain through the shared query layer (2.47 % total): **about 1.6 %** (1.0-2.0).
* **Cost / risk:** the frame model is the work: ~110 functions (table in section 8), float chains with saved FPRs (stfd + psq_st of
  f29-f31), a heap-order proof (the sequence new 0x14, new 4n, new 4n, new 0x14 ... delete in the reverse pattern must match the
  original's interleaving with other code), a contract test of the size of the round-5 one (~10,000 random queries plus randomised
  weapon-holder states), and two negative controls per float chain. Recommended order: (a) the query layer's entries (factory, copy,
  dtor, the five filters, nearest) with the shared-pointer glue, validated on both chains; (b) the 0x800B6E38 selector on top; (c) the
  weapon-holder branch last (hand back until then).
* **Unknown / not done:** U: the callers' view of 0x8040DDF8 / 0x8040DE20 (census shows 0 hooks reached; the generator probably inlines
  them); U: classes behind [p+0x238] outside Westopolis (ShadowWeapon only in the RTTI); U: the exact allocator interleaving with the
  rest of the frame (needs a heap trace such as GEKKO_HEAP_TRACE); U: the NpcTarget chain's own float code (0x802074A8 family); not
  measured: instruction counts per frame (the 12,000 is a size x count estimate).

## 8. Function table: every executed function of the closure (P: addresses, sizes, counts; roles P/I)

Closure = 312 functions reachable by call / tail edges from 0x800B6E38 (53,440 bytes); 184 were executed in the Westopolis run, 48 are
inlined into callers by the generator (their counts are inside the callers: 0x80014E1C, 0x800613A4, 0x8005FFF4, 0x8006CEA0, 0x8006D130,
0x8006CC90, 0x801E7B1C, 0x801E99B8, 0x801EA76C, 0x801EA780, 0x80206E58 and others), 80 never ran. "calls per run" is the whole-run count
of the function, **shared with the NpcTarget chain and other users** for the handle / vector / filter helpers (section 2 gives the
attribution of the direct call sites). The MSL heap and printf internals (0x803A2000-0x803B3000, reached through new / delete and the
debug stub) are one row below. The last column lists stores whose base register is not r1 (object, heap or vector writes).

| address | size | name | calls per run | role (P = read from the words, I = inferred) | stores through a non-stack base |
|---|---|---|---|---|---|
| 0x8000540C | 48 | memset | 106725 | memset | - |
| 0x8000543C | 184 | __fill_mem | 106725 | __fill_mem (memset body) | stbu r7,1(r6) stw r7,4(r4) stw r7,8(r4) stw r7,12(r4) stw r7,16(r4) stw r7,20(r4) stw r7,24(r4) stw r7,28(r4) stwu r7... |
| 0x800091A4 | 44 | fn_800091A4 | 306798 | Vec3 normalise thunk (0x8040DEC4 family) | - |
| 0x800091D0 | 28 | fn_800091D0 | 9346863 | Vec3 copy (3 x lfs/stfs) | stfs r0,0(r3) stfs r1,4(r3) stfs r0,8(r3) |
| 0x800091EC | 28 | fn_800091EC | 15821032 | Vec3 copy (twin) | stfs r0,0(r3) stfs r1,4(r3) stfs r0,8(r3) |
| 0x8000D374 | 80 | fn_8000D374 | 101278 | varargs debug-print stub (spills r3-r10 and f1-f8, does nothing) | - |
| 0x8000FEE4 | 32 | fn_8000FEE4 | 2246935 | sqrt wrapper (-> 0x8000FF04, owned natively) | - |
| 0x8000FF04 | 228 | fn_8000FF04 | 2267875 | sqrtf (owned natively by vec_math) | - |
| 0x800169AC | 28 | fn_800169AC | 24209268 | SharedCount_UseCount(pn) | - |
| 0x80017390 | 116 | fn_80017390 | 153372 | shared_count assign (releases the old count, copies the new, use + 1) | stw r0,8(r31) stw r0,8(r3) stw r31,0(r30) |
| 0x80017454 | 112 | fn_80017454 | 24392287 | shared_count dtor wrapper (calls release, optional operator delete) | stw r0,8(r3) |
| 0x80017930 | 36 | fn_80017930 | 20157646 | count handle copy/transfer (stores px and count word) | stw r0,0(r3) stw r0,8(r5) |
| 0x80019890 | 112 | fn_80019890 | 65137 | vector buffer deallocation (operator delete) | - |
| 0x80019A60 | 64 | fn_80019A60 | 65159 | vector destroy range | stw r0,4(r31) stw r0,8(r31) |
| 0x80019AA0 | 48 | fn_80019AA0 | 65159 | vector clear wrapper | - |
| 0x8001AEE8 | 36 | fn_8001AEE8 | 1657635 | handle copy: [r3] = [r4] | stw r0,0(r3) stw r0,8(r5) |
| 0x8001BC70 | 88 | fn_8001BC70 | 64436 | vector buffer allocation (operator new 4*n) | - |
| 0x8001BCC8 | 36 | fn_8001BCC8 | 794311 | vector max_size / capacity accessor | - |
| 0x8001BCFC | 36 | fn_8001BCFC | 748752 | vector accessor | - |
| 0x800578A0 | 156 | fn_800578A0 | 2231210 | WeakLock(out, holder): out = shared pointer if the holder is live, else null pair | - |
| 0x8005793C | 68 | fn_8005793C | 2224796 | shared_ptr from weak (throws if expired): count +1 | stw r0,0(r30) |
| 0x80057990 | 44 | fn_80057990 | 2281939 | IsExpired(holder): use count == 0 | - |
| 0x8005FEDC | 64 | fn_8005FEDC | 341350 | SharedPtr_Copy(dst, src) | stw r0,0(r31) |
| 0x8005FF1C | 80 | fn_8005FF1C | 416484 | SharedPtr_Dtor (release) | - |
| 0x80060180 | 108 | static_80576F54__instance | 65617 | TargetList_GetInstance: function-local static registry (first call builds it through 0x801E7F68) | stb r0,11865(r13) stw r0,11680(r13) |
| 0x800601EC | 80 | boost__shared_ptr__dtor_800601EC | 92528 | SharedPtr_Dtor (count release; delete when the delete flag is > 0) | - |
| 0x800610FC | 72 | fn_800610FC | 18873 | SharedPtr_Reset: temp empty handle, swap with this, destroy temp (0x8005FF1C) | - |
| 0x8006119C | 64 | fn_8006119C | 47478 | SharedPtr_AssignCopy(dst, src): px copy, count assign 0x80017390 | stw r0,0(r31) |
| 0x8006CC4C | 68 | fn_8006CC4C | 729876 | vector insert wrapper | - |
| 0x8006CEC4 | 568 | fn_8006CEC4 | 729876 | vector insert with growth (allocation of 4*n bytes through 0x8001BC70, element shift) | stw r0,0(r3) stw r0,4(r25) stw r3,8(r25) stw r0,0(r3) stw r29,4(r25) stw r28,0(r3) |
| 0x8007963C | 288 | Player__Shadow__PlayerTarget__vf1__sub1__sub2_ | 18873 | PlayerTarget_AimDirection: (target - origin) squared length < eps -> (0,0,1)-like constant; else normalise in the player frame (inverse basis 0x8040CF48, 0x8040C6CC, three axis rotations 0x8040E0FC / E168 / E094, 0x8040DF38) | stfs r1,0(r30) stfs r1,4(r30) stfs r0,8(r30) |
| 0x8007975C | 112 | fn_8007975C | 316311 | player-basis transform: builds the matrix from the player basis (0x8040D198 identity, scales 0x8040C898/C8D4/C85C from [player+0x10/0x0C/0x14]) and multiplies the vec3 (0x8040C6CC) | - |
| 0x800B6E38 | 1040 | Player__Shadow__PlayerTarget__vf1__sub1__sub2 | 18873 | PlayerTarget_Select: builds one TargetSearchList, applies the filter chain (flags, sphere, view cone, range), picks the nearest, stores the result and the aim vectors; 1040 B | - |
| 0x800B7288 | 72 | Player__Shadow__PlayerTarget__vf1__sub1__sub2_ | 18873 | make shared_ptr<TargetSearchList> from the raw query (control block via sub9__sub0), assign to the local | - |
| 0x800B72D0 | 92 | Player__Shadow__PlayerTarget__vf1__sub1__sub2_ | 18873 | shared_ptr ctor body: control block (0x800B732C) then the debug-print stub 0x8000D374 (does nothing but spill its argument registers) | stw r31,0(r3) |
| 0x800B732C | 156 | Player__Shadow__PlayerTarget__vf1__sub1__sub2_ | 18873 | new control block (operator new 0x14) for the query: sp_counted_base_impl<TargetSearchList*, checked_deleter> (ctor 0x8006014C inlined); throws bad_alloc path never taken | stw r0,0(r3) stw r0,0(r29) |
| 0x800B73C8 | 468 | Player__Shadow__PlayerTarget__vf1__sub1__sub2_ | 18873 | AimSmooth: from the chosen target position builds the aim vector (rotation about Y/X/Z by 0x8007975C, normalise 0x8040DF38 / Vec3Normalize 0x8040DEC4, tan-like 0x8000F3B0, optional slerp through 0x80410DDC / 0x8040D198 / 0x8040C6CC), writes [o+0x2C] vec3 and [o+0x38..0x40] | stfs r0,56(r30) stfs r0,60(r30) stfs r0,64(r30) |
| 0x800B759C | 140 | Player__Shadow__PlayerTarget__vf1__sub1__sub2_ | 18873 | target-origin vec3: if player flag bit 0x40 ([player+0xA8]) -> 0x8007975C(offset by player basis) else rotate by the player yaw (0x8040E0FC) and add the player base offset (0x8040C6CC) | - |
| 0x800D19C0 | 204 | fn_800D19C0 | 46282 | vector reserve(n): allocates 4*n through 0x8001BC70 and moves the elements | stw r3,8(r29) stw r30,0(r3) |
| 0x801E7C40 | 112 | fn_801E7C40 | 46264 | TargetSearchList_CreateFromRegistry: new(0x14), copy of the registry, prune with 0x801E9498 (class bit 0) | stw r3,16(r30) |
| 0x801E7CB0 | 32 | fn_801E7CB0 | 111401 | TargetSearchList_End (-> 0x801E7CD0 -> 0x8006CC90: [this+8] + 4*[this+4]) | - |
| 0x801E7CD0 | 32 | fn_801E7CD0 | 111401 | small helper (bound-functor / handle / vector glue); calls 0x8006CC90 | - |
| 0x801E7CF0 | 32 | fn_801E7CF0 | 369226 | TargetSearchList_Begin (-> 0x801E7D10 -> 0x8006D130: [this+8]) | - |
| 0x801E7D10 | 32 | fn_801E7D10 | 369226 | small helper (bound-functor / handle / vector glue); calls 0x8006D130 | - |
| 0x801E7EF0 | 36 | TargetSearchList__ctor_801E8C0C__sub1 | 46264 | small helper (bound-functor / handle / vector glue); calls 0x801E7F14 | - |
| 0x801E7F68 | 60 | static_80576F54__instance__sub0 | 1 | small helper (bound-functor / handle / vector glue); calls 0x801E8F14 | stb r0,0(r3) |
| 0x801E8158 | 128 | fn_801E8158 | 46264 | filter: remove the entry equal to the argument handle (predicate 0x801E906C), result to [q+0x10] | stw r3,16(r31) |
| 0x801E81D8 | 80 | fn_801E81D8 | 46264 | binder for 0x801E8158 (handle copy + functor build) | - |
| 0x801E8228 | 80 | fn_801E8228 | 92528 | handle copy used by the binder | - |
| 0x801E8278 | 76 | fn_801E8278 | 138792 | handle copy / functor field copy (0x801E86D4 vec copy inside) | - |
| 0x801E82C4 | 148 | fn_801E82C4 | 35691 | filter: sphere (centre vec3, radius f1) - predicate 0x801E90F0 (owner position vs centre; Target vf2 position, Vec3Sub, squared length compare) | stw r3,16(r29) |
| 0x801E83D0 | 120 | fn_801E83D0 | 50303 | KeepFlagsAny (flag filter, predicate 0x801E926C removes entries with no mask bit) - round 5 owner, withdrawn | stw r3,16(r31) |
| 0x801E84C0 | 104 | fn_801E84C0 | 20501 | filter: view cone (vec3 axis) - predicate 0x801E9324 (needs Target class bit 1; normalised direction) | stw r3,16(r31) |
| 0x801E8528 | 208 | fn_801E8528 | 5667 | filter: range (f1 squared) - two bound predicates (0x801E94E8, 0x801E9430) | stw r3,16(r31) |
| 0x801E85F8 | 92 | fn_801E85F8 | 5667 | small helper (bound-functor / handle / vector glue); calls 0x801E8704 0x801E8654 | - |
| 0x801E8654 | 64 | fn_801E8654 | 5667 | small helper (bound-functor / handle / vector glue); calls 0x801E8694 | stw r0,0(r31) |
| 0x801E8694 | 64 | fn_801E8694 | 31835 | policy-byte + vec copy of a bound functor | stb r0,0(r31) |
| 0x801E86D4 | 48 | fn_801E86D4 | 259055 | vec3 copy through 0x800091EC (inside bound-functor copies) | - |
| 0x801E8704 | 64 | fn_801E8704 | 2182 | add-candidate wrapper (rare: 2,182 per run) | - |
| 0x801E8744 | 80 | fn_801E8744 | 2182 | small helper (bound-functor / handle / vector glue); calls 0x801E8794 0xbctrl@801E877C | - |
| 0x801E879C | 400 | fn_801E879C | 70621 | TargetSearchList_Nearest: minimum squared distance, strict less, first wins; shared_ptr copy out | - |
| 0x801E892C | 32 | TargetSearchList__ctor__sub1__sub0 | 18873 | small helper (bound-functor / handle / vector glue); calls 0x801E894C | - |
| 0x801E894C | 32 | TargetSearchList__ctor__sub1__sub0__sub0 | 18873 | small helper (bound-functor / handle / vector glue); calls 0x801E896C | - |
| 0x801E8974 | 92 | TargetSearchList__dtor | 65137 | TargetSearchList_Dtor (vector destroy 0x801E89D0, delete when flagged) | stw r0,0(r30) |
| 0x801E89D0 | 76 | TargetSearchList__dtor__sub0 | 65137 | small helper (bound-functor / handle / vector glue); calls 0x801E8A1C 0x803A1334 | - |
| 0x801E8A1C | 76 | TargetSearchList__dtor__sub0__sub0 | 65137 | small helper (bound-functor / handle / vector glue); calls 0x80019890 0x803A1334 | - |
| 0x801E8A68 | 104 | TargetSearchList__ctor | 18873 | TargetSearchList_CopyCtor: copy of the entries [begin, end) of another query, end -> [this+0x10] | stw r0,0(r3) stw r3,16(r30) |
| 0x801E8AD0 | 44 | TargetSearchList__ctor__sub2 | 18873 | small helper (bound-functor / handle / vector glue); calls 0x801E8AFC | - |
| 0x801E8AFC | 56 | TargetSearchList__ctor__sub2__sub0 | 18873 | small helper (bound-functor / handle / vector glue); calls 0x801E8B34 | - |
| 0x801E8B34 | 84 | TargetSearchList__ctor__sub2__sub0__sub0 | 18873 | small helper (bound-functor / handle / vector glue); calls 0x801EA1F8 | - |
| 0x801E8B88 | 36 | TargetSearchList__ctor__sub1 | 18873 | small helper (bound-functor / handle / vector glue); calls 0x801E892C | - |
| 0x801E8BAC | 48 | fn_801E8BAC | 65137 | small helper (bound-functor / handle / vector glue); calls 0x801E8BDC | - |
| 0x801E8BDC | 48 | fn_801E8BDC | 65137 | small helper (bound-functor / handle / vector glue); calls 0x80019A60 | - |
| 0x801E8C0C | 188 | TargetSearchList__ctor_801E8C0C | 46264 | TargetSearchList_Ctor(registry): copies the registry list into a vector (loop 0x801E8CC8 / 0x801E8D18) | stw r0,0(r3) stw r3,16(r30) |
| 0x801E8CC8 | 52 | TargetSearchList__ctor_801E8C0C__sub6 | 594041 | list iterator step / compare (used per registry node) | stw r3,0(r31) |
| 0x801E8D18 | 32 | TargetSearchList__ctor_801E8C0C__sub4 | 547777 | vector push_back wrapper (-> 0x801E8D38 -> 0x8006CC4C -> 0x8006CEC4 insert with growth) | - |
| 0x801E8D38 | 32 | TargetSearchList__ctor_801E8C0C__sub4__sub0 | 547777 | small helper (bound-functor / handle / vector glue); calls 0x8006CC4C | - |
| 0x801E8D58 | 56 | TargetSearchList__ctor_801E8C0C__sub3 | 46264 | list begin accessor | stw r0,0(r31) |
| 0x801E8D90 | 32 | TargetSearchList__ctor_801E8C0C__sub2 | 46264 | vector reserve wrapper (-> 0x800D19C0) | - |
| 0x801E8F14 | 64 | TargetList__ctor | 1 | small helper (bound-functor / handle / vector glue); calls 0x801E8F54 | stw r0,0(r3) |
| 0x801E8F54 | 48 | TargetList__ctor__sub0 | 1 | small helper (bound-functor / handle / vector glue); calls 0x801EB09C | - |
| 0x801E9580 | 104 | fn_801E9580 | 5667 | remove_if (range, first predicate); find 0x801E95E8 | - |
| 0x801E95E8 | 88 | fn_801E95E8 | 5667 | small helper (bound-functor / handle / vector glue); calls 0x801E9640 | - |
| 0x801E9640 | 64 | fn_801E9640 | 2182 | remove_if (range, second predicate); find 0x801E9680 | - |
| 0x801E9680 | 84 | fn_801E9680 | 2182 | small helper (bound-functor / handle / vector glue); calls 0x801E96D4 0xbctrl@801E96BC | - |
| 0x801E96DC | 96 | fn_801E96DC | 20501 | remove_if (view-cone instantiation); find 0x801E977C + copy 0x801EB438 | - |
| 0x801E977C | 88 | fn_801E977C | 20501 | small helper (bound-functor / handle / vector glue); calls 0x801E97D4 | - |
| 0x801E97D4 | 64 | fn_801E97D4 | 92362 | invoke chain (view cone) | - |
| 0x801E9814 | 80 | fn_801E9814 | 92362 | invoke body (view cone) | - |
| 0x801E9864 | 104 | fn_801E9864 | 76066 | remove_if (flag predicate instantiation): find 0x801E98CC + copy loop 0x801EB230 | - |
| 0x801E98CC | 88 | fn_801E98CC | 76066 | find_if loop (flag instantiation) | - |
| 0x801E9924 | 64 | fn_801E9924 | 534962 | invoke chain for the bound flag predicate (builds the call frame, 0x801E9964 calls the functor) | - |
| 0x801E9964 | 84 | fn_801E9964 | 534962 | functor invoke: fn(*element, bound arg) through bctrl | - |
| 0x801E9B1C | 96 | fn_801E9B1C | 35691 | remove_if (sphere predicate instantiation); find 0x801E9C14 + copy loop 0x801EB300 | - |
| 0x801E9B7C | 64 | fn_801E9B7C | 29144 | small helper (bound-functor / handle / vector glue); calls 0x801E9BBC | stw r0,0(r31) |
| 0x801E9BBC | 88 | fn_801E9BBC | 64835 | small helper (bound-functor / handle / vector glue); calls 0x801E86D4 0x801E86D4 | stb r0,0(r30) stfs r0,28(r30) |
| 0x801E9C14 | 88 | fn_801E9C14 | 35691 | small helper (bound-functor / handle / vector glue); calls 0x801E9C6C | - |
| 0x801E9C6C | 64 | fn_801E9C6C | 220381 | invoke chain (sphere predicate) | - |
| 0x801E9CAC | 124 | fn_801E9CAC | 220381 | invoke body (sphere predicate): fn(element, point, radius) | - |
| 0x801E9D28 | 136 | fn_801E9D28 | 46264 | remove_if (same-target predicate); find 0x801E9DF0 / 0x801E9E60 | - |
| 0x801E9DF0 | 64 | fn_801E9DF0 | 46264 | small helper (bound-functor / handle / vector glue); calls 0x801E9E30 | stb r0,0(r31) |
| 0x801E9E30 | 48 | fn_801E9E30 | 92528 | small helper (bound-functor / handle / vector glue); calls 0x8005FEDC | - |
| 0x801E9E60 | 88 | fn_801E9E60 | 46264 | small helper (bound-functor / handle / vector glue); calls 0x801E9EB8 | - |
| 0x801E9EB8 | 64 | fn_801E9EB8 | 228406 | invoke chain (same-target) | - |
| 0x801E9EF8 | 80 | fn_801E9EF8 | 228406 | invoke body (same-target) | - |
| 0x801EA178 | 128 | fn_801EA178 | 46264 | prune pass (remove_if with predicate 0x801E9498: class bit 0) | - |
| 0x801EA1F8 | 328 | ref_vector_assign_length_error_801EA1F8 | 18873 | vector assign (copy of [begin, end)) with allocation (0x801EA340 -> 0x8001BC70 -> operator new) | stw r0,4(r29) stw r28,8(r29) stw r28,0(r3) stw r3,8(r29) stw r27,0(r3) stw r27,4(r29) |
| 0x801EA340 | 104 | ref_vector_assign_length_error_801EA1F8__sub10 | 18873 | small helper (bound-functor / handle / vector glue); calls 0x801EA3C0 0x801EA3A8 | stw r3,0(r31) stw r0,0(r29) |
| 0x801EA3C0 | 40 | ref_vector_assign_length_error_801EA1F8__sub10 | 91699 | small helper (bound-functor / handle / vector glue); calls 0x801EA3E8 | - |
| 0x801EA3FC | 84 | ref_vector_assign_length_error_801EA1F8__sub0 | 18873 | small helper (bound-functor / handle / vector glue); calls 0x801EA450 | - |
| 0x801EA468 | 96 | fn_801EA468 | 5667 | bind builder (range 1) | - |
| 0x801EA4C8 | 60 | fn_801EA4C8 | 5667 | small helper (bound-functor / handle / vector glue); calls 0x801E8694 | stw r4,0(r3) |
| 0x801EA504 | 64 | fn_801EA504 | 26168 | small helper (bound-functor / handle / vector glue); calls 0x801E86D4 | stb r0,0(r3) |
| 0x801EA544 | 48 | fn_801EA544 | 97550 | small helper (bound-functor / handle / vector glue); calls 0x800091EC | - |
| 0x801EA574 | 96 | fn_801EA574 | 5667 | bind builder (range 2) | - |
| 0x801EA5D4 | 60 | fn_801EA5D4 | 5667 | small helper (bound-functor / handle / vector glue); calls 0x801EA610 | stw r4,0(r3) |
| 0x801EA638 | 96 | fn_801EA638 | 20501 | bind builder (view cone) | - |
| 0x801EA698 | 60 | fn_801EA698 | 20501 | small helper (bound-functor / handle / vector glue); calls 0x801E8694 | stw r4,0(r3) |
| 0x801EA6D4 | 92 | fn_801EA6D4 | 76066 | bind builder (flag filter): {fn, policy byte, mask} | - |
| 0x801EA730 | 60 | fn_801EA730 | 76066 | bind store (flag filter) | stw r4,0(r3) |
| 0x801EA854 | 124 | fn_801EA854 | 46264 | bind builder (same-target) | - |
| 0x801EA8D0 | 60 | fn_801EA8D0 | 46264 | small helper (bound-functor / handle / vector glue); calls 0x801E9DF0 | stw r4,0(r3) |
| 0x801EA90C | 64 | fn_801EA90C | 46264 | small helper (bound-functor / handle / vector glue); calls 0x801E9E30 | stb r0,0(r3) |
| 0x801EA94C | 48 | fn_801EA94C | 46264 | small helper (bound-functor / handle / vector glue); calls 0x8005FEDC | - |
| 0x801EAAA4 | 136 | fn_801EAAA4 | 35691 | bind builder (sphere filter) | - |
| 0x801EAB2C | 60 | fn_801EAB2C | 35691 | small helper (bound-functor / handle / vector glue); calls 0x801E9BBC | stw r4,0(r3) |
| 0x801EAB68 | 92 | fn_801EAB68 | 35691 | bind copy (sphere) | stb r0,0(r3) stfs r0,28(r29) |
| 0x801EB09C | 88 | TargetList__ctor__sub0__sub0 | 1 | small helper (bound-functor / handle / vector glue); calls 0x801EB128 0x801EACBC 0x801EB0F4 0x801E7B1C | stw r3,4(r3) stw r3,0(r3) |
| 0x801EB0F4 | 48 | TargetList__ctor__sub0__sub0__sub2 | 1 | small helper (bound-functor / handle / vector glue); calls 0x801EB124 | - |
| 0x801EB128 | 48 | TargetList__ctor__sub0__sub0__sub0 | 1 | small helper (bound-functor / handle / vector glue); calls 0x801EB158 | - |
| 0x801EB160 | 104 | fn_801EB160 | 945 | small helper (bound-functor / handle / vector glue); calls 0x801E9640 | stw r0,0(r30) |
| 0x801EB230 | 104 | fn_801EB230 | 60915 | remove_copy_if loop (flag instantiation) | stw r0,0(r30) |
| 0x801EB300 | 104 | fn_801EB300 | 29144 | remove_copy_if loop (sphere) | stw r0,0(r30) |
| 0x801EB438 | 108 | fn_801EB438 | 40903 | remove_copy_if loop (view cone, predicate through bctrl) | stw r0,0(r30) |
| 0x802068F8 | 36 | fn_802068F8 | 44640 | weak/shared copy wrapper (copies the 2-word handle at +0xC of the player-target block) | - |
| 0x8020691C | 64 | fn_8020691C | 46268 | handle copy: [r3] = [r4], then count copy 0x8001AEE8 | stw r0,0(r31) |
| 0x803A1334 | 76 | __dl__FPv | 562747 | operator delete | - |
| 0x803A1380 | 140 | __nw__FUl | 565591 | operator new | - |
| 0x8040C110 | 120 | fn_8040C110 | 1881237 | rotation matrix about X (sin/cos through 0x80031568) | stfs r4,0(r31) stfs r2,4(r31) stfs r1,8(r31) stfs r1,12(r31) stfs r3,16(r31) stfs r4,20(r31) stfs r1,24(r31) stfs r1,... |
| 0x8040C188 | 120 | fn_8040C188 | 1840579 | rotation matrix about Y | stfs r4,0(r31) stfs r1,4(r31) stfs r3,8(r31) stfs r1,12(r31) stfs r1,16(r31) stfs r0,20(r31) stfs r1,24(r31) stfs r1,... |
| 0x8040C200 | 120 | fn_8040C200 | 1833937 | rotation matrix about Z | stfs r2,0(r31) stfs r1,4(r31) stfs r1,8(r31) stfs r1,12(r31) stfs r1,16(r31) stfs r4,20(r31) stfs r0,24(r31) stfs r1,... |
| 0x8040C85C | 60 | fn_8040C85C | 1847767 | matrix scale z | - |
| 0x8040C898 | 60 | fn_8040C898 | 1818154 | matrix scale x | - |
| 0x8040C8D4 | 60 | fn_8040C8D4 | 1831237 | matrix scale y | - |
| 0x8040DEC4 | 116 | fn_8040DEC4 | 972861 | Vec3Normalize (owned natively) | stfs r0,0(r31) stfs r0,4(r31) stfs r0,8(r31) |
| 0x8040DF38 | 68 | fn_8040DF38 | 680980 | Vec3 normalise in place (copy, 0x8040DEC4, copy back) | - |
| 0x8040E094 | 104 | fn_8040E094 | 19379 | rotate vec3 about Z by f1 | stfs r0,0(r30) stfs r1,4(r30) stfs r2,8(r30) |
| 0x8040E0FC | 108 | fn_8040E0FC | 378673 | rotate vec3 about the Y axis by f1 (sincos 0x80031568; fmadds) | stfs r0,0(r30) stfs r3,4(r30) stfs r1,8(r30) |
| 0x8040E168 | 104 | fn_8040E168 | 224823 | rotate vec3 about X by f1 | stfs r1,0(r30) stfs r0,4(r30) stfs r1,8(r30) |
| 0x804107B0 | 152 | fn_804107B0 | 84444 | matrix to angles / quaternion helper (used by AimSmooth) | stfs r1,0(r30) stfs r31,4(r30) |
| 0x80410DDC | 304 | fn_80410DDC | 3769 | axis-angle / slerp helper (AimSmooth, 20 % of frames) | stfs r3,0(r31) stfs r0,4(r31) stfs r5,8(r31) stfs r6,12(r31) stfs r4,16(r31) stfs r5,20(r31) stfs r4,24(r31) stfs r6,... |
| 0x803A1380 ... 0x803B2A00 | (about 40 functions, 14 KB) | MSL heap (new 0x803A1380 -> malloc 0x803A3E14 -> pool 0x803A473C / 0x803A4894; delete 0x803A1334 -> free 0x803A3DA4 -> 0x803A4690 / 0x803A46E8) and printf internals (0x803AA738 __pformatter, strchr, strlen: reached from the debug-print path of other users) | per run 562,747 delete / 565,591 new (whole game) | allocator and formatting code of the guest, not part of the tree's own logic; they are the crossings counted in section 6 | pool block headers / free lists |

## 9. Round 11 step 0: the three open items

**(c) 0x8040DDF8 / 0x8040DE20 (P).** They are `Vec3Scale` (r3 = r4 * f1, three fmuls) and `Vec3Add` (r3 = r4 + r5, three fadds),
installed natively by `vec_math` (`extract/native-owners.txt` rows 708-719: 0x8040DDC4 Sub, 0x8040DDF8 Scale, 0x8040DE20 Add, also
0x8040C6CC matrix point transform). The census showed 0 because a call to an owned address is served by the owner thunk before the
census hook is consulted; the generator did not inline them. Consequence: they (and Sub 0x8040DDC4, dot 0x8040DFE0, cross 0x8040E008,
length 0x8040DFA0) are already native and a whole-chain owner calls the same C++ directly.

**(b) The NpcTarget chain 0x802072CC (P).** `0x802072CC` = `0x80206F34` (PlayerTargetBase update), the clock read 0x80051BBC, `0x802074A8`
on the data object at [this+0x18], the clock read again. `0x802074A8` (172 B) has no float arithmetic of its own: it stores three
constants (0, 0, c) to the stack, makes **one guest virtual call**, `[[this+0]]+0x2C` (`bctrl`, out = sp+0x2C, arg = the constant
vec3; the object at [this+0] is the NPC being targeted: Sonic / Tails / ... classes, so the call stays a guest crossing and is not
devirtualisable), copies the result (0x800091EC), then runs: registry singleton 0x80060180, `CreateFromRegistry` 0x801E7C40, the
shared-pointer wrap 0x80207598 (control block allocation), `0x801E83D0(q, [this+0x14])`, `0x801E8448(q, 0x80000)`, `0x802068F8` + `0x801E8158`
(same-target filter), `0x801E879C(out, q, [this+4])` -> `[this+0xC]` (0x8006119C), then the handle / shared-pointer destructors. So per frame
it needs 5 query-layer entries and 1 own guest virtual; all float work is inside the entries' predicates. A query-layer owner serves it
unchanged.

**(a) Heap trace (design; the run needs the window to be closed).** `GEKKO_HEAP_TRACE` (runtime/src/gekko/heap_census.cpp) is a once-per-game-second
census of the OS heaps, not a per-call log, so it cannot prove the allocator order. The proof will be a per-call trace written by the new
code itself: with `GEKKO_S1_TARGET2_ALLOCTRACE=<file>` a declining hook on 0x803A1380 (operator new) and 0x803A1334 (operator delete)
appends `kind, size or pointer, LR` per call (the returned pointer from a second declining hook at the return, or the next call's
state); the run with the switch off and with it on must give byte-identical files. Per PlayerTarget frame the expected sequence
(from the census call sites, P): `new 0x14` (query object, 0x801E7C5C), `new 4n` (vector buffer via 0x800D19C0 / 0x8001BC70), `new 0x14` (control block
0x800B7358), `new 4n` (copy buffer 0x801EA340), then deletes `delete` buffer (0x800198D8, copy dtor), `delete` query (0x801E89B8 via dispose),
`delete` buffer, `delete` control block; the NpcTarget frame has the same shape without the copy.

## 10. Round 11: data layouts and the Stage 1 plan (read during the latency window; no build was made)

**Vector** (the type behind every query; P from 0x800D19C0, 0x8006CEC4, 0x8006CC90, 0x80019A60): `{+0 capacity, +4 size, +8 data pointer}` of 4-byte
elements; the accessors 0x8001990C (= the vector itself: capacity at +0), 0x80019900 (= the allocator argument, the vector), 0x80019904
(= vector + 8) are empty `blr` / `addi` leaves. `reserve(n)` = 0x800D19C0: if n > capacity: max_size test (0x3FFFFFFF, error print + abort path), `new(4n)`
through 0x8001BC70 (r4 = n, shifts to 4n and calls 0x803A1380), store the data pointer, move the old elements (0x800D198C) and `delete` the old
buffer when there was one, store the capacity. `insert(pos, count, value*)` = 0x8006CEC4: when size + count <= capacity it shifts the tail
(0x8006D138 backward copy) and writes the value (the fast path used by the factory after `reserve(N)`); otherwise it doubles the capacity
(max of 1, capacity x 2, needed) and allocates / copies / deletes. A **TargetSearchList** `Q` (20 B) is `{+0 vtable 0x8053ED04, +4 vector: capacity,
+8 size, +0xC data, +0x10 logical end}`; `begin = [Q+0xC]` (0x801E7CF0 -> 0x801E7D10 -> 0x8006D130), `end = begin + 4 * [Q+8]` (0x801E7CB0 -> 0x8006CC90).

**CreateFromRegistry 0x801E7C40 (P):** `new(0x14)`; ctor 0x801E8C0C: vector zero-init (0x801E8BAC -> 0x801E8BDC -> 0x80019A60 sets capacity 0 / size 0 / data 0),
`reserve(N)` where N = the registry list's element count (0x801E7EF0 reads it; one allocation of 4N bytes), then one push_back per registry list node
(0x801E8D18 -> 0x801E8D38 -> 0x8006CC4C -> insert with count 1; the list node value at +8 is the holder pointer, step 0x801E8D04 follows node +4); `[Q+0x10] = end`;
then the prune pass 0x801EA178 (std::remove_if with the plain predicate 0x801E9498 called directly through bctrl from 0x801EA19C / 0x801EB438 on the element value;
0x801E9498 locks the holder and removes the entries whose class word has bit 0 clear), `[Q+0x10] = new end`. Allocations in order: query object (4 + 0x14 byte
request 0x14), then the vector buffer 4N.

**What the staged owner needs (Stage 1 inventory, nothing written yet):** frame models for ~25 functions behind the factory (above), the copy constructor
0x801E8A68 (+ vector assign 0x801EA1F8 -> 0x801EA340 -> 0x8001BC70, one `new(4M)`), the destructor 0x801E8974 (-> 0x801E89D0 -> 0x801E8A1C -> 0x80019890 -> delete),
and the filters (flag A / B done in round 5 and re-usable, same-target 0x801E8158 with binder 0x801EA854 / 0x801EA8D0 / 0x801EA90C / 0x801EA94C and
remove_if 0x801E9D28, sphere 0x801E82C4 with binder 0x801EAAA4 and predicate 0x801E90F0, view cone 0x801E84C0 with binder 0x801EA638 / 0x801EA698 and
predicate 0x801E9324, range 0x801E8528 with two predicates, nearest 0x801E879C). Each is a chain of 10-20 small frames modelled store for
store (the round-5 flag filter is the template: 16 functions, ~250 lines, contract test caught every deviation).

**Ceiling check (the stop rule):** unchanged by step 0. Stage 1 alone covers the entries counted in 1.85 % of the samples (factory 0.56, same-target 0.32,
sphere 0.28, flags 0.24, nearest 0.18, view cone 0.09, copy 0.09, dtor 0.09) minus the guest allocator and the NPC guest virtual that stay; expected
1.0-1.3 % of game-thread CPU, above the 0.5 % floor.

## 11. Round 11 Stage 1: what was built (2026-11-14)

Files: `runtime/include/gekko/native_s1_target2.h`, `runtime/src/gekko/native_s1_target2.cpp` (+ `native_s1_target2_frames.inc` generated by
`tools/re/gen_frames.py`, `native_s1_target2_models.inc` hand-written), test `runtime/tests/s1_target2_contract_tests.cpp`.

**Method.** `gen_frames.py` reads the DOL and, for every straight-line function of the original (no conditional branch, no FP arithmetic), emits a
C++ model that performs exactly the original's memory effects: every store with the values the original stores (saved registers, return addresses,
back chains, locals, stale-word copies) in order, the guest loads it needs, and calls to the models of the functions it calls with the stack
pointer and return address the original would use; arithmetic that never reaches a store or call argument is left to the C++ compiler to delete.
108 functions are generated this way (the query layer's wrappers, binders, invoke chains, the lock / copy helpers, the flag and handle predicates).
Loops, branches and allocator calls are written by hand (about 30 functions, `native_s1_target2_models.inc`), each from its disassembly. Allocator calls
(`operator new` / `delete`) go through the guest functions, as the original's `bl`: r1 = the calling frame, LR = the original's return site, no extra
frame (`gekko::guest_call` pushes a frame of its own and would shift every callee word; `s1_chars::call_guest` is the exact seam).

**Entries served** (hooked only when the switch is on; each call is proven before any write, else the original runs):
`0x801E7C40` CreateFromRegistry, `0x801E8A68` copy constructor, `0x801E8974` destructor, `0x801E83D0` / `0x801E8448` flag filters, `0x801E8158` same-target
filter. Proofs per entry: live holders (use count in [1, 0xFFFFFF00), pn / px non-null, in RAM), vector sizes <= 4096, no word read or written inside
the stack window or the vector / query, a ring of exactly N registry nodes for the factory.

**Contract test** (`s1_target2_contract_tests`, reference interpreter, pinned DOL, the allocator as an identical bump stub on both machines whose call log
and pointer are compared): 13,000 calls, all equal over the registry, nodes, targets, counts, the allocation region, the 0x800-byte stack window with random
stale words, r1-r3, r13-r31, LR, the bump pointer and the allocator log; negative controls caught (missing deep stack word, extra allocation, clobbered
r29, wrong logical end, wrong allocator log, wrong survivor).

**Not yet served (Stage 1b):** the sphere (0x801E82C4), view-cone (0x801E84C0) and range (0x801E8528) filters and the nearest scan (0x801E879C): their predicates
do floating-point work (dot, normalise through the native Vec3Normalize owner, `fcmpo`) and save f31 with `stfd`; the generator needs FPR save / restore
support and the predicates hand models. They are 0.28 + 0.09 + 0.02 + 0.18 % of the samples; the served entries cover about 1.3 % of the 2.47 %.

## 12. Round 11 Stage 1b: the floating-point query entries (2026-11-14)

Sphere 0x801E82C4 (binder 0x801EAAA4, remove_if 0x801E9B1C -> find 0x801E9C14 / copy 0x801E9B7C / remove_copy 0x801EB300, invoke 0x801E9C6C -> 0x801E9CAC, predicate 0x801E90F0:
position - centre, normalised through 0x8040DF38, dotted with the axis, `not greater than f1`), view cone 0x801E84C0 (binder 0x801EA638, remove_if 0x801E96DC, invoke
0x801E97D4 -> 0x801E9814, predicate 0x801E9324: only targets with class bit 1, centre - position normalised, dot with `[target + 0x14]` < `[target + 0x24]`), range 0x801E8528 (pass 1: for_each
0x801E85F8 -> invoke 0x801E8744 with 0x801E94E8, which stores the squared distance into `[target + 4]`; pass 2: remove_if 0x801E9580 -> invoke 0x801E9680 with 0x801E9430: `[target + 4] > f1^2`),
nearest 0x801E879C (strictly smaller squared distance wins, the first on ties; out = weak copy of the winning holder, 0x8005FEDC). All call the Target's slot 2 (0x801E76FC, position copy)
through the vtable. Frames: Normalize 0x8040DEC4 (-> Length 0x8040DFA0 -> 0x8000FEE4 -> sqrt 0x8000FF04, whose 0 / NaN arm stores `f1` at sp + 8), Sub 0x8040DDC4 / Length2 0x8040DF7C / Dot 0x8040DFE0 are
leaves. Gates and counts: docs/agents/s1-target.md (Stage 1b).
