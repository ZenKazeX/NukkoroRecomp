#pragma once

// The three further effects of the SDK's AXFX library that the game's sound start-up initializes and never uses,
// owned natively (docs/agents/axfx-effects.md; the method is docs/GC_REMOVAL_PLAN.md "Step 2 done 2026-11-07").
//
// Original behaviour (main.dol, PAL GUPP8P): the sound system's start-up 0x8043F9A0 initializes four AXFX effect
// objects inside the sound system object (GCAX + 0x10000 + offset): ReverbStd at -0x1FC, ReverbHi at -0xA8 (owned,
// native_reverb_owner.h), Chorus at +0x138 and Delay at +0x1D4; the shutdown 0x8043F674 releases them in the same
// order. The effect selector 0x8043F304 could register any of the four effects' callbacks as an auxiliary callback
// (mode 1 ReverbStd 0x803890B0, mode 2 ReverbHi 0x803885A0, mode 3 Chorus 0x8038980C, mode 4 Delay 0x80389B5C), but
// its only two callers (0x80440290 enable, 0x8044024C disable) pass the constants 2 and 0; AXRegisterAuxACallback /
// AXRegisterAuxBCallback (0x80385050 / 0x8038505C) are called from nowhere else, no data word and no instruction
// pair holds the addresses of the other three callbacks outside the selector. So these three effects are initialized,
// parameter-checked and released, and their callbacks never run: nothing reads the state init builds.
//
// What the translated init does: it validates the parameters the game stored in the object (ReverbStd: five floats
// at +0x140..+0x150, with ordered compares; Chorus: three words at +0x90, +0x94, +0x98; Delay: nine words at +0x3C..
// +0x5C), returns 0 at once when one is out of range, and otherwise allocates the effect's delay lines from the game's
// sound heap through the AXFX alloc hook (r13 + 0x27B8), describes them in the object and returns 1 (0 when the heap
// cannot serve a request, after freeing what it took). Shutdown frees the lines through the free hook (r13 + 0x27BC)
// and returns 1. No caller uses the result: the start-up and shutdown routines reload r3 after every call but the last one, and
// the last (Delay) is followed by a caller that overwrites r3 (li r3, 0) before it reads it; it is kept exact anyway.
//
// Native implementation: the owners return the original's verdict on the parameters and write nothing else - the
// delay lines and the object's descriptors are retired state (guest watch over a full Westopolis replay: no reader).
// They allocate nothing, so the sound heap keeps the memory, and a heap that could not serve the requests no longer
// turns into a refused init (the game's sound heap never does in a replay). Interrupt masking inside the originals
// (OSDisableInterrupts / OSRestoreInterrupts around the setup) restores the entry state and is not reproduced.

#include "gekko/types.h"

#include <cstddef>

namespace gekko {
class Cpu;
}

namespace gekko::axfx_effects {

constexpr u32 kStdInit = 0x80388FF8u;       // AXFXReverbStdInit(context)
constexpr u32 kStdShutdown = 0x80389064u;   // AXFXReverbStdShutdown(context)
constexpr u32 kChorusInit = 0x80389464u;    // AXFXChorusInit(context)
constexpr u32 kChorusShutdown = 0x803896ACu;
constexpr u32 kDelayInit = 0x80389FF4u;     // AXFXDelayInit(context)
constexpr u32 kDelayShutdown = 0x8038A03Cu;

// Parameter fields of the three objects, as the originals read them.
constexpr u32 kStdParametersOffset = 0x140u;  // f32 at +0x140 coloration 0..1, +0x144 mix 0..1, +0x148 time 0.01..10, +0x14C damping 0..1, +0x150 pre-delay 0..0.1
constexpr u32 kChorusParametersOffset = 0x90u;  // u32 baseDelay 5..15, variation 0..5, period 500..10000
constexpr u32 kDelayParametersOffset = 0x3Cu;   // u32 delay[3] 10..5000, feedback[3] 0..100, output[3] 0..100

// The verdict of the original init on the parameters in the object: true when it would go on to build the effect.
// False when the object's parameters are not readable guest memory (the caller stops the run: the original faults).
enum class Verdict : u8 { Accepted, Refused, Unreadable };
Verdict judge_std(const Cpu& cpu, u32 context) noexcept;
Verdict judge_chorus(const Cpu& cpu, u32 context) noexcept;
Verdict judge_delay(const Cpu& cpu, u32 context) noexcept;

// Init (r3 = context): r3 = 1 when accepted, else 0. Returns false for an unreadable object.
bool serve_std_init(Cpu& cpu) noexcept;
bool serve_chorus_init(Cpu& cpu) noexcept;
bool serve_delay_init(Cpu& cpu) noexcept;
// Shutdown (r3 = context): r3 = 1; nothing else.
void serve_shutdown(Cpu& cpu) noexcept;

// Installs the six owners on the CPU (System::start). An init whose object is not readable stops the run, as the
// original would fault on it.
void install(Cpu& cpu);

// The guest entries these owners serve: must equal their rows of extract/native-owners.txt (native_file_api_tests).
struct Owners {
    struct Address {
        u32 address;
        const char* name;
    };
    static const Address* addresses(std::size_t& count) noexcept {
        static constexpr Address kAddresses[] = {{kStdInit, "AXFXReverbStdInit"},   {kStdShutdown, "AXFXReverbStdShutdown"},
                                                 {kChorusInit, "AXFXChorusInit"},   {kChorusShutdown, "AXFXChorusShutdown"},
                                                 {kDelayInit, "AXFXDelayInit"},     {kDelayShutdown, "AXFXDelayShutdown"}};
        count = sizeof(kAddresses) / sizeof(kAddresses[0]);
        return kAddresses;
    }
};

}  // namespace gekko::axfx_effects
