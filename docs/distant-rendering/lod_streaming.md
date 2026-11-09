# Level of detail and resource streaming - Shadow the Hedgehog (GUPP8P main.dol), static RE

Date 2026-10-29. Static analysis only (no game run). Scripts in this folder: `dis_all.py` (full text
disassembly -> `all.asm`, 1,216,776 lines), `xref2.py` (lis/addi + base+offset xref map, `xref.pkl`),
`calls.py` (calls + string refs in a range), `onels.py` (ONE archive listing), `txdall.py` / `txdstat.py`
(GCN native texture survey), `chunkscan.py` (RW chunk-ID census of every DFF/RG1/BSP/RP2 on disc).
r13 = 0x805EC500, r2 = 0x805FA780.

Legend: VERIFIED = read in the code/data this session; INFERRED = deduced, not proven; U = unknown.

---------------------------------------------------------------------------------------------------
## 0. Summary

* **There is no geometric LOD of any kind.** No RpLODAtomic, no MRM, no alternate low-poly models chosen by
  distance, no impostors/billboard substitutes for 3D models. What limits detail with distance is:
  1. the layout spawn range (object exists only within 100*range units; DRAW_DISTANCE.md),
  2. ART GADGET's 0.15 s alpha fade at the range edge (DRAW_DISTANCE.md),
  3. the landscape block visibility table (landscape block system, out of scope here),
  4. the camera far plane (fog end pinned to it),
  5. RwCameraFrustumTestSphere culls (ASPECT_INVENTORY G2).
* **There is no texture LOD.** 8,432 textures surveyed on disc: every one is a single level (no mipmaps),
  so the trilinear filter mode the game forces collapses to bilinear. LOD bias 0, max aniso 1 in every file.
* **There is no animation LOD.** The motion update has no distance or visibility input (one class, ElecCoreWall,
  gates an animation state on its frustum test).
* **Streaming other than landscape blocks:** everything a stage needs is loaded at stage start
  (gadget/effect/enemy archives, layout, sound banks), selected per stage by `setid.bin`. The only
  on-demand traffic during play is (a) ARAM -> main RAM copies of motion clips when a clip is first played
  (synchronous ARQ + busy-wait), (b) ADX music / AFS voice / SFD movie streams (CRI), (c) event archives.

---------------------------------------------------------------------------------------------------
## 1. Geometric LOD

### 1.1 RenderWare plugin registrations (VERIFIED)

Every `li r4, ID` followed by a `bl` within 10 instructions, IDs 0x100-0x1FF. Register functions are
identified by the plugin mix (INFERRED names; addresses VERIFIED):

| register fn | INFERRED identity | plugin IDs registered (call site) |
|---|---|---|
| 0x80486DC8 | RwEngineRegisterPlugin | 0x11D collis (0x80442098), 0x122 dmorph (0x804458BC), 0x11E HAnim (0x80448270), 0x12F PTank (0x804486B0), 0x116 skin (0x8044DC00), 0x11F userdata (0x80453D4C), 0x135 UVAnim (0x80454AE8), 0x120 MatFX (0x804A2EE4), 0x129 GCN mat (0x804A7CA0), 0x12C multitex (0x804A9F28) |
| 0x80459BBC | RpAtomicRegisterPlugin | 0x1FF game plugin size 0x20 (0x80037E5C), 0x11D, 0x122, 0x12F, 0x116 (size 0xC), 0x120 |
| 0x8045A99C | RpGeometryRegisterPlugin | 0x11D, 0x122, 0x116, 0x11F |
| 0x80460348 | RpWorldSectorRegisterPlugin | 0x11D, 0x11F, 0x120 |
| 0x8045C31C | RpMaterialRegisterPlugin | 0x11F, 0x135 (size 0x28), 0x120 |

* **rwID_LODATMPLUGIN = MAKECHUNKID(1, 0x12) = 0x112** (`rpcriter.h:62`). The only `li r4, 0x112` in the DOL
  is at 0x80115F80 and feeds 0x8006EF58, a game constructor (`bl 0x8006E57C; vtable 0x805226B0`) that takes
  55 different small ids 0x101-0x12A - not a RW registration. **RpLODAtomic is not linked.** VERIFIED.
