# Stage loading: the original sequence, end to end (discovery, 2026-11-03)

Shadow the Hedgehog, GameCube PAL `main.dol` (pal-v30 names). **Discovery only: nothing here is implemented.** The
native batches in section 8 wait for the user's approval.

Evidence:
- **Static:** the dtk assembly (`source/.build/dolrecon/pal-v30/build/asm`), string references (`dolrecon.strrefs`)
  and the fixed call graph (`dolrecon.callgraph`).
- **Dynamic:** one traced Westopolis load (section 7).
- **Detailed notes**, with the instructions quoted: the shared session-notes folder
  (`re_A_control_flow.md`, `re_B_world_data.md`, `re_C_objects_resources.md`).
- **Not repeated here:** facts already in other docs are cited, not restated: `NATIVE_THREADS.md` (loader threads),
  `DISC_SYSTEM_MAP.md` (disc layer), `DRAW_DISTANCE.md` (spawn ranges) and `SHADOW_PLAYER_TEST_STAGE.md` (file
  formats). **One correction:** the landscape-block state is on TBlockManage, not on TObjLand (section 3.3).

## 0. Summary

- **About one second of game time on Westopolis.** The hand-off at 1.267 s, the first gameplay frame at 2.250 s
  (`StageAction::PlayAction` constructed).
- **75 files.** The ordered list is in section 7.
- **The load is a chain of main-thread Tasks.** The only real threads are TBGRead / TExpandBG (one file read or one
  PRS decode each) and CRI's servers.
- **The loading screen is a Task.** CNowLoadingThread presents its own frames; it is not an OS thread.
- **Most files load synchronously**, inside StageManager's first update:
  - the set layouts, camera, light, fonts, sound banks and HUD CSD files;
  - every object archive (`stg_cmn_gdt.one`, `stgNNNN_gdt.one`, `_fx.one`, weapons, vehicles, enemies, characters).
- **Asynchronous** (TBGRead, then a TOneFileAsync Task with TExpandBG): `stgNNNN_dat.one`, `.fog`, the landscape
  blocks `stgNNNN_NN.one` and their render / collision worlds.
- **Census** (`test_data/stage_loading/`):
  - 737 loader functions (64 KB), plus 424 interface functions they call;
  - 311 members ran on Westopolis, 205 of them first in the load window;
  - **none is native yet.**
- **Already native below the loader:**
  - the file devices (DVD API, RtGCNFS, CRI gcCi), the directory scan and the PRS expander;
  - memcpy, world-collision queries, object range leaves, link groups;
  - the renderer's ONE / TXD side channel.
- **Already native next to it:** RenderWare's world stream read exists natively in `rwcore`, but it is not wired into
  the game.

## 1. Request to StageManager (the front end)

| # | Address | Function | Role |
|---|---|---|---|
| 1 | 0x802B7934 | RequestPhase(ctrl, id) | **Select Mode:** the confirm writes GameSystem 0x805F0A6C = 5, StageSequence kind 0x805ED220 = 0 (PlayOnce), the stage id 0x80585788 and play flags 0x8057E8A8..AA, then requests AdvReal phase 0. **`--boot-stage`** makes the same writes at AdvFolder's first phase request (`System::install_boot_stage_hooks`, `docs/tools/STAGE_CONTROL.md`). |
| 2 | 0x801E2210 | per-frame driver | Called once per pass of the main loop (0x800511E4). Its switch step 0x801E5B70 builds `StageSequence::Manager` for system kind 5. |
| 3 | 0x802CE764 | StageSequence::Manager vf1 | **First frame:** picks the phase from 0x805ED220 (jump table 0x805592A8; 0 = PlayOnce, 1 = PlayStory, 2 = PlayEvent, 7 = PlayFlashback, ...). |
| 4 | 0x802D10BC → 0x802D11CC | PlayOnce::Phase ctor → builder | `new StageManager(*0x80585788, 0)` |
| 5 | 0x80178C2C | StageManager ctor | **Pauses background reads:** 0x805EF1DC = 1 (debug text "裏読み停止"). **Shows the loading screen** (NowLoading 0x802BEFA4, CNowLoadingThread 0x802BECB0) and asks the stage-action controller 0x80577038 for InitAction (ctor 0x80206068). |
| 6 | 0x80178A48 | StageManager vf1 (every frame) | **State machine:** 0 = build (0x801783D4); 1 = retry the dat.one request; 2 = run the stage-action controller; 3 = teardown. |

## 2. StageManager state 0: 0x801783D4 builds the stage (one call, 1.317 s)

All in one main-thread call, in this order (traced: everything has the same first tick).

**Engine tasks.** CharCommandManager, Effect, MissionManagerTask 0x8016E4EC, SetManagerTask 0x800CB820, CharaColli
manager, AudioController::ActStageController 0x801D3EA8, LandDispManager 0x800460C0 / 0x8004625C, the full-screen
shimmer, BulletManager, the camera pre/post tasks 0x801D0CE0, StageCameraManager 0x801CEC6C, the light setup
0x80177798, TPjsEventFolder, TGameInfomation, TFogSet 0x8016AD3C, TStageInitalizeTask 0x80177684, TStageKeyNoInput,
TargetManager, the player post manager, the message task.

**Synchronous reads inside it.** Most go through 0x800450C4: a whole-file read through the RW stream / RtGCNFS
owner.
- `common/ScoreData.bin`;
- the stage fonts: `ref_fonts` 0x8004A6AC (`.fnt`, then `.txd` and `.met` via 0x8004A894);
- `setid.bin` and the set layouts (section 4);
- `stgNNNN_cam.dat` and `stgNNNN_light.bin`;
- the HUD CSD files (`.gncp`) and `common/mtarget[b|r].dff`;
- the sound banks `sound/B*.mlt` and `AudioDataStage*.bin` (AudioGCN vf4 / 0x801D8A20).

**Synchronous object archives**, through ResourceOneFileReq / ResourceOneFile (sections 4 and 5).

**Queued asynchronous reads.**
- `stgNNNN_dat.one`: fn_80177868 → async ONE open 0x8004CC94, callback 0x80177964.
- `stgNNNN.fog`: 0x8016A66C, callback 0x8016A4EC.
- They cannot start yet: the reads are paused (step 5 of section 1).

**Sound and title.**
- `sng_stgNNNN.adx`: ActStageController only builds the name (override table 0x8053DE60, existence check). CRI's
  stream server reads it later through the native gcCi device.
- `StageTitleNNNN_<lang>.txd`: 0x80209288 / StageTitleTask 0x80209158.

## 3. Asynchronous part: from paused reads to loaded blocks

### 3.1 Control: TStageInitalizeTask update 0x80177510 (240 passes, 1.320 → 2.240 s)

1. **Countdown.** A 30-pass countdown, then 0x80177540 clears the read pause (0x805EF1DC = 0). TBGReadManager::update
   0x800453E0 then spawns the queued reads (first TBGRead step at 1.450 s).
2. **dat.one.** Its member callback 0x80177964 runs at 1.633 s (section 3.2). It creates **TBlockManage** (ctor
   0x8003ADB4, published at 0x8051DFA8).
3. **Start blocks.** Waits until every landscape block around the start position is loaded (TBlockManage +0x979 set,
   +0x978 clear).
4. **Three more passes**, then:
   - StageManager +0x14 = 1;
   - the loading screen is closed;
   - 0x805F0CC8 = 1.
5. **InitAction vf0** 0x80205F80 sees 0x805F0CC8 and requests PlayAction (ctor 0x8020618C at 2.250 s). **This is the
   first gameplay frame.** NowLoading's update 0x802BED74 then removes the loading thread, re-enables task group 5
   (Render) and turns the frame pacer back on.

