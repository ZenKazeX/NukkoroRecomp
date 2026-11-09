# How the game draws each gadget / prop class (static RE, PAL GUPP8P main.dol)

2026-10-29. Static only (doldis/capstone, extracted `stg*_gdt.one` libraries and DFFs; layouts `stgNNNN_*.dat`).
VERIFIED = read in instructions or file bytes; INFERRED = follows from names / RW semantics, not every path read.
Tools (scratchpad, session 4b3aada8): `factories.py`, `ftab2.py` (factory table), `ctors.py`, `cls2.py` (per-class holder
calls), `libsum.py` (library parts + DFF layers), `agcensus.py` (ART GADGET/WORK layer census), `rsscan.py`/`rsclass.py`
(render-state sites), `exall.py` (all gdt archives -> `scratchpad/libs/`).

## 0. Headline

- The layout factory table is **0x8052C1A0**, indexed by the slot type (entry +0x10): 286 pointers, null-terminated at
  0x8052C618 (VERIFIED: 0x800CA118 `lis/addi 0x8052C1A0; lwzx [type*4]`; 0x800CBD5C stores 0x8052C1A0 to manager +0x14).
  197 entries have a create function: 171 of kind 1 (+0x1A = 1, gadgets/props/collisions/controllers) and 26 of kind 2
  (enemies/bosses). The other 89: 72 kind-4 entries (unnamed or character names: weapons, vehicles, partners - no create)
  and 17 Start/End markers or create-less structs. Full list in section 9.
- Ramp-like classes in the DOL: **DASHPANEL** (id 3), **JUMP_BOARD** (id 4, the "dash ramp"), **DASHRING** (id 6), SPRING
  (1), LONGSPRING (2), CATAPULT, ELEC_SPRING, POP FOOTING, GRINDGADGET, RAIL SWITCH, PLANE RAIL. There is **no** RAINBOWRING
  factory (no string, no table entry). DASHRING has **no far stand-in** (not in `kRules`): 80 layout entries in 14
  stages (none in Westopolis). Westopolis has 18 JUMP_BOARD, 5 DASHPANEL, 5 SPRING, 1 LONGSPRING entries (section 2).
- No listed class sets render states in its own code; the only gadget classes that do are ELEC_FAN (cull NONE),
  the ELEC_BAR/ELEC_LIGHT draw 0x8015390C (additive, cull NONE, zwrite off, fog off) and ELEC_FOOTINGROTATE
  0x802DE4BC/0x802DF140 (additive). Everything else is drawn by RpWorldRender of its parts' layer worlds, so **its
  blend/depth/cull/fog states are the layer's** (0x80046D80) and per-atomic only the holder callback 0x801645D0
  (material colour/alpha) changes anything.
- Proxies that ignore the layer states draw wrong in these classes (section 3 catalogue): HINTRING (layer 11 `_K_`,
  additive, *not* `_AL_`), SPRING/LONGSPRING glow, DASHRING glow, CAGE glows, FAN glares, BRK_OBJ bk02, TORNADO
  (`_AF_` alpha), POWER_DEVICE_CAGE (alpha colour records + `_G_` glare), CityLaser, ~600 ART GADGET/WORK atomics.

## 1. Layer table and the layer render states (0x80046D80) - verified, with one correction

`LandDispManager` table 0x8051E328, 0x18 per layer: +0 name, +4 task name, +8 flags, +0xC task group, +0x10 RpWorld.
Dumped (VERIFIED):

| # | name | flags | group | | # | name | flags | group |
|---|---|---|---|---|---|---|---|---|
| 0 | `_N_` | 0x28 | 1 | | 8 | `_AF_` | 0x5C | 5 |
| 1 | `_D_` | 0x28 | 1 | | 9 | `_A_` | 0x5C | 5 |
| 2 | `_OS_` | 0x2C | 1 | | 10 | `_AL_` | 0x4D | 5 |
| 3 | `_OSL_` | 0x2D | 1 | | 11 | `_K_` | 0x9C | 7 |
| 4 | `_O_` | 0x2C | 1 | | 12 | `_M_` | 0x11C | 7 |
| 5 | `_OW_` | 0x3C | 1 | | 13 | `_G_` | 0x98 | 9 |
| 6 | `_OL_` | 0x2D | 1 | | 14 | `_GL_` | 0x99 | 9 |
| 7 | `_P_` | 0x80C | 1 | | | | | |

The part -> layer index is used directly as the table index (0x80163C14: `mulli r5,idx,0x18; addi 0x8051E328; lwz +0x10`
at 0x80163C64..0x80163C78 / 0x80163CCC..0x80163CE0), VERIFIED. Maps: ATTR_WORLDPARM -> 0x8052C7E0 =
{4,4,5,6,2,1,8,9,11,13,14,12}; record +0x30 -> 0x804C39BC = {6,4,2,5,1,9,8,4,11,11,13,14,12}; light group 0x804C397C =
{4,5,6,7,0,1,2,3,8..15}.

0x80046D80(land, flags, world), RwRenderStateSet = `lwz r5,0x615C(r13); lwz r12,0x20(r5); bctrl` (r3 state, r4 value).
The blend branches are exclusive, tested in this order (VERIFIED, addresses = first state call of the branch):

| test | where | SRCBLEND / DESTBLEND | ZTEST | ZWRITE | GXSetAlphaCompare 0x80498F74 | GXSetZCompLoc 0x804987C8 |
|---|---|---|---|---|---|---|
| 0x40 | 0x80046DD8 | 5 SRCALPHA / 6 INVSRCALPHA | 1 | 0 | (7,0,0,7,0) ALWAYS | 1 |
| 0x80 | 0x80046E64 | 5 SRCALPHA / **2 ONE** | 1 | 0 | ALWAYS | 1 |
| 0x100 | 0x80046EF0 | **0xA / 1 ZERO** | not set | not set | ALWAYS | 1 |
| 0x800 | 0x80046F4C | 5 / 6 | 1 | 1 | (4 GREATER, 0xC8, 0, 7, [0x805E42D0+0x4D]) | 0 |
| 0x400 | 0x80046FE0 | 5 / 6 | 0 | 0 | ALWAYS | 1 |
| none | 0x8004706C | 5 / 6 | 1 | 1 | (4 GREATER, 1, 0, 7, [0x805E42D0+0x4D]) | 0 |

then 0x8 -> TEXTUREFILTER (9) = 2 LINEAR else 0 (0x800470F4); 0x10 -> CULLMODE (0x14) = 1 NONE else 2 BACK (0x80047130);
0x4 -> FOGENABLE (0xE) = land+0x1809 else 0 (0x8004716C); bit 0 -> lit (world flags |= 0x30, lights attached 0x80047E24).

Corrections to the coordinator's decoding: (a) 0x100's source factor 0xA is **rwBLENDINVDESTCOLOR** in RW 3.7
(`RW37SDK/.../rwplcore.h`: ZERO 1, ONE 2, SRCCOLOR 3, INVSRCCOLOR 4, SRCALPHA 5, INVSRCALPHA 6, DESTALPHA 7, INVDESTALPHA 8,
DESTCOLOR 9, INVDESTCOLOR 10), so 0x100 = src*(1-dst), and it leaves ZTEST/ZWRITE as the previous layer left them;
(b) 0x80 also sets ZTEST 1. Everything else in the decoding is right. Only `_M_` (12) has 0x100.

Per-layer result (what a part in that layer gets):

| layer | flags | blend | z | cull | other | **flag** |
|---|---|---|---|---|---|---|
| `_N_` `_D_` | 0x28 | alpha, AC>1 | test+write | NONE | linear | no-cull |
| `_OS_` `_O_` | 0x2C | alpha, AC>1 | test+write | BACK | linear, fog | - |
| `_OSL_` `_OL_` | 0x2D | alpha, AC>1 | test+write | BACK | linear, fog, **lit** | - |
| `_OW_` | 0x3C | alpha, AC>1 | test+write | NONE | linear, fog | no-cull |
| `_P_` | 0x80C | alpha, AC>200 | test+write | BACK | linear, fog | cutout |
| `_AF_` `_A_` | 0x5C | **alpha** | test, **no write** | NONE | linear, fog | ALPHA |
| `_AL_` | 0x4D | **alpha** | test, no write | BACK | linear, fog, lit | ALPHA |
| `_K_` | 0x9C | **additive** | test, no write | NONE | linear, fog | ADD |
| `_M_` | 0x11C | **INVDESTCOLOR/ZERO** | unchanged | NONE | linear, fog | MULT-like |
| `_G_` | 0x98 | **additive** | test, no write | NONE | linear | ADD, no fog |
| `_GL_` | 0x99 | **additive** | test, no write | NONE | linear, lit | ADD |

Note the "opaque" layers still blend SRCALPHA/INVSRCALPHA with alpha test > 1.

## 2. Ramp-like classes (the dash ramp report)

| class | slot / id | factory struct | create -> ctor | library | key | layout entries |
|---|---|---|---|---|---|---|
| SPRING | 8 / 0x01 | 0x805289B8 | 0x80120554 -> 0x801210F4 | type 0 "SPRING.BIN" (0x80121270) | 0 | 120 (0100:5) |
| LONGSPRING | 12 / 0x02 | 0x8052798C | 0x8010F8F0 -> 0x801102F4 | type 0 id 0xD9 LONGSPRING.BIN (0x801103D0) | 0 | 64 (0100:1) |
| DASHPANEL | 9 / 0x03 | 0x80526B34 | 0x800FF3D8 -> 0x800FFC00 | type 0 id 0xD0 DASHPANEL.BIN (0x800FFC94) | 0 (0x800FFCBC) | 193 (0100:5) |
| JUMP_BOARD | 10 / 0x04 | 0x805278A4 | 0x8010E8FC -> 0x8010F0A4 | type 0 id 0xD8 JUMPBOARD.BIN (0x8010F140) | 0 (0x8010F168) | 161 (0100:18) |
| DASHRING | 17 / 0x06 | 0x80526C1C | 0x80100580 -> 0x80100D10 | type 0 id 0xD1 DASHRING.BIN (0x80100DB0) | 0 (0x80100DD8) | 80 (0100:0) |
| CATAPULT | 127 / 0x1138 | 0x8052A864 | 0x8014211C -> 0x80142BFC | type 1 "CATAPULT.BIN" (0x80142D88) | (not read) | 17 (0301 only) |
| ELEC_SPRING | 183 / 0x07DE | 0x8052BF08 | 0x801593A4 -> 0x80159FB4 | (not read) | | 36 (0200/0403/0411) |
| POP FOOTING | 219 / 0x13F0 | 0x80547CA8 | 0x8022BFAC -> 0x8022C5F4 | (not read) | | 54 (0501/0602) |
| GRINDGADGET / RAIL SWITCH / PLANE RAIL | 203 / 220 / 224 | 0x80547808 / 0x8054AF2C / 0x80555820 | 0x80228674 / 0x80257D34 / 0x802ABEA0 | (not read) | | 9 / 2 / 1 |

