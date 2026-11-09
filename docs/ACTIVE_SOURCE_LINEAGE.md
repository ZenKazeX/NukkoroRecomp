# LINEAGE of scratch/int-20261021/src-combined (INT, 2026-10-21 03:3x)

GekkoForge paths are relative to `H:\ShadowTheHedgehogDiskGC_Extract\GekkoForge\scratch`. Manifest = the first 8 hex
digits of sha256(SHA256.txt), as ES's stage_verify prints it. Every source stage below was re-verified file by file
against its own SHA256.txt at 03:3x (all OK, `int-20261021/source_verify.txt`).

## 1. The stage graph (what each included line is built on)

```
hw/src-shim (91d8224f)  = step3b + G1 + present trace + R8 + F1-F3 aspect + mem + G-R1 + G10 + shim
 |-- hw/src-all          G11 + G12 + G13 + F-M1 + F-M2                      (diffs/all-vs-shim.diff)
 |    `-- hw/src-cpu (a47bfbae)  C2 + C3 + C4                               (diffs/cpu-vs-all.diff)
 |         |-- hw/src-g5         G5 legacy renderer out of lifted builds    (diffs/g5-vs-cpu.diff)
 |         |    |-- hw/src-g15g16 (60e5cacb) G15 + G16, NO G14              (diffs/g15g16-vs-g5.diff)   <- START TREE
 |         |    `-- hw/src-g14 -> src-g15 -> src-g16 (0f80c821)  (G14 REJECTED: +30 MiB VRAM)
 |         |              `-- hw/src-lies (cb9e7fed)   comment-only, lies 8/10/15/17   (lies-vs-g16.diff)
 |         |                   `-- hw/src-gle (d1febd3b)  GLE frame digest, dev-only     (gle-vs-lies.diff)
 |         |                        `-- hw/src-nerw1 (9295f163) NE-RW1               (nerw1-vs-gle.diff)
 |         |                             `-- hw/src-nerw2 (237b8755) NE-RW2a         (nerw2-vs-nerw1.diff)
 |         |                                  `-- hw/src-nerw2c (532be08c) NE-RW2c   (nerw2c-vs-nerw2.diff)
 |         `-- es/src-es-ram (e3b16d12)  ES W4 M1 M5 W3 T1, present-trace reserve, SHADOW_SHIPPING hunks, comment fixes
 |                                       (W5 was taken out at 16:45; 0 W5 hunks in the tree, see 3)
 |-- ev/src-r13s -> src-r16s -> src-fms -> src-fms2 -> ev/src-neos0 (4138fdb7)
 |                  R13, R16, GXD, RD1, FM, FM2, NE-OS-0 (+ the ev2 PRS cursor)
 `-- au/src-r18s -> au/src-r18s4 (d0698cae)
                    R18a, AU-W1, AU-T1, step 4, GEKKO_SHIPPING gates, ES-W7 (audio_runtime + the shadow_main call site),
                    finish_production, liveness root 0x80387EB4, comment fixes (lies 4-7, 16)
```

## 2. How src-combined was made (tools/merge_combined.py, git merge-file --diff3 per file)

Start = byte copy of hw/src-g15g16, verified against its SHA256.txt. Then each step applies ONLY the files that differ
between its base and its tip (so a step carries exactly its own delta, never its parent's):

| # | Step | base -> tip | files | how |
|---|---|---|---|---|
| 1 | HW lies | src-g16 -> src-lies | 5 | 5 byte copies (headers untouched by G14) |
| 2 | HW NE-RW1 | src-gle -> src-nerw1 | 10 | 7 copies, 1 delete (lens_flare.cpp), 2 clean merges (CMakeLists.txt, lifted_render.cpp). Base src-gle, so the GLE digest is NOT carried |
| 3 | HW NE-RW2a | src-nerw1 -> src-nerw2 | 12 | 11 copies, 1 clean merge (CMakeLists.txt) |
| 4 | HW NE-RW2c | src-nerw2 -> src-nerw2c | 11 | 10 copies, 1 clean merge (CMakeLists.txt) |
| 5 | ES es-ram | src-cpu -> src-es-ram | 25 | 21 copies, 4 clean merges (CMakeLists.txt, system.h, system.cpp, shadow_main.cpp) |
| 6 | EV neos0 | src-shim -> src-neos0 | 26 | 25 copies, 1 clean merge (CMakeLists.txt) |
| 7 | AU r18s4 | src-shim -> src-r18s4 | 22 | 14 copies, 7 clean merges (CMakeLists.txt, liveness-roots.txt, system.h, audio_runtime.cpp, savestate.cpp, system.cpp, ax_pb_tests.cpp), 1 CONFLICT (shadow_main.cpp, 1 hunk) |

Per-file log: `int-20261021/merge_log.json` (ours / base / theirs / result sha256 per action).

### Hand-resolved hunk (the only one)
`runtime/src/shadow_main.cpp`, after the renderer start (`#endif` of the legacy-renderer branch): AU's ES-W7 block
(`audio_runtime.wait_open()` + the "Host audio:" line) and ES-M1's `#if defined(SHADOW_RENDER_LIFTED)` guard (the
asset manager is not started in lifted builds) both insert at the same anchor. Resolution keeps BOTH: the ES-W7 block
first (it runs in both configurations, as in r18s4), then ES-M1's guard, whose `#else` keeps the legacy
`if (options.native_assets) { ... }` block unchanged. Conflicted original archived as
`int-20261021/archive/runtime_src_shadow_main.before-int-resolve1.conflicted.cpp`.

