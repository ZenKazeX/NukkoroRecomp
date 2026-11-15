# Original GameCube control map (PAL main.dol)

Status of every statement: **P** proven by the instructions cited; **I** inferred from the code (evidence given);
**UNKNOWN** not established. Function addresses are 0x8xxxxxxx. Names are RTTI or decomp/matched.json names when the
project has them; `fn_` otherwise. Nothing here was verified at runtime.

Notation: `P` = player object; `pad` = the player's pad object at `P+0x218`; `G` = settings block 0x8057E760;
`SDA2 r2` = 0x805FA780; `r13` = 0x805EC500. Cap bits are the bits of `P+0xA0`, state bits are the set at `P+0xA8`.

Physical mapping, proven by ManagerImpGc's table at 0x80513D48 (12 pairs, tested at 80359844/80359848):

| PAD bit | Engine bit | Control |
|---|---|---|
| 0x0100 | 0x0001 | A |
| 0x0200 | 0x0002 | B |
| 0x0400 | 0x0008 | X |
| 0x0800 | 0x0010 | Y |
| 0x0008 | 0x0040 | D-pad Up |
| 0x0004 | 0x0080 | D-pad Down |
| 0x0001 | 0x0100 | D-pad Left |
| 0x0002 | 0x0200 | D-pad Right |
| 0x1000 | 0x0400 | Start |
| 0x0010 | 0x0020 | Z |
| 0x0040 | 0x1000 | L (digital) |
| 0x0020 | 0x2000 | R (digital) |

Analog triggers are not read by the engine: ManagerImpGc 0x80359798 / 0x803599C4 copies only buttons and the two sticks. [P]

## 1. Manager::Update and the records

### 1.1 Manager (Sonicteam::System::Peripheral::Manager), port record = 0x2C bytes

`Manager::Update` = 0x8040EA3C. Reads through `imp->Read` (bctrl 0x8040EA68) into a stack table of 0x10-byte reports
(status, buttons, sticks); status switch 0x8040EA98: 0 = ok; 2 = only `+0x28` zeroed, rest kept (8040EB60); 3 = status stored, edges zeroed (8040EB6C); anything else (1, negative, ≥4) = status stored and record cleared via 0x8040ED38 (8040EB80).
Accessors: `Manager::GetPort(i)` 0x8040EBBC (`this + i*0x2C`), `GetPortCount` 0x8040EBC8. Stored at `Manager+0xB0` (count),
`+0xB4` (imp); records at `Manager+0`. Port count comes from `imp->GetPortCount` (0x8040E960, 4 on GC, 80359948).

Per-port record [P, 0x8040EABC-0x8040EB5C, clear 0x8040ED38]:

| Offset | Type | Meaning | Evidence |
|---|---|---|---|
| +0x00 | u32 | held, engine bits | 8040EAD0 |
| +0x04 | u32 | `~held` | 8040EACC, 8040EAD4 |
| +0x08 | u32 | pressed = `new & (old ^ new)` | 8040EAEC-8040EAF4 |
| +0x0C | u32 | released = `old & (old ^ new)` | 8040EAF8-8040EAFC |
| +0x10, +0x14 | f32 | left stick x, y, normalised | 8040EB3C-8040EB48 (`Normalize` 8040EDB0) |
| +0x18, +0x1A | s16 | left stick raw x, y | 8040EB0C, 8040EB14 |
| +0x1C, +0x20 | f32 | right stick x, y, normalised | 8040EB4C-8040EB58 |
| +0x24, +0x26 | s16 | right stick raw x, y | 8040EB1C, 8040EB24 |
| +0x28 | s32 | port status | 8040EABC |

Edge detection: when previous status is nonzero (just reconnected), pressed and released are forced to zero that frame
(8040EAD8-8040EAE8). [P]

Stick normalisation, `Normalize(rec+0x10, deadZone, range)` 0x8040EDB0 with `Clamp` 0x8040EE6C [P]:
per-axis dead zone first (`|v| < dead` → 0, else `v ∓ dead`, 8040EE94-8040EEBC / 8040EEC0-8040EEE8);
then circular clamp: if `x*x + y*y > range*range`, scale by `range / sqrt` with integer `divw` (8040EF00-8040EF40);
then `f32(v) / f32(range)` (8040EE28, 8040EE50). Stick range from `imp->GetStickRange` (0x8040EB38); GC returns
left dead 15, right dead 25, left range 0x39-15 = 42, right range 0x40-25 = 39 (80359768). y is negated by the report
(803599F0, 80359A1C): **y positive = down**, one unit toward centre for negative raw deflection (803599D0). [P]

