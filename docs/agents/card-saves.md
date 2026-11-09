# saves.card behind the save store (docs/GC_REMOVAL_PLAN.md "Next steps" 3) - agent `card-saves`, 2026-11-07

Branch `agent/card-saves`, worktree `wt-agent-card-saves`, cut from `gc-removal` 857399c. Nothing pushed or merged. The
user's saves, cards and SRAM were never opened for writing: every run used copies in `.build/`. The ratchet
(`SHADOW_GC_REMOVAL_RATCHET`) is not changed; the measured counts are below.

## Result in one paragraph

The game's save code (the card job runner `TMemoryCardCtrl`, 0x8029A66C..0x8029C2A4) is now served by the host save
store for good: the runner's `start` 0x8029BF84 and `tick` 0x8029BA8C, `CARDInit` 0x8038DE3C and `CARDGetResultCode`
0x8038E044 are generator-level native owners (extract/native-owners.txt), so the 14 CARD-operation steps, the CARD
library under them and the video-register site inside `__CARDFormatRegionAsync` are not compiled. Measured by the
generator on this tree: platform-SDK bodies compiled **1,044 -> 988 (shipping, .build/watch)** and **1,050 -> 994 (tests
tree)**. `saves.card` has no body left compiled (only the six block-device owner thunks, which nothing reaches).

Scope update (main session, sdk-identify: frontier 17 rows incl. CARDOpen / Close / Create / Read / Write / Delete /
GetStatus / SetStatus / FormatAsync, __CARDFormatRegionAsync, __CARDDefaultApiCallback): re-measured on this tree, all of
them except CARDClose 0x803924F4 and __CARDDefaultApiCallback 0x8038CDD8 were already pruned by the runner owners (they
were called only by the runner steps). The two others: __CARDDefaultApiCallback was a literal of the card model
(now supplied by the reference tests, `set_default_api_callback`; gekko_core_tests uses a stand-in address); CARDClose
and CARDUnmount were called by the runner's destructor path 0x8029C034, whose CARDClose call is lowered to nothing
(extract/lift-sites-native.txt, site 0x8029C078): the store ends every job with R+0x20 = 0 and no job entry sets a flag,
so no file is ever open there. The emulated memory card is no longer a way to run the game: `--legacy-memory-card` now
refuses to start (decision below).

## What was analysed

The six frontier rows of the removal plan (REMOVAL_PLAN.md "saves.card", before this work) and who entered them:

