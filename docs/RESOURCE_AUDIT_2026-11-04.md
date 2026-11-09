# Resource, loading, memory and streaming audit (2026-11-04)

The user's directive (verbatim in the stage-loading session): audit resource management, asset loading, memory
allocation and streaming; fix defects, inefficiencies, incomplete replacements and synchronisation issues; reconstruct
untranslated functions met on the way in C++20. Nothing here is committed or deployed. Every native switch named below
stays off by default unless stated.

## 1. How the audit ran

Five read-only audits over the native code (memory, file/asset I/O, background jobs and threading, audio/video
streaming, the untranslated load path), then every finding used here was re-read in the source before it was changed.
The supplementary tree `H:\ShadowTheHedgehogDiskGC_function_copies\world_distance\src` was consulted only through the
load-path ranking; nothing from it was reused.

## 2. Findings and their status

Legend: **fixed** (changed and tested), **open** (not changed; reason given).

### 2.1 Background jobs and savestates (`native_expand_jobs.cpp`, `savestate.cpp`, `native_cri.cpp`, `native_jobs.cpp`)

| # | Finding | Status |
|---|---|---|
| J1 | A state saved in expand mode 1/2 with a tagged TExpandBG pending, loaded in mode 3-5: the worker has no OSThread and mode 3+ never scanned for tags, so it never finished and its loader waited forever. | **fixed**: after a load, the mode 3+ reaper also publishes tagged workers (as mode 2) until none is left (`restored_tagged_`). Loading such a state with the feature off is still not detectable (no host record of pending tags). |
| J2 | A failed guest call during publication dropped the rest of the swapped-out queue; a run that continued (headless `clear_stop`) or a later save lost those jobs, and a lost W2 read left the in-flight count at 1 (reads stop for good). | **fixed**: the job it stopped in and the ones after it go back to the front of the queue. |
| J3 | The mode 1/2 creation stamp (tick, sequence) was host state outside the savestate: after a load, a job created at the saved tick could share a sequence number and publish out of creation order. | **fixed**: savestate version 30 carries it. |
| J4 | A decode-worker exception (bad_alloc) was stored in the future and rethrown through translated frames at publication. | **fixed**: caught in the task; the job is declined and decoded on the game thread. |
| J5 | Decodes queued for a discarded timeline stayed ahead of new ones after a load. | **fixed**: `load_state` clears the work queue. |
| J6 | Shutdown waited for every queued decode. | **fixed**: the worker returns once a stop is requested. |
| J7 | The CRI bridge (`SHADOW_NATIVE_CRI`, off) and the jobs bridge (`SHADOW_NATIVE_JOBS`, off) accepted states saved with the other setting (no CRI server would run; or bridge jobs run without the bridge). | **fixed**: such loads are refused with a message. |
| J8 | Savestate save/load dereference subsystems that exist only after assets mount (tests/tools without assets). | open: no game run reaches it. |
| J9 | `read_sync` is not re-entrant (shared `sync_` / `pending_`). | open: no re-entrant caller exists. |

### 2.2 File and asset I/O (`native_rtgcnfs.cpp`, `native_assets.cpp`, `prs_ahead.cpp`, `asset_providers.cpp`)

