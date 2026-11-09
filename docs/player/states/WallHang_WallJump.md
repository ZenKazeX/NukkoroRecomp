# Player::Behavior::WallHang (id 13) and Player::Behavior::WallJump (id 16) - binary specification

Source: PAL `main.dol` only (disassembly via `tools/doldis.py`, cross-checked with the lifted
C++ in `source/.build/dolrecon/all_lifted.cpp`). No code was written or changed.

Coverage files (end exclusive):

- **U** = `.build/player/userstage/play-144714/strict/verify.csv.coverage` (user route: WallHang
  entered 5x, WallJump 1x).
- **W** = `.build/player/verify-westopolis-all/verify.csv.coverage` (Westopolis: WallHang once,
  no WallJump).

Note: `test_data/player/audit/player_inventory.csv` lists every WallJump function with
`strict_replayed = 0`; file U shows them executed. The inventory predates U.

Tags: **[P]** PROVEN (read from code, constant read from the DOL), **[I]** INFERRED (meaning of
a field or helper; reasoning given), **[U]** UNKNOWN. Third-party notes
(`H:\Unresolved Shadow functions\native-phys\p9_behaviour2\P9_REPORT.md`,
`...\p3_move\P3_MOVE_REPORT.md`) were used only as hints; every claim below was re-read from the
binary, and field *names* taken from them are tagged [I].

Conventions: `P` = player object = first word of the Behavior handle at `this+4`
(`8006DD60` is `return *(u32*)r3`) [P]. `r2 = 0x805FA780`, `r13 = 0x805EC500`. `dt` = f1 argument
of `update` [I: seconds]. "Bit n" = bit n of the state bit set at `P+0xA8`: word
`P+0xA8+(n>>5)*4`, mask `1<<(n&31)` (8006CEA0 test, 80076DDC set, 80076DFC clear) [P]. "Cap n"
= single word `P+0xA0`, mask `1<<n` (80014E1C set, 800613A4 clear, 80014E34 test) [P].
`fcmpo` is printed by doldis as `.long 0xFC010040` (fcmpo cr0,f1,f0), `0xFC020040`
(f2,f0), `0xFC001040` (f0,f2). Every float compare below is written with its exact NaN
behaviour ("!(a >= b)" means the branch is also taken on NaN).

---------------------------------------------------------------------------------------------

## 0. Shared helpers and player fields

| addr | does [P] | notes |
|---|---|---|
| 800091D0 / 800091EC | vec3 copy `d = s` | |
| 8040DDC4 | `d = a - b` (3x fsubs) | |
| 8040DDF8 / 800091A4 | `d = v * s` (3x fmuls) | 800091A4 = wrapper |
| 8040DE20 | `d = a + b` (3x fadds) | |
| 8040DE88 | `v *= s` in place (3x fmuls) | |
| 8040E048 | `a += b` in place (3x fadds) | |
| 80016284 | `d = -v` (3x fneg) | |
| 8040DEB0 | `v = (0,0,0)` (0x805FBDF8 = 0.0) | |
| 8040DFE0 | dot: `t = ay*by` (fmuls); `t = ax*bx + t` (fmadds); `r = az*bz + t` (fmadds) | fused order matters |
| 8040DF7C | `x*x + y*y + z*z`: three fmuls, `(x2 + y2)` fadds, `z2 + that` fadds | not fused |
| 8040E008 | cross `d = a x b`: `dx = ay*bz - az*by` (fmuls + fmsubs), same pattern for y, z | fused |
| 8040DEC4 / 8040DF38 | normalize in place / into a copy: `len = 8040DFA0(v)`; `len < 1e-6` (0x805FBDFC) -> v = vec at 0x805E1860 (zeroed by 8040E07C) ; else 3x fdivs by len | |
| 8040D198 | copy 12 floats (3x4 matrix) | |
| 8040C6CC | `d = M * v + t`, M row-major 3x4 (rows at +0/+0x10/+0x20, translation at +0xC/+0x1C/+0x2C), fmuls/fmadds/fadds | |
| 800497A0 | 3x4 matrix from axis (r4) and angle (f1): calls 8000FFE8 / 8000F3B0 (sin/cos) and writes 12 floats | [I] axis-angle rotation, zero translation |
| 804103B4 | `out = {atan2(fwd.y, sqrt(fwd.x^2+fwd.z^2)), atan2(-fwd.x, -fwd.z), roll}` from (fwd r4, up r5); roll = atan2 of the up vector after un-rotating yaw (8040E0FC) and pitch (8040E168) | [I] euler {pitch, yaw, roll} |
| 80079810 | `out = 8007975C(P, local)` with `local = (0.0, 0.0, -1.0)` (0x805F3554 = 0.0, 0x805F355C = -1.0); 8007975C copies the matrix at `P+0x1E4`, rotates it by `P+0x10`, `P+0xC`, `P+0x14` (8040C898 / 8040C8D4 / 8040C85C) and applies it (8040C6CC) | [I] forward vector of P |
| 80079854 | `800939E4(handle(P+0x260), *P+0x60)` -> `80093A18(P, P+0x18, P+0x24, P+0xF4, P+0xA8, f1)` | [I] integrate velocity |
| 80097658 | 453-instruction routine; calls the sweep 80098ABC at 80097770 / 80097928 / 80097C20 | [I] collision step |
| 80098ABC -> 80099148 -> 80099358 | per contact, if the boost::function at `P+0x68` is non-empty (80099418), builds `CollisionEventArgs` on the stack (`+8 = contact`, `+0xC = owner handle` via 80052554) and invokes `P+0x68` | [P] contact event |
| 80026588 | `attr(contact) = contact->+0xC ? *(contact->+0xC + 0x1C) : 0` | [P] |
| 8001E9C4 | `face->+0x38 in {1,2} ? face+0x2C : face+0x20` | [I] face normal |
| 800266D4 | `face + 0x20` | [I] face normal (raw) |
| 800A82FC | behaviour change (see 0.1) | [P] |

