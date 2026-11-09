# Object draw distance

Two parts. The game's range (`runtime/src/gekko/native_object_range.cpp`, `NativeObjectRange`) decides what is
alive and stays the original's bit for bit. Seeing farther is the native renderer's alone: the far-object proxies
(`runtime/scaffold/rw_driver/far_objects.{h,cpp}`, drawn by `RwDriver::draw_far_proxies`). Setting:
`--draw-distance <1..8>` (default 1 since 2026-10-30: early alpha, withheld from the Options menu) and the launcher's "Draw distance" slider: 1 = no proxies (the original), above 1
proxies to that many times each entry's authored range, 8 = wherever the object still covers a pixel.

## What the game does

Every stage's layout (`stgNNNN_cmn/_nrm/_hrd/_ds1.dat`, "sky2", 0x2C-byte entries, little-endian on disc) gives each
object an authored range byte at entry +0x23 (Westopolis: 2..30, mostly 4..6). The layout manager
(singleton holder r13+0x327C; per-frame updates 0x800CAA2C two players / 0x800CAC08 one player, 0x800 slots of 0x20
bytes) spawns a dormant object through its type's factory (manager +0x14 table, slot +0x10 index; the factory's first
word is its class name) when a player is within the range, and asks a live one to leave once every player is outside
it. That boundary is where an object appears: the pop-in.

The four functions that define the range (owned natively, `extract/native-owners.txt`):

| Address | Contract |
|---|---|
| 0x800CA970 | in_range(manager, slot, point): flags & 0x80800 -> 1; else \|pos - point\|^2 <= (100 * range)^2 (single precision; `10000.0f * single(range*range)`, the same value rounded once) |
| 0x800C9D54 | out_of_range(slot): flags & 0x800 -> 0; else 1 iff every player's \|pos - player\|^2 is not below (100 * range + margin)^2 (0x80169FC8; margin r13+0x3270 = 50 from 0x800CA324; two players when 0x8057E760+0xB2) |
| 0x800C9D20 | range_radius(slot) -> f1 = 100 * range + 100 |
| 0x800CA200 | range(slot, float* out): *out = 100 * range |

Constants: 100.0f at 0x805F4148, 10000.0f at 0x805F4160, 50.0f at 0x805F415C. The "player" position = the list at
r13+0x2B34 (vector count +8, data +0xC, 8-byte elements) element's object +8. Measured 2026-10-29
(`docs/DISTANT_RENDERING.md` section 1): that list is the **camera list** 0x8056FED4 (one element per viewport) and
the position is the camera eye, equal to the in-stage RwCamera's frame position - so "player" in this document means
a viewport's camera, and the range is measured from the camera, not from Shadow.

### The whole cycle (disassembled 2026-10-28)

Spawning, leaving and respawning are three different owners:

- **Spawn** - the manager. Its one-player update 0x800CAC08 walks the 0x800 slots each frame. Skipped: while
  restricted (+0x428 bit 0), entries without 0x8000. A live entry (0x2) is only counted (manager +8). A dormant one
  (0x1) with a type goes to 0x800CA8D8: flags 0x2 or 0x20000 -> no; in_range -> clear 0x80040 (`& 0xFFF7FFBF`),
  call the factory's create (factory +0xC) with the slot. The set object's constructor (0x8015AE00 ->
  0x800CA0B8) stores itself at slot +0x0C, sets 0x2, clears 0x4; the object keeps its slot at +0x28.
- **Leave** - the object itself, never the manager. The base gate (vtable +0x1C = 0x8015AC7C, 153 classes; the base
  exec 0x8015AD28 skips the update while it returns true) marks the object done (+4 bit 0) when 0x800C9E4C says so -
  the global reset (r13+0x3274 bit 0) for entries without 0x8000, or the entry's 0x4 - or when out_of_range (range +
  50, so leaving lags spawning by 50 units). ART GADGET has its own gate (0x800E43F0) that fades instead (below).
- **Respawn** - the manager again, for an entry neither dormant nor live (destroyed, collected) whose +0x1C word has
  bit 0: with 0x10000, once the player is out of in_range it sets 0x40; with 0x40 (and 0x208) or 0x800000 it restores
  the saved flags (+0x1C) | 0x1, clears 0x10040 / 0x40, and passes the entry's link id (+0x22) to 0x800CAE24. The entry
  is then a candidate again and spawns when the player returns.

