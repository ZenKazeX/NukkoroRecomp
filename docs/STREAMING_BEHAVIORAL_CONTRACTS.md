# Streaming behavioural contracts (2026-11-03)

What a native streaming component must guarantee to the game, stated so a test can check it. Each contract has two
parts that are never mixed:
- **Algorithmic:** bytes and values. Exact: the native result must equal the original's for every input.
- **Scheduling:** when results become visible. Not exact: a stated rule, plus the observable state that must stay
  identical under it.

**Evidence marks:** P (proven by instruction or trace), I (inferred), U (unknown). The sources are the documents
named in each section.

## C1. PRS expansion (algorithmic, exact)

- **Input:** a byte stream at `src`. **Output:** bytes from `dst`, and the count written.
- **Format** (P, `prs.cpp`, `game_memory.cpp` 0x80043FE0):
  - flag bits are LSB first, eight per flag byte, the first flag byte read before the first bit;
  - 1 = a literal byte;
  - 0 1 = a long match: little-endian word w; w == 0 ends the stream; distance `(w >> 3) | ~0x1FFF`; length `(w & 7) + 2`,
    or the next byte + 1 when `w & 7 == 0`;
  - 0 0 = a short match: two more flag bits, MSB first, + 2; distance `byte | ~0xFF`.
- **Copies** go forward byte by byte and may overlap the output's own past.
- **No bounds:** the original stops only at the terminator. The native codec takes the source size and an expected size,
  and treats running past either as an error. The game never does that on its data: every disc member decodes in bounds
  (`asset_import_tests`).
- **Result:** the count of bytes written. TExpandBG stores it at +0x33C, and the notice passes it to the callback as
  its first argument (P).
- **Verified:** every PRS member on the disc, native against the oracle (`asset_import_tests`); the owner in every
  replay (strict verification).

## C2. Background read: TBGRead (scheduling contract; algorithmic part = the whole file)

Sources: `STAGE_LOADING.md` 9d / 9f, `NATIVE_WORKERS.md`.

- **Request:** made on the game thread by TBGReadManager::update 0x800453E0 from its three FIFO lists.
  - It spawns at most one read per update.
  - Only while the in-flight count 0x805EF1D8 is 0 and the pause byte 0x805EF1DC is 0 (P).
  - The file is opened on the game thread; a failed open drops the request with no callback (P).
- **Bytes:** the whole file from offset 0, in RwFread chunks of 0x2000.
  - The destination is given, or allocated with fn_80049BB8(length, 2) on the worker before the first read (P).
  - The reported size is each chunk's bytes, the last rounded up to 32 (9d).
  - The file's RtFile state afterwards (position, cache) is the inline owner's, as proven by `native_bg_read_tests`.
- **Completion:** a 0x48-byte TFileControlAsycManager Task under the read manager's task (`*0x805EF168`), built by
  the finish 0x80045890.
  - Its update 0x8004580C closes the file and calls the requester's callback.
  - The worker's done byte is set; the reaper deletes it; its dtor decrements the in-flight count (P).
