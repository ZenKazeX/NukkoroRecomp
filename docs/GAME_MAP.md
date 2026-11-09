# What the game is (PAL main.dol, 2026-11-02)

Every function of the executable (34,037; 4.86 MB of code) assigned to a subsystem from the binary's own evidence.
Reproduce: `python -B .build/dolrecon/gameplay_scope/game_map.py` (writes `game_map.json`, `game_map.txt`; inputs:
the pal-v30 skeleton `functions.csv`, its class headers' flattened RTTI bases, and the full call graph of
`tools/dolrecon/callgraph.py`, `.build/dolrecon/pal-v30/callgraph.json`).

Evidence per function, strongest first: **class** - its class (or a base, never the universal root Task) matches the
subsystem's rule in `game_map.py`; **unit** - no class, but the classed functions before and after it in link order,
at most 8 KB apart, are of one subsystem (a translation unit is contiguous); **callers** - every direct caller is of
one subsystem, by any edge of the call graph (direct and tail calls, addresses taken, tables and tables of tables,
vtables, resolved virtual calls; labelled apart when only a non-direct edge shows it); **library** - a library layer
(spine), split by name prefix and named neighbours. The unit / callers shares are estimates of ownership, not
proofs; the class share is firm. 9 % of the code is still unknown, 4 % is shared by several subsystems.

## The game's own code

### Gameplay

| subsystem | code | functions | main classes (evidence) |
|---|---:|---:|---|
| stage objects (gadgets) | 745 KB | 6,090 | 157 Gadget-derived classes: rings, springs, doors (SetDoor, SecretDoor), containers, switches, Elec* (search lights, panels, fire walls, security codes), footings, falling stones, vines, lanterns, warp holes, ArkLaser, ArkCannon, DefenseSystem, SuperComputer, Vacuum* objects |
| bosses | 384 KB | 2,594 | 206 BossCtrlUnit-derived control units (BossCtrlUnit_* checks, gauges, cameras, damage effects) for BlackDoom, DevilDoom, BlackBull (0210, 0412), HeavyDog (0410, 0510), EggMeca (IronBall, Bomb, Laser), EggLastMech; Boss::Diablon, Boss::Desire; BossRootTask, BossCharInfo, BossMotion, BossRender |
| enemies | 318 KB | 3,006 | 49 Enemy-derived types built from components (EnemyStatusCommon, EnemyMoveCommon, EnemyDispCommon, EnemyParamCommon, set / ctrl / disp / move / status / param bases), 84 TEnemyAIState states (EnemyBaseAI, SoldierCommonAI, BkSoldier, BkWing, BkWorm, EggPawn, Walker, Flyer...); types GunSoldier, GunBigfoot, BKTank, BkSoldier, BkWing, BkWorm, BkGiant, BkNinja, BkChaos, BkLarva, EggPawn, EggPierrot... |
| player: Shadow | 326 KB | 2,498 | Player::PlayerShadow, 38 Player::Behavior::* (Ground, Jump, SpinDash, Grind, LightDash, LineHang, WallHang, ChaosControl, DriveVehicle, PickupObject, GroundAttack, AirAttack...), Player::Control::UserInput, Player::Shadow::ShadowWeapon / ShadowMotion, 52 PlayerCommand classes, targeting (TargetSearchList) |
| partners / NPCs | 211 KB | 1,888 | Player::Npc::* on the player engine: Sonic, Tails, Knuckles, Amy, Rouge, Omega, Espio, Vector, Bee, Maria, DoomsEye, EggMonitor, BossSonic (class names); Chao |
| weapons | 185 KB | 1,724 | 43 Weapon::WeaponBase-derived (guns, special weapons such as the vacuum egg), bombs and blasts, rockets, thrown objects, weapon pickups |
| missions, stage flow, results | 141 KB | 1,235 | Mission::{Count, Enemy, Ring, TimerCount}Mission and their message managers, MissionClearCollision, StageAction::{Goal, Result2P, Show, TryAgain, Continue, Restart, Flashback}, StageSequence::Manager, StageManager, GoalRing, SavePoint, ResultTask, StageRouteDisplay |
| vehicles | 133 KB | 950 | Vehicle::{GunLift, AirSaucer, Walker, BatteryBlackKiller, BatteryArmy, BirdBase::AirWing / AirBolt, TVehicleCar} on TVehicleBase / VehicleTask |
| collision, ranges, physics | 114 KB | 662 | SonicteamUSA::System::CharaColli (Body, General, Bone, CharInfo), Range{Box, Cylinder, Capsule, Sphere, Plane}, LightColli, gravity / hint / mission collisions |
| camera | 81 KB | 718 | PJSCamera::* (StageCameraManager, 12 camera modes, post process, factory), SonicteamUSA::System::Camera modes, WatchCamera |
| effects | 75 KB | 322 | Effect::PJS::* (manager, particles, shimmer, rays, arranger) on Effect::System::EffectElement |
| in-game events | 32 KB | 197 | TPjsEventActionManager / Scene / Fade, TPjsEventFolder, StageTriggerEventTask |
| mini games | 15 KB | 117 | ShootingGame |
| other in-game tasks | 54 KB | 451 | Task-derived without a rule yet: TGameInfomation, RenderDiv / RenderUnit, TOneFileAsync&lt;RpWorld / RpClump / RwTexDictionary / RtDict&gt;, TFileControlAsycManager, debug and test tasks (Test*, Sample, TestNakazawa) |