Flags: 0x1 may spawn, 0x2 live, 0x4 asked to leave, 0x40 out of range while destroyed, 0x800 never leaves, 0x8000
runs while restricted, 0x10000 respawns, 0x20000 held, 0x80800 always in range.

Beyond the range the game draws nothing of an object; what can be seen is bounded by the camera's far plane (RwCamera
+0x84; fog end is pinned to it). Measured: Final Haunt about 3,180; Glyphic Canyon (stg0201) 25,000.

Classes load their models when the stage loads (the factory's preload, factory +4: 0x8006B59C by name /
0x8006B634 by id; enemies their archive, e.g. BK_SOLDIER `enemy/BkSoldierData.one` as resource type 0xC via
0x8001299C), so every model is resident before anything spawns.

## Why the range is not scaled

A first version scaled the range itself (`--draw-distance` multiplied 100 * range in all four functions). Measured on
the Westopolis replay, it changed the game. Scaling every class let enemies (GUN_SOLDIER, BK_SOLDIER, BK_WINGSMALL),
CityLaser and FallBuilding wake up early; the recorded inputs no longer fit and Shadow died (camera at 88 s: x = -3160
at scale 1, x = -70 at scales 2 and 4). Scaling only "passive" classes still diverged at scales 2 and 6. And at x8 the
game's own heap ran out in Final Haunt after ~101 s (malloc 0x803A3E14 returned 0 in operator new 0x803A12A4 ->
bad_alloc -> abort), because far more objects stay alive. The user's rule: no cap, no heap growth, no larger range -
a native solution. So the range stays the original's, and only the renderer looks farther.

## Far-object proxies

Each frame, at the end of the in-stage camera's bracket (`RwDriver::camera_end`, camera == `*0x8056FF1C`), the driver
draws, through that camera's pass, every layout entry the game has not spawned but would spawn if a player were in
range, using the game's own model data. Reads guest memory only (the mesh instance slot RpGeometry+0x58 is the
lifted renderer's own); no game object, no guest allocation.

**Which entries** - exactly the spawn loop's (0x800CAC08 / 0x800CAA2C) candidates, minus the range: slot type
(+0x10) >= 0; entry flags 0x1 set ("may spawn"; cleared when the object is destroyed or collected), 0x2 clear (set
while the game's object is alive: measured, `0x1001 -> 0x1003` at spawn, back at leave, `-> 0x1000` when destroyed),
0x20000 clear; and while the manager's restricted bit (+0x428 bit 0) is set, 0x8000 set.

**Which model** - resolved from data, never by spawning. Done for ART GADGET (factory "ART GADGET", 0x800E4360 ->
0x800E5934), the props that make up most of a stage (Westopolis 475 of 1133 entries, Final Haunt 755 of 1332):
- The entry's misc data is one word, the gadget id (0x800CA1A4).
- The library is the resource of type 1 named "ARTISTGADGET.BIN" (0x80012700). The resource manager (r13+0x2B7C) is a
  std::map<int, container> (header +4, root at its +0; node left +0, right +4, key +0xC, value +0x10; lower_bound,
  signed). Type 1's container is a ResourceOneFileReq (find 0x8006B090: records of 0x3C at +0x14, count +0x18, name
  inline, first match; object by kind 0x8006B114: 7 -> (+0x28)->+0x14, 0xA -> +0x24, 6 -> the loaded binary's
  "SCENE" symbol 0x8040F50C, else +0x20); a ResourceOneFile (find 0x800148E0, last match first) is read too. The
  library is the BIN's SCENE: its data +0x20.
- Library: gadget records (+0x28, 0x48 bytes, count +0x2C; id +4, model-entry name +8), model entries (+8, 0x10 bytes,
  count +0xC; name +0, part list +4 of {_, part index} pairs, count +0xC), parts (+0, 0x40 bytes; DFF name +4,
  translation +0x18, rotation X +0x24, Y +0x28, Z +0x2C in degrees). 0x80162DBC / 0x80162EB4.
- Each part is its DFF (resource type 1, kind 3: the RpClump at +0x20; e.g. CT0100BENCH.DFF), placed by
  T(+0x18) * Ry * Rx * Rz (RwMatrixRotate Z replace, X and Y postconcat, then translate; axes 0x804C392C/14/20).
