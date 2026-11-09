# Lighting, material effects and fog (static RE, 2026-10-27)

Family: LIGHTING / MATERIAL EFFECTS / FOG. Static analysis only (PAL main.dol, disc files read with a 30-line parser).
Register bases: r2 = 0x805FA780, r13 = 0x805EC500 (`__init_registers` 0x8000332C..0x80003338). The RW GC
device's GX light objects are at 0x805E2760 + 64 * i. The current view matrix is 0x805E428C.
Evidence levels: **P** PROVEN (code/constant read), **S** STRONGLY SUPPORTED, **M** STRUCTURAL MATCH, **U** UNKNOWN.

Ownership summary: no native owner exists for any of these effects. The game's logic still runs translated:
light slot selection, LightColli, SetLight commands, TFogSet, RpUVAnim, texture-pattern swaps and DMorph weights.
The native RW driver (`scaffold/rw_driver`) only reads the resulting RenderWare state at each atomic/sector draw:
RpLight lists, the RwCamera planes, RW render states, MatFX material data and material texture pointers.
`lift/render` then draws that state (mesh.hlsl / skinning.hlsl).

---------------------------------------------------------------------------------------------------------------------

## 1. Lights

### 1.1 Stage light manager and `stgNNNN_light.bin`

| Item | Value | Ev |
|---|---|---|
| Loader | `fn_80048738(mgr, stage)`: sprintf `"stg%04d/stg%04d_light.bin"` (0x804ACB18) -> `fn_8004867C` (file read 0x800450C4, copy to mgr+0x20, endian swap 0x80043C80 with format `"xxxxxxxxxxxii"` at 0x804ACB08, 16 records, or size/0x34 when the file is short). The 0x340-byte self-copy at 0x80048780 is a no-op (`cmplw r3,r3`). | P |
| Called from | `fn_80177798` (stage setup): `0x80047F5C` defaults, then load, then `fn_80048574` creates the lights | P |
| File | 832 bytes = 16 records x 0x34, **little-endian** on disc (41 files, e.g. game/files/stg0100/stg0100_light.bin) | P |
| Record (0x34) | +0x00 ambient RGBA f32x4; +0x10 directional RGBA f32x4; +0x20 vec3 (always (0,0,1) in stg0000/0100/0604 and only consumed for non-directional lights by `fn_80048088`); +0x2C s32 rotation about X (degrees); +0x30 s32 rotation about Y (degrees) | P (layout) / M (+0x20 meaning) |
| Slots | 16 file records = "LightNo:" strings 0x804D3CC8..0x804D3DD8: 0-3 PLAYER_0..3, 4-7 OBJECT_0..3, 8-11 ENEMY_0..3, 12 KAGE, 13-15 OTHER_0..2. The manager holds 17 slots (mgr+0x20+0x34*i); slot 16 is a built-in default (0x8051E564; the others default from 0x8051E530, 0x80047F5C) | P (indices) / S (name mapping by order) |
| Lights created | `fn_80048574`: mgr+0 = RpLight type index 0 -> **ambient**, mgr+4 = index 1 -> **directional** (index->RW type table 0x805E4978 = {2 ambient, 1 directional, 0x80 point, 0x81 spot, 0x82 soft spot}; RpLightCreate = 0x8045BD84). Up to 6 more dynamic lights in mgr+8..0x1C (`fn_800484AC`, slots 2..7), created by SetLight-style commands (`fn_80228AC0` -> `fn_80048A6C`), and these may be of any type 0..4 (`fn_80047870` handles position 0x80488600, rotation 0x804887D4, radius 0x8045B7D4, cone 0x8045BA94) | P |
| Select slot | `fn_80048308(mgr, slot)`: skipped when slot == mgr+0x454 (cache). Sets ambient colour = rec+0x00 and directional colour = rec+0x10 (`fn_80047808` -> RpLightSetColor 0x8045B814). Then `RwFrameRotate(X=(1,0,0) 0x8051E518, rec+0x2C deg, REPLACE)` and `RwFrameRotate(Y=(0,1,0) 0x8051E524, rec+0x30 deg, POSTCONCAT)` (`fn_80048184`). The light travels along the frame's `at`, which is R_y(b) R_x(a) (0,0,1) | P |
| Per-object selection | 11 draw wrappers (0x80038304, 0x800383DC, 0x8003851C, 0x800386EC, 0x80038AE0, 0x80112C70, 0x8011A268, 0x801645D0, 0x80238E1C, 0x80358A54, ...) read the model's slot byte (holder+0x24), then call `fn_800483B8` (pending slot mgr+0x456, range 0..16) and `fn_80048308`, then render (0x8045A138 clump / 0x80457C14 atomic). `fn_800477A8` restores the default slot mgr+0x455 (= 4, OBJECT_0, set at 0x800485FC) afterwards. So the light colours and direction **change between draws within one frame** | P |
| Light blending | `fn_80048864` (from `fn_80228B48`): interpolates a slot's colour or angles (0x80010EE0 / 0x80011B7C); modes 0..2 | M |
| LightColli / LightColliMasterTask | LightColli vt 0x8053F25C (m = 801EFA24 dtor, 8015AD28 ...); master task vt 0x8053F18C (801EEC6C dtor, 801EE128 exec). These are collision volumes (shape switch on +0x3C = 0..2, `fn_801EEFF0`) that assign a LightNo slot to whatever is inside them. The LightNo strings are their parameter enumeration | M |
| SetLight / SetLightCommand | `SetLight` 0x801DADF4 (name getter; strings 0x804D1940 / 0x804D1A64); SetLightCommand vt 0x8053E2E4 (801DC404, 801DA09C, 801DA050, 801DADF4). An event command; it drives the dynamic-light path above | M |