No buffering of frames in Manager. [P: fields are overwritten once per Update.]

### 1.2 Pad singleton at 0x80576DF4 (accessor 0x8005185C, r13+0x2D9C)

Ctor 0x8034B1B4 builds six 0x28-byte `PeripheralState` copies at +0x28 and zero-inits +0..+0x1F; init 0x8034AD30.
Per-frame composer 0x8034A784 (called from `GamePeripheralTask__update` 0x8034A534 after `Manager::Update`). [P]

| Offset | Meaning | Evidence |
|---|---|---|
| +0x00..+0x1F | s16 repeat counters: `port*0x10 + slot*2`, 8 per port, slot masks table 0x80512730 | 8034AA2C-8034AA58 |
| +0x20, +0x24 | Manager record pointers, ports 0, 1 | 8034ADB4-8034ADCC |
| +0x28 | port 0 state (0x28 bytes) | 8034A7D4.. |
| +0x50 | port 1 state | 8034AA64 |
| +0x78 | port 2 slot; not written by the composer | I (8034A784 only writes 0, 1, 3-5) |
| +0xA0 | merge of ports not assigned to P1/P2 (`or` buttons, largest stick) | 8034AA90-8034AC2C |
| +0xC8 | copy of state of port `+0x11C` (P1 logical) | 8034AC30-8034AC8C |
| +0xF0 | copy of state of port `+0x120` (P2 logical) | 8034AC90-8034ACEC |
| +0x118 | u8 "initialised" | 8034AD44, 8034B1FC |
| +0x11C | u32 P1 logical → physical port (default 0) | 8034B204 |
| +0x120 | u32 P2 logical → physical port (default 1) | 8034B208 |

Each 0x28 state: `+0` held, `+4` not-held, `+8` pressed, `+0xC` released, `+0x10,+0x14` left f32, `+0x18,+0x1A` left raw,
`+0x1C,+0x20` right f32, `+0x24,+0x26` right raw. Hence for port 0 (slot at +0x28): held **0x80576E1C**, not-held **0x80576E20**,
pressed **0x80576E24**, released **0x80576E28**, left x/y 0x80576E2C/0x80576E30, right x/y 0x80576E38/0x80576E3C. That resolves
the debug-camera reads: +0x28/+0x2C/+0x30 = held / not-held / pressed, +0x38/+0x3C = left stick, +0x44/+0x48 = right stick.
P1 logical copy (+0xC8): held **0x80576EBC**, pressed **0x80576EC4**; P2 logical copy (+0xF0): held 0x80576EE4, pressed 0x80576EEC. [P]

Synthetic bits added by the composer on top of engine bits [P]:
- 0x0800 mirrors Z (0x20) in held / not-held / pressed / released (8034A824-8034A86C).
- Stick digitals: x > +0.5 (0x805FB3D0) → held 0x200000 (8034A880..); x < -0.5 → held 0x100000; y > +0.5 → held 0x80000;
  y < -0.5 → held 0x40000. Pressed/released edges for these four bits are computed from previous held/not-held
  (8034A910-8034AA0C).
- Repeat counters, table 0x80512730: 0x40, 0x80, 0x200, 0x100 (D-pad), 0x40000, 0x80000, 0x200000, 0x100000 (stick digitals);
  incremented when held, zeroed otherwise (8034AA34-8034AA58).

Repeat query `0x8034A608(input, port, mask, delay, period)`: true on edge (whole mask, or the D-pad or stick subset), or
when `(count - (delay+1)) >= 0` and divisible by `period` (8034A704-8034A718); port 4 = P1, 5 = P2, else tested on both ports. [P]

### 1.3 The player pad object, `P+0x218` (`0x8006F9F8` returns it)

Refreshed every frame by `0x800A8998(pad, dt)`, called from `Player::Controller` update 0x8009F9E0, reading the **Manager**
record, so synthetic stick bits do not reach it. Built at 0x800A8BA8. Bound to a logical player by `0x800A8B04(pad, playerIndex)`:
`pad+0x14 = singleton[+0x11C + 4*i]`, `pad+0x10 = Manager record of that port` (only ports < 2), else null. [P]

| Offset | Meaning | Evidence |
|---|---|---|
| +0x00..+0x0C | f32 left x, y, right x, y, each stick length clamped to 1 | 800A8A20-800A8A74 |
| +0x10 | Manager record pointer | 800A8B48 |
| +0x14 | physical port | 800A8B2C |
| +0x18 | 22 logical bits, held | 800A89FC-800A8A10 |
| +0x1C | `~held` | 800A8AC8 |
| +0x20 | **pressed** (logical) = `held & ~prev` | 800A8AD8 |
| +0x24 | **released** (logical) = `prev & ~held` | 800A8AE8 |
| +0x28 | previous held | 800A89D4 |
| +0x2C.. | u32[22] binding table copied from 0x804B3C00 | 800A8BE0 |
| +0x84 | f32 idle timer, reset on any held bit or stick length² ≥ 1e-4 | 800A89B8, 800A8AA8 |

