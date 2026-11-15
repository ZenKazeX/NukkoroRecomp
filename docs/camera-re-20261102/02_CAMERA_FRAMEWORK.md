# SonicteamUSA::System::Camera - the generic camera framework (2026-11-02)

Read-only disassembly of PAL main.dol (`python tools/doldis.py`). Every claim cites an instruction (or data)
address. Names from symbols.txt / names.json are used only as labels; behaviour comes from the code.
`UNKNOWN` = not established by the code read for this document; `UNCERTAIN` = partly established, reason given.

Conventions in the pseudocode:
- `f32` arithmetic is single precision unless marked `(double)`. `fmadds a*b+c` is ONE rounding (fused);
  it is written `fma(a,b,c)` and must be reproduced fused. Plain `a*b+c` means two roundings (fmuls, fadds).
- Constants are given as `value [bits] @address`. r2 = 0x805FA780, r13 = 0x805EC500.
- `Vec3` = 3 consecutive f32 (x,y,z). `Mtx34` = 12 f32, row-major 3 rows x 4 columns (`m[r][c]` at
  `(4*r+c)*4`, accessor 0x8040D1CC: `index = r4*4 + r5`).

---------------------------------------------------------------------------------------------------------------

## 0. Summary (what reproduces the camera)

1. Each frame the task `PJSCamera::PJSCameraPostTask` (update 0x801D0B0C) calls
   `CameraManager::update(dt)` (0x8000D170), which calls `CameraManagerUnit::update(dt)` (0x80270A0C) for every
   unit whose enable byte is non-zero. There are exactly 2 units (0x8000D208..0x8000D24C); unit 0 is enabled at
   init (0x8000D258 -> 0x8000D29C), unit 1 is not.
2. A unit holds a priority-sorted list of camera requests. The FRONT request (highest priority) is the active
   one. Its mode's `update(dt, request)` (vtable +0x0C) produces a `CameraState` (eye, look, roll, up, AOV pair).
3. If the front request owns a `CameraAdjust`, the adjust blends from the camera output captured when that
   request became front (unit+0x08 at that time) to the mode's state: eye/look/roll by the subclass (e.g.
   `AdjustLinerWorld`: linear, weight = elapsed/duration), up by an axis-angle rotation, AOV pair linearly.
4. An optional `CameraShake` adds `up * amplitude * triangleWave` to eye and look.
5. The result is stored at unit+0x08 (CameraState), a world matrix at unit+0x6C (columns right, up, forward,
   eye; roll applied) and a look-at view matrix at unit+0x3C.
6. The RenderWare bridge (0x80271F9C) reads unit+0x6C (frame matrix) and the AOV pair at unit+0x08+0x1C
   (radians), multiplies the AOV pair by a viewport factor, and sets `RwCameraSetViewWindow(tan(h/2), tan(v/2))`.

The two AOV floats are radians: horizontal at +0x1C and vertical at +0x20 (see section 2.1 for the proof).
There is NO near/far in the framework state; the bridge sets near 1.0 / far 30000 on the RwCamera
(0x80271C98 / 0x80271CA4).

---------------------------------------------------------------------------------------------------------------

## 1. Math primitives used (exact operations)

All in the game's own small vector library (not the Dolphin SDK), or MSL.

| address | operation (r3 = out unless noted) |
|---|---|
| 0x800091D0 / 0x800091EC | Vec3 copy `*r3 = *r4` |
| 0x8040DDC4 | `out = a(r4) - b(r5)` componentwise fsubs |
| 0x8040DE20 | `out = a + b` (fadds) |
| 0x8040DE54 | `a(r3) -= b(r4)` |
| 0x8040E048 | `a(r3) += b(r4)` |
| 0x8040DDF8 | `out = v(r4) * s(f1)` (3 fmuls) |
| 0x8040DE88 | `v(r3) *= s(f1)` |
| 0x8040DFA0 | `len(v) = sqrtf(z*z + (x*x + y*y))` : `t=x*x; u=y*y; t=t+u; w=z*z; sqrtf(w+t)` (0x8040DFA0..0x8040DFBC) |
| 0x8040DFE0 | `dot(a,b) = fma(a.z,b.z, fma(a.x,b.x, a.y*b.y))` |
| 0x8040E008 | `cross(out,a,b)`: `x = fms(a.y,b.z, a.z*b.y)`, `y = fms(a.z,b.x, a.x*b.z)`, `z = fms(a.x,b.y, a.y*b.x)` (`fms(p,q,r)=p*q-r` fused, fmsubs) |
| 0x8040DEC4 | normalize in place: `l = len(v)`; if `l < 1e-6 [0x358637BD]@0x805FBDFC` then `v = zero(0x805E1860)` and return `0 @0x805FBDF8`; else `v.x/=l; v.y/=l; v.z/=l` (3 fdivs), return l |
| 0x8040DF38 | normalized copy (copy, 0x8040DEC4, copy back) |
| 0x8000FEE4 -> 0x8000FF04 | MSL sqrtf: for x>0: `y=frsqrte(x)`, three double Newton steps `y = (0.5*y)*(3 - x*y*y)` (constants 0.5 / 3.0 doubles @0x805F2868 / 0x805F2870, fnmsub), result `frsp(x*y)`. Reference implementation: `game_sqrtf` in runtime/lift/motion/pose_math.h. x<=0 branch (0x8000FF5C..) not traced here. |
| 0x8000A9A0 | `fabsf` (fabs, frsp) |
| 0x8000A950 | `isZero(x) = fabsf(x) < 1e-4 [0x38D1B717]@0x805F27B4` (fcmpo, CR LT bit) |
| 0x80031568 | `sincos(a, &s, &c)`: `s = frsp(sin(a))` (double sin 0x803B2AFC), `c = frsp(cos(a))` (0x803B2594) |
| 0x8000FFE8 / 0x8000F3B0 / 0x8000F3D4 / 0x8000F3F8 / 0x80016260 / 0x801B2150 | sinf / cosf / tanf / atanf / atan2f(y=f1,x=f2) / acosf (MSL double then frsp; runtime/lift/math/msl.h) |
| 0x803B2668 | MSL `floor` (double) |
| 0x8040C188 | `RotY(a)`: rows `(c,0,s,0) (0,1,0,0) (-s,0,c,0)` (1.0/0.0 @0x805FBDB8/0x805FBDBC) |
| 0x8040C200 | `RotX(a)`: rows `(1,0,0,0) (0,c,-s,0) (0,s,c,0)` |
| 0x8040C898 / 0x8040C8D4 | `M = M * RotY(a)` / `M = M * RotX(a)` (post-multiply via 0x8040CB14) |
| 0x8040CB14 | `M = A * B` (A = copy of M); element `= fma(A[r][2],B[2][c], fma(A[r][0],B[0][c], A[r][1]*B[1][c]))`, column 3 adds `A[r][3]` last (fadds) |
| 0x8040C6CC | `out = M * v` : `out.r = M[r][3] + fma(M[r][2],v.z, fma(M[r][0],v.x, M[r][1]*v.y))` |
| 0x8040D198 | Mtx34 copy (12 words) |
| 0x8040C38C | `LookAt(m, eye(r4), up(r5), target(r6))`: `L = normalize(eye - target)`; `R = normalize(cross(up, L))`; `U = cross(L, R)` (not normalized); rows `(R, -dot(eye,R))`, `(U, -dot(eye,U))`, `(L, -dot(eye,L))` (dot = 0x8040DFE0 with eye as first operand, then fneg) |
| 0x800498C8 | axis-angle matrix (axis r4, angle f1) used for ROLL: `s=sinf(a), c=cosf(a), t = 1.0[0x3F800000]@0x805F2D20 - c`; `m00=fma(t,x*x,c)`, `m10=t*(x*y)+z*s`, `m20=t*(z*x)-y*s`, `m01=t*(x*y)-z*s`, `m11=fma(t,y*y,c)`, `m21=t*(y*z)+x*s`, `m02=t*(z*x)+y*s`, `m12=t*(y*z)-x*s`, `m22=fma(t,z*z,c)`, `m[i][3]=0 @0x805F2D24`. Note `t*(x*y)` is computed once (x*y first, then *t) and reused for m10/m01 (same for the others). |
| 0x80410DDC | axis-angle matrix used by CameraAdjust (same formula, same operation order; 1.0 / 0.0 from 0x8051B64C / 0x8051B648). The axis is used as given (no normalisation inside). |
| 0x8041094C | `angleBetween(a,b) = acosf(clamp(dot(normalize(a), normalize(b)), -1 @0x8051B650, 1 @0x8051B64C))` (clamp: `if d > 1: d=1 else if d < -1: d=-1`) |
| 0x8003BA10 | `dirAngles(v, &pitch, &yaw)`: `yaw = atan2f(v.x, v.z)`; if `fabsf(v.x) > fabsf(v.z)`: `k = sinf(yaw)`, `pitch = atan2f(-v.y*k, v.x)` else `k = cosf(yaw)`, `pitch = atan2f(-v.y*k, v.z)`; then if `k < 0 @0x805F2B98`: `pitch += (pitch >= 0) ? -pi : +pi` (pi = 0x40490FDB @0x805FBDB0) |
| 0x8000AB90 | `deg2rad(d) = d * fdivs(pi [0x40490FDB]@0x805FBDB0, 180.0 [0x43340000]@0x805F27C8)` (pi/180 = 0x3C8EFA35, then fmuls) |

