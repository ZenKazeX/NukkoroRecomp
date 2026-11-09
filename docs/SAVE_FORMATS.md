# Shadow the Hedgehog save formats across consoles (2026-11-05)

This document covers the universal save recognizer, the first stage of the user's request: "recognize saves from
every region and every console, and offer to convert them into save slots". It records what each of the user's 19
sample saves is, how it is unwrapped, and where the game's record sits inside it. It also records what is proven
about the PS2 and Xbox records, and what is still unknown. Code: `runtime/include/gekko/save_recognize.h`,
`runtime/src/gekko/save_recognize.cpp`. Tests: `runtime/tests/save_recognize_tests.cpp` (target
`save_recognize_tests`).

**How the evidence was gathered.** The samples were inspected read-only, as copies in a scratch folder. They were
first decoded by an independent Python prototype (pure-Python DES and MD5, a port of Okumura's LZARI, Python's
`zipfile`), then by the C++ module. The two agree on every sample. Facts marked [V] were checked on the bytes.
Facts marked [I] are inferred. **Unknown** means it was not established, and nothing in the code depends on a guess
about it.

The sample folder (read-only): `H:\ShadowTheHedgehogDiskGC_Extract\shadow save files for universal save recognizier`.

## 1. Per-sample table

The "Record" column says where the game's record is inside the unwrapped file:
- GC: at GCI + 0x40 + 0x2040, 0x1F5C bytes;
- PS2: the first 0x1C40 bytes of the 0x2000-byte data file;
- Xbox: at `Save.dat` + 0x18, 0x1F5C bytes.

| Sample | Platform | Container | Region (product) | Unwrapping | Record | Valid | Converts |
|---|---|---|---|---|---|---|---|
| `.10128.gci` | GameCube | GCI | NTSC-U (GUPE) | none | decrypted, MD5 ok | yes | yes |
| `.10569.gci` | GameCube | GCI | PAL (GUPP) | none | decrypted, MD5 ok | yes | yes |
| `.10583.gci` | GameCube | GCI | NTSC-U (GUPE) | none | decrypted, MD5 ok | yes | yes |
| `.18165.gci` | GameCube | GCI | PAL (GUPP) | none | decrypted, MD5 ok | yes | yes |
| `.21761.gci` | GameCube | GCI | NTSC-J (GUPJ) | none | decrypted, MD5 ok | yes | yes |
| `.26078.gci` | GameCube | GCI | NTSC-U (GUPE) | none | decrypted, MD5 ok | yes | yes |
| `.34551.gci` | GameCube | GCI | PAL (GUPP) | none | decrypted, MD5 ok | yes | yes |
| `.34710.gci` | GameCube | GCI | PAL (GUPP) | none | decrypted, MD5 ok | yes | yes |
| `shadow_the_hedgehog_usa.gci` | GameCube | GCI | NTSC-U (GUPE) | none | decrypted, MD5 ok | yes | yes |
| `.14395.max` | PS2 | Action Replay MAX | NTSC-U (SLUS-21261) | LZARI, 5 files | encrypted, key unknown | yes (CRC-32) | no |
| `.28240.max` | PS2 | Action Replay MAX | NTSC-U (SLUS-21261) | LZARI, 5 files | encrypted, key unknown | yes (CRC-32) | no |
| `.32061.max` | PS2 | Action Replay MAX | NTSC-J (SLPM-66166) | LZARI, 5 files | encrypted, key unknown | yes (CRC-32) | no |
| `.32155.max` | PS2 | Action Replay MAX | NTSC-J (SLPM-66166) | LZARI, 5 files | encrypted, key unknown | yes (CRC-32) | no |
| `.18297.cbs` | PS2 | CodeBreaker | PAL (SLES-53542) | **not unwrapped**: RC4 key unknown | not reached | no | no |
| `.32290.xps` | PS2 | SharkPort / X-Port | PAL (SLES-53542) | uncompressed, 5 files | encrypted, key unknown | yes | no |
| `.32060.zip` | PS2 | ZIP (deflate) → PSV `BISLPM-66166.PSV` | NTSC-J (SLPM-66166) | **not unwrapped**: broken file table | not reached | no | no |
| `.32156.zip` | PS2 | ZIP (deflate) → PSV `Shadow-the-hedgehog_jpps2/BISLPM-66166.PSV` | NTSC-J (SLPM-66166) | PSV file table, 5 files | encrypted, key unknown | yes | no |
| `.10027.zip` | Xbox | ZIP (deflate) → `UDATA/53450086/25575D9B5D04/Save.dat` | **unknown** (title 53450086) | none | encrypted, key unknown | yes (length field) | no |
| `.34727.zip` | Xbox | ZIP (deflate) → `UDATA/53450086/25575D9B5D04/Save.dat` | **unknown** (title 53450086) | none | encrypted, key unknown | yes (length field) | no |

