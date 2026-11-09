#pragma once

// The AX mixer library's two trivial entries, owned natively (audio.ax, MIX group; docs/agents/native-boot.md Step 6).
//
// Original behaviour (main.dol, PAD GUPP8P):
//   MIXQuit          0x8038A2BC: `blr`. It does nothing. Called by the sound destructor 0x801F2DA8 (at 0x801F2E14).
//   MIXSetSoundMode  0x8038A2C0: `stw r3, 0x5DC8(r13); blr`. It stores its argument into the mixer's sound-mode word
//                    (sda+0x5DC8 = 0x805F20C8 with r13 = 0x805EC500). The mode (MIXInit sets 1 = stereo) is read by the
//                    mixer library's own bodies (0x8038A138, 0x8038A2C8, 0x8038B1F8: __MIXResetPan compares it with 3 =
//                    DPL2), so the store is state the surviving code reads: the owner performs exactly that store.
//                    Called by 0x801F21AC (three times, 0x801F2210 / 0x801F222C / 0x801F2248).
// No DSP or mixing behaviour is imitated: the mixer's channel table and voice state are owned elsewhere (audio-ax).

#include "gekko/types.h"

#include <cstddef>

namespace gekko {
class Cpu;
}

namespace gekko::mix_entries {

constexpr u32 kMixQuit = 0x8038A2BCu;
constexpr u32 kMixSetSoundMode = 0x8038A2C0u;
constexpr u32 kSdaSoundMode = 0x5DC8u;  // r13-relative: the mixer's sound mode

// The guest entries this owner serves: must equal its rows of extract/native-owners.txt (native_file_api_tests).
struct Owners {
    struct Address {
        u32 address;
        const char* name;
    };
    static const Address* addresses(std::size_t& count) noexcept {
        static constexpr Address kAddresses[] = {{kMixQuit, "MIXQuit"}, {kMixSetSoundMode, "MIXSetSoundMode"}};
        count = sizeof(kAddresses) / sizeof(kAddresses[0]);
        return kAddresses;
    }
};

// MIXSetSoundMode(mode): the store sda+0x5DC8 = r3. Nothing else is written, no register changes.
void serve_set_sound_mode(Cpu& cpu) noexcept;

// Installs both owners on the CPU.
void install(Cpu& cpu);

}  // namespace gekko::mix_entries
