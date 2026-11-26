# Mod loader and ShadowModManager

The PC build of Shadow the Hedgehog (a static recompilation made with GekkoForge, under development) loads mods
natively from a `mods/` folder beside the executable. What the community's mods are and which kinds can work:
RELOADED_RESEARCH.md. Texture packs: TEXTURE_PACKS.md. Status and evidence: HANDOFF.md.

## Pieces

| Piece | Where | What |
|---|---|---|
| Model library `shadow_mods` | `runtime/mods/` (headers in `runtime/mods/include/shadow/mods/`) | JSON, manifests, state, discovery, resolution, plan, AFS rebuild, format checks, installer. No game code; shared by game and manager. |
| Game hook | `runtime/src/shadow_main.cpp` (block "mods"), `storage_backends.cpp` (`add_overlays`) | `prepare_runtime()` at start-up; the result becomes `StorageOptions::overlays` and the texture-pack list. |
| Host-side readers | `gekko/mod_overlay.h` | Services that open disc files by path (switchable voice archive, native asset prefetcher) see the same files. |
| Texture packs | `gekko/texture_packs.h` (lifted renderer) | Dolphin-named replacement textures. |
| Manager | `tools/mod_manager/mod_manager.cpp` → `ShadowModManager.exe` | Window + command line. |
| Tests | `runtime/tests/mod_loader_tests.cpp` (106 checks), `runtime/tests/texture_packs_tests.cpp` | Run without game data. |

## Folder layout

```
mods/
  mods.json                          state (written by the manager only)
  <id>/                              one mod
    mod.json                         manifest
    files/<disc path>                replaces or adds a disc file (case-insensitive match, disc spelling kept)
    afs/<ARCHIVE>/<entry name>       replaces entries of <ARCHIVE>.afs by name
    textures/**/tex1_*.png|dds       Dolphin texture pack
    options/<option>/<choice>/...    the same three folders, applied when that choice is selected
    codes/, docs/                    kept for reference (code patches are never applied), readme/preview
  .cache/                            rebuilt archives (safe to delete)
```

A folder with `files/`, `afs/` or `textures/` but no `mod.json` also works (named after the folder).

### mod.json

```json
{
  "schema": 1,
  "id": "maria-fixed-eyes",
  "name": "Maria Fixed Eyes",
  "version": "1.1",
  "author": "...",
  "description": "...",
  "website": "https://...",
  "game_id": "GUPE8P",
  "preview": "docs/preview.png",
  "dependencies": ["base-fixes>=2.0", {"id": "optional-thing", "optional": true}],
  "incompatible": ["other-maria-eyes"],
  "load_after": ["hd-textures"],
  "options": [
    {"id": "widescreen", "name": "Widescreen overrides", "default": "off",
     "choices": [{"id": "off", "name": "Off", "folder": ""},
                 {"id": "on", "name": "On", "folder": "options/widescreen/on"}]}
  ]
}
```

Unknown members are kept when the manager rewrites a manifest. Ids are `[a-z0-9._-]`, 1–64 characters.

### mods.json

```json
{"schema": 1, "mods_enabled": true,
 "order": [{"id": "cream", "enabled": true}, {"id": "shdth-reloaded", "enabled": true, "options": {"16x9": "off"}}]}
```

`order` is highest priority first. Written atomically (temporary file + rename). A damaged or newer-schema file is
never overwritten silently: the game runs without mods and says why; the manager asks before starting a new list.

## Rules

- **Priority**: the higher mod's file wins; the manager lists every conflict on both mods. AFS entries the same way.
- **Dependencies**: a mod whose required mod is missing, disabled, too old or itself not loading does not load (with
  the reason). The manager offers to enable disabled dependencies; nothing is enabled silently. A dependency always
  loads below its dependent, whatever the list says (stable topological order; `load_after` is a soft version).
- **Incompatible**: of two enabled mods that declare it, the higher-priority one loads.
- **Cycles**: every mod on a dependency/load_after cycle is left out.
- **Damaged files**: `.one`, `.txd`, `.dff`, `.afs`, `.adx` are checked structurally (header length vs file size,
  signatures, table bounds). A damaged file is refused at install and skipped at start-up (the disc's own file is
  served), because the game's parsers trust their input and a truncated archive stops the game. No false positive on
  the PAL disc (3,480 files), the NTSC-U disc (3,479) or any real mod tested (1,659 files). Movies (`.sfd`, `.flo`)
  are walked packet by packet: both the disc's layout and CRI sfdmux's (no end code, no padding: the community's
  re-edited intros, refused as damaged until 2026-11-14) are accepted; a cut stream is not (FORMAT_CHECKS.md 3.10).
- **Code patches** (DOL, Gecko/AR, MCM, xdelta): recognised, kept in `codes/`, reported, never applied
  (RELOADED_RESEARCH.md §4).

## Zero cost and bit-exact when off

With no `mods/` folder, `mods_enabled` false, no enabled mod, `--no-mods`, or a damaged state file,
`prepare_runtime()` returns an inactive setup before scanning anything: `StorageOptions::overlays` stays empty (the
storage tree is exactly the scanned tree, keys and sizes included), `mod_overlay` stays empty (lookups are one empty-map
test, at file-parse granularity only), and no texture pack is configured (the renderer's hook is one relaxed atomic
load per texture build). Nothing is hooked in guest code; no liveness root was added. Verified: the regression gate
(Westopolis 316 s and Final Haunt 120 s, state.json + PCM, behaviour trace `--exact`) is identical between the base
build and the loader build (HANDOFF.md).

