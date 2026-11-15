# Code mods by recompiling a patched PAL DOL (feasibility, 2026-11-14)

> In this repository `test_data/mods/codemods/site_classes.json` is not kept: `tools/mods/codemods/classify_sites.py`
> makes it.

Question: can GekkoForge recompile a patched PAL `main.dol` into a working variant executable, and which patches would
do nothing? Everything below was run on the lifted tree (`SHADOW_RENDER_LIFTED=ON`, the shipped configuration) from this
worktree, with `-m:3`, one build at a time, headless `--no-audio` Westopolis captures (`cap.sh`, 1280x720, stg0100 tape).

## Answer in five lines

1. Yes. A PAL DOL with words changed in place recompiles and runs. Code in an ordinary translated function changes
   behaviour (frame proof below). The generator re-derives liveness, entry points, loops and leaf inlining from the
   patched DOL, so no list needs editing for in-place word changes.
2. Two tool gates reject a patched DOL and needed a change (prototyped on this branch): the SHA-1 identity gate
   (`disc_map.py identify`, run by CMake) and the SHA-256 gate of `make_empty_variadic_fixture.py`. Both now take a
   "variant of the retail PAL DOL" mode. Nothing else broke.
3. The No Bloom code is a no-op in this build: its site is in function `0x80498910`, which the lifted tree prunes
   (unreachable), and the call it removes is to `GXSetBlendMode`, a native owner. A variant built with it is
   byte-identical generated code and a pixel-identical frame.
