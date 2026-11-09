# Shadow player — native reconstruction

Status 2026-11-03: **discovery + plan only. No player code reconstructed, integrated or approved in this pass.**
Directive: the user's "Complete Native Player System Reconstruction" (2026-11-03). Rules it inherits:
NATIVE_MATH_METHOD.md (bit identity, no tolerance), NORTH_STAR_COMPACT.md (no wrappers/shims/fake MEM1),
NATIVE_COVERAGE_AUDIT.md (report the audit's numbers only), GAME_ERRATA.md (behaviour fixes need the user).

Evidence labels used below: **BIN** = read from the DOL; **ORACLE** = compared bit-for-bit against the original
words in an interpreter; **AGENT** = claimed by an earlier agent and not re-checked here; **HYP** = hypothesis.

---

## 0. Decisions in force (user, 2026-11-03)

**Boundary.** Option A was approved. Native player code uses the existing player object through a validated typed view (`runtime/include/gekko/player_view.h`):
- exact layout, alignment, aliasing and observable write order;
- no second authoritative player state.

The view is temporary. It is retired when no subsystem still reads the guest object.

**Linkage.**
- The final native Player and Physics code is compiled into the main Shadow executable, as C++20 sources or static libraries (`.lib`).
- No new Player or Physics DLL, no dynamic loading, no unnecessary ABI boundary.
- The code stays modular, independently testable and portable.
- The package DLLs in `H:\Unresolved Shadow functions` (sphys.dll, sanim.dll, i1_player.dll) may remain as research and comparison references only. They are never a runtime dependency.
- A working algorithm is not rewritten only to change its linkage.
- Removing a DLL removes no PowerPC dependency. Every execution path is still reconstructed and validated on its own.

**Trust nothing blindly.** This applies to package code, repository code, documentation, test results, names, and every earlier conclusion, whichever agent or model produced it. Before anything is reused or approved:
- trace it to binary evidence;
- verify identities, layouts, offsets, calling conventions and side effects;
- compare it against this repository's C++ reference oracle;
- audit its tests for stubs, substituted or shared implementations, circular validation and unexecuted paths;
- require positive and negative controls;
- explain every difference, never tolerate one away.

Anything without sufficient evidence is marked **UNVERIFIED**. A previous approval does not exempt code from regression testing when its dependencies or integration context change.

**Package status.** The `H:\Unresolved Shadow functions` package is approved for investigation and experimental reconstruction only. Its original-side stubs are listed in `docs/player/UNRESOLVED_PACKAGE_STUBS.md`. Nothing that passes through a stub counts as verified coverage.

**Final target.** One native Shadow executable containing Player, Physics, Animation and Gameplay, with no mandatory Player/Physics DLL and no translated PowerPC dependency.

## 1. Scope

The player = everything the game runs for the controlled character: the `Player::` class tree
(`source/.build/dolrecon/skeleton/classes/Player/`, 202 headers) minus `Player::Npc*`, plus `ShadowWeapon`,
`ControlGun`, `TargetSearchList`, `HandleUser` (rule of `source/.build/dolrecon/gameplay_scope/game_map.py`).

| Family (game_map.py, full call graph) | Functions | Code | Notes |
|---|---|---|---|
| player: Shadow | 2,544 (GAME_MAP.md table: 2,498) | 328 KB (table: 326 KB) | the two numbers disagree; recount in batch 00 |
| partners / NPC (`Player::Npc*`) | 1,848 | 208 KB | out of scope for "player"; own later programme |
| collision, ranges, physics (CharaColli, Range*) | 662 | 114 KB | SHARED_ENGINE_DEPENDENCY, partly native already |

Older counts: `inventory.py` (direct calls only, superseded) 2,098 fns / 300 KB; ATLAS
(`H:\Unresolved Shadow functions\findings-20261019-ATLAS\ATLAS.md`) 78,861 instructions in its player family.
Lift status of the 2,098 (`source/.build/dolrecon/lv3-merged.csv`): 1,353 lift-verified, 745 not.

Native in the running game today: **0 player-owned functions** (nothing owns 0x800A8160, 0x800A82FC, 0x80083A38,
0x8008331C, 0x80097658). Player-adjacent shared code that is already native (section 2.2).

## 2. Existing implementations

### 2.1 `H:\Unresolved Shadow functions` (offline package, 2026-10-13..20)

A complete *candidate* native player exists there as standalone DLLs. Product builds (HANDOFF_COMPACT.md):
`native-phys/build/sphys.dll` 40c84460, `integration/i1_player/out/i1_player.dll` 2101910f,
`native-anim/build/sanim.dll` af6eb382; frozen sources `findings-20261019-V5/stage_prom`. ~26k lines of C++.

State representation: `native-phys/p5_behaviour/include/sphys_player.h` — "Semantic state only: no guest pointers,
no object layouts, no console byte order". One dispatcher, one executer frame, one receive path. This matches the
directive's target shape and is the main reuse candidate.

| Module | Covers | Main VAs | Validation (AGENT) |
|---|---|---|---|
| p3_move | gravity, drag integration, collide-and-slide, ground snap, platform carry | 800A8160 order, 800A7AE8, 80097658, 80098ABC, 80094CFC→80094E88/80095298 | 14 kernels, 1,500 collision cases |
| p5_behaviour | ids 0 Idle 1 Ground 2 Fall 3 Landing 5 Jump 6 JumpDash 8 HomingAttack; dispatcher | 800A82FC, 800A8160, 80085628, 80085FE0 | 36,694 fuzz frames |
| p9_behaviour2 | 7 Sliding 12 EdgeHang 13 WallHang 16 WallJump 17 Grind 18 SpinDash 28 ChaosBlast 30 Damage 31 Dead | factory 800AE8D0, table 80522DE8, 80081360 | 10,600 fuzz frames, 14 controls |
| p13_lightdash | 20 LightDash, rings, lock-on | 80089D64, 80089B90, 800B6E38 | 10,800 frames |
| p15_behaviour3 | 4 Ottotto 9 HomingJump 14 LineHang 23 Launch 24 Stream 29 ChaosControl; gauges, routes | 8007BB8C.., 8016F4B0/F55C/F498/F478 | 16,950 frames, 17 controls |
| p19_behaviour4 | 11 TurnOver 15 TouchHang 19 DarkSpin 25 GroundAttack 26 AirAttack | P9 §1 vtables | 3,420 frames |
| p20_behaviour5 | 22 PickupWeapon 27 DriveVehicle 32 ExternalControl 33 ElectricCircuit 34 Super | 800AF924→800A7CE4 | 2,696 + 13 rides |
| p14_gadget2 | 10 Throw 21 PickupObject | — | ids never reached in parity_all |
| p21_weapon | ShadowWeapon task | vt 80548C44, 8023B6D0 | 1,296 shots 0 ulp |
| p22_control | command mailbox, AccelerateInput, jump boards, dash panels | 801DA2DC, 801DA5A4 | 24,480 cases |
| F1/F3/IT/IN/TM findings | executer pre-steps, gauge-full / time stop, items, rings, lives, barrier, input | 800AF868.., 800A51F0.., 8009B898 | 9,656 cases |
| native-anim a8_player | ShadowMotion pipeline | 800B5708→800B4BEC, 800AA7EC, 800AA4BC | 929,280 pose floats |
| i1_player / u1_dispatch | the whole loop, ids 0–34 | scheduler order | 64,000/64,000 frames; T3 223,200 frames on 6 stages |

How it was validated: the original words ran in the **Python** FP interpreter (`skinexec/sxfp.py`) on MEM1 snapshots,
native through ctypes, float32 bit compare, wrong-variant controls. No tolerance except NaN class.

What it is **not**:
- **Not approved.** No user approval line found. User decision 2026-10-14 17:55: "DON'T MERGE YET"
  (PROJECT_STATE.md l.79). Every "done" is the agents' own.
- **Not integrated.** 0 % live in the game (HANDOFF_20261014_WAVE8.md l.44).
- **Not on the main repo's oracle.** The C++ reference interpreter and `--verify-*` in-game replays were never run.
- Known gaps/debts: vacuum weapons (24/61/62), SatelliteLaser 8027B4A0, homing search 80206B98, Ground 0x10C loop,
  commands 0x11C/0x12A/0x133 unowned, ids 10/21 never reached, original-side stubs (pose history 80078E68, gauge add
  8016F4B0 in orig_loop, contact slot, ledge probe), intra-frame command order = HYP, frsqrte modelled as exact
  1/sqrt (deviation), x86 NaN sign, P10 PAST_END clamp (deviation), v7 splice SCAFFOLDING (user decision D1 open).
- ATLAS says 55 % of its player family (42.7k instr) is not covered by any module (PD_REPORT.md: partner NPC 19.4k,
  input layer 3.6k, …).

Classification: **experimental reconstruction, strong evidence, unapproved.** Reuse as the candidate source for each
batch; re-prove under the main repo's oracle before it may own anything.

### 2.2 Main repo (`wt-gc-removal/runtime`) — native, ON by default, shared with the player

| Code | Owns | Validation |
|---|---|---|
| `src/gekko/chara_colli_intrinsic.cpp` | CharaColli body/general ray+sweep 800C15A0/800C1108/800C31EC/800C2854, update vf 0x88, ground filter 80099454 | 8,430,465 calls 0 diff (CPU_FINAL_HAUNT.md) |
| `src/gekko/chara_colli_bone.cpp` | CharaColliGeneralBone 800C3D6C.. | in-game verify |
| `src/gekko/native_world_collision.cpp`, `native_world_line.cpp` | line pick (80443AF4/804421D8), sphere query 800215C0 | 1,149,567 calls / 15 routes 0 diff; pooled result objects (80021678 tail, 800201D4, 80021E24/80021FE4) left "to go native with their consumers" = the player |
| `src/gekko/gameplay_scheduler.cpp` | task traversal 8004ECAC | task bodies translated |
| `src/gekko/native_motion_store.cpp` | clip fetch/pool 80422234.. | motion player 80416EF4 still translated |

These are engine owners that still read guest memory (ppc_ops in guest order). They are the physics foundation the
player must call; the player must not get a second collision implementation.

## 3. Player graph (entry points, BIN + skeleton)

Per frame (I1 §1.1, AGENT; matches the scheduler): Controller 8009EFF8 → Command task 801DA0A0/801DA5A4 →
CharaColli → Shadow task: CharaColliMove::update 80094CFC → ShadowExecuter::update 800AFD28 → executer step
800A8160 (timers 80079098, frame 800A7AE8, input clamp 800A81A4, behaviour Update vt+0x1C, align 800A78F4, end
800A827C) → ShadowMotion 800B5708 → MotionEye.

- Behaviour change 800A82FC (130 callers, 44 outside the family): factory→CanEnter→Leave→Enter.
- Factory ShadowBehavior::vf1 800AE8D0, table 80522DE8 (35 ids; super form remaps 0/2/3 → 34).
- Receive 800AF924 → behaviour Event → base 800A7CE4 → command Execute.
- Behaviour vtable: 1 CanEnter 2 Enter 3 Leave 4 Event 5 Update; base vtable 80521150.
- Hot: Ground::update 80083A38 → 8008331C → 80097658 = 13.4 % of the Final Haunt game thread; 800A8160 subtree
  10.9–13.2 %.

Objects: PlayerShadow (new 0x29C, vt 80522D70, ctor 800AE378), ShadowExecuter (0x58, vt 80522FA0, current at +0x30,
factory +0x38), ShadowMotion (0x868), ShadowWeapon (0x60), CharaColliMove (0x30). Field map of PlayerShadow: P3 §1 /
P5 §1 / I1 §2.2 in the package (AGENT; e.g. +0x00 pos, +0x18 vel, +0xA8 state bits, +0x228 CharaColliMove, +0x230
motion, +0x238 weapon, +0x27C invulnerability). Two conflicts to settle: +0x98 (carry vs cooldown) and +0x238 (weapon
vs P5's special-surface branch).

Classification (directive §3) of the families: PLAYER_CORE (PlayerShadow, Executer, Factory), PLAYER_MOVEMENT
(CharaColliMove, Ground/Fall/Jump chain), PLAYER_ACTION (52 `*Command` classes, behaviours), PLAYER_ANIMATION
(ShadowMotion, MotionEye/Hand/Sub), PLAYER_COMBAT (ShadowWeapon, ControlWeaponBase, unnamed weapon units), PLAYER_ABILITIES
(ChaosControl/Blast, gauges, Super), PLAYER_INTERACTION (Pickup*, DriveVehicle, ElectricCircuit, items/rings),
PLAYER_STATE (Barrier, Effect, PostManager, Watch), SHARED_ENGINE_DEPENDENCY (CharaColli*, world queries, motion
player 80416EF4, Task, CharCommandManager, RW, sound), UNRESOLVED (helper bucket ~147 KB in inventory.py).

## 4. The boundary problem (decision needed)

The player object is read by translated code outside the family (camera, enemies, HUD, gadgets, missions: 44 outside
callers of 800A82FC alone). While those stay translated, the player's state must also exist in the guest layout.
Options:
- **A. Typed view (transitional).** Native player algorithms operate on a typed C++ view of the guest object
  (`PlayerShadowView` with named fields, byte order handled once). Owners replace VAs one batch at a time; outside
  readers keep working. Counts as transitional; removed when the last outside reader is native.
- **B. Semantic state + mirror.** Run the package's `sphys_player` semantic state natively and copy the fields
  outside readers need into the guest object each frame. Faster to the end state, but needs a complete reader census
  first and the copy is itself a bridge.
Recommendation: **A** for batches 01–08 (keeps every batch independently A/B-able in the real game), switching to
semantic-only storage in batch 14 once the census shows no outside readers.

## 5. Batch plan (provisional, ordered by dependency)

| # | Batch | Entry VAs | Reuse | Depends on |
|---|---|---|---|---|
| 00 | Infrastructure: per-function executed-set census for the player family; C++ oracle harness for player functions (reference interpreter, MEM1 snapshot from savestate, paired native); `--verify-player` in-game mode; reconcile counts; field map | — | runtime/tests/native_*_tests pattern, `GEKKO_CALL_CENSUS` | — |
| 01 | Movement core: CharaColliMove update + collide/slide/snap/carry + world result pools | 80094CFC, 80097658, 80098ABC, 80097DB4, 80021E24/80021FE4, 800201D4 | P3, native CharaColli + world queries | 00 |
| 02 | Executer frame + dispatcher + receive: timers, orientation frame, gravity, input clamp, behaviour change, factory, receive | 800A8160, 800A7AE8, 800A82FC, 800AE8D0, 800AF924, 800A7CE4 | P5/U1 | 01 |
| 03 | Input + commands: Controller, UserInput, command mailbox, AccelerateInput | 8009EFF8, 801DA2DC, 801DA5A4 | P22, IN | 02 |
| 04 | Ground behaviours: Idle, Ground (incl. 0x10C loop), Landing, Sliding, Ottotto, TurnOver | 80083A38, 8008331C | P5, P9, P15, P19 | 02 |
| 05 | Air: Fall, Jump, JumpDash, HomingAttack (+ search 80206B98), HomingJump, AirAttack, GroundAttack | 80085628, 80085FE0 | P5, P15, P19 | 04 |
| 06 | Traversal: Grind + rails, LightDash, LineHang, Stream, Launch, hangs, WallJump, SpinDash | 80081360, 80089D64, 8001C91C | P9, P13, P15 | 04 |
| 07 | Animation control: ShadowMotion, event pump, command table | 800B5708, 800AA7EC, 800AA4BC | a8_player (needs D1) | 02; motion player 80416EF4 stays shared |
| 08 | Combat: ShadowWeapon, ControlWeaponBase, all weapon units incl. vacuum + SatelliteLaser, Throw, Pickup* | 8023B6D0, 8027B4A0 | P21, P14, P20 | 02, 07 |
| 09 | Abilities + gauges: ChaosControl, ChaosBlast, DarkSpin, Super, gauge-full / time stop | 8016F4B0.., 8007BB8C.. | P15, F3 | 02 |
| 10 | Damage, death, respawn, lives, barrier, invulnerability, checkpoints | 8009B898 | P9, IT | 04 |
| 11 | Interactions + vehicles: DriveVehicle, ExternalControl, ElectricCircuit, items, rings, goal/mission events | 800AF924 cases | P20, IT, vehicle modules | 02 |
| 12 | Remaining family (census-driven): Effect, PostManager, Watch, Sound, MotionEye/Hand, helpers | — | — | all |
| 13 | Full integration: semantic-only storage, owner census = 0 translated player functions on the runtime path | — | — | all |

Completion criteria per batch: every VA in the batch's executed set owned natively; C++ original-word oracle 0
differences + killed negative controls; Westopolis + Final Haunt + one batch-specific replay with `--verify-player` 0
differences, identical frames, byte-identical self-determinism; savestate A/B differs only in dead stack; census shows
the batch's VAs no longer executed translated; 480p CPU (alternating arms) and 5K frame times measured; user approval.

**Earliest batch on approved foundations: 00, then 01.** 01 sits directly on the already-native, already-default
CharaColli and world-query owners, releases the pooled world-result objects that were left for "their consumers", is
the hottest player path (13.4 %), and has a strong candidate (P3) to re-prove.

## 6. Simulation and differential testing

- Isolated: C++ reference interpreter runs the original words on a MEM1 image taken from a savestate at a chosen tick;
  the native candidate runs on the same inputs; compare every written byte, return values, and the player field map,
  per call and per tick for sequences (hundreds–thousands of ticks). Deterministic seeds; failing sequences saved to
  `runtime/tests/data/player/` as permanent regressions; first divergent tick + field reported.
- Integrated: `--verify-player` re-runs the guest body from the same state (write-journal rollback, as
  `--verify-chara-colli`) and compares; plus full-game A/B replays (`.build/ab-jobs/run.sh`).
- The package's Python harness and 64k-frame parity are evidence for candidate selection only.

### 6.1 `--verify-player` modes (2026-11-03)

**Fast diagnostic (default).** Not full oracle equivalence. It checks some arrivals at candidate entries: the scoped
entries, or every address with `--verify-player-all-segments`. Each checked segment gets the full strict comparison
(native pass, interpreter replay of the original words, registers and every written byte). Everything else runs as
plain native code. Unchecked calls are not cut back to the dispatcher (the old per-call cut sent every guest call
there while verifying), and they get no journal and no replay. Policy (`gekko::Cpu::VerifySampling`):

| Option | Default | Meaning |
|---|---|---|
| `--verify-player-first N` | 4 | the first N arrivals at each entry are checked (new code first) |
| `--verify-player-period N` | 1024 | after those, one arrival in N |
| `--verify-player-known <verify.csv>` | — | entries that run verified with no mismatch (repeatable) ... |
| `--verify-player-known-period N` | 16384 | ... are checked only once in N arrivals, with no first-N |
| `--verify-player-cpu P` | 30 | verifier time capped at P % of the game thread's wall time (token bucket, 0.25 s burst) |

The verifier is single-threaded and inline on the game thread. It has no worker threads, no busy-waiting and no thread
priority changes. Above the budget, a wanted check is dropped (counted as `over_budget`). An entry still owed its
first checks keeps them for a later arrival. Each run writes `verify.csv.mode` with the policy and its counts: arrivals,
checked, sampled out, over budget, verifier seconds, wall seconds and process CPU. `player_verify_report.py` prints it
first. A fast run's coverage is what was checked, never the route's.

**Strict (`--verify-player-strict`).** The batch 00 oracle: every segment at the scoped entries, or every segment
with `--verify-player-all-segments`. It is needed for any claim of verified coverage or equivalence. Westopolis with
all segments took 8,507 s for 90 s of game time (833,186,125 segments matched, 0 mismatched).

Measured on Westopolis (`westopolis-cpu-spike`, stg0100, 90 s of game time, all segments as candidates, reference
runner from `.build/watch`):

| Run | Wall | Segments checked | Mismatches | Verifier time | Process CPU | Player instructions replayed |
|---|---|---|---|---|---|---|
| no verifier | 91 s | — | — | — | — | — |
| strict | 8,507 s | 833,186,125 | 0 | ~all | — | 36,125 of 82,045 (1,257 functions) |
| fast | 94 s | 525,545 of 420.9 M arrivals (386,362 over budget) | 0 | 7.3 s (8.1 %) | 0.32 cores | 32,697 (1,174 functions) |
| fast, `KNOWN` = strict csv | 94 s | 24,341 of 419.3 M arrivals (44,550 known entries) | 0 | 0.4 s (0.5 %) | 0.27 cores | not measured |

The fast run checked 90.5 % of the player instructions the strict run did. The rest, the remaining 87.4 % of
arrivals and every unsampled path, were not checked. The corrupt fixture (`corrupt:80083A38`) is still caught in fast
mode: 0xFB vs 0xFA, exit 5, after 6 s.
Fast mode must serve unsampled native owners inline from `try_hle_slow`, under `handler_depth_`. Without that guard a
guest call made inside an owner was sampled and cut away from it. That broke Westopolis at 2.9 s with a load from
0x40000055 in the world-result pool chain.

**Targeted strict.** A scope file line `address,size` makes every segment that starts inside that function a
candidate, including its post-call continuations. The rest of the game's segments are left out.
- Over all 2,541 member ranges (`.build/player/scope_member_ranges.txt`), strict Westopolis takes 183 s, against
  8,507 s for all segments. It replays 36,118 of the 36,125 member instructions. The 7 it misses are `fn_800AD114`, a
  leaf inlined into a non-player caller.
- Strict Final Haunt over the same ranges: 214 s, 40,049 instructions, 0 mismatches.

Fast mode has a structural blind spot here. The code after an unsampled call is never an arrival, because the call
returns inline. This post-call code accounted for 1,330 of the 3,442 instructions in which fast and strict
Westopolis differed. Escalate to targeted strict, not all-segments.

`tools/player/run_verify.sh` defaults to fast mode. `STRICT=1`, `KNOWN=<verify.csv>` and `CPU=<percent>` select the
rest.

## 7. Status tables (to be filled per batch)

```
BATCH:            00 (infrastructure) done; 01 not started
Approval status:  no player reconstruction approved
Integration:      0 player functions native in the game
```

Coverage audit (2026-11-03): `docs/SHADOW_PLAYER_COVERAGE_AUDIT.md`, with inventories in `test_data/player/audit/`.

| Figure | Value |
|---|---|
| Inventory | 2,541 functions / 82,045 instructions, re-derived from main.dol: no overlaps, padding or duplicates. 76,590 instructions strongly evidenced; 56 functions rest on link order alone |
| Strict oracle coverage of translated player code | 41,737 instructions (Westopolis all segments + Final Haunt player ranges), 0 mismatches |
| Independently verified native player | 0 |
| States | 16 of 35 executed by any recording; 19 never executed |
| Package candidates | 323 member functions, none verified: 14 of 56 test rows circular, 45 stub-affected |
| Numerics | game sqrt 0x8000FF04 proven insensitive to the frsqrte model over all single inputs; open: Gekko's frC 24-bit rounding in 263 single-multiply sites (unmodelled in both engines) |

## 8. Open decisions

1. Boundary option A vs B (section 4).
2. Whether the Unresolved package may be used as the candidate source (it was never approved; "DON'T MERGE YET").
3. D1 (v7 splice 8041BB8C) blocks batch 07.
4. frsqrte: the package models it as exact 1/sqrt; the game's estimate must be reproduced unless the user decides
   otherwise. 2026-11-03: the player's only estimate path, sqrt 0x8000FF04, gives identical results with either
   seed for every single input (coverage audit §6). New and open: Gekko's 24-bit rounding of frC in fmuls/fmadds/
   ps_mul* (263 member sites) is modelled by neither engine, so the oracle cannot detect it. It matters only where
   frC can hold more than single precision; a data-flow pass over the sites decides which.
5. Original-game bugs found on the way go to GAME_ERRATA.md, never silently fixed.

## 9. State specifications reverse engineered (2026-11-03)

These are binary-only specifications, not implementations. Every claim is tagged PROVEN, INFERRED or UNKNOWN with
addresses, and cross-referenced to the strict coverage of the recorded routes. Constants were spot-checked against
main.dol.

| Spec | States | Strictly executed evidence | Notable |
|---|---|---|---|
| `docs/player/states/EdgeHang.md` | 12 EdgeHang + Fall's ledge test 0x8009827C | the user's tapes `edgehang_user_session1/2` | only Fall enters; two line casts (reach 3.0 + radius, drop 6.0, floor dot ≥ 0.866); grab lock 0.8; climb = up·140 then forward·220; the census counts factory creations, not entries (CanEnter can reject) |
| `docs/player/states/WallHang_WallJump.md` | 13 WallHang, 16 WallJump | `walljump_user_session3` + Westopolis | WallHang only from JumpDash's contact handler on attribute 0x2 without 0x200; wall jump = command 0x104 after a 0.125 s lock; WallJump probe 80/dot(d, n); 3 of 4 direction branches never ran |
| `docs/player/states/SpinDash_Dead.md` | 18 SpinDash, 31 Dead | flat-stage move route; Dead has none | SpinDash launch = fmadds(850, min(charge, 1), 150), roll time 2k, ends below 150 speed; Dead comes from surface attribute 0x20 or damage at 0 rings, never from height, and has no exit of its own |

Every UNKNOWN and every never-executed branch is listed in its spec. They are the targets for further routes and,
later, for the native reconstruction's tests.

**Targeted routes for the specs' unexecuted branches (2026-11-03).**
- **Scripts:** `.build/player/targeted/run_all.sh`. Each route is cut from the user's tapes or the Westopolis fall route,
  then given a scripted ending. All 16 routes were strictly verified with 0 mismatches.
- **Per-state coverage:** `coverage_by_state.py`.
- **Dead: 0 → 178 of 267** instructions. The Westopolis fall route enters Damage and then Dead, so the 0-ring damage
  path ran.
- **SpinDash: 566 → 600.** Overcharge, jump-out and roll-into-wall all ran.
- **EdgeHang: 978 → 1,006.** Holding the stick left from the hang lets go into Fall. Still unexecuted: 8007F5D0-608,
  80080028-60 and 80098508-24.
- **WallHang: 750 → 757; WallJump: 452 → 452.**
  - All four sideways and diagonal stick variants at the jump gave byte-identical runs (8,291,314 segments each). The
    stick did not change the jump direction from that hang.
  - The spec's stick-bit (0x1B) condition is not met there. Why is still open: input lock or camera-relative zero?
  - The three remaining direction branches, the timer drop and WallJump's wall-ahead path stay unexecuted.