---------------------------------------------------------------------------------------------------------------

## 2. Data layouts

### 2.1 CameraState (0x34 bytes) - the value every stage passes around

Default ctor 0x8000AA70; ctor(eye*,look*,roll,aov*) 0x8000A9E8; assignment 0x8000B074; dtor 0x8000A9AC.

| off | type | meaning | evidence |
|---|---|---|---|
| +0x00 | Vec3 | eye (camera position) | LookAt takes it as eye (0x8000A69C..0x8000A6AC r4 = state); world-matrix column 3 (0x8000A704..0x8000A724); look - eye = forward (0x8000A3BC) |
| +0x0C | Vec3 | look-at target | 0x8000A6A8 (`r6 = state+0xC` = LookAt target); 0x8000A3BC computes `state+0xC - state` |
| +0x18 | f32 | roll, radians | rotation angle `-roll` about the forward axis (0x8000A2DC..0x8000A2F0); skipped when isZero(roll) (0x8000A8A8..0x8000A8B4) |
| +0x1C | f32 | horizontal angle of view, radians | see below |
| +0x20 | f32 | vertical angle of view, radians | see below |
| +0x24 | Vec3 | up reference vector | 0x8000A824 (copied as the up input of the basis); default (0,1,0) |
| +0x30 | u32 | UNKNOWN (set 0 by 0x8000AAEC; copied by assignment 0x8000B0C8 -> 0x8000B21C). No reader found in the framework. |

AOV pair proof: 0x8000F41C (the only producer from a single angle, called by 0x8000A3F0 = "set mode AOV"):
```
// 0x8000F41C(out, a)
k   = pi / 180.0f                       // 0x8000F434..0x8000F444 (180 [0x43340000]@0x805F2830, pi @0x805FBDB0)
lim = 180.0f * k                        // 0x8000F448
if (a > lim) a = 179.0f [0x43330000]@0x805F282C * k        // 0x8000F44C..0x8000F458
t   = tanf(0.5f [0x3F000000]@0x805F2834 * a)               // 0x8000F45C..0x8000F464
t   = t / K                             // K = f32 @0x805EBA30 (r13-0xAD0), file value 1.31958771 [0x3FA8E840]
out[0] = a                              // -> +0x1C
out[1] = 2.0f [0x40000000]@0x805F2838 * atanf(t)            // -> +0x20
```
So `+0x20 = 2*atan(tan(+0x1C / 2) / K)`: +0x1C is the horizontal AOV and +0x20 the vertical one for
aspect K = width/height. The bridge converts them with `tan(0.5*x)` (0x8000F860) into the RW view window
(section 9). The text search of the lifted C++ found only loads of 0x805EBA30 (3, all in 0x8000F2E8 /
0x8000F41C); no store was found, so K appears constant (UNCERTAIN: search was textual).
A second converter 0x8000F2E8 (one caller, 0x800A60AC) treats its input as a DIAGONAL angle:
`th = atanf(1.0 @0x805F2828 / K)`, clamp as above, `t = tanf(0.5*a)`, `h = 2*atanf(t*cosf(th))`,
`v = 2*atanf((t*cosf(th))/K)` (0x8000F308..0x8000F388).

Defaults (static at 0x8056FEE8, filled by __sinit 0x8000AAF4; AOV pair at r13+0x2B48 = 0x805EF048):
- eye = (0, 20, 20): 0 [0x00000000]@0x805F27AC, 20 [0x41A00000]@0x805F27B8 (0x8000AB10..0x8000AB30)
- look = (0, 0, 0) (0x8000AB34..0x8000AB3C)
- up = (0, 1, 0): 1 [0x3F800000]@0x805F27C4 (0x8000AB58..0x8000AB70)
- roll = 0 (0x8000AAA0)
- AOV h = deg2rad(63.2501488 [0x427D0027]@0x805F27C0) = 1.1039233 [0x3F8D4D5C]; AOV v =
  deg2rad(53.1300964 [0x42548538]@0x805F27BC) = 0.9272951 [0x3F6D6336] (0x8000AB28..0x8000AB64; deg2rad
  0x8000AB90: `d * fdivs(pi, 180.0 [0x43340000]@0x805F27C8)`, pi/180 = 0x3C8EFA35; result bits computed
  in float32 from those two operations).

A 0x24-byte "parameter" form {eye, look, roll, aovH, aovV} exists: 0x8000B3B4 builds a CameraState from it
(r4+0x00 eye, +0x0C look, +0x18 roll, +0x1C/+0x20 AOV) with up = default up.

### 2.2 CameraMode (base, 0xAC bytes; vtable 0x8051D6B8, vptr at +0xA8)

ctor 0x8000E9BC(this, const ModeParam* init /*may be null*/, int unitIndex); dtor 0x8000E954.

| off | type | meaning | evidence |
|---|---|---|---|
| +0x00 | CameraState | the mode's output state (default-constructed) | 0x8000E9E4 bl 0x8000AA70; unit update copies mode+0 as the state (0x80270BB0..0x80270BB8) |
| +0x34 | ModeParam (0x6C) | parameter block, ctor 0x8000EBB0, copied from `init` when non-null (0x8000EA0C -> 0x8000EA28, 0x6C bytes) | 0x8000E9E8..0x8000EA0C |
| +0xA0 | s32 | camera-manager unit index this mode belongs to | 0x8000E9F0; used as unit index by 0x8026F8F4, 0x8026FC90, 0x8026FCE8 |
| +0xA4 | s32 | activation count | 0x8000E9FC = 0; inc/dec 0x8000E8D8 / 0x8000E88C |
| +0xA8 | vptr | | 0x8000E9E0 |

ModeParam (+0x34 of the mode), ctor 0x8000EBB0 -> 0x8000B4AC: +0x00 eye (default 0,20,20), +0x0C look
(default 0), +0x18 roll (0 [0]@0x805F27A8), +0x1C/+0x20 AOV pair (defaults above); +0x24 u32 = 0, +0x28 u32
= 0, +0x2C 8 bytes 0 (memset), +0x34..+0x48 six f32 = 0 [0]@0x805F27F8, +0x4C 0x20 bytes 0 (0x8000EBC8..
0x8000EC0C). Alternate ctor 0x8000EB34(this, eye*, look*, roll, aov*, u32 r7 -> +0x24). The framework code
read here does not read ModeParam fields after construction (UNKNOWN who does; PJSCamera modes are the
candidates).