4. New code (caves) is not picked up by itself. The cave must be a row of the function table; without the row the
   build is green and the game stops at run time at the cave ("native execution cannot continue at 0x80003EA4 ...
   PPC interpretation is forbidden"). With one added row it runs, bit-identical to the same patch written inline.
5. Classifier: of the 224 Reloaded MCM patch sites 146 sit in translated code (13 of them in runtime-named functions),
   70 are data, 5 are refused by the generator (lowered sites) and 3 are ineffective (lifted renderer owner); Gecko: 30
   of 73 writes in translated code, 27 data, 16 bss/heap. Numbers and method in section 3.

## 1. What a variant build needs (config) and what broke

Variant DOL = retail PAL DOL, words changed in place (same size, same section header). Produced by
`tools/mods/codemods/patch_dol.py` (takes paths; never writes the input; `--set ADDR=WORD`, `--words`, `--copy-word
other.dol:NTSCADDR:PALADDR`). The DOL must sit in a directory that also holds `boot.bin` and `bi2.bin` (the Windows
build embeds the system area from the DOL's directory: `Cannot embed missing game-system input ...boot.bin`
otherwise). `game/sys/main.dol` is never touched.

| Step | Result with a patched DOL | Fix |
|---|---|---|
| CMake `disc_map.py identify` (SHA-1 vs `extract/discs.json`) | FATAL "unknown main.dol" | `-DSHADOW_RECOMP_VARIANT_OF=<retail PAL main.dol>`: accepts a DOL with the same size and the same 0x100-byte section header as the retail one. Prototype in `tools/ppcrecomp/disc_map.py` (`identify --variant-of`) and `CMakeLists.txt`. |
| post-build `make_empty_variadic_fixture.py` | fails: "requires the verified original DOL" (SHA-256 pinned) | `--variant` skips only the whole-file hash; the three leaf bodies and the 941 call edges stay pinned. CMake passes it when `SHADOW_RECOMP_VARIANT_OF` is set. |
| embedded system image | needs `boot.bin`/`bi2.bin` beside the DOL | put the variant in its own `sys/` folder |
| `check_native_boundary.py`, `verify_native_math.py` | passed unchanged. They pin function bodies (by hash) and incoming call edges of the retired math/leaf functions: a patch inside those bodies, or a patch that adds/removes a direct call to them, fails the build (loud). | none |
| runtime | no hash of the DOL anywhere (checked `runtime/src`). `native_pc_boot_contract` verifies the words of `__start`/`__init_data` only when `--native-boot-contract-dir` is given. The DOL is loaded into guest memory from the embedded copy. | none |
| `extract/*.csv/txt` | not hash-bound; they hold addresses and sizes. In-place edits keep every address valid. Lists that carry an expected word (`lift-sites*`, `gx-sites`, `platform-sites`, `wait-points`, `psq-sites`) make the generator REFUSE a variant that changes that word ("the DOL word is X, the list says Y (stale ...?)"). | by design: see classifier, class `lowered-site` |
| `SHADOW_BUILD_TESTS=ON` ctests | not run. Several take the DOL path and pin retail words/hashes (`asset_import_tests`, `button_glyphs_tests`, the axfx/gcax contract tests quote the SHA-256). A variant tree should build with tests off, as the shipped tree does. | not verified |

## 2. Experiment A: No Bloom, and an effective patch

### Address
NTSC-U `0x804978F0` (a `bl`, word `4BF01411`) maps through `extract/address-maps/GUPE8P.map` to PAL
`0x80498940`, word `4BF01405`, 13/13 masked window match. It is a `bl 0x80399D44` (`GXSetBlendMode`) inside
`fn_80498910` (size 0x44, one of the RenderWare driver's blend-state setters). Gecko `044978f0 60000000` becomes
PAL `0x80498940 <- 60000000`.

### Ownership of that site
`0x80399D44` is in `extract/native-owners.txt` (GXSetBlendMode, owned in every build) and `fn_80498910` is absent from
the lifted tree: `recomp_pruned.csv` lists it, no `fn_80498910` exists in any generated shard. The lifted renderer
draws bloom itself; the guest blend setter is never reached. So the patch is ineffective here.

### Builds (one tree `.build/watch`, same source, only the DOL changes; `-m:3`, 48 shards)

| Variant | DOL change | Build time | Generated files written | Result |
|---|---|---|---|---|
| base | none (retail) | 627 s (first full build) | 52 | exe `c57c4349` |
| V1 | No Bloom NOP | 91 s | 0 of 52 (shards, table, decls, layout byte-identical to base) | exe differs only by the embedded DOL copy; frame at tick 972M and 1.62G: **identical** sha256 prefix `fb8843301144` at 972M |
| V2 | V1 + No Skidding (`beq`->`b` at `0x8008347C`, copied from the Reloaded DOL) | 306 s | 4 (shard 046, table, `recomp_decls.h`, report) but `recomp_decls.h` changed so all 48 shards recompiled | tape never skids: frames identical to base (`fb88..`, `5092296adbb4`) |
| V4 | V1 + 3 forced branches: Disable Partner Intro `0x80073F38`, Keep Melee Weapons when Mounting `0x8007ECA4`, Fix Sliding on Steep Slopes `0x8008DF6C` (+ No Skidding) | 328 s | 6 | frame at tick 1.62G **differs** (920,915 of 921,600 pixels): see below |
| V5 | V4 but Partner Intro through a cave at `0x80003EA4`, no function row | 143 s | 2 (shard + table; `recomp_decls.h` unchanged, so one shard recompiled) | build OK, run STOPS: `execution fault ... native execution cannot continue at 0x80003EA4 (LR=0x80073F30) ... PPC interpretation is forbidden` |
| V6 | V5 + one function row `0x80003EA4` size 12 (`-DSHADOW_RECOMP_FUNCTIONS=<copy>`) | 305 s | 6 | runs; frame at 1.62G **bit-identical to V4** (`09a52e97faf0`) |

Rebuild cost after the first build: 91 s when the patched function is not emitted (nothing to compile); about 2.4 min when
the patch changes no entry point (only that function's shard is recompiled: the shard layout is sticky,
`recomp_layout.csv`); about 5 min (306-328 s) when it adds an entry point, because `recomp_decls.h` lists every
`fn_X_eADDR` entry and all 48 shards include it. A forced branch (`bne` -> `b`) always adds one: the fall-through
instruction becomes an extra entry (`fn_80073C54_e80073F3C` in V4's decls diff). A constant change inside an
instruction (immediate, compare value) keeps the entry set. Reducing the full recompile would be a generator change:
emit extra-entry declarations into a per-shard header instead of one shared header. Not done.

### Frame proof
Same tape, tick 1,620,000,000 (frame 2000), stock exe vs V4/V6 (variant with Disable Partner Intro):
stock shows Sonic's partner-intro close-up (`SONIC` caption) from the cutscene; the variant skips the intro and shows
gameplay (Shadow in the street with Sonic's partner bubble, ring counter 007, the same subtitle line). V1/V2 produce
the stock frames at all captured ticks. Frames: scratch runs `rc-base-1620000000`, `rc-v4-1620000000`,
`rc-v6-1620000000`; exes named `recomp-<variant>-<md5>.exe` in the scratch `exe/` folder.

### A translation hazard found on the way
Copying a Reloaded word to the PAL address is only safe when the word carries no address. Of the 62 inline-code MCM
patches in translated functions: 38 are immediates only (safe), 11 are branches (PC-relative within the same function,
safe when the displacement stays inside it; a branch out of the function needs retargeting), 5 are `lis` (constants in
the cases seen: floats `0x42C8`, `0x425C`, `0x4296`, `0x3F80`, not addresses) and 8 carry a small-data offset
(`lfs/stw ... (r2)` or `(r13)`), e.g. Light Dash `D00D3004 -> 91ED3020`, Strafe Walk `D00D2FB8 -> 920D2FD8`. The
`r13` offsets are NTSC-U's; PAL's `.sdata` layout differs, so the word must be re-encoded with the data address map,
not copied. `tools/mods/reloaded_code_map.py` currently reports these as `pal_status: shape` (address fields
masked). The first V4 attempt (copying 15 such words) was stopped before it was built for this reason. A patch
translator must re-encode `r2/r13` offsets (and any `lis/addi` pair that forms an address) with the data map and
refuse the patch otherwise.

## 3. Classifier

`tools/mods/codemods/classify_sites.py` (stdlib; all inputs are paths) and
`test_data/mods/codemods/pal_code_ownership.json` (108 KB: owner ranges, emitted/table coverage, runtime-named
functions, lowered sites, jump tables, sections; built from `extract/functions.csv`, `native-owners*.txt`,
`lift-sites*.txt`, `gx-sites.txt`, `platform-sites.txt`, `wait-points.txt`, `psq-sites.txt`,
`empty-variadic-leaves.txt`, `jump-tables.txt`, `liveness-roots-lifted.txt` and the `recomp_layout.csv` the generator
wrote; it was identical in this worktree and in `wt-mods`). Rebuild with
`classify_sites.py build ...`; query with `site --ownership ... 0x80498940` or `features --features
reloaded_features.json`. The JSON describes the lifted tree only.

Classes (verdict in brackets):

- `translated` [works] inside an emitted body, not a listed site.
- `translated-named` [works unless a hook runs first] as above, but the function entry is named by the runtime
  (`liveness-roots`: `install_hle` hooks that can decline, native callers, intrinsics). A hook that serves the
  call without declining preempts the body. 747 functions; checking each hook is open work.
- `lowered-site` [the generator refuses the patched word] a listed `lift/gx/platform/wait/psq/empty-variadic` site
  (3,639 addresses). Refusal is loud (build stops), not silent.
- `native-owner` [ineffective] 457 functions served by a runtime handler in every build.
- `lifted-owner` [ineffective] 143 functions owned only by the lifted renderer tree (`native-owners-r2.txt`).
- `pruned` [ineffective] a table row the tree never reaches (1,562 rows; the No Bloom site is here).
- `unemitted` [ineffective] outside any emitted or table row (cave range, padding).
- `data` / `data-jumptable` [data engine, or a rebuild if the data is in the DOL image].

Note: function rows overlap (an entry-point row nests inside its function), so coverage is computed as a union of
intervals, not by nearest start.

### Counts over the 224 Reloaded MCM sites (110 library entries) and 33 Gecko codes (73 writes)

| Class | MCM sites (224) | Gecko writes (73) |
|---|---|---|
| translated | 133 | 29 |
| translated-named | 13 | 1 |
| lowered-site | 5 | 0 |
| lifted-owner | 3 | 0 |
| data | 70 | 27 |
| unmapped (no PAL address: bss or heap writes) | 0 | 16 |
| pruned / native-owner / unemitted | 0 | 0 |

By patch kind: 85 of the MCM sites are `branch-to-cave` (79 translated, 5 translated-named, 1 whose hook sits in a
lifted owner `GcnCameraBeginUpdate`) and need cave code (section 4); 69 are inline code (54 translated, 8
translated-named, 5 lowered-site, 2 lifted-owner); 70 are data words. All five
`lowered-site` and the 3 `lifted-owner` rows belong to the Advanced Full Widescreen entry (this build has its own
widescreen; recommendation: never offer it as a code mod). Per library entry (worst site): 82 translated, 27 data-only,
6 translated-named, 1 lowered-site. Per Gecko code (33): 13 translated, 1 translated-named, 8 data, 11 unmapped.
Row-level data: `test_data/mods/codemods/site_classes.json`.

Caveat: this classifies the site, not the effect. `translated` means the word is in emitted guest code and the
recompile reflects it; it does not prove the mod works (see the translation hazard above).

## 4. Experiment B: new code (caves)

What was done: a branch-to-cave patch on a translated function: `0x80073F38 bne` -> `b 0x80003EA4`; cave at
`0x80003EA4` = `nop; b 0x80073F84`. The cave range 0x800034AC-0x800053E0 holds the original debugger stubs: 51
table rows (`fn_800034ac`, `pad_800034dc`, ...), none emitted in the lifted tree (unreachable), 32 rejected as
zeros, and `0x80003EA4` is inside the zero-filled row `pad_80003de0` (716 bytes), which is rejected because it starts
with zeros. Findings:

1. Branch targets into the cave are recognised from the patched site (the entry set and the shard come out right) but the
   cave itself is only translated if a table row starts at its entry. V5 without a row: green build, runtime stop at
   the first execution of the cave. Silent at build time; loud at run time.
2. Adding one row (`add_function_rows.py --row 0x80003EA4:12`, passed as `-DSHADOW_RECOMP_FUNCTIONS=<copy>`; the
   repository's table is not edited) fixes it. Liveness needs nothing else: the cave is reached through the
   patched branch (liveness roots come from the DOL's words), and V6 is bit-identical to the inline patch V4.
3. A cave must not be placed where a lowered site, native owner or other list expects the original words; the
   generator checks the site lists, not the cave.
4. Caves that call other functions (`bl`) need nothing more: callee entries come from the DOL. Caves that read/write
   free flag bytes or `.sbss` words depend on the data map: the PAL equivalents must be re-derived (hazard in
   section 2). Caves that were written for NTSC-U and call NTSC-U addresses must have those branches re-targeted to
   PAL; the cave contents are not address-neutral.
5. Exactly what a mod needs to ship code: (a) a patch list `PAL address -> word`, validated by the classifier; (b) a
   list of cave functions `(PAL address, byte length)` inside the free range or any free zero-filled text range;
   (c) a function-table copy with those rows; (d) a variant sys folder; (e) cmake args `-DSHADOW_RECOMP_DOL`,
   `-DSHADOW_RECOMP_VARIANT_OF`, `-DSHADOW_RECOMP_FUNCTIONS`.
   Not needed: `liveness-roots`, `jump-tables`, `native-owners`, `wait-points` (a loop in a cave that polls without
   a wait point would spin without advancing the game clock: add a wait point only for such loops).
6. A cave that adds a jump table needs a row in `extract/jump-tables.txt` copy (otherwise its words are plain data
   roots: still correct, a little more conservative liveness).

## 5. Recommendation

Do not make "recompile at Play" the manager's primary path for ordinary users.
It is correct and the tool changes are small, but (a) a rebuild is 2.4-5.5 minutes of CPU at `-m:3` per variant on a
machine that has the Visual Studio toolchain, the generator and the recompiled source tree (the shipped exe does not
contain them), (b) a variant exe embeds the game's DOL, so a prebuilt variant is a redistribution of game code, which
the project already avoids, and (c) most "mods" people use are data or small patches.

Recommended shape:
1. Keep one stock exe; apply data writes (Gecko `write32/write8` into data, `.sdata` constants, bss/heap) with the
   native engine, no rebuild.
2. For code patches, classify with `classify_sites.py` and tell the user per mod: works after recompile / ineffective
   (native owner, lifted owner, pruned) / refused (lowered site) / needs cave rows. Never claim an ineffective patch
   (No Bloom is the proof case: 91 s build, identical frame).
3. For a user who wants a code mod: an explicit "Build variant" action on a developer-equipped machine, producing a
   named exe `<mod set>-<hash>.exe` next to the manager, not on the Play button. Cache by the hash of
   (patch words, cave rows, source tree).
4. Before shipping this, a patch translator is needed that re-encodes `r2/r13` small-data offsets and address-forming
   `lis/addi` with the data map and refuses what it cannot translate (section 2 hazard). Without it 8 of 62 inline MCM
   patches (and the same shape in caves) would silently write wrong variables.
5. Generator work worth doing if variants become routine: put the extra-entry declarations in per-shard headers so
   a one-function patch recompiles one shard (91 s - 2.4 min instead of 5 min); make an unreachable cave target a build
   error instead of a run-time stop; extend `native_*` hook inventory so `translated-named` can be resolved to
   works / ineffective per hook.

## Risks / not verified
- No tests build (`SHADOW_BUILD_TESTS=OFF`), no `ctest` run for a variant.
- Only the lifted tree was built; the non-lifted reference tree classifies differently (the No Bloom site may be live
  there).
- Effect frames are one tape (Westopolis stg0100) at two ticks; the partner-intro patch is the only gameplay-visible
  patch proven. The other three forced branches were part of V4 and are not individually attributed.
- `translated-named` hooks are listed, not resolved.
- Timings are one run each on a loaded machine (`-m:3`, other tools may have been running).

## Files
`tools/mods/codemods/classify_sites.py`, `patch_dol.py`, `add_function_rows.py`;
`test_data/mods/codemods/pal_code_ownership.json`, `site_classes.json`;
tool changes: `CMakeLists.txt` (`SHADOW_RECOMP_VARIANT_OF`), `tools/ppcrecomp/disc_map.py` (`identify --variant-of`),
`tools/ppcrecomp/make_empty_variadic_fixture.py` (`--variant`).
