# Testing the mod loader and the manager

What each test proves, how to run it, and what it cannot prove. A test result is one of PASS, FAIL, UNSTABLE (its
control did not hold, so the comparison means nothing) or SKIP (an input is missing). SKIP and UNSTABLE are never
reported as passes.

## 1. Unit tests (no game data)

| Test | Covers |
|---|---|
| `mod_loader_tests` (139 checks) | JSON, versions, manifests, state file (atomic, damaged, newer schema), discovery, dependency / incompatibility / cycle resolution, priority plan, options, AFS rebuild (an unchanged rebuild is byte-identical to the original), zip reader (stored, deflate, traversal, drive paths, checksum, truncation), installer placement rules, saves and programs refused / left out, format checks (movies in the disc's and sfdmux's layout, cut movies), code items (an ini shipped twice offered once), `prepare_runtime` end to end |
| `texture_packs_tests` | XXH64 vectors, Dolphin names, palette ranges, DDS/PNG decoding, priority |
| `save_text_region_tests <PAL files> [<more files dirs>]` | the PC save wording lands on the right messages in PAL, NTSC-U and mod tables (SKIP without data) |

Build: `cmake --build .build/watch --config Release --target mod_loader_tests save_text_region_tests` (defined in
every tree; registered with CTest when `SHADOW_BUILD_TESTS=ON`).

## 2. End-to-end scenarios: `tools/mods/modtest.py run`

```
python -I tools/mods/modtest.py run --exe <game.exe> --manager <ModManager.exe> --game <game data> \
    --downloads <dir with gb_dl/ and dl/> --work <scratch dir> [--only a,b] [--report out.json]
```

Each scenario (`tools/mods/modtest_scenarios.json`) installs real downloads with the real manager (command line),
runs the real game headless on the Westopolis input tape, and checks:

