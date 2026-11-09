# Removing the GameCube SDK from the build

Status 2026-11-02. Measured, not estimated: every number here comes from the generator's own liveness pass
(`tools/ppcrecomp` `--live-graph`) classified by the platform manifest (`tools/dolrecon/platform_deps.py`,
`.build/dolrecon/pal-v30/platform_manifest.json`). Regenerate with the commands at the end. Nothing here is removed
yet: this is the plan, its tooling and its gates.

## Mechanism: ownership + static pruning

The generator emits a guest function only if it can execute: it is reachable from the DOL entry, a data word, or a
runtime liveness root (`extract/liveness-roots.txt`), and a native owner's original body reaches nothing. So a
GameCube service is removed when **every entry into it from outside the SDK** is served natively behind the runtime
contract (`shadow/runtime/v1`): the interior becomes unreachable and the generator stops emitting it. Removal is then
sound by construction (the generator proves nothing can enter the body), not "we never saw it run". Most of the DVD
library went this way (22 owners; 62 of its 92 bodies pruned, 8 still compiled).

A removed body is never replaced by a silent stub: an owner either serves the call completely (and is tested against
the original in the reference interpreter) or the call is listed as a work item.

## Where the SDK stands

Rows of the generator (split interiors count as rows): 1,057 platform-SDK bodies are compiled; 292 of them are
**frontier** rows, entered from outside the SDK. Owning all 292 leaves **0** SDK bodies (computed, by the generator's
reachability). What enters the frontier (an entry can have several):

| entered by | meaning | who must act |
|---|---|---|
| game code | the game calls the SDK API | a contract service owns the API (as DVD) |
| RenderWare | RW's GameCube driver / engine calls GX, OS | the native RW (rwcore) + renderer replace the driver |
| CRI middleware | ADX / Sofdec's GameCube layer calls OS / AX / DVD | owners of those APIs, or CRI's platform layer lifted |
| data word | a function pointer in the image (callback tables, vectors) | the table's consumer is owned, then the word is retired |
| runtime literal | **our own runtime** names the address (a hook, observer, continuation) | runtime work only |
| compiler runtime / DOL entry | `__start` and the C runtime's start-up | a native boot |

Per service (frontier rows; bodies freed when owned on top of the services before it, greedy by bodies freed per
frontier row):

| step | service | frontier rows | SDK bodies freed | left |
|---:|---|---:|---:|---:|
| 1 | os.core (OSInit and its interrupt / scheduler entries) | 3 | 36 | 1,021 |
| 2 | audio.ax (ReverbHiInit / ReverbHiShutdown) | 2 | 11 | 1,010 |
| 3 | graphics.rw-gamecube-driver | 96 | 441 | 569 |
| 4 | graphics.gx | 64 | 255 | 314 |
| 5 | saves.card | 6 | 21 | 293 |
| 6 | debug.trk (MetroTRK) | 5 | 14 | 279 |
| 7 | sdk:unidentified (identify first) | 61 | 168 | 111 |
| 8 | os.other | 20 | 53 | 58 |
| 9 | os.interrupts | 4 | 10 | 48 |
| 10 | graphics.vi | 6 | 11 | 37 |
| 11 | os.threads | 17 | 29 | 8 |
| 12-16 | os.memory, os.time, os.debug, cpu.config, cpu.cache | 1-3 each | 8 | 0 |

The order is a measurement of leverage, not a schedule: services reach each other's interiors (owning os.threads alone
frees 7 of its 26 bodies), so the totals only add up in combination. The per-row work list (every frontier row, every
caller with its address, every runtime file naming it) is generated: `.build/platform/removal_plan/REMOVAL_PLAN.md`.

Not in this count, and not SDK: the CPU-level dependencies of the game's own code (paired-single math in 11,813 game
functions, byte-reversed access, FP estimates). Those go with the C++ lift replacing recompiled execution
(docs/DOLRECON.md), not with service ownership.

## Defects found by the planner (fixed 2026-11-02)

- `native_liveness_roots` was failing: the root list was stale by 171,881 addresses. 171,849 came from the generated
  lift (`runtime/tests/generated/dolrecon_lifted_*`), whose literals were counted as shipping roots although its test
  links only the reference interpreter, never the recompiled shards. Regenerating the list as the test demanded
  would have rooted almost every guest function and pinned the whole SDK. `tools/dolrecon/lift_cpp.py` now marks
  its output `liveness-roots: reference-interpreter-only` (verified by liveness_roots.py: tests/ only, no
  generated-code include); the regenerated lift is byte-identical to the checked-in one apart from that line
  (regenerated from pal-v3). The remaining 32 addresses (native_world_collision.cpp, native_world_line.cpp,
  system.cpp, rwcore tests) were added; none was pruned, the generator's pruned set is unchanged (711 rows).
