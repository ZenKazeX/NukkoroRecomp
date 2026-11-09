# Native background loading workers: design (2026-11-03, proposal, not implemented)

The design for replacing the game's two background loader threads (TBGRead, TExpandBG) with native host work. It
implements the user's directive "identical behaviour, faster, native x64; it may look different". Implementation
waits for the user's go-ahead.

Evidence:
- the thread map: `STAGE_LOADING.md` 9f and the session note `re_E_threads.md`;
- the loader sequence: `STAGE_LOADING.md`;
- the earlier plan and the failed bridge: `NATIVE_THREADS.md`;
- CRI's server threads, out of scope here: `CRI_SERVERS.md`.

Marks: P = proven (instruction or trace), I = inferred, U = unknown.

## 1. What exists today

**The two worker classes** (P):
- `SonicteamUSA::System::Thread` subclasses: TBGRead (ctor 0x80045778) and TExpandBG (ctor 0x80043E78).
- The Thread base ctor 0x8035A15C:
  - allocates a 0x1000-byte stack on the game heap;
  - registers the worker in the manager's 0x80-slot table (0x80584964);
  - calls OSCreateThread (priority 16, the main thread's) and OSResumeThread.
- **Entry 0x8035A01C:** one step (vtable +0xC), then finish (+0x10), done byte = 1, OSExitThread.
- **The reaper 0x8035A2DC** runs once per frame from 0x801E231C, before the task-tree walk. It deletes finished
  workers; TBGRead's dtor decrements the in-flight count 0x805EF1D8.

**The work:**
- **TBGRead's step:** 0x80045680 → the chunk loop 0x80045150, the whole file in 0x2000-byte reads.
  - Its finish 0x80045890 builds a 0x48-byte **TFileControlAsycManager** Task. Its parent is **the read manager's
    task** (`*0x805EF168`, through fn_80037200) (P, this session).
  - That Task's update 0x8004580C, on the main thread, closes the file and calls the requester's callback.
- **TExpandBG's step:** 0x80043D5C, one PRS decode.
  - It builds a 0x38-byte **TExpandNotice** under group 0 of the task root (static_80571C6C [0], fn_801EB614) (P).
  - The notice's update 0x80043EF8 calls the callback.

**Requests:**
- **TBGReadManager::update 0x800453E0** spawns at most one read per update, while the in-flight count is 0 and the
  pause byte 0x805EF1DC is 0. The file open and the destination allocation happen on the main thread (P).
- **The ONE member extract 0x8004C770** creates a TExpandBG when given a callback (P).

**When a worker runs** (P):
- Equal priority and no time slicing, so it runs only when the main thread yields: RenderStart 0x801D0338, once per
  gameplay frame, suspended during the loading screen.
- Or when the main thread sleeps (the ShowRaster sleep).
- Or when CRI's retrace threads preempt the main thread at each retrace, which requeues it behind the worker.

**This runtime:**
- One host thread runs all guest threads, switching through the translated SelectThread / OSLoadContext.
- **Already native:** PRS (with host decode-ahead threads), file transfers, RtGCNFS, and TBGRead's chunk loop inline
  (`GEKKO_NATIVE_BG_READ=1`, `STAGE_LOADING.md` 9d).
- **Off by default:** the NativeJobs bridge `native_jobs.cpp` (failed its A/B).

**What the game observes of a background job:**

| Observed | By whom |
|---|---|
| The destination bytes | the completion's callback, read after completion (P) |
| The completion Task, its fields and its place in the task tree | task walk and callbacks |
| The done byte | the reaper |
| The in-flight count | TBGReadManager::update |
| The heap blocks and task handles the job's objects take | allocation order: all later addresses and handle ids |

**Not shown to be unobserved (U):**
- the OSThread bytes inside each worker and its 0x1000-byte stack. Erratum E1 (`GAME_ERRATA.md`) was a game read of
  dead stack bytes, so this needs the guest watch (§5, item 2).

## 2. The design

**Principle.** The game's objects and their game logic stay: the request queues, the producers, the worker objects
with their fields, the manager table, the in-flight count, the completion Tasks built by the game's own code, and the
reaper. What goes is only the thread: OSCreateThread / Resume / Yield / Exit, the context switches, and the
yield-driven timing.

1. **Submission.**
   - The Thread base ctor 0x8035A15C becomes a native owner. It keeps the guest-visible work: the stack allocation (the
     same heap call, so heap order is kept), the manager-table registration and the fields.
   - For TBGRead / TExpandBG it creates no OSThread. It submits a native job instead:
     - TBGRead: asset token, file length, destination;
     - TExpandBG: the source bytes, copied at submission, and the destination.
   - The other Thread subclasses: none exist (P, RTTI: only two subclasses).
2. **Host execution.** A small pool of 1–2 host threads (the CPU-heat rule) does the bytes into a **staging buffer**,
   never into guest memory:
   - a read is a copy from the mapped asset file;
   - an expand is the native PRS decoder (`prs.cpp`), sharing the decode-ahead cache.
3. **Publication at one deterministic point.** At the start of the main loop's iteration (the frame driver
   0x801E2210, before the reaper call at 0x801E231C), every job submitted in an earlier iteration is published, in
   submission order. Publishing a job means:
   - copy the staging bytes to the guest destination;
   - write the worker's result fields as the step left them (TBGRead +0x334 / +0x338: the file-size rounding of 9d);
   - run the **game's own finish** (0x80045890 / TExpandBG's) as guest code, which builds the completion Task exactly
     as the original did, with the same parent, ctor and handle allocation;
   - set the done byte.

   The reaper then deletes the worker as today: its dtor decrements the count, and the next read can spawn.
