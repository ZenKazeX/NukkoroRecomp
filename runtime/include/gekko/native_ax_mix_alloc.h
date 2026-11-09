#pragma once

// The two AX heap wrappers behind the MIX table entries (words 0x805EECB8 / 0x805EECBC), owned natively
// (docs/agents/os-other.md, step 11; removal plan service audio.ax, class D).
//
// Original (main.dol, PAL GUPP8P):
//   0x8038A0DC: OSAllocFromHeap(*(r13 + 0x26E0), r3)      r4 = r3; r3 = the current heap id; tail into 0x80371FB0
//   0x8038A104: OSFreeToHeap(*(r13 + 0x26E0), r3)         r4 = r3; r3 = the current heap id; tail into 0x803720AC
// Both callees are native owners (native_os_heap.h). The owners do exactly the wrappers' register setup and run the same
// heap routines on the same guest heap; no difference from the original is intended.

#include "gekko/types.h"

#include <cstddef>

namespace gekko {
class Cpu;
}

namespace gekko::ax_mix_alloc {

constexpr u32 kMixAlloc = 0x8038A0DCu;
constexpr u32 kMixFree = 0x8038A104u;

// False when a heap list walk does not end (the heap routines' own refusal).
bool serve_alloc(Cpu& cpu);
bool serve_free(Cpu& cpu);

struct Owners {
    struct Address {
        u32 address;
        const char* name;
    };
    static const Address* addresses(std::size_t& count) noexcept {
        static constexpr Address kAddresses[] = {{kMixAlloc, "AX MIX table alloc wrapper"},
                                                 {kMixFree, "AX MIX table free wrapper"}};
        count = 2;
        return kAddresses;
    }
};

void install(Cpu& cpu);

}  // namespace gekko::ax_mix_alloc
