# The game's memory allocators (reverse-engineered 2026-11-04)

main.dol PAL. Addresses are guest addresses; r13 (SDA base) = 0x805EC500. Nothing here is committed or deployed.
Takes M6 of docs/RESOURCE_AUDIT_2026-11-04.md (the allocators were "translated, not understood"). The user's
2026-10-06 directive rules out a native allocator behind 32-bit window addresses. This work therefore:
- reverse-engineers the stack;
- measures it in play;
- reconstructs the game's own heap class in C++20, proven against the original words;
- adds a read-only census of the live heap.

## 1. The stack

```
OS arena (OSGetArenaLo/Hi 0x8037221C / 0x80372214)
  -> ONE Dolphin OS heap over the whole arena (18,714,304 bytes in Westopolis)
     made by the game's RenderWare memory setup 0x80050C98:
       OSInitAlloc 0x80372138 (1 heap) -> OSCreateHeap 0x803721A8 -> OSSetCurrentHeap 0x80372128 (handle r13+0x2D90)
     users, all OSAllocFromHeap 0x80371FB0 / OSFreeToHeap 0x803720AC on the current heap:
       - RenderWare: the game's RwMalloc 0x80050E28 / RwFree 0x80050EF0 (realloc 0x80050D3C, calloc 0x80050DDC),
         installed as RW's memory functions by 0x80050C98 - one OS heap cell per RW allocation
       - MSL: __sys_alloc 0x803A3A4C (its own default-heap setup branch never runs: a current heap exists)
       - AudioGCN's buffers (AudioGCN ctor / dtor) and an SDK pair 0x8038A0DC / 0x8038A104
  -> MSL malloc 0x803A3E14 / free 0x803A3DA4 pools (pool object 0x805A6300, 0x34 bytes)
       <- operator new 0x803A1380 (1,999 call sites) / operator delete 0x803A1334 (2,904 call sites)
       <- RW's default memory function 0x80481264 (before the game installs its own)
  -> Specialised heaps built inside blocks of the above:
       - Sonicteam::System::Heap (effect system; section 4)
       - Chao::CSD::CMemoryAlloc (CSD UI: vf3 / vf4 -> TCsdSystem vf1 / vf2) - not analysed
       - RenderWare free lists (_rwFreeListAllocReal 0x80480E00 ...) and the resource arena
         (RwResourcesAllocateResEntry 0x804818C8 ...) - reconstructed in rwcore/
```

## 2. Dolphin OS heap (OSAlloc), confirmed against the original words

- **Globals:** HeapArray r13+0x5A60, NumHeaps +0x5A64, ArenaStart +0x5A68, ArenaEnd +0x5A6C, the current heap
  r13+0x26E0 (-1 = none).
- **HeapDesc**, 12 bytes: {size, free list, allocated list}.
- **Cell header**, 0x20 bytes: {prev, next, size}; the data is at +0x20 and the size includes the header.
- **OSAllocFromHeap(heap, size):**
  - need = round32(size + 0x20);
  - first fit along the free list, which is kept in address order;
  - a leftover under 0x40 stays inside the block, otherwise the cell is split;
  - the block goes to the head of the allocated list.
- **OSFreeToHeap:** unlinks the cell from the allocated list, inserts it into the free list in address order and
  merges it with touching neighbours.

**Game's RwMalloc 0x80050E28:**
1. A size of 0 returns NULL.
2. It spins while the busy flag r13+0x2D94 is set, printing "Critical Region [begin critical] Wait" each pass.
3. It sets the flag, turns off interrupts and the OS scheduler, calls OSAllocFromHeap on the current heap, then
   turns both back on.
4. If the flag was cleared meanwhile it prints "Critical Region [end critical] Warning ...". It clears the flag.

MSL's `__begin_critical_region` / `__end_critical_region` (0x803A906C / 0x803A9068) are empty stubs.

**Proof** (`heap_census_tests`): the original OSInitAlloc / OSCreateHeap / OSAllocFromHeap / OSFreeToHeap run in
the reference interpreter over 20,000 random operations on an 8 MiB heap: 10,174 allocations, 299 refused because
the heap was full, 9,527 frees, 118,474 checks, 0 failures. After every call the native census
(`gekko/heap_census.h`) agrees with:
- byte conservation: allocated + free bytes = the heap's size;
- the live block count;
- every live block's cell and size;
- a sorted and fully merged free list;
- first fit: every allocation is the lowest-address free cell that fits, and a refusal happens only when no cell
  fits.

