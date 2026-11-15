# Player movement and camera input (PAL main.dol)

How the left stick becomes on-foot movement, how the right stick reaches the camera, and what that means for a
keyboard's digital WASD. Static reading of the DOL only; nothing here was run. Addresses are instruction addresses in
the PAL `main.dol`. `fcmpo` is written by the disassembler as `.long 0xFC...`. r2 = 0x805FA780, r13 = 0x805EC500
(800032B0 init), so `lfs f0,-0x6bd8(r2)` reads 0x805F3BA8.

Evidence: everything is **BIN** (read from the DOL) unless marked **UNCERTAIN** or **UNKNOWN**. The `H:\Unresolved
Shadow functions` report was used only to find where to look; every behaviour below was re-read in the disassembly.

Sign convention (runtime/lift/peripheral/manager_imp_pc.cpp): a port reading is stick units, x right positive, y **down**
positive; a negative deflection reads one unit closer to centre (sample -100 becomes -99).

## 1. The input pipeline

### 1.1 Normalisation (Manager update 8040EA6C, axis pair 8040EDB0, clamp 8040EE6C)

The peripheral manager is the object 0x8056FE0C (accessor 80009208, 4 records stride 0x2C, 8040EBBC). A record:
`+0x00` held mask, `+0x04` ~held, `+0x08` pressed, `+0x0C` released, `+0x10/+0x14` left x/y f32, `+0x18/+0x1A` left raw s16,
`+0x1C/+0x20` right x/y f32, `+0x24/+0x26` right raw s16, `+0x28` state.

GetStickRange 80359768 writes s16[4] = {left dead 15 (`li 0xF`), right dead 25 (`li 0x19`), left range 57-15 = 42
(`subfic 0x39`), right range 64-25 = 39 (`subfic 0x40`)}.

8040EE6C(x*, y*, range, dead), integer arithmetic:
```
if (-dead < a && a < dead) a = 0; else a += (a > 0 ? -dead : +dead);     // per axis; |a| == dead also gives 0
if (range*range < x*x + y*y) {                                            // 8040EF00, cmpw
    len = (int) sqrt((float)(x*x + y*y));                                 // fctiwz truncates at 8040EF28
    x = (x*range) / len;  y = (y*range) / len;                            // divw truncates toward zero
}
```
8040EDB0 then stores x/range and y/range as f32. Nothing else scales the axis.

### 1.2 Controller object at player+0x218

Allocated 0x88 bytes in 8007A038 (ctor 800A8BA8 at 8007A0D0, stored 8007A0D8), accessor 8006F9F8. Update 800A8998(dt), called once
from Controller::update 8009F9A4 (8009F9E0):

| field | meaning | instruction |
|---|---|---|
| +0x00, +0x04 | left stick x, y (record +0x10, +0x14) | 800A8A20..800A8A30 |
| +0x08, +0x0C | right stick x, y (record +0x1C, +0x20) | 800A8A34..800A8A40 |
| +0x18 | logical held word, from pad held mask through table +0x2C | 800A89FC..800A8A1C |
| +0x1C, +0x20, +0x24 | ~held, pressed edge, released edge | 800A8AC8..800A8AEC |
| +0x28 | previous held word | 800A89D4 |
| +0x2C.. | 22-entry button table, defaults 804B3C00 (800A8BDC loop) | 800A8BE0 |
| +0x84 | idle timer, += dt; reset to 0 on any button or stick² ≥ 1e-4 | 800A89B8, 800A8A78..800A8AA8 |

Both sticks are length-limited here: 2D squared length > 1.0 (800A8A48/800A8A64, 8040DD24) renormalises (8040DCBC).
The object keeps **no previous stick value**.

Logical bit 8 (held word 0x100, table entry +0x20) defaults to pad mask 0x2000, `kControlR` (runtime controls.h).

### 1.3 Filling PJSCameraTargetInfo: PostManageTask (80297B8C, reached from vf1 80297E2C)

`PostManageTask` (vtable 0x805539B4, ctor 80297B30, created in group 0x0F, 8017860C). Per player, on a local TI built by
the ctor 80290710:

| TI field | source | instruction | scale / sign |
|---|---|---|---|
| +0x00 pos | player+0x00, or the +0x258 attached object's | 80297C68, 80297CB0 | |
| +0x0C vel | player+0x18 / attached | 80297C78, 80297CC0 | |
| +0x18 up | 800797CC (local up, (0,1,0) through the player frame) | 80297CD8, 80297CE4 | |
| +0x24 forward | 80079810 (local forward, (0,0,-1) through the player frame) | 80297CF0, 80297CFC | |
| +0x30 | player+0xE8 | 80297D08 | |
| +0x40 12-byte bitset | player+0xA8 bitset (3 words) | 80297D1C (80095654) | |
| +0x4C | controller +0x08 = right stick x | 80297D50 | none |
| +0x50 | controller +0x0C = right stick y | 80297D58 | none, y down positive |

- +0x4C/+0x50 are written only when byte 0x8057E760+0xC8 == 0 and player bit 0x2A is clear (80297D20..80297D40); otherwise the
  ctor's zero stays (vf0 802905D8 initialises +0x4C/+0x50 from r13+0x5F00). The same gate at 800A1DE0..800A1E00 sends the
  controller down its restricted path, so **0x2A = input disabled** is consistent, not named.
- TI flag bit 60 (word +0x44 bit 28) = player state bit **0x3C**, copied with the bitset. It is set by command 0x124 (ctor 800A2614,
  execute 8009ADC0 at 8009AE10) and cleared by 0x125 (ctor 800A25D0). UserInput issues 0x124 while held bit 0x100 (R) and
  neither 0x2E nor player+0x5C==2 (800A1C7C, 800A1ED0..800A1F18), and 0x125 on release (800A1F28..800A1F60). Command names
  **UNCERTAIN** ("StartParallelMove" comes from the package; only the bit behaviour is proven). Running state with bit
  0x3C uses the camera-aligned strafe body 80083138 (8008335C..80083384).
- 80297DC4 pushes the local TI to the camera unit's TI (`*(unit+0xF4)` vf3 at 80297E08).

So the right stick reaches the camera as normalised axes, with no scale and no sign change. CameraModeGeneral uses it as already
documented (TI+0x4C to impl+0x134, TI+0x50 to impl+0x130). StageCameraManager's test 801CB86C is magnitude² ≥ 1e-4.

## 2. Direction: UserInput (Player::Control::UserInput vf3 800A1D70, builder 800A1790)

UserInput is dispatched from Controller::update (vtable +0x14 at 8009FB30, object at Controller+8) after the pad update at 8009F9E0. Build at 800A20B8 (800A1790), send at 800A210C..800A213C:

```
s = (controller+0, controller+4)
v = zero
if (!(|s.x| < 1e-4 && |s.y| < 1e-4))  v = (s.x, 0, s.y)              // 800A17B4..800A17E0, 8000A950
if (!(player bit 0x3A))  v = rotate_by_camera(v)                     // 800A17E4..800A1814 -> 800940C0
if (v.x²+v.y²+v.z² > 1.0)  v = normalise(v)                          // 800A1818..800A1830
m = (cap bit 0 of player+0xA0) ? player+0x1E4 : player+0x11C         // 800A1834..800A1880
out = m · v                                                          // 8040C6CC
post Command id 1 {+0x14 = out}  when |out|² ≥ 1e-4 and bit 0x2E clear   // 800A20C8..800A213C
```

rotate_by_camera 800940C0:
```
c  = look - eye of camera unit (port = player+0x54, -1 or player+0x5C==2 → 0)   // 80094264, 8000A3BC: unit+0xA8 - unit+0x9C
c  = normalise(c); if the normalised length is ~0, c = (0,0,-1)                  // 80094128..8009415C
m  = (cap bit 0) ? player+0x1E4 : player+0x11C
c' = inverse(m) · c                                                              // 8040CF48 then 8040C6CC
θ  = atan2(c'.x, c'.z) - π                                                       // 80016260, 800941C4/80094228
out.x = v.x·cos θ + v.z·sin θ
out.z = -v.x·sin θ + v.z·cos θ                                                   // 8040E0FC; 803B2AFC=sin, 803B2594=cos
```
So with the stick up (s.y = -1) and camera yaw-flat, the output is exactly camera forward.