With mods on, guest-visible differences are exactly the data the mods change (file bytes and sizes, AFS tables). A
mod that only changes textures leaves guest state untouched.

## Installing (what the manager does with a download)

Archives: `.zip` read natively (stored/deflate, zip64, CRC checked, entry names sanitised: no `..`, drive letters,
absolute paths or links); `.7z`, `.rar` and others through Windows' own `tar.exe` (libarchive, Windows 10 1803+), then
the extracted tree is checked for links. Nested archives one level deep are unpacked (Dolphin resource packs). Long
paths (>260 characters) are handled with `\\?\` paths throughout.

Placement, file by file (first rule wins; `install.h` has the list): `mod.json` at the top → copied as is; `tex1_*`
→ `textures/`; programs (`.exe`, `.dll`, scripts) and GameCube saves (`.gci`, `.gcs`, `.sav`, memory-card images) → left
out and named (never installed, never run, never written anywhere; a download that is only a program or a save is
refused with what it is - for this game's own save, with how to play from a copy: `--save-file <copy>`); code
material → `codes/` (reported); other `sys/` files → left out; `<ARCHIVE>/<entry>` where the disc
has `<ARCHIVE>.afs` → `afs/`; the longest tail of the path that is a disc path → `files/`; below a `files/` or
`root/` folder → `files/` (new file); a name only one disc file has → that path (loose `shadow.one`); documents →
`docs/`; the rest is listed as left out. Sets of files from one download that place the same disc file again
(Reloaded's 4:3/16:9 overrides, Xbox vs GameCube folders) become options, off by default. Name, author, version and
description come from a Dolphin resource-pack `manifest.json` or the readme; the preview is the download's picture.
Installing a mod whose id is installed updates it in place (the old folder is replaced only after the new one is
complete).

## The manager

A separate native Win32 program (`ShadowModManager.exe`, placed beside `ShadowTheHedgehog.exe`) rather than a game
menu: the in-game Options menu is the only menu we restyle, mods must be chosen before the game mounts its files (a
storage tree cannot be swapped under a running game without changing its timing), and installing needs file dialogs,
drag-and-drop and archive handling that belong to the desktop. It uses only Windows components (common controls v6,
WIC for previews, shell for Recycle Bin and drop) and is per-monitor DPI aware. It is drawn in Windows 11's style
(tools/ui/theme.h): Segoe UI Variable, rounded buttons with icons, Play in the accent colour, the selected mod's side on
a card, the Shadow icon. Its theme (as Windows, light, dark or Shadow) and zoom (100 to 200 %) are shared with the
Settings program through `appearance.cfg`
([PC_SETTINGS_FILES.md](../PC_SETTINGS_FILES.md#appearancecfg-the-programs-zoom-and-theme)).

Window:

- **List** (highest priority first): checkbox, name, version, author, content summary, status in colour (green
  loads, orange loads with a region warning, red cannot load, grey off). Space or double-click switches a mod;
  drag a row to change its place (an insertion mark shows where); Alt+Up/Down/Home/End move the selected mod.
  Right-click: switch on/off, move to top/up/down/bottom, edit info, open folder, remove, switch all on/off, install.
- **Filter** (Ctrl+F): name, author, id, content or status; the status line says how many rows are shown.
- **Details**: preview, description, where the mod is read (stages, cutscenes, menus), files the game never opens,
  file and texture conflicts won and lost, voice lines it replaces, Reloaded features missing on this build, region
  notes (NTSC-U stage layouts, fonts), problems. Option drop-downs above it.
- **Install** by dropping archives or folders anywhere on the window, Install... (Ctrl+O) or the context menu; a
  marquee bar runs while installing. Installing a download that is installed already asks before replacing it (its
  place, on/off state and options are kept).
- **Profiles** button: save the current set (on/off, order, options) under a name, load or delete one
  (`mods/.profiles/`).
- **Edit info** (Ctrl+E): name, version, author, website, description, written to the mod's `mod.json`.
- Switching off a mod that others need offers to switch them off too; switching on one that needs others offers to
  switch those on.
- "Load mods" master switch, Refresh (F5), Play (Ctrl+P), Remove (Del, to the Recycle Bin).
- Failed saves are reported (after ~0.6 s of retries); a damaged `mods.json` answered No makes the window read only.
- The window's size and place are remembered in `mods/.manager.json` (ignored when that screen is gone).
- Bottom bar, right: Zoom (also Ctrl and + / -, Ctrl and 0, Ctrl and the mouse wheel) and Theme, used at once
  and kept in `appearance.cfg` beside the manager.
- `--snapshot <png> [--select <id>]` writes a picture of the window (documentation).

Command line (`ShadowModManager.exe help`): `list`, `info`, `install [--replace] [--name]`, `enable`, `disable`,
`enable-all`, `disable-all`, `remove`, `move <id> <pos>`, `option <id> <option> <choice>`, `master on|off`,
`profile list|save|load|delete [<name>]`, `set-info <id> <field> <value>`, `check` (exit 1 when an enabled mod
cannot load), `plan` (JSON), `validate <folder>`; `--mods-dir`, `--game-data`.

Game: `--mods-dir <dir>`, `--no-mods`. The console prints `Mods: N active ...` and one line per mod; problems go to
stderr as `[mods] warning|error: ...`.