Port: none of this is reimplemented. The driver reads the lights live at every atomic draw (`pipelines.cpp:695-696`
-> `light_set`, `pipelines.cpp:108-147`), so per-object slot switching is seen as the game leaves it. **EXACT (path)**, S.
A light's direction is taken from its root frame's modelling matrix (`pipelines.cpp:132-137`). The original uses
`RwFrameGetLTM` 0x804882E8, which syncs a dirty root before returning LTM+0x50. For a root frame the two are identical,
and the manager's light frames are root frames (`RwFrameCreate` 0x80487E7C in `fn_80047B5C`). A non-root light frame
would make the port read an unsynced LTM: a latent DEVIATION.

### 1.2 RW GC lighting callback (atomics)

`fn_80464478(atomic, state)` is called at the start of every lit atomic draw:
1. It resets the state: ambient (+0x0C..+0x14) = 0.0 (0x805FC208), +0x18 = 1.0, ambient-present +0x1C = 0, light mask +0x20 = 0, light count +0x24 = 0.
2. If `geometry.flags & rpGEOMETRYLIGHT (0x20)` and RwGlobals->curWorld != 0, it calls `fn_8046521C(flags=rpLIGHTLIGHTATOMICS)` over the world's directional list (world+0x3C, link at light+0x34), then **increments RwGlobals+0x0A (u16 lightFrame)**.
3. Local lights: for each world sector the atomic touches (atomic+0x64 ties, sector+0x40 light list), each light whose stamp (light+0x3C) differs from lightFrame and whose flags have bit 0 is stamped. If `|light.pos - sphere.c|^2 < (sphere.r + light.radius)^2` (sphere = `RpAtomicGetWorldBoundingSphere` 0x80457720), `fn_80465438` is called. That function returns early once 8 lights are loaded.
4. Each ambient channel is clamped to <= 1.0 (0x805FC20C) when an ambient light was seen (0x804645C0-0x80464604).

`fn_8046521C`, per light:

| Type | GX programming | Ev |
|---|---|---|
| 2 ambient | colour summed into state+0x0C..0x14 (float, unclamped until step 4), +0x1C = 1 | P |
| 1 directional | `at` (LTM+0x20) transformed by the view 0x805E428C (0x80483554), x and z negated (RW camera to GX eye space); **light placed as a point at -1048576 x dir** (0x805FC228 = -2^20, `GXInitLightPos` 0x803979FC); attenuation `GXInitLightAttn(1,0,0, 1,0,0)` (0x805FC220/224) = none; colour = trunc(c x 255) per channel, **no clamp** (`fctiwz` -> `stb`, 0x805FC22C = 255), alpha 0; `GXLoadLightObjImm(obj, 1<<n)`; mask \|= 1<<n; n++ | P |
| per-light attenuation plugin | RpLight extension at offset [r13+0x60D8]: when its first word != 0, six explicit coefficients replace the default. **Only writers: the constructor zeroing 0x80465800 and registration 0x80465818** -> always default | P |

`fn_80465438` (local lights, GX lights 0..7):

| Type | GX programming | Ev |
|---|---|---|
| 0x80 point | position = LTM+0x30 transformed by the view (0x804834A8), x and z negated; `GXInitLightAttnA(1,0,0)` (0x8039778C); `GXInitLightDistAttn(ref_dist = 0.5 x radius, ref_bright = 0.5, GX_DA_MEDIUM)` (0x8039792C, 0x805FC230 = 0.5) => k = (1, 1/r, 2/r^2) | P |
| 0x81 spot | direction = view x `at`; `GXInitLightSpot(cone x 57.29578 deg (0x805FC234), GX_SP_FLAT)` (0x8039779C, cone = 0x8045B868), same distance attenuation | P |
| 0x82 soft spot | as 0x81 but `GX_SP_COS` (r4 = 2) | P |

### 1.3 The GX lighting equation (channel control)

The decision table `fn_80466624` (default + skin pipelines, called from 0x80463B10 / 0x8044EF94) sets:
`GXSetNumChans(1)`, `GXSetChanCtrl(GX_COLOR0, en, amb_src, mat_src, mask, GX_DF_CLAMP, GX_AF_SPOT)` (0x80466CAC),
`GXSetChanCtrl(GX_ALPHA0, en, amb, mat, 0, GX_DF_NONE, GX_AF_NONE)`, and COLOR1/ALPHA1 off. The ambient register
is `trunc(ambient x ka x 255)` with alpha 0 (`fn_80466448`, 0x80397AB0). The MatFX passes (`fn_804A55CC`
0x804A5680 and the per-effect functions) use `GXSetChanCtrl(GX_COLOR0A0, 1, prelit?VTX:REG, REG, mask, DF_CLAMP,
AF_SPOT)` with ambient alpha 0xFF (`fn_804A3CE0` 0x804A3D24). Light alpha is 0, so the lit alpha equals mat.a.
The PTank callback 0x80449D54 lights with the same DF_CLAMP/AF_SPOT. **No call site in the game uses GX_AF_SPEC:
there is no specular** (all 58 GXSetChanCtrl sites were checked). P.

Per vertex, the GX hardware computes
`chan = mat x clamp01( amb + sum_i mask_i C_i max(0, N.L_i) spot_i / (k0 + k1 d + k2 d^2) )`, then quantises to 8
bits and Gouraud-interpolates. The TEV (decision-table stage 0, 0x80466784) computes `C1 + C0 x RASC` with clamp,
and stage 1 multiplies by the texture.

