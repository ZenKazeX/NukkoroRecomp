# Which game context reads which disc file

Data: `test_data/mods/file_contexts.json` (about 750 KB, names and numbers only, no game bytes).
Generator: `tools/mods/map_file_contexts.py` (stdlib; sub-commands `run`, `build`, `check`).
Input table of the guest stages: `test_data/mods/list_stages.txt` (the output of the stock executable's `--list-stages`).

The mod manager uses it for the per-mod line "affects: Westopolis, Glyphic Canyon, front-end menus, cutscene 1101 ..."
and to flag mod files the game never opens.

## Method

1. **File-open trace.** `--asset-trace <file.jsonl>` writes one `file_read` record per guest read, with the resolved
   disc path (`runtime/src/gekko/asset_trace.cpp`). The cheapest correct method: it needs no rebuild and sees every
   read of the native file API, including streamed ADX music and the voice archive. The `[file-api]` counters in
   stdout say `opens 0`, so open calls themselves are not counted; a read is the evidence. A request for a file that
   does not exist leaves no trace (see "Limits").
2. **Runs** (stock executable `base-42f71847`, one process at a time, `--headless --no-audio`, no save, no tape, no
   input). Every run takes 1 to 6 wall seconds except the two long story routes. 80 traced runs:

   | runs | flags | what it covers |
   |---|---|---|
   | 38 `bs<NNNN>` | `--boot-stage stgNNNN`, 30 s of game time | every stage `--boot-stage` accepts: 22 story stages and 16 bosses |
   | 20 `ss<NNNN>` | `--boot-story-stage stgNNNN`, 30 s | stages `--boot-stage` refuses (0001-0003, 0005, 0700, 0710, 0800-0802, 0901-0911) |
   | 4 `p2*` | `--boot-2p stg0800/0801/0802`, `--boot-2p-three` | the three 2P arenas and their intro event |
   | 11 `be<NNNN>` | `--boot-event event09NN`, 60 s | the eleven event stages 0901-0911 |
   | `story`, `laststory`, `flashback`, `expert0` | `--boot-story`, `--boot-last-story`, `--boot-flashback`, `--boot-expert 0` | new game start, the Last Story event chain, the library replay, Expert mode |
   | `fe_pal`, `fe_title`, `fe_select` | no boot flag; `--tv-format ntsc`; `--ui-test select` | cold boot (PAL Hz screen), the memory card / title screens, logo + intro movie + title |

   Not accepted by the game's own hand-off (exit code 2, no trace): stg0000, stg6000, stg8900, stg9900, stg9901.
   Of these only stg0000 is in the guest table (its id 0 is refused by `--boot-story-stage`); the other four have a
   folder on the disc and no record in the stage table, so nothing in the game can boot them.
3. **Attribution.** A file named after a stage (`stgNNNN/`, `fonts/stgNNNN/`, `sng_stgNNNN.adx`), an event
   (`event/eventNNNN*`) or a movie (`ENNNN.sfd`) gets that stage / event / movie as its context, whatever run read it.
   A shared file (character, enemy, sound bank, common data, UI layout) gets the context of the run that read it. The
   story-route runs (`ss`, `be`, `story`, ...) first build the story's own stage 0100, so for them a shared file counts
   only if the Westopolis `--boot-stage` run did not read it too (otherwise Westopolis' sound bank would be credited to
   every event stage). A shared file read by at least 95 % of the 38 `bs` runs is `all-stages`.
4. **Derived entries.** Files no run read get contexts from a name pattern (`basis` says which): stage folder, stage
   number in `StageTitleNNNN_XX.txd` / `pauseNNNN_XX.txd` / `sng_stgNNNN.adx`, event number (`sng_EVNNNN_E.adx`,
   `event/eventNNNN*`), movie number, language variants of an observed file (`_EN` to `_FR/_GE/_IT/_JP/_SP`, `PalSelEn`
   to `Fr/Ge/It/Sp`), UI layout names (menus, 2P, HUD). A file that is only a string in `main.dol` gets the context
   `named-in-code`: the game can open it by that name, no traced run did.
5. **Events.** The game's own table in `main.dol`: 86 pointers at 0x8054B338, each to a record
   `{event id, stage id}` (two big-endian words, records at 0x805F8880-0x805F8B2C). The routine at 0x8025D674 looks an
   event id up in it (docs/re/events.md section 3.2). Meaning of the second word: the stage whose environment the cutscene
   plays in (0 = a movie). **Checked against the game:** the `--boot-last-story` trace loads stg0100, then event 8001
   (table: 100), stg0909 + event 8002 (909), the movie E8003, stg0909 + event 8004 (909), stg0907 + event 8005 (907),
   stg0909 + event 8006 (909), then stg0700: 5 of 5 events matched. The other 80 records are not exercised.
6. **Stage names.** The disc holds no text table of stage titles: the titles are pictures
   (`csdFiles/stageTitle/StageTitleNNNN_XX.txd`) and `fonts/stgNNNN/*.fnt` carry dialogue only. The English titles are the
   ones in the repo's `tools/launcher.py` `STAGE_TITLES` (40 stages); the game's internal stage-table names (`City1`,
   `Circuit`, `HorrorCastle`, ...) come from `--list-stages` and are stored as `internal_name`. Stages with no English
   title (practice, 2P arenas, event stages) show the internal name. Event cutscenes have no title in the game; the label
   is "Cutscene NNNN".

Reproduce:

```
python tools/mods/map_file_contexts.py run   --exe <stock exe> --game-data <dir with sys/ and files/> --out <runs dir>
python tools/mods/map_file_contexts.py build --game-data <dir> --runs <runs dir> --list-stages test_data/mods/list_stages.txt --out test_data/mods/file_contexts.json
python tools/mods/map_file_contexts.py check --map test_data/mods/file_contexts.json --mods <mods dir>
```

The committed `file_contexts.json` was built from traces made with a pre-seeded 60 Hz console setting (so the PAL
screen read `palsel/PalSelEn.txd`); the `run` command uses `--tv-format ntsc`. Compared on stg0100: the two read the same
74 files except that one palsel file. The map was checked to be identical in the part that matters (stage / event files).

## Schema

```
{ "schema": 1,
  "stages": { "stg0100": {"name": "Westopolis", "name_source": ..., "internal_name": "City1",
                          "kind": "stage|boss|last-story|2p-arena|event-stage|debug|not-in-guest-table",
                          "in_guest_stage_table": true, "boot_stage_ok": true, "traced_by": ["bs0100"],
                          "files_observed": 76, "files_total": 150} },
  "events": { "1101": {"label": "Cutscene 1101", "files": [...], "movie": "E1001.sfd", "music": "sng_EV1101_E.adx",
                        "table_stage": "stg0904", "host_stage": "stg0904", "observed": false,
                        "confidence": "observed|table|name only", ...} },
  "files": { "<disc path>": {"contexts": [...], "source": "observed|derived", "basis": "...", "runs": 3} },
  "dol_names": [...], "dol_name_formats": [...], "summary": {...} }
```

Context vocabulary: `boot`, `frontend`, `all-stages`, `stage:stgNNNN`, `event:eventNNNN`, `movie:ENNNN`,
`mode:2p-battle`, `mode:story`, `mode:last-story`, `mode:flashback`, `mode:expert`, `in-stage-ui`, `credits`, `movie`,
`named-in-code`, `lang:XX`.

`dol_names` (3,508 lower-case file names the game's code contains) and `dol_name_formats` (23 printf-style names such as
`stg%04d/stg%04d_nrm.dat`) let the manager answer "can the game open a file of this name at all?" for a mod file that
has no disc counterpart. A mod file that matches neither is a file the game never opens.

## Coverage numbers

3,480 disc files under `files/` (the 82 Japanese voice files of the Japan tree are not in this count).

| source | files |
|---|---:|
| observed (read in at least one run) | 1,036 |
| derived (name pattern or main.dol name) | 2,444 (of them 33 only `named-in-code`) |
| not covered by anything | 0 |

By area (observed / derived): `stgNNNN/` 598 / 656 (the derived ones are 621 numbered sector files `stgNNNN_NN.one`, 22
`_hrd.dat`, and a few `_dat/_texCM/_tex0N/_light` of stages not booted by `--boot-stage`); `fonts/stgNNNN` 165 / 1,016
(six languages; the five non-English ones are language variants); `csdFiles` 81 / 336; `event` 31 / 234; `enemy` 38 / 3;
`character` 16 / 4; `sound` 46 / 5; root files 49 / 120; `common` 8 / 1.

Typical stage: 72 distinct files per `--boot-stage` run (`bs` runs, mean). 42 observed files are read by every
stage run (`all-stages`: player model `character/shadow.one`, common effects, weapon/vehicle resources, the HUD layouts,
the player and system sound banks, the loading screen, `stg_cmn_gdt.one`, the voice archive); 57 files carry the `boot` context (read before any menu).

Events: 154 ids in all. 153 have files under `event/` (265 files), 7 are movies (E1001, E4101, E4102, E4201, E5024,
E8003, E8201). 85 ids are in the game's event table; 8 events were seen in a run (0950, 0978, 1001,
8001, 8002, 8004, 8005, 8006; the movie E8003 too), 79 are "table" confidence, 67 "name only" (0800-0826,
0900-0926 and 0960-0980 style ids: 2P intro and bonus scenes; the table has no record for them, so their host stage is
unknown except for the eleven event stages `stg0901`-`stg0911`, which share the event's number).

Event -> stage (what the manager can say about a cutscene): `host_stage` is the table's stage id, observed to be the
stage loaded around the cutscene in 5 of 5 Last Story events. The rest follow the same table and are a high-confidence
inference, not an observation. In the story the stage graph is not in a table the runs could drive, so which
gameplay stage precedes which cutscene is **not** derived here beyond that record.

## Files no run opened

All 2,444 derived files were not read by any run. By reason:

- **Stage sector files** (`stgNNNN_01.one` ... , 621 files): the game streams them by the player's position; a headless
  run stands at the start, so only the first few load (stg0100: `_01`, `_02`, `_19` of 23). Context is the
  stage, certain by name, reading depends on where the player goes.
- **`stgNNNN_hrd.dat`** (22 stages): read in Expert mode only (`--boot-expert 0` read `stg0100_hrd.dat`); the normal
  `_nrm.dat` is read where it exists (stg0200 observed). Boss stages (`stg0210` ... `stg0618`) have neither on the disc.
- **Language variants** (`_FR/_GE/_IT/_JP/_SP` fonts, stage titles, pause layouts, `Loading_XX`, `Advertise_XX`,
  `PalSelXx`): the runs use English. The variant is read when that language is selected.
- **Event scenes** that no run played (the 79 table events and the 2P / bonus scenes).
- **33 `named-in-code` files** with no run: `enemy/GunSoldierData_ark00/densi/jungl.one` (enemy sets of ARK / Digital
  Circuit / Iron Jungle stages, not loaded in the first 30 s; a 120 s Iron Jungle run read nothing more), the four
  `character/shadow2p*` 2P skins, `common/VolumeShadowModel.one`, `sng_jin_*` jingles, `sng_vox01-06`,
  `sng_EV_music1-3`, `csdFiles/Warning.gncp`, `csdFiles/E3RomTitle.gncp`, `InfoWindow.txd`, `advertise.txd`,
  `gindows.png`, `mc_bunner_j.tp`, `opening.bnr`, `sound/B3_SHADOW_TEST.mlt`, `sound/GCAX.conf`, the `_J.mlt` banks.
  They have a name in the game's code; the trigger was not found.
- **Five stage folders no hand-off can boot**: stg0000, stg6000, stg8900, stg9900, stg9901 (40 files); stg6000, 8900,
  9900, 9901 have no record in the game's stage table.

## Mods installed in the test set, checked against the map

`check` run over `testmods/` (14 mods). Counts are files under `files/` plus options' `files/`; textures and AFS entries
are counted but not mapped (see Limits).

| mod | files | AFS | texture files | contexts touched | files never opened |
|---|---:|---:|---:|---|---|
| cream | 1 | 0 | 0 | `character/shadow.one`: every stage, boot, front end, 2P | 0 |
| playable-tails | 1 | 0 | 0 | same file: every stage | 0 |
| more-reasonable-missions | 1 | 0 | 0 | `nukkoro2.inf`: read at boot and by every stage | 0 |
| hq-gun-soldiers | 5 | 0 | 6 | enemy data in 10 stages (The Doom, Lost Impact, Westopolis, Lethal Highway, Central City, Death Ruins, GUN Fortress, 2P arenas 0800/0801) | 0 |
| maria-fixed-eyes | 16 | 0 | 4 | The Doom, Lost Impact (observed), event stage 0905; cutscenes 6401, 0916, 0917; 5 language copies | 0 |
| shadow-model-fixes | 7 | 0 | 0 | 9 stages (ARK, Lost Impact, Cosmic Fall, Iron Jungle, GUN Fortress, Black Comet, 2P arenas) | 0 |
| shadow-event-fixes | 138 | 0 | 0 | 124 cutscenes over 36 stages incl. Last Story events 8002/8004/8006, event stages 0904-0910, Blue Falcon, Iron Jungle, Egg Dealer (Lava Shelter); 16 files observed | 0 |
| text-fixes | 2 | 0 | 0 | Black Comet, The Last Way (dialogue fonts) | 0 |
| xbox-buttons-files | 372 | 0 | 0 | 55 stages, front end, movies, 2P (button layouts, UI) | 0 |
| shdth-reloaded | 763 | 37 | 21 | 55 stages, 124 cutscenes, front end, credits, movies, expert mode, 2P, UI | **10** never opened, 33 new but name known to the game |
| shadow-android, shadow-no-gloss, regular-rouge, subtitle-remover-texpack | 0 | 0 | 1-52 | texture packs only (not mapped) | n/a |

ShdTH-Reloaded detail: of its 763 files 378 are on the disc and read in a run, 342 are on the disc and derived, 43 have
no disc counterpart: 33 of those have a name the game builds (`stg%04d/stg%04d_nrm.dat`, `_hrd.dat` for the 16 boss
stages 0210-0618, `sng_stg0613.adx`) and 10 are never opened: `E4101.flo`, `E4102.flo`, `E4201.flo`, `E8003.flo`,
`E8201.flo`, `Op.flo` (no `.flo` name anywhere in the code), `common/ScoreDatb.bin`, `sng_EV_music4.adx`, `sng_vox07.adx`,
`stg_cmn_gd2.one`.

**The new boss `_nrm.dat` files are opened, checked in a run:** the loader executable with a probe mod that holds a dummy
`stg0210/stg0210_nrm.dat` and `stg0210_hrd.dat` read `stg0210_nrm.dat` in a plain `--boot-stage stg0210` run (and not
`_hrd.dat`, which Expert mode reads). Without the probe mod the same stage reads neither. So adding them to a boss
stage does change what the game loads, for the Select Mode and story route; `_hrd.dat` only in Expert mode.

## Limits

- Reads, not opens: a request for a file that is missing leaves no trace. Whether the game probes for a file that is
  absent was shown only for the boss `_nrm.dat` (probe mod above); other absent names are judged by the code strings.
- Headless runs stand still at the stage start with no input: streamed sectors, spawned enemies, mission-dependent
  data, pause menus, results, game over, cutscenes triggered inside a stage, the story library and every menu below the title
  were not exercised. The front end got as far as the logo, the intro movie `Op.sfd` and the title (`honban`), and a
  scripted press of Start led to a loading screen; the menu layouts (`SelectMode`, `Options`, `Story_Library`,
  `Synopsis`, `MediaMenu`, ...) are derived by name only.
- Story-route events other than 1001 and 8001-8006 were not played (the cutscene routes in this build reach the event's
  script (`event/eventNNNN.scr` is read) but not its scene file in a headless run).
- Hero / dark / normal route differences inside a stage are not distinguished.
- Language variants are derived, not run. The Japanese voice tree (`EXTRACTED_DISKS/Japan`) is outside this map.
- Texture packs (Dolphin names `tex1_*`) and AFS entries (`PRS_VOICE_E.afs` members) are not mapped to contexts. The voice
  archive itself is read at boot (`all-stages`), so any AFS-entry mod touches every stage's dialogue by that path.
- Names of English stage titles come from the launcher table; the game itself only holds the pictures.

## How the manager should use it

- Per mod: for each file under `files/` (and an option's `files/`), look up `files[<path>]` (case-insensitive). Group the
  contexts: `stage:*` -> `stages[id].name`, `event:*` -> `events[id].label` (+ `host_stage` name), the rest by the
  vocabulary above; collapse `all-stages` to "every stage". Show `lang:XX` as a language note. A file with only
  `named-in-code` is "loaded by the game, trigger unknown".
- A file with no entry: first test `dol_name_formats` (`format_regex` in the script) and `dol_names`; a match means the
  game builds that name (show the contexts the name pattern gives); no match means "the game never opens this file". The
  `check` command is the reference implementation.
- Show `source: derived` in a lighter style than `observed`.