| row | function (evidence) | entered by | now |
|---|---|---|---|
| 0x8038DE3C | CARDInit (extract/functions-dolphin-sigs.csv; body: OSGetFontEncode, OSRegisterVersion, DSPInit, OSInitAlarm, per channel result -3 + thread queue + alarm, __CARDSetDiskID(0x80000000), OSRegisterResetFunction(0x80568358)) | game init 0x801E258C | owner |
| 0x8038E044 | CARDGetResultCode (-128 outside channels 0..1, else `__CARDBlock[chan].result`) | reset path 0x8032D304 (waits while -1) + the runner steps | owner |
| 0x8038E074 | CARDFreeBlocks | runner step 0x8029A8D0 | pruned (runner owned) |
| 0x8038E1C4 | the CARD library's OS reset function (data word 0x80568358, registered by CARDInit) | OS reset queue | data word retired (lift-sites-native.txt), pruned |
| 0x80390680 | __CARDVerify | a runtime literal in native_card.cpp (the card model's mount) | literal removed, pruned |
| 0x80390C9C | CARDCheck | runner step 0x8029B42C | pruned |

Boundary (docs/SAVE_REROUTE_PLAN.md decision A): the manager (`TMemoryCardCtrl`) keeps building the file (banner, icon,
MD5, DES, messages); the runner object R (0x38 bytes, built by 0x8029C1C0) is served by `SaveStore` (save_store.cpp,
written before this work): job entries 0x8029BC74.. stay game words and call the owned `start`; `tick` runs the job on the
GCI and ends it as the runner's finish paths do. Job numbers (R+0x18): 1 format, 2 create, 3 delete, 4 read, 5 overwrite,
6 free blocks (0x8029BC74), 7 scan (0x8029BCC8).

## What was replaced (files)

- `runtime/src/gekko/save_store.cpp`, `runtime/include/gekko/save_store.h`: `SaveStore::addresses()` (the four owners),
  owners for CARDInit and CARDGetResultCode. CARDGetResultCode: -128 outside 0..1 (as the original), slot B -3 (what
  CARDInit leaves), slot A 0. CARDInit keeps the one effect surviving code needs, `DSPInit` (the audio layer's owner);
  everything else it wrote is retired (below).
- `runtime/include/gekko/system.h`, `system.cpp`: comments only plus: a System without `set_save_file` keeps the card
  model (the reference tests' oracle); the game always calls `set_save_file`.
- `runtime/src/shadow_main.cpp`: `--legacy-memory-card` prints a refusal and exits 2; the old-card path
  (`System::set_memory_card_path`) is gone from the program. `--memory-card` remains the import source.
- `runtime/src/gekko/native_card.cpp/.h`: no `__CARDVerify` literal (a runtime literal pinned the body); the mount check
  entry is supplied by the reference tests (`set_verify_entry`), and `deliver_pending` (oracle pump, reference tests only).
- `runtime/src/gekko/native_video.cpp`, `extract/platform-sites.txt`: the VISEL site inside `__CARDFormatRegionAsync`
  (0x80391910) left the list with its function (precedent: SISetSamplingRate's VICLK site); `gekko_core_tests.cpp` uses
  the OSGetFontEncode site 0x80375178 and the in-function address 0x80375180 as its listed / unlisted examples (the
  old addresses were liveness roots into the CARD library).
- New test `runtime/tests/save_store_contract_tests.cpp` (CTest `save_store_contract_tests`, reference label).

Shared list lines changed (all in this subsystem's rows):
- extract/native-owners.txt: appended block `0x8029BA8C`, `0x8029BF84`, `0x8038DE3C`, `0x8038E044`.
- extract/lift-sites-native.txt: appended `retire 0x80568358 0x8038E1C4` (+ comment).
- extract/wait-points.txt: removed the 16 progress points of the runner steps (lines 102-117: 0x8029A7B8 .. 0x8029B814).
- extract/platform-sites.txt: removed the 0x80391910 line.
- extract/liveness-roots.txt: removed 0x80390680, 0x80391910, 0x80391914; added 0x8029BA8C, 0x8029BF84, 0x8038DE3C,
  0x8038E044, 0x80375180. (The file is stale for unrelated reasons - `native_liveness_roots` fails at the baseline.)
- extract/jump-tables.txt: untouched. Lines 85-87 (0x8029B360, 0x8029BB1C, 0x8029BBD0) name pruned rows; the generator
  accepts them (removing them made the thunk tables free-standing data words that kept the whole runner alive: 1,039
  compiled bodies with them removed vs 1,004 with them kept, same other edits - measured both ways). Leave them.
- CMakeLists.txt: the `save_store_contract_tests` target.

## What the generator freed (measured)

`what_if.py --own 0x8029BF84 --own 0x8029BA8C --own 0x8038DE3C --own 0x8038E044 --drop-roots-from native_card.cpp`
(generator lock, before any edit): frees 107 rows (74 game runner rows, 21 sdk:unidentified, 12 saves.card), 0 revived,
no refusal, 19 listed sites stale (3 jump-table lines, 16 wait points). The real tree after the edits
(`removal_plan.py --release-dir .build/watch`): platform bodies compiled 988 (was 1,044; 992 before the CARDClose / default-callback step), frontier rows 272; tests tree
(`--release-dir .build/tests`) 994 (was 1,050); `platform_gc_removal_plan` passes (the ratchet is the old, higher value).
Removing the video site freed 9 more bodies (the format region and its callees). Suggested ratchet values for the main
session: 988 shipping / 994 tests tree.

## Retired state and who could still read it (gate 3)

CARDInit's writes that the owner does not make: the two `__CARDBlock` records (0x805A2CA0, 0x220 B), the encoding word
r13+0x5DF0, the OSReport formatting buffer 0x805A6460 and the debug-text counters 0x80568F38 / 0x80568F54 (the version
line, 70 characters), the font-encode cache words (0x805EEC08, 0x805F1F88: OSGetFontEncode's first call, now at its next
caller), the reset queue links (0x805612FC, 0x80568754) and the CARD reset record 0x80568358.

Guest watch (`--watch-guest`, headless, from boot, candidate exe, ranges in `.build/ab-jobs/cardw-ranges.txt`) on the
Westopolis replay (90 s) and on both save routes (create, import + Options + Load): no guest read of the `__CARDBlock`
records, the encoding word, the font cache words or the CARD reset record (only the harness's own state report and
the loader's memset at 0x8000543C touch them); the reset queue is read and written only by OSRegisterResetFunction
0x80376D6C itself (nobody walks it: OSResetSystem is a native owner that ends the program - the same reasoning as the
alarm module's record); the text counters / buffer only by the printf machinery (0x803A8C7C, 0x803A8D40, 0x803AF5A8,
0x803A9098), whose output goes to the discarded UART. 0x80568748 is GXInit's own reset record (read by 0x80393A88).

Documented behaviours that differ from the original, none game-visible: a job ends inside its tick (the original takes
3-6 ticks; the manager waits for busy == 0); CARDGetResultCode(0) is 0, never BUSY (-1) - its one caller (the reset
path) acts only on -1; the font-encode cache is filled at the next OSGetFontEncode caller instead of at CARDInit.
Precondition of CARDInit's owner: OSInit has run (OSInitAlarm is then a no-op); the test shows what the original does
when it has not (it installs the decrementer handler), which the owner would not.

## Gates (docs/AGENT_COORDINATION.md section 4)

1. Measure first: above (`what_if`, generator lock).
2. Contract test (`save_store_contract_tests`, `.build/tests/Release`, CTest 3 s, reference interpreter, pinned DOL
   0xD2CFB4A1CA897F9B):
   - CARDInit: original words vs the owner on the same fixture: registers, every byte of main RAM outside the retired
     ranges and the dead stack equal; the original's calls (OSGetFontEncode, OSReport, DSPInit) vs the owner's (DSPInit
     only); the retired ranges really differ (not vacuous); precondition shown.
   - CARDGetResultCode: channels -1, INT_MIN, 2, 3, INT_MAX = -128 in both; slot B -3; slot A for each block result the
     original can hold: the reset path's wait decision equal except BUSY (documented).
   - The runner: the original TMemoryCardCtrl runner + CARD library words (job entries, start, tick, 14 operations,
     CARDOpen/Create/Read/Write/Delete/Format/FreeBlocks/GetStatus ... as DOL words) on the card model's block device
     against the store on a GCI, 24 jobs in order (then 6 more on an interrupted file): scan, free blocks, read / delete / overwrite of a missing file (9),
     create (0), create again (10), read whole / first block / more than the file holds (2), overwrite / overwrite with
     more than the file holds (2), delete, create after delete, format, and with an interrupted file (iconAddr
     0xFFFFFFFF, card image and GCI patched identically): scan, read (8), overwrite (8), create over it (10), delete,
     read (9). After every job: started flag, result (R+0x1C), busy / job / flags, free bytes / entries (free job),
     scan count, the read buffer, and the file (directory entry except the clock and the first block, and its data) on
     the card vs the GCI. All equal. Host failure (the file's place is a directory): the store ends with the I/O error
     (5), idle, nothing created (the card model hid such failures, SAVEGAME_BUGS S7; no original to compare).
   - Negative controls (each fails the comparison it targets): an owner that does not reach DSPInit; a stray write
     outside the retired ranges; a CARDInit on a machine without OSInit; a different result code; one flipped data bit; a
     different directory-entry field; a job left busy.
   - Not covered by an original: disk full (needs a full volume) and a host that cannot be written beyond the directory
     case above.
3. Guest watch: above.
4. Game behaviour. Baseline exe = `.build/ab-jobs/base.exe` (built from 857399c in `.build/watch` before any edit);
   candidate = `final.exe` (built from this tree) and the earlier `cand.exe` (same behaviour, before the oracle pump was
   added). Westopolis 90 s, alternating, 4 runs each of base / final (+ 2 each base / cand): PCM `015445f2...` in all,
   3,645,000,000 ticks, 2,697 VI, 5,360 render frames; A/A and B/B memory identical (every 64 KB block hash); A/B: 4
   blocks differ (0x80560000, 0x805A0000, 0x805E0000, 0x805F0000), every word classified (peeks of A vs B, one pair):
   0x805612FC / 0x80568360 / 0x80568754 the reset-queue links and the CARD record (registered in A only);
   0x80568F38 / 0x80568F54 0x3D0 vs 0x38A (70 characters of version text); 0x805A2CA4 / 0x805A2DAC / 0x805A2DB4 / 0x805A2EBC
   the `__CARDBlock` records (-3 / disk ID in A); 0x805A6474 the OSReport buffer's tail; 0x805EEC08 / 0x805F1F88 the
   font-encode cache words (written by OSGetFontEncode only in A). Nothing is replicated.
   Save route (scripted, headless, `.build/save-route/*.uitest`, the earlier "0 jobs" came from a script that did not
   reach the screen: render frames run about 2x the script's VI counter, the START that skips the intro must precede the
   second START, and `--budget-ms` is game time): `save-create.uitest` (no save: the boot check's create: 1 create, 1 scan,
   1 free-blocks job, 1 GCI write) and `imp5.uitest` (an imported save: the boot check's read + scan + free, then main menu
   -> Options -> the Save-data tile -> Load: a second read + scan + free): base vs candidate, 2 each, alternating:
   PCM equal (create `e766546f...`, import `0c61e353...`), ticks 8,910,000,000, VI 6,575, render frames 8,781 / 12,475,
   A/A and B/B identical, A/B the same 4 blocks; the created GCI differs only in the modification-time bytes 0x2A-0x2B
   between any two runs (base vs base too); the imported GCI is byte-identical in all.
   NOT reached by any game route: overwrite (the stage-clear autosave; Options' exit save is skipped by the store by
   design), delete and format (no screen reaches them in the PC build). They are covered by the contract test against the
   original only. To close it with the game running them the user would have to record a tape that clears a stage with
   Autosave on (a cold boot `--input-tape-record` from the title through one stage; the store's counters then show
   `overwrites` > 0). I did not ask the user.
5. Performance (alternating, same binaries, capped runner's job CPU seconds, first of 4 discarded): base 29.7, 29.7, 28.6 vs
   final 29.8, 29.4, 27.1 (mean 29.3 vs 28.8): no measurable change (spreads overlap); wall time per run equal (92.0-92.3 s)
   except the first two final runs, which took 5-7 s longer at process exit (cold copy of a new exe; not repeated).
   No absolute FPS claim.

## CTests (tests tree, Release; the capped runner)

Pass: save_store_contract_tests (new), gekko_core_tests, save_file_tests, save_recognize_tests, platform_gc_removal_plan.
Fail, all at the baseline for the reasons listed in the coordination doc: native_file_api_tests (every "listed, not
installed" row is a CRI owner; no 0x8029xxxx / 0x8038xxxx row; the saves.card owners match), native_liveness_roots (stale
list; the stale-by-me check: no address of this work missing, none extra), platform_runtime_coupling (3 unclassified uses
at native_cri.cpp:370; this work removed one guest-call site: the card model's `__CARDVerify`). No new failure.

## Decisions for the user / open problems

1. `--legacy-memory-card` is retired. It cannot coexist with owners at generator level (the owners replace the runner the
   card model needed). The card model (`NativeCard`) stays in the tree only as the contract oracle of the reference
   tests; it can be deleted when a later step no longer needs an original-on-block-device oracle.
2. The destructor path 0x8029C034 is lowered, not owned (its CARDClose call is dead because the store keeps R+0x20 = 0):
   if a later change lets a job leave a file open, that site (lift-sites-native.txt) must go.
3. Overwrite / delete / format are not exercised by a game route (above).
4. The 3 jump-table lines of pruned rows stay in extract/jump-tables.txt (harmless; see above).

## Re-run after the scope update (final executable `final2.exe`)

Westopolis 90 s, base vs final2 alternating 2 each: PCM `015445f2...`, 3,645,000,000 ticks, 2,697 VI, 5,360 render frames,
A/A and B/B memory identical, A/B the same 4 blocks. Save routes (create, import + Options + Load), 2 each of final2:
PCM / ticks / VI / frames equal to the baseline runs, A/A identical, A/B the same 4 blocks, imported GCI byte-identical,
store counters unchanged (3 jobs / 6 jobs). CTests: save_store_contract_tests (default callback now passed by the test),
gekko_core_tests, save_file_tests, save_recognize_tests, platform_gc_removal_plan pass; native_file_api_tests,
native_liveness_roots, platform_runtime_coupling fail as at the baseline.

---

# os.interrupts (second task, 2026-11-07)

Task from the main session: os.interrupts (16 bodies, 4 frontier rows) after merging gc-removal 99fe8c3. Commit `67eb159` on `agent/card-saves`.

## Rows and who enters them (removal_plan, fresh, `.build/watch`)

| row | name | entered by | decision |
|---|---|---|---|
| 0x8037611C | OSDisableInterrupts | 79 callers: game (22+ rows), CRI 0x803C565C / 0x803C9E30, RW 0x8047AE1C / 0x8047B3D8, 0x8043F3E4 | owned here |
| 0x80376130 | OSEnableInterrupts | RW 0x8047AE1C / AE6C / B410, 0x8043F3E4, OSInit | owned here |
| 0x80376144 | OSRestoreInterrupts | 75 callers (game, CRI, RW) | owned here |
| 0x80371620 | __OSExceptionVector (template) | runtime literal (`os_exception_entry.cpp`, `tests/support/os_entry.h`) | literal replaced by a constexpr word table |

Not frontier rows, entered only through OSInit 0x80370E68 / `__OSInitMemoryProtection` 0x80376B60 / `__OSCacheInit` 0x80372838 (native-boot owns OSInit; the main session confirmed they go with it): `__OSExceptionInit` 0x80371348 (+ template interior/pads 0x80371678/7C/A8/B8), `__OSSetInterruptHandler` 0x80376168, `__OSMaskInterrupts` 0x803764E4, `__OSUnmaskInterrupts` 0x8037656C, `OSSetErrorHandler` 0x80373378. `__OSInterruptInit`, `SetInterruptMask` and the dispatch were already native.

## What replaced them

* `NativeInterrupts::install()` owns the three entries (`interrupt_gate` in `runtime/src/gekko/native_interrupts.cpp`). They change the CPU's real MSR[EE] (the state the guest reads back) and return the previous EE in r3; r4 / r5 / CR0 are left as the original instructions left them (Restore: r4 old MSR, r5 new MSR, CR0 = cmpwi r3,0). The two recognition points the mtmsr was are kept: a condition already deliverable is taken before the write (hand back at the entry), one the write makes deliverable (EE 0 -> 1) is taken at the return.
* `runtime/include/gekko/os_exception_vector.h`: the 38 template words as data plus `os_exception_vector_word` / `write_os_exception_vector`. `Cpu::os_vector_installed` compares the guest's vector page with it instead of reading the SDK body; the five fixtures that relied on a zero template now write the table.
* Not changed: the system-call head still reads PPCSync's words at 0x8037829C (another row).

## Evidence

1. **Measure**: `removal_plan.py --release-dir .build/watch` after the change: platform bodies compiled 958 (963 before), `os.interrupts` 11 live bodies, 0 frontier rows (16 / 4 before). The 11 left are the OSInit-only rows above.
2. **Contract test** `os_interrupts_contract_tests` (new, reference-interpreter-only): original DOL words vs the owners, driven by `run_steps`: 240 cases (3 functions x 5 MSR values x XER SO x Restore arguments 0, 1, 2, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF x with / without a pending external interrupt), all registers, CR, XER, CTR, LR, MSR, pc and the number of interrupts taken equal; 108 external interrupts taken in the original and the same in the owner (through the real vector page into a stand-in handler that restores the OSContext as OSLoadContext does). The table equals the original `__OSExceptionVector` words (37 words + the index patch). Negative controls: a wrong gate (returns EE 0, leaves MSR alone) is told apart in all three functions; different inputs are told apart; a flipped table bit is told apart. (While writing it the first loop ended at the return before the pending interrupt was taken; the test reported handled 1 vs 0, i.e. it catches an owner that does not hand back.)
3. **No retired state**: the owners retire no guest memory (MSR is the CPU's), so there is no surviving-reader scan to run; the template bytes in the image are still copied by `__OSExceptionInit` and nothing reads them from the runtime any more.
4. **Game behaviour**: Westopolis replay, 90 s game time, headless, `osbase.exe` (merged tree before the change) vs `oscand.exe`, alternating, 2 runs each: PCM `015445f2...` in all four, game ticks 3,645,000,000, 2,697 VI, 5,360 render frames; the 64 KB-block memory diff is **0 differing blocks** for A/A, B/B and both A/B pairs (nothing to classify). A `--profile` run of the candidate: OSDisableInterrupts x3,494,677, OSRestoreInterrupts x3,489,134, OSEnableInterrupts x5,506 served by the owners in one replay (game, CRI and RW callers all run in that route; PCM equal covers CRI).
5. **Performance**: 4 runs each (job CPU, 10 % cap): base 25.0, 26.6, 26.6, 26.9 s; cand 26.6, 26.2, 26.5, 25.8 s: spreads overlap, no measurable change.
6. **Tests**: `os_interrupts_contract_tests`, `gekko_core_tests`, `compiler_poll_tests`, `native_execution_tests`, `native_fallback_tests` pass in `.build/tests`. Other CTests were not re-run for this change; the baseline failures listed in docs/AGENT_COORDINATION.md are unchanged by it (not re-measured).

## Shared list edits

* `extract/native-owners.txt`: appended 0x8037611C / 0x80376130 / 0x80376144 (commented block).
* `extract/liveness-roots.txt` (hand-edited, the file is stale at baseline): removed 0x80371620; added 0x8037611C, 0x80376130, 0x80376144 and the data words 0x808000C0, 0x808000D4, 0x80A53000 that the table contains.
* `CMakeLists.txt`: the `os_interrupts_contract_tests` target and test, before `os_settings_contract_tests`.
* Ratchet not changed: measured 958 (shipping tree); the tests tree was not re-measured.

## Open

* The pending-interrupt hand-back at the three entries is exercised by the contract test only; in the game route no interrupt is ever pending at these calls that is not taken at the previous recognition point (the memory and PCM are equal), so the hand-back path is not hit in the replay.
* 11 os.interrupts bodies remain until native-boot owns OSInit.

---

# graphics.gx cluster gxA: the GX state API (third task, 2026-11-07)

Task from the main session: the 26 gxA rows of `docs/agents/gx-rw-plan.md` / `test_data/platform/gx_rw_frontier.csv`, served from what R2 consumes, never a GX register / FIFO model. gc-removal merged first (conflicts only in the two shared lists, resolved by taking gc-removal's file and re-applying my lines). Commit `9fa717d`. Roots files untouched except the lines of the earlier os.interrupts change (the per-build roots are sdk-identify's).

## What the 26 functions do and who reads it

`GXSetTexCoordGen2`, `GXSetNumTexGens`, `GXSetCullMode`, `GXLoadLightObjImm`, `GXSetChanAmbColor` / `MatColor`, `GXSetNumChans`, `GXSetChanCtrl`, `GXSetAlphaCompare`, `GXSetFog`, `GXSetBlendMode`, `GXSetColorUpdate`, `GXSetAlphaUpdate`, `GXSetZMode`, `GXSetZCompLoc`, `GXSetDstAlpha`, `GXSetProjection(v)`, `GXLoadPos/Nrm/TexMtxImm`, `GXSetCurrentMtx`, `GXSetViewport(Jitter)`, `GXSetScissor(BoxOffset)`: each writes fields of the library's shadow `__GXData` (`*(r2+0xFD8)` = 0x805A2F60 here) and sends BP / XF words through the write-gather pipe. `extract/gx-sites.txt` lists every one of those pipe stores as `dead`. The renderer gets draw state as values from the RenderWare driver (`runtime/scaffold/rw_driver/render_states.cpp`, `projection.cpp`, `frames.cpp`), so R2 consumes nothing these calls produce.

Executed in the Westopolis replay (`--watch` hit counts, 90 s, headless): 1 to 10 calls per function, all at start-up (GXInit's defaults, `lr` 0x80394450..) and the first camera begin (RW 0x80495358.., 0x804982EC..) - never per frame. (sdk-identify's step-0 hit counts had not landed; these are my own.)

## What replaced them

`runtime/src/gekko/native_gx_state.cpp` / `.h`: `gx_state::install` serves the 26 entries and writes nothing (the shadow is retired); `System` installs it next to the GX sync owners. The legacy native capture (non-lifted tree) chains its observers in front (`System::add_hle_observer`), unchanged. Nothing models a GX register, a FIFO or a command stream.

## Evidence

1. **Measure** (`what_if --own` the 26): frees 26 rows (the owned ones' interior jump-table rows), revives 0, no generator refusal; 125 listed sites become work items (123 `gx-sites.txt` dead stores inside the bodies, 2 `jump-tables.txt` lines). I left the lists alone: `native_gx_sites_reference_tests` needs the dead-store sites to run the originals (GXSetBlendMode, GXLoadPosMtxImm), and removing the jump-table lines made free-standing data words keep bodies alive in the card change. `removal_plan` after the change: graphics.gx 240 live bodies (292 at the plan's snapshot), 38 frontier rows (64); platform bodies compiled 897 (shipping tree).
2. **Contract test** `gx_state_contract_tests` (new, reference-interpreter-only, uses `extract/gx-sites.txt` like `native_gx_sites_reference_tests`): 72 calls (2 to 4 argument sets per function) of the original words: each returns with r1 restored, reaches no live pipe site and no unlisted store, writes only inside `__GXData` (or the stack frame / the caller's LR slot) and never a vertex-format word (VCD gx+0x14.., VAT rows: what the legacy immediate builder reads); the native owner returns with r1 intact and writes nothing. The retired map (offsets written per function) is printed by the test: +0 (vNum / bpSentNot), +0x80/0x84 tex-coord, +0xA8..0xB4 channel colours, +0xF8/0xFC scissor, +0x1D0..0x1DC cmode / zmode / peCtrl, +0x204 genMode, +0x4D8..0x50C projection and viewport, +0x5AC dirtyState. Negative controls: a body writing an SDA word and a body writing a VCD word are told apart.
3. **No surviving reader that matters** (guest watch, `--watch-guest` over those words, Westopolis, headless, from boot): in the candidate the readers are the harness's state report, the loader's memset (0x8000543C), GXInit (0x80393A88: read-modify-write of its own defaults) and un-owned GX functions that read-modify-write the same words and send dead pipe stores: GXSetPixelFmt 0x80399E58 and 0x80399F2C (cmode / peCtrl), GXSetCoPlanar / GXPixModeSync / `__GXSetSUTexRegs` / GXSetNumIndStages / GXSetNumTevStages (genMode, gxB), and the dirty-state tests in GXFlush 0x80396098 (10,721 reads), GXSetDrawSync 0x80396428 (5,360) and GXBegin / the vertex-state setters. The only live pipe sites they reach (draw-sync token, draw-done, copy trigger, GXBegin header and vertices) take their payload from arguments, not from the retired words (`native_gx_sites_reference_tests` shows the same for GXBegin / GXSetDrawSync / GXCopyDisp).
4. **Game behaviour**: Westopolis 90 s, headless, `gxbase.exe` (merged tree before the change) vs `gxcand.exe`, alternating, 4 runs each: PCM `015445f2...` in all eight, 3,645,000,000 ticks, 2,697 VI, 5,360 render frames; A/A and B/B memory identical. A/B: one 64 KB block differs (0x805A0000) and in it exactly 17 words of `__GXData`, every one inside the retired map (base / candidate): +0x80 3cf3cf00 / 0, +0x84 00f3cf3c / 0 (tex-coord gen); +0xB0, +0xB4 ffffffff / 0 (channel colours); +0xF8 20156156 / 20000000, +0xFC 213d5335 / 21000000 (scissor); +0x1D0 410004bd / 41000004, +0x1D8 40000017 / 40000000, +0x1DC 43000040 / 43000000 (cmode0, zmode, peCtrl); +0x204 00008001 / 0 (genMode); +0x4D8 00000001, +0x4E0 3f800000, +0x4E8 bf800000, +0x4EC c0000000 (projection), +0x4FC 44200000, +0x500 43f00000, +0x508 3f800000 (viewport), all / 0. No other word anywhere differs (the shadow words are not replicated: they are retired). Frames: see the pixel gate below.
5. **Performance**: job CPU, 4 runs each (Westopolis 90 s): base 26.1, 26.6, 22.5, 21.7 s; cand 27.3, 27.7, 21.8, 24.0 s - spreads overlap (the machine's load varied between pairs): no measurable change.
6. **Tests**: `gx_state_contract_tests`, `os_interrupts_contract_tests`, `native_gx_sites_reference_tests`, `native_gx_sync_reference_tests`, `gekko_core_tests` pass; `native_file_api_tests` lists no gx row as missing or duplicated (its baseline failure - CRI owners - is unchanged).

## Shared list edits

`extract/native-owners.txt`: appended the 26 rows (commented block). `CMakeLists.txt`: `native_gx_state.cpp` in the common sources, the `gx_state_contract_tests` target and test. `runtime/src/gekko/system.cpp` / `system.h`: install and include. `runtime/tests/native_file_api_tests.cpp`: `gx_state::Owners` in the collected owners. `extract/liveness-roots.txt`: unchanged by this task (the new source repeats literals the file already holds). Ratchet not changed: measured 897 (shipping tree).

## Pixel gate (added at the main session's request)

Headless, `--capture-at-tick T:file.png --stop-after-capture`, `SHADOW_R2_MSAA=1` (no multisampling, as earlier sessions used it for bit-exact comparisons; without it MSAA can vary one LSB per run on this GPU), Westopolis replay, `gxbase.exe` vs `gxcand.exe` (1920x1080 PNGs, same frame number and game tick on both sides). Six captures across the replay, every pair byte-identical (SHA-256 prefix):

| requested tick | frame / game tick | base = cand |
|---:|---|---|
| 1,000,000 | 3 / 1,620,000 (start-up, right after the GX defaults and the first camera begin) | 922c35a7651b |
| 40,000,000 | 27 / 47,250,000 | 8615941d846c |
| 400,000,000 | 552 / 400,275,000 (gameplay) | d948841c160b |
| 1,200,000,000 | 1737 / 1,200,150,000 | 49e7aa7a82df |
| 2,400,000,000 | 3515 / 2,400,300,000 (street scene, HUD, subtitle) | 2ec2823ec5ad |
| 3,600,000,000 | 5293 / 3,600,450,000 (late) | a1865b85116e |

Control: the baseline captured twice at 400,000,000 gives the same hash (d948841c160b). The frame at 2.4 G was looked at: a real gameplay frame, not a blank target. So the lifted renderer's output, not only its inputs, is unchanged by the 26 owners at these six points.


## Open

* 20 of the 26 rows are pinned only by `rw_capture.cpp` literals; the 26 owners free their bodies now, but the literals still root their callers' code in the lifted tree until the per-build roots land (sdk-identify step 1).
* The retired shadow words still feed gxB (TEV, `GXSetNumTevStages` etc.) and gxC (GXBegin's dirty flush): when those are owned the whole `__GXData` shadow can go; until then they read-modify-write words nothing consumes.


---

# graphics.gx cluster gxC: the vertex path (fourth task, 2026-11-07)

Queue from the main session: gxC, then gxD, then gxB. gc-removal merged first (7e3cda2, the line-ending normalization: my earlier commits made with `core.autocrlf=false` had stored CR in `extract/native-owners.txt`, `native_gx_state.cpp` and the contract test; they are LF now and `git diff --stat` equals `--ignore-cr-at-eol` against gc-removal for every file I touch). Code `af6a9ec`, follow-up `2227172`.

## The eight rows (`test_data/platform/gx_rw_frontier.csv`, hit counts from sdk-identify's step 0) and what each does

| row | what | replay hits | owner |
|---|---|---:|---|
| 0x80396428 | GXSetDrawSync(token): BP 0x48 (token + interrupt request) then BP 0x47 (token) through the pipe - the two live `draw_sync_token` sites - behind a dirty-state flush and an interrupt mask / restore pair | 5,360 (once per frame, RW show-raster 0x80495E08) | `NativeGxSync` owner (both builds): `on_draw_sync_token(token, true)` then `(token, false)`, nothing else |
| 0x80395D38 GXInvalidateVtxCache | one dead pipe store | 29 | `gx_state` silent owner (both builds) |
| 0x8039A044 GXCallDisplayList | dirty-state flush + the call words, all dead transport | 0 | `gx_state` silent owner (both builds; the legacy capture's observers chain in front) |
| 0x80395758 GXClearVtxDesc, 0x8039530C (switch on the attribute), 0x80395790 (switch with a format row), 0x80395CF4 (CP array base, GXSetArray shape) | the vertex descriptor / attribute-format / array shadow (`__GXData` VCD +0x14/+0x18, VAT rows +0x1C/+0x3C/+0x5C, +0x4D0/+0x4D4/+0x5A8) | 1, 0, 0, 16 (GXInit) | lifted build only (`native-owners-r2.txt`, `gx_state::install_lifted` from `LiftedRender::install_owners`): served, silent |
| 0x80396934 GXBegin | header opcode + vertex count: two live sites | 0 | lifted build only: refused (stops the machine naming GXBegin) |

Why the split: the vertex shadow words are exactly what the legacy tree's immediate builder (`native_immediate.cpp`) reads, and that builder is not compiled in the lifted build, where the GX immediate sites are dead and fail closed (`extract/gx-fail-sites-lifted.txt`, `System::GxSiteRouter`: "the lifted build has no GX immediate path"). A refusing GXBegin stops one step earlier than its first dead site would; nothing that ran before can differ. The legacy tree keeps the original bodies.

## Evidence

1. **Measure** (`what_if --own` the 8): frees 37 additional rows, revives 0, no refusal, 39 listed sites become work items (left in place, as in gxA). After this and gc-removal's per-build roots: `removal_plan` graphics.gx 185 live bodies, 29 frontier rows, platform bodies compiled 810 (shipping tree; the number includes sdk-identify's root work merged since gxA, so gxC's own share is the 37).
2. **Contract test** (`gx_state_contract_tests`, now 32 functions / 97 calls + GXBegin + GXSetDrawSync): the originals of the six silent rows return, restore r1, reach no live pipe site, and write only `__GXData` shadow words and stack frames; the four vertex setters do write the VCD / VAT words (their retired map: +0x14/+0x18, +0x1C..+0x38, +0x4D0/+0x4D4, +0x5A8, +0x5AC), which the test asserts only for the lifted-only rows (the common owners are asserted NOT to write a vertex-format word). The owners write nothing. GXBegin: the original emits exactly `opcode`, `count` (3 argument sets); the lifted owner stops the machine with a message naming GXBegin, writes nothing and reaches no site; without the lifted owners (legacy arrangement) the original runs (negative control). GXSetDrawSync (10 cases: 5 tokens x clean / dirty state): the original through the real `System`-style router into a `NativeGxSync` equals the owner on the token `GXReadDrawSync` returns, the token and token-interrupt counts, r1, LR, callee-saved r14..r31 and MSR (the mask / restore pair nets out); a different token is told apart.
3. **Game behaviour**: Westopolis 90 s, headless, `gxcbase.exe` (merged tree before gxC) vs `gxccand.exe`, alternating, 2 runs each plus 2 peek runs: PCM `015445f2...`, 3,645,000,000 ticks, 2,697 VI, 5,360 render frames in all; A/A and B/B memory identical. A/B: two 64 KB blocks differ, **two words**: (a) 0x805A2F74 = `__GXData` +0x14 (VCD low): base 0x00000200, candidate 0 - a retired vertex-descriptor shadow word, written by GXClearVtxDesc / the descriptor setters; (b) 0x8060C494: base 3, candidate 1 - a slot in the main game thread's stack (the region is a chain of frames: back-chain words 0x8060C498 / 0x8060C4B8 ... with return addresses 0x80379390, 0x80495E44 beside it): the word is the 8-byte frame linkage / local slot that hundreds of different functions use as their frames come and go (a watch armed over 0x8060C490..0x8060C4A0 for the last 0.025 s of the run shows 20 to 269 reads each from at least 19 distinct functions), so its end-of-run value is whatever the last callee wrote - dead stack garbage, not replicated.
4. **No surviving reader** (guest watch over the vertex words, headless, cand with its own symbol map): readers are the harness's state report and un-owned GX library functions that read-modify-write the same words and send dead pipe stores - GXInit's setup 0x803938E4, GXSetVtxAttrFmtv 0x803959EC (104 / 88 / 80 reads of VAT A / B / C), 0x80395C6C, and the dirty-state tests (GXFlush 10,721, GXSetDrawSync's old body is gone). No RenderWare or game code reads them; `gx+0x14` is read only by the state report.
5. **Pixel gate** (as for gxA: headless `--capture-at-tick`, `SHADOW_R2_MSAA=1`, same six ticks): all six captures byte-identical, base vs candidate (SHA-256 prefix 922c35a7651b, 8615941d846c, d948841c160b, 49e7aa7a82df, 2ec2823ec5ad, a1865b85116e at frames 3, 27, 552, 1737, 3515, 5293), and equal to the gxA run's six hashes (same frames, same pixels). Control: the gxA baseline captured twice at 4e8 gave one hash.
6. **Performance**: job CPU base 29.7, 26.4 s vs candidate 26.5, 27.1 s (Westopolis 90 s): no measurable change (the call frequency is 5,360 / replay for GXSetDrawSync; the rest are init-time).
7. **Tests**: `gx_state_contract_tests`, `os_interrupts_contract_tests`, `native_gx_sites_reference_tests`, `native_gx_sync_reference_tests`, `gekko_core_tests` pass; `native_file_api_tests` shows no GX row missing or duplicated (baseline CRI failure unchanged). `extract/liveness-roots-lifted.txt` regenerated with `native_liveness_roots_lifted`'s exact arguments (+2 roots: the new owners' literals).

## Shared list edits

`extract/native-owners.txt`: +3 rows (0x80395D38, 0x8039A044, 0x80396428). `extract/native-owners-r2.txt`: +5 rows (the lifted vertex path). `extract/liveness-roots-lifted.txt`: regenerated (+2). Sources: `native_gx_state.{h,cpp}` (two more common rows, `lifted_addresses` / `install_lifted`), `native_gx_sync.cpp` (GXSetDrawSync owner and row), `lifted_render.cpp` (`gx_state::install_lifted`). No roots, gx-sites or jump-table lines deleted.

## Open

* GXSetDrawSync's replacement stays inside NativeGxSync; the draw-done twin GXDrawDone (0x803964E8, gxD) is the next one.
* The legacy (non-lifted) tree still runs the original vertex setters and GXBegin (its immediate builder needs them); only the lifted build drops them.

---

# graphics.gx cluster gxD: init / sync / misc (fifth task, 2026-11-07)

Code `e0ca801`, roots `1ecc9fd` on `agent/card-saves` (7e3cda2 line-ending normalization already merged; endings checked: `git diff --stat` equals `--ignore-cr-at-eol` for every file).

## The eleven rows and what happened to each

| row | what | replay hits | now |
|---|---|---:|---|
| 0x80393A88 GXInit(base, size) -> FIFO object | `OSRegisterVersion` (+ its printed text), `GXSetMisc(1, 0)`, `__GXFifoInit`, `GXInitFifoBase` (0x80394AF4), `GXSetCPUFifo`, `GXSetGPFifo`, OS reset-function registration, `__GXPEInit`, then `__GXInitGX`: the whole default shadow (`__GXData`, texture regions, ...) | 1 (RW 0x8049522C, t = 0) | `NativeGxSync` owner `init`: calls the existing owners (`__GXFifoInit`, `GXSetCPUFifo`, `GXSetGPFifo`, `__GXPEInit`) in the original's order, writes the eight words `GXInitFifoBase` wrote (base, base + size - 4, size, size - 0x4000, size / 2, base, base, 0), returns the library's static FIFO object 0x805A2EE0 |
| 0x80396004 GXSetMisc (switch on the token) | shadow flags (+0, +2, +4, +0x5A9, +0x5AA, +0x5AC) | 2 (game boot 0x80051028, GXInit) | `gx_state` silent owner (both builds) |
| 0x803964E8 GXDrawDone | BP 0x45 = 2 (the live draw-done site 0x80396510) then a sleep on the finish queue | 0 - **no caller in this image**; its only entry was a test's name for the site address | `NativeGxSync` refusing owner (stops naming GXDrawDone) |
| 0x8039A648 + 0x8039A700, 0x8039AE44 / 54 / 64 / 74 (GXSetGPMetric and its split pieces) | five CP performance-select 16-bit stores behind GXSetGPMetric, whose only call was inside `__GXInitGX` (both metrics NONE) | 1 (entry) | gone with GXInit: the five platform sites (0x8039A754, 0x8039AE4C / 5C / 6C / 7C) and GXInit's (0x80393FF4) leave `extract/platform-sites.txt` and `NativeGxSync` (`platform_sites`, `is_platform_site`, `platform_read16/write16`, the `performance selects` counter -> `inits`); the generator is no longer given a literal for them |
| 0x80396EAC (the game's `ScreenTest` caller 0x80306888, 568 B, a float helper that calls 0x803A1DC8) and 0x803970E4 (GXSetDispCopyYScale shape: 256 / vscale, returns the line count from the copy-source height `gx+0x1E4`, writes BP stores) | | 0 and 3 (RW 0x80495424 x2, GXInit's `__GXInitGX` caller) | NOT owned yet: both are called by the game's ScreenTest debug code 0x80306888, which `removal_plan` still reaches; 0x803970E4 returns a value computed from `__GXData`, so it needs a native model of the display-copy source (open, below) |

## Evidence

1. **Measure.** `removal_plan` after the change: graphics.gx 83 live bodies (185 after gxC), 20 frontier rows (29 after gxC): the 2 rows above and the 18 gxB (TEV) rows; platform bodies compiled 706 (shipping tree).
2. **Contract test** (`gx_state_contract_tests`): GXInit, three argument sets (base / size 0x81400000 / 0x40000, 0x81000000 / 0x20000, 0x81234560 & ~0x1F / 0x100000): the original words on a bare machine (OS interrupts masked, 0xFFFFFFE0) against the owner on `NativeGxSync` + `NativeInterrupts`. Both return the library's FIFO object, reach no live pipe site and leave r1, LR and r14..r31 equal. The memory of the union of the two write sets is compared word by word: every word outside the named retired ranges equals (48 words across the three sets: the FIFO object's eight words, the CPU / GP FIFO SDA pointers, `CPGPLinked`, the OS global mask 0x800000C4 and the finish queue, ...); the retired ranges are listed in the test with a reason each (`__GXData`, the OS reset record and queue links, the printf stream state, the version string / node / list head, the hardware register pointers, GXInit's one-time flag, printf's SDA statics, the stack). Negative control: an owner that skips GXSetGPFifo differs on the compared words. GXSetMisc: 6 argument sets, original writes only `__GXData` shadow words, owner writes nothing. GXDrawDone: the draw-done site is a listed `draw_done` row; the owner stops the machine naming it. `native_file_api_tests`'s platform-site check ("the platform sites list matches NativeVideo and NativeGxSync") passes with the six lines removed; its baseline CRI owner-list failure is unchanged.
3. **Game behaviour**: Westopolis 90 s headless, `gxdbase.exe` (the committed tree before this change) vs `gxdcand.exe`, alternating, 2 runs each: PCM `015445f2...`, 3,645,000,000 ticks, 2,697 VI, 5,360 render frames in all four; A/A and B/B memory identical. A/B: four 64 KB blocks differ; every differing word (peeked and diffed): (a) 0x805A2F60..0x805A3510: 232 words of `__GXData` (the default shadow GXInit initialised: texture-region descriptors 0x805A3168.., TEV / TX defaults, the GX state words) - retired; (b) 0x805F22F8..0x805F2308 (four hardware register pointers, `__piReg` .. `__memReg`) and 0x805F231C (GXInit's one-time flag): base 0xCC003000 / CC000000 / CC001000 / CC004000 and 1, candidate 0 - retired; (c) 0x805612FC, 0x80562600, 0x80568754..0x8056875C: the OS reset queue links GXInit's registration made (and its record) - retired: `OSResetSystem` is an owner that ends the program and never walks the queue; (d) 0x80568F38, 0x80568F54: the printf stream's character counters (0x38A / 0x346) of the version banner OSRegisterVersion printed to the discarded UART - retired; (e) **one word outside GX**: 0x805CA36C, base 0xE0000000, candidate 0xA0000000 (bit 30). A watch over the whole 90 s run (cand) shows it is read once and written once, both by the same function 0x803DB528 (a runtime-library routine, t = 46,575,000, same arguments and call chain on both sides), never read afterwards: a dead word whose value comes from a local the function does not initialise (the stack under it differs because GXInit's printf call chain no longer runs); not replicated.
4. **No surviving reader.** Guest watch, cand, full 90 s replay: the four hardware register pointers and the one-time flag (0x805F22F8..0x805F231C) have **no guest reader** (only the harness's state report); the word 0x805CA36C as above; over the whole `__GXData` (0x600 bytes) the readers are the state report, `NativeGxSync`'s owners (CP control shadow), and un-owned GX functions that read-modify-write shadow words and send dead pipe stores: GXFlush 0x80396098 (10,721), GXSetPixelFmt-shape 0x80399E58, 0x80396BD4, 0x80396CCC, 0x80397430, 0x803992C8 (706, a TEV register setter, gxB), 0x80399FCC, and 0x803970E4 itself. RenderWare reads `gx+0x08` (the CP enable word the `NativeGxSync` owners maintain) at 0x80494A2C / 0x80494BE8 (5,349 reads, once per frame) / 0x80494F18 / 0x80495CBC (ShowRaster); that word is not retired and is equal on both sides.
5. **Pixel gate** (headless `--capture-at-tick`, `SHADOW_R2_MSAA=1`, the same six ticks): all six captures byte-identical base vs candidate (922c35a7651b, 8615941d846c, d948841c160b, 49e7aa7a82df, 2ec2823ec5ad, a1865b85116e at frames 3, 27, 552, 1737, 3515, 5293), equal to the gxA and gxC runs; control (baseline twice at 4e8) one hash.
6. **Performance**: job CPU base 26.5, 26.7 s; candidate 26.2, 27.8 s: no measurable change (GXInit runs once).

## Shared list edits

`extract/native-owners.txt`: +3 rows (0x80393A88 GXInit, 0x803964E8 GXDrawDone, 0x80396004 GXSetMisc). `extract/platform-sites.txt`: the six `gx_performance_select` lines removed, as the main session asked; no gx-sites / jump-table / roots lines were deleted by hand. `extract/liveness-roots-lifted.txt` regenerated with `native_liveness_roots_lifted`'s exact arguments (-6: the six site literals). Sources: `native_gx_sync.{h,cpp}` (GXInit / GXDrawDone owners, the performance-select sites and their routing removed), `system.cpp` (the routing and the stats line), `native_gx_state.cpp` (GXSetMisc), tests (`gekko_core_tests`: the "native GX platform sites" test removed with the mechanism it tested; `native_file_api_tests`; `gx_state_contract_tests`). Ratchet not changed: measured 706 (shipping).

## Open

* 0x80396EAC / 0x803970E4 (the two rows the game's ScreenTest debug code reaches): not owned. 0x803970E4 returns a value computed from the copy-source height in `__GXData`; owning it needs the display-copy source set natively first. 0x80396EAC is a pure float helper (two u16 heights in, a double loop, one call to the float-to-integer routine 0x803A1DC8) whose caller is debug code.
* RW's ShowRaster 0x80495CBC (5,360 hits) still carries the XFB copy logic; the main session's note puts it with the copy-display work; not touched here.

---

# graphics.gx cluster gxB: TEV / indirect state, and the structural-match finds (sixth task, 2026-11-07)

Code `a4b3cfb` on `agent/card-saves` (gc-removal merged first, 157005e; line endings checked as before).

## Rows

After gxD the generator's graphics.gx frontier was 20 rows: the 18 gxB rows (all entered only by RenderWare's RpMatFX / multitexture / skin / world nodes - 0x804A4E00, 0x804A7334, 0x804A7AAC, 0x80449D54, 0x804A0B68 - and, until GXInit became an owner, by `__GXInitGX`) and the two ScreenTest rows. Owned now, all in `gx_state` (both trees; the legacy capture's observers chain in front):

* 18 silent owners: 0x80398828, 0x80398E14 GXSetTevIndirect, 0x80398E80, 0x80398FF8 GXSetIndTexCoordScale, 0x8039913C GXSetIndTexOrder, 0x80399228 GXSetNumIndStages, 0x8039924C GXSetTevDirect, 0x803992EC GXSetTevOp, 0x80399378 / 0x803993BC GXSetTevColorIn / AlphaIn (shape), 0x80399400 / 0x80399468 GXSetTevColorOp / AlphaOp (shape), 0x80399530, 0x80399594 GXSetTevKColor, 0x803995F8 / 0x80399654 GXSetTevKColorSel / KAlphaSel (shape), 0x80399848 GXSetTevOrder, 0x803999E4 GXSetNumTevStages. Names are Dolphin shapes (the main session's note: same-shape collisions are possible, e.g. the csv names both 0x80399378 and 0x803993BC GXSetTevColorIn); the contract test, not the name, is the evidence.
* Four finds of `tools/platform/structure_match.py` that are exact bodies: 0x80399294 (empty: `blr`), 0x803952FC (`lwz r3, 0x5E20(r13); blr` = the CPU FIFO pointer getter, RW's ShowRaster calls it every frame), 0x803982B8 (`lwz r3, 24(r3); blr`, a texture-object getter), 0x80394F3C (swap of the SDA `BreakPointCB` 0x5E34 with interrupts off and restored, returning the old value = GXSetBreakPtCallback; RW's XFB ring uses it).

## Evidence

1. **Measure**: `removal_plan` after the change: graphics.gx 45 live bodies, 2 frontier rows (the two ScreenTest rows), platform bodies compiled 484 (shipping tree; the figure also carries gc-removal's merges since gxD).
2. **Contract test** (`gx_state_contract_tests`, now 52 silent functions / 164 calls + 3 small owners): every one of the 19 new silent originals (2 to 5 argument sets each, including out-of-range-looking values) returns, restores r1, reaches no live pipe site, writes only `__GXData` shadow words and stack frames and never a vertex-format word; the owners write nothing. The three small owners against their original words (5 cases x EE 0 / 1): r3, every guest write (address and value), MSR, r1 and LR equal; negative control: the swap's result is the previous callback, and an owner that returned the new one is told apart.
3. **Game behaviour**: Westopolis 90 s headless, `gxbbase.exe` (the tree before gxB) vs `gxbcand.exe`, alternating, 2 runs each: PCM `015445f2...`, 3,645,000,000 ticks, 2,697 VI, 5,360 render frames in all four; memory identical in every pairing - **0 differing blocks** in A/A, B/B and both A/B. Exercised in the replay (`--profile`, candidate): GXGetCPUFifo x5,360 (once per frame), GXSetBreakPtCallback x2, GXInvalidateVtxCache x28, GXSetBlendMode x4, GXSetChanMatColor / ChanCtrl x8, and a dozen state setters x2; the 19 TEV rows have 0 hits (their only caller in the replay was `__GXInitGX`, gone with GXInit).
4. **No surviving reader that matters** (guest watch over `__GXData`, cand, full replay): the readers are the state report, `NativeGxSync`'s owners, GXFlush's dirty-state test (10,721), 0x80396BD4, 0x80396CCC, 0x80397430, 0x803992C8, 0x80399E58, 0x80399FCC, 0x803970E4, and RenderWare's `gx+0x08` reads (0x80494A2C / 0x80494BE8 / 0x80494F18 / 0x80495CBC), the CP-enable word `NativeGxSync` maintains - the same list as after gxD; none of the TEV owners added a reader.
5. **Pixel gate** (as before): six captures at ticks 1e6 .. 3.6e9, all byte-identical base vs candidate (922c35a7651b, 8615941d846c, d948841c160b, 49e7aa7a82df, 2ec2823ec5ad, a1865b85116e), control (baseline twice at 4e8) one hash.
6. **Performance**: job CPU base 23.5, 25.3 s; candidate 23.1, 24.6 s: no measurable change.

## Shared list edits

`extract/native-owners.txt`: +22 rows (one commented block); `extract/liveness-roots-lifted.txt` regenerated (+1); `runtime/src/gekko/native_gx_state.cpp`, `runtime/tests/gx_state_contract_tests.cpp`. No roots, gx-sites, jump-table or platform-site lines edited by hand. Ratchet not changed (measured 484).

## Open

* The last two graphics.gx rows, 0x80396EAC and 0x803970E4, are called from the game's 0x80306888 (ScreenTest). 0x803970E4 (GXSetDispCopyYScale shape: `256 / vscale` through 0x803A1DC8, BP stores, and a returned line count from `gx+0x1E4`) executes 4 times in the replay; owning it needs the display-copy source set natively. Not done here.

---

# graphics.gx: the display-copy scale pair, the last two rows (seventh task, 2026-11-08)

Code `12ed5ee`. The two rows left after gxB (0x803970E4 and 0x80396EAC, entered by the game function 0x80306888 whose SISetSamplingRate call is already lowered to nothing; 0x803970E4 also by RenderWare's 0x80495424) are the SDK's GXSetDispCopyYScale and GXGetYScaleFactor by shape. They are arithmetic, not hardware logic, so they are ported exactly and proved against the original words.

* `GXSetDispCopyYScale(vscale) -> lines` (0x803970E4): `scale = cvt_fp2unsigned(256.0f / vscale) & 0x1FF` (`__cvt_fp2unsigned` 0x803A1DC8: below 0 -> 0, 2^32 and above or NaN -> 0xFFFFFFFF, else truncation); the shadow keeps `bpSentNot = 0` (gx+2) and cpDisp bit 10 = (scale != 256) (gx+0x1EC), both still read by GXCopyDisp; the answer is `((Y << 8) / scale) + 1` (+1 more when 128 < scale < 256 and the odd part of the scale divides the height), at most 1024, with Y + 1 = the copy height from the shadow word gx+0x1E4 (bits 10..19); a zero scale divides to 0 as `divwu` does.
* `GXGetYScaleFactor(efbHeight, xfbHeight) -> f32` (0x80396EAC): the same line count inlined three times in a search that steps the target height down while the count is above the xfb height and up while it is below, answering the ratio before the last step up (float arithmetic, `fdivs` single rounding).
* Both are `gx_state` owners (both trees): the first because the shadow words it keeps are read by the unowned GXCopyDisp, so it writes exactly what the original wrote, the second pure.

## Evidence

1. **Measure**: `removal_plan`: graphics.gx 43 live bodies, **0 frontier rows** (from 2); platform bodies compiled 482.
2. **Contract test** (`gx_state_contract_tests`, `test_scale_functions`): 274 cases of the original words against the owners: GXSetDispCopyYScale over 16 vscales (1.0, 0.5, 2.0, 0.75, 1.0833333, 1.5, 3.0, 0.1, 0.9, 1.0001, 1.9, 0.6, 0.55, 16, 100, 255) x 8 copy heights (1, 7, 240, 448, 480, 528, 574, 1024) x 2 low-bit patterns, plus 4 specials (vscale 512 and 1e30 give scale 0, -1, 0): r3 equal, every guest write (the two shadow words) equal in address and value, callee-saved GPRs and FPRs, r1, LR, no live pipe site; GXGetYScaleFactor over 14 (efb, xfb) pairs (480/480, 448/480, 480/448, 528/480, 480/574, 240/480, 480/240, 574/480, 448/448, 600/576, 240/240, 1/1, 1024/480, 448/524): f1 bit-equal. Pairs whose search never ends in the original either (an efb height of 2 against 480 lines) are excluded. Negative control: different efb heights give different factors.
3. **Game behaviour**: Westopolis 90 s headless, `gxsbase.exe` (= the gxB candidate) vs `gxscand.exe`, alternating, 2 runs each: PCM `015445f2...`, 3,645,000,000 ticks, 2,697 VI, 5,360 render frames, memory identical in every pairing (0 differing blocks). `--profile`: GXSetDispCopyYScale served x2 in the replay (RW 0x80495424), GXGetYScaleFactor x0.
4. **Pixel gate**: six captures (ticks 1e6 .. 3.6e9, `SHADOW_R2_MSAA=1`) byte-identical base vs candidate (922c35a7651b, 8615941d846c, d948841c160b, 49e7aa7a82df, 2ec2823ec5ad, a1865b85116e), control one hash.
5. **Performance**: job CPU base 20.9, 20.6 s, candidate 20.7, 21.0 s: no measurable change.

Shared list edits: `extract/native-owners.txt` +2 rows, `extract/liveness-roots-lifted.txt` regenerated (+0 net). Ratchet not changed (measured 482).

## State of graphics.gx

43 bodies are still compiled but none is entered by game, RenderWare-lowered or runtime code: they are callees of the rw-gamecube-driver cluster (the RwDevice standard-function tables, the Gcn raster / camera / texture natives) and go with it, not on their own. The GX state, vertex, init / sync and TEV rows are all owned; the GX shadow `__GXData` is written only by the unowned remainder of the library and read by nothing that reaches a live sink.

---

# graphics.rw-gamecube-driver, cluster rwR2: the raster life cycle, slice 1 (eighth task, 2026-11-08)

Code `9048e07` (with `8a05143`) on `agent/card-saves`; gc-removal 1858197 merged first (`f217519`), sdk-identify's guest-call seam cherry-picked as `95d59bb` (af380fa).

## Split

Settled with main, vi (rwR1 rows: GcnSystem, ShowRaster, RasterSetImage, RGB/Pixel, Im2D, render wrappers) and sdk-identify (native-texture read, ImageFindRasterFormat): mine is the raster life cycle (RasterCreate 0x80497CA8, Destroy 0x80498058, TextureSetRaster 0x8049819C, GetMipLevels 0x804966D8, Lock 0x80496CA4, Unlock 0x80497104 / UnlockEnd 0x804972B8, UnlockPalette 0x80497388, the format setup 0x80497458 and the level allocator 0x80497A70). Main agreed that I also keep SubRaster 0x804981B8, SetContext 0x8049A570 and the helper 0x8049B7DC.

## Slice 1 (owned now)

`runtime/src/gekko/native_rw_raster.{h,cpp}` (`rw_raster::install`, common list, both trees): 0x8049819C, 0x804981B8, 0x8049A570, 0x804966D8, 0x8049B7DC (the draw-sync token test: GXReadDrawSync is NativeGxSync's token; outstanding tokens are the ring window (cur, next]) and 0x80498058 RasterDestroy (own parent, pixels owned, type 0/4/5: issue the raster's last token if it is the next one - `NativeGxSync::set_draw_sync`, new, the same call GXSetDrawSync's owner makes -, check it is reached, drop the driver's texture binding, free the pixel block through the engine's free function with `guest_call`, the seam: a game RW function, never an SDK one). Refused with a message instead of ported: the error types 3 / 6+ (the original reports through RwErrorSet; no such raster exists in the game), a token that would never be reached (the original spins), a negative raster size in GetMipLevels (the original's shift loop never ends).

## Evidence

1. **Contract test** `rw_raster_contract_tests` (original words in the reference interpreter vs the owners, both with NativeGxSync): 2,019 small-row cases, 1,650 token-test cases (631 reached / 1,019 waiting, boundaries of the 0xE000 ring), 1,620 Destroy cases (types, flags, parent, token kinds, bindings, ring wrap); r3, every non-stack guest write (address and value), callee-saved registers, r1, LR, the sync token and tokens consumed, and the free callback's calls equal. Negative controls: no token issued / the ring wrap / the refused cases.
2. **Game behaviour**: Westopolis 90 s headless, `rw2base.exe` (= f217519 owners and roots) vs `rw2cand.exe`, alternating, 2 runs each: PCM `015445f2...`, 3,645,000,000 ticks, 2,697 VI, 5,360 render frames in all four; **0 differing memory blocks** in A/A, B/B and A/B (heap included: the free is the original's). Exercised in the replay (`--profile`): RasterDestroy x79, TextureSetRaster x22, SubRaster x12,761, token test x5,376; GetMipLevels and SetContext 0 hits (contract test only).
3. **Pixel gate** (`SHADOW_R2_MSAA=1`, headless captures): six ticks 1e6 .. 3.6e9 byte-identical base vs candidate (922c35a7651b, 8615941d846c, d948841c160b, 49e7aa7a82df, 2ec2823ec5ad, a1865b85116e), control hash equal.
4. **Performance**: job CPU base 30.9, 30.3 s; candidate 31.7, 29.2 s: the spreads overlap, no measurable change.
5. **Measure** (`removal_plan`, candidate): platform bodies compiled **390** (shipping tree), frontier 108; graphics.rw-gamecube-driver 183 bodies / 44 frontier rows, graphics.gx 43 / 0. gc-removal's ratchet is 421 (measured at 1858197); I did not build a separate baseline for this change, so no delta is claimed. Ratchet not changed.
6. `native_file_api_tests`: no mismatch for the six new rows; it still fails on other agents' rows (0x80376218.., 0x803C9E7C.. "listed, not installed") - not mine.

## Open

* Create / Lock / Unlock / UnlockEnd / UnlockPalette / 0x80497458 / the level allocator 0x80497A70 (which calls GXInitTlutObj 0x80398490 and DCInvalidateRange, so owning it retires those two SDK calls) follow in the next slices; Create and Lock allocate through the engine (guest_call) and convert tiles (0x80496A10).

## graphics.gx gxD fix: the GXInit tail state (ninth task, 2026-11-08) - commit 60b476a

**Bug** (strict reference oracle stopped at PC 0x4, LR 0x803984F4): the native GXInit (gxD, e0ca801) did not run the original's tail `__GXInitGX` (0x80394088), so the whole `__GXData` image stayed zero - the texture / TLUT cache region tables (+0x208..+0x388), the two region callbacks (+0x4C8 = 0x80393634, +0x4CC = 0x80393730, installed by GXSetTexRegionCallback / GXSetTlutRegionCallback at 0x803944CC / 0x803944D8) and the BP-register shadows. gxD had retired "the __GXData shadow" as unread; wrong for guest `GXLoadTexObj` / `GXLoadTlut`, which call through the callbacks (a null one jumps to address 0). Since e0ca801.
**Fix** (in the owner, not the runner): `NativeGxSync::init` writes the 249-word image the original leaves (constants; the same for all FIFO arguments tried). `runtime/src/gekko/native_gx_sync.cpp`.
**Evidence.** `gx_state_contract_tests`: `__GXData` is no longer retired - 846 surviving words equal across 3 argument sets; callbacks asserted; the ORIGINAL GXLoadTlut (0x803984C8) and GXLoadTexObj (0x8039843C) run to their return after the owner's GXInit; negative control (null +0x4CC: GXLoadTlut does not return). Westopolis A/B (alternating x2, PCM `015445f2...`, 3,645,000,000 ticks, 2,697 VI, 5,360 frames; A/A and B/B memory identical); A/B differs in exactly 245 words, all inside `__GXData` (0x805A2F60..+0x600) - the image. Pixel gate: 6 ticks (1e6..3.6e9, `SHADOW_R2_MSAA=1`, headless), byte-identical. Ratchet: shipping 350 vs 349 (+1: the default region callback 0x80393634 / 0x80393730 is a root again; axfx-effects' lifted GXLoad* owners e0ca4d2 make it prunable in shipping once merged).
**Not run:** the real `ShadowTheHedgehogReference --verify-player-strict` (no oracle exe / recording here); the contract test reproduces its exact crash path.

## graphics.rw-gamecube-driver, cluster rwR2: the raster life cycle, slice 2 (tenth task, 2026-11-08)

Owners added in `native_rw_raster.cpp`: `GcnRasterLockPalette` 0x804972B8, `GcnRasterUnlockPalette` 0x80497388, `GcnRasterUnlock` 0x80497104. Unlock converts a writable level's linear pixels to the driver's tiles by calling the game's own converter 0x8049677C (guest_call), takes the lock size back, frees the linear copy through the engine, resets the level byte and calls the engine's raster hook 0x8048E310 when the original did; the DCFlushRange and GXInvalidateTexAll the original made write no guest word and are not made. Error types are refused loudly.
1. **Measure**: planner, shipping tree: 325 platform bodies (c2bf26e: 326); rw-gamecube-driver 166 bodies.
2. **Contract test** (`rw_raster_contract_tests`): original words vs owners - palette lock/unlock 2,520 cases (540 locks, 90 clearing unlocks), Unlock 3,024 cases (432 tile conversions, 1,296 frees, 288 hook calls: every non-stack guest write, r3, callee-saved registers, free and hook calls equal); error types refused; negative controls.
3. **Game behaviour**: Westopolis A/B x2 (`s2base` = c2bf26e roots/owners, `s2cand`): PCM `015445f2...`, 3,645,000,000 ticks, 2,697 VI, 5,360 frames in all four; the 1 MiB at 0x80500000 (SDA, raster/heap statics) identical A/A, B/B, A/B; all memory blocks identical. `--profile`: Unlock served x3; the palette pair never entered (contract test only). The no-yield limit of the seam: Unlock's callees (converter, engine free, hook) were exercised live 3 times without a fault.
4. **Performance**: job CPU base 29.2 / 28.6 s, candidate 30.2 / 29.7 s: spreads do NOT overlap (~3 %); 3 owner calls cannot cost a second, so this is two separately built executables' layout/run noise, not the change - not re-measured further.
5. **Pixel gate**: not run for this slice (3 calls, no draw state); the GXInit fix's pixel gate is above.

## graphics.rw-gamecube-driver, cluster rwR2: slice 3 - Lock, and the pixel layout rows (eleventh task, 2026-11-08)

Owners added: `GcnRasterLock` 0x80496CA4, `GcnRasterTile` 0x8049677C, `GcnRasterUntile` 0x80496A10, `GcnRasterLevelBytes` 0x80496478. Main's coupling rule (a `guest_call` into a platform row counts) sent the three helpers Lock and Unlock used from `guest_call` to native code: the tile conversion (depth 4 = 8x8 tiles, 8 = 8x4, 16 = 4x4, 32 = 4x4 in two 32-byte halves; the original's per-row copies in the same order), its inverse, and the level byte size (rounded to 32). Lock itself: byte sizes of the levels below, the engine's allocator for the lock buffer (function pointer, `guest_call`), pixel pointer, tile -> linear for a read lock, the draw-sync wait of a write lock (`NativeGxSync::set_draw_sync`), in-place locks; error types / depths / in-place sub-rasters refused loudly; the camera-texture DCFlushRange is not made.
Also: the local owner helper is now named `install_hle` (the coupling tool did not see `own(...)`); `tools/platform/runtime_coupling.py` has a **data-word** class: a `{0xOFF, 0x8xxxxxxx}` row of a constant image a native owner writes into guest memory. Rows in use: `native_gx_sync.cpp` kGxDataImage `{0x4C8, 0x80393634}`, `{0x4CC, 0x80393730}` (the GX library's texture / TLUT region callbacks; guest GXLoadTexObj / GXLoadTlut call through them). Not a call; not counted by `--max-sdk-calls`. To be written only when the build still runs guest GXLoad* bodies once axfx-effects' lifted owners are merged (main's instruction).
1. **Measure**: shipping tree 300 platform bodies compiled (frontier 78); rw-gamecube-driver 141 bodies. (The merge of gc-removal 452099e moved the baseline; the tree before this slice's owners is not separately measured.)
2. **Contract tests** (`rw_raster_contract_tests`, original DOL words vs owners): Lock 2,160 cases (1,728 lock buffers, 432 tile conversions, token issue and wait), pixel layout 1,560 cases (tile, untile at 4 depths x 10 sizes x 2 strides, level sizes at 4 depths x 5 private flags x 7 levels x 10 sizes; pixels moved > 2,000 words), Unlock 3,024, palette 2,520; every non-stack guest write, r3, callee-saved registers, allocator / free / hook call words equal; refused cases (error types, depth 24, sub-raster in place, token never reached) stop loudly.
3. **Game behaviour**: Westopolis A/B x2 (`s3base` = deb55b6 raster owners, `s3cand`), at the new default (native async loading): PCM `46a70a10...` (the new canonical), 3,645,000,000 ticks, 2,697 VI, 5,360 frames in all four; all memory blocks and the 1 MiB at 0x80500000 identical in A/A, B/B and both A/B. `--profile`: GcnRasterLock x3, GcnRasterUnlock x3, GcnRasterLevelBytes x355, Destroy x78, SetRaster x22, SubRaster x12,279 served live; Tile / Untile / palette pair never entered (contract tests only).
4. **Performance**: job CPU base 20.6 / 20.0 s, candidate 20.0 / 22.1 s: spreads overlap, no measurable change.
5. **Coupling**: `runtime_coupling.py --require-classified --max-sdk-calls 6`: 0 unclassified, guest-call count unchanged. **Pixel gate**: not run (3 locks; no draw state).

## graphics.rw-gamecube-driver, cluster rwR2: slices 4 and 5 - Create, Format, SetMipLevels, exact errors (twelfth task, 2026-11-08)

Owners: `GcnRasterCreate` 0x80497CA8, `GcnRasterFormat` 0x80497458 (the type / format-code switch: texture format, palette format, palette flag, depth, format byte), `GcnRasterSetMipLevels` 0x80497A70 (its single caller 0x8049A9EC; shares Create's allocation tail: parent level count, level byte sizes through the native `GcnRasterLevelBytes`, pixel block / palette through the engine allocator (function pointer, `guest_call`), the palette object words GXInitTlutObj writes (format, address, count), no DCInvalidateRange).
**Exact errors** (main's correction): every path the original answers with `RwErrorSet` is now exact: `RwErrorSet(&{1, code})` (0x8047EBD4, RW core) through `guest_call`, then the original's answer: codes 0x8000000C depth, 0x8000000D type / format, 0x8000000E lock, 0x80000011 unlock, 0x80000013 memory (size), 2 palette-format; Lock with an invalid depth carries on with a 0-byte row like the original; Tile / Untile report and continue; LevelBytes answers 0. The record lives in a frame the owner holds below r1 (`report_error` lowers r1 before the call so the callee's frame cannot overwrite it: audited - it is the only owner passing a stack address to `guest_call`; the test callbacks now also scribble below their caller's r1). Remaining stops, each because the original cannot continue: (1) a draw-sync token never reached (Destroy / Lock spin forever), (2) a negative raster size in GetMipLevels / the level count (the log2 shift loop never ends).
1. **Measure**: shipping tree 280 platform bodies (285 before slice 5, 300 before slice 4); rw-gamecube-driver 122 bodies, frontier 28. Coupling tool: 0 unclassified, guest-call count unchanged.
2. **Contract tests** (original DOL words vs owners, every non-stack write, r3, callee-saved registers, allocator / free / hook call words, the engine's error slot - SDA 0x6100 offset, preset to "no error" 0x80000000 so a vacuous pass is impossible, and a pending error is kept): Format + Create 7,752 cases (4,152 answer an exactly reported error, 744 allocations, 384 palette rasters), SetMipLevels 3,456, Lock 2,160 + exact errors, Unlock 3,024 + exact errors, palette 2,520 + errors, layout 1,560 + depth 24, Destroy 1,620 + 8 error cases.
3. **Game behaviour**: Westopolis A/B x2 per slice at the default (PCM `46a70a10...`, 3,645,000,000 ticks, 2,697 VI, 5,360 frames; all blocks and the 1 MiB statics identical): slice 4 (`s4base` e294b30 vs 498287f), slice 5 (`s5base` 498287f vs 385612f). `--profile` of slice 5: GcnRasterCreate x370 and GcnRasterSetMipLevels x347 served live (engine heap through guest_call), Lock x3, Unlock x3, Destroy x78, SetRaster x22, SubRaster x12,279.
4. **Performance**: slice 4 base 22.5 / 21.4 s, cand 22.0 / 23.0 s; slice 5 base 20.6 / 20.4 s, cand 20.4 / 21.8 s: spreads overlap, no measurable change.
5. Pixel gate: not run (texture creation is driven by recorded content; identical memory, PCM and statics; no draw state changed).

## graphics.rw-gamecube-driver, cluster rwR3: the raster rectangle fill, exact (thirteenth task, 2026-11-08)

`0x8049A124` is not a texture-dictionary export (the name the earlier survey gave it): it is **GcnRasterClearRect(device, rect, pixel)**, the fill behind the device's two raster entries `0x8049A52C` (the whole current raster, its own `{0,0,w,h}`) and `0x8049A508` (the caller's rectangle); `0x8049A304/364/3B4/41C/480` are its per-depth switch arms. Owners in `native_rw_raster.cpp` replace native-boot's stop owners for the same eight addresses (they agreed; deletions in `native_rw_world.cpp`, `native-owners-r2.txt`, `native_rw_world_reference_tests.cpp`). Raster of the current context `*(r13+0x6210)`: types 0 / 4 / 5 = CPU fill through the core's `RwRasterLock(raster, 0, 9|3)` / `RwRasterUnlock` (only when the raster is not already locked: private flag 4); a full-raster clear with 0 or -1 is one memset of stride x height; otherwise rows per depth (4: byte of two nibbles, 8: byte, 16: halfwords, 32: words); an invalid depth reports `RwErrorSet(0x8000000C)` and unlocks; types 3 / >= 6 report `0x8000000D` and answer 0 (exact). Types 1 / 2 (z-buffer / camera) are the one stop: the original draws a clear quad through GX (`ClearQuad 0x80499E58`, which writes the render-state shadow at `0x805E42D0`, GX setters and a pipe quad); the lifted build only has the camera clear (`0x80495638`, scaffold `RwDriver::camera_clear`: background colour + alpha-test mode) and no recording reaches this raster-level one (0 census hits on three recordings, and this one); a `guest_call` of ClearQuad would keep an SDK drawing body alive with no translated counterpart. The stop names ClearQuad.
The Im3D owners 0x8049B87C / B95C / B9AC / BB48 (the "executor" 0x8049C6C4 is the Im3D transform / render dispatcher) are native-boot's (rwR3 step 8c); their `im3d_error` stops are the same "stop where the original carries on" pattern - left with native-boot / vi.
1. **Measure**: merged tree (gc-removal round 15 + this): 184 platform bodies compiled, frontier 51, rw-gamecube-driver 76 bodies (the baseline of the merge is not separately measured).
2. **Contract test** (original words vs owners, with test doubles for the core lock / unlock on both sides, logged): 6,480 cases (6 types x 5 depths x 2 lock states x 6 rectangles x 4 pixels x 3 entries x 2 half-pixel offsets): every write, r3, error words, lock / unlock call words equal; types 1 / 2 stop naming ClearQuad. `native_rw_world_reference_tests` 1,470 checks pass with the six entries moved from "stop" to "served by native_rw_raster".
3. **Game behaviour**: Westopolis A/B x2 (`s6base` aede12c files vs candidate): PCM `46a70a10...`, 3,645,000,000 ticks, 2,697 VI, 5,360 frames, 0 differing blocks, statics identical in A/A, B/B, A/B; the entries are not reached (0 hits) so the live evidence is "nothing changed".
4. **Performance**: CPU base 20.6 / 21.0 s, candidate 22.1 / 19.6 s: spreads overlap.

## rwR3: the Im3D error paths exact (fourteenth task, 2026-11-08)

`RwIm3DTransform` (count over 0x10000: RwError 0x32, answers 0, nothing written), `RwIm3DRenderPrimitive` / `RwIm3DRenderIndexedPrimitive` (no transform in progress: RwError 0x23, answers 0; a primitive type with no pipeline: RwError 0x25, then the original carries on: the record's pipeline stays NULL and the executor 0x8049C6C4 is called with it - the native executor of the lifted build, `RwDriver::im3d_render`, answers 0 for a NULL pipeline, documented there) no longer stop the run: the owner reports through `rw_raster::report_rw_error` (RwErrorSet 0x8047EBD4 over `{1, code}` through the seam) and continues as the original does. `native_rw_world_reference_tests` compares the RwErrorSet records (and the existing r3 / executor-call / memory comparison) for every error case: 1,806 checks pass. Not game-visible (no recording raises these).
**Raster types 1 / 2 of the rect fill (ClearQuad 0x80499E58), analysis for the next step.** ClearQuad = colour from `*r5` (flag 1) or 0xFF000000 (flag 2 only) -> GXSetZMode-like `0x80399DF0(1, 7, 1/0)`, `0x80397B98(4, &colour)`, `0x80399D44(1,1,0,0)`, `0x80396B54(0)`, `0x804987C8(1)` (alpha-test mode setter, native-boot's exact owner), the fog toggle `0x804986B8` (state struct at `0x805E42D0`: +0x10 fog-on, +0x14 mode, +0x1C colour, +0x24..+0x30 near/far), `0x80499B14(device)`, `0x80396934(0x80,0,4)`, then four vertices (x, y, z=0) through the GX pipe (`0xCC008000`), restoring the fog/blend state (`0x80498910`, `0x80399DF0`, `0x80396B54`). In the lifted build the GX setters are native owners (exact `__GXData` shadow words), the pipe stores are dead sites, and the picture is the scaffold's passes: a raster-level clear needs a renderer entry (a clear of the current pass target) that does not exist, and no recording reaches the path (0 census hits). Left as the one stop, naming ClearQuad, until the renderer has that entry.

## Survey (read-only): the live-hit frontier rows and the spec of GcnSystem 0x80494EDC (fifteenth task, 2026-11-08)

Rows of `test_data/platform/gx_rw_frontier.csv` with census hits and no owner in any owners file: GcnSystem 0x80494EDC (11,135 hits), GX/ShowRaster 0x80495CBC (5,360), GcnNativeTextureRead 0x8049A8DC (348); all claimed (vi: GcnSystem, ShowRaster; native-boot: NativeTexture*). GcnSystem is not 60 bytes: it is the RwDevice system function `GcnSystem(request r3, out r4, in r5, in r6)` 0x80494EDC..0x80495634 (~1.8 KB), a 23-way switch (table 0x8056F494; request > 0x16 answers 0):
* 0 `0x80495124` DeviceOpen: `r30 = *r5`; with a parameter block {a, b, fifo size}: `0x80494CD4(a, b)` (display parameters) and SDA `0x2ADC` = the FIFO size, else `0x80494CD4(0, [0x618C])`; allocates `(((w + 15) & ~15) * h) * 4 + FIFO size + 0x1F` bytes through the engine (`*(r13+0x615C)+0x108`, flags 0x40411) into SDA `0x619C`, `0x61E8` = the 32-aligned address, DCInvalidateRange over it (`0x80372508`), SDA `0x61AC/0x61A0` = the base, `0x61A4/0x61A8` = base + 2*pixels (the two XFB-sized halves), returns 1.
* 1 `0x804951D8` DeviceClose: `0x80396098`, the engine's free of `[0x619C]`, SDA `0x61AC/0x61A0/0x61A4/0x61A8/0x61E8/0x619C` = 0, returns 1.
* 2 `0x80495214` DeviceStart: first start: GXInit(`[0x61E8]`, `[0x2ADC]`) -> SDA `0x61EC`, `[0x61D4]` = 1; later starts re-bind the FIFO objects (`0x80394AF4/BDC/D04` over a stack object and over `[0x61EC]`); then waits for the draw-sync token (`0x80396428` / `0x803964DC` loop on SDA `0x6220`), sets up the VI/GX with the display raster `[0x61F4]` (`0x8037F19C`, `0x8037FEDC/FD38`, the video format, `0x8039A444/49C` projection-ish, viewport/scissor `0x8039A4E4/55C`, `0x80396BD4/CCC`, copy settings `0x80397228`, `0x803970E4` GXSetDispCopyYScale when the raster is the video height, SDA `0x6198`, `0x61F0`, `0x2AD8`, `0x2AE0`), the retrace callback registration (`0x8037E4F0/E534`), breakpoint callback `0x80394F3C`, `0x8037847C(&SDA 0x61D8)`; returns 1.
* 3 `0x804954CC` WaitIdle: spins while SDA `0x61C4 == 1` and `[0x61B8] != [0x61BC]`; returns 1.
* 4 `0x804950A0` GetDeviceInfo: copies the 0x38-byte record of `0x804960F0` into `*r4` and stores `r5` into SDA `0x61F8 + 4`; returns 1.
* 5 `0x80494F34` / 8 `0x80495098` / 9 `0x804954F0` / 10 `0x80495088`: `*r4 = 1` / answer 1 / answer 1 / `*r4 = 0`; 7 `0x80494F18`: a boolean of `r6` (`((1 ^ r6) >> 1) - ((1 ^ r6) & 1)` as a sign bit); 6 `0x80494F44`: for `r6 < 1`, by SDA `[0x6194]` mode (0, 1, 2, 3, 5, 0x2A): the 24-byte table row at `0x8056F408 + {0, 0x18, 0x30, 0x48}` copied to `*r4`, answers 1 (r6 >= 1: 0).
* 11 `0x804954F8` GetFormatTable: 27 (index, value) pairs of `0x8051D400-0x20`-ish table copied to the stack, then `*r4[i]` = `0x80494CCC` default for every slot below `r6` and the pairs stored by index; answers 1.
* 17 `0x804954B4` -> `0x804981E8()`; 18 `0x804954C0` -> `0x8049847C()`; both answer 1. 19 `0x804955FC`: `*r4 = 0x400`; 22 `0x8049560C`: `*(u16*)r4 = 3`; 12-16, 20, 21 and everything above 0x16: answer 0.
This is the spec for whoever owns it; nothing is written for it here.

## rwR3: the raster-level clear, GcnNativeTextureRead, and what went wrong with it (sixteenth task, 2026-11-09)

**Owners** (`native_rw_raster.cpp`): `ClearQuad 0x80499E58` and `RwRasterClearRect` on z-buffer / camera rasters now hand the clear to the renderer: `RwDriver::raster_clear` (a canvas quad at z 0.99999994 through the Im2D draw: colour replaced with blend ONE / ZERO, depth-only with ZERO / ONE and depth write, test off, the rectangle in raster pixels, a full-view rectangle stretched, alpha-test mode 1 left as the original leaves it), reached through `rw_raster::set_raster_clear` (set by `LiftedRender::install_owners`); without a renderer the owner stops naming ClearQuad. `GcnNativeTextureRead 0x8049A8DC` (lifted build): the original's reads step by step (RwStreamFindChunk, the 0x58-byte platform block, the 16-byte raster header, RwRasterCreate, `GcnRasterSetMipLevels`, palette and texels read into the raster's blocks, RwTextureCreate, filter word, **RwTextureSetName(texture, frame + 0x3C)**, **RwTextureSetMaskName(texture, frame + 0x5C)**, the LOD step through `TextureLoadedFn`); the lowered LOD site 0x8049AB50 left `lift-sites-r2.txt`.
**The mistake.** I committed the texture-read owner with the two name calls wrong (the first called with no arguments and labelled "hook", the second given the mask name as the name): natively read textures had garbage names, `RwTexDictionaryFindNamedTexture` answered 0 and the engine start faulted at 0.02 s (main found it in round 18 and fixed it in a19fe6c). My gates for that commit were the contract tests (their doubles counted calls and ignored r3 / r4) and a compile; I reported before a boot, a Westopolis run or a profile - the owner is on a live path (347 hits) and needed all three first. What is different now: the doubles log the texture and the 32 bytes of every name argument (and the other doubles' arguments: stream of FindChunk / StreamRead, raster of TextureCreate, pixel and format of RwRGBAFromPixel, the unlock hook's second argument), and three mutations of the original words (name swapped for mask name, mask name swapped for name, mask name call given the stream) must be detected by the same comparison. Line endings: seven files of mine had been committed with CRLF by my edit helper (the helper preserved the working copy's endings); they are LF again.
**Gates on the merged HEAD** (code identical to gc-removal a19fe6c; only tests differ): contract tests - ClearRect + wrappers 6,480, texture read 2,576 (144 complete reads, names compared) + 3 mutations; WARP pixel test (full colour clear uniform, rectangle clear exact outside / 48,000 inside pixels, depth-only keeps the colour, no-op clears change nothing, colour negative control); boot + default-config Westopolis x2: PCM `46a70a10...`, 3,645,000,000 ticks, 2,697 VI, 5,360 frames, the 1 MiB statics identical A/A; `--profile`: GcnNativeTextureRead x347, Create x370, Destroy x78, SubRaster x12,279, no fault; merged-tree plan: 60 platform bodies compiled, frontier 25, rw-gamecube-driver 11 bodies. PNG captures (headless, `SHADOW_R2_MSAA=1`) at 5e6 / 2e8 / 9e8 / 2e9 / 3.5e9: `22fdf629` / `7bfd6854` / `c44bde60` / `d471361d` / `0b42b725`; main's listed baselines are `22fdf629` / `d22a1750` / `b6511d3d` / `37299f60` / `ae967db4`: the first matches, the others differ - the code is the same as the baseline build's, so the capture settings of my run (window size, scale) are the difference, not checked further. Not tested: the depth effect of a depth clear (no draw after it in the harness reads the depth), and a texture read of a yielding stream beyond what the 347 live reads exercise.

## rwR4: the native-data stream callbacks and the vertex-format decoders (seventeenth task, 2026-11-09)

**Baselines first.** The PNG differences reported earlier came from `SHADOW_R2_MSAA=1` in my capture script. Rebuilt merged HEAD (gc-removal 3d85e87) and captured with no MSAA env: ticks 5e6 / 2e8 / 9e8 / 2e9 / 3.5e9 = `22fdf629`, `d22a1750`, `b6511d3d`, `37299f60`, `ae967db4`, byte-hash equal to `wt-gc-removal/.build/itest/baseline/ipx_merged_*/cap.png`. The same five again with this task's code.

**What was there.** vi's rwR4 stop owners (native_rw_world.cpp) covered 18 entries with 0 census hits. Five of them are the stream callbacks of the native geometry / world-sector plugins - size, write and read (`0x8046615C`, `0x80466188`, `0x804661D0`, `0x804661FC`, `0x80466250`) - and four are pure vertex-format decoders (`0x8046798C`, `0x804679F0`, `0x80467A3C`, `0x80467AA4`). A stop is wrong where the original can carry on (the standing rule), and these can: they are now **exact owners**.

**Owners.**
* `native_rw_registry.cpp`: `0x80465DFC` (the chunk writer both write callbacks call: chunk header type 1 / version 0x37002 / library 0x37, then three `RwStreamWrite` words - the tag 6, the first block size, the second block size - then the first block with its pointer entries un-relocated by the second block's 32-byte-aligned address, relocated back, then the second block) is `geometry_chunk_write`; the callbacks are the original's wrappers (flag `[r+8] & 0x01000000`, the data pointer, the count halfword). A stream write that fails answers 0 and leaves the object in the state the original leaves it in (tables stay un-relocated if the first block's write fails). The read is the existing world-sector body (`world_sector_read`, now taking the row it links from): `0x804661D0` is the same `0x80465AE4` under a different one-call wrapper. The object lookup `0x80460EF0` the world-sector callbacks start with is entered by `guest_call`. DCFlushRange and GXInvalidateVtxCache are dropped as in the existing reader (silent owners).
* `native_rw_world_decode.cpp`: the four decoders, pure.

**Contract tests** (original DOL words against the owners; new in CMake): `rw_world_stream_contract_tests` - 508 cases: every table shape (0/1/2/3/6 entries x 0/1/5/8/9/17 pointers), each of the six stream writes failing in turn, the flag clear, header refused, size cases (flag x data pointer), and the geometry reader's own cases (no chunk, 7 versions, each read failing, each block short, 48 table shapes). The doubles log every argument (registers, the word a pointer argument points at, a hash of the bytes a byte-range argument covers at the time of the call) and scribble below r1 as a callee's frame; the object's second-block pointer is a mapped address (an earlier version of the test read wild addresses: 95,674 unmapped accesses, found by the flat-map counter and fixed - now 0). Negative controls: a writer that writes nothing, a size off by one, and three patched ORIGINAL words (the chunk type, the library, the second block's length) are all told apart. `rw_world_decode_tests` - 9,424 cases: exhaustive over each decoder's selector and multiplier fields, every index byte, 400 wide-index cases, plus negative controls and a patched original word. `native_rw_world_reference_tests` (stop count 9, the nine owners census 0), `rw_world_contract_tests`, `rw_registry_contract_tests`, `native_file_api_tests`, `platform_runtime_coupling` (the three row constants had to take the `constexpr E = row + 0u` idiom the tool classifies) and `platform_gc_removal_plan` pass.

**Not tested / limits.** The lookup `0x80460EF0` is a double in the contract test (the real one walks the engine's object list, set up in no test); the size callback for a world sector and the write callbacks are therefore tested for their own logic, not for the lookup. No recording enters any of the nine owners (profile: 0 hits), so there is no live exercise.

**Gates.** Westopolis x2 (default config, exe built from this task's source): PCM `46a70a1051347069`, 3,645,000,000 ticks, 2,697 VI, 5,360 render frames, statics identical between the two runs; profile: `0x804662B0` x34 (the reader that was changed to take its row), no fault; the nine new owners 0 hits. Plan on the merged tree: 41 platform bodies compiled, frontier 21, rw-gamecube-driver 10 bodies (the base before this task was not separately measured on this tree). The final text differs from the gated exe only by the three `constexpr E = row + 0u` locals (compile-time identical), re-tested by the contract tests above.

**The nine that stay stops** (`0x8046521C 0x80465438 0x80465864 0x80466624 0x80466D2C 0x80466F24 0x804671E4 0x80467BA8 0x80468C18`): the pipeline node bodies and instance helpers of the world-sector pipeline, 126-1,052 instructions each, six of them calling GX state (`0x80397xxx`, `0x80399xxx`). They are entered only through `RxPipelineExecute`, which is lowered (axfx-effects' closure proof), so nothing reaches them. Not ported: a port of GX-emitting code would be hardware logic with no consumer.