Vtable slots (offsets from the vtable address; slot0 at +0x08):

| slot (+off) | base | role (from callers) |
|---|---|---|
| 0 (+0x08) | pure | returns the mode type id (MtnViewer -0x3FF 0x80008D40, Stop -0x400 0x800092CC, User -0x3FE 0x80009610) - same numbers the factory registers them under (0x8000BDC4/0x8000BDDC/0x8000BDF8). No virtual caller of +0x08 found in the framework. |
| 1 (+0x0C) | pure | `update(f32 dt, CameraRequest* req)` - called 0x8000E934..0x8000E940 (front request) and 0x80270B30..0x80270B3C (background requests) |
| 2 (+0x10) | pure | `onActivate(CameraRequest*)` - 0x8000E8F8 when the activation count goes 0 -> 1 |
| 3 (+0x14) | pure | `onDeactivate(CameraRequest*)` - 0x8000E8B8 when the count goes 1 -> 0 |
| 4 (+0x18) | 0x80008D48 (blr) | UNKNOWN; no caller in the framework code read |
| 5 (+0x1C) | 0x8000E954 | destructor (called with r4 = 1 by the mode deleter 0x8026FD14..0x8026FD28) |

### 2.3 CameraRequest (0x18 bytes; held by boost::shared_ptr with deleter 0x8026FC6C)

ctor 0x8000ED58(this, shared_ptr<CameraMode>* mode, s32 prio, s32 adjType, AdjustParam* adjParam, u32 flags);
dtor 0x8000ECDC.

| off | type | meaning | evidence |
|---|---|---|---|
| +0x00 | CameraMode* | mode (shared_ptr pointer word) | 0x8000ED7C (shared_ptr copy 0x80009804) |
| +0x04 | sp_counted_base* | shared_ptr count of the mode | 0x80009804 |
| +0x08 | s32 | priority (list order, higher first) | 0x8000ED80; compare 0x8000EC28 |
| +0x0C | s32 | adjust type id (factory key) | 0x8000ED84 |
| +0x10 | CameraAdjust* | adjust instance or null | 0x8000EDA4 = factory->createAdjust(type, adjParam) (0x8000BAB4) |
| +0x14 | u32 | flags; bit 0x2 = "mode stays active/updated while this request is not front" | tested 0x80270B14, 0x802703B4, 0x80270410, 0x80270684, 0x802706D4. Other bits UNKNOWN (callers pass 0 or 1, e.g. 0x801CA364 li r8,1). |

Priority compare 0x8000EC28(a, b) returns `a->prio >= b->prio` (signed; srawi/srwi/subfc/adde idiom
0x8000EC28..0x8000EC3C).

`setAdjust(type, param)` 0x8000EC44: if an adjust exists and `adjust->state == 1` (blending) return 0
(0x8000EC6C..0x8000EC7C); else destroy the old adjust (vtable +0x1C, r4 = 1), store type at +0x0C, create
the new one at +0x10, return 1.

Pause gate: in the unit update, if byte 0x805EF27C (r13+0x2D7C) is non-zero and `0 <= prio <= 0x400`
(0x80270A7C..0x80270A98), the whole unit update returns immediately (nothing is updated, no output change,
no velocity update). The byte is set to 1 by 0x801EB674 (called 0x801E3360 inside
`TPauseManager__update__sub4` 0x801E3298, and 0x801E4208) and cleared by 0x801EB668 (0x801E4214). Requests
with prio < 0 or > 0x400 keep running during pause (example: prio 0x401 at 0x8009EB44).

### 2.4 CameraAdjust (base 0xB0 bytes; vtable 0x8051D618, vptr at +0xAC)

ctor 0x8000B158(this, const AdjustParam* p); dtor 0x8000B0E4; start 0x8000AF74; update 0x8000ADD0.

| off | type | meaning | evidence |
|---|---|---|---|
| +0x00 | s32 | state: 0 constructed, 1 blending, 2 finished | 0x8000B18C, 0x8000AFB8, 0x8000AF3C |
| +0x04 | f32 | duration A (eye/look/roll/up) | p+0 (0x8000B164/0x8000B194); used 0x8000AE34, 0x8000AE48 |
| +0x08 | f32 | duration B (AOV pair) | p+4 (0x8000B170/0x8000B198); used 0x8000AEC4, 0x8000AEDC |
| +0x0C..+0x2F | | rest of AdjustParam copied word by word (p+8..p+0x2C) | 0x8000B180..0x8000B1D8; not read by the base (UNKNOWN use) |
| +0x30 | CameraState | start state (output captured at start) | 0x8000AF9C..0x8000AFA0 |
| +0x64 | CameraState | current blended state (returned by update) | 0x8000AF90; 0x8000AF40 |
| +0x98 | f32 | elapsed time | 0x8000AE08..0x8000AE18; reset 0x8000AFAC |
| +0x9C | f32 | weight A | 0x8000AE40, 0x8000AE5C |
| +0xA0 | f32 | weight B | 0x8000AED4, 0x8000AEF0 |
| +0xA4 | s32 | state A (1 active, 2 done) | 0x8000AE1C, 0x8000AE94, 0x8000AFB4, 0x8000AFE4 |
| +0xA8 | s32 | state B | 0x8000AEAC, 0x8000AF0C, 0x8000AFB0, 0x8000B010 |
| +0xAC | vptr | | 0x8000B184 |

AdjustParam (0x2C bytes, at CameraAdjust+0x04): `{f32 durA; f32 durB; u32 +8; u8 +0xC[16]; u8 +0x1C[16]}`.
Builders: 0x8000B264(durA=f1, durB=f2), 0x8000B2C0(durA=durB=f1), 0x8000B31C(both = 0 [0]@0x805F27E0);
each sets +8 = 0 and memsets the two 16-byte blocks (0x8000B264..0x8000B2A4).

Vtable (slot0 at +0x08):

| slot (+off) | base | role |
|---|---|---|
| 0 (+0x08) | pure | type id (LinerWorld returns 1 0x801CFEC8, LinerLocal 2 0x801CFAF0, OnlyAov 3 0x801D006C, CosRot 4 0x80359A28) |
| 1 (+0x0C) | pure | `begin(CameraState* startState, CameraMode* mode)` - called at the end of start (0x8000B01C..0x8000B02C) |
| 2 (+0x10) | pure | `blendEyeLookRoll(f32 dt, CameraState* target)` - writes +0x64 eye/look/roll (0x8000AE74..0x8000AE84) |
| 3 (+0x14) | 0x8000ADC8 | `weightA(t, dur) = t / dur` (fdivs) |
| 4 (+0x18) | 0x8000ADC0 | `weightB(t, dur) = t / dur` |
| 5 (+0x1C) | 0x8000B0E4 | destructor |
| 6 (+0x20) | 0x8000ABA4 (blr) | UNKNOWN |

### 2.5 CameraShake (0x18 bytes; shared_ptr deleter vtable 0x8051D65C)

| off | type | meaning | evidence |
|---|---|---|---|
| +0x00 | f32 | duration (< 0 = endless) | 0x8000EE10 / 0x8000F2C4; test 0x8000EF84..0x8000EF90 |
| +0x04 | f32 | amplitude | table / f2; multiplies the up vector 0x8000F09C, 0x8000F198 |
| +0x08 | f32 | frequency (cycles per unit of elapsed) | 1/freq = period (0x8000F00C); isZero -> finished 0x8000EF6C |
| +0x0C | f32 | elapsed | 0 at ctor (0x8000EE28 / 0x8000F2CC); += dt 0x8000EFF8 |
| +0x10 | s32 | sampling mode 0/1/2 | 0x8000F018..0x8000F044 |
| +0x14 | s32 | step / phase quadrant 0..3 | 0x8000F080..0x8000F168, 0x8000F1CC.. |