| # | Finding | Status |
|---|---|---|
| F1 | **The inline background read (`GEKKO_NATIVE_BG_READ=1`) wrote none of the original's stack frames** (0x80045150's 0x20, RwFread's 0x10, read's 0x40): the game copies stack leftovers into heap records, so TBGRead's stack (a heap block reused after the thread ends) could leak non-original bytes. | **fixed**: the loop now writes its frame, calls the allocator and each RwFread from it with the original return sites and registers, and runs RtGCNFS's read through `read_sync` (its own code, yields continued at once). `native_bg_read_tests` now compares the stack window after every read. |
| F2 | **RtGCNFS's read and open owners (default-on) were not frame-exact**: read kept its state in its own frame ('RTFS' tag at +8 ...) where the original keeps it in r23..r31; open, close, sync and abort did not write their frames. Found by F1's new stack compare. | **fixed**: read (0x40, _savegpr_23 area, state in r23..r31: file, destination, count, retries, position % 32, destination % 32, serve length, remaining, r31 = 0x8056EE40), the transfer (0x20 below it, r29..r31 saved, yield and error callback from its frame at the original sites 0x8047BA3C / 0x8047BBF8), open (0x120 with its path buffer, _rtGetFileNameFromDeviceName 0x20 and _rtFSBuildFileName 0x30 below it, the engine's strlen / strcpy slots called from 0x8047A744 / 0x8047A7DC), close / sync / abort (0x10). `native_rtgcnfs_tests` and `native_bg_read_tests` compare the stack window after every call. |
| F11 | The RtGCNFS oracle test was flaky (a read "could not be delivered" in some runs): every machine rewrote the fixture directory while an earlier machine had its files mapped. | **fixed**: written once per process; 3 of 3 runs pass. |
| F3 | Evicted textures were never re-parsed (`seen_` never forgot their file). | **fixed**: an eviction lets its file be parsed again. |
| F4 | The native texture manager matched `.one` / `.txd` case-sensitively, decode-ahead case-insensitively. | **fixed**: both case-insensitive. |
| F5 | A failed decode-ahead member never left the table: the stream was never queued again and its bytes held the 96 MiB budget. | **fixed**. |
| F6 | `PrsProvider::read` accepted a truncated PRS stream and zero-filled the rest. | **fixed**: shorter than the member's size is a failure (the provider is not registered today). |
| F7 | The texture manager reads archives with `std::ifstream` beside the asset system (replacements bypassed; bytes read and decoded twice). | open: a design change (pass the AssetId and the mapped span); textures are host-side only. |
| F8 | `AssetSystem::mount` does not clear `ResourceManager::entries_`; archive member pointers escape their lock. | open: one mount per process today. |
| F9 | The DVD API's auto-fatal / auto-invalidation flags are not in savestates. | open: low impact (only `DVDSet*`'s previous value). |
| F10 | 32-byte DVD alignment, the `AssetAddressSpace` token space and the DVD command blocks are still produced for callers. | open: guest-visible until RtGCNFS's callers are native (Phase 5+). |

### 2.3 Memory (`native_motion_store.cpp`, `guest_memory.cpp`, `game_memory.cpp`)

| # | Finding | Status |
|---|---|---|
| M1 | A zero-byte motion upload/fetch stopped the run (a zero-sized span resolves to no host memory). | **fixed**. |
| M2 | Motion store copies bypassed the verifier's journal and the access trace (a rollback left fetched bytes; traces logged a write as a read). | **fixed**: `GuestMemory::write` under the journal or a trace. |
| M3 | A savestate load zeroed, but did not decommit, a region committed past the saved size (save -> load -> save not identical; addresses that faulted then resolved). | **fixed**: the region is mapped again at the saved size. |
| M4 | Regions the state does not name were unmapped only after the saved ones were mapped (a load could fail half-way). | **fixed**: unmapped first. |
| M5 | The host PRS fallback retried with larger hints even when no hint could help (each retry zero-fills and decodes again). | **fixed**: only an output that reached the hint's end retries. |
| M6 | No native allocator: OSAlloc, RwMalloc/RwFree and operator new run translated; the 24 MiB MEM1 layout, the 4 GiB guest window and its VEH, the GameCube OS globals stay. | open: the architecture's next large step, not a defect. |
| M7 | POSIX: the flat map fails open and has no SIGSEGV handler. | open: Windows is the only platform built. |

### 2.4 Audio and video streaming

No ring-wrap, rate or channel defect; the only host thread (the audio consumer) uses a correct SPSC ring. Open: about
39k instructions of CRI middleware stay translated (ADX vsync/exec ~16.7k, the stream servers ~10k, Sofdec ~12k); the
zero-fill past end of file in the gcCi device writes byte by byte (one sector per file end).

## 3. Reconstructed functions

| Function | What | Status |
|---|---|---|
| 0x80043D0C, 0x80043D34 | in-place byte swap of halfwords / words (r0, r3, r4, CTR, CR0 as the original) | C++ owners under `GEKKO_NATIVE_ONE=1`; oracle in `native_one_tests` |
| 0x80043C80 | byte swap by format string (`c` skip, `s` halfword, `f i x` word; jump table 0x8051E158) | same |
| 0x80045150 | TBGRead's chunked read loop, now frame-exact (F1) | `native_bg_read_tests` |

The load-path ranking (untranslated functions by work during a load) found that loads are not CPU-bound: the
remaining translated load code is about 3-12 M guest instructions per load, a few milliseconds of host time; load time
is set by the game's pacing (the countdown passes, one read or loader step per pass, the loading screen's present).
Replacing more guest code will not shorten loads; the largest remaining unported data path is the RenderWare stream
readers (Phase 5, rwcore), which needs converted world assets and a host/guest boundary decision.