* No MRM (0x111), no mipmap-K (0x198), no anisotropy plugin (0x127), no PVS (0x106/0x12A) registration. VERIFIED.
* The game's own atomic plugin 0x1FF ([r13-0x7D08] = offset; stream callbacks registered by
  `RpAtomicRegisterPluginStream` 0x80459C3C: read 0x80037F74, write 0x80037EC0, size 0x80037EB8 = 0x20).
  Layout from the accessors 0x80037D08..0x80037E30: +0 RGBA, +4 RGBA, +8 word, +0xC 3 words (light group,
  DRAW_DISTANCE.md), +0x18 {word, render-callback object} (0x80037CC4..0x80037CEC calls `[+0x18]+4`). Nothing LOD.
  No disc file carries a 0x1FF chunk (1.4 below), so the stream read never runs. VERIFIED.

### 1.2 Strings (VERIFIED)
`strings.csv` has no "lod"/"LOD"/"Lod", "mip", "billboard", "impost", "detail" (other than boost::detail),
"lowpoly". Distance strings are AI-only ("EnemyAIState_BkSoldierAI_NearAttack" 0x804C7D84,
"ROLLING_DISTANCE m" 0x804B8A8C, editor "distance").

### 1.3 Alternate models (VERIFIED from disc)
* Stage gadget archives `stgNNNN_gdt.one`: pairs `X.DFF` / `X_BRK.DFF` (Westopolis: CT0100BENCH / _BRK ...)
  = intact vs broken versions (destruction), not LOD. `...LEFT.DFF` = mirrored halves.
* `character/shadow2p*.one` (SW2PY.DFF 109,169 B vs SHADOW_BODY.DFF 103,155 B): 2-player costume skins of
  equal complexity, not reduced LOD.
* `SDW_ALL.DFF` (5,655 B) in shadow.one and `common/VolumeShadowModel.one`: low-poly shadow-volume casters -
  a proxy for the shadow pass, not distance LOD (INFERRED from name/size).

### 1.4 Model chunk census (VERIFIED, chunkscan.py over every DFF/RG1/BSP/RP2 in stg*/, character/, enemy/,
enemy/boss/, event/, common/)
Chunk IDs present: 0x1 0x2 0x3 0x6 0x7 0x8 0x9 0xA 0xB 0xE 0xF 0x10 0x14 0x1A 0x1F 0x29 0x2A 0x116 0x11D
0x11E 0x11F 0x120 0x122 0x135 0x50E 0x510. **No 0x112 (LOD atomic), no 0x111 (MRM), no 0x1FF.**
Stage totals: 2,866 clumps / 7,151 atomics in DFFs; 1,625 RG1 worlds; 614 COLI BSPs.

### 1.5 Landscape display layers - meaning and distance (VERIFIED code, INFERRED meanings)
Table 0x8051E328, 15 x 0x18 `{suffix*, task name*, flags, level, 0, 0}` (suffix strings 0x805F2C50..0x805F2CA0):

| idx | suffix | flags | level | | idx | suffix | flags | level |
|---|---|---|---|---|---|---|---|---|
| 0 | _N_ | 0x28 | 1 | | 8 | _AF_ | 0x5C | 5 |
| 1 | _D_ | 0x28 | 1 | | 9 | _A_ | 0x5C | 5 |
| 2 | _OS_ | 0x2C | 1 | | 10 | _AL_ | 0x4D | 5 |
| 3 | _OSL_ | 0x2D | 1 | | 11 | _K_ | 0x9C | 7 |
| 4 | _O_ | 0x2C | 1 | | 12 | _M_ | 0x11C | 7 |
| 5 | _OW_ | 0x3C | 1 | | 13 | _G_ | 0x98 | 9 |
| 6 | _OL_ | 0x2D | 1 | | 14 | _GL_ | 0x99 | 9 |
| 7 | _P_ | 0x80C | 1 | | | | | |

Flag decoding is in docs/effects/census.md section 4 (0x80046D80): 0x1 lit, 0x4 fog, 0x8 force bilinear,
0x10 no cull, 0x40 alpha, 0x80 additive, 0x100 src*(1-dst), 0x800 punch (alpha ref 200). Only the table base
is referenced by code (xref of every entry address: only entry 0 offsets +0/+0x14 and entry 14 +0x10), so `_D_`
and `_N_` differ only by name - same flags, same level, same draw path. VERIFIED.