- Every atomic of the clump is posed by its frames' modelling matrices below the clump root.

The other classes (`kRules` in far_objects.cpp) are read the same way from their constructors: the holder
0x8015FBE4(out, type, name) or 0x8015FDBC(out, type, id) gives the library, 0x80162E4C(holder, key, _) the model entry
whose +8 is the key, and the class's own matrix routine its frame:

| class | constructor / model | library | model key | frame |
|---|---|---|---|---|
| SPRING | 0x801210F4 | "SPRING.BIN", type 0 | 0 | T * Rx * Rz (0x8012081C: never turns about Y) |
| DASHPANEL | 0x800FFC90 | id 0xD0 | 0 | T * Ry * Rx * Rz |
| JUMP_BOARD | 0x8010F13C | id 0xD8 | 0 | T * Ry * Rx * Rz |
| CONTAINER | 0x800F8660 / 0x800F6FC8 | id 0xCA | misc word 0 (of 12) | T * Ry * Rx * Rz, Y + 10 (0x800F7B24) |
| HRD_CONTAINER | 0x800FABE0 / 0x800FA0B0 | id 0xCB | misc word 0 (of 12) | T * Ry * Rx * Rz, Y + 2 |
| WEAPON_CONTAINER | 0x800FDE80 / 0x800FD3F8 | id 0xCE | misc word 0 (of 8) | T * Ry * Rx * Rz, Y + 10 |

A class is drawn by default only once its pose is measured equal to the game's: at every handover the proxy's atomic
is compared with the nearest game atomic of the same geometry (containers stand 20 units apart, so the nearest, not
the first). `SHADOW_FAR_VALIDATE=1` draws the unverified classes too and the exit report gives each class's
candidate-frames, resolved, drawn, handovers compared and the largest matrix element difference.

State follows from the candidate rule, per class alike: an entry is drawn only while the spawn loop would spawn it, so
a destroyed container or collected item (0x1 cleared) has no proxy, and one the game would bring back has one. The
entry-flag helpers (0x800CA284 / 0x800CA298) touch 0x4, 0x20000 and 0x800000 only.

**Where** - the entry's frame: position (+0) and rotation (+0xC, degrees) as R = Ry * Rx * Rz. Fitted to 162 spawned
objects' real world matrices: 147 exact (< 1e-3); the rest are classes that add their own transform (SPRING, CAGE).

**When** - inside the view (bounding sphere against the view's slopes and near/far) and at least one pixel across:
radius * focal / depth >= 1, focal = (render height / 2) / the view window's half height. A proxy begins below a
pixel, so it cannot be seen to appear; the slider's 2..7 cap it at that many times the entry's authored range.

**Handover** - when its entry spawns (0x2 set), a proxy shown last frame is kept until a model of the game's is drawn
at the entry's position (the nearest of the same geometry within 20 units; the real objects draw earlier in the same bracket), at most 3 frames, and not
at all when one already drew there this frame. A destroyed entry (0x1 cleared) has no handover. A draw the game makes
fully transparent (every material's alpha 0) does not count as drawn.

### The game's fade

ART GADGET fades its model at the range's edge (the only class that does; the holder's fade starters have one caller
each):
- out: its gate before each update (0x800E43F0, vtable +0x1C; the base exec 0x8015AD28 skips the update while it
  returns true) starts `0x80160078(holder, -1)` once every player is past the range + 50 (0x800C9D54), sets the
  object's +0x80 bit 3, and marks the object done when the holder's fade is done (0x8015FFDC); the manager then
  removes it.
- in: the constructor (0x800E5CD8) starts `0x80160004`.
- the holder (object +0x38): +0x3C bit 7 while fading, bit 8 out / bit 9 in, bit 10 once faded out; alpha +0x44,
  step 0.15 (+0x48, 0x805F5BBC) - not seconds: each update adds `0.98 * step` (0x801636A8), about 7 updates;
  each frame 0x80163670 steps it and `0x801601BC` writes it into the holder's own
  materials' colour alpha (the first frame prepares them, `0x801639BC`, and draws nothing).
- slot +0x0C is the live object (0x800CA0B8), object +0x28 its slot.

The original therefore fades a prop out at its range and back in as it comes into range. With proxies that read as a
blink - the prop faded out (with an empty first frame), then its proxy appeared at full opacity; or, coming closer,
the proxy handed over to a model at alpha 0. Measured on the user's Gun Fortress range (F9 markers,
docs/tools/REPLAY_MARKERS.md): 47 on-screen gaps over the run. So while an object's holder fades (bit 7) or has faded
out (bit 10), its proxy is drawn at full opacity whether or not the game draws the fading model: after the fix 0 gaps,
the crates steady through every frame (`stood in for the game's fade` in the exit report).