Type-0 id = member index in `stg_cmn_gdt.one` (VERIFIED for 0xC2..0xE6: 0xD0 DASHPANEL.BIN, 0xD1 DASHRING.BIN,
0xD8 JUMPBOARD.BIN, 0xD9 LONGSPRING.BIN, 0xE5 UPDOWNREEL.BIN, 0xCA CONTAINER.BIN ...).

Parts (VERIFIED from the BINs and DFFs; light group of every part = record +0x34 = 0 -> group 4):

| library | part | DFF | record +0x38 | WORLDPARM -> layer | flags | geometry | notes |
|---|---|---|---|---|---|---|---|
| JUMPBOARD | 0 model0 | JBOARD00 | 2 | 3 -> 6 `_OL_` | 0x2D lit | 0x37 (lit, not prelit) | |
| | 1 model1 | JBOARD01 (UVA JBOARD01.UVA) | 3 (attach to model0 node 0x7D0) | 1 -> 4 `_O_` | 0x2C | 0x3F, MatFX | arrows |
| DASHPANEL | 0 | DASHPANEL00 | **0** -> 0x804C39BC[0] = 6 `_OL_` | (WP 3 unused) | 0x2D lit | 0x37 | |
| | 1 | DASHPANEL01 (UVA) | 3 | 1 -> 4 `_O_` | 0x2C | 0x3F MatFX | |
| DASHRING | 0 | DASHRING00 | 0 -> 6 `_OL_` | | 0x2D lit | 0x3F, OBJPARM 0 | ring body |
| | 1 | DASHRING01 (UVA) | 3 | **9 -> 13 `_G_`** | **0x98 additive** | 0x3F MatFX `ob_hnlm0` | glow |
| | 2 | DASHRING02 (UVA) | 3 | 1 -> 4 `_O_` | 0x2C | 0x3F MatFX, unlit | arrows |
| SPRING | 0 | SPRING00 | 2 | 3 -> 6 `_OL_` | 0x2D | 0x3F | |
| | 1 | SPRING01 (UVA) | 3 | **8 -> 11 `_K_`** | **0x9C additive** | MatFX, unlit | glow |
| LONGSPRING | 0 / 1 | SPRINGB00 / SPRINGB01 (UVA) | 2 / 3 | 6 `_OL_` / **11 `_K_`** | 0x2D / **0x9C** | | glow |

Every one of these libraries also has one motion record (`mtn`/`motion0`/`motion` on model0, a `.BON` + `_ANM.MTN`).
DASHPANEL, JUMP_BOARD, DASHRING never attach it: 0x80160F84 (attach motion by name) has 21 call sites, none in their
code (VERIFIED `--xref`); so they stay in the clump's own frame pose. SPRING (0x80121354 "motion0") and LONGSPRING
(0x801104BC "motion") attach it and stop it (0x80160344 = 0x80417F78(m,0,0xB,0) on every motion), and start it on a bounce
(0x80160EFC(holder,0) at 0x80120714).

Per frame (JUMP_BOARD vtable 0x805278E8: [2] 0x8010ED38 dtor, [4] 0x8010ECE0 update, [6] 0x8010EBB8 replace;
DASHPANEL / DASHRING the same shape): update = 0x80169BD4 (two cooldown timers, 0x80169DF8) + holder update 0x80163394;
no hide/show, no colour, no motion, no layer move (VERIFIED: holder calls in 0x8010E8FC..0x8010F8F0 are only 0x801637C0,
0x801632D8, 0x80161470 (a `blr`), 0x80169BD4, 0x80163394, 0x80169910, 0x8015FDBC, 0x80162E4C, 0x801622EC, 0x8016999C,
0x80169BB0, 0x80169F94; DASHPANEL 0x800FF3D8..0x80100580 identical). The only per-frame change is the arrows' UV
animation, global and alive-dependent (uv_anim_start.md: registered at instancing). Misc data: 12 bytes copied to
+0x48 (0x8010F108 / 0x8010EB98, three floats, default r2-0x5BF8; INFERRED launch parameters) - no model choice.
Frame: 0x8010EAD4 = identity, translate +0 of the entry, RotY(+0x10), RotX(+0xC), RotZ(+0x14) (0x8040C5EC, 0x8040C898,
0x8040C8D4, 0x8040C85C), then 0x801637C0 (holder matrix).

So for the dash ramp the game draws, while alive and always: base in `_OL_` (lit, group 4, alpha AC>1, z on, cull back,
fog), arrows in `_O_` (unlit pipeline-prelit, MatFX UV transform, z on, cull back, fog). Nothing in the class hides
either part. Westopolis entries (VERIFIED from `stg0100_cmn.dat` / `_nrm.dat`, id at +0x20, range +0x23):
JUMP_BOARD cmn #1 (range 5), #2/#21/#22/#23 at (-55..65, -4479, -1560) range 10, #144/#145 range 4, #269/#270/#272,
#273/#274 range 8, #277..#280 range 10; nrm #1/#2 range 6 (entry word +0x1C = 0x1201 instead of 0x1101). DASHPANEL #47,
#191, #324, #325, #350. Static cause of "missing at higher draw distance": none found in the class or its data; the
draw path is the same at every distance. What a stand-in must reproduce is listed above; open runtime checks in 8.

## 3. Catalogue: parts in alpha / additive / multiply / no-cull layers, or own render states

| class | part (DFF) | layer | effect | stand-in today |
|---|---|---|---|---|
| HINTRING | model0 HINTRING00 (the only part of model "info0"; WP 8) | **11 `_K_` 0x9C** (not `_AL_`) | additive, zwrite off, cull none, fog | drawn (verified class) - wrong blend |
| SPRING / LONGSPRING | model1 SPRING01 / SPRINGB01 | 11 `_K_` | additive | drawn - wrong blend |
| DASHRING | model1 DASHRING01 | 13 `_G_` | additive, no fog | **no stand-in** |
| CAGE | bk1, bk3, bk4 (CASEBK01/03/04, UVA) | 13 `_G_` | additive | drawn |
| CAGE | gun1 CASEGUN01 | 11 `_K_` | additive | drawn |
| FAN | fan_glare1..3 A20000ARKFANGLARE (attached to fan nodes 5/8/0xB) | 13 `_G_` | additive | none |
| BRK_OBJ | bk02 BRKOBJBKSH (model info1) | 13 `_G_` | additive | none |
| POWER_DEVICE_CAGE | CoreGlare CN0000POWERDEVGLARE | 13 `_G_` | additive + colour record alpha 0x32 | validate-only |
| POWER_DEVICE_CAGE | Core CN0000POWERDEVICECORE atomic 4 | 4 `_O_` | colour record mode 1 alpha 0x68 (translucent) | validate-only |
| TORNADO | Tornado CN0201TORNADO (record layer 5, WP) | 8 `_AF_` 0x5C | alpha, zwrite off, cull none; animated | validate-only |
| CityLaser | LaserPredict a1/a2 | 11 `_K_` | additive | not a prop |
| CityLaser | Laser a1 | 13 `_G_` | additive | not a prop |
| ART GADGET (all stages) | 498 atomic-uses `_G_`, 95 `_A_`, 28 `_AF_`, 3 `_K_` (vs 1699 `_O_`, 509 hidden PTCL) | | glare/glass/water | drawn - wrong blend |
| ART WORK | 36 `_G_`, 9 `_A_`, 7 `_AF_`, 7 `_K_`, 1 WP out of range (BS0614LAVA) | | | drawn |
| ELEC_FAN | own code 0x802662A4: CULLMODE 1 at 0x802663E0, restored 2 at 0x80266400 | | no cull | n/a |
| ELEC_BAR / ELEC_LIGHT | own draw 0x8015390C: CULL 1, ZWRITE 0, FOG 0, SRC 5 / DST 2, VERTEXALPHA 1, then restores | | additive | n/a |
| ELEC_FOOTINGROTATE | own 0x802DE4BC, 0x802DF140: SRC 5 / DST 2, CULL 1, VERTEXALPHA 1, FOG 0 | | additive | n/a |

Lists of the ART GADGET / WORK DFFs per layer: `agcensus.py` output (e.g. `_G_`: *GLARE*, BS0000GLASSPLATEGUN*,
CT0202ROUTESIGNGLASS ...; `_A_`: *GLASS*, CT0202ARROW*; `_K_` ART WORK: JG0404 river/water). Render-state scan
method: every `lwz rX,0x615C(r13)` followed within 5 instructions by `lwz r12,0x20(rX)` and `bctrl` (241 sites in 51
functions), and every `bl 0x80498F74 / 0x804987C8`; attributed to classes by call-graph closure (depth 4) from each
factory's create, ctor and vtables. For the 26 listed classes no such site is in their own code (VERIFIED for the
closure; RW library pipelines reached from all classes - MatFX 0x804A3F74..0x804A7AAC, which set blend for env/dual
passes and ZCompLoc - are shared; the UV-transform-only materials here take no blend from them, INFERRED).

## 4. Common draw path (all model-holder classes)

- Library: 0x8015FBE4(out, type, name) / 0x8015FDBC(out, type, id); also wrappers 0x801641F4(_, type, name) (BRK_OBJ
  0x800F2324) and 0x80164360(_, type, id) (UD_REEL 0x80128EC8).
