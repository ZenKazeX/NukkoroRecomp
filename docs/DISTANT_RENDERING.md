# Distant rendering and streaming (reverse engineered, 2026-10-29)

Everything in Shadow the Hedgehog (PAL GUPP8P main.dol) that decides how far away something is drawn, and what is
loaded when. Each claim is marked:

- **VERIFIED** - read in the disassembly (address given);
- **MEASURED** - observed at runtime on the Westopolis (stg0100) and Glyphic Canyon (stg0201) tapes;
- **INFERRED** - follows from the code but not checked directly.

The full static reports, with quoted instruction sequences, are in `docs/distant-rendering/` (`land_streaming.md`,
`camera_fog_culling.md`, `object_distance.md`, `effects_distance.md`, `lod_streaming.md`, `fog_table.txt`). Object
draw distance, the far-object proxies and the display layers are in `docs/DRAW_DISTANCE.md`.

## The short version

Nothing in the game draws less detail far away, and only one effect is culled by distance. What limits the far view:

1. **The camera's far clip plane**, set per stage area by the `.fog` file and eased 2 % per frame. Fog always ends at
   the far plane.
2. **The landscape area boxes** (`STGnnnn.BDT`): a landscape block is loaded and drawn only while the camera eye is
   inside one of its designer-placed boxes. Far scenery is separate low-poly backdrop blocks swapped by region.
3. **The object spawn range** (layout byte +0x23 x 100 units), measured from the camera eye: beyond it an object does
   not exist (the native far-object proxies stand in for some classes).
4. **RenderWare view-frustum culling** of world sectors and atomics.

There is no geometry, texture or animation LOD, no screen-size cull and no distance-based update throttling.

## 1. Which point everything is measured from (MEASURED)

The list at r13+0x2B34 (getter 0x80009548, lazily set to 0x8056FED4) is the **camera list**: count +8 (2 - one per
viewport), elements of 8 bytes from +0xC; element 0's object +8 is the camera eye. At Westopolis frame 700 it read
(4.2, -2458.7, -1284.6), equal to the in-stage RwCamera's frame LTM position (`*0x8056FF1C` +4, LTM +0x50,
position +0x80) on every frame of both tapes. So:

- the area-box test (below) tests the **camera eye** (the game's mask matched the boxes tested at that point on
  4793 of 4801 Westopolis frames and 3000 of 3001 Glyphic Canyon frames; the remainder are the frames of a change);
- the layout spawn / leave range functions (0x800CA970, 0x800C9D54), which read the same list, measure from the
  **camera eye**, not from Shadow. `DRAW_DISTANCE.md` called it the player list; corrected there.

The stage start task (TStageInitalizeTask 0x80177510) sets an explicit test point (TBlockManage +0x3C, setter
0x8003ACD0) to the start position until the first blocks are in, then clears it (VERIFIED).

## 2. Landscape blocks (land streaming)

Three objects (VERIFIED; `land_streaming.md`):

| object | where | holds |
|---|---|---|
| land manager | static 0x80572560 (getter 0x8003A868 = [r13+0x2B5C]), built by 0x80047618 | 32 block slots of 0xC0: +0 state (0 free, 1 loader created, 2 reading, 4 layers in, 5 resident), +4 + 0xC * layer a record per display layer starting with its RpWorld, +0xB8 the collision (COLI) world, +0xBC block id; after the slots +0x1804 texCM dictionary, +0x1808 draw the COLI worlds instead (debug), +0x1809 fog enable, +0x1810 freeze new loads |
| TBlockManage | heap, 0x980 bytes, pointer at [0x8051DFA8] | the stage's BDT at +0x34, test point +0x3C, mask buffers +0x48 + 0x150 i (s16 per block, 168 blocks), index +0x44, per-block override +0x2E8 + 4 b (1 on, 2 off, one-shot), slot pointers +0x588, resident list +0x828 (s16, -1 ends), ready flags +0x978 / +0x979 |
| block loader | one task per block ("LandALoadMananger", 0xD8 bytes) | reads `stgnnnn/stgnnnn_NN.one`: each layer's RG1 world, then `_COLI` |

