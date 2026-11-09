# Player::Behavior::EdgeHang (state id 12) - binary specification

Source: PAL `main.dol` only (disassembly via `tools/doldis.py`, cross-checked with the
lifted C++ in `source/.build/dolrecon/all_lifted.cpp`). No code was written or changed.
Coverage numbers come from
`.build/player/userstage/play-143839/strict/verify.csv.coverage` (file "A") and
`.build/player/userstage/play-143928/strict/verify.csv.coverage` (file "B").
`play-*/counts.csv` shows the EdgeHang factory 0x8007F154 called 10x (A) and 1x (B),
both times from 0x800AE958 (the behaviour factory dispatcher). A factory call is not an
entry: the object is created before CanEnter runs (see 4.1).

Tags: **[P]** PROVEN (read from code), **[I]** INFERRED (reasoned; meaning of a field or
helper), **[U]** UNKNOWN.

Conventions: `P` = the player object that every behaviour reaches through `this+4`
(`8006DD60` is `return *(u32*)r3`). `r2 = 0x805FA780`, `r13 = 0x805EC500`.
`dt` = the f1 argument of `update` (Fall uses it to count down a 0.5 timer) [I: seconds].
"Bit n of P+0xA8" = word `P+0xA8 + (n>>5)*4`, mask `1<<(n&31)` (0x8006CEA0 test,
0x80076DDC set, 0x80076DFC clear) [P]. "Flag n of P+0xA0" = single word, mask `1<<n`
(0x80014E1C set, 0x800613A4 clear) [P].

---------------------------------------------------------------------------------------

## 0. Shared helpers (used below)

| addr | what it does [P] | notes |
|---|---|---|
| 800091D0 / 800091EC | vec3 copy `d = s` | |
| 8040DDC4 | `d = a - b` | (r3=d, r4=a, r5=b) |
| 8040DDF8 / 800091A4 | `d = v * s` (3x `fmuls`) | 800091A4 is a wrapper |
| 8040DE20 | `d = a + b` (3x `fadds`) | |
| 8040DE54 | `a -= b` in place | |
| 8040E048 | `a += b` in place | |
| 8000AD64 | wrapper of 8040DDC4 | |
| 80016284 | `d = -v` (`fneg`) | |
| 8040DEB0 | `v = (0,0,0)` (0x805FBDF8 = 0.0) | |
| 8040DFE0 | dot: `t = y*y'` (`fmuls`); `t = x*x' + t` (`fmadds`); `r = z*z' + t` (`fmadds`) | order matters for exactness |
| 8040E008 | cross `d = a x b` (`fmuls` + `fmsubs`) | |
| 8040DFA0 | length = `8000FEE4(x*x+y*y+z*z)` (sqrt routine, `fmuls`/`fadds`) | |
| 8040DEC4 / 8040DF38 | normalize in place / into copy: `len<eps(0x805FBDFC)` -> copy of 0x805E1860 and len=0; else 3x `fdivs` by len | |
| 804109E0 | `(proj, perp) = split(v, n)`: `d=dot(v,n)`, `proj=n*d`, `perp=v-proj` (outputs optional) | |
| 80079810 | `fwd(P)`: local vector (0.0 @0x805F3554, 0.0, -1.0 @0x805F355C) transformed by 8007975C = matrix at P+0x1E4 rotated by angles P+0x10, P+0x0C, P+0x14 | [I] P+0x0C..0x14 = rotation angles, P+0x10 = yaw |
| 80079854 | `80093A18(P, P+0x18, P+0x24, P+0xF4, P+0xA8)` with f1 = `*(float*)P[0x60]`: per-frame velocity damping split along P+0xF4 (mode chosen by bits 7/9/2/0x1D of P+0xA8) | [I] drag, not gravity |
| 80097658 | 453-insn player move/collide routine (called with r4=0) | [I] integrate velocity + world collision; sets bit 2 (grounded) [I] |
| 80097D6C | wrapper: `80097DB4(r3, pos=0, nrm=0, obj=0, a=0, b=0, seg=r4, filt=r5, w0=r6, w1=r7, mode=r8)` | |
| 80097DB4 | world line cast. `seg` = `{vec3 start; vec3 end}`. Builds filter {w0,w1} at sp+0x44, calls 80021E24 (mode 3, kind=`mode`, callback 0x80099454). Miss -> returns -1.0 (0x805F3ACC). Hit -> distance from start `d` (f1); `d <= 0.0` (0x805F3A90) is turned into -1.0 (80097EE0..EE8). On hit optionally writes hit position (r4), hit normal (r5), hit object handle (r6), attribute low 28 bits (r7) / high 4 bits (r8) | distance is in world units [P: it scales a unit vector in 8009827C] |
| 800A82FC | ChangeBehavior(mgr=P[0x22C], args) (see 4.1) | |

