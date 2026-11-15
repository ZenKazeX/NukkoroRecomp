# Structural checks for the loose disc files (mods map, field `formats`, 2026-11-14)

Scope: every loose file kind of `files/` that a mod replaces and the game parses without checking it: `.dat` (object
layouts, camera), `.inf`, `.fnt`, `.met`, `.gncp`, `.mlt`, `.bin`, `.scr`, `.fog`, `.bnr`, `.sfd`, `.tp`, `.srd`, `.conf`
(and the `.flo` movies the Reloaded pack ships). The loader already checked `.one .txd .dff .afs .adx` in
`check_file_format` (`runtime/mods/afs.cpp`); this extends it. Nothing in `wt-mods` was edited: the C++ is a block for
the coordinator (section 7), tested as a stand-alone program against the Python reference.

Deliverables (branch `mods/map-formats`):

| File | What |
|---|---|
| `tools/mods/check_formats.py` | reference implementation, stdlib Python 3, subcommands `scan` `mutate` `report` `rules` `budget` `list` `file` |
| `tools/mods/format_checks_snippet.inc` | the C++ block (also printed in section 7), `tools/mods/format_checks_test.cpp` = stand-alone harness |
| `test_data/mods/format_rules.json` | derived constants (inf sections of 3 regions, layout limits, `.tp` / `.bnr` sizes) |
| `test_data/mods/format_results.json` | false-positive run over 5 corpora (counts, errors, warnings by rule) |
| `test_data/mods/format_mutations_pal.json`, `..._mods.json` | detection run: every mutation and its verdict (no game bytes) |

## 1. Summary

* **14 kinds examined: 12 get structural rules, `.tp` only a size rule (its content is pixel data), `.conf` none (the game never reads it).** Every rule is `error` (the game reads outside the buffer or runs on a table that is certainly cut
  off: the loader should refuse the file) or `warning` (survivable; reported, never rejected).
* **Zero false positives.** Errors on 5 corpora: PAL `game/files` 1,388 files, NTSC-U 1,387, NTSC-J 1,388, the 14 installed
  real mods 794 files (`testmods`), the raw download repos 433 files = **0 errors**. The 133 warnings (all in the two Reloaded-derived corpora, none on the disc trees) are
  explained in section 6 (real mods deviate from the disc in 5 harmless ways).
* **Detection.** Truncation by 1 byte / at the last byte that carries information / by half, and a zeroed header, on 12
  sampled files per kind (PAL) and 8 per kind (real mods): every kind with a header is caught at `error` level by
  truncation-by-half and zeroed-header; truncation by 1 byte is caught wherever it removes information. The exceptions are
  listed in section 2 (they are cuts of bytes the game never reads).
* **In game** (final exe `47133fd7`, headless Westopolis, 300 frames): a half-length `stg0100_cmn.dat`, `_nrm.dat`, `_ds1.dat`
  or `common/ScoreData.bin` stops the game with an **execution fault** (exit 5), exactly the failure class of the
  truncated `.one`. Half-length `cam.dat`, `.fog`, `.fnt`, `.met`, `.gncp`, `nukkoro.inf`, `nukkoro2.inf` boot but change the
  picture (silent damage). Section 4.
* **C++ == Python**: the C++ block, built with `clang++ -std=c++20 -Wall -Wextra` (no warnings), agrees with the Python rules on
  all 5 corpora (5,000+ files) and on 632 mutated files (4 mutation sets over 105 PAL + 53 mod samples), **0 disagreements**.

## 2. Detection results

`tools/mods/check_formats.py mutate` (sampled spread over the sorted file list, so different banks / stages / languages).
Cells are `caught as error / caught as warning only / missed`. `trunc1` = last byte removed, `cutlast` = the last byte that
carries information removed (the shipped `.fnt`, `.met`, containers end in bytes the game never reads), `trunch` = first half,
`zero` = header bytes zeroed (first 4..0x40 bytes by kind).

PAL tree, 12 files per kind (`format_mutations_pal.json`):

| Kind | trunc1 | cutlast | trunch | zero |
|---|---|---|---|---|
| .dat (layouts + cam) | 12/0/0 | 12/0/0 | 12/0/0 | 12/0/0 |
| .fnt | 0/0/12 (harmless) | 12/0/0 | 12/0/0 | 12/0/0 |
| .met | 0/0/12 (harmless) | 12/0/0 | 4/0/8 (boundary) | 12/0/0 |
| .gncp | 11/0/1 (trailer) | 11/0/1 (trailer) | 12/0/0 | 12/0/0 |
| .mlt | 12/0/0 | 12/0/0 | 12/0/0 | 12/0/0 |
| .bin (setid, light, hint, AudioData, ScoreData) | 2/10/0 | 12/0/0 | 12/0/0 | 10/2/0 (light only warns) |
| .scr | 12/0/0 | 12/0/0 | 12/0/0 | 12/0/0 |
| .fog | 12/0/0 | 12/0/0 | 12/0/0 | 12/0/0 |
| .inf (2 files) | 0/2/0 | 0/2/0 | 2/0/0 | 2/0/0 |
| .sfd (4 movies, tail rewritten in memory) | 4/0/0 | | 4/0/0 | 4/0/0 |
| .srd (1 file) | 0/1/0 | 0/1/0 | 1/0/0 | 1/0/0 |
| .bnr (1) | 1/0/0 | 1/0/0 | 1/0/0 | 1/0/0 |
| .tp (5) | 5/0/0 | 5/0/0 | 5/0/0 | **0/0/5** (pixel data, no header) |
| .conf (1) | 0/0/1 | | | (never read, no rule) |

The same on 8 real-mod files per kind (`format_mutations_mods.json`, Reloaded + Xbox buttons + Text Fixes ...): the only
misses are the same pairs (`.fnt` trunc1, `.gncp` trunc1/cutlast on a bare project, `.tp` zero), plus `.flo` 4/0/0 on all 3.

Why the misses are not misses:
* `.fnt` trunc1: the file ends in `4 * count` bytes the game never reads (docs/re/text.md 4.1); cutting one changes nothing.
  Cutting into the last string (`cutlast`) is caught.
* `.met` trunc1: the final empty line is never read. `.met` trunch: shipped glyph lines are 24 bytes wide, so half the
  file often ends exactly on a line end and is a valid, smaller glyph table (there is no count to compare). No structural
  rule can tell it from a font with fewer glyphs.
* `.gncp` bare project (16 of 74 files): a trailer chunk follows the pointer table and is never read.
* `.bin` trunc1 is only a warning for 10 of 12: the container header size is `file size` and the cut byte lies in the
  string tail the loader never reads. A real mod (`stg0210_hint.bin`) ships one byte short of its header (118 vs 119)
  and works, so a size-field mismatch cannot be an error (see 6).
* `.inf` trunc1: removes only the last line break of a text file.
* `.tp` zero: the file is raw RGB5A3 picture data with no header.

## 3. Per kind

