# Every visual effect of the game, against the port (2026-10-27)

A static dissection of the PAL main.dol (GUPP8P), made with tools/doldis.py and tools/dolxref.py and the RTTI class
list (tools/guest_classes.py `classes()`: 1,887 classes with vtables). Each effect was compared with the port's
native renderer (runtime/lift/render, its shaders) and the native RenderWare driver layer (runtime/scaffold/rw_driver).
Nothing in this document was measured at runtime. Open questions that need a capture or a Dolphin check are listed
per family.

The evidence, with every address and every port file:line, is in five family reports:

| report | covers |
|---|---|
| [effects/screen.md](effects/screen.md) | the frame's render order, effect buckets, DrawBlur + SaveScreen, DrawScreenColor, heat haze, refraction, fades |
| [effects/particles.md](effects/particles.md) | the PJS effect engine (banks, emission, physics, affectors, RNG), PTank, rain, player effects |
| [effects/lighting_materials.md](effects/lighting_materials.md) | stage lights, the GX lighting equation, MatFX, UV / pattern / morph animation, fog |
| [effects/flare_trail_shadow.md](effects/flare_trail_shadow.md) | lens flare, rays, Locus trails, beams and lasers, Kage shadows, footmarks |
| [effects/census.md](effects/census.md) | the GX SDK census, every EFB copy, the 59 self-drawing functions, the model-name tag table |

The glare (bloom) has its own document, [GLARE_DISSECTION.md](GLARE_DISSECTION.md). The lens flare's earlier
dissection is docs/boundary-20261007/FLARE.md.

Evidence levels follow NORTH_STAR_COMPACT: P(ROVEN), S(TRONGLY SUPPORTED), M (STRUCTURAL MATCH), U(NKNOWN).

## 1. What the game is made of (headline findings)

1. **Game code almost never touches GX.** Every visual GX call comes from GXInit, the RenderWare GameCube driver or
   the disc-error screen. The only exceptions are two `GXSetBlendMode(LOGIC, SET)` calls in the atomic shimmer pass
   (0x804382A4, 0x804385D8) (P, census 0).
2. **No indirect-texture distortion.** Indirect TEV is used only by MatFX bump mapping (0x804A4E00) and by the GCN
   native-shader material. The heat haze, the shimmer and the refraction are all screen copies redrawn on displaced
   geometry (P).
3. **Only 59 functions draw by themselves** (render states, Im2D, Im3D). Every other effect draws through the
   RenderWare pipelines or the PJS effect engine (P, census 3).
4. **All effect LOGIC is still translated.** Only the draws reach the native renderer, through the lowered driver
   sites. Native owners exist only for the lens-flare layout (runtime/game/game_owners.cpp:201) and the attached
   effects' SetMatrix fix (native_attached_effects.cpp).
5. **There is no luminance threshold anywhere.** What blooms is decided by being drawn into the glare sources: the
   model tags `_G_` / `_GL_`, effect bucket 5, and the lens flare's pass 0 (GLARE_DISSECTION.md).
6. **The engine has one MT19937 random state** (0x805E1894), shared by effects and gameplay, with about 176 call
   sites. A native particle system must draw exactly the same random numbers until effects get their own generator
   (P, particles 1.8).

## 2. The frame's pass order (P, screen 1)

`0x801E4B0C` builds 23 render levels from the table at 0x804D24EC. Levels 1..13 run once per split-screen viewport
(RenderDiv). PostEffect and the 2D levels run once over the whole screen (RenderStatic).