- Test sources' literals root shipping code (one root list for both trees): dropping all 367 test-only roots frees
  one row (a GX row named only by gekko_core_tests.cpp). Recorded, not split.

## Gates for every removal step

A step that owns an API legitimately stops writing the SDK's private state, so "every guest memory block identical"
(tools/platform/ab_replay.py between backends) is the gate for a backend change, not for an ownership step. An
ownership step must pass all of:

1. **Contract test against the original**: the owner and the original body, both on the same inputs, in the reference
   interpreter (as native_file_api_reference_tests); every API output (return value, out-parameters, memory the API
   documents as written) equal.
2. **No surviving reader of the retired state**: the bytes the removed bodies wrote that the owner does not write are
   read by no surviving code during the Westopolis replay. Needs a guest load watch on those ranges (not built yet:
   the first work item of step 2).
3. **Game behaviour**: the Westopolis replay (`--boot-stage stg0100` + its input tape) reaches the same game tick
   with the same game-visible state; any difference outside the retired ranges is a failure.
4. **The ratchet**: `platform_gc_removal_plan` with `SHADOW_GC_REMOVAL_RATCHET` lowered to the new count; the
   generator refuses any listed site the step makes stale (the planner lists them first).

## Milestone 2026-11-02 (worktree wt-gc-removal): runtime coupling and gate 2's static half

Implemented and tested; **no SDK body removed** (no generator count changed: shipping 1,057 / tests tree 1,063
compiled platform-SDK bodies, before and after).

- `tools/platform/runtime_coupling.py`: every frontier row the runtime's own sources enter (91 in the tests tree,
  52 entered by nothing else), each use followed through its named constant and labelled by the runtime API:
  observer 41 rows, lr-identity 11, lowered-site 11, probe 9, comparison 6, trace-or-message 4, address-arith 3,
  owner-hook 2, **guest-call 3 / dispatch-to-sdk 3 rows = 11 native -> SDK call sites**, unclassified 0. The 11:
  native_interrupts.cpp 490 / 503 (SelectThread 0x803787B0, OSLoadContext 0x80372C68: the guest scheduler),
  native_rtgcnfs.cpp 147 (0x80378A08 yield) and 218 / 219 / 222 / 224 / 282 (OSReport 0x803731CC),
  native_gx_sync.cpp 442 and native_video.cpp 380 (OSWakeupThread 0x803793B0), native_card.cpp 1077
  (__CARDVerify 0x80390680; dropping that call alone frees that one body, measured).
- `tools/platform/retired_readers.py` (gate 2, static half): the retired rows' stores and every surviving row's
  loads at addresses the must-analysis proves (absolute, r2 / r13 small data), plus runtime literals inside them;
  accesses through unproven pointers are counted per row, never excluded. Controls (until OSInit became a native owner, 2026-11-07, owning it retired 38
  fixed-address locations read by 42 surviving loads in 26 rows (__OSArenaHi by OSGetArenaHi 0x80372214 - checked
  against the original code -, OSExceptionTable, the low-memory OS context words 0x800000D8 / 0x800000DC) and 3
  runtime files; now owning OSSetCurrentContext 0x80372B80 is the readers control: 2 loads read 0x800000D4) ; owning ReverbHiInit / Shutdown retires no fixed-address state but 119 pointer stores in 11 rows,
  which only the dynamic half can close.
- CTests (tests tree): platform_gc_removal_plan (ratchet now per configuration: 1,057 lifted, 1,063 not lifted -
  the 2026-11-02 value 1,057 failed the tests tree, which compiles 6 more RenderWare-driver bodies),
  platform_runtime_coupling (all classified, at most 11 native -> SDK sites), platform_retired_readers_context
  (readers must be found), platform_retired_readers_reverb (no proven reader, unproven stores reported). All pass;
  negative controls (ratchet 10, inverted expectations) fail as they must.

**Gate 2's dynamic half is not built** (the precise blocker): translated code does not record which guest row is
running (c.pc is set only at transfers), so a watch cannot attribute an access to retired or surviving code. Needed:
a ppcrecomp diagnostic mode that stores the current row at each entry and after each call returns (emitted only in
a diagnostic build tree), the GuestMemory access trace (`set_access_trace`, already used by --trace-stale-contacts)
marking bytes the retired rows write, clearing them on surviving writes and recording surviving reads with the row,
and a replay (Westopolis, then a save / load scenario for CARD). LR-based attribution (what --trace-stale-contacts
uses) is not exact and must not be used for this gate.