- Instance 0x80162E4C(holder, key, colourFlag) / 0x80162DBC(holder, name, colourFlag) -> 0x80162EB4 per part: clone, UVA
  register, light group + render callback 0x801645D0, then 0x80163C14 (layer world) and 0x80163A44 (attach).
- Holder per frame 0x80163394 (blink, fade, motions +0x58, attach frames). Render callback 0x801645D0: light group,
  colour (bit 2): material RGBA, MODULATEMATERIALCOLOR + alpha (bit 1) - never skips the draw.
- Motion helpers (VERIFIED bodies): 0x80160F84 attach by name; 0x8016100C attach by id (record +0x18); 0x80160344 all
  motions -> 0x80417F78(m,0,0xB,0) (stopped); 0x80160B8C(name) -> mode 1 (playing); 0x80160EFC(id) /
  0x80160DDC(name) -> 0x804185B8(m, record +0x1C) (start key); 0x80160E68(id, n) / 0x80160D44(name, n) -> key
  record+0x1C-1+n. A stopped motion still poses the HAnim hierarchy at its set key: a stand-in drawing the clump's frame
  pose matches only if that key equals the bind pose.
- Visibility: 0x80162B74 hide-all / 0x80162B00 show-all (holder bit 2, tested 0x80162AD8); PTCL locators hidden by
  0x8016A0E4 through 0x8015FE5C after 0x80166ED8 (PTCLFLAG bit 1 clear -> rpATOMICRENDER cleared, flag 8 sticky);
  blink 0x80162A70; fade 0x80160004 / 0x80160078 (moves every part to layer 10 `_AL_` while fading).
- Colour records (library +0x20) applied by 0x80162808 (by id) / 0x80162780 (by name): mode 1 = alpha.

## 5. Per class

Format: factory struct / create -> ctor (VERIFIED from 0x8052C1A0 and the create body); library; key; parts; per-frame;
hazards for a far stand-in. Light group of every part below = 4 (record +0x34 = 0) unless noted. Layers as section 1.

**DASHPANEL, JUMP_BOARD, DASHRING, SPRING, LONGSPRING** - section 2. Hazards: DASHRING glow additive and no stand-in;
SPRING/LONGSPRING glow additive; arrows' UV static until first spawn (uv_anim_start.md).

**ART GADGET** - 0x80525460 / 0x800E4360 -> 0x800E5934. type 1 "ARTISTGADGET.BIN" (per stage), model = gadget record by
id (0x80162DBC, colourFlag 1 at 0x800E5A94), motion attached by record (0x800E5AC4) and set to its start key (0x80160D44(name,1) at
0x800E5AE4). Fade in on spawn (0x800E5CD8 -> 0x80160004), fade out past range (0x800E4464 -> 0x80160078). Update
0x800E5280: switch-linked hide/show (0x80169430, 0x80162B74 / 0x80162B00, particles paused 0x80167054 / resumed
0x80166F54) when obj+0x80 bit 2; first update builds PTCL set (0x800E5394..0x800E53D4, hides locators). Blink at
0x800E4A90 (0x800E495C, INFERRED: damage). Hazards: ~600 atomic-uses in `_G_`/`_A_`/`_AF_`/`_K_` (section 3); 40 motion
records across stages (start-key pose); fade alpha layer `_AL_`.

**ART WORK** - 0x805255B0 / 0x800E5E70 -> 0x800E64FC. type 1 "ARTISTWORK.BIN", record by id (0x800E6664), motion
attached and set to its start key (0x800E6690, 0x800E66B0). No fade (base gate 0x8015AC7C). Matrix 0x800E5F00 with scale
(0x8040C770). PTCL set 0x800E6244. Hazards: layers (section 3), 13 motion records.

**STICK** - 0x80528B30 / 0x80121E94 -> 0x80123044. type 1 "STICK.BIN" (0x801230FC), record "stick" (0x801231A0), motion
by record (0x801231D0) at its start key (0x801231F0). Broken state: vt[6] 0x801224E4 re-instances (0x80122598) "stick_brk"; PTCL set
vt[8] 0x801229BC. Westopolis library: part 0 stick CT0100SIGNSPD `_O_` unlit; part 1 stick_brk (atomic 0 PTCL hidden).
Colour by thresholds 0x80124A08 (see object_distance.md 2.5).

**STONE_POLE** - 0x8052495C / 0x800D8208 -> 0x800D7A14 (dispatcher: 0x800D9C38 pole, 0x800D7EC8 / 0x800D7E38 /
0x800D7E78 parts). "STONEPOLE.BIN" type 1 (0x800D9D18), key object +0x38 (0x800D9D44), motion attached by id and
**started** (0x800D9D5C, 0x800D9D6C 0x80160EFC). Stones by name "StoneSoft" (0x800DA6A8) / "StoneHard" (0x800DB1C0)
(0x80162DBC). Three sub-object vtables 0x80524AE0 / 0x80524B18 / 0x80524B50; [10] = hide-all (0x800D8370, 0x800D83F0).
Library (stg0201): 5 models PoleFlat/PoleSlope/PoleAir (keys 0..2, 7 visible atomics + 1 hidden PTCL each, all
`_O_` unlit) and StoneHard/StoneSoft (3, 4); 3 motion records. Hazards: motion pose.

**STONE_STATUE** - 0x80524D00 / 0x800DD994 -> 0x800DD434. "STONESTATUE.BIN" (0x800DD518), key object +0x38 (0x800DD540;
keys 0 StoneStatue, 1 StoneStatueMirror - same part). PTCL set in the ctor (0x800DD724..0x800DD770): atomics 0..2
(PTCLFLAG 1) hidden at construction. Part `_O_` unlit. Update vt[4] 0x800DCF60, vt[6] 0x800DCED8 (0x801629BC).

**ROLL_CIRCLE** - 0x8052460C / 0x800D63E0 -> 0x800D6304; model in 0x800D6C80: "ROLLCIRCLE.BIN" (0x800D6D58), key
[+0x60] of a sub-object (0x800D6D84; INFERRED misc). Models Big/Sml/Head (keys 0..2), all `_O_` unlit; break parts
outside the model lists.

**MOVE_FOOTING** - 0x8052B1D0 / 0x80149C40 -> 0x8014BB94. "FOOTINGMOVABLE.BIN" (0x8014BD04), key +0x34, **colourFlag 1**
(0x8014BD34). Message 0x17 handler 0x80149DAC sets holder alpha from the message (0x801601BC at 0x80149DF0) and 0x80149E50
ends it (0x801600F4) - INFERRED camera/occlusion transparency, alive only. Switch link 0x8014B79C. One part `_O_` unlit.

**DOOR** - 0x8052AAF4 / 0x80144360 -> 0x80145248. "DOOR.BIN" type 1, key +0x68, colourFlag 1 (0x8014535C), motion by id
(0x80145368) stopped (0x80145370); state code sets the motion key (0x801444B0 -> 0x80160E68, 0x80144484 -> 0x80160424);
same alpha message pair as MOVE_FOOTING (0x801447DC / 0x80144884). Part `_O_` unlit, MatFX, geometry 0xBB.
Hazard: open/closed pose from the motion key.

**SECRETDOOR** - 0x80528278 / 0x8011B9B8 -> 0x8011BED8. type 0 "SECRETDOOR.BIN" ([r13-0x5900]), key 0, motion "mtn"
stopped (0x8011BFEC, 0x8011BFF4); update vt[4] 0x8011BB84 plays it (0x80160EFC at 0x8011BC98 / 0x8011BD74) and 0x80161E08.
6 parts: model0 (3 atomics) + lock0..4 attached to nodes 0x898..0x89C, all `_OL_` lit (record layer 0 / flags 0 or 3).

**FAN** - 0x80526F44 / 0x80102688 -> 0x80103E98. "FAN.BIN" type `r31` ([r13-0x5D78]), key 0, motion attached by id 0,
stopped, start key (0x80103FB4..0x80103FCC). **Hidden at construction when +0x80 == 0** (0x80104008..0x80104018), and
vt[6] 0x80103918 toggles show/hide on +0x80 changes (0x80103974 / 0x80103980); +0x80 from the switch table
(0x801029C0, INFERRED on/off). Parts: fan (5 atomics `_O_`) + 3 glares in `_G_` additive. Hazard: an off fan is invisible.

**CAGE** - 0x805260EC / 0x800F25B4 -> 0x800F3024. type 0 "CASE.BIN", key +0x54 (misc), motion by id, stopped, then
0x80161BB4(holder,key,-2) / 0x80161B24 (0x800F3204, 0x800F3228). Update vt[4] 0x800F2B40: switch link (0x80169430 at
0x800F2B88) starts the motion (0x80160EFC). Parts: object_distance.md 3.3; glow parts `_G_`/`_K_`.

**POWER_DEVICE_CAGE** - 0x805241B0 / 0x800D37EC -> 0x800D31AC. "POWERDEVICECAGE.BIN" type 1, key +0x58, then **colour
records 0 and 1 applied** (0x80162808 at 0x800D329C / 0x800D32A8): CoreOpaque alpha 0x68 on Core, CoreGlare alpha 0x32
on CoreGlare. Core atomics 0..3 PTCL (hidden), CoreGlare `_G_`, 3 motion records (not attached in the ctor).

**BRK_OBJ** - 0x80525FB0 / 0x800F1734 -> 0x800F2298. "BREAKOBJ.BIN" via 0x801641F4 (0x800F2324), key +0x30 (0
info0 gun box, 1 info1 BK box with bk02 `_G_`). Broken: 0x800F17B8 (0x8015F49C "BRKOBJGUNCRSH.DFF").

**BOMB** - 0x80525D98 / 0x800EE11C -> 0x800F10BC (alloc 0xFC; a weapon-style object, vtable 0x80525EFC). The holder is
built in vt[13] 0x800F02DC ("BOMB.BIN" type 0, key 0, 0x800F04DC), not in the ctor (INFERRED: on activation). Parts
model0 `_OL_` lit, model1 `_OL_` unlit.

**BlockChange** - 0x80525CF4 / 0x800EDD74 -> 0x800EDC48. No model: the ctor builds a collision (0x800C8290) from misc
0x1C bytes; vtable 0x80525D24 has 2 entries. Nothing to draw (VERIFIED for ctor and vtable).