- **Ordering:** reads complete one at a time, in spawn order (P: one in flight).
- **Scheduling rule for a native replacement:** the completion becomes visible at the first publication point (the
  main-loop iteration boundary before the reaper) after the bytes are ready.
  - The rule (P, W0): in gameplay the original's completion Task is first updated in the frame after the request, and
    the reaper deletes the worker in that frame. A native read published at the next boundary keeps that.
  - While loading it is faster. That is the intended change, and the user accepted it on 2026-11-03 ("identical
    behavior but faster").
- **Not part of the contract:** the worker object's OSThread bytes (W0: nothing reads them). Its stack bytes: the one
  reader, ShadowMotion +0x3EC, is fixed by erratum E2.
- **Without the worker object (W2, mode 5), also allowed:**
  - no 0x390 TBGRead and no 0x1000 stack: later heap allocations move (§G2);
  - the thread manager's slot table and next-slot counter;
  - the completion Task's worker word (+0x44): 0 instead of the TBGRead. Its update only clears it (P: 0x8004582C);
  - the Task is built at the reaper's entry of the iteration after the request, not in the worker's slice during
    the previous iteration's sleep. Nothing between the two points reads it: the task walk comes after the reaper;
  - the read completes at the first publication point after the request. The original's worker waited for the guest
    scheduler to run it (about 9 main-loop iterations per read on stg0402 with the inline read). This is the rule
    above, not a new one.

  What stays equal: every read's bytes, size and request words (callback, userdata), the reads' order (one in flight),
  the in-flight count's transitions (+1 at the spawn, −1 at the reap), the busy entry calls, the open and the
  request node's free.
- **Cancellation:** none exists in the game (P, `NATIVE_THREADS.md`). Nothing clears the running byte or deletes a
  pending worker. A native scheduler needs cancellation only for its own shutdown and savestates; it must never drop a
  game request.
- **Error:** reads from the RAM-resident asset namespace cannot fail short. A failing host read stops the run (the
  existing owners' rule): a refusal, never a silent partial file.

## C3. Background expand: TExpandBG (scheduling contract; algorithmic part = C1)

- **Request:** the ONE member extract 0x8004C770, for a PRS member with a callback (P). Without a callback it decodes
  synchronously on the game thread. An uncompressed member is a memcpy plus a TOneFileNotice.
- **Completion:** a 0x38-byte TExpandNotice under group 0 (static_80571C6C [0]).
  - Its update 0x80043EF8 calls `callback(size, userdata)` and clears its worker pointer (+0x34) without reading
    through it (P). So the worker may be gone before the notice updates.
- **Scheduling rule:** the same as C2's. W1 keeps the gameplay frame timing (13 of 13 jobs on the Westopolis tape).
- **Observable state that must stay identical:** the destination bytes, the size, the callback's arguments and order,
  and the gameplay readout (W1's 55 s A/B).
- **Allowed difference:** heap addresses of later allocations (the worker, the stack and the notice are allocated and
  freed at other points). This is reported, not hidden (§G2).
- **Without the worker object (Phase 2, mode 3), also allowed:**
  - the "Expanding" registry 0x8056FFC8: TExpandBG's ctor registers kind 1 there and its dtor removes it. The only
    reader, 0x8025D488 (from StageAction::EndAction), asks for kind 2 (P);
  - the thread manager's slot table and next-slot counter;
  - the notice's worker field (+0x34), which is 0 instead of the worker. The update only clears it (P).

  Everything else listed above stays equal (`STREAMING_DIFFERENTIAL_VALIDATION.md` §6).

## C4. ONE archive layer (algorithmic, exact; mostly synchronous)

Source: session note `spec_ONE_layer.md`.

- **The guest buffer is the only copy of the directory:** callers read entry +0x2C (size), +0x30 (data pointer after
  the parse) and the count at data +0x10 directly. A native layer must leave the same buffer bytes after its parse:
  swapped words, absolute pointers, slots 0 / 1 skipped.
- **Lookup:** the first entry whose name equals the query uppercased; the comparison is case-sensitive (P). The host
  `OneArchive::find` scans backwards and cannot be used as is.
- **Sync open:** returns 1 without reading when the stored name equals the path. It sets 0x805E4A88 to 0x40 before
  RwMalloc(len, 0x34) and to 0x20 after (P).
  - Its reads yield twice per chunk (RtGCNFS). A yield with no other ready thread of the same priority returns at
    once. A native open may continue there directly; with such a thread ready it must not, because the thread would
    run (Phase 4b declines then).
- **Async open, completion and extract:** C2 and C3.
- **The stack is part of the observable state** (found in Phase 4a): the game copies uninitialised stack bytes into heap
  records (fn_8006BEF0, 0x2C-byte struct copies with padding). A native owner of a function with a frame writes that
  frame, and makes its guest calls from it, or the heap changes.

## C5. Landscape blocks (algorithmic and scheduling)

Source: session note `spec_TBlockManage.md`.

- The 168-block mask from the BDT boxes: `!(p < min) && !(p >= max)` per axis, early exits, single-precision compares.
- The two mask generations, the forced states (+0x2E8), the hold counter (+0x40) and the loaded list (+0x828) are read
  by collision, draw, fog, goal and warp code, so they stay guest state with the same layout.
- Load and release go through the block loader, with deferred RW destruction (TDestroyManager, one per frame).
- **The contract a native block manager must keep:** the same block set every frame, the same order of loader
  creation and release, and the same `+0x978` / `+0x979` flags at the same frames.

## C6. Audio streaming (ADX)

`CRI_SERVERS.md`: SVM, SJ and the ADX output are native, with their own oracles (PCM byte-identical in A/B).
- **What must keep holding:** stream data reaches the ring before the AX mix consumes it. No underruns: the exit
  report counts them.
- **Still to specify:** ADX exec, ADXF and the stream server (`CRI_SERVERS.md` step 3).

## C7. Video (Sofdec)

Not specified yet (U). Sofdec decodes in CRI's Idle server (`CRI_SERVERS.md` §1).

## G. General rules

- **G1 Determinism.** Two runs with the same inputs are byte-identical (G0). A native scheduler publishes at game
  events, never at host-time points.
- **G2 Heap layout.** A change of completion timing moves later heap allocations. It must be reported and shown
  gameplay-neutral (semantic A/B, poison test) and is never called parity. Known readers of stale heap bytes are
  errata decisions for the user (E1, E2).
- **G3 Savestates.** No host work in flight inside a state. Pending results are saved or reconstructible from guest
  state (W1 keeps them in guest memory).
- **G4 Tapes.** Tick-indexed input tapes cannot judge a timing change: comparisons align on PlayAction and use
  neutral input, or tapes re-recorded on the new timing.
- **G5 No game request is ever dropped or reordered** by the native scheduler (C2 ordering).