Constructors: 0x8000EDD0(f1 duration, r4 type): amplitude/frequency from the table at 0x804AB620 + type*8
(0x8000EE04..0x8000EE2C): type 0 = (0.6 [0x3F19999A], 7.0 [0x40E00000]), type 1 = (1.5 [0x3FC00000], 7.0),
type 2 = (3.0 [0x40400000], 7.0). The table is followed by a string at 0x804AB638, so 3 entries; the index
is not range-checked. 0x8000F2C4(f1 dur, f2 amp, f3 freq) sets all three explicitly.

### 2.6 CameraManagerUnit (0x144 bytes; one per screen camera; shared_ptr deleter vtable 0x8051D680)

ctor 0x802700F0(this, s32 index); dtor 0x8026FD40; update 0x80270A0C. Allocated 0x144 bytes at 0x8000D208.

| off | type | meaning | evidence |
|---|---|---|---|
| +0x000 | u8 | enabled (update runs only if non-zero) | 0x80270108; test 0x8000D1A0; set 1 0x8000D29C; cleared 0x801CEC04 |
| +0x004 | s32 | unit index | 0x80270110 |
| +0x008 | CameraState | FINAL camera output of this frame | 0x80270BEC..0x80270BF4 |
| +0x03C | Mtx34 | view matrix = LookAt(eye, rolledUp, look) | 0x80270C28..0x80270C3C; identity at ctor (0x80270118..0x80270124, identity 0x805E17E0 built by 0x8040D1E0) |
| +0x06C | Mtx34 | camera world matrix, columns (right, up, forward, eye) | 0x80270C10..0x80270C24; identity at ctor |
| +0x09C | CameraState | copy of the output while +0xD0 == 0 | 0x80270BF8..0x80270C0C |
| +0x0D0 | u32 | gate for the +0x9C copy (0 at ctor 0x80270148); writers UNKNOWN |
| +0x0D4 | list | non-owning references to every CameraMode created for this unit (added 0x802708E8 from createMode 0x8026FB0C; expired entries removed 0x80270810 from the mode deleter) |
| +0x0E0 | list | non-owning references to CameraRequests, sorted by priority (section 4) |
| +0x0EC | shared_ptr<CameraShake> | active shake (ptr at +0xEC, count at +0xF0) | 0x80270BBC; reset 0x80270BE8 / 0x8000D554 |
| +0x0F4 | CameraTargetInfo* | created by factory vtable slot0 (0x80270180..0x80270198) |
| +0x0F8 | CameraState | previous frame output | 0x80270CE0..0x80270CE8 (init 0x8027016C, a PJSCamera-side CameraState ctor 0x80359FC4, UNCERTAIN only in that the ctor body was not read) |
| +0x12C | Vec3 | eye velocity `(eye - prevEye) / dt` | 0x80270C58..0x80270CD0 |
| +0x138 | Vec3 | copy of the first 12 bytes of *TargetInfo (its position) | 0x80270CD4..0x80270CDC |

List layout (+0xD4 and +0xE0, std::list-like): `+0 size` (0x80270530, empty test 0x802707E4), `+4` the
sentinel node; begin = sentinel.next (0x802700B8: `*(list+4+4)`), end = list+4 (0x8027007C), node layout
`+0 prev, +4 next, +8 {T* ptr, count*}`; `++it` = `it = it->next` (0x80270558). An entry does not keep its
object alive: 0x80270450 yields an empty pointer when the entry is expired (0x8027056C -> 0x800169AC), and
the request deleter runs while its entry is still listed (0x802702BC finds the expired entry).

### 2.7 CameraManager (singleton object at 0x8056FED4; accessor 0x80009548, pointer cached at r13+0x2B34)

ctor 0x8000DC68; static dtor 0x8000DB9C; init 0x8000D1E0; update 0x8000D170.

| off | type | meaning | evidence |
|---|---|---|---|
| +0x00 | CameraFactory* | set once by init | 0x8000DC80 (=0), 0x8000D200 |
| +0x04 | vector<shared_ptr<CameraManagerUnit>> | `+4 size`, `+8 data` (8 bytes per element), `+0` UNKNOWN (zeroed 0x8000DDC4..0x8000DDD4; reserve(2) 0x8000DC9C) | 0x8000D0B4, 0x8000D0E8; `at(i)` with range abort 0x8000DFD0 |

Init 0x8000D1E0(mgr, factory) (only caller 0x801D0E5C in the PJSCamera singleton ctor 0x801D0E18, which
creates the PJSCameraFactory at 0x801D0E40): if `mgr->factory == 0`: store it, create units 0 and 1
(0x8000D204..0x8000D24C) and enable unit 0 (0x8000D250..0x8000D258).

The ctor also creates the RW-bridge singleton 0x8056FF1C (0x8000DCA0 -> 0x8000DCC0) and its 5
`CameraScreen` objects (0x8000DCA4 -> 0x802721A4: loop 0..4, `new 0xC`, ctor 0x802730A8(this, i), pushed into
singleton+0x14).

### 2.8 CameraFactory (vtable 0x8051D640, vptr at +0x00; PJSCameraFactory is 0x24 bytes)

ctor 0x8000BD6C; dtor 0x8000BB44. `+0x04` map<s32, {name, creator, extra}> of modes (insert 0x8000B904),
`+0x14` map<s32, {name, creator}> of adjusts (insert 0x8000B76C). Lookups 0x8000B63C / 0x8000B50C return
`&value` or 0 when the key is absent (0x8000B548..0x8000B558).
- `createMode(type, init, unit)` 0x8000BB00: `creator(init, unit)` (no null check, 0x8000BB1C..0x8000BB2C).
- `createAdjust(type, param)` 0x8000BAB4: `creator ? creator(param) : 0` (0x8000BACC..0x8000BAE8).
- vtable slot0 (+0x08): pure in the base; PJSCameraFactory 0x801D079C returns `new PJSCameraTargetInfo`
  (0x54 bytes, ctor 0x80290710).

Base registrations (0x8000BDAC..0x8000BE00): -0x3FF "MtnViewer" (0x804AB324) creator 0x80008D4C; -0x400 "Stop"
(0x805F27E8) creator 0x800092D4; -0x3FE "User" (0x805F27F0) creator 0x80009894.
PJSCameraFactory adds (0x801D0858..0x801D09D4, for reference): modes -1 "Null" (creator 0), 0 "Command"
(creator 0), 1 Free, 2 TestPlayer, 3 TestPlayerNoRot, 5 SetEditor, 6 SetCamEdit, 7 FixEye, 8 FixLook,
10 General, 11 LookFront, 12 FixEye2, 13 FixLook2, 14 FixLookChar; adjusts (0x801D09D8..0x801D0A5C):
-1 "Void" and 0 "None" with creator 0 (=> no adjust, hard cut), 1 "LinerWorld" 0x801CFED0, 2 "LinerLocal"
0x801CFAF8, 3 "OnlyAov" 0x801D0074, 4 "CosRot" 0x80359A30.

### 2.9 CameraTargetInfo (vtable 0x8054F780, vptr at +0x3C)

ctor 0x80287310 only stores the vptr; dtor 0x802872C8. Base vtable: slot0 (+0x08) pure, slot1 (+0x0C) pure,
slot2 (+0x10) destructor (called 0x8026FD6C..0x8026FD7C). The unit copies `*(Vec3*)TargetInfo` every frame
(0x80270CD4). The subclass PJSCameraTargetInfo (0x54 bytes) resets +0x00 and +0x0C to the zero vector and
fills +0x18/+0x24/+0x30 with constant vectors (0x802905D8); its meaning belongs to the PJSCamera layer.

