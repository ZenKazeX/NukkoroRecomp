# Turning a code mod into a PAL patch set

Tool: `tools/mods/codemods/patchset.py` (stdlib Python, paths as arguments, reads the DOLs, never writes into `game/`).
Tests: `tools/mods/codemods/test_patchset.py` (19 tests on synthetic DOLs, maps and code lists written for the test; no game or mod bytes).

```
python -I tools/mods/codemods/patchset.py --src-dol <usa>/sys/main.dol --pal-dol game/sys/main.dol \
    --address-map extract/address-maps/GUPE8P.map --pal-functions extract/functions.csv --source-region GUPE8P \
    --out-dir <dir outside the repo> [--mod-dol <replacement main.dol>] [--selftest-relocate] \
    --input gecko-text:code.txt --input gecko-ini:GUPR8P.ini --input gct:codes.gct \
    --input mcm:<dir or file> --input dol-diff:<replacement main.dol>
```

Each `--input` gives `<name>.patchset.json`; `report.json` has the counts. Mod bytes only ever go to `--out-dir`.

## What a patch looks like

`{id, source_region, source_address, address (PAL), old_bytes (PAL, from the PAL DOL), source_old_bytes, new_bytes (relocated, PAL),
kind, role, confidence, section, provenance, condition, notes}`.

| kind | meaning | what the manager does with it |
|---|---|---|
| `static-patch` | fixed address, unconditional, lands in a DOL section (text, rodata, sdata2, or initialized data/sdata) | write into the PAL DOL image of the variant executable |
| `runtime` | conditional, base/pointer relative, state dependent, or the target is bss/heap (not stored in the DOL) | per-frame engine; `condition` has the translated PAL address of every test |
| `code-injection` | `role: hook` (a `b` into a cave) + `role: cave` (relocated code, ends in `b hook+4`) | DOL patch of the variant executable |

Writes into `.data`/`.sdata` are `static-patch` with a note: the DOL holds only the initial value, the game may rewrite it
(apply from the engine instead when the value must persist). `needs_user_value: true` marks codes with `X` placeholders (Jump Ball
Color Switch): the bytes are a template.

## Inputs and what is decoded

* Gecko (ini `[Gecko]`/`[Gecko_Enabled]`/`[ActionReplay]`, plain text, `.gct`): 00/02/04 writes and fills, 06 string, 08 serial,
  20-2E conditionals with E0/E2, C0 exec, C2 insert-asm, C6 branch. Everything that touches the base address, pointer, repeat or
  Gecko registers (40-9F, A0-BF, D0-FF) and every `ba/po` form (bit 0x10 of the type byte) makes the writes `runtime`,
  `confidence: unresolved` (the base is not modelled). AR: plain RAM writes only, other AR types are reported.
  Assumptions (from the usual Gecko code-type document, not checked against a handler): E0/E2 pop `max(1, low byte)` levels, E2 then
  opens an else level (so later writes stay conditional: the safe direction); 08 serial = `value += VVVVVVVV` per step,
  address step `NNNN` bytes; C6 is a plain `b`; bit 0 of the type byte selects 0x81xxxxxx.