- **exit code** and **log lines** (regular expressions over stdout + stderr; the game's CRLF is normalised).
- **manager plan == game applied**: the manager's `plan` (JSON) against the game's `--mods-report` (what the loader
  actually applied at mount). Two programs, one library, compared after the fact.
- **read proof**: every guest file read in `--asset-trace` carries the runtime's hash of the bytes the game received.
  The harness hashes the same byte range of the file it expects and compares:
  - a modded path must carry the mod's bytes (`must_read` also requires at least one such read);
  - every other path must carry the disc's bytes;
  - a path the port itself rewrites at mount (the PC save/menu wording, sharpened button art) is identified by the
    stock run reading something other than the disc; its reads must equal the stock run's reads. These are counted
    separately (`*_port_transformed`), never folded into the proven count.
  - the switchable voice archive (served under `PRS_VOICE_E.afs` when the player's `PRS_VOICE_J.afs` is found) is
    rebuilt in the harness exactly as the runtime composes it (`ComposedVoices`, after `VoiceArchive::open`) from the
    English file the game was given (the disc's, or the mod's / its rebuilt archive) and the Japanese file; each read
    must equal that composition byte for byte (`*_match_composed_voices`). Until 2026-11-14 these reads were only
    compared with the stock run. Discrimination: of the 47 reads in `reloaded-voices-audio`, 47 equal the composition
    with the mod's rebuilt archive, 9 the composition with the disc's.
- **scenario options added 2026-11-14**: `boot` (replaces `--boot-stage`, e.g. the front end's intro or a story movie),
  `tape: false`, `code` + `prepare_variant` (code choices with the manager, `build_variant.py --prepare-only`),
  `vs` (frame/state/pcm against another scenario of the same pass), `installed_mods` (folders in `mods/`),
  `game_unchanged` (SHA-256 of every player file in the game folder outside `files/` and `sys/` - saves, cards,
  settings - plus size and time of executables and disc files, before setup and after the run).
- **winner**: for a conflicting path, the applied host file belongs to the expected mod.
- **picture / state / audio vs stock**: SHA-256 of the capture at a fixed tick (`SHADOW_R2_MSAA=1`), of the
  end-of-run state report, of the audio dump. The stock run is made **twice**; if the two stock runs differ, the
  comparison is UNSTABLE.
- **negative controls**: scenarios with `expect_fail` state something false on purpose (Cream's frame equals stock;
  the lower mod wins; a texture pack changes game state). They pass only when the harness reports that check as
  FAIL. They show the checks can fail.

The runtime's FNV-1a uses the offset basis `1469598103934665603` instead of the standard `14695981039346656037`
(the constant lacks its last digit, in ten places of the runtime). Hashes still separate data, but only the runtime's
basis reproduces them; the harness uses it. It is not changed in the runtime because one use (the memory-card marker)
is persisted.

### Results, 2026-11-14 (game r3-f8e66a7e, manager 52e3068e, one pass)

16 scenarios, 16 PASS; the three negative controls failed as intended. Stock controls: the two stock runs gave the
same frame, state and reads (Westopolis capture at tick 1.2e9) and the same audio (90 s).

Read proof (reads byte-exact with the mod's file / with the disc / counted apart as port-rewritten):

| Scenario | modded exact | modded port-rewritten | others = disc | others = stock (port-rewritten) | mismatches |
|---|---|---|---|---|---|
| cream-model | 332 | 0 | 1,799 | 30 | 0 |
| conflict-tails-above-cream | 337 | 0 | 1,799 | 30 | 0 |
| eight-mods | 419 | 3 | 1,739 | 25 | 0 |
| reloaded-data | 1,206 | 30 | 924 | 4 | 0 |
| reloaded-voices-audio | 0 | 47 | 2,290 | 6 | 0 |
| texture-pack-android, failure paths, switches | 0 | 0 | 2,125 | 30 | 0 |

The voice scenario is **not** proven by the read check: with the player's Japanese voices present, the port serves
`PRS_VOICE_E.afs` through its switchable two-language archive, whose layout differs from any single file, so all 47
reads fall in the port-rewritten column. Only its audio-equals-stock check stands for it.

| Scenario | Proves |
|---|---|
| cream-model | a loose `shadow.one` from a .rar is what the game reads; picture differs |
| conflict-tails-above-cream / conflict-cream-above-tails | the higher mod's bytes are read, both orders |
| texture-pack-android | picture differs, **state equals stock** (texture packs do not touch game state) |
| eight-mods | 8 real mods, 535 files, every read accounted for |
| reloaded-data | Reloaded's data runs on the original code, every read accounted for; its DOL reported |
| reloaded-voices-audio | 37 voice entries rebuilt into the archive; 90 s audio equals stock (moved entries read correctly) |
| installer-refuses-damaged | the installer refuses a truncated download and names the file |
| truncated-file-skipped | a file damaged after install is skipped at start-up; play equals stock |
| missing-dependency | the dependent mod is left out with the reason; the other mod loads |
| damaged-state-file | a damaged `mods.json`: stock play, error printed |
| master-switch-off / no-mods-flag | stock play |
| stage-chaotic-westopolis | GameBanana "Stages": 40 files (layouts, stage archives, mission table, models); 611 reads byte-exact from the mod; more Black Arms on screen and the 100-kill hero mission counter at the capture |
| weapon-elephant-tank-cannon | GameBanana "Weapons": `common/WeaponResource.one`, 199 reads byte-exact from the mod. The picture equals stock at the capture tick: no tank cannon is on screen there, so the elephant model itself was not seen |
| control-* (3) | the frame, winner and state checks fail when they should |

## 3. Mods-off regression gate: `tools/mods/modtest.py gate`

Base vs candidate executables, mods off: Westopolis 316 s and Final Haunt 120 s (state report + audio dump), then
`tools/behaviour_run.sh` + `behaviour_diff.py --exact`. The base runs twice first; a gate whose base disagrees with
itself reports UNSTABLE. A part the harness cannot produce (e.g. no behaviour trace) is ERROR, with the tool's output,
never FAIL or PASS. `--parts runs|behaviour` reruns one part and merges it into the same report.

Result 2026-11-14 (base 42f71847 vs candidate f8e66a7e): Westopolis pcm/state 19fe683a6e87bbff / 8d30c24616360278 on
base, base repeat and candidate; Final Haunt a886ae3c0aadfca0 / e075d9a8d1a5482c on all three; behaviour --exact
PASS. (These are SHA-256 prefixes; the older gate.sh printed MD5 prefixes, hence other numbers for the same files.)

A harness bug found on the way: the first gate run printed "behaviour FAIL" because a bare `bash` from Windows
Python started System32's WSL launcher, so no trace was made. The gate now names the shell and reports such a case
as ERROR.

## 4. The window: `tools/mods/gui_test.ps1`

Drives the real manager window from outside through UI Automation, real OLE drag-and-drop, real mouse clicks, drags
and key presses, and reads every result back from disk and from the window (docs/mods/GUI_TEST.md). It uses the mouse
and keyboard for about 4 minutes: leave the machine alone while it runs.

Result 2026-11-14, manager 4901b6de: 31 passed, 0 failed, 0 skipped, two runs in a row (steps 01-18: install by drop
and dialog, ticks, order, conflicts, Load mods, options, Refresh, Remove, bad downloads, Play without the game,
damaged mods.json No/Yes, a locked mods.json, close; steps 19-30: context menu, drag to reorder, filter, keyboard,
edit info, profiles, the update prompt, dependency switch-off/on, status colours (pixel sampled), the progress bar,
remembered window placement).

Bugs the window test found, all fixed and each verified by a step: a failed save was silent (finding 1); edits were
accepted while mods.json was damaged (2); no accessible names on the list and details (3); "1 files" (4); drag to
reorder dropped nothing (WM_CAPTURECHANGED cancelled the drag, step 21); the update box did not name the installed
mod (step 26). Two failures were the test's own: a broken Windows path in the script and a race reading mod.json
before the manager had written it (step 24, now waits for the write).

## 5. What none of this proves

- That one of **Reloaded's 37** replaced voice lines is heard: no replay reaches one (a control with a stock line was
  proven: docs/mods/map/VOICE_LINES.md). A replaced line *is* heard with the Japanese voices mod, which replaces all
  4,480 entries (`voices-japanese-xbox`: every voice-archive read carries the mod's bytes, the PCM differs from stock).
- **Movies** beyond the first seconds (one capture per movie scenario), movie audio of a modded movie, and how
  a "wider" movie should be shown (the port draws every movie in a 4:3 box, so Wider FMV's extra picture is squeezed).
- Gameplay **beyond the tape** (one Westopolis route, one capture tick, 90 s of audio).
- **Windowed** presentation (all runs are headless; the renderer is the same, the present path is not).
- Front-end menus with mods (no scenario drives the menus).

## Recheck after code mods (2026-11-14, game 5c570697, manager 2b7dc5a4)

- `modtest.py run`: 18/18 PASS, including the stage mod (Chaotic Westopolis: plan == applied, 611 reads carry the
  mod's bytes, 1,598 other reads the disc's, frame and state differ from stock) and the weapon mod; the 3 negative
  controls fail as intended; both stock runs identical. `reloaded-data` now checks the code-mod contract (the mod
  loads with 0 data codes; no `[codes]` line and no warning about its main.dol until a code is chosen) instead of
  the old "main.dol not supported" warning.
- `gui_test.ps1`: 31/31 steps, plus the new step 31 (`-CodeMod <mod folder with codes>`): the Code... window lists
  the same 35 items as the CLI, ticking saves the choice and shows its signature, a report is summarised, unticking
  clears it. 13/13 checks.
- Gate (mods off): PASS (README of codemods/). Unit: mod_loader_tests 131/131, code_engine_tests, test_patchset.py,
  test_build_variant.py 10/10, save_text_region_tests PASS on PAL / NTSC-U / Reloaded tables.

## Mod types (2026-11-14, game types-69ce3cab, manager 3a21cb51)

Every kind of download GameBanana lists for the game, tested with real downloads (scratch only; nothing of them in the
repo) through the harness. `modtest.py run`: **31/31 PASS** (18 earlier scenarios + 13 new); the 3 negative controls
fail as intended; all 7 stock pairs identical (frames, states, PCM, reads). Stock controls of the new boots: the front
end's intro (`--ui-test select --tv-format ntsc`, tick 810M), the story movie (`--boot-flashback --tv-format ntsc`,
tick 486M), Westopolis at ticks 400M and 810M, 90 s of audio with `options.cfg voice=0`.

| Type | Download (GameBanana id) | What the loader / manager did | Evidence (scenario) |
|---|---|---|---|
| Movies | Re-edited Intro (649907), Early Demo Intro (539016) | **refused as damaged before the fix** (sfdmux layout); now installed as `files/Op.sfd` | the front end plays the mod's intro: 4 reads byte-exact from the mod, frame differs from stock and between the two mods (`movie-op-*`) |
| Movies | Wider FMV's (659865), all 8 movies | installed (8 files, 412 MB) | `Op.sfd` 10 reads, `E1001.sfd` 55 reads byte-exact from the mod; frames differ (`movie-wider-fmv-*`). The port draws every movie in a 4:3 box, so the extra width is squeezed, not shown wider |
| Effects | No Weapon Outline, original (551497) | `files/stg_cmn_gdt.one` | 282 reads byte-exact from the mod; at the capture the green glow under the weapon on the ground is gone (`effects-no-weapon-outline`); the end state also differs from stock |
| Voices (whole language) | Japanese Voices (XBOX) (624824) | `PRS_VOICE_E.afs`, 78 `sng_EV*_E.adx`, 3 `sng_jin_*_e.adx` installed; 7 Xbox 640x480 movies and 3 Xbox `.pac` banks left out, named | its `PRS_VOICE_E.afs` is byte-identical to the Japanese disc's `PRS_VOICE_J.afs` (all 4,480 entries; its 78 `sng_EV*` equal the Japanese disc's `_J` files). English setting: all 47 voice-archive reads equal the port's composition of the mod's archive with the player's Japanese archive; 90 s PCM differs from stock (`voices-japanese-xbox`). Japanese setting (`voice=0`): PCM **equals** stock with the same setting (`voices-japanese-xbox-japanese-setting`): the mod changes nothing audible there. With the English setting the dialogue is Japanese but the in-game character banks stay the PAL English ones (the mod's banks are Xbox `.pac`) - its PCM differs from stock-Japanese |
| Voices, movies of another version | same download's 640x480 movies | **played scrambled before the fix** (decoded into the game's 512x480 picture, no fault); now left out at install (and skipped at start-up if placed by hand) | `--boot-flashback` then plays the disc's E1001: frame and PCM equal stock; the installer's line naming E1001 is checked (`movie-japanese-xbox-story`) |
| Text / fonts | Shadow the Hedgehog Retranslated v2 (686724), 1,247 font files (NTSC-U layout) | installed; region notes listed (NTSC-U tables, PAL wording replaced) | `fonts/stg0100/stg0100_EN.fnt` read byte-exact from the mod; at tick 400M Shadow's first subtitle shows the mod's wording and nothing else on screen differs (`text-retranslated`). `save_text_region_tests` on the mod's `Save_EN.fnt` (162 messages, one fewer than PAL): PASS - the PC save wording lands on the right keys |
| Save | 100% Save File (Reloaded) (652243), one `.gci` | **refused**: "this download is a save, not a mod: 8P-GUPR-SHADOWRELOADED.gci is a GameCube save (game GUPR, maker 8P, file "SHADOWRELOADED")... it is not this game's own save file ... cannot be used here"; for this game's own save the message says how to play from a copy (`--save-file <copy>`) | nothing installed; the game folder (3,644 files: SHA-256 of every file outside files/ and sys/, size and time of the executables and disc files) before setup and after the run: unchanged; play equals stock (`save-file-refused`) |
| Program | ShadowRando (497286), a .NET randomizer (`.exe` + `.dll`) | **refused**, named: "this download is a program (.../ShadowRando.exe), not a mod: the manager never installs or runs programs..." | nothing installed, nothing run (the harness never executes downloads), game folder unchanged, play equals stock (`program-refused`) |
| Code (Gecko) | Reloaded's "No HUD" | chosen with `ShadowModManager code`, `build_variant.py --prepare-only`: 0 patches to build in, 1 engine code, 0 skipped | `[codes] shdth-reloaded:gecko:No HUD#No HUD: runs`, 1 data code per retrace, stock executable; frame differs from the same data without the code (HUD gone, scene the same; `code-no-hud-engine` vs `reloaded-data-hud-tick`) |
| Code, multiplayer | 2P-ShdTH (310633) / 2P-Reloaded (618179) | installed as a texture pack (16 Dolphin textures) + codes; 4 xdelta image patches reported (not applied), its saves and memory-card images left out; 66 code items (the Dolphin and Nintendont copies of one ini were listed twice before the fix) | not run in game. `--prepare-only` on Vertical SplitScreen (Required) + Target Other Player: 2 features applied (1 patch to build in, 3 engine codes); the multiplayer mode itself is in the xdelta patches, which this build does not apply |

Bugs found and fixed (all with unit tests; 141/141): movies muxed by sfdmux refused as damaged (movie check now walks
the stream); Xbox 640x480 movies installed and shown scrambled (now left out / skipped, named); saves and programs got
only "nothing in this download is content the game can load" (now named, with what to do); programs inside a mod
that has its own mod.json were installed (now removed); a mod shipping the same game ini twice offered every code twice
under one id (one choice switched on both copies; now offered once, a reused name in a different ini gets its own id);
`build_variant.py` reported the whole ini's unresolved lines against every chosen code (now only the code's own).

Gate (mods off, `modtest.py gate`, base 42f71847 vs types-69ce3cab): Westopolis 19fe683a6e87bbff / 8d30c24616360278 and
Final Haunt a886ae3c0aadfca0 / e075d9a8d1a5482c on base, repeat and candidate; behaviour --exact PASS.

Harness: the switchable voice archive is now proven byte for byte (composition model, section 2); new scenario options
`boot`, `tape`, `options_cfg`, `code`, `prepare_variant`, `vs`, `installed_mods`, `game_unchanged`,
`install_must_say`.

Found on the way, not a mod problem: `--movie-trace` stops every boot that plays a movie at `OSResumeThread(0x805ACD58)`
(CRI start-up, "single-thread configuration"); without it movies play headless. Same in base 42f71847 and the deployed
executable.
