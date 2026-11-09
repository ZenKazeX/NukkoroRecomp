# Save screen without a memory card: options (2026-10-27)

The user chose option A on 2026-10-27; it is implemented as a native replacement of the English message table
(see the note under option A). The other options are kept for reference.

## What the game shows today

The messages live in `files/FONTS/SAVE/Save_<lang>.fnt` (u32 LE count, 20-byte entries, UTF-16LE text). Each
language's glyph atlas is `Save_<lang>.txd`, and `Save_<lang>00.met` lists its glyphs. The table holds the PS2, Xbox
and GameCube sets; this PAL GameCube build uses ids 80-126. The English GameCube text, grouped:

| Group | Ids | English text today (line breaks removed) |
|---|---|---|
| Checking / loading | 108, 109, 110 | "Loading from the Memory Card in Slot A. Do not touch the Memory Card in Slot A or the POWER Button." / "Loading successful." / "Load failed. Please check the Memory Card in Slot A and try again." |
| No card | 81 = 94, 90 | "No Memory Card found in Memory Card Slot A. If you continue, Autosave will be disabled. To save Game Data at any time please insert a Memory Card into Slot A." / "No Memory Card found in Slot A." |
| Card full | 88, 91 | "There is insufficient space to save new Game Data on the Memory Card in Slot A. SHADOW THE HEDGEHOG(TM) requires 1 file and 2 blocks to save. To manage the contents of your Memory Card, use the Memory Card Screen." (91 adds "Do you want to continue the game?") |
| Saving | 80, 106, 107 | "Saving. Do not touch the Memory Card in Slot A or the POWER Button." / "Saving to the Memory Card in Slot A. ..." / "Save successful. Press the A Button to continue." |
| Do not remove (autosave notice) | 97 | "SHADOW THE HEDGEHOG(TM) uses an Autosave feature. If you wish to save Game Data you must have a Memory Card inserted into Slot A. If you remove the Memory Card at any point, Autosave will be disabled. To re-enable Autosave you must re-insert a Memory Card into Slot A." |
| Format | 85 / 99, 86 / 100, 87, 103, 104, 105, button 113 | "...formatted for another market. Do you want to format?" / "...is corrupt and must be formatted." / "Formatting the Memory Card in Slot A will erase all previously saved Game Data..." / "Formatting. Do not touch..." / "Format successful." / "Format failed. ..." / "Format Memory Card" |
| Other card errors | 82 / 96, 84 / 98, 125 | "The device in Slot A is not supported." / "The Memory Card in Slot A is damaged and cannot be used." / "Memory Card inserted in Slot A is not the original Memory Card. ..." |

After this session's fixes the PC always has a save store. A missing store is created empty, and a lone GCI is
placed on a new card. So "no card", "not supported", "other market" and "must be formatted" only happen if the save
file is damaged or cannot be written.

The English atlas has only these 75 glyphs:
`` !"&'(),-./12345678<>?ABCDEFGHIKLMNOPQRSTVWXYabcdefghiklmnopqrstuvwxyz®'♪ ? `` (no J, U, Z, j, 0, 9, colon).
Text that uses only these glyphs needs no new font art; text with any other character needs the atlas rebuilt for
every language. All options below need FR, GE, IT, SP and JP translations as well.

## Option A - same screens, PC words (smallest change)

Layout, flow, buttons and timing stay as they are. Only the text changes, so every English line below fits the
existing atlas. Messages for situations a PC cannot have get the nearest PC meaning.

