#pragma once

// The Dolphin OS heap routines, owned natively (docs/agents/os-other.md; removal plan service os.other, group A).
//
// Original behaviour (main.dol, PAL GUPP8P; docs/MEMORY_ALLOCATORS.md section 2). All heap state is guest memory and
// the game reads and compares it, so the owners keep it there and write exactly the words the originals write:
//   OSInitAlloc    0x80372138  (r3 = array start, r4 = arena end, r5 = heaps): the heap array r13+0x5A60, NumHeaps
//                              +0x5A64, ArenaEnd +0x5A6C and ArenaStart +0x5A68 (the array's end, 32-aligned), the
//                              current heap +0x26E0 = -1, every descriptor {-1, 0, 0}; returns ArenaStart
//   OSCreateHeap   0x803721A8  (r3 = start, r4 = end): the first unused descriptor (size < 0) gets the aligned region
//                              as one free cell {0, 0, size}; returns its index, -1 when none is free
//   OSSetCurrentHeap 0x80372128 (r3 = heap): swaps r13+0x26E0, returns the old one
//   OSAllocFromHeap 0x80371FB0 (r3 = heap, r4 = size): first fit along the address-ordered free list for
//                              round32(size + 0x20), a leftover under 0x40 stays in the cell, otherwise the cell is
//                              split; the cell goes to the head of the allocated list; returns cell + 0x20 or 0
//   OSFreeToHeap   0x803720AC  (r3 = heap, r4 = pointer): unlinks the cell from the allocated list and inserts it into
//                              the free list in address order, merging touching neighbours (the helper 0x80371F04,
//                              whose only caller this is, is part of the owner)
//
// What is kept: the heap words (descriptors, cells, both lists, the arena cells), written exactly as the originals
// write them, and the result in r3. The compares keep their signedness (OSAllocFromHeap compares the rounded size with
// the cell size as signed words, so a request above 0x7FFFFFE0 wraps and fits the first cell, like the original).
// Not kept: the scratch registers, CR, CTR and the stack frame OSFreeToHeap leaves behind - no caller can rely on them.
//
// Also not kept: an endless loop. A cyclic free list sends the original into an endless guest loop; the owners count the
// steps and stop the run with an execution fault past a bound no real list comes near.

#include "gekko/types.h"

#include <cstddef>

namespace gekko {
class Cpu;
}

namespace gekko::os_heap {

constexpr u32 kAllocFromHeap = 0x80371FB0u;
constexpr u32 kFreeToHeap = 0x803720ACu;
constexpr u32 kSetCurrentHeap = 0x80372128u;
constexpr u32 kInitAlloc = 0x80372138u;
constexpr u32 kCreateHeap = 0x803721A8u;

// Small-data cells (offsets from r13).
constexpr u32 kHeapArray = 0x5A60u;
constexpr u32 kNumHeaps = 0x5A64u;
constexpr u32 kArenaStart = 0x5A68u;
constexpr u32 kArenaEnd = 0x5A6Cu;
constexpr u32 kCurrentHeap = 0x26E0u;

// A list walk longer than this is a cycle (a real free list is a few hundred cells).
constexpr u32 kWalkLimit = 1u << 24;

// Each serves one call and leaves the registers as the original does. False when a list walk passed kWalkLimit (the
// registers and memory are then unspecified and the owner stops the run).
bool serve_init_alloc(Cpu& cpu);
bool serve_create_heap(Cpu& cpu);
void serve_set_current_heap(Cpu& cpu);
bool serve_alloc_from_heap(Cpu& cpu);
bool serve_free_to_heap(Cpu& cpu);

// Must equal this owner's rows of extract/native-owners.txt (native_file_api_tests).
struct Owners {
    struct Address {
        u32 address;
        const char* name;
    };
    static const Address* addresses(std::size_t& count) noexcept {
        static constexpr Address kAddresses[] = {{kAllocFromHeap, "OSAllocFromHeap"},
                                                 {kFreeToHeap, "OSFreeToHeap"},
                                                 {kSetCurrentHeap, "OSSetCurrentHeap"},
                                                 {kInitAlloc, "OSInitAlloc"},
                                                 {kCreateHeap, "OSCreateHeap"}};
        count = sizeof(kAddresses) / sizeof(kAddresses[0]);
        return kAddresses;
    }
};

// Installs the five owners.
void install(Cpu& cpu);

}  // namespace gekko::os_heap