Binding table 0x804B3C00, logical bit i tests engine mask (`and.` at 800A8A00) [P]:

| i | bit | engine | i | bit | engine |
|---|---|---|---|---|---|
| 0 | 0x1 | Start | 11 | 0x800 | D-pad Right |
| 1 | 0x2 | Start | 12 | 0x1000 | D-pad Up |
| 2 | 0x4 | B | 13 | 0x2000 | D-pad Down |
| 3 | 0x8 | A | 14 | 0x4000 | D-pad Left |
| 4 | 0x10 | A | 15 | 0x8000 | D-pad Right |
| 5 | 0x20 | B | 16 | 0x10000 | Z |
| 6 | 0x40 | X | 17 | 0x20000 | engine 0x4 (never set by GC table) |
| 7 | 0x80 | Y | 18 | 0x40000 | L |
| 8 | 0x100 | R | 19 | 0x80000 | R |
| 9 | 0x200 | D-pad Left | 20 | 0x100000 | engine 0x4000 (not in GC table) |
| 10 | 0x400 | D-pad Down | 21 | 0x200000 | engine 0x8000 (not in GC table) |

No code writes the table after 800A8BE0 was found by the scan used here (not a proof of absence). Logical bits the player
actually tests are below; bits 2, 3, 12-19 have no reader found through the pad object.

## 2. Scheduling and suppression

- `GamePeripheralTask` (vtable at 0x8056021C): update 0x8034A534 calls `Manager::Update` (0x8040EA3C, Manager via 0x80009208), then the
  singleton composer. Ctor 0x8034A5B8, created in 0x801E258C at 0x801E26F0 under task group **0** (`0x801EB614(_, 0)`, 801E26E0).
  Its `+4` flag sets 0x100 (8034A5EC), so it runs while the pause byte 0x805EF27C is set (`docs/DOLRECON.md`: while set, only flag 0x100 tasks run). [P]
- Task groups observed by creation site (`0x801EB614(_, n)` before the ctor): group 0 peripheral and NowLoading; group 1 audio; group 4 Vibration; group 5 CameraStart / RenderDiv / RenderStatic / CameraEnd; group 7 Player-related objects; group 0x12 PJSCamera Pre/Post tasks. `docs/DISTANT_RENDERING.md` states layout (group 6) before Landscape (10) and the player (13). I: walk order is by group index; not proven here.
- Player: `Player::Controller` update 0x8009EFF8 → 0x8009F9A4: refresh pad (0x800A8998), several per-frame handlers, then
  `UserInput::vf3` 0x800A1D70 through the Controller's weak `+8` (8009FB30, slot +0x14). Pad refresh and command build are in the same
  update. Commands are posted to the command queue and run later (docs/SHADOW_PLAYER_NATIVE_RECONSTRUCTION.md §3 gives Controller → Command → CharaColli → Shadow task). [P for the call chain; order against camera I.]
- Camera: consumes via singleton in `PJSCamera` modes (`CameraModeFree` 801C1E08, `CameraModeSetEditor` 801C6150, `CameraModeTestPlayer` 801C64B8, `CameraModeTestPlayerNoRot` 801C6960), which are not the gameplay modes; the user-camera right-stick path is UNKNOWN (see section 5).

Input suppression [P unless noted]:
- Pause: while 0x805EF27C is set only flag-0x100 tasks run (above); the peripheral task keeps running. Player tasks are not reported to carry 0x100. I.
- `G+0xC8` (byte) nonzero, or state bit 0x2A: UserInput posts only EndUseWeapon (if bit 0x19) and ControlVehicle(7) (if bit 0x10 and Cap 8), then returns (800A1DE0-800A1EB8). Writers of G+0xC8 UNKNOWN.
- State bit 0x2A/0x2E: counters `P+0x88`/`P+0x90` (`docs/player/states/SpinDash_Dead.md` 431-440); 0x2E skips the stick command (800A20C8-800A20DC).
- `P+0x5C == 2` plus 0x801F45D8 true or 800A1CE8 (idle ≥ threshold): UserInput returns early (800A1D88-800A1DB0). Meaning of `P+0x5C` UNKNOWN; UserInput code treats value 1 and 2 differently.
- `Player::IgnoreInputCommand` (id 0x127, ctor 800775F8) exists; its effect beyond bit 0x2A UNKNOWN.
- `TStageKeyNoInput` (801771EC, update 801770C0): idle timer driven from singleton +0xC8 held bits A/B/X/Y and left stick; effect after timeout UNKNOWN.

