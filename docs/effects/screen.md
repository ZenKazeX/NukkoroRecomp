# Screen-space / frame-level effects and the render order (PAL GUPP8P, static analysis 2026-10-27)

All addresses are PAL DOL (tools/doldis.py). Evidence: **P** = PROVEN (read in the disassembly), **S** = STRONGLY
SUPPORTED, **M** = STRUCTURAL MATCH, **U** = UNKNOWN. RW render-state ids as used by the game through
`[0x805F265C]` (+0x20 set, +0x24 get, +0x30 Im2DRenderPrimitive): 1 TEXTURERASTER, 6 ZTEST, 8 ZWRITE, 9 FILTER
(1 NEAREST, 2 LINEAR), 0xA SRCBLEND, 0xB DESTBLEND (1 ZERO, 2 ONE, 5 SRCALPHA, 6 INVSRCALPHA, 0xA INVDESTCOLOR),
0xC VERTEXALPHA, 0xE FOG, 0x14 CULL. Primitive 4 = TRISTRIP. `0x8048652c`/`0x80486504` = camera Begin/EndUpdate
(returns NULL on failure, the glare task's pattern at 0x80027688 proves it), `0x80496208` = CameraTextureCopy
(frame buffer -> raster at the raster's own offset/size), `0x8048aec4` = RwRasterCreate, `0x8048ae14` = SubRaster,
`0x8048ab7c` = GetOffset. Effect manager EffectManagerPJS = static object 0x8057798C (pointer r13+0x2DD8 =
0x805EF2D8, getter 0x800A35D8). Its screen-effect slots: **+0x3C glare, +0x40 blur (B), +0x44 screen colour (C),
+0x48 full-screen shimmer (H)**, +0x34 the shared full-screen copy texture.

Port status for the whole family: every effect's LOGIC is still translated (no entry in extract/native-owners*.txt);
only the draws and screen copies are lowered at call sites (extract/lift-sites-r2.txt) into
runtime/scaffold/rw_driver/screens.cpp + immediate.cpp, which hand them to the native renderer
(runtime/lift/render/renderer.cpp). The render-level tree itself is translated; the native renderer just records
passes in the order the translated tree submits them.

---------------------------------------------------------------------------------------------------------------------

## 1. The frame's render order

### 1.1 Task-tree mechanics (P)
- Task ctor `0x8004F014(task, parent)`: parent at +0x10, first child at parent+0x14, sibling `next` +0x0C, the first
  child's +0x08 points at the last; a new child is **appended** (0x8004F094..0x8004F0B4) -> children run in creation
  order. Runner `0x8004ECAC` walks first child -> next, depth first (S).
- Top-level phases (`0x8001C4F8` root, slot n = `0x801EB614(root,n)`, names table 0x8053ED20): 0 System, 1 Audio,
  2 Debug, 3 Scene (children 6..18: Manager, Controller, Command, CharaColli, Landscape, Gadget, Vehicle, Player,
  Enemy, PostManager, **16 Particle**, Editor, Camera), 4 PostSystem, **5 Render**.

### 1.2 The render-level table (P)
Built by `0x801E4B0C` (the RenderLevel singleton at 0x80574384, getter `0x800461A0`); table at 0x804D24EC, 23 entries
x 0x14 bytes {name, has-CameraRender byte, render mask, 2D-camera mode, parent root}. Level node k is at
singleton+4k. Four roots under phase 5 (Render), appended in this order:

| root (singleton off) | class / exec | names | children |
|---|---|---|---|
| +0x5C RenderAttach_Start | CameraStart vt 0x8053EAA8, exec 0x801E5100 | | 0 PreRender (RenderStart 0x801D03C0: clear) |
| +0x60 RenderAttach_Div | RenderDiv vt 0x8053EA70, exec 0x801E4EF0 | | levels 1..13, **run once per player viewport**: split mode [0x8057E760+0x60] 0 = one view; 1 = views 1 then 2 (horizontal); 2 = views 3 then 4 (vertical); the first viewport's children are run explicitly by `0x8004ECAC` at 0x801E4FA0 / 0x801E501C |
| +0x64 RenderAttach_Static | RenderStatic vt 0x8053EA54, exec 0x801E4E40 | | **PostEffect (created first)**, then 14 Sprite, 15 OnFade, 16 Gindows, 17 SpriteLast; full-screen viewport (mode 0) |
| +0x68 RenderAttach_End | CameraEnd vt 0x8053EA8C, exec 0x801E50B4 | | 18 PostRender (RenderStop 0x801D02DC) |

Per level (P, 0x801E4CE4..0x801E4DA8): a level with the flag byte gets `CameraRender(mask)` (vt 0x8053D584, exec
0x801D04D4 = camera BeginUpdate + draw the object lists with that mask via 0x80046D80) and the NEXT level gets
`CameraRenderClose` (exec 0x801D0418 = EndUpdate). Sprite gets CameraMatrix Set2D (0x801D01D8, mode 1), SpriteLast
Reset2D (mode 2). Sprite's children SpriteBack/Middle/Front (19..21) are created after those, so they run inside the
2D bracket.

| # | level | mask | parent | what runs there (creation order) |
|---|---|---|---|---|
| 0 | PreRender | - | Start | RenderStart; EffectDeleteResourceTask (0x801D1C30) |
| 1 | OpeqWorld | - | Div | LandDispManager_N, D, OS, OSL, O, OW, OL, P (table 0x8051E328, field +0xC = level; created 0x800460C0); **FullScreenShimmerDraw** is a child of LandDispManager_N (0x801785D4 reads [0x8051E33C]) |
| 2 | Opeq | 0x20 | Div | CameraRender(0x20); EffectDrawOpaq (bucket 0) |
| 3 | PunchWorld | - | Div | close of 2 |
| 4 | Punch | 0x800 | Div | CameraRender; EffectDrawPunch (bucket 1) |
| 5 | TransWorld | - | Div | close of 4; LandDisp AF, A, AL; event actions (0x8032A1D0) |
| 6 | Trans | 0x40 | Div | CameraRender; EffectDrawTrans (bucket 2) |
| 7 | AddWorld | - | Div | close of 6; LandDisp K, M |
| 8 | Add | 0x80 | Div | CameraRender; EffectDrawAdd (bucket 3), EffectDrawSub (bucket 4) |
| 9 | GlareWorld | - | Div | close of 8; LandDisp G, GL; **PreGlare** (0x801D21A0), DrawGlobalLensFlare (0x801D1F50), EffectDrawGlare (bucket 5, own glare-camera bracket) |
| 10 | Glare | 0x80 | Div | CameraRender (glare-flagged objects); ElecSearchLight etc. |
| 11 | PostGlare | - | Div | close of 10; **PostGlare** composite (0x801D213C), **DrawBlur** (0x801D2058), **EffectDrawShimmer** (bucket 6, refraction elements) |
| 12 | Sprite3D | 0x20 | Div | CameraRender |
| 13 | Last | - | Div | close of 12 |
| 22 | **PostEffect** | - | Static | **SaveScreen** (created inside DrawBlur's ctor 0x801D20B4), **DrawScreenColor** (0x801D1FF4) |
| 14 | Sprite | 0x418 | Static | Set2D, CameraRender; HUD (TGiPause, TGiHdGage, NpcNameDisplay, ...); 19 SpriteBack, 20 SpriteMiddle, 21 SpriteFront |
| 15 | OnFade | 0x418 | Static | close of 14 + CameraRender; **FadeDrawTask** (0x801E2884 reads +0x3C), StageTitleTask (loading screen) |
| 16 | Gindows | 0x418 | Static | close of 15 + CameraRender; windows (0x802D1790, 0x802E5F4C, ...) |
| 17 | SpriteLast | - | Static | close of 16, Reset2D |
| 18 | PostRender | - | End | RenderStop |

Not render-level tasks: FadeTask (update) in phase 0 System; FullScreenShimmerDrawSub (grid animation) in phase
16 Particle (0x801D22D4: `0x801EB614(root, 0x10)`).

Consequences (P): the trail, the screen colour and the inversion apply to the 3D picture of all viewports and never
to the HUD/fade (PostEffect precedes Sprite). DrawBlur (PostGlare, per viewport) runs BEFORE SaveScreen (PostEffect,
full screen) in a frame, so DrawBlur always reads the previous frame's save. The full-screen heat haze copies the
screen right after the opaque landscape layer N (before objects, other land layers, transparents, glare).

### 1.3 The effect draw buckets (P)
EffectManagerPJS ctor 0x801D1A40..0x801D1C10 creates the tasks. Each EffectDraw*Task exec = `fn_804261C0(mgr, bucket)`:

| task (RTTI) | vtable | m = dtor exec | exec -> | bucket | level |
|---|---|---|---|---|---|
| EffectDrawOpaqTask | 0x8053DCDC | 801D2ABC 801D2B14 | 0x8002774C | 0 | Opeq |
| EffectDrawPunchTask | 0x8053DCF8 | 801D2B38 801D2B90 | 0x80027728 | 1 | Punch |
| EffectDrawTransTask | 0x8053DD14 | 801D2BB4 801D2C0C | 0x80027704 | 2 | Trans |
| EffectDrawAddTask | 0x8053DD30 | 801D2C30 801D2C88 | 0x800276E0 | 3 | Add |
| EffectDrawSubTask | 0x8053DD4C | 801D2CAC 801D2D04 | 0x800276BC | 4 | Add (after Add) |
| EffectDrawGlareTask | 0x8053DD68 | 801D2D28 801D2D80 | 0x80027640: EffectSystemUSA cast (0x803A1AFC), camera vt+0x24, BeginUpdate, bucket 5, EndUpdate | 5 | GlareWorld |
| EffectDrawShimmerTask | 0x8053DD84 | 801D2DA4 801D2DFC | 0x8002761C | 6 | PostGlare |

`fn_804261C0` (P): iterates the manager's single element list (iterator over mgr+0x14, next via node+0x3C); draws
an element when `node+0x30 == bucket` by calling vtable+0x10 (the element's 3rd method, e.g. EffectShimmerPJS
0x8043531C). When mgr+0x24 is set it brackets the call with a re-entrancy bit (element+0x18 bit 3).

Port: no native owner; the brackets are recognised by frames.cpp (frames.cpp:93-97 describes RenderStart /
RenderDiv / RenderStatic exactly as above). Verdict **EXACT (M)** by construction: the native frame is recorded in
the translated tree's order.

---------------------------------------------------------------------------------------------------------------------

## 2. DrawBlur + SaveScreen: the feedback trail (B = mgr+0x40, 0x13C bytes)

| item | value / address | ev |
|---|---|---|
| tasks | EffectDrawBlurTask vt 0x8053DC6C (dtor 0x801D2788, exec 0x801D27FC -> 0x8042F16C -> **0x8042E254**); EffectSaveScreenTask vt 0x8053DC50 (dtor 0x801D270C, exec 0x801D2764 -> 0x8042F140 -> **0x8042E838**) | P |
| ctor | 0x8042F20C alloc 0x13C -> init 0x8042EA50(B, mgr, 0) | P |
| B+0x00 | enable (u8) | P |
| B+0x01 | alpha (u8): the trail's weight; vertex alpha of the quad | P |
| B+0x04 | zoom (f32), default 1.0 (0x8051C38C) | P |
| B+0x08, +0x0C | carried by get/set but not read by the draws | P |
| B+0x10 | mgr (the shared copy texture is [[mgr+0x34]]) | P |
| B+0x14.. | DrawBlur quad, 4 x 0x18 {x,y,z,rgba,u,v}; z = Im2D near-screen z ([0x805F265C]+0x18) | P |
| B+0x74 | history: sub-raster (0,0,W/2,H/2) of a W/2 x H/2 raster, type 0x505 (camera texture, RGB888, no alpha) | P |
| B+0x78 | history valid; cleared at SaveScreen entry, set at its end; DrawBlur requires it | P |
| B+0x7C | "stomp" quad (0,0)-(W/2,H/2), UV 0.5/W..(W+0.5)/W | P |
| B+0xDC | "restore" quad (0,0)-(W,H), same UVs | P |
| defaults | 0x8042F050: enable 0, alpha 0x80, zoom 1.0 | P |

**SaveScreen 0x8042E838** (PostEffect, once per frame, full screen): copy the frame into [[mgr+0x34]] (0x8042E8C0);
draw it ONE/ZERO into the top-left W/2 x H/2 (stomp, 0x8042E954); copy that corner into the history B+0x74
(0x8042E968); draw the full copy back over the screen ONE/ZERO (restore, 0x8042E99C). Filter, ZTEST and ZWRITE are
NOT set (inherited) - the source of the old "restore-quad depth rejection" speckle (memory note).

**DrawBlur 0x8042E254** (PostGlare, per viewport): needs enable, alpha, B+0x78. Quad = the viewport scaled by zoom
about its centre: x in [-(z-1)W/2, W+(z-1)W/2], same for y; UV = (viewport offset + 0.5) / parent size ..
(offset + size + 0.5) / parent size; colour (255,255,255,alpha). States: raster = history parent, SRCALPHA /
INVSRCALPHA, VERTEXALPHA on, ZTEST/ZWRITE/FOG off, **NEAREST**; restores all afterwards.

Maths: `scene_n = lerp(scene_n, zoom_z(history_{n-1}), a/255)`, `history_n = downsample(scene_n)` - a recursive
exponential trail with radial zoom 1.05 per generation (`a` = 64/255 under Chaos Control => weight of generation k
is 0.251^k).

**Who drives it (P):**
| caller | effect |
|---|---|
| Player::Behavior::ChaosControl (RTTI 0x805E51E4, vtable slots 0x8007BB8C / 0x8007C0AC) -> 0x800A2E3C -> 0x800A3714 | enable 1, alpha 0x80, zoom 1.05 (0x805F3BD0), spawns player effect id 0x51 |
| same -> 0x800A2DF0 -> 0x800A34C8 -> PlayerEffect **Slow** (ctor 0x800A4AB4, exec 0x800A4830, dtor 0x800A492C) | per frame: t += dt/0.5 (0x805F3BD4) to 1, or down to 0 after stop; enable = t > 0, alpha = trunc(64 x t) (0x805F3BE0), zoom 1.05; also writes 0.125 t (0x805F3BE4) to [0x8057E784]/[0x8057E790] (U: meaning) |
| same -> 0x800A2E18 -> 0x800A3528 | enable 0, alpha 0x80, zoom 1.05 |
| StageAction ContinueAction 0x802046BC, GoalAction 0x80205584, RestartAction 0x802062AC | read params, set enable 0 |
| EffectGlobalParamPJS exec 0x801FA5EC -> 0x80439A8C type 1 | data-driven from effect files: enable +0x15C, alpha +0x15D, zoom +0x160, blended with the previous values by t = +0x100 (`a = trunc(prev(1-t) + target t)`, zoom lerp, exact target when t >= 0.9999 (0x8051CB58)) |

**Port.** SaveScreen's first copy -> `rw_trail_save` -> `builder_.save_history(view)` for every view the copy
covers (screens.cpp:92-103). Stomp and restore quads sample camera textures and are dropped
(immediate.cpp:178-182); the second copy is a counter only (immediate.cpp:396-399). DrawBlur's quad ->
`rw_trail_draw` (screens.cpp:105-109) -> canvas draw sampling the view's History, UV mapped to canvas points
(screens.cpp:176-177, an undisplaced vertex samples what lies under it), **LINEAR** sampler (screens.cpp:202-203),
blend from the guest's states (SRCALPHA/INVSRCALPHA). History = full view resolution in the scene format
R16G16B16A16_UNORM (renderer.h:59, renderer.cpp:1281-1322); the scene's alpha is kept 1 (renderer.cpp:809-810) so
the history's alpha is 1 like the RGB888 original. The "constant depth snap" of the memory note does not exist in
the active tree (no match for constant_depth/snap/restore quad in runtime/); it is moot because the restore quad
is no longer drawn.
Verdict: **DELIBERATE DEVIATION** (full-resolution, 16-bit, bilinear history instead of half-res RGB888 point
sampled), behaviour otherwise **EXACT (S)**: same feedback recursion, same weight and zoom, same order.

**Original flaws -> proposed native maths (propose only):**
- Half-resolution 8-bit history, point-sampled: blocky trail, 1-LSB residue that never decays to the scene
  (round-to-nearest at 64/255 leaves a fixed point). Native: full-res 16-bit history (already done).
- Stomp UV offset 0.5/W on a 2:1 minification samples full-res texel 2i+1 (nearest) or the 2i+1/2i+2 pair: the
  history is shifted ~0.5 px (full-res) per generation, a drift compounded by the feedback (U: stomp filter state is
  inherited; needs a capture). Native: no stomp; sample at exact pixel centres (done).
- Frame-rate dependent: alpha and zoom are applied per frame (and Slow's ramp is time-based, but the trail is not).
  Native: `w = 1 - (1 - a/255)^(dt * 60)`, `zoom_eff = zoom^(dt * 60)` if the presentation rate ever differs from the
  60 Hz simulation.
- Restore quad with inherited depth state: z-rejection speckle (already avoided by dropping it).

---------------------------------------------------------------------------------------------------------------------

## 3. DrawScreenColor (C = mgr+0x44): multiply, colour overlay, and colour inversion

| item | value / address | ev |
|---|---|---|
| task | EffectDrawScreenColorTask vt 0x8053DC88, dtor 0x801D2820, exec 0x801D2878 -> 0x80439998 -> **0x80438C90**; level PostEffect (after SaveScreen) | P |
| C+0x00 | colour enable | P |
| C+0x01 | **inversion enable** | P |
| C+0x03..+0x06 | multiply colour RGBA (default 0xFFFFFFFF at 0x805F3BB8, 0x800A4A5C) | P |
| C+0x07..+0x0A | overlay colour RGBA (default from r2+0x1E10, U value) | P |
| C+0x10 | copy sub-raster, re-placed every draw at the camera viewport rect (0x80438E24) | P |
| C+0x18.. | quad, 4 x 0x18 (colour at +0x24/+0x3C/+0x54/+0x6C), covering the camera viewport | P |
| get/set | 0x804398F4 / 0x8043996C (0x804396D4) / 0x80439700 | P |

Passes in 0x80438C90 (P):
1. If C+0 and (multiply != white or overlay alpha != 0):
   a. multiply (if rgb != 255,255,255): copy the viewport (0x80439124), quad colour = C+3..6, texture = the copy,
      ONE/ZERO, NEAREST, no Z/fog -> `screen = screen x colour` (0x804392DC).
   b. overlay (if C+0xA != 0): untextured, colour C+7..0xA, SRCALPHA/INVSRCALPHA -> `lerp(screen, rgb, a)` (0x80439404).
2. If C+1: untextured white quad, SRC = **INVDESTCOLOR**, DST = ZERO -> `screen = 1 - screen` (0x80439590).
3. Restore the 7 saved states.

Who drives it (P): PlayerEffect Slow ctor 0x800A4AB4 sets C+0 = 1 and **C+1 = 1** keeping the colours (0x800A4B60..
0x800A4B9C); its dtor 0x800A492C clears both. So **Chaos Control inverts the picture** (negative) for the Slow's
lifetime, plus the zoom trail of section 2. EffectGlobalParam type 2 (0x80439FE0.., +0x177..+0x181, blended by t)
sets everything from effect data (screen tints/flashes).

**Port.** Multiply -> `rw_screen_multiply` (screens.cpp:125-129): a blend `src = DestColor, dst = Zero` times the
vertex colour, no copy (screens.cpp:207-210) - identical colour result. Overlay and inversion are untextured Im2D ->
generic im2d path; SRCBLEND INVDESTCOLOR is accepted (render_states.cpp:90-99; DESTBLEND refuses DESTCOLOR /
INVDESTCOLOR, render_states.cpp:100-108, which these passes never use) and mapped 1:1 (render_states.cpp:287-292,
renderer.cpp:102-103). UNORM16 scene clamps to [0,1] so the inversion stays in range.
Verdict: **EXACT (S)** for multiply (by blend instead of copy: DELIBERATE, cheaper, no 8-bit copy) and **EXACT (M)**
for overlay and inversion (not traced at runtime here).

Flaws: multiply on an RGB888 copy (8-bit quantised, point sampled) - gone natively. Full-screen colour pass is
resolution-independent; no other issue found.

---------------------------------------------------------------------------------------------------------------------

## 4. Full-screen heat haze (H = mgr+0x48) and the refraction elements (EffectShimmerPJS)

### 4.1 Full-screen shimmer (FullScreenShimmerDraw / Sub)
| item | value / address | ev |
|---|---|---|
| draw task | EffectDrawFullScreenShimmerTask vt 0x8053DBFC (dtor 0x801D2598, exec 0x801D25F0 -> 0x8043803C -> **0x804369CC**); ctor 0x801D224C, made per stage by fn_801783D4 (0x801785D8), parent = LandDispManager_N (level 1 OpeqWorld) | P |
| update task | EffectDrawFullScreenShimmerSubTask vt 0x8053DBE0 (dtor 0x801D250C, exec 0x801D2564 -> 0x80438068 -> **0x80437178**), phase 16 Particle | P |
| object | ctor 0x80437D98/0x80437E40: copy raster = [mgr+0x34] if present, else W x H RGB888 camera texture (0x505) | P |
| params (copied by 0x80437A9C to H+0x0C) | H+0x0C enable, +0x0D vertex alpha, +0x14 Nc (columns), +0x18 A_c, +0x1C T_1, +0x20 L_c, +0x24/+0x28 x range, +0x2C Nr (rows), +0x30 A_r, +0x34 T_2, +0x38 L_r, +0x3C/+0x40 y range | P |
| state | +0xA4 vertices (0x18 each), +0xA8 base grid {x,y,w} (12 B; w = 0 on the border rows/columns, 1 inside: 0x804377E4/0x804377F4), +0xAC row table, +0xB0 column table, +0xBC/+0xC0 phases, +0xCC/+0xD0 viewport W,H (grid rebuilt by 0x804374F0 when they change) | P |
| constants | 2 (0x8051C9A8) x pi (0x805FBDB0) = 2 pi; 0.5 (0x8051C99C) | P |

Update (0x80437178, P): phases advance by dt and wrap: `p1 = (p1 + dt) mod T_1`, `p2 = (p2 + dt) mod T_2`;
`R[r] = A_r sin(2pi (p1/T_1 + r H / (Nr L_r)))` for r = 0..Nr; `C[c] = A_c sin(2pi (p2/T_2 + c W / (Nc L_c)))`
for c = 0..Nc; each strip vertex: `x = base.x + w R[row]`, `y = base.y + w C[col]` (serpentine strips, rows
r, r+1). The **positions** are displaced; UVs stay at the undisplaced points, so the picture is warped by moving
the mesh (edges pinned by w = 0).
Draw (0x804369CC, P): copy the screen (0x80436EEC), then the grid as one TRISTRIP of H+0xB4 vertices
(0x80437040): SRCALPHA/INVSRCALPHA with vertex alpha H+0x0D, VERTEXALPHA on, ZTEST/ZWRITE/FOG off, **NEAREST**,
CULL NONE. No indirect texturing: it is a copied EFB redrawn on a displaced grid.
Driven by EffectGlobalParam type 3 (0x8043A374: 0x80437A9C copy, 11 floats lerped by t, +0x184..+0x1B8) -> 0x80438010.

Port: copy -> `rw_screen_snapshot` (screens.cpp:61-86), grid -> `rw_shimmer_draw` (screens.cpp:111-115), Snapshot
source, anchor Fill, LINEAR clamp sampler, positions and canvas UVs as the guest wrote them. Verdict: **EXACT (S)**
in order and maths, **DELIBERATE DEVIATION** in filtering (linear vs nearest) and snapshot precision.
Flaw: amplitudes A and wavelengths L are in 640 x 480 console pixels; with Fill on a wide view the grid is stretched
horizontally, so horizontal displacement and wavelengths grow with the aspect (1.33x at 16:9) while vertical ones do
not (M). Native proposal: express A and L in canvas-height units and lay the grid out isotropically over the view
(x scaled by view_height/480 instead of view_width/640).

### 4.2 Refraction elements (EffectShimmerPJS)
| item | value / address | ev |
|---|---|---|
| class | Effect::PJS::EffectShimmerPJS vt 0x8053D704: dtor 0x801D1224, update **0x80436164** (sin/cos 0x8000FFE8 / 0x8000FEE4, time via 0x8042B93C), draw **0x8043531C** (bucket 6, EffectDrawShimmerTask at PostGlare) | P |
| draw | project the element (0x804834A8), build a patch (types 0/2/4 at 0x80435740..), sin-displaced; copy the screen (0x80435FA4); draw (0x804360D4) SRCALPHA/INVSRCALPHA, VERTEXALPHA, **ZTEST on**, ZWRITE off, FOG off, NEAREST | P |
Port: `rw_screen_snapshot` + `rw_refraction_draw` (screens.cpp:117-123), anchor Center (the patch sits where the
element projects); a patch spanning the view's canvas width is promoted to Fill (screens.cpp:191-197, the Chaos
Control warp's full-screen refraction). Verdict **EXACT (M)** + DELIBERATE DEVIATION (linear, widening to the view).

### 4.3 PlayerBarrierHeatEffect::HeatEffect - not a screen effect (P)
RTTI vt 0x8055ED88, dtor 0x8034120C, exec **0x803411CC** = 0x803409CC (moves the owned effect instance at +0x30 to the
player's node via its vtable+0x3C) + 0x80356068/0x80355F24 (the Heat Barrier item's attack collision, CharaColli
callback). Its visuals are ordinary effect elements in the buckets above; nothing full-screen. Port: translated,
draws through the element families.

---------------------------------------------------------------------------------------------------------------------

## 5. Fade (SonicteamUSA::System::Fade)

| item | value / address | ev |
|---|---|---|
| tasks | FadeTask vt 0x8051E270 (dtor 0x80044D24, exec 0x80044D7C -> update 0x80044B00), phase 0 System; FadeDrawTask vt 0x8051E28C (dtor 0x80044DB0, exec 0x80044E08 -> draw **0x800448C8**), level **OnFade**; both made by 0x80044174 from 0x801E2888 | P |
| object F (singleton 0x8004427C, init FadeLogo 0x80044344) | F+0x00 level 0..1, +0x08 speed, +0x0C target, +0x14 state (0 / 1 / 2+ = moving), +0x10..+0x12 colour RGB, +0x18.. 10 quads x 0x60, +0x3D8 black-hold timer, +0x3DC logo alpha, +0x3E0 FadeLogo raster (FadeLogo.txd 0x804AC86C), +0x3E4 logo quad, +0x444 flag | P |
| curtain | ten 64 x 480 columns (0x805F2C18 = 64, 0x805F2C1C = 480), z 0.1 (0x805F2C14), colour (rgb, trunc(255 F+0)) | P |
| update | `F+0 -= / += dt * speed` to the target, state set at the end; while held black the timer accumulates; logo alpha += 5 dt (0x805F2C28) to 1 after 0.4 s (0x805F2C24), else -= 5 dt | P |
| draw | VERTEXALPHA on, ZTEST/ZWRITE off, CULL NONE, untextured, 10 TRISTRIPs (0x800449D8); blend not set (inherited, normally SRCALPHA/INVSRCALPHA) (U); if logo alpha > 0 and colour black: the logo quad (white, alpha trunc(255 a)) (0x80044A88) | P |

Port: plain Im2D through immediate.cpp; the curtain's columns are recognised as one ScreenCover group laid out Fill
(runtime/lift/ui/hud_layout.h:55-58), so a wide output is fully covered. Verdict **DELIBERATE DEVIATION (S)**
(Fill instead of the 640-wide box) - correct intent. Original flaw: hard 640 x 480 curtain (4:3 constants), alpha
quantised to 8 bits per step (trunc), logo fade time-based (fine).
`gekko/appear_fade.h` is unrelated to this Fade: it is a port-only dissolve for models that pop in inside the view
(appear_fade.h:1-21), not an original effect.

---------------------------------------------------------------------------------------------------------------------

## 6. Placement only: glare and lens flare
PreGlare (EffectPreGlareTask vt 0x8053DC18, exec 0x801D266C -> 0x80431968 -> 0x80430D64) at GlareWorld before the
lens flare and the bucket-5 glare sources; Glare level objects (mask 0x80); PostGlare (vt 0x8053DC34, exec 0x801D26E8
-> 0x8043193C -> 0x80430904) first in PostGlare, before DrawBlur and the refraction elements. Details:
source/docs/GLARE_DISSECTION.md. DrawGlobalLensFlare / Another (0x801D1F50 / 0x801D29D8) also at GlareWorld.

---------------------------------------------------------------------------------------------------------------------

## 7. Open questions (need a runtime capture or a Dolphin check)
1. The stomp quad's inherited TEXTUREFILTER (nearest or linear) at 0x8042E954: decides whether the console's trail
   drifted ~0.5 px per generation.
2. SaveScreen's restore quad's inherited ZTEST/ZWRITE at 0x8042E99C (the speckle cause), for the record.
3. The meaning of [0x8057E784]/[0x8057E790] = 0.125 t written by Slow (time scale? sound?).
4. Which effect files carry EffectGlobalParam elements (types 0 glare, 1 blur, 2 screen colour, 3 heat haze) and
   their values (grid Nc/Nr, A, T, L per stage).
5. The overlay default colour at r2+0x1E10 (0x805FC590, BSS - runtime value).
6. Whether LandDispManager_N's children (the heat-haze copy) really run before LandDispManager_D..P in OpeqWorld
   (task runner child-before-sibling order; S from 0x8004ECAC, not traced).
7. Fade's blend state at draw time (inherited) and whether any caller fades to a non-black colour with the logo.
