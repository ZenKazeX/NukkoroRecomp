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
// code executes and no guest sample buffer is read or written. Init and shutdown still run as translated SDK code
// (SCAFFOLDING: their delay-line allocations are no longer used; deleted when the sound system's effect setup is
// lifted); this owner only takes the parameters and the context from them.

#include "audio/effect_stage.h"
#include "gekko/types.h"

#include <cstddef>

namespace gekko {
class Cpu;
class GuestMemory;
}

namespace gekko::reverb {

constexpr u32 kInit = 0x803884E4u;      // ReverbHiInit(context): observed, then runs
constexpr u32 kShutdown = 0x80388554u;  // ReverbHiShutdown(context): observed, then runs
constexpr u32 kCallback = 0x803885A0u;  // ReverbHiCallback(buffers, context): served natively
constexpr u32 kBypassOffset = 0x1C4u;   // u8: nonzero = the callback does nothing
constexpr u32 kParametersOffset = 0x1C8u;  // six f32: color, level, time, damping, pre-delay, crossfeed

// The guest entries this owner serves on every call: must equal its rows of extract/native-owners.txt
// (native_file_api_tests). Init and shutdown are only observed, so they are not listed.
struct Owners {
    struct Address {
        u32 address;
        const char* name;
    };
    static const Address* addresses(std::size_t& count) noexcept {
        static constexpr Address kAddresses[] = {{kCallback, "AXFXReverbHiCallback"}};
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

// ReverbHiInit's entry (r3 = context): the effect takes the parameters, with a cleared tail. Parameters the
// original refuses leave the binding unconfigured.
void observe_init(const Cpu& cpu, Binding& binding, shadow::audio::EffectStage& effect) noexcept;
// ReverbHiShutdown's entry (r3 = context).
void observe_shutdown(const Cpu& cpu, Binding& binding) noexcept;

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