**Which camera vector, which frame:** the look-minus-eye of the framework unit's state at unit+0x9C. Only its yaw (in the
frame m) is used; the camera pitch has no effect on direction. Controller::update runs in group 7 (800AD840, Player ctor);
the camera update writes unit+0x9C (80270BF8) from the task `PJSCameraPostTask`, group 0x12 (801D0D30). Sibling tasks run in
creation order (8004F014 appends after the +0x14 list head; traversal 8004ECAC walks +0x14 then +0x0C next), and group 0x12 is created last (table
8053ED84: 6,7,8,9,A,B,C,E,D,F,10,11,12). So **the direction uses the previous frame's camera output.**
Note the gate: unit+0x9C is only updated if `unit+0xD0 == 0` (80270BF8..80270C0C); writers of +0xD0 UNKNOWN.

**Per state:**

| condition | input frame |
|---|---|
| normal play (bit 0x3A clear) | camera-relative (yaw of previous-frame camera in the ground/gravity frame) |
| bit 0x3A set: LineHang (8008B964 set, 8008B6A8 clear), DriveVehicle (8007EDB8 set, 8007EA1C clear) | stick axes directly in frame m, no camera |
| AccelerateInput / forced stick | set by the caller's vector, not the pad (not traced here) |

No character-relative frame is used for the steering vector. Forward-relative use exists only for *classification* (section 3 quantiser).

The command is delivered through Receive 800AF924 to Behaviour base 800A7CE4: id 1 sets bit 0x1B and adds `out` to player `+0xB8` when
|out|² ≥ 1e-4 (800A7D70..800A7DC8). The Executer clamps the accumulated `+0xB8` to length 1 (800A81A4..800A81C4), the state update
reads it, and the frame end zeroes it and clears bit 0x1B (800A827C..800A82A0). Input is one frame only; there is no stored intent.

## 3. Magnitude

The working value is `input` = player+0xB8, world space, |input| ≤ 1.

Stick-magnitude tests found anywhere in the player:

| test | meaning | instruction |
|---|---|---|
| axis |x|,|y| < 1e-4 | zero vector | 800A17B4..800A17D0 |
| |v|² ≥ 1e-4 | "stick active" (bit 0x1B) | 800A20F4, 800A7D84 |
| **|input|² < 0.81** (|in| < 0.9) | Ground run: "partial stick" | 800834C8 |
| |input|² > 0.25 (> 0.5) | LineHang starts responding | 8008AF48, 8008B200 |
| |input|² > 0.16 (> 0.4), result used later in the same update | WallHang (input read only when its timer +0x38 ≤ 0, 80091CF4..80091D18) | 80091D40..80091D50 |
| dot(forward, input) > 0.707 / < -0.707 | quantiser 80093F50 (input **not** normalised) | 80093F8C, 80093FAC |

Bit 0x1B is the only "has stick" flag: Idle → Ground (80086ECC), Landing → Ground (80088FCC), Landing zero-velocity (80088F48),
Fall/Jump turning, EdgeHang, Grind, Sliding (8008DCAC) and SpinDash (8008EFF0) only test it.

**Ground run body 8008331C** (acceleration only, `v` = velocity, `|v|`):
```
if (bit 0x1B clear)       run_acc = entry;                                     // 80082D8C
else if (|input|² < 0.81) run_acc = max(run_acc·0.9, min); slope field +0x34 = 360; // 800834C8..800834F8   per update, not dt
else {
    d = normalise(input); fwd = local forward; up = local up
    align = |dot(fwd, d)| (d projected off up first when dot(d,up) > 0)         // 8008354C..800835A8
    f3 = (|v| ≥ 30 && align ≥ 0.766) ? (align - 0.766)/0.234 : 0                // 800835C0..800835E8
    target = min + f3·(max-min)                                                 // 800835FC
    ramp = run_acc + align⁸ · dt · (max-min)·0.25                               // 80083608..80083630
    run_acc = min(ramp, target);  bit 0x3B (above 150 u/s): input = fwd·|input|, run_acc only rises
}
acc += input · run_acc                                                          // 80083700..8008371C, clamped to |acc| ≤ max
```
entry = min if |v| < 30 else min + min(|v|/750,1)·(max-min) (80082D8C). min 1000, max 15000 × char table (Shadow 1.0) or 3000 with bit
0x1E (80082E0C). Ground drag k = 20 (80093AA4), so terminal speed ≈ acc/20.

