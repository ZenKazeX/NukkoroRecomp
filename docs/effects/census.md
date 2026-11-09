# Effects census / scout (2026-10-27)

Static analysis only (PAL main.dol GUPP8P). Evidence levels: **P** = PROVEN (address/disassembly/generated site list),
**S** = STRONGLY SUPPORTED, **M** = STRUCTURAL MATCH (SDK layout / naming), **U** = UNKNOWN.
Tools used: `tools/dolxref.py callers|xrefs|owner`, `tools/doldis.py`, `extract/functions.csv`,
`extract/functions-dolphin-sigs.csv`, `extract/gx-sites.txt`, `extract/lift-sites-r2.txt`, `effects/rtti_classes.tsv`,
plus a raw-word search over main.dol for vtable/PMF slots (scratch scripts, not kept in the tree).

## 0. Headline findings

1. **Game code (almost) never touches GX.** Every caller of a visual GX SDK function is GXInit (0x80394088), the RenderWare
   GameCube driver (0x80449xxx-0x804A7xxx), or the OS disc-error screen (0x8032D7CC-0x8032E10C). Exactly **two** game-side
   GX calls exist: `GXSetBlendMode(GX_BM_LOGIC, ONE, ONE, GX_LO_SET)` at **0x804382A4** and **0x804385D8**, both in the
   effect library's atomic-shimmer pass (P, section 1.3). No game code writes the GX FIFO either: every pipe store in
   `extract/gx-sites.txt` is in the SDK, the RW driver (PTank 0x8044A25C.., Im2D 0x80491xxx, raster/clear 0x80499xxx,
   Im3D 0x804A0B68) or the disc-error screen 0x8032E038 (P).
2. **No Z-textures, no fog range adjust, no custom line/point width, no dst-alpha, no TEV swap tables** in the whole game:
   `GXSetZTexture`, `GXSetFogRangeAdj`, `GXSetLineWidth`, `GXSetPointSize`, `GXSetDstAlpha`, `GXSetTevSwapMode(Table)`,
   `GXSetCoPlanar` are called only by GXInit, with the disable/default arguments (0x803946D8 ZTexture(0,0x11,0);
   0x8039481C FogRangeAdj(0,0,0); 0x8039429C/0x803942A8 LineWidth/PointSize(6,0); 0x8039486C DstAlpha(0,0)) (P).
3. **Indirect textures (GX distortion) are used only by two MatFX/driver programs**: MatFX BUMPMAP/BUMPENVMAP
   0x804A4E00 and the GCN "native shader" material 0x804A7334 (called only from 0x804A7AAC). Hence every on-screen
   distortion (heat haze, shimmer, refraction) is **geometry/UV-offset based over an EFB copy, not indirect TEV** (P).
4. **Only 59 functions in the DOL issue RW render states / Im2D / Im3D themselves** (`extract/lift-sites-r2.txt`, which lists
   every RwRenderStateSet/Get, Im2D and Im3D dispatch site). Every other game "effect" class therefore draws only through
   (a) RW object pipelines (atomic/clump/world: default, MatFX, skin, DMorph, UV anim) or (b) the PJS effect library.
   Section 3 classifies all 59 (P for the list, S/M for the per-owner identification).
5. New / un-assigned visual features found: the **level-recipe tag table** (15 `_X_` tags, decoded, section 4, incl. an
   **inverse-colour level `_M_`**: `src*INVDESTCOLOR + dst*ZERO`), **Test::EffBelt** (Im3D additive ribbon used by the
   player Belt effect), the font/text darken-then-add blend (0x8004A09C), RGBA6 camera pixel format (0x80495858), and
   the TFogSet task (vtable at 0x8052C84C, exec 0x8016A904). Section 5 lists RTTI effect classes and their route.
6. **Port gaps** (section 6): MatFX bump / native-shader not rendered; the atomic-shimmer logic-op SET and its three
   camera-texture copies are no-ops in the lifted build; stage_preview tool's level-tag recipes disagree with the
   game's table for `_M_`, `_AL_`, `_G_`, `_GL_`.

## 1. GX SDK census

SDK addresses: `extract/functions-dolphin-sigs.csv` + `runtime/src/gekko/rw_capture.cpp:29-65` + SDK source order for the
unnamed gaps (M). Caller sets from `dolxref.py callers` (P). "RW" = RenderWare GC driver, "Init" = GXInit 0x80394088,
"DiscErr" = OS error screen 0x8032D7CC/0x8032DE30/0x8032E10C.

