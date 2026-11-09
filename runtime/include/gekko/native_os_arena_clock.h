#pragma once

// The OS arena accessors and the time-base readers, owned natively (docs/agents/axfx-effects.md, "Step B: os.memory
// and os.time"). Original behaviour (main.dol, PAL GUPP8P), all leaf routines:
//
//   0x80372214 OSGetArenaHi   r3 = *(u32*)(r13 + 0x5A70)           (a small-data cell)
//   0x8037221C OSGetArenaLo   r3 = *(u32*)(r13 + 0x26E8)
//   0x8037222C OSSetArenaLo   *(u32*)(r13 + 0x26E8) = r3
//   0x80379628 OSGetTime      r3:r4 = the 64-bit time base (mftbu / mftb / mftbu until the upper word is stable)
//   0x80379640 OSGetTick      r3 = the lower 32 bits of the time base (mftb)
//
// The arena cells stay guest memory: the OS start-up (OSInit, still compiled) and the game's own allocators
// (0x80050C98 and the two start-up routines 0x803A3994 / 0x803A3A4C) read and write them. The owners read and write
// exactly those cells, so the sound heap, the OS heap and the C runtime see the same values as before. OSSetArenaHi
// (0x80372224) is not a frontier row: only OSInit calls it, and it goes with OSInit's own owner.
//
// The time base is the runtime's own game clock (Cpu::time_base(): GameClock plus the guest's offset, 40.5 MHz). The
// guest runs in zero game time (game_clock.h), so the original's retry loop never retries; the owners return the
// clock directly. Nothing emulates a hardware time base register here, the value is the native clock's.
//
// What differs from the originals (and is not observable under the calling convention): OSGetTime leaves r5 (the
// second read of the upper word) and cr0 (the compare of the two reads) untouched where the original sets them to
// the upper word and "equal". Both are volatile and no caller of the five entries reads them.

#include "gekko/types.h"

#include <cstddef>

namespace gekko {
class Cpu;
}

namespace gekko::os_arena_clock {

constexpr u32 kGetArenaHi = 0x80372214u;
constexpr u32 kGetArenaLo = 0x8037221Cu;
constexpr u32 kSetArenaLo = 0x8037222Cu;
constexpr u32 kGetTime = 0x80379628u;
constexpr u32 kGetTick = 0x80379640u;

// Small-data cells (offsets from r13) the arena accessors read and write.
constexpr u32 kArenaHiCell = 0x5A70u;
constexpr u32 kArenaLoCell = 0x26E8u;

void serve_get_arena_hi(Cpu& cpu);   // r3 = arena hi
void serve_get_arena_lo(Cpu& cpu);   // r3 = arena lo
void serve_set_arena_lo(Cpu& cpu);   // arena lo = r3
void serve_get_time(Cpu& cpu);       // r3 = time base (high word), r4 = time base (low word)
void serve_get_tick(Cpu& cpu);       // r3 = time base (low word)

// Installs the five owners on the CPU (System::start).
void install(Cpu& cpu);

// The guest entries these owners serve: must equal their rows of extract/native-owners.txt (native_file_api_tests).
struct Owners {
    struct Address {
        u32 address;
        const char* name;
    };
    static const Address* addresses(std::size_t& count) noexcept {
        static constexpr Address kAddresses[] = {{kGetArenaHi, "OSGetArenaHi"}, {kGetArenaLo, "OSGetArenaLo"},
                                                 {kSetArenaLo, "OSSetArenaLo"}, {kGetTime, "OSGetTime"},
                                                 {kGetTick, "OSGetTick"}};
        count = sizeof(kAddresses) / sizeof(kAddresses[0]);
        return kAddresses;
    }
};

}  // namespace gekko::os_arena_clock
