# Formats: .CCL .PTB .PTP .BIN (and .ADB .SNB .BDT .CEN, hint / light / setid) byte level (S2 `formats`, round 4, 2026-11-12)

Branch `s2/formats` (cut from `s2/integrate` efc1aa8). Names are this project's. The unlicensed external decomp was not opened.
Evidence: **P** proven (a real file under `game/files` through `tools/re/formats_validate.py`, or disassembly of the pinned PAL
`main.dol`), **I** inferred, **U** unknown. Archive layer and PRS: `docs/re/files.md`; RenderWare / motion streams:
`docs/re/assets.md` (not repeated); effects `.EFD` and CSD `.GNCP` belong to other docs. Where a format belongs to a gameplay system
(object parameters, collision shapes, camera, paths) only its bytes and the reader address are mapped, not the logic.

## 1. Census (P, every file of `game/files`, loose and inside the 1,244 `.one`)

| Family | Streams | Bytes | What | Where |
|---|---:|---:|---|---|
| `.CCL` | 1,289 | 263,462 | collision shape list (container) | 1,289 members of `*_gdt.one`, `vehicleResource.one`, enemy / boss `.one` (71 archives) |
| `.PTB` | 16 | 10,954 | one path (container) | `EnemyPath.one` and `*_gdt.one` (`*GRINDOBJ_GR.PTB`) |
| `.PTP` | 60 | 1,047,845 | path set of a stage (container) | `PATH.PTP` in 60 `stgNNNN_dat.one` (958 paths, 30,437 points) |
| `.BIN` members | 456 | 547,216 | gadget definition (container), 143 distinct class names | `stgNNNN_gdt.one`, `stg_cmn_gdt.one` |
| `.bin` loose containers | 24 | 81,776 | `common/ScoreData.bin`, 23 `sound/AudioData*.bin` | `game/files` |
| `*_hint.bin` loose | 17 | 2,102 | hint timer table (container without pointers) | `stgNNNN/` |
| `.ADB` | 364 | 98,888 | texture-index animation (little-endian) | stage / boss / event archives |
| `.SNB` | 222 | 1,147,200 | `NSIF` camera motion (little-endian) | character, enemy, event archives |
| `.BDT` | 62 | 41,528 | stage box table (little-endian) | `stgNNNN_dat.one` |
| `.CEN` | 66 | 4,229 | CRLF text table | event archives |
| `stgNNNN_light.bin` / `setid.bin` | 41 / 1 | 34,112 / 3,632 | fixed blobs (little-endian), already sized by `files_setaudit.py` | loose |
| **Total** | **2,618** | 3,282,944 | | **0 anomalies** |

Container members: 1,862 blocks (1,289 + 16 + 60 + 456 + 24 + 17), 28,218 relocated words in total (CCL 1,918, PTB 32, PTP 2,874, BIN 21,479, loose
bin 1,915; hint 0). Extensions that only occur as part of larger bundles, `.EFD` `.GNCP`, `.RG1` ... are other fields'.

## 2. The "1B" relocatable container (P): `.CCL .PTB .PTP .BIN` and the loose `.bin`

All four extensions (and `ScoreData.bin`, `AudioData*.bin`, `*_hint.bin`) share one container; the guest splits it with ONE function.
Big-endian, 0x20-byte header, then four sections, then a string:

| Offset | Size | Field | Rule (validator id) |
|---|---|---|---|
| 0x00 | 4 | file size | equals the length (C04) |
| 0x04 | 4 | data length `D` | multiple of 4, section end inside the file (C06 C07) |
| 0x08 | 4 | relocation stream length `R` | multiple of 4 (C06) |
| 0x0C | 4 | table count `c` = 1 | (C05) |
| 0x10 | 4 | table count `d` = 0 | (C05) |
| 0x14 | 4 | `00 00 31 42`: the two bytes `'1' 'B'` | the guest tests the byte at +0x17 == 'B' (big-endian marker, anything else makes it byte-swap every word); C02 |
| 0x18 | 8 | zero | (C03) |
| 0x20 | `D` | data | structures, floats, strings; all pointers are offsets from 0x20 |
| 0x20+`D` | `R` | relocation stream | below (C08 C09 C12) |
| then | 8 | table C: one 8-byte entry, zero on all 1,862 blocks | (C10) |
| then | rest | exactly one NUL-terminated printable string | export name or source path (C11); `SCENE` on all 497 BIN-family blocks, a Windows export path on 1,231 CCL, all PTB / PTP, the member name (without extension) on 58 CCL |

**Relocation stream** (P, reader loop 0x8040F890..0x8040F9A8): a byte string over a running word position starting at 0. Code byte `c`:
`00xxxxxx` no-op (padding), `01xxxxxx` delta = c & 0x3F; `10xxxxxx` delta = ((c & 0x3F) << 8) | next byte; `11xxxxxx` delta =
((c & 0x3F) << 24) | next three bytes (big-endian). A non-zero code adds `delta * 4` to the position and rebases the word there: if the word
is non-zero it becomes `word + data start` (a zero word stays zero, so a pointer to offset 0 cannot exist). On the disc every position is
strictly ascending (no word is fixed twice), inside the data, every relocated word is non-zero (BIN rule B02), and the stream ends in
zero padding (C12).

**Guest reader (P)**: `0x8040F66C(obj, buffer)` builds a 0x28-byte descriptor from the buffer (byte +0 = 0, byte +1 = marker, then
+4 `D`, +8 `R`, +0xC `c`, +0x10 `d`, +0x14 data start, +0x18 stream start, +0x1C table C, +0x20 table D, +0x24 string) and calls the
relocation pass `0x8040F868(data, stream, R, marker)`. `GetObject` returns descriptor +0x14, the relocated data. The destructor
0x8040F5CC frees the five pointers only when byte +0 is set, which the parser never does (I: the buffer belongs to the loader).

| Call site of 0x8040F66C | In | What it loads |
|---|---|---|
| 0x80014238 | 0x80013C30 (`ResourceOneFile` per-member dispatcher, extension test `.PTB` 0x80014188, `.CCL` 0x800141C4) | `.PTB`, `.CCL` members, resource type 6 |
| 0x8006AAB0 | 0x8006A3C0 (`ResourceOneFileReq`, `.PTB` 0x8006AA00, `.CCL` 0x8006AA3C) | the same two for Req resources |
| 0x8006A360 | 0x8006A23C (Req `sub6`) | `.BIN` members, type 6 in the Req entry (+0x28) |
| 0x80177B54 | 0x80177B30, callback of the stage archive completion 0x80177964 (name test against the string `PATH.PTP` 0x804C6EC8) | `PATH.PTP` |
| 0x802068B0 | 0x80206868 | `common/ScoreData.bin` (whole-file reader 0x800450C4) |
| 0x801D8BB8 | 0x801D8AFC | `sound/AudioData*.bin`, 8 slots of 0x10 bytes `{w0, w1, file buffer, descriptor}`; first two words of the data are copied to the slot |
| 0x802F3448 | 0x802F33BC | `stg%04d/stg%04d_hint.bin` (format string 0x8050AE88): buffer r13+0x5378, descriptor r13+0x537C, data r13+0x5380 |

## 3. `.CCL` collision shape list (P bytes; semantics of the shapes: re-gameplay)

Data = array of 16-byte records `{u32 kind, u32 shape pointer, u32 flags, u32 id}` ended by one all-zero record, then the shapes,
back to back **in record order**, each of a fixed size by kind (L01 L04):