Legend: **R** reader (address, PAL `main.dol`, from docs or checked here), **T** fields it trusts, **Rule** = rule ids of
`check_formats.py` (E = error, w = warning). Little-endian (LE) vs big-endian (BE) was verified on the data.

### 3.1 `.dat` (158 on disc: 43 cmn, 23 nrm, 23 hrd, 41 ds1, 28 cam)

**Object layouts `stgNNNN_cmn|nrm|hrd|ds1.dat`** (LE; docs/distant-rendering/spawn_data.md, docs/re/files.md section 5).
R: stage build 0x801783D4 -> LoadLayout 0x800CBC54 -> per file 0x800CB1A4 -> whole-file read 0x800450C4.
T: magic `sky2`, `N` at +4 and `M` at +8; the loader reads `N` entries of 0x2C then `M` bytes of misc and copies them into fixed
arrays: **0x800 entries and a 0x10000-byte misc pool, no bounds check** (spawn_data.md 1.1, V).
Rule: `L02` signature, `L03` size == 12 + 0x2C*N + M (E), `L04` M%4, `L05` N <= 0x800, `L06` M <= 0x10000, `L07/L08` every
entry's misc length (+0x24) multiple of 4 and the lengths add up to M (E); `L09` an entry without saved bit 0 (w: shipped
all have it, one Reloaded entry does not).
Held on all 130 disc layouts x 3 regions and on 161 mod `.dat` (Reloaded). Detection 48/48.
Not checkable per file: **the combined budget**. The game holds `cmn + (nrm | hrd) + ds1` at once. `check_formats.py budget`
over the disc: max 1,404 entries (stg0700 nrm) of 2,048 and 25,516 misc bytes (stg0600 nrm) of 65,536 over 88 stage sets;
disc + the Reloaded layouts layered: 1,466 entries (stg0501) and 28,196 bytes (stg0600). A mod that adds to several layouts
of one stage can still overrun; the per-file rule cannot see it. The loader could sum the three files of the stage in the
plan (base file or mod file, whichever wins) and warn above 90 %; not part of the block.