## Milestone 2026-11-02 b (worktree): RtGCNFS's asynchronous branch retired, call-graph defects fixed

RESULT (tests tree rebuilt, CTest 8/8: native_rtgcnfs_tests 35 calls / 0 failures incl. the 3 refusals,
platform_rtfs_async_proof, native_liveness_roots, platform_gc_removal_plan, platform_runtime_coupling,
platform_retired_readers_context / _reverb, native_threads_tests): native -> SDK sites **11 -> 6 measured**
(runtime_coupling.py; ratchet SHADOW_RUNTIME_SDK_CALLS_RATCHET lowered to 6, a value of 5 fails as it must).
Compiled platform-SDK bodies unchanged: 1,063 (tests tree) - OSReport stays alive through game callers.
extract/liveness-roots.txt: 7 roots dropped (0x803731CC, 0x8047AA18, the five OSReport format strings), exactly the
runtime literals removed. NOT run: a Westopolis A/B replay of this step (the changed paths are unreachable per the
proof; the replay is still owed before the step is called verified in-game).
Also fixed on the way (the snapshot carried it from source/): shadow_main.cpp included render/present_trace.h and
gekko/phase_lock.h only under SHADOW_RENDER_LIFTED while the pacer uses both in every configuration, so every
non-lifted build (the tests tree) failed to compile. Both headers are header-only; the includes moved out of the
guard. source/ has the same defect.

- **5 of the 11 native -> SDK call sites retired** (all OSReport 0x803731CC, native_rtgcnfs.cpp). They sat in the
  owner's asynchronous branches. `tools/platform/rtfs_async_proof.py` (CTest platform_rtfs_async_proof) proves from
  the DOL's bytes that no DIRECT path opens a file asynchronously: the 8 references to _rtFSManagerFOpenOnFS
  0x8047AE1C / RtFSManagerFOpen 0x8047AFA0 / RtFileSystemDefaultToAsync 0x8047AA08 are the only ones (branches, words
  in any section, lis + addi / ori pairs), each mode argument is traced (fExists li r5,1; RtFSManagerFOpen's caller
  0x8004547C li r4,1; RwFopen 0x8047B1C8 builds r31 only from li 0 / ori 1,2,4,8; DefaultToAsync gets 0 or a value
  only IsDefaultedToAsync produced - dominance in the enclosing function). Register writes are decoded per form,
  unknown forms count as writes; straight-line walks stop at joins. --self-test: 7 mutations, each refused.
  NOT proven statically: 105 indirect calls through some object's +0x28 slot (the open owner is the file system's
  slot +0x28) - a byte-level check cannot type their receivers. On every path the owner refuses an asynchronous
  open / read / sync by stopping the run (`refuse_async`), so a gap stops loudly, never serves silently.
  The contract test native_rtgcnfs_tests no longer compares the five asynchronous calls with the original; it checks
  that a fresh owner refuses each (open 0x11, read / sync of a record with isAsync = 1). This narrows the contract
  to what the game can reach - a decision for the user to confirm.
  OSReport's body is not freed (game code and three test files still name it). Ratchet to lower after the run:
  SHADOW_RUNTIME_SDK_CALLS_RATCHET 11 -> expected 6 (to be measured by runtime_coupling.py, not assumed).
- **Call-graph defects** (reported by the recon side session, confirmed and counted): tools/dolrecon/callgraph.py read
  only `*_text.s` / `*_init.s` (93 dtk files skipped: duplicate units `*_text_N.s`, `__init_cpp_exceptions.s`),
  did not map dtk's `name_ADDR` spelling of scope:local functions, and did not resolve calls to labels
  (`_savegpr_N` / `_restgpr_N` at and inside __save_gpr / __restore_gpr). Fixed (TEXT_FILE, load_aliases; labels
  resolve only for branches - other label uses are address markers such as __OSDBINTSTART). On pal-v30: +993 call
  edges (462 files, 7 local names, 20 labels at a function start, 504 labels inside one), +103 data, +15 vtable,
  +13 addr. Same file filter fixed in platform_deps.py (service call sites, + name_ADDR) and lift_cpp.py (asm
  comments only - the lift itself was complete). tools/test_dolrecon_callgraph.py: 3 new tests, 2 of which fail on
  the old code. NOT regenerated: source/.build/dolrecon/pal-v30/callgraph.json, platform_manifest.json and their
  consumers (receivers.py, platform_deps.py -> what_if / removal_plan); regenerate deliberately, with the vcalls file.
