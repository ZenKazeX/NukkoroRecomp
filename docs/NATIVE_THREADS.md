# Native threads (2026-11-02)

**DIRECTION CHANGED (user, 2026-11-02): native execution, not scheduler replication.** The "Design" section below
(host-side 32 priority run queues, per-thread PowerPC register files swapped through leave_to_dispatcher) is
REJECTED: it is the GameCube scheduler under another name. It stays below only as a record of what not to build.
The method now:
1. List the game's actual thread users (Sonicteam::System::Thread 0x8035A..., the semaphore / queue wrappers
   0x8032D..., the stage loaders 0x80043D5C / 0x80045680, CRI's ADX / Sofdec server threads 0x803C5...) and, for each,
   the guarantee it needs: what must have happened before what (ordering), who waits for whom (synchronisation),
   creation / exit (lifecycle), and any real-time requirement (timing).
2. Prove each guarantee with oracle traces of the ORIGINAL (native_threads_tests.cpp; the guest watch for who reads
   what) - traces validate behaviour, they are not a specification to imitate.
3. Implement the guarantees with native C++20 primitives and explicit state ownership. A console algorithm (priority
   preemption, FIFO yield, the idle loop) is kept only where a concrete gameplay dependency on it is proven.
4. Per milestone report: GameCube functions eliminated, remaining reachable lifted functions, remaining compiled SDK
   bodies, remaining guest-context dependencies, native replacements + validation, measured CPU change.

## Step 1: the actual users (in progress, 2026-11-02)

- **The game's own threads are two background loaders.** SonicteamUSA::System::Thread has exactly two subclasses
  (RTTI): TBGRead (vtable 0x8051E2D0, ctor 0x80045778) and TExpandBG (vtable 0x8051E23C, ctor 0x80043E78). The base
  is a cooperative worker: entry 0x8035A01C runs `while (step() && running) OSYieldThread(); running = 0; finish();
  done = 1; OSExitThread` (step = vtable +0xC, finish = +0x10, running byte +4, done byte +6); ctor creates + resumes,
  dtor 0x8035A0C8 cancels. TBGRead's step 0x80045680: when the "load hold" query fn_8032D138 (already a native owner)
  returns a queue it sleeps there; otherwise it reads the next <= 0x2000-byte chunk of its file into its buffer
  (+0x334 / +0x338 / +0x348). A manager (static instance 0x80584964, fn 0x8035A2DC) wakes queue +8 and deletes
  finished workers (done byte) through vtable +8.
  So the guarantee is "a file is streamed / expanded in the background while frames continue, and the frame loop
  sees progress and completion" - native async asset I/O with completion state, not a scheduler. What the main thread
  observes (which fields, when) and whether any gameplay depends on load taking N frames: being established.