**No layer is distance-limited.** `0x800471BC(land, layer, camera)` (VERIFIED, quoted):
```
8004720c lbz r0,0x1808(r29)      ; combined-world mode?
8004722c lwz r0,0(r3); cmpwi r0,5 ; block slot state 5 = loaded
80047240 lwz r4,0xbc(r3); mr r3,r25 ([0x8051DFA8]); li r5,1; bl 0x8003AA58   ; block visible?
80047260 bl 0x80463180 (RpWorldAddCamera) / 8048652C (RwCameraBeginUpdate) / 80046D80 (states) /
80047280 bl 0x80460FAC (RpWorldRender) / 80486504 (EndUpdate) / 804631D4 (RemoveCamera)
800472a0 cmpwi r24,0x20          ; 32 block slots of 0xC0 bytes, layer world at +4+layer*0xC
```
The only gate is the block's load state and `0x8003AA58` - a per-player table lookup
(`lhax r0, block*2 + 0x48 + player*0x150`, 2 players) of the block system. No distance, no far-plane change
per layer. Then the layer's object world (layer entry +0x10) is drawn the same way.

What `_N_` and `_D_` contain (VERIFIED from files, meaning INFERRED): small, huge, unfogged backdrop worlds.
| file | tris | verts | bbox |
|---|---|---|---|
| STG0100_D_01.RG1 (Westopolis) | 122 | 77 | x -5739..5718, y -6957..4383, z -7048..4408 |
| STG0601_60_D_.RG1 (Final Haunt, block 60) | 192 | 104 | x +-10262, z -13187..7337 |
| STG0210_N_01.RG1 | 96 | 56 | symmetric +-5397 (dome-like) |
| compare STG0100_O_01.RG1 | 2782 | 3280 | -1595..1462 x, -4432..325 z |
`_D_` = distant scenery (likely "遠景" backdrop: skyline, far walls), `_N_` = sky/no-fog shell. They are not
low-detail versions of other geometry; they are always drawn when their block is visible. Final Haunt/
stg0604 put `_D_` in dedicated blocks 60-62 (`STG0601_60_D_`, `_61_D_`, `_62_D_`; stg0604 same), i.e. the
backdrop is a block of its own the block table can show from anywhere. Layer census over all `_NN.one`
(RG1 members): OS 374, G 290, O 139, A 73, D 53, AF 32, K 21, N 18, P 13, OW 1 (+ irregular names); `_D_`
in 16 stages, `_N_` in 5 (stg0210 0600 0610 0611 0801).

### 1.6 Other "LOD-like" mechanisms (VERIFIED where cited)
* Spawn range: 0x800CA970 / 0x800C9D54 (DRAW_DISTANCE.md). The only object-level distance cut.
* ART GADGET fade: 0x800E43F0 gate, holder fade 0x80160078 / 0x80160004, 0.15 s (0x805F5BBC).
* Frustum sphere test RwCameraFrustumTestSphere 0x80486718 (13 callers, ASPECT_INVENTORY G2) - culling only.
* Enemy on-screen test fn_8019DA1C (ASPECT_INVENTORY G1) - AI targeting, not drawing detail.

---------------------------------------------------------------------------------------------------
## 2. Texture LOD

### 2.1 GX texture object setup (VERIFIED)
* `GXInitTexObjLOD` = **0x803980C8** (identified by the GX filter conversion table {0,4,1,5,2,6} at 0x805EED30 =
  r13+0x2830, used at 0x80398138; LOD bias `*32` packing, min/max LOD `*16`). Callers: 0x8032E164 (SDK/font:
  `li r4,1; li r5,1; f1=f2=f3=[r2+0x6A8]; li r6,0; li r7,0; li r8,0`), and six inside the RW GCN texture driver
  0x8049AE54, 0x8049B004, 0x8049B1B8, 0x8049B348, 0x8049B4F8, 0x8049B6FC.
* RW driver binding (0x8049AC88 area): min/mag from the table at **0x8056FC90** indexed by RwTextureFilterMode
  (texture +0x50 low byte), 8 bytes per mode {min, mag}:
  `0 NA {0,0}, 1 NEAREST {0,0}, 2 LINEAR {1,1}, 3 MIPNEAREST {2,0}, 4 MIPLINEAR {3,1}, 5 LINEARMIPNEAREST {4,0},
  6 LINEARMIPLINEAR {5,1}`; address table at 0x8056FCC8 {NA 0, WRAP 1, MIRROR 2, CLAMP 0, BORDER 0}.
  For palettized rasters (cFormat & 0x6000) modes 5 and 6 are replaced by entry 4 {GX_LIN_MIP_NEAR, LINEAR}
  (0x8049AD94..0x8049ADB0, 0x8049B62C..0x8049B64C).
  `min_lod = 0.0 (r2+0x1D18 = 0x805FC498)`, `max_lod = (float)raster byte +0x32`, `lod_bias = 0.0`,
  `bias_clamp 1, edge_lod 1, max_aniso GX_ANISO_1` on the default path (0x8049AE28..0x8049AE54).
