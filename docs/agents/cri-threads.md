# Agent report: cri-threads (2026-11-07, branch agent/cri-threads, cut from gc-removal 857399c)

Scope (docs/AGENT_COORDINATION.md row `cri-threads`): NEXT_SESSION 2b - the CRI thread bridge's vsync upcall becomes plain
native calls at the retrace point - the bridge's acceptance gates, the `platform_runtime_coupling` guard uses, then ADX exec.

**Result: 2b DONE and gated (below). The bridge stays OFF by default - flipping it is the main session's decision (one
line, `NativeCri::install`, `enabled_ = env != nullptr && env[0] == '1'`). ADX exec (step 3) is NOT started** (section 7).

## 1. What was analysed

### Why the upcall can be a plain call
The upcall existed because the vsync server's ADX output waited at 0x803CF0D4 for an ARQ transfer's DSP DMA completion
(an interrupt; none can arrive in the retrace delivery, MSR[EE] clear). Since the native ADX output
(native_adx_output.cpp, docs/CRI_SERVERS.md 4c) each transfer is a synchronous copy and its completion runs at once.
Evidence that nothing else in the tick waits:
1. Static (analysis script `.build/logs/reach.py`, not kept; input: the removal plan's `live_graph.txt` edges, 270,031
   lines, and extract/wait-points.txt): reachable from the roots 0x803CC3FC (Vsync server), 0x803CC36C (svtype 4 server),
   0x803C23D0 (ADX vsync), 0x803BE1E0 (ADX stream server), 0x80408CEC (Sofdec vsync), 0x803B7424 / 0x803B7400 (their
   registered wrappers): 451 functions. Wait points inside them: two, both class `progress`, neither an interrupt wait -
   0x803A6764 (a bounded byte-copy loop, `blt` back to 0x803A6748 on a counter) and 0x803C1108 / 0x803C1114 (a loop in
   fn_803c1068 that itself calls the stream server 0x803BE1E0 each pass and exits on a handle flag). No `halt` / `poll` /
   `clock` wait. Functions outside the CRI / Sofdec / ADX ranges: OSDisableInterrupts / restore (0x8037611C..0x80376160),
   DCFlushRange (0x80372534) and friends, memcpy / memset, and AX voice functions (0x80387090..0x8038B18C): no thread API,
   no OSSleepThread / semaphore / mutex / VIWaitForRetrace. Limit: the graph is the generator's (direct calls + resolved
   function-pointer edges); an unresolved indirect call would be missed - hence 2 and 3.
2. Dynamic, fail-loud: `System::idle_wait` now calls `NativeCri::note_wait`; a wait inside a tick stops the run with the
   reason. The Westopolis replay (90 s, 5,367 ticks) never stopped; the Idle upcall's waits are counted
   (`waits in the Idle upcall 0`).
3. The tick was already unpreemptable in effect: no game thread ran inside it (the delivery also disables the scheduler).