## 3. Consumers

### 3.1 Player::Control::UserInput (vf3 0x800A1D70, builder 0x800A0130)

Reads `pad+0x20` (pressed) and `pad+0x24` (released). Slots filled by builder (press word, release word); dispatch after the builder in vf3, with "consumed" flag preventing later presses when an action is delivered to another target. The stick becomes `AccelerateInputCommand` (id 1, ctor 801DC224) when `|v|²` ≥ 1e-4, skipped under state 0x2E (800A20C8).

| Slot | Stack | Logical bit | Physical | Press dispatch | Release dispatch | Target |
|---|---|---|---|---|---|---|
| X | sp+0x28 | 0x40 (i=6) | X | 800A21EC | 800A2270 | `sp+0x10` (default player, redirectable) |
| A | sp+0x30 | 0x10 (i=4) | A | 800A22C0 | 800A2330 | `sp+0x18` (default player, redirectable) |
| B | sp+0x38 | 0x20 (i=5) | B | 800A2380 | 800A23D8 | player |
| Y | sp+0x20 | 0x80 (i=7) | Y | 800A2428 | 800A2480 | player |

Priority: X press first. When X lands on a non-player target (800A2214) the consumed flag blocks A, B, and Y press commands this frame (800A22CC, 800A238C, 800A2434). Release commands and direct commands are unaffected. [P]

Direct commands posted by vf3 regardless of slots [P]:
- B released → 0x103 `Player::EndAttackCommand` (800A1FF8-800A2030).
- X pressed → 0x106 `Player::StartActionCommand` (800A2034-800A206C). X released → 0x107 `Player::EndActionCommand` (800A2070-800A20A8).
- R held (`pad+0x18 & 0x100`, 800A1CBC; only when `P+0x5C != 2`, no 0x2E): when state 0x3C clear → 0x124 StartParallelMove; R released (state 0x3C set) → 0x125 EndParallelMove (800A1EC4-800A1F60).
- D-pad pressed, `G+0x125 == 0` (hard-mode flag per `docs/STAGE_LOADING.md`): Left → index 0, Down → 1, Right → 2 (800A1F88-800A1FAC); applied through `0x8016E1A8`/`0x8016E220` on the object returned by `0x8006D7D0`, else refusal sound through `P+0x240` (800A1FE8). I: mission-selection control, based on proximity to `MissionManagerTask` (0x8016E4EC) and the hard-mode gate; exact semantics UNKNOWN.

Command builder 0x800A0130, when state bits 0x16 or 0x14 are set the builder exits with no commands (800A017C-800A0198). [P] Cap bits are at `P+0xA0`, state at `P+0xA8`.

**Vehicle path (state 0x10 set)**, 800A01B0-800A0314, then exit:

- Cap 8 set: B slot = ControlVehicle(6)/(7).
- Cap 8 clear, state 0x1F and `0x8009EF4C(P+0x224)` true: B slot = 0x115 `StartUseWeaponCommand` / 0x117 `EndUseWeaponCommand`.
- Always: A slot = ControlVehicle(2)/(3); X slot = ControlVehicle(4)/(5). [P]

**Foot path (state 0x10 clear)**:

| Slot | Priority order | Result | Condition | Evidence |
|---|---|---|---|---|
| Y | 1 | 0x113 `ChaosBlastCommand` | state 0x23 | 800A0318-800A0350 |
| Y | 2 | 0x114 `ChaosControlCommand` | state 0x22 | 800A0354-800A038C |
| Y | 3 | 0x119 `CastWeaponCommand` | state 0x1F, not 0x33, not 0x0B | 800A0390-800A03F4 |
| X | 1 | 0x11E `Player::ThrowCommand` | state 0x1E | 800A03F8-800A045C |
| X | 2 | 0x10F `DarkSpinCommand`, arg from `0x8009EF1C(P+0x224)` | `0x800ABF30 == 4` | 800A0460-800A04AC |
| X | 3 | 0x110 `LightDashCommand` | `0x800A18B0(UserInput, stick)` true (Cap 3, nearby object flag, distance) | 800A04B0-800A04E4, 800A18B0 |
| X | 4 | 0x109 `SlidingCommand` | Cap 4 and `P+0x5C == 1` | 800A04E8-800A0530 |
| X | 5 | 0x10E `SpinDashCommand` | Cap 5 | 800A0534-800A0568 |
| B | 1 | 0x115 `StartUseWeapon` / 0x117 `EndUseWeapon` | state 0x1F, Cap 7, `0x8009EF4C(P+0x224)` | 800A056C-800A05F0 |
| B | 2 | 0x109 `SlidingCommand` | Cap 4 and `P+0x5C == 1` | 800A05F4-800A063C |
| B | 3 | 0x102 `StartAttackCommand` | Cap 6 | 800A0640-800A0674 |
| A | 1 | 0x10A `HomingAttackCommand` | Cap 2, `0x800A1A30` true | 800A0678-800A071C |
| A | 2 | 0x108 `JumpDashCommand` | Cap 2, `0x800A1A30` false | 800A0720-800A074C |
| A | 3 | 0x104 `StartJumpCommand` | Cap 1, Cap 2 clear | 800A0750-800A0784 |
| A release | | 0x105 `EndJumpCommand` | state 0x04 | 800A0788-800A07BC |

