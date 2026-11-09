# The disc system: what is left (map, 2026-10-29)

The next target for complete elimination. This map comes from reading the source, `extract/native-owners.txt`,
the release tree's generated code and `recomp_pruned.csv` (build 13:59), and `game/sys/main.dol`. Nothing here was
run. Addresses are PAL canonical.

## Summary

- **The hardware and the SDK internals are already gone.**
  - No DI register model is left: page 0x0C006000-0x0C0063FF is retired (`cpu.cpp:1133`), and any access stops the
    machine.
  - No FST in guest memory, no disc image, no DVD thread, no DI interrupt.
  - The SDK DVD library between 0x803798F8 and 0x8037E114 is pruned: DVDLow*, __DVDFSInit, DVDConvertPathToEntrynum,
    the state machine, the waiting queue, DVDCompareDiskID, __fstLoad and the DI handler (probably 0x80379938).
- **The console model survives as a contract.** 31 SDK entry points are native owners in
  `runtime/src/gekko/native_file_api.cpp` (table :27-51, `install()` :313-548). Over host files (`AssetNamespace`
  + `AssetSystem`) they still speak the console's language:
  - `DVDFileInfo` and command-block layouts, with SDK state codes (FATAL -1, END 0, COVER_CLOSED 3, CANCELED 10);
  - async callbacks (`(result, block)`, entered through the 'NFCB' continuation frame, LR 0x8037CF44);
  - drive status (always 0), the 32-byte disc ID / title block, BI2 at 0x817FE000;
  - the +32-byte read tail rule (`kTailAllowance`).
- **Four live translated clusters still talk to that contract**, and they are the real work:
  - the CRI gcCi device;
  - RenderWare RtGCNFS;
  - the game's directory scan and disc-error screen;
  - OSInit's DVD bring-up.

  Once nothing live calls a DVD API, the 31 owners and the contract can be deleted.

## The native side (runtime)

| Piece | Where | What it is |
|---|---|---|
| DVD SDK entry points (31 rows) | `native_file_api.cpp:313-548`, `extract/native-owners.txt:9-40` | Full native owners over host files: Open/Close, ReadAsyncPrio, ReadPrio, ReadAbsAsyncPrio, OpenDir/ReadDir, status, cancel, DiskID/Inquiry, Init and flags. Reboot paths (__DVDPrepareResetAsync, DVDCancelStreamAsync) stop the machine. |
| Path namespace | `asset_namespace.cpp` (`from_directory` :95, `resolve` :271) | Replaces the FST walk. Built from `files/` in pre-order to reproduce the disc's 3,615 entry numbers. The FST is a test oracle only (`asset_namespace_tests.cpp:136`). |
| Token space | `asset_runtime.{h,cpp}` (`AssetAddressSpace`) | 64 KiB pages from 0x60000000, written into `DVDFileInfo.startAddr`. The base sits above the old disc offsets on purpose. `ResourceManager` and `StreamingScheduler` have no production callers. |
| Mount / providers | `asset_mount.cpp`, `asset_providers.cpp` | `FileProvider` (mapped files, background preopen) and `MemoryProvider` are live. `ArchiveProvider` and `PrsProvider` are unmounted. |
| Boot wiring | `system.cpp` `mount_assets` :1597, `enable_file_api` :1675, `apply_boot_configuration` :1729 | Writes the 0x20-byte disc header at 0x80000000 and BI2. The FST pointers at 0x80000038/3C stay zero. `shadow_main.cpp` requires GUPP8P and mounts `files/`. |
| Renderer side channel | `native_assets.cpp` (`OneArchive::parse`, `notify_file` :390) | Called from `NativeFileApi::read`. |
| PRS codec | `prs.cpp`, owner of 0x80043FE0 | A codec, not disc. |
| Disc-error ARAM backup | `native_disc_error_backup.cpp` | Built but dormant: its 3 sites are commented out in `lift-sites-native.txt`. **The far-object session is bringing it live as ARAM step 4** (sites 0x8032E23C, 0x8032D844, 0x8032D690). |
| Stub | `os_stubs.c:31` `DVDOpen` | Clean-room stub, not wired to the guest. |

## The guest side still live (translated)