- **CRI (ADX / Sofdec): four threads, all frame-driven servers** (read-only survey 2026-11-02; addresses in
  auto_03_803A3920_text.s). Created once by ADXGC init 0x803C5118 (from the game's BGM / voice init 0x801D874C), every
  OSCreateThread detached: Vsync server thread (entry 0x803C5454, prio 12: wait retrace, svtype 2 server 0x803CC3FC,
  resume the idle thread, optional user callback - zero), svtype 4 thread (0x803C53FC, prio 14: wait retrace, server
  0x803CC36C), Idle server thread (0x803C5330, prio 24: server 0x803CC24C - MWSFSVR_IdleThrdProc, i.e. ALL Sofdec
  decoding - then suspends itself), and a prio-8 spinner (0x803C5504) that exists only for CRI's critical section:
  0x803C565C / 0x803C55F0 (lock enter / leave around server-table edits: interrupts off, scheduler off, caller raised
  to prio 1, spinner resumed) and 0x803C5534 (adxm_goto_mwidle_border: run the Idle server NOW and wait for it). No
  alarms, no sleeps, no message queues; svtype 5 "Main" server 0x803CC2DC is called directly from the game frame
  loop (UpdateFrame 0x801D6434 -> 0x801D868C -> 0x803C5710). The runtime only traces these threads today.
  Guarantees: per retrace, Vsync server -> svtype 4 server -> Idle server (the Idle server after the frame's work,
  with a budget); ADX before the AX mix that consumes it; mutual exclusion of server ticks against table edits; a
  synchronous "drain the Idle server" call. All of it maps to direct native calls from the frame loop plus one guard;
  the spinner and the four threads disappear.

## Dynamic evidence: who touches thread state (guest watch, 2026-11-02)

Run: .build/watch exe (sha256 30905ca7..., worktree 35dfa30 + working changes, PDB), Westopolis replay
(stg0100 + westopolis-cpu-spike tape), 30 s game time, windowed; ranges 0x80587038 +0x768 (RunQueue, default thread,
idle context) and 0x805ACA40 +0xC60 (CRI's four OSThreads in its static block 0x805ACA10). 6.13 M watched faults,
0 unattributed (report .build/watch-run2/watch.csv).
Every translated row that read or wrote those bytes is an OS thread / context function: __OSLoadFPUContext 0x8037292C,
__OSSaveFPUContext 0x80372A50, OSSetCurrentContext 0x80372B80, OSSaveContext (SelectThread__sub1) 0x80372BE8,
OSLoadContext 0x80372C68, OSClearContext 0x80372D78, OSSwitchFPUContext 0x80373100, __OSThreadInit, OSInitThreadQueue,
the run-queue helpers 0x8037854C / 0x803785B4 / 0x803785F0, SelectThread, OSCreateThread, OSExitThread,
OSResumeThread, OSSuspendThread, OSSleepThread, OSWakeupThread, OSSetThreadPriority, OSGetThreadPriority - plus
memset 0x8000543C (writes at creation) and fn_803BD934 (a math routine whose first FP instruction triggers the lazy FPU
switch: the FPU context save runs inside its frame - itself a guest-context dependency, not a game read).
Native runtime code: NativeInterrupts::deliver, os_set_current_context, GuestMemory accessors (native owners).
No game, CRI or RenderWare row reads these structures in this run. Not covered yet: the loader threads' OSThreads
(TBGRead / TExpandBG, heap-allocated at run time), other stages, save / load.
Census in the same build (GEKKO_CALL_CENSUS, 30 s): 7 TBGRead + 14 TExpandBG jobs (21 thread entries), 8,101
OSYieldThread, 5,229 OSSleepThread, 1,939 reaper calls (one per frame).
Correction to the loader survey: TBGRead's single step is NOT uninterrupted - it reads through RwFread -> the RtGCNFS
read owner, which yields (OSYieldThread) around every chunk transfer, so a background read is interleaved with the
main thread at chunk granularity. Only its completion is observable (the completion Task); whether its exact
completion point (counted in main-thread yield / wait points) must be kept is decided by an A/B replay, not assumed.

## Direction (2026-11-02): no scheduler at all

With the loaders as native async jobs and CRI's servers as frame-loop calls, the game has ONE thread. Nothing then
calls OSCreateThread; SelectThread / OSLoadContext / the interrupt reschedule tail have nothing to switch to and
become unreachable - removed by the generator's pruning, not re-implemented. Interrupt callbacks (VI retrace, GX
draw-done) stay as game behaviour, run in order at their events, returning to the one thread.
Answered 2026-11-02 (read-only survey, addresses in the dtk asm):
- Each loader thread does exactly ONE step and exits: TBGRead's step reads the whole file (a loop of 0x2000-byte
  freads with no yield inside, 0x80045150), TExpandBG's step runs one whole-buffer LZ decode (fn_80043FE0); the
  Thread entry's yield loop never iterates. Both are created detached at priority 16 (the main thread's), stack
  0x1000, registered in the manager's 0x80-slot table (0x80584964) under OSDisableInterrupts / OSDisableScheduler.
- With no time slicing at equal priority, a worker runs only when main yields - OSYieldThread at 0x801D0338 in
  RenderStart::update (0x801D032C) - or blocks (its retrace wait). Results reach the game as Tasks run on the main
  task tree (TFileControlAsycManager 0x8004580C, TExpandNotice 0x80043EF8); nothing polls thread fields. The reaper
  0x8035A2DC (once per frame from 0x801E231C) deletes finished workers; TBGRead's dtor decrements the in-flight count
  0x805EF1D8, which gates the next read (one background read at a time).
- The "load hold" (fn_8032D138) parks loaders only while the disc-error modal is up; the native owner returns "never".
- OSDisableScheduler / OSEnableScheduler in the game heap (0x80050E28 / 0x80050EF0) and in the thread table
  (0x8035A390) exist only to exclude the workers: with one thread they are trivially satisfied.
- OSGetCurrentThread (0x8032E254, 0x8032D3D4) only distinguishes main from a worker for the disc-error modal.

The required guarantees, stated as rules (not as a scheduler):
1. A background job (TBGRead / TExpandBG step + finish) runs at the main thread's next yield point (0x801D0338) or
   wait point after the job was created, jobs in creation order, each to completion; then its `done` byte is set.