### Measured

| route | frames | ART GADGET candidate-frames resolved | proxy entry-frames drawn | meshes | handovers |
|---|---|---|---|---|---|
| Westopolis replay, 90 s, rendered 1280x720 | 5054 | 2,190,054 of 2,190,054 | 326,814 | 437,812 | 148 (3 timed out) |
| Final Haunt tape, 335 s, rendered 1280x720 | 19,799 | 14,176,625 of 14,176,625 | 1,658,261 | 4,196,663 | 483 (42 timed out) |

Per class, both tapes at `--draw-distance 8` (2026-10-27): every candidate-frame resolved; handovers compared (largest
matrix difference): ART GADGET 584 (6e-8), SPRING 6 (0), DASHPANEL 15 (0), JUMP_BOARD 13 (0), CONTAINER 12 (0),
WEAPON_CONTAINER 5 (0) - all drawn by default. HRD_CONTAINER resolves (Westopolis 55,594 of 55,594) but no handover
was sampled: not drawn until measured. With all six classes the Final Haunt tape still ran its full 19,799 frames,
499 handovers (45 timed out, as with ART GADGET alone).

The Final Haunt tape (the route whose x8 range scaling ran the heap out at 101 s) ran its full 335 s at
`--draw-distance 8`: 20,043 frames presented, no abort. Frames captured at draw distance 1 and 8 differ only where far
props appear (Westopolis frames 900-2400: 0 to 626 changed pixels, all in distant regions; frames 1200 and 1800
identical).

## Fade-in (default off)

`gekko/appear_fade.h`, `scaffold/rw_driver/pipelines.cpp` (per atomic), `lift/render/shaders/mesh.hlsl`
(`dissolve_test`); `--fade-in-ms <0..5000>` (default 0). A model drawn now that was not drawn for 4 s (or never), whose
position was inside the previous frame's world view and at least 200 units from its eye, dissolves in with a 4x4
ordered dither (DrawConstants' former pad word; zero-initialised draws are unaffected). It is off because the rule
also catches deliberate effects - fireballs and other meshes the effect system creates (Effect:: classes, code
0x80027xxx-0x8043xxxx) - which must not fade. The proxies made it unnecessary for the props.

## Enemies and characters (in progress, 2026-10-28)

Glyphic Canyon's fall (the user's F9 range, census `SHADOW_FAR_CENSUS=720:880:80`): in view at the landing and not
spawned - BK_SOLDIER 21 (nearest 916), GUN_SOLDIER 8 (602), BK_WORM 7, BK_WINGLARGE 11, BK_WINGSMALL 6, BK_LARVA 5,
SET_CHARACTOR 7 (287; Knuckles and the other characters) - enemies' ranges are short, so the open area looks empty
until the player lands next to them. Far plane 25,000: not the limit there.

What an enemy stand-in needs, from the data:
- The enemy table at 0x804CCFDC: 16 bytes per kind {archive, DFF, BON, MTP} (GunSoldier row 0 ... BkSoldier row 5,
  BkWingLarge 6, BkWingSmall 7, BkWorm 8, BkLarva 9, BkChaos, BkNinja, EggPawn, EggPierrot, ...); read through four
  virtual accessors shared by every enemy class (0x801A6868 archive, 0x801A682C DFF, 0x801A67F0 BON, 0x801A67AC MTP;
  the kind is EnemyCharInfo +0x20). The archive (e.g. `enemy/BkSoldierData.one`) is preloaded as resource type 0xC.
- `.DFF` the skinned clump; `.BON` Sonic Team's bone hierarchy and `.MTP` its motion pack - big-endian, both starting
  `04 01`, the model's name, then per node channels (not RenderWare's RtAnim stream). The MTP is {u16 1, u16 count,
  ...} then from +0x14 one 12-byte entry per motion {name offset, motion offset, extra offset}: the soldier has 22
  (`soldierbk_alerta/b/c`, `_runa/b/c`, `_guard`, `_fall`, `_landing`, shots, damage ...).
