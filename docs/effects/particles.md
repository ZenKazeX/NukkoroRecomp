# PARTICLES family - static dissection (2026-10-27)

Scope: the PJS effect engine (manager, resources, elements, particle physics, affector bytecode, buckets), RpPTank and
its GameCube render path, the other particle users, and the port side. Static analysis only (doldis / dolxref /
DOL word reads); no build, no run. r2 = 0x805FA780, r13 = 0x805EC500 (tools/dolxref.py:66). RW blend enum:
1 ZERO, 2 ONE, 5 SRCALPHA, 6 INVSRCALPHA, 9 DESTCOLOR. RW render-state ids: 1 TEXTURERASTER, 9 TEXTUREFILTER,
10 SRCBLEND, 11 DESTBLEND, 12 VERTEXALPHAENABLE, 14 FOGENABLE, 20 CULLMODE.

Evidence levels: P = PROVEN (read from the code/data word cited), S = STRONGLY SUPPORTED, M = STRUCTURAL MATCH,
U = UNKNOWN.

**Headline: every piece of particle LOGIC (launch, emission, physics, affectors, random, sorting, fill) is still
translated guest code. Only the DRAWS reach the native renderer: PTank batches (pipelines.cpp particle_batch ->
particles.hlsl), 3D particles/objects as atomics (mesh path) or Im3D quads (immediate.cpp). The one native owner in
this family is the attach fix for SetMatrix 0x80426EB8 (native_attached_effects.cpp, a deliberate deviation).**

---------------------------------------------------------------------------------------------------------------------

## 1. The PJS engine

### 1.1 Classes (RTTI, rtti_classes.tsv; slot list starts at vtable +0x08)

| Class | vt | +0x08 dtor | +0x0C update(f1 = dt) | +0x10 draw | Role |
|---|---|---|---|---|---|
| EffectManagerPJS | 8053D65C | 800A3664 | 80426680 Launch | 80426644 | singleton at r13+0x2DD8, got by 0x800A35D8 (P) |
| EffectSystemPJS | 8053E920 (real table at 0x8053E93C, stored by ctor 0x801E293C) | 80028878 | ... | ... | services: +0x20 = random 0x80028754, +0x24 = 0x80028730; pool switch obj+4 (P) |
| EffectArrangerPJS | 8053D924 | 801D1434 | 8042736C | 800273E4 = blr | composite/timeline; child commands 0x11/0x12 (P) |
| EffectParticlePJS (= RW3/USA variants) | 8053D998 | 801D149C | 800282AC -> 80434B6C -> 80427E2C | **80434824** (PTank fill+render) | billboard particles (P) |
| EffectParticle3DPJS | 8053DA8C | 801D1844 | 80027DC4 -> 804341D0 | **80433DA8** | oriented quads (Im3D) or per-particle atomics (P) |
| EffectObjectPJS | 8053DA18 | 801D1504 | 80027AB4 -> 80433B50 | **80433AA0** | model elements (RpAtomic, world add/remove + RpAtomicRender 0x80457C14) (P) |
| EffectGlobalParamPJS | 8053D7C8 | 801D12F8 | 801FA5EC -> 80439A8C | 801D1360 = blr | timeline for screen params (flare 0x80431910, glare 0x8042F0E8, shimmer 0x8043996C); NOT a particle - screen family (P) |
| Launcher | 8055C0EC | 8030583C | - | - | boost-held launch handle (dtor only) (P) |
| Loader | 8055C0A0 (Task) | 80305704 | 8030569C | - | waits for the archive (state 3) then binds it (P) |

Element common header (P, from 0x804261C0 / 0x80427E2C / 0x80434824): +0x0C age (+= dt), +0x18 flags (bit0 alive,
bit2 running, bit3 drawing-guard, bit4 emitting), +0x30 bucket, +0x3C next element, +0x44/+0x120/+0x124 data
pointers, +0x48/+0x4C/+0x50 follow matrix/position/other (native_attached_effects.cpp:20-22), +0x54 emitter matrix,
+0x104/+0x108 flags, +0x110 live particle count, +0x114 gravity, +0x118 drag, +0x11C emission rate, +0x12C affector
list, +0x134/+0x138 pools, +0x13C RpPTank atomic (ParticlePJS) or record array (3D), +0x144 skip-render flag.

### 1.2 Resources (effect archives on disc) - P

Bank table 0x804D1344, 16 bytes per row {path, per-stage list, min, max}, read by the bank loader 0x801D3188
(slots 0x80582454 + 8 x bank, ref-counted at slot+0x10; release 0x801D30B4):

| bank | archive | callers (li r3, n before bl 0x801D3188) |
|---|---|---|
| 0 | common/CommonEffect.one | 0x801783D4, 0x801E28C8 (boot), ... |
| 1 | enemy/boss/diabloneffect.one | 0x802D91DC |
| 2 | common/SShadowEffect.one (Super Shadow) | 0x8016AE2C, 0x8027F3F0, 0x8028719C |
| 3 | per stage: table 0x8053DDC0 indexed by (0x8057E760+0xAC) - 0x1C, valid 0x1C..0x2C: Boss0210/0310/0510, BossBKlast, diabloneffect, EggLastMechEffect, BossDevilDoom .one | 0x8017844C |
| stage | stg%04d/stg%04d_fx.one (0x804C3F74), built at 0x80169294, held at r13+0x3DE8 | stage load |

