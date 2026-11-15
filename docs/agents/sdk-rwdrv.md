# sdk-rwdrv: groups B + C of the last shipping SDK bodies (2026-11-12)

Agent sdk-rwdrv, branch agent/sdk-rwdrv (cut at b75201a). Contract: docs/agents/SDK_2026-11-12.md. Group D (0x8049A124 and its
entries) was not touched.

## Result

Shipping tree (`.build/watch`, SHADOW_RENDER_LIFTED): platform-SDK bodies compiled **16 -> 11** (frontier rows 13 -> 8),
measured by `removal_plan.py`. The 11 left are the three AXFX callbacks with their 3 interiors (group A, agent sdk-axfx) and the
five group-D entries. `SHADOW_GC_REMOVAL_RATCHET` (lifted) 41 -> 11 (the contract said "lower to the measured count": 11).
The non-lifted default (443) is not changed: the tests tree has no measured value for this change (see "not done").

| row | what it is | owner | trees |
|---|---|---|---|
| 0x80393634 (+ entry 0x80393730) | GX default texture region callback / default TLUT region callback (`__GXData + 0x4C8 / 0x4CC`) | `driver_callbacks::install` (runtime/src/gekko/native_driver_callbacks.cpp), rows in extract/native-owners.txt | both |
| 0x80499A9C, 0x80499AC4, 0x80499AEC | the three 0x28-byte wrappers of the raster render core 0x80499924 (device standard-function table words 0x8051D494 / 0x8051D4A4 / 0x8051D49C) | `driver_callbacks::install_lifted`, rows in extract/native-owners-r2.txt | lifted |
| 0x8049AC88 | `GcnTextureBind(texture, unit)` (0x8E0 bytes, 16 `bl` sites in 12 RW functions) | `rw_texbind::install_lifted` (runtime/src/gekko/native_rw_texbind.cpp), row in extract/native-owners-r2.txt | lifted |

## RE (evidence: pinned main.dol words, tools/doldis.py; the reference decomp agrees on the three names and was only read)

**Group B.** 0x80393634 (0xFC bytes, GX library) and 0x80393730 (0x24 bytes, an entry inside the same generator row). The only
loads of `__GXData + 0x4C8` / `+ 0x4CC` in the whole image are at 0x803983C0, 0x8039845C, 0x803984E8 (found by scanning every
`lwz rX, 0x4C8/0x4CC(rY)` in the text sections): the pre-loaded texture load, GXLoadTexObj and GXLoadTlut. The lifted build serves
those three silent (gx_texobj::install_lifted), so nothing calls the callbacks there; the legacy tree still does. Behaviour:
- 0x80393634(obj r3, id r4): `format = *(obj+0x14)` (GXGetTexObjFmt 0x8039822C), `mip = *(u8*)(obj+0x1F) & 1` (GXGetTexObjMipMap
  0x80398234), `index = id % 8` (signed truncating: srawi 3 / addze / slwi 3 / subfc), `gx = *(r2 + 0xFD8)`. Result
  `gx + base + 16*index`; base: format 6 -> 0x288 without mipmaps, 0x308 with; formats 8..10 -> 0x208 always; every other format
  (signed compare, so negative and >= 11 too) -> 0x208 without mipmaps, 0x288 with.
- 0x80393730(id r3): `id < 20 ? gx + 0x388 + 16*id : 0`.
The runtime's literals (`kGxDataImage` {0x4C8, 0x80393634}, {0x4CC, 0x80393730} in native_gx_sync.cpp) are unchanged: the words
keep pointing at the same addresses, which are owners now, so the generator emits only an owner thunk (generated
recomp_029.cpp: `fn_80393634 ... serve_native_owner(0x80393634u)`, `fn_80393634_e80393730 ... (0x80393730u)`).