**Answers:**

- Magnitude scales acceleration **and** selects a branch. `|in|` multiplies acceleration directly at 800836FC (input·run_acc). Below 0.9 the
  ramp is replaced by `run_acc → min` decay, so the character walks at `≈ |in|·1000/20 = 50·|in|` u/s; at or above 0.9 it accelerates toward
  max with terminal `≈ |in|·750`. Below 0.9 a speed of 100 u/s or more can only be carried from inertia, not reached or held by the stick.
- At full deflection: full branch, |in| = 1, terminal 750. Partial deflection differs only through the two facts above; every other state
  uses magnitude only as a linear scale or a lower-bound test.
- **Nothing was found that requires partial deflection to be reachable.** Ottotto (8008BE90) reads neither input nor bit 0x1B; the Ground edge-catch 800971EC
  is gated on |v|² > 10000 (800972AC), i.e. speed, not stick. Walking is the only thing the partial branch gives. Tiptoe and aiming through the
  stick: no such path in the code traced; **UNCERTAIN** for weapons, see section 9.
- Raw-axis readers (not magnitude):
  - LineHang 8008AF00 (bit 0x3A is set, so these are stick axes in frame m), gated by |in|² > 0.25 or state flag: `input.z < -0.5` calls 800AABA0(+1), `input.z > 0.5` calls 800AABA0(-1) (8008AF94..8008B03C); `|input.x| > 0.5` applies `rot.y -= 4.5·dt·input.x` (8008B08C..8008B0B0). 800AABA0 is on the motion object +0x230; its meaning is UNCERTAIN (playback rate or direction).
  - 8009E738: raw `s.x·10, s.y·10` camera-relative, posted as a position-style command; virtual, no static caller; **UNCERTAIN** purpose.
- Quantiser 80093F50 returns 0 forward, 3 back, else 1 or 2 by cross-product side. On the stick: EdgeHang 8007F6FC (0 sets +0x24 = 1, 3 sets +0x24 = 2, which requests Fall at 8007F7AC), Grind 8008190C, LineHang 8008B218, 80206FC8. Its other callers, Jump enter 80087AB4, 8006C6D0 and 800B3818, pass a velocity-derived vector (804109E0 of velocity and +0xF4), not the stick.

## 4. Rotation and turning

`turn(rate)` 80093DE8: target = input if bit 0x1B set, else `perp(vel, ground normal)` if its |·|² ≥ 100 (805E5658, 80093E54). Applied by 80093E90: `yaw lerp by min(rate,1)`, target from atan2 scheme (8003BA10) + 180° (80410364 wraps angle).

| state | rate | instruction |
|---|---|---|
| Ground run, |v|² ≤ 22500 | 15·dt | 80083870..80083884 |
| Ground run, 22500 < |v|² < 90000 | (2 + 13·(|v|-300)/(-150))·dt | 800838A4..800838C4 |
| Ground run, |v|² > 90000 | 2·dt | 80083890..8008389C |
| Ground parallel body 80083138 | 15·dt, target camera-aligned 800942B0 | 800831E0, 80083220 |
| Fall | 15·dt | 80080A34 |
| Jump, JumpDash, Launch | 6.5·dt, 6.5·dt, 12·dt | 80087620, 80088508, 80089400 |
| GroundAttack | 2·dt | 80084310 |
| SuperJump dash | rate 1.0 (snap) | 800889DC |
| WallJump | 20·dt (bit 0x1B cleared just before: turns along velocity) | 80092B30 |
| Sliding, SpinDash | 2·dt | 8008DCC8, 8008F00C |

**Skid selector 80082F34** (called at 80083470 each Ground update): false if any of !bit 0x1B, bit 0x2E, |v|² < 40000 (200², 80082FB8), or
`dot(fwd, normalise(input)) > cos(90°)` (80083018). When true returns substate 2; on change runs skid body 80083048 once and ends the frame (8008347C..80083498). The angle
between input and facing is the only selector. No skid threshold is on stick magnitude.

