// The native owners of the game's ReverbStd, Chorus and Delay effect set-up (gekko/native_axfx_effects.h).

#include "gekko/native_axfx_effects.h"

#include "gekko/cpu.h"
#include "gekko/guest_memory.h"

#include <cstdio>
#include <cstring>

namespace gekko::axfx_effects {

namespace {

float float_at(const u8* bytes) noexcept {
    const u32 bits = load_be32(bytes);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// A compare chain of the originals: fcmpu then blt / bgt. A NaN is unordered, so neither branch is taken and it passes.
bool out_of_range(float value, float low, float high) noexcept { return value < low || value > high; }

const u8* parameters(const Cpu& cpu, u32 context, u32 offset, u32 bytes) noexcept {
    const u32 at = context + offset;
    if (at < context) return nullptr;
    return cpu.guest_memory().peek(GcAddr(at), bytes);
}

bool in_unsigned_range(u32 value, u32 low, u32 high) noexcept { return value >= low && value <= high; }

}  // namespace

Verdict judge_std(const Cpu& cpu, u32 context) noexcept {
    const u8* const p = parameters(cpu, context, kStdParametersOffset, 5u * sizeof(u32));
    if (p == nullptr) return Verdict::Unreadable;
    const float coloration = float_at(p), mix = float_at(p + 4), time = float_at(p + 8), damping = float_at(p + 12),
                pre_delay = float_at(p + 16);
    // 0x803886A4: f1 = +0x140, f2 = +0x148, f3 = +0x144, f4 = +0x14C, f5 = +0x150; 0.0, 1.0, 0.01, 10.0, 0.1 from r2.
    if (out_of_range(coloration, 0.0f, 1.0f) || out_of_range(time, 0.01f, 10.0f) || out_of_range(mix, 0.0f, 1.0f) ||
        out_of_range(damping, 0.0f, 1.0f) || out_of_range(pre_delay, 0.0f, 0.1f))
        return Verdict::Refused;
    return Verdict::Accepted;
}

Verdict judge_chorus(const Cpu& cpu, u32 context) noexcept {
    const u8* const p = parameters(cpu, context, kChorusParametersOffset, 3u * sizeof(u32));
    if (p == nullptr) return Verdict::Unreadable;
    const u32 base_delay = load_be32(p), variation = load_be32(p + 4), period = load_be32(p + 8);
    // 0x80389464: cmplwi against 5..0xF, <= 5, 0x1F4..0x2710 (all unsigned).
    if (!in_unsigned_range(base_delay, 5u, 15u) || variation > 5u || !in_unsigned_range(period, 500u, 10000u))
        return Verdict::Refused;
    return Verdict::Accepted;
}

Verdict judge_delay(const Cpu& cpu, u32 context) noexcept {
    const u8* const p = parameters(cpu, context, kDelayParametersOffset, 9u * sizeof(u32));
    if (p == nullptr) return Verdict::Unreadable;
    // 0x80389D2C: three delays 10..5000 ms, then three feedback and three output percentages 0..100.
    for (unsigned i = 0; i < 3u; ++i)
        if (!in_unsigned_range(load_be32(p + i * 4u), 10u, 5000u)) return Verdict::Refused;
    for (unsigned i = 3; i < 9u; ++i)
        if (load_be32(p + i * 4u) > 100u) return Verdict::Refused;
    return Verdict::Accepted;
}

namespace {
bool serve(Cpu& cpu, Verdict verdict) noexcept {
    if (verdict == Verdict::Unreadable) return false;
    cpu.gpr[3] = verdict == Verdict::Accepted ? 1u : 0u;
    return true;
}
}  // namespace

bool serve_std_init(Cpu& cpu) noexcept { return serve(cpu, judge_std(cpu, cpu.gpr[3])); }
bool serve_chorus_init(Cpu& cpu) noexcept { return serve(cpu, judge_chorus(cpu, cpu.gpr[3])); }
bool serve_delay_init(Cpu& cpu) noexcept { return serve(cpu, judge_delay(cpu, cpu.gpr[3])); }

void serve_shutdown(Cpu& cpu) noexcept { cpu.gpr[3] = 1u; }

void install(Cpu& cpu) {
    struct Entry {
        u32 address;
        bool (*init)(Cpu&) noexcept;
        const char* name;
    };
    static constexpr Entry kInits[] = {{kStdInit, serve_std_init, "ReverbStd: AXFXReverbStdInit (native)"},
                                       {kChorusInit, serve_chorus_init, "Chorus: AXFXChorusInit (native)"},
                                       {kDelayInit, serve_delay_init, "Delay: AXFXDelayInit (native)"}};
    for (const Entry& entry : kInits) {
        const u32 address = entry.address;
        const auto init = entry.init;
        cpu.install_hle(address, [address, init](Cpu& c) {
            if (!init(c)) {
                char detail[96];
                std::snprintf(detail, sizeof(detail), "AXFX init 0x%08X with an unreadable context 0x%08X", address,
                              c.gpr[3]);
                c.request_stop(StopReason::ExecutionFault, detail);
            }
            return true;  // an owner never declines
        }, entry.name);
    }
    static constexpr struct { u32 address; const char* name; } kShutdowns[] = {
        {kStdShutdown, "ReverbStd: AXFXReverbStdShutdown (native)"},
        {kChorusShutdown, "Chorus: AXFXChorusShutdown (native)"},
        {kDelayShutdown, "Delay: AXFXDelayShutdown (native)"}};
    for (const auto& entry : kShutdowns)
        cpu.install_hle(entry.address, [](Cpu& c) {
            serve_shutdown(c);
            return true;
        }, entry.name);
}

}  // namespace gekko::axfx_effects