**Camera `stgNNNN_cam.dat`** (28 on disc, LE). R: unknown (path string 0x804D01C8, camera field, not mapped). Header
`{3, 0x0131CD93, stage number, 1, 0, N, M', ...}` and then records: **size == 0x18 + 220 * N** where `N` is the word at +0x14
(derived here, true for all 28 disc files, and for Reloaded's `stg0100_cam.dat`). Rule: `K02` header words, `K03/K04`
size relation (E), `K05` stage number equals the file name (w). The record content is not checked (U).

### 3.2 `.fnt` message banks (348 on disc; 477 in mods)

R: FontBank::readMessages 0x8004A6AC (whole-file read, in-place swap), lookup 0x80049D60 / 0x80049E44 (docs/re/text.md 4.1-4.2).
LE. T: `count` at +0, `count` x 20-byte records, `textOff` per record; the loader adds the buffer base and **swaps 16-bit units
until a zero unit**, so an offset outside the buffer or an unterminated string walks off the heap buffer.
Rule (E): `F01` size >= 4, `F02` 4 + 20*count <= size and count >= 1, `F03` every `textOff` even and inside
`[4 + 20*count, size - 2]`, `F04` a zero unit exists between `textOff` and the end of the file. (w): `F05` keys ascend
(binary search), `F06` offsets ascend, `F07` last key 9999000.
Held: 348 x 3 regions + 477 mod files, 0 errors, 0 warnings. Detection 36/36 for cutlast, trunch, zero.
Not checked: the 4*count tail (never read, and Reloaded's files carry 4 bytes).

### 3.3 `.met` glyph metrics (551; 71 in mods)

R: FontReadMetrics1 0x804724EC (RwFgets, `%s`, `%d`, then `%i %i %i %i %i` per glyph); `Rt2dFontRead` 0x80474834 (docs/re/text.md 4.3).
Text. Rule (E): `M01` not empty, `M02` no NUL, `M03` ends with a line break, `M04` >= 4 lines, `M05` first line `METRICS1` (the only
variant shipped; METRICS2/3 parsers exist, no file uses them), `M07` third line an integer, `M08` every non-blank line is
`code x0 y0 x1 y1` plus an optional `#` tail, `M09` code in 1..65535, `M10` rectangle not inverted or negative, `M12` at least
one glyph. (w): `M06` the texture name line differs from the file's own name, `M11` duplicate code, `M13` carriage returns.
**Two findings forced warnings**: all 71 Reloaded `.met` use CRLF, and 57 of them name the shared sheet `Movie_EN00` instead of
their own. Both are proven to work: a CRLF copy of `stg0100_EN00.met` renders the identical frame as the original (section 4);
the texture name is resolved in the `.txd`, another file, so the `.met` alone cannot know.
Held: 551 x 3 + 71 mod files, 0 errors. Detection: cutlast 12/12, zero 12/12, trunch 4/12 (the boundary cases above).

### 3.4 `.gncp` CSD UI projects (74; 52 in mods)

R: loader 0x80360224 (container), 0x803603BC (NGIF header, offset table), chunk walk 0x80360444 (docs/re/csd.md section 3, RS 2.3).
Mixed endianness: wrapper `CPAF` (BE lengths) or bare `NGIF`; `NGIF` +0xC, +0x10, +0x14 BE; `nCPJ` chunk size LE; the pointer table
(NOF0) lists words that the loader **rebases in place**.
Rule (E): `G02/G03` both blobs inside the file (`12 + len1 + len2 <= size`), `G05` `NGIF`, `G06` data area inside the image,
`G07/G08` pointer table inside the image, `G09` every listed word inside the data area, `G10` every rebased pointer value inside the data
area (this is what makes a half file fail even when the header survives), `G11` one `nCPJ` chunk of `total - 8` bytes,
`G12` project root inside the data, `G13` the embedded texture dictionary (`0x16`) is whole. (w) `G04` bytes after blob 2.
Held: 74 x 3 + 52 mod files (Xbox button set 374 files incl. 51 gncp, HPBar). Detection: trunch 12/12, zero 12/12, cutlast 11/12 (bare trailer).

### 3.5 `.mlt` sound banks (27 in this tree; docs/re/sound.md counts 28)

R: gcaxLoadMLT 0x8044075C (docs/re/sound.md 6). BE. Layout (verified on all 27 + Reloaded `B81_FINALDOOM`): `gcaxMLT ` +8 zero +12 file
size, `gcaxMLTM` at 0x10, `gcaxMPB ` at 0x40, `gcaxMPBW` at 0x50 with the wave size at +0x5C, `gcaxMPBP` directly after the wave
bytes (at `0x60 + wave`) with the program size at +12; the file is padded to 32 bytes after the program table.
Rule (E): `S01` signature, `S02` header size field == file size (equal on all 28: the bank is read by size), `S03/S04` the sub
signatures, `S05` the program chunk where the wave size says, `S06` the program table inside the file. (w) `S07` MPB size field, `S08` >= 32 trailing bytes.
Not checked: the per-bank slot sizes (0xE1000 ... 0xFA000, docs/re/sound.md 6): an oversize bank takes the loader's `-2` path, an error
the game handles. Held 27 x 3 + 1 mod. Detection 48/48. **Not run in game**: headless `--no-audio` never loads banks (a half `B0_SYSTEM.mlt` changed nothing), and `--headless --audio` is excluded by the brief.

### 3.6 `.bin` (83)

Five different things share the extension:
* `setid.bin` (LE `{0, N}` + N x 12 bytes, reader 0x800CB044 / swap 0x800CB6EC): `B01..B03` size == 8 + 12*N (E). Half file: no
  fault and no visible change at stg0100 (the ids it lost are late ones); the game reads `N` records, so a truncated file that
  keeps a large `N` reads past the buffer.
* `stgNNNN_light.bin` (41, always 832 bytes, 208 LE words, reader 0x80048738): `B05` size >= 832, `B07` no NaN (E); `B06` size != 832,
  `B08` first 16 bytes zero (w: zeroed header cannot be told from a legal table, shipped files never begin that way).
* `stgNNNN_hint.bin` (17, "1B" container without relocation, reader 0x802F33BC): container rules plus `H01` data = n*12+12 and no
  relocation, `H02` the `{-1, -1, 0}` terminator record present (E; the reader runs until it).
* `AudioData*.bin` (23, "1B" container, 8 slots, reader 0x801D8AFC): container rules plus `A01..A04` the row count x 0x18 inside the
  data, every row's name offset inside the string area and NUL-terminated (E).
* `common/ScoreData.bin` (reader 0x802068B0 through 0x800450C4): container rules.
Container rules (docs/re/formats.md section 2; reader 0x8040F66C, relocation pass 0x8040F868): `C01` >= 32 bytes, `C06` D and R multiples
of 4, `C07` 0x20 + D + R <= size, `C02` byte +0x17 == `B`, `C08/C09` **the relocation stream stays inside the data** (the loader rebases
the words it names, so a bad stream writes outside the buffer; in game a half `ScoreData.bin` hits an execution fault) (E).
(w) `C04` the header's size word differs from the file: Reloaded ships `ScoreData.bin` / `ScoreDatb.bin` 22 bytes longer than their header
says, and `stg0210_hint.bin` one byte shorter; the loader works from the real file size, so this cannot be an error. `C05` counts != (1, 0).
Unknown `.bin` names (Reloaded `ScoreDatb.bin` is checked as a ScoreData file; other names) get no rule.
Held: 83 x 3 + 14 mod files. Detection: cutlast 12/12, trunch 12/12, zero 10 error + 2 warning (light).

### 3.7 `.scr` event scripts (90)

R: loader 0x80174500 / dispatcher 0x80174054 (docs/re/events.md 3.7-3.8). LE. Size == 0x188 + count * 0x80 on all 90 (`E01..E03`,
E); `E04` event id equals the file name (w). Held 90 x 3. Detection 48/48. Not run in game (needs `--boot-cutscene`, not tried).

### 3.8 `.fog` (39)

R: TFogSet 0x8016A5E4 -> 0x8016A66C (TBGRead), fixup 0x8016A4EC byte-swaps **24 entries x 0x28 = 960 bytes** unconditionally
(docs/distant-rendering/camera_fog_culling.md 2). Rule (E): `O01` size >= 960, `O03` no NaN/inf in the near/far/start/end floats,
`O04` the first area entry is not all zero (every shipped file fills it; a zeroed header is the only thing the damage test
can see, since all-zero later entries are normal). (w) `O02` size != 960. Held: 39 x 3 + 5 mod. Detection 48/48 (a half file: in game
`stg0100.fog` half changes the picture, no fault).

### 3.9 `.inf` (2)

R: InfDatabase ctor 0x801E0C68 (appfw.md: game init step 8), 59 x 192-byte slots + 27 x 16; the section names are matched against a
name table in the executable (it already has `EVENT0911`, which Reloaded's `nukkoro2.inf` adds). Shift-JIS text, CRLF.
Rule (E): `I01` not empty, `I02` no NUL, `I05` at least one `[SECTION]`, `I07` `nukkoro2.inf` contains the last shipped section
`[EVENT0910]`, `I08` `nukkoro.inf` contains the last shipped key `AREA :`. (w) `I04` no final line break (Reloaded's `nukkoro.inf`),
`I06` a shipped section is missing (`format_rules.json`: 58 sections for `nukkoro2.inf`, `ALL` for `nukkoro.inf`, the intersection of the 3 regions;
Reloaded adds `[EVENT0911]`, the More Reasonable Missions file has the 58). A byte-level scan avoids a Shift-JIS decoder in C++.
Held: 2 x 3 + 3 mod. In game: a half `nukkoro2.inf` booted at stg0610 changes the frame (silent: later stages lose RANK / GOALEVENTPOS tuning), a half
`nukkoro.inf` changes the frame at stg0100. The rule is a cut-off detector derived from the data, **not from a proven fault**.
**Region trap found on the way**: the PAL `nukkoro.inf` says `REGION : PAL50` / `AREA : EU`, Reloaded's says `NTSC` / `US`, and
the file is read at game init (appfw.md). A mod that ships it changes the console region. Not a format error; the manager should
treat `nukkoro.inf` as a settings file and say so.

### 3.10 `.sfd` movies (8) and Reloaded's `.flo` (6)

R: movie phase `E%04d.sfd` (0x802D33A4), TMovieManager (docs/re/events.md). MPEG program stream: `00 00 01 BA` first; every file ends
with `00 00 01 B9` then 0xFF fill to the 2048-byte sector; sizes are 11.2 .. 66.9 MB, all multiples of 2048. Rule (E): `V01` starts with the
pack header, `V02` the last sector holds the program end code and only 0xFF after it, `V03` size is a whole number of sectors. The
checker reads the head and the last 2048 bytes only. Held: 8 x 3 disc movies and the 6 `.flo` (9.6 .. 22.3 MB, all sector multiples
and ending in the end code), 0 errors. Detection 12/12 (rewritten in memory, no 60 MB copies). `V03` is a judgment call: 14 of 14
real movies are sector multiples, but a movie muxed without padding would be refused; drop `V03` if one appears. Not run in game.

**Revised 2026-11-14 (it appeared).** Both GameBanana intro mods (Re-edited Intro, Early Demo Intro) were refused as damaged:
CRI's later sfdmux writes a stream that starts with a private stream 2 packet (`00 00 01 BF`, "SofdecStream2"), not a pack
header, and ends at its last packet's last byte (no end code, no padding; 47,125,915 and 45,951,205 bytes). The game plays both
(modtest scenarios `movie-op-*`: 4 reads byte-exact from the mod, the movie's own picture on screen). The rule is now a walk of
the whole stream, packet by packet (pack headers MPEG-1/2, every other packet `6 + length`): `V01` the first bytes are not an MPEG
packet, `V04` no packet start where one must be, `V02` the stream ends inside a packet, or has data other than 0xFF/0x00 after an
end code, or (a stream that starts with a pack header, the disc's layout) has no end code - a cut at a pack boundary. `V03` is
gone. Held: the 8 PAL, 8 NTSC-U and 8 NTSC-J disc movies, Reloaded's 6 `.flo`, both intro mods; caught: cuts inside a packet
in either layout and a disc movie cut at a sector boundary (unit tests and real files). The checker now reads the whole movie.

### 3.11 `.tp` (5), `.bnr` (1), `.srd` (1), `.conf` (1)

* `.tp`: raw memory-card pictures (docs/resource-spine/boot_globals.md): `mc_bunner_e/j.tp` 6144 bytes (96x32 RGB5A3), `mc_icon01..03.tp`
  2048 bytes (32x32). Read at boot by TMemoryCardCtrl (0x8029A164, banner 0x8029A3D4, icons 0x8029917C). Rule `P01` (E): size >= the exact
  size. Content has no structure, so zeroed data cannot be detected (**not checkable**). Half-size `mc_icon01.tp` / `mc_bunner_e.tp` boot
  with the same frame (no fault seen at stage start; the card screen was not opened).
* `.bnr`: GameCube disc banner, read by the IPL, not by the game. `N01/N02` signature `BNR1` (6,496 bytes: the NTSC discs and Reloaded) or `BNR2` (8,096:
  PAL) and size >= that (E). A mod's banner has no effect on the PC build.
* `.srd` (`stafroll/StafRoll.srd`, 11,328 bytes, LE, reader around 0x8034304C): not mapped before. Layout derived here: `SSRD`, +0x10 = 44 entries of 12 bytes at
  +0x14 (=0x20) (the third word of an entry is a text offset), +0x18 = 95 strips of 8 bytes at +0x1C (=0x2938), then 16 zero bytes: size ==
  0x2938 + 95*8 + 16 exactly. Rule (E): `R01` signature, `R02/R03` both tables inside the file, `R05` entry text offsets inside the file; (w) `R04` size relation.
  Evidence: this one disc file + Reloaded's (same size relation). **Inferred (I), reader not disassembled.**
* `.conf` (`sound/GCAX.conf`): never read by the game (docs/re/sound.md 6: the name is not in the DOL). No rule, nothing to check; a replaced
  `GCAX.conf` has no effect.

## 4. In-game evidence

Run: `scripts/cap.sh`-style headless capture, `final-47133fd7.exe`, `--boot-stage stg0100` + the Westopolis tape, `--headless --no-audio`,
capture at tick 243,000,000 (300 frames), one mod `probe` that replaces one file, one process at a time. The unmodified control, the same
file placed through the mod loader, gives the baseline frame (sha256 prefix `26c1a16e9415`).

| Replaced file | Mutation | Result |
|---|---|---|
| stg0100/stg0100_cmn.dat | first half | **Stop reason: execution fault, exit 5**, no frame |
| stg0100/stg0100_nrm.dat, _ds1.dat | first half | **execution fault, exit 5** |
| common/ScoreData.bin | first half | **execution fault, exit 5** |
| stg0100/stg0100_cmn.dat | last byte removed | baseline frame (the cut byte is unread) |
| stg0100/stg0100_cmn.dat | first 0x30 bytes zeroed | boots, different frame (no "sky2" magic: objects missing) |
| stg0100/stg0100_cam.dat | first half | boots, different frame |
| stg0100/stg0100.fog | first half | boots, different frame |
| fonts/stg0100/stg0100_EN.fnt, ..._EN00.met | first half | boots, different frame (same for both) |
| fonts/stg0100/stg0100_EN00.met | CRLF line ends | **baseline frame** (CRLF accepted) |
| csdFiles/HeroGauge.gncp | first half | boots, different frame (HUD) |
| nukkoro.inf (stg0100), nukkoro2.inf (stg0610) | first half | boots, different frame |
| nukkoro2.inf (stg0300), setid.bin, light.bin, AudioData{Enemy,Gadget,Player}.bin, stg0210_hint.bin, mc_*.tp, B0_SYSTEM.mlt, opening.bnr | first half | baseline frame (no visible effect at this stage/tick; sound and hint tables are consumed later or with audio) |

All those files fail the checker at `error` level (the ones with no visible effect are shortened tables whose lost rows lie beyond
what the capture reaches). Not exercised in game: `.scr` (needs `--boot-cutscene`), `.sfd`/`.flo`, `.srd`, `.mlt` (audio), `.tp` card screens,
language banks other than EN. Scripts: `scratchpad/scripts/probe.py` + `probe_matrix.sh` (local helpers, not in the repo; they only wrap
`--mods-dir` and the documented capture options).

## 5. Findings on the way (for the manager / coordinator)

1. `nukkoro.inf` carries the console region (`REGION`, `AREA`, `LANGUAGE`, `SPEECH`): a mod's copy changes them (3.9).
2. Reloaded ships files the disc does not have: `common/ScoreDatb.bin`, six `.flo` movies (valid MPEG streams), and `[EVENT0911]` in `nukkoro2.inf`.
3. Real mods are looser than the disc: CRLF `.met`; `.met` naming a shared sheet; `.fnt` with a 4-byte tail; `ScoreData.bin` 22 bytes longer than its
   header; `stg0210_hint.bin` one byte shorter than its header; a `nukkoro.inf` without final newline; `opening.bnr` is `BNR1` (6,496 bytes) on the NTSC
   discs and Reloaded, `BNR2` (8,096) on PAL. Every one is accepted by the rules above.
4. Layout budget: the game's fixed room (2,048 entries / 65,536 misc bytes) is 69 % / 39 % used by the largest disc stage; Reloaded 72 % / 43 %.
5. `.srd` and `cam.dat` structure derived here (3.1, 3.11) were unmapped.

## 6. Warnings seen on real files (never rejected)

| Rule | Where | Count |
|---|---|---|
| `M13` carriage returns | Reloaded `.met` | 71 |
| `M06` texture name is another sheet | Reloaded `.met` | 57 |
| `C04` container size word differs | Reloaded `ScoreData.bin`, `ScoreDatb.bin`, `stg0210_hint.bin` | 3 |
| `L09` entry not enabled | Reloaded `stg0201_cmn.dat` entry 310 | 1 |
| `I04` no final line break | Reloaded `nukkoro.inf` | 1 |

Disc trees (3 regions): 0 warnings.

## 7. C++ for `check_file_format`

Two edits in `runtime/mods/afs.cpp`:

**(a)** paste the block below into the anonymous namespace after `align_up` (it uses `le32` and `lower` that are already there and
adds `be32`, `bad`, `num`, `parse_int`, the per-kind checkers, `is_data_kind`, `check_data_file`). No new include is needed
(`<algorithm> <cctype> <cstring> <fstream>` are in the file, `<vector> <string_view>` come with `mods.h`); no new dependency.

**(b)** in `check_file_format` replace the early return and add the dispatch (keep the rest):

```cpp
    if (ext != ".one" && ext != ".txd" && ext != ".dff" && ext != ".afs" && ext != ".adx" && !is_data_kind(ext)) return true;
    std::error_code ec;
    const std::uint64_t size = fs::file_size(file, ec);
    if (ec) {
        why = "cannot be read";
        return false;
    }
    if (is_data_kind(ext)) return check_data_file(file, ext, game_path, size, why);
    if (ext == ".afs") {
```

`install.cpp` line 848 passes only the file's last name as `game_path`; the block uses the base name only (never a directory), so both call
sites work. Cost: a file is read whole once (largest: `.gncp` 1.4 MB, `.mlt` 1.2 MB; `.sfd`/`.flo` read 2 x 2 KiB), the same pass that
already reads `.one` heads.

Tests to add next to the existing `check_file_format` test (`runtime/tests/mod_loader_tests.cpp` line 517): the disc's own files pass
(`stg0100_cmn.dat`, `stg0100_EN.fnt`, `HeroGauge.gncp`, `nukkoro2.inf`, `B0_SYSTEM.mlt`, `ScoreData.bin`, `stg0100.fog`, `E8003.sfd`) and the
same files cut to half or with the header zeroed fail. `tools/mods/format_checks_test.cpp` shows the whole-tree comparison.

```cpp
// ---- structural checks of the loose data files -------------------------------------------------------------------
// Paste inside the anonymous namespace of afs.cpp (after align_up). Needs le32() and lower() from the same file.
// Every rule here stops a read outside the file or a table that is certainly cut off; each held on every shipped file
// of the PAL, NTSC-U and NTSC-J trees and on every file of the real mods tried (docs/mods/map/FORMAT_CHECKS.md).

using Bytes = std::vector<std::uint8_t>;

std::uint32_t be32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) << 24 | static_cast<std::uint32_t>(p[1]) << 16 |
           static_cast<std::uint32_t>(p[2]) << 8 | static_cast<std::uint32_t>(p[3]);
}

bool bad(std::string& why, std::string text) {
    why = std::move(text);
    return false;
}

std::string num(std::uint64_t v) { return std::to_string(v); }

bool is_ascii_space(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'; }

// Decimal or 0x-hex integer as the metrics reader's "%i" takes it (no leading zeros, so no octal).
bool parse_int(std::string_view t, long long& out) {
    std::size_t i = 0;
    bool neg = false;
    if (i < t.size() && (t[i] == '+' || t[i] == '-')) neg = t[i++] == '-';
    int base = 10;
    if (i + 2 < t.size() && t[i] == '0' && (t[i + 1] == 'x' || t[i + 1] == 'X')) {
        base = 16;
        i += 2;
    }
    if (i >= t.size()) return false;
    if (base == 10 && t[i] == '0' && i + 1 < t.size()) return false;
    long long v = 0;
    for (; i < t.size(); ++i) {
        const int c = t[i];
        const int d = c >= '0' && c <= '9' ? c - '0' : (base == 16 && c >= 'a' && c <= 'f') ? c - 'a' + 10
                      : (base == 16 && c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
        if (d < 0 || d >= base) return false;
        v = v * base + d;
        if (v > (1ll << 40)) return false;
    }
    out = neg ? -v : v;
    return true;
}

// The "1B" container (ScoreData.bin, AudioData*.bin, stgNNNN_hint.bin): 0x20-byte big-endian header, data, relocation
// stream. The loader rebases the words the stream names, so a stream that leaves the data writes outside the buffer.
bool check_container(const Bytes& d, std::uint64_t& data_len, std::uint64_t& reloc_len, std::string& why) {
    const std::uint64_t n = d.size();
    if (n < 0x20) return bad(why, "is " + num(n) + " bytes, shorter than the 32-byte container header");
    data_len = be32(&d[4]);
    reloc_len = be32(&d[8]);
    if (data_len % 4 || reloc_len % 4) return bad(why, "has a data/relocation length that is not a multiple of 4 (damaged)");
    if (0x20 + data_len + reloc_len > n) return bad(why, "has tables that overrun the file (truncated or damaged)");
    if (d[0x17] != 0x42) return bad(why, "has no big-endian marker (\"1B\") in its header (damaged)");
    const std::uint8_t* s = d.data() + 0x20 + data_len;
    std::uint64_t pos = 0;
    for (std::uint64_t i = 0; i < reloc_len;) {
        const std::uint8_t c = s[i++];
        std::uint64_t delta = 0;
        switch (c >> 6) {
            case 0: continue;
            case 1: delta = c & 0x3Fu; break;
            case 2:
                if (i + 1 > reloc_len) return bad(why, "has a relocation stream cut inside a code (truncated or damaged)");
                delta = (static_cast<std::uint64_t>(c & 0x3Fu) << 8) | s[i];
                i += 1;
                break;
            default:
                if (i + 3 > reloc_len) return bad(why, "has a relocation stream cut inside a code (truncated or damaged)");
                delta = (static_cast<std::uint64_t>(c & 0x3Fu) << 24) | static_cast<std::uint64_t>(s[i]) << 16 |
                        static_cast<std::uint64_t>(s[i + 1]) << 8 | s[i + 2];
                i += 3;
                break;
        }
        pos += delta;
        if (pos * 4 + 4 > data_len) return bad(why, "has a relocation that points past its data (damaged)");
    }
    return true;
}

bool check_bin(const Bytes& d, const std::string& base, std::string& why) {
    const std::uint64_t n = d.size();
    if (base == "setid.bin") {
        if (n < 8) return bad(why, "is " + num(n) + " bytes, shorter than its 8-byte header");
        const std::uint64_t want = 8 + 12ull * le32(&d[4]);
        if (want != n)
            return bad(why, "is " + num(n) + " bytes but its header says " + num(le32(&d[4])) + " records (" + num(want) +
                                " bytes): truncated or damaged");
        return true;
    }
    if (base.size() > 10 && base.compare(base.size() - 10, 10, "_light.bin") == 0) {
        if (n < 0x340) return bad(why, "is " + num(n) + " bytes, the game reads 832");
        for (std::size_t i = 0; i < 208; ++i) {
            const std::uint32_t w = le32(&d[4 * i]);
            if ((w & 0x7F800000u) == 0x7F800000u && (w & 0x007FFFFFu) != 0) return bad(why, "holds a NaN in its light table");
        }
        return true;
    }
    const bool hint = base.size() > 9 && base.compare(base.size() - 9, 9, "_hint.bin") == 0;
    const bool audio = base.rfind("audiodata", 0) == 0;
    if (!hint && !audio && base.rfind("scoredat", 0) != 0) return true;  // not a table of the game
    std::uint64_t data = 0, reloc = 0;
    if (!check_container(d, data, reloc, why)) return false;
    if (hint) {
        if (reloc != 0 || data % 12 || data < 12) return bad(why, "is not a hint table (its data is not whole 12-byte records)");
        const std::uint8_t* last = d.data() + 0x20 + data - 12;
        if (be32(last) != 0xFFFFFFFFu || be32(last + 4) != 0xFFFFFFFFu)
            return bad(why, "has no terminator record in its hint table (truncated or damaged)");
    } else if (audio) {
        if (data < 8) return bad(why, "is too short for its sound table");
        const std::uint64_t rows = be32(&d[0x24]);
        const std::uint64_t rows_end = 8 + 0x18 * rows;
        if (rows_end > data) return bad(why, "says it has " + num(rows) + " sound rows but its data is " + num(data) + " bytes");
        for (std::uint64_t i = 0; i < rows; ++i) {
            const std::uint64_t off = be32(&d[0x28 + 0x18 * i]);
            if (off < rows_end || off >= data) return bad(why, "has a sound row whose name lies outside the file's string area");
            if (std::find(d.begin() + 0x20 + off, d.begin() + 0x20 + data, std::uint8_t{0}) == d.begin() + 0x20 + data)
                return bad(why, "has a sound row name without a terminator");
        }
    }
    return true;
}

bool check_dat(const Bytes& d, const std::string& base, std::string& why) {
    const std::uint64_t n = d.size();
    if (base.size() > 8 && base.compare(base.size() - 8, 8, "_cam.dat") == 0) {
        if (n < 0x18) return bad(why, "is " + num(n) + " bytes, shorter than the 24-byte camera header");
        if (le32(&d[0]) != 3 || le32(&d[4]) != 0x0131CD93u) return bad(why, "is not a stage camera file (header damaged)");
        const std::uint64_t want = 0x18 + 220ull * le32(&d[0x14]);
        if (want != n)
            return bad(why, "is " + num(n) + " bytes but its header says " + num(le32(&d[0x14])) + " camera records (" +
                                num(want) + " bytes): truncated or damaged");
        return true;
    }
    // Object layouts stgNNNN_cmn / nrm / hrd / ds1: the loader copies them into fixed arrays without a bounds check.
    if (n < 12) return bad(why, "is " + num(n) + " bytes, shorter than the 12-byte layout header");
    if (std::memcmp(d.data(), "sky2", 4) != 0) return bad(why, "is not an object layout (no \"sky2\" signature)");
    const std::uint64_t count = le32(&d[4]), misc = le32(&d[8]);
    const std::uint64_t want = 12 + 0x2Cull * count + misc;
    if (want != n)
        return bad(why, "is " + num(n) + " bytes but its header says " + num(count) + " entries + " + num(misc) + " bytes (" +
                            num(want) + "): truncated or damaged");
    if (misc % 4) return bad(why, "has a misc size that is not a multiple of 4");
    if (count > 0x800) return bad(why, "has " + num(count) + " objects, the game has room for 2048");
    if (misc > 0x10000) return bad(why, "has " + num(misc) + " bytes of object parameters, the game has room for 65536");
    std::uint64_t total = 0;
    for (std::uint64_t i = 0; i < count; ++i) {
        const std::uint32_t len = le32(&d[12 + 0x2C * i + 0x24]);
        if (len % 4) return bad(why, "has an object whose parameter size is not a multiple of 4");
        total += len;
    }
    if (total != misc) return bad(why, "has object parameter sizes that do not add up to its header (damaged)");
    return true;
}

bool check_fnt(const Bytes& d, std::string& why) {
    const std::uint64_t n = d.size();
    if (n < 4) return bad(why, "is " + num(n) + " bytes, shorter than its count word");
    const std::uint64_t count = le32(&d[0]);
    const std::uint64_t table_end = 4 + 20 * count;
    if (count == 0 || table_end > n)
        return bad(why, "says " + num(count) + " messages but is " + num(n) + " bytes (truncated or damaged)");
    for (std::uint64_t i = 0; i < count; ++i) {
        const std::uint64_t off = le32(&d[4 + 20 * i]);
        if ((off & 1) || off < table_end || off + 2 > n) return bad(why, "has message " + num(i) + " pointing outside its text area");
        std::uint64_t j = off;
        while (j + 1 < n && (d[j] | d[j + 1])) j += 2;
        if (j + 1 >= n) return bad(why, "has a message that is not terminated before the end of the file (truncated)");
    }
    return true;
}

bool check_met(const Bytes& d, std::string& why) {
    if (d.empty()) return bad(why, "is empty");
    if (std::find(d.begin(), d.end(), std::uint8_t{0}) != d.end()) return bad(why, "contains a NUL byte (a text file)");
    std::string text;  // carriage returns dropped: the Reloaded fonts ship CRLF
    for (const std::uint8_t c : d)
        if (c != '\r') text.push_back(static_cast<char>(c));
    if (text.back() != '\n') return bad(why, "does not end with a line break (its last glyph line is cut)");
    std::vector<std::string_view> lines;
    for (std::size_t at = 0; at < text.size();) {
        const std::size_t nl = text.find('\n', at);
        lines.emplace_back(text.data() + at, nl - at);
        at = nl + 1;
    }
    if (lines.size() < 4) return bad(why, "has " + num(lines.size()) + " lines, a glyph file needs 4");
    if (lines[0] != "METRICS1") return bad(why, "is not a METRICS1 glyph file");
    long long v = 0;
    std::string_view third = lines[2];
    while (!third.empty() && is_ascii_space(static_cast<unsigned char>(third.front()))) third.remove_prefix(1);
    while (!third.empty() && is_ascii_space(static_cast<unsigned char>(third.back()))) third.remove_suffix(1);
    if (!parse_int(third, v)) return bad(why, "has a third line that is not a number");
    std::size_t glyphs = 0;
    for (std::size_t k = 3; k < lines.size(); ++k) {
        std::string_view line = lines[k].substr(0, lines[k].find('#'));
        std::vector<std::string_view> tok;
        for (std::size_t i = 0; i < line.size();) {
            while (i < line.size() && is_ascii_space(static_cast<unsigned char>(line[i]))) ++i;
            const std::size_t b = i;
            while (i < line.size() && !is_ascii_space(static_cast<unsigned char>(line[i]))) ++i;
            if (i > b) tok.push_back(line.substr(b, i - b));
        }
        if (lines[k].find_first_not_of(" \t\v\f") == std::string_view::npos) continue;  // blank line
        long long f[5] = {};
        bool ok = tok.size() == 5;
        for (std::size_t i = 0; ok && i < 5; ++i) ok = parse_int(tok[i], f[i]);
        if (!ok) return bad(why, "has glyph line " + num(k + 1) + " that is not \"code x0 y0 x1 y1\"");
        if (f[0] <= 0 || f[0] > 0xFFFF) return bad(why, "has a glyph code outside 1..65535 on line " + num(k + 1));
        if (f[1] < 0 || f[2] < 0 || f[3] < f[1] || f[4] < f[2]) return bad(why, "has a glyph rectangle that is inverted or negative on line " + num(k + 1));
        ++glyphs;
    }
    if (glyphs == 0) return bad(why, "has no glyph lines");
    return true;
}

bool check_gncp(const Bytes& d, std::string& why) {
    const std::uint64_t n = d.size();
    if (n < 12) return bad(why, "is " + num(n) + " bytes, too short for a UI project");
    const std::uint8_t* image = d.data();
    std::uint64_t m = n;
    const std::uint8_t* tex = nullptr;
    std::uint64_t tex_len = 0;
    const bool wrapped = std::memcmp(d.data(), "CPAF", 4) == 0 || std::memcmp(d.data(), "FAPC", 4) == 0;
    if (wrapped) {
        const bool big = d[0] == 'C';
        auto rd = [&](std::uint64_t o) { return big ? be32(&d[o]) : le32(&d[o]); };
        const std::uint64_t l1 = rd(4);
        if (8 + l1 + 4 > n) return bad(why, "has a first part that runs past the file (truncated or damaged)");
        const std::uint64_t l2 = rd(8 + l1);
        if (12 + l1 + l2 > n) return bad(why, "has a texture part that runs past the file (truncated or damaged)");
        image = d.data() + 8;
        m = l1;
        tex = d.data() + 12 + l1;
        tex_len = l2;
    }
    if (m < 0x20 || std::memcmp(image, "NGIF", 4) != 0) return bad(why, "has no \"NGIF\" UI header (damaged)");
    const std::uint64_t data_off = be32(image + 0xC), total = be32(image + 0x10), reloc_off = be32(image + 0x14);
    if (data_off < 0x18 || data_off + total > m || total < 0x20) return bad(why, "has a data area outside the file (truncated or damaged)");
    if (reloc_off < data_off || reloc_off + 0x10 > m) return bad(why, "has a pointer table outside the file (truncated or damaged)");
    const std::uint64_t count = be32(image + reloc_off + 8);
    if (reloc_off + 0x10 + 4 * count > m) return bad(why, "has a pointer table that overruns the file (truncated or damaged)");
    const std::uint8_t* data = image + data_off;
    for (std::uint64_t i = 0; i < count; ++i) {
        const std::uint64_t o = be32(image + reloc_off + 0x10 + 4 * i) & ~3ull;
        if (o + 4 > total) return bad(why, "has a pointer entry outside its data (damaged)");
        if (be32(data + o) > total) return bad(why, "has a pointer that leads outside its data (damaged)");
    }
    if (std::memcmp(data, "nCPJ", 4) != 0 || le32(data + 4) != total - 8) return bad(why, "has a damaged project chunk");
    if (static_cast<std::uint64_t>(be32(data + 8)) + 0x10 > total) return bad(why, "has a project root outside its data (damaged)");
    if (tex_len != 0 && (tex_len < 12 || le32(tex) != 0x16 || static_cast<std::uint64_t>(le32(tex + 4)) + 12 > tex_len))
        return bad(why, "has an embedded texture dictionary that is not whole (truncated or damaged)");
    return true;
}

bool check_mlt(const Bytes& d, std::string& why) {
    const std::uint64_t n = d.size();
    if (n < 0x70 || std::memcmp(d.data(), "gcaxMLT ", 8) != 0) return bad(why, "is not a sound bank (no \"gcaxMLT\" signature)");
    if (be32(&d[8]) != 0 || be32(&d[12]) != n)
        return bad(why, "is " + num(n) + " bytes but its header says " + num(be32(&d[12])) + " (truncated or damaged)");
    if (std::memcmp(&d[0x10], "gcaxMLTM", 8) != 0 || std::memcmp(&d[0x40], "gcaxMPB ", 8) != 0 ||
        std::memcmp(&d[0x50], "gcaxMPBW", 8) != 0)
        return bad(why, "has a damaged bank header");
    const std::uint64_t ip = 0x60ull + be32(&d[0x5C]);
    if (ip + 16 > n || std::memcmp(&d[ip], "gcaxMPBP", 8) != 0) return bad(why, "has wave data that does not end at its program table");
    if (ip + 16 + be32(&d[ip + 12]) > n) return bad(why, "has a program table that runs past the file (truncated or damaged)");
    return true;
}

bool check_scr(const Bytes& d, std::string& why) {
    const std::uint64_t n = d.size();
    if (n < 0x188) return bad(why, "is " + num(n) + " bytes, shorter than the 392-byte event script header");
    const std::uint64_t want = 0x188 + 0x80ull * le32(&d[4]);
    if (want != n)
        return bad(why, "is " + num(n) + " bytes but its header says " + num(le32(&d[4])) + " records (" + num(want) +
                            " bytes): truncated or damaged");
    return true;
}

bool check_fog(const Bytes& d, std::string& why) {
    if (d.size() < 960) return bad(why, "is " + num(d.size()) + " bytes, the game reads 960 (24 fog areas of 40)");
    bool first_filled = false;
    for (std::size_t i = 0; i < 40; ++i) first_filled = first_filled || d[i] != 0;
    for (std::size_t e = 0; e < 24; ++e) {
        for (const std::size_t off : {16u, 20u, 28u, 32u}) {
            const std::uint32_t w = le32(&d[40 * e + off]);
            if ((w & 0x7F800000u) == 0x7F800000u) return bad(why, "has a non-finite number in fog area " + num(e));
        }
    }
    if (!first_filled) return bad(why, "has an empty first fog area (damaged: every shipped file fills it)");
    return true;
}

bool check_srd(const Bytes& d, std::string& why) {
    const std::uint64_t n = d.size();
    if (n < 0x20 || std::memcmp(d.data(), "SSRD", 4) != 0) return bad(why, "is not a staff roll table (no \"SSRD\" signature)");
    const std::uint64_t ents = le32(&d[0x10]), tab = le32(&d[0x14]), strips = le32(&d[0x18]), soff = le32(&d[0x1C]);
    if (tab + 12 * ents > n) return bad(why, "has an entry table that overruns the file (truncated or damaged)");
    if (soff + 8 * strips > n) return bad(why, "has a strip table that overruns the file (truncated or damaged)");
    for (std::uint64_t i = 0; i < ents; ++i)
        if (static_cast<std::uint64_t>(le32(&d[tab + 12 * i + 8])) + 1 > n) return bad(why, "has an entry pointing outside the file");
    return true;
}

// nukkoro.inf / nukkoro2.inf: Shift-JIS text; bytes only. A cut file loses its last section / key.
bool check_inf(const Bytes& d, const std::string& base, std::string& why) {
    if (d.empty()) return bad(why, "is empty");
    if (std::find(d.begin(), d.end(), std::uint8_t{0}) != d.end()) return bad(why, "contains a NUL byte (a text file)");
    const std::string text(d.begin(), d.end());
    bool sections = false, last_section = false, last_key = false;
    for (std::size_t at = 0; at < text.size();) {
        std::size_t nl = text.find('\n', at);
        if (nl == std::string::npos) nl = text.size();
        std::string_view line(text.data() + at, nl - at);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
        at = nl + 1;
        if (line.size() >= 3 && line.front() == '[' && line.back() == ']') {
            bool ok = true;
            for (const char c : line.substr(1, line.size() - 2)) ok = ok && (std::isalnum(static_cast<unsigned char>(c)) || c == '_');
            if (ok) {
                sections = true;
                last_section = last_section || line == "[EVENT0910]";
            }
        } else if (line.rfind("AREA", 0) == 0) {
            std::string_view rest = line.substr(4);
            while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t')) rest.remove_prefix(1);
            last_key = last_key || (!rest.empty() && rest.front() == ':');
        }
    }
    if (!sections) return bad(why, "has no [section] headers");
    if (base == "nukkoro2.inf" && !last_section) return bad(why, "is cut off: its last section [EVENT0910] is missing");
    if (base == "nukkoro.inf" && !last_key) return bad(why, "is cut off: its last setting (AREA) is missing");
    return true;
}

// Movies (.sfd, and the .flo files Reloaded ships): an MPEG program stream that starts with a pack header and ends with
// the program end code, padded with 0xFF to the sector.
bool check_movie(const Bytes& head, const Bytes& tail, std::uint64_t size, std::string& why) {
    if (size < 2048 || head.size() < 4 || head[0] != 0 || head[1] != 0 || head[2] != 1 || head[3] != 0xBA)
        return bad(why, "does not start with an MPEG pack header (not a movie, or damaged)");
    std::size_t k = tail.size();
    for (std::size_t i = 0; i + 4 <= tail.size(); ++i)
        if (tail[i] == 0 && tail[i + 1] == 0 && tail[i + 2] == 1 && tail[i + 3] == 0xB9) k = i;
    bool clean = k != tail.size();
    for (std::size_t i = k + 4; clean && i < tail.size(); ++i) clean = tail[i] == 0xFF;
    if (!clean) return bad(why, "does not end with the MPEG end code (truncated movie)");
    if (size % 2048 != 0) return bad(why, "is not a whole number of 2048-byte sectors (truncated movie)");
    return true;
}

bool check_small_kinds(const std::string& ext, const std::string& base, const Bytes& d, std::string& why) {
    if (ext == ".tp") {
        const std::uint64_t want = (base == "mc_bunner_e.tp" || base == "mc_bunner_j.tp") ? 6144
                                 : (base == "mc_icon01.tp" || base == "mc_icon02.tp" || base == "mc_icon03.tp") ? 2048 : 0;
        if (want != 0 && d.size() < want) return bad(why, "is " + num(d.size()) + " bytes, the memory card picture is " + num(want));
        return true;
    }
    if (ext == ".bnr") {
        const std::uint64_t want = d.size() >= 4 && std::memcmp(d.data(), "BNR1", 4) == 0 ? 0x1960
                                   : d.size() >= 4 && std::memcmp(d.data(), "BNR2", 4) == 0 ? 0x1FA0 : 0;
        if (want == 0) return bad(why, "is not a disc banner (no \"BNR1\"/\"BNR2\" signature)");
        if (d.size() < want) return bad(why, "is " + num(d.size()) + " bytes, a banner of this kind is " + num(want));
        return true;
    }
    return true;
}

// True when the kind is one this checker covers (the .one/.txd/.dff/.afs/.adx kinds have their own code below).
bool is_data_kind(const std::string& ext) {
    for (const char* k : {".dat", ".inf", ".fnt", ".met", ".gncp", ".mlt", ".bin", ".scr", ".fog", ".bnr", ".sfd", ".flo", ".tp", ".srd"})
        if (ext == k) return true;
    return false;
}

bool check_data_file(const fs::path& file, const std::string& ext, std::string_view game_path, std::uint64_t size, std::string& why) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return bad(why, "cannot be read");
    if (ext == ".sfd" || ext == ".flo") {
        Bytes head(static_cast<std::size_t>(std::min<std::uint64_t>(size, 2048)));
        Bytes tail(head.size());
        in.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
        in.seekg(static_cast<std::streamoff>(size - tail.size()));
        in.read(reinterpret_cast<char*>(tail.data()), static_cast<std::streamsize>(tail.size()));
        if (!in) return bad(why, "cannot be read");
        return check_movie(head, tail, size, why);
    }
    if (size > (64ull << 20)) return bad(why, "is far larger than any file of its kind (" + num(size) + " bytes)");
    Bytes d(static_cast<std::size_t>(size));
    if (size != 0 && !in.read(reinterpret_cast<char*>(d.data()), static_cast<std::streamsize>(size))) return bad(why, "cannot be read");
    std::string base = std::string(game_path);
    if (const std::size_t slash = base.find_last_of("/\\"); slash != std::string::npos) base.erase(0, slash + 1);
    base = lower(base);
    if (ext == ".dat") return check_dat(d, base, why);
    if (ext == ".bin") return check_bin(d, base, why);
    if (ext == ".fnt") return check_fnt(d, why);
    if (ext == ".met") return check_met(d, why);
    if (ext == ".gncp") return check_gncp(d, why);
    if (ext == ".mlt") return check_mlt(d, why);
    if (ext == ".scr") return check_scr(d, why);
    if (ext == ".fog") return check_fog(d, why);
    if (ext == ".srd") return check_srd(d, why);
    if (ext == ".inf") return check_inf(d, base, why);
    return check_small_kinds(ext, base, d, why);
}
```

## 8. Reproduce

```
# false positives (errors must be 0; --warnings lists the explained ones)
python -X utf8 -I tools/mods/check_formats.py scan --root <game>/files [--root <NTSC-U>/files] [--root <mods>] --rules test_data/mods/format_rules.json --warnings
python -X utf8 -I tools/mods/check_formats.py report --rules test_data/mods/format_rules.json --out test_data/mods/format_results.json --corpus PAL=<game>/files --corpus testmods=<mods> ...
# detection (writes the mutated trees under --work/<mutation>/<game path>, exit 0 when only the documented harmless pairs are missed)
python -X utf8 -I tools/mods/check_formats.py mutate --root <game>/files --work <scratch> --rules test_data/mods/format_rules.json --per-kind 12 --write --json out.json
# derived constants
python -X utf8 -I tools/mods/check_formats.py rules --root <PAL>/files --root <USA>/files --root <JP>/files --out test_data/mods/format_rules.json
python -I tools/mods/check_formats.py budget --layered --root <game>/files --root <mod>/files
# C++ against Python
clang++ -std=c++20 -O1 -Wall -Wextra tools/mods/format_checks_test.cpp -o format_checks_test
python -X utf8 -I tools/mods/check_formats.py list --root DIR | sort   vs   format_checks_test DIR | sort
```

Numbers in this document come from the runs of 2026-11-14 (inputs: `game/files`, `EXTRACTED_DISKS/USA`, `EXTRACTED_DISKS/Japan`, the 14
installed test mods and the raw download repos). Nothing here is read from the unlicensed external decomp; reader addresses are from this repo's
docs (docs/re/*.md, docs/distant-rendering/*.md) and the derivations of sections 3.1 (camera size), 3.11 (`.srd`) are this agent's.