Run target = `min..max` by `align`: `align` below 0.766 (40° off the forward axis) gives target `min`; it rises linearly to `max` at 0°, and the ramp rate is further weighted by `align⁸`. `align` is the absolute value of the dot, so facing away and facing toward give the same target (skid handles reversal first when it applies).

Above 150 u/s, the input direction is replaced by forward (`bit 0x3B`, 800836AC..800836E0), so at speed the player steers only by yaw (turn rate above); no strafing.

## 5. Rate of change and sign reversal

**None.** No code reads a previous or delta stick value in the player path:

- the controller object holds only the current values (section 1.2);
- UserInput reads only the current +0/+4 (800A17B4..800A17C4);
- the only readers of controller stick fields are 800A1790, 8009E738 (raw), and 80297D50/58 (TI);
- all state decisions are functions of the current frame `input`, `|input|`, velocity, and facing.

A sign reversal can only act through section 4's skid test, which compares input with facing, not with the previous input. Rate-based or smash behaviour: not present in the code traced.
Button edges (+0x20 pressed, +0x24 released) exist but are button-only.

A keyboard's 0-to-full transition therefore has no special meaning beyond being the same full value. Run acceleration is reset to the entry value each frame with no input
(80082D8C), which is speed-based, so a tap is not penalised beyond the frames without input.

## 6. Airborne, homing, grind

**Air accelerations** are `acc += input·K`, so linear in |in|:

| state | K | instruction |
|---|---|---|
| Fall | 220 | 80080998 |
| Jump | 220, 110 with bit 0x1E | 800875CC..800875D4 |
| JumpDash | 220 | 800884DC |
| Launch | 220 | 80089278 |
| Stream | 220 (no turn) | 8008FB24 |
| GroundAttack | 400 | 800842EC |

Jump enter tests bits 0x1B and 0x3C together (80087A24..80087A4C) and then picks an animation by the velocity-derived vector, not by the stick (80087A8C..80087AB4). The takeoff direction does not read `input`.

**Homing:** dispatch 800A0130 calls target search `+0x24C` 80206B98 (800A06C8) gated by character table byte +8 (804F2904+id·0x2C). HomingAttack (vf5 80085628) has no reads of `+0xB8` or bit
0x1B: **the direction is the target, not the stick**. JumpDash with no target: `vel`/`acc` use forward (`80079810`, 80088410), and input adds only `input·220` (800884E4). The stick does not aim the dash either.

The target search 80206FC8 uses input for a search-window offset when bit 0x42 set: `|input|` clamped ≤ 1 × a r13 constant (sign negated when quantiser returns 1), rate-limited (80207070..8020711C). **UNCERTAIN** whether this is on the homing path; constants are in BSS (runtime values, not file-backed).

**Grind:** no analog lean. Only side input used by 800811C4 → 800818CC: quantiser 80093F50; result 1 → (+1,0,0), result 2 → (-1,0,0) (local), posted as a rail-side change (800812F0); results 0/3 do nothing (8008190C..80081990). Needs bit 0x1B, with the input magnitude only through the quantiser's non-normalised dot.

## 7. Numeric answer for a keyboard

Engine normalisation, dead 15, range 42 (compute: section 1.1):

| raw (engine units) | normalised | note |
|---|---|---|
| |a| ≤ 15 | 0 | dead |
| 16..56 | (a-15)/42 | partial |
| ±36 | ±0.5 | |
| ±52 | ±0.881, |v|² 0.776 | highest "partial" on an axis |
| ±53 | ±0.905, |v|² 0.8186 | lowest full-branch axis |
| **\|a\| ≥ 57** | **±1.0 exactly** | circle clamp (a-15 ≥ 42): any larger value is also exactly 1.0 |

Diagonals (raw x = ±r, y = ±r, truncation in 1.1):