| # | level | what draws there |
|---|---|---|
| 0 | PreRender | clear; EffectDeleteResourceTask |
| 1 | OpeqWorld | landscape layers `_N_`, `_D_`, `_OS_`, `_OSL_`, `_O_`, `_OW_`, `_OL_`, `_P_`; **full-screen heat haze** (child of LandDispManager_N: copied right after layer N) |
| 2 | Opeq | effect bucket 0 (opaque) |
| 4 | Punch | effect bucket 1 (punch-through) |
| 5-6 | TransWorld / Trans | landscape `_AF_`, `_A_`, `_AL_`; event actions; **Kage shadows**; effect bucket 2 (alpha) |
| 7-8 | AddWorld / Add | landscape `_K_`, `_M_` (inverse colour); effect buckets 3 (add) and 4 (sub) |
| 9 | GlareWorld | landscape `_G_`, `_GL_`; **PreGlare**; **lens flare** (pass 0: glow, ghosts and flash, as glare sources); effect bucket 5 in its own camera bracket |
| 10 | Glare | glare-flagged objects (e.g. ElecSearchLight) |
| 11 | PostGlare | **PostGlare** composite; **DrawBlur** (trail); effect bucket 6 (refraction elements) |
| 12-13 | Sprite3D / Last | world-anchored 2D |
| 22 | PostEffect | **SaveScreen** (the trail's history); **DrawScreenColor** (multiply, overlay, inversion) |
| 14-17 | Sprite, OnFade, Gindows, SpriteLast | HUD; **fade curtain** + logo, loading screen; windows |

Effect buckets: each EffectDraw*Task exec is `fn_804261C0(manager, bucket)`. It walks the manager's single element
list and draws an element (vtable+0x10) when node+0x30 equals the bucket. The bucket walkers are 0x8002774C (0)
through 0x8002761C (6). There is no depth sort: elements draw in launch order, and particles in a PTank go oldest
first (P, particles 1.9).

## 3. The inventory

Port verdicts: EXACT, DELIBERATE DEVIATION (documented, intended), DEFECT, NOT RENDERED, or UNKNOWN (needs a capture).

### 3.1 Frame and screen effects

| effect | entry points (DOL) | data / constants | port | verdict |
|---|---|---|---|---|
| Glare (bloom) | PreGlare 0x80430D64, PostGlare 0x80430904, ladder 0x8042F258, blur 0x804302EC | G = manager+0x3C; levels 128² / 64²; gain trunc(byte·0.95), cascade raw | renderer.cpp add_glare, glare.hlsl, scaffold glare.cpp | EXACT except the capture filter: DELIBERATE DEVIATION (area average instead of one aliasing bilinear tap) - GLARE_DISSECTION.md |
| Motion trail (DrawBlur + SaveScreen) | DrawBlur exec 0x801D27FC → 0x8042E254; SaveScreen 0x801D2764 | B = manager+0x40 (0x13C bytes): +0x01 alpha, +0x04 zoom, +0x74 W/2×H/2 RGB888 history, +0x78 valid; SRCALPHA/INVSRCALPHA, nearest | screens.cpp trail_save / trail_draw: History source, full-resolution 16-bit history, bilinear | DELIBERATE DEVIATION (resolution, precision, filter); recursion, weight and zoom EXACT (S). Stomp and restore quads not reproduced |
| Trail trigger | Player::Behavior::ChaosControl → PlayerEffect "Slow": zoom 1.05, alpha ramps to 64/255 over 0.5 s; Continue / Goal / Restart turn it off | | translated | EXACT (logic translated) |
| Screen colour | DrawScreenColor exec 0x801D2878 | C = manager+0x44: multiply (screen copy × colour), untextured SRCALPHA overlay, inversion INVDESTCOLOR/ZERO (Slow sets the inversion: the picture goes negative) | screens.cpp screen_multiply (a DestColor blend); overlay and inversion through immediate.cpp | multiply EXACT (S, by blend instead of a copy); overlay and inversion EXACT (M) |
| Heat haze (full screen) | draw 0x801D25F0 → 0x804369CC; grid update 0x801D2564 → 0x80437178 | H = manager+0x48: Nc × Nr grid; R[r] = A_r·sin(2π(p1/T1 + rH/(Nr·L_r))), the same per column; border vertices pinned; positions move, UVs stay; NEAREST | screens.cpp screen_snapshot + shimmer_draw: Snapshot source, linear | EXACT in order and maths (S); DELIBERATE DEVIATION in filter |
| Refraction elements | EffectShimmerPJS draw 0x8043531C (bucket 6) | copy 0x80435FA4, patch 0x804360D4, depth-tested | screens.cpp refraction_draw | EXACT (M) + DELIBERATE DEVIATION (linear) |
| Atomic shimmer | 0x80438168, 0x804382F8, 0x8043873C | a GX LOGIC SET mask pass and three camera copies | the LOGIC SET is a dead pipe write; the copies are counted only (immediate.cpp:396) | **probable DEFECT** (census 6.3-6.4): needs a capture |
| Fade curtain | FadeTask 0x80044D7C, FadeDrawTask 0x80044E08 → 0x800448C8; FadeLogo.txd | ten 64×480 columns at z 0.1, level trunc(255·F+0), logo after 0.4 s black | immediate.cpp; laid out as one ScreenCover group (hud_layout.h:55-58) | DELIBERATE DEVIATION (covers any aspect) |
| Model fade-in | port-only (gekko/appear_fade.h) | | dither dissolve | not an original effect (default off, --fade-in-ms) |
| Watch camera noise (GUN Fortress CCTV) | WatchCamera::Monitor 0x80268660 → fn_8040C770 | BS0600CAMERANOISE.DFF unit quad 1.1 before the eye, scaled 2 × 1.1 × window; s0501_sa029 scanline + UVA scroll | lowered site 0x8026872C → game_owners.cpp watch_camera_sheet_scale | EXACT at 4:3; DELIBERATE DEVIATION wider / narrower (covers the displayed picture, ASPECT_INVENTORY E3) |

### 3.2 The PJS effect engine (particles, rays, trails, effect models)

| effect | entry points | data / constants | port | verdict |
|---|---|---|---|---|
| Banks and launch | Launcher 0x8030583C, launch 0x80426680; bank table 0x804D1344 | 0 CommonEffect.one, 1 diabloneffect.one, 2 SShadowEffect.one, 3 per-stage boss table 0x8053DDC0; stage bank stg%04d_fx.one | translated | EXACT. The "top nibble = bank" rule of older notes is refuted: the bank is chosen explicitly (P) |
| Time base | 0x801D2318 | dt = (60 / Hz) × frames: 1.2 per frame at PAL 50 Hz | translated | EXACT |
| Emission / physics / affectors | update 0x80427E2C, emit 0x80428338, step 0x80428044, affector bytecode 0x80429224 (17 ops) | explicit Euler; drag per update; colour keys in 8.22 | translated | EXACT |
| Billboard particles (PTank) | EffectParticlePJS draw 0x80434824; PTank callback 0x80449A74 | dataFlags 0xA7; blend from data+0x50: normal SRCALPHA/INVSRCALPHA or ONE/ZERO, add SRCALPHA/ONE or ONE/ONE, multiply DESTCOLOR/ZERO | pipelines.cpp:878-970 → particles.hlsl | EXACT, except rotation: exact sincos instead of the GC instancers' parabolic sine (DELIBERATE DEVIATION) |
| 3D particles | EffectParticle3DPJS draw 0x80433DA8 | Im3D quads or world atomics | immediate.cpp / pipelines.cpp | EXACT (M) |
| Effect models | EffectObjectPJS draw 0x80433AA0 | effect-template DFF + UVA (e.g. EF_LASER01-03) | atomic pipeline | EXACT (M) |
| Light rays | EffectRayPJS draw 0x8042D870 | one untextured camera-facing quad per ray, start → end colour in 10.22, 1e-4 fallback along the view | immediate.cpp:318 → immediate.hlsl | M |
| Trails (Locus) | EffectLocusPJS draw 0x8043B618, builder 0x8043AB68 | 2 vertices per sample, history shifted per tick, optional camera-facing (flag 0x4000); UVs computed, never submitted | immediate.cpp:318 | M |
| Belt ribbons | Test::EffBelt draw 0x802DE4BC; EffLocusMultiVtx (katana/sticks) | additive SRCALPHA/ONE Im3D strips | immediate.cpp | M |
| Rain | RainEffect (the RTTI row 0x805F42A4 is the string "Task", not a vtable) | a camera-glued model, box from the guest camera's view window (0x800D51E0) | atomic pipeline | M; 4:3 bound (flaw F-R) |
| Footmarks | PlayerEffect Footmark | an effect instance per ground surface (table 0x80522900); killed and respawned on a surface change | effect paths | M |

### 3.3 Lights, materials, animation, fog

| effect | entry points | data / constants | port | verdict |
|---|---|---|---|---|
| Stage lights | loader 0x80048738 → 0x8004867C; slot switch 0x80048308 (mgr+0x454, default slot 4 restored by 0x800477A8) | stgNNNN_light.bin: 16 × 0x34 records (ambient RGBA, directional RGBA, rotation X / Y in degrees), one per LightNo slot; LightColli volumes and SetLight commands pick the slot | pipelines.cpp:108-147 reads the RpLights live at each draw | EXACT for ambient + directional |
| GX lighting | 58 GXSetChanCtrl sites | per vertex, DF_CLAMP, AF_SPOT; no specular anywhere; directional = point at 2^20; point lights DA_MEDIUM | mesh.hlsl (equation, clamp points, two-stage TEV) | EXACT (precision: DELIBERATE) |
| Local lights | sector light lists (0x80465438, cap 8) | point, spot, soft spot | never collected; LightSet::points never filled; spots have no shader path | **DEFECT if the game creates any** (U: open question Q1) |
| MatFX environment map | dispatch 0x804A57D0, texgen 0x804A4B7C | coefficient, texture matrix −0.5·M + 0.5 | pipelines.cpp:535-579 → ExtraPass Environment | EXACT; the env-map frame (fx+0x00) is ignored (matters only if the game sets one: U) |
| MatFX dual / UV transform | same | | ExtraPass Dual, uv_transform | EXACT |
| MatFX bump / bump-env | 0x804A4E00 (indirect TEV + KColor) | 2 materials | base pass only | **NOT RENDERED** |
| UV animation | TextureUVAnimManagerTask exec 0x801EB790 (RpUVAnim) | | translated; the result is read at draw time | EXACT |
| Texture pattern animation | 0x8004F284 / 0x8004F0FC → RpMaterialSetTexture 0x8045C2C0 | authored 1..8..1 pulses | translated | EXACT |
| Delta morph | DeltaMorphAnimManagerTask; render callback 0x80444AC4 | | blended on the GPU, normals renormalised | EXACT (renormalisation: DELIBERATE) |
| Fog | TFogSet exec 0x8016A904; stgNNNN.fog: 24 × 0x28 records (zone mask, near, far, on, plane, colour) | the active zones averaged, smoothed 0.98/0.02 per tick; also moves the camera's clip planes; GX_FOG_PERSP_LIN only, no range adjust | recipe.fog, planar eye-depth linear fog | EXACT (precision differs); fog colour's blue lane is always 0 in the original too (U: Dolphin check) |

### 3.4 Lens flare, beams, shadows

| effect | entry points | data / constants | port | verdict |
|---|---|---|---|---|
| Lens flare | builder 0x80432360 (native owner game_owners.cpp:201, lens_flare.h); tasks 0x801D2928 and 0x801D2A94 ("Another": pass 0 only, in GlareWorld) | common/LensFlare.one, LENSFLARE.TXD; occlusion = the 128×128 glow quad's depth test at the sun's depth, nothing else | native layout, translated draw | EXACT at 4:3; DELIBERATE DEVIATION wider (FLARE.md) |
| SecurityLaser | draw 0x802309F4 | one model drawn 16 times at 22.5° steps, frustum radius 302.5 | atomic pipeline | M |
| Other lasers, searchlight | BulletLaser, WeaponLaserBase, SatelliteLaser, ArkLaser, Diablon beam, ElecSearchLight | probably effect-template models; draw site not found | atomic pipeline | U |
| Kage shadows | KageTask exec 0x80357A78, draw 0x80357578; probe 0x80079994 / 0x800938A4; placement 0x80357260 | one ray 300 units down the character's up axis; model at the hit + 0.2 along the normal, aligned unless dot ≥ 0.9995, scale (4, 1, 4); very likely SHDVOL.DFF (common/VolumeShadowModel.one, a 16-sided prism, grey 102); drawn in Trans with no state change | atomic pipeline | M; how the grey prism darkens the ground is U |

## 4. Defects and gaps in the port (ranked)

1. **Atomic shimmer** (census 6.3-6.4): the LOGIC SET mask pass draws with the last RenderWare blend instead of solid
   white, and its three camera-texture copies are never filled. Probable DEFECT; needs a capture of a scene that uses it.
2. **Local lights never collected** (lighting 7.1): point and spot lights in the sector lists are dropped. Only a
   defect if the game creates any (Q1).
3. **MatFX bump / bump-env not rendered** (2 materials): base pass only.
4. MatFX env-map frame ignored; directional cap 4 against GX 8; non-root light frames read without an LTM sync. All
   latent.
5. tools/stage_preview/stage_scene.cpp gives `_M_`, `_AL_`, `_G_` and `_GL_` the wrong recipes against the table at
   0x8051E328. Tool only; the runtime is right.

No change was made for any of them: each is a behaviour change and waits for the user's decision.

## 5. The original engine's flaws, with correct-math replacements (proposals only)

| # | flaw (evidence) | replacement |
|---|---|---|
| G1 | Glare level A is captured with one bilinear tap per texel: small glowing things shimmer (GLARE_DISSECTION) | area-average capture (already native) |
| G2 | Glare chain in 8-bit EFB copies, every step clamps and rounds; level B clips above ~54/255 | keep UNORM8 saturation where it is the look (level B clipping); float elsewhere |
| T1 | Trail: half-resolution 8-bit point-sampled history; round-to-nearest at 64/255 leaves a residue that never decays | full-resolution 16-bit history (already native) |
| T2 | Trail weight and zoom applied per frame | w = 1 − (1 − a/255)^(dt·60), zoom^(dt·60) if presentation ever leaves 60 Hz |
| T3 | Stomp-quad UVs may drift the history ~0.5 px per generation (U) | no stomp; sample at pixel centres (already native) |
| H1 | Heat-haze amplitudes and wavelengths are in 640×480 pixels; on a wide view the horizontal warp grows with the aspect | express A and L in view-height units, lay the grid out isotropically |
| R1 | Fade curtain hard-coded 640×480 | Fill over the view (already native) |
| P1 | Particle drag per update, not per dt (0x80428298): PAL 50 Hz particles slow 17 % less per second | v ×= (1 − d)^dt |
| P2 | Emission truncates rate × dt once it reaches 1 (0x804283C4): rate 1.5 emits 1 | fractional accumulator per emitter |
| P3 | Explicit Euler, gravity after the position step | exact constant-acceleration step |
| P4 | Colour keys skip their first update, truncate each step, never snap to the target | c(t) = lerp(start, target, saturate(t / duration)) in float |
| P5 | Effects and gameplay share one MT19937 | a per-effect generator; until then a native owner must consume identical draws |
| P6 | Random float single(u32·2^-32) can round to exactly 1.0 | (u32 >> 8)·2^-24 |
| P7 | Rotation wrap by repeated ±2π can loop forever on huge or infinite angles | remainder(r, 2π) once |
| P8 | No depth sort for alpha-blended particles | per-bucket back-to-front sort, or weighted blended OIT for the trans bucket |
| P9 | Hard particle/geometry intersections | soft particles: alpha × saturate((scene depth − particle depth) / k) |
| P10 | Fog colour added to additive and multiply particles | fog toward the blend's neutral (0 for adds, 1 for multiply) |
| P11 | Rain box sized from the guest camera's window (4:3 bound) | the active viewport's real half-width |
| P12 | 8-bit colour out of an 8.22 state: dark additive fades band | float colour |
| L1 | Fog zones smoothed 0.98/0.02 per tick (frame-rate dependent) | α = 1 − 0.98^(dt·60) |
| L2 | GX fog depth from 24-bit z: banding at far planes of 30000+ | eye-space depth (already native) |
| L3 | Light colour quantised with fctiwz and no clamp: values above 1.0 wrap | saturate before quantising, or float |
| L4 | Per-vertex (Gouraud) lighting on low-poly geometry | optional per-pixel evaluation of the same equation (changes the look: opt-in only) |
| L5 | Unrenormalised normals on scaled or morphed meshes (if the GX XF does not renormalise) | inverse-transpose + normalise (already native) |
| F1 | Lens flare: the ghosts and the flash ignore occlusion; only the glow is clipped, with a hard per-pixel edge | visibility fraction from 16-32 depth taps around the sun, smoothed with a time-based exponential; multiply glow, ghosts and flash |
| F2 | Lens flash level is an 8-bit integer feeding the glare's q8 capture (the Sky Troops blink chain) | float flash level, applied after the glare |
| Y1 | Rays: one hard-edged quad, popping flip when seen along the axis | capsule / camera-aligned impostor with soft edges and depth fade |
| Y2 | Locus trails: history per tick, linear kinks at fast turns, 8-bit colour ramps, double-sided self z-fighting | time-based history, spline (Catmull-Rom) sampling, float colour |
| Y3 | SecurityLaser: 16 planar slices alias edge-on and brighten toward the axis | one camera-facing cylinder impostor |
| S1 | Kage shadow: constant 0.2-unit offset against z-fighting fails at distance | slope-scaled depth bias, or a deferred decal reading scene depth |
| S2 | One ray, one normal: the shadow floats or clips on steps and curved ground | screen-space decal projected through the volume, or a per-character shadow map |
| S3 | No fade with height: full shadow up to 300 units, then it pops off | alpha and size by hit distance (smoothstep) |
| S4 | 0.9995 dot threshold: no tilt below ~1.8°, visible snap on gentle slopes | always build the frame from the normal |
| S5 | Footmark effect killed and respawned on a surface change (a visible pop) | cross-fade between surface effects |

Deviations from the original must be confirmed against Dolphin before they become defaults (user rule).

## 6. Layer switches (viewing tool, 2026-10-27)

Every effect above can be switched off or soloed at runtime. It is a viewing tool: the game runs the same, and only
the packet the renderer draws is filtered. With every layer on, the packet is untouched.

**Layers** (`--list-effect-layers`): glare, lens-flare, motion-blur, heat-haze, refraction, screen-colour, screen-fade,
particles, particles-3d, effect-models, rays, trails, shadows, land-unfogged, land-fogged, land-alpha, land-additive,
land-darken, land-glow, lighting, baked-light, fog, environment-maps, dual-textures, appear-fade.

The six land layers are the world sectors of the landscape's render classes (the model-name tags of the table
0x8051E328, census 4): unfogged = `_N_` `_D_` (in Westopolis the tunnels as well as the burning sky), fogged = `_O_`
`_OS_` `_OSL_` `_OW_` `_OL_` `_P_`, alpha = `_AF_` `_A_` `_AL_`, additive = `_K_`, darken = `_M_`, glow = `_G_` `_GL_`.
A sector drawn under the class draw 0x800471BC takes its class from the flags the level recipe 0x80046D80 holds in
r31 when it sets its render states. The same draw renders the class's models after its sectors (characters and
props carry these classes too); those stay world.

