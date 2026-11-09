#pragma once

// Voice slots for an AX frame's parameter-block chain (System::select_ax_parameter_blocks).
//
// The chain is the list of PB addresses the frame's command list links, in link order. A PB keeps the voice slot it
// held in the previous frame; a new PB takes the lowest slot no chained PB holds (and that slot's host state is reset);
// a slot whose PB left the chain is reset and emptied. These are the rules of the former quadratic scans, kept
// exactly (the lowest slot when two slots hold one PB, zero slots included); only the search is sorted.

#include <algorithm>
#include <array>
#include <cstdint>

#include "gekko/types.h"

namespace gekko {

inline constexpr u32 kAxVoiceSlots = 64u;
inline constexpr u32 kAxParameterBlockBytes = 244u;

// The index of the first chained block that overlaps an earlier one (a repeated block, so also a cycle), or `count`.
// The overlap test is the original one, on 32-bit addresses; a sorted pass rules out the common case first.
inline u32 first_ax_block_overlap(const u32* addresses, u32 count) noexcept {
    std::array<u32, kAxVoiceSlots> sorted{};
    std::copy(addresses, addresses + count, sorted.begin());
    std::sort(sorted.begin(), sorted.begin() + count);
    bool any = false;
    for (u32 i = 1; i < count && !any; ++i)
        any = u64(sorted[i]) < u64(sorted[i - 1]) + kAxParameterBlockBytes;
    if (!any) return count;  // no two intervals meet in 64-bit arithmetic, so none meets in 32-bit
    for (u32 n = 0; n < count; ++n) {
        for (u32 i = 0; i < n; ++i) {
            if (addresses[n] < addresses[i] + kAxParameterBlockBytes &&
                addresses[i] < addresses[n] + kAxParameterBlockBytes)
                return n;
        }
    }
    return count;
}

// Assigns `count` (at most kAxVoiceSlots) distinct, non-overlapping chained blocks to slots. `voice_pb` is updated
// in place, `order[i]` is the slot of addresses[i]; the result has bit v set for every slot whose host state must be
// reset (a slot given a new block, or emptied).
inline u64 assign_ax_voices(const u32* addresses, u32 count, std::array<u32, kAxVoiceSlots>& voice_pb,
                            std::array<u32, kAxVoiceSlots>& order) noexcept {
    // (block, slot) for every slot, ordered: the first entry of a block is its lowest slot.
    std::array<u64, kAxVoiceSlots> held{};
    for (u32 voice = 0; voice < kAxVoiceSlots; ++voice) held[voice] = (u64(voice_pb[voice]) << 32) | voice;
    std::sort(held.begin(), held.end());
    std::array<u32, kAxVoiceSlots> chained{};
    std::copy(addresses, addresses + count, chained.begin());
    std::sort(chained.begin(), chained.begin() + count);
    u64 retained = 0;
    for (const u64 entry : held) {
        if (std::binary_search(chained.begin(), chained.begin() + count, u32(entry >> 32)))
            retained |= u64(1) << u32(entry);
    }
    u64 reset = 0;
    u32 free_slot = 0;  // retained slots only accumulate, so the lowest free one never moves down
    for (u32 i = 0; i < count; ++i) {
        const u64 key = u64(addresses[i]) << 32;
        const auto it = std::lower_bound(held.begin(), held.end(), key);
        u32 voice;
        if (it != held.end() && (*it >> 32) == addresses[i]) {
            voice = u32(*it);
        } else {
            while ((retained >> free_slot) & 1u) ++free_slot;  // count <= kAxVoiceSlots: a free slot exists
            voice = free_slot;
            reset |= u64(1) << voice;
            voice_pb[voice] = addresses[i];
            retained |= u64(1) << voice;
        }
        order[i] = voice;
    }
    for (u32 voice = 0; voice < kAxVoiceSlots; ++voice) {
        if (!((retained >> voice) & 1u)) {
            reset |= u64(1) << voice;
            voice_pb[voice] = 0u;
        }
    }
    return reset;
}

// The common frame: the chain is the one the previous accepted frame assigned. After assign_ax_voices, slot
// order[i] holds addresses[i]; nothing but that assignment, the host-state reset and a checkpoint load writes the
// slots, and the last two set the previous count to zero. So with a nonzero, equal count and every slot still
// holding its block, `addresses` is the previous (validated) chain, and assigning it again is idempotent: each block
// keeps its slot, no block is new, and the slots that are empty now are the only ones reset (again).
inline bool ax_chain_unchanged(const u32* addresses, u32 count, const std::array<u32, kAxVoiceSlots>& voice_pb,
                               const std::array<u32, kAxVoiceSlots>& order, u32 previous_count) noexcept {
    if (count == 0u || count != previous_count) return false;
    bool same = true;
    for (u32 i = 0; i < count; ++i) same &= voice_pb[order[i]] == addresses[i];
    return same;
}
inline u64 ax_empty_slots(const std::array<u32, kAxVoiceSlots>& voice_pb) noexcept {
    u64 empty = 0;
    for (u32 voice = 0; voice < kAxVoiceSlots; ++voice) empty |= u64(voice_pb[voice] == 0u) << voice;
    return empty;
}

}  // namespace gekko