Per frame (VERIFIED): TBlockManage's update 0x8003AAE8 = 0x8003A8D4 (mask: a block is in when the point lies in one
of its boxes, `min <= p < max` per axis, 0x8003A578; overrides applied) then 0x8003A6DC (load every block in either
mask buffer - 0x800462EC - unload the others - 0x800473A8 - and list the loaded ones in the current mask). The
display-layer draw 0x800471BC draws, per layer, each slot's world for that layer (state 5 and in the mask, 0x8003AA58)
then the layer's object world. Order each frame: the layout manager (task group 6) before Landscape (group 10), the
player (group 13), then rendering.

`STGnnnn.BDT` (in `stgnnnn_dat.one`): a 0x40-byte name, count, then 0x20-byte records {block, min[3], max[3], word}.
The last word is 80 in all 1,166 records of the game and is never read (VERIFIED). Westopolis: 28 records for 23
blocks; Glyphic Canyon: 36, whose block 90 spans the whole stage.

MEASURED (Westopolis tape): drawn blocks 1 -> 1 2 -> +19 -> +22 -> 2 19 22 -> +3 -> +4 -> 3 4 19 22 -> 3 4 5 19;
Glyphic Canyon starts with 1 2 23 60 90. A block takes **14 frames** from entering the mask to its slot reaching
state 5, every time. A block is unloaded only once it is in neither mask buffer (138 to 1,581 frames after it left
the current mask): the "current plus previous mask" hysteresis. Limits (VERIFIED): 32 resident or loading blocks,
16 file members loading at once, block ids below 168. Far scenery is low-poly backdrop blocks swapped by region
(Westopolis 19, 20, 23, and the backdrop layers of blocks 1 and 18).

## 3. Camera clip planes and fog

Clip writes (VERIFIED, `camera_fog_culling.md`): RwCameraSetNearClipPlane 0x804865A0 / SetFarClipPlane 0x8048665C
(camera +0x80 / +0x84) are called only by RwCameraCreate, the camera-manager create 0x80271BB4 (far 30000 from
0x805F8F20, near 1.0), the fog task TFogSet (constructor 0x8016AD3C: far 30000 from 0x805F5C4C; per frame
0x8016A904), event start 0x80174500 (saves far, sets 30000 from 0x805F5E68) / end 0x80174480 (restores), and the
event-script command 0x801740B8 (far = command +0x44; event8001 50000, event8004 15000). No camera mode or `cam.dat`
sets them.

`stgnnnn.fog`: 24 entries of 0x28 bytes (little-endian): +0x00 a 128-bit block mask, +0x10 near, +0x14 far, +0x18
fog on, +0x1C fog start, +0x24 colour bytes B, G, R. Per frame TFogSet averages the entries whose mask meets the
visible blocks and eases toward them: `far = 0.98 far + 0.02 average` (0.98 / 0.02 at 0x805F5C50 / 54; VERIFIED
`fmadds` at 0x8016AAC4..0x8016AAD0), the same for near (clamped >= 1) and the fog start; frame-counted, so it eases
slower in wall time at 50 Hz. The first frame forces far 30000. MEASURED: Glyphic Canyon 30000 -> 29950 -> 29900.5
... toward 25000; Westopolis stays 30000. Per-stage values: `fog_table.txt` (Final Haunt 3000 / 20000 / 30000 by area,
stg0401 600000, stg0500 1.5e6 ...).

Fog (VERIFIED): GXSetFog 0x80399A0C with start = camera +0x88, end = the far plane. Fog is on for the layers with flag
0x4 (`_OS_ _OSL_ _O_ _OW_ _OL_ _P_ _AF_ _A_ _AL_ _K_ _M_`), off for `_N_ _D_ _G_ _GL_`. **Probable original
defect**: TFogSet packs the colour as the word `[e+0x24, e+0x25, e+0x26, 0]` (0x8016ABFC..0x8016AC0C), and the GC
driver splits it as r = bits 16-23, g = 8-15, b = 0-7, a = 24-31 (0x80498AA8..0x80498ACC), so with the file's B, G, R
order the GX fog colour is (G, R, 0) while the clear colour (0x8057E760) is the correct (R, G, B). The native renderer
decodes FOGCOLOR the same way (render_states.cpp), so it reproduces the GameCube's colours.

## 4. Culling (VERIFIED)