---------------------------------------------------------------------------------------

## 1. Function table

Vtable of EdgeHang = 0x8052169C (stored by ctor 0x800800F8) [P]. RTTI name pointer at
0x805E5284 -> "Player::Behavior::EdgeHang". Slot roles from the calls made by
ChangeBehavior 0x800A82FC (+0x0C CanEnter, +0x10 Enter, +0x14 Leave) [P]; +0x18
onCommand, +0x1C update [I, inventory naming + argument shapes].

| addr | name | role | insns | executed A | executed B |
|---|---|---|---|---|---|
| 8007F154 | fn_8007F154 | factory: `new(0x50)` (803A1380) then ctor(this, P) | 18 | 18 | 18 |
| 800800CC | EdgeHang::ctor | base ctor 80076D70(this, P, *(r13-0x72A0), id=0xC); vptr=0x8052169C; handle +0x48 = {0,0} (80051D4C) | 19 | 19 | 19 |
| 80080064 | EdgeHang::dtor (vt+0x08) | vptr reset, release handle +0x48 (80051CFC), base dtor 80076C6C, delete if flag>0 | 26 | 26 | 26 |
| 8007FF94 | EdgeHang::CanEnter (vt+0x0C) | ground/flag gate | 30 | 28 | 26 |
| 8007FBB8 | EdgeHang::Enter (vt+0x10) | | 116 | 116 | 116 |
| 8007F854 | EdgeHang::Leave (vt+0x14) | | 30 | 0 | 30 |
| 8008000C | EdgeHang::onCommand (vt+0x18) | | 22 | 0 | 7 |
| 8007F810 | EdgeHang::update (vt+0x1C) | dispatch on +0x1C | 17 | 15 | 16 |
| 8007F80C | EdgeHang vt+0x20 | `blr` (empty) | 1 | 0 | 0 |
| 8007F670 | update__sub0 | phase 0: hang | 103 | 54 | 72 |
| 8007F19C | update__sub0__sub2 | wall-still-there probe | 49 | 49 | 49 |
| 8007F260 | update__sub0__sub8 | hold position + face wall | 67 | 67 | 67 |
| 8007F45C | update__sub1 | phase 1: climb up | 133 | 0 | 117 |
| 8007F36C | fn_8007F36C | moving-object callback (bound in Enter) | 60 | 0 | 0 |
| 8008011C | enter__sub8 | bind PTMF {0,-1,0x8007F36C} to `this` | 31 | 31 | 31 |
| 80080198 | enter__sub8__sub1 | (bind internals) | 20 | 20 | 20 |
| 800801E8 | enter__sub8__sub0 | (bind internals) | 5 | 5 | 5 |
| 8007FE34 | enter__sub9 | wrap bound functor in a callback holder | 29 | 29 | 29 |
| 8007FEA8 | enter__sub9__sub0 | (holder internals) | 23 | 23 | 23 |
| 8007FF04 | enter__sub9__sub0__sub0 | (holder internals) | 36 | 36 | 36 |
| 80080118 | enter__sub9__sub0__sub0__sub0 | `blr` | 1 | 1 | 1 |
| 80078F8C | enter__sub10 | `if P[0x228]: 80094CD4(P[0x228], cb)` set callback | 11 | 11 | 11 |
| 80094CD4 | enter__sub10__sub0 | `80094A28(P[0x228]->+0x28 + 8, cb)` | 10 | 10 | 10 |
| 8007FD88 | (callback holder dtor) | | 18 | 16 | 16 |
| 80080CE4 | InitializeArgsEdgeHang::ctor | | 29 | 29 | 29 |
| 80080C74 | InitializeArgsEdgeHang::dtor | | 28 | 26 | 26 |
| 8008096C | Fall::update | contains the ledge trigger | 194 | 159 | 128 |
| 8009827C | Fall::update__sub13 | ledge geometry test | 221 | 209 | 207 |
| 80080ED4 | Fall::Enter | sets the trigger delay | 95 | 67 | 67 |
| 80093F50 | (stick classifier) | used by hang | 53 | 0 | 51 |
| 80093E90 | (turn yaw toward dir) | used by hold | 48 | 47 | 47 |
| 800B7F40 | (descending test) | used by climb | 29 | 29 | 29 |
| 80078F34 | attach object to P[0x228] | | 11 | 11 | 11 |
| 80078F60 | reset P[0x228] callback to default (800949AC) | | 11 | 0 | 11 |
| 80078F08 | detach P[0x228] object (800943F4) | | 11 | 0 | 11 |

