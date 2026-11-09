#pragma once

// The OS's console-settings accessors, owned natively (docs/agents/os-other.md; removal plan service os.other).
//
// Original behaviour (main.dol, PAL GUPP8P): OSGetSoundMode 0x80377DA0, OSSetSoundMode 0x80377E20, OSGetLanguage
// 0x80377EC4, OSGetProgressiveMode 0x80377F30 and OSSetProgressiveMode 0x80377FA0 each take the SRAM lock of the OS
// control block (Scb 0x80586FE0: OSDisableInterrupts, Scb.enabled = the old level, Scb.locked = 1; a lock that is
// already held sends the original through a null base register), read or change one field of the cached SRAM image
// (Scb + 0x11 bit 0x40 progressive mode, + 0x12 language, + 0x13 bit 0x04 sound mode) and release it through UnlockSram
// 0x803778E8: the getters with commit = 0, the setters with commit = 0 when the value is unchanged and otherwise with
// commit = 1, offset 0 after storing the field. The SRAM chip itself is the NativeIpl settings image (native_ipl.h);
// the field values come from the PC settings applied over it at boot, so the getters answer the user's choice.
//
// Native implementation: the same field reads and writes, Scb.enabled (the interrupt level the lock saved) and the
// same UnlockSram call (the NativeIpl owner, found among the installed entries), without the interrupt mask/restore
// pair, which leaves MSR[EE] exactly as it was. A held lock is a reentrancy the original turns into a read through a
// null pointer; the owner stops the run instead. Results: a getter returns the field (sound mode and progressive
// mode 0 / 1, language the raw SRAM byte); a setter returns what UnlockSram returned (Scb.sync).

#include "gekko/types.h"

#include <cstddef>
#include <functional>

namespace gekko {
class Cpu;
}

namespace gekko::os_settings {

constexpr u32 kGetSoundMode = 0x80377DA0u;
constexpr u32 kSetSoundMode = 0x80377E20u;
constexpr u32 kGetLanguage = 0x80377EC4u;
constexpr u32 kGetProgressiveMode = 0x80377F30u;
constexpr u32 kSetProgressiveMode = 0x80377FA0u;
constexpr u32 kUnlockSram = 0x803778E8u;  // served by NativeIpl; the setters and getters release through it

constexpr u32 kScb = 0x80586FE0u;
constexpr u32 kScbEnabled = 0x44u;
constexpr u32 kScbLocked = 0x48u;
constexpr u32 kSramProgressive = 0x11u;  // bit 0x40
constexpr u32 kSramLanguage = 0x12u;
constexpr u32 kSramFlags = 0x13u;        // bit 0x04: sound mode

// UnlockSram(commit, offset) -> Scb.sync.
using Unlock = std::function<u32(Cpu&, bool commit, u32 offset)>;

// Must equal this owner's rows of extract/native-owners.txt (native_file_api_tests).
struct Owners {
    struct Address {
        u32 address;
        const char* name;
    };
    static const Address* addresses(std::size_t& count) noexcept {
        static constexpr Address kAddresses[] = {{kGetSoundMode, "OSGetSoundMode"},
                                                 {kSetSoundMode, "OSSetSoundMode"},
                                                 {kGetLanguage, "OSGetLanguage"},
                                                 {kGetProgressiveMode, "OSGetProgressiveMode"},
                                                 {kSetProgressiveMode, "OSSetProgressiveMode"}};
        count = sizeof(kAddresses) / sizeof(kAddresses[0]);
        return kAddresses;
    }
};

// Each serves one call. False when the SRAM lock is already held (the call is refused; r3 untouched).
bool serve_get_sound_mode(Cpu& cpu, const Unlock& unlock);
bool serve_set_sound_mode(Cpu& cpu, const Unlock& unlock);
bool serve_get_language(Cpu& cpu, const Unlock& unlock);
bool serve_get_progressive_mode(Cpu& cpu, const Unlock& unlock);
bool serve_set_progressive_mode(Cpu& cpu, const Unlock& unlock);

// Installs the five owners. The UnlockSram entry must already be installed (NativeIpl); returns false if it is not.
bool install(Cpu& cpu);

}  // namespace gekko::os_settings