| Aspect | Original | Port | Verdict |
|---|---|---|---|
| Where | per vertex (XF) | per vertex, `VSMesh` mesh.hlsl:97-131 | EXACT, P |
| Diffuse fn | DF_CLAMP | `max(0, dot(n, -dir))` mesh.hlsl:82 | EXACT, P |
| Directional L | `normalize(pos_far - v)` with pos at 2^20 along -dir: angular error ~ \|v_eye\|/2^20 rad | exact -dir (frame_math.h:302-306 normalises) | DELIBERATE DEVIATION (improvement), P |
| Point distance attenuation | GX_DA_MEDIUM, ref 0.5 at r/2 => 1/(1 + d/r + 2d^2/r^2) | mesh.hlsl:88-92, identical formula | EXACT formula, P ... |
| ... but local lights reach the port? | point/spot from sector lists (`fn_80465438`) | `light_set` reads **only** the world directional list (pipelines.cpp:116-143); `LightSet::points` is never filled anywhere (only frame_math.h:301-323 reads it) | **DEFECT if any local light exists** (NOT RENDERED); spot/soft-spot cones have no shader path at all. Whether the game creates local lights at runtime (dynamic slots 2..7 of type 2..4) is U |
| Light count | 8 hardware lights shared (local path caps at 8; the directional path has no cap check) | 4 directional (frame.h:117, frame_math.h:300) | DEVIATION, latent (the manager has 1 directional) |
| Ambient | float sum clamped <= 1, then x ka, trunc to u8 | min(sum,1) (pipelines.cpp:144-145, frame_math.h:297-299) x ka, float | EXACT within 1/255; ka = 1.0 on all 17,430 materials (census, asset_import_tests.cpp:420) |
| Light colour | trunc(c x 255), **wraps above 1.0** (no clamp before `stb`) | float, unclamped | DEVIATION <= 1 LSB for data <= 1.0 (all three light.bin files read are <= 1.0); for c > 1 the original wraps, the port saturates the sum |
| Prelit + lit | amb_src = VTX: clamp(v + D) x mat | `saturate(base + d)`, base = v (mesh.hlsl:127-128) | EXACT, M (the decision-table cases are as in the port's comments; only the channel / TEV arguments were re-checked here) |
| Two-stage | TEV0 `C1 + C0 x RASC` (C1 = C0 a k), clamp; TEV1 x TEXC | mesh.hlsl:118-124 | EXACT, P (TEV args 0x80466770-0x80466BD4) |
| Normal | modelview 3x3 (0x804960FC loads the same matrix as GXLoadNrmMtxImm, not inverse-transposed, not normalised); normals quantised on the GC instance | world 3x3 then **normalised** (mesh.hlsl:105-108), float normals | DEVIATION for non-uniformly or non-unit scaled models; U whether the GX XF renormalises (open question) |
| 8-bit quantisation of channel colour | yes | no | DELIBERATE DEVIATION (precision) |
| lightFrame RwGlobals+0x0A | ++ per lit atomic in a world | ++ (pipelines.cpp:622-631, same conditions incl. PTank needs normals) | EXACT, P. The per-light stamps (light+0x3C) are not written. No other reader was found (S) |

Dead code: `fn_8032D7CC` sets up its own GX spot light (0x8032D930-0x8032D9D0). It has no `bl` caller, no address
materialisation and no data word pointing at it (word scan of the DOL): unreachable, P.

### 1.4 light_slots (mention only)
`lift/assets/light_slots.*` is a deliberate data fix. It moves GUN Fortress's blinking corridor glow quads (the
textures `*_zn055.N`, swapped by the pattern animation, section 3.2) onto their painted wall slots
(pipelines.cpp:1036-1061). It is not part of the lighting model.

---------------------------------------------------------------------------------------------------------------------

## 2. Materials: RpMatFX and surface properties

Material data: material colour (+0x04 RGBA8), surface properties ka (+0x0C; ks/kd are unused by the GC driver). The
MatFX data pointer is at material + [0x805F2750]; the effect type is at +0x30 of the data. Pipelines: MatFX atomic
[0x805F2764], MatFX sector [0x805F2768], skin+MatFX [0x8056E7F4]. All share the render callback `fn_804A62D0`,
which calls lighting 0x80463C80, the matrix 0x804960FC, and per mesh `fn_804A55CC` dispatching on the effect type
(0x804A57D0-0x804A5804). Disc census (asset_import_tests.cpp:420-424): ENVMAP 4,837, UVTRANSFORM 552, DUAL 310,
DUALUVTRANSFORM 75, BUMPENVMAP 2, BUMPMAP 0. The native-shader material plugin 0x129 (0x804A7A58) is carried by no
asset.