| \|r\| | axes each | magnitude | note |
|---|---|---|---|
| 42 | 27/42 = 0.643 | 0.909 (|v|² 0.8265) | lowest full-branch diagonal |
| 41 | 26/42 | 0.8755 | partial |
| 44, 47, 49, 52, 54, 57, 59, 61, 62, 64, 66, 67, ... | 29/42 | 0.97648 | truncated: not 1.0 |
| 45, 46, 48, 50, 51, 53, 55, 56, 58, 60, 63, 65, 68, 70, 75, 80, 85 | 30/42 | 1.01015 → renormalised to exactly 1.0 | the exact-length set |

Integer exactness comes from the rational √2 approximations: r=70 (55·42/77=30), 75 (60·42/84=30), 85 (70·42/98=30); use r = 70, 75 or 85.

Current keyboard (runtime/lift/peripheral/keyboard_layout.cpp, ±100 per axis; negative sample = raw +1):

| sample (x,y) | engine raw | normalised | magnitude |
|---|---|---|---|
| (100,0) (0,±100) etc | | ±1.0 | 1.0 |
| (100,100) | (100,-100) | (0.690,-0.690) | 0.97648 |
| (100,-100) | (100,99) | (0.714,0.690) | 0.99346 |
| (-100,100) | (-99,-100) | (-0.690,-0.714) | 0.99346 |
| (-100,-100) | (-99,99) | (-0.690,0.690) | 0.97648 |

So diagonals are currently above the 0.9 threshold but short of 1.0 and not symmetric: terminal ≈ 732 vs 750 u/s.

What to send:

- Send **full deflection**, not a threshold value. Any sub-0.9 value selects the walk branch, and a 2-axis digital input has no use for it; the only thing it adds is a walking speed (optional, see below).
- Cardinal: sample `+100` / `-100` are fine; anything ≥ 57 raw is exactly 1.0.
- Diagonal: to get exactly 1.0, send a value from the exact-length set, e.g. raw magnitude 75: sample **+75** and **-76** (the negative sample loses one unit). Result: (±0.714, ±0.714), renormalised to (±0.7071, ±0.7071).
- Do not pre-scale to a "circle of the live range": the engine's own clamp already does it, and pre-scaled values like raw 45 are also in the exact set but are lower than the clamp point for no gain.
- A walk modifier is optional and is the only use of partial values: sample giving norm 0.5 (raw 36) gives terminal ≈ 25 u/s; ≥ 0.9 (raw ≥ 53 on an axis) is already the run branch.
- Right stick has its own dead 25 / range 39; full for camera is raw ≥ 64 (axis), same diagonal logic applies with its numbers.

## 8. Constants