Notes on the table:
- The sample names start with `shadow-the-hedgehog`, shortened to the dot above.
- "Valid" means every integrity check that can run offline passed. A save whose record could not be reached is not
  valid.
- `save_recognize_tests <sample folder>` prints the same table, and checks each row against the table in the test
  (19/19, 2026-11-05).

**Duplicates and relations found.** The test checks the [V] pairs below.
- `.26078.gci` and `shadow_the_hedgehog_usa.gci` are byte-identical. [V]
- `.10569.gci` (GUPP) and `.10583.gci` (GUPE) have identical data (comments, banner, icon, record). Only the header
  differs: someone relabelled the region. [V] Whether the relabelled file loaded on a console is unknown.
- `.32155.max` and `.32156.zip` hold the same JP PS2 data file. [V]
- `.32061.max` and `.32060.zip` hold the same JP PS2 data file, at PSV offset 0x130, which the PSV's own table does
  not point to. This was found by comparing bytes during analysis. The code does not search for it (see §3.4).
- The CodeBreaker `.18297.cbs` stores the title "BF2:MC(SP)", but its directory name is BESLES-53542, which is this
  game's PAL product. [V] The writer apparently left the title field stale.

## 2. GameCube

### 2.1 Containers

**GCI** [V]: a 64-byte directory entry, big-endian as on the card, then the file's 8 KiB blocks.
- The entry: 0x00 game code (4), 0x04 maker "8P", 0x08 file name "SHADOWTHEHEDGEHOG", 0x28 modified time, 0x2C icon
  address, 0x30 icon format, 0x32 animation speed, 0x34 permissions, 0x35 copy count, 0x36 first block, 0x38 block
  count (2), 0x3C comment address.
- The region comes from the game code's last letter: E = NTSC-U, P = PAL, J = NTSC-J. Any other letter is reported
  as unknown and is not converted.
- `.34551` and `.34710` carry odd header values: icon format 0x5556, speed 0xFFF2, copy count 1, first block 0xCF.
  The game ignores these. The conversion normalises them.