- **dolrecon_lift_tests timeout**: the "[fm] ... refusing to guess" lines are not failures - the reference core's
  fault handler passes a host fault on and the harness's SEH (dolrecon_lift_tests.cpp:342) ends that case as
  invalid input. The workload grew from 31,572 to 34,045 lifted functions (FP / paired-single tiers); the 16-process
  DOL pass took 2,712 s on 2026-10-31 against the test's 1,800 s cap. Uncapped measurement of the exact CTest command:
  see the result line added below when it finishes.

Other findings from the read-only review agents (recorded, not acted on yet):
- Scheduler (for retiring the last 4 scheduler sites): only SDK code touches OSThread fields 0x2C8-0x304; the game
  enters the scheduler through 15 SDK thread functions (Sonicteam::System::Thread 0x8035A..., semaphore / queue
  wrappers 0x8032D...). A native SelectThread must keep the RunQueue (0x80587038, 32 x 8 B) / Bits / Hint layout,
  the "current context != current thread -> return 0" rule (the idle loop depends on it) and replace the idle spin
  0x803788F8 with a host wait. Risk noted: Reschedule (sda+0x5AE8) is incremented / decremented through r13 read at
  two times, with no underflow guard (native_interrupts.cpp:421 / 482).
- __CARDVerify 0x80390680 (native_card.cpp:1077): full native spec (VerifyID with the SRAM flash-id serial LCG,
  VerifyDir / VerifyFAT with checksums, freeBlocks count, newest-copy selection and the 8 KiB work-area copy) and a
  contract-test plan exist in the agent report; no test reaches Completion::Mount steps 0 / 1 today. Also:
  active_card_filesystem (native_card.cpp:179-213) accepts cards the guest verify later rejects (-6).
- receivers.py drops struct-return virtual calls (receiver in r4) and secondary-subobject vtables silently (not counted
  in `rejected`), which is why U01 / U02 / U03 stay unresolved; the live graph never reads receivers.json. Generator
  row anomaly: fn_801645D0 is 344 B in live_graph.txt but 0x1C8 in symbols.txt, with a bogus row at 0x80164728 (a
  bctrl return point) - not traced yet. Proposed names for 40 of the 61 unidentified SDK frontier rows (CARD by
  Dolphin signature; AX / MIX from the archive's ENTRY_MAP) - to be verified before use.

## Step 2 done 2026-11-07: audio.ax ReverbHiInit / ReverbHiShutdown owned

Owners: `runtime/src/gekko/native_reverb_owner.cpp` (`serve_init`, `serve_shutdown`), installed in
`System::install_reverb_owner`; rows `0x803884E4 AXFXReverbHiInit`, `0x80388554 AXFXReverbHiShutdown` in
extract/native-owners.txt; the six progress points inside `__ReverbHiCreate` removed from extract/wait-points.txt.
The init writes what the game can observe (bypass byte context + 0x1C4 = 0, r3) and allocates nothing; the 21
allocations (18 delay lines, 3 pre-delays, about 85 KB of the GCAX sound heap) and the descriptors at context +
0x000..0x1C3 are retired.

| gate | evidence |
|---|---|
| what it frees | what_if own-reverb (`--release-dir .build/watch --dolrecon-dir ../source/.build/dolrecon`): 9 rows, 0 revived, no refusal; the real build prunes 751 rows (what_if: 742 before, 751 after) with 209 owner rows |
| 1 contract | axfx_reverb_contract_tests `owned_init_shutdown`: original ReverbHiInit/Shutdown from the pinned DOL vs the owners on the same object, 61 parameter sets (every bound, one ulp outside, signed zeros, infinities, NaN): r3 and the bypass byte equal, no other byte written. NaN: the original's ordered compares accept it and then size a heap request from it, so its result is the game heap's; the owner refuses (r3 0, unbound; the callback then stops the run), and the test proves both callers discard r3 (`lwz r3, 0x5FAC(r13)` at 0x8043F9E4 / 0x8043F6D0) |
| 2 no surviving reader | guest watch (new `--watch-guest-from-tick`, headless, cold-boot Westopolis tape) over context + 0..0x1C4 and the delay lines 0x80842F20..0x80857F60, armed at tick 22,882,500 (just after init) and at 40,500,000: no access except the sound heap's alloc / free (0x8043BDE4 / 0x8043BD38) walking its block headers. The RwStreamRead / native memcpy traffic a from-boot watch shows in those addresses all precedes the heap's carve-up (earlier reuse of the memory, none after) |
| 3 game behaviour | Westopolis 90 s, baseline vs owner alternating 2x each: PCM 015445f2... in all four, 3,645,000,000 ticks, 2,697 VI / 5,360 render frames; A/A and B/B memory identical. A/B: 14 64 KB blocks differ, every word classified - retired descriptors; the sound heap's layout (later sound allocations move into the freed 85 KB) and the pointers to it (GCAX effect objects, dead stack); 13 words 32000.0 vs 96000.0 = the stale ps1 half of a paired-single register (the original's Create loads the AX rate 32000 with lfs, which fills both halves) carried by OSSaveFPUContext slots, dead stack spills and RwMatrix pad3 copies (frame modelling -> LTM, 0x8048C258 / 0x8048C628 copy it pad to pad). Not replicated (no garbage replication) |
| 4 ratchet | platform bodies compiled: shipping (.build/watch) 1,044, tests tree 1,050 (SHADOW_GC_REMOVAL_RATCHET defaults lowered from 1,057 / 1,063) |