4. **If a job is not finished at its publication point,** the game thread waits for it. The wait is measured and
   reported (p50 / p99 / max). The host work for a whole block archive is under 1 ms
   (`NATIVE_THREADS.md` "Measured"), so waits should be rare and short.
5. **Determinism.** The publication point is a game event (the iteration boundary), not host timing. Two runs publish
   every job in the same iteration, so G0 (two identical runs are byte-identical) must hold.
6. **Savestates.** Before a save, the pool is joined. Finished-but-unpublished jobs are saved with their staging bytes
   (a savestate version bump) and published at the same iteration boundary after a load. No host work is ever in
   flight inside a state.
7. **What leaves the build** (the generator prunes what loses its last caller):
   - the worker entry 0x8035A01C;
   - TBGRead's step 0x80045680 and chunk loop 0x80045150;
   - TExpandBG's step 0x80043D5C;
   - the workers' OSCreateThread / OSResumeThread / OSExitThread / OSYieldThread paths;
   - the RtGCNFS yields for background reads.

   SelectThread, OSLoadContext and the idle loop stay while CRI's four threads exist (`CRI_SERVERS.md` section 5).

## 3. What changes, and what does not

- **Unchanged:** the bytes every completion delivers; the completion Tasks, their parents, ctors and callbacks; the
  order of completions (submission order, one read in flight as before); every game object and gameplay state.
- **Changed, on purpose:** when a completion appears. It moves to the next iteration boundary, instead of after N
  yields or preemptions.