| value | hex | address (load instruction) | role |
|---|---|---|---|
| 15 | li 0xF | 80359768 | left dead |
| 25 | li 0x19 | 8035976C | right dead |
| 57 | subfic 0x39 | 80359780 | left range source: 57-dead |
| 64 | subfic 0x40 | 8035978C | right range source |
| 1.0 | 3F800000 | 805F3C98 (800A8A48, 800A8A64) | stick 2D length limit |
| 1e-4 | 38D1B717 | 805F3C9C (800A8A90) | idle timer reset |
| 1e-4 | 38D1B717 | 805F27B4 (8000A960) | per-axis "is zero" |
| 1e-4 | 38D1B717 | 805F3BA8 (800A20F4) | post command threshold |
| 1.0 | 3F800000 | 805F3BA4 (800A1820) | move vector length limit |
| 1e-4 | 38D1B717 | 805F3C84 (800A7D84) | id 1 accept |
| 1.0 | 3F800000 | 805F3C6C (800A81B0) | accumulated input clamp |
| 10.0 | 41200000 | 805F3BAC (800A1D08) | idle timer gate, player+0x5C==2 path |
| 180, π | 43340000, 40490FDB | 805F3A50, 805FBDB0 | yaw offset in 800940C0 |
| 40000 | 471C4000 | 805F371C (80082FB8) | skid |v|² gate |
| 90 | 42B40000 | 805F3720 (80083008) | skid angle |
| 0.81 | 3F4F5C28 | 805F3738 (800834C8) | partial stick |in|² |
| 0.9 | 3F666666 | 805F373C (800834D8) | partial decay per update |
| 360 | 43B40000 | 805F3740 (800834F8) | partial slope field |
| 30 | 41F00000 | 805F3708 (800835C0, 80082DAC) | low-speed gate |
| 0.766 | 3F441893 | 805F3744 (800835CC) | align floor |
| 0.234 | 3E6F9DB4 | 805F3748 (800835E4) | align range |
| 0.25 | 3E800000 | 805F374C (8008360C) | ramp fraction |
| 1000 | 447A0000 | 805F3710 | min run acc |
| 3000 | 453B8000 | 805F3714 | max with bit 0x1E |
| 15000 | 466A6000 | 805F3718 | max base (× char factor, 804F2904+0x2C·id+4) |
| 750 | 443B8000 | 805F370C | entry-acc speed normaliser |
| 20 | 41A00000 | 805E564C (80093AA4) | ground drag |
| 15 | 41700000 | 805F372C (800838..) | Ground turn low speed |
| 22500 | 46AFC800 | 805F3758 | turn band / bit 0x3B squared speed |
| 90000 | 47AFC800 | 805F375C | turn band |
| 2, 300, -150, 13 | | 805F3760, 805F3768, 805F376C, 805F3764 | Ground turn band formula |
| 62500 | 47742400 | 805F3790 (80083B8C) | Ground gravity-on-slope speed² |
| 10 | 41200000 | 805E5658 (80093E54) | turn fallback velocity |
| 10000 | 461C4000 | 805F3AA0 (800972AC) | edge catch speed² |
| 0.707, -0.707 | 3F34FDF4, BF34FDF4 | 805F3A70, 805F3A74 | quantiser forward/back |
| 220 | 435C0000 | 805F3698, 805F3854, 805F3888, 805F39B8 | air accel Fall, JumpDash, Launch, Stream |
| 220, 110 | 435C0000, 42DC0000 | 805F3824, 805F3820 | Jump accel |
| 15, 6.5, 6.5, 12 | | 805F369C, 805F3828, 805F3858, 805F3894 | air turn rates |
| 400, 2 | 43C80000, 40000000 | 805F37A0, 805F37A4 | GroundAttack accel, turn |
| 20 | 41A00000 | 805F3A28 | WallJump turn |
| 1.0 | 3F800000 | 805F3860 | SuperJump dash turn |
| 0.25 | 3E800000 | 805F38D8 | LineHang |in|² |
| ±0.5 | | 805F38D4, 805F38DC | LineHang raw axis |
| 4.5 | 40900000 | 805F38E8 | LineHang yaw rate |
| 0.16, 0.7071 | 3E23D70B, 3F3504F3 | 805F39F0, 805F39D8 | WallHang |

## 9. Unknowns

- Whether weapon or gun aiming reads the left or right stick by another route. The controller's stick fields are read only by the sites listed in section 5, and
  the player `+0xB8` reader list (section 3, 1.3) has no weapon function in it; but a search on the offset alone is not exhaustive for weapon objects. **UNCERTAIN.**
- Writers of camera unit +0xD0 (gate for the +0x9C copy): UNKNOWN. If set, the steering camera stops updating.
- 80206FC8's role in homing and its constants (BSS): UNCERTAIN / UNKNOWN value.
- Game-side names of commands 0x124/0x125 and bit 0x3C beyond the proven behaviour: UNCERTAIN ("parallel move", from the package).
- Bit 0x2A meaning beyond the gate; byte 0x8057E760+0xC8 meaning: UNKNOWN.
- 8009E738's purpose and caller (virtual, no static caller): UNCERTAIN.
- Pad singleton 80576DF4 (8005185C) fields f32 +0x38/+0x3C/+0x44/+0x48: UNKNOWN. The stick pipeline of this document uses the manager 0x8056FE0C (80009208) records, not this object; the +0x11C port-map word (800A8B28) is the only singleton field used by the player controller.
- Matrix `m` (player+0x1E4 / +0x11C) is built from ground / gravity normals by code not re-read here; both identity on flat ground is expected, not proven here.
- Drag k=20 applies under flag bit 2 (80093A8C); the state flags that select it are from the package, not re-read. Terminal-speed numbers depend on it.
- Partial-stick walking effect in other states than Ground (e.g. Grind, air): only linear scaling, as listed; none with a threshold beyond section 3's table.
