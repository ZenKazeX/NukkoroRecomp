# os-font: the OS font library owned natively (agent os-font, 2026-11-07)

Branch `agent/os-font`, worktree `wt-agent-os-font`. Scope: the three os.font frontier rows named by sdk-identify
(`test_data/platform/unidentified_names.csv`): `OSGetFontEncode` 0x80375150, `OSInitFont` 0x80375DA0,
`OSGetFontTexture` 0x80375FAC.

## What the library does and who uses it (evidence)

* `OSGetFontEncode` returns 0 (ANSI) or 1 (SJIS). Original: a u16 cache at sda+0x2708 (0xFFFF = unset); otherwise it
  reads the OS TV-format word 0x800000CC and, only for 0 (NTSC), bit 1 of the video encoder status 0xCC00206E
  (Japanese encoder). It then stores the string parser 0x80375618 into sda+0x5A88.
* The runtime serves the encoder status as 0 (`native_video.cpp`, kind `video_encoder_status`: "no Japanese video
  encoder"), so the answer is ANSI (0) for every TV format.
* Callers in the translated program (live graph of the snapshot, `tools/doldis.py --xref`): the only live caller is
  CARDInit 0x8038DE3C (call at 0x8038DE74, `sth r3, 0x5DF0(r13)`), whose value `__CARDGetFontEncode` 0x8038DEE8
  hands to the CARD format code. The game's own message screen (0x8032D4CC, 0x8032D760, 0x8032D7CC, 0x8032DE9C) and the
  DVD ShowMessage 0x8037DF10 are `pruned` in the live graph: nothing live calls `OSInitFont` or `OSGetFontTexture`.
  Their only "entries" were the three `video_encoder_status` platform sites of `native_video.cpp`, which exist
  only to patch the MMIO reads inside these three bodies.
* `OSInitFont` fills the font buffers through ReadFont 0x803751B4 -> `__OSReadROM` 0x80377C7C, which
  `native_ipl.cpp` refuses (this program has no boot ROM). A native version could serve no glyph data; the repository
  holds no IPL font.
* Readers of the font state: a scan of every D-form load / store with RA = r13 in the DOL's text for offsets
  0x2708, 0x5A80, 0x5A84, 0x5A88 finds them only in 0x80375150..0x80376044 (the OSFont bodies); nothing else.

## What I replaced (files)

| file | change |
|---|---|
| `runtime/include/gekko/native_os_font.h`, `runtime/src/gekko/native_os_font.cpp` (new) | `os_font::font_encode` (TV word, encoder status -> encoding), owners `serve_get_font_encode` (r3 = encoding, writes nothing) and `serve_never_entered` (`OSInitFont`, `OSGetFontTexture`: the run stops with the entry's name, the pattern of the refused `__OSReadROM` / `InitMetroTRK`), `install(Cpu&)`, `Owners` address table |
| `runtime/tests/os_font_contract_tests.cpp` (new) | contract test against the original body (below) |
| `runtime/src/gekko/system.cpp` | `os_font::install(cpu_);` after `install_reverb_owner();` (+ include in `system.h`) |
| `CMakeLists.txt` | source `native_os_font.cpp`; test `os_font_contract_tests` (labels reference;native-contract) |
| `runtime/tests/native_file_api_tests.cpp` | `add_owner_addresses<os_font::Owners>` (owner list equals extract/native-owners.txt) |
| `runtime/src/gekko/native_video.cpp` | the three `kSites` entries 0x80375178 / 0x80375DDC / 0x8037600C removed (the card site 0x80391910 stays: card-saves owns it) |
| `extract/native-owners.txt` | appended block: 0x80375150, 0x80375DA0, 0x80375FAC |
| `extract/platform-sites.txt` | the three stale site lines removed |
| `extract/liveness-roots.txt` | 0x80375178 / 0x80375DDC / 0x8037600C removed; 0x80375150 / 0x80375DA0 / 0x80375FAC added (the generator-of-record scan `liveness_roots.py --out` lists exactly these for my files) |

Not changed: `SHADOW_GC_REMOVAL_RATCHET`, wait points, `docs/GC_REMOVAL_PLAN.md`.

What the owner writes: only r3. Not replicated (nothing reads them): sda+0x2708 (encode cache), sda+0x5A88 (parser pointer).

## Gate evidence

Environment: tree `wt-agent-os-font`, every command through `capped_run.ps1` (10 %; quoted job CPU shares below). Baseline
exe built from the snapshot before the first change (`.build/ab-jobs/base.exe`, sha256 6e62201f61ce4984...), candidate
`cand.exe` (805ef7a2f6d1c8dd...) built from the final tree. Configure flags: `-DSHADOW_DEPLOY_DIR=` (copy_if_different count in
the vcxproj: 0), `-DSHADOW_RECOMP_DOL=.../game/sys/main.dol`.

1. **Measure first** (lock `generator`): with the edited extract files the baseline of `what_if` already contains the
   owners, so `what_if` reports "frees 0". The measured effect is therefore the difference of the generator's pruned
   lists: snapshot build 751 pruned rows -> 760 (owner rows and roots absent) -> 758 with the three owner roots (the
   two never-entered owner rows count as owner rows instead of pruned rows). Newly unreachable bodies: ReadFont
   0x803751B4, Decode 0x80374FDC, GetFontCode 0x80374E68, ParseStringS 0x80375618 (+ interior 0x80375628),
   ExpandFontSheet 0x803759F4, the interior 0x80376060, plus the bodies of OSInitFont and OSGetFontTexture.
   `removal_plan.py --release-dir .build/watch` (the candidate): **platform bodies compiled 1034** (frontier 286);
   the documented shipping figure before this step was 1,044 (docs/GC_REMOVAL_PLAN.md step 2); I did not measure the
   snapshot's own shipping figure, so the "-10" is that documentation figure minus my measurement. sdk:unidentified
   bodies 156. Revived: 0. Refusals: none (the stale sites were removed in the same change, as the generator requires).
2. **Contract test** `os_font_contract_tests` (pinned DOL, reference interpreter; 2,083 original instructions): the
   original `OSGetFontEncode` against `font_encode` for 12 TV formats x 9 encoder statuses (cache unset): equal; the
   original's side effects (reads the register once, caches in sda+0x2708, sets sda+0x5A88, touches no header
   slot); cache hit does not read the register; PAL never reads the chip; with the runtime's encoder status 0 the
   original answers ANSI for every format as the owner does; installed owners: `OSGetFontEncode` returns 0 and
   returns to the caller, `OSInitFont` / `OSGetFontTexture` stop by name; the call chain OSInitFont -> ReadFont ->
   `__OSReadROM` is found in the original's code (why "never entered" is the honest owner). **Negative control**: a wrong
   encode (SJIS whenever the chip says Japanese, ignoring the TV word) is caught by the same comparison. Result: PASS.
3. **No surviving reader** (`--watch-guest` headless, from boot, Westopolis tape, ranges sda+0x2708 and
   sda+0x5A80..0x5A8C). Baseline (control, shows the watch sees them): the OSFont bodies read and write the slots
   (cache write, parser write, header reads). Candidate: no access by any game or SDK function. The remaining accesses
   are the harness's own state report reading both ranges (`state_report_json`) and three writes by the C start-up's
   zero fill of the small-data BSS (guest row 0x8000543C, before any use). 0 unattributed.
4. **Game behaviour** (Westopolis 90 s, headless, alternating base / cand, 3 runs each, `--report-peek 0x805E0000:0x20000`):
   game ticks 3,645,000,000, VI frames 2,697, render frames 5,359 in all six; PCM sha256 b8e749ac2ab87aed (first 8 bytes of the hash)
   identical in all six; A/A and B/B memory blocks identical. A/B: exactly two 64 KB blocks differ (0x805E0000,
   0x805F0000); the differing words are the complete list: **0x805EEC08** (sda+0x2708) 0x0000 -> 0xFFFF
   (the cache: computed by the original at CARDInit, never computed natively) and **0x805F1F88** (sda+0x5A88)
   0x80375618 -> 0 (the parser pointer the original stored). Both are state read only by the retired bodies
   (gate 3 and the static scan); no stack or register garbage is involved.
5. **Performance**: six alternating runs each of the same pair, wall 2.8-2.9 s and job CPU 2.6-2.8 s for both, spreads overlap:
   no measurable change (the code runs once at start-up).

Other CTests run in my tree (Release): `os_font_contract_tests` PASS. `platform_gc_removal_plan` PASS,
`platform_retired_readers_osinit` PASS, `platform_retired_readers_reverb` PASS, `native_wait_points_classifier`
PASS. The snapshot's known failures `native_liveness_roots`, `platform_runtime_coupling` (3 unclassified native_cri
uses) and `native_wait_points` still fail as at the snapshot (I regenerated the roots into a scratch file: of the
451 differing lines none concerns this step after my edit). `native_video_reference_tests` PASS (platform sites list matches NativeVideo and NativeGxSync). `native_file_api_tests` FAILS as
at the snapshot (CRI owners and a few other rows "listed, not installed"); none of my three rows (0x80375150, 0x80375DA0,
0x80375FAC) is among the listed-not-installed or installed-not-listed addresses, so the owner list agrees with `os_font::Owners`.

## Open problems

* The card site 0x80391910 (an inlined OSGetFontEncode in `__CARDFormatRegionAsync`) remains with card-saves; with
  it gone, nothing reads the encoder status register any more and the `video_encoder_status` kind can be deleted.
* Anyone who later wants the OS message screens needs a font source (the boot ROM font is not shipped); the owners then
  must become real.

## Shared list lines changed (exactly)

`extract/native-owners.txt`: appended 9 lines at the end (comment block + 3 rows). `extract/platform-sites.txt`: removed lines 14-16.
`extract/liveness-roots.txt`: removed 0x80375178, 0x80375DDC, 0x8037600C; added 0x80375150, 0x80375DA0, 0x80375FAC at their
sorted positions. `CMakeLists.txt`: +1 source line, +8 test lines. `runtime/src/gekko/system.cpp`: +2 lines. `runtime/include/gekko/system.h`: +1 include.