### 2.10 CameraScreen (0xC bytes, 5 instances in the bridge singleton; shared_ptr deleter vtable 0x8054DDAC)

ctor 0x802730A8(this, s32 i): `+0 = i`, `+4/+8` raster handles: for i != 0 two rasters are created
(0x8048AEC4, types 2 and 1) from the viewport table entry 0x8058516C + i*0x30 (+8, 16 bytes) and the
singleton's rasters; for i == 0 they alias singleton+4 / singleton+0xC (0x80273174..0x80273188).
Activation 0x80272FF0 writes them into the RwCamera (+0x60 / +0x64). Selecting a screen: 0x8027211C. These
are viewport/raster objects of the RW bridge; the bridge document owns them. The framework hands them no
camera values.

---------------------------------------------------------------------------------------------------------------

## 3. Who calls the framework each frame

```
PJSCameraPreTask::update   0x801D0C18: blr (does nothing)
PJSCameraPostTask::update  0x801D0B0C(this, f32 dt):
    CameraManager::update(instance(), dt)             // 0x801D0B20 bl 0x80009548, 0x801D0B28 bl 0x8000D170

CameraManager::update      0x8000D170(mgr, dt):
    for (it = units.begin(); it != units.end(); ++it)  // 0x8000D190..0x8000D1C4, element stride 8
        if (it->ptr->enabled)                          // lbz +0 0x8000D1A0
            CameraManagerUnit::update(it->ptr, dt)     // 0x8000D1B0 bl 0x80270A0C
```
Both tasks are created by 0x801D0CE0 (Pre ctor 0x801D0C90, Post ctor 0x801D0BC8, each with parent
`0x801EB614(0x8001C4F8(), 0x12)`). `dt` is the f1 the task-tree traversal passes (0x8004ECAC/0x8004ED94 per
docs/distant-rendering/uv_anim_start.md); its unit (seconds vs frames) is NOT established here (UNKNOWN). It
matters for the shake sampling mode and the velocity (sections 7, 5).

---------------------------------------------------------------------------------------------------------------

## 4. Requests: creation, ordering, activation, removal

Creation API (CameraManager free functions):
- `createMode(out, type, ModeParam* init, unit)` 0x8026FA98: `factory->createMode` (0x8026FACC), wrap in
  shared_ptr with deleter 0x8026FCD0 (0x8026FAE0), register in `units[unit].modes` (0x8026FB0C).
- `createRequest(out, shared_ptr<CameraMode>* mode, prio, adjType, AdjustParam* adjParam, flags)`
  0x8026F858: `new 0x18` + ctor 0x8000ED58, shared_ptr with deleter 0x8026FC6C (0x8026F8C8), then
  `units[mode->unit].addRequest(req)` (0x8026F8F4..0x8026F914 -> 0x80270598). The caller keeps the
  shared_ptr; releasing the last reference runs the deleter = `removeRequest` + delete.

addRequest 0x80270598(unit, req):
```
it = list.begin()
while (it != list.end()):                       // 0x802705C4..0x80270620
    if (req.prio >= lock(*it).prio) break       // 0x8000EC28
    ++it
if (it == list.begin()):                        // new request becomes FRONT  (0x8027062C..0x8027063C)
    if (!list.empty()):
        old = lock(list.front())
        if (!(old.flags & 2)) deactivate(old.mode, old)     // 0x80270680..0x80270690 -> 0x8000E88C
    activate(req.mode, req)                                  // 0x802706A0..0x802706A8 -> 0x8000E8D8
    if (req.adjust) req.adjust->start(&unit.output /*+0x08*/, req.mode)   // 0x802706AC..0x802706C4
else if (req.flags & 2):
    activate(req.mode, req)                                  // 0x802706CC..0x802706E0
list.insert(it, weak(req))                                   // 0x80271764
```
So equal priority: the newer request goes in front of older ones.

removeRequest 0x802702BC(unit, req) (from the deleter 0x8026FC6C):
```
it = first entry whose reference has expired                 // 0x802702E8..0x80270324
if (it == list.begin()):
    deactivate(req.mode, req)                                // 0x80270344..0x8027034C
    if (list.size() > 1):                                    // 0x80270350..0x8027035C
        nxt = lock(second entry)
        if (!(nxt.flags & 2)) activate(nxt.mode, nxt)        // 0x802703AC..0x802703C0
        if (nxt.adjust) nxt.adjust->start(&unit.output, nxt.mode)   // 0x802703C4..0x802703DC
else if (it != list.end() && (req.flags & 2)):
    deactivate(req.mode, req)                                // 0x8027040C..0x80270420
list.erase(it)                                               // 0x80270438
```
activate 0x8000E8D8: `if (mode.activeCount == 0) mode.onActivate(req) /*+0x10*/; mode.activeCount++`.
deactivate 0x8000E88C: `if (count > 0) { count--; if (count == 0) mode.onDeactivate(req) /*+0x14*/ }`.

There is no request duration and no automatic expiry in the framework: a request lives as long as its owner
holds the shared_ptr. Blend-in is the new front request's adjust; there is no blend-out except that removing
the front starts the next front's adjust (from the current output).

---------------------------------------------------------------------------------------------------------------

## 5. CameraManagerUnit::update 0x80270A0C(unit, f32 dt) - exact order

```
if (unit.requests.empty()) return;                                   // 0x80270A2C..0x80270A34 (0x80270D14)
top = lock(unit.requests.front());                                    // 0x80270A38..0x80270A6C
if (top) {
    if (byte[0x805EF27C] && top.prio >= 0 && top.prio <= 0x400) return;   // 0x80270A7C..0x80270AA8
    mode  = top.mode;                                                 // shared_ptr copy 0x80270AB0
    CameraState s;                                                    // default 0x80270AB8
    mode->update(dt, top);                                            // 0x80270AC8 -> 0x8000E928 (vtable +0x0C)
    for (each later entry e, in list order) {                         // 0x80270AD4..0x80270B80
        r = lock(e);
        if (r && (r.flags & 2)) r.mode->update(dt, r);                // 0x80270B10..0x80270B3C
    }
    if (top.adjust) s = *top.adjust->update(dt, &top.mode->state);    // 0x80270B88..0x80270BA8 (0x8000ADD0)
    else            s = top.mode->state;                              // 0x80270BB0..0x80270BB8
    if (unit.shake) {                                                 // 0x80270BBC
        shake_apply(unit.shake, &s, dt);                              // 0x80270BD0 -> 0x8000EFC0
        if (shake_finished(unit.shake)) unit.shake.reset();           // 0x80270BD8..0x80270BE8
    }
    unit.output = s;                                                  // +0x08, 0x80270BF4
    if (unit.word_D0 == 0) unit.state_9C = s;                         // 0x80270BF8..0x80270C0C
    unit.world = WorldMatrix(&unit.output);                           // +0x6C, 0x80270C18 (0x8000A6C4) + copy
    unit.view  = ViewMatrix(&unit.output);                            // +0x3C, 0x80270C30 (0x8000A67C) + copy
}
// reached with or without a live front request (0x80270A78 beq -> 0x80270C58)
d = unit.output.eye - unit.prev.eye;                                  // 0x80270C64 (prev = +0xF8)
unit.velocity = d;                                                    // 0x80270C70
if (isZero(dt)) unit.velocity = (0,0,0) /*0x805E1860*/;               // 0x80270C78..0x80270C90
else            unit.velocity = (unit.output.eye - unit.prev.eye) * (1.0 [0x3F800000]@0x805F8F18 / dt);
                                                                      // 0x80270C98..0x80270CD0 (fdivs then 3 fmuls)
unit.targetPos = *(Vec3*)unit.targetInfo;                             // +0x138, 0x80270CD4
unit.prev = unit.output;                                              // full CameraState assign, 0x80270CE8
```
Note: a mode shared by two requests is updated once per request that reaches an update call.

