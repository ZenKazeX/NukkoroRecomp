#pragma once

// Sonicteam::System::Heap in C++20 (docs/MEMORY_ALLOCATORS.md; reverse-engineered 2026-11-04 from main.dol PAL).
//
// A free-list heap inside a buffer its owner supplies. Its only user is the effect system: EffectSystemUSA's
// constructor (vf0) builds one. The object is 8 bytes: +0 the vtable (0x8056AF2C; IHeap's 0x8051E300 first), +4 the
// buffer. The buffer begins with a two-word anchor {first free block, 0}; the 0 is a size, so the anchor never
// coalesces with a block. Every block has an 8-byte header {next free (free blocks only), size (header included)};
// the free list is singly linked and kept in address order.
//
//   ctor 0x8040C040 (this, buffer, size)  anchor {buffer + 8, 0}; one free block {0, size - 8} at buffer + 8
//   vf0  0x8040BFA8 Alloc(size)           need = RoundUp(size + 8, 8); first fit; an exact fit is unlinked, a larger
//                                         block keeps `need` and its tail becomes the free block in its place
//   vf1  0x8040BEB0 AllocAligned(a, size) need = a + RoundUp(size + 8, 8); first fit; data = RoundUp(block + 8, a):
//                                         the padding in front stays a free block, the rest is split as in Alloc
//   vf2  0x8040BE60 Calloc(size)          this->vf0(size), then memset 0 of `size` bytes
//   vf3  (IHeap 0x80045FC4)               nothing
//   vf4  0x8040BDB8 Free(p)               NULL: nothing; the block (p - 8) goes into the address-ordered list and
//                                         merges with the next and the previous free block when they touch
//   vf5  0x8040BD88 LargestFree()         the largest free block's size (header included), 0 if none
//   vf6  0x8040BCB8 Realloc(p, size)      NULL: this->vf0(size); the block's size == size: p itself (the original
//                                         compares the header-inclusive size with the request - almost never equal);
//                                         else q = this->vf0(size), copy min(size, block size - 8), this->vf4(p)
//   vf7  0x8040BC90 TotalFree()           the sum of the free blocks' sizes
// RoundUp(x, a) = (x + a - 1) & ~(a - 1) (0x80045FC8). vf2 / vf6 call vf0 / vf4 through the vtable; this class has
// one vtable in the game, so the native code calls its own functions.
//
// Memory is reached through GuestAccess so the same code runs on a test bus or the game's memory.

#include <cstdint>
#include <functional>

namespace gekko::sonicteam {

struct GuestAccess {
    std::function<std::uint32_t(std::uint32_t)> read32;
    std::function<void(std::uint32_t, std::uint32_t)> write32;
    std::function<void(std::uint32_t dst, std::uint32_t src, std::uint32_t n)> copy;  // memcpy
    std::function<void(std::uint32_t dst, std::uint8_t value, std::uint32_t n)> fill;  // memset
};

inline constexpr std::uint32_t kHeapVtable = 0x8056AF2Cu;

class Heap {
public:
    Heap(const GuestAccess& memory, std::uint32_t self) : m_(memory), self_(self) {}

    void construct(std::uint32_t buffer, std::uint32_t size);
    std::uint32_t alloc(std::uint32_t size);
    std::uint32_t alloc_aligned(std::uint32_t align, std::uint32_t size);
    std::uint32_t calloc(std::uint32_t size);
    void free(std::uint32_t pointer);
    std::uint32_t largest_free() const;
    std::uint32_t realloc(std::uint32_t pointer, std::uint32_t size);
    std::uint32_t total_free() const;

    static constexpr std::uint32_t round_up(std::uint32_t x, std::uint32_t a) { return (x + a - 1u) & ~(a - 1u); }

private:
    std::uint32_t r(std::uint32_t a) const { return m_.read32(a); }
    void w(std::uint32_t a, std::uint32_t v) const { m_.write32(a, v); }
    std::uint32_t anchor() const { return r(self_ + 4u); }

    const GuestAccess& m_;
    std::uint32_t self_;
};

}  // namespace gekko::sonicteam