EdgeHang object layout (size 0x50, from `li r3,0x50` @8007F168) [P]:

| off | type | meaning |
|---|---|---|
| +0x00 | ptr | vtable 0x8052169C |
| +0x04 | handle | player reference (`*(this+4)` = P) |
| +0x0C | u32 | `*(r13-0x72A0)` from ctor [U meaning] |
| +0x10 | s32 | state id = 12 |
| +0x14,+0x18 | u32 | zeroed by base (80076DC8) |
| +0x1C | s32 | phase: 0 hang, 1 climb |
| +0x20 | s32 | sub-phase counter of the current phase |
| +0x24 | s32 | request: 0 none, 1 climb, 2 let go |
| +0x28 | f32 | grab lock-out timer (0.8 at Enter) |
| +0x2C | f32 | climb timer |
| +0x30 | vec3 | ledge point (InitializeArgs +0x08) |
| +0x3C | vec3 | wall normal (InitializeArgs +0x14) |
| +0x48 | handle {ptr,ctrl} | wall hit object (InitializeArgs +0x20) |

InitializeArgsEdgeHang (built on Fall's stack at sp+0x64) [P]: +0 vptr 0x8052173C
(base InitializeArgs vptr 0x80520F18 written first), +4 id = 0xC, +0x08 vec3 ledge
point, +0x14 vec3 wall normal, +0x20 object handle (copy via 800520B0).

---------------------------------------------------------------------------------------

## 2. Entry

### 2.1 Only one producer of state 12 [P]

* InitializeArgsEdgeHang::ctor 0x80080CE4 is the only code that writes id 0xC into an
  InitializeArgs (`li r7,0xc; stw r7,4(r29)` @80080CF8/80080D1C). It has exactly one
  caller: 0x80080AC4 in Fall::update (`--xref 80080CE4`: 1 site).
* A scan of all 131 `bl 0x800A82FC` sites (16 instructions before each) found no
  inline `li rX,0xc` args. Enter 8007FBB8 dynamic-casts the args to
  InitializeArgsEdgeHang (803A1AFC with target typeinfo 0x805E526C) and dereferences
  the result without a null check (8007FC44), so a plain id-12 args object could not work
  [P for the missing check, I for "therefore no other producer"].
* EdgeHang itself is entered only from Fall (state 2).

### 2.2 Gate in Fall::update (0x8008096C)

Fall object: +0x1C f32 = ledge-check delay.

Fall::Enter 0x80080ED4 sets `Fall+0x1C = 0.5` (0x805F36A0) @80080F34, then overrides it
with 0.0 (0x805F3690) when the previous state id is 5 (Jump, @80080FA8) or 6 (JumpDash,
@80080FE4). So after leaving EdgeHang (prev id 12) Fall waits 0.5 before it can grab again
[P].

Fall::update, after its move/collide (80097658 @80080A4C) and 800808C8 [P]:

```
t = Fall+0x1C - dt                      // fsubs @80080A60
Fall+0x1C = t
if (t <= 0.0 /*0x805F3690*/) {          // fcmpo + cror(lt|eq) @80080A6C..A74
    Fall+0x1C = 0.0                      // @80080A78
    ObjHandle h = {0,0}                  // 80051D4C on sp+0x38
    float prm[2] = { *(f32*)0x805EF48C /*3.0*/, *(f32*)0x805EF488 /*6.0*/ }  // sp+0x30
    vec3 ledge /*sp+0x58*/, wallN /*sp+0x4C*/
    if (LedgeTest_8009827C(&ledge, &wallN, &h, &Fall->player, prm)) {
        InitializeArgsEdgeHang a(ledge, wallN, h)    // 80080CE4
        ChangeBehavior(P[0x22C], &a)                 // 800A82FC @80080AD4
        ~a; ~h
        return                                        // b 80080C58, result of ChangeBehavior ignored
    }
    ~h
}
... rest of Fall (landing tests) ...
```

`*(0x805EF488)` and `*(0x805EF48C)` are .sbss; they are written only by static
initializer 0x80081160 (listed in .ctors at 0x804AAD14): 0x805EF488 = 6.0 (from
0x805F36A8), 0x805EF48C = 3.0 (from 0x805F36AC) [P]. If the test succeeds but CanEnter
fails, Fall's own landing checks are skipped for that frame [P].

### 2.3 Geometric test 0x8009827C(out ledge r3, out wallN r4, out obj r5, &player r6, prm r7)

```
f    = fwd(P)                                   // 80079810 -> sp+0x134
up   = P+0xE8                                   // copy -> sp+0x128
reach= prm[0] + P[0x274]                        // fadds @800982EC  (3.0 + P+0x274)
A    = P.pos + f*reach                          // P+0x00 is the position; sp+0x14C
B    = A + up*prm[1]                            // prm[1]=6.0; sp+0x140
disp = P.pos - P+0xD0                           // 8040DDC4 @8009834C
B   -= up * dot(disp, up)                       // 804109E0(proj) @80098360, 8040DE54
// query 1: vertical, from above the reach point down to it
d1 = Cast(seg{start=B, end=A}, nrm->sp+0x110, w0=1, w1=0x40000, mode=2)   // @800983A8
if (dot(n1, up) < 0.866 /*0x805F3AD0*/) return 0     // @800983BC..C4
if (!(d1 >= 0.0 /*0x805F3A90*/))       return 0     // @800983D0..DC (miss = -1)
top = B + normalize(A - B) * d1                     // ledge top point, sp+0xF8
// query 2: horizontal, from the player to the same reach point
A2 = P.pos + f*(prm[0] + P[0x274])                  // recomputed, same as A
d2 = Cast(seg{start=P.pos, end=A2}, nrm->out wallN, obj->sp+0x30,
          w0=0xFFFFFFFF, w1=0x40200, mode=2)        // @800984C8
if (out obj) copy hit object handle into *out obj   // 800984D8..80098530
if (!(d2 >= 0.0)) return 0                           // @80098534..40
v  = P.pos - A2; L = |v|                             // 8040DDC4, 8040DFA0
out ledge = top + normalize(v) * (L - d2)            // fsubs @80098568
return 1
```

Meaning [I]: query 1 finds a walkable top surface (normal within ~30 deg of up,
cos = 0.866) between 6.0 above and the reach point (3.0 + P+0x274 ahead); the start is
raised by this frame's vertical travel so fast falls do not skip the lip. Query 2 finds
the wall face along the facing direction. The ledge point is the top-surface hit pulled
back toward the player onto the wall face. P+0x274 [I] collision radius, P+0xE8 [I] up,
P+0xD0 [I] previous position. Filter words / mode values are passed through to 80021E24
[U: exact collision-type meaning].

### 2.4 CanEnter 0x8007FF94 [P]

```
if (bit 0x1E of P+0xA8) return 0                        // @8007FFB4
g = P[0x278]
if (g->u8[+0x24] != 0 && g->f32[+0x20] < 20.0 /*0x805F3678*/) return 0
return 1
```
[I] g = ground probe record (+0x24 valid, +0x20 distance below; Fall uses the same record
for its landing test @80080B88..BD0). So no grab when the ground is closer than 20.0.

ChangeBehavior 0x800A82FC sequence [P]: prevId = current->+0x10 (or 0x25 if none);
`new = factory->vt+0x0C(id)`; mgr+0x48 = 10.0 (0x805F3C8C); `new->CanEnter(prevId,
args)`; if false the new object is released (no state change); else `old->Leave(newId)`,
swap (800A8420), `new->Enter(prevId, args)`, return 1.

---------------------------------------------------------------------------------------

## 3. Per-function behaviour

### 3.1 Enter 0x8007FBB8 (this, prevId, args) [P]

```
a = __dynamic_cast(args, 0, &ti InitializeArgsEdgeHang 0x805E526C, &ti InitializeArgs 0x805E5264, 0)  // 803A1AFC
set bit 0x0B of P+0xA8                         // 80076DDC
set flag 1 of P+0xA0                           // 80014E1C
P+0x18 = (0,0,0)                               // 8040DEB0  (velocity [I])
P[0x230]->vt(+0x18 table)[+0x10](P[0x230], 0x0D, f1=0.0 /*0x805F3658*/)   // [I] play motion 13, blend 0
this+0x1C = 0; this+0x20 = 0; this+0x24 = 0
this+0x28 = 0.8 /*0x805F3674*/
this+0x30 = a+0x08 (ledge point); this+0x3C = a+0x14 (wall normal)
this+0x48 = a+0x20 (handle copy, 80052554 = ptr copy + addref 80017390)
tmp = ObjRef(this+0x48)                         // 80334280 + 80334180 on sp+0x54
fn  = bind(PTMF{0,-1,0x8007F36C}, this, tag=*(u8*)(r13+0x2F68))   // 8008011C; PTMF read from 0x80521684
cb  = Callback(fn)                              // 8007FE34
SetCallback(P[0x228], cb)                       // 80078F8C -> 80094CD4
~cb                                             // 8007FD88
Attach(P[0x228], tmp)                           // 80078F34 -> 8009443C
set bit 0x37 of P+0xA8
~tmp                                            // 80334224
P[0x240]->vt(+0x18 table)[+0x14](P[0x240], 0x12)   // [U] (sound/voice/effect request 0x12?)
x = 8034ED50(sp+0xC, 10, P[0x54]); 80076930(x,-1)   // [U] temporary built with 10 and P+0x54, destroyed at once
```
No `fctiwz`, `frsp`, or paired-single math (only `psq_st/psq_l` saves of f30/f31 in callees).

### 3.2 Update 0x8007F810 (this, f1=dt) [P]

```
switch (this+0x1C) { case 0: Hang(this, dt) /*8007F670*/; case 1: Climb(this, dt) /*8007F45C*/; default: nothing }
```

### 3.3 Hang 0x8007F670 (this, dt) [P]

```
switch (this+0x20) {
case 0:                                         // grab lock-out
    this+0x28 = this+0x28 - dt                  // fsubs @8007F6B4
    if (this+0x28 < 0.0) this+0x20 += 1         // fcmpo/bge @8007F6C0
    break
case 1:                                         // accept stick
    if (bit 0x1B of P+0xA8) {                   // [I] stick deflected
        r = StickClass(&P, P+0xB8)              // 80093F50
        if (r == 0) this+0x24 = 1               // toward wall -> climb
        if (r == 3) this+0x24 = 2               // away from wall -> let go
    }                                           // r 1/2 (sideways): nothing
}
if (!WallProbe(this)) this+0x24 = 2             // 8007F19C @8007F730
switch (this+0x24) {
case 1: this+0x1C = 1; this+0x20 = 0; break     // start climb next frame
case 2:                                         // let go
    v = -fwd(P) * 50.0 /*0x805F3670*/           // 80079810, 80016284, 8040DDF8
    P+0x18 = v                                  // velocity = push away from wall [I]
    ChangeBehavior(P[0x22C], InitializeArgs{0x80520F18, id 2 /*Fall*/})
}
Hold(this, dt)                                  // 8007F260, runs even after the state change
80079854(P)                                     // damping
```
Note [P]: Hold and 80079854 still execute on the old EdgeHang object after the change to
Fall in the same call; [U] whether the object is still owned at that point (not reached in
coverage).

StickClass 0x80093F50(&P, in = P+0xB8) [P]:
```
f = fwd(P); d = dot(f, in)
if (d > 0.70710677 /*0x805F3A70*/)  return 0
if (d < -0.70710677 /*0x805F3A74*/) return 3
s = normalize(cross(f, P+0x1C8))                // 8040E008, 8040DF38
return dot(in, s) > 0.0 /*0x805F3A4C*/ ? 1 : 2
```
[I] P+0xB8 = stick direction in world space; P+0x1C8 = a reference axis (likely up).

WallProbe 0x8007F19C(this) [P]:
```
reach = *(f32*)0x805EF474 /*4.0*/ + P[0x274]    // fadds @8007F1DC
seg = { P.pos, P.pos + fwd(P)*reach }
d = 80097D6C(&P, &seg, 0, w0=0x1A93, w1=0x40000, mode=2)
return !(d < 0.0)                               // fcmpo, LT bit, cntlzw @8007F230..240
```

Hold 0x8007F260(this, dt) [P]:
```
T = this+0x30 + (P+0xF4)*(-6.4 /*0x805EF46C*/) + (this+0x3C)*0.25 /*0x805EF470*/
k = 15.0 /*0x805F365C*/ * dt ; if (!(k >= 1.0 /*0x805F3660*/)) use k else 1.0   // fmuls @8007F2E0, min(k,1)
P.pos += (T - P.pos) * k                        // 8000AD64, 800091A4, 8040E048
TurnToward(&P, -(this+0x3C), 6.5 /*0x805F3664*/ * dt)   // 80093E90, fmuls @8007F340
```
.sbss constants 0x805EF46C = -6.4 (0x805F367C), 0x805EF470 = 0.25 (0x805F3680),
0x805EF474 = 4.0 (0x805F3684) are written only by static initializer 0x800801FC
(.ctors 0x804AAD10) [P]. [I] P+0xF4 = up axis: the player hangs 6.4 below the ledge
point, 0.25 off the wall face.

TurnToward 0x80093E90(&P, dir, rate) [P]: `l = Transform(inv?(P+0x1E4) 8040CF48, dir)`;
`yaw = atan2(l.x, l.z)` via 8003BA10/80016260; `target = yaw + 180.0*(pi/180.0)`
(`fdivs` 0x805FBDB0/0x805F3A50 then `fmuls`, `fadds`); `rate = min(rate, 1.0 0x805F3A58)`;
`P+0x10 = cur + wrap(target - cur) * rate` (80410364: `fsubs`, 8040F9EC wrap, `fmadds`).
[I] faces the player into the wall.

### 3.4 Climb 0x8007F45C (this, dt) [P]

```
switch (this+0x20) {
case 0:
    clear bit 0x37 of P+0xA8
    ResetCallback(P[0x228])   // 80078F60 -> 800949AC (default functor 0x80095298)
    Detach(P[0x228])          // 80078F08 -> 800943F4
    P+0x18 += (P+0xF4) * 140.0 /*0x805F3668*/    // upward kick
    set bit 0 of P+0xA8                          // [I] airborne
    P[0x230]->motion(0x0C, f1 = 15.0 /*0x805F365C*/)   // [I] play motion 12
    this+0x2C = 0.0; this+0x20 = 1
    break
case 1:
    if (Descending(&P)) this+0x20 = 2           // 800B7F40
    break
case 2:
    P+0x24 += fwd(P) * 220.0 /*0x805F366C*/     // every frame in sub-phase 2 [I] forward push onto ledge
    break
}
80079854(P)
if (this+0x20 >= 2) 80097658(&P, 0)             // move + collide only from sub-phase 2
this+0x2C = this+0x2C + dt                      // fadds @8007F5BC
if (this+0x2C > 1.0 /*0x805F3660*/)  ChangeBehavior(InitializeArgs{id 2 Fall})
else if (bit 2 of P+0xA8)            ChangeBehavior(InitializeArgs{id 3 Landing})
```

Descending 0x800B7F40(&P) [P]: `if (bit 2 of P+0xA8) return 0; return !(dot(P+0x18, P+0xE8) >= 0.0 /*0x805F3E38*/)`.

### 3.5 Leave 0x8007F854 (this, newId) [P]

```
clear bit 0x00 of P+0xA8; clear bit 0x0B of P+0xA8
clear flag 1 of P+0xA0                          // 800613A4
clear bit 0x37 of P+0xA8
ResetCallback(P[0x228]) /*80078F60*/; Detach(P[0x228]) /*80078F08*/
```

### 3.6 onCommand 0x8008000C (this, cmd) [P]

```
c = *(u16*)(cmd+4)
if (c == 0x104) { this+0x24 = 1; *(u8*)(cmd+7) |= 2 }   // climb request, mark handled
if (c == 0x106) { this+0x24 = 2; *(u8*)(cmd+7) |= 2 }   // let-go request, mark handled
// anything else (incl. 0x105, >= 0x107, < 0x104): ignored
```
The request is acted on only in Hang (3.3); in Climb +0x24 is never read.

### 3.7 Moving-object callback 0x8007F36C (this, rec) [P, never executed]

Builds a matrix from rec+0x0C (8040C084), applies rec+0x18/+0x24 (8041100C) and
rec+0x34/+0x40 (80410F0C) and a translation by `-(rec+0x28)` (80016284, 8040C5EC); then
`this+0x30 = M*this+0x30` (point, 8040C6CC), `this+0x3C = M*this+0x3C` (direction,
8040C668), `*rec[4] = M*(*rec[4])` (point). [I] keeps the ledge point, wall normal and
player position glued to a moving hit object.

---------------------------------------------------------------------------------------

## 4. Exits

| from | condition | target | where |
|---|---|---|---|
| Hang | sub-phase 1, bit 0x1B set, StickClass == 3 (stick away from wall) | 2 Fall, velocity = -fwd*50 | 8007F724 -> 8007F774 |
| Hang | WallProbe fails (no wall within 4.0+P[0x274] ahead) at any sub-phase | 2 Fall, velocity = -fwd*50 | 8007F73C -> 8007F774 |
| Hang | onCommand 0x106 | 2 Fall, velocity = -fwd*50 | 8008004C -> 8007F774 |
| Hang -> Climb (internal) | sub-phase 1, bit 0x1B set, StickClass == 0 (stick toward wall), or onCommand 0x104 | phase 1 next frame | 8007F760 |
| Climb | bit 2 of P+0xA8 set while +0x2C <= 1.0 | 3 Landing | 8007F60C..644 |
| Climb | +0x2C > 1.0 | 2 Fall | 8007F5D0..5F0 |
| (none) | sideways stick (StickClass 1/2) | stays hanging | |

The user's recording B: Hang -> stick toward wall (8007F718 executed, 80093F50 return 0
path) -> Climb -> Landing (8007F624..8007F650 executed). The Fall-by-timeout exit and all
let-go paths were never executed.

