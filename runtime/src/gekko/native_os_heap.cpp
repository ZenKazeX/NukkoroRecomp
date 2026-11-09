// The native owners of the Dolphin OS heap routines (gekko/native_os_heap.h).
//
// The heap descriptors, cells and free / allocated lists are guest memory (the game stores and compares the
// pointers), so each owner works on those words exactly as the original does and returns the original result in r3.
// Nothing else of the original is kept: no stack frame, no scratch register, no condition register.

#include "gekko/native_os_heap.h"

#include "gekko/cpu.h"

#include <string>

namespace gekko::os_heap {

namespace {

constexpr u32 kAlign = 0xFFFFFFE0u;

inline u32 rd(Cpu& cpu, u32 address) { return cpu.mem_read32(address); }
inline void wr(Cpu& cpu, u32 address, u32 value) { cpu.mem_write32(address, value); }

// Cell: prev +0, next +4, size +8 (header included); data at +0x20. Descriptor: size +0, free list +4, allocated +8.

// Inserts `cell` into the address-ordered free list `head`, merging with the next and the previous cell when they
// touch; returns the new head. False on a cyclic list.
bool insert_free(Cpu& cpu, u32 head, u32 cell, u32& new_head) {
    u32 next = head, prev = 0u, steps = 0u;
    while (next != 0u && cell > next) {
        if (++steps > kWalkLimit) return false;
        prev = next;
        next = rd(cpu, next + 4u);
    }
    wr(cpu, cell + 4u, next);
    wr(cpu, cell, prev);
    if (next != 0u) {
        wr(cpu, next, cell);
        const u32 size = rd(cpu, cell + 8u);
        if (cell + size == next) {  // touches the next free cell: absorb it
            wr(cpu, cell + 8u, size + rd(cpu, next + 8u));
            next = rd(cpu, next + 4u);
            wr(cpu, cell + 4u, next);
            if (next != 0u) wr(cpu, next, cell);
        }
    }
    if (prev == 0u) {
        new_head = cell;
        return true;
    }
    wr(cpu, prev + 4u, cell);
    const u32 size = rd(cpu, prev + 8u);
    if (prev + size == cell) {  // touches the previous free cell: it absorbs this one
        wr(cpu, prev + 8u, size + rd(cpu, cell + 8u));
        wr(cpu, prev + 4u, next);
        if (next != 0u) wr(cpu, next, prev);
    }
    new_head = head;
    return true;
}

}  // namespace

bool serve_init_alloc(Cpu& cpu) {
    auto& g = cpu.gpr;
    const u32 sda = g[13];
    const u32 array = g[3], arena_end = g[4], heaps = g[5];
    wr(cpu, sda + kHeapArray, array);
    wr(cpu, sda + kNumHeaps, heaps);
    for (u32 i = 0; static_cast<s32>(i) < static_cast<s32>(heaps); ++i) {
        if (i > kWalkLimit) return false;
        const u32 descriptor = array + 12u * i;
        wr(cpu, descriptor, 0xFFFFFFFFu);
        wr(cpu, descriptor + 8u, 0u);
        wr(cpu, descriptor + 4u, 0u);
    }
    wr(cpu, sda + kArenaEnd, arena_end & kAlign);
    wr(cpu, sda + kCurrentHeap, 0xFFFFFFFFu);
    const u32 arena_start = (array + 12u * heaps + 0x1Fu) & kAlign;
    wr(cpu, sda + kArenaStart, arena_start);
    g[3] = arena_start;
    return true;
}

bool serve_create_heap(Cpu& cpu) {
    auto& g = cpu.gpr;
    const u32 sda = g[13];
    const u32 heaps = rd(cpu, sda + kNumHeaps), array = rd(cpu, sda + kHeapArray);
    const u32 start = (g[3] + 0x1Fu) & kAlign, end = g[4] & kAlign;
    for (u32 i = 0; static_cast<s32>(i) < static_cast<s32>(heaps); ++i) {
        if (i > kWalkLimit) return false;
        const u32 descriptor = array + 12u * i;
        if (static_cast<s32>(rd(cpu, descriptor)) >= 0) continue;  // in use
        wr(cpu, descriptor, end - start);
        wr(cpu, start, 0u);
        wr(cpu, start + 4u, 0u);
        wr(cpu, start + 8u, rd(cpu, descriptor));
        wr(cpu, descriptor + 4u, start);
        wr(cpu, descriptor + 8u, 0u);
        g[3] = i;
        return true;
    }
    g[3] = 0xFFFFFFFFu;
    return true;
}

void serve_set_current_heap(Cpu& cpu) {
    auto& g = cpu.gpr;
    const u32 old = rd(cpu, g[13] + kCurrentHeap);
    wr(cpu, g[13] + kCurrentHeap, g[3]);
    g[3] = old;
}

bool serve_alloc_from_heap(Cpu& cpu) {
    auto& g = cpu.gpr;
    const u32 descriptor = rd(cpu, g[13] + kHeapArray) + g[3] * 12u;
    // The rounded size is compared as a signed word, as the original does.
    const u32 need = (g[4] + 0x3Fu) & kAlign;
    u32 cell = rd(cpu, descriptor + 4u), steps = 0u;
    while (cell != 0u && !(static_cast<s32>(need) <= static_cast<s32>(rd(cpu, cell + 8u)))) {
        if (++steps > kWalkLimit) return false;
        cell = rd(cpu, cell + 4u);
    }
    if (cell == 0u) {
        g[3] = 0u;
        return true;
    }
    const u32 leftover = rd(cpu, cell + 8u) - need;
    if (leftover < 0x40u) {  // the leftover stays inside the cell: unlink it from the free list
        const u32 next = rd(cpu, cell + 4u), prev = rd(cpu, cell);
        if (next != 0u) wr(cpu, next, prev);
        if (prev == 0u) wr(cpu, descriptor + 4u, next);
        else wr(cpu, prev + 4u, next);
    } else {  // split: the tail takes the cell's place in the free list
        wr(cpu, cell + 8u, need);
        const u32 tail = cell + need;
        wr(cpu, tail + 8u, leftover);
        wr(cpu, tail, rd(cpu, cell));
        wr(cpu, tail + 4u, rd(cpu, cell + 4u));
        const u32 next = rd(cpu, tail + 4u);
        if (next != 0u) wr(cpu, next, tail);
        const u32 prev = rd(cpu, tail);
        if (prev == 0u) wr(cpu, descriptor + 4u, tail);
        else wr(cpu, prev + 4u, tail);
    }
    const u32 head = rd(cpu, descriptor + 8u);  // the cell goes to the head of the allocated list
    wr(cpu, cell + 4u, head);
    wr(cpu, cell, 0u);
    if (head != 0u) wr(cpu, head, cell);
    wr(cpu, descriptor + 8u, cell);
    g[3] = cell + 0x20u;
    return true;
}

bool serve_free_to_heap(Cpu& cpu) {
    auto& g = cpu.gpr;
    const u32 cell = g[4] - 0x20u;
    const u32 descriptor = rd(cpu, g[13] + kHeapArray) + g[3] * 12u;
    u32 allocated = rd(cpu, descriptor + 8u);
    const u32 next = rd(cpu, cell + 4u), prev = rd(cpu, cell);
    if (next != 0u) wr(cpu, next, prev);  // unlink from the allocated list
    if (prev == 0u) allocated = next;
    else wr(cpu, prev + 4u, next);
    wr(cpu, descriptor + 8u, allocated);
    u32 free_head = 0u;
    if (!insert_free(cpu, rd(cpu, descriptor + 4u), cell, free_head)) return false;
    wr(cpu, descriptor + 4u, free_head);
    return true;
}

void install(Cpu& cpu) {
    const auto bind = [&](u32 address, bool (*serve)(Cpu&), const char* what) {
        cpu.install_hle(address, [serve, what](Cpu& c) {
            if (!serve(c)) {
                const std::string detail = std::string(what) + ": a heap list walk does not end (cyclic list)";
                c.request_stop(StopReason::ExecutionFault, detail);
            }
            return true;
        }, std::string(what) + " (native)");
    };
    bind(kInitAlloc, serve_init_alloc, "OSInitAlloc");
    bind(kCreateHeap, serve_create_heap, "OSCreateHeap");
    bind(kAllocFromHeap, serve_alloc_from_heap, "OSAllocFromHeap");
    bind(kFreeToHeap, serve_free_to_heap, "OSFreeToHeap");
    cpu.install_hle(kSetCurrentHeap, [](Cpu& c) {
        serve_set_current_heap(c);
        return true;
    }, "OSSetCurrentHeap (native)");
}

}  // namespace gekko::os_heap