* **0x8049B568(texture, f1 lodBias, r4 biasClamp, r5 edgeLOD, r6 maxAniso)** re-inits the texobj with explicit
  LOD params (`mr r8,r28` = r6 -> max_aniso at 0x8049B6E4). Callers: native-texture stream read 0x8049AB50
  (values from the 0x58-byte GCN texture header: +0x8 aniso, +0xC clamp, +0x10 edge, +0x14 float bias), and
  the game at 0x80038C24/0x80038C48/0x80038C90 and 0x800395C0/0x800395F0.

### 2.2 The game's forced filter (VERIFIED)
`0x80039640(r4 = texdict, r5 = mode)` stores the mode in [r13+0x2C88] and runs callback 0x80039550 over every
texture (0x8048DC30 = RwTexDictionaryForAllTextures); 0x80038B80 is the same with callback 0x80038BB0 and the
mode in r5. Every caller passes **mode 6 (rwFILTERLINEARMIPLINEAR)**:
0x80013D2C (`li r5,6`, after a TXD load, ResourceOneFile path), 0x80046CC8 (landscape TXD, `li r5,6`),
0x8006A45C (`li r4,6`, ResourceOneFileReq TXD), 0x80171994 (`li r5,6`).
Callback 0x80039550:
```
80039580 bl 0x8048AD34          ; raster mip level count (1 unless cFormat & 0x8000)
80039584 cmpwi r3,1 ; ble 0x800395F8
8003958c ... rlwimi filter = [r13+0x2C88]           ; mips: use mode 6
800395a0 rlwinm. r0,r3,8,0x11,0x12 ; palettized?
800395ac -> 0x8049B568(tex, bias, clamp, 1, 0)       ; palettized: aniso 1
800395c8 andi. r0,r3,0x9000 ; mode==6 -> 0x8049B568(tex, bias, clamp, 1, 2)  ; GX_ANISO_4
800395f8 single level: filter = 2 (LINEAR) if mode 6 else 1 (NEAREST)
```
0x80038BB0 differs only in passing aniso 0 / clamp 0 / edge 0 for non-palettized mip textures.
Landscape layers additionally set `RwRenderStateSet(rwRENDERSTATETEXTUREFILTER=9, 2 LINEAR)` when layer flag 0x8
is set (0x800470F4..0x8004712C; all 15 layers have 0x8), else 0 (NA).

### 2.3 Mipmaps on disc (VERIFIED, txdall.py)
GCN native texture struct: +0 platform 6, +4 filter|addrU|addrV (0x00001101 = NEAREST, WRAP, WRAP), +8 aniso,
+0xC clamp, +0x10 edge, +0x14 bias, +0x18 name[32], +0x38 mask[32], +0x58 raster format, +0x5C width, +0x5E height,
+0x60 depth, +0x61 level count.

