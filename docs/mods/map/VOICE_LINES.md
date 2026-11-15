# Voice lines: which AFS entry is which line, and proof that a replaced entry is heard

> In this repository `test_data/mods/voice_lines.json` is not kept: `tools/mods/map_voice_lines.py` makes it from your
> own disc.

Branch `mods/map-voice`. Data: `test_data/mods/voice_lines.json`. Script: `tools/mods/map_voice_lines.py`
(stdlib Python, `python -I tools/mods/map_voice_lines.py <game>/files out.json`). All numbers below were measured on the
PAL tree (`game/files`) with `final-47133fd7.exe`.

## 1. The map

### Archives
- `PRS_VOICE_E.afs` is the only voice AFS in the PAL tree (270,645,248 bytes). `AFS\0`, u32 LE count **4480**, TOC of
  `{offset, size}` LE pairs from +8, name table at the offset/size stored right after the TOC (48-byte records, 32-byte
  NUL-padded name then a date/size tail). Entry data are mono 24 kHz ADX files.
- `PRS_VOICE_J.afs` exists only on the Japanese disc (same 4480 entries, same indices). The PAL tree has none; the
  port ships the Japanese banks from the Japanese disc folder when the voice-language option is used
  (`runtime/include/gekko/voice_language.h`). The game picks the file by name at 0x801D844C
  (`docs/OPTIONS_MENU_ORIGINAL_RE.md`). Mods target the English archive: `afs/PRS_VOICE_E/<entry>.adx`.
- Other AFS-like archives a mod could target: none on the disc. `find` over the PAL tree, the USA tree and the Japanese
  tree shows only `PRS_VOICE_E.afs` / `PRS_VOICE_J.afs`. Cutscene voices and music are plain files
  (`sng_EV*_E.adx`, `sng_*.adx`, `*.sfd`), already reachable as `files/<name>`; they are not in the voice AFS.
- ADX length: header bytes 0x08 (sample rate, u32 BE) and 0x0C (total samples, u32 BE); seconds = samples / rate.

### Subtitle table -> entry
- `fonts/<stage>/<stage>_<LANG>.fnt`, 58 stages per language (`stg0005` ... `stg0802`, `Advertise` has no voice).
  Little-endian: `i32 count`, then `count` x 20-byte records `{text_offset, key, entryType, activeTime, audioId}`,
  then UTF-16LE NUL-terminated text.
- `audioId` is the AFS entry **index** (0..4479), -1 = no voice. Checked: all 9,138 voiced EN records have
  `0 <= audioId < 4480` (0 bad).
- FR, GE, IT, SP, JP tables have the same `(key, audioId)` pairs as EN in all 58 stages, so one map covers every text
  language: the voice is always the PRS_VOICE_E entry (or J when the Japanese voice is on), the text language only
  changes the string. Mod replacement of an English entry therefore changes the line in every text language.
- Result: **3,831 of 4,480** entries are referenced by a stage table; 649 are not (never referenced by any `*_EN.fnt`;
  these may be played by code or be unused). 9,138 voiced records in total. 2,748 entries are used in one stage, 1,083
  in more than one (generic lines such as GUN soldier calls appear in up to 40 stages).
- 317 voiced records are chained: key+1, key+2... follow with audio -1 and non-zero time (more text under the same voice).

## 2. JSON (no game text)

`test_data/mods/voice_lines.json`:

```
{ "lang":"EN", "stats":{...},
  "entries": { "<entry name>.adx": { "index": N, "seconds": S, "stages": ["stg0100",...],
        "keys": [key per ref], "refs": [ {"stage","key","type","frames","chain"} ... ] }, ... },
  "unreferenced": ["<entry name>", ...] }
```
`refs` is the authoritative list (stage, key, entryType, active window in frames, number of chained follow-on parts);
`stages` / `keys` are convenience views of it. No subtitle text and no audio bytes are stored (1.07 MB).

## 3. How the manager shows "this mod replaces N voice lines: <text> (stage)"