Overrides when `P+0x5C == 1` (800A07C0 onward); each overrides the slot it names [P for the structure, object type names UNKNOWN]:

| Trigger object | Slot | Command | Condition | Evidence |
|---|---|---|---|---|
| Approach target (`P+0x24C` vt +0x28), object flags `+0x30` bit 0x20 | X | 0x11C ApproachControl(1) | not state 0x1E, geometry checks | 800A0838-800A09E0 |
| Same, flags bit 0x40 | X | 0x11C ApproachControl(2) | as above | 800A09E4-800A0A40 |
| `P+0x248` object, type `+0x50 == 0x1B` | direct | UseCommand (0xD) posted immediately to object; clears A, B, X slots | 800A0AF8-800A0C78 |
| type 0x33 | X | 0x123 `GetOnVehicleCommand` when `0x801B6B64`/`0x8005232C` usable | not 0x0B, 0x1E, 0x10 | 800A0E8C-800A0F7C |
| type 0x34 | direct | UseCommand when weapon id matches and `vt+0x44`; else X = 0x118 `PickupWeaponCommand` (state 0x1F) or post Pickup directly when `P+0x98 == 0` | 800A0C80-800A0E88 |
| type 0x86 | X | 0x10F DarkSpin when `0x800ABFF4 == 4` | not 0x0B, 0x1E | 800A0FE8-800A10CC |
| second object (0x80078ED4), type 0xC | X | UseCommand; speed < constant; target cursor set | not state 0x1E | 800A114C-800A1214 |
| same object, type 0x37 | A | UseCommand | | 800A1218-800A1294 |

State 0x15 set: X slot discarded (800A12A4-800A12E4).

### 3.2 Player helpers reading the pad directly

| Function | Reads | Meaning | Status |
|---|---|---|---|
| 0x800A1C7C | held 0x100 (R) | Parallel-move gate used by vf3 | P |
| 0x800A1CE8 | pad +0x84 | Idle-time test used by `P+0x5C == 2` gate | P |
| 0x8009E738 | left stick, held 0x80 (Y), 0x10 (A) | Builds a velocity from stick (y from A/Y) and posts SetPositionCommand (id 2). Owning class UNKNOWN | P (behaviour), class UNKNOWN |
| 0x800701F0 | held 0x10 (A) | While bit set at `+0x40`, accumulates `+0x3C`; posts StartJump(4) then EndJump(4). Class UNKNOWN | P |
| 0x8006F8E8, 0x800A76CC | pad +0x84 | idle timer consumers | P (purpose UNKNOWN) |
| 0x80297B8C | pad +0x08, +0x0C (right stick) | Class UNKNOWN | P (purpose UNKNOWN) |

### 3.3 Vehicles: CharCommand id 0x15 (ControlVehicle, sub-code +0x14)

Player-side `DriveVehicle` handler 0x8007E8B8 forwards ids 1 and 0x15 to the vehicle when `+0x20` bit 1 is set (8007E950, 8007E9A8), and marks 0x111/0x112 as handled.
Sub-code mapping from UserInput: **2/3 = A press/release, 4/5 = X press/release, 6/7 = B press/release**. Id 1 AccelerateInput carries the stick.
Sub-code 1 is handled by vehicles but UserInput never posts it; UNKNOWN sender.

