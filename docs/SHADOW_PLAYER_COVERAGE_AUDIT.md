# Shadow player — coverage audit and independent revalidation (2026-11-03)

This is a discovery and revalidation pass. No player function was replaced and nothing was integrated or committed.
Every number below comes from a file in `test_data/player/audit/`, written by `tools/dolrecon/player_audit.py`.
Inputs: main.dol, `skeleton/functions.csv` and `player_census.csv`, with SHA-256s in `player_audit.json`. The
verification runs live in `.build/player/`. Where this audit changes an earlier figure, the earlier figure stays
next to it.

## 0. Summary

The rows are mutually exclusive and add up to the inventory. Every member runs as translated PPC in the game today.

| Category | Functions | Unique PPC instructions | Verification status |
|---|---:|---:|---|
| Complete original player inventory (census members) | 2,541 | 82,045 | membership graded by evidence (§1); 41,737 instructions strictly verified as translated code on two routes |
| Independently verified native | 0 | 0 | none |
| Approved native | 0 | 0 | none (no member is a native owner, §3) |
| Existing but unverified candidates (package) | 323 | 26,520 | 13,077 instructions on the strict routes; the candidates themselves are unverified (§4) |
| Still translated, no candidate | 2,147 | 53,622 | 1,224 functions executed, 923 never executed |
| Unresolved identity (link-order evidence only) | 56 | 1,273 | membership unconfirmed |
| Classed outside the Player namespace | 15 | 630 | HandleUser, TBlockManage, TTextureChange, TPauseManager, TargetSearchList |

Reported separately (these overlap the rows above):
- Behaviour classes not in Shadow's state table: Fly, Hide and FirstMeet (21 functions, 482 instructions), §2.
- Members also called from outside the family ("callers-mixed"): 138 functions, 3,060 instructions. Examples are
  800A8160 and 80094CFC, which are shared with partners.
- Shared engine code the player reaches: 3,549 outside functions are reachable transitively. Of these, 21 are native
  owners hit on Westopolis (CharaColli queries, world collision, motion store, RwMatrixMultiply …), §3.

Answers to the required figures:
1. Original player functions: **2,541**. Strongly evidenced:
   - 709 by Player-family class, without the 21 Fly/Hide/FirstMeet functions;
   - 1,600 because every static caller is a member.

   The rest is weaker (§1).
2. Unique original PPC instructions: **82,045** (328,180 bytes). Ranges are not overlapping, have no padding
   and no duplicates.
3. Player instructions running as translated PPC: **82,045**.
4. Independently verified native player instructions: **0**.
5. Approved native player functionality: **none** inside the inventory. The approved native engine foundations
   (CharaColli, world queries) are shared dependencies, not player members.

## 1. The inventory, recomputed from the binary

- **Ranges.** Every census member's range was read from main.dol: 0 sizes that are not a multiple of 4, 0 overlaps
  with the next function, 0 trailing or interior zero words, 0 duplicate addresses. 2,541 functions × their sizes =
  82,045 instruction words. The earlier "2,541 / 328 KiB" figure is numerically correct.
- **Function identities.** These are functions.csv starts. functions.csv agrees with ppcrecomp's function set on all
  34,037 functions (recon foundation, 2026-11-02).
- **Membership is a rule set, not a proof.** Re-graded per member (`grade` column):

| Grade | Functions | Instructions | Meaning |
|---|---:|---:|---|
| class-player | 730 | 29,156 | method of a Player:: or Player*.cpp class (RTTI / vtable) |
| callers-exclusive | 1,600 | 47,916 | no class; every static caller is a member |
| callers-mixed | 138 | 3,060 | no class; called from members and from outside |
| address-only | 58 | 1,283 | link order between classed members only (2 of them carry a package candidate) |
| class-other | 15 | 630 | classed outside the player namespace |

The census's 1,135 "unit" members are not all weak. Re-graded:
- 939 have only member callers;
- 138 also have outside callers;
- 58 rest on link order alone.

## 2. The 35 behaviour states (binary, not carried over)

`ShadowBehavior` vf1 0x800AE8D0 indexes table 0x80522DE8, which holds 35 records. Each record is a creator, an args
creator, 0 and the inline class name. The table is read straight from main.dol into `player_states.csv`, and it
matches the package's id list. Coverage below is the union of all runs.