RpWorldRender 0x80460FAC: the camera's sector list is built by a BSP walk against the frustum (0x804626A8, on
RpWorldAddCamera / BeginUpdate), then each sector's atomics go through 0x804603E0: tied to the sector, render flag 0x04,
`renderFrame` (u16 +0x60) != RwGlobals' (dedupe across sectors), world bounding sphere (0x80457720) inside the
frustum (0x80486718, a plain 6-plane sphere test). No distance limit beyond the far plane. Clumps outside worlds are
not culled by RenderWare; 13 game sites call the sphere test themselves (RING r 5, HINTRING / ITEM_BOX r 10, ...).
The aspect scalar 0x805EBA30 is a constant 1.3196: view window = (tan(h/2), tan(h/2) / 1.3196).

## 5. Objects and actors (`object_distance.md`)

- Spawn / leave range: `DRAW_DISTANCE.md` (measured from the camera eye, section 1 here).
- Enemy out-of-range fade (VERIFIED 0x801A6EB8, body 0x801A6F08, 14 enemy classes): alpha (model +0xEC)
  `-= 2.0 dt` (`fnmsubs` 0x801A6F6C, 2.0 at 0x805F68D8) once out of range + 50, then the object leaves.
- Actor leave radius (VERIFIED 0x801B60BC, test 0x801B6284): 100 range + 100, or 500 without a slot, against every
  viewport's eye, no +50 margin (SET_VEHICLE and others).
- BK_SOLDIER (VERIFIED): misc words copied to obj +0x2E8 (0x8018E618); word 8 WeaponType, 9 AppearType (0 STAND ... 4
  OFFSETPOS, 5 WARP); OFFSETPOS / WARP move the spawn by (word 13, word 16, word 14). The soldier stays invisible until
  the eye enters its search cylinder (0x801A6C7C; SearchRange is the cylinder's forward offset, SearchWidth its
  radius); WARP then waits 1.0 s before showing.
- Model holder (VERIFIED): no distance logic. +0x3C bits: 2 hidden, 4 timed blink (+0x40), 5 endless blink, 6 parts
  moved to the fade layer, 7-10 fading / out / in / faded. A fade steps alpha (+0x44) by `0.98 * [+0x48]` (0.15) per
  update (`fmadds` 0x801636A8) - about 7 updates, not a fixed time; during it the parts move to layer 10 `_AL_`.
- Particle locators (VERIFIED 0x80167D28; MEASURED): a 2-triangle untextured quad (material colour 0x666666FF) whose
  geometry has the user data `ATTR_PTCL.PTCLCATEGORY / PTCLID / PTCLFLAG` is hidden - atomic flags `(f & ~4) | 8`
  (8 from [r2-0x7C58]) - unless PTCLFLAG bit 1, and an emitter is registered at its frame (functor 0x8016A0E4, run by
  ART GADGET 0x800E53CC, STICK 0x80122A0C and ~60 sites). Glyphic Canyon's torch (STICK part 1, geometry 0x810FADE0)
  was drawn 0 times in 400 frames including its spawn frame: it never shows. These are the "parts the game never
  draws" of ART GADGET, FallBuilding and STICK.
- CAGE (MEASURED, corrects the static report): all six parts are drawn, parts 0, 1, 3 in `_OL_` (world 0x807A25A0)
  and 2, 4, 5 in `_G_` (0x807A3100); those reported "not drawn" are drawn more than 20 units from the library pose -
  they move.
- No animation / AI throttling by distance, no screen-size cull, no live-object cap beyond the 0x800 layout slots.

## 6. Effects (`effects_distance.md`, VERIFIED unless marked)

EffectManagerPJS (r13+0x2DD8 = 0x805EF2D8): Launch 0x80426680 has no count cap, distance test or LOD (only allocation
failure stops it); update 0x804262E4 and the bucket draw 0x804261C0 have no culling. The only distance cull in the
effect system: EffectShimmerPJS's draw 0x8043531C skips an element in front of the near plane or beyond the far plane
(`fcmpo` against camera +0x80 / +0x84 at 0x80435498..0x804354A4). Object / Particle3D elements added to a world are
frustum-culled by the sector loop; PTank billboards, Im3D quads, rays and trails are not culled. Layout effect objects
(PARTICLE, FIRE = effect 0xA6, BREAK_BLAST, CityLaser, BACKDROP) use the ordinary layout range; their effect dies the
frame the object leaves. CityLaser fires within misc word 1 x 10 units of a character (activation, not visibility).
Lens flare: the sun is a fixed point; only "behind the near plane" rejects it, no occlusion query. No skydome: the sky
is the landscape layers `_N_` / `_D_` (unlit, unfogged), under the far plane and the block mask. Shadows: a 300-unit
downward ray, no camera-distance test. No streaming during play: `stgnnnn_gdt.one` / `_fx.one` load once per stage.

