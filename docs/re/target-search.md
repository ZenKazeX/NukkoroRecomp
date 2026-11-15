# Target search (0x801E7000-0x801EB000): map and owner decision (S1 `weapons`, round 3, 2026-11-12)

Claim: `s1-claims/weapons.txt` line "0x801E7000 0x801EB000 target-search-manage-list" (no other claim and no native owner
covers the range; checked in every `../wt-*/extract/native-owners.txt`). The player field is a caller only. Names are ours;
class names and vtables come from the game's RTTI (dolrecon class table), everything else from the disassembly of the pinned
PAL DOL (`tools/doldis.py`). The external decomp was not used. Evidence: P proven, I inferred, U unknown.

**Round 3 decision (section 6): no owner for the block as a whole; round 5 (section 8) owns the two flag-filter entries as call trees.** Self time of the whole block is 1.35 % of the game thread, but it is about 100 tiny
template functions (largest row 0.06 %), not a tight loop; the part that is native-able is far below what dispatch and the
guest-visible heap / reference-count traffic would eat. Measured numbers in section 5.

## 1. What it really is

Round 2 called this "target search". The code shows more precisely: a **registry of Target objects plus a query builder**.
There is no per-frame search pass; each caller builds a one-shot query object, narrows it with filter steps and asks for the
nearest survivor. The per-frame `TargetManageTask::update` is only a debug marker drawer.

| class (RTTI) | vtable | size | role | ev |
|---|---|---|---|---|
| `Target` | 0x8053ECB0 | 56 | one registered target: position/aim data, owner reference, flag word | P |
| `TargetManageTask` (Task) | 0x8053ECF4 | 40 | per-frame task; update 0x801E7A08 | P |
| `TargetSearchList` | 0x8053ED04 | 20 | one-shot query: vtable, copy of the candidate vector, logical end pointer | P |
| `TargetList` | 0x8053ED10 | - | the registry (global singleton), a vector of `shared_ptr<Target>` | P |
| `sp_counted_base_impl<Target*, void(*)(Target*)>` | 0x8053ECCC | 20 | control block of Target (function-pointer deleter; dispose 0x801E7934) | P |
| `sp_counted_base_impl<TargetSearchList*, checked_deleter>` | 0x8051FE84 | 20 | control block of a query | P |

## 2. Structures

### Target (56 bytes, ctor 0x801E77A0 / 0x801E783C, dtor 0x801E7744; P from the ctor)
| off | field |
|---|---|
| +0x00 | vtable 0x8053ECB0 (slots 0x801E76FC = copy position (vec copy of `this+8`), 0x801E7744 dtor) |
| +0x08..+0x24 | floats initialised to two constants (position / aim vectors and radii; +0x08 is the position read by slot 0, I) |
| +0x28 | weak reference to the owner object (initialised by the 0x800520B0 smart-pointer ctor) |
| +0x30 | **u32 target-class flag word** (ctor param r5). ctor sets bit 0; if (flags & 0x1C08) sets 0x2000; if (flags & 0x000F8000) == 0 sets 0x8000 |
Callers pass masks such as 0x1800 / 0x2000 to the filter below; the mask is tested against +0x30 (P, predicate 0x801E926C).

### Registry singleton (`TargetList`, accessor 0x80060180, static at 0x80576F54, flag r13+0x2E59/r13+0x2DA0; P)
+0x00 byte: debug-draw enable; +0x04 sub-object; +0x08 vector begin (`0x8006D130` returns `*(this+8)`), +0x0C end/size
(`0x8006CC90`: begin + count*4), elements are 4-byte `shared_ptr` pointers with the count pointer in a parallel array (I:
the vector holds the raw pointers and the control blocks side by side, 8-byte steps appear in the node copy at 0x801E7A6C).