| Seen | States (id) |
|---|---|
| Executed, strictly verified on Westopolis | Idle 0, Ground 1, Fall 2, Landing 3, Jump 5, JumpDash 6, HomingAttack 8, HomingJump 9, WallHang 13, Launch 23, AirAttack 26, Damage 30 |
| Executed only in Final Haunt (now strictly verified, §5) | Sliding 7, LightDash 20, PickupWeapon 22, GroundAttack 25 |
| **Never executed by any recording (19)** | Ottotto 4, Throw 10, TurnOver 11, EdgeHang 12, LineHang 14, SomethingHang 15, WallJump 16, Grind 17, SpinDash 18, DarkSpin 19, PickupObject 21, Stream 24, DriveVehicle 27, ChaosBlast 28, ChaosControl 29, Dead 31, ExternalControl 32, ElectricCircuit 33, Super 34 |

**Not in Shadow's table.** Player::Behavior::Fly, Hide and FirstMeet (21 functions) are not in the table, yet all of
them executed. They are behaviours of other characters running the shared Player code. They stay in the census
because of their class, but they are not Shadow behaviours (§0).

**Never-executed functions.** 1,098 functions (33,683 instructions) ran in no recording. By category: movement 219,
action 142, unresolved 138, abilities 125, combat 112, interaction 108, state 90, animation 83, core 41, support 36,
collision 4. **Abilities have 4 of 4,439 instructions verified.** They are practically untested.

## 3. Native status: who actually owns player code

- **No member is a native owner.** That holds both for `extract/native-owners.txt` (223 owners) and for any hook served
  at a member entry on the strict route. The 2,541 functions run translated.
- **Native code the player depends on (approved, default ON, outside the inventory).** Of the 3,549 functions
  reachable transitively, 21 were hit as native owners on strict Westopolis:
  - CharaColliBody / General / GeneralBone vf 0x7C, and the sweep, ray and enemy queries;
  - world collision (0x80443AF4);
  - motion-store clip fetch, effect SetMatrix and RwMatrixMultiply;
  - SE Play3D / PlaySimple;
  - memcpy, PRS expand, `__dynamic_cast`, the 64-bit divide intrinsics, SJ / SVM.

  The strict player runs did not re-verify these owners: they are outside the scope, so their hooks ran once outside
  the oracle. Their own proofs are earlier sessions' (CharaColli all-native exact, world collision Phase IX).
- **D1 0x8041BB8C** (motion core) is not in the census. It is a deeper engine dependency, and it stays unresolved.

## 4. The Unresolved package, revalidated

Per-candidate rows: `test_data/player/package_candidates.csv` (56 rows, each with file:line evidence). Gaps and the
state table: `test_data/player/package_gaps.md`. Two citations were spot-checked against the package (P21_REPORT.md
lines 192-200 and FX_NOTE.md lines 47-53); both match.

- **Reference side.** Every test compares against the package's own Python interpreter (`skinexec/sxfp`). None was
  run on this repo's C++ reference oracle, and none on hardware or Dolphin. Every row therefore passes through stub
  row 35 (estimates computed exactly; see §6 for why that is harmless for sqrt).
- **Stub-affected: 45 of 56 rows.** They run through original-side stubs from `docs/player/UNRESOLVED_PACKAGE_STUBS.md`
  or equivalent ones. PickupObject / Throw's `pickup_orig.py` logs 800A82FC instead of calling it, and its
  dynamic_cast returns its argument.
- **Circular (yes or partial): 14 rows.** The candidate side answered the original side's queries:
  - the world queries come from `world_p3q.GameQueries`, i.e. the package's `sphys_world_*` DLL;
  - lock-on picks come from sphys_p13, and vehicle traits from the native table;
  - motion-finished events are random at 6 % per frame (`parity_all_scenarios.py:77`).

  Affected rows: P5 frame, JumpDash wall, dispatcher / parity_all, F2 carry, FX receive, LightDash, HomingJump /
  Ottotto / Launch, Ground / AirAttack, DarkSpin, TurnOver / TouchHang, DriveVehicle, the ShadowWeapon task,
  Ground-with-weapon, and P22. **The package's "33/35 states at parity" rests on these.**