### 1.1 TEV / indirect / texgen

| GX function | Addr | Callers (function entry) | Class |
|---|---|---|---|
| GXSetTevOp | 0x803992EC | Init; DiscErr 0x8032D7CC; PTank 0x80449D54; Im2D 0x8049148C; raster 0x80499004, 0x80499B14; Im3D 0x804A0B68; MatFX 0x804A3F74, 0x804A4358, 0x804A48DC, 0x804A4B7C, 0x804A55CC | RW |
| GXSetTevColorIn / AlphaIn / ColorOp / AlphaOp | 0x80399378 / 0x803993BC / 0x80399400 / 0x80399468 | default decision table 0x80466624; MatFX bump 0x804A4E00; native shader 0x804A7334 | RW |
| GXSetTevColor (C0/C1) | 0x803994D0 | 0x804662DC, 0x804663C0, 0x80466624 (material colour registers of the default pipeline) | RW |
| GXSetTevColorS10 | 0x80399530 (M) | 0x804A7334 only | RW (unused, see 6) |
| GXSetTevKColor | 0x80399594 | 0x804A4E00, 0x804A7334 | RW |
| GXSetTevKColorSel / KAlphaSel | 0x803995F8 / 0x80399654 | Init; 0x804A7334; (KAlpha also 0x804A4E00) | RW |
| GXSetTevSwapMode / SwapModeTable | 0x803996B0 / 0x803996F8 | Init only | - |
| GXSetTevOrder / NumTevStages | 0x80399848 / 0x803999E4 | same set as TevOp + 0x80466624, 0x804A4E00, 0x804A7334 | RW |
| GXSetTevIndirect | 0x80398E14 | GXSetTevDirect 0x8039924C (SDK), 0x804A4E00, 0x804A7334 | RW |
| GXSetIndTexMtx | 0x80398E80 (M) | 0x804A7334 only | RW |
| GXSetIndTexOrder | 0x8039913C | 0x804A7334 only | RW |
| GXSetIndTexCoordScale / NumIndStages | 0x80398FF8 / 0x80399228 | Init, 0x804A7334, 0x804A7AAC | RW |
| GXSetTexCoordGen2 | 0x80395D48 | DiscErr 0x8032E10C; Init; PTank 0x80449D54; 0x80466624; Im2D 0x8049148C; raster 0x80499004; Im3D 0x804A0B68; MatFX 0x804A3F74/4240/4358/48DC/4B7C/4E00/55CC; 0x804A7334 | RW |
| GXLoadTexMtxImm | 0x8039A300 | DiscErr 0x8032E10C; Init; MatFX 0x804A4358, 0x804A4B7C (env), 0x804A4E00 (bump), 0x804A55CC (UV xform), 0x804A7334 | RW |
| GXSetZTexture | 0x803997BC | **Init only** (disable) | - |

### 1.2 Pixel / blend / depth / fog / channels

