// The native owners of the OS's console-settings accessors (gekko/native_os_settings.h).

#include "gekko/native_os_settings.h"

#include "gekko/cpu.h"

#include <cstdio>

namespace gekko::os_settings {

namespace {

constexpr u32 kMsrEE = 0x8000u;

// The SRAM lock without the mask/restore pair: Scb.enabled = the interrupt level, Scb.locked = 1.
bool lock(Cpu& cpu) {
    if (cpu.mem_read32(kScb + kScbLocked) != 0u) return false;
    cpu.mem_write32(kScb + kScbEnabled, (cpu.msr() & kMsrEE) != 0u ? 1u : 0u);
    cpu.mem_write32(kScb + kScbLocked, 1u);
    return true;
}

}  // namespace

bool serve_get_sound_mode(Cpu& cpu, const Unlock& unlock) {
    if (!lock(cpu)) return false;
    const u32 mode = (cpu.mem_read8(kScb + kSramFlags) & 0x04u) != 0u ? 1u : 0u;
    unlock(cpu, false, 0u);
    cpu.gpr[3] = mode;
    return true;
}

bool serve_get_language(Cpu& cpu, const Unlock& unlock) {
    if (!lock(cpu)) return false;
    const u32 language = cpu.mem_read8(kScb + kSramLanguage);
    unlock(cpu, false, 0u);
    cpu.gpr[3] = language;
    return true;
}

bool serve_get_progressive_mode(Cpu& cpu, const Unlock& unlock) {
    if (!lock(cpu)) return false;
    const u32 mode = (cpu.mem_read8(kScb + kSramProgressive) & 0x40u) != 0u ? 1u : 0u;
    unlock(cpu, false, 0u);
    cpu.gpr[3] = mode;
    return true;
}

bool serve_set_sound_mode(Cpu& cpu, const Unlock& unlock) {
    const u32 wanted = (cpu.gpr[3] << 2) & 0x04u;
    if (!lock(cpu)) return false;
    const u8 flags = cpu.mem_read8(kScb + kSramFlags);
    if (wanted == (flags & 0x04u)) {
        cpu.gpr[3] = unlock(cpu, false, 0u);
    } else {
        cpu.mem_write8(kScb + kSramFlags, static_cast<u8>((flags & ~0x04u) | wanted));
        cpu.gpr[3] = unlock(cpu, true, 0u);
    }
    return true;
}

bool serve_set_progressive_mode(Cpu& cpu, const Unlock& unlock) {
    const u32 wanted = (cpu.gpr[3] << 6) & 0x40u;
    if (!lock(cpu)) return false;
    const u8 value = cpu.mem_read8(kScb + kSramProgressive);
    if (wanted == (value & 0x40u)) {
        cpu.gpr[3] = unlock(cpu, false, 0u);
    } else {
        cpu.mem_write8(kScb + kSramProgressive, static_cast<u8>((value & ~0x40u) | wanted));
        cpu.gpr[3] = unlock(cpu, true, 0u);
    }
    return true;
}

bool install(Cpu& cpu) {
    Cpu::HleHandler unlock_handler;
    for (const auto& entry : cpu.hle_entries())
        if (entry.address == kUnlockSram) unlock_handler = entry.handler;
    if (!unlock_handler) return false;
    // UnlockSram's owner reads (commit, offset) from r3 / r4 and answers in r3; keep the caller's registers.
    const Unlock unlock = [unlock_handler](Cpu& c, bool commit, u32 offset) {
        const u32 r3 = c.gpr[3], r4 = c.gpr[4];
        c.gpr[3] = commit ? 1u : 0u;
        c.gpr[4] = offset;
        unlock_handler(c);
        const u32 result = c.gpr[3];
        c.gpr[3] = r3;
        c.gpr[4] = r4;
        return result;
    };
    const auto bind = [&](u32 address, bool (*serve)(Cpu&, const Unlock&), const char* what) {
        cpu.install_hle(address, [unlock, serve, what](Cpu& c) {
            if (!serve(c, unlock)) {
                char detail[96];
                std::snprintf(detail, sizeof(detail), "%s while the SRAM lock is held", what);
                c.request_stop(StopReason::ExecutionFault, detail);
            }
            return true;
        }, std::string(what) + " (native)");
    };
    bind(kGetSoundMode, serve_get_sound_mode, "OSGetSoundMode");
    bind(kSetSoundMode, serve_set_sound_mode, "OSSetSoundMode");
    bind(kGetLanguage, serve_get_language, "OSGetLanguage");
    bind(kGetProgressiveMode, serve_get_progressive_mode, "OSGetProgressiveMode");
    bind(kSetProgressiveMode, serve_set_progressive_mode, "OSSetProgressiveMode");
    return true;
}

}  // namespace gekko::os_settings