- The evaluator: `runtime/lift/motion` (`shadow_motion`). `curves` and `pose_math` are the animation research's
  bit-exact transcriptions (the user's package `H:/Unresolved Shadow functions`: `native_motion_v7`, its MOTION_REPORT.md
  and the A2 / A3 reports; the test-only mutant switches removed); `skeleton_pose` adds the .BON / clip parsing, the
  chunk binding (8041C30C) and the single-channel pose (time advance 80419A9C with the loop wrap and cursor rewind,
  components, world = parent * S_pre * T * Rz*Ry*Rx * S). Checked with `tools/motion/motion_pose_check.cpp` against the
  research's reference player (`motion_player.py`, itself bit-identical to the game's pose slots in memory snapshots):
  every clip of BKSOLDIER (22), BKWINGLARGE (7) and BKLARVA (1), 150 frames each - every bone matrix bit-identical.
- In memory: BON banks are a list at [r13+0x5F30] (link +0x14; wrapper +0 record count, +0x10 first record; records
  consecutive, 0x30 + 0x70 * bones; child / sibling words absolute); motion banks a list at [r13+0x5F58] (link +0xC;
  +0 the bank's first clip id, +2 clip count, +8 the free callback 0x8041214C). A bank whose byte +6 is set keeps its
  clips in ARAM (every enemy bank in Westopolis and Glyphic Canyon): from +0x1C one 0x18-byte record per clip of
  ARAM addresses {name, name size, clip, clip size, events, events size}, +0x18 the list of clips copied into main
  memory on demand (the loader 0x80414370). Any other bank holds the file's table of {name, clip, events} pointers at
  +0x10. The renderer reads ARAM read-only (`LiftedRender::set_aux_memory`, `System::sample_memory`).
- Drawing (2026-10-28): `FarObjects::enemy_for` resolves a kind once its banks are loaded (a failed lookup retries
  every 60 frames; validation prints the reason once). The palette is placement * bone pose * inverse bind[b], skin
  bone b = the skeleton bone with the hierarchy node's HAnim id; the skinned stand-in draws with no world of its own
  (`RwDriver::draw_proxy_atomic` with a palette). All far stand-ins of a kind share one running loop of their clip.
- Handover, measured on the Westopolis tape (`SHADOW_FAR_VALIDATE=1`): the game's skinned atomic hangs on the node
  whose frame carries HAnim id 40 (soldier) / 4101 (small wing) - bone 1, the hips - so the comparison predicts
  placement * that bone. The game restarts the clip at the spawn: the small wing's first draw is `wingbksmall_fly`
  frame 0 at the entry's own yaw to 0.001; 4 of 8 handovers exact, the other 4 equal in rotation and 0.5 apart along
  one axis (the wing's own first flight step). The stand-in therefore shows the clip's first frame while it hands
  over. The soldier's first draw is already in its run cycle (`soldierbk_runa` frame 58, matched offline over every
  clip): its AI runs from the spawn, so no fixed clip matches (difference 4, the pose; placement equal). Of 11
  enemy handovers that timed out, 8 were enemies the game never drew near their entry (flying off, or not drawn).
- Glyphic Canyon range 1 (`replay_markers.py inspect --range 1`, `SHADOW_FAR_VALIDATE=1`): an upright, animated
  Black Arms soldier stands on the plaza at draw distance 8 and is absent at 1.
- 2026-10-29: a live enemy's object (slot +0x0C) keeps its root position at +0x40 (the game's skinned draw - the hips
  - stays within a few units of it while it moves; Westopolis tape, BK_SOLDIER and BK_WINGSMALL). Not every soldier
  is late: one was drawn 2 frames after its spawn. The layout data of enemies (misc words): BK_SOLDIER word 8 the
  weapon, word 9 the AppearType (0 STAND, 1 LINEAR_MOVE, 2 TRIANGLE_MOVE, 3 RANDOM_MOVE, 4 OFFSETPOS, 5 WARP per the
  editor strings at 0x804C9BF4); neither tape has a WARP soldier.
