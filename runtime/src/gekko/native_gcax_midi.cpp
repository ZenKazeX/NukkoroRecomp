// GCAX's MIDI input, owned natively (gekko/native_gcax_midi.h).

#include "gekko/native_gcax_midi.h"

#include "gekko/audio_audit.h"
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/ppc_ops.h"

#include <cstring>

namespace gekko::gcax_midi {

namespace {

constexpr u32 kSurroundMode = 2u;

// The original's int -> single conversion: (double)(0x43300000:x^0x80000000) - 2^52 - 2^31, then fsubs.
u64 single_from_int(Cpu& cpu, s32 value) noexcept {
    const u64 biased = (u64{0x43300000u} << 32) | (static_cast<u32>(value) ^ 0x80000000u);
    return ppc::fps_sub(cpu, biased, 0x4330000080000000ull);
}

// lfs: a single constant widened exactly.
u64 single_constant(u32 bits) noexcept {
    float value;
    static_assert(sizeof(value) == sizeof(bits), "f32");
    std::memcpy(&value, &bits, sizeof(value));
    return double_to_bits(static_cast<double>(value));
}

s32 truncate_to_s32(Cpu& cpu, u64 bits) noexcept {
    return static_cast<s32>(static_cast<u32>(ppc::fctiw(cpu, bits, true)));
}

// Pan and CC63's mapping: trunc(fmadds(0.5f, value, 64.0f)) as a signed byte, negative -> 0.
s32 pan_value(Cpu& cpu, s32 value) noexcept {
    const u64 x = ppc::fps_madd(cpu, single_constant(0x3F000000u), single_from_int(cpu, static_cast<s8>(value)),
                           single_constant(0x42800000u));
    s32 v = static_cast<s8>(truncate_to_s32(cpu, x));
    if (v > 0x7F) v = 0x7F;
    else if (static_cast<s16>(v) < 0) v = 0;
    return v;
}

}  // namespace

bool push(GuestMemory& memory, u32 port, u32 message, Report& report) noexcept {
    const s32 count = static_cast<s32>(memory.peek32(port + kCount));
    if (count >= static_cast<s32>(kRingWords)) {
        ++report.dropped;
        audio_audit::event(audio_audit::Lane::Guest, "midi-drop", port, message, static_cast<u32>(count));
        return false;
    }
    const u32 write = memory.peek32(port + kWrite);
    memory.write32(port + (write << 2) + kRing, message);
    memory.write32(port + kCount, static_cast<u32>(count + 1));
    // The original's signed remainder: (write + 1) % 0x400.
    const s32 next = static_cast<s32>(write + 1u) % static_cast<s32>(kRingWords);
    memory.write32(port + kWrite, static_cast<u32>(next));
    ++report.pushed;
    audio_audit::event(audio_audit::Lane::Guest, "midi-push", port, message, static_cast<u32>(count));
    return true;
}

bool send(GuestMemory& memory, u32 handle, u32 status, u32 d1, u32 d2, u32 priority, Report& report) noexcept {
    const u32 context = memory.peek32(kContextPointer);
    bool open = false;
    if (context != 0u && handle != 0u) {
        for (u32 i = 0; i < kPortCount; ++i) {
            if (handle == context + kPorts + i * kPortBytes) {
                open = memory.peek32(handle) != 0u;
                break;
            }
        }
    }
    if (!open) {
        ++report.refused;
        return false;
    }
    status &= 0xFFu;
    const u32 kind = status & 0xF0u;
    const bool three_bytes = kind == 0x80u || kind == 0x90u || kind == 0xA0u || kind == 0xB0u || kind == 0xD0u ||
                             kind == 0xE0u;
    const u32 message = (status << 24) | ((d1 & 0xFFu) << 16) | ((three_bytes ? d2 & 0xFFu : 0u) << 8) |
                        (priority & 0xFFu);
    return push(memory, handle, message, report);
}

u32 SoundObject::handle(s32 port) const noexcept {
    return cpu.guest_memory().peek32(object + 8u + static_cast<u32>(static_cast<s8>(port)) * 4u);
}

void SoundObject::set_bank_program(s32 port, u32 channel, s32 bank, s32 program) noexcept {
    // No port check in the original: ports 0..2 only reach here.
    const u32 h = handle(port);
    const u32 ch = static_cast<u32>(static_cast<s8>(channel));
    send(cpu.guest_memory(), h, ch | 0xB0u, 0x20u, static_cast<u32>(static_cast<s8>(bank)), 0u, report);
    send(cpu.guest_memory(), h, ch | 0xC0u, static_cast<u32>(static_cast<s8>(program)), 0u, 0u, report);
}

void SoundObject::note_on(u32 priority, s32 port, u32 channel) noexcept {
    const u32 p = priority & 0xFFu;
    const u32 order = p == 3u ? 15u : (p == 2u ? 2u : 1u);
    send(cpu.guest_memory(), handle(port), channel | 0x90u, 0x30u, 0x7Fu, order, report);
}

void SoundObject::note_off(s32 port, u32 channel) noexcept {
    send(cpu.guest_memory(), handle(port), channel | 0x80u, 0x30u, 0u, 0u, report);
}

void SoundObject::volume(s32 attenuation, s32 port, u32 channel) noexcept {
    s32 v = static_cast<s8>(attenuation);
    if (v > 0) {
        const u64 x = ppc::fps_mul(cpu, single_constant(0x3E59B3D0u), single_from_int(cpu, v));  // 0.2126f * v
        v = static_cast<s8>(truncate_to_s32(cpu, x));
    }
    v += 100;
    if (static_cast<s16>(v) < 1) v = 1;
    if (static_cast<s16>(v) > 0x7F) v = 0x7F;
    send(cpu.guest_memory(), handle(port), channel | 0xB0u, 7u, static_cast<u32>(static_cast<s8>(v)), 0u, report);
}

void SoundObject::pan(s32 value, s32 port, u32 channel) noexcept {
    send(cpu.guest_memory(), handle(port), channel | 0xB0u, 0x0Au, static_cast<u32>(pan_value(cpu, value)), 0u,
         report);
}

void SoundObject::surround(s32 value, s32 port, u32 channel) noexcept {
    send(cpu.guest_memory(), handle(port), channel | 0xB0u, 0x3Eu, static_cast<u32>(static_cast<s8>(value)), 0u,
         report);
}

void SoundObject::bend(s32 value, s32 port, u32 channel) noexcept {
    s32 b = value;
    if (static_cast<s16>(b) > 0x3FFF) b = 0x3FFF;
    else if (static_cast<s16>(b) < -0x4000) b = -0x4000;
    const s32 v = static_cast<s16>(b + 0x4000);
    send(cpu.guest_memory(), handle(port), channel | 0xE0u, static_cast<u32>(v) & 0x7Fu,
         static_cast<u32>(v >> 8), 0u, report);
}

void SoundObject::reset_pan(s32 port, u32 channel) noexcept {
    if (static_cast<s32>(cpu.guest_memory().peek32(kOutputConfig + 0x78u)) == static_cast<s32>(kSurroundMode))
        surround(0, port, channel);
    pan(0, port, channel);
}

void serve_push(Cpu& cpu, Report& report) noexcept {
    push(cpu.guest_memory(), cpu.gpr[3], cpu.gpr[4], report);
}

}  // namespace gekko::gcax_midi
