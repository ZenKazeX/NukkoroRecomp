# Player::Behavior::SpinDash (state id 18) and Player::Behavior::Dead (state id 31) - binary specification

Source: PAL `main.dol` only (disassembly via `tools/doldis.py`, cross-checked with the
lifted C++ in `source/.build/dolrecon/all_lifted.cpp`). No code, config or test was written
or changed. Coverage: `.build/player/flat-strict-move/verify.csv.coverage` (file "F", flat
test stage, SpinDash entered once by holding X, route `.build/player/routes/hold_X`).

**Dead has no strict coverage.** Dead was entered on Westopolis by falling (route
`.build/player/routes/mash`), but no strict run covers it: none of the 20 `*.coverage`
files under `.build/player/` contains an address in 0x8007D8E4..0x8007DD54 (the only
nearby range, 0x8007DD58-0x8007DD5C, is the next function). Every Dead claim below is
static.

Tags: **[P]** PROVEN (read from code), **[I]** INFERRED (meaning of a field, helper or
constant; reasoned, not proven), **[U]** UNKNOWN, **[X]** executed in F.

Conventions (same as `EdgeHang.md`): `P` = the player object reached through `this+4`
(`8006DD60` = `return *(u32*)r3`); PlayerShadow keeps its vptr at `P+0x38` (= 0x80522D70,
stored by ctor 0x800AE3B4) and its virtuals are called with `r3 = P` [P].
`r2 = 0x805FA780`, `r13 = 0x805EC500`. "Bit n of P+0xA8" = word `P+0xA8+(n>>5)*4`, mask
`1<<(n&31)` (0x8006CEA0 test, 0x80076DDC set, 0x80076DFC clear) [P]. "Flag n of W"
(W = P+0xA0, P+0xA4, P+0xB4) = single word, mask `1<<n` (0x80014E34 test, 0x80014E1C set,
0x800613A4 clear) [P]. `dt` = the f1 argument of `update` [I: seconds; SpinDash adds it to
a 10.0 limit]. `G` = global block 0x8057E760 (`lis -0x7fa8; addi -0x18a0`); `P+0x60` holds
`G+0x18`, the pointer to the frame `dt` float (ctor 0x800AE3C0-0x800AE3D0) [P].
`fcmpo` is printed by doldis as `.long 0xFC..0040/F840` (opcode 63, xo 32) [P].

Coverage caveat [P]: in F several small callees have no coverage line although the `bl`
before them and the instruction after them are covered (e.g. 0x80052EFC, 0x80053074,
0x800AA204, 0x801D48FC, 0x8034ED50, 0x8040E1D0). Callee-level "executed" counts are
therefore taken from the caller's covered return address, not from the callee's own lines.

---------------------------------------------------------------------------------------

## 0. Shared machinery

### 0.1 Behaviour object, factory, change protocol

| item | evidence | tag |
|---|---|---|
| Base ctor 0x80076D70(this, P, name, id): `+0` vptr 0x80521150, `+4` player handle (0x8006D83C copy), `+0xC` name ptr, `+0x10` id, `+0x14`=`+0x18`=0 (0x80076DC8) | 0x80076D80-0x80076DAC | P |
| Factory 0x800AE8D0 indexes table 0x80522DE8 by id; entry 18 = 0x804B1E84 whose word 0 = 0x8008EDA4; entry 31 = 0x804B0BE0, word 0 = 0x8007D8E4. In super form (bit 0x25) ids 0..3 except 1 are remapped to 0x22; 18 and 31 are not remapped | 0x80522E30, 0x80522E64, 0x800AE8F0-0x800AE924 | P |
| Name strings: SpinDash `+0xC` = *(r13-0x6FC0) = 0x804B1E78 "SpinDash"; Dead `+0xC` = r2-0x7164 = 0x805F361C "Dead" | 0x8008FA60, 0x8007DD14 | P |
| Vtable slots: +0x08 dtor, +0x0C CanEnter(prevId,args), +0x10 Enter(prevId,args), +0x14 Leave(newId), +0x18 onCommand(cmd), +0x1C Update(dt), +0x20 (blr) | calls in 0x800A8374-0x800A83E4 (CanEnter/Leave/Enter), 0x800AF9C8 (onCommand) | P (+0x1C role I) |
| ChangeBehavior 0x800A82FC(mgr=P+0x22C, args): `prev = mgr+0x30 ? cur->id : 0x25`; `new = mgr+0x38->vt+0xC(args+4 id)`; `mgr+0x48 = 10.0` (0x805F3C8C); if `new->CanEnter(prev,args)`: `cur->Leave(new->id)`, replace (0x800A8420), `new->Enter(prev,args)`, return 1; else destroy new, return 0 | 0x800A82FC-0x800A841C | P |
| InitializeArgs: `{vptr 0x80520F18, s32 id}` built on the stack; Ground uses InitializeArgsGround (ctor 0x80084648(r4=0), dtor 0x800845EC) | e.g. 0x8008F5EC-0x8008F600 | P |
| The manager `P+0x22C` is the ShadowExecuter (its update sub2 0x800AF6A4 calls 0x800A82FC with `r3 = this`; `this+0x28 = P`) | 0x800AF778-0x800AF784 | P |

### 0.2 Command delivery