## 4. Validation

**Whole-memory A/B, pre-audit build vs the audit build** (`tools/stage_loading/mode_equal.sh` with EXEA/EXEB; cold
boots of stg0100, stg0402, stg0604; saved at game tick 202,500,000; every guest region compared byte for byte;
`.build/stage_loading/audit-ab/`):

| Mode | Result |
|---|---|
| default (no switch) | identical on 3 of 3 |
| `GEKKO_NATIVE_EXPAND_JOBS=4` | identical on 3 of 3 |
| `GEKKO_NATIVE_BG_READ=1` + `GEKKO_NATIVE_ONE=1` + `EXPAND_JOBS=5` | identical on 3 of 3 (W2 runs the loop at the reaper, whose stack the publication restores) |
| `GEKKO_NATIVE_BG_READ=1` alone | 4 / 2 / 8 heap bytes differ (one or two words per stage): stack leftovers the game copied into heap records. The new build holds the original's frame values there (0x8047B460, RwFread's return site; 0x00002000, the chunk RwFread saves from r31); the old build held unrelated leftovers (0x00520064, 0x39E4002B, 0xC026BC61). F1's effect, as intended. |

**The final build (with F2) against the pre-audit build** (`.build/stage_loading/audit-ab2/`, same method, plus the
60 s Westopolis tape in default mode, saved at tick 2,430,000,000):

| Mode | Result |
|---|---|
| default, 3 cold boots | 14 / 49 / 68 bytes differ: all inside guest thread stacks (back-chained frames in heap-allocated thread stacks, and the main stack in the image's bss at 0x8060BBxx-0x8060BDxx) |
| default, Westopolis tape 60 s | 18 bytes differ, the same kind (thread-stack words) |
| `GEKKO_NATIVE_BG_READ=1` | the same kind (F1 + F2) |
| BG_READ + ONE + mode 5 | identical on 2 of 3; stg0604: 41 bytes of the main stack (ONE's sync reads now leave read's original frames) |

What changed in those words, old -> new: the owner's own continuation entry 0x8047BC68 as a saved LR -> 0x8047BA40,
the original's return after the transfer's `bl OSYieldThread`; unrelated leftovers (0x0001A000, the text "04_E") ->
the original's back chains and saved registers (0x807A47E0, 0x0000000C, 0x00000020). Nothing outside stack memory
changed: no object, heap record, game variable or gameplay outcome differs, on the cold boots or over the tape.

**CTests** (`.build/loadfix`, Release; the original words in the reference interpreter against the owners):

| Test | Result |
|---|---|
| native_rtgcnfs_tests | 35 calls (14 also through read_sync), 0 failures, stack window compared after every call; 3 of 3 runs |
| native_bg_read_tests | 207 calls, 69 chunked reads, 2,105,696 bytes, 741 chunks, 0 failures, stack window compared after every call (opens and closes included) |
| native_one_tests | 586,027 checks, 0 failures (the swap leaves added: every size class, negative counts, the game's two format strings, every character class) |
| native_expand_jobs_tests | 0 failures (savestate blocks at version 30) |
| asset_import, game_memory_owner, native_assets | passed (before F2) |

The test fixtures' stand-ins (identical on both machines) gained the engine's strlen / strcpy, and the stack compare
masks the native DVD API's 'NFCB' completion trampoline (a fixture frame no game state reaches: in the game that
callback runs from the DVD interrupt), copying its bytes to the owner side.

## 5. Measured effect, and what was not verified

- **Load time:** not changed by this audit, and not re-measured. The fixes are correctness (stack bytes, savestates,
  caches, error paths); the swap owners and the PRS retry change remove host work in the low milliseconds per load at
  most, below what the load-time tools resolve. Loads stay set by the game's pacing (section 3).
- **Not run:** a savestate save -> load -> save round trip for M3 / M4 / J3 (version 30), a mid-load save in modes 1/2
  loaded in mode 3+ for J1, the remount and POSIX paths. The CTests cover the version-30 blocks; nothing in-game.
- **The build of `ShadowTheHedgehog` currently fails** in `runtime/src/shadow_main.cpp:3477`
  (`shadow::peripheral::combine`, edited at 10:33 by another session, not by this audit). Every run above used the exe
  linked at 10:26, which holds every runtime change of this audit; the later edits were tests only.