### 5.1 Basis, world matrix and view matrix (roll applied)

Basis 0x8000A800(state, Vec3* outRight, Vec3* outUp, Vec3* outFwd) (any out may be null):
```
up  = state.up;                                         // 0x8000A824
f   = normalize(state.look - state.eye);                // 0x8000A834..0x8000A848 (0x8000A3BC, 0x8040DEC4)
tmp = cross(up, f);  (computed and discarded)           // 0x8000A858..0x8000A864
r   = cross(up, f);  r = normalize(r);                  // 0x8000A874..0x8000A888
u   = cross(f, r);                                      // 0x8000A898..0x8000A8A4 (not normalized)
if (!isZero(state.roll)) {                              // 0x8000A8A8..0x8000A8B4
    M = AxisAngle_800498C8(axis = f, angle = -state.roll);   // 0x8000A8C4 -> 0x8000A2D4 (fneg 0x8000A2EC)
    r = M * r;  u = M * u;                              // 0x8000A8D4, 0x8000A8F0 (0x8040C6CC; M[*][3] = 0)
}
outRight = r; outUp = u; outFwd = f;                    // 0x8000A900..0x8000A938
```
WorldMatrix 0x8000A6C4: rows `(r.x, u.x, f.x, eye.x)`, `(r.y, u.y, f.y, eye.y)`, `(r.z, u.z, f.z, eye.z)`
(0x8000A6F0..0x8000A758): columns are right, up, forward, position.
ViewMatrix 0x8000A67C: `up' = basis(state).outUp` (0x8000A7B8), then `LookAt(eye, up', look)` (0x8040C38C).

---------------------------------------------------------------------------------------------------------------

## 6. CameraAdjust - the blend (exact)

start 0x8000AF74(adj, CameraState* from, CameraMode* mode):
```
adj.cur   = *from;                     // 0x8000AF90
adj.start = adj.cur;                   // 0x8000AF9C
adj.elapsed = 0 [0]@0x805F27D4;  adj.stateB = 1;  adj.stateA = 1;  adj.state = 1;   // 0x8000AFA4..0x8000AFB8
if (isZero4(adj.durA) || adj.durA <= 0) adj.stateA = 2;    // 0x8000AFBC..0x8000AFE4 (isZero4 = 0x8000B044:
if (isZero4(adj.durB) || adj.durB <= 0) adj.stateB = 2;    //  fabsf(x) < 1e-4 [0x38D1B717]@0x805F27D0)
adj.begin(from, mode);                 // vtable +0x0C, 0x8000B01C..0x8000B02C
```
`from` is always `&unit.output` (+0x08): the blend starts from the last output, shake included.

update 0x8000ADD0(adj, f32 dt, CameraState* target /*= &mode.state*/) -> CameraState*:
```
if (adj.state != 1) return target;                          // 0x8000ADF0..0x8000AE04
adj.elapsed = adj.elapsed + dt;                             // 0x8000AE08..0x8000AE18
doneA = doneB = 0;
if (adj.stateA == 1) {                                      // 0x8000AE1C
    adj.wA = adj.weightA(adj.elapsed, adj.durA);            // vtable +0x14 (base: elapsed/durA)
    if (adj.elapsed > adj.durA) { adj.wA = 1.0 [0x3F800000]@0x805F27D8; doneA = 1; }   // 0x8000AE44..0x8000AE5C
    blendUp(adj, target);                                   // 0x8000AE68 -> 0x8000ABA8
    adj.blendEyeLookRoll(dt, target);                       // vtable +0x10
    if (doneA) adj.stateA = 2;
} else {
    adj.cur.eye = target.eye; adj.cur.look = target.look; adj.cur.roll = target.roll;  // 0x8000AE9C..0x8000AEA8
}
if (adj.stateB == 1) {                                      // 0x8000AEAC
    adj.wB = adj.weightB(adj.elapsed, adj.durB);            // vtable +0x18 (base: elapsed/durB)
    if (adj.elapsed > adj.durB) { adj.wB = 1.0; doneB = 1; }    // 0x8000AED8..0x8000AEF0
    // 0x8000AD90:
    adj.cur.aovH = fma(adj.wB, target.aovH - adj.start.aovH, adj.start.aovH);
    adj.cur.aovV = fma(adj.wB, target.aovV - adj.start.aovV, adj.start.aovV);
    if (doneB) adj.stateB = 2;
} else {
    adj.cur.aovH = target.aovH; adj.cur.aovV = target.aovV; // 0x8000AF14 -> 0x8000AF60
}
if (adj.stateA == 2 && adj.stateB == 2) adj.state = 2;      // 0x8000AF20..0x8000AF3C
return &adj.cur;                                            // 0x8000AF40
```
The up vector of `cur` is only written by blendUp (while A is active). With durA <= 0 it stays the start up
until the whole adjust finishes; once `state == 2` the mode state is passed through unchanged.
The `+0x30` word of cur is never updated by the adjust.

blendUp 0x8000ABA8(adj, target):
```
a = adj.start.up (+0x54);  b = target.up;
if (|a - b|^2 < 1e-4 [0x38D1B717]@0x805F27D0) return;       // 0x8000ABD4 (0x8000AD08: x*x, y*y, x+y, z*z, +)
axis  = cross(a, b);                                        // NOT normalized (0x8000ABEC..0x8000ABF8)
angle = angleBetween(a, b);                                 // 0x8041094C
if (|axis|^2 < 1e-4) {                                      // 0x8000AC08..0x8000AC34 (same sum order)
    axis = (0 [0]@0x805F27D4, 0, 1 [0x3F800000]@0x805F27D8);    // 0x8000AC38..0x8000AC54
    if (|axis - a|^2 < 1e-4) axis = (0, 1, 0);              // 0x8000AC58..0x8000AC88
}
M = AxisAngle_80410DDC(axis, angle * adj.wA);               // 0x8000AC8C..0x8000AC9C (fmuls angle*wA)
adj.cur.up = M * a;                                         // 0x8000ACA8..0x8000ACC4 (0x8040C6CC, +0x88)
```
Because `axis` is not normalized (|axis| = sin(angle) for unit ups), the matrix is a true rotation only when
the two up vectors are perpendicular; reproduce it as written.

### 6.1 Concrete adjust subclasses (PJSCamera namespace, outside this framework; summary for reference)

- AdjustLinerWorld (0xB0, vtable 0x8053D494): begin 0x801D0010 = blr; blendEyeLookRoll 0x801CFF20:
  ```
  roll = fma(wA, target.roll - start.roll, start.roll)       // 0x801CFF4C..0x801CFF78 (rolls via 0x8000A610 +8)
  dE = (target.eye  - start.eye)  * wA                         // 0x801CFF7C..0x801CFFB0
  dL = (target.look - start.look) * wA                         // 0x801CFF94..0x801CFFBC
  cur.eye = dE + start.eye;  cur.look = dL + start.look;  cur.roll = roll   // 0x801CFFC0..0x801CFFF0
  ```
  weights: base t/dur for A and B.
- AdjustOnlyAov (0xB0, vtable 0x8053D4C4): begin 0x801D00E8 = blr; blendEyeLookRoll 0x801D00C4 copies the
  target's eye/look/roll (0x8000A544). Net effect: only the AOV pair (and up) blend.
- AdjustLinerLocal (0xCC, ctor 0x801CFE8C) and AdjustCosRot (0xCC, ctor 0x80359D64): blend relative to a
  reference point obtained from game objects (0x800742BC, 0x800A903C, 0x801C0F38); begin stores
  `+0xB0 = start.eye - ref`, `+0xBC = normalize(start.look - start.eye)`, `+0xC8 = |start.look - start.eye|`
  (0x801CFD64 / 0x80359C84). CosRot's weightA 0x80359A78 = `1.0 - 0.5*(1.0 + cosf(pi*(t/dur)))`
  (`fnmsubs`: `1 - 0.5*(1 + c)` fused; 1.0/0.5 @0x805FB650/0x805FB654). Their position formulas are NOT
  specified here (the reference-point functions belong to the PJSCamera/player layer).

