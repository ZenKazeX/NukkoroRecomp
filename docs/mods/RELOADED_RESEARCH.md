# What "Reloaded mods" are, and what the PC build can take

Researched 2026-11-14 for the mod loader (docs/mods/DESIGN.md). Sources: the GitHub organisation
ShadowTheHedgehogHacking (repo listing, README files, trees, release assets; licences from the GitHub API), the
GameBanana listing of Shadow the Hedgehog (game 6218, most-downloaded mods, their files and descriptions), and our own
extracted PAL (GUPP8P) and NTSC-U (GUPE8P) discs. Nothing downloaded was executed. No file or code of a repository
without a licence, or under GPL/LGPL, was copied into this tree; downloads used for testing stayed outside it.

## 1. Shadow the Hedgehog: Reloaded itself

`ShdTH-Reloaded` (archived, no licence: knowledge only) is a single improvement mod, not a mod loader. Its v1.2
(2024-07-01, final) is distributed as xdelta patches applied to a whole disc image. Its README states:

- The patch exists for NTSC-U, PAL and NTSC-J source images, and every one yields the **same** patched image
  (one SHA-1 per aspect variant). The result is a new game id, **GUPR8P**, built from the **NTSC-U** disc
  (`workspace/README.md`: "You must use the NTSC-U ROM as a base").
- The repository is the patch's source: `ISO EDITS/files` (738 disc files: stage layouts `.dat`, archives `.one`,
  textures `.txd`, fonts `.fnt/.met`, music `.adx`, new files), `ISO EDITS/sys/main.dol` (a modified NTSC-U
  executable), `AFS/PRS_VOICE_E/*.adx` (37 voice entries, applied with PAFS by entry name), `code/` (Gecko codes in
  `GUPR8P.ini`, and Melee Code Manager assembly libraries `code/MCM/*.txt` injected into the DOL), `Dolphin Addons/`
  (a Dolphin texture pack and a GraphicMods bloom setting), `workspace/Widescreen-ShadowTH-overrides/{4x3,16x9}`
  (alternative file sets).
- Most of its gameplay changes are **code**: S rank, Red Rings, skippable cutscenes, mission lock-in, jump-dash on L,
  weapon styles, vehicle handling, faster Shadow, ShadowSX timing. They live in the modified DOL and the MCM/Gecko
  code. Its stage remixes, restored dialogue and music, model and texture fixes are **data**.

So "Reloaded" is an ecosystem of **disc-image patches built on top of NTSC-U data plus NTSC-U code changes**. There is
no Reloaded mod format, manifest or loader to be compatible with. (Reloaded-II, Sewer56's PC mod loader, is unrelated:
it injects into native x86/x64 games; Shadow has no PC release and no Reloaded-II mods exist for it. Its code is
GPL-3.0; nothing of it is used here.)

## 2. What the community distributes (GameBanana, the org's repositories)

Real downloads examined and installed in testing (section 6):

| Kind | Examples | Layout as shipped |
|---|---|---|
| Disc-file replacement | Cream the Rabbit, Playable Tails (`shadow.one`), Xbox Buttons (`files/csdFiles/...`), Text Fixes (`fonts/...fnt`), More Reasonable Missions (`files/nukkoro2.inf`), HQ GUN Soldiers (`files/enemy/*.one` + `sys/bi2.bin`), Maria Fixed Eyes (`.../Gamecube Rebuilder/.../root/character/maria.one`), shadow-model-fixes, shadow-event-fixes (`files/event/*.one`) | `files/`, `root/` (GameCube Rebuilder), loose files, any wrapping folders |
| Dolphin texture pack | Shadow Android, Regular Rouge (DDS), Subtitle Remover, Maria Fixed Eyes (Dolphin variant), HQ GUN Soldiers, Reloaded's addons | `tex1_<w>x<h>[_m]_<hash>[_<tluthash>]_<fmt>.png/.dds` in `GUPE8P/` (or any) folders |
| Dolphin resource pack | No Gloss (a zip with `manifest.json`, `textures/GUPE8P/...` inside the download zip) | nested archive |
| AFS entry replacement | Reloaded's `AFS/PRS_VOICE_E/*.adx` | folder named after the archive, files named after entries (PAFS) |
| Gecko / Action Replay codes | No Bloom (`044978f0 60000000`), Reloaded's `GUPR8P.ini` | text to paste into Dolphin |
| Modified executable / injected assembly | Reloaded `sys/main.dol`, MCM libraries; ShadowSX; 2P mods | DOL or disc-image patch |
| Whole-image patches | Reloaded, 2P-Reloaded, Ultimate Mode (xdelta) | `.xdelta` against a specific image hash |