| Effect | GC implementation | Port | Verdict |
|---|---|---|---|
| 0 none / base | inline in 0x804A55CC; channel per 1.3 | `Combine::OneStage` for MatFX (pipelines.cpp:769) | EXACT, M |
| 5 UVTRANSFORM | `GXLoadTexMtxImm(base, GX_TEXMTX0, 2x4)` 0x804A5A98 from rows (right.x, up.x, 0, pos.x / right.y, up.y, 0, pos.y); `GXSetTexCoordGen2(0, MTX2x4, TEX0, TEXMTX0, 0, PTIDENTITY)` 0x804A5AB4 | `uv_matrix` pipelines.cpp:537-545 -> `uv_row` mesh.hlsl:132 | EXACT, P |
| 4 DUALTEXTURE | `fn_804A3F74`: base pass, then second pass with the material's blend pair via RwRenderStateSet(SRC/DESTBLEND) 0x804A4258-0x804A4284, texgen from TEX1 when geometry has TEXTURED2 (0x804A4288-0x804A42AC), alpha compare from the texture's alpha 0x804987C8; same channel colour | `ExtraPass::Dual`, pipelines.cpp:554-566; `PSDual` mesh.hlsl:197-205 | EXACT, M (4 blend pairs on disc) |
| 6 DUALUVTRANSFORM | `fn_804A4358`: base matrix on pass 1, dual matrix (identity if none) on pass 2 | pipelines.cpp:569-574 | EXACT, M |
| 2 ENVMAP | `fn_804A48DC` base (alpha test forced off) + `fn_804A4B7C` env pass. Blend SRC = ONE (or DESTALPHA when useFrameBufferAlpha, +0x0C), DST = ONE. Both colour channels disabled, mat colour = trunc(coef x 255.9) grey (0x805FC4F4) when coef < 1.0, else white. TevOp MODULATE. Texgen `MTX2x4 from NRM` with TEXMTX0 = -0.5 x M rows 0/1 + 0.5 (0x805FC4E0 = -0.5, 0x805FC4E4 = 0.5), where M = orthonormalised (atomic LTM x (env frame ? inverse(env frame LTM) : view 0x805E428C)) (0x804A4CDC-0x804A4DA8) | `PSEnvironment`: `min(coef,1) x env.rgb`, UV `(0.5 + 0.5 n.x, 0.5 - 0.5 n.y)` from the normalised view normal (mesh.hlsl:136-141, 189-195); ONE/ONE | EXACT (RW's eye x points screen-left, so -0.5 on RW x = +0.5 on screen-right x), M. Coefficient quantisation (<= 1/255) is a DEVIATION. **Env-map frame (fx+0x00) is ignored by the port** (pipelines.cpp:546-553 reads only +0x04 and +0x08): DEFECT only if the game sets an env frame (U); useFrameBufferAlpha unused on disc (census) |
| 3 BUMPENVMAP | `fn_804A4E00(r7=1)`: indirect-TEV bump plus environment (rw_capture.cpp:150 marks it unsupported) | falls through matfx_layers: base pass only | NOT RENDERED (2 materials), DEFECT (minor) |
| 1 BUMPMAP | `fn_804A4E00(r7=0)` | none | not on disc |

Fog on extra passes: GX fogs each pass's fragment before blending, so an additive env pass adds f x fog colour. The
port does the same (mesh.hlsl:153-162 applied in all three pixel functions): EXACT, P.

---------------------------------------------------------------------------------------------------------------------

## 3. Texture animation

### 3.1 TextureUVAnimManagerTask (RpUVAnim)
- vtable 0x8053EDC4 (type info 0x805E9E74 -> name 0x804D38D0): dtor 0x801EB7C4, exec 0x801EB790.
- exec: dt = `fn_8010A840`. `fn_801EBA74` walks the registered clumps; per clump `fn_80039BBC(clump, dt, -1.0 (0x805F71A8))` runs `RpClumpForAllAtomics` (0x80457CA4) with callback 0x80039BF8. That callback finds each material's UV-anim interpolator, sets or adds its time (0x8045593C / 0x804559B4) and applies it (0x804A3334, MatFX module), writing the MatFX UVTRANSFORM / DUALUVTRANSFORM matrices. M.
- Port: logic translated; the matrices are read at each draw (pipelines.cpp:569-578). **EXACT (path)**.
- Flaw: the clock is the game's dt. It is fine at a fixed 60 Hz tick (the game's own step).