- **Clean: about 10 rows**, pure kernels tested at function level: pow, normalize, split, integrate, gravity, Fall
  input, the P16 CharaColli queries, the player tick, bullet births and A13 presentation. They still need re-running on
  this repo's oracle before any approval.
- **Inconsistent with itself.**
  - Ottotto's ctor and CanEnter addresses differ between reports.
  - `U1_REPORT.md:320` swaps the Throw and PickupObject ids.
  - P19 says P15 swapped GroundAttack and AirAttack.
  - 8007A6EC is "not ported" in one report and "ported" in another.
- **Result.** No package candidate is independently verified. All 323 member functions they cite (26,520
  instructions) are **NATIVE_CANDIDATE_UNVERIFIED**, and 64 more cited addresses lie outside the census (interior
  addresses or shared engine code).

## 5. Verification coverage: three quantities, never mixed

1. **Confirmed original player instructions:** 82,045 (§1; 76,590 of them strongly evidenced).
2. **Instructions reached and replayed by the strict oracle (translated code = original words):** 41,737 (50.9 %).
   This is the union of:
   - Westopolis all segments: 36,125 instructions, 833 M segments, 8,507 s;
   - Final Haunt, the player's function ranges only: 40,049 instructions, 24.2 M segments, 0 mismatches, 214 s.

   Every instruction any run reached (fast included) is inside this set.
3. **Instructions replaced by independently verified native C++20:** 0.

**Fast vs strict.** Fast mode samples calls, and its CPU budget is wall-clock, so a fast run's coverage is not
reproducible to the instruction:

| Run | Player instructions replayed | Wall |
|---|---:|---:|
| Westopolis strict, all segments | 36,125 | 8,507 s |
| Westopolis strict, player ranges (new targeted mode) | 36,118 | 183 s |
| Westopolis fast, first run | 32,697 | 94 s |
| Westopolis fast, re-run (with arrival counts) | 32,683 | 93 s |
| Final Haunt fast | 36,632 | 91 s |
| Final Haunt strict, player ranges | 40,049 | 214 s |

The range mode misses exactly 7 instructions that all-segments sees: `fn_800AD114`, a leaf the recompiler inlines
into a non-player caller. It is replayed only inside that caller's segment.

**The 3,428-instruction difference.** The first fast run's outputs were overwritten before it recorded arrivals. Its
re-run, with the same policy and the same route (end state identical to strict), differs from strict by **3,442**
instructions, every one classified in `player_coverage_gap.csv`:

| Reason | Instructions |
|---|---:|
| Post-call continuation: the code after a call is never an arrival in fast mode unless that call was sampled (an unsampled call returns inline) | 1,330 |
| Reached and checked, but the checked arrivals took other branches | 1,178 |
| Reached, never checked (sampled out, or every wanted check fell over the CPU budget) | 934 |

- **By status:** translated 1,686, candidate 1,268, unresolved identity 488.
- **By category:** action 877, unresolved 1,155, support 403, movement 309, combat 199, animation 146, core 129,
  state 127, interaction 92, collision 5.
- **None of them is unexecuted code or a missing implementation**: strict replayed all 3,442. None fell in the
  "checked but skipped by the verifier (device / no return)" class: that class counted 0.
- **Indirect ownership.** 11 of the 208 gap functions are members only through data, address or virtual references.
  Their ownership rests on those references, not on direct calls. Strict executed them as members of the route.
- The post-call blind spot is structural in fast mode. Targeted strict over the player's function ranges closes it in
  about 3 minutes per route.

**Paths only Final Haunt reached:** 5,108 instructions in 208 functions. Top functions:
- LightDash update / enter / sub29;
- Sliding update sub0;
- ControlCannon vf1 / sub10 / dtor;
- ShadowWeapon vf11 sub3;
- PickupWeapon enter;
- GroundAttack enter / update.

By category: combat 2,841 and movement 1,319. All are now strictly verified as translated code.

## 6. Numerical audit