1. Enumerate the mod's replaced entries: files under `afs/PRS_VOICE_E/` (after the loader's name resolution; ignore
   entries not in the AFS table). N = number of distinct entry names.
2. For each entry look it up in `voice_lines.json` -> `index`, `seconds`, `refs`. Entries with no refs: show
   "<name> (<seconds> s, not referenced by any subtitle table)".
3. Text, read live from the player's own files, never stored by us. For each ref:
   - open `<game>/files/fonts/<stage>/<stage>_<LANG>.fnt` where `<LANG>` is the current text language (EN default; the
     same key exists in every language, see above). If a mod also ships `files/fonts/...fnt`, read the mod's file when
     it is enabled and has priority, so the text shown is what the player will see.
   - find the record with `key == ref.key` and `audioId == index` (match both: keys are unique per file but this guards
     against a mod's renumbering). Reloaded renumbered four keys in stg0501 (see 4); if the key is not found, fall back
     to the first record whose `audioId == index` in that stage's table.
   - text = UTF-16LE string at `text_offset` up to the first NUL, `\n` is a line break (show with spaces). Read to the
     NUL, not to the next offset (the last record can have trailing junk).
   - chained parts: records `key+1, key+2, ...` (consecutive, `audioId == -1`, non-zero time) belong to the same voice;
     append them (`ref.chain` is how many). Show the first part plus "(+N more)" in a list view.
   - empty text is normal: `entryType` 100 (BACKGROUND_VOICE, GUN soldier chatter) has no subtitle. Show the entry name
     and length instead.
4. Stage display: the folder name `stgNNNN`; map to a level name with the existing stage table of the manager (the
   coordinator's other maps), fall back to the id. Entries in many stages: show the first 3 stages and "+K more".
5. Order the list by stage id then key; show `seconds` (e.g. "6.4 s"). A replacement much longer than the original is
   worth a hint: the subtitle window (`frames`) does not change with the audio, so a longer replacement can outlast its
   subtitle (stock windows are timed to the Japanese recording anyway).

## 4. Reloaded's 37 replaced entries

Source: `testmods/shdth-reloaded/afs/PRS_VOICE_E/` (37 files, all names are existing AFS entries).

| group | entries | stage | keys | notes |
|---|---|---|---|---|
| act04_g149..g157 `_sl` | 9 | stg0501 | 2010000-2010002, 2011000-2011005 | stock lengths in the JSON |
| all01_n04_rg | 1 | stg0404 | 715200 | 5.9 s |
| all04_e06_bd | 1 | 16 stages (stg0100, 0200/1/2, 0301, 0400/2/3/4, 0500/1/3, 0600/1/3/4) | 139000 | Black Doom on his soldiers (type 150) |
| bas03_* `_eg` (e00-e07, h01-h06, m00-m05, n00-n03, w00-w01) | 25 | stg0602 | 1307000-1326000, 480000, 481000, 672000-675000 | 0.9 to 9.6 s stock length each |
| can01_m06_bd | 1 | stg0201 | 852000 | 4.5 s |

(25 + 9 + 1 + 1 + 1 = 37. The bas03 list is exactly the keys in `voice_lines.json`; use the JSON for the full rows.)

Reloaded's own `fonts/*/*_EN.fnt` (42 EN tables in the mod): compared by
(stage, key) against the stock tables, every one of the 37 entries is referenced by the same stage and key in Reloaded's
tables **except four**: `act04_g154..g157_sl` (stg0501) move from keys 2011002..2011005 to 2012000, 2013000, 2014000,
2015000 (it split the group's sequence into new keys; audio ids unchanged). 33 of 37 are identical, no entry is dropped,
none is newly referenced by another stage. Of those 42, Reloaded's tables have different record counts in
13 stages (stg0200, 0201, 0301, 0401, 0501, 0502, 0503, 0504, 0602, 0604, 0700, 0710, Advertise; stg0504 has 325 records against 427): it adds and removes
records, so compare by key, not by position.

## 5. In-game proof

Run: `pcm.sh`-style headless Westopolis tape (`--boot-stage stg0100`, input tape of the Westopolis replay,
`--headless --no-audio`-free audio dump via `--audio-dump`, 100 s budget, 99.4 s of audio, S16 stereo 32 kHz), same exe
`final-47133fd7.exe`; stock = `--no-mods`; mod = `--mods-dir` holding one mod with the files listed. Each run is about 9 s
of wall time; one process at a time. Comparison script: first differing stereo frame / 32000.

### 5.1 A real Reloaded entry: not reachable
- `all04_e06_bd` (the only Reloaded entry referenced by stg0100, key 139000) alone as a mod: PCM **byte-identical** to
  stock over 99.4 s (md5 a19216e1... both). The Reloaded full voice set (37 entries) over the whole tape, 316 s budget
  (315 s of audio): **identical** to stock (md5 c64ee788...). Idle (no tape) 60 s boots of stg0201, stg0400, stg0501,
  stg0602 with all 37 entries: identical to stock.
- Conclusion: none of the 37 replaced lines plays in the Westopolis replay, so the replay cannot show that a Reloaded
  line is heard. This is not a loader failure (see the control); the lines are triggered later or in other stages. NOT
  PROVEN for a real Reloaded line: needs a replay of stg0501 / stg0602 / stg0404 (or any stage with those keys).

### 5.2 Control (not a real mod): replace a reachable line with another disc line
- How the reachable lines were found: a control mod replacing all 394 entries referenced by the stg0100 table with
  one donor entry (`all01_h00_bd`, 2.746 s, read from the same disc AFS) changed the PCM in 8 spans starting at
  5.498 s. Bisecting the 394 names (9 runs) isolated the entries.
- `cty01_e04_sd` (index 3585, 6.378 s, stg0100 key 755100, entryType 150, window 395 frames): replaced by the donor ADX.
  PCM differs from stock **from 5.4977 s (frame 175925)**; the differing span is **5.498 to 11.873 s = 6.375 s**, which
  is the original line's length; a second span at 87.598 to 93.973 s (6.375 s) is the same line playing again. Nothing
  else differs. Its subtitle is the two-line string of key 755100.
- `act04_g34_sl` (index 285, 1.48 s, stg0100 key 1880003, entryType 100 background voice, no text; the line is shared by
  9 stages): replaced by the donor. Differs only in **32.930 to 34.865 s**. Nothing else differs.
- Reading: the loader's rebuilt AFS makes the game play the replacement bytes at exactly the original trigger, the
  differing span equals the original entry's length (the donor is longer but the voice slot and its window stay). The
  trigger time lines up with when the game requests the entry (key 755100 first voiced request in the stage at about
  5.5 s of replay time).
- Earlier whole-Reloaded run at 89 s (identical PCM) is explained: no replaced line plays in that replay.

### 5.3 What is and is not verified
- Verified: entry name -> index mapping (names from the AFS table, indices from the fnt `audioId`), a replaced
  entry changes the audio exactly where the game plays that entry (two control entries), and unaffected entries do not
  change. Entry length numbers equal the PCM span lengths to 3 ms.
- Not verified: that a real Reloaded line is heard (none reachable in the tape); that the subtitle text and the heard
  voice agree for each Reloaded line (only the map says so); the second language path (Japanese voice on).

## 6. Traps found
- `--asset-trace` shows reads from `PRS_VOICE_E.afs` of entries that are not played (read-ahead and stage preload):
  e.g. `all01_n04_rg` is read in the Westopolis run but replacing it changes nothing. Do not infer "plays" from reads.
- Replacing `act04_g52_sl` or `all02_h18gc_sn` (read in the run, referenced by stg0100) changed nothing either.
- Python `-I` plus Git Bash: `pcm.sh` needs MSYS paths (`/h/...`, `cygpath`); drive it from Bash, not a Windows subprocess
  with the Windows bash.
