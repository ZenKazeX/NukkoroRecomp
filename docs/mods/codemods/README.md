# Code mods (2026-11-14)

Gecko / Action Replay codes, Melee Code Manager (MCM) libraries and replacement `main.dol` files, written for the
NTSC-U disc (GUPE8P / Reloaded's GUPR8P), applied to this PAL build. Opt-in: nothing runs unless the player chooses a
code in the manager, and with no code chosen the game is bit-identical to a build without this support (gate below).

Detail documents: TRANSLATION.md (NTSC-U -> PAL patch sets), RECOMPILE.md (recompiling a patched PAL DOL),
ENGINE.md (the in-game engine for data codes).

## How it works

1. **Choose.** The manager lists what each mod offers as code (`list_code_items`): its replacement `main.dol`, MCM
   libraries, each named code of a Dolphin game ini, plain code text files. *Code mods...* (window) or
   `ShadowModManager code <mod> <item> on|off` (CLI) saves the choice in `mods.json`. The chosen items of the enabled
   mods, in load order, with the bytes of their files, give the *selection signature*.
2. **Build.** *Build...* runs `tools/mods/codemods/build_variant.py` for that signature:
   - `patchset.py` translates each item to PAL addresses (address map + byte signatures; caves relocated into
     0x800034AC-0x800053E0; the mod's own memory outside the DOL image relocated into 0x817C0000-0x817F0000, one
     shared map per build). A replacement `main.dol` that ships with its MCM libraries is taken apart into its named
     entries (features); what the DOL changes beyond them is not applied.
   - Every patch is classified against what this build is made of (`classify_sites.py`): translated code, data,
     native or lifted owners (no effect), lowered sites (the recompiler refuses a different word), functions the
     runtime enters its own way (not applied without `--apply-named`), pruned code.
   - **A feature applies whole or not at all.** One patch that cannot work drops its whole feature, with the reason.
     Further rules that drop a feature: injected code jumping into the middle of code this build replaced; a read of
     relocated memory that no applied code writes (a game heap object the NTSC-U heap layout put at that address,
     e.g. Reloaded's pointer at 0x807D71A0 - its PAL address is not known); an engine code that cannot be written.
   - Code and static data go into a copy of the PAL DOL, recompiled into
     `mods/.variants/ShadowTheHedgehog-code-<signature>.exe` (about 6 minutes; the executable embeds game code: keep
     it on this machine). Writes to memory the DOL does not hold (bss, heap) and conditional codes become PAL Gecko
     codes for the engine (`engine_codes` in `mods/.variants/<signature>.json`). A selection with only engine codes
     needs no build: the stock executable runs it.
   - The report `mods/.variants/<signature>.json` lists every applied feature, every skipped patch with its reason,
     and the engine codes.
3. **Play.** The manager starts the variant built for the current signature (or the stock executable when nothing
   needed building). At start-up the game reads the engine codes of the chosen signature (`prepare_runtime`) and runs
   them once per retrace. A code whose feature also changes game code runs only in the executable built for that
   signature (`SHADOW_CODE_VARIANT`); a variant started with another selection says so on stderr.

## What is supported, and what is not

| Kind | State |
|---|---|
| Gecko 00/02/04/06/08 writes, 20-2E conditions, E0/E2/F0, 40-4C base/pointer, 80-8C registers | engine (ENGINE.md); 40-4C/80-8C and 08 layouts are from public docs, unit-tested, not console-checked |
| Gecko C2/C0/D2 (code insertion, execute) and writes into code | recompiled variant |
| MCM static overwrites and injection (branch hook + cave) | recompiled variant, per entry |
| Replacement `main.dol` | through its MCM libraries when shipped; alone, its whole difference as one feature |
| Patches in native or lifted owners, lowered sites, pruned code | skipped with the reason (the original code is not what runs there) |
| Code that reads NTSC-U heap objects at fixed addresses | skipped (the PAL heap is laid out differently) |
| Whole-image xdelta/ISO patches | not supported (install the patched image's files instead) |

## Evidence (all headless `--no-audio`, Westopolis tape `--boot-stage stg0100`, captures 1280x720 MSAA off)

| Run | Result |
|---|---|
| Unit tests | `mod_loader_tests` 131/131 (incl. engine codes read for the chosen signature, nothing when not built / nothing chosen, damaged code reported); `code_engine_tests` pass; `test_patchset.py` OK; `test_build_variant.py` 10/10 (Gecko remapping, store/load scan, write lines) |
| No HUD chosen in the manager (Reloaded's Gecko ini), stock executable, tick 810M | `[codes] ... No HUD: runs`; HUD gone, scene identical to the same mods without the code (fdc9bbbba011 -> 0245693c6f17); engine 1196 frames, 1196 writes |
| No mods, same tick | 0813d7fd6208 (= the engine agent's stock base) |
| Mods-off gate (`modtest.py gate`, base 42f71847 vs this build 5c570697) | Westopolis 19fe683a6e87bbff / 8d30c24616360278, Final Haunt a886ae3c0aadfca0 / e075d9a8d1a5482c on base, repeat and candidate; behaviour --exact PASS |
| ShdTH-Reloaded `main.dol` chosen (taken apart through its 4 MCM libraries: 106 entries enabled in its DOL) | 93 features applied (224 patches, 57 injected routines), 13 skipped whole (below); variant built in 365 s |
| Reloaded variant vs Reloaded data only, tick 1.2e9 | data only: Sonic's partner intro cutscene (1a14d535740e); variant: no intro, Sonic already beside Shadow in play, Reloaded's mission counter on the HUD (aeb669dd1e0c) = Disable Partner Intro and the HUD features at work; no fault |

Before the heap-object rule, the same variant stopped at tick 64M: Melee Weapon Core Revamp loaded a pointer from the
relocated (empty) copy of NTSC 0x807D71A0 and then read through it (null load at cave 0x80003D9C).

Reloaded features skipped whole in this build, with the first reason:

| Feature | Why |
|---|---|
| Advanced Full Widescreen v5 | a lowered site: the recompiler refuses a different word there (the port has its own widescreen) |
| Circuit Speed Control v2, Hold L to JumpDash v5, Holdable Slide v2, LightDash Transition Expansion, Melee Weapon Core Revamp v3 | read the NTSC-U heap object at 0x807D71A0 (PAL address not known) |
| IGT Message Render | reads relocated NTSC 0x8060B33F, which nothing applied writes |
| SX - Dont stop time in checkpoint, SX - Only Increment New IGT if in Normal Level | hook 0x80337DD4, which the native frame limiter tail-calls (`--apply-named` to try; stopped the game before) |
| Slide Run Threshhold, Strafe Walk Speed | hook 0x800841E0, entered by the runtime its own way (same) |
| Light Dash ValidRing Distance, Spindash Startup Delay | translation confidence low |

## Open

- The PAL counterparts of the NTSC-U heap objects Reloaded reads (0x807D71A0, 0x8060B33F) would bring back 6 features:
  find the game global that holds the same object in PAL and map to it instead of relocating.
- Hooks in runtime-named functions (4 features) need each function's native entry checked before they can be applied.
- Engine code types 40-4C/80-8C and pad-conditioned codes (Moonjump, Fill Meter) are unit-tested, not run in game.
- The variant run above covers the first 1.2e9 ticks of one stage; the features' gameplay changes were not each played.