### 1. CRI gcCi: the cvFs "DVD" device
- Interface table at 0x80569FC8, returned by gcCi_GetInterface 0x803CEA9C.
- Registered by the ADX/cvFs init 0x803C5750, which the game's audio init 0x801D874C calls.
- Entries:
  - Open 0x803CE59C ("DVDOpen fail.(gcCiOpen)"), Close 0x803CE3D8, GetFileSize 0x803CE7AC;
  - ReqRd 0x803CDE84 (ReadAsyncPrio / ReadPrio), StopTr 0x803CDCE4 (Cancel);
  - ExecServer 0x803CE924 (CommandBlockStatus, TransferredSize), Seek 0x803CE30C, Tell 0x803CE2AC;
  - GetStat 0x803CDC80, Get/SetSctLen 0x803CDC20 / 0x803CDB3C (sector length), GetNumTr 0x803CDADC.
- Reached only through the table. ADXF, cvFs, Sofdec (MWSFD) and the ADX stream server sit above it.
- The stream ring's ARAM transfers (0x803CF0D0, 0x803CF21C) are already native.

### 2. RenderWare RtFSManager + RtGCNFS (0x8047A090-0x8047C7xx)
- Init 0x8047C45C, reached from RW app init 0x80051684 → 0x80051238. The device table is built at run time: open
  0x8047B874, close 0x8047B988, read 0x8047B9C8, plus 0x8047BC60 and 0x8047C0A0.
- The read calls DVDReadAsyncPrio and then switches 13 ways on DVDGetDriveStatus (jump table 0x8047BA68).
- 0x8047C1E8 switches on CommandBlockStatus (table 0x8056EF6C). 0x8047C1A4 is a drive-status helper, and 0x8047C3D4
  cancels. "Unaligned async read failed" lives here.
- Not traced yet: the exact path from RwStreamOpen into RtFS.

### 3. The game's loaders and its disc-error screen
- **Directory scan 0x80044ED4:** the only game code that calls DVDOpen, DVDOpenDir, DVDReadDir and DVDClose
  directly. Chain 0x80331E9C → 0x803313E0 → 0x80044ED4.
- **ONE archives:**
  - lookup 0x8004C938 (entry index 0x8004CA50, strcmp);
  - extract 0x8004C770 (PRS plus memcpy);
  - sync open 0x8004CC94 (file load 0x80037200);
  - async manager TOneFileAsyncManager 0x8004D564, with its six loaders at 0x8004AE60-0x8004C408;
  - parse 0x8004CE1C, through RW streams.

  They reach the disc only through RW streams or the file load. They are not disc code themselves, but they would be
  a natural place to load .one files natively.
- **Disc-error screen:**
  - game main 0x800510C0 calls DVDSetAutoFatalMessaging and, at 0x800511F0, the per-frame drive check 0x8032D444;
  - 0x8032D444 calls the error screen 0x8032D4CC (also reached through its thread form 0x8032D3D4);
  - backup ARAlloc 0x8032E200, save/restore around 0x8032D760;
  - localized texts from 0x8050EE62 ("please close the Disc Cover.").

  It is unreachable in practice only because DVDGetDriveStatus always returns 0. The code is live and runs every
  frame.

### 4. OS bring-up
- OSInit 0x80370E68 calls DVDInit and DVDInquiryAsync. 0x80051028 calls DVDInit.
- OnReset 0x80371E64 calls __DVDTestAlarm through the OS reset record.

## Cut points (proposed order)

Each step follows the usual method: understand, write the native code, A/B on the Westopolis tape (journal,
savestate classify, frame, audio), prune, then delete.

1. **Disc-error screen.** Lower the drive-check call at 0x800511F0 to nothing, or own 0x8032D444 as "the drive is
   always ready". That prunes 0x8032D4CC, 0x8032D3D4, 0x8032E200 and the texts' only reader, and it makes the
   dormant ARAM backup unnecessary. **Coordinate with the far-object session first:** its ARAM step 4 is wiring the
   backup's sites now, so the two must not both land.
2. **Directory scan 0x80044ED4.** Own it outright over `AssetNamespace` (small, one caller chain).
3. **CRI gcCi → a native cvFs device.** Replace the interface table at 0x80569FC8 / gcCi_GetInterface 0x803CEA9C
   with host open/read/stat. Then ADXF, Sofdec and ADX never see a command block, and the sector-length entries
   lose their meaning.
4. **RtGCNFS → a native RtFS file system** at its init 0x8047C45C, or RW's file-function table one level up.
   That prunes 0x8047B874-0x8047C3D4 and the drive-status switches.
5. **OS bring-up.** Lower the DVDInit / DVDInquiryAsync sites in OSInit and 0x80051028, and refuse __DVDTestAlarm
   in OnReset.