| Vehicle (handler) | 1 | 2 (A) | 3 (A up) | 4 (X) | 5 (X up) | 6 (B) | 7 (B up) | Evidence |
|---|---|---|---|---|---|---|---|---|
| `Vehicle::BatteryArmy` vf 0x800EA15C | jump-off fn 800E90E0 | - | - | 800E90E0 | - | sets `+0x1C8` bit 0, timer `+0x1CC` | clears bit | 800EA240-800EA2D4 |
| `Vehicle::BatteryBlackKiller` 0x800EC13C | 800EB160 | - | - | 800EB160 | - | sets `+0x1C4` bit 0 | handled only | 800EC220-800EC29C |
| `Vehicle::GunLift` 0x803178F4 | 803185FC | sets `+0x15C` bit 2 | clears bit 2, stops loop | 803185FC | - | sets bit 0, timer `+0x16C` | clears bit 0, stops `+0x1D8` | jump table 0x8055D238, 803179C4-80317A54 |
| `Vehicle::Walker` vf14 0x801AEE10 | `0x801B63D0(this,0)` | if `0x803342F4(+0xA8)` set bit 5 else 6 in `+0x104` | clears bits 2, 3 | sets bit 4 | - | `0x80348714(+0x9C)` | `0x803486E8(+0x9C)` | 0x8053C4C8, 801AEF50-801AEFF0 |
| `Vehicle::AirSaucer` vf13 0x801B2370 | `0x801B63D0(this,0)` | object interact (type 0x37 via 0x801B64F0) | none | if `+0x144` bit 1 set bit 5, else `0x801B63D0(0)` | none | none | none | id table 0x8053C698, sub-switch 801B24D0-801B2670 |
| `Vehicle::BirdBase` vf13 0x802CC3D4 | `0x801B63D0(this,0)` | sets `+0x9C` bit 2 | clears bit 2 | `0x801B63D0(this,0)` | none | none | none | id table 0x80559004, 802CC4E8-802CC574 |
| `Vehicle::BirdBase::AirBolt` 0x802CCDF8 | - | - | - | - | - | sets `+0x120` bit 0 | clears bit 0 | 802CCE20-802CCE64, then forwards to BirdBase |

Stick (id 1): BatteryArmy 800EA190, BlackKiller 800EC170, GunLift 80317928 integrate the stick into `+0x1B8/+0x1BC` style angles. Walker/AirSaucer store the vector (801AEE4C, 801B23E4). [P]
800E90E0 and 803185FC build `StartJumpCommand` (id 0x104, arg 4) and post it to the rider (800E9134, 80318650); I: code 1 / X press = dismount. The `0x801B63D0(this, 0)` helper (AirSaucer, BirdBase, Walker) is the vehicle's rider-release path by the same reasoning, I. Reading B as fire: I, from the held-flag pattern; no firing code was traced.

Not found: id 0x15 handler for `TVehicleCar`, `BKTank`, `EscapePlane`, `EggBalloon`, `Coaster`, stationary guns. The scan looked for `lhz +4` compare to 0x15 in text; those classes did not match, and a handler using a different comparison order would be missed. Status UNKNOWN (section 5).

### 3.4 Menus and pause

Menu action map (identity table 0x804FF6F8 → masks at `r13+0x59E4`, table 0x80560788, 3 words per action, per `docs/OPTIONS_DISPLAY_TILE.md`) [P]:

| Action | Masks | Control |
|---|---|---|
| 0 | 0x400 | Start |
| 1 | 0x400, 0x8, 0x1 | Start / X / A (confirm) |
| 2 | 0x10, 0x2 | Y / B (back) |
| 3 | 0x40040 | Up / stick up |
| 4 | 0x80080 | Down / stick down |
| 5 | 0x100100 | Left / stick left |
| 6 | 0x200200 | Right / stick right |
| 7 | 0x800 | Z |
| 8 | 0x2000 | R |
| 9 | 0x1000 | L |

`0x802B810C(action, port)` = repeat (delay 12, period 1); `0x802B8168(action, port)` = edge via 0x80357068 on merged `+0xD0` for port 4. Both gate on 0x801178FC / 0x802E5D88 (fade). Callers 0x802B964C (options), 0x802B9A54, 0x802BC164, 0x802BC87C, 0x802BD90C, 0x802BDBFC, 0x802BDF0C, 0x802BE284, 0x802C1F94, 0x802C2120, 0x802C27B8, 0x802C7860, 0x803426E4, 0x8034504C. Screen identity of each caller UNKNOWN except options per that doc.