| item | evidence | tag |
|---|---|---|
| Command header: `+0` vptr, `+4` u16 id, `+6` u8, `+7` u8 flags, bit 0x02 = handled; `+8` word (ctor arg) | 0x8006E57C-0x8006E5B8, 0x8006EF58 | P |
| ShadowExecuter onCommand 0x800AF924: (1) id 0x106 + bit 0x15 -> 0x800A2DCC, handled; (2) if not handled: current behaviour `vt+0x18(cmd)`; (3) if not handled: 0x800A7CE4; (4) if not handled: switch on id (7, 8, 0x108, 0x115-0x117); (5) if not handled: `cmd->vt+0xC(cmd, target)` (execute) | 0x800AF958-0x800AFCA0 | P |
| Commands are posted with `P->vt+0x10(cmd)` (vptr at P+0x38) | 0x800A2020-0x800A2030 etc. | P |

### 0.3 Player fields used here (P = PlayerShadow)

| offset | width | meaning | tag |
|---|---|---|---|
| +0x00 | vec3 f32 | position (probe origin in 0x80097108/0x8009702C) | I |
| +0x0C/+0x10/+0x14 | f32 | rotation angles about X / Y / Z (used as rotX/rotY/rotZ angles, 0x8008F010-0x8008F0E4) | P (axis), I (units: radians) |
| +0x18 | vec3 f32 | velocity (zeroed at SpinDash entry, set at launch) | I name, P use |
| +0x54 | s32 | player index (key of per-player game data 0x80178F10) | I |
| +0x60 | ptr | -> frame dt float (G+0x18) | P |
| +0x88 / +0x90 | s32 | lock counters driving bits 0x2A / 0x2E (0x80079360 inc+set, 0x800792F0 dec+clear at 0) | P |
| +0xA0 | u32 | action-enable flags read by UserInput (flag 1 -> 0x104 cmd, flag 5 -> 0x10E cmd, ...) | P |
| +0xA4 | u32 | death/event request flags (flags 2, 3 -> DeadCommand) | P |
| +0xA8 | bitset | state bits | P |
| +0xB4 | u32 | flag 3 tested by 0x800AA204 | P (meaning U) |
| +0xB8 | vec3 | stick direction used for turning when bit 0x1B (0x80093E20-0x80093E2C) | I |
| +0xD0 | vec3 | previous-frame position (stuck test) | I |
| +0xE8 | vec3 | "up" vector for 0x8009702C | I |
| +0x1C8 | vec3 | ground normal | I |
| +0x214 | u32 | flag 1 suppresses the super ring drain | P (meaning I) |
| +0x22C | ptr | ShadowExecuter (behaviour manager) | P |
| +0x230 | obj | motion; `vt(+0x18)+0x10(id, f1 blend)` = set motion | I |
| +0x234 | obj | component; 0x80077A84(obj, 5) / 0x80077A60(obj) | U |
| +0x238 | obj | weapon (Dead enter calls `vt(+0x18)+0x34`) | I |
| +0x23C | obj | effect/attack component (0x800A313C, 0x800A3118, 0x800A2F2C, 0x800A2B7C, 0x800A2B3C) | I |
| +0x240 | obj | sound; `vt(+0x18)+0x14(id)` = play one-shot, `vt(+0x18)+0x18(out,id,owner)` = start loop, returns a 0x1C-byte handle | I |
| +0x274 | f32 | collision radius; static default 5.0 (0x805EF660 <- 0x805F3D08 by .ctors entry 0x800AE70C, stored into P by ctor 0x800AE3C8) | P value, I meaning |

### 0.4 Helpers

| addr | does [P] |
|---|---|
| 0x8040DF7C | `len2(v)` = x*x, y*y, z*z (`fmuls`) then `fadds` (no fused op) |
| 0x8040DFE0 | dot: `t=y*y'` (`fmuls`), `t=x*x'+t` (`fmadds`), `r=z*z'+t` (`fmadds`) |
| 0x804109E0 | split(par_out, perp_out, v, n): `par = n*dot(v,n)`, `perp = v - par` |
| 0x8040DF38 | normalize copy |
| 0x8040DDF8 | `d = v*s`; 0x8040DE20 `d = a+b`; 0x8040DE54 `a -= b`; 0x800091D0 copy; 0x8040DEB0 zero |
| 0x8040E0FC / 0x8040E168 / 0x8040E094 | rotate vec about Y / X / Z by f1 (Y, X, Z component preserved respectively), sin/cos from 0x80031568 |
| 0x80079810 | `fwd(P)` = local (0,0,-1) through 0x8007975C |
| 0x80093DE8(this+4, rate) | turn: target = P+0xB8 if bit 0x1B, else tangential velocity (`split(v, P+0x1C8)` perp) and only if its len2 >= 10.0^2 (0x805E5658, `fmuls`); then 0x80093E90 |
| 0x80093E90(this+4, dir, rate) | turn P toward `dir` at `rate` (180, pi constants 0x805F3A50/0x805FBDB0) |
| 0x80079854(P) | per-frame physics step (0x800939E4 with `*(P+0x60)` dt) - shared, see EdgeHang.md §0 |
| 0x80097658(this+4, 0) | move/collide (453 insns) - shared |
| 0x80097108(this+4, L) | head-clear: cast from P+0 to P+0 + `up_local(P)`*L (0x800797CC), filter 0x1A93 / 0x40000, mode 2 (0x80097DB4); returns 1 if `d < 0.0` (miss) or `d > L` | 
| 0x8009702C(this+4, L) | same, direction P+0xE8 |
| 0x8008EE2C(this) | `0x80097108(this+4, fmsubs(2.0, this+0x20, P+0x274))` i.e. L = 2*r0 - r |
| 0x8008EDF0(this) | `0x8009702C(this+4, fmsubs(2.0, this+0x20, P+0x274))` |
| 0x8008EF54(this) | attack on: `0x800A313C(P+0x23C, f1 = *0x805EF550 - 0.1(0x805F3984))`; set bit 0x35; `0x80077A84(P+0x234, 5)` |
| 0x8008EF04(this) | attack off: `0x800A3118(P+0x23C)`; clear bit 0x35; `0x80077A60(P+0x234)` |

