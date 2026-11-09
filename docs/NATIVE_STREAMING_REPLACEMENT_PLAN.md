# Native streaming replacement plan (2026-11-03)

This is the plan for the user's directive "complete native streaming replacement" (2026-11-03):
- **The goal:** replace the game's streaming and resource-delivery runtime with independent native C++20, so that the
  production path from a request to published data runs no PowerPC code and no GameCube runtime.
- **The method:** in dependency order, each stage verified against the original before the next.
- **The companion documents:**
  - `STREAMING_DEPENDENCY_REMOVAL_MAP.md` (what exists, what remains);
  - `STREAMING_BEHAVIORAL_CONTRACTS.md` (what must hold);
  - `STREAMING_DIFFERENTIAL_VALIDATION.md` (how it is proven);
  - `STREAMING_PERFORMANCE_COMPARISON.md` (what was measured).

**Status:** the W1 TExpandBG layer (`native_expand_jobs.cpp`) is transitional. It removed the guest thread but still
runs the game's step through guest calls at an HLE hook. This plan replaces it. Phases 1, 2 and Phase 3's expand part
are done behind `GEKKO_NATIVE_EXPAND_JOBS=2` / `=3` / `=4` (§3), and W2 (TBGRead without its worker) behind `=5`
(timing accepted by the user 2026-11-04). Since the C++20 pass (2026-11-03) every project target builds as C++20.
Phase 4 (the ONE layer, `GEKKO_NATIVE_ONE=1`) is done and passed the strict oracle (2026-11-04). Everything is
off by default until 2026-11-08, when the user made the three switches the DEFAULT (S4, docs/NATIVE_THREADS.md; `=0` selects the original). Not started, each awaiting the user: Phase 5 (the rwcore
RenderWare readers), the expand waits (later publication), committing.

## 1. Workspace and boundaries

- **The workspace:** `H:\ShadowTheHedgehogDiskGC_Extract\wt-gc-removal` (the gc-removal worktree): its source,
  `.build/` trees and docs.
- **Read only,** as inputs the repo's own tools take:
  - the dolrecon data `H:\ShadowTheHedgehogDiskGC_Extract\source\.build\dolrecon` (the disassembly, function table and
    symbols);
  - the game data `H:\ShadowTheHedgehogDiskGC_Extract\game`, read only, with copies of any card, SRAM or config.
- **No other path** is written. The Sonnet workspace (`H:\ShadowTheHedgehogDiskGC_function_copies`) is not touched,
  and set-object ranges and draw distance (its L6 area) stay excluded.
- **No commit, merge or deploy** without the user's approval. Each component ships off by default behind an
  environment switch until the user accepts it.
- **CPU:** one game instance at a time; builds at 4 parallel jobs (the CPU-heat rule). The 15 % research budget is the
  Sonnet workspace's and does not apply here; the same restraint does.

## 2. The target architecture

```
  game / middleware request  (translated today; native as each consumer is reconstructed)
        |  a submit call at the request's own site (a native owner of the requesting function)
  StreamScheduler (native)
        |  Request queue: bounded, FIFO per class, the game's ordering rules (C2 "one read in flight" as a policy)
        |  Worker pool: 1-2 std::jthread, std::condition_variable, no busy-waiting
        |    Source:  AssetNamespace / mapped host files (existing, NativeFileApi's backend)
        |    Codec:   prs.cpp (existing host PRS), later the RW stream readers (rwcore) and ADX / Sofdec decoders
        |    Output:  a staging buffer owned by the job (std::vector / pool), never guest memory
  Publisher (native, game thread, one deterministic point per main-loop iteration)
        |  in submission order: copy the staging bytes to the destination; write the result values; deliver the
        |  completion to the consumer (today: the game's own completion Task -> the requester's callback)
  consumer (game logic)
```

**Rules** (from the contracts):
- **Workers never touch guest or gameplay state.** Publication happens only on the game thread, at a game event (G1).
- **Backpressure:** a full queue blocks the submitter only at the publication point, which is measured. The game
  itself never has more than one read and a handful of expands in flight (W0).
- **Cancellation:** at shutdown and savestates (join, then save finished jobs). The game has none (C2).
- **Errors:** a failed host read or decode stops the run with a stated reason. Never a silent partial result.
- **Priorities:** the game's own order. C2 keeps FIFO with one read in flight; a priority scheme is an extension for
  native consumers only.