| Message | Ids | New English text (`/` = line break) |
|---|---|---|
| Checking / loading | 108 | Loading Game Data. / Do not quit the game or / turn off your PC. |
| Load failed | 110 | Load failed. / Please check the Save Data / and try again. |
| No card | 81, 94 | The Save Data could not be created. / If you continue, Autosave / will be disabled. |
| No card (short) | 90 | No Save Data was found. |
| Card full | 88 | There is not enough disk space / to save Game Data. / Please free some disk space / and try again. |
| Card full + continue | 91 | (88's text) / Do you want to continue the game? |
| Saving | 80, 106 | Saving. / Do not quit the game or / turn off your PC. |
| Do not remove | 97 | SHADOW THE HEDGEHOG& uses an / Autosave feature. Do not quit the / game or turn off your PC while / the game is saving. |
| Format: confirm | 87 | Creating new Save Data will erase / all saved progress. / Do you want to continue? |
| Format: needed | 86 | The Save Data is damaged. / Do you want to create / new Save Data? |
| Format: running | 103 | Creating Save Data. / Do not quit the game or / turn off your PC. |
| Format: done / failed | 104, 105 | Save Data created. / Press the A Button to continue. -- The Save Data could not be created. |
| Format: button | 113 | Create New Save Data |
| Format: needed (statement) | 100 | The Save Data is damaged. / New Save Data must be created. |
| Damaged | 84, 98 | The Save Data is damaged / and cannot be used. |
| Not supported / other market | 82, 96, 99 | The Save Data is from another / version and cannot be used. |
| Other market (question) | 85 | The Save Data is from another / version. Do you want to create / new Save Data? |
| Deleting | 101 | Deleting Game Data. Do not quit / the game or turn off your PC. |
| No game data | 83, 93 | There is no Game Data. / Create Game Data? -- No Game Data was found. |
| Keep this card? | 92 | Do you want to continue the / game with this Save Data? |
| Game data corrupted | 95 | The Game Data is corrupted. / This Game Data needs to be deleted. |
| Not the original card | 125 | This Save Data belongs to another / game file. Do you want to / overwrite it? Progress will be / saved here automatically. |

**Implemented 2026-10-27** (`runtime/src/gekko/native_save_text.cpp`): English only. While implementing, the
table gained 83, 92, 93, 95 and 101, which also named the card, slot or POWER button. 85 keeps its yes/no
question, and 100 stays a statement as in the original. `--original-save-text` shows the game's own text.
The same messages also live in the front-end table `files/FONTS/ADVERTISE/Advertise_<lang>.fnt` (ids 530-576 in
every language; the boot's autosave notice is drawn from there). Both English tables are rewritten at mount, the
front-end one by matching the original text. Verified in game (headless cold boot, fresh card): the autosave notice
(97), "Create Game Data?" (83) and "Saving." (80) show the new text; before/after pair in
`.build/validation/save-text-before-after.png`.

## Option B - same screens, platform-neutral words (PC and Android)

Same as A, but the text never names a device, so the same table works for the Android port. It also fits the
existing atlas.

| Message | Ids | New English text |
|---|---|---|
| Checking / loading | 108 | Loading Game Data. / Please do not quit the game. |
| No card | 81, 94 | Game Data cannot be saved. / If you continue, Autosave / will be disabled. |
| Card full | 88, 91 | There is not enough free space / to save Game Data. (91: / Do you want to continue the game?) |
| Saving | 80, 106 | Saving. Please do not quit the game. |
| Do not remove | 97 | SHADOW THE HEDGEHOG& saves / automatically. Please do not quit / the game while it is saving. |
| Format: confirm / needed / running | 87, 86, 100, 103 | This will erase all saved progress. / Do you want to continue? -- The Game Data is damaged. / Create new Game Data? -- Creating Game Data. / Please do not quit the game. |
| Format: button | 113 | Create New Game Data |
| Damaged / not supported / not original | 84, 98, 82, 96, 85, 99, 125 | The Game Data is damaged / and cannot be used. -- (same) -- This Game Data belongs to / another game file. Overwrite it? |

## Option C - quiet autosave (modern PC behaviour)

Routine saving never stops play. The autosave notice (97) becomes one line on the title screen, "This game saves
automatically." Saving (80 / 106) is a small corner indicator, "Saving...", shown for at least one second, and the
"Save successful / Press the A Button" box (107) is dropped. Loading at boot shows nothing unless it fails. Only
errors stay as dialogs, with Option A's or B's text: card full, damaged data, could not be written.

This changes the game's flow: SaveCycle phases that waited for a button press no longer wait. Its timing must be
proven equal for gameplay. The corner indicator needs one new native UI element ("..." needs no new glyph). This is
more work than A or B and is a deliberate deviation from the original.

## Option D - a "Save Data" panel at boot (most work)

The boot card check becomes one panel: "SAVE DATA", the save's last-saved date and time, story progress, and the
buttons Continue / Start New Game / Delete. Saving during play works as in C. This needs a new screen, digits (0 and
9 are missing) and probably a colon, so the atlas or a native text renderer must be rebuilt. It also needs the save
contents decoded (progress, emblems) to show anything beyond a date.

## Not decided here

- Whether "A Button" stays in the text: the PC build maps keyboard and XInput too. A and B keep "A Button" because
  the controller prompts elsewhere in the game do too.
- How the text is delivered: new `Save_<lang>.fnt` (+ atlas) assets, or a native message table keyed by id.
- A host write failure (disk full, no permission) is never reported to the game today: the guest sees every save
  succeed. Options A-D can only show "not enough disk space" once the store reports that failure. This is a
  separate change (see the bug list).
