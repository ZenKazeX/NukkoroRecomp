# Save / load reroute - proposal (2026-11-05)

The user asked for this together with the Options overhaul (docs/OPTIONS_MENU_OVERHAUL.md): "we're gonna have to
reroute the save load system as well". This document is the map and the proposal. Nothing here is implemented
until the user approves the scope.

**Decisions (the user, 2026-11-05):**
- A: the native save store at the card job runner.
- B: a GCI-compatible file.
- C: the game's options live in the PC settings file.
- D: automatic one-time import of the newest valid save.

Evidence: static disassembly and code reading (2026-11-05 research). [V] = verified that way, [I] = inferred. No
save or load has been run against these findings yet.

## 1. How the game saves today

The game saves in three layers above the memory card. [V]

1. **The screens.** `SaveCycle::CycleTask` (0x802E4314) runs one phase per kind:
   - kind 3 `Check::Phase`: the boot check / create / format / delete, from `AdvReal::AdvFolder`;
   - kind 1 `Load::Phase`: the Options "Load" item;
   - kind 2 `Save::Phase`: leaving Options with changes (Options state 5), the stage-clear autosave
     (`StageAction::SaveAction` 0x802E61D8), and two other callers not yet identified;
   - `MessageDrawTask` draws the messages.
2. **The save manager.** `TMemoryCardCtrl` (vtable 0x805539E0) takes the requests create / load / save / check /
   format / delete.
   - It builds the file: comments, the 96x32 banner, the icon and the game record.
   - It protects the record with an MD5 and DES-ECB.
   - It reports the result codes the screens show, for example "not the original card" (message 125).
3. **The card job runner** (0x8029A66C-0x8029C2A4). Seven jobs made of 14 card operations, each an issue / poll
   pair through CARDMountAsync, CARDCheckAsync, CARDOpen, CARDCreateAsync, CARDReadAsync, CARDWriteAsync,
   CARDSetStatusAsync and the rest.

**The file:** "SHADOWTHEHEDGEHOG", 2 blocks, 16 KiB, GCI-compatible.

| Offset | Content |
|---|---|
| 0x0000 | comments |
| 0x0040 | banner |
| 0x1840 | icon |
| 0x2040 | the 0x1F5C-byte record |

**The record** is copied from GameData 0x80577A18:

| Record offset | Content |
|---|---|
| +0x18 | text language |
| +0x1C | voice language |
| +0x20 | speaker mode (stereo / mono / Pro Logic II) |
| +0x24 | vibration |
| +0xB0 | 59 stage records |
| +0x1774 | save stamp |

**The PC port today:** the guest's whole CARD file system still runs. `native_card` emulates the card as a block
device under it: a 512 KiB-16 MiB `.raw` image, or a `.gci` with a `.pc.raw` working image, persisted after 250 ms
of quiet.
- Host write failures are never reported to the game (docs/SAVEGAME_BUGS.md S7).
- The language and the speaker mode also live in the emulated console SRAM (`SRAM.pc.raw`). The game reads them
  at boot before any save is loaded, and writes the speaker mode back.

## 2. Proposal

### A. The boundary: replace the card job runner with a native save store (recommended)

- **Native:** the seven card jobs, the runner's tick and its status getters become one native `SaveStore` that
  reads and writes one host file.
  - CARDInit and CARDGetResultCode are owned too: the reset handler polls the latter.
  - The asynchronous protocol is kept: a job answers "busy" for at least one game tick and then its result, so
    every screen sees the timing shape it expects.
- **Kept, unchanged:** every save screen and message, the save manager (MD5, DES, banner, icon, comments), the
  autosave, and the "not the original card" stamp check.
- **Removed from the save path:** the emulated memory card, the guest CARD file system, the 16 MiB card images and
  the flash-ID binding.
- **Fixed:** a host error becomes the game's own error code. For example, disk full shows the game's "not enough
  space" message instead of a silent failure (S7).
- **Writes:** atomic (a temporary file, then rename), and finished before the job reports success. A crash never
  leaves half a save.

Other boundaries were considered and not recommended:
- **The CARD API itself (~16 entry points).** It has to emulate CARD file semantics (status, file numbers, broken
  or encoding states) for no gain.
- **The manager level.** It is the most readable, but it must re-create the manager's state fields and loses the
  GCI round trip.
- **Options only.** It leaves three sources of truth (record, SRAM, file) and keeps the card.

### B. The file

- One file, `save-data/ShadowTheHedgehog.gci` by default (`--save-file <path>` overrides; the decision below may
  change the format).
- **GCI-compatible:** the standard 64-byte header plus the 16 KiB data. Dolphin and GameCube save managers can
  import and export it.
- **Or native:** the game's 16 KiB data alone (`.sav`). Simpler, but not exchangeable with Dolphin.

### C. Where the options live

The game-owned options are text language, voice language, speaker mode and vibration. They are stored twice today
(the save record and SRAM), and the Options menu's own settings live in display.cfg / options.cfg. Proposal:
- **One source of truth: the PC settings files.**
  - At boot and after every load, the native layer applies them over the values the game read from the save or
    SRAM.
  - When the game saves, the record still gets them, so the save stays valid in Dolphin.
- **Options no longer trigger a 16 KiB save.** The original rewrote the whole save every time the player left
  Options, changed or not (state 5, when G + 0x7D is set; docs/OPTIONS_MENU_ORIGINAL_RE.md). On PC the settings
  file is written instead, and leaving Options skips the card save. Progress saves are unchanged.
- **SRAM stops holding settings.** The IPL language and sound mode the game reads at boot come from the settings.

### D. One-time import (never modifies the originals)

1. On first start without a save file, look for the newest valid save, read-only, in this order:
   - the current working card image `save-data/8P-GUPP-SHADOWTHEHEDGEHOG.pc.raw`;
   - the GCI under `GC/EUR/Card A/`;
   - `--import-save <path>`.
2. Check each candidate's record (MD5 after DES), and prefer the newer save stamp.
3. Write the new file atomically, plus a `.source` note saying where it came from.
4. Keep the card images and SRAM as they are. They are no longer used for saving.

The dated snapshots in save-data/ are only used if the user asks.

### E. Replays, tools and tests

- Recordings keep working:
  - a new recording stores the save file in `initial/`;
  - an old recording's `card.raw` is imported, read-only, into the replay's own folder.
- `--memory-card` stays for one release as an import source. Tools and docs that pass a card are updated.
- **Validation:**
  - Westopolis replay identity (state, audio, pixels) against the card build: the save path must not change
    gameplay;
  - a boot with no file (create), a load from Options, a stage-clear autosave, a read-only folder (the error
    message), disk full (simulated), and a Dolphin round trip of the GCI.

## 3. Scope and order

1. SaveStore + the job runner owned natively + atomic writes + error codes.
2. Import + `--save-file` + recorder / tool updates.
3. Options source of truth (C) together with the Options overhaul's settings files.
4. Validation runs, then docs (SAVEGAME_BUGS S7 closed, GC_REMOVAL_PLAN updated).

Not in scope unless asked: several save slots, cloud saves, a readable (decrypted) save format.