Runtime constants written by .ctors entry 0x8008FA9C (pointer at 0x804AAD34, section 9) [P]:
`*0x805EF550 = 2.0 (from 0x805F3980)`, `*0x805EF554 = 0.25 (0x805F39A8)`,
`*0x805EF558 = 0.5 (0x805F39AC)`. Executed in F (0x8008FA9C-0x8008FAB8 covered) [X].

---------------------------------------------------------------------------------------

## A. SpinDash (id 18)

### A.1 Function table

Vtable 0x80521BAC (RTTI ptr 0x805E5550) [P]. "exec F" = instructions covered in F.

| addr | name (inventory) | role | insns | exec F |
|---|---|---|---|---|
| 8008EDA4 | (creator) | `new(0x58)` (0x803A1380) + ctor | 18 | 18 |
| 8008FA48 | SpinDash::ctor | base(name "SpinDash", id 0x12); vptr; `+0x34` handle init (0x8007D8B4); `+0x50` weak handle init (0x80053064) | 21 | 21 |
| 8008F9E0 | SpinDash::dtor | slot +0x08 | 26 | 26 |
| 8008F9D8 | SpinDash::canEnter | slot +0x0C: `return 1` | 2 | 2 |
| 8008F8D0 | SpinDash::enter | slot +0x10 | 66 | 66 |
| 8008F7EC | SpinDash::exit | slot +0x14 (Leave) | 57 | 57 |
| 8008F728 | SpinDash::onCommand | slot +0x18 | 49 | 26 |
| 8008F41C | SpinDash::update | slot +0x1C | 195 | 109 |
| 8008EDEC | SpinDash::vf6 | slot +0x20: `blr` | 1 | 0 |
| 8008F3DC | update__sub0 | phase 0 (shrink) | 16 | 16 |
| 8008EE68 | update__sub0__sub0 | radius step (shrink/grow) | 39 | 31 |
| 8008F26C | update__sub1 | phase 1 (charge / release) | 92 | 76 |
| 8008EF54 | update__sub1__sub3 | attack on | 24 | 24 |
| 8008EFB4 | update__sub2 | phase 2 (roll) | 174 | 96 |
| 8008EF04 | (shared) | attack off | 20 | 20 |
| 8008EE2C | (shared) | head-clear, local up | 15 | 15 |
| 8008EDF0 | update__sub22 | head-clear, P+0xE8 | 15 | 0 |
| 8009702C | update__sub22__sub0 | cast along P+0xE8 | 55 | 0 |
| 80097108 | (shared) | cast along local up | 57 | 54 |
| 800A2B7C | enter__sub4 | effect create (0x801D2ED8) | 16 | 16 |
| 8008FA9C | (.ctors) | runtime constants | 7 | 7 |
| 800A14D0 | SpinDashCommand::ctor | id 0x10E, vptr 0x805224B4 | 17 | 17 |
| 8009DB5C | SpinDashCommand::vf0 | dtor (slot +0x08) | 23 | 23 |
| 8009C6B0 | SpinDashCommand::vf1 | execute (slot +0x0C, called at 0x800AFC98) | 42 | 42 |

Note: inventory status "UNREACHED_OR_UNTESTED" for 8009C6B0, 8008EE68, 8008EF54, 8008EFB4,
8008F26C, 8008F3DC, 8008F9E0, 8008FA48, 800A2B7C is stale; F covers them [X].

### A.2 SpinDash object layout (size 0x58)

| off | type | meaning | set by | tag |
|---|---|---|---|---|
| +0x00 | ptr | vptr 0x80521BAC | 0x8008FA74 | P |
| +0x04 | handle | player | base ctor | P |
| +0x0C | ptr | "SpinDash" | 0x8008FA60 | P |
| +0x10 | s32 | 18 | 0x8008FA50 | P |
| +0x1C | s32 | phase: 0 shrink, 1 charge, 2 roll | enter 0x8008F968, 0x8008F404, 0x8008F3BC | P |
| +0x20 | f32 | radius at entry (r0) | 0x8008F95C | P |
| +0x24 | f32 | charge time | 0x8008F960, 0x8008F294 | P |
| +0x28 | f32 | roll timer | 0x8008F338, 0x8008F18C, 0x8008F234 | P (not initialised by enter; always written at launch before use) |
| +0x2C | f32 | stuck timer | 0x8008F964, 0x8008F668, 0x8008F6C4 | P |
| +0x30 | u8 | released flag | 0x8008F96C, 0x8008F770 | P |
| +0x34 | 0x1C bytes | looping sound handle (ptr, 3x u16, 2x u8, vec3 @+0xC, word @+0x18; copy 0x8007D768, stop+clear 0x801D48FC) | 0x8008F990 | P layout, I "sound" |
| +0x50 | 8 bytes | weak handle to effect 0x31F (expired test 0x80052EFC, lock 0x80052E0C/0x80052D7C) | 0x8008F9B4 | P layout, I "effect" |