## 3. MSL malloc / free (structure only)

- **malloc 0x803A3E14:**
  - zeroes the pool object 0x805A6300 (0x34 bytes) on first use (flag r13+0x5E90);
  - calls the core 0x803A46E8 between the empty critical-region stubs.
- **Core 0x803A46E8:**
  - size 0 or above 0xFFFFFFCF: NULL;
  - up to 0x44 (68) bytes: the fixed-size small-block path 0x803A4894 (-> 0x803A4DF8 -> 0x803A5060);
  - larger: the variable-size path 0x803A4ED0 (-> 0x803A4FAC -> 0x803A5244, new blocks from __sys_alloc).
- **free 0x803A3DA4:** -> 0x803A4690 -> 0x803A473C / 0x803A4B64.
- **Pool object:** 0x34 bytes, consistent with one variable-block list head plus six small-size classes of 8 bytes.
- **Not reconstructed yet:** the block and sub-block formats, the size classes, and how blocks return to the OS
  heap. Next step: the same pairing test as section 4.
- **In play**, the pools absorb almost everything:
  - 121,828 mallocs / 118,589 frees in 80 s of Westopolis took only 11 OS cells (745,568 bytes) through
    __sys_alloc.

## 4. Sonicteam::System::Heap, reconstructed in C++20 (`gekko/sonicteam_heap.h`)