- **Changed as a consequence:**
  - with the completion at a fixed point, the heap blocks and task handles taken after it can land at other
    addresses or ids than in the original. This was already seen with the inline read (`STAGE_LOADING.md` 9e: the
    player's heap address on 20 of 34 stages);
  - the frame counter since boot is lower at gameplay start.
- **Not reproduced:** OSThread bytes and thread-stack contents of the workers, if the guest watch shows no game reader
  (§5, item 2). If it shows one, that becomes a user decision, like erratum E1.

## 4. Why not other designs

- **Keep the threads, schedule them natively:** that replicates the GameCube scheduler, which the user rejected
  (2026-11-02, `NATIVE_THREADS.md`).
- **Publish at the main thread's yield / wait points:** that is the failed NativeJobs bridge. During the loading
  screen the yield is suspended and the main thread hardly waits (9f).
- **Host threads writing guest memory directly:** this saves one copy (about 10 µs per 100 KB) but breaks savestate
  quiescence and the write journal of the verifiers. Staging keeps both.
- **Publish immediately on the game thread (no pool):** simplest and deterministic. But a cold PRS decode can cost
  1.4 ms on the game thread (`NATIVE_THREADS.md` "Measured"). The pool keeps that off the frame.

## 5. To measure or prove before implementing (game runs, one at a time)

1. **A gameplay streaming trace:** block loads after PlayAction at chunk level, on the Westopolis and Final Haunt
   tapes. It shows how often a job completes in gameplay and what its completion currently costs.
2. **A guest watch** (`--watch-guest`) on the worker objects, their OSThread bytes and their freed stacks, over a load
   and a stretch of gameplay: is any game read of those bytes found?
3. **No other priority-16 thread**, and a CRI PCM A/B: the pool must not change audio.
4. **The completion Task's update order** within its parent's children, versus the original (the task-tree census).
5. **A savestate taken mid-load,** resumed and compared with an uninterrupted run.
6. **G0:** two identical runs are byte-identical with the design on.
7. **The stall budget:** game-thread waits at the publication point, p99 / max, on cold and warm loads.

## 5a. W0 results (2026-11-03; tools in `tools/stage_loading/`, runs in `.build/stage_loading/w0/`)

All runs use the original read path (`GEKKO_NATIVE_BG_READ=0`) of the `.build/loadfix` exe.

**1. Gameplay streaming** (`chain_trace.sh` + `chain_report.py`; the Westopolis and Final Haunt tapes, 92 s each, the
per-call census on the whole chain):
- **Before PlayAction:** reads start about 4 frames after creation and take 30–134 frames (the yield cost). Expands
  finish within 0–5 frames.
- **After PlayAction:** 13 reads and 25 expands. **Every job starts and finishes in the frame it is created**, during
  the main thread's present wait.
  - Its completion Task's first update is in the **next frame**.
  - The reaper deletes the worker in that same next frame.
  - No exception in either tape.
- **Consequence:** the rule of section 2 (publish at the start of the next iteration, before the reaper) reproduces
  the original's gameplay frame timing, not only its order. Only loading gets faster.

**3. Threads.** In both tapes OSCreateThread ran only for CRI's four threads and the worker entry 0x8035A01C (32 and 30
workers). No other thread exists.

**4. Completion order.**
- Completion Tasks hang under the read manager's task (`*0x805EF168`) or group 0 (section 1).
- In gameplay, each is created in frame N and first updated in frame N+1 (100 %); in loading, in its ctor's frame.

**2. Stale worker bytes.**
- **The guest watch is inconclusive here:** the freed stacks and OSThreads are reused by gameplay objects (the player
  object itself lands on a freed worker stack on Westopolis, 0x80F15A00), and the watch only aggregates accesses.
- **The poison test** (diagnostic `GEKKO_POISON_WORKERS=<byte>` [`GEKKO_POISON_WORKERS_PARTS=stack|thread`],
  `system.cpp`; checker `poison_check.py`). The Westopolis tape, headless, was saved at 85 s with poison 0xA5 and 0x5A.
  The control (two unpoisoned runs) is byte-identical.
  - **OSThread bytes** (object +0x10, 0x318), poisoned when the object is freed: **nothing flows.** 612 differing
    bytes, all still holding their poison. The native design may drop them.
  - **Stack bytes** (0x1000), poisoned at the Thread dtor's entry: the thread has exited, and the dtor frees the stack
    right after (0x8035A110). **They flow.**
    - **The extent:** 906,519 bytes of heap and image differ beyond the poison at 85 s, mostly heap pointers. Two runs
      then place the same objects at different addresses.
    - **Gameplay unaffected:** the semantic readout is identical (player, entities, set records, mission, RNG).
    - **Timing:** the first flow appears between 26 and 27 s, at the first gameplay block stream.
    - **Allocations:** sizes and callers stay identical to 27.8 s (`poison-alloc`). The difference is where blocks
      land, not what is allocated.
  - **So game code reads stale bytes of a freed loader stack** (the class of erratum E1). The reader is not yet
    located. The suspects are the heap allocator's handling of freed blocks, or an object built over a freed stack that
    reads an uninitialised field.