### A.3 Entry

Request paths (both call 0x800A82FC with InitializeArgs `{0x80520F18, 0x12}`):

1. **SpinDashCommand (input)** [P, X]
   - Pad object `pad = 0x8006F9F8(P)` (0x800A1DB8). UserInput 0x800A1D70 calls the command
     builder `Player__Control__UserInput__vf3__sub23` 0x800A0130 (at 0x800A21C0), which fills
     slot pairs; the SpinDash slot is the first word of `sp+0x28` (sub23 `r30`).
   - In sub23, reached after a chain of state-bit checks (0x16, 0x14, 0x10, 0x23, 0x22,
     0x33/0x1F/0xB, 0x1E, executer type 4, 0x800A18B0) [P]:
     `if (P+0xA0 flag 4 && P+0x5C == 1) slot = new cmd 0x109 (0x800A1514)`
     `else if (P+0xA0 flag 5) slot = new SpinDashCommand(r4=2)` (alloc 0x14 via 0x801DA024,
     ctor call 0x800A0560) [P: 0x800A04E8-0x800A0568].
   - P+0xA0 flag 5 is set only by Ground enter (0x80083F8C), Idle enter (0x80087054) and
     Landing enter (0x800890FC) and cleared by their Leave functions (0x80083E84, 0x80086F8C,
     0x80089074) [P: every 0x80014E1C/0x800613A4 call with `addi r3,rX,0xa0; li r4,5`]. So the
     command can only be built in Idle (0), Ground (1) or Landing (3).
   - Dispatch (0x800A21DC-0x800A2260): if the slot is non-null and `pad+0x20 & 0x40`, the
     command is posted with `P->vt+0x10`; otherwise it is deleted (`vt+8(cmd,1)`) [P].
     `pad+0x20 & 0x40` = X held/pressed [I: measured "holding X enters SpinDash"; whether
     +0x20 is held or edge is U].
   - Execute: ShadowExecuter onCommand (0x800AF924) -> current behaviour onCommand ->
     0x800A7CE4 -> `cmd->vt+0xC` = 0x8009C6B0: resolve target (0x8009CFFC), and if non-null
     request id 0x12 on `target+0x22C` (0x8009C714), then `cmd+7 |= 2` [P, X].
   - CanEnter 0x8008F9D8 always returns 1, so the request always succeeds [P].
   - Side effect: 0x8009FE58 sets HUD action icon 9 for a 0x10E command in that slot
     (0x8009FF58-0x800A0000) [P; I "button prompt"].
2. **GroundAttack (id 25) update 0x800842B4** [P, never executed in F]:
   after physics, `if (ga+0x20 flag 1 && !flag 2 && ga+0x1C == 1 && flag 3 && !flag 4)`
   request 0x12 (0x800843B0); other cases with flag 1 && !flag 2 request Ground (0x800843DC).
   Flag 2 is set by GroundAttack onCommand on cmd 0x102 (0x800846C4, unless bit 0x1E), flag 3
   is cleared on cmd 0x103 (0x800846E0), flag 4 is set by GroundAttack enter (0x80084B34).
   Who sets flags 1 and 3 is [U] (no 0x80014E1C call inside GroundAttack; likely the motion
   callback bound at 0x80084B5C-0x80084BCC [I]). Cmds 0x102/0x103 are built for the
   `pad & 0x20` button (0x800A0640-0x800A066C, 0x800A1FF8-0x800A2030) [P].

### A.4 Enter (0x8008F8D0) [P, X all 66]

```
// args r4=prevId, r5=args: unused (r4 overwritten at 0x8008F8D8)
set bit 0x3E                                   // 8008F8F0
P+0xA0 flag 1 = 1                              // 8008F900   (enables 0x104 cmd building)
set bit 0                                      // 8008F910
P+0x230->vt(+0x18)+0x10(motion 0xC, f1=5.0)    // 8008F91C [0x805F39A4]=5.0
P+0x18 = (0,0,0)                               // 8008F93C 0x8040DEB0
this+0x20 = P+0x274                            // 8008F950/95C   r0
this+0x24 = 0.0; this+0x2C = 0.0               // [0x805F3988]
this+0x1C = 0; this+0x30 = 0                   // 8008F968/96C
tmp = P+0x240->vt(+0x18)+0x18(&tmp, 0xC, this) // 8008F984  start loop sound 0xC [I]
this+0x34 = tmp                                // 8008F990 0x8007D768
eff = 0x800A2B7C(P+0x23C, P, &P+0xC, 0x31F)    // 8008F9A8 -> 0x801D2ED8 (effect 0x31F) [I]
this+0x50 = eff (weak); destroy temp            // 8008F9B4, 8008F9C0
```