---------------------------------------------------------------------------------------------------------------

## 7. CameraShake (exact)

finished 0x8000EF58: `isZero(freq) -> 1`; else `dur < 0 [0]@0x805F2800 -> 0`; else `elapsed > dur`.

apply 0x8000EFC0(shake, CameraState* s, f32 dt):
```
if (finished(shake)) return;                                        // 0x8000EFE4..0x8000EFEC
shake.elapsed = shake.elapsed + dt;                                 // 0x8000EFF0..0x8000F004
p    = 1.0 [0x3F800000]@0x805F2808 / shake.freq;                    // 0x8000F00C
half = 0.5 [0x3F000000]@0x805F280C * p;                             // 0x8000F010
qtr  = 0.25 [0x3E800000]@0x805F2804 * p;                            // 0x8000F014
shake.mode = (dt > half) ? 2 : (dt > qtr) ? 1 : 0;                  // 0x8000F018..0x8000F044
off = (0,0,0);                                                      // 0x8000F054
A = s.up * shake.amp;                                               // computed per branch (0x8040DDF8)
switch (shake.mode):
case 2:                                                             // 0x8000F080..0x8000F0E8
    if (shake.step == 1) { shake.step = 3; off = A * -1.0 [0xBF800000]@0x805F2810; }
    else                 { shake.step = 1; off = A; }
case 1:                                                             // 0x8000F0EC..0x8000F168
    shake.step = shake.step + 1;  if (shake.step >= 4) shake.step = 0;
    if (shake.step == 1) off = A;  else if (shake.step == 3) off = A * -1.0;
case 0:                                                             // 0x8000F170..0x8000F25C
    e  = shake.elapsed;
    x  = (e - frsp(floor(e))) * shake.freq;                         // fsubs, fmuls
    ph = x - frsp(floor(x));                                        // phase in [0,1)
    off = A;
    if      (ph < 0.25) { shake.step = 0; w = ph * 4.0 [0x40800000]@0x805F2814; }
    else if (ph < 0.5)  { shake.step = 1; w = 1.0 - (ph - 0.25) * 4.0; }
    else if (ph < 0.75 [0x3F400000]@0x805F2818) { shake.step = 2; w = -((ph - 0.5) * 4.0); }
    else { shake.step = 3; w = -(1.0 - frsp((double)ph - 0.75 /*double 0x3FE8000000000000 @0x805F2820*/) * 4.0); }
    off = off * w;                                                  // 0x8000F25C
s.eye += off;  s.look += off;                                       // 0x8000F268 -> 0x8000A508
```
In mode 0 the offset is a triangle wave of `freq` cycles per unit of elapsed, phase-locked to
`frac(elapsed)`, peak `amp` along the CURRENT up vector of the state being shaken (the state's +0x24, i.e. the
reference up, not the roll-rotated basis up).

Shake API: `shake(mgr, type, unit, dur)` 0x8000D8EC (9 call sites); `shake(mgr, unit, dur, amp, freq)`
0x8000D664 (12 call sites); `stopShake(mgr, unit)` 0x8000D534. A new shake replaces the old one (shared_ptr
assign 0x8000D6E8).

---------------------------------------------------------------------------------------------------------------

## 8. Generic modes

### 8.1 CameraModeStop (0xAC, vtable 0x8051D55C) - matched names in symbols.txt

- creator 0x800092D4(init, unit): `new 0xAC`, CameraMode ctor, vptr.
- type 0x800092CC: -0x400. update 0x800095B4: blr. onDeactivate 0x8000932C: blr.
- onActivate 0x80009330: `src = &units[0].output` (0x80009370 with unit index hard-coded 0, 0x80009348);
  `this.eye = src.eye; this.look = src.look; this.roll = src.roll` (0x8000A544). Up and AOV are NOT copied
  (they stay as constructed: default up (0,1,0), default AOV pair).
  Effect: the camera freezes at the eye/look/roll unit 0 showed at activation.

### 8.2 CameraModeUser (0xD0, vtable 0x8051D5A4)

- creator 0x80009894(init, unit): `new 0xD0`, ctor 0x80009CC0 (CameraMode ctor, vptr, three empty
  boost::function objects at +0xAC, +0xB8, +0xC4, each `{+0 non-null when set, +4 functor word, +8 invoker}`
  zeroed by 0x80009D88 / 0x80009D44).
- helper 0x80009704(out, init, unit) = createMode(-0x3FE, init, unit) + shared dynamic cast to
  CameraModeUser (0x80009DC8 -> __dynamic_cast 0x803A1AFC). 14 call sites (e.g. 0x800A69B4, 0x800E9828).
- type 0x80009610: -0x3FE.
- update 0x800099B8(dt, req): if `+0xC4` set: invoke with `{f32 dt; CameraRequest* req}` (0x800099E8..0x800099F8
  -> 0x8000A124).
- onActivate 0x8000996C(req): if `+0xAC` set: invoke with `{req}`. onDeactivate 0x800098E4(req): `+0xB8`.
  Invocation (0x80009EB0 / 0x8000A124): `invoker(&ret, &functor, args)`; an empty function throws
  bad_function_call (0x80009ED8..0x80009EEC), unreachable here because emptiness is tested first.
- The mode itself computes nothing: the callbacks (set by the game code that owns the mode) write the
  CameraState directly. Setter helpers exist: 0x8000DB50(mode, eye*, look*), 0x8000DB78(mode, eye*),
  and on the mode of the unit's front request (0x80270DCC): 0x8000DAB8(mgr, eye*, look*, unit),
  0x8000DA24(mgr, unit, f1 roll) (stores +0x18, 0x8000DAB0), 0x8000D988(mgr, unit, f1 hAov)
  (-> 0x8000A3F0 -> 0x8000F41C, sets both AOV floats).

### 8.3 CameraModeMtnViewer (0xAC, vtable 0x8051D52C; source/decomp/game/SonicteamUSA/System/Camera/CameraModeMtnViewer.cpp is byte-matched for vf0/vf2/vf3/dtor)

- creator 0x80008D4C(init, unit). type 0x80008D40: -0x3FF. onDeactivate 0x80008DA4: blr.
- onActivate 0x80008DA8: `roll = 0 [0]@0x805F2780`; `eye = (0, 10 [0x41200000]@0x805F2784, 20 [0x41A00000]@0x805F2788)`;
  `look = (0,0,0)` (two 0x8000A5D4 calls, 0x80008DE0 / 0x80008E04).