### Presentation and front end

| subsystem | code | functions | main classes; what it stands on |
|---|---:|---:|---|
| front end | 84 KB | 367 | AdvReal::{Logo, Title, SelectMode, BattleMode, StoryLibrary, Synopsis, Option, Movie, MemoryCheck, StaffRoll} phases on AdvReal::AdvBase / AdvFolder |
| menus, HUD, 2D | 50 KB | 312 | TGiPause / TPauseManager, GiGameOver, TGiHdGage, TGiHPGaugeBoss, TGiScoreDisplay, TGiRingChallenge, TGiCommandWindow, TGiHissatu, MessageTask, TExpand* notices, TPjsCsd / CsdOneShot - on Sonic Team's CSD 2D library (csd*.cpp: scenes, motion palettes, texture lists) |
| sound player | 56 KB | 326 | AudioGCN, AudioController::ActStageController, Audio{Manager, LoopManager}Task, TObjBGM (music), SetSeLoop / SetSeOneShot (placed sound emitters), TObjSERIF (voice lines) - on CRI ADX streams (46 KB) and the SDK's AX / AI / AR / DSP (7 KB) |
| video player | 2 KB own | 3 | TMovieManager, StageMovie (stage flow), AdvReal::Movie - on CRI Sofdec (36 KB) |
| save data | 35 KB | 205 | SaveCycle::{Check, Load, Save, CheckModule} phases, TMemoryCardCtrl - on the SDK's CARD (15 KB) |
| resources and loading | 39 KB | 285 | ResourceOneFile(Req), LandManager / LandALoadMananger, CNowLoadingThread, set data (SetGenerator, SetControl, SetAdapter) - on the SDK's DVD and CRI's file system |
| rendering environment | 2 KB | 8 | TFogSet and similar (most rendering is RenderWare's) |

### Framework under all of it

| subsystem | code | functions | main classes |
|---|---:|---:|---|
| game framework | 50 KB | 532 | Task (426 classes derive from it; update = slot 1 with dt), Gadget, CharInfo, ~90 CharCommand messages (Damage, Ride, Leave, Use, Accelerate, SetPosition, SetVelocity, WallJump...), input (PJSPeripheral::ManagerImpGc, Pad2Keyboard / Pad2Mouse implementors), Vibration::ServerGC, Sonicteam::System::Heap / Thread, DebugDisp |
| Boost | 90 KB | 1,365 | shared_ptr instances (217 sp_counted_base classes) |

## Libraries (the platform the game stands on)

| library | code | functions | native replacement |
|---|---:|---:|---|
| RenderWare 3.7 | 323 KB | 978 | source/rwcore + source/rpworld, rebuilt natively (docs/RENDERWARE_RECONSTRUCTION.md); its GameCube driver and pipelines (66 KB) are replaced by a Direct3D 12 driver |
| CRI: ADX sound streams | 46 KB | 257 | native audio |
| CRI: Sofdec movies | 36 KB | 231 | native video decoding |
| CRI: other (file system, memory, common, unnamed) | 265 KB | 692 | host files and memory; to be split further |
| SDK: OS | 41 KB | 223 | host OS (threads, timers, memory) |
| SDK: GX / VI | 38 KB | 187 | none (the native renderer) |
| SDK: DVD | 18 KB | 92 | host files |
| SDK: CARD | 15 KB | 45 | host save files |
| SDK: audio (AX, AI, AR, DSP) | 7 KB | 34 | native audio |
| SDK: PAD | 0.1 KB | 1 | host input |
| SDK: debugger (TRK) | 28 KB | 181 | none |
| SDK: other (unnamed) | 68 KB | 206 | to be identified |
| MSL (C / C++ library) | 50 KB | 116 | the host C++ library |
| CodeWarrior runtime | 4 KB | 37 | the host compiler |

## Not yet assigned

| what | code | functions |
|---|---:|---:|
| unknown: no class, no classed neighbours within 8 KB, callers themselves unknown (game 241 KB, unresolved 119 KB, game-unit 80 KB) | 440 KB | 2,802 |
| shared game code: called from several subsystems (utilities, containers, math - not yet identified) | 196 KB | 1,820 |
| no caller found: dead code, or reached through pointers the graph does not see yet | 49 KB | 180 |
| game callbacks only a library calls (RenderWare 34, CodeWarrior runtime 3, Boost 1) | 9 KB | 38 |

With direct calls only (the first version of this map) 865 KB was unknown; the full call graph brought it to 440 KB.
What remains needs typed receivers for the other 5,730 indirect calls (vcalls resolves 514 of 6,244 `bctrl` sites),
layer evidence beyond link order, and identification of the shared code - see `.build/dolrecon/astra/question7.md`.

## Direct GameCube dependence (`.build/dolrecon/gameplay_scope/gc_census.py`)

Of 29,394 game-side functions, 3,601 show a GameCube fact in their own instructions: MSL calls 1,736, paired singles
1,099, RenderWare calls 562, runtime calls 208, SDK calls 147, CRI calls 37, GQR 5, low memory 3, MMIO 1, cache 1; no
FPSCR or timebase use. Player 180 of 2,098 functions (102 paired singles), enemies 240 of 2,134, missions 52 of 590 (one
SDK call: a debug movie test opening DVD directories). The gameplay code depends on the GameCube mostly indirectly -
through the framework and the 32-bit big-endian data layouts, which the census does not yet measure.
