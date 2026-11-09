// The AX mixer library's MIXQuit and MIXSetSoundMode, native. Contract: include/gekko/native_mix_entries.h.

#include "gekko/native_mix_entries.h"

#include "gekko/cpu.h"

namespace gekko::mix_entries {

void serve_set_sound_mode(Cpu& cpu) noexcept {
    // 0x8038A2C0: stw r3, 0x5DC8(r13)
    cpu.mem_write32(cpu.gpr[13] + kSdaSoundMode, cpu.gpr[3]);
}

void install(Cpu& cpu) {
    cpu.install_hle(kMixQuit, [](Cpu&) {
        return true;  // the original body is a bare blr
    }, "native mixer: MIXQuit");
    cpu.install_hle(kMixSetSoundMode, [](Cpu& c) {
        serve_set_sound_mode(c);
        return true;
    }, "native mixer: MIXSetSoundMode");
}

}  // namespace gekko::mix_entries