- **Estimates (frsqrte / fres / ps_rsqrte / ps_res).** Both engines compute these exactly, so the oracle cannot see
  them. No player member contains one.
  - The only one the player reaches is the game's sqrt 0x8000FF04: frsqrte, then three times
    r = 0.5·r·(3 − x·r²) with fused fnmsub, then frsp(x·r).
  - **Swept exhaustively:** all 2,139,095,039 positive finite single inputs. Seeding it with the runtime's exact model
    or with the Gekko estimate (Dolphin's `FloatUtils.cpp` model, used as a test reference only) gives bit-identical
    results, and both equal `sqrtf`.
  - So the exact-frsqrte modelling does not change this function for single-precision inputs. Double-valued inputs and
    a hardware check of the estimate table remain open.
  - The sweep source is `scratchpad/sqrtsweep/sweep.cpp`. It is not in the repo, because it embeds Dolphin's table.
- **Single-precision multiplies (fmuls, fmadds, ps_mul, ps_madd …).** 263 sites in 106 member functions.
  - Gekko rounds the frC operand's mantissa before multiplying (Dolphin `Interpreter_FPUtils.h:91`, `Force25Bit`,
    applied in fmuls / fmadds / ps_mul / ps_madd).
  - Neither this repo's interpreter (`RefFp`) nor the native ops (`fps_fused`) model that. The oracle compares two
    implementations that share the omission, so it cannot detect it.
  - The rounding only changes a result when frC holds more precision than a single, for example a double-precision
    result fed into a single multiply. Operands loaded with `lfs` or produced by single ops are unaffected.
  - Resolved for every executed path in §12: no executed site ever saw a frC that was not a single.
- **Paired singles.** 402 sites in 124 functions (psq load/store with GQR quantisation, ps arithmetic). Both engines
  model them. This is not independently checked against hardware.
- **Fused double multiply-add.** 0 sites in members. fctiw / fctiwz: 7 sites.
- **Width / endianness / write order.** The PlayerShadow typed view is proven for its 41 evidenced fields
  (`player_view_tests`).
- **Counterexamples.** None found, so no regression case to add. The sweep's result is recorded here.

## 7. Missing logic, named gaps, unresolved identities

Every item below is in the census and translated. None is "missing" from the game; each is missing from any native
reconstruction.

| Gap | Facts (binary + package evidence) | Recordings |
|---|---|---|
| Vacuum weapons (24, 61, 62) | ControlVacuum 8023DBA4 (vt 80548DF4); WeaponVacuumBase 80239988 / 80239DB8; VacuumPod 802264FC; VacuumEgg 80277D6C / 80277A5C. Package: not ported (`env.unported`) | not executed |
| SatelliteLaser (59, 60) | beam spawn 8027B2D4 / 8027B290 → 8027B4A0; the package ported only the request | not executed |
| Homing target search 0x80206B98 | `Player::Control::UserInput` vf3 sub23 sub22; pipeline 801E7C40 … 801E879C; radius filter 801E8528 has no native version | 2,231 calls; 174 of 176 instructions strictly verified |
| Ground 0x10C loop | SetLoopPath Execute 8009C8B8; Ground Event → 80082EE0 path follower; package fails closed | Ground executed; loop path not reached |
| Command 0x11C ApproachControl | Execute 8009B72C → Controller +0x224 → 8009EF70 → 8009F764 (Control state machine) | not executed |
| Command 0x12A SetExternalMotion | Execute 8009AA28 → 800B5380, hand 800B050C | not executed |
| Command 0x133 SetLookAtPosition | Execute 8009A02C → 800B6C4C → 800B7670 | not executed |
| Throw 10 / PickupObject 21 | creators 8008FD40 / 8008C084; slide step 801BFFC8; grab frame 800B1C20 (bctrl 800B1E04 never reached) | not executed |
| Unresolved indirect sites | 800B1C7C, 800B1E04, 800B1FA8, 800B2010, 800B1764, 800B1788, 800B2434, 800B2638, 800B2A1C (the animation hand / attachment code) | partly |
| Animation | D1 8041BB8C (sealed v7 refuses it); D12 Super bank unregistered; D6 / D10 undecided | Super never executed |
| Also not ported in the package | bird controller 802C959C; laser redirect 8023C5E4 / 8023C2B0; drop wall 80221FA4; DarkSpin trigger 800AC2B4 / junction 800AC8DC; Grind rail switching 800811C4 / 8008151C; counter 80079060 | various |

**Unresolved identities.** 56 functions (1,273 instructions) are members only by link order (`grade=address-only` in
`player_inventory.csv`).

## 8. False coverage found and corrected