## 6b. UV animations: why a far dash ramp's arrows stand still (`uv_anim_start.md`)

The user saw the dash ramps appear with still arrows that start scrolling later. Static (VERIFIED) and measured:

- A model's UV animation lives on its **library** material, shared by every instance (RpAtomicClone reuses the
  geometry and its materials). The model holder's instancing 0x80162EB4 registers the part's `.UVA` (record +0x08,
  e.g. `JBOARD01.UVA`) with the library clump in a `std::map<name, {clump, count}>` at 0x80580B08 (0x80162FA0 ->
  0x801EB970); releasing the object decrements it (0x80162BE8 -> 0x801EB860) and erases the node at 0. A global
  TextureUVAnimManagerTask (vtable 0x8053EDC4, exec 0x801EB790) advances only the registered entries (0x801EBA74 ->
  0x80039BBC -> per material 0x80039C44: AddAnimTime 0x804559B4, then for MatFX kinds 5 / 6 ApplyUpdate 0x8045567C,
  whose 0x804A3C30 stores the uv matrix pointer into the material's MatFX data). `JBOARD01.UVA`: 3 keys, a seamless
  0.625 s scroll from t = 0 - no hold at the start.
- MEASURED (Westopolis, `SHADOW_FAR_MATERIAL=81084740`, the arrows' material): frames 1-19 no uv matrix (static);
  the stage-start ramp (entry 0x809B6314) spawns on frame 19 and the scroll runs from frame 20 (v += ~0.11 a frame);
  that ramp leaves on frame 71 and the scroll **freezes there at v = 2.400**; the four slope ramps spawn on 805 and it
  resumes on 806 (2.5, 2.6, ...).
- So the game scrolls a shared UV animation only while at least one instance of its object exists. At draw distance
  1 nobody sees the frozen arrows (no instance, nothing drawn); the far proxies draw the shared library material and
  show its frozen phase until a spawn re-registers it. The same holds for DASHPANEL, SPRING, HINTRING, SAVEPOINT and
  every other object with a `.UVA`. The native renderer adds no delay (matfx_layers reads the matrix at every draw).
- Fix (user's choice 2026-10-29: scroll them natively; final form below, "One shared clock"): `RwDriver::uv_animation`
  (pipelines.cpp) evaluates a material's RpUVAnim itself, exactly as the game does. The plugin (material + [0x805F2544], 0x28 bytes): +0 /
  +4 the uv[0] / uv[1] matrices, +8.. eight interpolators {+0 animation, +4 time, +0x2C node count}; the animation
  {+0 info (+0 type 0x1C0 linear / 0x1C1 parameterised, +8 key size 0x20), +4 key count, +0xC duration, +0x10 keys
  {prev, time, six floats}, +0x14 custom (+0x20 per node: channel)}. Interpolation 0x804543A0 / 0x80454704 (a lerp
  of the six floats; theta's difference wrapped into (-pi, pi] by r2+0x19E0 / 0x19E4 / 0x19E8); the matrix from
  ApplyUpdate 0x8045567C: linear - right (k0, k1), up (k2, k3), pos (k4, k5); parameterised - right (s0, skew), up
  (0, s1), pos (x, y), translated by (-0.5, -0.5) (0x8051CE58), turned about z (0x8051CE70) by theta, translated
  back by (0.5, 0.5) (0x8051CE64); each node pre-multiplies its channel's matrix from identity (0x8048077C mode 1).
- The first version followed the game's time (the interpolator's time plus the game time since it last changed).
  It matched the game while the game advanced the animation, but left a one-frame phase jump at the handover: a
  spawn re-registers a frozen animation and the game resumes from its frozen time, while the proxies were at their
  running time (16 restarts on the two tapes, e.g. the slope ramps' spawn: game 0.125 s, proxies 0.342 s of the
  0.625 s scroll).

### One shared clock (final, 2026-10-29)

Registering the animations at stage load was rejected: 0x801EB970 allocates a map node on the game heap
(0x801EC384 -> 0x803A3BF4) and changes the manager's tree and counts, which is game state. Instead a render-only
clock, native, owned by the renderer, drives **both** the stand-ins and the game's own draws of the same materials,
so a handover cannot change the phase.

- Which animations: only geometries whose clump the game registers in the map at 0x80580B08
  (`uv_registered_now`, an in-order walk of the std::map: node {left +0, right +4, parent|colour +8, clump +0x18})
  and proxy parts whose model record has a `.UVA` (record +0x08). Nothing else gets a clock; no object, file or game
  allocation is created. A clock is keyed by (animation, interpolator) and created at first sight from the
  interpolator's time.
- Stepping (`advance_uv_clocks`, once per frame): while the animation is registered, the clock takes the game's own
  step (the interpolator's time minus last frame's, wrapped by the duration), so it keeps the game's speed, including
  the per-animation rates the manager applies (most 1.0x, some 1.5-9.2x a frame), and a time the game sets itself (an
  object re-created and its animation starting over) - only the phase is drawn, so whole periods a long gap wrapped
  away change nothing. While unregistered (the game froze it; only proxies show it) it advances by the time the game
  handed TextureUVAnimManagerTask since the clock's last step (2026-11-05, `gekko/uv_anim_time.h`): the native
  scheduler sums the delta it passes that task's execute 0x801EB790, so the clock runs exactly as the animation would
  if registered, stands whenever the game stops the task (a load, the traversal's pause skip), and loses nothing over
  frames that drew none of it. Before, it took the host's frame time plus a heuristic (registered clocks exist and
  none moved = paused), which moved through loads and could not see a pause with nothing registered. Measured: the
  pause menu does NOT stop the task - registered animations keep running under it in the original's own code, so
  the shared clocks do too. Under `--legacy-scheduler` (nothing feeds the sum) the old frame-time rule remains. The
  clock wraps by the duration, as RtAnimInterpolatorAddAnimTime 0x80478F24 does.
- Stage changes (2026-11-05): besides the far library epoch, the stage's archive word 0x805F03C0 (stgNNNN_dat.one,
  kept open until the StageManager's teardown) is read once a frame (`uv_stage_check`, from `submit_frame`); any
  change - a stage loaded or left - drops every clock and kept geometry, so a reload at the same addresses cannot
  revive another stage's clocks. A pause-menu restart keeps that archive and the libraries with it (measured on
  stg0600: 0x80EEFA48 before and after; the scene reset 0x801EB624 and the task destructor 0x80171D74 do not run
  for it either), so the clocks carry on, as the libraries' interpolators do.
- Use (`shared_uv`): after `matfx_layers`, for MatFX kinds 5 / 6, the material's uv matrix (and the dual one for 6)
  is evaluated at the clock's time with the game's own interpolation and ApplyUpdate maths. Applied only with draw
  distance above 1 (`far_.enabled()`); at 1 the game's matrix is drawn unchanged. Clocks and the geometry cache
  reset with the stage epoch and the stage archive. `SHADOW_UV_SHARED_CLOCK=0` turns it off. No offsets, no per-frame corrections: the
  only inputs are the game's interpolator time, the animation data and the game time.
- Validation (`SHADOW_FAR_VALIDATE=1`, every 600 frames a `[far-uv]` line), tolerance 1e-4 throughout:
  (1) every shared evaluation is also evaluated at the game's own time and compared with the game's matrix
  (plugin+0 / +4 floats); (2) while registered, the clock's offset from the game's time must never change (a change
  would be a phase jump or a speed difference); (3) every draw of an animation within one frame must use one time.

| check | Westopolis tape | Glyphic Canyon tape | Westopolis, 2 restarts (windowed) | Glyphic Canyon, 2 restarts (windowed) |
|---|---|---|---|---|
| matrices at the game's time vs the game's | 46,394, 0 differ | 189,948, 0 differ | 11,015, 0 differ | 133,507, 0 differ |
| registered steps / offset breaks | 22,287 / 0 | 18,942 / 0 | 2,917 / 0 | 9,560 / 0 |
| repeat draws in one frame / at another time | 83,456 / 0 | 305,402 / 0 | 10,612 / 0 | 237,986 / 0 |
| paused frames | 3 | 0 | 5 | 2 |
| clocks / geometries | 27 / 31 | 22 / 22 | 16 / 18 | 22 / 22 |

  Restarts are the game's own pause-menu Restart (`tools/stage_control.py`, which needs a window: the dev link
  serves no requests in headless runs).
  2026-11-05 (manager-time stepping, stage-archive reset), GUN Fortress stg0600, headless, draw distance 4, a tape
  that walks, opens the pause menu for 10 s and picks Restart (`.build/uv-anim/pause-restart.tape`): 9,639 matrices
  at the game's time, 0 differ; 1,410 registered steps, 0 offset breaks; 7,479 repeat draws, 0 at another time.
  Same tape against the previous build: draw distance 1, and draw distance 4 with `SHADOW_UV_SHARED_CLOCK=0`, give
  byte-identical frames (10 of 10); draw distance 4 with the clock on differs only after the restart's reload, in the
  red glare strips' phase (the old clock ran through the 4.3 s load, the game's does not). Two long free-running sessions (5.39 M frames Westopolis, 3.72 M Glyphic
  Canyon) also gave 0 differ, 0 offset breaks, 0 split frames.