**Raw memory-card image** (a Dolphin `.raw`, or the PC port's `.pc.raw`): read with `save_file::extract_from_card`
for each of GUPE / GUPP / GUPJ. No sample is a card image. This path is covered by fixtures built in the tests.

Not handled: GameShark `.gcs` and MaxDrive `.sav`. No sample of either was available, so neither format was
verified. They are reported as "not a save container this recognizer knows".

### 2.2 The record is the same in every region

The record is at file data + 0x2040 (GCI + 0x2080) and is 0x1F5C bytes:
- DES-ECB with key EE B0 6C AE A4 78 49 90 over its first 0x1F58 bytes; the last 4 bytes are not encrypted.
- An MD5 of record[0x10 .. 0x1F5C), padded with 0x00, stored at record[0 .. 0x10).
- Fields are big-endian.

Evidence:
- The MD5 matches, under the same key, on all 9 GC samples: 5 NTSC-U, 3 PAL and 1 NTSC-J. [V]
- The key EEB06CAEA4784990 appears at the same table in the US DOL (0x57D7C0), the JP DOL (0x57DE28) and the PAL DOL
  (`game/sys/main.dol`, 0x57E848). In each DOL it follows the strings `8P\0\0GUPJGUPEGUPP`. [V]
  - [I] This is one game-code table, indexed by region, in code shared by all three builds. So the record code is
    the same across regions.
- `li/addi rX, 0x1F5C` occurs in all three DOLs. [V] The counts differ (US 12, JP 10, PAL 25), and the call sites
  have not been read, so this supports the claim but does not prove it on its own.
- The options fields of all 9 samples read sensibly: text language, voice, speaker, vibration, and the stamp at
  +0x1774 (a big-endian u32, then 0). [V] The JP sample has text 1 (English), voice 0 (Japanese).

### 2.3 Conversion to the PC GCI (`to_pc_gci`)

The save store (`save_store.cpp`) matches the file by the game code and maker that the PAL build asks for (GUPP /
8P). The conversion therefore:
- **keeps the 16 KiB of data byte for byte**: comments, banner, icon and record. The record is not re-encrypted,
  because the key and the MD5 are region-independent (§2.2);
- **rewrites the 64-byte entry** as the save store's own create does: GUPP, 8P, SHADOWTHEHEDGEHOG, banner format 2,
  icon address 0x40, icon format / speed 0x0002, permissions 0x04, copies 0, first block 5, 2 blocks, 0xFFFF, comment
  address 0. It keeps the source's modified time.

It refuses:
- a record whose MD5 does not match;
- an interrupted create (icon address 0xFFFFFFFF);
- a file that is not 2 blocks;
- an unknown region letter.

Open points for GC conversion:
- **The comments and banner stay in the source's language.** For example, the JP save keeps its Shift-JIS
  comments. The game rebuilds the comments, banner and icon on its next save (TMemoryCardCtrl builds the whole file),
  so this is cosmetic, and only until then. [I from SAVE_REROUTE_PLAN §1]
- **Unknown:** whether the stamp check ("not the original card", message 125, result 11) compares the record's stamp
  with anything outside the record. If it does, a converted save could trip it. This needs a load test in the game.
- **Unknown:** what the PAL build does with a JP save's voice language 0 (Japanese) and text language values. The PC
  settings already carry Japanese text (save_store.h, decision C). Voice is not covered there.

## 3. PlayStation 2

### 3.1 The game's PS2 save directory

Every PS2 sample has the same directory, named `B` + a region letter + the product: `BASLUS-21261`,
`BESLES-53542`, `BISLPM-66166`. It holds 5 files: [V]

| File | Bytes | Content |
|---|---|---|
| `<directory name>` | 0x2000 | the game's data file |
| `LIST.ICO`, `COPY.ICO`, `DEL.ICO` | 0xBAB8 each | icons |
| `icon.sys` | 0x3C4 | "PS2D" |

The recognizer accepts only these three products:

| Product | Region |
|---|---|
| SLUS-21261 | NTSC-U |
| SLES-53542 | PAL |
| SLPM-66166 | NTSC-J |

Any other product is reported as another game. Other Shadow PS2 releases, if any exist (budget or Korean), are
unknown: a save from one would not be recognized.

### 3.2 The PS2 record

- **Size.** In all six distinct data files, bytes [0x1C40, 0x2000) are zero, and [0, 0x1C40) looks like 8-byte-block
  ciphertext. [V] So the PS2 record is 0x1C40 bytes, against 0x1F5C on the GC.
- **Cipher.** The cipher works on 64-bit blocks, ECB-like: identical 8-byte blocks repeat on an 8-byte grid. The most
  common block, presumably zero plaintext, is `31FD2AF225B9BF58` in every PS2 sample, US and JP alike. So all PS2
  regions share one key. [V]
  - The GC key does not produce that block. `DES(GC key, 0)` is `B5CF6234554B054F`. [V]
  - Neither do byte-reversed, word-swapped, half-swapped or bit-reversed forms of the GC key, with zero or 0xFF
    plaintext, nor the same transforms of the output. [V]
  - **The PS2 key, and whether the cipher is DES at all, are unknown.**
- **Layout.** The map of zero blocks of a PS2 record was compared with a GC record's.
  - The leading 0x10 bytes, presumably an MD5, look alike.
  - From about 0x30, the layouts differ. The GC record's periodic region (about 0x1200-0x16FF, presumably per-stage
    data) is absent, and other periodic regions appear at 0x300-0xEFF.
  - The best whole-record alignment matches only 73% of blocks, against 99.5% for Xbox (§4).
  - **So the PS2 record layout is not the GC layout.** Field order and sizes differ. Endianness is unknown, but the
    PS2 is little-endian, so [I] the fields are probably little-endian.