Seen on the way, not part of this step: the GCAX start-up also initializes three more SDK effects (0x80388FF8,
0x80389464, 0x80389FF4) whose lines are allocated from the same heap (41 boot allocations in all) and never
registered as aux callbacks - the same kind of step; native_file_api_tests' owner list misses the CRI owners
(66 rows, now printed by the test).

## Next steps

1. **Runtime-held coupling.** Our own code is the sole outside entry of 53 frontier rows (rw_capture.cpp 22,
   native_gx_sync.cpp 7, measure_probes.cpp 7, native_video.cpp 4, os_exception_entry.cpp 3, native_card.cpp 2,
   game_memory.cpp 2, native_interrupts.cpp 2, system.cpp 1, gameplay_scheduler.cpp 1, tests only 2). Each is either a native owner
   still calling into SDK internals, an observer, or a continuation; classify each (the planner's work list) and move
   it behind the contract. These rows are also entered from inside the SDK, so this step frees little alone; it
   removes our dependence on SDK internals, which every later step needs.
2. **audio.ax: ReverbHiInit 0x803884E4 / ReverbHiShutdown 0x80388554** (callers: the game's sound start-up
   0x8043F9A0 and 0x8043F674). The callback is already native; init / shutdown are marked SCAFFOLDING in
   native_reverb_owner.h. Owning both frees 9 bodies (the init internals, four delay-line loops) and makes six
   wait points stale (extract/wait-points.txt lines 264-269). Blocker: the original init allocates the delay lines
   through the game's allocator; the owner must decide those allocations from the contract (gate 2 proves no
   survivor reads them).
3. **saves.card through a Saves contract service** (6 frontier rows, entered by the game's save code
   0x8029A7D8..0x8029B494 and 0x801E258C). Backends: the GameCube card image (NativeCard block device) and host save
   files. Needs a replay that saves and loads (boot-stage replays do neither).
4. **Native boot** (os.core, debug.trk, cpu.config): `__start` 0x80003154 -> DBInit 0x80370BF0 / InitMetroTRK
   0x803A0000 -> OSInit 0x80370E68 -> constructors 0x803796CC -> main 0x800510C0. Owning `__start`, with the native
   boot entering the constructors and main, frees 11 rows (measured, what_if native-boot): OSInit is also entered
   from the game (0x80051028) and the runtime.
5. Then the OS services, VI, GX + the RW GameCube driver (the RenderWare reconstruction programme,
   docs/RENDERWARE_RECONSTRUCTION.md), and the 61 unidentified SDK frontier rows (named from evidence first).

## Commands

    # the work list and the coupling gate (about 2 minutes; ppcrecomp from the release tree)
    python -B tools/platform/removal_plan.py --control-own 0x80372B80 --max-platform-bodies 1057
    # what a step would free, by a real generator run on scratch copies (extract/ is never written)
    python -B tools/platform/what_if.py --name reverb --own 0x803884E4 --own 0x80388554
    python -B tools/platform/what_if.py --name no-observer --drop-roots-from runtime/src/gekko/measure_probes.cpp
    # the runtime's own SDK coupling, and gate 2's static half for a step
    python -B tools/platform/runtime_coupling.py --plan .build/platform/removal_plan/removal_plan.json --require-classified
    python -B tools/platform/retired_readers.py --own 0x803884E4,0x80388554 --release-dir .build/tests \
        --graph .build/platform/removal_plan/live_graph.txt --out .build/platform/retired_readers/reverb
    # the CTests (tests tree; SHADOW_DOLRECON_DIR points at the tools/dolrecon outputs)
    ctest -C Release -R "^platform_"

Outputs: `.build/platform/removal_plan/{removal_plan.json,REMOVAL_PLAN.md,live_graph.txt}`,
`.build/platform/what_if/<name>/what_if.json` (freed rows by service, listed sites made stale, the generator's
refusal if any).