| GX function | Addr | Callers | Class |
|---|---|---|---|
| GXSetBlendMode | 0x80399D44 | DiscErr; Init; **0x804381C4 site 0x804382A4, 0x80438570 site 0x804385D8 (game, LOGIC SET)**; FINALIZESTART 0x804981E8; RW blend states 0x80498910, 0x80498D38, 0x80498DE8; clear quad 0x80499F10 | RW + **2 game** |
| GXSetZMode | 0x80399DF0 | DiscErr; Init; 0x804981E8; ZTEST/ZWRITE states 0x80498C38, 0x80498CA4; clear quad 0x80499E58 | RW |
| GXSetZCompLoc | 0x80399E24 | Init; 0x804981E8; alpha-test mode 0x804987C8, 0x80498854 | RW |
| GXSetAlphaCompare | 0x80399778 | Init; 0x804981E8; 0x804987C8, 0x80498854, 0x80498E78, 0x80498F18, 0x80498F74 | RW (0x80498F74 / 0x804987C8 are also called directly by the game's level recipe 0x80046D80, lift-sites-r2 624-637) |
| GXSetColorUpdate / AlphaUpdate | 0x80399D98 / 0x80399DC4 | Init; 0x804981E8; colour also DiscErr, clear 0x80499E58, 0x80499F10 | RW |
| GXSetDstAlpha / Dither | 0x80399F58 / 0x80399F2C | Init only (DstAlpha off, Dither on) | - |
| GXSetPixelFmt | 0x80399E58 | Init; 0x80495214; CameraBeginUpdate 0x804956F4 (sites 0x80495860 RGBA6_Z24 when the camera raster ext bit0 set, 0x80495884 RGB8_Z24); raster 0x80499B14 | RW |
| GXSetFog | 0x80399A0C | Init; RW fog states 0x804986B8, 0x80498990, 0x80498A80, 0x80498AF8 | RW |
| GXSetFogRangeAdj | 0x80399C20 | **Init only** (disabled) | - |
| GXSetChanCtrl / NumChans | 0x80397CBC / 0x80397C80 | DiscErr; Init; PTank; 0x80466624; Im2D; FINALIZESTART; raster; Im3D; MatFX | RW |
| GXSetChanAmbColor / MatColor | 0x80397AB0 / 0x80397B98 | 0x80466448, 0x804664D0, 0x8046656C, 0x8046659C, 0x80466624, MatFX, PTank | RW |
| GXLoadLightObjImm | 0x80397A34 | DiscErr; light upload 0x8046521C, 0x80465438 | RW |
| GXSetCullMode | 0x80396B54 | DiscErr; Init; 0x804981E8; CULLMODE state 0x80498E50; raster 0x80499394 | RW |
| GXSetLineWidth / PointSize / CoPlanar | 0x80396A8C / 0x80396ACC / 0x80396B7C | **Init only** (width 6 = 1 px) | - |
| GXSetScissor | 0x8039A4E4 | Init; Im2D 0x80491774; camera clear 0x80495214; CameraBeginUpdate 0x804956F4; raster 0x80499004, 0x80499B14 | RW |

### 1.3 The two game-side GX calls (P)

0x804381C4 (inside the atomic shimmer pass B, `Effect::PJS::EffectDrawShimmerTask` family; neighbouring copy sites
0x80438370/0x80438460) and 0x80438570: `if (task+0x78 == 0) GXSetBlendMode(3 LOGIC, 1, 1, 0xF SET)` else
`RwRenderStateSet(SRCBLEND, ZERO/ONE)` + `DESTBLEND ONE/ONE` (0x80438244-0x804382A4, 0x80438578-0x804385D8). LOGIC SET writes
all-ones (white, alpha 255) for every covered pixel: the shimmer's mask/silhouette pass (CULLMODE CULLFRONT is set just before,
lift-sites 0x80438240). Owner deep-dive belongs to family (a).

## 2. EFB -> texture copies (P)

`GXCopyTex` 0x803975AC has one caller, 0x80496380 (tail of `CameraTextureCopy` 0x80496208, which alone calls
GXSetTexCopySrc 0x80396C50 / GXSetTexCopyDst 0x80396D00). `CameraTextureCopy` has exactly **17** call sites
(`dolxref callers 0x80496208`), all in the effect library 0x8042E8xx-0x80439xxx; no game class copies the EFB any other way.
`GXCopyDisp` 0x80397444: ShowRaster 0x80495CBC (the present, lifted to `render_submit_frame` at 0x80495DFC) and DiscErr.

| Site | Owner | What / why | Port (lift-sites-r2 line) |
|---|---|---|---|
| 0x8042E8C0 | SaveScreen 0x8042E838 | copy #10, view history for DrawBlur | `rw_trail_save` (native) |
| 0x8042E968 | SaveScreen | second copy | `rw_camera_texture_copy` = counter only (immediate.cpp:396) |
| 0x8042F41C, 0x8042F630, 0x8042F7E8, 0x8042F9C4, 0x8042FF40 | glare ladder 0x8042F258 | glare downsample chain | counter only; glare is native (glare.cpp) |
| 0x804305A8, 0x80430810 | glare blur pair 0x804302EC | glare blur | counter only; native glare |
| 0x80430B58 | PostGlare 0x80430904 | copy #2 glare-source image | `rw_glare_composite` |
| 0x80430FAC | PreGlare 0x80430D64 | copy #1 scene before glare sources | `rw_glare_sources_begin` |
| 0x80435FA4 | refraction element 0x8043531C | copy #13 scene snapshot | `rw_screen_snapshot` + `rw_refraction_draw` 0x804360D4 |
| 0x80436EEC | full-screen heat haze 0x804369CC | copy #12 snapshot | `rw_screen_snapshot` + `rw_shimmer_draw` 0x80437040 |
| 0x80438370, 0x80438460 | atomic shimmer pass B 0x804382F8 | per-object shimmer background | **counter only** |
| 0x80438B18 | atomic shimmer 0x8043873C | per-object shimmer | **counter only** |
| 0x80439124 | ScreenColor 0x80438C90 | screen multiply | counter; the multiply is `rw_screen_multiply` 0x804392DC |