Player fields used (offsets [P]; names [I] unless stated):

| P+ | width | use here | name |
|---|---|---|---|
| 0x00 | vec3 | probe start, WallJumpCommand payload | position [I] |
| 0x0C/0x10/0x14 | f32 x3 | 0x10 written by wall frame | euler (pitch, yaw, roll) [I] |
| 0x18 | vec3 | zeroed on hang, set on drop / jump | velocity [I] |
| 0x24 | vec3 | `+= dir * k` | force/acceleration accumulator [I] |
| 0x54 | u32 | passed to 8034ED50 in WallHang Enter | [U] |
| 0x60 | ptr | `*(P+0x60)` = f32 passed to the integrator | dt source [I] |
| 0x68 | boost::function<void(CollisionEventArgs&)> | contact event slot (8008636C assign, 800859F4 clear) | [P] |
| 0xA0 | u32 | Cap 1 set on hang | capability flags [I] |
| 0xA8.. | bit set | state bits | [P] |
| 0xB8 | vec3 | stick input | world-space stick, accumulated by command id 1 (800A7D70: if `|v|^2 >= 1e-4` (0x805F3C84): set bit 0x1B, `P+0xB8 += v`) [P] |
| 0xE8 | vec3 | wall-frame reference up | gravity up [I] |
| 0xF4 | vec3 | set to wall normal while running | integration normal [I] (passed to integrator) |
| 0x1C8 | vec3 | set to wall "up" b | ground normal [I] |
| 0x228 | ptr | attach tracker (80078F34 set, 80078F08 clear, 80078ED4 get) | [I] CharaColliMove / attached object |
| 0x22C | ptr | behaviour manager for 800A82FC | [P] |
| 0x230 | ptr | `vcall(+0x18 vtbl, slot 0x10)(id, f1)` | [I] motion change (id, blend) |
| 0x23C | ptr | 800A313C(f1) / 800A3118 | [U] |
| 0x240 | ptr | `vcall(+0x18 vtbl, slot 0x14)(id)` | [U] (sound/voice/effect request) |
| 0x260 | handle | self handle copied into WallJumpCommand | [I] |
| 0x274 | f32 | added to probe length | radius [I] |
| 0x278 | ptr | `+0x24` u8 flag, `+0x20` f32 | [I] ground-shadow probe (hit flag, distance) |

### 0.1 Behaviour change 800A82FC(mgr = P+0x22C, args) [P]

`prev = mgr->+0x30 ? mgr->+0x30->+0x10 : 0x25`; `new = factory(mgr+0x38)->vslot0xC(args->+4 id)`;
`mgr+0x48 = f32 @0x805F3C8C`; if `new && new->canEnter(prev, args)` (vslot +0xC): `old->exit(new->id)`
(vslot +0x14), `mgr+0x30 = new`, `new->enter(prev, args)` (vslot +0x10), return 1; else return 0.

Behavior vtable layout (CodeWarrior: `{rtti, 0, dtor, canEnter, enter, exit, onCommand, update, vf6}`) [P]:

| | vtable | dtor | canEnter | enter | exit | onCommand | update | vf6 |
|---|---|---|---|---|---|---|---|---|
| WallHang | 0x80521CB4 | 80092600 | 800925F8 | 800924C4 | 80092418 | 80092168 | 80091C98 | 800919B0 (blr) |
| WallJump | 0x80521D14 | 8009321C | 80093210 | 80092F04 | 80092BC0 | 80093218 (blr) | 800929FC | 80076DD8 (base blr) |

Behavior base 80076D70 [P]: `+0` vtable, `+4..+0xB` player handle (8006D83C), `+0xC` name
(WallHang: `*(r13-0x6F30)` = 0x804B21E8 "WallHang"; WallJump: `*(r13-0x6EF0)` = 0x804B2390
"WallJump"), `+0x10` id, `+0x14/+0x18` zero.

Requests to Fall (id 2) and Landing (id 3) use a stack `InitializeArgs {vtable 0x80520F18, id}` [P].

---------------------------------------------------------------------------------------------

## 1. WallHang (state 13)

### 1.1 Function table

Instructions executed = count of the function's instructions inside a U / W range.