2. Its completion Task is inserted into the main task tree by the job (as the original's step does) - unchanged,
   since the job now runs on the game thread itself.
3. CRI: right after each retrace event, the Vsync server (0x803CC3FC, then the idle-thread resume) and the svtype 4
   server (0x803CC36C) run, in that order; the Idle server (0x803CC24C) runs at the main thread's wait point; the
   "drain the Idle server now" call (0x803C5534) runs it directly; CRI's lock enter / leave (0x803C565C / 0x803C55F0)
   keep only their counters (one thread: no exclusion needed).
4. The main thread's own waits (OSSleepThread on the retrace / draw-done queues) become "advance to the next event
   and deliver it" - the idle loop and the context switch disappear.
These rules reproduce the original's order exactly (they are the original's scheduling outcomes for this program's
actual thread population), so replays must stay byte-identical apart from dead stack bytes and the retired OS words.

**User directive 2026-11-02 ("native job system architectural correction"):** stage A as built
(runtime/src/gekko/native_jobs.cpp) is a TEMPORARY BRIDGE: it still runs the translated worker step / finish on the
game thread, atomically, through guest continuation entries and OSYieldThread. The target is real native
asynchronous loading:
- TBGRead -> a native read request: the file is read from the asset namespace on a host I/O worker (std::jthread /
  job queue), into the destination buffer the game allocated, with no guest execution; TExpandBG -> a native
  decompressor (fn_80043FE0's format reimplemented in C++20, verified bit-exact against the original on every
  compressed entry the game loads) on a host worker.
- Determinism without stalls: the work starts at request time on the host; its completion becomes visible to the
  game only at a deterministic game point (the frame / yield point the bridge uses, or a later fixed one) - if the
  host has not finished by then the game thread waits, and that wait is measured and must stay within budget.
- The game-visible effects the original produced at completion (destination bytes, the completion Task inserted into
  the main task tree, the done byte, the in-flight counter at reap) are produced at that point; the translated step /
  finish, the worker entry, guest OSThread handles, continuation entries and OSYieldThread are then unused and leave
  the build through the generator's pruning.
- Validation: cold / warm stage loads, overlapping jobs, cancellation, save / load (a request in flight at a
  savestate), audio streaming, gameplay tapes; per-job worst case, game-thread p99 / max frame time vs the original.
Tracked bridge dependencies until then: guest PPC execution of step / finish (0x80045680, 0x80043D5C and their
subs), worker entry 0x8035A01C as the job key, OSThread handles as job keys, continuation entries 0x80376218 /
0x8037621C, OSYieldThread 0x80378A08 as the run point.

## Step 1 result (2026-11-02, exe 17602395..., Westopolis tape, 90 s, windowed)

- G0 PASSES: two runs with the bridge off are byte-identical (state report: every 64 KiB block; PCM 11,447,680 B).
- The bridge FAILS: with it on, the game created 1 worker job and it never ran (0 runner passes, still pending at
  exit); the in-flight count stayed 1, so no further load started and the stage never finished loading - the user saw
  the loading screen blinking. 156 memory blocks and the PCM (from byte 217,621) diverged; 4 OS threads created vs
  36. Cause: the bridge ran jobs only at the game thread's OSYieldThread (RenderStart::update); during the stage
  loading screen the game thread WAITS (sleep / idle) instead of passing that point, and the original workers ran
  exactly in those waits. The bridge is now OFF by default (SHADOW_NATIVE_JOBS=1 to diagnose).
- Consequence for the design: a completion must be publishable while the game thread waits (its sleep / idle wait
  points), not only at a once-per-frame yield - the native executor's publication points are the game thread's
  wait points AND its yield point, with the host work already running asynchronously.

Note (2026-11-02): in this runtime every guest thread is a slice of the ONE host game thread, so today - with or
without the bridge - all background-load work (TBGRead's reads through the RtGCNFS owner, TExpandBG's PRS decode
through the native PRS owner) already runs on the host game thread, interleaved at the guest's yield / wait points.
The native async loader is the first version that moves that work off the game thread. Its acceptance therefore
compares game-thread time per frame (p99 / max) against today's interleaved baseline, not against zero.
Native loader design decisions (from the spec): host workers write straight into the guest destination buffer (the
game reads nothing there before the completion Task - spec item 4 - and never cancels), workers are quiesced before
a savestate, and the completion is published on the game thread at a deterministic point - the first game-thread
yield OR wait point after the request (the loading screen waits, it does not yield: the bridge failure) - where the
game thread waits for the host job only if it is not done yet (that wait is the measured stall).

## Measured: how much loading WORK the game thread does today (2026-11-02, Westopolis 90 s, exe with PRS timing)

| work on the game thread | count | total | worst single item |
|---|---|---|---|
| file reads (assets are RAM-resident; NativeFileApi::device_read) | 2,351 | 6.7 ms | 0.37 ms |
| PRS expands served from the decode-ahead workers (a copy; gekko/prs_ahead.h) | 601 | 0.75 ms | 0.02 ms |
| PRS expands decoded on the game thread (ahead-cache misses; sliced at interrupts) | 11 slices | 4.68 ms | 1.42 ms |