### A.5 Update (0x8008F41C) [P]

```
f30 = dt
switch (this+0x1C) {                // 8008F444-8008F468 (default: skip, 8008F45C never [X])
 case 0: Shrink();  case 1: Charge();  case 2: Roll(); }
0x80079854(P)                       // physics step              [X]
0x80097658(this+4, 0)               // move/collide              [X]
if (!expired(this+0x50)) {          // 8008F494-8008F4A0          [X]
   e = lock(this+0x50)
   0x80426EB8(e, *0x800AAB48(P+0x230, 0))      // place effect on motion node 0 [I]
   if (P->vt+0x3C())  e+0x18 |= 8   else e+0x18 &= ~8      // vt+0x3C = 0x80077350 = bit 0x27
}                                    // true branch 8008F504-8008F518 never [X]
if (this+0x1C == 2) {               // 8008F53C
   split(&par, &perp, P+0x18, P+0x1C8)          // 8008F55C
   if (len2(par) > len2(perp) && Head(this)) {  // fcmpo 8008F574; 0x8008EE2C
      if (len2(par) > 270*270)                   // [0x805E5544]=270.0 (r13-0x6FBC), fmuls 8008F598
         par = normalize(par) * 270.0
      P+0x18 = par + perp
      ChangeBehavior(Fall=2); return             // 8008F60C, branch to epilogue 8008F624
   }                                             // block 8008F57C-8008F624 never [X]
   if (bit 0x1B) {                               // 8008F634
      d2 = len2(P+0 - P+0xD0)                    // 0x8040E1D0
      if (d2 < 0.5*0.5) {                        // [0x805EF558]=0.5, fmuls 8008F650
         this+0x2C += dt                         // fadds 8008F664
         if (this+0x2C >= 0.1 && HeadE8(this)) { // [0x805F39A0]=0.1 (0x3DCCCCCD), 0x8008EDF0
            AttackOff(); ChangeBehavior(Ground=1, InitializeArgsGround(0))   // 8008F6AC
         }
         goto tail
      }
   }
   this+0x2C = 0.0                               // 8008F6C4  [X]; block 8008F640-8008F6BC never [X]
}
tail:
if (bit 2) P+0xA0 flag 1 = 1 else P+0xA0 flag 1 = 0   // 8008F6D4-8008F700; clear never [X]
```

**Shrink (0x8008F3DC -> 0x8008EE68(this, 1))** [P, X]
```
r = P+0x274 - 0.25            // fsubs 8008EE9C, [0x805EF554]
if (r <= 2.0) { r = 2.0; done = 1 }      // fcmpo + cror lt|eq 8008EEA0, [0x805EF550]
P+0x274 = r                   // 8008EEE0 (per frame, not dt-scaled)
if (done) this+0x1C = 1       // 8008F404
```
The grow branch (0x8008EEB8-0x8008EED4: `r += 0.25; if (r >= this+0x20) r = this+0x20`) is
unreachable: the only call site 0x8008F3F4 passes `li r4,1` [P]. With the 5.0 default
radius, the shrink takes 12 frames (5.0 - 12*0.25 = 2.0, clamped on `<=`) [I: radius at
entry may differ from the static default].

**Charge / release (0x8008F26C)** [P]
```
this+0x24 += dt                                   // fadds 8008F290
if (this+0x24 > 10.0) { ChangeBehavior(Idle=0); return }   // [0x805F3994]; 8008F2A4-8008F2DC never [X]
0x80093DE8(this+4, 2.0*dt)                        // fmuls 8008F2E8 [0x805F3980]; turn while charging [X]
if (this+0x30 == 0) return                        // 8008F2F8
k = (this+0x24 > 1.0) ? 1.0 : this+0x24          // fcmpo 8008F304 [0x805F3998]; F took k = 1.0 (8008F310 never [X])
set bit 9                                         // 8008F320
AttackOn()                                        // 0x8008EF54: 0x800A313C(P+0x23C, 2.0-0.1=1.9), set bit 0x35, 0x80077A84(P+0x234,5)
this+0x28 = 2.0*k                                 // fmuls 8008F334: roll time 0..2 s
P+0x18 = fwd(P) * fmadds(850.0, k, 150.0)         // 8008F354 [0x805F399C]=850, [0x805F3990]=150: 150..1000
P+0x240->vt(+0x18)+0x14(0xD)                      // 8008F384 one-shot sound 0xD [I]
t = 0x8034ED50(&t, 1, P+0x54); 0x80076930(&t,-1) // [U] purpose (callee absent from F)
clear bit 0x3E                                    // 8008F3B4
this+0x1C = 2                                     // 8008F3BC
```
The launch happens on the first Charge call after the release flag was set; a release
during the shrink phase is latched in `+0x30` and launches as soon as phase 1 runs [P].