**The object** (8 bytes): +0 the vtable (0x8056AF2C, IHeap's 0x8051E300 first), +4 the buffer.

**The buffer:**
- an anchor {first free block, 0}: the 0 sits where a size would be, so the anchor never merges with a block;
- blocks with an 8-byte header {next free, size including the header};
- the free list is singly linked and kept in address order.

| slot | address | what |
|---|---|---|
| ctor | 0x8040C040 | (this, buffer, size): anchor {buffer + 8, 0}; one free block {0, size - 8} |
| vf0 (+8) | 0x8040BFA8 | Alloc(size): need = RoundUp(size + 8, 8); first fit; exact fit unlinked, else the tail splits off in place |
| vf1 (+0xC) | 0x8040BEB0 | AllocAligned(align, size): need = align + RoundUp(size + 8, 8); data = RoundUp(block + 8, align); the front padding stays a free block |
| vf2 (+0x10) | 0x8040BE60 | Calloc(size): this->vf0, then memset 0 |
| vf3 (+0x14) | 0x80045FC4 | IHeap: nothing |
| vf4 (+0x18) | 0x8040BDB8 | Free(p): address-ordered insert, merges with the next and the previous free block |
| vf5 (+0x1C) | 0x8040BD88 | LargestFree() |
| vf6 (+0x20) | 0x8040BCB8 | Realloc(p, size): NULL -> vf0; same pointer if the header-inclusive size == size; else vf0, copy min(size, old - 8), vf4 |
| vf7 (+0x24) | 0x8040BC90 | TotalFree() |

RoundUp is 0x80045FC8: (x + a - 1) & ~(a - 1).

**In play** (80 s of Westopolis):
- one heap (object 0x807F1598, buffer 0x80B1EE40), built by EffectSystemUSA's constructor 1.3 s in;
- 8,486 AllocAligned calls, every one with alignment 16, and 8,376 Frees. Nothing else was called.

**Proof** (`sonicteam_heap_tests`): the original constructor and methods run in the reference interpreter; the
C++20 class drives a second machine at the same addresses.
- **The program:** 30,000 seeded random calls - Alloc 8,136, AllocAligned 3,891 (alignments 8-256), Calloc
  2,122, Free 9,959, Realloc 3,479, LargestFree 1,181, TotalFree 1,232. The heap ran full 3,091 times, and every
  live block was filled with a pattern so that Realloc's copy is checked.
- **The checks:** after every call, the return value, the 8-byte object and every byte of the 256 KiB buffer must
  match.
- **Result:** 62,800 checks, 0 failures.

**Hazards found in the original** (the game's observed calls avoid both):
- **Alignment below 8 corrupts the heap.** With AllocAligned alignment 4:
  1. need = 4 + a multiple of 8 leaves blocks that are not 8-byte aligned;
  2. later padding leaves free fragments smaller than a header, overlapping the next block;
  3. a Free writes through those fragments.
  The free list becomes a cycle and LargestFree never returns (reproduced: `sonicteam_heap_tests <dol> 4`). The
  contract is an alignment that is a multiple of 8; the game uses 16.
- **Realloc's identity check is off by the header.** It compares the header-inclusive size with the request, so
  when they are equal it returns the same block, which holds only `size - 8` bytes. A caller that then writes
  `size` bytes runs into the next block's header. Reachable only if a caller asks for exactly a stored size;
  Realloc was not called in play.
- **The copy can overlap.** Once the heap is corrupt, Realloc's new block can overlap the old one, and the game's
  memcpy 0x800054F4 is overlap-safe (memmove). The C++ class leaves the copy to its caller (GuestAccess::copy),
  which must be overlap-safe too.

**Status:** a proven reconstruction, not installed as an owner of the game's code. It handles about 106 calls per
second at a negligible cost, and it is ready for when the effect system is lifted.

## 5. Measured in play

**Cost.** The allocators take 14 of 35,564 game-thread samples (0.04 %, 0.007 ms per frame; tools/perf/
sample_threads.py, 1080p Westopolis). Replacing them buys no speed.

**Call census** (GEKKO_CALL_CENSUS, 80 s headless Westopolis replay; .build/perf/alloc-census/):

| function | calls |
|---|---|
| RwMalloc (game) / OSAllocFromHeap | 320,634 / 320,645 (70.3 MB requested) |
| RwFree (game) / OSFreeToHeap | 307,193 / 307,193 |
| MSL malloc / free | 121,828 / 118,589 |
| MSL __sys_alloc | 11 (745,568 bytes) |
| Sonicteam Heap AllocAligned / Free | 8,486 / 8,376 |

**The live OS heap over time** (GEKKO_HEAP_TRACE, new; .build/perf/heap-trace/heap.csv):
- size 18,714,304 bytes;
- allocated 15.56-16.41 MB during play, 13,203-13,729 cells;
- free 2.31-3.16 MB, which is 12-17 % headroom;
- largest free block 2.16-3.13 MB;
- 17-125 free fragments;
- the free list stayed sorted and merged in every sample.

**Leak check across stage restarts** (tools/stage_control.py, the pause menu's Restart, 4 restarts of stg0100;
.build/perf/leak-out/):
- per segment, minimum allocated = 15,317,472-15,319,712 bytes (within 2.3 KB);
- 20 s after each start, 15.67-15.79 MB;
- largest free 2.848 MB every time;
- 0 walk errors.

No leak per restart. Not covered: a full stage-to-stage load, because the pause-menu Restart may not reload every
asset.

## 6. Before any allocator becomes an owner of game code

These rules come from the stage-loading session's audit (2026-11-04). Nothing here is installed as an owner yet;
they apply when something is.
- **Stack frames are observable.** fn_8006BEF0 copies uninitialised stack bytes into heap records. An owner must
  write the original's frame: back chain, LR slot and saved registers. Its guest calls must leave from that frame
  with the original return sites. RtGCNFS diverged in a whole-memory A/B until it did this.
- **Registers.** Leave r3 / r4 / f1 as the original does on every path, and restore pc after guest calls. The strict
  verifier checks this.
- **Interpreter oracle tests.** Every guest call an owner makes needs a stand-in installed on both machines. The
  two tests here call no guest code from the native side; Sonicteam::System::Heap's memcpy / memset are the
  GuestAccess callbacks.
- **Writes during journaling or tracing** go through memory_.write, as the motion store's copy() does
  (native_motion_store.cpp).
- **Savestates** are at version 30. Owner state means version 31 and an update to native_expand_jobs_tests, which
  loads with 30u.
- **guest_memory.cpp load_state** unmaps regions the state does not name first, and remaps a region committed past
  its saved size. That matters for any change to the MEM1 layout.

## 7. Open

- **MSL pool internals** (section 3): the block formats, the size classes, and returns to the OS heap; then a
  byte-exact reconstruction with the same pairing test.
- **Not analysed:**
  - Chao::CSD::CMemoryAlloc (CSD UI);
  - AudioGCN's direct OS allocations;
  - the SDK pair 0x8038A0DC / 0x8038A104 (which SDK library is not identified).
- **Not measured:**
  - heap headroom on the other stages;
  - a stage-to-stage load sequence for leaks. GEKKO_HEAP_TRACE + tools/perf/all_stages.sh would do both.