No other render-to-texture exists (no Z copies, no texture-copy users outside the effect library).

## 3. The 59 self-drawing functions (P list; identification S/M)

Source: owner column of every `rw_render_state_set`, `rw_im2d_*`, `rw_pipeline_execute_im3d_*` site in
`extract/lift-sites-r2.txt`, plus the Im3D callers (`callers 0x8049B87C/0x8049BB48/0x8049B9AC`: 0x80039978, 0x80153938,
0x802DE520, 0x802DF18C, 0x8042D8DC, 0x80433F40, 0x8043B688, 0x80471CA4, 0x80471EB8, 0x80477E8C). Owners resolved through
RTTI methods or by locating the PMF/vtable word in the DOL.

| Owner | Identified as | States / prims | Family |
|---|---|---|---|
| 0x80036570 | movie player frame blit | `rw_movie_frame` 0x80036764 | 2D |
| 0x800396F8 | generic textured 2D quad helper (callers 0x802E51B0, 0x80306160, 0x8030657C, TPjsEventActionFade draw 0x80329CA4) | VERTEXALPHA, TRISTRIP | 2D / (a) fade |
| 0x80039948 | SonicteamUSA::System::DebugDispLine (via 0x802E2858) | Im3D | debug |
| 0x800448C8 | SonicteamUSA::System::Fade::FadeDrawTask (0x80044E08) | Im2D | (a) |
| 0x80046D80 | **render-level recipe latch** (section 4) | blend/z/alpha/filter/cull/fog | (a) + this file |
| 0x8004A09C | font glyph draw (called via 0x80049BE8 by MessageTask, ResultTask, menus; fonts code 0x8004A6AC "./fonts/") | `SRCBLEND ZERO, DESTBLEND INVSRCCOLOR` then `ONE/ONE` | 2D, new (5.2) |
| 0x800BCBA4 | ResultTask / result-screen 2D (0x80202F24, 0x80201620 TGiRingChallenge) | untextured + textured TRISTRIP | 2D |
| 0x8015390C | CharaColliBase debug volume (PMF at 0x8052B910, M) | additive untextured Im3D, fog off | debug |
| 0x8016A904 | **TFogSet** exec (vtable 0x8052C84C, RTTI name 0x805F5C38; loads `stg%04d/stg%04d.fog` in 0x8016A5E4) | FOGCOLOR, FOGTYPE get/set | (c) fog |
| 0x802662A4 | ElecFanDraw render functor (bind word 0x8054D2F4; frustum test 0x80486718 then clump render 0x80037BAC) | CULLNONE / CULLBACK around the model | model |
| 0x802BE9D4 | CNowLoadingThread slot 2 (0x80558AC4) | untextured overlay | 2D |
| 0x802DE4BC | **Test::EffBelt** draw (exec 0x802DE244) | additive SRCALPHA/ONE, untextured, cull none, fog off; Im3D 22-vtx TRISTRIP | new (5.1) |
| 0x802DF140 | Test::EffLocusMultiVtx (weapon katana/stick trails) | Im3D | (d) |
| 0x802E51B0 | SaveCycle::MessageDrawTask window | Im2D incl. 2x POLYLINE (0x802E5444, 0x802E54A0) | 2D |
| 0x80306354, 0x80306D80 | 2D screen helpers next to Effect::PJS::Launcher / AdvReal::MemoryCheck (M) | Im2D | 2D |
| 0x8034359C .. 0x80346908 | AdvReal::StaffRoll::StaffRollDrawTask (0x80343268) and staff-roll 2D | SRCALPHA/INVSRCALPHA, LINEAR | 2D |
| 0x80363CC8 .. 0x80364614 (8 fns) | C function tables at 0x80560D14 / 0x80560DAC (no RTTI) - a 2D platform draw interface (M; likely CSD) | blend/z/cull/address, TRISTRIP | 2D |
| 0x8042D870 | Effect::System::EffectRay (RTTI slot 2) | Im3D | (d) |
| 0x8042E254, 0x8042E838 | DrawBlur, SaveScreen | | (a) |
| 0x8042F258, 0x804302EC, 0x80430904 | glare ladder / blur / PostGlare | | done (GLARE_DISSECTION) |
| 0x80431E38 | lens flare | Im2D projected | (d) |
| 0x80433DA8, 0x80434824 | EffectParticle3D / EffectParticle | Im3D | (b) |
| 0x8043531C | refraction element | snapshot + patch | (a) - see 7 |
| 0x804369CC | full-screen heat haze | | (a) |
| 0x80438168, 0x804382F8, 0x8043873C | atomic shimmer (mask CULLFRONT, LOGIC SET, FLAT shading) | | (a) - see 6 |
| 0x80438C90 | ScreenColor (`SRCBLEND INVDESTCOLOR` at 0x804394AC) | | (a) |
| 0x8043B618 | EffectLocus | Im3D | (d) |
| 0x80449A74 | PTank render | | (b) |
| 0x80471CA4, 0x80477D48 | UTF-8 text system (0x80472BCC/0x804735CC; reached through 0x80473AB0/0x80473CA0/0x80474878) | Im3D indexed glyphs | 2D |
| 0x80479750 | text/charset Im2D (via 0x80479B18 <- 0x80051540) | NEAREST, SRCALPHA/INVSRCALPHA, TRILIST | 2D |
| 0x804A3F74, 0x804A4358, 0x804A48DC, 0x804A7334, 0x804A7A58 | MatFX DUAL, DUALUV, ENVMAP, native shader blend states | | (c) |
| 0x800620D4 (native owner, not in list) | Weapon::LockonMark draw | | native (game_owners.cpp) |