**5 and 6. Savestate mid-load and G0** (`midload_state.sh`, `state_equal.py`; Westopolis, headless):
- two cold runs are byte-identical at 4.0 s (G0);
- a state saved at 1.8 s, with a background block read in flight, resumes to exactly the uninterrupted run's 4.0 s
  state (every region).

**7. Stall budget** (from `NATIVE_THREADS.md` "Measured"; no new run). The host work per job is a copy from a
RAM-resident file (worst 0.37 ms) or a PRS decode (worst 1.42 ms on a decode-ahead miss). With a host pool and
publication one iteration later, the expected game-thread wait is zero, bounded by about 1.4 ms.

**Also confirmed (from the Sonnet workspace's notes, checked in the DOL).** RenderWare objects freed on unload are
destroyed deferred: TDestroyManager (update 0x8004384C pops one per call through fn_80043768), drained by
fn_80043724 from its dtor and from the frame driver (0x801E2438). A native unload must keep that queue and its pacing.
Which releases enqueue into it, block release included, is I.

**W0b: the reader of the stale stack bytes is found (2026-11-03).**

Method:
- Poisoning only the stacks (0xA5 / 0x5A), the first flow is in frame 1615 (tick 1,090,125,000, 26.92 s; `poison-frames`,
  states saved every frame).
- In the frame-1614 states, the 5,117 bytes still holding each run's poison are exactly the stale bytes
  (`stale_f1614.txt`).
- A guest watch on only those bytes, resumed from the 0xA5 run's frame-1614 state for one frame, found **one guest
  read**: row 0x800B4B68 at 0x80F125D4 (`watch1614/watch.csv`).

**The bug (P, original game):**
- **The reader** is `Player::Shadow::ShadowMotion::update` sub0 0x800B4B3C. When bit 0x18 of the motion's flag set
  (motion +0xA8, fn_8006CEA0) is set, it increments the counter **+0x3EC**:
  `lwz r3,0x3ec(r30); addi r3,r3,1; clrlwi. r0,r3,31; stw r3,0x3ec(r30)`.
- **The counter's parity selects** ShadowMotion vf13 (0x800B54C8: fn_800613A4(…, 3) and fn_80038FBC) or vf14
  (0x800B541C: fn_80014E1C(…, 3) and fn_80038FE8).
- **What the pair does:** each applies an `RpClumpForAllAtomics` callback (0x80039298 / 0x800393A8) to the player
  model, so the pair toggles the model's atomics on and off. It is a visual toggle (I: what sets bit 0x18 is not
  traced).
- **Nothing initialises +0x3EC.**
  - The base ctor `Player::PlayerMotion` 0x800AAE18 memsets +0xD0..+0x3DF and stores +0x3E0, +0x3E8, +0x3F0,
    skipping **+0x3E4 and +0x3EC**.
  - The ShadowMotion ctor 0x800B5844 does not write them either.
  - The only store to +0x3EC is the increment.
  - +0x3E4 is read at 0x800B2BD8..0x800B2D84. It did not leak in this run (I: written before it is read there).
- **The object** (0x868 bytes, `new` at 0x800B14D0) is allocated over a freed loader stack. On Westopolis that is
  0x80F121E8: +0x3EC starts as the worker's leftover pointer 0x810132E0 (even), and by 85 s it has counted 381 events.

