# The game's memory, as it is today (2026-11-26)

This is the map the 64-bit rewrite works from (the plan is `REWRITE_PLAN.md` beside this file). It describes the
release worktree `H:\NukkoroGit-rel` (branch `release-0.0.4`, head `a2c93e9` plus the release session's uncommitted
memory work), Shadow the Hedgehog PAL `main.dol`.

Every statement carries one of these marks:

- **MEASURED**: seen in a run made for this record (below) or in a run a cited record describes.
- **READ**: read in the source code or in a project document (the file is named).
- **INFERRED**: concluded from measured or read facts; not checked directly.
- **UNRESOLVED**: not known; what would settle it is named.

## 0. The run behind the numbers

**MEASURED.** Westopolis tape (`stg0100`), run with `run_tape.ps1` under one coordinator reservation:
- **Executable:** `H:\NukkoroGit-rel\.build\tests\bin\Release\ShadowTheHedgehog.exe` (md5 `fc3121b7`, built 2026-11-26 23:42, not a shipping build).
- **Recording:** `GEKKO_HEAP_TRACE` (one heap census per game second) and `GEKKO_CALL_CENSUS` on MSL `malloc` 0x803A3E14 and `__sys_alloc` 0x803A3A4C.
- **Length:** 12,798,000,000 ticks = 316 game seconds, 43 s of wall time, exit 0.
- **Outputs:** in the session scratchpad `...\scratchpad\heaprun2\` (`heap.csv`, `alloc_calls.csv`, `stdout.log`). These are not kept in the repository.

**MEASURED.** This run's state hash is `1cdca8379c8d`. The gate's Westopolis reference (`gate_references.json`) is `6d9d956eec70`.
- That reference belongs to the `wt-fix-shoes` line.
- **UNRESOLVED:** whether the difference comes from the release branch's own changes or from the census hooks. A plain `gate.ps1` run of this executable would tell.
- Nothing below depends on the hash.

**MEASURED, a limit of the run.** The census asked for RwMalloc 0x80050E28 and OSAllocFromHeap 0x80371FB0 too, but recorded no call of either.
- OSAllocFromHeap is a native owner (`extract/native-owners.txt:241`, READ).
- **INFERRED:** calls that enter an owner directly never reach the census's chained hook.
- So the split of the heap's bytes by caller or by kind of data is **UNRESOLVED**. Piece M0 of the plan exists to measure it.

## 1. The address window

**READ** (`runtime/include/gekko/guest_memory.h`):
- The translated game sees one flat 32-bit window of 4 GiB.
- Each region of the window has its own host reservation, committed on demand. The CPU's accessors address it as `fm_window + guest address`: one 4 GiB host reservation, plus a 64 KiB no-access guard past the end.
- A 16 KiB page table gives the translation. An unmapped page fails closed: the run stops and names the address.

**INFERRED.** The window is already a "compressed pointer" scheme: the game runs as a 64-bit process, and each 32-bit value it holds is an offset from one 64-bit base. The limit is therefore not the console's 24 MiB but the regions mapped in the window. That ceiling is set by the placement rules in section 1.2, not by the 32-bit width.

### 1.1 Regions mapped in a run without mods

| Window range | Region | Size | Mark |
|---|---|---|---|
| 0x80000000..0x80610000 | `image`: OS low memory, the DOL's code, data and BSS, the main thread's stack | 6,356,992 B committed | MEASURED (run summary `[guest-memory]`) |
| 0x80610000..0x81800000 | `legacy-os-heap`: the range the game's own OS heap runs over | 18,808,832 B committed | MEASURED |
| 0xE0000000..0xE0004000 | `locked-cache`: the scratch the game's own code addresses (LCEnable, `dcbz_l`) | 16,384 B | MEASURED |
| 0xCC000000..0xCC010000 | test fixture for GameCube device addresses. Production maps nothing here; an access stops the run | 64 KiB | READ (`guest_memory.h:335`) |

**MEASURED.** Total committed in the window: 25,182,208 bytes in 3 regions.

**MEASURED.** Inside `image`, the DOL's sections, decoded from the header of `.build\release\generated\embed\main.dol` (SHA-256 prefix fde4fa6f81a6):

| Section | Address | Size |
|---|---|---|
| T0 | 0x80003100 | 0x2500 |
| T1 (the game's code) | 0x80008D40 | 4,857,632 B |
| D0 / D1 | 0x80005600 / 0x80007520 | 0x1F20 / 0x1820 |
| D2 / D3 | 0x804AAC60 / 0x804AB0E0 | 0x480 / 0x20 |
| D4 | 0x804AB100 | 468,000 B |
| D5 (vtables and tables) | 0x8051D520 | 338,144 B |
| BSS | 0x8056FE00 | 575,468 B, ending at 0x805FC5EC |
| D6 (`.sdata`: RTTI records; r13 = 0x805EC500) | 0x805E4500 | 0xAB20, inside the BSS span |
| D7 (`.sdata2`) | 0x805F2780 | 0x9DC0, inside the BSS span |

The entry point is 0x80003154.
- The main thread's stack follows BSS (0x805FC5EC..0x8060C5F0), and the heap's arena follows the stack.
- The image region ends at 0x80610000 (INFERRED from the boot constants in section 2).

### 1.2 Ranges that hold handles, not memory

Each of these is a range of 32-bit values that the game carries but never dereferences. Only native code turns such a value into bytes.

| Value range | What the value names | Resolved by | Mark |
|---|---|---|---|
| 0x90000000..0x94000000 (64 MiB) | motion clip pool: byte `p` of the pool is `0x90000000 + p` | `NativeMotionStore::pool_bytes` | READ (`native_motion_store.h:52-53`) |
| 0x90000000..0xA0000000 | "retired arenas": `map()` refuses this range, and any CPU access stops the run | `guest_memory.h:191-198` | READ |
| 0x01000000..0x02000000 (16 MiB) | GCAX sound bank addresses (the console's ARAM addresses) | `SoundArena::source` | READ (`native_sound_banks.h`, `sound_arena.cpp:106`) |
| 0x02000000..0x03000000 (16 MiB) | CRI stream ring addresses | `NativeStreamRing` | READ (`native_stream_ring.h:40-44`) |
| 0x60000000.. (about 1.41 GiB, 64 KiB per file) | file tokens in `DVDFileInfo.startAddr` | `AssetSystem::read` | READ (`asset_runtime.h:236`) |
| 24-bit slot + 8-bit generation | renderer texture and buffer handles stored in RenderWare object words | the lifted renderer | READ (`lift/render/frame.h:34-42`, `rw_driver.h:9-14`) |

**READ** (`guest_memory.h:168-182`): the window's placement rule.
- The SDK's DMA-style contracts carry `address - 0x80000000` (OSCachedToPhysical).
- `from_os_physical` inverts that, and only for values below 0x40000000.
- So every region except the locked-cache scratch must lie in **0x80000000..0xC0000000**.
- **INFERRED:** that 1 GiB, minus the retired-arena labels at 0x90000000..0xA0000000, is the most game memory the present window design can ever hold in one contiguous heap: 0x80610000..0x90000000, about 250 MiB. Moving the labels elsewhere would raise this to about 1 GiB, as 0x01000000 and 0x60000000 were already used for other handles. Piece W1 of the plan does this.

### 1.3 The mod path (the release session's uncommitted work)

**READ** (`game_memory_end.h`, `guest_memory.cpp:303-340`, `shadow_main.cpp:2395-2410`, `system.cpp:1986-2018`):
- When any mod replaces game files, `extend_legacy_heap(0x90000000)` remaps the heap region as 0x80610000..0x90000000, zero-filled, before the game's first instruction.
- `g_game_memory_end` then reads 0x90000000. The OS low-memory words follow from it:
  - 0x80000028 and 0x800000F0: memory size;
  - 0x800000F4: the BI2 address, `end - BI2 bytes`;
  - 0x80000034: arena high, below BI2 by the disc directory table's reservation.
- The game then builds its one heap over the larger arena.
- A run without mods is unchanged.

**READ.** Fifteen host-side checks of the form "is this a pointer into the game's memory" now compare with `game_memory_end()`:
- `system.cpp`, `native_s1_target2.cpp`, `far_objects.cpp`, `material_probe.cpp`, `gameplay_camera.cpp`, `game_errata.cpp`, `voice_trace.cpp`, `replay_check.h`, and tests.
- These are host-code assumptions about the window, a class of code the rewrite must also visit (see `REWRITE_PLAN.md` section 2).

**READ** (`docs/RELEASE_VALIDATION.md:576-581`): why this was done.
- With all 15 community mods, the run stopped at tick 2,002,050,000 on a 2-byte load from address 4.
- The motion bank's clip allocation had returned 0, 28 times. The heap was exhausted.

## 2. Boot: how the heap comes to exist

1. **READ** (`system.cpp:1986-2018`): the runtime writes the low-memory words.
   - Memory size 0x80000028 = `game_memory_end() - 0x80000000`: 24 MiB without mods.
   - BI2 at 0x800000F4: 0x817FE000 on the console layout.
   - Arena high 0x80000034 = `(BI2 - directory reservation) & ~31` when assets are mounted.
   - Arena low 0x80000030 is never written.
2. **READ** (`native_boot.cpp:52-59, 86-87, 248-254`): the native `__start` runs OSInit natively.
   - Arena low (cell r13+0x26E8) = word 0x80000030, or if that is 0, `(stack top 0x8060C5F0 + 31) & ~31` = **0x8060C600** (INFERRED arithmetic).
   - Arena high (cell r13+0x5A70) = word 0x80000034, or if that is 0, 0x817A0000.
   - **INFERRED:** the arena therefore starts 14,848 bytes before the `legacy-os-heap` region begins, inside the `image` region. The heap's first cells sit in the image mapping.
3. **READ** (`docs/MEMORY_ALLOCATORS.md` section 1, `native_os_heap.cpp:179-180`): the game's RenderWare memory setup 0x80050C98 builds the heap.
   - It calls OSInitAlloc 0x80372138 (one heap), OSCreateHeap 0x803721A8 and OSSetCurrentHeap 0x80372128. OSInitAlloc and OSCreateHeap are native owners.
   - The heap handle is at r13+0x2D90; the current heap is r13+0x26E0.
   - It installs RwMalloc 0x80050E28 / RwFree 0x80050EF0 as RenderWare's memory functions.
4. **MEASURED:** the OS heap's size in this run is **18,711,904 bytes**. The 2026-11-04 record measured 18,714,304 on an older build.
   - **INFERRED:** the 2,400-byte difference is a change in the directory-table reservation.
5. **READ** (`native_os_arena_clock.cpp:9-30`; `docs/agents/axfx-effects.md:140,187`): OSGetArenaHi, OSGetArenaLo and OSSetArenaLo are native owners.
   - In play only 0x80050C98, OSInit and OSSetArenaHi touch the arena cells.
   - **MEASURED** (run summary): OSGetArenaHi was called once, OSGetArenaLo once, OSSetArenaLo twice.

## 3. The allocator stack on top of the heap

**READ** (`docs/MEMORY_ALLOCATORS.md`), with this run's numbers added.

```
OS arena  0x8060C600 .. arena high
  -> ONE Dolphin OS heap (18,711,904 B; first fit, address-ordered free list, 0x20-byte cell header)
       OSAllocFromHeap 0x80371FB0 / OSFreeToHeap 0x803720AC   (native owners)
     users:
       RwMalloc 0x80050E28 / RwFree 0x80050EF0     one OS cell per RenderWare allocation
       MSL __sys_alloc 0x803A3A4C                  feeds MSL's pools
       AudioGCN buffers, the SDK pair 0x8038A0DC / 0x8038A104, the AX MIX heap wrappers
  -> MSL malloc 0x803A3E14 / free 0x803A3DA4 (pool object 0x805A6300, 0x34 B)
       <- operator new 0x803A1380 (1,999 call sites) / delete 0x803A1334 (2,904 call sites)
  -> heaps built inside blocks of the above:
       Sonicteam::System::Heap (effect system), Chao::CSD::CMemoryAlloc (CSD UI),
       RenderWare free lists and the RW resource arena, the GCAX context's own heap
