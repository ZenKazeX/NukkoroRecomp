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

bool serve_init(Cpu& cpu, Binding& binding, shadow::audio::EffectStage& effect) noexcept {
    const u32 context = cpu.gpr[3];
    shadow::audio::Reverb::Parameters parameters;
    if (context + kBypassOffset < context || cpu.guest_memory().peek(GcAddr(context + kBypassOffset), 1u) == nullptr ||
        !read_parameters(cpu.guest_memory(), context, parameters))
        return false;
    cpu.mem_write8(context + kBypassOffset, 0u);  // stb r0, 0x1C4(r31), before the parameters are judged
    binding.context = context;
    binding.configured = effect.configure(parameters);
    cpu.gpr[3] = binding.configured ? 1u : 0u;
    return true;
}

void serve_shutdown(Cpu& cpu, Binding& binding) noexcept {
    if (cpu.gpr[3] == binding.context) binding.configured = false;
    cpu.gpr[3] = 1u;
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