6. **Delete the contract.** No live caller is left, so the 31 owners in `native_file_api.cpp` go, along with:
   - `DVDFileInfo` and command-block writes;
   - the 'NFCB' continuation;
   - the disc ID / BI2 serving, where nothing else reads them (check OS globals first);
   - the token space's disc-offset bias;
   - the `os_stubs.c` stub.
7. **Optional, higher up: the ONE layer** (0x8004CC94, 0x8004D564, 0x8004C938, 0x8004C770). Load .one archives
   natively and bypass RW streams for them.

## Cut 1: the disc-error screen (2026-10-29, built and A/B-verified; not committed)

### What the module is

The game's disc-error module, 0x8032D138-0x8032E284. Every user of its small-data words (0x5734-0x577C(r13)) is
inside it.

| Entry | Role | Reached from |
|---|---|---|
| 0x8032D444 | Per-frame drive check. Once (while 0x5734 is 0) it reserves the ARAM backup (0x8032E200: 0x20120 bytes, or 0x120F00 when 0x80375150 reports 1). Then it reads DVDGetDriveStatus, stores 0 to 0x5760, and on states -1, 4-6 or 11 enters the screen and loops. | game main 0x800511F0 |
| 0x8032D4CC | The screen: texts from 0x8050EE62, SE and rumble stop, backup save/restore (0x8032D760, 0x8032D7CC), text drawing 0x8032DB70-0x8032E1FF with a jump table at 0x8055E2B0. | 0x8032D444, 0x8032D3D4 |
| 0x8032D3D4 | RtGCNFS's drive-error callback, stored at the device's +0x5C by main (0x8005127C). Stores 0 to 0x5760. States 2-6 open the screen; any other state returns 1. | RtGCNFS (0x8047BA84-0x8047C2B4) |
| 0x8032D138 | The loaders' hold query: the screen's thread queue while 0x5758 is set, else 0. | 0x80043D70, 0x80045694 |
| 0x8032E254 | Init: the current thread into 0x5774, thread queues 0x5764 and 0x576C. | game main 0x8005126C |
| 0x8032D150 / D17C / D1D0 | The reset button and the reset handler. Not disc code; they stay. | main, 0x80306A7C, 0x801E2234, 0x80306DA8 |

### Why it reduces to almost nothing

DVDGetDriveStatus is a native owner that returns 0 on every call. With status 0:
- the drive check never enters the screen;
- both RtGCNFS switches (tables 0x8056EE40 and 0x8056EF6C, indexed by status + 1) pick the case that calls the
  callback with 7, or no callback at all.

So:
- 0x5758 (set only inside the screen) stays 0;
- 0x5760 is only ever stored 0, since its non-zero stores are inside the screen and in 0x8032D3D4's 2-6 branch;
- the backup's address and size (0x5734, 0x5738) and the init's words are read only by the screen's paths.

### The change

- `extract/lift-sites.txt`: the calls at 0x800511F0 (drive check) and 0x8005126C (init) become `nothing`. The
  screen's rumble-stop site 0x8032D528 is gone with the screen.
- New owners in `gekko/native_disc_error.{h,cpp}` (`extract/native-owners.txt`):
  - 0x8032D138 returns 0;
  - 0x8032D3D4 returns 1, and stops the machine, naming the state, on 2-6, which the native drive cannot produce.
- Removed: the native renderer's `DrawProducer::DiscError` (the screen's GXBegin at 0x8032E088), and the liveness
  roots 0x8032D528 and 0x8032E088 that those two runtime literals made.
- The far-object session's dormant ARAM backup for the screen (`native_disc_error_backup`, ARAM step 4) was removed
  from that change; its sites never ship.
- The generator (both configurations, scratch runs) prunes 16 module rows plus their now-unreferenced helpers
  (651 rows pruned against 618 lifted). Every lowered site is emitted (lifted 1713 of 1713, tests tree 1041 of
  1041).

### Evidence: same-tree A/B (diag, Westopolis tape, 40 s, `--button-style gamecube`)

- **Arms.**
  - Base: the same tree with the two `nothing` rows, the two owner rows and the owner install reverted, and the
    rumble site restored. Generator: 1712 of 1712 sites.
  - Cut: 1713 of 1713 sites. All 21 MSL contracts pass.
- **What the cut arm prunes beyond the base** (25 rows):
  - the 16 module rows;
  - ARAlloc 0x80382884, ARFree 0x803828EC and their internal helpers. The backup reserve was the last live guest
    ARAlloc: the banks, the stream ring and the motion pool are already native;
  - a few helpers only the screen used.
- **The game-memory journal** (every memcpy and PRS expansion with its hash): 110,307 lines in both arms, 0
  differing.