**TORNADO** - 0x80524F28 / 0x800DE7C4 -> 0x800DE3B4. "TORNADO.BIN" type 1, key 0, motion "TornadoMtn" attached
(0x800DE4EC), **played** (0x80160B8C), stopped flag pass (0x80160344), start key (0x80160DDC). Part in `_AF_` (alpha,
zwrite off, no cull), UVA. Hazard: animated + alpha.

**UD_REEL** - 0x805290E0 / 0x80127784 -> 0x80128D30. type 0 id 0xE5 UPDOWNREEL.BIN via 0x80164360, three holders:
+0x38 key 0 udreel, +0x40 key 2 stick, +0x3C key 1 grip (0x80128EE0 / 0x80128F14 / 0x80128F48). grip part udreel2 in
`_O_` unlit, the rest `_OL_` lit.

**FallBuilding** - 0x805252F8 / 0x800E2CAC -> 0x800E2978 -> 0x800E39F0. "FALLBUILDING.BIN" type 1, key misc (+0 of a
record, 0x800E3B74); a fall state path re-instances (0x800E3EE4 / 0x800E3EF8), starts the motion (0x800E3F08,
0x800E3F18) and hides all (0x800E3F20). Part 2 atomic 0 PTCL hidden. Models: Ant15/Ant30/Bldg Normal + Fell.

**CityLaser** - 0x805251C0 / 0x800E0D04 -> 0x800E09C0 (-> 0x800E1BE4 loads "CITYLASER.BIN"). Nothing instanced until
state 2 (0x800E1110 key 0 LaserPredict + motion 0x8016100C / 0x80160EFC) or state 3 (0x800E11D4 key 1 Laser); leaving a
state destroys the instances (0x80162BE8 at 0x800E1088 / 0x800E10B8). Not a static prop: draws nothing while idle.

**LIGHT_COLI** - 0x8053F168 / 0x801EF2B0 -> 0x801EFF34. No model holder (vtable 0x8053F270; no library call in ctor or
vtable). INFERRED a light-group trigger volume. Nothing to draw.

**PARTICLE** - 0x805286D8 / 0x8011E024 -> 0x8011E350. No holder: 0x8016902C (ctor 0x8011E3F8, vt[6] 0x8011E130) creates
an emitter. Effects path (effects_distance.md), not a model.

**HINTRING** (asked in the addition) - 0x805275B0 / 0x80109D58 -> 0x8010AE60. "HINTRING.BIN" type 0 by name
(0x8010AF30, name at r30+0x2D0), key 0. Model "info0" = model0 only: HINTRING00, WORLDPARM 8 -> **11 `_K_`**
(0x9C additive, zwrite off, cull none, fog), UVA, MatFX. model1 (HINTRING01, `_O_`, unlit) is outside every model list.

## 6. Hazards for a far stand-in, by kind

| kind | classes |
|---|---|
| layer blend not reproduced (section 3) | HINTRING, SPRING, LONGSPRING, CAGE, ART GADGET, ART WORK; DASHRING, FAN, BRK_OBJ, TORNADO, POWER_DEVICE_CAGE when added |
| colour records / alpha | POWER_DEVICE_CAGE (ctor), MOVE_FOOTING and DOOR (message 0x17, alive only), ART GADGET fade (`_AL_`) |
| motion pose != frame pose | STICK, ART GADGET, ART WORK (start key), STONE_POLE (playing), TORNADO (playing), FAN (start key), DOOR (state key), CAGE, SECRETDOOR, SPRING, LONGSPRING (stopped motion attached) |
| hidden while alive | FAN (+0x80 == 0), ART GADGET (switch link), STONE_POLE sub-objects, FallBuilding fall state, PTCL locators |
| no model while idle / no model | CityLaser, BOMB (holder built later), BlockChange, LIGHT_COLI, PARTICLE |
| UV animation static until first spawn | every part with a `.UVA` (dash ramp arrows, DASHPANEL, DASHRING, SPRING glow, HINTRING, CAGE, TORNADO) |
| no stand-in rule | DASHRING, FAN, BRK_OBJ, BOMB, UD_REEL, SECRETDOOR, CATAPULT, ... (everything not in `kRules`) |

## 7. Westopolis classes (stg0100, all 4 layout files)

From the layout census: ART GADGET 475, PARTICLE 52, LIGHT_COLI 50, RING 38, STICK 40, BRK_OBJ 34, SETCOLLISION 30,
HINTRING 23, JUMP_BOARD 18, CONTAINER 17, HINT COLLISION 17, HRD_CONTAINER 11, CityLaser 109, Fruit 39, WEAPON_CONTAINER 9,
FIRE 9, SAVEPOINT 6, DASHPANEL 5, SPRING 5, SECRETKEY 5, CAGE 4, BALLOON 3, MOVE_FOOTING 3, TURNOVER 3, GOALRING 2,
UNBRK_CONTAINER 2, LONGSPRING 1, UD_REEL 1, SWITCH 1, SECRETDOOR 1, plus enemies.

## 8. Open / runtime checks

- Dash ramps missing at high draw distance: the class draws both parts unconditionally (section 2), so check the
  proxy side for JUMP_BOARD entries #2/#21-23 (range 10) and #277-280: candidate selection, the 2..7 range cap, the
  pixel-size gate, and whether `_OL_`'s lights are present in the pass that draws far proxies.
- CATAPULT, ELEC_SPRING, POP FOOTING, GRINDGADGET, RAIL SWITCH, PLANE RAIL: libraries and parts not read.
- 0x80160344 mode 0xB vs the bind pose: compare the HAnim pose of a stopped motion at its start key with the frame pose
  for SPRING/CAGE (verified exact by handover) and STICK/ART GADGET.
- MOVE_FOOTING / DOOR message 0x17: who sends it.

## 9. Full factory table 0x8052C1A0

Struct = {+0 name, +4 f1, +8 f2, +0xC create, ..., +0x18 u16 layout id, +0x1A kind (1 gadget, 2 enemy, 4 unnamed),
+0x20, +0x24 data pointers}. ctor = the call that takes the allocated object in create (`ctors.py`); "-" = no create.
Layout entries = count over every `stg*_{cmn,nrm,hrd,ds1}.dat` (kind-1 names only).

