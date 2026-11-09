# The object spawn system and everything distance related (2026-10-29)

The user's request: "Reverse engineer the whole object spawn system. Everything distance related." This page is the
entry point: what the game does, in the order a frame does it, with each part's full report beside it. All static
readings of the PAL main.dol (GUPP8P), marked VERIFIED / INFERRED in the reports; runtime measurements are marked
MEASURED. Earlier reports these build on: `../DISTANT_RENDERING.md`, `../DRAW_DISTANCE.md`, `land_streaming.md`,
`camera_fog_culling.md`, `object_distance.md`, `effects_distance.md`, `lod_streaming.md`.

| report | what it covers |
|---|---|
| `spawn_data.md` | layout files on disc, the layout manager, entries, slots, flags, setid.bin, factories, gates |
| `spawn_loop.md` | the per-frame spawn / leave / respawn / destroy logic, restrictions, timing |
| `object_side_distance.md` | what each of the 197 creatable classes does with distance after it exists |
| `distance_census.md` | every distance computation and threshold in the DOL (696 comparisons, 85 helpers) |
| `frame_lights_spawn.md` | the frame's render order, the current world and lights at each draw |
| `draw_paths_items.md` / `draw_paths_props.md` / `draw_paths_enemies.md` | how each class draws: layer, lights, render states |

## 1. The data (spawn_data.md)

- A stage's objects: `stgNNNN_cmn.dat`, then `_hrd.dat` (hard mode byte [0x8057E760+0x125]) or `_nrm.dat`, then
  `_ds1.dat`, loaded by 0x800CBC54 at stage build. Header "sky2", count; 0x2C-byte entries (the game's swap format
  "ffffffiisccii" at 0x804B7768): position, rotation (degrees), flags, saved flags, u16 class id, u8 link, **u8
  range**, misc length, misc pointer; misc words after the entries. Westopolis normal: 504 + 71 + 536 = 1111
  entries; ranges 1..30, mostly 5 and 6.
- The layout manager (0x42C bytes, holder r13+0x327C, ctor 0x800CB474): entries +0x41C, misc pool +0x420, slots
  +0x18 (slot i = entry i: +0x0C object, +0x10 factory index, +0x14 a state object that survives the object, +0x18
  weak handle), link-list heads +0x1C, factory table +0x14 (0x8052C1A0, 286 pointers, 197 creatable), restricted
  bit +0x428.
- Entry flags: 0x1 **enabled** (cleared only by a kill or pickup), 0x2 live (object exists), 0x4 leave request,
  0x8 killed, 0x40 respawn armed, 0x200 permanently killed (savepoint), 0x800 / 0x80800 never leaves / always in
  range, 0x1000 / 0x2000 / 0x4000 / 0x8000 file layer (cmn / nrm / hrd / ds1), 0x10000 kill-with-respawn, 0x20000
  held by a link group, 0x80000 one-shot force spawn, 0x800000 resurrect.
- setid.bin: 302 {class, 64-bit stage mask} records - only decides which classes are preloaded for a stage.

## 2. A frame (spawn_loop.md, frame_lights_spawn.md)

1. **Spawn** - SetManagerTask (0x800CB794 -> 0x800CB97C, task group 6), before the camera group (18): the test
   uses **last frame's camera eye**. Nothing spawns while any of four gates fails: the holder's enable byte, the
   stage's init counter (+0x14), **the resource manager 0x8056FF94 still loading anything (+0x10 != 0)**, the
   factory table. Then every one of the 0x800 slots, in file order, no budget, no queue: spawn when enabled, not
   live, not held, and `|camera eye - position|^2 <= 10000 * range^2` (3D, height included; range 0 never; either
   camera in split screen). Create runs the class constructor, whose base (0x8015AE00) binds the object to its slot
   and sets 0x2.
2. **Objects update** - the gadget base gate 0x8015AC7C runs first in every gadget's exec: it leaves on the event
   flag (entries without 0x8000), on 0x4, or when the camera eye is at least 100 * range + 50 away (the 50 from
   [r13+0x3270], set at 0x800CA324). The object is deleted next frame; the entry can spawn again from frame N+2.