- update 0x80008E2C(dt, req) - pad driven, dt unused:
```
pad = PadManager(0x8056FE0C via 0x80009208) + 0*0x2C;          // 0x80008E58..0x80008E60 (0x8040EBBC)
if (pad.buttons(+0) & 0x4000) return;  if (pad.buttons & 0x8000) return;   // 0x80008E6C..0x80008E88
eye = this.eye; look = this.look;
sx = pad.f32(+0x1C); sy = pad.f32(+0x20);                       // 0x80008EA8 / 0x80008EB0
k  = pi / 180.0 [0x43340000]@0x805F2790;                        // fdivs 0x80008F08 (recomputed for each use)
M  = I (0x805E17E0)
d  = eye - look;
dirAngles(d, &pitch, &yaw);                                     // 0x80008EF4 (pitch unused)
M  = M * RotY(yaw + (2.0 [0x40000000]@0x805F278C * sx) * k);    // 0x80008EF8..0x80008F1C
M  = M * RotX((2.0 * sy) * k);                                  // 0x80008F20..0x80008F3C
M  = M * RotY(-yaw);                                            // 0x80008F40..0x80008F4C
eye = (M * d) + look;                                           // 0x80008F50..0x80008F78
s  = len(eye - look) / 25.0 [0x41C80000]@0x805F2794;            // 0x80008F7C..0x80008FA4
px = pad.f32(+0x10); pz = pad.f32(+0x14);                       // 0x80008FA0 / 0x80008FA8
M2 = I; dirAngles(eye - look, &p2, &y2); M2 = M2 * RotY(y2);    // 0x80008FB0..0x80008FF8
w  = M2 * (px, 0, pz);                                          // 0x80008FFC..0x80009030
eye  += w * s;  look += w * s;                                  // 0x80009034..0x80009068
if (pad.buttons & 0x40) { h = 0.5 [0x3F000000]@0x805F2798 * s; eye.y += h; look.y += h; }   // 0x8000906C..0x80009098
if (pad.buttons & 0x80) { h = 0.5 * s; eye.y -= h; look.y -= h; }                          // 0x8000909C..0x800090C8
z = 1.0 [0x3F800000]@0x805F279C;
if (pad.buttons & 0x1000) z += 0.03 [0x3CF5C28F]@0x805F27A0;    // 0x800090D4..0x800090E4
if (pad.buttons & 0x2000) z -= 0.03;                            // 0x800090E8..0x800090F8
if (len(eye - look) > 0.1 [0x3DCCCCCD]@0x805F27A4 || z > 1.0)  // 0x800090FC..0x80009128
    eye = (eye - look) * z + look;                              // 0x8000912C..0x80009164
this.eye = eye; this.look = look;                               // 0x80009174 (0x8000A5D4)
```
Pad layout: PadManager singleton 0x8056FE0C holds Pad[4] of 0x2C bytes (0x8040EBBC `mulli 0x2C`); +0 is the
held-button word. The two float pairs +0x10/+0x14 and +0x1C/+0x20 are filled from two driver halfword
pairs at 0x8040EB3C..0x8040EB58; WHICH physical stick each pair is, and which physical buttons the bits
0x40/0x80/0x1000/0x2000/0x4000/0x8000 are, is UNCERTAIN (the button word is a game remap: runtime/src/gekko/
system.cpp:3058 notes A/START = 0x001/0x400 and D-pad left/right = 0x100/0x200 in this word). Up and AOV are
left as constructed.

---------------------------------------------------------------------------------------------------------------

## 9. Hand-off to the rendering side (what values reach the RW bridge)

Reader accessors (all take `(mgr, unit)` and go through `units.at(unit)`, 0x8000D0F0):

| accessor | returns | evidence |
|---|---|---|
| 0x800D5224 | `&unit+0x6C` world Mtx34 | 0x800D5258 |
| 0x8000F808 | `&unit+0x3C` view Mtx34 | 0x8000F83C |
| 0x80010244 / 0x80109F68 | `&unit+0x08` CameraState (eye) | 0x80010270 / 0x80109F8C (19 + callers) |
| 0x800101F8 | `&unit+0x14` (look) | 0x80010224 |
| 0x8000F7BC | `&unit+0x24` (AOV pair) | 0x8000F7E8 |
| 0x80009370 | `&unit+0x08` CameraState | 0x800093A4 |
| 0x80094264 | uses `&unit+0x9C` | 0x80094288 |
| 0x801C0F38 | `unit+0xF4` (TargetInfo*) | 0x801C0F64 |
| 0x80270D44 | front request (locked) | 0x80270D74..0x80270DA8 |

The bridge apply 0x80271F9C(bridge, unit) (6 call sites 0x801E4E90, 0x801E4F58, 0x801E4F94, 0x801E4FD4,
0x801E5010, 0x801E5050; the first two lie in RenderStatic__update 0x801E4E40 and RenderDiv__update
0x801E4EF0, symbol names as hints):
```
W = *(Mtx34*)(unit+0x6C)                                        // 0x80271FB4..0x80271FC0 (0x800D5224)
RwMatrix m: right = (W00,W10,W20), up = (W01,W11,W21), at = (W02,W12,W22), pos = (W03,W13,W23),
            flags/pad words = 0                                 // 0x80271FC4..0x80272038
RwMatrixUpdate(&m) 0x8047FBF0; fn_80488870(frame); RwFrameTransform(frame, &m, 2)   // 0x8027203C..0x8027205C
                                                                // frame = RwCamera(bridge+0)->+4
aov  = &unit.output.aovH (0x80009370 then 0x801C28A8 = +0x1C)    // 0x80272060..0x80272090
fac  = viewportFactor(table 0x8054DDC8[bridge+0x20][unit])      // 0x802732B4 -> 0x802731A4 (bridge side)
a.h  = aov.h * fac.x;  a.v = aov.v * fac.y                      // 0x80272094..0x802720B8
if (!(|a.h - bridge+0x24| < 1e-4 && |a.v - bridge+0x28| < 1e-4)) {    // 0x801C48FC = isZero(a-b)
    vw = (tanf(0.5 [0x3F000000]@0x805F2848 * a.h), tanf(0.5 * a.v))   // 0x8000F860
    RwCameraSetViewWindow(camera, &vw)                           // 0x804869A0
    bridge+0x24 = a.h; bridge+0x28 = a.v
}
```
viewportFactor 0x802731A4 (for completeness, owned by the bridge document): entry -1 -> (1.0, 1.0)
[0x3F800000]@0x805F8F4C; otherwise `(table[e].+0x28 * f32@0x805F1238, table[e].+0x2C * f32@0x805F123C)` with
table = 0x8058516C + e*0x30 (r13+0x4D38 / +0x4D3C), and for e != 0 a further transform 0x80033934 with
1.25 [0x3FA00000]@0x805F8F48 (not traced here).
RwCamera creation 0x80271BB4..0x80271CA8 sets `RwCameraSetFarClipPlane(30000.0 [0x46EA6000]@0x805F8F20)` and
`RwCameraSetNearClipPlane(1.0 [0x3F800000]@0x805F8F24)` - near/far come from the bridge, not from the
framework state.

So the values a native port must deliver per enabled unit: eye (W column 3), the orthonormal-ish basis
right/up/forward (W columns 0-2, roll applied, `up` = cross(f, r) not re-normalized), and the horizontal /
vertical AOV in radians (before the viewport factor). The look-at view matrix +0x3C is also published
(accessor 0x8000F808) for consumers that want the GX-style view matrix.

---------------------------------------------------------------------------------------------------------------

## 10. UNKNOWN / UNCERTAIN list

- Unit of `dt` (seconds vs frames): UNKNOWN; affects shake mode selection (`dt > 0.5/freq`) and velocity.
- CameraState+0x30 word, unit+0xD0 gate writers, unit+0x9C consumers beyond 0x80094264: UNKNOWN.
- ModeParam (+0x34 of a mode) fields +0x24..+0x6B and AdjustParam +0x08..+0x2B: not read by the framework.
- CameraMode vtable slot 4 (+0x18) and CameraAdjust slot 6 (+0x20): no framework caller found.
- Request flag bits other than 0x2: UNKNOWN.
- MtnViewer pad stick/button identities: UNCERTAIN (game-remapped button word).
- The aspect K at 0x805EBA30 (1.31958771) has no writer in a textual search of the lifted code; constancy
  UNCERTAIN.
- Unit+0xF8 constructor 0x80359FC4 body not read (layout use as CameraState is established by 0x8000B074 /
  0x8040DDC4 uses).
- AdjustLinerLocal / AdjustCosRot position formulas and the reference-point providers: not specified here.
- x <= 0 branch of sqrtf (0x8000FF5C..) not traced (lengths of non-zero vectors only reach x > 0).
