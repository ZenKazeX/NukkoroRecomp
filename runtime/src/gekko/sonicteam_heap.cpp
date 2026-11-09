#include "gekko/sonicteam_heap.h"

#include <algorithm>

namespace gekko::sonicteam {

void Heap::construct(std::uint32_t buffer, std::uint32_t size) {
    w(self_, kHeapVtable);
    w(self_ + 4u, buffer);
    w(buffer, buffer + 8u);  // the anchor: first free block, then a 0 "size" that never touches a block
    w(buffer + 4u, 0u);
    w(buffer + 8u, 0u);      // one free block over the rest
    w(buffer + 12u, size - 8u);
}

std::uint32_t Heap::alloc(std::uint32_t size) {
    const std::uint32_t need = round_up(size + 8u, 8u);
    for (std::uint32_t prev = anchor(), cur = r(prev); cur != 0u; prev = cur, cur = r(cur)) {
        const std::uint32_t have = r(cur + 4u);
        if (have < need) continue;
        if (have == need) {
            w(prev, r(cur));  // exact fit: unlinked
        } else {
            const std::uint32_t tail = cur + need;  // the rest stays free, in the block's place
            w(tail, r(cur));
            w(tail + 4u, have - need);
            w(cur + 4u, need);
            w(prev, tail);
        }
        return cur + 8u;
    }
    return 0u;
}

std::uint32_t Heap::alloc_aligned(std::uint32_t align, std::uint32_t size) {
    const std::uint32_t need = align + round_up(size + 8u, 8u);
    for (std::uint32_t prev = anchor(), cur = r(prev); cur != 0u; prev = cur, cur = r(cur)) {
        const std::uint32_t have = r(cur + 4u);
        if (have < need) continue;
        const std::uint32_t header = round_up(cur + 8u, align) - 8u;
        if (have == need) {
            if (header == cur) {
                w(prev, r(cur));
            } else {  // the padding in front stays free; the block from `header` on is the allocation
                w(cur + 4u, header - cur);
                w(header + 4u, need - (header - cur));
            }
        } else {
            const std::uint32_t tail = cur + need;
            w(tail, r(cur));
            w(tail + 4u, have - need);
            if (header == cur) {
                w(cur + 4u, need);
                w(prev, tail);
            } else {
                w(cur + 4u, header - cur);
                w(cur, tail);
                w(header + 4u, need - (header - cur));
            }
        }
        return header + 8u;
    }
    return 0u;
}

std::uint32_t Heap::calloc(std::uint32_t size) {
    const std::uint32_t p = alloc(size);
    if (p != 0u) m_.fill(p, 0u, size);
    return p;
}

void Heap::free(std::uint32_t pointer) {
    if (pointer == 0u) return;
    const std::uint32_t block = pointer - 8u;
    // The last free entry below the block (the anchor if none), and the first at or above it.
    std::uint32_t prev = anchor(), next = r(prev);
    while (next < block && next != 0u) {
        prev = next;
        next = r(prev);
    }
    if (next == 0u) {
        w(block, 0u);
    } else if (block + r(block + 4u) == next) {  // touches the next free block: one block
        w(block, r(next));
        w(block + 4u, r(block + 4u) + r(next + 4u));
    } else {
        w(block, next);
    }
    if (prev + r(prev + 4u) == block) {  // the previous free block touches it: one block
        w(prev, r(block));
        w(prev + 4u, r(prev + 4u) + r(block + 4u));
    } else {
        w(prev, block);
    }
}

std::uint32_t Heap::largest_free() const {
    std::uint32_t largest = 0u;
    for (std::uint32_t cur = r(anchor()); cur != 0u; cur = r(cur)) largest = std::max(largest, r(cur + 4u));
    return largest;
}

std::uint32_t Heap::realloc(std::uint32_t pointer, std::uint32_t size) {
    if (pointer == 0u) return alloc(size);
    const std::uint32_t old = r(pointer - 4u);  // the block's size, header included
    if (old == size) return pointer;
    const std::uint32_t moved = alloc(size);
    if (moved == 0u) return 0u;
    m_.copy(moved, pointer, std::min(size, old - 8u));
    free(pointer);
    return moved;
}

std::uint32_t Heap::total_free() const {
    std::uint32_t total = 0u;
    for (std::uint32_t cur = r(anchor()); cur != 0u; cur = r(cur)) total += r(cur + 4u);
    return total;
}

}  // namespace gekko::sonicteam
