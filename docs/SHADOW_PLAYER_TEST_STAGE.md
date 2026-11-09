# Player test stage: Shadow alone on a flat plane (2026-11-03)

This is a test environment for the player reconstruction (`docs/SHADOW_PLAYER_NATIVE_RECONSTRUCTION.md`). Westopolis
(stg0100) is booted with its collision replaced by one flat plane and with no set objects, so Shadow is the only
entity.
- **Only the environment is synthetic.** Every player function that runs is the original game's code, and the oracle
  compares it against the original words exactly as on any stage.
- **It is a development artifact.** It lives in `.build/player/teststage/game`, is never deployed, and leaves the
  original game files untouched.
- **Built with this repo's own tools only** (user decision 2026-11-03; ShadowRecomp stays unused).

## Build

```
python -B tools/player/make_test_stage.py ../game .build/player/teststage/game [--half-size 20000] [--y -100]
```

**Overlay.** Every file of `game/files` and `game/GC` is hard-linked. A replaced path is unlinked first and written as
a new file, so no original is ever written through its link.

**Collision.** `stg0100_01.one` gets `STG0100_COLI_01.BSP` as a square of 2 triangles at y = −100, from −20,000 to
+20,000 in x and z.
- **Template.** It is not synthesised from a specification. It is a real 2-triangle collision world of the game,
  `stg0908_01.one STG0908_COLI_01.BSP`: world, material, atomic sector, BinMesh, RpCollTree with one leaf (2 entries,
  0 splits) and user data.
- **What changes:** only the 4 vertices, the 3 bounding boxes (world, sector, tree) and the 2 per-triangle
  `attribute` values.
- **Attribute 0x20001** is Westopolis's walkable floor: 1,569 of its up-facing collision triangles carry it. The
  second user-data array, `FVF.UserData` = 0x3003, is the same in both stages.
- **Winding** puts both normals at +y, as Westopolis floors are wound.

**Other blocks.** The other 18 blocks with a collision world get the same world shrunk to a 1-unit square 100,000
units below the stage. Every archive keeps a valid world, and no block adds floor or walls.

**Layouts.** `stg0100_cmn / _nrm / _hrd / _ds1.dat` become `sky2` layouts with 0 objects: no enemies, items, springs,
rails or the stage-start launcher.

**Everything else is original:** visual meshes (.RG1), cameras, paths and textures.

**Archives.** The rewritten archives keep the original header, slot order, names and member order. Replaced members are
stored as literal-only PRS (`tools/player/stage_files.py prs_store`; `prs_expand` round-trips it). Every untouched
member is byte-identical to the original, which was checked after the build.

**Spawn.** Shadow spawns at about (0, −93, 260). This was measured, not assumed:
`GEKKO_WORLD_COLLISION_TRACE=<n>` is an env-gated diagnostic in `native_world_collision.cpp` that prints the first n
queries, and the player's first body sphere is centred there. The original floor under him is at y ≈ −100, hence the
plane height.

## Running routes

```
GAME=$(cygpath -m .build/player/teststage/game) bash tools/player/run_route.sh <out> <recording dir> stg0100 <route.tape> <budget ms>
GAME=... TAPE=<route.tape> STRICT=1 bash tools/player/run_verify.sh <out> <recording dir> stg0100 <scope file> <budget ms>
```

- **Routes.** `tools/player/route_tape.py` cuts a recording (configs only; cut at 1 s, before any input) and appends a
  scripted controller segment.
- **Acceptance.** `run_route.sh` counts the 35 behaviour-state creators (factory table 0x80522DE8) with the runtime call
  census. A state counts as entered only when its creator ran.

## Results so far

- **Neutral input, 15 s.** Fall → Landing → Idle, then nothing. No Launch, no fault.
- **Movement route, `.build/player/flat/move.txt`, 24 s:** Idle, Ground, Fall, Landing, Jump (3), AirAttack, SpinDash,
  JumpDash.

- **Strict oracle on that route** (`.build/player/flat-strict-move`, player plus batch 01 scope, 79 s):
  - 12.1 M segments matched, 0 mismatched, 0 executions with a wide frC;
  - 23,726 member instructions replayed;
  - **697 of them were reached by no earlier strict run**, almost all SpinDash: update, enter, exit, onCommand and
    SpinDashCommand.

**Controller mapping** (real-stage experiments on Westopolis, cut at 30 s):
- B on the ground → GroundAttack;
- hold X on the ground → SpinDash;
- jump + B → AirAttack;
- Y, Z, L and R alone → nothing.

## Limits

A plane has no edges, walls, rails or objects. These states need the real stages and their objects, or deliberate
additions of real geometry or objects to this stage:
- **Edges and walls:** EdgeHang, Ottotto, WallHang and WallJump.
- **Objects:** Grind, LineHang, SomethingHang, Stream, DriveVehicle, PickupObject, Throw, ElectricCircuit, Launch,
  ExternalControl.
- **Other conditions:** Chaos Control and Chaos Blast need the gauge; Super is entered at stage init.

## State routes found (original logic, deterministic tapes)

| State | Where | Route |
|---|---|---|
| 31 Dead | Westopolis | the 30 s cut + `.build/player/routes/scripts/mash.txt` (A mashed with the stick held) |
| 18 SpinDash | Westopolis and flat stage | hold X on the ground |