- Cost (Westopolis tape, headless, 90 s budget, 3 alternating runs per arm, another session's headless job running
  on the same machine throughout): `guest_run_wall_seconds` on 12.243 / 12.249 / 12.272 (mean 12.254), off
  12.300 / 12.175 / 12.074 (mean 12.183) - +0.07 s, inside the off runs' 0.23 s spread; peak working set 244.8-246.3
  MiB on vs 245.0-246.3 off; peak private 198.1-201.1 vs 198.9-200.4. Stage loading: the clock reads no file and
  allocates nothing at load; both arms open the same 3 files. Evaluation 136-439 ns while validating (which
  evaluates twice). Memory: one small map entry per animated material (16-27 per stage).
- Visual: not yet checked frame by frame on screen; the matrix checks above cover every drawn frame.

## 7. LOD and other streaming (`lod_streaming.md`, VERIFIED)

- No geometric LOD: RpLODAtomic is not linked; no LOD / MRM chunk in any DFF, RG1, BSP or RP2 on the disc; `_BRK`
  models are broken variants, `shadow2p*` 2-player costumes. No display layer is distance-limited.
- No texture LOD in effect: the game asks for trilinear filtering (mode 6) and 4x anisotropy (0x80039640,
  0x80038B80), but all 8,432 textures on the disc have one level, so it is plain bilinear at full size.
