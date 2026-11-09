#pragma once

// Read-only census of the game's OS heaps (docs/MEMORY_ALLOCATORS.md). Measurement only: it reads guest memory and
// changes nothing.
//
// What the game allocates from (reverse-engineered 2026-11-04, main.dol PAL):
//   * One Dolphin OS heap over the whole arena, made by the game's RenderWare memory setup 0x80050C98
//     (OSInitAlloc 0x80372138 with one heap, OSCreateHeap 0x803721A8, OSSetCurrentHeap 0x80372128; handle at
//     r13+0x2D90). MSL's own default-heap setup in __sys_alloc 0x803A3A4C never runs: the current heap is already set.
//   * Its users, all through OSAllocFromHeap 0x80371FB0 / OSFreeToHeap 0x803720AC on the current heap:
//       - RenderWare: the game's RwMalloc 0x80050E28 / RwFree 0x80050EF0 (realloc 0x80050D3C, calloc 0x80050DDC),
//         one heap cell per allocation, under a busy flag (r13+0x2D94) + interrupts and the scheduler off;
//       - MSL malloc 0x803A3E14 / free 0x803A3DA4 (operator new 0x803A1380 / delete 0x803A1334 - 1,999 / 2,904 call
//         sites) take pool blocks from __sys_alloc 0x803A3A4C;
//       - AudioGCN's buffers and an SDK pair 0x8038A0DC / 0x8038A104.
//   * Globals (SDA, r13 = 0x805EC500): HeapArray r13+0x5A60, NumHeaps +0x5A64, ArenaStart +0x5A68, ArenaEnd +0x5A6C,
//     the current heap r13+0x26E0 (-1: none).
//   * HeapDesc (12 bytes): size, free list, allocated list. Cell (header 0x20 bytes, data at +0x20): prev +0, next +4,
//     size +8 (header included). OSAllocFromHeap rounds (size + 0x20) up to 32, takes the first free cell that fits,
//     keeps a leftover under 0x40 inside the block, and puts the block at the head of the allocated list.
//
// Every pointer the walk follows must lie inside [ArenaStart, ArenaEnd) and be 32-byte aligned, and every list is
// bounded, so a corrupt heap ends the walk with an error instead of a fault.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace gekko {

struct OsHeapCensus {
    int index = 0;
    std::uint32_t desc = 0;            // the HeapDesc's guest address
    std::int32_t size = 0;             // HeapDesc.size (-1: not created / destroyed)
    std::uint32_t allocated_bytes = 0, allocated_cells = 0;
    std::uint32_t free_bytes = 0, free_cells = 0, largest_free = 0;
    bool free_sorted = true;           // free cells in address order
    bool free_coalesced = true;        // no two free cells touch (OSFreeToHeap merges neighbours)
    std::string error;                 // non-empty: the walk stopped (a pointer outside the arena, a cycle ...)
};

struct OsHeapsCensus {
    std::uint32_t heap_array = 0, arena_start = 0, arena_end = 0;
    std::int32_t heaps = 0, current = -1;
    std::vector<OsHeapCensus> census;
    std::string error;
};

// read32: a guest word (big-endian value). Reads only inside the arena and the SDA globals.
using GuestRead32 = std::function<std::uint32_t(std::uint32_t address)>;

inline constexpr std::uint32_t kSdaBase = 0x805EC500u;  // r13
OsHeapsCensus census_os_heaps(const GuestRead32& read32, std::uint32_t sda_base = kSdaBase);

// GEKKO_HEAP_TRACE=<csv path> (diagnostics): the census of every OS heap once per second of game time, appended as
// rows (game seconds, heap, size, allocated bytes / cells, free bytes / cells, largest free, sorted, coalesced, error).
// Call tick() from the game thread's host service; it reads guest memory only on the second boundaries.
class HeapTrace {
public:
    static void tick(std::uint64_t game_ticks, const GuestRead32& read32);
};

// For a test or a report: every cell of a list, in list order (address, size).
std::vector<std::pair<std::uint32_t, std::uint32_t>> os_heap_list(const GuestRead32& read32, std::uint32_t head,
                                                                   std::uint32_t arena_start, std::uint32_t arena_end,
                                                                   std::string* error);

}  // namespace gekko