- **The frame at 40 s:** byte-identical PNG (sha256 1387078...).
- **Savestates at 2, 10, 20, 30 and 40 s**, compared byte by byte with `tools/state_delta.py`, nothing excluded.
  The CPU blocks and the trailing state are identical. 20 image bytes plus 2-6 heap bytes differ, and every one is
  attributed:
  - 0x805F1C34 / 0x805F1C38 / 0x805F1C74 (0x5734, 0x5738 and 0x5774 of r13): the backup's ARAM address and size and
    the init's thread word. They are 0 in the cut arm, and only the pruned screen read them.
  - 0x805F21B0-0x805F21BB (0x5CB0, 0x5CB4 and 0x5CB8 of r13) and the block-length entry 0x8053F2B0: the guest ARAM
    allocator holds one block fewer (0x20120). Nothing translated reads it in the cut arm: ARAlloc and ARFree are
    pruned, and ARInit 0x80382960 is a native owner.
  - 0x8060C580: offset +0x38 of the live 0x40-byte frame of fn_801E2210 (entered from 0x80049078). That function
    accesses only +0x08..+0x18, +0x20..+0x2F and +0x3C, and hands only r1+8 to a callee, so the slot is never read.
    Its residue came from the frames of the drive check's first-frame callees.
  - The heap words at 0x80FE7BB4, 0x80FF169C, 0x8131A9AC, 0x8131B654 and 0x8154F84C hold 0x8047BBF8 in the base and
    0x8047BA40 in the cut. This is the LR-save slot (SP+4) of RtGCNFS's read frame:
    - the translated callback saved its return address 0x8047BBF8 there;
    - the native owner makes no frame, so the slot keeps the LR from the earlier call at 0x8047BA3C;
    - a callee writes that slot before it reads it.
- **Scripts:** `.build/validation/msl-bench/ab_pair.ps1`, `classify.py` and `dump.py`. The data is in
  `cut1-{base,cut}-{journal,states}` and `cut1-delta-*.json`.

## Cut 2: the directory scan (2026-10-29, built and verified; not committed)

### Evidence

- **Owner:** `gekko/native_directory_scan.{h,cpp}` (`extract/native-owners.txt` 0x80044ED4).
- **Oracle:** `runtime/tests/native_directory_scan_tests.cpp` (CTest `native_directory_scan_tests`), the original
  words in the reference interpreter against the owner. All 8 cases equal:
  - the root, with and without filters, by both of its names;
  - a subdirectory, an empty directory, a missing path, and a file path.

  Two more checks: the bare-name-from-root quirk is pinned, and an over-capacity directory is refused. The first
  run caught a real difference: the original's prologue stores LR into the caller's frame on its failure paths too.
  The owner now writes its frame first on every path.
- **Builds:** both diag arms built from one tree (base: the owner row and install reverted).
  - The cut arm passes all 25 native-math contracts and the boundary check, with 1715 of 1715 sites.
  - These tests pass: `native_file_api_reference_tests`, `native_file_api_tests`, `gekko_core_tests`,
    `lift_sites_reference_tests`, `lift_l1_evidence`, `lift_native_math_evidence` and `native_liveness_roots`.
- **Westopolis A/B** (40 s, `--button-style original`):
  - the journal is 110,305 lines in both arms, 0 differing;
  - the 40 s frame is identical;
  - the savestates at 2, 10, 20, 30 and 40 s are byte-identical files.

  As expected, since the scan never runs.
- **Found on the way:** the wholesale roots regeneration (far-object session) made a `--trace-stale-contacts` hook
  literal at `system.cpp` root the pruned matrix-copy duplicate 0x8040D198, and the math gate failed. The hook could
  never fire, so it is removed and the roots are regenerated.