**Passes, not frames.** While the loading screen is up, NowLoading suspends group 5 (where RenderStart's
OSYieldThread 0x801D0338 lives) and sets 0x8057E774, so the frame pacer's busy-wait (0x801E20BC) is skipped. The main
loop runs unpaced, so the 30 and 3 are loop passes, not displayed frames.

**Correction (section 9f, measured): the main thread barely blocks during the loading screen.**
- GcnRasterShowRaster's OSSleepThread (0x80495D2C) ran 26 times in stg0402's 124 loading frames, against once per
  frame in gameplay.
- A loader gets the CPU mainly when CRI's two retrace threads (priorities 12 and 14) preempt the main thread at each
  retrace. SelectThread then puts the main thread at the tail of priority 16, behind the loader.
- **This explains the NativeJobs bridge failure** (`NATIVE_THREADS.md`, step 1 result). That bridge ran jobs only at
  the RenderStart yield, and during the loading screen that point is suspended.

### 3.2 `stgNNNN_dat.one` members (callback 0x80177964)

| Member | Path | Consumer |
|---|---|---|
| `PATH.PTP` | async PRS (TExpandBG), then 0x80177B30 → fn_8040F66C: relocatable header; byte +0x17 'B' means big-endian, else swapped | path manager static_80571C50 via fn_8001E20C (the consumer's role is unverified) |
| `STGNNNN.BDT` | sync extract (fn_8004C380 spins until done), then TBlockManage ctor 0x8003ADB4, then fn_8003AB90 | fn_8003AB90 swaps the little-endian 0x20-byte records {id, min, max, pad}, caps the count at 168, and loads `<BDT name>_texCM.one` **synchronously** (fn_80046BB4, `%s/%s_texCM.one`). Its TXD goes to TObjLand +0x1804. |
| `*.ADB` | sync extract, then fn_8004FEE0 | appended to the "texture change data" manager static_80571C00 (pair fields unverified) |

### 3.3 Landscape blocks `stgNNNN_NN.one` (TBlockManage, a 0x980-byte Task)

**Correction to `DRAW_DISTANCE.md`.** The block state lives on TBlockManage, not on TObjLand 0x80572560:
- **TBlockManage:** BDT +0x34, point +0x3C, the two mask generations, slot pointers +0x588, the loaded list +0x828.
- **TObjLand:** only the 32 block slots (0xC0 bytes each), texCM +0x1804 and a lock byte +0x1810.

**Per-frame update 0x8003AAE8** (2,425 calls on Westopolis):
- **Mask.** 0x8003A8D4 builds the 168-block mask from the point (+0x3C, else player 0) against the BDT boxes
  (min <= p < max, 0x8003A578).
- **Load and unload.** 0x8003A6DC keeps a block loaded while it is in either of the last two generations, so there is
  one change of hysteresis.
  - A wanted block with no loader gets a **LandALoadMananger** task: 0x800462EC → ctor 0x80046AF0.
  - An unwanted loaded block is released by 0x800473A8, which destroys its worlds and drops the TEXnn refcount.

**LandALoadMananger update 0x800466B4** (states in jump table 0x8051E4C4):
1. Async open of `%s/%s_%02d.one` (TBGRead).
2. A `TEX` member → async `%s/%s_TEX%02d.one` and its TXD, held in the refcounted table 0x805742E0 (ids 1–9). There
   are none on stg0100.
3. **Each `*.RG1` member**, matched against 15 layer patterns (table 0x8051E328), goes through
   `TOneFileAsync<RpWorld>` (0x8004B318 / task 0x8004B3A0):
   - a memory stream (0x804823B8), RwStreamFindChunk 0xB (0x8047DFA0), RpWorldStreamRead (0x80456744), with the
     TEXnn or texCM dictionary current;
   - callback 0x80046438 stores the world in slot +4 + 12·layer and registers it with the ADB manager (fn_8004FBD0).
4. **The `*_COLI_NN.BSP` member** goes through the same RpWorld loader. Callback 0x80046418 stores it at slot +0xB8
   and sets the slot to state 5 (loaded).

**Collision has no registration call.** Queries find TBlockManage through its handle, walk the +0x828 list and read
slot +0xB8 (fn_80021E24 / fn_80021FE4). The native world-collision owner already mirrors the sectors when RW's
collision plugin reads them (`native_world_collision.h`).

**Drawing.** The land draw 0x800471BC (via LandDispManager update 0x8004620C) draws only the blocks in the current
mask. A block from the previous generation can still be collided with, but is not drawn.

**Traced on Westopolis** (Shadow standing at the start):
- blocks 01, 02, 19 and 22 load;
- RG1 callbacks 10× (first at 2.195 s), COLI callbacks 3× (2.230 s);
- the player ctor 0x8007A038 runs at 2.233 s, the first gadget at 2.245 s.

## 4. Set objects (`sky2` layouts) and the object-type table

**`ref_setid_bin` 0x800CBC54, synchronous** (fopen / fread via 0x800450C4, no PRS):
1. `setid.bin` (0x800CB044), then the used types are marked (0x800CA7F0).
2. `stgNNNN_cmn.dat`.
3. `_hrd.dat` if byte 0x8057E760 +0x125 is set, else `_nrm.dat`.
4. `_ds1.dat`, always.
5. 0x800CB0EC forces each entry's layer bits: exactly one of 0x2000 / 0x4000 / 0x8000.

2P does not change which files load. It only switches the per-frame update to two cameras.

**Parser `ref_ffffffiisccii` 0x800CB1A4:**
- the magic `sky2`; header: +4 count, +8 misc-block size, records from +0xC;
- the disc files are little-endian and are byte-swapped by the format string `ffffffiisccii` (fn_80043C80);
- at most 0x800 entries per stage across all files.

**The 0x2C-byte record:**

| Offset | Field |
|---|---|
| 0x00 | position, 3 floats |
| 0x0C | rotation, 3 floats |
| 0x18 | live flags |
| 0x1C | saved flags |
| 0x20 | u16 class id |
| 0x22 | u8 link group |
| 0x23 | u8 range (×100 units) |
| 0x24 | misc length |
| 0x28 | misc offset (turned into a pointer) |

**The object-type table `lbl_8052C1A0`:** 286 descriptors plus a NULL terminator (`test_data/stage_loading/object_types.txt`).
- Each descriptor is 0x24 bytes: name, stageLoad, stageRelease, create (+0xC), u16 id (+0x18), kind (+0x1A), the used
  bit (+0x1B), the parameter description (+0x20).
- Marker entries bracket the batches. Each batch's End marker queues one archive:

| Batch | Entries | Archive queued |
|---|---|---|
| GadgetCommon | 0–56 | `stg_cmn_gdt.one` |
| Weapon | 57–88 | `common/WeaponResource.one` |
| GadgetStage | 89–234 | `stgNNNN_gdt.one`, then `stgNNNN_fx.one` (GadgetStageDataEnd 0x80169248) |
| EnemyEntry | 235–262 | `enemy/EnemyPath.one`, plus each enemy's `enemy/*Data*.one` |
| Vehicle | 263–273 | `common/vehicleResource.one` |
| NPC | 274–285 | `character/*.one` |

**Two passes:**
- **Preload, at init.** 0x800CA780 calls stageLoad for every type whose setid.bin stage bit is set, whether or not the
  layout uses it. stageLoad only appends a request (fn_8006B59C by name, fn_8006B634 by member index) to the list
  0x80576FA4. The End markers turn the list into a ResourceOneFileReq queued in ResourceManagerTask 0x8056FF94
  (fn_8001299C; keys 0 common gdt, 1 stage gdt, 2 weapons, 3 vehicles, 6 EnemyPath).
- **No create pass at init.** SetManagerTask update 0x800CB794 scans the 0x800 slots every frame, gated on the
  resource manager having nothing pending (+0x10 == 0). The layout manager (0x800CAC08 1P / 0x800CAA2C 2P, spawn test
  0x800CA8D8) creates an object when the camera comes within range (`DRAW_DISTANCE.md`). The range leaves are already
  native owners.

**Traced.** The first spawn (Gadget ctor 0x8015AE00) runs at 2.245 s, after the start blocks.

## 5. The I/O layers (who reads what)

| Layer | Functions | Sync / async | Already native below it |
|---|---|---|---|
| Whole-file read | 0x800450C4 (file), 0x80045014 (stream open) | sync | RtGCNFS owner (`native_rtgcnfs.cpp`; yields where the original did) and the file API owners |
| ONE archive object (0x58 bytes) | sync open 0x8004CE1C (+0x8004CD38 / 0x8004CFD8), async open 0x8004CC94 (completion 0x8004CC28), lookup 0x8004C938 / 0x8004CA50, extract 0x8004C770 | both | PRS expand 0x80043FE0 (native owner + decode-ahead workers); memcpy (owner) |
| TBGRead queue | three FIFO lists 0x805742A0, TBGReadManager::update 0x800453E0, worker step 0x80045680, at most one read in flight (0x805EF1D8), pause flag 0x805EF1DC | async (thread) | none (NativeJobs bridge is off: `NATIVE_THREADS.md`) |
| TExpandBG | worker step 0x80043D5C (PRS of one member) | async (thread) | PRS itself |
| TOneFileAsync<T> (manager 0x80574C2C, 16 jobs) | RpDMorphAnimation 0x8004ADD8 / AE60, RpWorld 0x8004B318 / B3A0, RtDict 0x8004B734 / B7BC, RpClump 0x8004BB4C / BBD4, RwTexDictionary 0x8004BF68 / BFF0, raw 0x8004C380 / C408 | a Task with a callback; spins on the main thread without one | none |
| ResourceOneFile (vf1 0x800149F4) / ResourceOneFileReq (.BIN 0x8006A23C; other members ref_XNCP 0x8006A3C0), ResourceManagerTask 0x8056FF94 | sync at stage start (every stage-start caller passes async = 0) | none |

PRS covers every ONE member; the loose files are raw.

## 6. Every file of a stage load, with its parser

| File | Opener → parser | Sync / async | Consumer |
|---|---|---|---|
| `stgNNNN_dat.one` | 0x80177868 → 0x80177964 | async | PTP: path manager; BDT: TBlockManage; ADB: texture change manager |
| `stgNNNN_texCM.one` | 0x8003AB90 → 0x80046BB4 (TXD) | sync | TObjLand +0x1804 |
| `stgNNNN_NN.one` (RG1 render worlds, COLI collision worlds) | 0x80046AF0 / 0x800466B4 → TOneFileAsync<RpWorld> → RpWorldStreamRead 0x80456744 | async | block slots; land draw; collision |
| `stgNNNN_TEXnn.one` | 0x800466B4 | async | table 0x805742E0 |
| `stgNNNN_cmn/_nrm or _hrd/_ds1.dat` | 0x800CBC54 → 0x800CB1A4 | sync | layout manager slots |
| `setid.bin` | 0x800CB044 | sync | used-type bits |
| `stg_cmn_gdt.one`, `stgNNNN_gdt.one`, `stgNNNN_fx.one`, `common/WeaponResource.one`, `common/vehicleResource.one`, `enemy/*.one`, `character/*.one` | stageLoad requests → ResourceOneFileReq / ResourceOneFile → 0x8004CE1C → 0x8006A23C / ref_XNCP | sync | object models (DFF, TXD, .BIN, motions, ...) |
| `stgNNNN_cam.dat` | 0x801C85AC → 0x801C8340 (whole file word-swapped; version 3, date 20041107, count +0x14, 0xDC records; stg0100: 70) | sync | StageCameraManager |
| `stgNNNN_light.bin` | 0x80048738 → 0x8004867C (16 × 0x34 groups, `xxxxxxxxxxxii` swap) | sync | light manager |
| `stgNNNN.fog` | 0x8016A66C → 0x8016A4EC (24 × 0x28 records, little-endian, partly swapped) | async | TFogSet update 0x8016A904 |
| `stgNNNN_hint.bin` | HINT_CONTROLLER stageLoad 0x802F33BC → fn_8040F66C | sync | hints (not read on stg0100: the hint type is not used there) |
| `fonts/stgNNNN/stgNNNN_<lang>.fnt/.txd/.met` | ref_fonts 0x8004A6AC / 0x8004A894 | sync | stage text |
| `sng_stgNNNN.adx` | name built by 0x801D3EA8; read by CRI's stream server | streamed | BGM (gcCi device is native) |
| `sound/B*.mlt`, `sound/AudioDataStage*.bin` | AudioGCN vf4; 0x801D8A20 | sync | GCAX banks (bank upload is a native owner) |
| `csdFiles/*.gncp`, `StageTitleNNNN_<lang>.txd`, `pause/pauseNNNN_<lang>.txd` | CSD loader 0x80363A88; 0x80209288 | sync | HUD, title card, pause menu |

## 7. The traced load (Westopolis, 2026-11-03)

**Run.**
- Exe: `.build/watch` (lifted renderer, the oracle runner).
- `--boot-stage stg0100 --headless`, neutral input, `--budget-ms 40000`.
- `GEKKO_CALL_CENSUS_COUNTS` on all 29,282 game-layer and RW-read functions; `--asset-trace`.
- 24 s wall, exit 0.

**Timeline** (first call, game time):

| Time | Event |
|---|---|
| 1.150 s | front end; NowLoading built |
| 1.267 s | Select Mode hand-off |
| 1.317 s | StageManager ctor and state 0 (all synchronous loads) |
| 1.320 s | init task and InitAction start polling |
| 1.450 s | first TBGRead (fog, dat.one) |
| 1.633 s | dat.one callback, TBlockManage, first LandALoadMananger |
| 2.195 s | first RG1 world |
| 2.230 s | first COLI world |
| 2.233 s | player ctor |
| 2.240 s | init task done |
| 2.245 s | first gadget spawned |
| 2.250 s | PlayAction (first gameplay frame) |

The window is ticks 51,300,000 – 91,125,000 (40.5 M ticks / s).

**Files.** 75 distinct files, 2,172 reads. In order: front-end and system files, then `character/shadow.one`,
CommonEffect, fonts, the stage title, ScoreData, the sound banks, the stage fonts, setid.bin and the layouts, cam,
light, the HUD gncp files, `stg_cmn_gdt.one`, `stg0100_gdt.one`, WeaponResource, vehicleResource, EnemyPath and the
enemy archives, LensFlare, `stg0100_fx.one`, sonic and doomseye. After the read pause: `stg0100.fog`, `stg0100_dat.one`,
`stg0100_texCM.one`, blocks 01, 02, 19, 22 and `sng_stg0100.adx`.

**Reproduce.**
- `tools/stage_loading/run_trace.sh .build/stage_loading/trace-westopolis stg0100` is the run above.
- Its outputs are kept in that folder (`counts.csv`, `assets.jsonl`).
- The fixed call graph is in `.build/stage_loading/callgraph_fixed.json`.

## 8. Census (`tools/dolrecon/loader_census.py`, outputs in `test_data/stage_loading/`)

```
cd tools
# the fixed call graph, written outside source/ (dolrecon.callgraph's main would overwrite pal-v30/callgraph.json)
python -B -c "import json,os;from dolrecon import callgraph;p='../../source/.build/dolrecon/pal-v30';json.dump(callgraph.build(p,None,os.path.join(os.path.dirname(os.path.abspath(p)),'skeleton')),open('../.build/stage_loading/callgraph_fixed.json','w'),indent=0)"
python -B -m dolrecon.loader_census ../../source/.build/dolrecon ../.build/stage_loading/callgraph_fixed.json \
    ../test_data/stage_loading/loader_seeds.csv ../test_data/stage_loading --native .. \
    --pruned ../.build/watch/generated/recomp_pruned.csv \
    --dynamic ../.build/stage_loading/trace-westopolis/counts.csv --window 51300000,91125000
```

**Membership.**
- **Seeds:** 550 entry points (`loader_seeds.csv`, each with its evidence):
  - the RE seeds;
  - 24 vtable-dispatched functions the trace found;
  - every stageLoad / stageRelease of the object-type table.
- **Closure:** call / tail / addr edges, through game_map's loader subsystems and unplaced code.
- **Not walked:** mission, result and stage-action bodies (their loader functions are seeds), and code owned by any
  other subsystem.

**Results.**

| Category | Functions | Bytes |
|---|---|---|
| OBJECT_RESOURCES (stageLoad / stageRelease and helpers) | 521 | 21,684 |
| ASYNC_IO | 64 | 18,140 |
| SHARED_LOADER | 66 | 4,308 |
| WORLD_BLOCKS | 22 | 4,864 |
| STAGE_FLOW | 16 | 4,332 |
| ENVIRONMENT | 13 | 3,180 |
| SET_OBJECTS | 11 | 2,644 |
| LOADING_SCREEN | 13 | 2,452 |
| WORLD_DATA | 7 | 1,960 |
| AUDIO_TEXT | 4 | 832 |
| **members** | **737** | **64,396** |

- **Native status of the members:** 726 TRANSLATED. The other 11 are RUNTIME_REFERENCED, and those references are
  only `--asset-trace` labels, frame-limiter clock notes and the lifted renderer's land-layer table; none of them
  replaces a function.
- **Interface:** 424 functions (RW 49, MSL 18, Boost 22, other game subsystems 330). Among them are native owners
  (PRS, memcpy, the object range, link groups, the disc-error hold).
- **Dynamic:**
  - 311 members ran; 205 ran first inside the load window;
  - 426 did not run, mostly the stageLoad / stageRelease of types not on Westopolis;
  - 3,245 other game functions first ran in the window: object constructors, the player, the HUD and the camera, all
    built during the load but not loader code.
- **`loader_scope_ranges.txt`:** the members as a `--verify-player` scope file (`address,size`).

## 9. Gaps (what remains GameCube-shaped, and what is still unknown)

**GameCube-shaped:**
- the TBGRead / TExpandBG threads and the scheduler control around them;
- the read pause as a global byte;
- the loading screen presenting through GcnRasterShowRaster's sleep;
- all parsing and swapping (`sky2`, cam, light, fog, BDT, PTP): byte-swapping of little-endian disc data into guest
  structures;
- RpWorld creation from streams on the guest heap;
- ONE archive handling: translated, except PRS.

**Unknown or unverified.** Most of the original list is closed in 9a. What remains open:
- fog record selection;
- the player archive's index argument;
- Manager vf4;
- the exact parent of RenderStart in group 5;
- the PTP point flags and the BlockChange parameter +0x38;
- whether a sequence-level exit (codes 2 and 6) builds a new StageManager.

**Scope of the evidence:** sections 9b and 9c add more traced loads and every bootable stage's cold load. 2P, hard
mode and story chains are still untraced.

## 9a. Unknowns closed (static, 2026-11-03; notes `re_D_open_items.md`)

**PATH.PTP is the stage's path network.**
- **Load:** 0x80177B30 → fn_8040F66C (Sonic Team relocatable: data at +0x20, POF0 relocations) → fn_8001E20C, into
  the network singleton static_80571C50.
- **Classes:** `SonicteamUSA::System::PathData` (one per path), and `PathCtrl` / `Player::PathImpl` cursors on a path.
- **Path record (0x30):** point count, total length, points, type mask +0xC, bbox max +0x10, id +0x1C, bbox min +0x20,
  name.
- **Point (0x20):** position, rotation in radians, length to the next point, flags (no reader found).
- **stg0100:** 3 paths (`jn_cc_010` / `jn_cc_011`, type 0x2001, chained; `lp_001`, type 0x100). The point lengths sum
  to each path's length.
- **Users:**
  - the nearest-point search fn_8001CDA0: the player controller every frame, Chaos Control (filter 0x6000), vehicles;
  - lookup by id fn_8001C7B4: moving platforms, Coaster, EscapePlane, EggBalloon, the BK tank, EggLastMech;
  - the cursor advance fn_8001C91C: those objects and the enemy MoveOnPath AI states.
- **Not in the file:** grind rails and enemy flight paths add their own paths at run time (fn_8001DB08).

**ADB is a flip-book texture animation.**
- **Layout:** +0 period in frames, +4 the material's texture name (`s0100_nn066.1`), +0x24 base name, +0x48 key count,
  +0x4C keys {start frame, texture number}.
- **Setup:** an RG1 world's callback matches materials and creates a TTextureChange task. Frame textures `%s.%d` come
  from the current TXD (stg0100's texCM holds .1 to .16).
- **Per frame:** the clock advances 60·dt and wraps at the period; a key change sets the material's texture.

**fn_80038148:** on each loaded RG1 world, every material with MatFX environment mapping gets its coefficient × 0.45,
clamped to 0–1. fn_80038184 does the same for clumps.

**TBlockManage fields:**

| Field | Written by | Meaning |
|---|---|---|
| +0x2E8 | only the BlockChange set object (type 0x59: mode, radius, 4 block ids) | forced block state per block (1 in, 2 out) |
| +0x38 | TStageInitalizeTask and WarpHole | dirty flag |
| +0x3C | the init task (start or checkpoint position), set back to null once loaded | the point blocks are chosen from |
| +0x40 | WarpHole and event ends (set to 2) | hold counter |

Goal, triggers, events and WarpHole wait for +0x40 == 0 and +0x978 == 0 (blocks settled).

**TEXnn blocks.**
- **Rule:** a block member containing `_TEX` (case-sensitive) followed by two decimal digits opens `%s/%s_TEX%02d.one`.
- **Latent original bug:** that sprintf passes the stage name and the id, so the second `%s` receives an integer.
- **On disc:** only stg9900 (not bootable) has such members. `stg0402_tex02.one` is never loaded, and stg0402's trace
  confirms it.

**Restart, death and continue.**
- **Death:** death → DeadAction → ContinueAction. With lives left, the players reset to the checkpoint and a new
  TStageInitalizeTask (flags 2) plus an InitAction run.
- **Restart** (pause, try again, time up): the same, from the start.
- **StageManager is not rebuilt.** Only the block archives the new position needs and that are not loaded are read.
- **State 3** (teardown) is entered from EndAction's destructor 0x80178B2C: quit, goal, try-again exits.
- **Traced:** the mash route on Westopolis dies at 35.05 s. DeadAction 35.05 s, ContinueAction 36.57 s, InitAction
  36.72 s (4 passes), PlayAction 36.78 s. **No file was read**, and the StageManager and player were not rebuilt.

## 9b. More traced loads (2026-11-03, `.build/stage_loading/trace-*`, `tools/stage_loading/trace_report.py`)

| Trace | Load window | Blocks | Beyond the cold load |
|---|---|---|---|
| Westopolis 90 s tape (`westopolis-cpu-spike`) | 0.983 s | 7 (01, 02, 19, 22 at start; 03, 04, 05 streamed at 28–76 s) | 2 EventActions (Sonic / Doom's Eye); the player rebuilt 3×, PlayAction 3×; files beyond the cold load: the two Mchara gncp and blocks 03–05 |
| Final Haunt tape | 1.383 s | 8 (01, 02, 60, 90 at start; 03, 61, 04, 05 streamed) | 1 EventAction; enemy archives BkGiant / BkLarva / BkNinja / BkWingLarge |
| Westopolis death (mash route) | 0.983 s | 4 | the continue above: no files |
| stg0402 (neutral) | 2.067 s | 5 (01, 02, 03, 60, 90) | no `_tex02.one` read |

- **Streaming in play:** the block archives are the only files read during play, in these traces.
- **Coverage:** across the five traces, 341 of the 737 census members ran. Of those that never ran, 381 are object-type
  stageLoad / stageRelease functions for types the traced stages do not use. The others are the 2P layout update,
  the RpDMorphAnimation loader, static destructors and 8 small helpers.
- **Not traced:** hard mode and 2P. Neither is reachable without a new boot option, which would be a code change.
  - **Hard mode:** the static path selects `_hrd.dat` instead of `_nrm.dat` (byte 0x8057E760 +0x125).
  - **2P:** the static path only switches the per-frame layout update to two cameras.

## 9c. Load times of every bootable stage, and why some are long (2026-11-03)

**Method** (`tools/stage_loading/measure_loads.sh` + `load_times.py`):
- Each of the 38 bootable stages cold, `--boot-stage`, windowed, neutral input.
- The per-call census on 18 sequence markers only, so wall time stays representative.
- `--perf-log` places each game tick in wall time.
- Results: `test_data/stage_loading/load_times_baseline.csv` (exe `.build/watch`, unchanged source).

**Result: from 0.65 s (bosses) to 2.08 s.**
- **Longest:** stg0402 2.08, stg0302 1.95, stg0201 1.90, stg0404 1.87, stg0400 1.79, stg0600 1.70, stg0503 1.65 s.
- **Wall time = game time** to within 2 %, and the largest gap between two presentations in a load is about 25 ms.
  The host never stalls; the loading screen's length is the game's own scheduling.

**Where the time goes** (every stage):
- **0.06 s:** state 0. Every synchronous read and archive load (18 files, 11–16 ONE archives).
- **About 0.19 s:** the init task's 30-pass countdown before the read pause lifts. This is game logic.
- **Everything else:** the background chain. dat.one, then each start block's archive, strictly one read at a time,
  then its worlds.

**Cause: each yield of a background read costs one presented frame.**
- **The read loop:** a TBGRead step reads its file in 0x2000-byte RwFread calls.
- **The yields:** every RwFread is an RtGCNFS read, which yields twice, like the original: OSYieldThread at the chunk
  loop head, and again after the transfer (the original waited on the drive there).
- **What a yield costs** (mechanism corrected in section 9f):
  - while the loading screen is up, the main thread runs unpaced and almost never waits;
  - a worker's yield puts it behind the main thread at priority 16;
  - it runs again only after the next retrace, when CRI's retrace threads (VIWaitForRetrace 0x8037ECD4, its sleep at
    0x8037ECFC) preempt the main thread and requeue it behind the worker;
  - so each yield costs one retrace, about 15.6 ms, and the trace shows one RtGCNFS yield per frame during the load;
  - **measured on stg0402** (`.build/stage_loading/chain-stg0402`): its 98–104 KB block archives took 31 yields and
    0.483 s each. Five blocks were read one after another, 1.3 s of the 2.07 s;
  - the stages with more or larger start blocks are the long ones.
- **On the GameCube** the drive really took that long.
- **Here** the transfer is a copy from a RAM-resident file (`NativeFileApi::device_read`, 0.37 ms worst case,
  `NATIVE_THREADS.md`). The yields only wait for frames.

**Proposed fix (an experiment, not yet built).** In the native RtGCNFS read owner (`native_rtgcnfs.cpp`), stop
yielding around a transfer that the host already completed. The chunk loop and the transfer's completion run on at
once, so a TBGRead step reads its whole file in one worker slice.
- **What it changes:** only when a background read completes. The bytes, the order of reads and every callback stay
  the same.
- **Expected:** stg0402's five start-block reads drop from about 1.3 s to a few frames each. The remaining floor is
  about 0.3–0.5 s: the countdown, one read in flight reaped once per frame, and the TExpandBG / TOneFileAsync Tasks.
- **It deliberately changes load timing:** PlayAction comes earlier.
- **Verification it needs:**
  - the 38-stage sweep again (load times before and after);
  - targeted strict `--verify-player-strict` over `loader_scope_ranges.txt` on several stages (0 mismatches);
  - an A/B at the first gameplay frame: same files, same block set, same set-object slots, same player state. Fields
    that count loading passes or game time are expected to differ, and are listed and shown to be gameplay-neutral;
  - Westopolis and Final Haunt tape replays reaching the same states;
  - a savestate taken during a load.

## 9d. The 0x2000-byte chunk method, and its native replacement (2026-11-03; user request: "identical behavior but faster")

**The original, end to end.**

**TBGRead's step 0x80045680.** On the worker thread, after the load-hold query, it calls
`0x80045150(manager, file = this+0x348, &destination = this+0x334, -, chunk = 0x2000)`. 0x800456C4 is the only caller.

**The read loop 0x80045150:**
- with no file, it returns 0;
- with no destination, it calls `fn_80049BB8(length, 2)`, the RenderWare allocator;
- if `chunk == 0`, it makes one `RwFread(destination, length, 1, file)`;
- otherwise, while `remaining != 0`: if `(s32)remaining > (s32)chunk`, it calls `RwFread(destination, chunk, 1, file)`
  and adds `chunk × result`; else it calls `RwFread(destination, remaining, 1, file)` and adds
  `round_up_32(remaining) × result`;
- it returns the sum.

**`RwFread` 0x8047B430:** the file system's read slot (fs +0x30, RtGCNFS read 0x8047BC60) with `size × count` bytes,
then `divwu` by the size.

**RtGCNFS read** (`RTGCNFS_SPEC.md`), per call:
- **Entry:** initialPos = position, and the count is clamped to the file length.
- **Loop**, while remaining > 0 and retries < 1:
  - it yields first (OSYieldThread 0x8047BE54);
  - an aligned destination and position with at least 32 bytes left → a direct transfer of `remaining & ~31`;
  - a valid cache → a memmove from the cache;
  - otherwise a cache fill: one transfer of up to 0x800 aligned bytes.
- **Each transfer:** a DVD read whose completion is polled, which is the original's second yield, the drive wait;
  then the drive-error callback (fs +0x5C, state 7 = no error), then status READY, then position, remaining and the
  cache fields are updated.
- **Exit:** status READY; it returns `n - remaining`.
- **Cost:** a ~100 KB block archive is 13 RwFread calls, so about 26 yields and 26 main-thread passes. During the
  loading screen each pass ends at the retrace sleep: one frame each (section 9c).

**The native replacement** (`runtime/src/gekko/native_rtgcnfs.cpp`, owner of 0x80045150, `GEKKO_NATIVE_BG_READ=1`):
- **What it does:** the loop and each read's state machine in host code, with the same signed compares, `divwu`,
  round-up, clamps, direct / fill / serve decisions, cache bookkeeping and status writes.
- **Transfers** copy straight from the mapped file (`NativeFileApi::device_read`).
- **Guest calls kept, in order:** the allocator, every drive-error callback and the memmove of a cache serve.
- **Removed:** the 2 yields per chunk and the 13 × (RwFread → vtable → owner entry / continuation) round trips.
- **A file system other than RtGCNFS** goes through its own read slot as guest code, as before.
- **Off by default.** The source is shared with other trees, so default-off keeps their builds byte-identical until
  the user decides.

**Evidence** (Release tree `.build/loadfix`, built from this working tree; not deployed):
- **Oracle CTest `native_bg_read_tests`.** The owner runs against the original words of 0x80045150, 0x8047B430 and
  RtGCNFS in the reference interpreter.
  - **Inputs:** 20 file sizes around the chunk and the 32-byte alignment (1 B to 110,752 B); aligned, unaligned and
    allocated destinations; chunk sizes 0, 0x100, 0x800, 0x1000, 0x3000 and 0x7FFFFFFF; a nonzero start position; a
    cache left valid by an earlier read.
  - **Compared after each call:** the result, the non-volatile registers, LR / PC, the ordered guest calls, and every
    byte of the file system, the drive state, the records, caches and destinations.
  - **Result:** 207 calls, 69 chunked reads, 2.1 MB, 741 chunks, **0 failures**. The original yielded 2,342 times, the
    owner 0.
  - **Negative controls**, all caught: a wrong round-up, a missing drive-error callback, a wrong cache offset after a
    fill. `native_rtgcnfs_tests` still passes.
- **Same work in game, owner off vs on** (stg0402, stg0604, stg0100): equal RtGCNFS opens, reads, transfers and bytes
  (e.g. stg0402: 99 / 432 / 2,039 / 9,980,736 both ways). Only the yields go: 6,337 → 6,297.
- **Strict oracle over the loader scope** (`test_data/stage_loading/loader_scope_ranges.txt`; Westopolis tape, 30 s,
  owner on): 10,399,498 call segments matched, **0 mismatched**, 158 M instructions replayed.
- **Semantic A/B at the first gameplay frame** (`tools/stage_loading/ab_first_frame.sh` + `ab_semantics.py`): a cold
  load with the owner off and on, states saved at PlayAction + 5..21 frames, paired by frames after each run's own
  PlayAction, and the comparator's semantic readout compared.
  - **Westopolis:** 16 pairs. The player's position, velocity and heading, the camera, the 31 live entities by class,
    the 1,111 set records, the mission, rings, score and **the RNG state are identical**.
  - **What differs:** only the frame counter since boot (247 vs 73 at +6 frames) and the game tick.
  - **Raw bytes:** 30 KB differ out of 18.8 MB of image and heap, in timers and audio buffers. They count time since
    boot, which the shorter load changes.
  - The all-stage run is in section 9e.
- **Timing changes, intentionally:** the loading screen ends earlier (table in 9e).
  - **Consequence for tapes:** tick-indexed input tapes recorded before this change replay shifted by the saved time,
    because their inputs are tied to game ticks since boot. Comparisons must align on PlayAction, as above.

## 9e. Original vs native read, stage by stage (2026-11-03)

**Exe:** `.build/loadfix` (this working tree), the same exe both ways, `GEKKO_NATIVE_BG_READ=0` (original read path)
vs `=1`.
- **Load times:** windowed, the user's display config copied, audio on.
- **State check:** headless; states at PlayAction + 5..10 quanta, paired **by the game's frame counter** with a constant
  frame offset D. Pairing by ticks is wrong where the game misses 60 Hz quanta right after PlayAction (stg0202,
  stg0310, ...).
- **Tools:** `watch_pair.sh`, `ab_first_frame.sh`, `ab_semantics.py`.

| Stage | Loading screen original | native | Saved | State at first gameplay frames |
|---|---|---|---|---|
| stg0100 Westopolis | 1.00 s | 0.32 s | 0.68 s | identical (5 pairs, D 174) |
| stg0402 | 2.08 s | 0.44 s | 1.64 s | identical (5 pairs, D 424) |
| stg0302 | 1.95 s | 0.40 s | 1.55 s | identical except the player's heap address (3 pairs) |
| stg0201 | 1.89 s | 0.44 s | 1.45 s | identical except the player's heap address |
| stg0404 | 1.87 s | 0.33 s | 1.54 s | identical except the player's heap address |
| stg0400 | 1.78 s | 0.50 s | 1.28 s | identical except the player's heap address |
| stg0600 | 1.70 s | 0.37 s | 1.33 s | identical except the player's heap address |
| stg0503 | 1.64 s | 0.40 s | 1.24 s | identical except the player's heap address |
| stg0700 The Last Way (story entry) | 1.52 s | 0.42 s | 1.10 s | not reached: see below |
| stg0710 Devil Doom (story entry) | 0.54 s | 0.34 s | 0.20 s | not reached: see below |

The other Select Mode stages: bosses 0.75–0.95 s → about 0.31 s; the rest 0.8–1.4 s → 0.30–0.50 s
(`.build/stage_loading/watch2`).

**Correction: the story-entry stages do not reach gameplay (measured, call census).**
- **What happens:** `--boot-story-stage` builds the requested stage's StageManager (stg0700: ctor 0x80178C2C with
  r4 = 0x2BC) and the load completes (its init task ends at 2.78 s). But no PlayAction follows. About 6.6 s later the
  story builds StageManager(100), Westopolis, and plays that.
- **Affected:** stg0700, stg0710, the practice / EVENT stages 0001–0005 and 0901–0911.
- **What the measurements mean for them:**
  - their loading-screen times are their own loads;
  - their "first gameplay" times and state comparisons are Westopolis's, after the requested stage was loaded and
    left. They only show that loading the stage first perturbs nothing.
- **Not affected:** the 2P stages stg0800–0802, which do reach PlayAction. They render a dark world with a T-posed
  player, the same with the original read and the native read (captures `.build/stage_loading/cap2p`), because the
  1P story path skips the 2P sequence's setup.
- **Where they should come from:** a proper entry runs through the game's own phases. The phase table 0x805592A8 is
  indexed by kind + 1: 0 PlayOnce, 1 PlayStory, 2 PlayEvent, 3 Play2POnce, 4 Play2PThree, 5 MovieTest, 6 PlayHard,
  7 PlayFlashback (P).
- **Being reverse engineered:** the Last Story entry, and 2P (section 9g).

**The headless state check: all 38 Select Mode stages.** It also covers 17 story-entry runs, which compare
Westopolis after the requested stage was loaded (see the correction above). The pairing takes, among the frame
offsets D with at least 3 pairs, the D with the fewest differences. For EVENT0901 / 0908 the first choice (more
pairs) put the states one frame apart.
- **Every stage:** the camera, the player's position, velocity and heading, the live entities by class and layer, the
  set records, the mission, rings, score and **the RNG state** are identical at equal game frames.
- **The frame offset D is constant per stage:** the game frames the faster load saved.
- **The only difference: the player object's heap address**, on 20 of 34 stages.
  - **Cause:** the background read's completion objects (the 0x48-byte TFileControlAsycManager Task, the worker and its
    0x1000-byte stack) are created and freed at different points relative to the main thread's allocations. The same
    objects then land at different heap addresses.
  - **Why it comes with the speed-up:** the original heap order needs the original completion timing.
  - **A finding, not yet cleared:** nothing in the readout depends on it, but a longer gameplay A/B (tapes aligned on
    PlayAction) should show that nothing in the game orders by address.