### 3.2 Texture pattern animation ("Texture Change : ")
- Init `fn_8004F284` (from `fn_8004F4A8`): up to 32 entries of 0x14 at obj+0x44: {pattern def, material, texture dictionary, frame texture table}. Frames are found by name `"%s.%d"` (r2-0x79C8) in the dictionary (0x8048E04C).
- Frame select `fn_8004F0FC`: t = entry time (+0x34), or the global time [r13-0x7AA0] when the entry time is <= 0. t is wrapped by the period (frame count x per-frame times at def+0x4C, 8-byte entries); the result is the last frame with start <= t.
- Apply `fn_8004F1C4`: only on a change of frame, `RpMaterialSetTexture(material, frame tex)` (0x8045C2C0), cached at +0x10.
- Port: the material texture is read per draw (pipelines.cpp:729-734): **EXACT (path)**. The corridor glow `*_zn055.N` of 1.4 is one of these patterns.

### 3.3 DeltaMorphAnimManagerTask (RpDMorph)
- vtable 0x8053E6EC (type info 0x805E9D54 -> 0x804D1E90): dtor 0x801DE324, exec 0x801DE2F0 (-> 0x80130F10 dt, 0x801DE590 walks the animations).
- Render callback `fn_80444AC4` (1352 bytes) blends `base + sum w_t delta_t` **in place** into the geometry arrays between lock/unlock (0x8045A1BC / 0x8045A214). Normals are not renormalised (no sqrt/normalise callee).
- Port: the weights are read from dmorph current values (pipelines.cpp:678-692) and blended on the GPU before skinning (skinning.hlsl:69-82). The shape is the same; normals are renormalised later in mesh.hlsl:107-108. STRUCTURAL MATCH, DEVIATION for the lit normal length (U on hardware, see 1.3).

---------------------------------------------------------------------------------------------------------------------

## 4. Fog

### 4.1 Stage fog file and TFogSet

| Item | Value | Ev |
|---|---|---|
| Path | `"stg%04d/stg%04d.fog"` 0x804C40A0, sprintf in `fn_8016A5E4`; load `fn_8016A66C`; swap `fn_8016A4EC` | P |
| File | 960 bytes = 24 records x 0x28, little-endian; header word written as 2 (0x8016A4F8) | P |
| Record | +0x00 u32[4] zone mask (bits 0..167; `fn_8016A788` marks a record active when any current zone, tested by 0x8003AA58, is in the mask); +0x10 f32 near clip; +0x14 f32 far clip; +0x18 u32 fog on (1); +0x1C f32 fog plane (start); +0x20 f32 fog end (clamped to +0x14 when < 1.0 (0x805F5C30); **not read by exec**); +0x24 u8 x 3 colour | P |
| Examples | stg0100: 4 zone records, far 30000, fog 0..3500/5000/8000/7000, colour bytes 96 32 00; stg0200: all zones, 5000..10000; stg0401: far 600000 no fog | P |
| TFogSet | vtable 0x8052C84C (type info 0x805E77E4 -> "TFogSet" 0x805F5C38): dtor 0x8016ACD0, exec 0x8016A904 | P |
| exec | Runs only when the fog data word0 == 2 and byte+8 == 0. Reads the current camera (`fn_8000DCC0`) near +0x80, far +0x84, fog plane +0x88, and the current FOGCOLOR (RwRenderStateGet 0xF). Over the active records it averages near/far over all of them and fog plane + colour over fog-on ones, with the current value counted as one sample. Then it smooths: `new = 0.98 cur + 0.02 avg` (0x805F5C50, 0x805F5C54); near is clamped >= 1.0 (0x805F5C48); reset flag +0x2C gives near = plane = 1, far = 30000 (0x805F5C4C). It writes `RwCameraSetNearClipPlane` 0x804865A0, `SetFarClipPlane` 0x8048665C, cam+0x88, `RwRenderStateSet(FOGCOLOR)` and `FOGENABLE = 1`, plus a byte copy to 0x8057E760 and flag +0x1809 of `fn_8003A868()` (1 = fog zone active, else 0) | P (M for the 0x8057E760 meaning) |