**Consequence (P):** ChaosControl, ChaosBlast, HealingBlast, Player::Barrier, BAShield, DefenseShield, ElecBarrier,
EnemyShieldTask, BlackDoom aura/eye, DevilDoom cloud, HeavyDog destroy/energy/canopy, Boss::Diablon::Effect,
Vehicle::Effect, WarpHole, Meteors, BossCtrlUnit_DamageEff, Player::Shadow::Electric, PlayerBarrierHeatEffect, all
PlayerEffect_cpp classes except Belt, etc. issue **no** render state, Im2D, Im3D, GX call or EFB copy of their own. Their
visuals are models through the RW pipelines (material colour/alpha is re-read per draw: pipelines.cpp:757) and/or PJS
effects. Their port verdicts reduce to families (b) and (c).

## 4. Model-name tag classifier fn_8004652C and the level recipe 0x80046D80 (P unless marked)

fn_8004652C (sole caller 0x800469A0) walks the 15-entry table at **0x8051E328** (stride 0x18: `{tag*, taskName*, flags,
level, 0, 0}`), finds each tagged object in the loaded landscape (0x8004C878), parses an optional `_TEXnn` suffix
(string 0x805F2CBC = r2-0x7AC4; digits at 0x800465C8-0x800465E4) to pick texture dictionary nn (1..9) from table 0x805742E0
(else the stage default at game+0x1804) via **RwTexDictionarySetCurrent 0x8048E114**, then creates task
`LandDispManager_<tag>` (0x8004B318, callback 0x80046438). The same table is read by 45 other functions (enemies, bosses,
player: `xrefs 0x8051E328`), i.e. it is the game-wide render-class table. The flags word drives 0x80046D80:

| flag bit | test site | effect |
|---|---|---|
| 0x001 | 0x80046DA0 | geometry flags |= 0x30 (LIGHT|NORMALS) -> dynamically lit ("L" tags) |
| 0x004 | 0x8004716C | FOGENABLE = game fog flag (game+0x1809), else fog off |
| 0x008 | 0x800470F4 | TEXTUREFILTER LINEAR, else NAFILTER |
| 0x010 | 0x80047130 | CULLNONE, else CULLBACK |
| 0x040 | 0x80046DD8 | SRCALPHA/INVSRCALPHA, ztest on, zwrite off, alpha compare always |
| 0x080 | 0x80046E64 | SRCALPHA/ONE (additive), ztest on, zwrite off |
| 0x100 | 0x80046EF0 | **SRCBLEND INVDESTCOLOR, DESTBLEND ZERO** (out = src*(1-dst)), z states untouched |
| 0x800 | 0x80046F4C | punch: SRCALPHA/INVSRCALPHA, zwrite+ztest on, alpha ref **200** (0x80046FC0) |
| 0x400 | 0x80046FE0 | blend with ztest OFF (not used by the table) |
| none of 0x40/0x80/0x100/0x800/0x400 | 0x8004706C | opaque: zwrite+ztest on, alpha ref = byte 0x805E431D (0x800470DC) |
| 0x020, 0x200 | not tested here | U (used elsewhere) |