- **Portable:** C++20 standard threading and the existing storage backends (`shadow/backends`), with no Windows-only
  API in the scheduler (Linux, Android and iOS later).

## 3. Phases (dependency order; entry and exit criteria)

| Phase | Content | Entry | Exit (proof) |
|---|---|---|---|
| 0 Inventory | `STREAMING_DEPENDENCY_REMOVAL_MAP.md`, contracts | — | **done (this set of documents)** |
| 1 Native PRS job | the expand runs on the host codec (`prs.cpp`) on a pool thread; staging buffer; published at the boundary; the game's step 0x80043D5C no longer runs | W1 | oracle: bytes and size equal the game's step on every compressed member the game expands (traced corpus); C3 timing on the tapes; semantic A/B on all stages; G0; savestates |
| 2 Native job lifecycle | no worker object for expands: the ONE extract 0x8004C770 submits a host job directly (no TExpandBG, stack, slot or OSThread); TBGRead the same (W2) | 1 | the 0x8004C770 owner against its original words (CTest: every path: PRS with / without callback, memcpy); chain reports; A/B; the manager's slot table unused by native jobs |
| 3 Scheduler | the StreamScheduler of §2 with bounded queues; the read policy (one in flight, pause byte) as native policy; TBGReadManager::update owned | 2 | contract C2; read order equal to the original's (census); stall budget measured |
| 4 File and stage resources | the ONE layer native (C4); TOneFileAsync loaders submit to the scheduler; 0x800450C4 and the RW file streams over AssetNamespace | 3 | oracles per function (spec_ONE_layer); every archive on the disc parsed equal; strict over the loader scope |
| 5 Geometry, textures, models, motions | RW stream readers (RpWorld, TXD, clump, motion) from `rwcore` (proven natively; to be wired) behind the scheduler; blocks (C5) | 4 + rwcore wiring decision (the user's) | rwcore oracles; the block set per frame equal; collision verify |
| 6 Audio and video | CRI ADX exec / ADXF / stream server, then Sofdec (`CRI_SERVERS.md` order) over the scheduler's source | 3 | PCM byte-identical A/B; no underruns; movie frames equal |
| 7 Memory | native ownership of staging, pools and lifetimes; the console heap only for what game code reads | 4–6 | peak memory measured; no guest-address assumptions in native code |
| 8 End to end | every stage, 2P, Last Story, transitions, restart, teardown | 1–7 | the validation document's full matrix |
| 9 Removal | NativeExpandJobs, the NativeJobs bridge, the inline RtGCNFS read, the owners whose callers are gone | 8 | the generator prunes the bodies; the coverage audit counts drop; gate 2 |

**Phase 1 is done** (2026-11-03, `GEKKO_NATIVE_EXPAND_JOBS=2`, off by default):
- **The change:** the expand runs on the host decoder (`GameMemory::expand_for_native`), and the game's step no longer
  runs.
- **Proof:** byte-identical to the game's step in 51 runs (`STREAMING_DIFFERENTIAL_VALIDATION.md` §5).
- **What remains synchronous:** a decode-ahead miss (34 of 536 jobs, up to 0.81 ms) decodes on the game thread. Moving
  it to a pool thread is Phase 3's.
- **Next:** Phase 2, the job lifecycle without a worker object.

**Phase 2 is done for the expand** (2026-11-03, `GEKKO_NATIVE_EXPAND_JOBS=3`, off by default; TBGRead's W2 is not
started):
- **The change:** the ONE member extract 0x8004C770 is a native owner. A compressed member with a callback becomes a
  host job record `{src, dst, callback, userdata}` in a FIFO queue. There is no `new` 0x348, no TExpandBG, no
  Thread base (stack, OSThread, manager slot), no "Expanding" registration and no reaper delete. At the reaper's entry
  the queue is published in submission order: the host decoder's expand, then the game's TExpandNotice, built as
  0x80043DB0 builds it (its worker field 0: the notice's update only clears that word). The extract's other paths
  (stored member, compressed without a callback, not open, no member) run the same guest functions as the original.
- **What the removed objects did, and why removing them is safe:**
  - The "Expanding" registry (0x8056FFC8, 128 entries) has one reader, 0x8025D488, and it asks for kind 2. TExpandBG
    registers kind 1, so no game code reads its entries.
  - The thread manager's table is read only by the register, the reaper and the dtor.
  - The two calls around the `new` (0x8035A018 / 0x8035A014) are bare `blr`.
- **The savestate:** the queue is host state, saved in a new block (savestate version 28). A state with queued jobs
  refuses to load without mode 3.
- **Proof:** `STREAMING_DIFFERENTIAL_VALIDATION.md` §6. In short:
  - every published job equals mode 2's (tick, size, callback, destination bytes): 509 jobs on 48 runs;
  - the only memory differences are the heap layout and the two tables;
  - a state saved with 2 queued jobs resumes byte-identical;
  - the oracle CTest covers every extract path, with controls.
- **What stays guest code:** the member lookup 0x8004CA50 (the ONE layer, Phase 4), memcpy, the stored member's
  notice 0x8004AD84, the PRS owner for synchronous expands and declined streams, operator new and the notice's ctor
  (the completion is a game Task), and the publication hook at the reaper.

**Phase 3, the expand part, is done** (2026-11-03, `GEKKO_NATIVE_EXPAND_JOBS=4` = mode 3 + a native worker, off by
default):
- **The change:**
  - At the extract, a job whose stream the decode-ahead does not already hold decoded is copied from guest memory
    and handed to one native worker thread (`std::thread`, a FIFO, a condition variable). The worker never touches
    guest memory. The copy is bounded by the member's output size × 9/8 + 0x200.
  - At the same publication point, in the same order, the job is served from:
    1. the decode-ahead;
    2. else the worker's result, waited for only if it is not done, and placed only when every consumed stream
       byte still equals the guest's;
    3. else the game-thread decode, as before.
- **Proof:** whole memory byte-identical to mode 3 in 52 runs; a test with real archive members and a changed-stream
  control (`STREAMING_DIFFERENTIAL_VALIDATION.md` §7).
- **Effect:** in those runs no expand job decoded on the game thread (mode 3: 41). With the decode-ahead on, the
  worst stall at publication went from 0.80 ms to 0.40 ms (`STREAMING_PERFORMANCE_COMPARISON.md` §3c).
- **What is left of Phase 3:**
  - **The waits:** during loading, the publication comes so soon after the extract that a miss still waits for part
    of its decode. Removing that wait means publishing later, a timing change that needs contract C3's evidence first.
  - **The read side:** the scheduler of §2 for reads (C2's one-in-flight policy, TBGReadManager::update). That part
    depended on W2 (TBGRead without its worker), done since 2026-11-03 (mode 5); the read stays on the game thread
    (24–70 µs per read), and a pool thread for it is not built.

**The C++20 pass is done** (2026-11-03, the user: "Turn everything into C++ 20"):
- **The build:** every project target compiles as C++20 (one source change: `path::u8string()`'s type).
- **The expand path in C++:** the extract's member lookup (One::Entry 0x8004CA50), the stored member's memcpy and the
  synchronous PRS expand are no longer guest calls (the guest owners remain the fallback when the memory is not
  direct host memory). The Phase 3 worker is a `std::jthread`.
- **Proof:** the C++17 build against the C++20 build, whole memory identical on 91 run pairs, switches off and mode 4
  (`STREAMING_DIFFERENTIAL_VALIDATION.md` §8).
- **The boundary left on purpose:** game Tasks and their callbacks (the notices, TFileControlAsycManager's Task base),
  operator new, and the ONE layer's other functions (Phase 4).

**W2 is done** (2026-11-03, `GEKKO_NATIVE_EXPAND_JOBS=5`, off by default, with `GEKKO_NATIVE_BG_READ=1`):
- **The change:**
  - TBGReadManager::update is a C++ owner with the game's own policy. A read becomes a host job: no TBGRead, no Thread
    base, no stack, no OSThread, no manager slot.
  - At the reaper's entry, in order with the expand jobs, the inline read fills the destination and the completion
    Task is built.
  - The in-flight count moves as the ctor and dtor moved it.
- **Proof:** `STREAMING_DIFFERENTIAL_VALIDATION.md` §9. Every read and expand delivers the same bytes, and reads keep
  their order, on 50 runs. The first-gameplay readout is equal on 38 stages, and over 55 s on 2. A state with a
  queued read resumes exactly.
- **Not byte-identical to mode 4, by design:** a read completes at the next publication point instead of when the
  guest scheduler would have run its worker (contract C2's rule). PlayAction comes 0.02–0.22 s earlier. The user
  accepted this timing on 2026-11-04 ("Accept, keep going").
- **What is left of Phase 3:**
  - the read runs on the game thread at the publication point, measured at 24–70 µs per read (worst: a 111 KB
    gameplay block on the Westopolis tape; `STREAMING_PERFORMANCE_COMPARISON.md` §3d). **A pool thread is not built:**
    the files are RAM-resident, so the read is one host copy into guest memory, and a pool thread would still need
    that copy from its staging buffer on the game thread. Nothing to win at this cost;
  - the waits of §3's expand part;
  - the open 0x8047AFA0 and the RenderWare heap stay game code (Phase 4).

**Phase 4 batch 4a is done** (2026-11-04, `GEKKO_NATIVE_ONE=1`, off by default; `docs/ONE_LAYER.md`):
- **The change:** the ONE archive layer's queries, parse, adopt, close, ctor and dtor are C++ owners. Their guest
  calls are only the RenderWare free, operator delete and the game's sscanf.
- **The exactness rule it set:** an owner writes its original's stack frame and calls from it. The game copies stack
  leftovers into the heap (fn_8006BEF0), so frames are guest-visible.
- **Proof:** every disc archive parsed both ways; whole memory identical on 86 run pairs
  (`STREAMING_DIFFERENTIAL_VALIDATION.md` §10).
- **Batch 4b, the sync Open, is done** (2026-11-04, the same switch):
  - Open, its chunk-header read and RwStreamRead are C++.
  - Its reads go through `NativeRtGcnFs::read_sync`: RtGCNFS's read with its yields continued at once. That is only
    used while no other thread of the same priority is ready, where the yield returns at once anyway.
  - Whole memory identical on 86 run pairs (`STREAMING_DIFFERENTIAL_VALIDATION.md` §12).
- **Batch 4c, OpenAsync and OnRead, is done** (2026-11-04): the ONE layer's 13 functions are all C++. OnRead enters
  the requester's callback through the dispatcher, as its bctrl does; an owner at the word after the bctrl runs the
  tail. Whole memory is identical on 86 run pairs (`STREAMING_DIFFERENTIAL_VALIDATION.md` §13).
- **Batch 4d, the whole-file sync read 0x800450C4, is done** (2026-11-04): RwFread through read_sync, the rest game code
  from its frame; whole memory identical on 86 run pairs (`STREAMING_DIFFERENTIAL_VALIDATION.md` §14).
- **Batch 4e, the six TOneFileAsync loaders, is done** (2026-11-04): one table-driven owner for the six updates; the
  RenderWare readers stay game code until Phase 5. Whole memory identical on 86 run pairs
  (`STREAMING_DIFFERENTIAL_VALIDATION.md` §15).
- **Phase 4's exit criteria:**
  - oracles per function: done;
  - every disc archive parsed equal: done (1,244);
  - the strict oracle over the loader scope: run (`STREAMING_DIFFERENTIAL_VALIDATION.md` §16). 0 mismatched on
    Westopolis, Final Haunt and a cold stg0402 load (22.4 M segments), the 8 replayable ONE owners owner-verified.
  The I/O owners and the loaders' updates cannot be replayed by it: the retired disc layer, and interrupt timing
  inside the RenderWare readers. They rest on their CTests and the whole-memory sweeps.

What Phase 1 started from:
- the host codec is verified on the whole disc;
- the decode-ahead workers already decode members on host threads;
- W1 already provides the publication point and the savestate-safe pending list.

What remains: the job's bytes come from the host codec on a pool thread instead of from the game's step, and the job
publishes the bytes, the size and the game's notice.

## 4. What this plan does not claim

- **Consumers stay translated:** callbacks into game logic remain guest calls until the game logic itself is native.
  That is tracked per consumer in the removal map (§3 there), not hidden.
- **Scheduling is not exact:** completions are visible earlier while loading. Only the gameplay frame timing and the
  observable state of C2 / C3 are held equal.
- **Heap addresses** of later allocations may differ (G2). They are reported and shown gameplay-neutral. Known readers
  of stale bytes are errata decisions (E1, E2).