**The native read already changes it.** The experimental `GEKKO_NATIVE_BG_READ=1` from `STAGE_LOADING.md` 9d:
ShadowMotion +0x3EC's start value at the first gameplay frames, original vs native read (`ab/`, `ab2p/` states):
- **Same parity on 31 of 40 stages.** Westopolis, every boss but stg0615, and the 2P arenas have the identical value.
- **Opposite parity on 9:** stg0201, 0300, 0302, 0400, 0404, 0501, 0503, 0604, 0615. There the object lands on other
  leftover bytes, so from the first bit-0x18 event on the native run toggles the model the other way.
- **Not seen by the first-frames check:** the semantic readout checks the first frames only, before any event.
- So the native read is **not** "identical apart from timing" on those 9 stages, through this original bug.

**Decision (user, 2026-11-03): option 1, erratum E2** (`GAME_ERRATA.md`).
- **Implementation:** +0x3EC is zeroed at the PlayerMotion constructor's entry (`game_errata.cpp`).
- **Proof:** `game_errata_tests` passes.
- **In game:** stg0201, stg0604 and Westopolis start the counter at 0 with both the original and the native read.
- **What is left:** the stale stack bytes now feed no known reader, so a native worker leaves no difference through
  them. Their only other effect is the heap-address difference of `STAGE_LOADING.md` 9e.
- **The poison test re-run with E2 on** (`w0/poison-E2`, Westopolis tape, 85 s): **0 bytes flow.** 2,440 differing
  bytes, all still holding their own poison, and an identical semantic readout. The whole 906 KB divergence came from
  this one counter. On this tape the stale loader-stack bytes reach no game state any more.
- **Not covered:** other stages and longer play.

**Options that were offered:**
1. **Erratum E2:** zero +0x3EC (and +0x3E4) at the PlayerMotion ctor. The toggle phase is then fixed and independent
   of the loader. It equals Westopolis's original phase (even), and differs from the original on stages whose leftover
   is odd.
2. **Leave it:** the phase follows whatever the heap holds. A native loader cannot reproduce the original residue, so
   9 of 40 stages differ from the original.
3. **Keep the original worker stacks** (the translated worker runs as today) until a decision. That rules out native
   workers.