| slot | struct | name | layout id | kind (+0x1A) | f1 (+4) | f2 (+8) | create (+0xC) | ctor | layout entries (all stages) |
|---|---|---|---|---|---|---|---|---|---|
| 0 | 8052C0E8 | GadgetCommonDataStart | 0x2580 | 01 | 8015DC98 | 8015DC94 | - | - |  |
| 1 | 8055A2D8 | Sample | 0x2586 | 01 | 802DFC04 | 802DFC00 | 802DFBAC | 802DFB4C |  |
| 2 | 80526590 | CONTAINER | 0x0009 | 01 | 800F6C74 | 800F6C70 | 800F6A38 | 800EDF64 | 191 |
| 3 | 80526778 | HRD_CONTAINER | 0x000A | 01 | 800F9D64 | 800F9D60 | 800F9B28 | 800EDF64 | 110 |
| 4 | 805268C4 | UNBRK_CONTAINER | 0x000B | 01 | 800FC1D4 | 800FC1D0 | 800FC17C | 800FC98C | 92 |
| 5 | 805269C4 | WEAPON_CONTAINER | 0x000C | 01 | 800FD0AC | 800FD0A8 | 800FCE70 | 800EDF64 | 433 |
| 6 | 8054E84C | SECRET CONTAINER | 0x003A | 01 | 80278748 | 80278744 | 8027850C | 800EDF64 | 72 |
| 7 | 80525828 | BALLOON | 0x0013 | 01 | 800E6EB0 | 800E6EAC | 800E6E54 | 800E8044 | 79 |
| 8 | 805289B8 | SPRING | 0x0001 | 01 | 801205AC | 801205A8 | 80120554 | 801210F4 | 120 |
| 9 | 80526B34 | DASHPANEL | 0x0003 | 01 | 800FF434 | 800FF430 | 800FF3D8 | 800FFC00 | 193 |
| 10 | 805278A4 | JUMP_BOARD | 0x0004 | 01 | 8010E958 | 8010E954 | 8010E8FC | 8010F0A4 | 161 |
| 11 | 80527D04 | RING | 0x0010 | 01 | 80112790 | 8011278C | 80112738 | 80114564 | 990 |
| 12 | 8052798C | LONGSPRING | 0x0002 | 01 | 8010F948 | 8010F944 | 8010F8F0 | 801102F4 | 64 |
| 13 | 80527FC0 | SAVEPOINT | 0x0005 | 01 | 80117850 | 80117830 | 801177D8 | 80118E08 | 166 |
| 14 | 80529464 | WEIGHT | 0x0019 | 01 | 8012B00C | 8012B008 | 8012AFB0 | 8012C0C8 | 18 |
| 15 | 805272CC | GOALRING | 0x0014 | 01 | 8010670C | 80106708 | 801066B4 | 80106F14 | 37 |
| 16 | 80528C60 | SWITCH | 0x0015 | 01 | 80123544 | 80123540 | 801234E8 | 801240E8 | 31 |
| 17 | 80526C1C | DASHRING | 0x0006 | 01 | 801005DC | 801005D8 | 80100580 | 80100D10 | 80 |
| 18 | 80528E2C | TARGETSWITCH | 0x0016 | 01 | 801249DC | 801249D8 | 80124984 | 80125400 | 1 |
| 19 | 80525D98 | BOMB | 0x000D | 01 | 800EE1F0 | 800EE1EC | 800EE11C | 800F10BC | 28 |
| 20 | 80526F44 | FAN | 0x001A | 01 | 801026E8 | 801026E4 | 80102688 | 80103E98 | 22 |
| 21 | 805282EC | SECRETKEY | 0x001D | 01 | 8011C374 | 8011C370 | 8011C318 | 8011CA94 | 115 |
| 22 | 80528278 | SECRETDOOR | 0x001E | 01 | 8011BA14 | 8011BA10 | 8011B9B8 | 8011BED8 | 26 |
| 23 | 80525B14 | BREAK_BLAST | 0x001C | 01 | 800ECD84 | 800ECD80 | 800ECD2C | 800ED378 | 180 |
| 24 | 80525FB0 | BRK_OBJ | 0x001B | 01 | 800F178C | 800F1788 | 800F1734 | 800F2298 | 367 |
| 25 | 805260EC | CAGE | 0x0007 | 01 | 800F2610 | 800F260C | 800F25B4 | 800F3024 | 63 |
| 26 | 80526238 | CAPTURE_CAGE | 0x0037 | 01 | 800F38C8 | 800F38C4 | 800F386C | 800F46DC | 8 |
| 27 | 805275B0 | HINTRING | 0x0011 | 01 | 80109DB0 | 80109DAC | 80109D58 | 8010AE60 | 305 |
| 28 | 805290E0 | UD_REEL | 0x0008 | 01 | 801277E0 | 801277DC | 80127784 | 80128D30 | 48 |
| 29 | 805276D0 | ITEM_BOX | 0x0012 | 01 | 8010BFA8 | 8010BFA4 | 8010BF50 | 8010C2B4 | 152 |
| 30 | 80525960 | (unnamed) | 0x0018 | 04 | 800E8CF8 | 800E8CC8 | - | - |  |
| 31 | 80525A40 | (unnamed) | 0x0039 | 04 | 800EADB4 | 800EAD84 | - | - |  |
| 32 | 805270C4 | FIRE | 0x0034 | 01 | 80104424 | 80104420 | 801043C8 | 80104DF8 | 98 |
| 33 | 80526CDC | ENERGY_CORE | 0x0033 | 01 | 80101594 | 80101590 | 80101538 | 80101A24 | 8 |
| 34 | 80527AE4 | POISON_GAS | 0x0035 | 01 | 80110C00 | 80110BFC | 80110BA4 | 80111970 | 13 |
| 35 | 805291FC | WARP_HOLE | 0x001F | 01 | 8012997C | 80129894 | 80129840 | 8012A9F0 | 24 |
| 36 | 80527E6C | ROCKET | 0x000E | 01 | 80114D20 | 80114D1C | 80114CC4 | 801158E8 | 23 |
| 37 | 80528790 | SET_VEHICLE | 0x004F | 01 | 8011E560 | 8011E55C | 8011E508 | 8011E934 | 132 |
| 38 | 80528854 | SET_WEAPON | 0x0020 | 01 | 8011F068 | 8011F064 | 8011F010 | 8011F3E0 | 37 |
| 39 | 80528138 | SET_CHARACTOR | 0x0190 | 01 | 80119E70 | 80119E6C | 80119E0C | 8015AE00 | 121 |
| 40 | 80525CF4 | BlockChange | 0x0059 | 01 | 800EDDCC | 800EDDC8 | 800EDD74 | 800EDC48 | 13 |
| 41 | 80526398 | CATCHSTICK | 0x005A | 01 | 800F4F44 | 800F4F40 | 800F4EEC | 800F58F8 | 104 |
| 42 | 805286D8 | PARTICLE | 0x258A | 01 | 8011E07C | 8011E078 | 8011E024 | 8011E350 | 1970 |
| 43 | 80527354 | HEALING_UNIT | 0x0038 | 01 | 801078C8 | 801078C4 | 801077F4 | 800EDF64 | 5 |
| 44 | 80528560 | SETCOLLISION | 0x0050 | 01 | 00000000 | 00000000 | 8011D038 | 8011DE34 | 1020 |
| 45 | 805271C4 | Fruit | 0x0022 | 01 | 80105750 | 8010574C | 801056F4 | 8010630C | 221 |
| 46 | 8053F168 | LIGHT_COLI | 0x0062 | 01 | 801EF344 | 801EF304 | 801EF2B0 | 801EFF34 | 598 |
| 47 | 805451B4 | HINT COLLISION | 0x0051 | 01 | 00000000 | 00000000 | 802079A4 | 80208740 | 796 |
| 48 | 80558D5C | DebugDebugMissionClearCollision | 0x2592 | 01 | 802C8D78 | 802C8D74 | 802C8D20 | 802C8DE4 |  |
| 49 | 8054AFE0 | DARKSPINENTRANCE | 0x0061 | 01 | 80258C38 | 80258C34 | 80258BE0 | 80258EAC | 28 |
| 50 | 8054EEC0 | SET GENERATOR | 0x2593 | 01 | 8027C364 | 8027C360 | 8027C308 | 8027C700 | 74 |
| 51 | 805481F4 | MissionClearCollision | 0x2595 | 01 | 80231CB0 | 80231CAC | 80231C58 | 80231D28 | 1 |
| 52 | 8055B0C8 | HINT CONTROLLER | 0x2596 | 01 | 802F33BC | 802F3364 | 802F330C | 802F36CC | 17 |
| 53 | 80528F50 | TANK | 0x0021 | 01 | 80125974 | 80125970 | 8012590C | 80127138 |  |
| 54 | 8055E960 | SET_SE_LOOP | 0x2597 | 01 | 00000000 | 00000000 | 8033DC70 | 8033DD38 | 116 |
| 55 | 8055EB90 | SET_SE_ONESHOT | 0x2598 | 01 | 00000000 | 00000000 | 8033E41C | 8033E4E4 | 10 |
| 56 | 8052C10C | GadgetCommonDataEnd | 0x2581 | 01 | 8015DC38 | 8015DC04 | - | - |  |
| 57 | 8051FA88 | WeaponDataStart | 0x2584 | 01 | 8005E428 | 8005E424 | - | - |  |
| 58 | 8051FC88 | (unnamed) | 0x00C8 | 04 | 8005F138 | 8005F100 | - | - |  |
| 59 | 80520930 | (unnamed) | 0x00C9 | 04 | 80068F24 | 80068EEC | - | - |  |
| 60 | 8051F698 | (unnamed) | 0x00CA | 04 | 8005CCA8 | 8005CC70 | - | - |  |
| 61 | 8051F530 | (unnamed) | 0x00CB | 04 | 8005C394 | 8005C35C | - | - |  |
| 62 | 80520A98 | (unnamed) | 0x00CC | 04 | 80069878 | 80069840 | - | - |  |
| 63 | 80542F60 | (unnamed) | 0x00CE | 04 | 801F41D0 | 801F4198 | - | - |  |
| 64 | 8051FD58 | (unnamed) | 0x00CF | 04 | 8005F634 | 8005F5FC | - | - |  |
| 65 | 8051FAD0 | (unnamed) | 0x00D0 | 04 | 8005E750 | 8005E718 | - | - |  |
| 66 | 8051FEC8 | (unnamed) | 0x00D1 | 04 | 80064934 | 800648FC | - | - |  |
| 67 | 8055AF68 | (unnamed) | 0x00D2 | 04 | 802F1E04 | 802F1DCC | - | - |  |
| 68 | 8051FB88 | (unnamed) | 0x00DC | 04 | 8005EBAC | 8005EB74 | - | - |  |
| 69 | 8051F780 | (unnamed) | 0x00DD | 04 | 8005D3D8 | 8005D3A0 | - | - |  |
| 70 | 805209E8 | (unnamed) | 0x00DE | 04 | 800690CC | 80069094 | - | - |  |
| 71 | 8051F8E0 | (unnamed) | 0x00DF | 04 | 8005D8D8 | 8005D8A0 | - | - |  |
| 72 | 8051F830 | (unnamed) | 0x00E0 | 04 | 8005D660 | 8005D628 | - | - |  |
| 73 | 80542EB0 | (unnamed) | 0x00E1 | 04 | 801F4044 | 801F400C | - | - |  |
| 74 | 8051F5E8 | (unnamed) | 0x00E6 | 04 | 8005CB2C | 8005CAF4 | - | - |  |
| 75 | 8051FFA4 | (unnamed) | 0x00E7 | 04 | 80064F54 | 80064F1C | - | - |  |
| 76 | 8051FFF8 | (unnamed) | 0x00E8 | 04 | 80064D6C | 80064D34 | - | - |  |
| 77 | 8054E19C | (unnamed) | 0x00E9 | 04 | 80275690 | 80275658 | - | - |  |
| 78 | 8054E1F0 | (unnamed) | 0x00EA | 04 | 802754F8 | 802754C0 | - | - |  |
| 79 | 8054E244 | (unnamed) | 0x00EB | 04 | 80275360 | 80275328 | - | - |  |
| 80 | 80547660 | (unnamed) | 0x00F0 | 04 | 802260E8 | 802260B8 | - | - |  |
| 81 | 80547248 | (unnamed) | 0x00F1 | 04 | 8022192C | 802218F4 | - | - |  |
| 82 | 80547588 | (unnamed) | 0x00F2 | 04 | 80225C00 | 80225BC8 | - | - |  |
| 83 | 805474B0 | (unnamed) | 0x00F3 | 04 | 80225714 | 802256DC | - | - |  |
| 84 | 805200E4 | (unnamed) | 0x00FA | 04 | 800673C0 | 80067390 | - | - |  |
| 85 | 8051F990 | (unnamed) | 0x00FB | 04 | 8005E0B8 | 8005E088 | - | - |  |
| 86 | 8055AEB8 | (unnamed) | 0x00FC | 04 | 802F1860 | 802F1830 | - | - |  |
| 87 | 80520138 | (unnamed) | 0x00FD | 04 | 8006725C | 8006722C | - | - |  |
| 88 | 8051FAAC | WeaponDataEnd | 0x2585 | 01 | 8005E3C8 | 8005E394 | - | - |  |
| 89 | 8052C798 | GadgetStageDataStart | 0x2582 | 01 | 801693D8 | 801693D4 | - | - |  |
| 90 | 80528B30 | STICK | 0x012C | 01 | 80121EEC | 80121EE8 | 80121E94 | 80123044 | 841 |
| 91 | 80528F2C | TURNOVER | 0x0023 | 01 | 80125A08 | 80125A04 | 801259A0 | 80127138 | 32 |
| 92 | 805255B0 | ART WORK | 0x2588 | 01 | 800E5ED4 | 800E5ED0 | 800E5E70 | 800E64FC | 1202 |
| 93 | 805255D4 | ART WORK ZONE | 0x2590 | 01 | 800E5E44 | 800E5E40 | 800E5DE0 | 800E64FC | 159 |
| 94 | 80525460 | ART GADGET | 0x2589 | 01 | 800E43C4 | 800E43C0 | 800E4360 | 800E5934 | 6769 |
| 95 | 80525484 | ART GADGET ZONE | 0x2591 | 01 | 800E4334 | 800E4330 | 800E42D0 | 800E5934 | 1615 |
| 96 | 8052AE88 | FOOTING | 0x1130 | 01 | 80149744 | 80149740 | 801496EC | 80149A24 | 143 |
| 97 | 8052B1D0 | MOVE_FOOTING | 0x000F | 01 | 80149C98 | 80149C94 | 80149C40 | 8014BB94 | 403 |
| 98 | 8052ADD0 | ELEVATOR | 0x1132 | 01 | 80147F18 | 80147F14 | 80147EBC | 801490D4 | 15 |
| 99 | 8052AAF4 | DOOR | 0x1133 | 01 | 801443C4 | 801443C0 | 80144360 | 80145248 | 264 |
| 100 | 8052A918 | DAMAGE BLOCK | 0x1134 | 01 | 80143488 | 80143484 | 80143430 | 80143768 | 28 |
| 101 | 80526F68 | FAN STAGE | 0x2594 | 01 | 80102660 | 8010265C | 80102600 | 80103E98 | 29 |
| 102 | 8052018C | (unnamed) | 0x00FE | 04 | 800670F8 | 800670C8 | - | - |  |
| 103 | 805201E0 | (unnamed) | 0x00FF | 04 | 80066F80 | 80066F50 | - | - |  |
| 104 | 80520234 | (unnamed) | 0x0100 | 04 | 80066E14 | 80066DE4 | - | - |  |
| 105 | 80520288 | (unnamed) | 0x0101 | 04 | 80066CA8 | 80066C78 | - | - |  |
| 106 | 80548E20 | (unnamed) | 0x0102 | 04 | 8023E138 | 8023E108 | - | - |  |
| 107 | 805202DC | (unnamed) | 0x0103 | 04 | 80066B30 | 80066B00 | - | - |  |
| 108 | 80520330 | (unnamed) | 0x0104 | 04 | 800669C4 | 80066994 | - | - |  |
| 109 | 80520384 | (unnamed) | 0x0105 | 04 | 80066858 | 80066828 | - | - |  |
| 110 | 805203D8 | (unnamed) | 0x0106 | 04 | 800666E0 | 800666B0 | - | - |  |
| 111 | 8052042C | (unnamed) | 0x0107 | 04 | 80066574 | 80066544 | - | - |  |
| 112 | 80520480 | (unnamed) | 0x0108 | 04 | 800663FC | 800663CC | - | - |  |
| 113 | 805204D4 | (unnamed) | 0x0109 | 04 | 80066284 | 80066254 | - | - |  |
| 114 | 80520528 | (unnamed) | 0x010B | 04 | 80066118 | 800660E8 | - | - |  |
| 115 | 8052057C | (unnamed) | 0x010C | 04 | 80065FAC | 80065F7C | - | - |  |
| 116 | 805205D0 | (unnamed) | 0x010D | 04 | 80065E34 | 80065E04 | - | - |  |
| 117 | 80520624 | (unnamed) | 0x010E | 04 | 80065CBC | 80065C8C | - | - |  |
| 118 | 80520678 | (unnamed) | 0x010F | 04 | 80065B50 | 80065B20 | - | - |  |
| 119 | 805206CC | (unnamed) | 0x0110 | 04 | 800659E4 | 800659B4 | - | - |  |
| 120 | 80520720 | (unnamed) | 0x0111 | 04 | 80065878 | 80065848 | - | - |  |
| 121 | 80520774 | (unnamed) | 0x0112 | 04 | 80065700 | 800656D0 | - | - |  |
| 122 | 805207C8 | (unnamed) | 0x0113 | 04 | 80065594 | 80065564 | - | - |  |
| 123 | 8052081C | (unnamed) | 0x0114 | 04 | 80065428 | 800653F8 | - | - |  |
| 124 | 8052B5B4 | FIRE_VINE | 0x1131 | 01 | 8014F204 | 8014F200 | 8014F1AC | 801506C0 | 6 |
| 125 | 8052A654 | BREAKWALL | 0x1135 | 01 | 801406B8 | 801406B4 | 8014065C | 801418C8 | 82 |
| 126 | 8052B410 | NOBI_KUSA | 0x1137 | 01 | 8014C150 | 8014C14C | 8014C0F8 | 8014CF7C | 20 |
| 127 | 8052A864 | CATAPULT | 0x1138 | 01 | 80142178 | 80142174 | 8014211C | 80142BFC | 17 |
| 128 | 8052AC14 | EGG_BALLOON | 0x1450 | 01 | 80145954 | 80145950 | - | - |  |
| 129 | 8054DF5C | E_B_COLI | 0x1451 | 01 | 80273DFC | 80273DF8 | 80273848 | 8025E738 | 35 |
| 130 | 8055B6C4 | SECRET DISK | 0x1453 | 01 | 802FA3BC | 802FA3B8 | 802FA360 | 802FAABC | 5 |
| 131 | 80524F28 | TORNADO | 0x0834 | 01 | 800DE81C | 800DE818 | 800DE7C4 | 800DE3B4 | 9 |
| 132 | 80525070 | TORNADO_COLLI | 0x0835 | 01 | 800DFD04 | 800DFD00 | 800DFCAC | 800DFD88 | 5 |
| 133 | 8052460C | ROLL_CIRCLE | 0x0836 | 01 | 800D6438 | 800D6434 | 800D63E0 | 800D6304 | 27 |
| 134 | 8052495C | STONE_POLE | 0x0837 | 01 | 800D8270 | 800D826C | 800D8208 | 800D7A14 | 53 |
| 135 | 80524D00 | STONE_STATUE | 0x0838 | 01 | 800DD9EC | 800DD9E8 | 800DD994 | 800DD434 | 18 |
| 136 | 805241B0 | POWER_DEVICE_CAGE | 0x0839 | 01 | 800D3844 | 800D3840 | 800D37EC | 800D31AC | 5 |
| 137 | 80523FB0 | EggFleet | 0x1069 | 01 | 800CC418 | 800CC414 | 800CC3B0 | 800CC140 | 5 |
| 138 | 805242E8 | POWER_DEVICE_NAKED | 0x106C | 01 | 800D4DA0 | 800D4D9C | 800D4D48 | 800D4B1C | 5 |
| 139 | 805243F8 | RainEffect | 0x106D | 01 | 800D5644 | 800D5640 | 800D5584 | 800D56EC | 1 |
| 140 | 8055A318 | TESTNAKAZAWA | 0x2587 | 01 | 802DFC60 | 802DFC5C | 802DFC08 | 802DFED4 |  |
| 141 | 80529864 | CIRCUS_BALL | 0x0C80 | 01 | 8012FADC | 8012FAD8 | 8012FA80 | 801305F8 | 33 |
| 142 | 80529C94 | FIRE_RING | 0x0C85 | 01 | 801356E0 | 801356DC | 80135688 | 8013645C | 19 |
| 143 | 80529BD0 | Cream | 0x0BBD | 01 | 80134C24 | 80134C20 | 80134BCC | 80134FCC | 2 |
| 144 | 805296F8 | Chao | 0x0BBE | 01 | 8012E278 | 8012E274 | 8012E210 | 8012DEB0 | 27 |
| 145 | 80529F0C | Monster | 0x0BC7 | 01 | 80139684 | 80139680 | 8013962C | 80139AE0 | 1 |
| 146 | 8052A0BC | MonsterControl | 0x0BC8 | 01 | 8013A978 | 8013A974 | 8013A920 | 8013ACC8 | 7 |
| 147 | 80548A50 | GiantLantern | 0x0BC9 | 01 | 80237094 | 80237090 | 8023703C | 80237478 | 5 |
| 148 | 80529ACC | Coaster | 0x0C88 | 01 | 80132FAC | 80132FA0 | 80132F4C | 80133978 | 20 |
| 149 | 8052A524 | ThreatObj | 0x0BBC | 01 | 8013F658 | 8013F64C | 8013F5E8 | 8013F1B0 | 98 |
| 150 | 8052A2D8 | PARK_BALLOON | 0x0C82 | 01 | 8013B2C8 | 8013B2A8 | 8013B250 | 8013C084 | 29 |
| 151 | 8052A41C | SHOOTING_PANEL | 0x0C83 | 01 | 8013DD00 | 8013DCFC | 8013DCA4 | 8013EE04 | 13 |
| 152 | 80529DB4 | GONG | 0x0C81 | 01 | 80136EF8 | 80136EF4 | 80136EA0 | 80137FC4 | 4 |
| 153 | 805295D0 | CandleStick | 0x0BBB | 01 | 8012DA5C | 8012DA38 | 8012D9E4 | 8012DB80 | 13 |
| 154 | 80529924 | CIRCUS_POLE | 0x0C87 | 01 | 80130BBC | 80130BB8 | 80130B64 | 80131260 | 3 |
| 155 | 8052AB24 | MORPHDOOR | 0x0C89 | 01 | 801438E8 | 801438E4 | 80143874 | 80143E40 | 2 |
| 156 | 805250BC | CityChaosEmerald | 0x03E8 | 01 | 800E01B8 | 800E01B4 | 800E0160 | 800E0544 | 1 |
| 157 | 805252F8 | FallBuilding | 0x03E9 | 01 | 800E2D14 | 800E2D10 | 800E2CAC | 800E2978 | 8 |
| 158 | 805251C0 | CityLaser | 0x03EA | 01 | 800E0D6C | 800E0D68 | 800E0D04 | 800E09C0 | 109 |
| 159 | 8055B41C | CITYGADGET | 0x0FA4 | 01 | 802F7500 | 802F74FC | 802F74A8 | 802F75A0 | 46 |
| 160 | 8055B610 | Helicopter | 0x0FA2 | 01 | 802F8B0C | 802F8B08 | 802F8AB4 | 802F948C | 9 |
| 161 | 8055B178 | CITY_BOMB_BIG | 0x0FA0 | 01 | 802F3D38 | 802F3D34 | 802F3CE0 | 802F43B8 | 6 |
| 162 | 8055B288 | CITY_BOMB_SMALL | 0x0FA1 | 01 | 802F58B8 | 802F58B4 | 802F5860 | 802F5F30 | 65 |
| 163 | 8055D600 | BK_TANK | 0x0898 | 01 | 803200B0 | 803200AC | - | - |  |
| 164 | 8055D8A8 | BK_TANK_COLI | 0x0899 | 01 | 80322908 | 80322904 | 8032253C | 8025E738 | 49 |
| 165 | 8055E1D4 | GUN_BUILDING | 0x089B | 01 | 8032C414 | 8032C410 | 8032C3BC | 8032CBD4 | 6 |
| 166 | 8055E160 | FALL_HIWAY | 0x089A | 01 | 8032B584 | 8032B580 | 8032B52C | 8032C0A8 | 15 |
| 167 | 8055E090 | FALL_LARGE | 0x089C | 01 | 8032AD88 | 8032AD84 | 8032AD30 | 8032B2C8 | 4 |
| 168 | 805433C8 | ELEC_CIRCUIT | 0x07D0 | 01 | 801F7C20 | 801F7C1C | 801F7BC8 | 801F7EFC |  |
| 169 | 80543478 | ELEC_ENTRANCE | 0x07E0 | 01 | 801F808C | 801F8088 | 801F8034 | 801F8788 | 44 |
| 170 | 80558C08 | ELEC_EXIT | 0x07EA | 01 | 802C8658 | 802C8654 | 802C8600 | 802C87A0 | 36 |
| 171 | 8052B9DC | ELEC_LIGHT | 0x07D1 | 01 | 801538E0 | 801538DC | 80153888 | 80155B10 | 11 |
| 172 | 8054B238 | ELEC_PANEL | 0x07D2 | 01 | 80259818 | 802597D0 | 8025977C | 8025AB74 | 164 |
| 173 | 8052B808 | ELEC_BAR | 0x07D3 | 01 | 80152818 | 80152814 | 801527C0 | 80153028 | 21 |
| 174 | 8052BC10 | ELEC_SECURITY | 0x07D4 | 01 | 80158010 | 80157FE8 | 80157F94 | 80158830 | 29 |
| 175 | 8052BDE0 | ELEC_BIGBLOCK | 0x07D5 | 01 | 80158A68 | 80158A64 | 80158A10 | 8015922C | 58 |
| 176 | 805435CC | ELEC_WALL | 0x07D7 | 01 | 801F8FEC | 801F8FE8 | 801F8F94 | 801F9CA4 | 171 |
| 177 | 80543680 | ELEC_WALLSWITCH | 0x07E1 | 01 | 801FA304 | 801FA300 | 801FA2AC | 801FA564 | 37 |
| 178 | 8054D050 | ELEC_BARRIER | 0x07D8 | 01 | 80263C18 | 80263C14 | 80263BC0 | 8026401C | 14 |
| 179 | 8054309C | ELEC_CORE | 0x07DB | 01 | 801F4800 | 801F47FC | 801F47A8 | 801F5DA0 | 1 |
| 180 | 8054D0C4 | ELEC_BOMB | 0x07DC | 01 | 80264300 | 802642FC | 802642A8 | 8026484C | 30 |
| 181 | 80549C78 | ELEC_ACCESS | 0x07DD | 01 | 80248D68 | 80248D64 | 80248D10 | 802495A4 | 4 |
| 182 | 8054D2C8 | ELEC_FAN | 0x07DA | 01 | 80266AFC | 80266AF8 | 80266AA4 | 8026734C | 5 |
| 183 | 8052BF08 | ELEC_SPRING | 0x07DE | 01 | 801593FC | 801593F8 | 801593A4 | 80159FB4 | 36 |
| 184 | 80543320 | ELEC_FIREWALL | 0x07DF | 01 | 801F6944 | 801F6940 | 801F68EC | 801F75F8 | 68 |
| 185 | 80546A48 | ELEC_CUBE | 0x07E2 | 01 | 8021C0CC | 8021C0C8 | 8021C074 | 8021C5B0 | 5 |
| 186 | 80546994 | ELEC_SMALLCUBE | 0x07E4 | 01 | 8021B118 | 8021B114 | 8021B0C0 | 8021B9D8 | 18 |
| 187 | 80546AB0 | ELEC_PILLAR | 0x07E3 | 01 | 8021C988 | 8021C984 | 8021C930 | 8021CBAC | 18 |
| 188 | 805480A0 | ELEC_SMALLPILLAR | 0x07E7 | 01 | 80231374 | 80231370 | 8023131C | 80231594 | 11 |
| 189 | 80548108 | ELEC_WAKKA | 0x07E5 | 01 | 8023171C | 80231718 | 802316C4 | 80231958 |  |
| 190 | 8054803C | ELEC_KYUU | 0x084A | 01 | 80230918 | 80230914 | 802308C0 | 80230E80 | 1 |
| 191 | 8054D3B4 | ELEC_ROLLHEXA | 0x07E8 | 01 | 8026752C | 80267528 | 802674D4 | 80267C88 | 10 |
| 192 | 8054D18C | ELEC_COREWALL | 0x07E9 | 01 | 80264BAC | 80264BA8 | 80264B54 | 802659D4 |  |
| 193 | 80559F68 | ELEC_FOOTINGROTATE | 0x07EB | 01 | 802DD7E4 | 802DD7E0 | 802DD78C | 802DD6F0 | 16 |
| 194 | 8055E740 | ELEC_CIRCUITCOLLI | 0x07EC | 01 | 8033C8AC | 8033C8A8 | 8033C854 | 8033CE3C |  |
| 195 | 8055CFFC | BombingWall | 0x1004 | 01 | 8031653C | 80316538 | 803164E4 | 803165E8 | 22 |
| 196 | 8055D2FC | Researcher | 0x1005 | 01 | 8031A404 | 8031A400 | 8031A3AC | 8031A7AC | 20 |
| 197 | 8055D0BC | BombServer | 0x1006 | 01 | 80316EFC | 80316EF8 | 80316EA4 | 80317298 | 12 |
| 198 | 8055D174 | GunLift | 0x1518 | 04 | 80317708 | 80317704 | - | - |  |
| 199 | 8054743C | DEFENSE_SYSTEM | 0x1388 | 01 | 80224790 | 8022478C | 80224738 | 802248DC | 10 |
| 200 | 805473A4 | DEFENSE_SHIELD | 0x1389 | 01 | 80223384 | 80223380 | 8022332C | 8022343C | 63 |
| 201 | 80546CD4 | METEOR_L | 0x138A | 01 | 8021D6B4 | 8021D6B0 | 8021D65C | 8021DA60 | 44 |
| 202 | 80546F48 | METEOR_S | 0x138B | 01 | 8021E520 | 8021E51C | 8021E4B0 | 800C9EBC | 44 |
| 203 | 80547808 | GRINDGADGET | 0x138D | 01 | 802286CC | 802286C8 | 80228674 | 802287C4 | 9 |
| 204 | 805477A4 | ARKCANNON | 0x138E | 01 | 8022813C | 80228138 | 802280E4 | 80228200 | 35 |
| 205 | 8054D834 | SPACEGADGET | 0x1392 | 01 | 8026B558 | 8026B554 | 8026B500 | 8026B5F8 | 378 |
| 206 | 80549A0C | GravityChangeSwitch | 0x14B4 | 01 | 80247178 | 80247158 | 80247104 | 80247580 | 14 |
| 207 | 80549BFC | GravityChangeZone | 0x14B5 | 01 | 8024832C | 8024830C | 802482B8 | 80248750 | 20 |
| 208 | 80549B38 | GravityChangeCollision | 0x14B6 | 01 | 80247C98 | 80247C78 | 80247C24 | 80247D2C | 17 |
| 209 | 8054B048 | GravitySignBoard | 0x14B7 | 01 | 80259474 | 80259454 | 80259400 | 80259520 | 26 |
| 210 | 8054DCB0 | ArkLaser | 0x14BE | 01 | 8026E7F8 | 8026E7F4 | 8026E7A0 | 8026EC04 | 12 |
| 211 | 8054D898 | BACKDROP | 0x189C | 01 | 8026B990 | 8026B98C | 8026B938 | 8026BA30 |  |
| 212 | 8054CF40 | FootingBreak | 0x189E | 01 | 802631E4 | 802631E0 | 8026318C | 80263594 | 16 |
| 213 | 8054CD2C | FootingRoll | 0x189F | 01 | 80261A4C | 80261A48 | 802619F4 | 80261DD0 | 40 |
| 214 | 80547EF4 | SECURITY LASER | 0x13EF | 01 | 8022C81C | 8022C818 | 8022C7C0 | 8022D6D8 | 115 |
| 215 | 80547FAC | SUPER COMPUTER | 0x1771 | 01 | 8022DD78 | 8022DD74 | 8022DD1C | 8022EA1C | 3 |
| 216 | 80547BA4 | MAGMA GUARD | 0x1838 | 01 | 8022B0FC | 8022B0F8 | 8022B0A0 | 8022BAA0 | 6 |
| 217 | 805479D0 | CLOSE WALL | 0x1773 | 01 | 80228DB8 | 80228DB4 | 80228D5C | 802294FC | 1 |
| 218 | 80547B30 | DEFENSE WALL | 0x1772 | 01 | 80229AE8 | 80229AE4 | 80229A8C | 8022AAF0 | 31 |
| 219 | 80547CA8 | POP FOOTING | 0x13F0 | 01 | 8022C008 | 8022C004 | 8022BFAC | 8022C5F4 | 54 |
| 220 | 8054AF2C | RAIL SWITCH | 0x13ED | 01 | 80257D90 | 80257D8C | 80257D34 | 802585C4 | 2 |
| 221 | 8054AE4C | BASE DOOR | 0x13F1 | 01 | 80256A50 | 80256A4C | 802569F4 | 8025785C | 91 |
| 222 | 8054AC30 | ESCAPE_PLANE | 0x13EC | 01 | 8025530C | 80255308 | - | - |  |
| 223 | 8054C918 | E_P_COLI | 0x13F2 | 01 | 8025EBE4 | 8025EBE0 | 8025E500 | 8025E738 | 60 |
| 224 | 80555820 | PLANE RAIL | 0x13F3 | 01 | 802ABEFC | 802ABEF8 | 802ABEA0 | 802AC1C0 | 1 |
| 225 | 8054D6B0 | WATCH CAMERA | 0x1770 | 01 | 80268184 | 80268180 | 80268128 | 80269E60 | 9 |
| 226 | 8054ED68 | MAGMA | 0x1839 | 01 | 8027BD5C | 8027BD58 | 8027BD00 | 8027C1EC | 15 |
| 227 | 8055BE00 | BA Shield | 0x1901 | 01 | 80301A7C | 80301A78 | 80301A20 | 80302088 | 10 |
| 228 | 8055BF60 | BD Hologram | 0x1903 | 01 | 80303D24 | 80303D20 | 80303CC8 | 8030403C | 3 |
| 229 | 8055BE80 | BA Switch | 0x1900 | 01 | 80302540 | 8030253C | 803024E4 | 80303700 | 4 |
| 230 | 8055BD04 | BA Mine | 0x17D5 | 01 | 803005B0 | 803005AC | 80300554 | 803011C8 | 64 |
| 231 | 8055C03C | SUCTION PILLAR | 0x1902 | 01 | 80304698 | 80304694 | 8030463C | 80304F24 | 10 |
| 232 | 8055BB20 | BA GUNSHIP | 0x17D4 | 01 | 802FE820 | 802FE81C | 802FE7C4 | 802FFBAC | 22 |
| 233 | 8055BFC8 | BD True | 0x1B58 | 01 | 80304248 | 80304244 | 803041EC | 80304490 | 2 |
| 234 | 8052C7BC | GadgetStageDataEnd | 0x2583 | 01 | 80169248 | 8016914C | - | - |  |
| 235 | 8053BAB0 | EnemyEntryStart | 0x258E | 02 | 801A238C | 801A2364 | - | - |  |
| 236 | 80539868 | GUN_SOLDIER | 0x0064 | 02 | 80196EB8 | 80196E90 | 80196E38 | 8019708C |  |
| 237 | 80537158 | GUN_BEETLE | 0x0065 | 02 | 80193E54 | 80193E2C | 80193DD4 | 80193FF0 |  |
| 238 | 8054F964 | GUN_BIGFOOT | 0x0066 | 02 | 802873A8 | 80287380 | 80287328 | 80287600 |  |
| 239 | 805384AC | GUN_ROBOT | 0x0068 | 02 | 80195378 | 80195350 | 801952F8 | 80195514 |  |
| 240 | 805508EC | BK_GIANT | 0x008C | 02 | 802889AC | 80288984 | 8028892C | 80288C18 |  |
| 241 | 80531E70 | BK_SOLDIER | 0x008D | 02 | 8018CB50 | 8018CB28 | 8018CAD0 | 8018CCEC |  |
| 242 | 805337C8 | BK_WINGLARGE | 0x008E | 02 | 80190070 | 80190048 | 8018FFF0 | 80190260 |  |
| 243 | 80534BA0 | BK_WINGSMALL | 0x008F | 02 | 801916C0 | 80191698 | 80191640 | 8019185C |  |
| 244 | 80535BEC | BK_WORM | 0x0090 | 02 | 8019212C | 80192104 | 801920AC | 801923C8 |  |
| 245 | 80531924 | BK_LARVA | 0x0091 | 02 | 801898E0 | 801898B8 | 80189864 | 8018A2B4 |  |
| 246 | 805570B0 | BK_CHAOS | 0x0092 | 02 | 802AF78C | 802AF764 | 802AF70C | 802AF978 |  |
| 247 | 80555C48 | BK_NINJA | 0x0093 | 02 | 802AC318 | 802AC2F0 | 802AC298 | 802AC4BC |  |
| 248 | 8052D4C4 | EGG_PAWN | 0x0079 | 02 | 8017BE10 | 8017BDE8 | 8017BD90 | 8017BFAC |  |
| 249 | 80551918 | EGG_PIERROT | 0x0078 | 02 | 80289C0C | 80289BDC | 80289B84 | 80289EDC |  |
| 250 | 80553CB8 | EGG_SHADOWANDROID | 0x007A | 02 | 8029C680 | 8029C658 | 8029C600 | 8029C898 |  |
| 251 | 8054847C | BLACK_BULL_0210 | 0x00B4 | 02 | 80233760 | 80233730 | 80233030 | 80233088 |  |
| 252 | 8054950C | BLACK_BULL_0412 | 0x00B5 | 02 | 80243FCC | 80243F9C | 80243E64 | 80243EBC |  |
| 253 | 8054A26C | HEAVY_DOG_0410 | 0x00B6 | 02 | 8024F59C | 8024F56C | 8024F058 | 8024F0B0 |  |
| 254 | 8054AB3C | HEAVY_DOG_0510 | 0x00B7 | 02 | 802545CC | 8025459C | 8025447C | 802544D4 |  |
| 255 | 8054F400 | BLACK_DOOM | 0x00B8 | 02 | 80283CC8 | 80283C98 | 8028390C | 80283964 |  |
| 256 | 8055AB7C | EGG_MECA_0310 | 0x00B9 | 02 | 802EB478 | 802EB448 | 802EB3A8 | 802EB400 |  |
| 257 | 8055AA54 | EGG_MECA_0411 | 0x00BA | 02 | 802EAE54 | 802EAE24 | 802EAD84 | 802EADDC |  |
| 258 | 8055A914 | EGG_MECA_0511 | 0x00BB | 02 | 802EA7FC | 802EA7CC | 802EA2DC | 802EA334 |  |
| 259 | 80559CE8 | (unnamed) | 0x00BC | 02 | 802D9198 | 802D915C | 802D9108 | 802D9988 |  |
| 260 | 8055B900 | EGG_LASTMECH | 0x00BD | 02 | 802FE4A8 | 802FE47C | 802FE428 | 802FE6A8 |  |
| 261 | 8055C8AC | DEVIL_DOOM | 0x00BE | 02 | 8030E11C | 8030E0EC | 8030DBBC | 8030DC14 |  |
| 262 | 8053BAD4 | EnemyEntryEnd | 0x258E | 02 | 801A2360 | 801A235C | - | - |  |
| 263 | 8053D018 | VehicleDataStart | 0x258C | 01 | 801C09F8 | 801C09F4 | - | - |  |
| 264 | 8053CA50 | Noraml Car | 0x0046 | 01 | 801B8E70 | 801B8C84 | - | - |  |
| 265 | 8053CA7C | Open Car | 0x0047 | 01 | 801B8E44 | 801B8C60 | - | - |  |
| 266 | 8053CAA8 | Armed Car | 0x0048 | 01 | 801B8E18 | 801B8C3C | - | - |  |
| 267 | 8053CAD4 | Bike | 0x0049 | 01 | 801B8DEC | 801B8C18 | - | - |  |
| 268 | 8053C3E0 | (unnamed) | 0x004A | 04 | 801ACDFC | 801ACDCC | - | - |  |
| 269 | 8053C404 | (unnamed) | 0x004B | 04 | 801ACD30 | 801ACD00 | - | - |  |
| 270 | 8053C5B8 | (unnamed) | 0x004C | 04 | 801AFA64 | 801AFA34 | - | - |  |
| 271 | 8053C74C | (unnamed) | 0x004D | 04 | 801B30C8 | 801B3098 | - | - |  |
| 272 | 805590BC | (unnamed) | 0x004E | 04 | 802CC868 | 802CC838 | - | - |  |
| 273 | 8053D03C | VehicleDataEnd | 0x258D | 01 | 801C0998 | 801C096C | - | - |  |
| 274 | 8054EA80 | Sonic | 0x0191 | 04 | 8027ACFC | 8027AB4C | - | - |  |
| 275 | 8054EAA4 | Tails | 0x0192 | 04 | 8027ACD8 | 8027AB28 | - | - |  |
| 276 | 8054EAC8 | Knuckles | 0x0193 | 04 | 8027ACB4 | 8027AB04 | - | - |  |
| 277 | 8054EAEC | Amy | 0x0194 | 04 | 8027AC90 | 8027AAE0 | - | - |  |
| 278 | 8054EB10 | Rouge | 0x0195 | 04 | 8027AC6C | 8027AABC | - | - |  |
| 279 | 8054EB34 | Omega | 0x0196 | 04 | 8027AC48 | 8027AA98 | - | - |  |
| 280 | 8054EB58 | Vector | 0x0197 | 04 | 8027AC24 | 8027AA74 | - | - |  |
| 281 | 8054EB7C | Espio | 0x0198 | 04 | 8027AC00 | 8027AA50 | - | - |  |
| 282 | 8054EBA0 | Maria | 0x0199 | 04 | 8027ABDC | 8027AA2C | - | - |  |
| 283 | 8054EBC4 | Charmy | 0x019A | 04 | 8027ABB8 | 8027AA08 | - | - |  |
| 284 | 8054EBE8 | Eggman | 0x019B | 04 | 8027AB94 | 8027A9E4 | - | - |  |
| 285 | 8054EC0C | DoomsEye | 0x019C | 04 | 8027AB70 | 8027A9C0 | - | - |  |

kinds {'01': 186, '04': 72, '02': 28} total 286

Note: ctor 0x800EDF64 (CONTAINER, HRD_CONTAINER, WEAPON_CONTAINER, SECRET CONTAINER, HEALING_UNIT) is the create's
cleanup helper, not the constructor - those creates build the object through a second step (CONTAINER: 0x800F6FC8 /
0x800F8660 per DRAW_DISTANCE.md). BOMB's ctor is 0x800F10BC (corrected by hand). Enemy (kind 2) layout counts are not
filled in: their entries are counted under their ids (GUN_SOLDIER 0x64 ... EGG_SHADOWANDROID 0x7A) in `layoutcensus.txt`.