---------------------------------------------------------------------------------------

## 5. Inputs

* Stick: only through bit 0x1B of P+0xA8 (gate) and vec3 P+0xB8 (direction), compared with
  the facing vector in 80093F50 (+-0.70710677 cone = 45 deg) [P read; I that they are the
  stick]. Read only in Hang sub-phase 1 (after the 0.8 lock-out).
* Commands: onCommand ids 0x104 (climb) / 0x106 (let go) [P]. [U] which object or input
  path sends them.
* No buttons are read directly by EdgeHang code [P].
* Other player state read: P+0x00 pos, P+0x10 yaw, P+0x18 velocity, P+0x24 extra
  velocity, P+0x54, P+0x60, P+0xA8 bits 0x02/0x1B/0x1E, P+0xB8, P+0xD0, P+0xE8, P+0xF4,
  P+0x1C8, P+0x1E4, P+0x228, P+0x22C, P+0x230, P+0x240, P+0x274, P+0x278 [P].

---------------------------------------------------------------------------------------

## 6. What is established vs inferred

PROVEN: every address, branch, constant value and field offset quoted above; the vtable
slots; the call graph; the order of float ops (dot = `fmuls`+2x`fmadds`, cross with
`fmsubs`, angle approach `fmadds`, normalize `fdivs`); none of the EdgeHang functions use
`fctiwz`, `frsp` or paired-single arithmetic.