- **Conversion: none.** The record cannot be decrypted, and its layout differs. A field mapping would need, first,
  the PS2 key (from the PS2 executable, which is not in this repository) and then the field layout, from the PS2
  executable's save code. **Both are unknown.**

### 3.3 Containers

**Action Replay MAX (`.max`)** [V]
- Header:
  - "Ps2PowerSave";
  - +0x0C a CRC-32 of the whole file, taken with this field zeroed;
  - +0x10 the directory name (32 bytes);
  - +0x30 the title;
  - +0x50 the compressed length, counted from +0x58;
  - +0x54 the file count;
  - +0x58 the decompressed length.
- The LZARI stream starts at +0x5C: Okumura's LZARI with N=4096, F=60, THRESHOLD=2, the ring buffer filled with
  spaces, MSB-first bits.
- The decompressed stream holds one entry per file: a u32 length, a 32-byte name, the data. The next entry starts at
  round_up(end + 8, 16) - 8.
- `.32061` and `.32155` store the decompressed length at +0x50. The code notes this ("nonstandard length field") and
  decodes from +0x5C up to the file's end. These two also store the files in reverse order (icon.sys first).
- The CRC-32 matches on all four samples.

**SharkPort / X-Port (`.xps`)** [V]
- Header:
  - u32 13, then "SharkPortSave";
  - u32 0;
  - three length-prefixed strings: title, title, comment ("Made using file converter by ffgriever");
  - a u32 with the length of the rest.
- The directory header: u16 0xFA, a 64-byte name, u32 7 (5 files plus . and ..), modes and times.
- Each file: a header (u16 0xFA, a 64-byte name, u32 the length at +0x42), then the data.
- Four trailing bytes after the stated length are not checked: their checksum form is **unknown**.

**PSV (a PS2 save exported by a PS3)** [V on `.32156`]
- Header:
  - "\0VSP", 0;
  - a seed at 0x08 and a signature at 0x1C. This is the PS3's HMAC, keyed with PS3 keys, and is **not checked**;
  - 0x3C type 2 (PS2);
  - 0x64 the file count;
  - the directory entry at 0x68, with its name at 0x80;
  - file entries from 0xA0, 0x3C bytes each: +0x10 the length, +0x18 the name, +0x38 the offset in the PSV.

**CodeBreaker (`.cbs`)** [V on the header only]
- Header:
  - "CFU\0";
  - +0x04 0x1F40;
  - +0x08 the payload offset (0x128);
  - +0x0C the decompressed length (0x2552C);
  - +0x10 the payload length (0x4D9B, which equals the file length minus 0x128);
  - +0x14 the directory name;
  - +0x5C the title.
- The payload is not plain zlib at any offset 0-7. [V] It is RC4-encrypted zlib under CodeBreaker's fixed 256-byte
  key. That key is not in the repository and was not reproduced from memory; the rules allow no downloads.