**Roll (0x8008EFB4)** [P]
```
if (bit 0x1B) {                                   // 8008EFF8; block 8008F004-8008F0FC never [X]
   a = P+0xC, b = P+0x10, c = P+0x14
   0x80093DE8(this+4, 2.0*dt)                     // steer by stick
   v = P+0x18
   v = rotY(v,-b); v = rotX(v,-a); v = rotZ(v,-c) // 0x8040E0FC/E168/E094 with fneg
   v = rotZ(v, P+0x14); v = rotX(v, P+0xC); v = rotY(v, P+0x10)   // new angles
   P+0x18 = v                                     // velocity follows the body turn
} else
   0x80093E90(this+4, P+0x18, 2.0*dt)             // 8008F114: body turns toward velocity [X]
if (dot(P+0x18, P+0x1C8) >= 0.0)                  // 8008F124 fcmpo + cror gt|eq
   P+0x18 -= P+0x1C8 * dot(P+0x18, P+0x1C8)       // 8008F138-8008F170 never [X]
v2 = len2(P+0x18)                                 // 8008F17C
this+0x28 -= dt                                   // fsubs 8008F188
if (this+0x28 <= 0.0 || v2 < 22500.0) {           // [0x805F398C]=22500=150^2; 8008F194-8008F1A8
   if ((P+0xB4 flag 3) && Head(this)) {           // 0x800AA204, 0x8008EE2C
      AttackOff()                                 // 0x8008EF04
      ChangeBehavior(Ground=1, InitializeArgsGround(0))   // 8008F1EC [X]
   } else {
      P+0x18 = normalize(P+0x18) * 150.0          // 8008F200-8008F22C [X]
      this+0x28 = 0.0
   }
}
```
Ground following [P/I]: (a) the roll removes the ground-normal velocity component only
when it points away from the ground (`dot >= 0`), (b) Update leaves to Fall when the
normal component dominates (`len2(par) > len2(perp)`) and there is head room, clamping the
normal part to 270, (c) `P+0xA0 flag 1` mirrors bit 2 every frame [I: bit 2 = grounded,
flag 1 = jump allowed]. In F the dot was always negative and the Fall exit never ran [X].

How it ends: the roll lasts `2k` seconds (or until speed < 150); then, each frame,
it leaves to Ground if `P+0xB4 flag 3` and the head-clear test pass; otherwise speed is
forced to 150 along the current direction and the timer stays at 0, so the next frame
retests (keeps rolling at 150 under a low ceiling) [P]. Speed loss during the roll comes
from the shared physics step 0x80079854 / 0x80097658 [U: friction values not analysed].

### A.6 onCommand (0x8008F728) [P]

No return value; handling is signalled by `cmd+7 |= 2`.

| cmd id | action | exec F |
|---|---|---|
| 0x107 (release; built by 0x800A2548 when `pad+0x24 & 0x40`, posted at 0x800A2070-0x800A20A8) | `this+0x30 = 1`; stop sound `0x801D48FC(this+0x34)`; handled | X |
| 0x104 (built by 0x8009CD60 when P+0xA0 flag 1; execute 0x8009CC1C requests Jump 5 if P+0xA0 flag 1) | if `!Head(this)`: handled (blocks the jump); else not handled -> executes -> Jump | never |
| 0x113 (built by 0x800A16C4 under bit 0x23; execute 0x8009C0A4 requests id 0x1C) | if `!Head(this)`: `P+0x240->vt(+0x18)+0x14(0)` (sound 0 [I: refusal buzz]) and handled; else not handled -> id 0x1C | never |
| other | not handled | X (fall-through 0x8008F7D8) |

### A.7 Exit / Leave (0x8008F7EC) [P, X all 57]

```
clear bit 0x3E; P+0xA0 flag 1 = 0; clear bit 0; clear bit 9
P+0x274 = this+0x20                    // radius restored
AttackOff()                            // 0x8008EF04
0x801D48FC(this+0x34)                  // stop loop sound
if (!expired(this+0x50)) { e = lock; e+0x18 |= 0x10; e+0x48 = e+0x4C = e+0x50 = 0 }   // [I] kill effect
```

### A.8 Exits

| target | condition | site | exec F |
|---|---|---|---|
| Idle (0) | phase 1, charge time > 10.0 s | 0x8008F2C4 | never |
| Ground (1) | phase 2, (timer <= 0 or speed^2 < 22500) and P+0xB4 flag 3 and head clear (local up, L = 2*r0 - r) | 0x8008F1EC | X |
| Ground (1) | phase 2, bit 0x1B, moved < 0.5 per frame for >= 0.1 s, head clear along P+0xE8 | 0x8008F6AC | never |
| Fall (2) | phase 2, `len2(par) > len2(perp)` w.r.t. P+0x1C8, head clear; normal part clamped to 270 | 0x8008F60C | never |
| Jump (5) | cmd 0x104 not blocked (head clear) -> execute 0x8009CC1C (needs P+0xA0 flag 1) | 0x8009CC90 | never |
| ChaosBlast (0x1C) | cmd 0x113 not blocked -> execute 0x8009C0A4 | 0x8009C108 | never |
| any | external requests (damage, death, ...) through the executer; SpinDash has no CanEnter veto on others | - | - |

Note [P]: after a request in Roll (0x8008F1EC) Update continues (physics, effect, phase-2
tail) on the same object; the Fall request returns directly.

### A.9 Never-executed SpinDash instructions in F