- **DVDOpenDir and DVDReadDir deleted** (2026-10-29, after cut 2):
  - Removed from `native_file_api.{h,cpp}`: both owners, their rows, the name-pool hand-off (`set_name_pool`,
    `name_pool_`), the `directory_reads` stat and the report's dir-reads field.
  - Removed from `AssetNamespace`: `name_pool_offset`. Removed from the tests: the file API's directory test and the
    reference test's DVDOpenDir/DVDReadDir comparisons.
  - The scan oracle now runs the ORIGINAL SDK DVDOpenDir/DVDReadDir words over a published directory image
    (`support/fst_oracle`), so its original side is all original words.
  - Kept: the arena limit (0x80000034), still BI2 less the disc directory table's size. The game's heap layout
    depends on it; nothing is stored in that region now (`System::arena_high_`).
  - The generator, lifted and unlifted (scratch runs), prunes the SDK bodies 0x8037B130 and 0x8037B1F0. Every site
    is emitted (1715 and 1043). The liveness roots were regenerated with the tool (the two rows went) and
    `--check` passes.
  - Verified: the diag build passes all 25 native-math contracts, with 0x8037B130 and 0x8037B1F0 in
    `recomp_pruned.csv`.
    - These tests pass: `asset_namespace_tests`, `native_file_api_reference_tests`, `native_directory_scan_tests`
      (now all original words on its original side), `gekko_core_tests`, `native_file_api_tests`,
      `lift_sites_reference_tests`, `lift_l1_evidence` and `lift_native_math_evidence`.
    - `native_liveness_roots` fails only on 0x804D24EC, a new literal in the far-object session's in-progress
      `rw_driver/far_objects.h`. That session regenerates the file when its change settles.
    - Westopolis A/B against the verified cut-2 exe (`--button-style original`): journal 110,305 lines, 0 differing;
      the 40 s frame is identical; the savestates at 2-40 s are byte-identical files.

### What it is

`fn_80044ED4(r4 = out, r5 = path, r6 = filter or 0)` returns a count. It is the only game code that calls
DVDOpenDir and DVDReadDir.

1. `DVDOpenDir(path, &dir)` (dir at r1+0x14). On failure: `*out = 0`, return 0.
2. `temp = 0x80049BB8(cap * 0x108, 2)`: the game's heap allocator, heap 2. `cap` is the word at -0x7BF0(r13) =
   0x805E4910, 0x500 in the DOL.
3. Each `DVDReadDir(&dir, &entry)` (entry at r1+8: +4 is-directory, +8 name pointer):
   - with a filter, a name that does not contain it (strstr 0x803AD710) is skipped;
   - `rec = temp + i * 0x108`, `strcpy(rec, name)` (0x803ADB50): only the name and its NUL are written, and the
     rest of the record keeps what the heap block held;
   - a directory gets +0x104 = 1 and +0x100 = -1;
   - a file gets +0x104 = 2, then `DVDOpen(rec)` with the bare name, which resolves from the root (the game never
     changes directory). On success +0x100 = the file length (DVDFileInfo +0x34) and the handle is closed; on
     failure +0x100 = -1.
4. `buf = 0x80049BB8(count * 0x108, 2)`, `*out = buf`, `memcpy(buf, temp, count * 0x108)` (the native memcpy
   owner), `0x80049B5C(temp)` (free), return count.

### Who uses it

`StageSequence::Manager` (vtable slot 0x80559298 → 0x802CE764) → 0x80331388 → 0x80331E9C → 0x803313E0:
- it scans `"."` (0x805FAF18) with no filter;
- for each file record whose name contains `".sfd"` (0x805FAF1C) it copies the name into a 12-byte slot of its
  object (0x80014CF4 into +0x10 + 12i) and counts the slots at +0x310;
- it frees the array.

This is the movie list. The disc root holds 241 entries (72 directories, 8 `.sfd` files), under the cap of 1280.

The Westopolis tape never reaches it: its file-API report shows `dir-reads 0`, and DVDOpenDir/DVDReadDir are
absent from its hit list.

**The retail game never reaches it.** Slot 0x80559298 is `StageSequence::Manager`'s exec (vtable +0x0C). It
switches on the mode word at 0xD20(r13), jumping through the table at 0x805592A8 indexed by mode + 1, and builds
the sequence object for that mode. The movie-list constructor 0x80331388 is case index 6, which is mode 5.
- Every writer of the mode word: 0x802CE6F8 stores -1 (manager init), and the setter 0x802CF734 is called only
  with the constants 0, 1, 3, 4, 6 and 7 (0x802BB894, 0x802BB904, 0x802BB9BC, 0x802BD6E8, 0x802C14A8, 0x802C1538,
  0x802C48BC, 0x803424BC).
- No data word holds the setter or the mode word's address, and no lis/addi pair or r13 addi forms that address.

So mode 5 (and mode 2) is never selected. This is a debug movie selector that lists every `.sfd` on the disc,
whose case the generator keeps only because the jump table is a verified dispatch edge. The proof for this cut is
therefore the interpreter oracle; a gameplay tape cannot reach the code.

### The rewrite

`NativeDirectoryScan`, the owner of 0x80044ED4, over `AssetNamespace`:
- It enumerates the directory in the namespace's own order (the order the DVDReadDir owner serves) and takes each
  file's length from the namespace entry that the bare name resolves to from the root, as the original's DVDOpen
  did.
