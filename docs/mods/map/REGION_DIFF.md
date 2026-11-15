# Region differences that matter when an NTSC-U-made mod is installed on PAL

Field `region`, 2026-11-14. Branch `mods/map-region`. Data: `test_data/mods/region_diff.json`, `test_data/mods/set_types.json`,
`test_data/mods/region_ingame_proof.json`. Generators: `tools/mods/map_region_diff.py`, `tools/mods/map_region_ingame.py`.
Marks: **M** measured from the files, **G** run in game, **I** inferred (not proven).

Almost every community mod is made from NTSC-U files; this build runs PAL GUPP8P data. A mod file that is a copy of the NTSC-U
original silently replaces the PAL file, and where the two differ the PAL content is lost.

## 1. The numbers (M)

PAL `game/files`: 3,480 files. NTSC-U: 3,479. Shared names 3,479, byte-identical 3,188, differing 291 (273 under `fonts/`,
18 elsewhere). The only PAL-only file is `fonts/stg0905/stg0905_JP02.met`; NTSC-U has no file PAL lacks.

The 18 non-font paths: 13 stage layouts, `nukkoro.inf`, `opening.bnr`, `csdFiles/SelectMode_GE.txd`, `csdFiles/SelectMode_IT.txd`,
`palsel/PalSelIt.txd`.

**Languages.** Both discs ship the same files for all six text languages (EN FR GE IT SP JP): 58 font banks x 6 on each disc
(the brief's "PAL has languages NTSC-U lacks" is not visible in the file sets; which languages an exe offers in its Options menu is
exe code, not data). So a font file for any language can be installed on PAL; whether the PAL exe reads it depends on the text
language selected (EN 1, FR 2, SP 3, GE 4, IT 5, JP 0, plus the Hungarian slot, which serves English-named files).

## 2. Stage layouts (13 files, all gameplay)

Format: `sky2` header (count, misc bytes), 44-byte records {position, rotation, flags, saved flags, class id, link id, range x100,
misc length, misc pointer}, then the misc words (`docs/distant-rendering/spawn_data.md`). Class names and parameter names come from the
game's own type table (0x8052C1A0, read from the PAL `main.dol`; `set_types.json`, 216 placeable classes). Layers: `cmn` always loaded,
`nrm` normal missions, `hrd` hard missions, `ds1` always-on scenery. `SETCOLLISION` (id 0x50, params kind, form, X, Y, Z, id, appear) is an
invisible collision volume with no model; its ctor 0x8011D4EC maps `kind` to a collision flag word / attribute (I for the in-game meaning).

Reading: PAL is a later revision. Its dominant change is **added invisible collision volumes** (in the hard layers and in Glyphic
Canyon / Lost Impact), plus a few enemy / ring / door tweaks. The pattern (boxes strung along a route, in the hard layers) looks like
out-of-bounds / sequence-break closure; that purpose is **I**, the placements are **M**.

| Layout | Stage | PAL vs NTSC-U (M) |
|---|---|---|
| `stg0201_cmn` | Glyphic Canyon | PAL adds 6 SETCOLLISION kind 4 boxes in a chain, x 7254..8969, y -273..484, z -13249..-16781, yaw 10..50 (334 vs 328 entries) |
| `stg0300_hrd` | Cryptic Castle | +1 SETCOLLISION kind 7, X/Y/Z 130/140/130 at (-3420, -1721, -20200), link 50 (90 vs 89) |
| `stg0401_cmn` | The Doom | one GUN_ROBOT at (2248, 5, 415): activation range 7 -> 5 (x100 units); one SETCOLLISION disc-only flag word (no effect: the game overwrites +0x18 on load) |
| `stg0403_nrm` | Mad Matrix | one SET_CHARACTOR moved 30 units (z 14604 -> 14574) and its ContactRange 5.0 -> 7.0 |
| `stg0501_cmn` | Air Fleet | one RING raised 3 units (y -610 -> -607) |
| `stg0501_hrd` | Air Fleet | +2 SETCOLLISION kind 7 (30 vs 28) |
| `stg0502_hrd` | Iron Jungle | +1 SETCOLLISION kind 7 at (-6176, 0.5, -10670); one RING group 30 -> 40 rings (12 vs 11) |
| `stg0504_cmn` | Lost Impact | +1 GUN_SOLDIER (27, -490, 5818) and +1 GUN_BEETLE (-168, -470, 5954); +7 SETCOLLISION kind 0 (small boxes in the start corridor); SET_VEHICLE moved 4.5 units and its range 5 -> 10; 3 DOORs with box size 20-30 -> 100 (348 vs 339) |
| `stg0600_cmn` | GUN Fortress | one WATCH CAMERA (CCTV) activation range 10 -> 6 (x100 units) |
| `stg0600_hrd` | GUN Fortress | +2 SETCOLLISION kind 7 (19 vs 17) |
| `stg0602_hrd` | Lava Shelter | +1 SETCOLLISION kind 7 (7 vs 6) |
| `stg0604_ds1` | Final Haunt | one ART GADGET (scenery) moved 9 units in x. Cosmetic |
| `stg0604_hrd` | Final Haunt | +4 BK_GIANT (two positions, each present in two layer variants), +2 MOVE_FOOTING (same pair), +3 SETCOLLISION kind 7 (14 vs 5) |