| address | name / role | instr | U | W |
|---|---|---|---|---|
| 800914D0 | factory: `new(0x50)` -> ctor 80092668 | 18 | 18 | 18 |
| 80092668 | ctor: Behavior(name, id 0xD); vtable 0x80521CB4; `+0x48` handle = {0,0} (80051D4C) | 19 | 19 | 19 |
| 80092600 | dtor: vtable reset, handle `+0x48` released (80051CFC), base dtor 80076C6C, delete if r4>0 | 26 | 26 | 26 |
| 800925F8 | canEnter: `return 1` | 2 | 2 | 2 |
| 800924C4 | enter | 77 | 77 | 77 |
| 80092418 | exit (leave) | 43 | 43 | 43 |
| 80092168 | onCommand | 101 | 63 | 18 |
| 80091C98 | update | 220 | 211 | 209 |
| 800919B0 | vf6 (blr) | 1 | 0 | 0 |
| 800916B8 | helper: wall frame from normal | 88 | 87 | 87 |
| 80091818 | helper: contact handler `WallHang::OnCollision(CollisionEventArgs&)` (PTMF {0,-1,80091818} at 0x80521C9C) | 28 | 28 | 28 |
| 80091888 | helper: wall-contact probe (bool) | 42 | 42 | 42 |
| 80091930 | helper: drop (push off + request Fall) | 32 | 32 | 32 |
| 80091518 | helper: wall-jump direction | 104 | 71 | 0 |
| 80092798 | static init: `[r13+0x3084] (0x805EF584) = 4.0` (0x805F3A0C) | 3 | 3 | 3 |
| 800926B8, 80092734, 80092784, 800926B4 | boost::bind_t<WallHang::OnCollision> construction | 31/20/5/1 | all | all |
| 80092008, 8009207C, 800920D8 | boost::function construction (vtable {manager 800919B4, invoker 80091BA0}, heap functor 0x14 B via 80091B48) | 29/23/36 | all | all |
| 800919B4, 80091A3C, 80091AB0, 80091AE0, 80091B34, 80091B48 | boost manager (op 2 = type check via 803AD96C against RTTI `*(r13-0x6F14)`; op 0 clone; else destroy 803A1334) | 34/29/12/21/5/22 | 19/29/12/21/5/14 | same |
| 80091BA0, 80091BD4, 80091C14, 80091C8C, 80091C90 | boost invoker -> `__ptmf_scall` 803A1AAC | 13/16/30/1/2 | all | all |
| 80088384 / 80088314 | InitializeArgsWallHang ctor / dtor | 25 / 28 | 25 / 26 | 25 / 26 |
| 800922FC / 80092358 | InitializeArgsWallJump dtor / ctor (built by WallHang) | 23 / 25 | 21 / 25 | 0 / 0 |
| 800923BC | handle copy for WallJumpCommand | 23 | 0 | 0 |

Object layout (size 0x50, 800914D0) [P offsets; meanings I where marked]:

| this+ | type | written | meaning |
|---|---|---|---|
| 0x1C | s32 | enter 0, update 1/2 | sub-state [P] |
| 0x20 | vec3 | enter (args normal), OnCollision | wall normal n (points out of the wall) [I] |
| 0x2C | vec3 | update | normalized stick direction [P] |
| 0x38 | f32 | enter 0.125, update `-= dt`, clamp 0 | jump lock [I] |
| 0x3C | f32 | enter 2.0, sub 1 sets 1.0, update `-= dt` | hang / run timer [I] |
| 0x40 | s32 | OnCollision `++`, sub 2 reset 0 | wall contacts since last frame [P] |
| 0x44 | s32 (0/1) | wall frame | side flag (picks motion 0x11/0x12, 0x13/0x14) [P] |
| 0x48 | handle (2 words) | enter (args+0x14) | wall owner handle [I] |

### 1.2 Entry conditions

Callers of 800A82FC with id 13: only through `InitializeArgsWallHang` ctor 80088384 (stores
vtable 0x805219A0, id `li r6,0xD` @80088398, `+8` vec3 normal, `+0x14` handle copy 800520B0).
`doldis --xref 80088384` = 2 sites [P]:

**A. JumpDash contact handler 8008823C** (PTMF {0,-1,8008823C} at 0x80521958; bound by JumpDash
enter 80088948 at 80088A24..80088A9C only when bit 0x25 is clear (80088990 branch); executed U, W).