| Tag | flags | level | Meaning (from flags) |
|---|---|---|---|
| `_N_` | 0x28 | 1 | opaque, culled, **no fog** (far scenery per tools/stage_preview) |
| `_D_` | 0x28 | 1 | same as _N_ (distant) |
| `_OS_` | 0x2C | 1 | opaque, culled, fogged (identical flags to _O_; 'S' difference U) |
| `_OSL_` | 0x2D | 1 | _OS_ + lit |
| `_O_` | 0x2C | 1 | opaque, culled, fogged |
| `_OW_` | 0x3C | 1 | opaque, **double-sided** |
| `_OL_` | 0x2D | 1 | opaque + lit |
| `_P_` | 0x80C | 1 | punch-through ref 200, culled, fogged |
| `_AF_`, `_A_` | 0x5C | 5 | alpha blend, double-sided, fogged |
| `_AL_` | 0x4D | 5 | alpha blend, **culled**, lit |
| `_K_` | 0x9C | 7 | additive, double-sided, fogged |
| `_M_` | 0x11C | 7 | **inverse-colour blend** (src*(1-dst)), double-sided, fogged |
| `_G_` | 0x98 | 9 | additive, double-sided, **no fog** |
| `_GL_` | 0x99 | 9 | `_G_` + lit |

Level 1/5/7/9 matches the RenderLevel string order at 0x804D22E8 (0 PreRender, 1 OpeqWorld, ... 5 TransWorld, 7 AddWorld,
9 GlareWorld) - M; note `_P_` goes to OpeqWorld, not PunchWorld. Port: 0x80046D80 stays translated and its states reach
the native recipe (render_states.cpp:90-108 accepts src INVDESTCOLOR=10 and dst INVSRCCOLOR; renderer.cpp:97/103 and
render_vk/pipelines.cpp:182 map the factors) -> **EXACT path** (S). The punch ref 200 and alpha compares go through
`rw_alpha_compare` / `rw_alpha_test_mode` (lift-sites 624-637).

## 5. New effects in this family

### 5.1 Test::EffBelt (player "Belt" ribbon) - S
- RTTI Test::EffBelt, vtable 0x80559FFC {dtor 0x802DE29C, exec 0x802DE244}; ctors 0x802DE374 (caller 0x8029DDB8, U owner)
  and 0x802DE414 (caller 0x800A5E40 = PlayerEffect_cpp::Belt, exec 0x800A5D14 -> 0x802DE220).
- Draw 0x802DE4BC: SRCBLEND SRCALPHA, DESTBLEND ONE, CULLNONE, TEXTURERASTER NULL, VERTEXALPHA on, FOGENABLE off
  (0x802DE584-0x802DE600); two 11-point edges built from 0x8040C278/0x8040D198/0x8040C6CC; `RwIm3DTransform(v, 22, 0,
  0x18 XYZ|RGBA)` 0x802DE7AC, `RwIm3DRenderPrimitive(TRISTRIP)`; states restored after.
- Port: logic translated; Im3D reaches the native Im3D path (immediate.cpp:313, `rw_pipeline_execute_im3d_*`). Verdict
  **EXACT (S)** for blending; width/AA follow the generic Im3D path. Flaw: untextured vertex-colour ribbon, hard edges
  (aliasing) - proposal: fade alpha across the ribbon width in a native version.

### 5.2 Font glyph darken-then-add (0x8004A09C) - S
- Per glyph: pass 1 `ZERO / INVSRCCOLOR` (dst *= 1-src colour: dark outline/shadow), pass 2 `ONE / ONE` (add the glyph).
  Used by MessageTask, ResultTask, menus (via 0x80049BE8).
- Port: render_states.cpp:100-107 accepts dst INVSRCCOLOR (4); src ZERO accepted -> **EXACT path (S)**. Flaw: two additive
  passes saturate at 8 bit on bright backgrounds (original behaviour).

### 5.3 Level `_M_` inverse-colour class - P (recipe), U (which assets use it)
See section 4. out = src * (1 - dst): bright source inverts the scene behind it. Port path EXACT (S); tool deviation in 6.