### TargetSearchList (20 bytes)
+0x00 vtable 0x8053ED04; +0x04 vector (begin at +0x08, see 0x801E7CF0 = begin, 0x801E7CB0 = end through 0x8006CC90);
**+0x10 logical end** (`remove_if` result, stored after every filter step). ctors: 0x801E8C0C (copy all targets of the registry
into a fresh vector, 0x801E8D58 copy-construct each `shared_ptr`), 0x801E8A68 (copy of another query's [begin, end)),
factory 0x801E7C40 (`operator new(0x14)`, ctor, then 0x801EA178 initial `remove_if`-style prune with predicate 0x801E9498).

## 3. Entry points (the query builder; P from the bodies)

Each filter step has the same shape: build a bound predicate object on the stack (`boost::bind`-style, byte r13+0x4608 or
r13+0x4618 is a policy flag captured into it), then `this->end = remove_if(begin, this->end, predicate)` using a
predicate-type-specific copy of the algorithm (0x801E9580, 0x801E96DC, 0x801E9864, 0x801E9B1C ...).

| entry | args | filter | ev |
|---|---|---|---|
| 0x801E83D0 | (query, mask) | KeepFlagsAny: removes targets with no bit of mask in Target+0x30 (predicate 0x801E926C returns (flags & mask) == 0) | P |
| 0x801E8448 | (query, mask) | KeepFlagsNone: same frame, predicate 0x801E92CC returns (flags & mask) != 0, so it removes targets WITH a bit of mask (round 5 correction: the two predicates are not twins, the second does not invert) | P |
| 0x801E84C0 | (query, vec3*) | keep within a vector criterion (predicate 0x801E9324, vec copy 0x800091EC) | I |
| 0x801E82C4 | (query, centre vec3*, radius f1, second vec3*) | sphere filter: predicate bound with 0x801E90F0 | I |
| 0x801E8528 | (query, centre vec3*, f1 = limit) | distance filter against f1*f1 | I |
| 0x801E8158 / 0x801E81D8 / 0x801E8228 | further bound filters (angle / owner) | I |
| **0x801E879C** | (out shared_ptr, query, point vec3*) | **nearest**: first candidate defines the running minimum (its position via the candidate's owner object `vtable+8(out, owner, point)`, `Vec3Sub` 0x8040DDC4, squared length 0x8040DF7C); then scans the rest, replacing the best when `d2_new < d2_best` (strict, `fcmpo`, ties keep the first; a null owner gives the large constant at r2-0x3654); returns the winner by `shared_ptr` copy 0x8005FEDC | P |
| 0x801E7A08 | TargetManageTask update | if registry byte +0 set: for each target, draw a white marker (colour 0xFF/0xFF/0xFF, alpha 0x80) at its position through the debug-draw call 0x802E1494; otherwise nothing | P |

Call sites of 0x801E83D0 (11): 0x8005FDEC (weapon lock-on, mask 0x1800), 0x800B6F9C and 0x800B708C (mask 0x2000), 0x802069AC /
0x80206BE0 / 0x8020751C, 0x80239AA8, 0x8023C360, 0x80295EA0, 0x803414C4, 0x803493D8: Shadow player target, LightDash, NPC and
boss-Sonic targets, lock-on weapons, a magnet effect. A typical query (0x8005FDA0): construct -> mask filter -> sphere filter
-> distance filter -> nearest -> destroy.

## 4. Per-query cost anatomy (I, from the code)
1. `operator new(0x14)` (guest allocator `__nw` 0x803A1380) and a vector allocation.
2. Copy N `shared_ptr<Target>` out of the registry (N reference-count increments, guest memory writes).
3. K filter passes; each visits the surviving candidates, locks the target's owner (`weak_ptr` lock 0x800578A0 builds a
   temporary `shared_ptr`: count increment, then dispose 0x80057850 decrement) and tests it.
4. Nearest scan: one virtual call per candidate (position accessor of the owner object, outside this range) plus
   `Vec3Sub` and squared length.
5. Destruction: N decrements, vector free, query free (guest allocator).

## 5. Measurement (base profile `profile-base-700f72d.json`, 11,230 game-thread samples, self time)
* Block 0x801E7000-0x801EB800, every sampled function: **1.37 %** self, 87 functions; largest rows 0x801E926C 0.06,
  0x801E8C0C 0.05, 0x801E92CC 0.05, 0x801E86D4 0.05, 0x801E879C 0.04, 0x801E7D10 0.04; the median row is 0.01 %.
  (Round 2's 0.74 % counted only the rows the S2 table labelled unknown; the rest of the block is labelled shared game code.)
* Callees outside the block that these queries use, self % from the same profile (shared with other users, so upper bounds):
  weak lock 0x800578A0 0.07, 0x8005793C 0.04, shared dispose 0x80057850 0.06, 0x8008A310 0.02, operator new 0x803A1380 0.05,
  Vec3Sub 0x8040DDC4 0.03, vec copy 0x800091EC 0.35 (generic; only part is this block's).
* **Inclusive estimate: about 1.5 % of the game thread (1.35 block + at most 0.25 of shared helpers).** The sampler has no stack
  walk, so this is a sum of rows, not a measured inclusive; there is no per-function call census for this range in
  `test_data/player` (checked: it covers player action words, not this registry).

## 6. Why it does not pay (decision)
1. **Not a loop with arithmetic to speed up.** The only arithmetic is, per candidate, one virtual call, a Vec3Sub, a squared
   length and one compare. Everything else is `shared_ptr` / vector / bind-functor template code spread over about 100 functions
   of 5-60 instructions each (rows of 0.01-0.06 %). OPT_2026-11-12.md measured exactly this shape (leaf owners that redo lifted
   code with the same helpers): exact, no faster, because dispatch eats the gain.
2. **What must stay guest-visible cannot be shortened.** The allocator calls (`operator new` / delete are guest functions whose
   call order fixes heap addresses and residue that `state.json` hashes), every reference-count write (guest memory), and the
   per-candidate virtual call into enemy/player owner objects (guest code outside this range). A native owner would call all of
   them anyway through `guest_call`, so only the glue between them is replaceable.
3. **Dead-stack residue.** The query path nests 5-8 frames of tiny functions that all leave saved registers / link words below
   the stack pointer; the ADX owner needed exact dead-stack words to keep `state.json` byte-identical
   (docs/agents/opt-adx.md). Matching that for ~100 distinct frames is a large exactness cost with no way to prove completeness
   from one tape.
4. **Best-case gain below the noise floor.** Replacing only the glue (about half of 1.35 %) at the 40-45 % saving seen in the
   Mtn whole-loop owners gives at most 0.3 % of the game thread, under the 0.5 % run-to-run spread of the benchmark. Per the
   contract ("no gain = no owner") the build, contract test and gates were not started.
5. 12 distinct entry points / predicate types would each need their own contract test with negative controls.

Therefore: no build in this worktree, no owner, no shared-list change, no liveness-list regeneration. The map above is the
deliverable. A future owner would have to start from the filter-step family (0x801E83D0 / 0x801E82C4 / 0x801E8528 plus
`remove_if` copies 0x801E9580 / 0x801E96DC / 0x801E9864 / 0x801E9B1C) together with the allocator contract; the entry that matters
for gameplay exactness is the nearest scan 0x801E879C (strict-less, first wins, null owner = large constant).

## 7. Open questions
* U: exact predicate semantics of 0x801E92CC, 0x801E9324, 0x801E90F0, 0x801E8158/81D8/8228 (only their shapes were read).
* U: number of registered targets and queries per frame in Westopolis (would need a call census; not available without a run
  with a guest-call counter).
* U: the meaning of the Target float block +0x08..+0x24 beyond the position at +0x08.

## 8. Round 5 (2026-11-12): owner for the two flag filters, as whole call trees

Files: `runtime/include/gekko/native_s1_weapons_target.h`, `runtime/src/gekko/native_s1_weapons_target.cpp`, test
`runtime/tests/s1_weapons_target_contract_tests.cpp`. Entries 0x801E83D0 / 0x801E8448 are hooked (`install_hle`), the original
bodies stay for the hand-back.

Round 3 judged an owner of the block too small. What changed the judgement: the quiet-window result of amendment 5 (owners of ~1 %
inclusive pay when timed on game-thread CPU), and the discovery while reading the stores that the whole filter is regular enough to
reproduce store for store: a flag filter is `remove_if` over 4-byte entries (the address of a holder {px, pn}) with a bound
predicate; every predicate call runs the same lock / read / release chain of 12 small functions whose only memory effects are
stack words (the counts end where they started).

Served call = the whole tree, in the original's order: the bound-predicate build (0x801EA6D4 / 0x801EA780 / 0x801EA730), the
begin accessor (0x801E7CF0 / 0x801E7D10), `remove_if` (0x801E9864: find 0x801E98CC, then copy 0x801EB230), per entry the invoke chain
(0x801E9924 / 0x801E9964), the predicate (0x801E926C or 0x801E92CC) and the lock / copy / release helpers (0x800578A0, 0x80057990,
0x8005793C, 0x80016838, 0x80016914, 0x80057810, 0x80057850, 0x80009408, 0x8000945C), then the store of the new logical end. The
owner writes every stack word those frames write (including the stale words the original copies: the half-written policy word of the
bound predicate), the compacted vector and `[query + 0x10]`; the use counts end where they started, so they are not written. The
original returns the new end in r3.

Hand-back (before any write; the proof pass is read-only): an entry whose holder is not word aligned or outside RAM, whose px / pn
is null or outside RAM, whose use count is 0 (expired: the original's predicate then reads the word at address 0x30) or >= 0xFFFFFF00,
a vector that is unaligned / reversed / longer than 4096 entries, any word the call reads or writes that overlaps the stack window
(0x400 below r1) or the vector or the query.

Gates (full numbers in docs/agents/s1-weapons.md): contract test PASS, 10,600 calls (4,000 live: all served; 3,000 with expired / null / huge-count entries: 512 served, 2,488
declined, same memory either way; 1,500 with wild pointers: all served), compared over the query, the vector, all holders, count
blocks, targets, a 0x600-byte stack window with random stale words, r1 r2 r3 r13 r27..r31 LR; negative controls caught: wrong
survivor 296, wrong new end 400, missing deep stack word 400, other variant's constants 400, flipped mask 400, clobbered r29 100.
Switches: run time `GEKKO_S1_TARGET=0/1`, `GEKKO_S1_TARGET_STATS=1`; build time CMake option `SHADOW_S1_TARGET` (default OFF).