**Group C wrappers.** Each is `li r5, A; li r6, B; bl 0x80499924` between a 0x10-byte frame: 0x80499A9C (scaled 0, fast 1),
0x80499AC4 (0, 0), 0x80499AEC (1, 1). 0x80499924 is already an owner (os-other step 13, scaffold::install_raster_render_owner),
so the owner sets r5 / r6 and serves the core through the owner table (`Cpu::serve_owner`), returning its r3. The device
standard-function table words stay as they are.

**0x8049AC88 = GcnTextureBind.** RE'd and owned earlier by agent axfx-effects (docs/agents/axfx-effects.md, "Step L", commit
2370456, unmerged and never run in game: "game gates not run (paused)"). I adopted that commit by cherry-pick (this project's own
code, no external-decomp code) and re-ran its contract test on this base. Summary of what it does: (re)initialises the GXTexObj
inside the texture's plugin data from the raster when the texture is new (flag 0x01000000) or its version halfword differs, records
the texture in the bound table (0x8056FC90 + 0x4C + 4*unit); r3 = 0 selects the default texture. Its three GX loads are silent in
the lifted build, every other callee is an owner already, so the owner is exact and lifted-only.

## Gates

1. **Contract tests against the pinned DOL** (hash 0xD2CFB4A1CA897F9B, reference interpreter), `.build/tests`, Release, capped 10 %:
   - `driver_callbacks_contract_tests` (new, runtime/tests/driver_callbacks_contract_tests.cpp): 9,191 cases, PASS. Tex region: 5,700
     (25 formats incl. negative / > 11 / 0x80000000 x 27 ids incl. every signed edge x 4 mip bytes, + 3,000 random); TLUT region
     1,091; the three wrappers 2,400 (random r3 / r4, random core results; the original core replaced by a recorder, the owner served
     through the installed owner table with a recorder owner at 0x80499924). Compared: r3, r1, LR, r14..r31, f14..f31 both halves,
     every word of a 0x600 arena (incl. a random `__GXData`), the written-word sets (the original writes only its own frame and the
     caller's LR word, the owner nothing), and what the core was handed (r3, r4, r5, r6).
     Negative controls (each caught 40/40, 1,040/1,040 in all): another format word, the mipmap bit flipped, another `__GXData`
     pointer, an extra arena word, r20 clobbered, r1 moved, f31's second half damaged, LR changed, another r3, the owner served at
     another address (x4).
   - `rw_texbind_contract_tests` (adopted): 6,384 cases PASS on this base (its 8 negative controls are part of it).
   - native_file_api_tests (owner list == extract/native-owners.txt, new `driver_callbacks::Owners` entry), native_liveness_roots and
     native_liveness_roots_lifted: PASS.
2. **what_if prediction** (pristine lists of b75201a, `--own` the six addresses, `.build/watch`): `frees 0 rows, revives 0, 0 listed
   sites become work items`, no generator refusal. Zero freed *pruned* rows is the expected figure: all six are leaves whose callees
   are owners already, so an owner replaces the row by a thunk instead of pruning interiors. **Real generator run**
   (`removal_plan.py --release-dir .build/watch --out .build/platform/rp_watch --dolrecon-dir .build/dolrecon --max-platform-bodies 9999`,
   lock generator): bodies compiled 16 -> **11**, frontier 13 -> 8, bodies left if all frontier owned 0; sdk:unidentified 6 (3 AXFX +
   interiors), graphics.rw-gamecube-driver 5 (the group-D entries). The 5 bodies gone are 0x80393634, 0x80499A9C, 0x80499AC4,
   0x80499AEC, 0x8049AC88. SHADOW_GC_REMOVAL_RATCHET (lifted) set to 11.
3. **Westopolis replay**, `--boot-stage stg0100 --headless --no-audio`, 316,000 ms budget, 1280x720, input tape of
   source/.build/platform/ab-westopolis, `--native-screenshot-frame 600 --native-screenshot-stride 900 --native-screenshot-count 6`
   (render frames 600, 1500, 2400, 3300, 4200, 5100; script `.build/run_shots.sh`, a copy of the samp-2026-11-11 run.sh plus the screenshot
   flags), base exe base-b75201a.exe x2 and the candidate (`.build/watch/bin/Release/ShadowTheHedgehog.exe`, commit of this branch) x2:
   state.json md5 3a75268e85cd9037a60d04f739f10686 in all four runs; the six PNGs have the same md5 in all four runs
   (0caae18a..., 83dc2b89..., 74e6113c..., ffef6bca..., fa1b5104..., 8977f208...). Note: the owners are not on the replay's path
   (the bind and wrappers had 0 hits on 38 stage boots in axfx-effects' census, the GX load paths are silent in the lifted build), so
   this gate shows the build is unchanged where it runs, not that the owners ran; their exactness is gate 1. PCM not compared
   (`--no-audio`; no audio code changed).
   After a last refactor (the owner's dispatch reads its own address table instead of `case kName:` labels, which
   `runtime_coupling.py` could not classify: it listed 0x80499A9C / AC4 / AEC at native_driver_callbacks.cpp as three new
   unclassified uses) the driver test (9,191 cases, 1,040/1,040 controls), native_file_api_tests, both liveness tests and
   `ctest -R "^platform_"` were re-run on the tests tree, and the watch exe rebuilt (`.build/cand3.exe`) and replayed once more:
   state.json and the six frames identical to the base again. `platform_gc_removal_plan`, `platform_retired_readers_context`,
   `_reverb`, `platform_rtfs_async_proof`, `platform_closure_proof_tool`: PASS. **`platform_runtime_coupling` FAILS, but only on
   its baseline defect**: 3 unclassified uses of 0x8037ECD4 at runtime/src/gekko/native_vi_wait.cpp:32/45/59 (listed as a baseline
   failure in docs/AGENT_COORDINATION.md; not this work, not touched). My change adds 0 unclassified uses and 0 native -> SDK calls
   (SHADOW_RUNTIME_SDK_CALLS_RATCHET not changed: no runtime coupling freed). native_file_api_reference_tests was not built here.
4. **Liveness lists** regenerated with the native_liveness_roots / _lifted tests' own arguments (3 new roots in each:
   0x80499A9C, 0x80499AC4, 0x80499AEC; the texbind commit adds its two) and both tests pass.
5. **Speed**: not measured; no owner is on the replay path (see gate 3).

## Files

New: runtime/include/gekko/native_driver_callbacks.h, runtime/src/gekko/native_driver_callbacks.cpp,
runtime/tests/driver_callbacks_contract_tests.cpp. Adopted from agent/axfx-effects 2370456: runtime/include/gekko/native_rw_texbind.h,
runtime/src/gekko/native_rw_texbind.cpp, runtime/tests/rw_texbind_contract_tests.cpp (its docs/agents/axfx-effects.md hunk left out).
Shared lists, one block each: extract/native-owners.txt (+2 rows, B), extract/native-owners-r2.txt (+1 row texbind, +3 rows wrappers),
extract/liveness-roots.txt and -lifted.txt (regenerated), CMakeLists.txt (+1 source line, +1 test target, ratchet 41 -> 11 with a comment),
runtime/src/gekko/system.cpp (+include, +1 install line), lifted_render.cpp (+include, +texbind and wrapper install lines),
runtime/tests/native_file_api_tests.cpp (+include, +1 Owners line).

## Not done / open

- Non-lifted ratchet (443) not remeasured: `.build/tests` has no generator output (ppcrecomp was built there only for the ctests below).
- Owner hit counts on a stage that draws through the raster-render wrappers or the bind were not re-run (axfx-effects' census says 0 on 38
  stages, title / attract unrun); the owners are exact by contract test, but no gameplay path exercised them in this session.
- 0x8049AC88's frame words (saved registers, int-to-float scratch) and the wrapper's 0x10-byte frame / LR slot are not reproduced:
  dead below the caller's frame at every call site (checked by axfx-effects for the 16 bind sites; the three wrappers are called only
  through the table word by RwRasterRender's callers).