**How a draw knows its layer**
- **From the packet itself:** glare is its passes (Glare and GlareSources; an untagged glare source is the world's
  `_G_` geometry). Lighting off gives a lit draw a neutral white ambient light instead of its lights, so the shading
  goes and baked vertex colours stay. Baked light off draws a prelit draw's vertex colours white in RGB and keeps
  their alpha (mesh.hlsl kUnbaked), so the stage's prelighting goes and its see-through fades stay. Fog off clears
  the recipe's fog. Environment / dual off drop the material's
  extra pass. Appear-fade off clears the dissolve.
- **From the driver site:** the trail, heat haze, refraction and screen multiply are tagged by their own sites in
  screens.cpp.
- **From the guest call chain** (scaffold/rw_driver/layers.cpp): the innermost return address that lies in a known
  effect function names the layer. Those functions are the PJS element draws, the screen-task execs, the fade draw,
  the Kage draw, the lens-flare tasks, EffBelt and the atomic shimmer. The chain is walked only while a switch is set
  (the guest back chain, as asset_trace.cpp walks it).

**Solo** draws only the soloed layer; the other effect layers, modifiers included, are off. **Isolate** (Num ., or
`--effects-isolate`) also hides the world, so the effect is seen alone. For a modifier layer (lighting, fog,
environment, dual) isolate keeps only the draws that carry it.

**Controls**
- **Window**, on the numeric keypad with Num Lock on: Num / shows or hides the layer list. While it shows: Num 8 /
  Num 2 move, Num 5 switches the layer, Num 0 solos it, Num . hides or shows the world in a solo, Num - switches
  every layer on. F6, Page Up / Page Down, Insert, Home, End and Delete do the same. None of these keys reaches the
  game (with Num Lock off the keypad sends arrows, which the game reads). While the list is hidden and a switch is set, one status line says what is not drawn.
- **Command line:** `--effects-off glare,fog`, `--effects-solo particles`, `--effects-isolate`.
- **Launcher:** the Effects box.

Captures and snapshots are taken with the switches applied and without the overlay.

**Known limits**
- A 2D strip drawn across a layer change is flushed at the change, so it keeps the layer it was drawn in.
- An effect whose draw function is not in the table (the lasers' and searchlight's draw sites are U) counts as world
  until it is identified.
- UV and texture-pattern animations are not switchable: they are the game's logic, not a draw.