**What W0 decides for the design:**
- the publication rule holds;
- OSThread bytes can go;
- **the stack bytes are the open question.** A native worker leaves different bytes in the freed stack memory, so the
  heap layout after the first gameplay stream will differ from the original, as the poison runs show. Gameplay was
  unaffected over 85 s in that test. Before W1 / W2:
  - **(a)** locate the reader (W0b: bisect the poison flow to the first object and instruction that reads poisoned
    stack bytes);
  - **(b)** then the user decides, as with E1:
    - keep the original bytes (impossible natively: they are the worker's own execution residue);
    - initialise the reader's field (an erratum fix);
    - or accept a heap-layout difference shown to be gameplay-neutral on long tapes.

## 5b. W1: TExpandBG without a thread (2026-11-03, `GEKKO_NATIVE_EXPAND_JOBS=1`, default off)

**Code:** `runtime/src/gekko/native_expand_jobs.cpp` and its header. Not used together with the NativeJobs bridge.
- **The Thread base ctor 0x8035A15C, only when entered from TExpandBG's ctor (LR 0x80043EA4).** It makes the
  original's writes and guest calls in the original's order:
  - +0x328 vtable, +0 = −1, running 1, +5 / done 0, +0xC, priority 16;
  - the stack through the same allocator 0x803A12A4, and its 0xCD guard (memset 0x10);
  - the manager instance and register, with the slot stored at +0.

  It makes no OSCreateThread or OSResumeThread call. The OSThread's state (+0x2C8) is 0, which the dtor's
  OSIsThreadTerminated reads as terminated, and its first words hold a tag and the creation stamp (game tick,
  sequence). Every other caller (TBGRead) runs the original ctor.
- **At the reaper's entry 0x8035A2DC** (declining), each manager slot holding a tagged TExpandBG (vtable 0x8051E23C)
  that is not done runs in creation order, exactly as the worker entry 0x8035A01C runs it:
  - the step through its vtable: the game's own PRS (a native owner) and TExpandNotice;
  - running = 0, the finish, done = 1;
  - the tag is then cleared.

  The reaper then deletes the worker as before.
- **Pending jobs live only in guest memory** (the slot table and the tags), so savestates need nothing new.
- **A bug found on the way and fixed:** a TBGRead later built over a published TExpandBG's memory kept the stale tag
  (OSCreateThread does not clear that word), so it was run on the game thread. Hence the vtable check and the tag
  clearing.

**Evidence** (`.build/loadfix`, `.build/stage_loading/w1/`):
- **Oracle CTest `native_expand_jobs_tests`.** The native ctor against the original words of 0x8035A15C, entered as
  TExpandBG's ctor enters it, for stack sizes 0 / 0x800 / 0x3000.
  - The same fields, stack guard, r1 / r2 / r13–r31, and guest calls in order (allocator, memset, manager, register).
  - The original creates (entry 0x8035A01C, priority 16, detached) and resumes a thread; the native one does neither,
    and its state is 0.
  - TBGRead's ctor is not served.
  - 0 failures. A negative control (a wrong priority) gives 3 failures.
- **Westopolis cold load:** 6 expands made without a thread and published. OSCreateThread now runs only for CRI's four
  threads and the TBGReads. PlayAction is one frame earlier (2.233 s instead of 2.250 s).
- **G0:** two W1 runs are byte-identical.
- **Mid-load savestate:** a state at 1.8 s resumes to exactly the uninterrupted 4.0 s state.
- **Gameplay timing** (Westopolis tape; `chain_report.py`): 13 of 13 gameplay expands have their notice's first update
  one frame after creation and are reaped in that same frame, like the original (15 of 15). While loading, at most 1
  frame instead of up to 4.
- **Reap before notice update:** while loading, the worker is now reaped before its notice's first update. That is
  safe: the notice stores the worker at +0x34 but its update 0x80043EF8 only clears that word and calls
  callback(size, userdata). The original's gameplay order already reaps first.
- **Tapes:** with a one-frame-shorter load, the Westopolis tape's tick-indexed inputs land one frame differently, and
  the route changes (6 gameplay reads instead of 8). Tape replays therefore cannot judge a timing change; the gameplay
  equality check uses neutral input.
- **Long neutral A/B** (`w1/long`; `ab_first_frame.sh` with `MODEVAR=GEKKO_NATIVE_EXPAND_JOBS`, states at PlayAction
  + 600 / 1800 / 3300 frames, `ab_semantics.py`):
  - on Westopolis and Final Haunt, the 12 pairs (frame offset 4) are identical in player, camera, entities, set records,
    mission and RNG;
  - **the one difference is the player's heap address**, because finished workers and their stacks are freed at other
    points. It is the class already reported for the native read (`STAGE_LOADING.md` 9e); E2 removed the one known
    reader of stale worker-stack bytes.

## 6. Batches

| Batch | Content | Proof |
|---|---|---|
| W0 | the measurements of §5 | traces and a watch report |
| W1 | TExpandBG as a native job (pure PRS: no file, one notice) | oracle CTest against the original step and notice; A/B on all stages; G0 |
| W2 (**done 2026-11-03**, `GEKKO_NATIVE_EXPAND_JOBS=5`; `STREAMING_DIFFERENTIAL_VALIDATION.md` §9) | TBGRead as a native job (the read through the asset namespace, the game's finish at publication) | oracle CTest; first-gameplay semantic A/B on all 59 stages, 2P and Last Story; aligned gameplay tapes |
| W3 | the Thread base ctor owned for both classes, no OSThread; pruning; generator counts | the 5-item report; gate 2 |
| W4 (**done in effect**: savestate v28 keeps queued expand jobs, v29 queued reads; a save with a queued read resumes exactly, `STREAMING_DIFFERENTIAL_VALIDATION.md` §6, §9) | savestates with pending jobs; version bump | mid-load save/resume identical to an uninterrupted run |

Each batch ships off by default behind an environment switch until the user accepts it, like
`GEKKO_NATIVE_BG_READ`.
