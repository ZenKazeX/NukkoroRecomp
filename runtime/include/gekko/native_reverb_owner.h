#pragma once

// The game's room reverb, owned natively (docs/OPTIMIZATION_RESULTS.md, twenty-third increment).
//
// Original behaviour (main.dol, PAL GUPP8P): the game's effect control (0x80440290 enable, 0x8044024C disable,
// through the selector 0x8043F304) registers exactly one auxiliary callback, the SDK's ReverbHi callback 0x803885A0,
// on aux A with the context audio object + 0xFF58; nothing registers aux B. The sound system's start-up
// (0x8043F9A0) initializes that context with ReverbHiInit 0x803884E4 from the six parameters at context + 0x1C8;
// ReverbHiShutdown 0x80388554 (one caller, 0x8043F674) releases it. Every audio frame the SDK's aux processor
// 0x80384E14 calls the registered callback on the room channels the DSP sent one block earlier; the callback
// skips its work while the byte at context + 0x1C4 is set and otherwise runs the effect (the crossfeed 0x80387D24
// and HandleReverb 0x80387EB4 per channel) in place. The DSP returns the result one block later.
//
// Native implementation: the mixer's EffectStage holds the room channels, the effect and its tail; the command list
// schedules each block's send and return; this owner is the callback. The game's own registration, the aux
// processor's dispatch, the bypass byte and the floating-point mode decide when it runs, so no translated effect
// code executes and no guest sample buffer is read or written.
//
// Init and shutdown are owned too (docs/GC_REMOVAL_PLAN.md step 2). The original init clears the bypass byte,
// validates the six parameters with ordered compares (a NaN passes them) and, for accepted ones, allocates 18 delay
// lines and 3 pre-delays (about 85 KB) from the game's sound heap through the AXFX alloc hook, describes them in
// context + 0x000..0x1C3 and returns 1; refused parameters return 0 before any allocation. Shutdown frees them and
// returns 1. With the effect native, nothing reads those lines or descriptors: the guest watch over a full Westopolis
// replay saw no access to them after init except the sound heap walking its own block headers (gate 2). So the
// owners write what the game can observe - the bypass byte and the return value - and allocate nothing: the
// descriptors stay as the game left them (zero) and the sound heap keeps the 85 KB. For finite parameters the return
// value is the original's (1 accepted, 0 refused). A NaN parameter passes the original's ordered compares, and its
// result then depends on a heap request sized from the NaN; the owner refuses it (0, unbound, so the callback stops
// the run instead of playing it). Neither caller can tell: both overwrite r3 at once (0x8043F9E4, 0x8043F6D0).

#include "audio/effect_stage.h"
#include "gekko/types.h"

#include <cstddef>

namespace gekko {
class Cpu;
class GuestMemory;
}

namespace gekko::reverb {

constexpr u32 kInit = 0x803884E4u;      // ReverbHiInit(context): served natively
constexpr u32 kShutdown = 0x80388554u;  // ReverbHiShutdown(context): served natively
constexpr u32 kCallback = 0x803885A0u;  // ReverbHiCallback(buffers, context): served natively
constexpr u32 kBypassOffset = 0x1C4u;   // u8: nonzero = the callback does nothing
constexpr u32 kParametersOffset = 0x1C8u;  // six f32: color, level, time, damping, pre-delay, crossfeed

// The guest entries this owner serves on every call: must equal its rows of extract/native-owners.txt
// (native_file_api_tests).
struct Owners {
    struct Address {
        u32 address;
        const char* name;
    };
    static const Address* addresses(std::size_t& count) noexcept {
        static constexpr Address kAddresses[] = {{kInit, "AXFXReverbHiInit"},
                                                 {kShutdown, "AXFXReverbHiShutdown"},
                                                 {kCallback, "AXFXReverbHiCallback"}};
        count = sizeof(kAddresses) / sizeof(kAddresses[0]);
        return kAddresses;
    }
};

// Which game object the native effect belongs to, and whether its parameters were taken.
struct Binding {
    u32 context = 0;
    bool configured = false;
};

// The six parameters the game wrote for ReverbHiInit; false when the object is not readable.
bool read_parameters(const GuestMemory& memory, u32 context, shadow::audio::Reverb::Parameters& out) noexcept;

// ReverbHiInit (r3 = context): clears the bypass byte, binds the effect to the context with a cleared tail and sets
// r3: 1 when the effect took the parameters, else 0. False when the context's parameters are not readable guest memory (the caller
// stops the run: the original would fault on them).
bool serve_init(Cpu& cpu, Binding& binding, shadow::audio::EffectStage& effect) noexcept;
// ReverbHiShutdown (r3 = context): unbinds a matching context; r3 = 1.
void serve_shutdown(Cpu& cpu, Binding& binding) noexcept;

enum class CallbackResult : u8 {
    Processed,   // the block's room channels went through the effect
    Bypassed,    // context + 0x1C4 set: nothing changes, as in the original
    Idle,        // no finished block is waiting for an effect pass
    Unbound,     // the context is not the configured effect: the caller must stop
};
// The callback (r3 = the SDK's buffer record, r4 = context). Reads the bypass byte and the floating-point mode
// (FPSCR[NI]: the original's non-IEEE mode flushes tiny results); changes no guest state.
CallbackResult serve_callback(const Cpu& cpu, const Binding& binding, shadow::audio::EffectStage& effect) noexcept;

}  // namespace gekko::reverb