| Function | Reads | Result | Status |
|---|---|---|---|
| 0x80173418 | P1 pressed +0xD0 & 0x400 (Start) | With 0x80044E38 == 0, `0x801727C4()` byte +8 == 0, `G+0xD1` != 0: sets `+0x34 = 1`, begins a pause-style path | P; class UNKNOWN (I: stage pause) |
| 0x802D3118 `StageMovie::vf1` | +0xD0 & 0x400 | Start skips movie (or when `+0x48` set) | P |
| 0x80342C08 | +0xD0 & 0x400 | Start aborts a timed sequence | P |
| 0x801E3D38 | P1/P2 pressed & 0x400 | returns which port pressed Start (2P gate `G+0xB2`) | P; screen UNKNOWN |
| 0x801E3714 | pressed + stick digitals | cursor Up/Down/Left/Right; Y `0x10` and 0x80357068 for confirm | P; screen UNKNOWN |
| 0x801E3570 | P1 pressed 0x10 (Y) | returns 1 | P; screen UNKNOWN |
| 0x802E5798 | pressed by port, A / B | selects entries, plays sounds 0x1E003 / 0x1E004 (802E5A14-802E5A5C) | P; screen UNKNOWN |
| 0x80331888 | port 0 pressed Up 0x40, Down 0x80, Start 0x400 | cursor `+0x314`; Start activates selection | P; screen UNKNOWN |
| 0x80306638 (`palsel`) | Manager records, OR of pressed | 0x401 confirm, 0x200 / 0x100 | P; per docs/REGION_FREE_BUILDER.md PAL mode screen |
| 0x80171544 | port 0 pressed bit 0 (A) | `+0x34 → +0x3C` | P; class UNKNOWN |


### 3.5 Two-player

Logical player index i selects physical port through `singleton+0x11C + 4*i` (`0x800A8B04`, 0x8034C088). Manager-record binding is only for port < 2 (800A8B38). The 13 callers of 0x800A8B04 at the 0x8020F680-0x8029408C range with player index 1 bind a character's pad to player 2 (`0x8020F70C` etc.); their classes UNKNOWN. `G+0xB2` is the 2-player flag (`docs/SE_MANAGER_MAP.md`).

### 3.6 Debug cameras (not gameplay)

Reads via singleton +0x28 (held), +0x30 (pressed), +0x2C (not-held), +0x38,+0x3C (left), +0x44,+0x48 (right): `PJSCamera::CameraModeFree` 801C1E08, `CameraModeSetEditor` 801C6150, `CameraModeTestPlayer` 801C64B8, `CameraModeTestPlayerNoRot` 801C6960, `fn_801C59CC`, `fn_801C5CDC`, `fn_801BDBA0`, `fn_801BD8FC`, `fn_801BF1F8`.

## 4. Context table

Edge: P = pressed, H = held, R = released. Status as above.