**Correction (W0b, `NATIVE_WORKERS.md` 5a).** On 9 of 40 stages (stg0201, 0300, 0302, 0400, 0404, 0501, 0503,
0604, 0615) the native read changes the starting parity of an uninitialised counter, ShadowMotion +0x3EC (an original
game bug: PlayerMotion's ctor skips it). That counter toggles the player model's atomics on each bit-0x18 event, so
there the toggle runs in the opposite phase from the original. The first-frames check above could not see it.
Resolved by the user's erratum **E2** (`GAME_ERRATA.md`): the counter is zeroed at construction, so both reads start
it at 0.

## 9f. The worker and thread system (2026-11-03; notes `re_E_threads.md`, P = proven, I = inferred, U = unknown)

**Threads.** OSCreateThread 0x80378A44 has exactly 5 call sites (P):
- **The Thread base ctor 0x8035A1F4:** TBGRead and TExpandBG workers; priority 16, a 0x1000-byte heap stack, detached,
  resumed at once; entry 0x8035A01C; one step each, then exit.
- **ADXGC init 0x803C5118 (0x803C5218 / 48 / 78 / A8):** CRI's four threads (`CRI_SERVERS.md`).

**The main thread:** priority 16 (P: __OSThreadInit 0x80378334). There are no message queues and no OSLockMutex in
the game (P).

**Scheduling.** Equal priority, no time slicing. A worker runs when the main thread:
- **yields:** RenderStart 0x801D0338, once per gameplay frame; suspended with group 5 during the loading screen;
- **sleeps:** the ShowRaster sleep 0x80495D2C;
- **is preempted at a retrace** by CRI's priority 12 / 14 threads, which wait in VIWaitForRetrace 0x8037ECD4.
  SelectThread then requeues the main thread at the tail of priority 16 (P: 0x8037882C / 0x8037886C).

**Wakers:**
- the retrace queue: `native_video.cpp:380`;
- draw-done: `native_gx_sync.cpp:442`;
- ShowRaster: the GX breakpoint callback fn_80494A2C and the VI post-retrace callback fn_80494BE8.

The loaders' load-hold sleep never happens: the native owner of 0x8032D138 returns 0.

**Lifecycle** (P):
- **Spawn:** TBGReadManager::update 0x800453E0 spawns at most one read per update, and only while the in-flight count
  0x805EF1D8 is 0 and the pause byte 0x805EF1DC is 0.
  - The pause byte is written only by fn_80045320: the StageManager ctor sets 1, the init task sets 0.
  - The file open and the destination allocation happen on the main thread.
- **In-flight count:** +1 in the TBGRead ctor, −1 in its dtor.
- **Completion:** the worker builds its completion Task (TFileControlAsycManager 0x8004580C, TExpandNotice 0x80043EF8),
  sets done = 1 and exits.
- **Reap:** the reaper 0x8035A2DC (from 0x801E231C, once per frame, before the task-tree walk) deletes finished
  workers across 128 slots.

**Costs** (stg0402 trace, original path):
| Step | Cost |
|---|---|
| Spawn to the worker's first slice | at most 1 frame |
| Reading | 2 frames per 8 KB chunk |
| Completion Task | the same or the next frame |
| Reap to the next spawn | about 0.2–1.2 frames |
| TExpandBG to its notice | under 1 frame |

**The runtime today:**
- **One host thread runs every guest thread.** Switches are the translated SelectThread / OSLoadContext, entered from
  `NativeInterrupts::reschedule`, and the SDK idle loop goes to `System::advance_to_next_event`.
- **Already native:** PRS (with host decode-ahead threads), file transfers, the RtGCNFS / DVD owners, the disc-error
  owners, CRI's SVM / SJ / ADX output, and since 9d the inline background read (`GEKKO_NATIVE_BG_READ=1`).
- **Still translated:** os.threads, the Thread base class, the worker steps, the managers, the completion Tasks and
  CRI's thread loops.

### Proposed native design (not implemented; needs the user's go-ahead). The full design: `docs/NATIVE_WORKERS.md`

The completion Tasks' parents are resolved (P):
- TFileControlAsycManager (built by TBGRead's finish 0x80045890) goes under the read manager's task `*0x805EF168`
  (fn_80037200);
- TExpandNotice goes under group 0 of the task root (static_80571C6C [0]);
- TOneFileNotice goes under its caller's parent.

1. **Keep the game logic:** the producers, the heap allocations, the counter, the busy entries, the game's own Task
   ctors and the reaper.
2. **A host worker pool** (1–2 threads, CPU-heat rule) does the bytes: the file read (already a copy), the PRS
   decode, and later the RpWorld / TXD parse into staging buffers. Workers never touch guest memory.
3. **Deterministic publication.** A job submitted in main-loop iteration k is published at the start of iteration
   k+1, before the reaper. Publishing means: copy the bytes, write the sizes, build the completion Task through the
   game's own ctor, set done.
   - If the host job is not finished there, the game thread waits for it, and that wait is measured.
   - This replaces the yield / preemption timing, which depends on retraces, with a game-event rule, so it is
     deterministic.
4. **Savestates:** join the pool and save finished-but-unpublished jobs. States never carry host work in flight.
5. **What leaves the build:** the worker entry 0x8035A01C, the two steps, and the workers' OSCreateThread / Resume /
   Exit / Yield calls. SelectThread, OSLoadContext and the idle loop stay while CRI's threads exist
   (`CRI_SERVERS.md`).

**To measure or prove before implementing:**
1. A gameplay block-streaming trace (no read after PlayAction has been traced at chunk level yet).
2. A guest watch on the worker objects, their OSThread bytes and freed stacks, for the stack-garbage decision
   (`CRI_SERVERS.md` 5.3, erratum E1).
3. That no other priority-16 thread exists, plus a CRI PCM A/B.
4. The task-tree position of the completion Tasks (their parent group is U).
5. A savestate taken mid-load, resumed and compared with an uninterrupted run.
6. The stall budget at the publication point.

## 9g. 2P mode, Last Story and Expert: how the game enters them (2026-11-03; notes `re_F_2p.md`; P / I / U)

G = 0x8057E760, S = 0x80585788.

**2P is decided by the stage, not by the sequence (P).**
- The StageManager ctor calls 0x80203BC4, which spreads the stage record's flag word (+8) into G+0xB1..B4.
- G+0xB2 is flag bit 0x4. Only stg0800 / 0801 / 0802 carry it (flags 0x6).
- Every 2P difference reads G+0xB2 or bit 0x4:
  - two players: 0x801773CC;
  - two cameras: 0x801CEC9C;
  - the 2P layout update: 0x800CB9CC → 0x800CAA2C;
  - the 2P HUD;
  - InitAction choosing ShowAction (2P_Start) instead of PlayAction: 0x80205FA8.
- No 2P-specific light, fog or motion file was found (I).

**The 2P front end (P):**
- Character select writes P1 = G+0x13C (0x8057E89C) and P2 = G+0x140 (0x8057E8A0), each 14 + cursor (0..5). Both
  players are PlayerShadow.
- One arena (0x802C1514): kind 3, S+0xC = 800 / 801 / 802.
- Three arenas (0x802C148C): kind 4, S+0x10..0x18 = 800, 801, 802.
- Then the play-record reset fn_80206680(0x8057E82C), GameSystem = 5, the play flags 0x8057E8A8..AA, and the same
  RequestPhase(…, 0) hand-off as Select Mode.

**The phases (P):**
- Play2POnce builds StageManager(S+0xC, 0) and the round state: G+0x12C = 1 round, G+0x130..0x144.
- Play2PThree plays the three arenas in random order (G+0x12C = 3).

**Why the story-entry tool showed a dark world and a T-posed player** (cause P, visuals I):
- PlayStory builds a story-entry stage as **StageManager(stage, 1)** (0x802D221C). Its gameplay load is
  StageManager(stage, 0) (0x802D2148).
- With mode 1, fn_80177268 parks player 1 at (−100000, −100000, −100000) and never places player 2.
- The 2P InitAction then skips the event that would move them. The cameras follow a player outside the world, and the
  visible figure is the unplaced player 2.

**Why story-entry 0700 / 0710 / 09xx left their stage (P chain).**
- InitAction requests PlayAction, then StageTriggerEventTask (0x8025DFB4) pops the story queue.
- The popped entry, the new game's first event, overwrites the request with EventAction.
- That event plays (about 6.6 s), then the story continues to stg0100.

**Last Story (P).** The story menu's Last Story is PlayStory with start mode **2** (0x8058578C; menu 0x802BB878).
- The PlayStory ctor (0x802D2618) sets the current stage to 700 and G+0xD4..0x120 = 1.
- It then plays its own queue: the Last Story event scenes, then StageManager(700, 0).
- Devil Doom (stg0710) follows 700 through the story (I). No direct entry exists (U).

**Expert mode is PlayHard (kind 6) (P).**
- S+0x20 = 1 continues a run, 0 starts one.
- The stage comes from table 0x80560708[save +0x9C] (records 5..27 = stg0100..stg0700).
- It sets G+0x125 = 1, which selects `_hrd.dat`.

**New developer options** (test tooling, uncommitted). Each writes what the game's menu writes at the existing
hand-off, including the play-record reset:
- `--boot-2p <stg0800|0801|0802>` and `--boot-2p-chars a,b` (14..19): Play2POnce.
- `--boot-2p-three`: Play2PThree.
- `--boot-last-story`: PlayStory mode 2.
- `--boot-expert <0..22>`: PlayHard, continuing at that run position. Experimental: the save block's run word is
  written, not loaded from a save.

**Verified so far:**
- **`--boot-2p stg0800`:** StageManager(800) at 1.317 s, init done 1.575 s, ShowAction at 1.583 s, PlayAction at
  12.517 s. The capture at 13 s shows the split screen, the arena lit and both players posed
  (`.build/stage_loading/cap2p/boot2p-0800/t13.png`).
- **`--boot-last-story`:** PlayStory mode 2 plays the Last Story event scenes:
  - StageManager(100) at 1.4 s, then EVENT0909 / 0907 scenes at 18, 131, 181 and 208 s, each with an EventAction;
  - each loads in 0.23–0.33 s with the native read;
  - then **StageManager(700), The Last Way**, at 341.36 s, PlayAction at 341.65 s. With no input the player dies and
    continues every 37.7 s.
- **Original vs native read on the Last Story chain:**
  - The Last Way's own load is 1.42 s original (349.88 → 351.30 s) and 0.29 s native;
  - the six loads of the chain together reach The Last Way's gameplay **9.65 s sooner** (341.65 s instead of
    351.30 s).
- **2P through the real 2P boot, original vs native read** (`ab2p/`, states paired by game frame):

| Stage | PlayAction original | native | Result |
|---|---|---|---|
| stg0800 | 13.18 s | 12.52 s | identical (4 pairs, D 174), including heap addresses |
| stg0801 | 13.48 s | 12.52 s | identical (4 pairs, D 252) |
| stg0802 | 13.32 s | 12.52 s | identical (5 pairs, D 208) |

  In 2P, PlayAction follows the 2P start sequence (ShowAction), about 11 s after the load.

## 10. Proposed batch plan (native C++20, each batch verified against the original)

**Method per batch** (as for the player, `SHADOW_PLAYER_NATIVE_RECONSTRUCTION.md` 6):
1. A scope file of the batch's ranges, cut from `loader_scope_ranges.txt`.
2. **Before:** targeted strict `--verify-player-strict` over those ranges on three loads: Westopolis stg0100, Final
   Haunt stg0604 and one more stage with TEX blocks and hard mode. Each must show 0 mismatches and full coverage of the
   batch.
3. **The native body.**
4. **Offline oracle CTest:** the reference interpreter runs the original words and the native code on every file of
   that kind on the disc (all 59 stages), and compares the guest structures written byte for byte. Negative controls
   must be killed.
5. **After:** the same strict runs with the native owner in place.
6. **A/B replay:** `--state-report` at the first gameplay frame plus a 90 s tape (PCM, frames, AI census).
7. **Census re-run:** the owners counted.

No batch starts before the user approves, and nothing is committed or deployed without a request.

| Batch | Content (addresses) | Why this order | Verification specifics |
|---|---|---|---|
| L0 infrastructure | Census and trace kept as test data; a load-window acceptance probe (state-report at PlayAction 0x8020618C); per-batch scope files; more traced stages (stg0604, a TEX-block stage, hard mode, 2P, a restart) | Sets the baseline that every batch compares against | Two identical runs must be byte-identical (G0) before any A/B claim |
| L1 pure parsers | `sky2` 0x800CB1A4 + swap fn_80043C80; setid 0x800CB044 / 0x800CA7F0 / 0x800CB0EC; cam 0x801C8340; light swap (0x8004867C path); fog records 0x8016A4EC; BDT 0x8003AB90 (records part); PTP relocation fn_8040F66C; hint.bin | Leaves: deterministic, no async, no RW objects; byte-swap disappears for host-order data | Oracle over every layout / cam / light / fog / BDT / PTP file on the disc; strict ranges |
| L2 ONE archive layer | lookup 0x8004C938 / 0x8004CA50, sync open / parse 0x8004CE1C / 0x8004CD38 / 0x8004CFD8, member extract 0x8004C770 (PRS already native) | Every archive goes through it; `native_assets.cpp` already parses the format host-side (shared reader, one implementation) | Oracle over all `.one` files on the disc (members, sizes, PRS bytes); strict |
| L3 synchronous file reads | 0x800450C4 / 0x80045014 → asset namespace directly (no RtGCNFS, no RW file stream for whole files) | Removes the yields inside synchronous loads; prerequisite for DISC_SYSTEM_MAP cuts 6–7 | Same bytes at the same destination; A/B (the yields today interleave the workers: completion order checked) |
| L4 asynchronous I/O | TBGRead / TExpandBG as native async requests (`NATIVE_THREADS.md` plan, host worker pool, completion at a deterministic point) + TOneFileAsync's six loaders + ResourceOneFile async mode; read pause 0x805EF1DC as native state | **New input for the plan:** during the loading screen the publication point is the ShowRaster sleep 0x80495D2C (and the RtGCNFS yields), not RenderStart's yield, which is suspended | Completion order and tick per job vs the original (trace); stall time budget; savestate with a request in flight; cold / warm loads |
| L5 world blocks | TBlockManage mask / load / release (0x8003AAE8, 0x8003A8D4, 0x8003A6DC, 0x800473A8), LandALoadMananger states 0x800466B4, callbacks 0x80046418 / 0x80046438; RpWorld creation stays translated until rwcore is wired (separate decision) | Block choice is pure gameplay state (which worlds exist and collide) | Block sets per frame equal on tapes that cross block boundaries; collision verify (`GEKKO_VERIFY_WORLD_COLLISION`) |
| L6 set objects | preload pass 0x800CA780, factory table walk, stageLoad request list 0x80576FA4, ResourceManagerTask queue, SetManagerTask gate 0x800CB794, layout manager scans 0x800CAC08 / 0x800CAA2C / 0x800CA8D8 (range leaves already native) | After L1 / L2 (it consumes the layouts and archives) | Spawn census (slot, tick, class) equal; AI census; object archives byte-equal |
| L7 stage flow and loading screen | StageManager ctor / vf1 / state 0 (0x80178C2C, 0x80178A48, 0x801783D4), TStageInitalizeTask 0x80177510, InitAction / PlayAction hand-off, NowLoading / CNowLoadingThread | Glue last: it orders the batches above; the loading screen's own present is a renderer decision | First-gameplay tick equal (or a user-approved change, e.g. no unpaced loop), the full A/B |

**Decisions the user should make before L4 / L5:**
- whether load timing may change: completion at the same tick, or as fast as the host allows if gameplay is shown to
  be unaffected;
- whether RpWorld streams move to `rwcore`'s native reader and the offline-converted assets ("no DVD code, converted
  assets").
