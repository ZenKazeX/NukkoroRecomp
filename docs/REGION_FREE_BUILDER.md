# Region-free builder

Asked for by the user 2026-10-27: "make the recompiler's builder region free by knowing all existing versions of
the gamecube disk and still be able to build the PC version with the optimized code."

Status: step 1 (disc identity + verified address maps + rebasing of the generator inputs) is implemented and
tested. Step 2 (the runtime's own addresses) is designed below and needs the user's go-ahead because it touches
about 800 literal sites in about 140 files.

## 1. The known discs

All three are revision 0 of the same program (`boot.bin` 0x07 = 0; `bi2.bin` 0x18 is the region code).

| Game ID | Region | bi2 | main.dol bytes | main.dol SHA-1 | text1 size | .bss start |
| --- | --- | --- | --- | --- | --- | --- |
| GUPP8P | PAL (canonical) | 2 | 5,773,024 | 6ff04994371e93d6ba1015644f4e7467713b9363 | 0x4A1F20 | 0x8056FE00 |
| GUPE8P | NTSC-U | 1 | 5,768,800 | 118ef49ebd45b371b6f514e0e1b6d1ba7cf12904 | 0x4A0EC0 (-0x1060) | 0x8056ED40 (-0x10C0) |
| GUPJ8P | NTSC-J | 0 | 5,770,464 | 72f47bca7b72ec385a6fa30e123a96cc0fd19bab | 0x4A1500 (-0xA20) | 0x8056F3C0 (-0xA40) |

Same entry (0x80003154), same section count and order, same text0. Everything after the first changed function
moves: the NTSC-U code is 0x1060 bytes shorter, NTSC-J 0xA20. The catalogue is `extract/discs.json` (with SHA-256).
The disc folders differ in localized files (`PRS_VOICE_E/J.afs`, `sng_EV*_E/_J.adx`, `sng_jin_*_e/_j.adx`); the
PAL disc has its own set.

## 2. How the releases differ (measured)

`tools/ppcrecomp/disc_map.py build` pairs every row of `extract/functions.csv` (39,355 functions) with the other
DOL. Results with the checked-in maps:

| | NTSC-U GUPE8P | NTSC-J GUPJ8P |
| --- | --- | --- |
| identical functions (PROVEN) | 39,229 (99.68 %) | 39,325 (99.92 %) |
| changed functions | 126 | 30 |
| code segments (distinct moves) | 73 | 18 |
| relative branches checked against the map | 198,682, 0 disagree | 200,930, 0 disagree |
| data proof points / data segments | 17,627 / 43 | 17,733 / 27 |
| data words naming a function, checked | 23,116, 0 disagree | 23,315, 0 disagree |
| runtime address literals (non-test) in the image, mapped | 788 of 798 | 791 of 798 |
| extract/ address tokens, mapped | 5,720 of 5,756 | 5,725 of 5,756 |

What does not map, by cause:

1. **The PAL 50/60 Hz screen does not exist on either NTSC disc.** Canonical 0x80306160-0x803071FC (16
   functions: `palsel` exec 0x80306638, the mode branch 0x80306888, loader 0x80306B48, gate 0x8030717C, its
   draws). Runtime hooks: `runtime/src/gekko/system.cpp` (boot-stage Hz-screen step, movie events),
   `runtime/src/shadow_main.cpp` (`palsel-gate`/`palsel-loader` names); 16 lowered RenderWare draw sites of
   `extract/lift-sites-r2.txt`, one of `extract/lift-sites.txt` (0x80306A84). The right handling is "absent
   feature", not a mapping.
2. **NTSC-U only: real behaviour differences.** E.g. 0x8008A7E4 and 0x8008FA9C, small static initializers that
   store different small-data floats (both named by `runtime/include/native_pc_boot_original_contract.h`);
   0x80050BF8 / 0x80050C40 (liveness roots); the AX compressor table 0x80563420 (`system.cpp` checks the game's
   table against the native copy) has no proof in NTSC-U. Each needs a reviewed decision, never an inferred address.
3. **Not DOL addresses.** Heap/arena addresses above the image (`kImageEnd = 0x80610000` in
   `gekko/guest_memory.h` and `native_bootstrap.h`, about 118 literals that only feed the liveness roots).
   They have no static proof; as liveness roots they root nothing.
4. **Pinned instruction words** that embed small-data offsets (e.g. `0x806D4550` = `lwz r3,0x4550(r13)` in
   `native_pc_boot_original_contract.h`) change with the layout although they are not addresses.

Also hard-coded today: `shadow_main.cpp:1396` refuses any disc but GUPP8P; the TV format default is PAL
(`shadow_main.cpp:527`); the memory card file is `8P-GUPP-SHADOWTHEHEDGEHOG.gci` (saves are per region); 64
analysis scripts under `tools/` carry canonical constants (e.g. `gx_site_inventory.py` `GX_POINTER_SLOT`), and
`tools/test_data` fixtures are PAL.

## 3. Design

### 3.1 One canonical address space

Every address in the tree stays a PAL GUPP8P address: `extract/*`, the runtime, the scaffold's `layout::`
constants, the docs and the tests. Nothing is renamed. A build for another release translates canonical addresses
through that release's verified map at the edge where they enter the build (generator inputs; runtime constants),
so the knowledge is written once and the PAL build is untouched by construction.

Rejected: relinking the NTSC DOL into the PAL layout (needs every pointer in data identified - unprovable without
relocations - and the changed functions do not fit); a run-time address table (hot paths and `switch` labels need
compile-time constants, and one executable serves one disc anyway); keeping per-region copies of `extract/`
(three copies of every finding).

### 3.2 Disc identity (implemented)

`disc_map.py identify` names a `main.dol` by SHA-1 and size against `extract/discs.json` and refuses an unknown or
modified one. CMake runs it at configure time on `SHADOW_RECOMP_DOL` (`SHADOW_DISC_ID`), so the build never
recompiles a DOL whose addresses the tree does not know. A non-canonical release is refused with a pointer to this
document until step 2 lands.

### 3.3 The address map (implemented)

`extract/address-maps/<ID>.map` (addresses and counts only - no byte of game code) is generated by
`disc_map.py build` and checked by `disc_map.py check`. Evidence classes, strongest first:

- `fixed` - OS low memory 0x80000000-0x80003100 (identity; same IPL/OS layout, same first section).
- `code` (PROVEN) - a function whose body is identical with only the address-carrying fields masked (branch
  displacements, `lis` high halves, low halves applied to a `lis` register or r2/r13; `li` constants, stack and
  structure offsets are compared). Every relative branch of every such body must land on the mapped target, or
  the whole map is refused. Pairs are kept only in link order (a longest increasing chain), so an identical stub
  elsewhere cannot be taken for the function.
- `data` (STRONGLY SUPPORTED) - (canonical, target) address pairs formed by identical bodies, kept only inside
  one data section or .bss of both DOLs and only in layout order; a segment spans a run of one delta. Data words
  naming a mapped function are re-checked through it.
- `aligned` (STRUCTURAL MATCH) - equal instructions inside a changed function.
- `changed` - listed for review; anything else is unmapped. A consumer refuses an unmapped address.

### 3.4 Generator inputs (implemented as a tool; wired in step 2)

`disc_map.py rebase` writes each canonical input for a release: the address columns mapped, a pinned instruction
word re-read from the target DOL at the mapped site and required equal up to its address fields (a branch must
also reach the mapped target), notes kept verbatim. Native owners and empty-variadic leaves require a `code`
(identical) body, since they replace the whole function. Every line it cannot carry goes to a report.

In the build (step 2), for a non-canonical disc, CMake rebases the inputs into `<build>/disc/<ID>/` and runs the
same generator with them, so the target gets exactly the same optimizations: native owners, lowered call sites,
wait points, platform and GX sites, paired-single specialisations, pruning. Lists whose tools are disc-agnostic
(`jump_tables.py`, `wait_points.py`) are regenerated from the target DOL and must agree with the rebased list on
every line the map can carry. Each line the rebase drops must be acknowledged in a reviewed
`extract/address-maps/<ID>.absent` (`0x80306638 pal-only: the 50/60 Hz screen`); an unacknowledged drop fails
the build.

Verified now (outside the build, into scratch, 2026-10-27): with the rebased inputs, the generator accepts both
NTSC DOLs (exit 0): 121/121 native owners, 763/763 lowered call sites (the 764th is in the PAL-only screen), 14/14
platform sites, 1,511/1,527 GX sites (the same 16 pruned or owned as PAL), 290/442 wait points, 38,769 (U) /
38,783 (J) functions recompiled against PAL's 38,777. Its own pinned-word checks all passed. Regenerating from the
NTSC DOLs gives identical wait points; the jump tables differ by one line, the table at 0x8056FDCC lying past the
last data proof point (the regenerated list carries it).

### 3.5 Runtime and scaffold addresses (step 2 - needs the user's approval)

About 800 distinct in-image literals in about 140 runtime/scaffold files. Proposal:

- A generated header `disc_layout.h` (from the map at configure time) defines
  `template <u32 Canonical> inline constexpr u32 guest_v = ...;` - a compile-time lookup over the segments. An
  unmapped address fails to compile (`static_assert`) naming the address, so no build can silently use a PAL
  address on another disc. For PAL the map is the identity, the constants fold to the same values, and the
  objects are unchanged (to be verified by hashing the outputs).
- Every in-image literal becomes `guest_v<0x805F265Cu>` (mechanical rewrite with a checker:
  `liveness_roots.py`, which already scans every literal, refuses a raw in-image literal; data words that are
  instruction words, like the boot contract's, are excluded and handled by the next point).
- Contracts of original instruction words (`native_pc_boot_original_contract.h`, other `*_contract.h`) are
  generated per disc by `rebase` (the word re-read at the mapped site), or refused where the body changed.
- Absent features become compile-time facts of the disc (`disc::kPalHzScreen`): the PAL-only hooks compile out on
  NTSC instead of naming addresses that do not exist. Changed bodies a hook depends on (NTSC-U section 2.2) get
  a reviewed native decision each.
- Disc facts move out of code: the GUPP8P check (`shadow_main.cpp:1396`) becomes the catalogue check, the TV
  format default follows the region, the card file name follows the game code.

The canonical analysis scripts in `tools/` stay PAL tools (they prove facts about the canonical program; the
NTSC builds consume those facts through the map). A script that must run on another disc takes its constants
through `disc_map.py lookup`.

### 3.6 Tests

- `disc_map_unit` - identity, map derivation, the branch check refusing a wrong pair, constants compared,
  rebasing with re-pinned words, stale-map refusal, on synthetic DOLs built in the test.
- `disc_address_map_<ID>` - registered for every catalogued disc found under `SHADOW_DISC_SEARCH`
  (default `../EXTRACTED_DISKS`): the checked-in map must be exactly what the DOLs produce.
- PAL-keyed reference tests stay on the PAL DOL. A non-canonical build (step 2) runs the disc-agnostic tests and
  the generator's own checks.

## 4. Steps

1. **Done:** catalogue, `identify` at configure time, `disc_map.py` (build / check / lookup / rebase), the two
   maps, CTest. PAL build: no generator, runtime or scaffold source changed (verification in section 5).
2. Runtime literal layer (3.5) + `.absent` review lists + CMake rebase wiring; a GUPJ8P executable first (30
   changed functions, only the PAL-only screen missing), then GUPE8P (review the 126 changed functions that the
   hooks touch).
3. Region facts (card name, TV format, languages) and a gameplay validation per disc.

## 5. Verification of step 1

Run 2026-10-27, tests preset (`.build/tests`, `-j 2`), game not running:

- Configure: `-- Recompiling GUPP8P (canonical address space)`.
- Build: rc 0. The generator re-ran; all 64 `recomp_*.cpp`, `recomp_decls.h` and `recomp_layout.csv` are
  byte-identical to the files before the change (SHA-256 compared). The one source compiled was
  `runtime/lift/render/renderer.cpp`, edited by another session at 13:07 that day (not by this change); the five
  executables that link it (model_preview, render_core_tests, rwim_tests, stage_preview, stage_viewer) relinked,
  the other 92 test executables are byte-identical.
- CTest: 121/121 passed, including `disc_map_unit`, `disc_address_map_GUPE8P` and `disc_address_map_GUPJ8P`.

Not run: the release preset (it differs from tests only in options this change does not read), and anything for
the NTSC discs beyond generation into scratch (no NTSC executable exists yet).