**Colour lane order (open):** exec treats the FOGCOLOR word as bytes [b0, b1, b2, b3]. Lanes 0..2 are averaged
with file bytes +0x24..+0x26, and lane 3 is forced to 0 (0x8016ABE8). The driver decodes the word as 0xAARRGGBB
(0x80498AA0-0x80498ACC). So file byte 0x24 lands in **alpha**, 0x25 in R, 0x26 in G, and **blue is always 0**. The
port decodes the same word the same way (render_states.cpp:305-309), so it reproduces whatever the console shows.
Treat this as an original-engine flaw candidate until a Dolphin check settles it.

### 4.2 GX fog programming (RW GC driver)

| Item | Original | Port | Verdict |
|---|---|---|---|
| FOGENABLE | 0x804986B8: on first enable caches start = cam+0x88, end = cam+0x84 (unless pinned, state+0x20, never set), near = +0x80, far = +0x84; `GXSetFog(table[type], start, end, near, far, colour)`; disable -> GX_FOG_NONE | render_states.cpp:109-129, same cache | EXACT, P |
| FOGCOLOR / FOGTYPE | 0x80498A80 / 0x80498AF8 re-arm with the current camera planes; FOGTYPE accepts only 1 (linear), returns 0 otherwise | render_states.cpp:131-146, 228-235 | EXACT, P |
| Type | table 0x8056FA18 = {NONE, PERSP_LIN(2), PERSP_EXP(4), PERSP_EXP2(5)}; only LINEAR is reachable | linear only | EXACT, P |
| Range adjust | `GXSetFogRangeAdj` called only from GXInit 0x80394088 (off) | planar eye depth (`o.depth = position.w`, mesh.hlsl:142) | EXACT, P |
| Factor | f = clamp((z_eye - start)/(end - start)), z_eye **reconstructed from the 24-bit screen z** with A/B/C from near/far (0x80399A0C), 8-bit blend | float eye depth, `lerp(rgb, fog, saturate(...))` mesh.hlsl:154-162 | DELIBERATE DEVIATION (precision), P |
| Colour | GXColor from the RW word | render_states.cpp:305-309 | EXACT, P |
| Camera fog plane | RwCamera+0x88 (layout::kCameraFogPlane) | rw_driver.h:66 | EXACT, P |

---------------------------------------------------------------------------------------------------------------------

## 5. Reflector, water, toon

- `Reflector` (type info vt 0x804F2394; real vtable 0x80547540: 80225A34, 802259F0, 802259AC, ...) is
  `WeaponReflector_cpp::Reflector`, a **weapon** that reflects bullets. It is not a render reflection. P.
- No water, mirror, refraction, toon or cel effect class or string exists ("MIRROR 0:off 1:On" and "PathMirror"
  are path/debug options). Water surfaces are ordinary materials (ENVMAP / UV transform / pattern), P/S.