- **So the CBS payload is not unwrapped.** Even unwrapped, it would only yield the encrypted PS2 record of §3.2.
  Once the key is supplied, the decoder needs only RC4 and the inflate the module already has.

### 3.4 The broken PSV (`.32060.zip`)

The PSV's file table holds one good entry: `BISLPM-66166`, 0x2000 bytes at 0x1CC. Entries 2-5 are garbage, such as
pointers like 0x0400F000, and the data for entry 1 is not at 0x1CC. At 0x1CC is the middle of a record; the data file
starts at 0x130, byte-identical to `.32061.max`. [V] The icons and icon.sys sit near the end, alongside PS2
memory-card directory entries.

The recognizer reports this file as "PSV, JP, not valid, not unwrapped: the file table is broken, so its offsets
cannot be trusted". It does not search the file for the data, because no rule found here would place it. Whatever
tool wrote this file is unknown.

## 4. Xbox

- **Container** [V]: a ZIP (deflate) holding the Xbox `UDATA` tree:
  - `UDATA/53450086/TitleMeta.xbx`: UTF-16, "TitleName=Shadow The Hedgehog";
  - `TitleImage.xbx`;
  - `SaveImage.xbx`;
  - `datelinfo.xbx`, in `.10027` only: an Action Replay / Datel export's note "Savename=Shadow The Hedgehog
    Complete";
  - `25575D9B5D04/SaveMeta.xbx`: "Name=Shadow Game Saves";
  - `25575D9B5D04/Save.dat`.
- **`Save.dat`**, 0x1F74 bytes [V]:
  - +0x00: a u32 LE holding its own length, 0x1F74;
  - +0x04: 20 bytes, different in each save. [I] Probably the Xbox per-save HMAC-SHA1 signature, which needs the
    title's signature key from the XBE; it is **not checked**;
  - +0x18: the record, 0x1F5C bytes;
  - 0x18 + 0x1F5C = 0x1F74.
- **Cipher.** The Xbox record also works on 8-byte blocks, aligned on the file's 8-byte grid from +0x18. Its most
  common block is `C5766715F7D5AB7E`, a third key. The PS2 and GC transforms of §3.2 do not reach it. [V] **The Xbox
  key, and whether the cipher is DES, are unknown.**
- **Layout.** The positions of that block in an Xbox record were compared with the zero 8-byte blocks of a decrypted
  GC record (`usa.gci` against `.34727`, two different saves). At record offset 0 they match on 99.5% of the 1003
  blocks; the next best alignment matches 96%. The tail is identical too: zero from 0x1778 to the end. [V]
  - So **the Xbox record has the GC record's block layout**: the same size, the same zero and non-zero regions, the
    MD5-like first 16 bytes, the stamp area at 0x1774.
  - This is structural evidence only. Field values, endianness (the Xbox is little-endian, the GC big-endian) and
    whether the MD5 / padding convention is the same are **unknown** until the record can be decrypted.
- **Region: unknown.** Both samples use title ID 0x53450086, and nothing in the save or its folder names a region.
- **Conversion: none.** It needs the Xbox key, then a check of each field's byte order. If the layout holds field for
  field, as §4's map suggests, the mapping would be:
  1. decrypt;
  2. byte-swap each multi-byte field. That needs a field list, which is unknown; a blanket u32 swap is wrong for the
     byte fields such as vibration at +0x24;
  3. recompute the MD5;
  4. encrypt with the GC key.

## 5. Cross-console summary

| | GameCube (all regions) | PlayStation 2 (all regions) | Xbox |
|---|---|---|---|
| Record size | 0x1F5C | 0x1C40 [V] | 0x1F5C [V] |
| Cipher key | EEB06CAEA4784990 (DES-ECB) | unknown, shared by US and JP | unknown |
| MD5 at +0 | yes, 0x00-padded | unknown | unknown |
| Layout vs GC | — | different | same block layout (99.5%) |
| Endianness | big | unknown ([I] little) | unknown ([I] little) |
| Converts | yes | no | no |