| Context | Control | Edge | Action | Condition | Function | Evidence | Status |
|---|---|---|---|---|---|---|---|
| On foot | A | P | HomingAttack (0x10A) | Cap 2 and target found | 0x800A0130 | 800A0678-800A071C | P |
| On foot | A | P | JumpDash (0x108) | Cap 2, no target | 0x800A0130 | 800A0720 | P |
| On foot | A | P | StartJump (0x104) | Cap 1, not Cap 2 | 0x800A0130 | 800A0750 | P |
| On foot | A | R | EndJump (0x105) | state 0x04 | 0x800A0130 | 800A0788 | P |
| On foot | B | P | StartAttack (0x102) | Cap 6 | 0x800A0130 | 800A0640 | P |
| On foot | B | R | EndAttack (0x103) | always, direct | 0x800A1D70 | 800A1FF8 | P |
| On foot | B | P | Sliding (0x109) | Cap 4, `P+0x5C == 1`, before Attack | 0x800A0130 | 800A05F4 | P |
| On foot | B | P | StartUseWeapon (0x115) | state 0x1F, Cap 7, gun-usable test | 0x800A0130 | 800A056C | P |
| On foot | B | R | EndUseWeapon (0x117) | same | 0x800A0130 | 800A05F0 | P |
| On foot | X | P | StartAction (0x106), direct | always | 0x800A1D70 | 800A2034 | P |
| On foot | X | R | EndAction (0x107), direct | always | 0x800A1D70 | 800A2070 | P |
| On foot | X | P | Throw (0x11E) | state 0x1E | 0x800A0130 | 800A03F8 | P |
| On foot | X | P | DarkSpin (0x10F) | `0x800ABF30 == 4` | 0x800A0130 | 800A0460 | P |
| On foot | X | P | LightDash (0x110) | Cap 3, object near (`0x800A18B0`) | 0x800A0130 | 800A04B0 | P |
| On foot | X | P | Sliding (0x109) | Cap 4, `P+0x5C == 1` | 0x800A0130 | 800A04E8 | P |
| On foot | X | P | SpinDash (0x10E) | Cap 5 (Ground, Idle, Landing) | 0x800A0130 | 800A0534 | P |
| On foot | X | P | ApproachControl (0x11C) | approach target flags 0x20/0x40 | 0x800A0130 | 800A0838 | P |
| On foot | X | P | GetOnVehicle (0x123) | object type 0x33 | 0x800A0130 | 800A0E8C | P |
| On foot | X | P | PickupWeapon (0x118) | object type 0x34, state 0x1F | 0x800A0130 | 800A0D50 | P |
| On foot | - | (none) | PickupWeapon | object type 0x34, not state 0x1F, `P+0x98 == 0` | 0x800A0130 | 800A0DF8 | P |
| On foot | - | (none) | UseCommand direct | object type 0x1B or weapon match | 0x800A0130 | 800A0AF8, 800A0C80 | P |
| On foot | Y | P | ChaosBlast (0x113) | state 0x23 | 0x800A0130 | 800A0318 | P |
| On foot | Y | P | ChaosControl (0x114) | state 0x22, not 0x23 | 0x800A0130 | 800A0354 | P |
| On foot | Y | P | CastWeapon (0x119) | state 0x1F, not 0x33, 0x0B | 0x800A0130 | 800A0390 | P |
| On foot | R | H | StartParallelMove (0x124) | `P+0x5C != 2`, no 0x2E, 0x3C clear | 0x800A1D70 | 800A1EC4 | P |
| On foot | R | R | EndParallelMove (0x125) | state 0x3C set | 0x800A1D70 | 800A1F1C | P |
| On foot | D-pad L/D/R | P | select index 0/1/2 (I: mission view) | `G+0x125 == 0` | 0x800A1D70 | 800A1F88 | P (action I) |
| On foot | left stick | continuous | AccelerateInput (id 1) | `|v|² >= 1e-4`, not 0x2E | 0x800A1D70 | 800A20C8 | P |
| Vehicle | A | P/R | ControlVehicle 2/3 | state 0x10 | 0x800A0130 | 800A0284 | P |
| Vehicle | X | P/R | ControlVehicle 4/5 | state 0x10 | 0x800A0130 | 800A02D0 | P |
| Vehicle | B | P/R | ControlVehicle 6/7 | state 0x10, Cap 8 | 0x800A0130 | 800A01D0 | P |
| Vehicle (Battery, GunLift) | X | P | Rider jump-off (StartJump arg 4) | | 800E90E0 / 803185FC | 800E9130 | P (I: dismount) |
| Vehicle (Battery, GunLift) | B | P/R | Set/clear fire-like bit, timer | | handler table above | 800EA2B0 | P (I: fire) |
| Pause (P1) | Start | P | Enter pause path | gates in 3.4 | 0x80173418 | 80173444 | P (I: pause) |
| Movie | Start | P | Skip | | 0x802D3118 | 802D31F4 | P |
| Menus | Start/A/X | P | Confirm action 1 | fade gate | 0x802B8168 | 80560794 | P |
| Menus | B/Y | P | Back action 2 | fade gate | 0x802B8168 | 805607A0 | P |
| Menus | D-pad / stick | P, repeat 12/1 | Navigate actions 3-6 | | 0x802B810C | 805607AC | P |

## 5. Unknowns

1. Gameplay camera (right stick) consumer: no hit through pad object except 0x80297B8C. The user camera path (L/R, right stick) is not traced. Logical L/R bits (i=18,19) have no found reader.
2. BKTank, EscapePlane, EggBalloon, Coaster, TVehicleCar, stationary guns: no pad read found and no id 0x15 handler matching the search; whether they accept the commands by another path is UNKNOWN.
3. Which button "drops" a weapon: no direct binding found; 0x116 `CancelUseWeaponCommand` is posted by `Player::Controller` (8009FB6C), not by a button.
4. Weapon switch: no D-pad/button binding found in UserInput (D-pad indices go to the mission-style object).
5. Grind-rail switch and edge-hang: no button is read in the states examined; edge hang reads no buttons (`docs/player/states/EdgeHang.md`); rail switching via stick/jump unresolved.
6. State bits 0x16, 0x14, 0x0B, 0x33, 0x3C, and `P+0x5C` meaning remain inferred or UNKNOWN; `G+0xC8` writer UNKNOWN.
7. Exact menu screens for the caller lists in 3.4, and who writes `singleton+0x11C/+0x120` after init.
8. Whether X on AirSaucer's non-bit path, and sub-code 1 on every vehicle, is reachable from a real input (sub-code 1 has no UserInput sender found).
9. Task walk order across groups is inferred; precise position of Player::Controller relative to camera tasks not proven here.
10. Purpose of 0x8009E738, 0x800701F0, 0x80297B8C, TStageKeyNoInput timeout action.
