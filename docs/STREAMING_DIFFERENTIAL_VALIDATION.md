# Streaming differential validation (2026-11-03)

How every native streaming component is proven against the original, the tools that already exist, and what each
phase of `NATIVE_STREAMING_REPLACEMENT_PLAN.md` must pass. **The rule:** a component replaces the original only after
an independent check shows the behaviour its contract (`STREAMING_BEHAVIORAL_CONTRACTS.md`) demands. A loading
screen that finishes is never the evidence.

## 1. The checks and their tools

| Check | What it proves | Tool | Used for |
|---|---|---|---|
| Oracle CTest | native function = original words, in the reference interpreter: results, registers, every written byte, guest calls in order | per component (`native_rtgcnfs_tests`, `native_bg_read_tests`, `native_expand_jobs_tests`, `game_errata_tests`, the CRI tests) | every owner |
| Whole-disc corpus | a native codec / parser on every file of its kind against the original's results | `asset_import_tests` (PRS, ONE, TXD), a new corpus per phase | codecs, parsers |
| Negative controls | the check catches a wrong implementation | deliberate mutations, run and reverted (each CTest has some) | every check |
| Strict in-game oracle | translated code around the change still equals the original words | `--verify-player-strict` with a scope file (`test_data/stage_loading/loader_scope_ranges.txt`) | loader-scope changes |
| First-gameplay semantic A/B | game state equal at equal game frames after PlayAction, original vs native | `tools/stage_loading/ab_first_frame.sh` + `ab_semantics.py` (pairs by the game's frame counter; `MODEVAR`, `KS`, `BOOT`) | timing changes |
| Long neutral A/B | the same over 10–55 s of play | the same tools, KS = 600..3304 | timing changes |
| Job-chain timing | creation, start, finish, completion update and reap per job, in frames | `chain_trace.sh` + `chain_report.py` | schedulers |
| Stale-byte poison | whether game code reads leftover bytes of what changed | `GEKKO_POISON_WORKERS` (`system.cpp`) + `poison_check.py`; frame bisection + a guest watch from a saved state | lifecycle changes (found E2) |
| G0 determinism | two identical runs are byte-identical | `midload_state.sh` (A1 / A2) + `state_equal.py` | every change |
| Savestate mid-load | a state taken during a load resumes exactly | `midload_state.sh` (B → C) | schedulers, lifecycles |
| Load-time sweep | loading-screen time per stage, wall and game | `measure_loads.sh` + `load_times.py` (`BOOT=story` / `2p` entries) | performance |
| PCM A/B | audio byte-identical, underruns 0 | the A/B harness (`.build/ab-jobs/run.sh`), the exit report | audio |

## 2. What is distinguished

- **Algorithmic equivalence:** bytes, values and result fields equal the original's for every input tested. Claimed
  only with a corpus or an oracle, and negative controls.
- **Scheduling equivalence:** completion at the same frames. Claimed only where measured (W1: gameplay frames equal).
  Elsewhere the contract's rule is stated and the observable state is compared.
- **Heap layout:** reported separately (contracts G2). Never folded into "identical".

## 3. Gates per phase

| Phase | Gates |
|---|---|
| 1 Native PRS job | corpus: every compressed member the game expands (from a trace of 0x8004C770 / TExpandBG on all 59 stages + 2P + Last Story), host codec = the step's bytes and size; controls; chain timing on 2 tapes; semantic A/B, all stages; G0; mid-load savestate |
| 2 Job lifecycle | 0x8004C770 owner oracle (every path); the manager's table unused by native jobs (watch); poison test (no new stale readers); the A/B again. For the expand: done except the poison test (§6), which has no object left to poison: the 0x348 worker and its stack are no longer allocated |
| 3 Scheduler | read order = the original's (census of spawn order on all stages); C2 timing in gameplay; stall p50 / p99 / max at publication; savestate with a job in flight |
| 4 ONE layer / file streams | per-function oracles (13 ONE functions); every archive on the disc: buffer bytes after the parse equal; strict over the loader scope |
| 5 RW readers / blocks | rwcore oracles (existing); the block set per frame equal on tapes crossing boundaries (re-recorded); collision verify (`GEKKO_VERIFY_WORLD_COLLISION`) |
| 6 Audio / video | PCM byte-identical; underruns 0; movie frames equal (captures) |
| 8 End to end | all of the above on all bootable stages, 2P (`--boot-2p`), Last Story (`--boot-last-story`), restart / death (the mash route), stage transitions |

## 4. Results so far

| Component | Checks passed | Not yet |
|---|---|---|
| PRS codec (`prs.cpp`) | whole-disc corpus (`asset_import_tests`) | — |
| Inline background read (9d) | oracle CTest (207 calls, 3 controls), strict loader scope (10.4 M segments), semantic A/B on 38 + 20 stages, same RtGCNFS work counts | — |
| W1 TExpandBG without a thread | oracle CTest (ctor, 1 control), G0, mid-load savestate, chain timing (gameplay equal), 55 s neutral A/B on 2 stages | the all-stage A/B |
| Erratum E2 | oracle CTest, in-game counter = 0 both ways, poison test: 0 bytes flow | other stages' poison tests |
| Phase 1: the native expand (`GEKKO_NATIVE_EXPAND_JOBS=2`) | byte-for-byte against the game's own step (mode 1), see §5 | — |
| Phase 3 (expand part): misses on a native worker (`GEKKO_NATIVE_EXPAND_JOBS=4`) | CTest with real members, a changed-stream control, 1 mutation; whole memory = mode 3 on 52 runs; queued-job savestate; see §7 | the read side (needs W2) |
| Phase 2: no worker object (`GEKKO_NATIVE_EXPAND_JOBS=3`) | extract oracle CTest (every path, 2 controls); job census equal to mode 2 on 48 runs; memory differences classified; queued-job savestate; G0; see §6 | TBGRead (W2) |
| The C++20 pass: every project target C++20; the extract's lookup, memcpy and synchronous PRS in C++; the worker a `std::jthread` | the C++17 build against the C++20 build, whole memory, 91 run pairs (switches off and mode 4); oracle CTest of the native lookup (bounds, 1 mutation) and of the synchronous paths with real members; see §8 | — |
| W2: TBGRead without its worker (`GEKKO_NATIVE_EXPAND_JOBS=5`, with `GEKKO_NATIVE_BG_READ=1`) | oracle CTest (every exit of the update, the completion, a savestate; 2 mutations); 50 runs: every read and expand delivers the same bytes, reads in the same order; the first-gameplay readout on 38 stages and 55 s on 2; a state with a queued read resumes exactly; see §9 | whole-memory identity with mode 4 (not possible: reads complete earlier, C2) |
| Phase 4a: the ONE archive layer's 9 functions in C++ (`GEKKO_NATIVE_ONE=1`) | oracle CTest: every function, every disc archive adopted and parsed both ways, the stack frames compared, 4 mutations; whole memory identical on 86 run pairs; see §10 | Open, OpenAsync, OnRead (game code) |
| Frame-exact expand owners (the extract, TBGReadManager::update; the publication kept off the main stack) | oracle CTest with the stack compared and the guest calls' stack pointer / return site / registers, 3 mutations; the previous build against this one, whole memory identical on 86 run pairs; see §11 | the frames of the ctors a queued expand or a spawned read no longer runs |
| Phase 4b: the sync Open in C++ (`GEKKO_NATIVE_ONE=1`), its reads through `NativeRtGcnFs::read_sync` | Open against the original words for every disc archive (stack, buffer, calls with their registers); read_sync against the yielding owner on every read of the RtGCNFS script; 1 + 1 mutations; whole memory identical on 86 run pairs; see §12 | OSYieldThread's bookkeeping at the elided yields (no lasting trace found) |
| Phase 4c: OpenAsync and OnRead in C++ (`GEKKO_NATIVE_ONE=1`; the ONE layer complete) | OnRead (with and without a callback) for every disc archive and OpenAsync's 4 exits against the original words, with the stack and the callback's entry state; 2 mutations; whole memory identical on 86 run pairs; see §13 | the requesters' callbacks (game code) |
| Phase 4d: the whole-file sync read 0x800450C4 in C++ (`GEKKO_NATIVE_ONE=1`) | every disc archive read both ways (buffer, size, stack, calls), a missing file, a declined case; 1 mutation; whole memory identical on 86 run pairs; see §14 | RwFopen / RwFclose / RwMalloc (game code) |
| Phase 4e: the six TOneFileAsync<T> loaders' updates in C++ (`GEKKO_NATIVE_ONE=1`) | every variant through every state against the original words (task, counters, result word, stack, calls by arity), 2 mutations; whole memory identical on 86 run pairs; see §15 | the RenderWare readers, the manager, the heap (game code) |
| Phase 4: the strict in-game oracle over the loader scope (`--verify-player-strict`) | 0 mismatched on Westopolis (10.1 M segments), Final Haunt (11.0 M) and a cold stg0402 load (1.3 M); 8 ONE owners owner-verified; two owner defects found and fixed (r3 / r4 / f1, pc); see §16 | the I/O owners and the loaders' updates (not replayable: §16) |

## 5. Phase 1 result (2026-11-03)

**The check** (`tools/stage_loading/mode_equal.sh`): for each run, a headless cold boot with
`GEKKO_NATIVE_EXPAND_JOBS=1` (the game's step 0x80043D5C at the publication point) and `=2` (the expand by the host
decoder through `GameMemory::expand_for_native`, then the game's notice builder 0x80043DB0). Both publish at the same
point, so both runs are compared at the same tick over **every guest memory region**: image, heap, locked cache, sound
banks, stream ring, motion pool.

| Set | Runs | Result |
|---|---|---|
| all 38 Select Mode stages (saved at 5.0 s) | 38 | identical in every region |
| 2P arenas through `--boot-2p` (14.0 s) | 3 | identical |
| story-entry stages 0700, 0710, 0001, 0901, 0907 (10.0 s) | 5 | identical |
| decode-ahead off (`GEKKO_PRS_AHEAD=0`: every job through the host decoder on the game thread): stg0100, 0604, 0402 | 3 | identical |
| Westopolis and Final Haunt tapes to 85 s (gameplay streaming) | 2 | identical |

- **The corpus:** 536 expand jobs. 502 were served from the host workers' decode-ahead, and 34 were decoded by the host
  decoder on the game thread (the decode-ahead misses, and the runs with it switched off). **0 fell back to the game's
  step.** The step 0x80043D5C no longer runs: a census on it counts 0 calls.
- **What stays guest code:** the load-hold query (a native owner that never holds) is skipped. The notice builder (the
  completion: the requester's callback) stays game code, as the plan's §4 says.
- Runs are kept in `.build/stage_loading/p1/`.

## 6. Phase 2 result: the expand without a worker object (2026-11-03)

Whole-memory byte equality cannot hold here, and is not claimed. Mode 3 allocates no 0x348 TExpandBG and no
0x1000-byte stack, so every later heap allocation can land elsewhere (contracts G2). What is proven instead:

**1. The extract owner against the original words** (`native_expand_jobs_tests`, reference interpreter):
- **The paths that make no worker** (not open, open by name with no member, stored with and without a callback,
  compressed without a callback): r3, r1 / r2 / r13 / r14–r31, the guest calls in order and the destination bytes
  equal the original's.
- **Compressed with a callback, two members:**
  - the original makes two 0x348 workers; the native makes no allocation and only the two lookups, and returns 0
    the same way;
  - publishing at the reaper produces the original steps' calls in order (expand, new 0x38, groups, group 0, notice
    ctor with callback, size and userdata). The differences: the step's load-hold query (skipped, as in Phase 1),
    the notice's worker field (0), and the heap addresses, which are compared by allocation order;
  - the destination bytes are equal; the queue's savestate block round-trips.
- **Controls, run and reverted:** a wrong stored-member result (2 failures) and a reversed publication order
  (4 failures) are both caught.

**2. The job census** (`GEKKO_EXPAND_CENSUS`, `tools/stage_loading/census_compare.py`): one line per published job
(tick, src, dst, size, callback, userdata, FNV-1a of the destination bytes), mode 2 against mode 3, through
`mode_equal.sh` with the census on.

| Set | Runs | Jobs | Equal tick, size, callback, bytes |
|---|---|---|---|
| all 38 Select Mode stages (5.0 s) | 38 | 342 | 342 |
| Westopolis and Final Haunt tapes to 85 s | 2 | 42 | 42 |
| 2P arenas through `--boot-2p` (14.0 s) | 3 | 70 | 70 |
| story entries 0001, 0700, 0710, 0901, 0907 (10.0 s) | 5 | 55 | 55 |

- Userdata, and on some stages src / dst, are heap pointers that moved; they are counted, not failed.
- In mode 3: 0 workers built, 0 jobs left pending, 0 expands through the PRS owner.

**3. Every memory difference, classified** (all 38 stage pairs at 5.0 s, every differing word of the program
image):
- the "Expanding" registry 0x8056FFC8: 452 words (entries and its cursor; no game reader of kind 1);
- the thread manager's next-slot counter (0x80584964 +4): fewer registrations;
- words that hold pointers into the heap, where the block moved (e.g. lbl_8051DFA8 +0 on all 38: 0x80FE3CD8 against 0x80FEC248 on stg0100).

Nothing else differs. The heap region differs as its layout moved (49 KB on stg0100).

**4. Determinism and savestates** (`midload_state.sh` with `TAPE=`, Westopolis tape, mode 3):
- **G0:** A1 = A2 at tick 675675000.
- **A save with 2 queued jobs:** B saved at tick 591300000, between the extract and the publication ("saved with 2
  queued job(s)"). Loaded and run on as C, it equals A1 at 675675000 in every region.
- **The same state loaded without mode 3** stops with "the state has 2 queued jobs; it needs
  GEKKO_NATIVE_EXPAND_JOBS=3" (exit 5). It does not drop the jobs.

**5. The gameplay readout** (`ab_first_frame.sh` with `MODEVAR=GEKKO_NATIVE_EXPAND_JOBS MODEA=2 MODEB=3`, then
`ab_semantics.py`), all 38 Select Mode stages:
- PlayAction comes at the same game time in both modes on every stage;
- 16 pairs per stage (5–20 frames after PlayAction) all pair at a frame offset of 0;
- every readout field is equal except `player.address`, the player object's heap address, on 29 stages: G2's heap
  layout, as in W1.
- Position, velocity, camera, entities, set records, rings, score and the RNG hash are equal on all 608 pairs.

Runs: `.build/stage_loading/p2/` (`all`, `tape-*`, `2p`, `story`, `midload-tape`, `ab`).

## 7. Phase 3 result: misses decoded by a native worker (2026-11-03)

Mode 4 (`GEKKO_NATIVE_EXPAND_JOBS=4`) differs from mode 3 only in where a decode-ahead miss is decoded. It allocates
nothing in guest memory, so here whole-memory byte equality is the claim.

**1. Oracle CTest** (`native_expand_jobs_tests`, Phase 3 section). Two real compressed members of
`files/character/amy.one` (AMY_HAND_L01.DFF, AMY_HAND_L02.DFF) go through the extract and the publication, against
the original's extract and step. On the native side a GameMemory is attached, so its worker decodes the copies.
- Destination bytes and the notices' arguments are equal; both jobs were copied and both placed from the worker's
  result.
- **Control:** a stream byte is changed between the extract and the publication on both sides. The change reaches
  the output (checked). The worker's copy is refused, the stream is decoded at publication, and the bytes still equal
  the original's.
- **Mutation, run and reverted:** placing the worker's result without the stream check fails 4 checks.

**2. Whole memory, mode 3 against mode 4** (`mode_equal.sh`, census on):

| Set | Runs | Result |
|---|---|---|
| all 38 Select Mode stages (5.0 s) | 38 | identical in every region |
| decode-ahead off (`GEKKO_PRS_AHEAD=0`): stg0100, 0402, 0604, 0616 | 4 | identical |
| Westopolis and Final Haunt tapes to 85 s | 2 | identical |
| 2P arenas (14.0 s) | 3 | identical |
| story entries 0001, 0700, 0710, 0901, 0907 (10.0 s) | 5 | identical |

- 543 jobs, every census line equal.
- 502 jobs were held by the decode-ahead at the extract (no copy); 41 were copied and decoded by the worker, and all
  41 were placed from its result. 0 were declined, and 0 decoded on the game thread (mode 3: 41).

**3. Savestate with queued jobs** (`midload_state.sh`, `TAPE=`, mode 4): B is saved at tick 591300000 with 2 jobs
queued. Loaded as C, those 2 jobs have no worker result and are published through the other paths. C equals A1 at
675675000 in every region; A1 equals A2 (G0).

Runs: `.build/stage_loading/p3g/` (the final code); `.build/stage_loading/p3/` is the same sweep before the copy gate.

Reproduce: the commands are in each tool's header; the runs are kept in `.build/stage_loading/`.

## 8. The C++20 pass (2026-11-03; the user: "Turn everything into C++ 20")

**The change:**
- Every project target (43 targets: 42 in `CMakeLists.txt` and `runtime/lift/render_vk`'s core, `cxx_std_17` → `cxx_std_20`) compiles as
  C++20. Third-party libraries keep their own settings (plume C++17, tracy C++11).
- One source needed a change: `runtime/backends/storage/storage_backends.cpp`. `path::u8string()` is a `std::u8string`
  in C++20, so its messages go through a UTF-8 helper (the same one `shadow_main.cpp` already had).
- The expand path's remaining proven guest calls are now C++ (modes 3–5):
  - the ONE member lookup fn_8004CA50 (One::Entry, a leaf), computed in `one_entry`;
  - the stored member's memcpy 0x800054F4: one host memmove (`GameMemory::move_for_native`), the memcpy owner only
    when the memory is not direct host memory;
  - a compressed member without a callback: the host decoder (`GameMemory::expand_for_native`), the PRS owner only
    when it declines.
- The Phase 3 worker is a `std::jthread` with a stop-aware `condition_variable_any`.
- Still game code: the stored member's notice 0x8004AD84, operator new, the task groups and the notice's ctor (the
  completion is a game Task). Those are the documented boundary.

**Proof:**
- **Oracle CTest** (`native_expand_jobs_tests`):
  - the lookup now runs as the original words on the original's side. The native side has no stand-in: 9 extract cases,
    6 of them the lookup's bounds (index 1, -1, count + 2, 0x80000000, no member, not open);
  - the synchronous paths with real `amy.one` members on a GameMemory: a stored member through the host memmove and a
    compressed one through the host decoder. Bytes, size and registers equal the original's memcpy / PRS, and the
    native side makes no guest call;
  - **mutation, run and reverted:** the lookup's lower bound 2 → 1 fails 1 check.
- **The other CTests** built as C++20 pass: `native_rtgcnfs_tests` (35 calls), `native_bg_read_tests` (207 calls),
  `game_errata_tests`, `asset_import_tests` (the whole disc).
- **In game,** the C++17 build (`.build/stage_loading/cxx17-baseline/`, the exe of §7) against the C++20 build
  (`.build/stage_loading/cxx20-exe/`), same switch on both sides, `mode_equal.sh` with `EXEA` / `EXEB`:

| Set | Runs | Result |
|---|---|---|
| switches off, all 38 Select Mode stages (5.0 s) | 38 | identical in every region |
| mode 4, all 38 stages (5.0 s) | 38 | identical in every region |
| mode 4, decode-ahead off: stg0100, 0402, 0604, 0616 | 4 | identical |
| mode 4, 2P arenas (14.0 s) | 3 | identical |
| mode 4, story entries 0001, 0700, 0710, 0901, 0907 (10.0 s) | 5 | identical |
| mode 4, Westopolis and Final Haunt tapes to 85 s | 2 | identical |
| switches off, Westopolis tape to 85 s | 1 | identical |

  The mode 4 side of the C++20 build also served the lookup, memcpy and synchronous PRS natively, so the same runs
  prove those (on stg0100: 597 extracts, 589 served from the decode-ahead).
- **Not built in this tree, before and after:** `gekko_core_tests` and `native_failure_publication_tests` do not link
  in the lifted tree. The legacy renderer and immediate builder they call are removed from it (`SHADOW_RENDER_LIFTED`).
  That is a configuration fact, not a C++20 one.

Runs: `.build/stage_loading/cxx20/` (`sweep.txt`).

## 9. W2: TBGRead without its worker (2026-11-03; the user: "then start W2")

**The change** (`GEKKO_NATIVE_EXPAND_JOBS=5` = mode 4 + W2; it needs `GEKKO_NATIVE_BG_READ=1`, whose inline read it
uses, and refuses to install without it; contract C2, `native_expand_jobs.h`):
- TBGReadManager::update 0x800453E0 is a C++ owner with the game's policy:
  - nothing while a read is in flight or reads are paused;
  - the first non-empty request list, the busy entry, the open on the game thread, and the destination allocated when
    the requester gave none;
  - then a host read job instead of `new` 0x390 + the TBGRead ctor (no Thread base, stack, OSThread or manager slot);
  - the in-flight count + 1, and the request node popped and freed.
- At the reaper's entry, in submission order with the expand jobs, TBGRead's step without a thread:
  - the inline read of the whole file (`NativeRtGcnFs::chunked_read`, the read loop's work);
  - the completion Task as 0x80045890 builds it: `new` 0x48 and Task ctor 0x8004F014 under the read manager's task,
    then TFileControlAsycManager's ctor writes in C++, with the worker word 0;
  - the in-flight count − 1 (the dtor's).
- Savestates: version 29 adds a kind to each queued job; a queued read is its six request words and the file.

**Proof:**
- **Oracle CTest** (`native_expand_jobs_tests`, original words in the interpreter: the update, TBGRead's ctor, step,
  completion tail, TFileControlAsycManager's ctor and the dtor):
  - the update's six exits (a read in flight, paused, no request, a failed open, a read with and without the
    requester's destination). Compared: the in-flight count, the three lists, the request words, the non-volatile
    registers and the guest calls, which equal the original's minus the TBGRead's own (`new` 0x390, Thread ctor,
    name copy);
  - the completion: the read, `new` 0x48 and the Task ctor under the same parent. Every byte of the Task is equal
    except the worker word (the TBGRead / 0). The destination bytes are equal, and the in-flight count goes 1 → 0;
  - a queued read's savestate block round-trips, and mode 4 refuses it;
  - **mutations, run and reverted:** the Task's armed flag (0x8000) left out fails 2 checks; the pause byte ignored
    fails 5 or more.
- **In game: mode 4 against mode 5,** the same exe, both with the inline read, census on (`.build/stage_loading/w2/`):

| Set | Runs | Expand jobs | Reads | Result |
|---|---|---|---|---|
| all 38 Select Mode stages (5.0 s) | 38 | 361 | 203 | equal bytes, sizes and callbacks; reads in the same order |
| decode-ahead off: stg0100, 0402, 0604, 0616 | 4 | 34 | 20 | the same |
| 2P arenas (14.0 s) | 3 | 70 | 21 | the same |
| story entries 0001, 0700, 0710, 0901, 0907 (10.0 s) | 5 | 65 | 34 | the same |
| Westopolis and Final Haunt tapes to 85 s | 2 | — | — | not comparable: see below |

  `census_compare.py --unordered` compares the jobs and the reads as multisets of (size, callback, bytes), and the
  reads' order. The hash covers each file's bytes; the up to 31 bytes after a file's end, up to the read's 32-byte
  rounding, are whatever the heap held there.
- **Whole memory** is not equal, and cannot be:
  - the reads complete earlier;
  - the heap moves without the 0x390 objects and their stacks;
  - the audio stream ring and the timers count the shorter load.
- **The first gameplay frames** (`ab_first_frame.sh` + `ab_semantics.py`, all 38 stages):
  - every readout field is equal: player, camera, entities, set records, mission, rings, score, the RNG hash;
  - the exceptions are the player object's heap address (G2) and the counters of time since boot;
  - with 3 states per run, 13 stages paired one frame apart and showed position / camera / RNG differences. With 16
    states (`.build/stage_loading/w2/ab16`) all 13 pair at a constant offset, and differ only in the player's
    address. A pairing artifact, as 9e describes.
- **Long neutral A/B**, no input, up to 3304 frames (55 s) after PlayAction, stg0100 and stg0604: equal apart from the
  player's address.
- **The tapes:** a tape plays from boot, so with the load shorter its inputs reach other game states. The
  gameplay diverges and other blocks stream (19 / 15 reads on Westopolis). That is the tape's alignment, the same
  as for the inline read (`STAGE_LOADING.md` 9d), not a read difference. The long neutral A/B above covers gameplay
  instead.
- **Determinism and savestates** (Westopolis tape, mode 5):
  - G0: two cold runs are equal at 676350000 in every region;
  - a state saved with a read queued (the saves land at host-service boundaries, so the run saves at every
    frame of a window: the state at 559575000 holds one read job, `.build/stage_loading/w2/qtry4`). Loaded and run on,
    it publishes that read and equals the cold runs at 676350000 in every region;
  - the same state loaded in mode 4 stops with "the state has a queued read; it needs GEKKO_NATIVE_EXPAND_JOBS=5".
  - **A defect found this way and fixed:** the first build wrote the kind word without raising the savestate version,
    so a state with queued jobs did not load ("mismatched renderer block"). The version is now 29.
- **Load time:** PlayAction 0.016–0.217 s earlier than mode 4 (mean 0.075 s over 38 stages; stg0400 1.750 →
  1.533 s). Mode 5 reaches it at 1.517–1.533 s on every stage.

Runs: `.build/stage_loading/w2/` (`sweep.txt`, `ab16.txt`, `long.txt`, `midload-read2`).

## 10. Phase 4a: the ONE archive layer in C++ (2026-11-04; the user: "Start Phase 4")

**The change** (`GEKKO_NATIVE_ONE=1`, off by default; `native_one.h`, `docs/ONE_LAYER.md`):
- C++ owners for 9 functions: Entry, IsOpen, FindName, FindSubstr, ParseDirectory, Adopt, Close, the ctor without a
  path, and the dtor.
- Declined to the original: an allocator object (no caller passes one), the ctor with a path (its Open's RtGCNFS
  reads yield the thread), and a parse of a closed archive.

**A defect found on the way, and the rule it set:**
- The first version wrote no stack frames.
- Whole memory then differed on stg0100 in 18 heap bytes: 2 padding bytes in each of 9 records of 0x2C bytes.
- The guest watch found the writer: fn_8006BEF0, a 0x2C-byte struct copy whose source is a stack temporary with
  uninitialised padding. The game copies stack leftovers into the heap, so a skipped frame is visible.
- **The owners now write their originals' frames:** back chain, saved LR, saved registers, and FindName's uppercase
  buffer.
- **Their guest calls run from those frames:** the RenderWare free, operator delete, and the game's own sscanf for the
  version. Each uses the original return site and the original non-volatile registers.
- memset's frame is written in C++.

**Proof:**
- **Oracle CTest** (`native_one_tests`): the original words in the interpreter against the owners. Compared after
  every call: r3, the non-volatile registers, the archive object, the archive buffer, the 0x400 stack bytes below the
  caller and the guest calls with their stack pointer, LR and registers.
  - **every archive on the disc:** 1,244 `.one` files, 13 of them version 0.50, 15,122 members;
  - **calls per archive:** Adopt, a direct ParseDirectory as a sync open does it, Entry over its bounds, IsOpen,
    FindName (first / middle / last member, lowercase, absent, 43 and 44 characters), FindSubstr (6 substrings
    including the empty one, 3 starts), Close and the dtor;
  - **cases outside the corpus:** the ctor (with and without a path or an allocator), the dtor's flags, a sync Close,
    an allocator object and a version string the disc does not hold;
  - **result:** 383,488 checks, 0 failures;
  - **mutations, run and reverted:** Entry's upper bound fails 1,437 checks; the 0.50 layout's flag copy 39;
    FindName's saved registers left out 12,444; Close's memset return site 1,245.
  - In the test, sscanf is a stand-in on both sides. The native calls whatever sscanf the machine has, as the original
    does, so it does not depend on sscanf's result.
- **In game, `GEKKO_NATIVE_ONE` 0 against 1,** the same exe, `mode_equal.sh` (`.build/stage_loading/one/`):

| Set | Runs | Result |
|---|---|---|
| all 38 Select Mode stages (5.0 s) | 38 | identical in every region |
| 2P arenas (14.0 s) | 3 | identical |
| story entries 0001, 0700, 0710, 0901, 0907 (10.0 s) | 5 | identical |
| Westopolis and Final Haunt tapes to 85 s | 2 | identical |
| all 38 stages with W2 on both sides (expand jobs mode 5, inline read) | 38 | identical |

  The owners served 193,821 Entry, 24,716 IsOpen, 23,655 FindName, 4,496 FindSubstr, 1,512 ParseDirectory, 381 Adopt,
  1,900 Close, 388 ctor and 1,810 dtor calls; 1,512 calls (the ctors with a path) ran the original.
- **Not covered:** the game-thread time saved was not measured. These functions are small, so the change is in what
  runs, not in speed.

Runs: `.build/stage_loading/one/` (`sweep.txt`).

## 11. The expand owners made frame-exact (2026-11-04; the user: "Yes, fix them first")

**Why:** Phase 4a showed the stack is observable state. The game copies stack leftovers into heap records
(fn_8006BEF0). The expand owners written before that (the extract 0x8004C770, modes 3-5; TBGReadManager::update,
mode 5) wrote no frames, and their publication ran its guest calls on the main thread's stack.

**The change** (`native_expand_jobs.cpp`, `native_expand_jobs.h`):
- **Frames:** the extract and the update write their originals' frames: back chain, saved LR, saved r27..r31 /
  r29..r31.
- **Guest calls from those frames:** memcpy / the PRS owner / the stored member's notice for the extract; the busy
  entry, the open, the allocator and the free for the update. Each uses the original return site and the original
  non-volatile registers at that point.
- **Where no object is built** (a queued expand, a spawned read): the skipped ctor's return address goes into the
  owner's LR slot, as the ctor's prologue wrote it. The ctor's own frames below are not made: that work no longer
  exists.
- **The publication** (originally on the workers' own stacks): the main thread's stack below the reaper, within the
  thread's stack bounds and up to sp + 8, is restored after it.

**Proof:**
- **Oracle CTest** (`native_expand_jobs_tests`): the stack below the caller is pre-filled on both machines.
  - **compared:** the whole 0x400-byte window after every extract path that builds no object and every update exit
    that spawns nothing; the owner's own frame after a queued extract or a spawned read;
  - **the publication:** it leaves the main stack equal to a reaper call with nothing queued;
  - **the guest calls' logs:** now carry their stack pointer, return site and r27..r31;
  - **mutations, run and reverted:** no stack restore at publication fails 2 checks; the extract's saved registers
    left out 11; a wrong return site for the update's open 3.
- **In game: the previous build** (`.build/stage_loading/w2-exe`, owners without frames) **against this one**
  (`.build/stage_loading/frames-exe`), the same switches, `mode_equal.sh`:

| Set | Runs | Result |
|---|---|---|
| mode 5 (inline read), all 38 stages (5.0 s) | 38 | identical in every region |
| mode 5, 2P arenas (14.0 s) | 3 | identical |
| mode 5, story entries (10.0 s) | 5 | identical |
| mode 5, Westopolis and Final Haunt tapes to 85 s | 2 | identical |
| mode 3, all 38 stages (5.0 s) | 38 | identical |

  So on these runs the frames the old owners skipped never reached anything that survives to the save point. The
  earlier mode 3-5 results were not hiding a stack-borne difference. The owners are now exact by construction for
  that part, and the CTest holds them to it.
- **What still cannot equal the original:** a queued expand or a spawned read makes no TExpandBG / TBGRead, so those
  ctors' frames below the owner are never written. That is the work Phase 2 and W2 removed. Its other effects (the
  heap layout, the tables) are already classified in §6 and §9.

Runs: `.build/stage_loading/frames/` (`sweep.txt`).

## 12. Phase 4b: the sync Open in C++ (2026-11-04; the user: "Continue Phase 4")

**The obstacle:** Open (0x8004CE1C) reads through RwStreamRead, then RwFread, then RtGCNFS's read, which yields the
thread twice per chunk. A native owner cannot make a synchronous call into code that yields.

**The change:**
- **`NativeRtGcnFs::read_sync`:** read's own state machine (the same code as the owner: its frame below the caller's,
  the transfers, the cache, the drive-error callback, the memmove of a cache serve). Each yield is continued at once
  instead of being handed to OSYieldThread 0x80378A08.
- **When it is used:** only when no other thread of the current thread's priority is ready (RunQueueBits). There,
  OSYieldThread would return at once, so scheduling is unchanged.
- **What is not done:** OSYieldThread's own bookkeeping: its frames, the current thread's context save, the run-queue
  round trip.
- **The Open owner in `native_one.cpp`** (frames as in §10):
  - its frame; the pending / null-path / already-loaded exits; Close;
  - the read manager's getter and 0x80045014 (the path copy and RwStreamOpen) as game code;
  - the chunk header reader 0x8047E99C, RwStreamRead and RwFread in C++, with their frames, the byte swap, the
    version / build decode and isComplex from the game's own jump table;
  - RwMalloc between the category push and pop, the body read, and RwStreamClose as game code;
  - then strcpy, the fields and the parse.
- **The ctor with a path** opens the same way.
- **Declined (the original runs):** a ready thread of the same priority, and an allocator object.

**Proof:**
- **`native_rtgcnfs_tests`:** a third machine runs the whole script with every read through read_sync. After each of
  the 14 reads it must equal the yielding owner machine: result, registers, the drive-error callback calls, the file
  system object, the records, the caches, the buffers. Only the yields are missing. 0 failures.
  **Mutation:** continuing every yield as a chunk (skipping the transfer step) fails 23 checks.
- **`native_one_tests`:** for every archive on the disc, Open against the original words: Open, the chunk header
  reader, RwStreamRead and RwFread run as the original's code. Stand-ins on both sides: the path copy + stream open, the
  RtGCNFS read slot (the native side's read_sync does the same copy), RwMalloc, RwStreamClose, the getter and the
  category push / pop, each logging its stack pointer, return site and r28..r31.
  - **compared:** r3, the registers, the archive object, the buffer, the 0x400 stack bytes and the calls;
  - **also:** Open again (already loaded), the pending / null-path / missing-file exits, a ready thread (declined),
    and the ctor with a path;
  - **result:** 409,669 checks, 0 failures.
- **In game, `GEKKO_NATIVE_ONE` 0 against 1,** the same exe (`.build/stage_loading/open/`):

| Set | Runs | Result |
|---|---|---|
| all 38 Select Mode stages (5.0 s), everything else off | 38 | identical in every region |
| 2P arenas (14.0 s) | 3 | identical |
| story entries (10.0 s) | 5 | identical |
| Westopolis and Final Haunt tapes to 85 s | 2 | identical |
| all 38 stages with W2 on both sides (expand mode 5, inline read) | 38 | identical |

  - **Native opens:** 1,407 sync opens ran natively and read 303 MB; 447,894 yields were continued at once.
  - **Declined:** 106 opens ran the original, because a TBGRead worker of the same priority was ready (the switches-off
    runs only; with W2 there are none).
  - **No lasting trace:** the skipped OSYieldThread bookkeeping (its frames, the main thread's saved context)
    survived to no save point on these 86 runs.

Runs: `.build/stage_loading/open/` (`sweep.txt`).

## 13. Phase 4c: OpenAsync and OnRead in C++, the ONE layer complete (2026-11-04; the user: "Continue Phase 4")

**The obstacle:** OnRead calls the requester's callback (the stage archive's fn_80177964, ResourceOneFile's
fn_80014DAC, an event scene's fn_80171AC4). That is game code that may read synchronously, and a read yields. So
OnRead cannot call it and wait.

**The change** (`native_one.cpp`, `native_one.h`):
- **OpenAsync:** the request block is written on its own frame at +8 (path, destination, size, the archive, OnRead;
  +0x1C is never written, as in the original). Then Close, the callback fields, and the read manager's getter and the
  submit 0x8004506C (game code: the exists check and the queued node) from its frame.
- **OnRead:** its frame, then Adopt. Then the callback is entered exactly as the original's bctrl enters it: r1 =
  OnRead's frame, r31 = the archive, r3 = the argument, LR = 0x8004CC6C, the word after the bctrl. It goes through the
  dispatcher (`leave_to_dispatcher`), so the callback runs on its own and may yield.
- **The tail:** an owner at 0x8004CC6C (an interior address of OnRead's translated range, which the dispatcher serves
  as it serves the RtGCNFS read's continuations) clears the pending byte and runs the epilogue back to OnRead's caller.
- **Declined:** a request without a buffer (the original parses a closed archive, through 0) and an allocator object.
- **The layer:** all 13 ONE functions are now C++. The only game code left on its paths is the RenderWare heap, the
  RW stream open / close, the read queue's node, the Task ctors, and the requesters' callbacks.

**Proof:**
- **Oracle CTest** (`native_one_tests`), against the original words:
  - for every disc archive, OnRead with a callback (a stand-in logging its argument, stack pointer, LR and r31: the
    entry state the original's bctrl gives it) and without one, each followed by the dtor;
  - OpenAsync while pending, on a closed and on an open archive (Close frees first), and refused by the submit (only
    the low byte of its result counts). The submit stand-in logs the request block it is handed;
  - compared as before: r3, registers, the archive object, the buffer, the 0x400 stack bytes (the request block's
    unwritten word included), the calls;
  - **result:** 444,558 checks, 0 failures;
  - **mutations:** writing the request block's +0x1C word fails 6 checks. Entering the callback with LR = 0x8004CC70
    sends the native machine into OnRead's other branch: it never returned (run to the step limit, stopped by hand).
- **In game, `GEKKO_NATIVE_ONE` 0 against 1** (`.build/stage_loading/async/`):

| Set | Runs | Result |
|---|---|---|
| all 38 Select Mode stages (5.0 s), everything else off | 38 | identical in every region |
| 2P arenas (14.0 s) | 3 | identical |
| story entries (10.0 s) | 5 | identical |
| Westopolis and Final Haunt tapes to 85 s | 2 | identical |
| all 38 stages with W2 on both sides | 38 | identical |

  377 OpenAsync and 371 OnRead calls ran natively. 97 OnRead calls entered a requester's callback and came back
  through the tail owner (the stage archive's, once per stage load); the rest had none (the landscape loader polls
  instead). The sync opens of §12 ran natively too (1,407).

Runs: `.build/stage_loading/async/` (`sweep.txt`).

## 14. Phase 4d: the whole-file sync read in C++ (2026-11-04; the user: "Continue Phase 4")

**What it is:** 0x800450C4 (manager, path, &size) is how the game's synchronous loads read a whole file (the
set-object tables, the stage's other files, `STAGE_LOADING.md` §5):
- RwFopen;
- RwMalloc of the file's length;
- the whole file by RwFread, which ends in RtGCNFS's read and yields;
- the size stored, RwFclose.

**The change** (`native_one.cpp`):
- **The owner:** its frame; RwFopen, RwMalloc and RwFclose as game code from it (they reach only RtGCNFS's open /
  close, which do not yield).
- **RwFread in C++:** with its frame, through `NativeRtGcnFs::read_sync` (§12). It is now one helper (`rw_fread`)
  shared with Open's RwStreamRead.
- **Declined:** as Open, while a thread of the current priority is ready.

**Proof:**
- **Oracle CTest** (`native_one_tests`):
  - every disc archive read both ways; the buffer, the stored size, the 0x400 stack bytes and the calls (RwFopen /
    RwMalloc / the read slot / RwFclose stand-ins logging their stack pointer, return site and registers) equal the
    original words';
  - a missing file (no size stored) and a declined call (a ready thread);
  - **result:** 454,534 checks, 0 failures;
  - **mutation:** RwFread saving the wrong register in its frame fails 2,490 checks.
- **In game, `GEKKO_NATIVE_ONE` 0 against 1** (`.build/stage_loading/whole/`): the same 86 runs as §13 (all 38 stages,
  2P, story entries, both tapes, all 38 stages with W2) are identical in every region.
  - 2,485 whole-file reads ran natively (438 MB); none was declined.

**Next (batch 4e), mapped:** the six TOneFileAsync<T> loaders (`STAGE_LOADING.md` §5; update functions 0x8004AE60,
0x8004B3A0, 0x8004B7BC, 0x8004BBD4, 0x8004BFF0, 0x8004C408, about 200 instructions each). Each is a Task state
machine:
- the member's entry, an allocation with a retry through the TOneFileAsyncManager, and the extract (owned);
- a memory stream opened over the bytes, the chunk found, the RenderWare reader (RpWorldStreamRead and the others:
  game code until Phase 5's rwcore), the stream closed, the buffer freed;
- the requester's callback, entered as OnRead's is (§13).

Runs: `.build/stage_loading/whole/` (`sweep.txt`).

## 15. Phase 4e: the TOneFileAsync loaders in C++ (2026-11-04; the user: "Continue Phase 4")

**What they are:** the stage's archives (render worlds, collision, clumps, texture dictionaries, morph animations,
raw members) are loaded by TOneFileAsync<T> Tasks. Six update functions share one template and differ only in the
done callback, the chunk type, the reader and two small steps:
- RpDMorphAnimation 0x8004AE60, RpWorld 0x8004B3A0, RtDict 0x8004B7BC, RpClump 0x8004BBD4, RwTexDictionary 0x8004BFF0,
  raw 0x8004C408.
- **State 0:** wait while 16 jobs run; the allocation with a retry (size + 0x401F, freed, then size + 0x1F; hint 1 when
  that fails); registration; the extract (async with the type's done callback, or sync).
- **State 1:** the wait for the expand; a failed extract frees and ends.
- **State 2:** a memory stream over the bytes, the chunk, the texture dictionary (RpWorld, RpClump), the reader
  (RtDict's takes the dictionary list), the texture dictionary's after-call.
- **State 3:** a wait flag.
- **State 4:** the buffer freed unless the type keeps it, the stream closed, the result stored, the requester's
  callback, then the archive's and the global job counts - 1 and the task done.

**The change** (`native_one.cpp`): one generic owner driven by a table per variant. The table holds the type's
return sites (all 21, extracted from the code), its done callback, chunk, reader, the texture dictionary's use (r30 =
+0x54 from the chunk search on, as in the original), the after-call and the name word state 4 compares.
- **Frames:** the owner writes the update's frame.
- **Game code from it:** the manager, the heap, the extract (an owner in expand modes 3-5), the RW stream and chunk
  calls and the readers (Phase 5's rwcore later).
- **The callback:** entered as OnRead's is (§13); an owner at the word after its bctrl runs the tail.

**Proof:**
- **Oracle CTest** (`native_one_tests`): every variant through its states, against the original words.
  - **Paths:** async with a callback; sync with the chunk not found; a failed extract; the job limit; both allocation
    retries; state 3's flag; the update argument zero and not.
  - **Compared after every update:** r3, the registers, the task (0x60 bytes), the archive's count, the global count,
    the result word, the stack and the calls. Each stand-in logs its arguments by its arity, its stack pointer, return
    site and r28..r31. The registers beyond a function's arity hold leftovers, which no callee reads.
  - **Result:** 455,279 checks, 0 failures.
  - **Mutations:** the tail without the archive count's decrement fails 6 checks; r30 not set for the texture
    dictionary 4.
- **In game, `GEKKO_NATIVE_ONE` 0 against 1** (`.build/stage_loading/loaders/`): the same 86 runs as §13 are identical
  in every region. 169,219 loader updates ran natively; 824 requester callbacks came back through the tail owners.

**Phase 4's state:** the ONE layer (13 functions), the sync open, the whole-file read and the loaders are C++ behind
`GEKKO_NATIVE_ONE=1`. What is left on these paths as game code:
- the RenderWare stream readers (RpWorld, clump, texture dictionary, morph, dictionary: Phase 5, rwcore);
- the RW stream open / close and the RenderWare heap;
- the TOneFileAsyncManager and the read queue's node;
- the Task ctors and the requesters' callbacks.

Runs: `.build/stage_loading/loaders/` (`sweep.txt`).

## 16. Phase 4: the strict in-game oracle over the loader scope (2026-11-04; the user: "Run the oracle")

**The tool:** `--verify-player-strict` on the reference runner (`ShadowTheHedgehogReference.exe` of this tree, rebuilt
with the owners). Every call segment entered in the scope is replayed by the interpreter over the original words and
compared: registers and every written byte.
- **Native owners** (owner passes) are held to the call's contract: r1-r4, r13-r31, f1, f14-f31, LR, MSR, CR2-CR4, the
  end pc. Every byte they write is compared, except dead stack below the returning r1.
- **The scope:** `test_data/stage_loading/loader_scope_ranges.txt` (737 loader functions, the census of
  `STAGE_LOADING.md`) plus the 8 ONE functions the census missed (Entry, IsOpen, FindSubstr, ParseDirectory, Adopt,
  Close, ctor, dtor): `.build/stage_loading/scope_phase4.txt`.
- **Inputs:** the in-workspace copies the sweeps use (the Westopolis and Final Haunt tapes under
  `.build/stage_loading/p3g`, the card / SRAM / config of `.build/player/census-westopolis`).

**What it found, fixed:**
1. **The call contract's registers.** FindName left r4 as its argument; the original leaves it where its last strcmp
   stopped. The owners now leave r3 / r4 / f1 exactly as their originals do on every path:
   - MSL strcmp is ported word for word (`msl_strcmp`: its byte / word / byte phases decide where r4 stops);
   - ParseDirectory: r3 the count, r4 the last pre-swap flags word, f1 the version (`lfs`, both halves);
   - Close, memset, the chunk-header reader, the loaders' tail and state 4: their own r4 writes;
   - a game-code call whose original loads only r3 is made with r4 and up as they are, since a callee may hand r4
     back unchanged.
   `native_one_tests` now compares r4 and f1 as well: 584,957 checks, 0 failures.
2. **The end pc.** An owner's game-code calls left pc at the last call's return site. Dispatch resumes a served owner
   at LR regardless, but the verifier's owner pass only when pc is still the entry, so the replay ran on past the call.
   The owners now restore pc after each game-code call (`native_one.cpp` and `native_expand_jobs.cpp`).

Neither changed guest memory: the 7-pair recheck (stg0100 / 0402 / 0604 with everything else off and with W2, and the
85 s Westopolis tape) is identical in every region with the final build.

**Results** (`GEKKO_NATIVE_ONE=1`; `.build/stage_loading/strict-phase4/`):

| Run | Segments matched | Mismatched | Skipped (hardware / did not return) | Instructions replayed |
|---|---|---|---|---|
| Westopolis tape, 30 s | 10,091,488 | 0 | 277 / 1 | 157 M |
| Final Haunt tape, 30 s | 11,048,861 | 0 | 381 / 1 | 166 M |
| cold stg0402 load, 6 s | 1,294,602 | 0 | 558 / 1 | 24 M |

Owner passes verified (Westopolis / Final Haunt / stg0402), all with 0 mismatches:

| Owner | Passes |
|---|---|
| Entry | 1,000 / 1,089 / 1,079 |
| IsOpen | 160 / 713 / 1,439 |
| FindName | 343 / 395 / 403 |
| FindSubstr | 64 / 80 / 80 |
| dtor | 21 / 23 / 23 |
| OnRead | 4 / 5 / 5 (with a callback: did not return into the pass) |
| Close | 1 each |
| ParseDirectory | 1 each |

**What this oracle cannot verify, and why:**
- **The I/O owners:** the whole-file read 0x800450C4, Open, OpenAsync, and the ctor (which opens when given a path).
  An owner pass replays the owner's whole subtree as original words. Theirs reach RwFopen / RtGCNFS's open, and from
  there DVDOpen. The disc layer was retired (its FST is gone), so the replay faults in DVDOpen's path lookup
  (0x8037AA5C, a load from 0x8). They are covered by `native_one_tests` (every disc archive) and the whole-memory
  sweeps (§12-§14).
- **The six loaders' updates:** their state 2 runs a RenderWare reader of ~100 k instructions. An interrupt inside it
  lands at a different instruction in the native pass than in the replay, so SRR0 / SRR1, the interrupt FPU state and
  f1 differ. This is interrupt timing, not a different result. The RpClump loader's one pass that saw no interrupt
  verified (549,633 instructions). They are covered by `native_one_tests` (every state, every variant) and the sweeps
  (§15).
- **The expand owners of modes 3-5** (the extract, TBGReadManager::update): they build no TExpandBG / TBGRead by
  design, so a replay of the original always differs (§6, §9).

With these out of scope, the extract 0x8004C770 is also out (it is called from the loaders); its segments are
replayed inside the loaders' callers' passes.

Runs: `.build/stage_loading/strict-phase4/` (`westopolis-one`, `finalhaunt-one`, `stg0402-one`, `recheck`).
