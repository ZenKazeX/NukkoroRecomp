#pragma once

// The DSP side's lagging copy of the voice parameter blocks, kept natively (docs/agents/audio-ax.md, stage e step 2).
//
// On the console the AX microcode reads its parameter blocks (PB, 0xF4 bytes per voice) from a RAM array that the SDK fills from the
// guest's AXVPB records once per AI frame (__AXSyncPBs), so the mixer always sees the voice parameters of the PREVIOUS sync: the
// setters and MIXUpdateSettings that run in the frame callback only reach the DSP at the next frame. That one-frame lag is the
// observable behaviour ("one-frame-late copy-back, preserved for identical output"), so the copy stays; it is stored here, in host
// memory, instead of in guest memory at 0x8058FCA0: nothing in the guest reads that array (static scan and guest watch), and the
// native sync and the native mixer are its only users. The update-list ring (0x100 bytes per voice, the DSP's pending-write list) is
// the other half of the array region (guest 0x805949A0).
//
// Voices are identified by the PB's former guest address (0x8058FCA0 + 0xF4 * index): the mixer's slot identity, the category
// tables and the command list's PB chain all keep naming them that way.

#include <array>
#include <cstddef>

#include "gekko/types.h"

namespace gekko {

struct AxPbImage {
    static constexpr u32 kBase = 0x8058FCA0u, kStride = 0xF4u, kCount = 64u;
    static constexpr u32 kRingBase = 0x805949A0u, kRingStride = 0x100u;

    std::array<u8, kCount * kStride> pb{};
    std::array<u8, kCount * kRingStride> ring{};

    static constexpr bool owns(u32 address) noexcept {
        return address >= kBase && address < kBase + kCount * kStride && (address - kBase) % kStride == 0u;
    }
    static constexpr u32 index_of(u32 address) noexcept { return (address - kBase) / kStride; }

    void clear() noexcept {
        pb.fill(0);
        ring.fill(0);
    }
    u8* block(u32 index) noexcept { return pb.data() + std::size_t{index} * kStride; }
    const u8* block(u32 index) const noexcept { return pb.data() + std::size_t{index} * kStride; }

    // Big-endian accessors at any byte offset of a block (the SDK copies some words at halfword alignment).
    u16 r16(u32 index, u32 offset) const noexcept {
        const u8* p = block(index) + offset;
        return static_cast<u16>((u16{p[0]} << 8) | p[1]);
    }
    void w16(u32 index, u32 offset, u16 value) noexcept {
        u8* p = block(index) + offset;
        p[0] = static_cast<u8>(value >> 8);
        p[1] = static_cast<u8>(value);
    }
    u32 r32(u32 index, u32 offset) const noexcept {
        const u8* p = block(index) + offset;
        return (u32{p[0]} << 24) | (u32{p[1]} << 16) | (u32{p[2]} << 8) | p[3];
    }
    void w32(u32 index, u32 offset, u32 value) noexcept {
        u8* p = block(index) + offset;
        p[0] = static_cast<u8>(value >> 24);
        p[1] = static_cast<u8>(value >> 16);
        p[2] = static_cast<u8>(value >> 8);
        p[3] = static_cast<u8>(value);
    }
    // The update-list ring: word `n` of voice `index`'s list (the arrays are contiguous, like the guest region was).
    u32 ring_word(u32 index, u32 n) const noexcept {
        const std::size_t at = std::size_t{index} * kRingStride + std::size_t{n} * 4u;
        if (at + 4u > ring.size()) return 0u;
        const u8* p = ring.data() + at;
        return (u32{p[0]} << 24) | (u32{p[1]} << 16) | (u32{p[2]} << 8) | p[3];
    }
    void set_ring_word(u32 index, u32 n, u32 value) noexcept {
        const std::size_t at = std::size_t{index} * kRingStride + std::size_t{n} * 4u;
        if (at + 4u > ring.size()) return;
        u8* p = ring.data() + at;
        p[0] = static_cast<u8>(value >> 24);
        p[1] = static_cast<u8>(value >> 16);
        p[2] = static_cast<u8>(value >> 8);
        p[3] = static_cast<u8>(value);
    }
};

}  // namespace gekko