- No animation LOD (motion update 0x80416EF4, time advance 0x80419A9C, enemy component 0x801A4CF0).
- Preload: at stage start 0x800CBC54 reads `setid.bin` (302 rows: type id + a 64-bit stage mask); 0x800CA7F0 marks
  the factories the stage uses and 0x800CA780 calls each one's preload, so class archives stay resident all stage.
- On demand during play: motion clips copied from ARAM on first play (0x80414368 -> 0x80422234, a busy-wait on the
  game thread; pool `ARAlloc(0x600000)` at 0x804225B0), CRI audio / movie streams, event archives, landscape blocks.
  ARAM is about 15.4 of 16 MiB used (INFERRED from the allocations).

## 8. Open

- The game heap's high-water mark and sub-heap sizes; how long the ARAM clip copies stall (measure 0x80422234).
- Which `setid.bin` bit belongs to which stage (log 0x800CA7F0's index per stage).
- Whether a new enemy's alpha (+0xEC) starts at 0 (fades in) - read [model +0xEC] on the spawn frame.
- The lens-flare glow when the sun's depth exceeds the far plane (0x80432360); which world Object effects join
  (0x80433B14); whether a stage's `_N_ / _D_` sky exceeds its far plane (static estimate: 11,000-20,000 units in
  Westopolis, inside 30000).
- The meaning of the layer letters N, D, OS, K, M; +0x180A / +0x180C of the land manager (no readers).
- Event far planes (50000 / 15000) were not observed on a tape.

## Tools

`SHADOW_DISTANCE_TRACE=1` (far_objects.cpp `FarObjects::distance_trace`): per frame the block manager's mask against
the boxes at the camera list's eye and at the RwCamera frame, the land slots, block load / unload latency, and the
camera's clip planes. `SHADOW_FAR_TRACE=<class>:<first>:<last>` (also at draw distance 1): a class's parts, layers,
light groups and every game draw with its pass and world (`DRAW_DISTANCE.md` "Layers"); with it,
`SHADOW_FAR_MATERIAL=<material>` prints that material's UV plugin words and the uv matrix its MatFX data points at.