### Design (files)
- `runtime/include/gekko/native_interrupts.h`, `runtime/src/gekko/native_interrupts.cpp`: `relocate_frame` (copies the
  delivery handler's frame - back chain, exception OSContext, tags, scratch words - to the top of a given thread stack,
  makes the copy's context current, returns the new frame); `callback_return` / `deliver` finish on the frame r1 names.
- `runtime/src/gekko/native_video.cpp/.h`: the retrace delivery's last phase calls `RetraceTick = bool(Cpu&, u32& frame)`
  repeatedly (true while a guest call was started).
- `runtime/src/gekko/native_cri.cpp/.h`: `tick_work` (scratch word 1 = progress, survives the frame move and a savestate
  inside a server), `retrace_tick`, deferred tick through the HostWork delivery (same code), `note_wait`; the tick upcall
  kind is gone. Idle server: unchanged thread-context upcall (a retrace tick preempts it, as on the console).
  The three thread-API guards are written as explicit `install_hle(kSuspendThread / kSetPriority / kGetPriority, ...)`.
- `runtime/src/gekko/system.cpp`: wiring + `idle_wait` hook. `runtime/src/shadow_main.cpp`: two more stats in the CRI line.
- `runtime/tests/native_video_reference_tests.cpp`: `tick_cases`.

### Guard classification (`platform_runtime_coupling`)
At the snapshot's plan the three rows (0x80379154 / 0x803794B4 / 0x80379574) are not runtime-literal rows, so the CTest
does not show them (its two unclassified uses at the snapshot are runtime/include/gekko/native_reverb_owner.h:57 and :58 -
step 2's, not changed here). Reproduced with the plan plus a `runtime-literal:` entry per row: all three uses were
`unclassified` at native_cri.cpp:370 (`for (api : {kSuspendThread, kSetPriority, kGetPriority})` - the `install_hle` call
inside took the loop variable). The hooks ARE owner hooks (install_hle on the API's own entry; they serve only the case
"API on a replaced CRI thread" by stopping the run and decline every other call), so the code was changed to name the
address in each `install_hle` call; the classifier is untouched. After: three `owner-hook` uses (verified on the same
synthetic plan, before / after copies of the tool and `runtime/` in the scratchpad).

## 2. Gates (evidence)

Commands: every build / test / game run through `tools/agents/capped_run.ps1` (10 %, job CPU 2.5-5 %, no lock needed:
headless runs, own build trees). Harness: `.build/ab-jobs/run.sh` (headless Westopolis cold-boot replay,
`--boot-stage stg0100 --input-tape-play`, 90 s budget; copies of the recording's card / SRAM / cfgs). Baseline exe
`.build/ab-jobs/base.exe` = `.build/watch` built at 857399c BEFORE the first change; candidate `cand1.exe` /
`cand2.exe` (final source) with `SHADOW_NATIVE_CRI=1`.

- Gate 1 (what_if): not applicable - nothing is freed or pruned (no owner added; the bridge calls translated servers).
- Gate 2 contract test: `native_video_reference_tests` (86 checks, 0 failures): original OS words (OSWakeupThread,
  OSLoadContext, SelectThread...) + production NativeInterrupts / NativeVideo / NativeCri; the servers are stubs logging
  r1, r3, order and the current OSContext. Checks: order Vsync -> callback -> svtype 4, callback argument, every server on
  the Vsync thread's stack (r1 and exception context inside it), counters B+0x14 / B+0x18 and 0x805A6924, interrupted
  context current again, r14..r31 restored, no FPU ownership left, no upcall; deferred tick (lock held) runs in the
  HostWork delivery on the Vsync stack; a wait inside a tick stops the run. Negative controls (one-line mutations,
  rebuilt, run, reverted): M1 no frame move -> 6 failures; M3 `note_wait` ignored -> 1 failure; both killed. M2 (finish
  the delivery on the old frame instead of r1's) SURVIVES: the old frame is a complete copy, so nothing observable differs
  (equivalent for every property the test checks; r1's frame is the right one by construction).
- Gate 3 guest watch (`--watch-guest`, headless, from boot; 37 words that differ A/B outside the CRI structures,
  `.build/ab-jobs/watch2.txt`; A = wA.csv, B = wB.csv; PCM 015445f2... both): NOT a proof of deadness. The words are
  live heap memory: reads 0..41,824 per word in both runs, almost all by block copies / allocator walks (native memcpy and the like;
  A's report attributes everything to native code - its copied exe has no symbols); only 4 words are never read in A
  (0x8158C664, 0x8158C79C, 0x8158DB54 ...). Their A/B values: B holds scheduler return addresses (0x80379494,
  0x80376210/14), delivery tags ('NIRH' 0x4E495248, 'NIRQ' 0x4E495251 at 0x8060C408 / 0x8060C440) and vtable-looking
  pairs (0x8081EE88 / 0x8056E158 written in two unrelated objects: a copied struct padding word pair), A holds
  other thread garbage - i.e. leftovers of stack garbage that block copies carry in padding, the same class as erratum
  E1 (docs/GAME_ERRATA.md). INFERRED, not proven: no run shows a consequence (gate 4). The stack range
  0x8060C040..0x8060C460 could not be watched (the main thread's stack: the watch deadlocks, `resource deadlock would
  occur`); it lies below the main thread's saved sp 0x8060C468, which is identical in A and B (OSThread 0x80587450:
  srr0 0x8004ECB0, lr 0x8004ECB0, r1 0x8060C468 in both).
- Gate 4 (4 A, 4 B runs alternating, plus 2 more of the final source): PCM `015445f2...` in all 14 runs; game ticks
  3,645,000,000, VI frames 2,697, render frames 5,360, idle waits 23,090 in all. A/A (A1/A2) and B/B (B1/B2, fin1 vs B1):
  0 differing memory blocks (deterministic). AI census (GEKKO_CALL_CENSUS_ADDRS=8019F528,801A0000 R3_WORD=C, 90 rows):
  identical on the lock byte (the first byte of the recorded word, `lbz` at +0x34) between bridge off (bo0), the old upcall
  bridge (bu0) and the new bridge; the other three bytes (padding +0x35..+0x37) differ as GAME_ERRATA.md E1 documents.
  A vs B memory: 765 differing words in 10 blocks (A1/B1 = A2/B2 identically), classified:
  - 639 words inside the four replaced CRI threads' OSThread blocks (0x805ACA40..0x805AD6A0: 139 words) and stacks
    (idle 0x805AD6C8, svtype 4 0x805AF6C8, vsync 0x805B16C8, spinner 0x805B36C8 .. 0x805B46C8: 500 words) - in B those
    threads are never created (zero), in A they exist; plus 1 word at B+0xC90 (0x805AD6A0, the lock priority word, no longer
    written - native_cri.h) = 640;
  - 11 words of OS lists and saved contexts (+ 3 of the watched bss words below): 0x800000E0 (active-thread queue tail: A 0x805ACA40), 0x805870F8 / FC (run-queue entry
    for priority 24), 0x8058774C (main thread's active link), 0x805F20D4 / D8 (VI retrace queue: A holds the vsync and
    svtype 4 threads), 0x80587468 and 0x805877DC and 0x805877D8: saved volatile registers of the main thread's
    OSContext (same pc / lr / r1 in both) and the IdleContext (0x80587768, SelectThread's `r31 + 0x730`: 0x8058776C
    r1, 0x80587784, 0x805877E8 CR, 0x805877F0 CTR: the context the idle loop saves into on every interruption);
    0x805F1FE0 = the OS RunQueueBits word (r13 + 0x5AE0, r13 = 0x805EC500): written by SelectThread 0x803787B0
    (`stw r0,0x5ae0(r13)` at 0x8037889C; 28,961 writes in B) and the thread queue code 0x803793B0 / 0x80378ECC, read by
    SelectThread's idle loop (0x803788FC `lwz r0,0x5ae0(r13); cmplwi; beq` - a listed wait point, 107,514 reads in B) and by
    NativeCri::idle_may_run (5,268). End value A 0x80 = the ready bit of priority 24 (the CRI Idle thread exists and is
    ready in A), B 0. The read counts (636,160 in A, 118,108 in B) are the idle loop's polling count: CRI's threads change how
    often the loop spins, not what it decides - `idle_waits` is 23,090 in both. So the difference is thread existence, not behaviour;
    the retrace-queue words 0x805F20D4 / D8 likewise (VIWaitForRetrace 0x8037ECD4 sleeps on 0x805EC500+0x5BD4 through OSSleepThread
    0x803792C4 - in A the CRI vsync / svtype 4 threads are on it);
  - 0x8060C040..0x8060C460 (77 words): the main thread's stack below its saved sp (dead frames, B holds this run's delivery
    frames: tags 'NIRH' / 'NIRQ');
  - 37 watched words (34 heap + the 3 bss words 0x805F1FE0 / 20D4 / 20D8, gate 3). 640 + 11 + 77 + 37 = 765.
  The earlier bridge reports (docs/CRI_SERVERS.md 4b / 4c) classified the same kinds of words.
- Default-path identity (the change also touches deliver() / callback_return() for every source; bridge OFF, base.exe vs
  cand2.exe, 2 + 2 alternating, `--report-peek` of the 10 blocks): PCM 015445f2..., ticks 3,645,000,000, 5,360 frames, AI
  census (lock byte) identical, **0 differing memory blocks over all regions, 0 differing words** in A/A, B/B, A1/B1,
  A2/B2 (runs oA1/oB1/oA2/oB2). r1 is the handler's frame at every non-relocating return, so finish(c, c.gpr[1]) = finish(c, frame).
- Gate 5 performance (alternating, first pair discarded, process CPU seconds from session.json, capped runner CPU 3.0 %):
  A (bridge off, baseline exe) 12.52 / 12.36 / 12.24 / 12.71 -> kept 12.36, 12.24, 12.71; B (bridge on, plain tick)
  12.45 / 12.26 / 12.39 / 12.45 -> kept 12.26, 12.39, 12.45. Spreads overlap: **no measurable change**, no claim.
- Build + CTests: `.build/watch` (ShadowTheHedgehog) builds; tests tree targets native_video_reference_tests,
  native_threads_tests, native_svm_tests, native_sj_tests, native_adx_output_tests, native_cri_file_device_tests: 6 / 6
  pass. The full tests tree was not built or run; baseline failures were not re-run.
- Ratchet unchanged. Bodies compiled: not measured (no owner added or removed).

## 3. What is NOT done / open
- Default stays OFF. What a flip changes: `enabled_` in NativeCri::install, so CRI's four threads are never created and a
  savestate saved with it needs it (load refuses otherwise). Only the Westopolis replay is gated; other stages were not run.
- Heap-word deadness is inferred (gate 3); 0x805F1FE0 is unidentified.
- The Idle server remains an upcall (a retrace tick must preempt it; interrupt context would delay ticks during long Sofdec
  passes). The upcall machinery (continuation 0x80376224) therefore stays.

## 4. Shared list lines changed
None (no change to extract/*.txt, CMakeLists.txt; system.cpp: two lines in the CRI wiring / idle_wait).

## 5. Removed dependencies
The bridge no longer needs a thread-context upcall for the tick (kUpcallTick removed).

## 6. Docs
CRI_SERVERS.md section 5 amended (below this report's commit).

## 7. ADX exec (CRI step 3) and VIWaitForRetrace: discovery and plan (2026-11-07; nothing replaced yet)

Assigned by the main session after 2b: ADX exec (adxt_ExecHndl 0x803C3F4C / vint 0x803B7810) in oracle-first steps, plus
VIWaitForRetrace 0x8037ECD4.

### VIWaitForRetrace 0x8037ECD4 (14 words)
`OSDisableInterrupts; n = r13[0x5BCC] (retrace count); do OSSleepThread(r13 + 0x5BD4) while (n == r13[0x5BCC]);
OSRestoreInterrupts`. The queue is the VI retrace queue (0x805F20D4), woken by the native retrace delivery
(`kPhaseWakeup`, OSWakeupThread). Eight call sites: 0x8032D2B0 / 0x8032DB24 / 0x8032DE6C and 0x80495458 / 0x8049546C (game
side), 0x803C5418 / 0x803C5488 (CRI Idle / Vsync thread loops - dead under the bridge) and 0x803C573C (CRI's movie refresh
wait 0x803C5730, entered from the game via 0x804040AC). A native owner would still have to block the calling GUEST thread
(OSSleepThread: the guest scheduler); its value is only the 14 words. It belongs with the thread-substrate stages
(docs/NATIVE_THREADS.md, stage D), not ahead of them: left as is, deliberately.

### adxt_ExecHndl 0x803C3F4C
Heuristic extent 211 words; transitive closure in the removal plan's live graph (roots 0x803C3F4C): 413 functions /
39,498 instruction words including the already native SVM / SJ / rna owners' thunks (so the exec's own code is smaller; the
exact figure needs the closure minus owners - first task of step 3a). Replay census (Westopolis 90 s, base.exe, bridge off;
`.build/ab-jobs/ex.csv`): ExecHndl 21,324 calls (4 per vsync), vint 0x803B7810 21,324, 0x803C9234 21,324, 0x803C2F38 43,519,
0x803C12E4 35,519, 0x803B84B0 (status getter, 3 calls inside ExecHndl) 8,516, 0x803B843C 7,705, 0x803B7704 292, 0x803C59A4
238, 0x803C59C4 / 0x803C59E4 89, 0x803B76BC 55, 0x803C42C8 (state 1 = start-up path) 55, 0x803C5984 54, 0x803B7780 19.
Structure read from the first 211 words: handle (r31) byte +1 is the state (3 = playing, 1 -> 0x803C42C8, 2 = prepare:
rna free space 0x803C59A4 / 0x803C5984 against `+0x48 * 2` capped at 0x4000, SJ `vtable +0x18` GetChunk / `+0x20`
PutChunk per channel with the +0x18 joints, memset 0x8000540C of the chunk, 4 = finish); +0x4 = the ADXSTM handle,
+0xC = the rna handle, +0x6C / +0x70 / +0x71 / +0x72 flags, +0x9C / +0xA0 a vsync counter snapshot (0x805A6924).
Hot path = state 3 (per vsync, per used handle).

### Proposed steps (each: dedicated oracle in the reference interpreter vs the original words, boundary / failure cases,
negative controls, guest watch where state is retired, Westopolis A/B, census, benchmark)
- 3a  Closure map + oracle harness: the ADXT handle (16 x 0xC4 at 0x805A6928) layout from every reader / writer in the
  closure, the exact closure minus owners, and a test fixture (`native_adx_exec_tests`, template native_adx_output_tests)
  that runs the ORIGINAL functions over scripted handles with test stand-ins only for what is hardware or already native.
- 3b  Leaf layer: ADXSTM status getters 0x803B76BC / 0x803B7704 / 0x803B7780 / 0x803B77AC / 0x803B843C / 0x803B84B0 and the
  rna accessors 0x803C5984 / 0x803C59A4 / 0x803C59C4 / 0x803C59E4 (small, hot: 8.5k + 7.7k calls).
- 3c  The state-1 and state-2 paths (0x803C42C8, the prepare branch), then 0x803C27D0 / 0x803C2F38 / 0x803C12E4.
- 3d  ExecHndl itself and the vint 0x803B7810, then the vsync server's remaining ADX callee 0x803C23D0 -> native.
None of 3a-3d is started in this branch; the report above is complete for 2b only.

### Boundary (main session, 2026-11-07)
All AX / MIX rows - including those CRI calls (SetVoice*, AXAcquireVoice / AXFreeVoice, MIXInitChannel / ReleaseChannel /
SetInput / SetPan) - belong to agent audio-ax (local_55711a19-7351-41e8-9386-4967be9b5e9e). Steps 3a-3d neither own nor
change them: native ADX code calls the AX API entries as the guest does, builds nothing on the DSP PB array layout (audio-ax
replaces the command list with native voice state), and asks audio-ax (copying the main session) for a native entry point
when one is needed.

## os.threads: step plan (2026-11-07; planning only - nothing in this section has been built or run)

Assignment (main session): os.threads, 25 bodies / 17 frontier rows (docs/GC_REMOVAL_PLAN.md step 11), the thread substrate of
docs/NATIVE_THREADS.md stages A-D, ending with VIWaitForRetrace 0x8037ECD4 once a native owner can block a guest thread.
Native only. gc-removal 99fe8c3 merged into this branch (047785e). Interrupt-dispatch rows are coordinated with card-saves
(os.interrupts) through the main session; OSDisableInterrupts / OSRestoreInterrupts (0x8037611C / 0x80376130 / 0x80376144)
and the exception entry 0x80371620 are theirs, not mine.

Starting point (read from docs/NATIVE_THREADS.md and the removal plan of 2026-11-02, whose os.threads section predates the
2b merge - S0 regenerates it): the 17 frontier rows and who enters them.
| group | frontier rows | entered by |
|---|---|---|
| game Thread wrapper + loaders | OSInitThreadQueue 0x8037847C, IsThreadTerminated 0x80378498, Create 0x80378A44, Exit 0x80378C2C, Cancel 0x80378D10, Resume 0x80378ECC, Sleep 0x803792C4, Wakeup 0x803793B0, Yield 0x80378A08 | 0x8035A... (Thread ctor / dtor / reaper), loaders 0x80043D5C / 0x80045680, RenderStart 0x801D032C, 0x8032D1D0 |
| game scheduler exclusion | Disable / EnableScheduler 0x803784CC / 0x8037850C | 0x80050E28 / 0x80050EF0 (heap), 0x8035A390 (thread table), CRI lock 0x803C565C |
| CRI | GetCurrentThread 0x8037848C, Suspend 0x80379154, SetPriority 0x803794B4, GetPriority 0x80379574, Create / Resume above | ADXGC init 0x803C5118, lock 0x803C565C / 0x803C55F0, goto_mwidle_border 0x803C5534, Idle entry 0x803C5330 |
| runtime literals | SelectThread 0x803787B0, idle 0x803788E8, Resume / Wakeup / Yield literals | native_interrupts.cpp, system.cpp, native_video.cpp, native_gx_sync.cpp, native_rtgcnfs.cpp |
| other service | VIWaitForRetrace 0x8037ECD4 (graphics.vi, left to me by vi) | game 0x8032D1D0, CRI vsync / svtype 4 entries |

Facts that shape the order (all from NATIVE_THREADS.md / CRI_SERVERS.md, to be re-verified in S0):
- The game's only thread users are the two loaders (TBGRead, TExpandBG) and CRI's four server threads. Nothing else calls
  OSCreateThread. With the 2b bridge ON, CRI's four OSCreateThread calls are already replaced (guards cover Suspend /
  SetPriority / GetPriority); what still enters the thread rows from CRI is the lock pair, goto_mwidle_border and ADXGC init.
- The loaders: the stage-A bridge (SHADOW_NATIVE_JOBS) FAILED (the loading screen blinks: jobs ran only at the yield point
  while the game waits); GEKKO_NATIVE_BG_READ / GEKKO_NATIVE_EXPAND_JOBS exist but change game-visible load timing, so tapes
  cannot validate them (threading audit 2026-11-06). Removing the loader rows needs that timing decision from the user.
- Stage transitions and death / checkpoint reloads are not covered by any tape.

Steps (each ends with ALL section-4 gates: contract test vs the original incl. a negative control, guest watch of the retired
state, alternating 2x2 Westopolis A/B with PCM 015445f2..., every differing word classified, AI lock-byte census, performance
from alternating runs only; default OFF whenever game-visible timing changes; stop and report on a failed gate):
- S0 Census (no source change). Regenerate the os.threads frontier on the merged tree (removal_plan.py, generator lock);
  call census (GEKKO_CALL_CENSUS) of every thread-API entry per caller on Westopolis, Final Haunt, Lethal Highway and one
  stage transition, bridge OFF and ON; thread population per stage. Output: which of the 17 rows are still entered with
  the CRI bridge ON, and the exact threads that exist.
- S1 Contract oracle, stage 2 of runtime/tests/native_threads_tests.cpp: the scripted scenarios (create / resume / priority /
  yield / sleep / wakeup / suspend counts / exit / cancel / scheduler disable) become scenarios over the SINGLE-THREAD
  population the game actually has plus the CRI lock pair; the original words are the oracle. No native code yet.
- S2 CRI critical section and ADXGC init native (lock enter / leave 0x803C565C / 0x803C55F0, goto_mwidle_border 0x803C5534,
  GetCurrentThread / Suspend / SetPriority / GetPriority users): with the bridge ON these keep only their counters (one
  thread: no exclusion). Gates as above; the Idle-server drain call is a direct native call. Needs the bridge default
  decision from the main session (it is OFF today).
- S3 Main-thread waits (stage C): OSSleepThread / OSWakeupThread on the retrace and draw-done queues become "advance to the
  next event and deliver it"; native_video.cpp / native_gx_sync.cpp stop calling OSWakeupThread 0x803793B0. Interrupt tail
  rows (native_interrupts.cpp literals SelectThread 0x803787B0 / OSLoadContext 0x80372C68) are the os.interrupts boundary:
  coordinate with card-saves via main BEFORE touching them.
- S4 Loaders (stage A) - BLOCKED on the user's decision about load-timing change (native async loader vs original interleave);
  without it the Thread-wrapper rows (Create / Exit / Cancel / Resume / Yield / IsTerminated / Sleep on loader queues) stay.
- S5 VIWaitForRetrace 0x8037ECD4: only after S3 (it sleeps on the OS thread queue; the CRI vsync / svtype 4 entries and
  0x8032D1D0 are its users). Then prune (stage D): the generator's count, the ratchet value REPORTED (not changed), gate 2.
Dependencies: S0 -> S1 -> S2 -> S3 -> S5; S4 independent but blocked. Open question to main: S4's timing decision.

### S0 result (2026-11-07): thread-API census, Westopolis 90 s, exe cand2.exe (2b candidate), headless, 10 % cap
Frontier regenerated on the merged tree (e14f510; `removal_plan.py --release-dir .build/watch`, generator lock, 46 s wall,
2.55 % CPU): os.threads 25 bodies, 17 frontier rows, 7 freed by owning only the frontier; entered by code:game 20, code:cri 17,
runtime-literal 14 (8 before: native_cri.cpp adds the Suspend / SetPriority / GetPriority guards and the 0x803788FC idle
literal; native_jobs.cpp adds IsTerminated / Create / Cancel / Resume / Yield literals).
Census = GEKKO_CALL_CENSUS on the 17 entry addresses (declining hooks, so the guest runs as without). Calls, bridge OFF
(default) / bridge ON (SHADOW_NATIVE_CRI=1):
| API | OFF | ON | callers with the bridge ON |
|---|---|---|---|
| OSGetCurrentThread | 185,737 | 1 | CRI init 0x803C52B0 |
| OSDisable / EnableScheduler | 835,616 | 742,748 | game heap 0x80050E90 / 0x80050F50 (378,032 + 364,684), thread table 0x8035A3B4 (32) |
| OSSetThreadPriority / GetThreadPriority / Suspend | 185,736 / 92,868 / 98,136 | 0 / 0 / 0 | none (CRI lock pair and Idle entry no longer run) |
| OSCreateThread / Resume / Exit / IsTerminated / InitQueue | 36 / 98,270 / 32 / 32 / 40 | 32 / 32 / 32 / 32 / 40 | the game Thread wrapper 0x8035A1F8 / 0x8035A200 / 0x8035A0A4 / 0x8035A100 (32 loader jobs) |
| OSYieldThread | 11,794 | 11,794 | RenderStart 0x801D033C 5,271; RtGCNFS 0x8047BC64/68 6,491; Thread dtor path 0x8035A064 32 |
| OSSleepThread | 16,029 | 5,293 | RW 0x80495D30 (draw-done wait) 5,291; 0x8037ECFC (inside VIWaitForRetrace) 2 |
| OSWakeupThread | 16,324 | 16,324 | reaper 0x8035A2F8 5,539; retrace delivery 0x80376210 5,395; GX 0x80494C68 5,349 / 0x80494AAC 9; 0x80378CCC (exit) 32 |
| SelectThread 0x803787B0 | 466,542 | 78,848 | internal SDK / continuation entries |
| VIWaitForRetrace 0x8037ECD4 | 10,738 | 2 | RW 0x8049545C 2 (CRI 0x803C541C / 0x803C548C: 0 with the bridge) |
Reading (census facts, not yet a design):
- With the bridge ON the CRI thread rows are gone from the frontier's live calls except ONE OSGetCurrentThread in ADXGC init
  (0x803C52B0): S2 shrinks to that call plus the bridge default. The lock pair, Suspend, SetPriority, GetPriority and
  VIWaitForRetrace from CRI do not run at all.
- What remains is the loaders (32 jobs: Create / Resume / Exit / IsTerminated / Yield and the reaper's Wakeup), the heap's
  scheduler exclusion (743 k Disable / Enable pairs, the hot rows), RW's draw-done Sleep and the retrace / GX Wakeups.
- The heap's Disable / EnableScheduler pairs (0x80050E28 / 0x80050EF0, 371 k pairs in 90 s) only exclude the workers; with
  no other thread alive they could be a native counter - but loaders still exist, so that needs S4's decision first or a
  proof that the count is never observed except by the thread switch.
Not run: Final Haunt, Lethal Highway, a stage transition (tapes final-haunt / lethal-highway-sfx exist; run in S1's
population check). Replays were headless, bridge default; no gate here beyond the census itself (no code changed).

### Bridge default ON (2026-11-07; condition set by the main session: A/B on Final Haunt and Lethal Highway)
Method: `.build/ab-jobs/drive.sh` - exe cand2.exe (the 2b candidate), headless, 120 s budget, 10 % cap, alternating A1 B1 A2 B2
(A = bridge off, B = SHADOW_NATIVE_CRI=1), `--report-peek` of 10 guest ranges + the AI lock-byte census (0x8019F528 / 0x801A0000).
| | Final Haunt (stg0604, final-haunt tape) | Lethal Highway (stg0202, lethal-highway-sfx tape) |
|---|---|---|
| game ticks / VI / render frames, all four runs | 4,860,000,000 / 3,597 / 7,160 | 4,860,000,000 / 3,597 / 7,159 |
| PCM (15,287,680 B), all four runs | 2c72d41d... | 34d3e1c2... |
| A1 vs A2, B1 vs B2: differing blocks / words | 0 / 0 | 0 / 0 |
| AI census (lock byte, A1 vs B1 and A vs A, B vs B) | identical (144 records) | identical (78 records) |
| A vs B differing blocks / words | 10 / 772 | 8 / 581 |
Every differing A-vs-B word is in one of: CRI thread blocks and CRI thread stacks (FH 247 + 523 words, LH 136 + 428; the OSThreads
and stacks that exist in A and are never created in B), the OS active-thread queue 0x800000E0 (1), the idle context / run queue
words 0x80587038-0x805877F8 (10 / 9), RunQueueBits 0x805F1FE0 (1), the retrace queue 0x805F20D4/D8 (2), the main thread's dead stack
below its saved sp 0x8060C468 (FH 80 words, LH 0), and 0x805AD6A0 (1; just past the spinner OSThread, 0xE in A - identified in the
Westopolis report as the "+1"). UNCLASSIFIED beyond inference: 7 heap words (FH 0x815853FC.., 0x81587370) and 3 (LH 0x8158F140..)
- stack-garbage padding as in Westopolis (INFERRED, not proven by a watch).
Flip: `enabled_ = env == nullptr || env[0] != '0'` (SHADOW_NATIVE_CRI=0 = the original CRI threads, diagnosis); header, comment and
status line updated. A state saved with the bridge OFF and live CRI threads (idle OSThread state word != 0 after the memory
restore) is refused by a bridge-ON build with a message - the mirror of the existing check (saved ON, loaded OFF). A state saved
with the bridge ON needs the bridge (unchanged).
Check on the rebuilt exe (.build/watch/bin/Release, full rebuild: first attempt hit the known C1090 PDB error, the re-run passed):
Westopolis 90 s, default / SHADOW_NATIVE_CRI=0 alternating D1 Z1 D2 Z2: PCM 015445f28cd10698 in all four, 3,645,000,000 ticks, 2,697 VI
and 5,360 render frames; D1 = D2 and Z1 = Z2 (0 differing blocks); D vs Z 10 differing blocks (the classified set above); the status line
shows "native CRI: on (default)" / "off (SHADOW_NATIVE_CRI=0)". CPU: replays 2.6-2.9 % of 32 logical processors (cap 10 %).
Not run: the full tests tree after the flip (see the next entry).
Tests after the flip + merge of gc-removal 7e3cda2 (tests tree rebuilt, `ctest -C Release`, capped runner): native_cri_file_device_tests,
native_threads_tests, native_adx_output_tests, native_video_reference_tests (86 checks incl. the CRI tick cases), native_liveness_roots,
native_liveness_roots_lifted (no regeneration needed: the flip adds no root address), platform_gc_removal_plan, platform_runtime_coupling,
native_wait_points_classifier PASS. native_wait_points FAILS ("wait points file is stale"): it is in the baseline-failure list of
docs/AGENT_COORDINATION.md and depends on the DOL / function list, not on anything this branch changed; not touched. Tests-tree build:
wall 2,359 s, 9.49 % of the machine (cap 10 %).

## os.threads S1: OSDisableScheduler / OSEnableScheduler owned natively (2026-11-07)
Original (disassembled, tools/doldis.py --func): both are `OSDisableInterrupts; n = *(r13+0x5AE8); *(r13+0x5AE8) = n +/- 1;
OSRestoreInterrupts(prev); return n` - the SDK scheduler-disable count, read by the guest scheduler's reschedule code. Census
(S0): 835,616 calls per 90 s with the bridge off, 742,748 with it on (game heap 0x80050E90 / 0x80050F50 and the thread table).
Owner: runtime/src/gekko/native_os_threads.{h,cpp} (`gekko::os_threads::install`, wired in system.cpp after os_settings; rows
added to extract/native-owners.txt; Owners added to native_file_api_tests' owner list). It reads and writes the same guest word,
returns the previous value in r3, hands control back before touching anything when a deliverable interrupt is pending (like the
interrupt gate's owner), and leaves MSR[EE] as it was. The word stays a guest word: the guest scheduler (loader threads) still reads it.
Gate 1, contract test `os_threads_contract_tests` (original instructions in the reference interpreter vs the owner): 96 inputs
(8 counts incl. the wrap values x EE 0/1 x 3 register salts x 2 entries) equal in r3, r1, r13-r31, LR, MSR[EE], the count word and
its 12-word neighbourhood; negative controls (count restored, opposite direction, EE left cleared) are detected 96 / 96 / 48 times.
Volatile scratch (r0, r4, r5, CR0) is not compared (the original leaves the gate's values there). NOT covered by the oracle: the
interrupt-pending hand-back (mirrors the interrupt gate, tested by os_interrupts_contract_tests; exercised by the replays).
Gate 2, retired state: none. The owner writes the only word the original wrote besides the dead frames below the caller's sp.
Gate 3, Westopolis 90 s, alternating A1 B1 A2 B2, A = the SAME tree built without the owner (rows not listed, install
commented out; exe base3), B = with it (exe new): PCM 015445f28cd10698 in all four, 3,645,000,000 ticks, 2,697 VI / 5,360 render
frames; A1 = A2, B1 = B2, **A vs B: 0 differing blocks, 0 differing words** over the 10 peeked ranges (including the dead
stack regions), AI lock-byte census identical (90 records). NB: a first comparison against the earlier pre-merge exe showed 239
differing words at 0x805A2F60-0x805A31xx - those came from the OTHER agents' work merged in gc-removal 7f5c5ef, not from this owner;
the same-tree baseline above is the valid isolation. (A baseline that merely comments out the hook exits 6: the generator prunes
the original bodies of listed rows.)
Gate 4, ratchet: removal plan on the tests tree: platform bodies compiled 768 (the CMake tests default is 770 at b1456bc), os.threads
23 bodies / 15 frontier rows (was 25 / 17). The shipping count is not measured here. SHADOW_GC_REMOVAL_RATCHET not changed.
extract/liveness-roots.txt and liveness-roots-lifted.txt regenerated with the tests' exact arguments (+0x803784CC, +0x8037850C only);
native_liveness_roots, _lifted, platform_gc_removal_plan, platform_runtime_coupling, native_wait_points(+classifier),
os_interrupts_contract_tests, native_threads_tests, native_video_reference_tests PASS. native_file_api_tests still fails on the 66
CRI "listed, not installed" rows of the baseline; it reports no row of this owner as missing or unlisted.
Performance: wall seconds of the headless replays (A 26/20, B 19/19, first of each discarded) - only 2 runs per side, below the
protocol's 3, so no performance claim is made.

## os.threads S2: OSGetCurrentThread / OSIsThreadTerminated / OSGetThreadPriority owned natively + FH/LH thread census (2026-11-08)
Merged gc-removal 1858197 (fast-forward) first. Original (doldis): GetCurrentThread 0x8037848C = `lwz r3, 0x800000E4`; IsThreadTerminated
0x80378498 = `(u16)thread[+0x2C8] is 8 or 0`; GetThreadPriority 0x80379574 = `thread[+0x2D4]` - confirmed identities, pure readers (no
interrupt gate, no store). Owners in native_os_threads.{h,cpp} (serve_get_current_thread / _is_thread_terminated / _get_thread_priority),
rows added to extract/native-owners.txt, liveness-roots lists regenerated (+0x8037848C only; the other two already had roots).
Thread-usage census, Final Haunt (stg0604, headless tape replay, 22 s wall) and Lethal Highway (stg0202, 14 s wall), bridge ON, scheduler owner off so its
calls show: same callers as Westopolis, nothing new. FH: Disable/EnableScheduler 1,716,970 each (heap 80050E90/80050F50, thread table
8035A3B4 x30), Yield 11,512 (801D033C, 8047BC64/68, 8035A064), Sleep 5,269 (80495D30 draw-done 5,267; 8037ECFC x2), Wakeup 16,402
(8035A2F8, 80376210, 80494C68, 80378CCC x30, 80494AAC x9), Create/Resume/Exit 30 each, IsTerminated 30, GetCurrent 1 (803C52B0),
VIWaitForRetrace 2 (8049545C), SelectThread 78,348. LH: Disable/Enable 1,034,205, Yield 11,884, Sleep 27, Wakeup 16,446,
Create/Resume/Exit 45, IsTerminated 45, GetCurrent 1, VIWaitForRetrace 2. GetThreadPriority, SetPriority, Suspend, Cancel: 0 calls.
Gate 1, contract test (os_threads_contract_tests, original instructions vs owner): 4,032 reader inputs (12 states incl. 0/8/0xFFFF x 7
priorities x 4 current-thread words x 2 thread addresses x 2 salts) equal in r3, r1, r13-r31, LR and two 0x300-byte thread windows plus
the current-thread word (so any store is caught); negative controls (wrong state test / wrong field / zeroed result, inverted
result, clobbered r31) detected 2,464 / 3,696 / 4,032 times. The scheduler-count part still passes (96 inputs, 96/96/48).
Gate 2: no retired state - the readers wrote nothing in the original either.
Gate 3, Westopolis 90 s, alternating A1 B1 A2 B2, A = same tree without the three S2 rows (exe base4), B = with (exe new): PCM
015445f28cd10698 in all four, 3,645,000,000 ticks, 2,697 VI / 5,360 render frames, A1=A2, B1=B2, A vs B 0 differing blocks and 0
differing words over the 10 peeked ranges, AI lock-byte census identical (90 records). FH/LH were censused, not A/B'd, for S2 (the
S2 rows are only reached through the same Westopolis paths; GetCurrent 1 call, IsTerminated in the loader exit path).
Gate 4, ratchet (tests tree, merged): platform bodies compiled 681, frontier 185; os.threads 17 bodies / 12 frontier rows
(S1 report: 23 / 15 before the merge, so not a pure S2 delta). Shipping not measured. SHADOW_GC_REMOVAL_RATCHET not changed.
Tests: os_interrupts_contract, os_threads_contract, native_threads, native_video_reference, native_liveness_roots (+_lifted),
platform_gc_removal_plan, platform_runtime_coupling, native_wait_points (+classifier) PASS; native_file_api_tests fails on the same
66 CRI "listed, not installed" rows as before, none of them mine.
Performance: wall seconds A 17/15, B 18/15 (2 per side) - no claim.
Left in os.threads: Create/Resume/Exit/IsTerminated callers = the 30-45 loader jobs (S4, blocked on the load-timing decision);
Sleep/Wakeup/Yield main-thread waits and SelectThread (S3); VIWaitForRetrace (S5).

## os.threads S3 design (2026-11-08) - users, guarantees, native design; NO CODE for Sleep/Wakeup/Yield yet
Per main's directive (user 2026-11-02, NATIVE EXECUTION, NOT GAMECUBE SCHEDULER REPLICATION): no native run queue, no swapped
register contexts. OSLoadContext / SelectThread / OSSwitchFPUContext must end up UNREACHABLE, not reimplemented.
Measured callers (bridge ON, Westopolis 90 s / Final Haunt / Lethal Highway; call-site = return address):
- OSSleepThread: ShowRaster 0x80495CBC (ret 0x80495D30), queue = r13+0x61D8, only when the swap ring is full: about 5.3k (Westopolis) / 5,267 (Final Haunt) / 25 (Lethal Highway);
  inside VIWaitForRetrace 0x8037ECD4 (ret 0x8037ECFC): 2 per run, from RW engine start (0x8049545C / 0x80495470). Static callers never
  reached in these runs: loader steps 0x80043D7C / 0x800456A0 (load-hold park; the fn_8032D138 owner returns "never"), semaphore
  wrapper 0x8032D42C, GXDrawDone 0x8039653C, SDK 0x8037B0E4 / 0x8037D7E4 / 0x80385F3C.
- OSWakeupThread (16.4k per run): reaper 0x8035A2F8 (once per frame, queue +8 of the loader manager), the retrace delivery
  (native_video, ret 0x80376210, 5.4k, the VI queue), RW swap callback 0x80494C68 / 0x80494AAC (5.3k / 9, wakes ShowRaster's
  queue 0x61D8 and sets displayed index 0x61B0), OSExitThread 0x80378CCC (joiners; nobody joins: the dtor cancels).
  Only the VI queue and 0x61D8 ever hold a sleeper; every other wake finds an empty queue (no-op).
- OSYieldThread (11.5k): RenderStart::update 0x801D033C (once per frame; the loader jobs' run point), RtGCNFS owner 0x8047BC64 / 68
  (chunk transfers), the Thread entry 0x8035A064 (loader jobs).
- OSInitThreadQueue: 12 static sites; 38 runtime calls (thread creation + queue setup).
- Thread population with the CRI bridge ON: the main game thread + the loader jobs (TBGRead / TExpandBG; 30 per Final Haunt run,
  45 per Lethal Highway run, each a detached priority-16 worker doing one step, exit). Nothing else creates a thread (CRI is gone).
Guarantees each user needs: (1) ShowRaster: main blocks until the retrace-event callback publishes a new displayed index -
ORDERING callback-before-return, timing = it paces the game to the VI rate; (2) VIWaitForRetrace: return after the next retrace
event; (3) reaper / exit / empty-queue wakes: none (no sleeper); (4) Yield: "ready equal-priority threads run once" - with no
loader alive it is a no-op; (5) loaders: run only while main yields or waits; results reach the game as completion Tasks.
DESIGN (native primitives, explicit state; no scheduler): with exactly one guest thread, a wait is not a switch.
 - `MainWait { queue_addr, woken }` owned by the runtime (one slot). OSSleepThread(q) owner: set {q,false}; loop { advance the game
   clock to the next event and deliver it (the existing Cpu::idle_wait / native_interrupts machinery) } until woken; return.
   OSWakeupThread(q) owner: if MainWait.queue == q set woken, else no-op (= the original on an empty queue). The queue's guest
   words are not maintained (retired state; gate 2 must show no reader). VIWaitForRetrace and vi's ShowRaster wait become the same
   primitive (wait for an event predicate).
 - Precondition checked loudly (not silently): the owner asserts no other guest thread is alive; a violation is a hard stop, never a
   fallback to an emulated scheduler.
 - Once Create / Resume / Exit / Cancel / IsTerminated / Yield / Suspend / SetPriority have no live caller, SelectThread,
   OSLoadContext, OSSwitchFPUContext and the idle loop are unreachable; the interrupt tail in native_interrupts (finish / resume ->
   SelectThread) stops rescheduling - that edit is os.interrupts (card-saves), to coordinate through main.
BLOCKER (not a design choice): a native Sleep may only wait natively if no loader job needs to run in that wait. The loader jobs ARE
guest threads that run exactly in main's waits (the 2026-11-02 bridge failure: the loading screen never finished). So the order is
forced: loaders first (S4: native async jobs, publishable at yield AND wait points - changes load timing, needs the user's decision
recorded in this file), then the Sleep/Wakeup/Yield/VIWaitForRetrace owners (S3/S5), then the pruning. Doing Sleep before the loaders
would need a native scheduler. For vi: ShowRaster can keep calling guest OSSleepThread 0x803792C4 until then; it is the only
thing that lets loader threads run during the swap wait.
Done now (no scheduler involved): see 'S3a' below.

## os.threads S3a: OSInitThreadQueue owned natively; S2 hook-replacement fix; loader census with the native-loader switches (2026-11-08)
OSInitThreadQueue 0x8037847C = `queue->tail = 0; queue->head = 0` (two stores; r3 unchanged). Owner serve_init_thread_queue; rows in
native-owners.txt; roots lists regenerated (+0x8037847C). Contract test (readers harness, now 4 functions): 5,376 inputs equal,
negative controls (head-only clear, tail left non-zero, clobbered r31 etc.) detected 3,808 / 5,040 / 5,376 times.
Westopolis 90 s 2x2 alternating, A = same tree without the owner (base5), B = with (new2): PCM 015445f28cd10698 x4, 3,645,000,000
ticks, 2,697 VI / 5,360 render frames; A1=A2, B1=B2, A vs B 0 differing blocks / 0 differing words; AI lock-byte census identical.
No retired state (the original stores the same two words). Tests as S2 (same 66 CRI rows in native_file_api_tests, none mine).
**Defect in S2 (c6496a1), fixed here:** Cpu::install_hle REPLACES an earlier hook. native_jobs (OSIsThreadTerminated
0x80378498) and native_cri (OSGetThreadPriority guard 0x80379574, the replaced-CRI-thread hard stop) install their hooks
unconditionally in System before os_threads::install, so the S2 owners replaced them. The default configuration never calls
either (A/B identical), but SHADOW_NATIVE_JOBS=1 and the guard were silently disabled. All owners that share an address now install
through install_after_existing(): the earlier hook runs first (serves, or declines), only a declined call reaches the owner. NOT
verified in-game: no run exercises the guard or SHADOW_NATIVE_JOBS=1; verified by code reading and the default-config A/B only.
Loader census with GEKKO_NATIVE_BG_READ=1, GEKKO_NATIVE_EXPAND_JOBS=5, GEKKO_NATIVE_ONE=1 (bridge ON, S2 exe, headless tapes
Westopolis stg0100 / Final Haunt stg0604 / Lethal Highway stg0202, 23-27 s each, exit 0, PCM differs from switches-off as expected):
OSCreateThread, OSResumeThread, OSExitThread, OSIsThreadTerminated, OSCancelThread: 0 calls in all three - no loader guest thread
exists. Remaining thread-API calls per run: OSSleepThread 27 (one tape) / 5,337 (two tapes) from ShowRaster 0x80495D30 + VIWaitForRetrace
x2; OSWakeupThread ~16.1k (retrace delivery 5.4k, reaper 0x8035A2F8 5.4k, RW swap callback 5.3k + 9); OSYieldThread ~5.9k (RenderStart
0x801D033C 5.3k, RtGCNFS 0x8047BC64/68 ~560); OSInitThreadQueue 4; OSGetCurrentThread 1 (CRI init 0x803C52B0); VIWaitForRetrace 2.
So with the switches the only guest thread is main; S4 is "make the switches the default" (a load-timing decision for the user,
not mine), after which the single-wait-slot design above applies.


## os.threads S4 (2026-11-08): native async loading is the default (user: "Yes, make default")

Changes: `native_rtgcnfs.cpp` (BG_READ), `native_expand_jobs.cpp` (EXPAND_JOBS defaults to 5), `native_one.cpp` (ONE) - each
ON when the variable is unset, `=0` is the original. Details, load times and the census: docs/NATIVE_THREADS.md "S4".
Also: `os_threads_contract_tests` gained the hook-chain regression (earlier hook serves -> owner not reached; declines ->
owner reached; four rows x 2 = 8 checks); negative control: plain install_hle fails it ("the earlier hook was not consulted
first"). Gates run: the oracle CTests (bg_read, expand_jobs, one, rtgcnfs, os_threads_contract) pass;
native_file_api_tests fails on the same 66 "listed, not installed" rows as before (no new row). Westopolis 2x2+ A/B is
`=0` vs default on the same exe (s4.exe): `=0` PCM 015445f2 (as the old canonical) in 5 of 5, default 46a70a10 in 5 of 5;
both 3,645,000,000 ticks / 2,697 VI / 5,360 render frames; A vs A and B vs B: 0 differing blocks; A vs B 150 blocks.
Guest loader threads: WP 32->0, FH 30->0, LH 45->0. Not run: strict oracle, all-stage first-gameplay A/B, save/load and audio
listening checks, manual play (user).


### S4 validation round 2 (2026-11-08, exe s4 = f551333's tree; `=0` means all three switches off, same exe)

- **Every bootable stage, default** (38 stages, windowed, 12 s, `measure_loads.sh`): 38 of 38 exit 0, stop
  reason 0 (not failed), 674+ rendered frames each.
- **First-gameplay semantic A/B, all 38 stages** (`ab_first_frame.sh` with the new `ENVA=`/`ENVB=` per-side variables;
  A = `=0`, B = default; `ab_semantics.py`, 10-16 frame pairs per stage): the comparator readout is equal in every
  field except `player.address` (the player object's heap address, 38 of 38 stages; the heap layout differs because the
  default allocates no TExpandBG/TBGRead workers, contracts G2) and the expected frame counter / tick. PlayAction comes
  0.38-1.80 s earlier (default always at 1.517 s).
- **Savestates**: mid-load round trip under the default (stg0100 saved at 56.0 M ticks while the load is running, loaded,
  resaved at 120 M) and under `=0` (70.0 M): both identical to the straight run in every guest region. Cross-switch: a
  state saved after the load under `=0` loaded under the default (and the reverse), resaved at 140 M: identical to the
  same state loaded under its own setting, in every region. Memory-card saves were NOT exercised in-game (no tape that
  saves; recording one needs the user); the card path does not touch the loader switches.
- **Strict oracle: not runnable at this tree, independent of S4.** `ShadowTheHedgehogReference --verify-player-strict`
  (scope `loader_scope_ranges.txt`) stops at game time 0.02 s: "unimplemented instruction" at PC 0x4, LR 0x803984F4
  (a call through the SDA table word 0xFD8(r2)+0x4CC, null in the interpreter's boot; around GXInit, a native owner).
  Identical with all three switches `=0`, 12 segments matched, 0 mismatched before the stop. The loader-scope oracle is
  covered by the owner CTests (all pass) until the reference runner's boot is repaired (not mine; main to route).
- **Audio divergence**: only the timing shift, nothing missing. Neutral-input stg0100, headless, 40 s: all 475 AX voice
  calls (AXAcquireVoice 54, AXSetVoiceState 280, AXSetVoiceAddr 141) occur in both runs, in the same order with identical
  arguments (address, r3, r4, LR); the 54 boot-time acquires have delta 0, the rest are shifted by 29,767,500 ticks
  (0.735 s; 250 calls) or 29,565,000 (0.730 s; 164) or 29,700,000 (7): the load finishes 0.73 s earlier, and the 5 ms
  audio pass quantises each event to its own pass. First audible sample 1.700 s -> 0.970 s (the same 0.730 s). On the
  Westopolis tape the cross-correlation lag is +0.735 s in 14 of 16 windows; a 1 s window at 5 s is 93 % bit-identical
  after shifting by 23,520 samples. Past that the waveforms differ because the shift is 44.1 video frames, not a whole
  number: game logic keyed to frames lands a sub-frame later against the 5 ms pass, and the tape's inputs meet the game
  0.73 s earlier in its timeline. No stream or voice is missing.

## os.threads S5 attempt (2026-11-08): Sleep/Wakeup/Yield owners - reverted, waiting for a decision on how an event runs without an exception

State of the branch: the first S5 code (owners for OSSleepThread 0x803792C4 / OSWakeupThread 0x803793B0 / OSYieldThread 0x80378A08 with a
single-thread assert, 13 unit checks) was committed as f5bf640 and reverted in a907b73. Nothing of S5 is live. Main objected to the design (the
wait waited through the guest interrupt path, with a core change `Cpu::request_masked_delivery` that I had already reverted before building
again, and kept state in an OSThread field); the single-wait-slot design stands: runtime-owned `MainWait{queue, woken}`, guest MSR untouched, no
OSThread field, no core change, and the wait runs the event's NATIVE processing directly (no exception, no rfi).

### Root cause of the 37-frame stall (first in-game run of the owners)
The owner kept the wait in OSThread::queue (+0x2DC), the field the original OSSleepThread itself writes. The interrupt tail that every delivery
ends with (`__OSReschedule` -> SelectThread, native_interrupts.cpp kSelectThread) still runs the original SetRun / UnsetRun, and SetRun writes
`thread->queue = &RunQueue[priority]` (0x80379420: `stw r0, 0x2dc(r6)`). Traced with owner prints: after each wake the tail overwrote the wake
mark, the restarted OSSleepThread found 0, took it for a NEW call, and waited for a wake that had already been consumed; ShowRaster's swap queue
(r13+0x61D8) then never woke again (28 wakes by the swap callback 0x80494C68, then only the VI queue's), the game stopped at 39 VI commits / 37
render frames while the game clock went on (Westopolis tape: 3,685,500,000 ticks, 1-4 s wall). So: any state that lives in an OSThread field is
shared with the scheduler code the delivery still runs; the wait state has to be runtime-owned, as designed.
A second finding, which does not depend on that bug: VIWaitForRetrace (0x8037ECD4) and ShowRaster (0x80495CBC) call OSSleepThread with MSR[EE]
CLEAR (they bracket the call with OSDisableInterrupts / OSRestoreInterrupts), so the first run's "Sleep with interrupts off" stop fired at game time 0.

### What is needed for "the wait runs the event natively", and what is not possible today
`wait_for_event` (the wait point) raises a native interrupt LINE; the event's work is then done ONLY by NativeInterrupts::deliver(): the OS context
save, OSDisableScheduler, an exception OSContext made current, the source's handler as a STEP MACHINE (`bool handler(Cpu&, frame, step)`: it starts a
guest call with `interrupts_.call_guest(...)` and returns true; the delivery continuation re-enters it with step+1), then the tail (clear context,
OSEnableScheduler, __OSReschedule -> SelectThread, OSLoadContext). A native Sleep that wants to process an event directly needs a synchronous
driver for those step machines: run `handler(c, frame, 0)` on a scratch frame, perform each requested guest call with `gekko::guest_call`, feed
the result back as step n+1, and skip the context save / reschedule / rfi. That is a NEW function in native_interrupts (os.interrupts, card-saves),
not something an os.threads owner can do from outside, because `call_guest` writes pc / LR / the continuation and the frame words are private.
Which events cannot be driven that way today (each is a handler registered with set_handler):
 - Vi (native_video.cpp retrace_handler, 0x8037E27C): counters, the game's pre-retrace callback, the staged commit, the post-retrace callback (the RW
   driver's 0x80494A../0x80494C68 swap callback, which does OSWakeupThread on ShowRaster's queue), OSWakeupThread(VI queue). Driveable by a
   synchronous driver (all its guest calls are plain functions); it is the one event that ends the swap wait.
 - AiDma / DspDma / Dsp (native_audio.cpp): the AX library's callbacks (AXRegisterCallback 0x80385F60's, the DSP/AI handlers) as guest calls. Driveable the
   same way, but they must run in the same order and with the same FPU-context protocol (clear_exception_context between callbacks).
 - PeFinish / GxBreakpoint (native_gx_sync.cpp): draw-done / breakpoint callbacks + OSWakeupThread(finish queue). Driveable.
 - Card (native_card.cpp): the card operation's completion callback. Driveable.
 - HostWork (native_cri.cpp, the CRI bridge): ONLY through `request_upcall` (a thread-context upcall on the interrupted or a server stack, EE set,
   with further deliveries nested on top of it) - the Idle / Vsync servers. A synchronous driver would have to run an upcall's guest code inside the
   owner, and that code itself waits (ADX output spin 0x803CF0D4 for the DSP DMA of its ARQ transfer): a nested wait inside a wait. Not drivable
   without a design for nested synchronous waits (or without removing the bridge first - os.threads stage C / CRI servers).
The delivery tail's reschedule also covers `yield_on_finish` (transitional CRI) and the upcall return protocol, which the synchronous driver would
not run: with the bridge ON these exist today.

Decision needed from main / the user: (1) allow the new synchronous event driver in native_interrupts (card-saves owns it) for Vi, Ai/Dsp/Dsp-DMA,
PeFinish, GxBreakpoint and Card, with HostWork/upcalls either excluded (the CRI bridge must be gone or its servers run elsewhere first) or given
their own design; (2) or keep waits as they are until os.interrupts removes the exception/rfi path. Until then OSSleepThread / OSWakeupThread stay
guest code (the single-thread precondition is already true: S4 removed every loader thread).

## Scheduler remnants after S5: what still reaches SelectThread / OSLoadContext / OSSwitchFPUContext (2026-11-08)

Question from main: for every scheduler function that is alive only through the thread switch, which entry path still reaches
it once Sleep / Wakeup / Yield are native? Method: static `bl` cross-references (tools/dolxref.py, tools/doldis.py) mapped to
the enclosing function, plus a call census (GEKKO_CALL_CENSUS, hooks that count and decline) of the merged tree 934191d
(`.build/ab-jobs/s5m.exe`, defaults: native loaders, native CRI, S5 waits): Westopolis / Final Haunt / Lethal Highway input
tapes (40 s budget, ~2395 fields each) and all 38 boot stages (8 s budget, ~474 fields each, no input). Plus a guest watch
of the run-queue words in Final Haunt. Scripts: `.build/ab-jobs/schedcen.sh`, `schedcen38.sh`, `wsched.txt`.
Function names are the Dolphin SDK names by position, size and call pattern (the DOL has no symbols for most of them).

### What the 38 stages and 3 tapes actually enter

Only ten of the 34 listed addresses were ever entered. Totals over the 38 boots: OSLoadContext 44,824; OSWakeupThread 52,660
(owner); OSYieldThread 15,002 (owner); OSSleepThread 14,282 (owner); OSSwitchFPUContext 16,271 (= __OSLoadFPUContext 16,271,
__OSSaveFPUContext 4,060); SelectThread 3,650 (= OSSetCurrentContext 3,650 = the switch tail 0x803789A8, 3,650). Never entered
in any of the 41 runs: OSSaveContext 0x80372BE8, OSClearContext 0x80372D78, OSInitContext 0x80372D9C, __OSUnlockAllMutex
0x80376C78, the idle server call 0x803788E8, __OSReschedule 0x803789D8, OSCreateThread 0x80378A44, OSExitThread 0x80378C2C,
OSCancelThread 0x80378D10, OSResumeThread 0x80378ECC, OSSuspendThread 0x80379154, 0x803794B4 (thread priority), the guest
exception tail 0x80371BE4, __OSUnhandledException's helpers 0x80373590 / 0x80373684 / 0x80373708, the guest external-interrupt
handler 0x803765F4, and the DVD / VI / AI interrupt handlers 0x80379938 / 0x80379CA8 / 0x8037E290 / 0x80381D00 / 0x803823B0
(they call OSClearContext + OSSetCurrentContext around their callbacks; the native Vi / Ai / Dsp / Card source handlers
replace them). HostWork deliveries: 0 in all 41 runs ("host-work deliveries 0"); waits inside the Idle upcall: 0.
The census covers boots and three tapes, not every possible play; "never entered" means not in these 41 runs.

### Entry paths that remain, per function

| Function | Static callers | Entered at run time by | Evidence |
|---|---|---|---|
| SelectThread 0x803787B0 | __OSReschedule 0x803789D8, OSYieldThread (owner), OSExitThread 0x80378C2C, OSCancelThread 0x80378D10, OSResumeThread 0x80378ECC, OSSuspendThread 0x80379154, OSSleepThread (owner), OSWakeupThread (owner), 0x803794B4 (priority) | ONLY `NativeInterrupts::reschedule` (native_interrupts.cpp:758) through `finish()` / the upcall tail: `leave_to_dispatcher(0x803787B0)`, lr 0x80376214, r3 = 1 | every census row has lr 0x80376214 and r3 = 1; none of the guest callers appears |
| OSLoadContext 0x80372C68 | exception tail 0x80371BE4, __OSUnhandledException 0x80373590/684/708, guest external-interrupt handler 0x803765F4, SelectThread tail 0x803789B8 | (1) `NativeInterrupts::resume` (lr 0x80376210, the return of every delivered interrupt): WP 2,731, FH 18,122, LH 2,729; (2) SelectThread's tail (lr 0x803789BC): 55 / 2,364 / 55 | the guest callers 80371BE4 / 80373590 / 803765F4 never enter |
| OSSwitchFPUContext 0x80373100 | none (exception vector index 7) | the FP-unavailable trap served by NativeBoot (`kFpUnavailableHandler`); lr = the faulting FP instruction (0x8043F5C0, 0x801E2324, ...); WP 251, FH 10,263, LH 249; it calls __OSLoadFPUContext 0x8037292C each time and __OSSaveFPUContext 0x80372A50 when an owner existed (66 / 2,375 / 65) | first FP use after every OSLoadContext into a context that does not own the FPU |
| OSSetCurrentContext 0x80372B80 | 20 sites: 0x80371BE4, 0x80372E58, 0x803746B0, 0x80376CE8, 0x80378324, SelectThread (2), DVD 0x80379938 / 0x80379CA8, VI 0x8037E290, AI 0x80381D00 / 0x803823B0 | SelectThread's tail only (lr 0x803789B4): 55 / 2,364 / 55. The runtime has its own copy for deliveries (`os_set_current_context`, native_interrupts.cpp:193) | |
| OSSaveContext 0x80372BE8 | SelectThread (0x803788B0) only | never | 0 rows in 41 runs |
| OSClearContext 0x80372D78 | 20 sites (same families) | never (native copy `os_clear_context`, native_interrupts.cpp:185) | 0 rows |
| 0x80372D24 | none: the epilogue of OSLoadContext (SRR0 / SRR1 reload, `rfi` at 0x80372D3C) | with OSLoadContext | not a function entry |
| OSInitContext 0x80372D9C | OSCreateThread 0x80378ACC | never (no guest thread is created; the loaders are native) | 0 rows |
| 0x8037292C / 0x80372A50 / 0x80372B78 | OSSwitchFPUContext (both); 0x80373630 (unhandled-exception path) calls the 0x80372B78 stub | via OSSwitchFPUContext only | counts above |
| 0x80376128 | none: the tail of OSDisableInterrupts (rlwinm; blr) | only as the clamp target of OSLoadContext (a context interrupted inside OSDisableInterrupts resumes there) | |
| __OSUnlockAllMutex 0x80376C78 | OSExitThread, OSCancelThread | never | 0 rows |

The "switch" in Final Haunt is main to main: census r3 = r4 = 0x80587450 on every 0x803789A8 row (the switch-callback word
r13+0x2710 is called first, then OSSetCurrentContext + OSLoadContext on the same thread). In Westopolis and Lethal Highway only
the first 55 happen (game ticks 23 M - 60 M), then none.

### Why SelectThread still runs, and why FH differs from WP / LH

`NativeCri::tick_work` calls `request_yield_on_finish()` at every retrace tick ("the server threads preempted the running
thread: it went to its queue's tail", native_cri.cpp:123). When the interrupt is delivered the normal way, `finish()` then calls
`reschedule(yield = true)` -> guest SelectThread(1), which moves main from state 2 into its run-queue slot and takes it back
(its own guest code writes RunQueueBits r13+0x5AE0 and RunQueueHint r13+0x5AE4; guest watch of Final Haunt: the only writers
are `os_init` (once) and SelectThread (4,656 writes); the hint is read by `NativeInterrupts::reschedule`, 8,922 reads).
When the same event runs inside the native Sleep wait, `drive_sync` clears the request (native_interrupts.cpp:660: nothing else
is ready). So Westopolis and Lethal Highway (main sleeps ~2,337 times) stopped taking SelectThread after boot; Final Haunt
(main sleeps 27 times; the game is CPU-bound and the interrupts hit running code) takes it at every field (2,364) and pays the
full OSLoadContext round trip behind each of its 20,486 interrupt returns.

### What is left to cut, in order

1. **The CRI preemption yield.** Behaviour to preserve: the words SelectThread(1) leaves behind for one thread (the run-queue
   head / tail words and entry, Bits, Hint = 0, main's state field 2, the 0x2710 callback call, the OS current-context words).
   With one guest thread that round trip is the identity on the thread set; a native `reschedule` that asserts
   `only_current_thread` can leave exactly those words without entering SelectThread / OSSetCurrentContext, to be validated by
   whole-memory equality on the 38 stages and the FH 2x2 A/B. It removes the 2,364 + 55 SelectThread entries. This is not a
   scheduler reimplementation (no queue is modelled); it needs the interrupt-tail edit, which main routes.
2. **OSLoadContext on interrupt return (`resume`).** The exception-return path (SRR0 clamp, GQR / CR / LR / CTR / XER / MSR
   restore, `rfi`) is the largest remaining entry (18,122 in FH). It belongs to os.interrupts, not os.threads: a native restore
   of the interrupted context from the saved OSContext, same fields, same order, no guest code. Main's decision.
3. **OSSwitchFPUContext (lazy FPU).** Reached by the FP-unavailable trap after every OSLoadContext into a context that is not
   the FPU owner. Going native means serving exception 7 in NativeBoot with the same effect (SRR1[FP], the 0xD8 owner word, FPR
   save into the old owner's context if there is one, load from the new one, the 0x1A2 flag). It cannot vanish with (1) and (2)
   alone, because the interrupt context owns the FPU during a handler; it needs its own owner after (2).
4. **Guest paths with zero entries** (list above) can be pruned from the liveness roots once a loud single-thread guard covers
   their entry: OSCreateThread / OSResumeThread / OSSuspendThread / OSSetThreadPriority owners that make a second thread an
   ExecutionFault (the same check as Sleep's `only_current_thread`).

### HostWork / Idle upcall paths and their wait points

- HostWork is raised only by `NativeCri` (native_cri.cpp:278 a deferred tick found by `run_idle_in_wait`, :319 `idle_point`,
  :350 lock leave with a tick pending). Its handler `host_work` either runs a deferred tick (`tick_work`: the Vsync and svtype 4
  servers through `call_guest`, with the yield request above) or asks for the Idle upcall (`request_upcall(kUpcallIdle)`). In
  41 runs: 0 deliveries and 0 Idle upcalls from interrupts. The Idle server runs only inside the native Sleep wait
  (`run_idle_in_wait` -> `run_upcall`): 2,312 times in WP and LH, 2 in FH.
- Wait points that remain: (a) `NativeCri::idle_point` hooks SelectThread's idle spin at 0x803788FC (wait-points.txt
  `0x80378904 0x803788FC poll`). It is reached only when SelectThread finds no runnable thread after a thread blocked or
  exited; with the native Sleep and no second thread nothing reaches it (HostWork deliveries 0 confirm the loop never fired).
  (b) `NativeCri::note_wait` stops the machine if the guest waits for an event inside a CRI server tick (the tick runs in
  interrupt context). (c) The drain hook (adxm_goto_mwidle_border) stops if it finds the Idle upcall mid-pass. Both stops are
  loud and were never hit.
- Removal: once (1) lands `request_yield_on_finish` has no consumer; HostWork / upcall are then the only users of
  `begin_upcall`'s `reschedule(yield)` tail, and `idle_point` plus the 0x803788FC hook can be deleted with SelectThread.

### Strict oracle (done in parallel; details in the report to main)

Fresh ShadowTheHedgehogReference (gc-removal 4691f55 + 60b476a, Plume ON), `--verify-player-strict`, loader_scope_ranges.txt:
with the S4 defaults WP / FH / LH / cold stg0402 all stop at tick 22,680,000 with 94 matched and 1 mismatched at 0x800450C4
(the file-load wrapper: the interpreter runs the original DVD path, the native side the native loader). With SHADOW_NATIVE_CRI=0
and the three native loaders off, WP reaches 6,118,001 matched (92.9 M instructions) before a mismatch at 0x80038148 (tick
745,875,000, a gameplay-task function, not thread code). The first is an expected divergence of the native loader from the
original guest-thread path; the oracle was not changed. FH / LH were not run with the loaders off.

## S5 re-gate on the merged tree 934191d (2026-11-08)

A = `.build/ab-jobs/s4.exe` (S4, before round 12 and S5), B = `s5.exe` = the merged build of 934191d.
- Unit/contract tests: ctest -R "os_threads|native_|platform_|guest_call|rw_raster|cri_|svm": 82 of 85 pass. The three failures
  are not thread code: native_file_api_tests (the 66 "listed, not installed" CRI rows, pre-existing), platform_runtime_coupling
  (2 unclassified shipping uses of fn_80393634 at native_gx_sync.cpp:373, an owner of another agent), native_host_isa_gate
  (guest_watch.obj thread_local initializer runs before the ISA gate; options_audio_mix_tests carries no gate).
- Westopolis / Final Haunt / Lethal Highway, alternating A1 B1 A2 B2, 4.86 G ticks: PCM hash identical in all four runs of each
  tape (bac0b1f3e4600ebb / a886ae3c0aadfca0 / 71eb526fc185b826); game ticks 4,860,000,000, VI frames 3,597 and render frames
  (7,160 / 7,159 / 7,160) equal A vs B; whole-memory differing blocks: 4 per tape, the same 4 as before (retired OSThread /
  OSContext / idle-context words and dead stacks).
- 38 boot stages at 200,475,000 ticks: the retired classes only (main OSThread + OSContext 7,193 bytes, second OSThread-sized
  object 2,196 bytes, dead main stack 9,228 bytes), plus ~1,000 bytes per stage in 0x805A2F61..0x805B36BF that are zero in A and
  data in B (stg0100: 1,017 bytes, 277 of them where A is nonzero). That region is next to the GX FIFO object (0x805A2EE0) and
  is the `__GXData` image that card-saves' GXInit fix 60b476a writes (its message: "region tables, tex/TLUT region callbacks,
  BP shadows"); A predates that fix. Attributed by region and commit, not proven with a baseline exe built from the merged tree
  without S5; the Westopolis / Final Haunt / Lethal Highway whole-run compares above (same 4 blocks) do not show it as a new
  difference.
- Memory-card save gate (scripted UI create + import/Load): default and `=0` produce the identical created GCI (sha 54482b4dea21)
  and identical imported GCI (a05644a2fee3); the default runs the 3 + 6 save-store jobs. (PCM of these headless UI runs differs
  between modes because `=0` produces none in this script; the audio check is the tape A/B above.)
- Savestate round trip (stg0100, no input, main sleeps natively every field): saved at 150 M ticks, loaded in a new process,
  continued to 400 M and compared with the straight run's 400 M state: identical in every guest region. A wait is atomic with
  respect to a save: the boundary is taken between waits, and `savestate.cpp` refuses while an event runs.
- Strict oracle: see "Strict oracle scope" in NATIVE_THREADS.md.

## Cut #1 (2026-11-08): the preemption yield is skipped when it is the identity

`NativeInterrupts::reschedule` no longer enters guest SelectThread(1) when `yield_is_identity` holds: switches disabled, or the
current context is not the thread's own (the original returns 0 at once), or exactly one guest thread exists (active list head =
tail = current), running (state 2), on no queue, with an empty run-queue entry at its priority, no ready bits, the empty switch
callback 0x80378320 and a context saved by an exception. Then the original's work is a queue insert and removal that restores
every word, an empty callback, and OSSetCurrentContext + OSLoadContext on the same context, which `resume` does anyway. Any
other state still enters the guest SelectThread (no queue is modelled, nothing is maintained). Contract test
`run_yield_identity` (os_threads_contract_tests): the identity, eight negative controls (second thread, ready state, queued,
linked, ready bit, queued entry, other callback, context not saved by an exception) and the two no-switch cases.
Evidence (y1.exe vs s5.exe = the merged tree): census over the 3 tapes - SelectThread, 0x803789A8 and the guest OSSetCurrentContext
have no entries any more (were 55 / 2,364 / 55); OSLoadContext keeps only the `resume` entries (2,786 / 20,486 / 2,784);
alternating 2x2 A/B on all three tapes: PCM hashes identical (same as before), game ticks 4.86 G, VI frames 3,597, render frames
equal, whole-memory differing blocks 0 (the retired OSThread / OSContext words are now equal as well); 38 boot stages at
200,475,000 ticks: 0 differing bytes in every region.

## Cut #2 (2026-11-08): the delivery's return restores the interrupted context in host code

`NativeInterrupts::resume` no longer enters the guest OSLoadContext (0x80372C68) for a context saved by an exception (EXC flag):
`restore_interrupted_context` does its loads in the same order (SRR0 clamp for 0x8037611C < SRR0 < 0x80376128, r0-r2, EXC flag
cleared, r5-r31, GQR1-7, CR / LR / CTR / XER, MSR without EE / RI, SRR0 / SRR1, r4, r3) and `Cpu::return_from_interrupt` does what
the rfi instruction does (MSR from SRR1, interrupt-FPR bookkeeping, resume at SRR0, an "rfi" fallback count at 0x80372D3C). A
context without the EXC flag still goes through the guest function. No queue, no thread switch, no scheduler state here.
Contract test (os_threads_contract_tests `run_restore_context`): against the original OSLoadContext run in the test machine, for
SRR0 outside / inside / at the edges of the clamp window: registers r0-r31, CR / LR / CTR / XER, GQR0-7, MSR, SRR0 / SRR1, PC and
all 0x2C8 context bytes equal; a planted wrong register is seen; a non-EXC context is refused.
Evidence (y2.exe vs y1.exe = cut 1): census - OSLoadContext has no entries in Westopolis / Final Haunt / Lethal Highway (was 2,786 /
20,486 / 2,784); alternating 2x2 A/B on the three tapes: PCM hashes identical (bac0b1f3e4600ebb / a886ae3c0aadfca0 /
71eb526fc185b826), ticks 4.86 G, VI frames 3,597 and render frames equal, whole-memory differing blocks 0; 38 boot stages at
200,475,000 ticks: 0 differing bytes in every region. Full rebuild needed (cpu.h gained `return_from_interrupt`).

## Cut 2b (2026-11-08): the guest SelectThread / OSLoadContext fallbacks are loud stops; their literal roots are gone

After cuts 1 and 2 the runtime had two fallbacks into the guest scheduler (a delivery end that needs SelectThread, a context
without the EXC flag). They are now ExecutionFault stops that print the words SelectThread would read (yield, RunQueueHint,
RunQueueBits, Reschedule count, current thread / context, active list), so `native_interrupts.cpp` names neither 0x803787B0 nor
0x80372C68 (nor the OSLoadContext rfi / OSDisableInterrupts tail: the clamp window is computed from the OSDisableInterrupts
owner address). `extract/liveness-roots.txt` and `-lifted.txt` regenerated: exactly those two roots removed. Gates on y3.exe: PCM
hashes of the three tapes unchanged (bac0b1f3e4600ebb... as in cuts 1 / 2), census - none of the 34 scheduler addresses except
the owners and the lazy-FPU trio is entered; ctest: same 82 / 85 (the same three failures as before).
The two functions do NOT leave the generated set yet: the generator keeps any function reachable from a DOL data word or a
live root, and OSExitThread 0x80378C2C (called by 0x8035A094, a data-pointer entry), OSCancelThread, OSResumeThread, OSSuspendThread,
OSCreateThread, the priority function and __OSReschedule still reach SelectThread / OSLoadContext. They leave when those
become owners that stop on a second thread (they are never called in the 41 runs, but SHADOW_NATIVE_CRI=0 and the three loader
switches `=0` would create real guest threads through them: those diagnostic modes would end in a loud stop). Decision for main.

## Cut 2c (2026-11-08): mode B runs again - the waits and the guest scheduler entry belong to the single-thread configuration

Running the strict oracle (mode B: loader switches 0, SHADOW_NATIVE_CRI=0, SHADOW_ERRATA=0) on this tree failed twice, and both
were mine: S5's Sleep owner stopped at the first sleep with guest threads existing (104 segments matched, 0 mismatched), and
cut 2b's loud stops did the same at the first delivery end that needs SelectThread (9 segments). Main's decision: keep the guest
thread path in the reference build, drop it from the lifted shipping build. Done:
- OSYieldThread / OSSleepThread / OSWakeupThread moved from extract/native-owners.txt to extract/native-owners-r2.txt (bodies
  pruned in the lifted build only). `install_waits` installs nothing when SHADOW_NATIVE_CRI or GEKKO_NATIVE_BG_READ /
  EXPAND_JOBS / ONE is 0 (reference build: the guest functions run); in a lifted build (SHADOW_RENDER_LIFTED) such a switch prints a
  message saying that mode needs the reference build and exits with 2.
- `runtime/src/gekko/native_guest_scheduler.cpp` (listed in SHADOW_LIFTED_UNCOMPILED_SOURCES, so the lifted roots are generated without
  it) gives NativeInterrupts the guest SelectThread / OSLoadContext for a delivery end that needs them. Without it (lifted) those are
  the loud stops of cut 2b. liveness-roots.txt regains 0x803787B0 and 0x80372C68, liveness-roots-lifted.txt does not list them.
- Gates: strict oracle mode B on this tree (Reference exe built from it): Westopolis 10,193,167 matched / 0 mismatched / 155.7 M
  instructions, Final Haunt 11,168,290 / 0 / 164.0 M, Lethal Highway 9,925,688 / 0 / 151.9 M (the numbers main measured on the
  oracle tree). Default configuration: census PCM hashes unchanged on the three tapes (f6f51a27134c8b45 / 198371b2789d1ebc /
  3a21b192f4161852 as before); ctest 82 / 85 with the same three unrelated failures (native_file_api_tests still lists exactly the
  66 CRI rows).
- NOT built or run: a lifted (SHADOW_RENDER_LIFTED) tree. The r2 rows, the start-up refusal and the missing scheduler entry there are
  untested; the generator's pruning of the three waits' bodies and, after the thread-lifecycle owners, of SelectThread / OSLoadContext needs
  that build.

## Cut 3 (2026-11-08): MSR[FP] stays set; the host saves and restores the FPRs around a delivery; exception 7 does not fire

Decision (main, 2026-11-02 directive): not a native copy of the lazy FPU switch. In the single-thread configuration
(`os_threads::single_thread_configuration()`: native CRI and the three native loaders on):
- `os_set_current_context` keeps MSR[FP] (and the context's SRR1[FP]) set, whoever owns the FPU word. No other thread's FPRs exist, so
  nothing is ever switched.
- `Cpu::set_interrupt_fpr_shadow(true)` (shadow_main.cpp): the existing host snapshot of f0-f31 + FPSCR at the first native
  interrupt entry, restored at the rfi that resumes the interrupted pc (`restore_interrupt_fpr`). This is the protection the lazy
  switch gave (the Gun Fortress freeze, cpu.cpp comment: a handler that picked another thread must not have its snapshot stamped on
  the new one, hence "restore only if SRR0 == the interrupted pc", unchanged). It was off by default because the lazy switch made
  it a no-op (1,141,282 restores, 0 changes); now it is the mechanism. The synchronous wait driver keeps its own explicit
  save / zero / restore.
- The guest-thread configuration (reference build, mode B) keeps the OS's lazy switch.
What depended on the old behaviour (read, not assumed): gameplay_scheduler.cpp calls `recomp::fp_unavailable` only when MSR[FP] == 0
(begin() and the checkpoints), so with FP always set those branches never run; the probe only observes. The words the lazy switch
wrote - *0xD8, the OSContext FPR / FPSCR / PS area, the FPSAVED bit - have these readers in the DOL (static scan of lis 0x8000 +
lwz/stw 0xD8): OSSetCurrentContext 0x80372B80, OSClearContext 0x80372D78, 0x80372E58, OSSwitchFPUContext 0x80373100, 0x80373184,
0x80373590 (the unhandled-exception report), 0x80378324; all SDK bodies, none game code, none entered in the 41 runs except
OSSwitchFPUContext itself. Guest watch of Final Haunt (0xD8, main OSContext FPR+FPSCR, PS area, state word): accessors are
0x80373100 / 0x8037292C / 0x80372A50 (the retired bodies), the native interrupt C++, and the rows that call OSSleepThread (the
synchronous driver's native work, attributed to the calling row).
Evidence (y7.exe vs y5.exe): the call census shows OSSwitchFPUContext entered ONCE per run (tick 0: static initialisers run an FP
instruction before the OS bring-up sets MSR[FP], the hardware reset state) instead of 251 / 10,263 / 249; PCM hashes identical on the
three tapes; alternating 2x2 A/B: ticks 4.86 G, VI 3,597, render frames equal. Differing words (WP 54, FH 62, LH ~54) classified:
(1) 0x800000D8 (the FPU owner word: 0 in A at the snapshot, the main context in B); (2) main's OSContext FPR / FPSCR / PS words
0x805874E0.. 0x805875E0.. 0x80587618.. (A has the lazy switch's saved registers, B none), its SRR1 word 0x805875EC (FP bit set in
B) and the state word 0x805875F0 (FPSAVED 1 in A); (3) dead stack words: 0x8060C1AC..0x8060C33C (main stack below the final sp
0x8060C4C8) and 0x805B33DC..0x805B356C (the Vsync server thread's stack, no server running at a boundary): A holds the
FP-unavailable exception frame (return address 0x8043F5C0 = the faulting FP instruction, SRR1 0x1032), B leftover floats of other
calls. 38 boot stages at 200.475 M ticks: the same classes only (5,169 bytes in main's OSContext, 170 in the dead main stack, 0x800000D8
and the dead 0x805B33DC..0x805B356F server-stack words). Savestate round trips taken in Final Haunt at 150 / 233 / 311 / 357 M ticks,
continued to 450 M, identical to the straight run in every region (saves land between native deliveries; none is refused). Strict
oracle mode B (reference build, guest threads, lazy switch kept): Westopolis 10,193,167 / Final Haunt 11,168,290 / Lethal Highway
9,925,688 segments, 0 mismatched, unchanged.

## Thread lifecycle owners (2026-11-08): SelectThread, OSLoadContext, OSSetCurrentContext and OSSaveContext leave the lifted generated set

Lifted-only owners (extract/native-owners-r2.txt; installed in the single-thread configuration, never in the reference build): OSCreateThread
0x80378A44, OSExitThread 0x80378C2C, OSCancelThread 0x80378D10, OSResumeThread 0x80378ECC, OSSuspendThread 0x80379154,
OSSetThreadPriority 0x803794B4 - ExecutionFault naming the function and its r3 / r4; __OSReschedule 0x803789D8 - nothing with
RunQueueHint clear (the original's own no-op), a fault with it set; and the guest exception tails that end in OSLoadContext
(0x80371BE4, 0x80373590, 0x80373684, 0x80373708, 0x803765F4: the default exception report and interrupt-handler tail the native
delivery replaced; entered only by guest exception vectors, census 0 in every run), also faults.
Two more things kept SelectThread alive and are gone from the lifted roots: `NativeCri::idle_point`'s literal 0x803788FC (now
`set_idle_wait_loop`, set by native_guest_scheduler.cpp, so the lifted build has no idle loop to recognise) and the literals of
runtime/tests/compiler_body_tests.cpp (a test of the generated body of that idle spin; it runs against the reference tree, so it is in
SHADOW_LIFTED_UNCOMPILED_SOURCES and `--without` for the lifted roots; the same removed 12 other test-only roots: 0x80080000, 0x80090000,
0x800A0000/10/20, 0x800F0000/1000, 0x803C5B2C, 0x803C5D8C, 0x803C607C).
Result in the lifted build (.build/watch/generated): the shards no longer contain fn_803787B0 (SelectThread), fn_80372C68
(OSLoadContext), fn_80372B80 (OSSetCurrentContext), fn_80372BE8 (OSSaveContext); OSExitThread and __OSReschedule are thunks to their
owners. Gates on that exe (y9.exe vs y7.exe): census PCM hashes unchanged, no stop in any of 81 runs ("still running"); alternating
2x2 A/B on the three tapes: PCM bac0b1f3e4600ebb / a886ae3c0aadfca0 / 71eb526fc185b826, 4.86 G ticks, VI 3,597, render frames equal,
0 differing memory blocks; 38 boot stages: 0 differing bytes. Reference build (non-lifted, guest threads), strict mode B with
SHADOW_ERRATA=0: Westopolis 10,193,167 / Final Haunt 11,168,290 / Lethal Highway 9,925,688 segments, 0 mismatched (unchanged).
ctest 82 / 85 (the same three unrelated failures). Contract test `run_lifecycle` (stops name the call and arguments; __OSReschedule
hint clear / set). OSClearContext 0x80372D78 and OSInitContext 0x80372D9C were not checked individually here; the generator report is the
authority (pruned unreachable 1,386 rows in this build).

## What is left of os.threads / os.core in the lifted build (2026-11-09)

Last cut: the lazy-FPU handler. In the single-thread configuration OSInit leaves MSR[FP] set (native_boot.cpp), so exception 7 never
fires - not even the one at tick 0. The lifted build writes the unhandled-exception owner into exception slot 7 (the reference build keeps
OSSwitchFPUContext via `guest_scheduler::fp_unavailable_handler()`, native_guest_scheduler.cpp), gameplay_scheduler.cpp's literal
0x80373100 is marked value-only (and the one inside a JSON note string reworded). The lifted roots lose 0x80373100, and the shards lose
OSSwitchFPUContext 0x80373100, __OSLoadFPUContext 0x8037292C, __OSSaveFPUContext 0x80372A50 and the 0x80372B78 stub.
Gates (y10.exe vs y9.exe): 79 stage / census runs "still running" (exception 7 would be an unhandled-exception stop); PCM
unchanged on the three tapes; 2x2 A/B: ticks, VI and render frames equal, exactly two differing words per tape - 0x800000D8 (the
FPU owner word: 0, was the main context) and 0x8000301C (exception slot 7: 0x803716BC, was 0x80373100); 38 stages: the same two words
(+ the retired classes of cut 3 are now equal because both sides are single-thread). Strict mode B on the reference exe: Westopolis
10,193,167 / Final Haunt 11,168,290 / Lethal Highway 9,925,688 segments, 0 mismatched. ctest 82 / 85 (same three).

Generated lifted shards, address range 0x80371000-0x8037B000 (the OS, EXI/DVD front, VI): 76 rows, 70 of them native-owner thunks
("the original body is not emitted"); the 6 bodies left are OSRegisterVersion 0x8037177C and the C++ start-up glue 0x803796AC /
0x803796CC (__init_cpp) / 0x803796FC / 0x80379720 / 0x8037976C. No scheduler, context, thread, FPU or interrupt-tail body is left in the
lifted set. What still reaches those 6: __start's static-initialiser loop (OSRegisterVersion is called from the SDK libraries' init
functions as a version string registration). The reference build still has every original body (guest threads, mode B).

## Start-up glue (2026-11-09): OSRegisterVersion, the constructor table walk, the PPCHalt wrapper

- **OSRegisterVersion 0x8037177C** (read to the instruction): `mr r4, r3; crclr cr1eq; addi r3, r13, 0x26DC; bl OSReport` and the
  frame. It writes no global of its own: its whole effect is OSReport("%s" + newline, version) with the format string in the SDA
  (0x805EEBDC). os-other already owned it (os_misc::serve_report_string, merged in round 18); I had written the same owner, found the
  duplicate ("an address is owned twice", native_file_api_tests) and dropped mine. What stays from this work is the contract test
  (os_report_contract_tests): the original run in the interpreter against os-other's owner for 11 version strings (empty, 3, 300, 5000
  bytes, the 254..258 stdout-buffer straddle, tab, `%d%s`) x console ok / failing: same text, no guest memory written, suppressed on a
  UART-less console; a wrong version pointer is seen (negative control). 22 cases + control, passes.
- **Constructor table walk** (0x803796AC wrapper, 0x803796CC `__init_cpp`, return point 0x803796FC): the wrapper was already skipped
  by the native boot; the walker is now native (`NativeBoot::constructors_begin / constructors_next / after_constructor`): the frame
  (LR saved above it, r31 saved in it), r31 = the table 0x804AAC60, per slot LR = 0x803796FC and a jump to the function, `addi r31, 4`
  at the owner entry 0x803796FC, cr0 as `cmplwi r12, 0` left it, the frame popped, return to 0x800032A0. The same constructors in the
  same order with the same link register; the opt-in boot contract's observers (entry 0x803796CC, per-constructor audit by LR 0x803796FC
  and table slot r31) still fire: the run with --native-boot-contract-dir reports constructor_order_complete true. 0x803796CC and 0x803796AC
  are value-only in the contract header and the native boot (liveness-roots: value-only), so the lifted roots lose both.
- **0x80379720**: a frame, `bl PPCHalt`, the frame popped (callers: MSL's abort/exit paths 0x803A3BD4 / 0x803A3C70). Owner = the
  PPCHalt owner's named stop (the original hangs there). **0x8037976C / 0x80379740** are paired-single game math (psq loads / stores of a
  matrix), not OS: left to native-math.
- Gates (y11 vs y10, lifted): PCM unchanged; 2x2 A/B on the three tapes 0 differing memory blocks; 38 stages 0 differing bytes (76 runs
  "still running"); strict mode B (reference exe, SHADOW_ERRATA=0): 10,193,167 / 11,168,290 / 9,925,688 segments, 0 mismatched; ctest
  82 / 85: native_file_api_tests (66 "listed, not installed" CRI rows), platform_runtime_coupling (23 unclassified shipping uses, e.g.
  fn_80393634 at native_gx_sync.cpp:373), native_host_isa_gate (guest_watch thread_local initializer, options_audio_mix_tests carries no gate).
- NOT covered: the opt-in boot contract capture was not compared against the original boot in the reference interpreter again (only
  that its constructor audit reports a complete, ordered table walk); the walker's frame bytes below sp are equal to the previous native
  boot's only by A/B memory compare at 200 M and 4.86 G ticks.

### Static closure of the stop owners (finding, 2026-11-09)

The exception tails (0x80371BE4, 0x80373590, 0x80373684, 0x80373708, 0x803765F4) and __OSReschedule have static closure in the lifted
shards: every caller is a pruned row (0x80371E5C, 0x8037170C, 0x80376938) or an owner thunk (0x803769DC, the tails themselves). The six
thread-lifecycle stop owners do NOT: OSCreateThread <- 0x8035A15C, 0x803C5118; OSExitThread <- 0x8035A094 (no direct caller: a data
pointer); OSCancelThread <- 0x8035A0C8; OSResumeThread <- 0x8035A15C, 0x803C5118, 0x803C5454, 0x803C5534, 0x803C565C; OSSuspendThread <-
0x803C5330, 0x803C55F0; OSSetThreadPriority <- 0x803C5330, 0x803C5534, 0x803C55F0, 0x803C565C. Those callers are emitted bodies (the
game's thread-start helper and CRI's thread routines); NativeCri and the S4 loaders serve them through runtime hooks that can decline, and
only then the stop owner fires. They are assert owners backed by the census of 41 runs, not a closure proof; decision with main.

## Static closure of the thread API and of the exception-7 claim (2026-11-09)

Tool: `tools/platform/closure_proof.py` and `indirect_calls.py` (from agent/axfx-effects 879b043), run with
`--pruned .build/watch/generated/recomp_pruned.csv` of the lifted build (SHADOW_RENDER_LIFTED=ON). It classifies every `bl`, branch,
constant-folded materialisation (lis+addi / ori) and aligned data word of an address as OWNED / LOWERED / PRUNED / LIVE. Its documented
gaps (register-held pointers, std-function slot readers, non-constant arithmetic) are named per row below. "Owned" counts the rows of
native-owners.txt and native-owners-r2.txt; an owner that can DECLINE is not a closure, so every owner used below was checked for decline
paths (the lifted build removes them: see the code changes listed).

### Result: the callers of the six thread functions after this change

| Target | Callers (closure_proof) | Status |
|---|---|---|
| OSSuspendThread 0x80379154 | 0x80394A70 (PRUNED), 0x803C53C8 in server entry 0x803C5330 (OWNED), 0x803C5634 in lock leave 0x803C55F0 (OWNED) | CLOSED by pruning/ownership; no materialisation, no data word |
| OSSetThreadPriority 0x803794B4 | 0x803C53A4 (entry 0x803C5330, OWNED), 0x803C5574 / 0x803C55D0 (drain 0x803C5534, OWNED), 0x803C5640 (lock leave, OWNED), 0x803C56C0 (lock enter 0x803C565C, OWNED) | CLOSED |
| OSExitThread 0x80378C2C | 0x8035A0A0 in the Thread worker entry (PRUNED), 0x80378AD4 materialisation inside OSCreateThread (OWNED) | CLOSED |
| OSCancelThread 0x80378D10 | 0x8035A10C in Thread::~Thread 0x8035A0C8 (OWNED: fault if entered), 0x80376F4C (PRUNED) | CLOSED |
| OSResumeThread 0x80378ECC | 0x8035A1FC in Thread::Thread (OWNED), 0x80394A00 (PRUNED), 0x803C54B8 / 0x803C558C / 0x803C56E0 (entries / drain / lock enter, OWNED), 0x803C52E4 / 0x803C52EC / 0x803C52F4 in 0x803C5118 (LIVE, see below) | CLOSED up to 0x803C5118 |
| OSCreateThread 0x80378A44 | 0x8035A1F4 in Thread::Thread (OWNED), 0x803C5218 / 0x803C5248 / 0x803C5278 / 0x803C52A8 in 0x803C5118 (LIVE, see below) | CLOSED up to 0x803C5118 |

### The one live caller: 0x803C5118 (CRI start-up, called once from 0x801D874C)

It starts with `r31 = 0x805B0000 - 0x35F0 = 0x805ACA10` (constant), and every thread call in it has constant arguments:

| site | r3 (OSThread) | r4 (entry) | NativeCri expectation |
|---|---|---|---|
| 0x803C5218 OSCreateThread | r31 + 0x978 (spinner) | 0x803C5504 | kSpinnerEntry / kSpinnerThread |
| 0x803C5248 OSCreateThread | r31 + 0x348 (vsync) | 0x803C5454 | kVsyncEntry / kVsyncThread |
| 0x803C5278 OSCreateThread | r31 + 0x660 (svtype 4) | 0x803C53FC | kSvtype4Entry / kSvtype4Thread |
| 0x803C52A8 OSCreateThread | r31 + 0x30 (idle) | 0x803C5330 | kIdleEntry / kIdleThread |
| 0x803C52E4 / 0x803C52EC / 0x803C52F4 OSResumeThread | r31 + 0x348 / 0x660 / 0x30 | - | the three resumed threads, after the four creates |

`NativeCri::create_thread` / `resume_thread` serve exactly these (index from `thread_index`), set `replaced_` at the first create, and in
the lifted build have no decline path: an entry outside the four, or a thread outside the four, is a stop ("statically unreachable"); the
lock enter / leave hooks and the drain no longer test `enabled_` / `replaced_` for a decline (SHADOW_NATIVE_CRI=0 is refused at start-up).
So no call reaches the guest OSCreateThread / OSResumeThread bodies from 0x803C5118; the six stop owners are redundant there and fire
only for an internal error. Gaps: argument registers are constants by inspection of this one function (a linear read of 26 instructions,
not a dataflow tool); the SDA/GQR do not matter. The server entry bodies 0x803C5330 / 0x803C53FC / 0x803C5454 / 0x803C5504 are
materialised only at the four sites above and nowhere else (closure_proof: 1 materialisation each, no bl, no b, no data word), they are
passed to OSCreateThread (served natively, so never installed in a thread), and they are lifted-only fault owners.

### The Thread class and the loaders

- Objects: vtable bases 0x805608C8 (Thread), 0x8051E23C (TExpandBG) and 0x8051E2D0 (TBGRead) are materialised only by their constructors
  and destructors (closure_proof: 2 materialisations each: ctor + dtor), so an object of these classes exists only after a constructor ran.
- Constructors: Thread::Thread 0x8035A15C is called only from TExpandBG::TExpandBG 0x80043E78 and TBGRead::TBGRead 0x80045778; those are
  called only from fn_8004C770 (site 0x8004C808) and TBGReadManager::update 0x800453E0 (site 0x800454D8). Both callers are now
  unconditional lifted-only owners (rows in native-owners-r2.txt): the expand-jobs hooks have no `return false` path in their bodies
  (read: native_expand_jobs.cpp lines of the two lambdas), they are installed only in mode 5 (W2), and the lifted build refuses any other mode
  (system.cpp: install() false => message and exit 2; native_expand_jobs.cpp: lifted accepts env 5 only; GEKKO_NATIVE_BG_READ=0 and
  SHADOW_NATIVE_JOBS=1 make install() false). The constructors, destructors and the worker entry are lifted-only fault owners.
- Gaps: a Thread-class object could in principle be created by code that writes the vtable word itself; closure_proof finds the vtable
  bases materialised nowhere else (constant folds only; non-constant arithmetic is the stated limit). Virtual calls through the objects'
  slots (the manager's reaper 0x8035A2DC) need an object, so they cannot run.

### The five exception tails and __OSReschedule: exact closure

- 0x80371BE4: one caller 0x80371E5C (PRUNED, no caller of its own, no data word). 0x80373590: callers 0x8037170C (PRUNED) and 0x803769DC
  (OWNED, MEMIntrruptHandler). 0x80373684 / 0x80373708: no callers. 0x803765F4: caller 0x80376938 (PRUNED, no caller). The rows are
  owned as faults only so that their OSLoadContext calls disappear; none has a LIVE caller.
- `__OSReschedule` 0x803789D8: callers 0x80371DE8 (inside 0x80371BE4, OWNED), 0x803736A8 (0x80373684, OWNED), 0x8037370C (0x80373708, OWNED),
  0x80376908 (0x803765F4, OWNED). Its owner is the original's own nothing when RunQueueHint is clear and a fault when it is set.
- Result: SelectThread 0x803787B0 and OSLoadContext 0x80372C68 are PRUNED, and every one of their callers is OWNED or PRUNED
  (closure_proof output above for 0x803787B0: nine callers, all OWNED; for 0x80372C68: ten callers, nine OWNED and 0x803789B8 PRUNED).

### Exception 7 (the FPU claim): every writer of MSR / SRR1 in the lifted build

Guest instructions that write MSR, SRR0 or SRR1 (mtmsr, mtspr 26 / 27, rfi) in the DOL: 132 sites (129 mtmsr / mtspr SRR0-1 / rfi and 3 `sc`) in 49 functions (scan of the text sections;
`.build/msr_sites.json`). In the lifted build all 49 containing functions are OWNED or PRUNED (`recomp_pruned.csv` and the owner files);
none is LIVE, and none of the 132 site addresses appears in an emitted row of the generated shards (only two addresses show up, both as
the thunk of an owned row: 0x8039F928 inside InitMetroTRK's owner and 0x80370B14, a `sc`, inside PPCHalt's). The owned ones that write MSR:
OSDisableInterrupts / OSEnableInterrupts / OSRestoreInterrupts (0x8037611C / 0x80376130 / 0x80376144: EE only, native), InitMetroTRK and
InitMetroTRK_BBA (refused at start-up, 0x8039F8E0 / 0x8039F978). `sc` sites (0x80370B14, 0x8037255C, 0x8037258C) are inside owned rows.
Native writers of the MSR (`set_msr` in runtime/src and include): EE-only changes (native_ipl 308, native_rw_display 298/311/380/389,
native_ax_voice 491/494, chara_colli_intrinsic 999/1011, native_interrupts 125/624/945, the OSDisable/Enable/Restore owners); FP forced
set (native_boot.cpp 308 in the single-thread configuration, native_interrupts.cpp 212 and 645); FP cleared only in the guest-thread
configuration (native_boot.cpp 311, native_interrupts.cpp 218, both behind `single_thread_configuration()`) and by the exception entry
`Cpu::enter_native_interrupt` (cpu.cpp 1555 / 1579: the handler runs with FP clear until `os_set_current_context(exception context)`
sets it); the interrupted state comes back by SRR1 (`return_from_interrupt` cpu.cpp 1411, the interpreter rfi 3847, `return_from_os_vector`
os_exception_entry.cpp 151, recomp.h 189), whose FP bit is the interrupted MSR's FP, saved from `msr_` at entry (FP set, by induction from
the boot value) and from context SRR1 words that `os_set_current_context` forces to |FP. Contexts are zero-initialised only for state
and mode (os_clear_context), and begin_upcall's `set_msr(context SRR1)` reads the interrupted context's SRR1 (FP set). cpu.cpp 531 (reset) sets FP.
The interpreter's mtmsr (cpu.cpp 4142) is reachable only by a guest mtmsr, and there is none in an emitted row.
Gaps: the proof covers instructions the generator emits and the runtime's own writes; an FP-clearing exception (Program, DSI, alignment)
ends in the unhandled-exception owner (a stop), it does not resume. The single-thread configuration is a start-up property (refused
otherwise in the lifted build), so the invariant "MSR[FP] set whenever guest code runs" holds by construction there. The ONE observed
tick-0 FP-unavailable exception (before OSInit) is gone since OSInit leaves FP set; the census shows none.

### Gates of the closure change (y15 = merged round 18 + this change; 2026-11-09)

- Lifted exe: PCM unchanged on the three tapes (bac0b1f3e4600ebb / a886ae3c0aadfca0 / 71eb526fc185b826), 76 stage / census runs "still
  running", ticks 4.86 G, VI 3,597, render frames equal. The A/B against y11 (before the round 18 merge) shows 36 / 35 / 43 differing
  blocks - these are round 18's own changes (the other agents' owners), not attributable to this change; there is no baseline exe of the
  merged tree without this change (y12 was overwritten), so the memory diff is NOT a gate for this step. What stands in its place: a
  call census on the pre-change lifted exe (y11, three tapes) of every guest body this change turns into a fault owner or a thunk (0x8004C770,
  0x800453E0, 0x803C565C, 0x803C55F0, 0x803C5534, the four server entries, Thread::Thread / ~Thread / worker entry, TExpandBG and TBGRead
  ctor / dtor) - the census observer sits behind the owners, so it counts calls that fell through to the guest body: 0 for all of them
  (only 0x803C5118, itself not an owner, once per run). The refusal of GEKKO_NATIVE_EXPAND_JOBS=3 in the lifted build was run (message,
  exit 2).
- ctest on this tree: 81 / 85: rw_raster_contract_tests (Timeout 600 s at the 10 % cap), platform_runtime_coupling (native_rw_display.cpp
  / native_gx_sync.cpp uses, other agents), native_host_isa_gate (guest_watch TLS initializer, options_audio_mix_tests), and
  native_file_api_reference_tests failed once in the full run (82 s) and passed when run alone (load-dependent; not reproduced).
- Strict mode B could NOT run on the merged tree: all three tapes stop at 12 matched segments with "native RW raster: ClearQuad 0x80499E58:
  this build has no renderer to clear a raster (RasterClearFn not set)" - round 18's rw raster owners need a renderer that the Reference
  build does not have. Mode B ran clean (10,193,167 / 11,168,290 / 9,925,688 segments, 0 mismatched) on this branch before the merge.

## Failing tests, root causes (2026-11-09)

- **native_host_isa_gate** (two causes, the gate is not relaxed): (1) `runtime/src/gekko/guest_watch.cpp` had `thread_local std::vector<uintptr_t> t_open`:
  a thread_local with a constructor gets a dynamic initializer in `.CRT$XDU` that runs from the TLS callback of every thread, before the
  host ISA gate's `.CRT$XIU` initializer (lane A: "thread_local initializer ... runs from a TLS callback, before the host ISA gate"). It is now
  constant-initialised plain storage (16 page slots, abort with a message if one instruction ever needs more). (2) `options_audio_mix_tests` was
  compiled with a hand-set `/arch:AVX2` and did not link `shadow_host_isa` (the model's flags and the CPUID gate objects), so lane E found AVX2
  code and no gate; it now links `shadow_host_isa`. Result: lanes A-E pass for both images, "148 built executable(s) with a non-default host ISA
  model all carry the gate" (ctest native_host_isa_gate Passed, 188 s). Note: `bt9.cmd` does not rebuild the ShadowTheHedgehogReference target; the
  gate checks it too, so build it (refb) before the gate.
- **native_file_api_reference_tests** (failed once in a full run at 82 s, passed alone): the test used a fixed directory
  `%TEMP%/gekko-file-api-reference` and removed it at its end; `native_file_api_tests` the same with `gekko-native-file-api-tests`. Every worktree of the same
  user shares that directory, so a concurrent ctest (another agent, or the same one) deletes the files under a running test. Reproduced on
  demand: two instances started 20 s apart (one capped job): the first exits 0, the second ends with "File API: DVDOpen(): FAILED: the namespace did
  not mount" and exit 1. Fix: `runtime/tests/test_scratch.h` gives a per-process name; both tests use it. Same two instances afterwards: both
  "22333 cases, 0 mismatches", exit 0. The same fixed-name pattern is in asset_namespace_tests, asset_runtime_tests, ax_pb_tests,
  native_bg_read_tests, native_cri_file_device_tests, native_directory_scan_tests, native_rtgcnfs_tests, options_config_tests, options_input_tests,
  options_settings_tests, save_file_tests, save_recognize_tests, save_store_contract_tests, settings_registry_tests: not changed here (each would need its own
  run); they have the same exposure when two ctests of the same user overlap.

### __init_cpp leaves the lifted set (2026-11-09, after the round 20 merge)

The generated lifted shards still held the body of 0x803796CC (`__init_cpp`): the walker's return point 0x803796FC is an owner entry
inside it, and an entry inside an unlisted function keeps the function. It is now a lifted-only fault owner (native-owners-r2.txt: nothing
enters it; its only caller is the wrapper 0x803796AC, called from __start's owner only) and `constructors_begin` no longer calls the contract
observer's hook in the lifted build. OS range 0x80371000-0x8037B000 in the lifted shards after round 20: 75 rows, 73 owner thunks, the two bodies
left before this change were `__init_cpp` and 0x8037976C (paired-single matrix math, native-math's); `__init_cpp` is a thunk now. Census PCM on
the three tapes unchanged (f6f51a27134c8b45 / 198371b2789d1ebc / 3a21b192f4161852), "still running". removal_plan.py was started
(`--release-dir .build/watch --out .build/rp`, lock generator) but the lock was held by another session for the whole wait, so the planner's
os.threads / sdk:unidentified lists are NOT produced here; the numbers above come from the generated shards directly.