INFERRED: dt = seconds; P+0x18 velocity, P+0x24 per-frame extra velocity, P+0xE8/P+0xF4
up axes, P+0xD0 previous position, P+0x274 radius, P+0x278 ground probe, P+0xB8 stick
vector, bit 0x1B stick held, bit 2 grounded, bit 0 airborne; the motion calls
(P[0x230], ids 0x0D hang / 0x0C climb); P[0x228] = attachment component (callback +
attached object); 8007F36C = moving-platform correction; 80097658 = move and collide;
80079854 = velocity damping.

UNKNOWN:
1. Meaning of the cast filter words (1/0x40000, 0xFFFFFFFF/0x40200, 0x1A93/0x40000) and
   mode 2 inside 80021E24.
2. What applies gravity in Climb sub-phase 1 (EdgeHang never integrates there, yet
   Descending turns true in recording B); probably the owner's per-frame code outside the
   behaviour.
3. P[0x240] vt+0x14 call with 0x12, and 8034ED50(tmp, 10, P[0x54]) in Enter (sound/effect?).
4. Ctor value `*(r13-0x72A0)` stored at +0x0C.
5. Who sends commands 0x104 / 0x106.
6. Whether the old EdgeHang object is still alive while Hold/80079854 run after the
   let-go ChangeBehavior (8007F7DC..7EC).
