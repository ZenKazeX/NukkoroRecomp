#pragma once

// The AX library's per-voice sync bookkeeping, kept natively (native/win N9).
//
// Five words of every AXVPB record exist only for the SDK's own AXVPB -> PB exchange (__AXSyncPB):
//     +0x1C  the dirty mask the voice setters OR into and the sync consumes and clears
//     +0x20  "depop pending" (set when a running voice is stopped or taken; cleared by the sync)
//     +0x24  cleared by the sync (never read)
//     +0x28  the number of words in the pending-write ring (cleared by the sync)
//     +0x30  the ring's write pointer (reset to vpb + 0x34 by the sync)
// Every SDK function that touches them is a native owner (the voice setters, MIXInitChannel, MIXUpdateSettings,
// AXAcquireVoice, AXFreeVoice, AXInit, __AXSyncPBs), and the guest watch of the game's audio (docs/NATIVE_WINDOWS_MIGRATION.md,
// N9) finds no translated code reading or writing them: the game's sound driver and CRI's reach a voice through those
// owners. So the words live here, in host memory, and the owners use them directly; the guest copy at the same
// offsets is not maintained while the game runs (the sync alone wrote 4 to 5 of them per voice every 5 ms frame).
//
// The guest image still describes them wherever the machine is described in the guest's terms: publish_to_guest
// before a checkpoint or a state report, import_from_guest after a checkpoint is loaded (every checkpoint version
// carries them in its guest image, so none needs converting).
//
// A voice record outside the SDK's array (never produced by the SDK) keeps using guest memory: `owns` decides.
//
// verify_guest_untouched is the proof obligation made checkable: with it set, the sync compares the guest's copy of
// the five words of every voice with what this runtime last put there and stops the run on a difference, i.e. on a
// write by code that is not one of the owners. Diagnostic only (it reads guest memory every frame).

#include <array>
#include <cstddef>

#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/types.h"

namespace gekko {

struct AxVoiceWords {
    static constexpr u32 kVpbBase = 0x805989A0u, kVpbStride = 0x22Cu, kCount = 64u;
    enum Word : u32 { kFlags = 0, kDepop = 1, kWord24 = 2, kRingCount = 3, kRingPointer = 4, kWords = 5 };
    static constexpr u32 kOffset[kWords] = {0x1Cu, 0x20u, 0x24u, 0x28u, 0x30u};

    std::array<std::array<u32, kWords>, kCount> voice{};
    std::array<std::array<u32, kWords>, kCount> guest{};  // what the guest image holds (last publish / import / AXInit)
    bool verify_guest_untouched = false;

    static constexpr bool owns(u32 vpb) noexcept {
        return vpb >= kVpbBase && vpb < kVpbBase + kCount * kVpbStride && (vpb - kVpbBase) % kVpbStride == 0u;
    }
    static constexpr u32 index_of(u32 vpb) noexcept { return (vpb - kVpbBase) / kVpbStride; }

    void clear() noexcept {
        voice = {};
        guest = {};
    }
    // AXInit zeroed the guest's voice array: the guest copy of every word is 0 from here on.
    void note_guest_zeroed() noexcept { guest = {}; }

    void publish_to_guest(GuestMemory& memory) {
        for (u32 i = 0; i < kCount; ++i)
            for (u32 w = 0; w < kWords; ++w) {
                if (guest[i][w] == voice[i][w]) continue;
                memory.write32(kVpbBase + i * kVpbStride + kOffset[w], voice[i][w]);
                guest[i][w] = voice[i][w];
            }
    }
    void import_from_guest(GuestMemory& memory) {
        for (u32 i = 0; i < kCount; ++i)
            for (u32 w = 0; w < kWords; ++w) voice[i][w] = guest[i][w] = memory.read32(kVpbBase + i * kVpbStride + kOffset[w]);
    }
    // The first voice word whose guest copy is not what this runtime left there; false when all are untouched.
    bool guest_differs(GuestMemory& memory, u32& address, u32& found, u32& expected) const {
        for (u32 i = 0; i < kCount; ++i)
            for (u32 w = 0; w < kWords; ++w) {
                const u32 at = kVpbBase + i * kVpbStride + kOffset[w];
                const u32 value = memory.read32(at);
                if (value == guest[i][w]) continue;
                address = at;
                found = value;
                expected = guest[i][w];
                return true;
            }
        return false;
    }
};

// One voice's words, resolved once: `native` is the voice's five words, or null when they are in guest memory.
// Resolving costs a range check and a division by the record size; the per-frame sync touches up to seven words of
// every voice, so it resolves a voice once and uses this (measured 2026-11-18: resolving per word made the native
// arrangement 0.26 us per frame SLOWER than the guest words it replaced).
struct AxVoiceWordRef {
    Cpu& c;
    u32* native;
    u32 vpb;
    u32 get(AxVoiceWords::Word word) const { return native != nullptr ? native[word] : c.mem_read32(vpb + AxVoiceWords::kOffset[word]); }
    void set(AxVoiceWords::Word word, u32 value) const {
        if (native != nullptr) native[word] = value;
        else c.mem_write32(vpb + AxVoiceWords::kOffset[word], value);
    }
};

// One word of one voice, wherever it lives: native for a record of the SDK's array when `words` is given, guest
// memory otherwise (no native storage attached - the contract tests' reference arrangement - or a foreign record).
struct AxVoiceWordAccess {
    Cpu& c;
    AxVoiceWords* words;
    AxVoiceWordRef of(u32 vpb) const {
        return {c, words != nullptr && AxVoiceWords::owns(vpb) ? words->voice[AxVoiceWords::index_of(vpb)].data() : nullptr, vpb};
    }
    u32 get(u32 vpb, AxVoiceWords::Word word) const {
        if (words != nullptr && AxVoiceWords::owns(vpb)) return words->voice[AxVoiceWords::index_of(vpb)][word];
        return c.mem_read32(vpb + AxVoiceWords::kOffset[word]);
    }
    void set(u32 vpb, AxVoiceWords::Word word, u32 value) const {
        if (words != nullptr && AxVoiceWords::owns(vpb)) {
            words->voice[AxVoiceWords::index_of(vpb)][word] = value;
            return;
        }
        c.mem_write32(vpb + AxVoiceWords::kOffset[word], value);
    }
    void or_flags(u32 vpb, u32 bits) const { set(vpb, AxVoiceWords::kFlags, get(vpb, AxVoiceWords::kFlags) | bits); }
};

}  // namespace gekko