### 5.4 RGBA6 camera pixel format - S
CameraBeginUpdate 0x804956F4 switches the EFB to GX_PF_RGBA6_Z24 when the camera raster extension bit0 is set
(0x80495844-0x80495860), else RGB8_Z24 (0x80495884); cached in r13+0x6190. Cameras that need destination alpha lose 2
bits per channel. Port renders >= 8 bit per channel: **DELIBERATE DEVIATION (beneficial)**; anything thresholding
destination alpha may differ by up to 3 LSB of 8 bit.

### 5.5 TFogSet task - P (identity)
Vtable 0x8052C84C (RTTI 0x805E77E4 -> name "TFogSet"), exec 0x8016A904: reads FOGCOLOR (0x8016A9B4), sets FOGCOLOR/FOGTYPE
(0x8016AC34/0x8016AC4C). The .fog files (one per stage, strings 0x804E4F20..0x804EBE74). Port: render_states.cpp:131-145
re-arms fog. Deep-dive belongs to (c).

### 5.6 Others found and routed
| Item | Evidence | Route / note |
|---|---|---|
| RenderLevel_Sprite3D (0x804D23F0) / Sprite3D functor 0x802B4A8C | lift-sites: `world_to_screen` 0x802B4B20 | native boundary projection (projection.cpp) |
| RenderLevel_Gindows (0x804D2444), Sonicteam::Gindows (VariableEditor, GINDOWS.PNG 0x804E3C5C) | RTTI | in-house debug GUI; no draw owner in section 3 beyond the 2D tables (U if reachable in retail) |
| TPjsEventActionFade / Scene (cutscene) | draw functor 0x80329CA4 (word at 0x8055DF90 just before its vtable 0x8055DF94) -> 0x800396F8 Im2D quad | (a) Fade |
| ElecFanDraw | 0x802662A4 | model with cull toggle; RW pipelines |
| VacuumRing::Ring, SetItem::ItemBox, VehicleArrowImpl, StageRouteDisplay, TGi* HUD | RednerDrawArgs functors (rtti_classes.tsv) | no own states -> models / CSD 2D |
| Disc-error screen 0x8032D7CC..0x8032E10C | direct GX (TevOp, ChanCtrl, CopyDisp...) | `DrawProducer::DiscError` (rw_capture.cpp:126) |
| MatFX "native shader" plugin 0x129 (0x804A7AAC -> 0x804A7334) | only user of IndTexMtx/TevColorS10/KColorSel | rw_capture.cpp:156 "no asset carries it" |

## 5b. RTTI effect classes (entry points; dtor exec from rtti_classes.tsv)

None of these is a section-3 owner, so they draw via models/PJS (P by elimination); their visuals are families (b)/(c).

| Class | dtor / exec | | Class | dtor / exec |
|---|---|---|---|---|
| PlayerEffect_cpp::Awake | 0x800A50C4 / 0x800A4D2C (exec reaches 0x800A4C04, a render-table reader) | | PlayerEffect_cpp::Footmark | 0x800A4664 / 0x800A4324 (calls 0x80426E9C / 0x80426DE4, PJS object setters) |
| PlayerEffect_cpp::Jet | 0x800A5AA4 / 0x800A5924 | | PlayerEffect_cpp::Slow | 0x800A492C / 0x800A4830 |
| PlayerEffect_cpp::Grind | 0x800A55D8 / 0x800A55D4 (empty) | | PlayerEffect_cpp::RangeEffect | 0x800A40A8 / 0x800A3F6C |
| PlayerEffect_cpp::CameraDemo | 0x800A7394 / 0x800A72BC | | PlayerEffect_cpp::Belt | 0x800A5D70 / 0x800A5D14 -> EffBelt |
| ChaosBlast / ChaosControl | 0x8007AD94/0x8007AD90, 0x8007BA94/0x8007BA90 (trivial) | | Player::Barrier | 0x8033EB34 / 0x8033EB10 |
| Player::Shadow::Electric | 0x8028DCDC / 0x8028DC98 | | BlackDoomCtrlUnit_DispAura / DispEye | 0x8027F63C/0x8027F5A8, 0x8027FA84/0x8027F9BC |
| DevilDoomCtrlUnit_Cloud | 0x8030804C / 0x80307FCC | | HeavyDogCtrlUnit_DestroyEffect / DispEnergy / DispCanopy | 0x80249C84/0x80249C30, 0x8024A3E4/0x80249F64, 0x8024A71C/0x8024A660 |
| BossCtrlUnit_DamageEff | 0x802928C0 / 0x802928B8 | | EnemyShieldTask | 0x801A7CEC / 0x801A7C9C |
| Vehicle::Effect | 0x803500B0 / 0x8035008C | | WarpHole::ManagerTask | 0x80129F38 / 0x80129A28 |
| MeteorsTask | 0x8021E650 / 0x8021E624 | | BAShield::BAShield | 6 methods 0x80302008.. |
| TTextureChange | 0x8004F420 / 0x8004F3C4 | | BossRender / RenderStatic / RenderDiv | 0x8017AB00/0x8017AAD0, 0x801E4A58/0x801E4E40, 0x801E5178/0x801E4EF0 |
| StageAction::FlashbackAction | 0x803549FC / 0x8013D760 | | Boss::Diablon::Effect, HealingBlast, DefenseShield, ElecBarrier, RainEffect | no methods parsed in tsv (U) |