7. The exact angle convention of P+0x0C..0x14 and the meaning of P+0x1C8.

### 6.1 Instructions never executed in A or B (unverified paths)

| function | unexecuted ranges (inclusive) | meaning |
|---|---|---|
| CanEnter | 8007FFC0-8007FFC4 | reject because bit 0x1E set |
| update | 8007F834 | phase < 0 |
| vt+0x20 | 8007F80C | empty slot |
| Hang | 8007F6A8 | sub-phase < 0 |
| Hang | 8007F724-8007F728 | StickClass 3 -> let go |
| Hang | 8007F73C-8007F740 | wall probe failed -> let go |
| Hang | 8007F774-8007F7D8 | let-go transition (velocity, ChangeBehavior to Fall) |
| Climb | 8007F494 | sub-phase < 0 |
| Climb | 8007F5D0-8007F608 | 1.0 timeout -> Fall |
| onCommand | 80080028-80080060 | all of 0x104 / 0x106 handling (only commands < 0x104 seen, in B) |
| fn_8007F36C | 8007F36C-8007F458 | moving-object callback (whole function) |
| 8007FD88 | 8007FDB0-8007FDB4 | holder delete path |
| ArgsEH dtor | 80080CC4-80080CC8 | delete path |
| StickClass | 80093FB8-80093FBC | return 3 |
| TurnToward | 80093F18 | rate clamp to 1.0 |
| Fall::update | 800809C8-800809F0, 80080B2C-80080B58, 80080C28-80080C54 | unrelated Fall paths (+0x58 target, flag set, ground bit Landing) |
| Fall ledge test | 80098508-80098524 | copy a non-null wall hit object (every grab was on static geometry) |
| Fall ledge test | 800985C4-800985D0 | query 2 missed after query 1 hit |
| Fall::Enter | 80080F58-80080F8C, 80080FDC-80081010 | prev id 9/0x1D/6 branches |
| 80097DB4 | 80097E54-80097E9C, 80097EE8-80097EEC, 80097F80-80097FA0 | filter object, d<=0 remap, non-null hit object |

Recording A: Leave, Climb, StickClass, onCommand and 80078F60/80078F08 were never
executed while CanEnter's reject path (8007FFEC, ground < 20.0) was. With 10 factory calls,
this means most of the 10 ledge hits were rejected by CanEnter and the one accepted hang
was still active when the verified window ended [I].