- **"Player instructions verified" was a translated-code figure.** 36,125 (now 41,737) counts original words replayed
  and matched by translated code. It is not native reconstruction, which stands at 0.
- **"33/35 states at parity" (package) is not independent evidence.** 14 of its tests are circular or partly circular,
  and 45 of 56 are stub-affected (§4). This repo's routes executed 16 of the 35 states.
- **Fast coverage is a sample**, at about 90 % of strict per route, and not reproducible to the instruction. It is
  never a proof.
- **Membership noise.**
  - 15 functions are classed outside the player namespace.
  - 21 are other characters' behaviours.
  - 58 rest on link order alone.
  - 138 are shared with outside callers.

  These are reported, not silently dropped. The 2,541 figure keeps them, so it stays comparable with earlier reports.
- **Unchanged since earlier proof:** nothing player-owned had been approved, so no approved path could have drifted.

## 9. Reconstruction readiness and batch order

Readiness per function: sufficient binary evidence plus a reliable oracle (the strict range mode on a route that
executes it). Missing for the 1,098 never-executed functions: a deterministic scenario that drives them.

1. **01 movement core** (unchanged proposal): CharaColliMove 80094CFC, collide 80097658, sweep-slide 80098ABC,
   80097DB4, the world-result pools and the Ground chain. Every path is executed and strictly verified on both routes,
   and it sits on approved CharaColli and world-query owners. **Blocker to settle first:** the frC rounding question
   (§6) for its fmuls / fmadds sites.
2. **02 executer / dispatcher / behaviour change**: 800A8160, 800A82FC, 800AE8D0. Shared with partners (callers-mixed).
3. **States executed and strictly verified:** the 16 in §2.
4. **Scenarios first, then reconstruction**, for the 19 never-executed states, abilities, vacuum / SatelliteLaser
   and the three commands. Each needs a savestate plus a tape, or a command-injection fixture that reaches it under
   the strict range mode. No reconstruction should start before that route exists.
5. **Animation hand / attachment and D1:** blocked by D1 and by the nine unresolved indirect sites.

## 10. Not done in this pass (stated, not implied)

- **No targeted simulation scenarios were built for unreached behaviours (directive §7).** The 19 states and the
  named gaps have no route in this repo. Building one savestate plus tape per state is the next concrete step.
- **No package candidate was re-run on this repo's oracle.** The ~10 clean kernels are the ones to re-run first.
- **The frC 24-bit rounding question is unresolved.** It needs a hardware-derived reference, or Dolphin's model
  adopted as the spec by decision.

## 11. Reproduce

```
cd tools
python -B -m dolrecon.player_audit ../../source/.build/dolrecon ../test_data/player/player_census.csv \
  ../test_data/player/audit --strict ../.build/player/verify-westopolis-all/verify.csv \
  --strict ../.build/player/strict-ranges-final-haunt/verify.csv --fast ../.build/player/fast-westopolis/verify.csv \
  --fast ../.build/player/fast-final-haunt/verify.csv --owners ../extract/native-owners.txt \
  --candidates ../test_data/player/package_candidates.csv
```

- Targeted strict runs: `STRICT=1 tools/player/run_verify.sh <out> <rec> <stage> .build/player/scope_member_ranges.txt`.
  The scope file's "address,size" lines make every segment inside those functions a candidate.
- The census call graph is `callgraph_fixed.json` (SHA-256 in `player_census.json`), regenerated with
  `dolrecon.callgraph`.

## 12. frC precision at the single-multiply sites (2026-11-03, second pass)

User decision: settle the frC question first, scoped to batch 01, then build routes.

**What the reference says.**
- The Gekko manual, PAGE 82, states that single-precision arithmetic instructions "require all operands to be
  single-precision". A double-precision frC is outside the architecture. The manual gives no rule for it.
- Dolphin's `Force25Bit` (Interpreter_FPUtils.h:91) rounds frC to a 25-bit significand: 24 fraction bits, rounding up
  at fraction bit 27. The Dolphin tree holds no hardware test data for it, and its interpreter and both JITs share one
  implementation, so the rule cannot be validated independently from the sources available here.
- **So the rule was not implemented in the reference interpreter.** It would bake an unvalidated rule into the oracle,
  and no executed site needs it (below). The rule is the identity on any single-representable frC, which is the only
  kind observed.