- KAGE (slot 12) is the light slot used for the shadow-related draws; `EffectKage::KageTask` (vt 0x8056080C:
  80357D80 dtor, 80357A78 exec) belongs to the shadow family.

---------------------------------------------------------------------------------------------------------------------

## 6. Original-engine flaws and proposed native math (proposals only)

| # | Flaw | Evidence | Proposed replacement |
|---|---|---|---|
| F1 | Fog zones smoothed by `0.98/0.02` per exec (frame-rate dependent; tau ~ 50 ticks) | 0x8016AAC4, 0x8016AB8C | alpha = 1 - 0.98^(dt x 60) |
| F2 | TFogSet also drives the camera near/far planes (averaged and smoothed) | 0x8016AAE8 / 0x8016AAF8 | keep the game's planes as the fog/clip intent, but take render depth from the port's own far plane (DRAW_DISTANCE.md) |
| F3 | GX fog z_eye from 24-bit z: banding near far = 30000..600000 | 0x80399A0C | eye-space depth, as the port already does |
| F4 | FOGCOLOR lane order: B always 0, byte 0x24 goes to alpha | 0x8016ABE8, 0x80498AA0 | confirm in Dolphin; if real, keep it (it is the authored look) or offer an option |
| F5 | Directional light = point at 2^20 | 0x80465314 | exact direction (port) |
| F6 | Light colour `fctiwz` without clamp (wraps > 1.0); ambient x ka wraps the same way | 0x80465368, 0x80466484 | saturate before quantising, or keep float |
| F7 | 8-bit channel / env-coefficient quantisation (`trunc(c x 255.9)`) | 0x804A4C30 | float |
| F8 | Lighting per vertex (Gouraud) on low-poly geometry | 1.3 | optional per-pixel evaluation of the same equation (would change the look: opt-in only) |
| F9 | Unrenormalised normals for scaled / morphed meshes (if the GX XF does not renormalise) | 0x804960FC, 0x80444AC4 | inverse-transpose + normalise (the port normalises already) |

## 7. Port defects and deviations, ranked

1. **Local (point/spot/soft-spot) lights are never collected** (pipelines.cpp:108-147). The shader handles points but they are never filled (frame_math.h:301); spots have no shader path. Impact depends on runtime use (open question Q1).
2. **BUMPENVMAP not rendered** (2 materials; pipelines.cpp:535-579 has no case 3).
3. **MatFX env-map frame ignored** (pipelines.cpp:546-553). Matters only if the game sets frames (Q3).
4. Directional cap 4 vs GX 8 (frame.h:117), latent.
5. Non-root light frames read without an LTM sync (pipelines.cpp:132-133), latent.
6. Precision deviations (quantisation, far-point directional, fog z): deliberate.

## 8. Open questions (need a runtime capture or Dolphin)

- Q1: Does any stage or event create a point / spot / soft-spot RpLight (dynamic slots 2..7 via 0x80048A6C, or 0x8045BB6C stream reads)? Capture: count world-sector light lists (sector+0x40) per frame, or the light count at 0x8046579C.
- Q2: Fog colour lane order (F4): in Dolphin, stg0402 records give bytes 80 B4 FF. Is the fog sky-blue (0x80B4FF) or yellow-green (R 0xB4, G 0xFF, B 0)?
- Q3: Is `RpMatFXMaterialSetEnvMapFrame` ever called (fx+0x00 of an ENVMAP data non-zero at draw)?
- Q4: Does the GX XF renormalise transformed normals (scaled-object brightness)? Compare a scaled lit prop in Dolphin against the hardware references.
- Q5: Which 2 materials are BUMPENVMAP, and what does the indirect stage look like (0x804A4E00 TEV dump)?
- Q6: The LightColli shape kinds (+0x3C 0..2) and the exact field holding the target slot: one LightColli object dump.
- Q7: The meaning of the 0x8057E760 byte copy and flag +0x1809 written by TFogSet (clear colour?).