| kind | shape bytes | records on the disc | shape words (floats unless 0) |
|---|---:|---:|---|
| 0 | 16 | 321 | 4 words, mostly zero |
| 1 | 40 | 651 | origin 3 f32, extents 3 f32, 4 zero words (box-like) |
| 2 | 36 | 692 | origin 3 f32, 2 sizes, angle (radians appear: 1.5708) |
| 3 | 36 | 211 | same size as kind 2 |
| 4 | 40 | 43 | like kind 1 with an angle |

The relocation stream fixes exactly the pointer word of each record (L02: positions 4, 20, 36 ...); record count per file 1 (922), 2 (230), 3
(78), 4+ (59, max 11). `flags` is a packed word (`0x10000202` is the commonest of many values), `id` equals the record index in
588 of the 1,289 files and is another number elsewhere (3212, 3222 in `GUNBIGFOOT`); meaning **U**. All shape words are finite floats (L05). The string is the export path or object name. Because the shape area is
tiled exactly (no gap, no overlap, ends at the data end), a wrong kind, a moved pointer or a swapped record is detected (controls).
Consumer (which shape type the kind numbers are, how `flags` select it): object classes, **U**, re-gameplay's.

## 4. `.PTB` path and `.PTP` path set (P)

One **path** = a 0x30-byte header followed by its points:

| Offset | Field |
|---|---|
| 0x00 | u32 point count `n` |
| 0x04 | f32 path length, equal to the sum of the points' segment lengths (all 974 paths, tolerance 1 %) |
| 0x08 | pointer to the points (always header + 0x30) |
| 0x0C | u32 flags (21 distinct values on the disc, for example 0x1, 0x200, 0x400, 0x2001, 0x2005, 0x400001) |
| 0x10 | f32 x3 bounding box maximum; all points lie inside the box |
| 0x1C | u32 path id (PTP: 0 .. 250, mostly the path's number; PTB: 0) |
| 0x20 | f32 x3 bounding box minimum |
| 0x2C | pointer to the path name |
| 0x30 | `n` points of 32 bytes: position 3 f32, a second vector of 3 f32 (not unit length), f32 segment length to the next point, u32 attribute (0 .. 7) |

The last point has segment length 0 and attribute 0 (P01, all 974). **PTB** = one path at data offset 0, its name (the export path) directly after the points, followed by zero padding to a multiple of 4; relocation positions are exactly {8, 0x2C} (P04) and the pool string equals the in-data name
(P05). **PTP** = a list of path pointers ended by a zero word, padded with zero words to a multiple of 16 bytes (P11), then the paths tiled
head to tail (P12), then all names packed as NUL-terminated strings in path order, then zero padding to a multiple of 4 (P15 P16);
relocation positions are the list entries plus {points, name} of every path (P14). 958 paths in 60 files; 25 of the files have none (a
16-byte data block). Names such as `stg0100_jn_cc_010`, `path101_jn`, `stg0200exlpa_lp`. Reader: container parser (section 2) with the
stage callback 0x80177B30; the path consumers are gameplay's.

## 5. `.BIN` gadget definitions and the other BIN-family containers (P)

Data is a graph of 4-aligned structures and a **string area** at the end:

- Every relocated word is non-zero, the stream is ascending and unique, and every target is inside the data (B02 B03).
- The string area starts at a 4-aligned offset `R` (B09, 480 of 480 blocks that contain strings; 17 hint tables have none), is tiled exactly by
  NUL-terminated printable strings followed by zero padding (B05), every string start is the target of a pointer (B07), every pointer
  target at or above `R` is a string start (B06), every target below `R` is a 4-aligned structure address (B04). 9,253 strings in the 456
  members, 1,885 in the 24 loose containers; 3,398 distinct structure targets in the members.
- Strings that end in a resource extension name a member: **DFF** 2,703 sites, **CCL** 1,135, **BON** 256, **UVA** 338, **MTN** 69, **MTP**
  189, **ADB** 76. All resolve inside the object's own `*_gdt.one` except 18 DFF, 5 CCL, 4 ADB, 3 UVA, 3 BON and 1 MTP, which resolve in
  another archive (shared with the common set), and **12 that exist in no archive** (dangling references of the disc, listed in section 8).
  Example: `CITYCHAOSEMERALD.BIN` names `CT0100CHAOSEMERALD.DFF` and `CT0100CHAOSEMERALD.CCL` (B08).
- The top of every member is a table of `{pointer, count}` style entries into arrays of equal-size parameter records, laid out per object class
  (class = the member name, e.g. `ARTISTGADGET`, `STICK`, `FOOTINGMOVABLE`, `DOOR`); the class's own constructor reads it by name (the
  string `BDTRUE.BIN` at 0x8050C608 belongs to `BDTrue::BDTrue`). The per-class field meaning is **U** and gameplay's; the validator pins
  the structure, not the fields.