3. **Render** - level 1 (Opeq land and object worlds, layers N D OS OSL O OW OL P), level 2 (CameraRender: the
   light manager's world, rings / hint-ring "?" / item panels), level 5 (AF A AL), level 7 (K M), level 9 (G GL),
   effects, HUD. Each layer pass sets its render states from its flags (0x80046D80) and attaches the lights only in
   lit layers (OSL, OL, AL, GL).

## 3. Everything distance does (object_side_distance.md, distance_census.md)

- **Spawn / leave** - the layout range above (from the camera eye, never the player); 153 gadget classes pop in
  and out at full opacity, no fade, no delay.
- **Fades** - ART GADGET / ART GADGET ZONE: in over 7 updates (0.147 a step) and out at the range edge; 14 enemy
  classes: alpha from 0 at spawn, +2.0 / s (0.5 s), out past range + 50 at the same rate, in layer 10 `_AL_`
  while below 1.
- **Hidden until near** - 7 enemies with appear types (EGG_PAWN, BK_SOLDIER, GUN_BEETLE, GUN_ROBOT, GUN_SOLDIER,
  BK_GIANT, BK_NINJA): until the player enters their search cylinder (0x801A6C7C, 79 callers); WARP + 1.0 s.
  BK_LARVA nests: one larva per frame, each hidden until the player is in the nest's cylinder and a 1-2 s timer
  ends. 9 bosses wait for resource group 0x16 and never leave by distance.
- **Own leave rules** - released physics bodies (containers, BOMB, HEALING_UNIT) leave at 100 * range + 50 from
  where they now are; Monster and Coaster at 100 * range + 100 from the player; dropped items and weapons beyond
  1000 of every camera (0x8010C3C4, 0x8011F5CC via 0x80169FC8); characters and vehicles 100 * range + 100.
- **Draw culling** - RenderWare's frustum (0x804603E0 -> 0x80486718) and 13 objects that test a sphere against the
  frustum before drawing themselves (RING r 5, HINTRING 10, ITEM_BOX 10, CIRCUS_POLE, MORPHDOOR, the Kage drop
  shadow r 5, ...). The far plane comes from the stage's `.fog` (eased 2 % a frame); EffectShimmer alone compares
  against it.
- **Behaviour only** - lock-on (TargetManageTask: cone, range cut at target +4 squared, nearest pick), sound
  (full under 60, linear to row x 100, default 400; pan inside 50; no Doppler past 800), rumble (250), AI
  triggers (306 thresholds), collision library (143). No radar.
- **Absent** - no LOD of any kind, no distance-based update throttling, no particle distance cull, no distance
  alpha except the two fades.

## 4. What this means for the far stand-ins (STANDINS_PLAN.md)

- A stand-in covers an entry that is enabled, not live, not held, and outside the camera range - drawn as the game
  draws the class: its parts' layers, the layer's render states, the layer's lights and light group, the class's
  own camera-facing models (HINTRING01, item panels) and level-2 draws (rings). Done and validated 2026-10-29: layer
  render states (199,643 game draws compared, 0 differ), ring lights / fog and level-2 states, the hint-ring "?"
  (0 differ in lights over 54,580 stand-in parts).
- Hand over on the spawn frame for gadgets (they draw at full opacity at once); for fading enemies keep the
  stand-in while the enemy's alpha (+0xEC) is below 1; never show appear-gated enemies, larvae or bosses before the
  game does; give no stand-in at the entry for objects that move away (released bodies, Monster, Coaster).
- The user's dash ramp after the Westopolis fall (MEASURED on the user's own recording,
  `.build/recordings/user-westopolis-ramp`): the game spawns the four slope ramps at frame 804, during the fall,
  and draws them itself; the far plane stays 30000; draw distance 1 and 8 are pixel-identical through the landing,
  where smoke and fog around Shadow hide the slope until the timer reads 19:09.

## Open

- Enemy stand-ins (soldiers): GUN_SOLDIER has none (its clump is under no resource type looked up so far);
  BK_SOLDIER's lights still differ (2,061 of 28,186 comparisons); appear-gated soldiers must stay hidden.
- Who sets entry flags 0x80000 and 0x40000000; the hard-mode byte; class bit 0x400000; setid's stage index +0xAC.
- Inline distance comparisons after an integer conversion, and distances stored in one function and compared in
  another, are found only by hand (distance_census.md "Limits").