0x8008F45C; 0x8008F504-0x8008F518; 0x8008F57C-0x8008F624; 0x8008F640-0x8008F6BC;
0x8008F6F4-0x8008F700; 0x8008F75C-0x8008F764; 0x8008F788-0x8008F7D4; 0x8008EEB8-0x8008EED4
(unreachable); 0x8008F2A4-0x8008F2DC; 0x8008F310; 0x8008F004-0x8008F0FC; 0x8008F138-0x8008F170;
0x8008EDF0-0x8008EE28; 0x8009702C-0x80097104; 0x800971BC-0x800971C0, 0x800971CC (head cast
never hit); 0x8008EDEC. GroundAttack 0x800842B4 (all 206).

---------------------------------------------------------------------------------------

## B. Dead (id 31)

### B.1 Function table (nothing executed in any strict run)

Vtable 0x80521624 (RTTI ptr 0x805E5238) [P].

| addr | name | role | insns | exec F |
|---|---|---|---|---|
| 8007D8E4 | (creator) | `new(0x28)` + ctor | 18 | 0 |
| 8007DD0C | Dead::ctor | base(name "Dead", id 0x1F); vptr; `+0x1C` weak handle init | 19 | 0 |
| 8007DC58 | Dead::dtor | slot +0x08 | 45 | 0 |
| 8007DC30 | Dead::canEnter | slot +0x0C | 10 | 0 |
| 8007DAA4 | Dead::enter | slot +0x10 | 99 | 0 |
| 8007DA44 | Dead::exit | slot +0x14 | 24 | 0 |
| 8007D9F0 | Dead::onCommand | slot +0x18 | 21 | 0 |
| 8007D92C | Dead::update | slot +0x1C | 49 | 0 |
| 80076DD8 | (base) | slot +0x20: `blr` | 1 | - |
| 8009F258 | DeadCommand::ctor | CharCommand id 8, vptr 0x8053E530 (execute slot 0x801DA09C, generic) | 17 | 0 |
| 8009F17C | Controller update sub0 sub4 | posts DeadCommand from P+0xA4 | 55 | 25 |
| 800AF924 | ShadowExecuter onCommand | cmd 7 / 8 -> Dead | 234 | 108 |
| 800AF6A4 | ShadowExecuter update sub2 | super ring drain -> Dead | 69 | 16 |
| 80076794 | ElectricCircuit::onCommand | cmd 7 -> Dead | 56 | 0 |
| 800798A4 | PlayerShadow vt+0x50 | "hit is lethal" | 38 | 0 |

Layout (0x28 bytes): `+0x1C` weak effect handle (8 bytes), `+0x24` u8 notified flag [P].

### B.2 Entry - what decides death

All paths call 0x800A82FC with `{0x80520F18, 0x1F}` [P].

| # | path | condition | tag |
|---|---|---|---|
| 1 | Collision 0x80098ABC (in move/collide): contact attribute word `r23 & 0x20` -> `P+0xA4 flag 2` (0x80098FF0-0x80099004) | touching a surface with attribute 0x20 | P (I: kill surface / pit) |
| 2 | Probe 0x800988A8 (cast with filter 0x24, along P+0x1C8 by `P+0x274 - c`): hit attribute `& 0x20` -> `P+0xA4 flag 3` (0x80098A64-0x80098A7C) | probe hit with attribute 0x20 | P (I: variant, e.g. liquid) |
| 3 | Controller 0x8009F17C (called from 0x8009F9FC): flag 2 -> clear it, clear bit 0x13, post DeadCommand; flag 3 -> clear it, set bit 0x13, post DeadCommand (`new DeadCommand(r4=4)` 0x8009F228, `P->vt+0x10`) | | P |
| 4 | Executer onCommand case 8 (DeadCommand): request 0x1F, handled (0x800AFB2C-0x800AFB68) | unconditional | P |
| 5 | Executer onCommand case 7 (damage): if `!bit 0x17 && !bit 0x18 && cur->id != 0x1E`: if `P->vt+0x50()` request 0x1F (0x800AFAC8) else request Damage (30) with InitializeArgsDamage built from the cast command (0x803A1AFC, 0x800724F4) | lethal hit | P |
| 6 | ElectricCircuit onCommand 0x80076794 case 7: same guard (bits 0x17, 0x18); lethal -> request 0x1F (0x80076828); else drop 10 rings (0x800B8130(this+4, 10)) and 2.0 s invulnerability (0x8007915C, [0x805F3504]=2.0) | | P (I: rings, invulnerability) |
| 7 | Executer update sub2 0x800AF6A4, only while bit 0x24: `exe+0x50 += *(P+0x60)` (dt); when `>= 1.0` subtract 1.0 and, unless `P+0x214 flag 1`, `count += -1` (0x8016F66C on data+0x28); then if `count <= 0`: bit 0x25 -> request 0x1F (0x800AF784), else end the effect 0x800A2F2C(P+0x23C) | super form at 0 rings | P (I: count = rings, 1 per second) |
| 8 | Other DeadCommand creators: ShootingGame::Balloon vf1 (0x8013C5E8), fn_801B6344 (0x801B6398) | not analysed | U |

Lethal test `P->vt+0x50` = 0x800798A4 [P]: return 0 if bit 0x24 or bit 0x20; return 0 if
`P+0x54 < 0` or no per-player data (`0x80178F10(0x8007993C(), P+0x54)`); else return
`*(data+0x28) == 0` (0x8016F664, `cntlzw/srwi`) [I: ring count == 0].