`*_hint.bin` (17): data `n * 12 + 12` bytes of records `{u32 message group, link word (-1 or 0), f32 seconds}` ended by `{-1, -1, 0}`;
no relocation. The group numbers are those of `docs/re/text.md` section 4 (for example 0xA0..0xA3 = 160-163 in stg0210, 0xDA..0xDE in stg0410, 0x12C..0x130 in stg0613, 0x19E..0x1B6 in stg0710). Rules H01-H06.

`ScoreData.bin` (4,522 bytes) and `AudioData<Name>.bin` (23, 586 .. 5,366 bytes, 8 slots in the loader) obey the same B rules; their field
layouts belong to scoring (re-gameplay) and `docs/re/sound.md`.

## 6. `.ADB` texture-index animation (P, little-endian; reader swaps in place)

`{u32 duration, char name1[32], char name2[32], u32 zero, u32 n, n * {u32 frame, u32 value}}`, length exactly 0x4C + 8n (A02 A03). name1 is
`name2` or `name2.<digits>` (A05, 363 `.1`/`.01`/`.2`/`.10` forms and 1 without suffix, `S0601_ZA012`), name2 a texture-like name such
as `s0610_zn036` (the member's own name in 240 of 364); a name field is a NUL-terminated printable string followed by stack residue in 25
files (benign, ignored). Frames strictly ascend, the duration is not before the last frame (101 end exactly), value >= 1, n >= 2
(values up to 32; the long ones run sequences such as 1,2 ... 8 ... 1). 8,903 keys. Reader: `0x8004FEE0(list, data)` swaps the words at +0, +0x44, +0x48 and
every key (not the names) with 0x80043D34 and appends the node to the texture-change manager (getter 0x80013BC4); called from the stage
archive completion 0x80177964 (name test `ADB`), the sync dispatcher (0x80014414), Req (0x8006ACA8). Boss code names
`S0610_ZN036.ADB` / `S0617_ZN061.ADB` directly (0x80283270).

## 7. `.SNB` camera motion (`NSIF`), `.BDT`, `.CEN`, light / setid (P)

**.SNB** (little-endian, loader swaps then relocates): `NSIF` info chunk `{NSIF, 0x18, 1, 0x20, dataLength, NOF0 offset, 0x30, 1}` (S01 S02),
chunk `NSMC {tag, payload size, root offset, 0}` at 0x20 ending exactly at the NOF0 offset (S07), `NOF0 {tag, size, count, 0, count offsets}`
(size = count + zero + offsets rounded to 8; offsets ascending, relative to 0x20; S04 S06), padding to a 16-byte boundary (S16), `NEND` chunk of
16 bytes ends the file (S05). The root (32 bytes, type `0x10010002`, track count 6 or 7, pointer to the track array, f32 values)
follows the 40-byte tracks `{type, flags u16 x2, 0, f32, f32 end frame, f32, count, stride (4, 8 or 16), key pointer}` that follow the key arrays; the
key arrays tile from 0x30 up to the track array (S13), and the NOF0 offsets are exactly the root->tracks pointer and each track->keys pointer (S15).
Key records are `{f32 frame, f32 value}` for stride 8; 59 tracks of type `0x200012` (stride 4) hold packed non-float words (S12). 1,391 tracks,
1,613 relocations. Reader: `0x80010290` (swap NSIF, swap and apply NOF0, then dispatch the chunk tags by their third letter: `C` 0x8000F8B4, `L`
0x800106B4, `M` (NSMC) 0x80011244; marks the file as resolved by copying the string `RSIF` over the tag, 0x805F2880; the chunk loop ends at the tags `NEND` / `NOF0`); the string `NSIF` has a second user 0x80010648 and `NSMC` a second user 0x80011CE8 (both read only as far as their callers); the loaders take the member raw (type 7) in 0x80013C30 (0x80014308) and event scenes (0x80171310, 0x801721D0).

**.BDT** (little-endian): `{char stage[64] ("stgNNNN", zero filled), u32 n, n * {u32 id, f32 min[3], f32 max[3], u32 0x50}}`, length 0x44 + 32n; 1,166
boxes in 62 files, ids 0..98, min <= max, all tails 0x50 (D01-D08). Reader: `0x8003AB90` (swaps the count and every record, clamps
the count to 0xA8, then builds the table in the block manager `0x8003ADB4`), called from 0x80177A94 after the stage archive member name matched
the substring `BDT`; max count in the data 139 (stg0403).

**.CEN** (text): `\r\n`-terminated lines of four signed decimal integers whose first column strictly ascends, last line `END\r\n`; 66 files,
233 data lines (E01-E06). Reader: extension test 0x80171240 and loader 0x8032AA78 from the event scene member walk (0x8017175C), parse
callback 0x8032A588 (a character state machine, no libc scanf).

**stgNNNN_light.bin**: 0x340 bytes, 208 words, each a float in [0,1] or a small integer (F01 F02); reader 0x80048738 (`files.md`).
**setid.bin**: `{0, 302}` + 302 * 12 bytes, ids unique but not sorted (F10 F11; `files.md` section 5).

## 8. Findings (data oddities of the disc, all pinned by the validator)

1. **12 dangling name references** in the BINs (names found in no archive): `CT0202HELI.DFF`, `CT0202HELI_BRK.DFF`, `CT0202HELI.BON`,
   `CT0202HELI_ANM.MTN`, `BS0501TWINSWITCHBODY.DFF`, `BS0501TWINSWITCHBAR.DFF`, `DARKEGG00.DFF`, `DARKEGG01.DFF`, `DARKEGG02.DFF`,
   `DARKEGG.CCL`, `COLITECH.DFF`, `COLITECH.UVA`: objects whose assets were cut from the shipped game. Pinned in `KNOWN_DANGLING`; if one of
   them appears in an archive the validator reports the allow-list entry as stale.
2. `stg0613/stg0613_hint.bin` lists message group 0x130 twice (30.0 s and 60.0 s; the order is 0x12C 0x12D 0x12F 0x130 0x130 0x12E): pinned.
3. Hint ids are not in ascending order (stg0410: 0xDA 0xDB 0xDC 0xDE 0xDD); `setid.bin` ids are not sorted either.
4. `.CCL` object names repeat across stages with identical bytes (for example `CT0000BARRICADE.CCL` in stg0100, 0210, 0400).
5. ADB names carry stack residue after the NUL in 25 files; `S0601_ZA012` has no `.1` suffix.
6. 59 camera tracks carry non-float packed key data (type `0x200012`).

## 9. Tools and exact commands (all Python 3, no dependencies, `python -I`)

| Tool | Use |
|---|---|
| `tools/re/formats_common.py` | member / loose iterator on top of `assets_common.py` (read only), cache `.build/formats/cache.pkl` fingerprinted by path, size, mtime; `Container`, `reloc_stream`, `parse_container` |
| `tools/re/formats_validate.py` | strict validator + census, 12 rule families (C, L, P, B, H, A, S, D, E, F), exit 1 on any anomaly |
| `tools/re/formats_negctl.py` | 73 negative controls on in-memory corrupted real files |
| `tools/re/formats_dump.py <label-substring>` | prints a container with its relocated words marked |
| `tools/re/formats_cache.sh`, `formats_run.sh` | cache build, full run (validator + controls) |

    powershell -NoProfile -ExecutionPolicy Bypass -File tools/agents/capped_run.ps1 -CpuPercent 7 -Lock s2-slot-b -WorkingDirectory H:/ShadowTheHedgehogDiskGC_Extract/wt-s2-formats -Command 'C:\PROGRA~1\Git\bin\bash.exe /h/ShadowTheHedgehogDiskGC_Extract/wt-s2-formats/tools/re/formats_run.sh'

Measured (cold cache, which decodes all 1,244 archives): wall 27.9 s, job CPU 27.6 s = 3.1 % of the machine under the 7 % cap; warm cache: 1.1 s.
Single tools: `python -I tools/re/formats_validate.py --root <game/files> --census [--only CCL,PTB] [--json out]`,
`python -I tools/re/formats_negctl.py --root <game/files>`.

## 10. Negative controls (73, all CAUGHT on a clean pristine sample)

Container (13): truncated 4 bytes; 4 trailing bytes; size field + 4; tag `1B` -> `1L`; header byte 0x1C; table count 2; data length + 4; first relocation
delta turned into a repeat (same word fixed twice); byte after the last relocation code; relocation past the data; table C byte; pool
NUL removed; extra NUL in the pool. CCL (5): kind 5; shape pointer + 4; records swapped; NaN in a shape; terminator made non-zero. PTB (8): count + 1; length
+ 50; points pointer + 16; bounding box lowered; last point keeps a length; name pointer + 4; padding byte set; NaN position. PTP
(5): list entries swapped; list not terminated; padding word set; second name pointer + 1; name area truncated. BIN (8): structure pointer + 1; pointer
past the data; relocated word zeroed; string pointer + 1; first string unreferenced; byte after the last string; model name changed to a missing member (needs the archive
member list); ScoreData pool changed. HINT (4): terminator; negative value; link 5; duplicate group. ADB (7): key count + 1; keys out of order;
duration low; name unterminated; word 0x44; value 0; second name changed. SNB (10): magic; NOF0 count + 1; NOF0 offset altered; NEND lost; stride 12; key pointer
+ 8; root type; NaN key; NSMC size + 8; padding byte after the root. BDT (5): count + 1; min above max; tail word; stage name; NaN. CEN (4): END removed; LF line ends; non-integer;
first column descending. Fixed blobs (4): light length - 4; light NaN; setid count + 1; setid duplicate id.

## 11. Open questions

- Per-class field meaning of every BIN (143 classes) and the flag word of CCL records / shape kinds 0-4 (re-gameplay: the constructors read them by name).
- The descriptor words +0x20 / +0x24 (table D, string) have no reader found; table C is always empty.
- `.ADB` value meaning (texture slot index inside the named texture, I) and which TXD name2 matches.
- SNB track type / flag words and the `C` / `L` chunk handlers (0x8000F8B4, 0x800106B4): no file of the disc uses them.
- `.CEN` column meaning (frame number then three values, I).

## S2 report

**Proven.** Sections 1-7 for every one of the 2,618 streams: 0 anomalies; the container, relocation stream and tiling rules hold on 1,862 blocks; 73/73 negative
controls caught, 0 pristine samples dirty. **Inferred.** Meaning of ADB values, CEN columns, the BIN `SCENE` string. **Unknown.** Section 11.
**No owner, no removal, no shared-list line, no claim in `s2_claims.tsv`**: documentation and validators only; the field wrote nothing under `runtime/`.

Commits (branch `s2/formats`): docs first (`docs/re/formats.md`, `docs/re/text.md` section 14), then the tools. Commands: section 9.