- Open: cross-model binding (BkWorm pairs BKWORM6.BON with the `bkworm` clips: bind through the clip model's BON,
  8041C270); BkLarva (no skin); GunSoldier (its clump is under no resource type up to 0x40); SET_CHARACTOR; enemies
  stay unverified (drawn only with `SHADOW_FAR_VALIDATE=1`) until their handovers are decided.

## Props (2026-10-28)

Every class's model comes from its create function: the library call (0x8015FBE4 by name, 0x8015FDBC by id; its
first argument the resource type) and 0x80162E4C(holder, key, ...). A scan of each factory's functions for those calls
(the factory struct is {name, three function pointers}) gives the library and whether the key is a constant or an
object field. Each class is drawn in play only once its handovers measured exact on the Glyphic Canyon and Westopolis
tapes (`SHADOW_FAR_VALIDATE=1`):

| class | library | key | handovers |
|---|---|---|---|
| HINTRING | type 0 HINTRING.BIN | 0 | 10 / 10 exact |
| SAVEPOINT | type 0 id 0xDC | 0 | 1 / 1 |
| CityChaosEmerald | type 1 CITYCHAOSEMERALD.BIN | 0 | 1 / 1 |
| STICK | type 1 STICK.BIN | 0 (no holder key) | 16 / 16 |
| STONE_POLE | type 1 STONEPOLE.BIN | misc word 0 | 1 / 1 |
| CAGE | type 0 CASE.BIN | misc word 0, misc 1..3 its scale (object +0x44) | 2 / 2 |
| FallBuilding | type 1 FALLBUILDING.BIN | misc word 0 (object +0x38) | 3 / 3 (0.0005) |
| RING | (above) | | 64 / 64 |
| ART WORK | | | 20 / 20 |

Waiting for a handover sample (drawn only with `SHADOW_FAR_VALIDATE=1`): LONGSPRING (0xD9), BALLOON (0xC2), Fruit
(0xD3), TORNADO, SECRETKEY (0xDE), ROLL_CIRCLE, MOVE_FOOTING, STONE_STATUE, UNBRK_CONTAINER (0xCD), DOOR, GOALRING,
POWER_DEVICE_CAGE. Not a prop: CityLaser (an emitter whose beam the game draws while it fires: 16 handovers, models up
to 4083 apart). No library call found: BlockChange, BRK_OBJ (holder key only), BOMB, SECRET CONTAINER, GUN_BEETLE.

Coincident copies: two entries may place one model at one spot turned apart (ART WORK at -122, -590, -4187 in Glyphic
Canyon, yaw 0 and 180). A handover matches the game's draw nearest in place, then nearest in matrix.

### Handover gaps

Some objects first draw well after their spawn: ART GADGET / ART WORK entries flagged 0x8000 in Glyphic Canyon
(58-65 frames), Black Arms soldiers (134-254 frames, their warp-in), small wings (8 frames, or elsewhere on their
path). A prop's proxy now stays while its entry is live and the game has not drawn the object, up to 120 frames
(`kHandoverHold`); it goes the frame the entry stops being live. Glyphic Canyon: 0 handovers time out (3 before).
Westopolis: 2 ART GADGETs (ids 0 and 2, flags 0x1001) spawn, are never drawn and are deleted - their proxies show
before the spawn something the game never shows; open. Enemies keep the 3-frame window (they move or warp).

The event flags ([r13+0x3274] bit 0, set by TPjsEventActionManager's constructor 0x80173EF4 -> 0x800CB8C0, cleared by
the layout load): while set, the leave test 0x800C9E4C sends every object whose entry lacks 0x8000 away the frame it
spawns, so the proxies skip those entries then, as for the manager's own restriction (+0x428).

## Layers (2026-10-29)