## 6. API and hook points for the slot prompt

The in-game "convert these saves into slots?" prompt and the save slots are not part of this task. Here is how they
can call the module:

1. **Find saves.** `scan(folder)` lists every file with a save. Each `Save` carries:
   - its platform, container, region, product and location;
   - `valid`, a one-line `reason`, and `record_state`;
   - `convertible`;
   - a stable `id` (path + "|" + location), for the prompt's selection.

   `identify(path)` does the same for one file. `identify_bytes` does it for bytes already in memory.
2. **Show.** List every save, so the user sees their PS2 and Xbox saves recognised. Offer conversion only where
   `convertible` is true. The `reason` explains the others: key unknown, layout differs, not unwrapped.
3. **Convert.** `to_pc_gci(save, gci)` returns the canonical PC GCI. The slot code writes it with
   `save_file::write_gci`, which is atomic, into the slot's path. The module itself writes nothing, and sources are
   only read.
4. **Options.** `extract_record(save, rec)` returns `rec.options` (language, voice, speaker, vibration, stamp). This
   is for the first fill of the PC settings, as `save_import` does today.

Where it fits:
- `save_import::import_if_missing` (save_import.cpp) is where a one-time "convert what is found" step would run when
  the save file is missing. It currently reads GCIs and card images through `save_file`, and could take the module's
  `scan()` results instead.
- The slots would sit beside `save_import::resolve` (the save path) and the save store's `set_path`.
- Neither file was changed in this task. Other sessions own `save_store.cpp`, `shadow_main.cpp` and `settings_*`.

## 7. Open questions

1. **PS2 and Xbox keys.** They are needed to decrypt either record. Both should be in each console's executable
   (the PS2 ELF, the Xbox XBE), and neither is in the repository.
   - Not yet tried: scanning the GC DOL's constants as candidate keys against the two observed zero blocks. This was
     skipped because a game was running during validation.
2. **The PS2 field layout.** Even with the key, the PS2 record differs from the GC's and needs the PS2 save code.
3. **The CodeBreaker RC4 key**, to unwrap `.cbs`.
4. **The Xbox signature** at Save.dat + 0x04. Is it the standard XCalcSignature (HMAC-SHA1 with the title key)? A
   converted save *to* Xbox would need it; reading does not.
5. **The GC stamp check.** Does message 125 / result 11 compare anything outside the record (§2.3)? Only an in-game
   load of a converted save answers this.
6. **Other PS2 product codes** for this game, and GameShark `.gcs` / MaxDrive `.sav` GC containers. These are not
   supported, for lack of samples.

## 8. Validation (2026-11-05)

- `cmake --preset tests`, then `cmake --build .build/tests --config Release --target save_recognize_tests -j 1`:
  built clean at /W4.
- `ctest -C Release -R save_recognize_tests`: passed. That is 105 checks on fixtures built in the test:
  - CRC-32;
  - raw deflate vectors from zlib (stored, fixed, dynamic);
  - LZARI round trips;
  - GCI and card image in GUPE / GUPP / GUPJ, plus conversion;
  - MAX, XPS, PSV (sound and broken), CBS;
  - Xbox in a ZIP, on disk and with another title;
  - a mixed and nested ZIP, a ZIP with a bad CRC;
  - `scan()`.
- `save_recognize_tests <copy of the 19 samples>`: 186 checks, 0 failures.
  - All 19 samples were identified as §1 records them.
  - Each GC sample was converted in memory, and its record re-checked.
  - The PS2 zero padding was confirmed after 0x1C40.
  - The duplicate pairs were confirmed.
  - The originals were checked unchanged afterwards (MD5 of each file against the scratch copy).
- Not run: the game. Nothing here shows that a converted save loads in the game (§7.5).