Falling on Westopolis [I]: no height threshold was found on any of these paths; the
observed fall death is most likely path 1 -> 3 -> 4 (a kill surface with attribute 0x20).
Not verified: no strict run covers it [U].

CanEnter 0x8007DC30 [P]: `if (prevId == 0x1F) return 0; return *(u8*)(G+0xB5) == 0;`
(G+0xB5 = 0x8057E815, meaning [U]; a nonzero byte blocks every death).

### B.3 Enter (0x8007DAA4) [P, not executed]

```
P->vt+0x48()                                   // 0x80076EBC: release object held in P+0x290 [I]
if (P+0x238) P+0x238->vt(+0x18)+0x34()         // weapon [I]
if (bit 0x24) 0x800A2F2C(P+0x23C)              // end super effect [I]
set bit 0; set bit 0x12                         // 8007DB1C, 8007DB2C
P+0x230->vt(+0x18)+0x10(motion 0x2A, f1=5.0)    // [0x805F3618]=5.0
0x800795FC(P)                                  // clear bits 0x23 and 0x22
0x80079360(P)                                  // P+0x88++ -> set bit 0x2A; P+0x90++ -> set bit 0x2E
P+0x240->vt(+0x18)+0x14(bit 2 ? 0x10 : 0x11)   // 8007DB74-8007DB94 sound [I: grounded vs air]
if (bit 0x13) {
   id = (G+0xA8 == 0x1F4) ? 0x272 : (G+0xA8 == 0x25B) ? 0x276 : -1   // 8007DBBC-8007DBE0
   if (id > 0) this+0x1C = 0x800A2B3C(P+0x23C, id)                   // effect [I]; G+0xA8 = stage number [I]
}
this+0x24 = 0                                  // 8007DC18
```
Bits 0x2A / 0x2E gate the user input: 0x800A1DEC-0x800A1E00 skips command building while
bit 0x2A is set, 0x800A20C8-0x800A20DC skips the stick command while bit 0x2E is set [P].

### B.4 Update (0x8007D92C) [P, not executed]

```
if (this+0x24 == 0) {
   s = 0x80074598()
   if (s+8 == 3 && s+0xC == 0) { this+0x24 = 1; 0x801762D0(s, 9, 0) }   // [I] notify game flow once
}
0x80079854(P)                       // physics step (bit 0 set: falls/settles) [I]
0x80097658(this+4, 0)               // move/collide
if (!expired(this+0x1C)) 0x80426E9C(lock(this+0x1C), P)   // effect follows player [I]
```
No `fmadds`/`frsp`/`fctiwz`/paired-single math in any Dead function [P]; the only float
in Dead is the 5.0 motion blend.

### B.5 onCommand (0x8007D9F0) [P]

```
id = cmd+4 (u16)
if (id < 0x100) return                       // not handled (all CharCommands, incl. 7, 8)
if (id == 0x101 || id == 0x117 || id == 0x127 || id == 0x137) return   // pass through
cmd+7 |= 2                                    // every other player command is swallowed
```
The `id == 0x15` compare at 0x8007DA14 is dead (already returned for `< 0x100`) [P].

### B.6 Exit / Leave (0x8007DA44) [P]

`clear bit 0; clear bit 0x12; clear bit 0x13; 0x800792F0(P)` (P+0x88--, clear 0x2A at 0;
P+0x90--, clear 0x2E at 0). The effect at +0x1C is not stopped here (dtor only) [P].

### B.7 Exits

Dead never calls 0x800A82FC [P]. CanEnter rejects 31 -> 31. It leaves only when another
system requests a state (e.g. respawn/restart) [U: who; not in these functions].

### B.8 Never-executed Dead instructions

All of 0x8007D8E4-0x8007D928, 0x8007D92C-0x8007DD54, 0x8009F258-0x8009F298,
0x80076794-0x80076870, 0x800798A4-0x80079938; in F also 0x8009F1AC-0x8009F1CC,
0x8009F1E8-0x8009F208, 0x8009F214-0x8009F240, 0x800AF6D0-0x800AF7A0, 0x800AF980-0x800AF994,
0x800AFA4C-0x800AFB6C, 0x800AFBB4-0x800AFC6C.

---------------------------------------------------------------------------------------

## C. UNKNOWN list

1. `pad+0x20` vs `pad+0x24` semantics (held / pressed / released) and the 0x40 = X mapping
   (only the user measurement supports X).
2. GroundAttack flags 1 and 3 (who sets them) - the SpinDash entry from GroundAttack.
3. `P+0xB4` flag 3 (required for the normal roll -> Ground exit).
4. Meaning of state bits 0, 0x3E, 0x35, 0x27, 0x1B; `P+0x234` component and value 5.
5. 0x8034ED50(1, P+0x54) at launch (rumble? camera?).
6. Friction / speed loss during the roll (inside 0x80079854 / 0x80097658).
7. Radius at SpinDash entry (static default 5.0, runtime value not measured).
8. Dead: G+0xB5 (death veto), G+0xA8 values 0x1F4/0x25B (stages?), 0x80074598 /
   0x801762D0(9) (game-flow notice), what leaves Dead, Balloon/0x801B6344 DeadCommands,
   and whether the Westopolis fall goes through path 1 (no strict coverage).