Lens flare uses common/LensFlare.one + LENSFLARE.TXD (0x804D0EF0/0x804D10CC), not this family. Archive loading:
0x803054B8 allocates a 0x28-byte Loader and a 0x14-byte holder (0x80305894), polled by Loader::exec 0x8030569C.

**Effect IDs (correction to the brief's hypothesis):** no "top nibble = bank" decode was found. Launch 0x80426680
takes an explicit resource (r28) and walks resource->+0x2C list (next +0x28) comparing entry+0x24 == id (r29)
(0x8042670C..0x8042672C, P). Error codes in manager+0x28: 1 no resource, 4 id not in bank, 5 element create failed.
Wrappers 0x801D2E9C/2ED8/2F14/2F50/2F8C/2FC8 take the bank index in r4 (slwi 3 into 0x80582454, P). Gadget
particles choose the bank by PTCLCATEGORY: 0 -> stage bank (0x8016902C uses r13+0x3DE8), 1 -> bank 0 via 0x8015DBC0
(0x801675DC..0x80167638, P). A nibble encoding may exist in some caller's own tables (U).

### 1.3 Time base - P

EffectTask exec 0x801D2318: `dt = (60.0 / hz) * min(G+0x0C, G+0x34)` with G = 0x8057E760, hz = G+0x50, 60.0 at
0x805F6E50. G+0x0C is the frame count n handed to 0x80203D20 by the frame limiter (native_frame_limiter.h:24-26, S).
So the effect clock is **per-dt in units of 60 Hz frames**: dt = 1.0 per 60 Hz frame, 1.2 per 50 Hz (PAL) frame,
k when k frames elapsed (capped). Manager update 0x804262E4(f1 = dt) calls every element's +0x0C with that dt.

### 1.4 Element update (0x80427E2C) - P

age += dt; dies when age > life(data+8) + 1e-4 (0x8051BC54) unless flag 0x108 bit0; then affectors (+0x12C list,
vtable +0x0C(dt), removed on false), emitter matrix 0x80426C84 (follows +0x48 etc.), emission 0x80428338 when
flag bit4, particle step 0x80428044. Element bucket switches to 5 (glare) while flag 0x104 bit1|bit2 (0x800282D8..
0x80028318: saves bucket at +0x148, restores) - a particle can be a glare source.

### 1.5 Emission (0x80428338) - P

| Step | Code | Math |
|---|---|---|
| count | 0x80428388..0x804283CC | n = rate(+0x11C) x dt; if n < 1: emit 1 with probability n (random < n); else **n = trunc(n)** (fctiwz) - the fraction is discarded |
| allocate | 0x80428410..0x804284B0 | from pool +0x138 (free list 0x8042B55C); kind by data+0x20 (2 or 5 -> ctor 0x80429708 / 0x8042965C); appended at the TAIL (+0x100) |
| speed | 0x804284CC..0x804284E0 | v = data+4 + data+8 x rand |
| shape (data+0x14 -> +0) | 0: 0x80428528 | cone/sphere: angles from +0xC (evenly spaced when flag 0x40 and n > 1, else random, centred: angle - 0.5 range), radius +8 (x rand unless flag 0x80), +0x10 second angle; sin 0x8000FFE8 / cos 0x8000F3B0; position = dir x r, velocity = dir x v |
| | 1: 0x80428700 | ring/disk: random angle around, radius, height +0x10; flag 0x20 negates velocity (inward) |
| | 2: 0x8042881C | box: each axis (rand - 0.5) x extent (+8, +0xC, +0x10); velocity (0, v, 0) |
| | other | position = +0xB4, velocity (0, v, 0) |
| to world | 0x804288B4..0x804288F4 | unless flag 0x800: pos = M(+0x54) x pos (0x8040C6CC), vel = M x vel (0x8040C668); positions stored in WORLD space |
| bias | 0x80428908..0x80428958 | with |+0xC0| > 1e-4: vel += axis(+0xC0) x (dot(vel, axis) / (len(vel) x |axis|)) (sign-kept) - pushes velocity along an axis |
| program | 0x8042895C | particle+0x6C = data(+0x124)+0x1C (affector bytecode, 1.7) |

### 1.6 Particle step (0x80428044) - P

| Step | Code | Frame-rate behaviour |
|---|---|---|
| age += dt; dead when age > life(+0xC) + 1e-4 | 0x80428094..0x804280C0 | per-dt |
| run the particle's affector program (1.7) and its affector list +0x70 | 0x804280E4..0x804281B0 | per-dt |
| free dead particles (pool +0x138), count-- | 0x804281C0..0x80428264 | - |
| pos += vel x dt | 0x80428288..0x80428294 | explicit Euler |
| drag: if drag (+0x118) != 0 and |v| > 1e-4: v x= (1 - drag), or v = 0 when drag x |v| > |v| | 0x80428298..0x804282EC | **per update, not per dt** |
| gravity: vel.y -= dt x g (+0x114) (fnmsubs 0x80428300) | 0x804282F0..0x80428304 | per-dt, applied after the position step |

Particle record (pool object, P from 0x80434824/0x80428338/0x80429224): +4 pos (world), +0x18 rotation (radians),
+0x1C vel, +0x28 size (w, h), +0x34/+0x38 uv0, +0x40/+0x44 uv1, +0x4C flags (bit0 dead), +0x50 age,
+0x54/+0x58/+0x5C/+0x60 colour R/G/B/A in **unsigned 8.22 fixed point** (byte = (x >> 22) & 0xFF,
`rlwinm 10,24,31` at 0x80434998), +0x64/+0x68 prev/next, +0x6C program counter, +0x70/+0x74 affector list.

### 1.7 Affector bytecode (0x80429224, jump table 0x8056B5E8, 17 entries) - P for the table, M for semantics

Command: byte0 opcode, byte1 "continue" (0 ends the program), size per opcode (r5). Each opcode allocates an
affector from the current pool bound to one particle field; affector vtable +0x0C(dt) runs per step.

| op | ctor | field | size | meaning |
|---|---|---|---|---|
| 0, 2, 9, 10 | - | - | - | marks the particle dead (0x80429558) |
| 1 | 0x804252AC | uv0/uv1 (+0x34/+0x40) | 0x10 | **texture flip-book**: grid of 2/4/8 cells (cmd+4 = 0/1/2), start cell cmd+6 (+ rand x cmd+7), frame count cmd+8, interval u16 cmd+0xC (+ rand x cmd+0xE) in 60 Hz frames; u = (cell mod g)/g, v = (cell div g)/g |
| 3 / 4 / 8 | 0x8042561C | +0x04 pos / +0x10 / +0x1C vel | 0x24 | vec3 tracks (vtables 0x8056B13C / 0x8056B110 / 0x8056B0E4) |
| 5 | 0x80425BD0 | +0x18 rotation | 0x14 | spin (vt 0x8056B65C) |
| 6 / 7 | 0x80424D48 / 0x80424A30 | +0x28 size | 0x24 / 0x1C | scale over life |
| 11 | 0x8042446C | +0x54 RGB | 0x14 | colour key: target bytes cmd+0xC..E (+ rand x cmd+0xF..0x11, truncated), absolute or relative (cmd+4), duration u16 cmd+8 (+ rand x cmd+0xA) frames; 8.22 values clamped to [0, 255<<22] (0x3FC00000), values below 1<<22 snap to 0; per-frame step = trunc((target - cur) / duration) |
| 12 | 0x80424050 | +0x60 alpha | 0x10 | alpha key, same scheme |
| 13, 14, 16 | 0x80423E84 / 0x80423D80 / 0x80423B90 | - | 8 | wait / misc (U) |
| 15 | 0x80423BF8 | - | 4/8 | jump (rewrites +0x6C: loops) |

Colour affector update 0x80424324: first call only arms (byte +0x10) and applies nothing; then t += dt, stops
when t > duration (optionally kills the particle: cmd+2); else cur += trunc(step x dt) with clamps.

### 1.8 Random source - P

`random()` = EffectSystemPJS +0x20 = 0x80028754 -> 0x8040FEE0 -> **MT19937** 0x8040FB60 on the global state
0x805E1894 (624 words: `cmplwi 0x270`, `li 0xE3`, tempering masks 0x9D2C5680 / 0xEFC60000), result
u32 x 2^-32 (0x8051B630) rounded to single. The same generator serves 0x8040FEE0 (69 call sites), 0x8040FCD0
(range, 81) and 0x8040FAE0 (int, 26) across gameplay (enemy code 0x8018A2B4.., gadgets, RainEffect, Jet). Not
natively owned (no hit for 0x805E1894/0x8040FB60 in runtime/).

### 1.9 Buckets, pass order and sorting - P (task/bucket), M (level mapping)

Draw tasks (exec -> 0x804261C0(manager, bucket)) and the RenderLevel slot each ctor takes (lwz r4, X(levels)):

| Task (vt) | exec | bucket | level slot -> RenderLevel (string order 0x804D22E8..) |
|---|---|---|---|
| EffectDrawOpaqTask 8053DCDC | 801D2B14 -> 0x8002774C | 0 | +0x08 -> Opeq (after OpeqWorld) |
| EffectDrawPunchTask 8053DCF8 | 801D2B90 -> 0x80027728 | 1 | +0x10 -> Punch |
| EffectDrawTransTask 8053DD14 | 801D2C0C -> 0x80027704 | 2 | +0x18 -> Trans |
| EffectDrawAddTask 8053DD30 | 801D2C88 -> 0x800276E0 | 3 | +0x20 -> Add |
| EffectDrawSubTask 8053DD4C | 801D2D04 -> 0x800276BC | 4 | +0x20 -> Add (same level, created after Add: 0x801D1BB4) |
| EffectDrawGlareTask 8053DD68 | 801D2D80 -> 0x80027640 | 5 (only when a glare camera exists, 0x8048652C) | +0x24 -> GlareWorld |
| EffectDrawShimmerTask 8053DD84 | 801D2DFC -> 0x8002761C | 6 | +0x2C -> PostGlare |

All created in 0x801D1A64. 0x804261C0 walks the manager's element list (head manager+0x14, next +0x3C) and draws
(+0x10) every element whose +0x30 equals the bucket, **in list (launch) order - no depth sort**, guarded by flag
bit3 when manager+0x24 is set. Inside a PTank the particles go out in list order, oldest first (tail append
0x80428498). The bucket names a pass, not a blend: the PTank blend comes from the particle data (1.10).

### 1.10 EffectParticlePJS: init and draw (the billboard path) - P

Init 0x80434D30: RpPTankAtomicCreate-equivalent 0x804490C4(max = +0x10C, dataFlags **0xA7** = POSITION | COLOR |
SIZE | 2DROTATE | VTX2TEXCOORDS, 0). Sets ACTNUMCHG (instFlags 0x00800000), active = 0, vertexAlpha = 1, then by
data+0x50:

| data+0x50 | flag 0x108 bit2 | src / dst | instFlags |= ALPHABLENDING |
|---|---|---|---|
| 0 normal | set | SRCALPHA / INVSRCALPHA (0x80434E90) | yes |
| 0 | clear | ONE / ZERO (0x80434ED0) | yes |
| 1 add | set | SRCALPHA / ONE (0x80434F20) | yes |
| 1 | clear | ONE / ONE (0x80434F60) | yes |
| 2 multiply | - | DESTCOLOR / ZERO (0x80434FA0) | no |
| >= 3 | - | none (PTank default) | no |

Draw 0x80434824 (needs alive, not guarded, count +0x110 > 0):
1. Lock (0x80449458, rpPTANKLOCKWRITE) clusters 1 pos, 2 colour, 4 size, 0x20 rotation, 0x80 uv2.
2. Position: flag 0x800 set -> M(+0x54) x local pos (0x8040C6CC), else the stored world position.
3. Colour bytes = (8.22 >> 22) & 0xFF per channel; size (+0x28, +0x2C); rotation wrapped to [-pi, pi] by
   `while (r > pi) r -= 2 pi; while (r < -pi) r += 2 pi` written BACK into the particle (0x804349F8..0x80434A2C,
   pi 0x805FBDB0, 2.0 0x8051C798); uv (+0x34,+0x38,+0x40,+0x44).
4. Unlock 0x804495E8; instFlags |= ACTNUMCHG; active = count.
5. Unless +0x144: save FOGENABLE, set it to (flag 0x108 bit1 ? off : on), call the atomic's render callback
   (atomic+0x48 = PTank callback 0x80449A74), restore FOG.

### 1.11 EffectParticle3DPJS draw 0x80433DA8 - P

Two modes. (a) +0x140 != 0: each particle owns an RpAtomic in a 0x98-byte record (+0x90 atomic, +0x94 in-world
flag); the draw adds it to / removes it from the world (0x8046334C / 0x8046343C, query 0x804635BC) so it is drawn by
the **world pass**, or renders it at once with RpAtomicRender 0x80457C14 when +0x148 is set. The update 0x804341D0
writes each particle's model frame from the emitter matrix (0x804342DC..0x8043431C, RENDER_FLICKER.md fourth bug)
and per-particle material colour via 0x800381C0 (0x80027E44..0x80027F0C). (b) else Im3D: CULLNONE, filter LINEAR,
VERTEXALPHA on, FOG per flag bit1, raster = texture (+0x144) or NULL; per particle RwIm3DTransform(record, 4, NULL,
0x19 = VERTEXUV | VERTEXXYZ | VERTEXRGBA) + RwIm3DRenderPrimitive(4 = TRISTRIP) (0x804340D8..0x804340FC), one
RwIm3DEnd 0x8049B95C; restores the five states. The quad corners are built in the update in 3D (oriented by the
particle matrix: not camera-facing).

EffectObjectPJS draw 0x80433AA0: the same world add/remove + RpAtomicRender pattern for model elements (P).

---------------------------------------------------------------------------------------------------------------------

## 2. RpPTank (RenderWare plugin) - the GameCube path

Layout (RW37SDK rpptank.h RpPTankAtomicExtPrv + RpPTankData; port rw_driver.h:111-130): ext = atomic +
[0x805F24F0]; +0 max, +4 active, +0x10 defaultRenderCB, +0x3C lock, +0x40 instFlags, +0x44 platFlags, +0x4C..
clusters {ptr, stride}, +0x94 userData, +0xA0 dataFlags, +0xA4 srcBlend, +0xA8 dstBlend, +0xAC vertexAlpha,
+0xC0 cRotate, +0xC4 cColor, +0xF8.. cMatrix. Pipeline word 0x805F24F8.

| Stage | Address | What it does | Evidence |
|---|---|---|---|
| render callback | 0x80449A74 | axes: CNSMATRIX (0x8000) -> constant matrix rows; else the current camera frame's LTM (0x804882E8) right/up, rotated by cRotate when CNS2DROTATE (0x20000) (sin 0x803B2AFC, cos 0x803B2594); **up x -1** (0x805FC0E8); userData(+0x94) = &axes (a stack frame of this call); vertexAlpha ? set SRC/DST from +0xA4/+0xA8 and VERTEXALPHA on : VERTEXALPHA off; call defaultRenderCB (+0x10 = AtomicDefaultRenderCallBack 0x80456F04 -> RxPipelineExecute 0x80456F3C); restore SRC/DST | P |
| pipeline callback | 0x80449D00 | lighting frame stamp only for geometry with normals (pipelines.cpp:623-627) | S (port comment) |
| GX instancers | 0x8044A25C, 0x8044A620, 0x8044A9E4, 0x8044ADAC, 0x8044B1B8, 0x8044B590, 0x8044B98C, 0x8044BDF8, 0x8044C260 | per data-format paired-single vertex writers; half size = size x 0.5 (0x805FC110); the rotating variants (0x8044B98C/BDF8/C260) use a **parabolic sine** `s = (4/pi^2) x (pi - |x|) x x` on [-pi, pi] with table 0x8051CE08 {0, pi, 0, -pi/2, 1, 1, 4/pi^2 = 0.405284703, 1} (0x8044BB94..0x8044BBDC); cos via the same with -pi/2 | P (constants), M (formula) |
| count limit | GX 16-bit vertex count: 4 x (count & 0x3FFF) | port pipelines.cpp:889-891 | S |

Why the rotation wrap in 1.10 exists: the instancers' parabolic sine is only valid on [-pi, pi], so the effect
must keep every rotation in range before each fill (S).

---------------------------------------------------------------------------------------------------------------------

## 3. Other particle users

| User | Entry points | What it is | Draw path | Evidence |
|---|---|---|---|---|
| GadgetParticle::GadgetParticleCtrl | scan 0x80167D28 (RpUserData ATTR_PTCL.PTCLCATEGORY / PTCLID / PTCLFLAG / ATTR_ID, names at 0x805F5BE0..EC), entries 0x4C bytes: +0 category, +4 id, +8 flags, +0x10 frame, +0x14 transposed LTM, +0x44 effect; launch 0x80167564 / 0x801676C8 / 0x8016782C; attach step 0x80167A00 (LTM -> SetMatrix 0x80426EB8 every frame) | emitters embedded as frames with user data in gadget models; category 0 stage bank, 1 common bank | PJS elements (1.10/1.11) | P |
| SetParticle (set object "PARTICLE", table row 0x805286D8: 0x8011E07C, 0x8011E078, 0x8011E024) | ctor 0x8011E024 -> 0x8011E350 (0x80 bytes); update 0x8011E130: state 0x8011E080 (in-range test), on change stop old effect (+0x48 bit0 clear) and launch via 0x8016902C(stage bank, id), follow = obj+0x50 | a placed stage emitter switched by player range | PJS | P |
| RainEffect (gadget "RainEffect", RAINEFFECT.BIN 0x804B844C; ctor 0x800D56EC, update 0x800D53CC) | in-area test 0x800C5C60 -> show model 0x80162B00, loop SE 0x733E (0x801D4290), timer = random(3, 6) (0x8040FCD0, 0x805F42CC/D0) counts down by f1, on expiry random one of SE 0x7340 / 0x7341 (r2-0x64E0) | rain = a model glued to the camera + thunder SFX; not a particle system | model (pipelines.cpp mesh path) | P |
| Rain box 0x800D5140 | position = camera + 1.1 x forward (0x805F42B4); scale = (2 x 1.1 x vw.x, 2 x 1.1 x vw.y, 1) with vw = camera view window (+0x68/+0x6C of 0x8000DCC0's camera), 2.0 0x805F42B8, 1.0 0x805F42BC | the model is sized to the frustum cross-section at d = 1.1 | - | P |
| RTTI note | rtti_classes.tsv lists RainEffect vt = 0x805F42A4 and SetParticle vt = 0x805F4ED0: 0x805F42A4 is the .sdata2 string "Task" (DOL bytes 0x805F42A4), not a vtable | - | - | P |
| Footmark (vt 80522A50; ctor 0x800A4738, exec 0x800A4324, dtor 0x800A4664) | at foot contact launches bank-0 effects (0x800A2BBC, id from the player data +0x28 -> +0x60 + 4) and sets position/scale (0x80426DE4/0x80426E9C) | footprints / dust | PJS | P |
| RangeEffect (80522A34; 0x800A4220 / 0x800A3F6C) | keeps an attached effect's matrix (0x800A3DA8 -> SetMatrix) | range ring/aura | PJS | P |
| Slow (80522A6C; 0x800A4AB4 / 0x800A4830) | 0x800A47B0 -> manager 0x8042F114 (screen parameter block), shimmer 0x804398F4/0x8043996C in dtor | Chaos-Control slow: a screen effect, not particles | screen family | P |
| Awake (80522A88; 0x800A51F0 / 0x800A4D2C) | aura model added/removed from the world (0x800A4C04), material anims 0x8003913C / 0x80039BBC | Hero/Dark awakening aura | model | P |
| Grind (80522AA4; 0x800A5670 / exec 0x800A55D4 = blr) | grind sound only (0x800A5430 -> SE start 0x801D4290, stop 0x801D48FC) | grind SFX holder | none | P |
| Jet (80522AC0; ctor 0x800A5B6C SHADOW_FOOTEFF_R.DFF, exec 0x800A5924 -> 0x800A57B8) | shoe-jet flame model, random flicker 0x8040FCD0, frame update 0x8048827C | model | model | P |
| Belt (80522ADC; 0x800A5E40 / 0x800A5D14) | drives a Test::EffBelt task (vt 80559FF0; 0x802DE4BC/0x802DE520 build geometry) | ribbon/belt | U (Im3D or model; not traced) | M |
| EffectParticle3D in Final Haunt wall boxes | RENDER_FLICKER.md fourth bug | - | - | (doc) |

---------------------------------------------------------------------------------------------------------------------

## 4. Port side

| Item | Port file:line | What the port does | Verdict |
|---|---|---|---|
| PTank recognition | scaffold/rw_driver/pipelines.cpp:625, 746-752 | an atomic whose pipeline is [0x805F24F8] draws its first material as one particle batch; alpha cutoff when the texture has alpha | EXACT (S) |
| PTank read | pipelines.cpp:878-970 | requires POSITION/SIZE/2DROTATE/VTX2TEXCOORDS, per-particle or constant colour; refuses MATRIX/NORMAL/VTXCOLOR/VTX4/USECENTER, count > 0x3FFF; copies clusters by value; axes from userData (+0x94) = the callback's already rotated, up-negated axes; constant colour from +0xC4 | EXACT for the only format the game creates (0xA7, 1.10) (P) |
| GX instancers 0x8044A25C.. | not run: the lifted site 0x80456F3C (lift-sites-r2.txt:663) replaces RxPipelineExecute | native draw from the records | DELIBERATE DEVIATION (P) |
| Blend / vertex alpha / fog | lift-sites-r2.txt:392-397, 547-548 (callback's state sets lifted); recipe() at pipelines.cpp:745 | the PTank's src/dst from +0xA4/+0xA8 and the effect's FOGENABLE reach the recipe | EXACT (S) |
| Quad expansion | lift/render/shaders/particles.hlsl:54-71 | W = sx/2 (cos R - sin U), H = sy/2 (sin R + cos U), exact sincos | DELIBERATE DEVIATION vs the parabolic sine (documented particles.hlsl:8-9); max 5.6 % rotation error removed |
| Colour x texture, cutoff, linear eye-depth fog | particles.hlsl:73-92 | modulate; discard a <= cutoff; fog lerp | EXACT in intent (S); TEV of the GC PTank not traced (U) |
| Batch transport | lift/render/frame.h:208-233, frame.cpp:122-136, renderer.cpp:1516-1570, render_vk/renderer_draws.cpp:171-215 | 44-byte instance, one DrawInstanced(6, n) per batch, in submission order | EXACT order (no sort, like the original) |
| Glare-source particles | renderer.cpp:1513-1514 | depth write off in the glare-sources pass | EXACT (S) |
| 3D particles, Im3D mode | scaffold/rw_driver/immediate.cpp:307-334 | the Im3D pool as triangles, textured with VERTEXUV (flags & 1) | EXACT (S) |
| 3D particles / objects, atomic mode | pipelines.cpp mesh path | ordinary atomics | EXACT (S) |
| Attached effect one frame late | runtime/src/gekko/native_attached_effects.cpp:15-22, 115 | native owner of SetMatrix 0x80426EB8 points the follow field +0x48 at the attach entry | DELIBERATE DEVIATION (RENDER_FLICKER.md) |
| Rotation-wrap hang | runtime/include/gekko/recomp.h:159, runtime/src/gekko/cpu.cpp:1297 | the infinite loop at 0x804349F8 was a recompiler FPR-context defect, fixed in the CPU core | fixed (not a renderer item) |
| Diagnostics | runtime/src/gekko/system.cpp:2073-2219 | inert probes on 0x80434824 and its five locks | diagnostic only |
| tools/particle_numeric_contract.py | docstring line 1 | "Lane-level original-word evidence; never a native retirement certificate": a static paired-single lane/dependency census of a guest procedure (schema particle_numeric_effect_candidate_v2; native_replacement_proven=False, deletion_authorized=False) | evidence tooling, no owner |
| tools/particle_lane_events.py | docstring line 1; FIRST_FIVE line 8 | "Validate bounded ordered reference observations; never infer native ownership": validates runtime traces of f18/f19 lanes at the entries 0x8044A25C, 0x8044A620, 0x8044A9E4, 0x8044ADAC, 0x8044B1B8 (the PTank instancers); native_retirement_proven=False | evidence tooling for a planned PTank-instancer owner, which does not exist |

Native vs translated: launch, banks, archives, emission, physics, affector bytecode, colour fixed point, MT19937,
bucket walk, PTank fill (0x80434824), the PTank callback, gadget/set/rain logic: **translated**. Native: the
PTank/Im3D/atomic DRAWS (scaffolding R2), and the SetMatrix attach owner.

---------------------------------------------------------------------------------------------------------------------

## 5. Original-engine flaws and proposed native replacements (proposals only)

| # | Flaw | Evidence | Effect | Proposed correct math |
|---|---|---|---|---|
| F1 | Drag applied per update, not per dt: v x= (1 - d) | 0x80428298..0x804282EC (no f30 = dt) | at PAL 50 Hz (dt 1.2) particles slow 17 % less per second than at 60 Hz; any dt != 1 changes trajectories | v x= (1 - d)^dt (or exp(-k dt) with k = -ln(1 - d)); clamp d >= 1 to 0 as now |
| F2 | Emission truncates n = rate x dt when n >= 1 (fraction lost); stochastic only below 1 | 0x804283C4 | rate 1.5/frame emits 1/frame (33 % short); PAL emits differently from NTSC | keep a fractional accumulator per emitter: acc += rate x dt; emit floor(acc); acc -= floor(acc) (deterministic, rate-exact) |
| F3 | Explicit Euler, gravity after the position step | 0x80428288, 0x80428300 | arcs depend on step size (PAL vs 60 Hz, frame hitches with dt = k) | exact constant-acceleration step: p += v dt - 0.5 g dt^2 (y); v.y -= g dt |
| F4 | Colour/alpha keys: first update skipped, step truncated, end not snapped to target | 0x80424324 (+0x10 arm byte; fctiwz; stop when t > duration) | final colour short of the target by up to one step x dt; differs between 50/60 Hz; one-frame latency | evaluate keys as a function of time: c(t) = lerp(start, target, saturate(t / duration)) in float |
| F5 | Global MT19937 shared by effects and gameplay | 0x8040FEE0 (69 sites), 0x8040FCD0 (81), 0x8040FAE0 (26), one state 0x805E1894 | the number of particles emitted (a visual) perturbs gameplay random draws; culling or a native particle system that stops drawing numbers would change gameplay | give effects their own generator (seeded per effect); keep the gameplay stream untouched. Until then a native particle owner must consume exactly the same draws in the same order |
| F6 | Random float = single(u32 x 2^-32) can round to exactly 1.0 | 0x8040FCB4..0x8040FCC4 (frsp) | rare out-of-range [0,1] value (u32 >= 2^32 - 2^7) | (u32 >> 8) x 2^-24 |
| F7 | Rotation wrap by repeated +/- 2 pi, written back | 0x804349F8..0x80434A2C | O(|r| / 2 pi) loop; once ulp(r) > 2 pi (|r| above ~5e7) a step no longer changes r and the loop never ends; Inf never ends either (the Gun Fortress freeze was a different cause: zeroed FPR constants, recomp.h:159) | r = remainder(r, 2 pi) once (or drop the wrap: the native shader takes any angle) |
| F8 | Parabolic sine in the instancers | 0x8044BB94.., 0x8051CE08 | up to 5.6 % error in the quad's rotation | already exact in the port (particles.hlsl:59) |
| F9 | No depth sort; list/launch order per bucket, oldest-first inside a tank | 0x804261C0, 0x80428498 | SRCALPHA/INVSRCALPHA particles composite in the wrong order across and within emitters | per-bucket back-to-front sort of batches + per-tank sort by view depth, or weighted blended OIT for the trans bucket; add/multiply buckets are order-independent already |
| F10 | Hard particle/geometry intersection (no soft particles) | draws are plain quads with depth test | visible cut lines where smoke meets floors | soft particles: fade alpha by saturate((scene_depth - particle_depth) / k) in particles.hlsl (needs the depth SRV) |
| F11 | Fog colour added to additive and multiply particles | the GX fog applies before blending; PS does the same (particles.hlsl:84-90) | additive particles glow toward the fog colour in fog; multiply particles tint | fog toward the blend's neutral: 0 for ONE/ONE and SRCALPHA/ONE, 1 for DESTCOLOR/ZERO, the fog colour only for alpha blends |
| F12 | Rain box sized from the guest camera's view window | 0x800D51E0..0x800D5200 (vw.x, vw.y x 2.2) | 4:3 bound: if the native view is wider than the guest camera's window, the rain model does not reach the screen edges | scale x by the active viewport's actual half-width at d = 1.1 (vw.y x aspect of the real view) |
| F13 | 8-bit colour out of an 8.22 state; values below 1/255 snap to 0 | 0x804243C8..0x80424454 | banded slow fades of dark additive particles | keep the 8.22 state, send float colour (the instance format already allows R16G16B16A16 if needed) |
| F14 | Effect durations, flip-book intervals and colour keys in 60 Hz frames, life in frames | 1.3, 1.7 | fine (dt scales them) but integer frame counters (u16) quantize timing | interpret as seconds x 60 in a native owner |

---------------------------------------------------------------------------------------------------------------------

## 6. Open questions (need a runtime capture or a Dolphin check)

1. The Z states for each bucket (ZWRITE/ZTEST around 0x804261C0): set by the RenderLevel or left from the world pass?
   Capture the recipe of a trans-bucket PTank (SHADOW_R2_TRACE_FRAME) and compare with Dolphin's GX state.
2. The GC PTank TEV/alpha-compare setup inside the pipeline node (not traced): modulate RASC x TEXC and the alpha
   test value - confirm the port's colour x texture and cutoff (Dolphin TEV dump on a smoke particle).
3. Which shipped effects use data+0x50 = 2 (multiply) and data+0x50 >= 3, and whether any keep fog on while additive
   (F11's visibility). Needs an archive census of CommonEffect.one / stg*_fx.one.
4. Semantics of affector ops 4 (+0x10), 13, 14, 16 and the 0x80429708/0x8042965C particle kinds (data+0x20 = 2/5).
5. Belt (Test::EffBelt) draw path: Im3D (0x802DF18C has Im3D calls) or a model - trace once.
6. Whether 0x8057E760+0x0C can exceed 1 in normal play on the port (dt = k): confirms how often F1-F4 matter.
7. The bank-3 index variable 0x8057E760+0xAC (boss id range 0x1C..0x2C) - confirm it is the stage/boss number.

---------------------------------------------------------------------------------------------------------------------

## 7. Runtime cost, net (2026-10-30; Phase VII)

Measured, not static: Westopolis headless replay, GEKKO_SELF_SAMPLE every 50 us with stack walks (21,613 game-thread
samples), tools/sample_subtree.py (exclusive = innermost frame inside the subtree, so the shares add up to the
inclusive total without double counting). Note: self_sample_report.py --select takes a LIST of frame numbers, not a
range (a two-number "range" selects two frames).

Effect update, EffectTask 0x801D2318 -> manager update 0x804262E4: **8.51 %** of the game thread, net by function:

| net | function | identified as |
|---|---|---|
| 1.35 % | 0x80428044 | particle step (1.6) |
| 0.82 % | 0x8040E048 | vec3 add a += b (three fadds; P from the disassembly); called by the step, 0x80424958, 0x80425544 |
| 0.69 % | 0x80423F20 | **alpha-key affector update** (op 12, 1.7) - P from the disassembly: first call arms (+0x10), t (+0x1C) += f1, t > duration (+0x20) -> stop (optionally kill: cmd byte 2), else target word += fctiwz(step (+0x24, int) x dt) with clamp to 0x3FC00000 and zero below 0x3FFFFF; returns bool |
| 0.40 % | 0x8040DDF8 | vector helper (via 0x800091A4) |
| 0.28 % | 0x80425B20 | spin affector update (op 5) |
| 0.19 / 0.15 / 0.13 % | 0x80425088 / 0x80425544 / 0x80424958 | affector updates (size / vec3 tracks) |
| ~0.4 % | fp_compare, call_indirect, find_hle, DynamicCastIntrinsic | recompiler dispatch overhead inside the subtree |
| rest | long tail (emission 0x80428338 0.06 %, MT19937 0x8040FB60 0.07 %, matrix 0x80426C84 0.10 %) | |

Draw walk 0x804261C0: **4.80 %**, direct children: PTank fill 0x80434824 2.39 % (self 0.80 %, matrix x vector
0x8040C6CC 0.37 %, RwDriver::particle_batch 0.20 %, lock 0x80449458 0.11 %), **shimmer element draw 0x8043531C 1.90 %**
(self 1.12 %; reached through draw task 0x801D2DFC -> 0x8002761C, bucket 6 PostGlare; ~900 instructions building
geometry with the sqrt / sin helpers 0x8000FEE4 / 0x8000FFE8, then nine render-state / draw calls by bctrl - not in
sections 1-6, to be traced), Particle3D draw 0x80433DA8 0.06 %.

Particle engine total (update + draw walk, disjoint subtrees): ~13.3 % of the game thread. Largest coherent native
targets by net cost: the per-particle affector updates (~1.5 % net together), the particle step with its vector leaves
(~2.6 %), the shimmer draw (1.9 %), the PTank fill (0.8 % self + its matrix helper).

Native-owner contract for these (as for every native owner in this project: native_link_groups.cpp): every
memory-visible effect including stack scratch words, the caller-visible registers, CR fields - and FPSCR: the affector
updates execute fsubs / fmuls / fctiwz, which set FPSCR sticky status bits per operation. Whether native owners may
leave FPSCR status bits unmodelled (no game code was found reading them) is an open decision shared with the native
MSL math work.