```
8008823C (this = JumpDash, e = CollisionEventArgs&):
  contact = e->+8
  attr = 80026588(contact) & 0x0FFFFFFF                       ; clrlwi 4  @80088264
  if ((attr & [r13-0x6F28]) == 0                               ; 0x805E55D8 = 0x200 (exclude)
      && (attr & [r13-0x6F2C]) != 0) {                          ; 0x805E55D4 = 0x2  (wall)
      n = *8001E9C4(contact->+0x14)                              ; face normal
      args = InitializeArgsWallHang{ normal = n, handle = copy(e+0xC) }
      ok = 800A82FC(P+0x22C, args)                               ; @800882AC
      ~args (80088314)
      if (ok & 0xFF) return
  }
  800A82FC(P+0x22C, {0x80520F18, 2})                             ; Fall @800882E8 (never executed U/W)
```
[P]. U: 40/54 instructions, W: 40/54; the Fall fallback 800882C8..80088300 never ran (every
contact seen while JumpDash's handler was bound was a wall and WallHang accepted it).

**B. WallJump contact handler 800927F0** (section 2.2, request at 80092894) - never executed.

The wall test is therefore *only* the attribute test: no speed, height, angle or normal threshold
exists on the entry path [P]. CanEnter (800925F8) always returns 1 [P]. The JumpDash requirement
(state 6, entered with bit 0x25 clear) is [P]; the contact must come from the sweep inside the
collision step (80098ABC -> 80099358) [P].

### 1.3 Enter 800924C4 (this, prev r4, args r5) [P unless noted]

```
a = __dynamic_cast(args, 0, &r13-0x6F1C, &r13-0x6F24, 0)        ; 803A1AFC -> InitializeArgsWallHang*
set bit 0x0E ; set bit 0x36                                     ; 80092500, 80092510
set Cap 1   (P+0xA0 |= 2)                                       ; 80092520
P+0x18 = (0,0,0)                                                ; 8040DEB0  velocity
800916B8(this, &a->+8)                                          ; wall frame (1.7)
this+0x20 = a->+8                                               ; wall normal n
this+0x38 = 0.125  (f32 @0x805F3A04, r2-0x6D7C)
this+0x3C = 2.0    (f32 @0x805F3A08, r2-0x6D78)
this+0x1C = 0
this+0x48 = copy(a->+0x14)                                      ; 80052554
tmp = 80334280(); 80334180(tmp, this+0x48); 80078F34(P, tmp); 80334224(tmp)
                                                               ; P+0x228 ? 8009443C(P+0x228, tmp) [I: attach to wall owner]
set bit 0x37                                                    ; 80092598
vcall P+0x240 (vtbl at +0x18, slot 0x14)(0x13)                  ; [U] effect/sound id 0x13
t = 8034ED50(&t, 0xA, P+0x54); 80076930(&t, -1)                 ; [U] (sound-like object, id 0xA)
```

### 1.4 Update 80091C98 (this, dt f1)

```
this+0x38 -= dt;  if (!(this+0x38 >= 0.0)) this+0x38 = 0.0              ; fsubs, fcmpo @80091CC0
this+0x3C -= dt;  if (!(this+0x3C >= 0.0)) { 80091930(this); return }  ; @80091CE0, exit 1 (skips tail) [never executed]
s = (this+0x38 <= 0.0) ? P+0xB8 : (0,0,0)                               ; cror lt|eq @80091CFC
F = 80079810(P)                                                         ; forward
big = (8040DF7C(s) > 0.16)                                              ; 0x805F39F0 = 0.16f (0x3E23D70B); GT bit via rlwinm @80091D50
this+0x2C = normalize_copy(s)                                           ; 8040DF38
switch (this+0x1C):                                                     ; <0 or >=3: no case (80091D80 never executed)
case 0:                                                                 ; 80091D90
   if (big && dot(s, F) > 0.0) this+0x1C = 1                            ; ble @80091DAC (NaN: no change)
   vcall P+0x230 (slot 0x10)(this+0x44 ? 0x12 : 0x11, 5.0)              ; 5.0 @0x805F39F4; every frame in sub 0
   if (!80091888(this)) 80091930(this)                                  ; exit 2, then tail [drop never executed]
   break
case 1:                                                                 ; 80091E04 (lasts one frame)
   set bit 0 ; set bit 0x0D ; clear bit 0x0E ; set bit 0x39
   P+0xF4 = this+0x20
   P+0x68 = boost::function(bind(&WallHang::OnCollision, this, _1))     ; 800926B8, 80092008, 8008636C, 80079DF4
   this+0x1C = 2
   this+0x3C = 1.0                                                      ; 0x805F39F8
   vcall P+0x230 (slot 0x10)(this+0x44 ? 0x14 : 0x13, 10.0)             ; 10.0 @0x805F39FC
   break
case 2:                                                                 ; 80091F28
   P+0x24 += F * 500.0                                                  ; 500 @0x805F3A00; 800091A4 fmuls, 8040E048 fadds
   80079854(P)                                                          ; integrate
   P+0xF4 = this+0x20
   if (!big || !(dot(s, F) > 0.0)) 80091930(this)                       ; exit 3 (cror lt|eq @80091F80; NaN keeps hanging)
   if (this+0x40 == 0)        80091930(this)                            ; exit 4 (U only)
   this+0x40 = 0
tail:                                                                   ; 80091FAC, runs after exits 2-4 too
   g = P+0x278
   if (g->+0x24 (u8) != 0 && !(g->+0x20 >= 10.0)) 80091930(this)        ; exit 5 [never executed]; 10.0 @0x805F39FC
   set bit 2                                                            ; [I] "grounded" pre-set
   80097658(&this->player, 0)                                           ; collision step: fires OnCollision
```

Notes [P]: the stick test uses the *raw* stick `s` (not normalized) for the dot with F; `big`
compares `|s|^2` (not `|s|`) with 0.16, i.e. `|s| > 0.4`. Two drop requests (exit 3 and 4) can be
issued in one frame, and the tail (bit 2 + collision) still runs after a drop.

### 1.5 Contact handler 80091818 `WallHang::OnCollision(e)` (bound in sub 1)

```
contact = e->+8
attr = 80026588(contact) & 0x0FFFFFFF
if (attr & [r13-0x6F2C] (= 2)) {                    ; no 0x200 exclusion here
   this+0x20 = *8001E9C4(contact->+0x14)              ; refresh wall normal
   800916B8(this, this+0x20)                          ; recompute wall frame (yaw, P+0x1C8, side)
   this+0x40 += 1
}
```
[P], executed U and W (28/28).

### 1.6 Wall probe 80091888 -> bool

```
start = P+0x00                                        ; 80029DA4 is an empty ctor (blr)
len   = [0x805EF584] (4.0, from 80092798) + P+0x274   ; fadds
end   = start - this+0x20 * len                       ; 8040DDF8 fmuls, 8040DDC4 fsubs
d = 80097D6C(&this->player, &{start,end}, 0, need=0x1A93, reject=0x40000, flags=2)
return !(d < 0.0)                                     ; mfcr / srwi 31 / cntlzw / srwi 5
```
80097D6C -> 80097DB4 [P]: filter record `{0x1A93, 0x40000}`, line pick 80021E24(r5 = 3, r6 = 2,
filter 0x80099454). Filter 80099454: reject if `attr & 0x40000`; (object-owner exclusions);
accept iff `attr & 0x1A93 != 0` [P]. Result = hit `+0x18` [I: distance]; `<= 0.0`
(0x805F3A90) or no hit -> `-1.0` (0x805F3ACC) [P]. So the probe passes iff a surface with any
attribute bit of 0x1A93 (includes wall 0x2 and floor bit 0x1) lies within `4 + radius` behind
the player along `-n`, at distance > 0. Flag value 2 -> object/character query too
(WORLD_COLLISION.md) [I].

### 1.7 Wall frame 800916B8 (this, nrm)

```
a = normalize(cross(P+0xE8, nrm))                     ; 8040E008, 8040DEC4
b = normalize(cross(a, nrm))
F = 80079810(P)
if (dot(a, F) <= 0.0)      a = -a                     ; cror lt|eq @80091748, 80016284
if (dot(b, P+0xE8) <= 0.0) b = -b                     ; @80091780
c = cross(a, b)
this+0x44 = (dot(c, nrm) >= 0.0) ? 1 : 0              ; bge @800917C4 (NaN -> 0)
e = 804103B4(a, b)                                    ; euler from fwd a, up b
P+0x10 = e.yaw (e+4)                                  ; facing along the wall
P+0x1C8 = b
```
[P]. Note `cross(a, a x n) = -n` for unit `a` orthogonal to `n`, so `this+0x44 = 1` only when b
was flipped [I]. 800917D0 (`side = 1`) was **never executed** in U or W.

### 1.8 Drop 80091930

```
P+0x18 = this+0x20 * 50.0         ; 50 @0x805F39EC; 8040DDF8 (3x fmuls)
800A82FC(P+0x22C, {0x80520F18, 2}) ; Fall
```
[P].

### 1.9 onCommand 80092168 (this, cmd)

`id = (u16) cmd->+4` [P]. `cmd->+7 |= 2` marks the command handled (8009CDAC is the same op) [P].

```
if (id == 0x104) {                                   ; 80092190..80092194
   if (this+0x38 <= 0.0) {                           ; cror lt|eq @800921B4
      dir = 80091518(this)                           ; 1.10
      tgt = 80078ED4(P)                              ; P+0x228 ? (P+0x228)->+0x28 + 0x74 : 0
      if (tgt) {
         h = lock(tgt+4)                             ; 8005223C / 800521AC
         if (h.ptr) {                                ; [never executed]
            c = new(0x28) (801DA024)
            if (c) c = WallJumpCommand(c, handle(P+0x260) /*800923BC*/, P /*copies P+0x00*/) ; 801DBABC
            vcall h.ptr->+0x38 (slot 0x10)(c)        ; deliver to the wall owner
         }
         release h
      }
      args = InitializeArgsWallJump{ vtable 0x80521CE4, id 0x10, +8 = this+0x20, +0x14 = dir }  ; 80092358
      800A82FC(P+0x22C, args)                         ; @800922A4 -> WallJump
      ~args (800922FC)
   }
   cmd->+7 |= 2                                      ; also when the lock is still running
} else if (id == 0x106) {                            ; [never executed]
   if (this+0x38 <= 0.0) 80091930(this)              ; drop (r3 still = this)
   cmd->+7 |= 2
}                                                    ; 0x105, <0x104, >=0x107: ignored, not marked
```
[P]. WallJumpCommand (801DBABC): CharCommand base 8006E57C(id 0xA, 4), vtable 0x8053E4E8,
`+0x14` = player handle, `+0x1C` = copy of `P+0x00` (position) [P]. Its target is the object
tracked by `P+0x228` (registered in Enter from the wall owner handle) [I].

### 1.10 Wall-jump direction 80091518 (out, this)

```
if (!bit 0x1B)                       out = this+0x20               ; [never executed]
else if (dot(this+0x20, this+0x2C) > 0.70710677)  out = this+0x2C  ; 0x805F39D8 = 0x3F3504F3; ble @80091574 [never executed]
else {
   F = 80079810(P)
   d = dot(cross(F, P+0x1C8), this+0x20)
   e = dot(F, this+0x2C)
   deg = (d >= 0.0) ? ((e >= 0.0) ? 45 : -45)       ; [d>=0 branch never executed]
                    : ((e >= 0.0) ? -45 : 45)       ; [e<0 here never executed]
   ang = deg * (pi / 180)            ; fdivs pi(0x805FBDB0 = 0x40490FDB) / 180(0x805F39E4), then fmuls
   M = 800497A0(axis = P+0x1C8, ang) ; copy 8040D198
   out = M * this+0x20               ; 8040C6CC
}
```
Constants [P]: 45 @0x805F39E0, -45 @0x805F39E8, 180 @0x805F39E4. `d >= 0` / `e >= 0` use
`cror gt|eq` (NaN goes to the `< 0` side). The only executed combination (U) is `d < 0, e >= 0`
-> -45 deg. Bit 0x1B = stick present this frame (set by command id 1, 800A7DA8) [P]; this+0x2C is
the normalized stick (zero vector when the lock is running or the stick is tiny) [P].

### 1.11 Exit 80092418 [P]

clear bits 0, 0x0D, 0x0E, 0x36, 0x39; clear Cap 1; `800859F4(P+0x68)` (empty the contact
slot); clear bit 0x37; `80078F08(P)` (P+0x228 -> 800943F4: set an empty attach handle).

### 1.12 Exits (transitions out of WallHang)

| # | site | target | condition | U | W |
|---|---|---|---|---|---|
| 1 | 80091CEC | Fall 2 | `!(this+0x3C >= 0)`: 2.0 s in sub 0, 1.0 s in sub 2 (tail skipped) | no | no |
| 2 | 80091DFC | Fall 2 | sub 0: wall probe 80091888 fails | no | no |
| 3 | 80091F8C | Fall 2 | sub 2: `|s|^2 <= 0.16` or `!(dot(s,F) > 0)` (stick released or pointed away from the run direction) | yes | yes |
| 4 | 80091FA0 | Fall 2 | sub 2: no attribute-2 contact since the previous frame (`this+0x40 == 0`) | yes | no |
| 5 | 80091FD4 | Fall 2 | `P+0x278` hit flag set and its value `< 10.0` (near ground [I]) | no | no |
| 6 | 800922D8 | Fall 2 | command 0x106 after the jump lock | no | no |
| 7 | 800922A4 | WallJump 16 | command 0x104 after the jump lock (0.125 s) | yes | no |

All Fall exits set `P+0x18 = n * 50` first.

### 1.13 Controller inputs

- Stick: `P+0xB8` (world-space vector accumulated by command id 1 in 800A7CE4; bit 0x1B set when
  `|v|^2 >= 1e-4`) [P]. Read only when the jump lock has expired [P]. Holding it with
  `|s| > 0.4` and a positive component along the facing F starts the run (sub 0 -> 1) and keeps
  it (sub 2) [P].
- Wall jump: command id **0x104** (JumpCommand, ctor 8009CD60, vtable 0x80522614, `+0x14` vec3 =
  0) [P]. Player::Control::UserInput sub 800A0130 creates it at 800A077C only if **Cap 1**
  (`P+0xA0 & 2`, test @800A075C) is set and Cap 2 is clear (Cap 2 set branches to 800A0690 and
  skips it, @800A068C); WallHang Enter sets Cap 1 and Exit clears it [P]. Other 0x104 producers:
  800701A8 and 800702F0 (each gated on bit 1 of its object's `+0xA0`), 8009CCCC (variant with a
  direction vector) [P]. The physical pad button that routes into that
  UserInput slot was not traced [U] (third-party notes say "jump press").
- 0x106 (ctor 800795B8, vtable 0x805225E8) = drop [P]; producer button [U].

---------------------------------------------------------------------------------------------

## 2. WallJump (state 16)

### 2.1 Function table

| address | name / role | instr | U | W |
|---|---|---|---|---|
| 800927A8 | factory: `new(0x3C)` -> ctor 80093274 | 18 | 18 | 0 |
| 80093274 | ctor: Behavior(name "WallJump", id 0x10), vtable 0x80521D14 | 17 | 17 | 0 |
| 8009321C | dtor | 22 | 22 | 0 |
| 80093210 | canEnter: `return 1` | 2 | 2 | 0 |
| 80092F04 | enter | 107 | 92 | 0 |
| 80092BC0 | exit (leave) | 24 | 24 | 0 |
| 80093218 | onCommand: `blr` (ignores every command) | 1 | 1 | 0 |
| 800929FC | update | 113 | 73 | 0 |
| 80076DD8 | vf6 (base, blr) | - | - | - |
| 800927F0 | helper: contact handler `WallJump::OnCollision` (PTMF {0,-1,800927F0} at 0x80521CFC) | 63 | 40 | 0 |
| 80092984 | helper: wall-ahead probe along jump dir | 30 | 30 | 0 |
| 800928EC | helper: line probe returning "attribute has 0x2" | 38 | 38 | 0 |
| 800932BC, 80093338, 80093388, 800932B8 | bind_t construction | 31/20/5/1 | all | 0 |
| 800930B0, 80093124, 80093180 | boost::function construction | 29/23/36 | all | 0 |
| 80092C20, 80092CA8, 80092D1C, 80092D4C, 80092DA0, 80092DB4 | boost manager (RTTI `*(r13-0x6EDC)`) | 34/29/12/21/5/22 | 19/29/12/21/5/14 | 0 |
| 80092E0C, 80092E40, 80092E80, 80092EF8, 80092EFC | boost invoker | 13/16/30/1/2 | all | 0 |
| 80092358 / 800922FC | InitializeArgsWallJump ctor / dtor (in WallHang) | 25 / 23 | 25 / 21 | 0 |

Object layout (size 0x3C) [P offsets]: `+0x1C` vec3 jump direction (args+0x14); `+0x28` vec3
velocity saved after integration; `+0x34` s32 set to 0 in Enter, no other use in these
functions [P]; `+0x38` f32 timer.

InitializeArgsWallJump (80092358) [P]: vtable 0x80521CE4, `+4` id 0x10, `+8` vec3 wall normal,
`+0x14` vec3 jump direction.

### 2.2 Entry conditions

Only caller: WallHang onCommand at 800922A4 (section 1.9): command 0x104, `WallHang+0x38 <= 0`
(jump lock 0.125 s expired), args = {WallHang n, 80091518 direction} [P]. No other site
constructs InitializeArgsWallJump (`--xref 80092358` = 1) [P]. CanEnter always 1 [P].

### 2.3 Enter 80092F04 (this, prev, args)

```
a = __dynamic_cast(args, 0, &r13-0x6EE4, &r13-0x6EEC, 0)                 ; InitializeArgsWallJump*
P+0x68 = boost::function(bind(&WallJump::OnCollision, this, _1))         ; 800932BC, 800930B0, 8008636C, 80079DF4
vcall P+0x230 (slot 0x10)(0x10, 10.0)                                     ; 10.0 @0x805F3A2C
this+0x1C = a->+0x14
this+0x34 = 0
if (80092984(this, &a->+8, &a->+0x14)) {                                  ; wall ahead [never executed]
   this+0x38 = 4.0                                                        ; 0x805F3A30
   P+0x18 = this+0x1C * 200.0                                             ; 0x805F3A34
   800A313C(P+0x23C, 5.0)                                                 ; 0x805F3A38 [U]
} else {                                                                  ; executed U
   set bit 0
   this+0x38 = 0.75                                                       ; 0x805F3A3C
   P+0x18 = this+0x1C * 60.0                                              ; 0x805F3A40
}
vcall P+0x240 (slot 0x14)(3)                                              ; [U]
```
[P]. Velocity is replaced, not added (`800091D0` copy) [P].

### 2.4 Wall-ahead probe 80092984 (this, n, dir) and 800928EC (this, P, delta)

```
80092984: d = normalize_copy(dir)
          k = dot(d, n)
          d *= 80.0 * (1.0 / k)        ; fdivs 1.0(0x805F3A18)/k, fmuls 80(0x805F3A14); 8040DE88
          return 800928EC(this, P, d)

800928EC: end = P+0x00 + delta (8040DE20); line = {P+0x00, end} (80020A20)
          attr_out (stack, uninitialised)
          80097DB4(&this->player, 0,0,0, &attr_out, 0, &line, 0, need 0x1A93, reject 0x40000, flags 2)
          return (attr_out & [r13-0x6F2C] (= 2)) != 0
```
[P]. `attr_out` is written (attr & 0x0FFFFFFF, @80097FC4) only on a hit; on a miss the stack word
is stale [P]. The probe length `80 / (d.n)` is a fixed 80 units measured perpendicular to the
wall being left [P arithmetic, I meaning]. No test of `k <= 0` exists (division by a zero or
negative `k` is possible) [P].

### 2.5 Update 800929FC (this, dt)

```
this+0x38 -= dt
if (!(this+0x38 >= 0.0)) { 800A82FC(P+0x22C, {.., 2}); return }    ; Fall, 0.0 @0x805F3A10 [never executed]
if (bit 0) P+0x24 += this+0x1C * 400.0                              ; 0x805F3A1C (executed)
else {                                                              ; [never executed]
   P+0x24 += this+0x1C * 450.0                                      ; 0x805F3A20
   v = normalize_copy(P+0x18) * 113.6                               ; 0x805F3A24 = 0x42E33333
   if (!800928EC(this, P, v)) this+0x38 = 0.0                       ; -> Fall next frame
}
80079854(P)                                                         ; integrate
clear bit 0x1B
80093DE8(&this->player, 20.0 * dt)                                   ; 20 @0x805F3A28 (fmuls); [I] turn toward stick (bit 0x1B) or velocity
this+0x28 = P+0x18
80097658(&this->player, 0)                                          ; collision step: fires OnCollision
if (bit 2) 800A82FC(P+0x22C, {.., 3})                               ; Landing @80092B94 (executed)
```
[P]. Note 80093DE8 runs after bit 0x1B was cleared, so it always takes its "no stick" branch
(804109E0 on `P+0x18`, `P+0x1C8`) [P].

### 2.6 Contact handler 800927F0 `WallJump::OnCollision(e)`

```
contact = e->+8 ; face = contact->+0x14
attr = 80026588(contact) & 0x0FFFFFFF
if (dot(*800266D4(face), this+0x28) > 0.0) return          ; 0.0 @0x805F3A10; NaN continues
if (!bit 0 && !(attr & 0x200) && (attr & 0x2)) {            ; [never executed]
   args = InitializeArgsWallHang{ *8001E9C4(face), copy(e+0xC) }
   800A82FC(P+0x22C, args)                                  ; WallHang @80092894; result ignored
} else
   800A82FC(P+0x22C, {.., 2})                               ; Fall @800928C8 (executed U)
```
[P]. The dot uses `face+0x20` while the WallHang args use 8001E9C4 (`+0x2C` for face types 1/2) [P].

### 2.7 Exit 80092BC0 [P]

clear bit 0; clear bit 5; `800859F4(P+0x68)`; `800A3118(P+0x23C)` [U].

### 2.8 Exits

| # | site | target | condition | U |
|---|---|---|---|---|
| 1 | 80092A58 | Fall 2 | timer `!(this+0x38 >= 0)`: 0.75 s (no wall ahead) or 4.0 s (wall ahead); 0 after a failed in-flight probe | no |
| 2 | 80092B94 | Landing 3 | bit 2 set after the collision step | yes |
| 3 | 800928C8 | Fall 2 | contact whose normal does not point along the saved velocity, while gravity bit 0 is set or the surface is not a 0x2 wall (or is 0x200) | yes |
| 4 | 80092894 | WallHang 13 | same contact, gravity bit 0 clear (wall-ahead jump) and attribute 0x2 without 0x200 | no |

Both executed exits (2 and 3) come from the single recorded wall jump; coverage does not show
whether they happened in the same frame [U].

### 2.9 Controller inputs

None read directly: onCommand is `blr` [P]. The stick bit 0x1B is cleared every frame and the
turn helper is called after the clear [P]. The jump direction is fixed at entry (from WallHang
1.10) [P].

---------------------------------------------------------------------------------------------

## 3. State bits touched

| bit | WallHang | WallJump | meaning |
|---|---|---|---|
| 0 | set sub 1, clear exit | set (no wall ahead), tested update/handler, clear exit | [I] gravity on |
| 2 | set in tail before collide | tested after collide -> Landing | [I] grounded |
| 5 | - | clear exit | [U] |
| 0x0D | set sub 1, clear exit | - | [I] wall run |
| 0x0E | set enter, clear sub 1 / exit | - | [I] wall hang (before run) |
| 0x1B | tested (jump dir) | cleared each update | stick present this frame [P] |
| 0x36, 0x37 | set enter, clear exit | - | [U] |
| 0x39 | set sub 1, clear exit | - | [U] (third-party: normal lock) |
| Cap 1 (P+0xA0) | set enter, clear exit | - | gates JumpCommand 0x104 creation [P]; [I] "can jump" |

---------------------------------------------------------------------------------------------

## 4. Never-executed paths (unverified at runtime)

WallHang (union of U and W):

- 80091CE8-80091CF0: timer expiry drop (exit 1).
- 80091D80: sub-state < 0 default.
- 80091DF8-80091E00: probe-miss drop (exit 2).
- 80091FD0-80091FD4: ground-shadow drop (exit 5).
- 800917D0: side flag = 1 (`this+0x44 = 1`, motions 0x12 / 0x14 never selected).
- 80091518: 80091550-8009155C (no stick -> n), 80091578-80091584 (stick within 45 deg ->
  stick), 800915D0-80091618 (`d >= 0` both signs), 80091650-80091660 (`d < 0, e < 0` -> +45).
  80091688 (`bl 8040D198`) is reported unexecuted although 80091684 and 8009168C are and 8009168C
  is not a branch target: a coverage artefact, executed by control flow [P].
- 80092168: 8009219C-800921A4 (ids > 0x105), 80092214-80092278 (WallJumpCommand creation and
  delivery), 800922C4-800922E4 (0x106 drop).
- 800923BC (handle copy), 800919B0 (vf6), 800919D0-80091A08 (boost type check), 80091B68-80091B84
  (allocation failure), 80092338-8009233C (args delete).
- 8008823C: 800882C8-80088300 (Fall fallback when the wall request fails or the contact is not a
  wall).

WallJump (U only; W never entered):

- 8009300C-80093044: wall-ahead enter path (4.0 s, `vel = dir*200`, 800A313C).
- 80092A38-80092A70: timer-expiry Fall.
- 80092AB4-80092B14: no-gravity thrust 450 and in-flight wall probe (113.6).
- 8009284C-800928A4: WallJump -> WallHang re-entry.
- 80092C3C-80092C74, 80092DD4-80092DF0: boost type check / allocation failure.
- Whether 800928EC ever returned on a miss with stale `attr_out` [U].

## 5. UNKNOWN list

1. The pad button(s) feeding the UserInput slot that emits 0x104 / 0x106 (800A0130 caller
   arguments not traced).
2. `P+0x230` slot 0x10 is a motion request (id, f1) [I]; exact meaning of f1 (5.0 / 10.0) [U].
3. `P+0x240` slot 0x14 ids 0x13 (WallHang enter) and 3 (WallJump enter); 8034ED50(0xA, P+0x54)
   in WallHang enter; `P+0x23C` calls 800A313C(5.0) / 800A3118 [U] (likely audio / effects).
4. Meaning of bits 5, 0x36, 0x37, 0x39.
5. `P+0x278` object (`+0x24` flag, `+0x20` value) [I] ground-shadow probe; only its use is proven.
6. The target object behind `P+0x228` that receives WallJumpCommand (vtable slot 0x10 of
   `h.ptr->+0x38`) and what it does with it.
7. Runtime behaviour of every path in section 4.

## Addendum (2026-11-03): targeted routes and the stick bit

- **The stick bit 0x1B has one setter and one clearer.** It is set only by the executer's command receiver 0x800A7CE4
  (0x800A7DA8), for a stick command whose |v|² ≥ 1e-4 (0x805F3C84); the same code copies v to P+0xB8. It is cleared
  every frame at the end of the executer step 0x800A8160 (0x800A82A0), which also zeroes P+0xB8. WallJump::update
  clears it as well (0x80092B2C) [P].
- **When the stick counts.** Moving the stick in the same input sample as the jump leaves the bit clear at the jump,
  so the "no stick" branch runs. In `.build/player/targeted/wj_*` all four variants gave byte-identical runs. Holding
  the stick 300 ms before A reaches the stick branches: `wj2_*` and `wj3_*`, all strict, 0 mismatches. WallHang went
  from 750 to 766 strictly executed instructions [P, measured].
- **The last unexecuted direction half, 0x800915D0–0x80091618, does not depend on the stick.**
  - 0x800915B0 computes c = F × b, with F the facing from 0x80079810 and b the wall frame at P+0x1C8.
  - 0x800915C0 takes 0x800915D0 only when dot(c, n) < 0 (fcmpo with 0.0 at 0x805F3A84, then cror gt|eq / bne).
  - The stick picks only between the ±45° sub-cases inside each half (0x800915E8, 0x80091634).
  - Every recorded hang was one approach to one wall, so dot(c, n) ≥ 0 every time. Reaching the other half needs a
    hang whose wall frame b points the other way relative to the facing, i.e. a different approach angle or wall
    orientation [P for the condition; which approach gives it: U].
- **Still unexecuted:** the WallHang timer and probe-miss drops, 0x106, and the WallJump wall-ahead path.