## 6. NOT RENDERED / port gaps

| # | Feature | Evidence | Verdict |
|---|---|---|---|
| 1 | MatFX BUMPMAP / BUMPENVMAP (0x804A4E00, indirect TEV + KColor) | pipelines.cpp:535-579 handles only Environment / Dual / UvTransform; rw_capture.cpp:150 marks it `MatFxUnsupported` | **NOT RENDERED** (base only). Asset use U -> (c) |
| 2 | GCN native-shader material 0x804A7AAC/0x804A7334 | rw_capture.cpp:156 | NOT RENDERED; no asset carries it (claimed there) |
| 3 | Atomic-shimmer GX LOGIC SET (0x804382A4, 0x804385D8) | translated GXSetBlendMode only writes the pipe ("dead" family, gx-sites.txt 0x80399D44); D3D12 pipelines.cpp:60 LogicOp NOOP; native recipe uses the RW-cached blend | **probable DEFECT**: the mask pass draws with the last RW blend instead of solid white -> (a) |
| 4 | Atomic-shimmer / ScreenColor / SaveScreen#2 camera copies | immediate.cpp:396-399 counts only | shimmer: raster never filled natively -> **probable DEFECT** for (a); ScreenColor/SaveScreen covered by native passes |
| 5 | GX copy filter / deflicker on display copy (GXSetCopyFilter 0x80397228 from 0x80495214) | memory note "copy filter dropped" | DELIBERATE DEVIATION |
| 6 | RGBA6 EFB precision | 5.4 | DELIBERATE DEVIATION |
| 7 | GXSetDither(1) | Init 0x80394860 | DELIBERATE (no dither at 8 bit) |
| 8 | tools/stage_preview/stage_scene.cpp:28-60 level recipes | vs table 0x8051E328: `_M_` drawn additive (game: INVDESTCOLOR/ZERO); `_AL_` cull None (game: CULLBACK, 0x10 clear); `_G_`/`_GL_` "Glare" = default opaque (game: additive, no fog, no cull, level 9) | **DEFECT (tool only)**, runtime unaffected |
| 9 | Z textures, fog range adjust, line/point width, TEV swap | Init-only (section 0.2) | nothing to render (P) |

## 7. Cross-check notes for the other families

- (a) the refraction element 0x8043531C (copy 0x80435FA4, patch 0x804360D4) is a separate effect from the heat haze
  0x804369CC; make sure it is covered. The atomic shimmer (0x80438168/0x804382F8/0x8043873C) is the only GX logic-op user.
- (a) the level-recipe `_M_` inverse blend and ScreenColor's INVDESTCOLOR are the only "negative" blends in the game.
- (b) PJS object setters 0x80426E9C (54 call sites) / 0x80426DE4 (17) give the list of game classes that place PJS effects.
- (c) only bump/native-shader use indirect textures; env map texgen is in 0x804A4B7C with GXLoadTexMtxImm.
- (d) EffLocusMultiVtx is used by weapon katana/sticks (ctor caller 0x802DF098 <- 0x802763D0 WeaponStickBase slot 10).

## 8. Open questions (need runtime capture / Dolphin)

1. Which stages/models carry `_M_` (inverse) and `_G_`/`_GL_` tags, and do any materials use MatFX BUMPMAP? (asset scan)
2. Atomic shimmer in the lifted build: what does the mask pass look like with LOGIC SET dropped? (Dolphin vs port capture)
3. Meaning of flag bits 0x020 / 0x200 and of the `_OS_` vs `_O_` distinction (same recipe flags).
4. Owner of the second EffBelt ctor caller 0x8029DDB8.
5. Is RenderLevel_Gindows reachable in retail builds?