- It makes the same guest heap calls in the same order:
  - alloc(cap * 0x108, 2), with `cap` read from 0x805E4910 at call time;
  - the records written into that block exactly as above (name, NUL, +0x100 and +0x104 only);
  - alloc(count * 0x108, 2), then memcpy, then free.

  The calls use the NativeMotionStore `call_guest` pattern (`recomp::call_indirect`, stop if the call does not
  return). So the heap's allocations, and the bytes left in the freed temp block, equal the original's.
- A directory with more entries than `cap` overflowed the original's block. That is refused (stop, naming it)
  rather than reproduced.
- There is no DVDDir or DVDFileInfo, no name pool, and no open or close.

### Afterwards

- DVDOpenDir 0x8037B130 and DVDReadDir 0x8037B1F0 lose their last caller. Their owners, their tests and the name
  pool below BI2 (check its other users first) are deleted.
- DVDOpen and DVDClose lose one caller.

### Proof plan

1. **An owner oracle test.** The original fn_80044ED4 runs in the reference interpreter and the owner runs on the
   same guest image (the GameMemory owner-test pattern). Cases:
   - the root with no filter;
   - the root with the `.sfd` filter;
   - a subdirectory;
   - a missing path.

   Compare every guest byte (out array, temp block, heap headers) and the returned registers.
2. **No reaching tape exists:** mode 5 is never set (above).
3. **The Westopolis A/B**, for no change where the scan never runs, plus the prune check (fn_80044ED4 emits only a
   thunk; DVDOpenDir and DVDReadDir pruned once their owners go).

## Cut 3: CRI's file device gcCi (2026-10-29, written; verification pending)

### The device, from its disassembly

- **Registration.** cvFs registers the device by passing gcCi_GetInterface 0x803CEA9C (formed at 0x803C57B0) to
  its add-device call 0x803C8D24, then calls it through that record. It returns the interface table 0x80569FC8,
  and every entry is reached through the table.
  - GetInterface zeroes the path prefix 0x805C2B70 (nothing else ever writes it), the error callback and the
    global block's first 12 bytes.
  - 0x803CEB08 (from the ADX init) only sets the read-mode flag 0x8056A030. 0x803CEB14 belongs to the ADX
    stream's reporter, not the device.
- **State.** All in guest memory, touched only by gcCi's own functions (every formation of 0x805C1BB8-0x805C2C70
  checked).
  - Global block 0x805C1BB8: +0 last status, +4 device state byte, +8 cancel-in-progress, +0x0C / +0x10 error
    object / function.
  - 40 handles of 0x64 bytes from 0x805C1BCC: +0 used, +2 state (0 idle, 1 done, 2 reading, 3 error), +8 buffer,
    +0x0C last status, +0x10 sector length, +0x14 length, +0x18 sectors, +0x1C position, +0x20 transferred,
    +0x24 requested, and a DVDFileInfo at +0x28.