## The user's test stage (`H:\ShadowTheHedgehogDiskGC_Extract\Test_stage`, 2026-11-03)

```
python -B tools/player/make_test_stage.py ../game .build/player/teststage/game \
    --obj ../Test_stage/Stage.obj --obj-offset 0 -77.8693 0 --obj-texture-png ../Test_stage/Diffuse.png
```

The user's mesh (Stage.obj: 3,896 vertices, 7,576 triangles, one material) becomes both the stage's collision and its
look. The offset puts its base slab's top (y = −22.13) at −100, under Shadow's spawn (0, −93, 260).

**Collision** (`tools/player/obj_collision.py`): block 01's world holds all 7,576 triangles in one atomic sector.
- **Tree.** 2,047 splits, in the layout of the game's own split trees (decoded from stg0100 COLI worlds): 16-byte
  pre-order nodes, leaves of at most 4 triangles, and bounds that are the children's true extents (asserted).
- **Attributes.** Up-facing triangles (normal y > 0.7) are walkable floor, 0x20001. Everything else is wall, 0x2, the
  wall WallHang works on.
- **Winding.** The OBJ's winding already matches the game's: slab tops at +y.

**Look** (`tools/player/obj_visual.py`): block 01's OS render world (.RG1) draws the mesh. Every other Westopolis render
world becomes one triangle far below the stage, so none of the city is drawn.
- **Format.** The game's prepared GameCube sector: position u16, colour u8 and UV0 u16 arrays, plus one GX triangle
  strip per triangle.
- **Fixed parts** come from STG0100_D_20.RG1. The material list and the group header round-trip it byte for byte.

**Texture** (`tools/player/stage_texture.py`): Diffuse.png joins STG0100_TEXCM.TXD as `test_diffuse`, an RGB565
record in the exact form of the game's own (stg0002 `s0002_dn014`: raster flags 0x204, depth 16, one level).

**Checks:**
- **Collision.** On the tour route the game made 4,952 world queries. With `GEKKO_VERIFY_WORLD_COLLISION=1` every one
  was answered by both the native mirror and the original code: 0 differed.
- **Render worlds.** The repo's importer (`world_dump.exe`) decodes block 01's render world into exactly the input's
  7,576 triangles, at float32 precision and with winding kept.
- **Offline converter.** `stage_convert.exe` reads the whole overlay: 143 textures and 7,611 render triangles.
- **In game.** `.build/player/userstage/tex_9s.png` (headless, `--capture-at-tick`) shows Shadow on the user's stage,
  drawn with the user's checker.

**Cameras:** `stg0100_cam.dat` has a 24-byte header and `count` records of 220 bytes (true of every stage's
file). It is written with its own header and 0 records, so there are no scripted cameras and the game's default
follow camera drives the view (user request; `.build/player/userstage/cam_6s.png`, `cam_9s.png`).
- Stick input is camera-relative and this camera swings behind Shadow as he turns, so a route's directions drift.
  Replays stay deterministic.
- Routes recorded under the scripted cameras do not replay the same.

**Not carried over:** Glare.png (an environment-reflection map; the material is single-texture) and the FBX (the OBJ
holds the same mesh).

**Box layout** (`BOXES`, default without `--obj`): walls of 400, 100 and 40 units plus WallHang (13) reached on the
0x2 wall. A standing jump rises only about 21 units, so the 100-unit box is not climbable. EdgeHang and Ottotto need
ledges within reach, and the user's stage has them.

## Recorded routes (the user's play on the test stage, 2026-11-03)

The user played the stage, with the free camera and recording on, and grabbed the ledges by hand.
`--input-tape-record` wrote the input, and the call census logged the states. The tapes are in
`test_data/player/routes/`.

| Tape | Game time | States (census) | Replay |
|---|---|---|---|
| `edgehang_user_session1.tape` | 13 s | **EdgeHang 10×**, Jump 3, Fall 4, Landing 3, Ground 4, Idle 4 | census identical; strict 4.6 M segments, 0 mismatches, 0 wide frC |
| `edgehang_user_session2.tape` | 69 s | **EdgeHang 1×** (7.8 s), held for about a minute, then climbed up (Landing 69.4 s, Idle 70.0 s; no Jump state) | census identical; strict 20.1 M segments, 0 mismatches, 0 wide frC |
| `walljump_user_session3.tape` | 19.5 s | **WallJump 1×** (19.2 s), WallHang 5, JumpDash 5, Jump 5, Fall 7, Landing 6, Ground 7, Idle 6 | census identical; strict 7.6 M segments, 0 mismatches, 0 wide frC; 828 new instructions (WallJump 277 of 334, WallHang 492 of 542 now verified); player total 44,388 of 82,045 |

- **Scope:** both strict replays cover the player plus the batch 01 dependencies (`scope_b01_probe.txt`).
- **New coverage:** 1,075 player instructions that no earlier strict run had reached. They include 297 of EdgeHang's
  318 instructions, and Fall's ledge check `Fall::update` sub13 (113).
- **Running total:** strict coverage of the player is now 43,560 of 82,045 instructions.
- **Climbing up from the hang** runs inside EdgeHang and ends in a landing; it is not a behaviour state of its own.
- **Replaying:** use the stage's tape with `run_route.sh` or `run_verify.sh` via `TAPE=`. The session folder's
  `initial/` holds the configs used, including the user's 3840×1200 window.