### Independent check (tools/verify_order.py)
Every one of the 103 touched files was re-merged in the REVERSE order (src-shim -> AU -> EV -> ES -> HW g15g16 -> lies ->
nerw1 -> nerw2 -> nerw2c): 102 results are byte-identical to src-combined; the 103rd (shadow_main.cpp) conflicts at the
same anchor in that order too, and its content with the three marker lines removed is byte-identical to my resolution
(same block order). (`int-20261021/verify_order.txt`)

### Generated inputs
- `extract/liveness-roots.txt`: regenerated with `tools/ppcrecomp/liveness_roots.py --runtime runtime --census
  scratch/address-census-20261005-v1/census-v2.json --out` -> byte-identical to the merged file; `--check` passes
  (1,083 runtime roots, 118 census roots).
- `extract/native-owners.txt`: only EV changed it (copy). `extract/native-owners-r2.txt`: only HW (copy).
- STAGE.json / SHA256.txt written by tools/stage_write.py: 3,110 files, manifest (sha256 of SHA256.txt) 89291c8f9cc866b4...

## 3. Absence proofs (grep over runtime/, tools/, extract/, CMakeLists.txt of src-combined; docs/ and scratch/ excluded)

| Excluded | Patterns | Hits |
|---|---|---|
| G14 (custom D3D12MA asset pools for mesh streams / textures / skin sources) | asset_pool, CreatePool, kAssetBlockBytes, CustomPool, "G14" | 0 |
| GLE frame digest (HW src-gle) | frame_digest, FRAME_DIGEST, track_identities | 0 |
| ES-W5 (sparse card image) | image_read, image_fill_sector (+ W5-only.diff's native_card.cpp / gekko_core_tests hunks: files equal src-shim) | 0 |
| Cheat overlay | test_cheats, TEST_CHEAT | 0 |
| ES census | alloc_census, ALLOC_CENSUS | 0 |
| NE-SAVE | ne_save, NeSave (2 comment mentions "to be replaced ... NE-SAVE" only) | 0 code |
| NE-RW3 / NE-VIDEO / NE-1 | rw_frame_pacing, frameclock, ne_audio, SampleStore | 0 |

The only "GLE" mentions left are comments naming the proof standard (game_memory.h/.cpp, native-owners.txt from EV;
rwim/immediate.cpp from HW).

## 4. Why GLE and G-SHIP are out
- GLE (src-gle): the env switch is under `#if !defined(GEKKO_SHIPPING)`, but frame_digest.cpp, the renderer's
  `track_identities_` member / branches and `impl->digest(packet)` stay compiled in the shipping config (inert, not
  compiled out) -> left out per the brief. NE-RW1 / 2a / 2c were applied as their own deltas (base src-gle), all clean.
- HW G-SHIP (src-ship, on G5): staged, never proven, not in the brief; it would also compile out
  SHADOW_R2_CAPTURE_AT_RENDER_SIZE, which the 4K capture identity check needs.

## 5. Cheat variant (bench only)
`scratch/ch-20261020/src-cheat-int-combined` = CH's cheat_merge.py (overlay v2, the same overlay as build-ch-shim's
src-cheat-shim) on src-combined: 3 new files + 4 shared files (edit script == git merge-file for shadow_main.cpp,
CMakeLists.txt, input_tape.cpp; git merge-file for input_tape.h). It differs from src-combined in exactly those 7
files (manifest e657a1e4...).