**Static data flow.** `tools/dolrecon/player_frc.py` writes `test_data/player/audit/player_frc_sites.csv`.
- **Method.** Walk back to every definition of frC:
  - singles: lfs, psq_l, opcode-59 results, paired arithmetic, frsp;
  - small-data constants: read from main.dol and tested for single representability;
  - doubles: opcode-63 arithmetic, lfd from memory;
  - copies are followed to their sources;
  - parameters: resolved through every static caller and the resolved virtual callers;
  - call results: resolved through the callee's returns, including registers the callee passes through untouched;
  - observed indirect callers: taken from the runtime call census and tagged as such.
- **Controls:** `python -B -m dolrecon.test_player_frc` uses synthetic code. A double result, a copy of a double and
  a wide constant are flagged; a single load, frsp and a single constant are proven. All seven cases pass.
- **Whole game:** 7,609 of 8,001 single-multiply sites are proven single, and no site anywhere has a double, wide
  constant or memory definition of frC. The game's compiled code never feeds a double into a single multiply where the
  data flow can see it.
- **Player members:** 263 sites, classified as follows.

| Verdict | Sites | Note |
|---|---:|---|
| PROVEN UNAFFECTED (static) | 220 | 118 of them executed under the probe |
| UNRESOLVED, single on every observed caller | 15 | virtual-method parameters (behaviour updates) |
| UNRESOLVED | 28 | 5 executed under the probe |
| AFFECTED | 0 | |

**Dynamic probe.**
- **What it does.** `Cpu::note_single_multiply_frc` runs in the reference interpreter and only observes. It counts,
  per instruction, replayed executions whose frC is not a single, and of those the ones that 25-bit rounding would
  change.
- **Control:** a core test (`frC probe classifies single multiply operands`) checks 0.5 (single), 0.1 (wide and
  changed), 1+2⁻²⁴ (wide but unchanged) and fmadds.
- **Strict runs over the player's ranges, Westopolis and Final Haunt (20.8 M and 24.2 M segments):** 0 wide
  executions.
- **Strict runs over the player plus the batch 01 dependencies (45.1 M and 44.8 M segments, 0 mismatches):** 0 wide
  executions.
- **Negative control:** the corrupt fixture at 0x80083A38 is still caught (0xFB vs 0xFA) under the same scope.

**Batch 01 (CharaColliMove 80094CFC, collide 80097658, sweep-slide 80098ABC, 80097DB4, pools 80021E24 / 80021FE4 /
800201D4, Ground 80083A38 / 8008331C, plus direct callees: 86 sites).**
- 75 are proven.
- 6 are in the vector-scale helpers 0x8040DDF8 / 0x8040DE88. Their 240 engine-wide callers keep them open. Batch 01's
  own 21 calls into them all pass a single: 20 statically, and 1 through the executer's observed virtual call
  0x800A81F4 into Ground::update. The probe saw them execute: 0 wide.
- 1 core site (0x800838D0) is single on its observed caller, and probed with 0 wide.
- **3 sites were never executed on either route** and stay tracked batch 01 dependencies: 0x800202A0, 0x80083858 and
  0x8008322C.
- **Batch 01 has no frC rounding dependency on any executed path.**

**Owner-contract findings (strict scope with the batch 01 dependencies).**
- Two approved native CharaColli owners mismatched the original words on registers the caller does not use:
  - 0x800C615C (enemy query): r4, native 0x8060C4D4 vs 0x2000.
  - 0x800C5D20 (ray query): f1.
- **0x800C615C.** `tools/dolrecon/player_r4dead.py` proves r4 dead at both callers. It uses forward liveness through
  branches and callees, and fails closed on indirect branches. Scope lines can carry `,r4dead` / `,f1dead`, and only
  those entries' owner passes skip that register (`Cpu::set_verify_r4_dead`). 805 of 2,644 scoped entries are r4-dead.
- **0x800C5D20's f1 could not be proven dead.** A virtual destructor call (`bctrl` 0x80017498) lies on the path, and
  this pass does not claim it.
- Both owners were therefore left out of the probe scope. The game runs their native C++, not those PPC words, so their
  frC question is the native code's, not the translated code's.
- These are contract findings about approved engine owners, not player defects. They are reported for the engine
  owners' own audit.