```

**MEASURED.** Call counts in this run (316 s):

| Function | Calls | Bytes requested |
|---|---|---|
| MSL `malloc` | 566,004 | 17,698,726 |
| MSL `__sys_alloc` (cells MSL takes from the OS heap) | 11 | 745,568 |

**INFERRED.** MSL's pools hold 745,568 bytes of the OS heap: the same 11 cells and the same bytes as the 80-second count of 2026-11-04. MSL takes its memory once and recycles it.

**READ** (`native_rw_alloc.cpp:14-19`): a native server for RwMalloc, RwFree and the RW free lists exists. It is off unless `GEKKO_RW_ALLOC=1` or the CMake option `SHADOW_RW_ALLOC_DEFAULT` is set. Its memory still comes from the guest OS heap.

## 4. The heap over the Westopolis tape (MEASURED, `heap.csv`)

| Game time | Allocated bytes | Cells | Free bytes | Largest free | Free fragments |
|---|---|---|---|---|---|
| 0.005 s | 1,622,656 | 78 | 17,089,248 | 17,089,120 | 3 |
| 2 s (stage loaded) | 15,413,920 | 12,508 | 3,297,984 | 3,297,888 | 2 |
| 20 s | 15,743,296 | 13,061 | 2,968,608 | 2,846,496 | 43 |
| 120 s | 16,127,968 | 13,285 | 2,583,936 | 2,473,824 | 85 |
| **174 s (peak)** | **16,428,192** | n/a | **2,283,712** | **2,213,536** | n/a |
| 316 s (end) | 16,338,176 | 13,460 | 2,373,728 | 2,213,536 | 69 |

**MEASURED, summary:**
- During play (20 s onward) the allocated bytes stay between 15,381,056 and 16,428,192.
- Cells peak at 13,967.
- Free fragments peak at 138.
- Every census found the free list sorted and merged: 0 errors in 317 samples.
- Minimum headroom: 2,283,712 bytes, **12.2 % of the heap**.

**INFERRED.** The Westopolis stage fills 82-88 % of the console heap. A mod set that adds about 2.2 MB of resident data, or any single allocation above about 2.2 MB, runs it out. That is consistent with the 15-mod failure.

**READ** (`docs/MEMORY_ALLOCATORS.md` section 5): no leak across 4 pause-menu restarts (minimum allocated within 2.3 KB each time).

**UNRESOLVED:**
- Headroom on the other stages.
- A full stage-to-stage load sequence.
- `tools/perf/all_stages.sh` with `GEKKO_HEAP_TRACE` would measure both.

## 5. Pools and fixed tables carved out by the game

The sources are the documents named in each row, read 2026-11-26.
- "BSS" means the table is a fixed address in the image.
- "heap" means it is allocated from the OS heap at run time.

| Pool or table | Where | Size | Who allocates / frees | Mark |
|---|---|---|---|---|
| Main thread stack | 0x805FC5EC..0x8060C5F0, right after BSS | about 64 KiB | fixed | READ (`native_boot.cpp:57,70`); INFERRED placement (BSS ends at 0x805FC5EC) |
| Default thread object | BSS 0x80587450 | n/a | fixed | READ |
| MSL pool object | BSS 0x805A6300 | 0x34 B; 745,568 B of cells | malloc / free | READ / MEASURED |
| Sonicteam effect heap | object 0x807F1598, buffer 0x80B1EE40 (heap) | buffer size UNRESOLVED | built by EffectSystemUSA's constructor; 8,486 AllocAligned / 8,376 Free in 80 s | READ (`MEMORY_ALLOCATORS.md` section 4) |
| CSD UI allocator | inside heap blocks | UNRESOLVED (not analysed) | Chao::CSD::CMemoryAlloc vf3 / vf4 | READ |
| RW resource arena | heap, `RwMalloc(0x40000, 0x4040B)` | 256 KiB | `_rwResourcesOpen`; least-recently-used eviction 0x804818C8 | READ (`rw_capture.cpp:1848`, `rwcore/src/resources.cpp:112-136`) |
| RW free lists | heap blocks | entries per block: frame 50, matrix 256, raster 128, texture 128, material 256, light 32, image 128, stream 16, camera 4, texture dictionary 5, plugin registry 64, world 8, light ties 64 | `_rwFreeListAllocReal` 0x80480E00 | READ (`rwcore/src/*.cpp`, `rpworld/src/*.cpp`) |
| GCAX context and its own heap | heap, `alloc(0xE1000)` | 921,600 B (context 0x14470 B: 64 voices x 0x10C, 64 notes, 64 ARQ requests, 64 load handles, 8 ports x 0x1530, the 'MF' / 'MB' cell heap) | game constructor 0x801F2E58 -> `gcaxInit` | READ (`docs/GCAX_SOUND_MAP.md:135-146, 186, 319, 370-391`) |
| SE manager channels / AudioData table | GCAX | 43 x 0x34 / 8 x 0x10 | n/a | READ (`GCAX_SOUND_MAP.md:226-230`) |
| CRI SVM servers | BSS 0x805C0E20 | 8 types x 0x48 | fixed | READ (`docs/CRI_SERVERS.md:8-12`) |
| CRI ADX handles | BSS 0x805A6928 | 16 x 0xC4 | fixed | READ (`CRI_SERVERS.md:81`) |
| CRI SJMEM / SJRBF joints | BSS 0x805BBD8C / 0x805BC214 | 32 x 0x24 / 256 x 0x40; plus a 0xC00-byte pool at 0x805C021C | fixed; 35 rings created, 19 destroyed in Westopolis | READ (`CRI_SERVERS.md:131-135`) |
| Stage block slots (TObjLand) | heap task | 32 x 0xC0; TBlockManage 0x980 B, 168-block mask | stage loader | READ (`docs/STAGE_LOADING.md:120-127`) |
| Set-object layout slots | heap | 0x800 x 0x20 B = 64 KiB | layout manager | READ / INFERRED (`STAGE_LOADING.md:207`, `docs/re/core.md:51-60`) |
| Background-read workers | heap | 0x1000-byte stack each; reaper scans 128 slots | TBGRead / TExpandBG; reads in 0x2000 chunks into `fn_80049BB8(length, 2)` (RW allocator) | READ (`STAGE_LOADING.md:483-487, 618, 646`) |
| Motion clip nodes | heap, one node per clip in use | 2,721 clip loads, 2,536 node frees in this run | MotionBankStore (native) allocates in the original's order | MEASURED (run summary), READ (`native_motion_bank_store.h:31-41`) |
| Range-object pools | singletons 0x80571D40 / 0x80571D9C | n/a | 0x800268B0 / 0x800271A8 | READ (`docs/re/core.md:149`) |

## 6. Native stores that already live outside the window

| Store | Holds | How the game names it | Size in this run | Mark |
|---|---|---|---|---|
| `NativeMotionStore` (`native_motion_store.h:40`) | each motion pack's clip spans (the console kept them in a 6 MiB ARAM pool) | handles 0x90000000 + offset | 56 uploads, 2.39 MiB; 7,518 fetches, 19.75 MiB copied back into heap nodes; 2.41 MiB committed of 64 MiB reserved | MEASURED / READ |
| `MotionBankStore` (`native_motion_bank_store.h:58`) | the bank registry, per-clip pool records, node lists | through 15 native owners; heap nodes kept as scaffolding | 34 banks, 326,611 acquires | MEASURED |
| `NativeSoundBanks` / `SoundArena` (`sound_arena.h:25`) | GCAX `.mlt` wave data | handles 0x01000000 + offset | 8 uploads, 4.55 MiB; 5.08 MiB committed of 8 MiB | MEASURED |
| `NativeStreamRing` (`native_stream_ring.h:33`) | the ADX stream PCM ring | handles 0x02000000 + offset | 15,881 transfers, 61.20 MiB moved; 0.25 MiB committed | MEASURED |
| Native file API / `AssetSystem` (`asset_runtime.h:218`) | disc files as read-only mapped host files | tokens from 0x60000000 | 2,899 reads, 27.88 MiB. **Each read copies the bytes into a heap buffer**, so files still occupy the window | MEASURED / READ |
| Lifted renderer GPU resources (`lift/render/resources.h`) | textures and meshes on the GPU | slot handles in RW object words | geometry uploads 6,571 (25,304,637 B); "instance" bytes 21,985,533 | MEASURED (`[native-capture]`) |
| Native asset manager, PRS look-ahead, collision mirror, texture packs | host-side copies | no handle given to the game | `[native-memory] assets` committed 16,355,328 B | MEASURED / READ |

**MEASURED** (`[native-memory]`):
- Process private bytes: 299,524,096. Resident: 292,139,008.
- Tracked native allocations: 25,051,136 B committed, of which assets 16,355,328, renderer-host 7,602,176 and scratch 1,089,536.

**READ** (`native_motion_store.h:13-26`), the property that makes the handle scheme work:
- On the console, motion, sound-bank and stream-ring data lived in ARAM, which the CPU cannot address. The game only ever carried those addresses and handed them to DMA calls.
- Every function that moves those bytes (upload, fetch, `gcaxAramDma`, the ARAlloc call sites) is a native owner or a rewritten call site. No translated code dereferences a handle.

**INFERRED:** this is why exactly those three stores could leave the window without changing one structure layout. Main-memory data is a different case, because translated code dereferences it directly. Moving it needs every reader to be native first.

**READ** (`native_motion_store.h:22-26`): the motion store still copies each clip into a heap node on first play.
- Giving the game the clip bytes in place, with no node, changed a soldier's animation for 271 frames.
- The original's animation depends on the node's lifetime. The most likely cause is the shared current-events pointer [r13+0x5F70] outliving its node.

**READ** (`scaffold/rw_driver/textures.cpp`, `native_rw_raster.cpp:511-603`): texels and vertex data stay in the heap.
- GcnRasterCreate and the level allocation are native owners. They allocate every mip level through the engine's allocator, which is the OS heap, with hint 0x30411.
- The GPU copy is built from the heap copy.
- So each texture is held twice: once in the heap and once on the GPU.

## 7. Which structures store raw 32-bit addresses

**READ / INFERRED.** In short: all of them.

- **Heap and BSS objects.**
  - Every C++ object of the game has a 32-bit vtable pointer at +0. Its pointer members are 32-bit.
  - Examples: task tree nodes, the player, enemies, the stage managers, smart-pointer counts (`NATIVE_WINDOWS_MIGRATION.md` section 3, READ).
- **RenderWare objects.**
  - `RwRaster` keeps parent at +0x00 and pixels at +0x04 (`native_rw_raster.cpp:94-97`, READ).
  - Frames, atomics, geometries, materials, textures, dictionaries and the plugin extension blocks all link by 32-bit pointers (`rwcore/`, `rpworld/`, READ).
  - The RW engine instance holds its memory functions at +0x108 / +0x10C (`native_rw_raster.cpp:86`, READ).
- **Allocator metadata.**
  - OS heap cells {prev, next, size}; HeapDesc {size, free, allocated}; Sonicteam heap blocks {next, size}; RW free-list blocks; GCAX 'MF' / 'MB' cells.
  - All of these are 32-bit links (`MEMORY_ALLOCATORS.md`, READ).
- **SDK state.**
  - The OS thread queue, the AX voice parameter blocks (64 x 0x180 with physical-address links), the MIX channel table, the GX FIFO and `__GXData` function-pointer words, the DVD / CARD command blocks.
  - **READ** (`NATIVE_WINDOWS_MIGRATION.md`): 494 pointer stores in AIInit, AXInit, MIXInit and AXRegisterCallback alone.
- **The image's data sections.**
  - Vtables, function-pointer tables, string tables, and constructor tables (`__start` walks them).
  - All of these are 32-bit absolute addresses resolved at link time.
  - The counts are in `REWRITE_PLAN.md` section 2.
- **Loaded file data.**
  - **READ** (`docs/re/formats.md` sections 1, 2 and 7; `docs/ONE_LAYER.md` section 9; `docs/STAGE_LOADING.md:186`): several formats are loaded as one block in the heap and then relocated in place, turning their offsets into 32-bit pointers:
    - the "1B" container (.CCL, .PTB, .PTP, gadget .BIN, ScoreData / AudioData .bin): loader 0x8040F66C, relocation pass 0x8040F868, 28,218 relocated words over 1,862 blocks on the disc;
    - .SNB camera motion (NOF0, reader 0x80010290): 1,613 relocations in 222 files;
    - `.one` archive directories (entry +0x30 = archive base + offset, 0x8004CD38);
    - stage set records (+0x28 turned into a pointer).
  - **INFERRED:** RenderWare streams are parsed into newly allocated objects and are not relocated in place, but the objects they produce hold 32-bit pointers.
  - **UNRESOLVED:** motion (Mtn) and CSD (.GNCP) files. The cited `docs/re/assets.md` does not exist.
- **Registers and the stack.** Every translated function keeps addresses in 32-bit `gpr[]` slots. Saved frames hold 32-bit back chains and return addresses: `fn_8006BEF0` copies uninitialised stack bytes into heap records (`MEMORY_ALLOCATORS.md` section 6, READ).

## 8. What this map does not cover (UNRESOLVED)

- The heap's bytes by kind: textures, geometry, collision, motion nodes, objects, file buffers. This is the first thing the plan measures (piece M0).
- The sizes of the effect heap, the CSD allocator and the CRI work areas.
- The headroom on stages other than Westopolis.
- Whether any translated code builds an address with the uncached-mirror arithmetic (`+ 0x40000000`) or compares a pointer with a console constant such as 0x81800000. This matters for raising the window's ceiling (piece W1).
- The state-hash difference from the gate reference noted in section 0.