- **The entries.**

  | Entry | What it does |
  |---|---|
  | ExecServer 0x803CE924 | Per frame: each reading handle's block status. END completes the transfer: count, position, zero fill past the end of file. FATAL means error. CANCELED reads the transferred size. |
  | EntryErrFunc 0x803CE910 | Registers the reporter. |
  | GetFileSize 0x803CE7AC | DVDOpen and DVDClose of prefix + name, with `\` turned into `/`. |
  | Open 0x803CE59C | First free handle, DVDOpen, field set-up. |
  | Close 0x803CE3D8 | Stop, DVDClose, zero the handle. |
  | Seek / Tell 0x803CE30C / 0x803CE2AC | Sector position, clamped to 0..sectors. |
  | ReqRd 0x803CDE84 | One transfer at a time across the device. Clipped at the end of file, rounded to 32 bytes, through DVDReadAsyncPrio, or DVDReadPrio when the flag is set. |
  | StopTr 0x803CDCE4 | DVDCancel, then a 2000 ms OSGetTime spin until the status is END or CANCELED. |
  | GetStat / GetSctLen / SetSctLen / GetNumTr | Field reads and writes. SetSctLen checks the current length's alignment, not the new one's. |

  Every error path calls the reporter with (object, message 0x80528A40 + n, handle or 0).

### The rewrite

`gekko/native_cri_file_device.{h,cpp}` owns all 14 entries and keeps the game's state layout.
- A request copies the bytes at once through `NativeFileApi::device_read`. That helper shares the file API's
  asset copy, counters, asset trace and renderer notification, with no command block.
- The handle reads "reading" until the next server pass, as before, and the server completes it with the
  original's bookkeeping.
- Of the DVDFileInfo only the asset token (+0x58) and length (+0x5C) are kept. There is no DVDOpen, DVDRead,
  status poll, cancel spin, or cache maintenance.
- Refused rather than reproduced: an undeliverable read, and a path past the original's 0x104-byte buffer.

### Evidence (2026-10-29)

- **Oracle `native_cri_file_device_tests`:** 153 calls through every entry and path, 0 differences. Compared after
  each call: the result, the non-volatile registers, the global block, the prefix, every handle byte (outside the
  old DVDFileInfo, which the owner must leave untouched), every buffer, and every error report. It found two real
  defects on the way:
  - **My message base.** The reporter's messages are at 0x80518A40 (lis -0x7FAE, addi -0x75C0), not 0x80528A40.
  - **A file-API defect.** A zero-length read (gcCi asks for one at an exact end of file) completed FATAL, because
    `GuestMemory::write_in_place` resolves no span of size 0. The SDK's bounds check admits it, and the console
    completes it. `NativeFileApi::copy_asset` now delivers nothing successfully. The far-object session's earlier
    tapes showed `failed 0`, so nothing recorded had reached it.
- **Builds:** both diag arms built from one tree, with 1715 of 1715 sites and 25 native-math PASS lines (cut: 138
  owner rows; base: 124). These tests pass: `native_file_api_reference_tests`, `native_directory_scan_tests`,
  `native_cri_file_device_tests`, `gekko_core_tests`, `native_file_api_tests`, `lift_sites_reference_tests`,
  `lift_native_math_evidence` and `native_liveness_roots` (after the roots were regenerated for the message-base
  literal).
- **Westopolis A/B** (40 s, `--button-style original`):
  - The journal is 110,305 lines in both arms, 0 differing, and the 40 s frame is identical.
  - The file API saw the same 2,190 reads and 12,809,120 bytes. 18 fewer DVDOpen and 116 fewer callbacks: CRI's
    opens and reads now go through the device.
  - The mixed audio (`--audio-dump`, 5,111,680 bytes) is byte-identical, over 2,092 ADX stream transfers.
  - Savestates at 2-40 s: every differing byte is either an open handle's old DVDFileInfo (unread) or below r1 in
    the stream server thread 0x805AD070's stack (dead callee frames). Nothing else differs.
- **Not covered by a tape:** Sofdec movie playback (the Westopolis tape boots straight into the stage). The oracle
  covers the device's behaviour for any caller; a movie replay is still worth running.

## Cut 4: RenderWare's RtGCNFS (2026-10-30, built and verified; not committed)

The full specification from the disassembly is `docs/RTGCNFS_SPEC.md`.

### The rewrite

`gekko/native_rtgcnfs.{h,cpp}` owns the device functions that touched the disc: open, close, read, sync and abort,
plus read's two continuation entries 0x8047BC64 and 0x8047BC68. The internal submit 0x8047B9C8 and its DVD callback
0x8047B9C0 lose their only caller and are pruned.
- **read** is the whole algorithm: the 64-bit position, the 0x800-byte cache, 32-aligned direct transfers, and the
  dead async branch with its OSReports.
- **It yields exactly where the original did.** The original yields (OSYieldThread) at the top of every chunk and
  once per transfer. The native read keeps its state in a tagged frame at the original's stack depth, hands the CPU
  to OSYieldThread, and resumes at a continuation entry, so a savestate taken inside a yield resumes. Each transfer
  still calls the file system's drive-error callback with RenderWare's "no error" state 7. The cache-serve memmove
  is called with the original call site's return address 0x8047BF74, which the game-memory journal records.
- **open** reproduces RenderWare's path helpers natively: `_rtGetFileNameFromDeviceName`, the device name "" at
  0x2A44(r13), and `_rtFSBuildFileName`. It then resolves the path in the namespace.

### Evidence

- **Oracle `native_rtgcnfs_tests`:** 39 calls through open, read, setPosition, sync, abort and close, sync and
  async, against the original words. 0 differences. Compared after each call: results, registers, every yield (in
  order, with its stack pointer), every error-callback call and report, the file system object, G, the buffers,
  the caches and the records (outside the old DVD block, which the owner must leave untouched).
- **Builds:** both diag arms built from one tree (cut: 143 owner rows; base: 138), 1715 of 1715 sites, 25
  native-math PASS lines. The focus tests pass, including all three disc oracles and `native_liveness_roots`.
- **Westopolis A/B** (40 s):
  - the journal is 110,305 lines, 0 differing;
  - the 40 s frame is identical;
  - the mixed audio is byte-identical;
  - the file API now sees **0 opens**: every RenderWare and CRI file access goes through the native devices.
- **Savestates:**
  - **Attributed:**
    - the RtFile records' old DVD command block (+0x48..+0x6B other than the state, and +0x78);
    - dead stack below r1;
    - RwMatrix pad slots (stack residue, as in the MSL cut);
    - at 2 s, thread 0x80FF17B8 parked inside a read's yield: its live read frame, and its saved r23-r28, where the
      original's read held its locals in callee-saved registers and the native read holds them in its frame;
      both return the same values to the caller.
  - **Most likely, not proven:** stack bytes of loader threads that had exited by the time of the save: 0x80FE7Bxx
    (thread 0x80FF17B8, off the thread list after 2 s), and 0x80FF16xx and 0x8154F8xx (thread records in state 0).
    The bytes at 0x8131A9xx / 0x8131B6xx look the same (frame back chains, read return addresses, the file system
    pointer), but no owner was established.
  - **Nothing propagates:** journal, frame and audio are identical, and at 40 s only one byte outside the old DVD
    blocks differs.

## Cut 5: the OS bring-up (2026-10-30, built and verified; not committed)

There is no drive to bring up. Four call sites and one data root go, all in `extract/lift-sites-native.txt`:

- `site 0x803712E8 nothing` (OSInit: DVDInit) and `site 0x80051044 nothing` (game init 0x80051028: DVDInit). The
  native file API's DVDInit owner did nothing.
- `site 0x80371328 nothing` (OSInit: DVDInquiryAsync). It fetched the drive's revision record, and its callback
  0x80370E2C wrote the OS device-code word 0x800030E6. That word's only reader (0x803B419C, in 0x803B3F90) is
  pruned, and nothing else reads the info buffer.
- `site 0x800510DC nothing` (game main: DVDSetAutoFatalMessaging(1)). No fatal screen exists; the owner only kept a
  flag.
- `retire 0x805612F0 0x80371E64`: the alarm module's OS reset record {OnReset, priority -1, next, prev}. Only
  OSResetSystem walks the reset queue, and it is a native owner that ends the program, so OnReset and its
  __DVDTestAlarm call have no caller.

After this cut, no live code calls a DVD owner directly.

### Evidence

- **Builds:** both diag arms from one tree. Cut: 1719 of 1719 sites, 7 retired roots. Base: 1715 of 1715, 6
  retired roots. Both have 25 native-math PASS lines.
- **Focus tests** (Release `tests` preset, 10 of 10 pass): the file API reference, directory scan, gcCi and RtGCNFS
  oracles, plus `gekko_core_tests`, `native_file_api_tests`, `lift_sites_reference_tests`, `lift_l1_evidence`,
  `lift_native_math_evidence` and `native_liveness_roots`.
- **Westopolis A/B** (40 s, `--button-style original`):
  - the journal is 110,305 lines, 0 differing;
  - the 40 s frame is identical (B477B9B5FF7B);
  - the raw audio is byte-identical (5,111,680 bytes);
  - the cut arm's file API shows 0 opens and 0 callbacks, with 2189 device reads (12,809,088 B).
- **Savestates** (2, 10, 20, 30 and 40 s, every guest region compared): at every time point exactly 25 bytes differ,
  all attributed:
  - 2 bytes are the device-code word 0x800030E6 (base 0xD050, cut 0), which the inquiry callback no longer writes;
  - 23 bytes are the inquiry's command block and info buffer at 0x80586C40.., which are no longer filled.
  - The CPU state and header are identical. No dead-stack difference and no unattributed byte.

## Loose ends found while mapping

- `extract/liveness-roots.txt` is about 180 rows behind what `tools/ppcrecomp/liveness_roots.py` generates from the
  current runtime sources (literals other sessions added). That is why `native_liveness_roots` fails. Cut 1 changed
  only its own rows.
- The tests tree (no lifted renderer) kept the projection fn_8000F654 and its four tanf calls. They are now in
  `extract/lift-sites-math-unlifted.txt`, added only when `SHADOW_RENDER_LIFTED` is off.

- `docs/native-asset-pipeline-20261005/`, `docs/dvd-ownership-20261005/CONTRACTS.md` and `docs/storage-2026-09-21/`
  are cited in source headers but do not exist.
- Not located yet: the reader of "Error(File not found)" (0x804C47E0) and the RwStreamOpen → RtFS path.