Replay byte-identical to the baseline with the timing in. So the loading WORK is already native and, for decoding,
already asynchronous (host decode-ahead workers); ~12 ms in 90 s, worst item 1.42 ms (a decode that missed the
ahead cache). What is still GameCube-shaped is the CONTROL around it - the worker threads, their context switches and
the guest scheduler deciding when they run - plus those cache misses. Consequences:
- The native loader's value is removing the thread / scheduler machinery and the misses, not moving heavy work.
- Its completion must be published at the main thread's wait points, and those waits are today inside the guest
  scheduler (OSSleepThread -> SelectThread -> the idle loop). The loader can only be fully native once the main
  thread's waits are native (stage C) and CRI's threads are frame calls (stage B); until then any publication from a
  wait point goes through the guest scheduler's idle state.
- The decode-ahead misses (diagnostic log in game_memory.cpp, "[prs-ahead miss]"): 11 in one run, 3 in another -
  host-timing dependent (the game's output is identical either way). All "not decoded ahead / refused"; 10 of 11 from
  fn_8004C770's synchronous decode (0x8004C828), 1 from TExpandBG's step (0x80043D8C). Mapped with --asset-trace: two
  were .one members (stg0100_01.one +0x60, stg0100_05.one +0x392C) asked for before a worker had decoded them (the
  expand follows the read within the frame); one had no file read behind it (bytes produced in memory - a member of a
  decoded member - never announced). Fixes, each exact by construction (serve_prs_ahead still compares the bytes):
  wait for a member a worker is already decoding instead of decoding it again; announce members found inside decoded
  output. Gain bounded by ~1.4 ms per miss.

## Stage B result (2026-11-02): the CRI thread bridge failed - see docs/CRI_SERVERS.md section 5

## Plan (2026-11-02, written while the build / spec agent ran)

0. In flight: watch-tree build with the bridge + per-job timing; read-only spec of TBGRead / TExpandBG.
1. Measure (before any redesign): three Westopolis replays on one exe - bridge off, off again (G0: run-to-run
   determinism; if two identical runs differ, no A/B claim is possible until that is fixed), bridge on. Record per-job
   game-thread ms (p50 / p99 / max, slowest kind), frame quantum p99 / max, state-report + PCM diffs. Then the same
   for Final Haunt and Lethal Highway (cold = first stage load after boot, warm = a restart).
2. Verify the spec: re-check its load-bearing claims in the asm (read loop, decoder format, ownership, cancellation,
   completion point) before writing code.
3. Decompressor: ALREADY NATIVE - fn_80043FE0 is Sega PRS and an always-served native owner (game_memory.cpp
   kPrsExpand, extract/native-owners.txt:273, tests in game_memory_owner_tests / native_assets_tests). What remains is
   only to call the host decoder from the async executor instead of from the translated worker step, and to add the
   coverage the owner's tests lack, if any (every compressed entry the game loads, corrupt input).

Spec facts the native loader must keep (read-only survey 2026-11-02, to re-verify in step 2):
- TBGRead request: 6 words (path, dest buffer or 0 = allocate file size rounded to 32 from the RW allocator
  fn_80049BB8(size, 2), size, userdata, callback, extra); queued in 3 FIFO lists (0x805742A0), ONE read in flight
  (lbl_805EF1D8, incremented in the ctor, decremented only when the reaper deletes the worker - a frame or more after
  the callback); a pause flag (lbl_805EF1DC, StageManager ctor 0x80178C84 / stage-init countdown 0x80177540) blocks
  spawns only. File opened on the MAIN thread (0x8047AFA0); a failed open drops the request silently (no callback).
- The read: always from offset 0, the FILE size (not the request size), 0x2000-byte RwFread chunks, a short read does
  not abort, the reported size = file size rounded up to 32 on success. Completion = a 0x48 TFileControlAsycManager
  Task (ctor 0x800458F0, update 0x8004580C on main: close the file, callback(block)); built on the WORKER today - the
  native version must build / publish it on the game thread.
- TExpandBG: src / dst / callback / userdata; PRS decode; a 0x38 TExpandNotice (ctor 0x80043F60, update 0x80043EF8:
  callback(size, userdata)). Without a callback fn_8004C770 decodes synchronously on main; uncompressed entries are a
  memcpy + immediate callback.
- No cancellation exists in the game for either (nothing clears the running byte, deletes a pending worker or a
  queued node). Nothing reads the destination or the result fields before the completion Task runs.
- Boundary: the native loader replaces TBGRead::vf1 0x80045680, sub3 0x80045150, sub4 0x800456EC / 0x80045890,
  TExpandBG::vf1 0x80043D5C, sub3 0x80043DB0 and the Thread layer for these two classes; the producers (TBGReadManager
  ::update 0x800453E0, fn_8004C770), the Task classes and the callbacks are game logic and stay (translated until
  lifted).
4. Native async executor: a bounded host worker pool (1-2 threads, CPU-heat rule), request queue, completion ring;
   determinism: completion published only at a fixed game point; wait time there measured and budgeted; savestate:
   in-flight requests finish (bounded) before the state is written, so states never carry host work; cancellation:
   a cancelled request is dropped at publication, its buffer never written after the game frees it.
5. TBGRead / TExpandBG native: the Thread ctor of these two classes submits a native request (asset id, offset,
   size, destination; or src / dst for expand); at publication the game-visible effects are written (bytes, done
   byte, the completion Task via the game's own Task ctor - game logic, stays translated until lifted). The worker
   entry, step / finish, OSThread handles, continuation entries and the OSYieldThread run point leave the build.
6. Validation matrix: cold / warm loads x 3 stages, overlapping requests, cancellation (stage exit mid-load),
   savestate mid-load, audio streaming during loads, gameplay tapes (state-report + PCM; any difference explained by
   load timing must be shown gameplay-neutral or fixed), frame p99 / max no worse than the original.
7. Report (the five items): guest threads eliminated, worker bodies eliminated (generator counts), remaining
   translated functions, remaining GameCube OS dependencies (reachability graph), measured CPU / frame time.
8. Then CRI's four threads (stage B), the main thread's waits (C), os.threads pruning (D).
Side tracks: RtGCNFS step A/B replay (owed); launch the newest lifted build for the user after step 1;
dolrecon_lift_tests [fm] stderr flood (needs the user's go-ahead to run); regenerate pal-v30 callgraph outputs with the
fixed tools; B1 denormal fix (user decision); the research plan's 32-queue scheduler conflicts with the user's rule.

Implementation stages (each with its own A/B replay before the next):
A. Loader jobs native: the Thread ctor (0x8035A15C) registers a native job instead of OSCreateThread / Resume; the
   job runner executes at rule-1 points; the dtor (0x8035A0C8) / OSIsThreadTerminated (0x80378498) see "terminated".
B. CRI servers native at their events (rule 3); ADXGC init (0x803C5118) creates no threads.
C. Main-thread waits native (rule 4); the interrupt tail no longer reschedules (one thread).
D. Prune: the generator then compiles no os.threads body; measure the count, the CPU change, and run gate 2.

Note on docs/native_os_research/00_MASTER_INTEGRATION_PLAN.md (another session's research, merged 2026-11-02): its
section 3 target (one 32-queue priority run queue with cooperative switching) is the scheduler replication the user
rejected the same day; and its fact 8 ("only SDK code reads OSThread fields", tagged VERIFIED) cannot be established
by an offset scan - 269 non-SDK accesses share those offsets (see "Open questions" below). Its other facts (all
OSCreateThread calls detached; the decrementer / alarm path at cpu.cpp:1359; the idle spin is a wait point) are
consistent with what this file found.

Original title: Native threads: design (2026-11-02, not implemented yet)

Goal: retire the GameCube OS thread service (os.threads, 26 live SDK bodies, 17 frontier rows) and the four remaining
native -> SDK scheduler call sites (native_interrupts.cpp finish / resume -> SelectThread 0x803787B0 /
OSLoadContext 0x80372C68; native_video.cpp and native_gx_sync.cpp -> OSWakeupThread 0x803793B0; plus
native_rtgcnfs.cpp's OSYieldThread 0x80378A08) with a host-native scheduler. Not a C++ copy of SelectThread: the
GameCube mechanism (OSContext save areas in guest memory, setjmp-style OSSaveContext, lazy FPU ownership through
low-memory words, the idle loop polling RunQueueBits with interrupts enabled) is what goes away.

## What the game and the middleware actually use (the contract)

Measured from the DOL (scheduler survey, 2026-11-02): only SDK code reads or writes OSThread fields 0x2C8-0x304 and
the OSContext inside it. Game code (Sonicteam::System::Thread 0x8035A..., semaphore / queue wrappers 0x8032D...,
stage loaders 0x80043D5C / 0x80045680, 0x80050E28 / 0x80050EF0) and CRI (0x803C5...) only CALL the API:

| API | address | entered by |
|---|---|---|
| OSInitThreadQueue | 0x8037847C | game |
| OSGetCurrentThread | 0x8037848C | CRI |
| OSIsThreadTerminated (0x2C8 state 0 / 8) | 0x80378498 | game |
| OSDisableScheduler / OSEnableScheduler | 0x803784CC / 0x8037850C | game |
| OSYieldThread | 0x80378A08 | game, runtime (RtGCNFS) |
| OSCreateThread | 0x80378A44 | game, CRI |
| OSExitThread | 0x80378C2C | game |
| OSCancelThread | 0x80378D10 | game |
| OSResumeThread | 0x80378ECC | game, CRI |
| OSSuspendThread | 0x80379154 | CRI |
| OSSleepThread | 0x803792C4 | game |
| OSWakeupThread | 0x803793B0 | game, runtime (VI retrace, GX finish) |
| OSSetThreadPriority | 0x803794B4 | CRI |
| OSGetThreadPriority | 0x80379574 | CRI |
| SelectThread / idle | 0x803787B0 / 0x803788E8 | runtime literals only |

Observable contract per call: return values (thread handles = the caller's OSThread address, priorities, suspend
counts, "terminated"), which thread runs next (priority 0 highest, FIFO within a priority, the running thread keeps
the CPU unless outranked or it yields / sleeps), sleeping threads wake on OSWakeupThread of their queue, the
scheduler-disable count, and the order in which callbacks / interrupts interleave with thread switches. The guest
memory layout of OSThread is NOT part of it (no non-SDK reader) - gate 2 (retired_readers.py + the dynamic watch)
must confirm that before the guest fields stop being written.

## Design (REJECTED 2026-11-02 - scheduler replication; kept as a record)

- **Thread table, host-side**: one record per OSThread handle (the guest address is only the key the game passes):
  state, base / effective priority, suspend count, the queue it sleeps on, the join queue, the owned-mutex list (the
  image has no OSLockMutex: only the release-on-exit walk 0x80376C78 needs it), stack bounds for diagnostics.
- **Run queues, host-side**: 32 FIFO lists + a 32-bit ready mask; selection = countl_zero of the mask (the same
  priority rule, no guest RunQueue at 0x80587038).
- **Execution context, host-side**: a saved register file per thread (GPR, FPR both halves, CR, LR, CTR, XER, GQR,
  PC, the MSR bits the code observes). A switch copies the outgoing Cpu registers into its record and the incoming
  record into the Cpu, then leaves to the dispatcher at the incoming PC - the mechanism translated code already uses
  for every non-local transfer (Cpu::leave_to_dispatcher). FPRs are always switched: no lazy FPU ownership, no
  OSContext at 0x800000D4 / D8 for threads.
- **Idle**: no polling loop. When no thread is ready, the game thread waits on the host for the next scheduled event
  (the game clock's next interrupt source) and delivers it - the idle context at RunQueue+0x730 disappears.
- **Interrupt delivery** (native_interrupts.cpp): keeps delivering the guest's registered handlers / callbacks
  (those are game behaviour) but its tail calls the native scheduler's reschedule instead of SelectThread /
  OSLoadContext, and the interrupted thread's registers are saved into its host record, not an OSContext on the stack.
- **Savestates**: the thread table, run queues and register files are saved with the scheduler (savestate version
  bump); old states load only through a converter that reads the guest structures once.

## Validation (in order; nothing ships before all pass)

1. Contract test against the original in the reference interpreter: scripted sequences over the 15 APIs (create,
   resume, priorities, yield, sleep / wakeup, suspend counts, exit / cancel / join, scheduler disable) on both
   machines; compare return values, the order threads run in (each thread appends its id to a trace buffer), and the
   game-visible memory (thread stacks' contents, caller buffers). Not compared: OSThread internals (retired state).
2. Gate 2: retired_readers.py on all 26 bodies (static) + the dynamic watch on the Westopolis and Final Haunt replays
   (no surviving reader of any retired byte).
3. Replays: Westopolis + Final Haunt + Lethal Highway tapes reach the same game ticks with byte-identical
   --state-report outside the retired ranges and byte-identical --audio-dump (CRI audio threads run under it).
4. Ratchets: platform-SDK bodies 1,057 / 1,063 -> minus the freed rows (what_if.py predicts; the generator decides),
   native -> SDK sites 6 -> 1 (only native_card's __CARDVerify left).

## Open questions (to measure before coding)

- How many threads exist per stage and which (game main, Sonicteam threads, CRI ADX / Sofdec server threads)?
- Does any translated game or middleware code depend on a thread's stack being in a particular range (alloca checks,
  stack-end guards)? The host design keeps the guest stacks the game allocated, so it should not.
- Which code outside os.threads reads OSThread fields? NOT ESTABLISHED. An offset-only scan (lwz / stw / lhz / sth
  at 0x2C8-0x308 off any base but r1 / r2) finds 477 accesses: 208 in the SDK and 269 in game, CRI (0x803B-0x803F)
  and RenderWare code - offsets that other structures share, so the scan can neither prove nor refute a reader (an
  earlier note here that restricted the scan to the SDK range was wrong to conclude "private"). The proof needs typed
  receivers (only bases proven to hold an OSThread pointer) or the dynamic watch (gate 2's dynamic half: every read
  of a live OSThread object's bytes, attributed to its exact guest instruction). Until then the native scheduler
  keeps writing the documented guest fields it retires only after that proof.

## Threading audit (2026-11-06): measured, nothing changed

Question: does the port still reproduce GameCube scheduling where host cores would help? Method: three read-only
audits (guest threads and wait points; host threads and synchronisation; loading and streaming) plus traced replays
of the current build (.build/threads/: report.py, handoff.py). Westopolis (westopolis-cpu-spike, stg0100, 75 s) and
Final Haunt (final-haunt, stg0604, 120 s), 1920x1080 borderless, V-Sync on, GEKKO_PRESENT_TRACE thread sampler.

| | Westopolis | Final Haunt |
|---|---|---|
| game thread, % of a core / context switches per s | 14.8 / 305 | 15.9 / 296 |
| main-ui (render + present), % / cs per s | 5.3 / 177 | 3.4 / 165 |
| input-poll (1 kHz polling), % / cs per s | 1.3 / 800 | 0.7 / 776 |
| audio-wasapi, % / cs per s | 0.1 / 100 | 0.3 / 100 |
| 15 other threads (prs-ahead, host jobs, resource worker, D3D) | ~0 | 0.2 |
| game thread busy per 60 Hz frame p50 / p99 / max ms | 2.48 / 4.47 / 23.37 | 2.60 / 5.50 / 22.53 |
| frames over 16.7 ms of game-thread work | 1 | 1 |
| [host-stall] reports | 0 | 0 |
| PRS decodes left on the game thread (misses) | 9 slices, 4.42 ms total | 11 slices, 3.25 ms total |

Findings:
- Guest waits (idle loops, DVD / ARAM / DSP / GX completions, OSSleepThread) are zero host time: Cpu::idle_wait and
  poll points advance the game clock to the next event. The only real sleep is the 300 Hz host pacer (the ~300 context
  switches per second of the game thread). The guest scheduler's own share of the ~2.5 ms frame is unmeasured, but the
  whole frame is 15 % of one core.
- The only overrun in both runs is game frame 80 (~1.3 s), the stage-start hand-off right after the 606 ms load: one
  frame pushed ~6 ms late and recovered by the next (22.98 / 10.36 ms intervals). Nothing in gameplay comes near the
  16.7 ms slot.
- Host threads: bounded (about 16 always-on, mostly idle, on 32 logical processors), no affinity pinning, no long lock
  holds on the game thread found; spins only in the V-Sync-off precise frame wake and the opt-in JIT pacer.
- GameCube scheduling still costs stage-load LATENCY in game time (loader-thread yields, ~one retrace each), not host
  CPU. The native replacements exist behind off-by-default switches (GEKKO_NATIVE_BG_READ, GEKKO_NATIVE_EXPAND_JOBS,
  SHADOW_NATIVE_CRI). They change game-visible timing, so tape replays cannot validate them; enabling them needs manual
  play tests and the user's decision.

Not changed, by the rule "fix confirmed bottlenecks": no threading code. Candidates if the stage-start frame matters:
PrsAhead::announce's archive table parse on the game thread (~2.5 ms of that frame per FRAME_LATENCY_ROOT_CAUSE_ANALYSIS
frame 79; would not bring it under 16.7 ms alone), closing the remaining decode-ahead misses. Not covered: stage clear ->
next stage transitions and death/checkpoint reloads (no tape crosses them).


## S4 (2026-11-08): the native async loaders are the default (user decision "Yes, make default")

`GEKKO_NATIVE_BG_READ`, `GEKKO_NATIVE_EXPAND_JOBS` (mode 5, W2) and `GEKKO_NATIVE_ONE` are ON when unset. `=0` selects
the original code for each (TBGRead thread, TExpandBG thread, the ONE layer's guest code); this is the A side of every
comparison. This replaces the "off-by-default" sentences above and in NATIVE_STREAMING_REPLACEMENT_PLAN.md.

- **Guest loader threads removed** (call census of Create 0x8035A1F8 / Resume 0x8035A200 / Exit 0x8035A0A4 /
  IsTerminated 0x8035A100, stage tapes, 90 s): Westopolis 32 -> 0, Final Haunt 30 -> 0, Lethal Highway 45 -> 0.
  Only the main thread is alive after boot.
- **Load time, cold boot to the end of the loading screen** (`tools/stage_loading/measure_loads.sh`, windowed, exe
  s4, three alternating repeats each, deterministic in game time): Westopolis 0.978 s -> 0.248 s game time (wall
  0.99-1.08 -> 0.26-0.27), Final Haunt 1.378 -> 0.250 (wall 1.39-1.40 -> 0.27), Lethal Highway 1.533 -> 0.243 (wall
  1.55-1.56 -> 0.26). Presents inside the load 60/84/94 -> 16/17/16. Walls are under the 10 % CPU cap. Game time is identical in all
  three repeats (the load is game-clock driven); the OS file cache state (repeat 1 cold, 2-3 warm) moved the wall time
  by under 0.02 s.
- **Gameplay, Westopolis tape 90 s headless**: wall 12,12,13,12 s (switches =0) vs 10,11,11,11 s (default), first pair
  discarded, 4 each. Game ticks 3,645,000,000, VI frames 2,697 and render frames 5,360 in both. Headless under the cap;
  not a GPU or frame-time claim.
- **Tapes diverge** (the load finishes earlier): Westopolis PCM 015445f28cd10698 (=0) -> 46a70a1051347069 (default,
  5 of 5 runs identical). 150 memory blocks differ (heap layout and the moved timeline). The audio differs from sample
  ~62,000 on (same length); nobody listened to it. The default value is the new canonical.
- Gates: oracle CTests native_bg_read_tests, native_expand_jobs_tests, native_one_tests, native_rtgcnfs_tests pass
  against the original words (they set their own switches); os_threads_contract_tests passes (with the new hook-chain
  checks). Not run in this step: the strict in-game oracle, the all-stage first-gameplay A/B, save/load and card
  checks, a manual load check (the user does it).
- Consequence for os.threads: with nothing but the main thread alive, OSSleepThread / OSWakeupThread / OSYieldThread /
  VIWaitForRetrace can become the single-wait-slot primitive of the S3 design.


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

### Strict oracle scope (2026-11-08, decided by main)

`ShadowTheHedgehogReference --verify-player-strict` (scope test_data/stage_loading/loader_scope_ranges.txt) is NOT a gate for
the S4 defaults. With the native loaders on, WP / FH / LH / cold stg0402 all stop at tick 22,680,000 (94 segments matched, 1
mismatched at 0x800450C4, the file-load wrapper): the interpreter replays the original guest loader path that S4 removes. This
is a stated scope limit of the oracle (the same class as the I/O owners and loader updates it cannot replay), not a pass. The
loader owners are covered by their own oracle CTests.

Mode B is the strict gate for everything else: SHADOW_ERRATA=0 GEKKO_NATIVE_BG_READ=0 GEKKO_NATIVE_EXPAND_JOBS=0
GEKKO_NATIVE_ONE=0 SHADOW_NATIVE_CRI=0, same exe and arguments. SHADOW_ERRATA=0 is required: errata E3 (the corridor-lights fix) hooks
entry 0x80038148 and changes the guest pattern clocks in the native pass, which the interpreter replay of the original cannot match
(the mismatch at 0x80038148 in the first mode-B runs, traced by main). With errata off, loader switches off, CRI off, 30 s tapes:
Westopolis 10,193,167 segments, Final Haunt 11,168,290, Lethal Highway 9,925,688, 0 mismatches, ~472 M instructions verified
(oracle exe from gc-removal 4691f55 + 60b476a). The same on the cri-threads tree (S5 + cuts 1 / 2 / 2b, Reference exe built from
it): 10,193,167 / 11,168,290 / 9,925,688 segments, 0 mismatched, 155.7 M / 164.0 M / 151.9 M instructions (277 / 381 / 421 skipped
for hardware access).
Mode B needs the guest threads, so on a tree with the S5 waits it can only run in the reference (non-lifted) build: the waits
(OSYieldThread / OSSleepThread / OSWakeupThread) are listed in extract/native-owners-r2.txt (bodies pruned in the lifted build only)
and `install_waits` does not install them when SHADOW_NATIVE_CRI or one of the three loader switches is 0; the delivery's end
reaches the guest SelectThread / OSLoadContext through runtime/src/gekko/native_guest_scheduler.cpp, which the lifted build does not
compile (there a delivery end that needs the scheduler is an ExecutionFault, and a =0 switch is refused at start-up). Before this
split the S5 owners stopped mode B at its first sleep with guest threads existing ("OSSleepThread with a second guest thread",
104 segments matched, 0 mismatched), and cut 2b's stops did the same at the first reschedule (9 segments). The lifted-build parts
(the r2 rows, the start-up refusal, the missing scheduler entry) have NOT been built or run: no lifted tree was built here.