| set | textures | levels | file filter | (aniso, clamp, edge, bias) |
|---|---|---|---|---|
| stg*/*.one (gdt, fx, texCM, TEXnn, blocks) | 5,928 | all 1 | all NEAREST | all (0,1,1,0.0) |
| event/*.one | 2,166 | all 1 | NEAREST | (0,1,1,0.0) |
| enemy/boss/*.one | 144 | all 1 | NEAREST | same |
| common/*.one | 113 | all 1 | NEAREST | same |
| character/*.one | 81 | all 1 | NEAREST | same |
| enemy/*.one | 60 | all 1 | NEAREST | same |
Formats (stage): 0x4204 PAL4|565 3,579; 0x0004 (native GX format, no RW palette) 1,106; 0x2204 PAL8|565 508;
0x4304 PAL4|4444 298; 0x0204 565 198; 0x0304 4444 92; 0x4104 PAL4|1555 79; others < 50.
**Result: zero mipmapped textures; the forced mode 6 resolves to rwFILTERLINEAR (bilinear, no mips) through
0x800395F8; max_lod = 0; LOD bias and anisotropy are never used.** No distance-dependent texture swapping
(the `_TEXnn` landscape dictionaries, 0x8004652C, are selected per model name, not by distance).

---------------------------------------------------------------------------------------------------
## 3. Animation LOD (no evidence of any)

* Motion player update **0x80416EF4** (VERIFIED, quoted): enter motion lock ([r13+0x5F50] 0x8040D254), clear
  flags +0x14, per channel (`mulli r0,r4,0x130`) 0x8041BB8C, then 0x8041AFAC, 0x8041A33C, **0x80419A9C** (time
  advance), 0x804198F0, 0x80419094, 0x80418618, unlock 0x8040D294. No distance, camera or visibility input.
* Enemy motion component: `0x802CE434` (dt = [0x80578760+0x24] * speed +0x38, `fmuls`) -> 0x801A4C04 ->
  **0x801A4CF0** which runs 0x80416EF4 iff holder +0 != 0 and enable byte +0xC != 0. VERIFIED. Who clears +0xC
  was not found (INFERRED: motion stop / state, not distance).
* 0x80416EF4 has 31 callers (player 0x800B13FC, model holders 0x80163310/0x80163518, enemies, bosses...); none
  inspected passes a far/near flag. INFERRED: all updated every frame while alive.
* The only visibility-gated animation found: ElecCoreWall 0x80264C64 (state 1 -> 3 only inside the frustum
  branch, ASPECT_INVENTORY G2).
* Skinning: RpSkin 0x116 and RpHAnim 0x11E only; no bone-count reduction exists in the data (single skin per model).
* The effective "animation LOD" is the spawn range: an unspawned enemy has no object, no motion.

---------------------------------------------------------------------------------------------------
## 4. Resource streaming outside the landscape blocks

### 4.1 Framework (VERIFIED)
* Resource manager: [r13+0x2B7C] = std::map<int type, container>; `0x8001299C(mgr, type, obj)` inserts only if
  absent (lower_bound 0x8001247C/0x800124E4, count +0x10); `0x800127F8(mgr, type)` removes. 0x800122D4 returns
  the manager.
* `ResourceOneFile` ctor **0x80014C58(this, name, async)** (vtable 0x8051D7DC; only stores the name; async flag ->
  bit 0 of +0x1C). Load = vtable +0xC **0x800149F4**, state at +4: 0 -> sync: `TOneFile` 0x58 bytes,
  0x8004CFD8(onefile, name, 0) reads and parses at once, 0x80013C30 instantiates, state 3; async: 0x8004CC94
  (callback 0x80014DAC sets state 2) and registration in the async poll list (0x8025D580 on 0x8056FFC8),
  state 1; state 2 -> instantiate, state 3; 4 = failure. **Every caller passes async 0** (32 sites, list in 4.3).
* `ResourceOneFileReq` ctor 0x8006B3CC (gdt, stg_cmn_gdt, WeaponResource, vehicleResource) - per-member records
  (0x3C each, find 0x8006B090), TXD members get the forced filter at 0x8006A45C.
* File read: 0x80037200() = file system object, vtable +4 exists(name); 0x800450C4 reads a whole file into
  RAM (synchronous). Async ONE reads: 0x8004CC94 via `TOneFileAsyncManager` (0x8004D564, string
  0x8004D594) - used by the land manager (`%s/%s_TEX%02d.one` 0x800468A0), `_dat.one` (0x80177920, followed by
  the wait 0x8004CE1C at 0x80177940) and event archives (0x80171D50).

### 4.2 Stage-start preload selection: setid.bin (VERIFIED)
`0x800CBC54` (called once from the stage builder 0x801783D4 at 0x8017871C):
```
800cbc7c "setid.bin" -> 0x800CB044 (read 0x800450C4, swap 0x800CB728/0x800CB6EC)
800cbc94 bl 0x800CA7F0(table, factory list 0x8052C1A0, stage index r30)
800cbca4 name 0x800CB6BC (cmn.dat) -> 0x800CB1A4(mgr, name)
800cbcbc lbz [0x80578760+0x125] ? 0x800CB65C hrd.dat -> 0x800CB0EC(mgr, name, 0x4000, 0xB000)
                                : 0x800CB68C nrm.dat -> 0x800CB0EC(mgr, name, 0x2000, 0xD000)
800cbd28 name 0x800CB62C (ds1.dat) -> 0x800CB0EC(mgr, name, 0x8000, 0x7000)  ; (r5, r6) meaning U (id bases?)
800cbd50 bl 0x800CA780 -> factory +4 (preload) for each factory with +0x1B bit 0
```
`setid.bin` (3,632 B): {u32 0, u32 count = 302 (LE)}, 302 x 12 B {u32 object type id, u32 mask0, u32 mask1}
(little-endian on disc). 0x800CA7F0 clears bit 0 of every factory's +0x1B, then for each entry whose bit
`1 << (31 - s%32)` in word s/32 is set (s = stage index argument) marks the factory whose u16 +0x18 equals the id.
0x800CA780 then calls `factory->preload` (+4) for each marked factory; 0x800CA720 calls `release` (+8) at teardown
(0x800CBC28). Factory record: {name*, preload, release, create, ..., u16 id +0x18, flags +0x1B}; e.g. BK_SOLDIER at
0x80531E70 = {0x804C9DF4, 0x8018CB50, 0x8018CB28, 0x8018CAD0, 0, 0, id 0x008D}. Its setid row: id 0x8D,
masks 0x87EAAA38 / 0x80070000. **So a class's archive is resident for the whole stage if setid.bin says the stage
uses it, whether or not any instance ever spawns.**

### 4.3 Resource types (VERIFIED: every `li r4,N; bl 0x8001299C`)
| type | file | registered at | loader / ctor |
|---|---|---|---|
| 0 | stg_cmn_gdt.one | 0x8015DC7C | ResourceOneFileReq (0x8015DC68) |
| 1 | stg%04d/stg%04d_gdt.one | 0x801692CC (fn 0x80169248, if file exists) | ResourceOneFileReq |
| 2 | common/WeaponResource.one | 0x8005E40C | ResourceOneFileReq |
| 3 | common/vehicleResource.one | 0x801C09DC | ResourceOneFileReq |
| 6 | enemy/EnemyPath.one | 0x801A23D4 | ResourceOneFile sync |
| 7 | enemy/GunSoldierData_%s.one | 0x80196F24 | sync |
| 8 | GunBeetleData | 0x80193E9C | sync |
| 9 | GunBigfootData | 0x802873F0 | sync |
| 0xA | GunRobotData | 0x801953C0 | sync |
| 0xB | BkGiantData | 0x802889F4 | sync |
| 0xC | BkSoldierData | 0x8018CB98 (preload 0x8018CB50, release 0x8018CB28) | sync |
| 0xD | BkWingLargeData | 0x801900B8 | sync |
| 0xE | BkWingSmallData | 0x80191708 | sync |
| 0xF | BkWormData | 0x80192174 | sync |
| 0x10 | BkLarvaData | 0x80189928 | sync |
| 0x11 | BkChaosData | 0x802AF7D4 | sync |
| 0x12 | BkNinjaData | 0x802AC360 | sync |
| 0x13 | EggPawnData | 0x8017BE58 | sync |
| 0x14 | EggPierrotData_{horror,busou,pierro} | 0x80289C94/CCC/D04 | sync |
| 0x15 | EggShadowAndroidData | 0x8029C6C8 | sync |
| 0x16 | boss archives (BlackBull0210/0412, HeavyDog0410/0510, BlackDoom, diablon, EggMeca, EggLastMech, DevilDoom) | 0x802337A0 ... 0x8030E15C | sync |
Also ResourceOneFile sync without a typed slot: common/LensFlare.one (0x801D1FC0), 0x8027B208, 0x803054F4 (fx).

### 4.4 Motion clips: ARAM residency and on-demand copy (VERIFIED)
* Motion system init 0x800151EC: allocator hooks 0x8041213C(alloc/free/...) -> RW memory with hint 1 or 2
  (0x80049BB8 = RwEngineInstance+0x108 malloc, +0x32 hint; 0x80049B88 calloc +0x114; 0x80049B5C free +0x10C);
  pools `0x80411810(0x80, 0xA00, 0x100, 0x1400)`; `0x80422224(1)` sets ARAM mode [r13+0x5F90].
* ARAM pool: **0x804225B0 `ARAlloc(0x600000)` = 6 MiB** at [r13+0x5F98]; block list [r13+0x5F9C] of
  {next, prev, aram addr, size} nodes (0x10 B, from 0x80412184); first-fit alloc 0x804223B8 with the bound
  `addis r0, base, 0x60` (0x804224B4); free 0x80422614.
* Bank -> ARAM: 0x80414588 (called from the bank registration at 0x80414274): table `count*0x18 + 0x1C`
  (`mulli r4,r0,0x18; addi r4,r4,0x1C`), each clip's name/clip/events uploaded MRAM->ARAM (0x804222C0), the
  main-RAM copy freed (0x8041214C).
* Clip acquire **0x80414368** (entry, body 0x80414370) called by SetMotion paths 0x804128A4, 0x80412E78,
  0x80413444, 0x80413664, 0x80413888, 0x8041ADD4: find the bank by id range (+0 first id, +2 count); if bank
  byte +6: search the resident list (+0x18, link +0x10) -> refcount +2 ++; else allocate
  `name+clip+events (32-aligned) + 0x40` from the motion allocator and copy each part ARAM->MRAM with
  **0x80422234**: `ARQPostRequest(.., type 1, prio 1, ..., cb 0x80422344)` then
  `80422294 lwz r0,0x5FA0(r13); cmpwi r0,1; bne 0x80422294` - a **synchronous busy-wait** on the game thread.
  So the first play of a clip stalls for one ARAM DMA; later plays reuse the refcounted copy.

### 4.5 Sound
* Sound banks `.mlt` (sound/B0..B81): loaded synchronously at stage/menu setup into fixed ARAM slots
  (GCAX 8 MiB `ARAlloc(0x800000)` 0x8043F7A4); bank 7 slot = stage bank (B71_CITY..B81_FINALDOOM), chosen by
  0x801F2A80 (docs/GCAX_SOUND_MAP.md sections 1-2, VERIFIED there).
* `sound/AudioDataStage*.bin` SE tables: 0x801D8A20 picks one per stage group (0x801D8AFC loads), plus
  System/Gadget/Weapon/Player/Enemy tables (strings 0x804D1600..0x804D18C0).
* Streams (CRI, outside the game's resource manager): `sng_stg%04d.adx` (0x801D3F60, BGM), `sng_jin_*`,
  `sng_EV%04d_E.adx`, `PRS_VOICE_E.afs` (voice), `*.sfd` (movies). CRI ARAM: `ARAlloc(0x40000)` 0x803D06E0.
  IPL font ARAM 0x8032E23C (0x120F00 SJIS / 0x20120 ANSI).

### 4.6 Events
`event\event%04d_scene%c.one` (0x80171BA0; sync 0x80171D28 or async 0x80171D50) and `event/event%04d.scr`
(0x80174500); event music `sng_EV%04d_E.adx` (0x80173CEC). Loaded when the event starts (INFERRED: in-stage
events pause play while loading).

### 4.7 Memory budgets (VERIFIED where cited)
* Main RAM: operator new 0x803A1380 -> MSL malloc 0x803A3E14 (pool control 0x805A6300, 0x34 B; calloc
  0x803A3C90, free 0x803A3DA4). No fixed per-system budgets were found; the pool grows from the OS arena
  (INFERRED: arena = MEM1 above the image, ~24 MiB minus DOL/BSS). Running out aborts (bad_alloc;
  DRAW_DISTANCE.md: Final Haunt at x8 range after ~101 s).
* A `Sonicteam::System::Heap` class (vtable 0x8056AF2C, ctor 0x8040C040 over a caller buffer, first-fit
  alloc 0x8040BFA8) exists for sub-heaps (built by 0x80028878, reached through vtables 0x8051DB38/0x8053E944);
  sizes U.
* ARAM (16 MiB): AR stack base 0x4000; GCAX 8 MiB; motion pool 6 MiB; CRI 256 KiB; font up to 1.13 MiB. INFERRED
  total ~15.4 MiB - ARAM is nearly full, which explains why motion clips live in ARAM and are paged in.
* Motion pool sizes 0x80/0xA00/0x100/0x1400 (0x80015244..0x80015254; meaning of each U).

---------------------------------------------------------------------------------------------------
## 5. Every file category under game/files/stgNNNN and its loader

Census (VERIFIED, all stage folders): `_NN.one` 787, `_dat.one` 63, `_texCM.one` 62, `_cmn.dat` 43, `_fx.one` 42,
`_gdt.one` 42, `_ds1.dat` 41, `_light.bin` 41, `.fog` 39, `_cam.dat` 28, `_hrd.dat` 23, `_nrm.dat` 23,
`_hint.bin` 17, `_tex02.one` 2, `_tex01.one` 1.

| file | contents (members) | loader (string xref) | when / how |
|---|---|---|---|
| stgNNNN_NN.one | landscape block: `STGxxxx_<LAYER>_NN.RG1` (RpWorld per display layer), `_COLI_NN.BSP` collision, rare `.RP2` | `%s/%s_%02d.one` 0x80046B28 (LandALoadManager 0x80046AF0) | landscape block system (out of scope) |
| stgNNNN_texCM.one | `STGxxxx_TEXCM.TXD` landscape textures | `%s/%s_texCM.one` 0x8003AC38 (fn 0x8003AB90, sync 0x800450C4); filter forced at 0x80046CC8 | stage start |
| stgNNNN_tex01/02.one | extra landscape TXDs (`_TEXnn` suffix select, 0x805742E0) | `%s/%s_TEX%02d.one` 0x80046874 (async 0x800468A0) | stage start |
| stgNNNN_gdt.one | gadget DFF/_BRK.DFF, STG_GDT.TXD, .BIN libraries, .CCL collision, .BON, .MTN/.MTP, .UVA, .ADB, .DMA, .PTB | 0x8016925C (fn 0x80169248) -> resource type 1 | stage start, all members resident |
| stgNNNN_fx.one | STGxxxx_FX.EFD effect defs, FX.TXD, effect DFFs, .UVA | 0x801692D8 -> effect manager 0x803054B8 / r13+0x3DE8 | stage start |
| stgNNNN_dat.one | PATH.PTP (paths), STGxxxx.BDT (block table), Sxxxx_*.ADB | 0x80177898 (fn 0x80177868; async 0x80177920 + wait 0x80177940; PATH.PTP 0x801779A4) | stage start |
| stgNNNN_cmn/nrm/hrd/ds1.dat | object layout ("sky2", 0x2C entries) | 0x800CB62C..0x800CB6BC via 0x800CB044 (sync read); hard flag [0x80578760+0x125] | stage start |
| stgNNNN_light.bin | 16 light groups x 0x34 | 0x8004874C (fn 0x80048738) | stage start |
| stgNNNN.fog | fog parameters | 0x8016A5FC (fn 0x8016A5E4) | stage start |
| stgNNNN_cam.dat | camera regions | 0x801C8784 (fn 0x801C873C) | stage start |
| stgNNNN_hint.bin | hint messages | 0x802F33DC (fn 0x802F33BC, sync 0x800450C4) | stage start |
| (root) setid.bin | per-object-type stage bitmask | 0x800CBC7C | stage start |
| (root) stg_cmn_gdt.one, common/*.one, enemy/*.one, enemy/boss/*.one | shared / class archives | 4.3 | factory preload at stage start (setid) |
| sng_stgNNNN.adx | BGM stream | 0x801D3F60 | streamed during play |

---------------------------------------------------------------------------------------------------
## 6. Runtime checks needed (exact addresses)

1. Confirm no texture ever has >1 level at runtime: break/log 0x8048AD34 return in 0x80039580 and 0x80038BE4
   (expect always 1), and log GXInitTexObjLOD 0x803980C8 args (r4..r8, f1..f3) for a stage: expect max_lod 0,
   bias 0, aniso 0.
2. Is the landscape render-state filter override (state 9 value 2 at 0x80047100) honoured over the texture's
   own filter by the RW GCN driver? Trace [r13+0x615C]+0x20 for state 9 and the next 0x8049AC88 bind.
3. Animation LOD negative check: count 0x80416EF4 calls per enemy object per frame on the Westopolis tape vs
   its distance / on-screen flag (expect exactly one per frame while spawned).
4. Who clears the enemy motion enable byte (component +0xC tested at 0x801A4D10): data breakpoint.
5. ARAM clip stalls: time 0x80422234 (ARQ busy-wait on [r13+0x5FA0]) and count 0x80414368 misses
   (refcount path 0x804143D4 vs copy path 0x804144C0) during play; peak motion pool usage (list
   [r13+0x5F9C]) against the 6 MiB bound at 0x804224B4.
6. setid.bin stage index: log r5/r30 of 0x800CA7F0 (call 0x800CBC94) per stage to map bit -> stage; list the
   factories with +0x1B bit 0 after it (= resident class archives per stage).
7. Main-heap high-water mark per stage: MSL pool at 0x805A6300 (free list) and OS arena hi/lo after stage load.
8. `_D_` vs `_N_`: verify at runtime that both are drawn in every frame their block is visible
   (0x800471BC layer 0 and 1) - i.e. no camera-distance switch exists anywhere else.

## 7. Open questions
* Meaning of the letters `N`, `D`, `OS`, `K`, `M` (names only; flags known).
* Sub-heap sizes (Sonicteam::System::Heap users via vtables 0x8051DB38 / 0x8053E944).
* Meaning of the motion pool parameters (0x80, 0xA00, 0x100, 0x1400).
* Whether any bank is registered without the ARAM conversion (the path through 0x80414274 always converts;
  other registration paths not traced).