The stage is drawn in display layers. 15 `LandDispManager` entries at 0x8051E328 (0x18 bytes: name `_N_`, task name,
flags, ..., the layer's RpWorld at +0x10): `_N_ _D_ _OS_ _OSL_ _O_ _OW_ _OL_ _P_ _AF_ _A_ _AL_ _K_ _M_ _G_ _GL_`. The
landscape blocks' files carry the same names (`STG0100_D_01.RG1`, `_G_`, `_O_`, `_OS_`). `0x800471BC(land, layer,
camera)` draws one layer: each of the 32 land block slots' world for that layer, then the layer's own object world
(if it holds clumps): RpWorldAddCamera, RwCameraBeginUpdate, `0x80046D80(land, flags, world)`, RpWorldRender,
RwCameraEndUpdate, RpWorldRemoveCamera. So each layer is its own pass, at its own place in the frame.

Layer flags (entry +8; 0x80046D80): bit 0 = lit - the world's flags get 0x30 and the light manager's lights are
attached to it (0x80047E24); otherwise they are detached and the world's flags lose 0x30. Bits 0x40, 0x80, 0x100,
0x800 set the layer's blend and depth states (0x40: source alpha / inverse source alpha, depth write off; ...).
Values: `_N_` `_D_` 0x28, `_OS_` `_O_` 0x2C, `_OSL_` `_OL_` 0x2D, `_OW_` 0x3C, `_P_` 0x80C, `_AF_` `_A_` 0x5C, `_AL_`
0x4D, `_K_` 0x9C, `_M_` 0x11C, `_G_` 0x98, `_GL_` 0x99.

A model holder puts each part's instance in one layer's world (0x80163C14). The part record (0x40 bytes in the model
library) decides: record +0x38 bit 1 set - the layer the part's geometry asks for: the first int user-data array
named `ATTR_WORLDPARM.ATTR_WORLDPARM` (RpUserData plugin at geometry + [r13+0x6000] = {count, arrays}, array 0x10
bytes {name, format, count, data}; 0x80169748 over the clump's atomics; value 0 when none) through 0x8052C7E0 (4 4 5
6 2 1 8 9 11 13 14 12); clear - 0x804C39BC[record +0x30] (6 4 2 5 1 9 8 4 11 11 13 14).

The light manager ([r13+0x2C30], static 0x80571DD8): light 0 the ambient (RpLight subtype 2), light 1 a directional
(subtype 1, root frame). Group records at +0x20 + 0x34 g (17 groups; the stage's `stgNNNN_light.bin`, 16 x 0x34 =
832 bytes): +0x00 light 0's colour, +0x10 light 1's colour (RGBA floats), +0x2C and +0x30 two angles a, b in degrees.
Each part's instance stores the group 0x804C397C[record +0x34] (0x8016454C, with the part, in the atomic's plugin
at [r13-0x7D08] + 0xC) and gets the render callback 0x801645D0, which before every draw requests the group
(0x800483B8, +0x456) and applies it when it changed (0x80048308, +0x454): the colours through RpLightSetColor
(0x80047808) and light 1's frame by RwFrameRotate(X, a, replace) and RwFrameRotate(Y, b, postconcat) (0x80048184), so
its direction is (cos a sin b, -sin a, cos a cos b). Verified live: group 4 (a 70, b 90) gives (0.3420, -0.9397, 0).

### Parts in two worlds: the dash ramp (the user's report, Westopolis)

"The dash panel renders in two parts after the falling part ends" - the four JUMP_BOARD dash ramps at the top of the
slope after the landing (entries 0x809B6340 / 6684 / 66B0 / 66DC, range 10 = 1,000 units).

- Two parts, two layers: the base (`ob_hnal0`, geometry flags 0x37 - lit, not prelit) is in layer 6 `_OL_` (world
  0x807A25A0), the arrows (`ob_hnuv0`, 0x3F - prelit, UV-animated pipeline 0x80786100) in layer 4 `_O_` (world
  0x807A2260); both records have bit 1, so their geometry's ATTR_WORLDPARM picks the layer. Both use light group 4.
- The game draws `_OL_` early in the frame and `_O_` in its last pass (renderFrame 7BB6 and 7BDB of frame 806). The
  layout manager spawns between them: on the spawn frame (805) only the arrows are drawn, the base from the next
  frame. Every instance is tied to a sector, flagged to render and placed on the spawn frame - nothing is late but the
  pass. This is the original engine's frame order (measured at draw distance 1).
- The proxies used the lights of whatever world was current when they were collected (the last pass, an unlit
  layer): the lit base drew unlit - black. From afar: bright arrows over a black base; at the spawn the base appears
  lit. The same for DASHPANEL (base 0x81079C60, flags 0x37).

The proxies now (far_objects.cpp `layer_of`, `light_group_of`; pipelines.cpp `RwDriver::layer_lights`): each part
carries its layer and light group from its record; it is lit only if its layer is, by the light manager's lights set
to its group, as `light_set` would read them at the game's draw. And the handover goes part by part: a part the game
has not drawn yet stays a proxy (this runs after the frame's last pass, so by the first full frame after the spawn
every part the game draws has drawn; a part still missing then is one it never draws - a cage's, a stick's - and goes).

Measured (Westopolis tape): every frame around the spawn each part is drawn exactly once (by the proxy or the game);
the proxy's material, texture and lighting flags equal the game's draw of the same part (P0 M0 L1 X1 base, P1 L0 X0
arrows); `SHADOW_FAR_VALIDATE` compares every proxy part's lights with the game's draws in the same layer world and
light group in the same frame - Westopolis 45,899 compared (3 lit, the ramp base: ambient, colour and direction
equal), Glyphic Canyon 1,461,745 (unlit), 0 different. Draw distance 8 twice: 30 of 30 frames identical around the
spawn; draw distance 1 and 8: identical guest memory after 40 s. The ramps spawn behind the building wall on this
tape; the DASHPANEL at 74 s (screen centre) shows the change: black silhouette before, lit panel now, equal to the
game's own draw after the spawn.

Tools: `SHADOW_FAR_TRACE=<class>:<first>:<last>` now also runs at draw distance 1 (nothing drawn), lists which parts
the game draws each frame, describes the parts (record, layer fields, textures), the light manager and the layer
worlds, and `[far-pass]` lines with each draw's renderFrame, camera and world; `SHADOW_FAR_WATCH=<atomic>,...` the
sector renderer's gates (0x80460418) for those atomics; `SHADOW_R2_TRACE_FRAME` lists proxy draws too.

### Landscape blocks (read, not yet tied to this runtime)

0x8003AAE8 (on TObjLand, r13+0x2B5C = 0x80572560): 0x8003A8D4 builds a mask of 168 blocks - a block is in when a point
lies in one of its boxes (0x8003A578: min <= p < max per axis) from the stage's `STGnnnn.BDT` (in `stgnnnn_dat.one`:
a 0x40-byte name, count, then 0x20-byte records {block, min, max, pad}; Westopolis 28, Glyphic Canyon 36, whose block
90 spans the whole stage); the point is [+0x3C] (the event action manager's copy of player 0's position, 0x80177510)
or player 0's position. 0x8003A6DC then loads the blocks in either mask buffer, unloads the others and lists the
loaded (state 5) ones at +0x828. Read at the in-stage camera's end in this runtime, +0x28..+0x5C, +0x588, +0x828
are all zero on both tapes, while the land draw (0x800471BC) runs - where this runtime keeps the land state is open.

## Open

- HRD_CONTAINER: needs a handover sample before it is drawn.
- RING (factory 0x80527D04, group object 0x80114564): no holder - a group of `count` rings placed by pattern (misc of
  0x14: pattern 0 point / 1 line / 2 circle / 3 wave, count, length or radius, amplitude; 0x801130A4), models by id
  0x7C and 0x5E (0x8001277C), and each ring's collected state restored from a per-slot bitset that outlives the
  object (0x801124C4 after a dynamic cast at 0x80113A74). A proxy must read that bitset, so collected rings stay
  gone.
- UNBRK_CONTAINER (0xCD, key from the object's +0x44 at 0x800FCAE8), ITEM_BOX (0xD7), DASHRING, SECRET CONTAINER.
- Enemies: animated stand-ins exist ("Enemies and characters") but stay unverified, so in play enemies still appear
  at their original range.
- The Vulkan backend draws the proxies through the same packets; it reads `dissolve` too (render_vk
  renderer_draws.cpp).
- Layers (2026-10-29): proxies take their layer's lights; not yet its blend and depth states (flags 0x40 / 0x80 /
  0x100 / 0x800 - alpha and additive layers `_OW_ _AF_ _A_ _AL_ _K_ _M_`). They still use the recipe of the game's
  last plain world model. Parts from a bare clump (no part record: RING, the enemies) keep the current lights.
- Parts the game never draws (ART GADGET part 1, CAGE part 4 / 6, FallBuilding part 2, STICK part 1, every
  geometry with render flags 05): why - and whether proxies should leave them out from afar - is not reversed yet.
- The light-group directions are verified for group 4 only (live); the other groups' angles follow the same code.