Region: almost everything is built on **NTSC-U**. Measured here: of the 3,479 file names the two discs share
(PAL has one extra, `fonts/stg0905/stg0905_JP02.met`), **3,188 are byte-identical**; 291 differ — 273 under `fonts/`
(PAL carries more languages per table), the language-specific `csdFiles/SelectMode_GE/IT.txd`, `palsel/PalSelIt.txd`,
`nukkoro.inf`, `opening.bnr`, and **13 stage layout files** (`stg0201_cmn.dat`, `stg0300_hrd.dat`,
`stg0403_nrm.dat`, `stg0501_cmn/hrd.dat`, `stg0502_hrd.dat`, `stg0504_cmn.dat`, `stg0600_cmn/hrd.dat`,
`stg0602_hrd.dat`, `stg0604_ds1/hrd.dat`, `stg0401_cmn.dat`: PAL's later revision). An NTSC-U-made file therefore
drops into the PAL tree under the same name; replacing one of those 13 layouts replaces PAL's revision with the
modder's. Texture hashes are content hashes, so a pack made on NTSC-U matches PAL wherever the texel bytes are equal
(TEXTURE_PACKS.md: e.g. Subtitle Remover 51 of 52 on PAL, 52 of 52 on NTSC-U).

## 3. Licences (from the org inventory, wt-voice-e4/docs/external/SHADOW_HACKING_ORG.md)

- No licence (all rights reserved, knowledge only): ShdTH-Reloaded, CharacterMods, shadow-model-fixes,
  shadow-event-fixes, ShadowTHTextEditor, Widescreen-ShadowTH, 2P mods. Their files were used only as local test
  inputs; none are in this repository.
- GPL-3.0 / LGPL-3.0 (copyleft, knowledge only unless the owner decides otherwise): HeroesONE-Reloaded, ShadowSET,
  ShadowMotionSwapper, ShadowONE, ShadowRando, Reloaded-II.
- MIT/BSD (reusable with notices): PAFS, ShadowFNT, ShadowTXD, ShadowMLT, ONEBatch, prs.net, VCDiffGUI. **No code was
  taken from any of them.** The AFS layout (header, table, name records) is our own reading of the disc's
  `PRS_VOICE_E.afs`; PAFS only established that entries are matched by name, which is a convention, not code.
- Dolphin (GPL-2.0): the texture-name rule was re-derived and proven against real packs (TEXTURE_PACKS.md); no
  Dolphin code is used. XXH64 is our own implementation (runtime/src/gekko/texture_packs.cpp), checked against the
  algorithm's reference values; no xxHash source is vendored.

## 4. How each kind maps onto the port

The PC build is a static recompilation made with GekkoForge: the PAL DOL's code is translated ahead of time into the
executable, and the disc's files are read natively from the extracted `files/` tree through one storage backend
(`shadow/backends/storage_backends.h`) whose tree is scanned once at mount; the game's `DVDOpen` resolves names in
that tree (`native_file_api.cpp`), never in a guest FST. That makes the file layer the natural, zero-cost place for
mods.

| Kind | Supported | How |
|---|---|---|
| Disc-file replacement (any format) | **Yes** | Overlay applied to the storage tree at mount: same path (any case) served from the mod's file, sizes from the host file. New files are added the same way (`DVDOpen` finds them; only code that asks for them reads them). |
| AFS entry replacement | **Yes** | The archive is rebuilt once into `mods/.cache/<name>-<digest>.afs` (entries re-laid out, alignment and name records kept) and overlaid; reused while inputs are unchanged. The switchable English/Japanese voice archive reads the rebuilt English half. |
| Dolphin texture packs / resource packs | **Yes** (lifted renderer) | `gekko/texture_packs.h`: Dolphin's names computed from the guest texture bytes at texture build, looked up in the packs; PNG and DDS (RGBA, BC1–BC3). Only what the GPU samples changes. |
| Dolphin GraphicMods (bloom scaling etc.) | No | They patch Dolphin's own renderer passes; the port's bloom is its own (Options menu). |
| Gecko / AR codes | **No** | Codes are writes into guest memory addresses of a specific DOL. `04xxxxxx` writes into **code** (e.g. No Bloom's `nop` at 0x804978F0) change instructions; this executable does not run the DOL's instructions, it runs translated C++ compiled from them, so such a write changes nothing (or corrupts data that happens to sit there). Data writes would need addresses of the PAL DOL (the codes target NTSC-U or GUPR8P) and a per-frame hook into guest memory; even then they can change gameplay in ways no one can validate. Reported, kept in the mod's `codes/` folder, never applied. |
| Modified `main.dol`, MCM/assembly injections | **No** | The executable *is* a translation of one specific DOL (PAL GUPP8P). Another DOL's code (Reloaded's NTSC-U-based one) would have to be recompiled into a different executable. The installer recognises these files, reports them, and does not use them. |
| Whole-image xdelta/VCDIFF patches | **No** (directly) | A patch is relative to a disc image of a fixed hash and rewrites the image, DOL included. Users who have the patch's *source* tree (Reloaded publishes it) install that tree instead; its data parts work (section 6). Applying the patch to an image and installing the resulting `files/` would give the same data parts. |
| Riivolution XML | Not attempted | Wii-side file patching; same data-vs-code split applies. |

Consequence for Reloaded specifically: **its data loads and runs, its code does not.** Its remixed layouts, models,
textures, fonts, music references and voice lines are served by the loader; S rank, Red Rings, the
new player actions, mission lock-in, skippable cutscenes and the other code changes are absent. Data that only
Reloaded's code reads (e.g. the new `stg0210_nrm.dat`-style boss layout variants and new music tracks) is installed but
never asked for. Layouts that assume Reloaded's object behaviour run with the original behaviour (in testing, the
Westopolis remix's partner meeting triggers its stock intro at a new place). This is "Reloaded data on the original
game", and the manager says so (it lists the ignored code files on the mod).

## 5. Decisions taken

- Our own folder layout and `mod.json` manifest (no Reloaded format exists to adopt); field names follow common mod
  loader practice (id, name, version, author, dependencies, incompatible, load_after, options).
- Priority list in `mods.json`, highest first; the higher mod wins a file. Dependencies always load before (below)
  their dependents.
- Community downloads are installed by placing each file against the game's own file index, so users drop the
  archive they downloaded; no re-packing is asked of them.
- Code patches are never "translated" or partially applied: there is no honest way to validate them against the
  recompiled executable, and the gameplay rule (bit-exact unless the user chose a mod that changes data) forbids
  guessing.

## 6. Real mods tested (2026-11-14; details and evidence in HANDOFF.md)

Installed by the manager from the downloaded archive or repository folder, all successfully: Maria Fixed Eyes (zip),
Cream the Rabbit (rar), Shadow Android (zip), More Reasonable Missions (zip), Playable Tails (zip), Regular Rouge
(zip), Xbox Buttons file version (zip), Text Fixes (7z), Subtitle Remover texture pack (zip), HQ GUN Soldiers (zip),
No Gloss (zip with nested resource-pack zip), shadow-model-fixes, shadow-event-fixes, ShdTH-Reloaded (repository
tree: 737 files, 37 voice entries, 21 textures, two widescreen override sets as options, DOL/Gecko/MCM reported).
