// The native owner of the game's room reverb (gekko/native_reverb_owner.h).

#include "gekko/native_reverb_owner.h"

#include "gekko/cpu.h"
#include "gekko/guest_memory.h"

#include <cstring>

namespace gekko::reverb {

namespace {
constexpr u32 kFpscrNonIeee = 0x4u;  // FPSCR[NI]
}

bool read_parameters(const GuestMemory& memory, u32 context, shadow::audio::Reverb::Parameters& out) noexcept {
    const u32 at = context + kParametersOffset;
    if (at < context) return false;
    const u8* const bytes = memory.peek(GcAddr(at), 6u * sizeof(u32));
    if (bytes == nullptr) return false;
    float values[6];
    for (u32 i = 0; i < 6u; ++i) {
        const u32 bits = load_be32(bytes + i * 4u);
        std::memcpy(&values[i], &bits, sizeof(float));
    }
    out = {values[0], values[1], values[2], values[3], values[4], values[5]};
    return true;
}

void observe_init(const Cpu& cpu, Binding& binding, shadow::audio::EffectStage& effect) noexcept {
    shadow::audio::Reverb::Parameters parameters;
    binding.context = cpu.gpr[3];
    binding.configured = read_parameters(cpu.guest_memory(), binding.context, parameters) &&
                         effect.configure(parameters);
}

void observe_shutdown(const Cpu& cpu, Binding& binding) noexcept {
    if (cpu.gpr[3] == binding.context) binding.configured = false;
}

CallbackResult serve_callback(const Cpu& cpu, const Binding& binding, shadow::audio::EffectStage& effect) noexcept {
    const u32 context = cpu.gpr[4];
    if (!binding.configured || context != binding.context) return CallbackResult::Unbound;
    const u32 bypass_at = context + kBypassOffset;
    const u8* const bypass = cpu.guest_memory().peek(GcAddr(bypass_at), 1u);
    if (bypass == nullptr) return CallbackResult::Unbound;
    if (*bypass != 0u) return CallbackResult::Bypassed;
    const auto mode = (cpu.fpscr & kFpscrNonIeee) != 0u ? shadow::math::SubnormalMode::Flush
                                                        : shadow::math::SubnormalMode::Preserve;
    return effect.process(mode) ? CallbackResult::Processed : CallbackResult::Idle;
}

}  // namespace gekko::reverb