SETCOLLISION kinds in the PAL layouts overall: kind 6 x415, 5 x264, 0 x156, 2 x85, 7 x47, 4 x23, 10 x23, 3 x3, 9 x3, 11 x1.

### In game (G): one case, two stages

Setup: `final-47133fd7.exe` (the game with the mod loader), PAL `game/` data, `--boot-stage <stage> --headless --no-audio
--stop-at-tick 607500000` (15 game seconds), `--state-report` with `--report-peek "0x805EF77C>0:4,0x805EF77C>0x41C>0:<44*n>"` (the
layout manager's placement count and entry array). Stock = `--no-mods`. Mod = `--mods-dir` pointing at a temporary dir holding one mod
(`mod.json` + `files/stgNNNN/stgNNNN_cmn.dat` = the NTSC-U file, size 24,448 / 25,584 B). One game process at a time; the asset trace shows
the guest read of `stg0201/stg0201_cmn.dat` with length 24,448 (mod) against 24,864 (stock).

| Stage | Loaded placements stock | with the NTSC-U `cmn` | Missing in the mod run |
|---|---|---|---|
| stg0201 | 998 | 992 | exactly the 6 SETCOLLISION boxes above (SETCOLLISION 38 -> 32) |
| stg0504 | 972 | 963 | GUN_SOLDIER, GUN_BEETLE, 7 SETCOLLISION; the SET_VEHICLE has the old position and range 5 instead of 10 |

So installing a NTSC-U layout on PAL really removes the PAL placements in the running game. **Not verified:** what the volumes do in play
(no tape drove the player into them); the `hrd` layers (the boot-stage route runs the normal layer; hard mode needs the mission's hard
flag, byte `[0x8057E760+0x125]`).

## 3. Other files (M unless marked)

| Path | What differs | Risk |
|---|---|---|
| `nukkoro.inf` | Only `REGION : PAL50` / `AREA : EU` against `NTSC` / `US` (CRLF line 65-66; 4,144 vs 4,143 B). The game parses them into its InfDatabase (G: peek of 0x8057B954, region 2 -> 0, area 2 -> 1). A 10 s Westopolis boot with the NTSC-U file produced the same frame sha256 and the same log as stock. What reads the field was not traced | none observed |
| `opening.bnr` | GameCube banner: PAL `BNR2` (6 text blocks, 8,096 B), NTSC-U `BNR1` (one block, 6,496 B). Image and texts identical. The IPL reads it, the game does not (docs/resource-spine/boot_globals.md) | none |
| `csdFiles/SelectMode_GE.txd`, `_IT.txd` | same size; one 512x512 texture `selectmode` (the mode-select artwork of that language) differs (15,229 / 4,173 bytes, palette included); the other 4 textures are identical. The decode of the artwork itself was not finished | cosmetic |
| `palsel/PalSelIt.txd` | the Italian PAL 50/60 Hz screen: texture `screentest` differs (244,729 of 262,800 bytes), `test` identical. NTSC-U ships a different picture | cosmetic |

## 4. Fonts (M)

Per language, PAL against NTSC-U (58 banks each):

| Lang | `.fnt` differ | of which bytes only | key set differs | records with other text | timing differs | `.txd` differ | `.met` differ (code set / rects) |
|---|---|---|---|---|---|---|---|
| EN | 16 | 13 | 2 | 3 | 0 | 3 | 3 (1 / 3) |
| FR | 13 | 10 | 2 | 11 | 0 | 3 | 3 (1 / 3) |
| GE | 19 | 16 | 2 | 5 | 0 | 3 | 3 (0 / 3) |
| IT | 27 | 24 | 2 | 5 | 0 | 3 | 3 (1 / 3) |
| SP | 26 | 23 | 2 | 5 | 0 | 2 | 2 (1 / 2) |
| JP | 57 | 3 | 2 | 63 | 0 | 38 | 49 (46 / 49), +1 PAL-only `.met` |

1. **Most `.fnt` differences are nothing.** 89 of 158 differing `.fnt` have the same keys, texts, priorities, windows and voice ids; only the unread
   `4 x count` byte tail differs (`docs/re/text.md` 4.1). Priorities / windows / voice ids are identical in every record both discs have: a NTSC-U `.fnt`
   never changes subtitle timing.
2. **Real text differences in EN** are confined to `Advertise_EN`, `Save_EN` (memory-card wording: key 5522100 line breaks) and `stg0700_EN` (key 1373001:
   NTSC-U has a leftover debug string `35 X 3 lines`, PAL has an empty text). FR/GE/IT/SP: the same banks plus `6058100` (the "not the original card"
   message is worded differently) and FR `5106100` (list number NTSC-U `099:`, PAL `100:`).
3. **Key shift (the one thing that bites).** PAL `Save_<L>.fnt` has 163 records and `Advertise_<L>.fnt` 958; NTSC-U 162 and 957: key 5489100 (the PS2
   "Game Data corrupted" message) is missing, at index 0. The PC save-text rewrite (`native_save_text.cpp`, on by default) rewrites the Save tables **by
   message index** (80..126). With a NTSC-U table all 47 of those indexes name a different message than in PAL (index 80: PAL key 6012100, NTSC-U 6013100), so
   the PC wording lands on the wrong messages and the intended ones keep the GameCube words. The Advertise rewrite matches by original text and only
   misses the PS2 wording. Real case: Reloaded's `Save_*.fnt` (all six languages) have 163 records but lack key 5489100 and add 6045101: 34 of the 47 indexes are
   shifted. Xbox Buttons' Save/Advertise tables carry the PAL key sets (0 shifted).
4. **A mod that replaces only `*_EN` files** changes only English: with FR/GE/IT/SP/JP selected the game opens that language's own `<bank>_<LANG>.*`; the
   Hungarian slot serves the English-named files under its own text (it builds its fonts itself), so an EN font mod does not show there either.
5. **`.met` / `.txd` travel together.** Every differing `.met` (63) has different glyph rectangles: PAL re-packed the glyph sheets. A NTSC-U `.met` with the
   PAL `.txd` (or the reverse) points glyphs at the wrong cells. The glyph code sets are identical except `stg0700_{EN,FR,IT,SP}00.met` (NTSC-U has the glyph `5`
   for the debug string) and 46 JP files.
6. **Glyph coverage.** The PAL texts of each language are fully covered by NTSC-U's own-language `.met` of the same bank (0 banks missing a glyph, EN/FR/GE/IT/SP). Used with
   the NTSC-U **EN** `.met` instead, 57 banks of each of FR/GE/IT/SP lack glyphs (accented letters, `ß`, `ä ö ü`, `ç`, `œ`, `…`; the exact set per language is in
   `fonts.ntsc_u_glyph_coverage_of_pal_text`): those characters would draw as nothing.
7. **JP.** 57 of 58 JP `.fnt` differ (63 record texts, e.g. list numbering `100 ` (NTSC-U) vs `100: ` (PAL); the memory-card messages) and 38 sheets, 49 metrics: the JP set is
   close to a different font build. A mod rarely ships JP; if it does, ship the PAL JP files as a whole.

### Real mods (M, `real_mods` in the JSON; `$S/testmods`)

| Mod | Differing paths it ships | What it is |
|---|---|---|
| Text Fixes | 1 (`stg0700_EN.fnt`; `stg0601_EN.fnt` is region-identical) | EN only. Changes button wording ("R Button" -> "R trigger", "B Button" -> "X button") and 14 windows / priorities in `stg0601`; at key 1373001 it carries PAL's empty text, not NTSC-U's debug string |
| Xbox Buttons | 158: 14 byte-equal to PAL, 9 byte-equal to NTSC-U, 135 modified | all six languages; Save / Advertise tables have the PAL key sets (163 / 958) |
| Reloaded | 99: 1 byte-equal to NTSC-U, 98 modified | all 13 layouts rebuilt from NTSC-U: 2 of the 31 PAL-only placements are present (both in `stg0604_hrd`), 29 are lost; its Save tables shift 34 indexes; Advertise tables carry the NTSC-U key set |

## 5. What the manager should do, and the exact warning texts

The manager can decide with data it already has: `region_diff.json` holds, per differing path, `pal_md5` / `usa_md5`. For a mod file at a listed path (match case-insensitively):

* md5 equals `usa_md5` -> the mod ships the **unchanged NTSC-U file**: show the warning of the path's row below (`{summary}` = the JSON `summary`).
* md5 equals `pal_md5` -> no warning.
* neither -> the mod edited it; layouts: if the manager decodes the `.dat` it can count how many of the entry's `pal_only` placements are still present
  (as `check_mods` does) and warn "N of M PAL changes are missing"; otherwise show the short text.
* If `mod.json` says `game_id: GUPE8P`, add one line: "Built for the NTSC-U disc."

Texts (English, plain):

| Path | Risk | Warning |
|---|---|---|
| the 13 `stgNNNN_{cmn,nrm,hrd,ds1}.dat` | gameplay | "This mod replaces the {stage_title} {layer_meaning} layout with the NTSC-U version. The PAL version is a later revision: {summary}. Installing this file removes those changes and can change gameplay in this stage (enemies, collision, rings). Keep it only if the mod is meant to change this stage." Short form for modified files: "This mod edits the {stage_title} layout. {n} of {m} PAL changes ({summary}) are not in its file." |
| `stg0604_ds1.dat` | cosmetic | "This mod uses the NTSC-U Final Haunt scenery layout; one scenery piece sits 9 units away from where PAL puts it." |
| `fonts/Save/Save_<L>.fnt`, `fonts/Advertise/Advertise_<L>.fnt` with the NTSC-U key set (162 / 957 records) | cosmetic | "This mod's {L} save / front-end message table comes from the NTSC-U disc (one message fewer). The PC memory-card wording can then appear on the wrong messages, and some save dialogs show GameCube wording. Use a table that has the PAL message set (163 / 958 messages)." |
| other `.fnt` with a text or key difference | cosmetic | "This mod ships the NTSC-U wording of {n} message(s) in {bank} ({L}). Only the text changes; timing is the same." |
| `.fnt` with bytes-only difference | none | no warning |
| `.met`, `.txd` under `fonts/` | cosmetic | "This font file is from the NTSC-U disc, whose glyph sheets are laid out differently. Install the .met, .txd and .fnt of one bank and language together; a mix draws wrong or missing letters." |
| mod ships only `*_EN` fonts | info | "This mod changes English text only. It has no effect while another game language is selected." |
| `csdFiles/SelectMode_GE.txd`, `SelectMode_IT.txd`, `palsel/PalSelIt.txd` | cosmetic | "This mod replaces the {German / Italian} screen artwork with the NTSC-U picture ({file}). Other languages are not affected." |
| `nukkoro.inf` | none | "This mod's nukkoro.inf declares an NTSC-U disc (REGION NTSC, AREA US). No effect was found in tests." (show only in the details) |
| `opening.bnr` | none | none (the IPL banner is never read) |

## 6. Limits

* The effect of each added SETCOLLISION in play, and the hard layers, were not run in game; only the placement load was (section 2).
* `nukkoro.inf`: consumer of the REGION / AREA triple not traced; one 10 s boot compared.
* `SelectMode_*` / `PalSelIt` artwork not decoded to a picture; the pixel counts only say the artwork was redone.
* "Later revision" is inferred from the data (PAL fixes in 13 files); no disc revision field distinguishes them (all three discs are revision 0).
* The save-text index shift is shown by key arithmetic on the real tables (`save_advertise_key_shift`, `real_mods.*.save_tables`), not by a screenshot of a save dialog.