* MCM libraries: DOL offsets (file offset, converted with the DOL's section table) or 0x8xxxxxxx addresses; forms `old -> new`,
  `old -> Branch` + cave text (first word = the replaced instruction, last word `00000000` = placeholder for the return branch), and the
  empty-header form (old block, ` -> `, new block).
* DOL diff: changed words against the source DOL; a changed text word that is a `b` into 0x800034AC-0x800053E0 is a hook; changed
  words reachable from hooks (CFG walk) are cave code, the rest of the changed cave words are data (65 words) or zero fill.

## Address translation and confidence

* Code: `extract/address-maps/<region>.map` (function level, verified), else a byte-signature search in the PAL text (masked for branch
  displacements and D-form immediates; widest unique window 17..5 words, near the expected place). Every code address is then confirmed by
  comparing the +-4 instruction words around it (modulo relocation): `high` = at most one word differs, `medium` = at most three, else `low`.
* Data: map segments (proof points), else interpolation when both neighbour segments have the same delta (`medium`), else a byte
  signature of +-32/16/8 bytes (`medium`). In-segment: `high` when the bytes agree between regions (or the address is bss, nothing to compare),
  `medium` when the value differs. Constants outside the DOL image (heap/stack area, for example 0x807D71A0) are relocated into the mod scratch region, see "Scratch region" below.
* r13/r2 are read from `__start` of each DOL; small-data offsets are re-derived against the PAL bases.
* Instruction words written over text (Gecko 04, MCM single words, diff words) go through the same relocator with the surrounding code as
  context: a lone `bl` is retargeted, a lone `ori`/`addi` low half is re-derived against the PAL `lis` (and reported when the PAL `lis` cannot reach).

## Relocating injected code

`relocate_block(words, source_base, pal_base)` decodes every word and rewrites: `b/bl/bc` targets that leave the block (internal ones
keep their displacement), `lis` + `addi|ori|load|store` pairs (also through a register chain, one `lis` feeding several uses must agree on the new
high half, otherwise `unresolved`), and `r13/r2` offsets. Branch displacements of blocks of unknown placement (Gecko C2 body, MCM text
without a mod DOL) that leave the block are `unresolved`. Placement in PAL: same address as in the source DOL when known
(`--mod-dol` for MCM, always for the diff), else allocated from 0x800034AC upward. Verification of the result: every word decodes as a
Gekko instruction (a stray zero word fails), every external branch lands in a PAL function (a `bl` must land on a function start), every
absolute address found by re-scanning the PAL words lies in a PAL section, small-data accesses land in sections. Failures lower the cave
to `low`.

Free space: the cave region differs between the two DOLs and nothing references it in PAL (no branch from outside, no pointer word in data;
`cave_region` in `report.json`). Seven numeric liveness roots of `extract/liveness-roots.txt` (0x80004000/02, 0x80004200/40, ...) fall in it: they
look like literals, not entries; the recompile agent should confirm. The 51 PAL functions listed there (`fn_800034ac`, `pad_800034dc`, ...)
must be dropped from `functions.csv` in the variant build.

## Results on the real inputs (NTSC-U source, PAL target)

Run with `--src-dol` = NTSC-U DOL, `--mod-dol` = Reloaded's DOL; 1.2 s without the self test.

| input | patches | static | runtime | injection (hook+cave) | high | medium | low | unresolved |
|---|---|---|---|---|---|---|---|---|
| No Bloom (`044978f0 60000000`) | 1 | 1 | 0 | 0 | 1 | 0 | 0 | 0 |
| Reloaded `GUPR8P.ini` (33 codes) | 80 | 50 | 18 | 12 (6 caves) | 79 | 1 | 0 | 0 |
| Reloaded MCM libraries (110 entries, 224 sites) | 309 | 139 | 0 | 170 (85 caves) | 289 | 17 | 3 | 0 |
| Reloaded `main.dol` vs NTSC-U | 208 | 117 | 0 | 91 (85 hooks, 1 contiguous cave image, 5 zero-fill runs) | 204 | 2 | 2 | 0 |

* No Bloom: NTSC-U 0x804978F0 -> PAL 0x80498940, nop over a `bl`, window 9/9, the replaced `bl` goes to the same function in both.
* `GUPR8P.ini`: 19 codes static only, 10 runtime only (conditional or bss targets: the Z-button codes test the controller word, the music/HUD/subtitle/race-mode
  writes hit bss), 3 with C2/C6 injection, 1 mixed. The 6 C2 caves (68 instructions) relocate with 0 unresolved items and 0 verification failures; the
  one `medium` cave keeps a heap constant. Jump Ball Color Switch carries `needs_user_value`.
* MCM: 60 entries are static-only, 50 have branch hooks. 85 caves, 1389 instructions: 234 internal branches, 104 `lis` pair rewrites, 9 small-data rewrites,
  0 unresolved items, 0 verification failures, 69 caves `high`, 16 `medium` (13 caves keep 17 heap/scratch constants 0x807D71A0, 0x807D5700, 0x8060B33C/F; 2 caves whose
  data pointers resolve only by byte signature, 0x804E3626 and 0x804D6D7E; 1 hook with a medium window), 0 `low`. The three `low` patches are
  static: Light Dash ValidRing distance (2 words) and Spindash startup delay: the functions are different bodies in the two regions (window 2/9 and 3/9), the map has no
  segment there, and the two words of the library entry map to places that disagree with the DOL-diff run (0x8008A7F0 vs 0x8008A7EC): not usable without manual work.
* DOL diff: 1765 changed words (1516 in the cave area: 1451 code reachable from 85 hooks, 65 data words; 249 elsewhere, of which 85 hooks and 164 inline). Header sections
  unchanged. The cave image relocates with 0 verification failures and the 2 data signature lookups the MCM run also needs.

### Cross-check MCM vs DOL diff

* All 208 diff patches lie within 64 bytes of an MCM site (202), are in an MCM cave (1) or are the zero fill of the
  5 stubs (cave-clear; not emitted, the PAL stubs are unreferenced). So the earlier statement holds: the library texts explain the DOL, plus the zero fill.
* The PAL images agree word for word: 1637 words are in both patch sets, 1599 identical. The other 38 words are 4 library entries
  (SuperShadow chaos spears, two IGT boss-penalty entries, the Diablon IGT entry) whose cave text is not what the Reloaded DOL contains (the check
  "cave body equals the library text in the mod DOL" fails for them: the DOL is a different version of the code). 63 diff words are not in the MCM patch set (not analysed further); 1 MCM word (the second Light Dash word, above) differs in address.

### Ground truth for the relocator (`--selftest-relocate`)

36,817 PAL functions that are identical modulo relocation to their NTSC-U counterparts (1,878,544 words) were relocated from NTSC-U to PAL by the same code that relocates
caves (142,615 external branches, 31,022 `lis` pairs, 30,265 small-data accesses) and compared with the real PAL words: 1,878,449 equal (99.995 %), 95 different (55 `bl`/`b`
to targets inside changed bodies, 23 loads/stores, 13 `addi`, 4 `ori`; includes the `__start` r2/r13 setup, which is expected). The mismatching ones sit in places where the
address map has gaps next to changed functions; the cave verification (target is a PAL function start) is what catches them in mod code.

## Limits and risks

* Nothing here ran in the game. The patch set is a static proposal; the variant executable still has to be built (the hook is a mid-function `b`, the recompiler needs the
  cave as new code and the stub functions removed) and gameplay checked.
* Patches that land in functions the port replaced with native C++ (native owners, lift sites) are not detected here; that is a separate pass over `extract/native-owners*.txt`.
* Scratch contents: the region is assumed zero at the start (measured all-zero at the end of long stock runs, by the coordinator); a mod that expects other initial data is not covered.
* Branches/pairs inside caves are tracked linearly (a `lis` is forgotten at join points, `b`, `blr`, `bl` clobbers r0 and r3-r12). Code that builds an address in an unusual
  way (`addis` with a base register, address in a table) is not seen and stays untouched.
* Gecko semantics are decoded from the standard type table; `ba/po`, repeat and register codes are not interpreted (`runtime`, `unresolved`).
* Combining several mods needs one run with a shared cave allocator (each input gets its own allocation from 0x800034AC now; same-address placement of a replacement DOL
  collides with it).

## Round 2: scratch region and features

### Scratch region (`Scratch`, `scratch` in every patch set, `--scratch-map`)

Constants a mod uses as scratch memory (a `lis`+low-half pair, or a Gecko RAM write/condition, whose value is in 0x80003100-0x81800000 but in no DOL section and not in the
bss) are relocated into the PAL region the coordinator reserves: base 0x817C0000, size 0x30000 (above OSArenaHi 0x817A0000). Two passes: pass 1 collects every
such constant of all inputs of the run, clusters are built, pass 2 emits. A cluster is a run of constants with gaps of at most 0x4000, padded by 0x1000 on both sides and page
aligned on both ends, so offsets inside the cluster and the low 12 bits (alignment) are kept; clusters are placed in address order from the region base. The mapping
is reported as `scratch: [{ntsc_range, pal_range, size, constants, constants_pal}]` in each patch set and in `report.json`. If the region does not fit the tool
stops with `ScratchOverflow` (exit text "scratch region overflow"). `--scratch-map FILE` loads clusters of earlier runs, extends them (new clusters go after the last one,
existing placements never move) and writes them back: pass the same file to every run that is applied to one executable (the manager runs one tool per item, so they
would otherwise all start at the region base and overlap). The same translation applies to C2/MCM/diff cave code (patches say `scratch` in their relocation stats and are `medium`),
and to runtime Gecko codes that write or test such addresses (kind `runtime`, PAL address in the region). Verification requires every such constant of the relocated code to lie
in the region.

What 0x8060B33C/F are: not bss variables. The NTSC bss ends at 0x805FB564 (PAL 0x805FC5EC); stock code uses 0x8060B568 (NTSC) / 0x8060C5F0 (PAL) = bss end + 0x10004 as the stack boundary,
and 0x80808080, 0x817A0000 (OSArenaHi) and others, all region-shifted the same way (these 19 constants are reproduced by the relocator's ground-truth run). 0x8060B33C/F lie 0xFDD8 above
the bss end and 0x22C below that boundary: arena/stack memory the IGT-message entry uses as a text buffer. A bss-end-relative shift would land inside the PAL stack region, so
they go to the scratch region like 0x807D5700/0x807D71A0.

Not relocatable: `r2/r13`-relative accesses beyond the DOL image (Advanced Full Widescreen uses `stw/lfs 0x2000(r2)` = the first bytes after the bss as a float temporary). The offset
cannot reach the region (16 bit), and the same offset in PAL hits the start of the game heap, so those patches are `unresolved` (the cave and the feature too).

### Features

Every patch has `provenance.feature` (name) and `provenance.feature_id` (`m001`.. MCM entries in library order, `g001`.. Gecko codes in file order, `f001`.. otherwise); each
patch set has `features: [{id, name, patches, kinds, confidence (lowest of its patches), needs_user_value?, status?}]`. Skipping single patches of a feature is unsafe: a feature
is applied whole or not at all (`unresolved` = do not apply).

MCM with `--mod-dol` (`--input mcm:<dir> --mod-dol <dol>`; add `--all-entries` for everything): the mod DOL decides which entries are ENABLED. An entry is `enabled` when every
site carries the library's new bytes in the DOL (a `b` into the cave region whose code equals the library text and ends in `b hook+4`), `not-enabled` when the original
bytes are there, `partial` when only some sites are, `version-mismatch` when the hook is there but the cave is not the library text. Only enabled entries are emitted, each patch
tagged with its entry; the other entries are listed in `features` and `extra.entries_detail` with their status and no patches. `extra.residue` lists the DOL-diff words no enabled
entry carries: `cave-zero-fill` (zeroed original stubs), `version-mismatch` (hook/cave words of a version-mismatch entry), `unexplained`.

### Results on Reloaded (NTSC-U source, `--mod-dol` Reloaded's DOL; 3 s)

* MCM: 110 entries: 106 `enabled`, 4 `version-mismatch` (SuperShadow Always Has Fully Charged Spears, Dont Pause IGT during Egg Dealer Attacks, Boss IGT Penalties, Sonic&Diablon IGT
  Penalties), 0 not-enabled, 0 partial. 291 patches of the enabled entries (152 injection, 139 static): 269 high, 17 medium, 3 low, 2 unresolved.
  Features: 92 high, 11 medium (all because of scratch constants or a hook window), 2 low (Light Dash ValidRing Distance, Spindash Startup Delay: different function
  bodies in PAL), 5 unresolved (the 4 version-mismatch entries, which emit nothing, and Advanced Full Widescreen: r2-relative scratch).
* Scratch: 2 clusters. NTSC 0x8060A000-0x8060D000 (constants 0x8060B33C, 0x8060B33F) -> PAL 0x817C0000-0x817C3000 (0x817C133C, 0x817C133F); NTSC 0x807D4000-0x807D9000 (constants
  0x807D5700, 0x807D71A0) -> PAL 0x817C3000-0x817C8000 (0x817C4700, 0x817C61A0). 20 KB of the 192 KB are used. The cave of Melee Weapon Core Revamp at 0x80003D9C now builds
  `lis 0x817C; ori 0x61A0` for the old 0x807D71A0; 17 scratch pair rewrites in the MCM caves, all 17 verified inside the region.
* Residue of the DOL diff (1765 words): 1470 explained by enabled entries, 65 zero fill, 168 words of the 4 version-mismatch entries, 62 unexplained. The 62 sit in the cave area
  0x80004078-0x80004A74 between the version-mismatch caves (single return words and a 57-word run at 0x800047AC: a shared routine of the other version of those entries); no unexplained word
  lies outside the cave area.
* The ini: 33 codes = 33 features (g001-g033), 32 high, 1 medium (SuperShadow: scratch constant, 0x817C...).
* A DOL-diff-only run still produces one merged cave image (no features); use the MCM run with `--mod-dol` to get features. The cave image is `unresolved` as a whole because of the Widescreen entry.
