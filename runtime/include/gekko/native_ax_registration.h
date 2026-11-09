#pragma once

// The AX library's registration entries, owned natively (docs/agents/audio-ax.md, step 1).
//
// Original (main.dol, PAL GUPP8P; names: test_data/platform/unidentified_names.csv):
//   AXRegisterCallback     0x80385F60  (callback) -> old: swaps the frame callback sda+0x5D58 with interrupts off;
//                                      its only caller is the sound ctor 0x801F2EC0, which passes the GCAX frame callback.
//   AXRegisterAuxACallback 0x80385050  (callback, context): sda+0x5CF0 / +0x5CF8; caller 0x8043F3C4.
//   AXRegisterAuxBCallback 0x8038505C  (callback, context): sda+0x5CF4 / +0x5CFC; caller 0x8043F3D0.
//   AXSetMode              0x80385784  (mode): sda+0x5D38 when it changes; callers 0x8043E680, 0x8043F4AC, 0x8043F4B8.
// The words are where the SDK keeps them because the guest frame engine (__AXOutNewFrame, the aux processor
// 0x80384E14, __AXNextFrame) still reads them; they go away with it (audio-ax step 3). The interrupt masking of
// AXRegisterCallback only protects the swap from the frame interrupt: the owner runs between guest instructions, so
// the swap is atomic and the MSR is untouched (the original restores it, so the guest sees no difference).

#include <cstddef>

#include "gekko/cpu.h"
#include "gekko/types.h"

namespace gekko::ax_registration {

constexpr u32 kRegisterCallback = 0x80385F60u;
constexpr u32 kRegisterAuxA = 0x80385050u;
constexpr u32 kRegisterAuxB = 0x8038505Cu;
constexpr u32 kSetMode = 0x80385784u;

constexpr u32 kSdaFrameCallback = 0x5D58u;
constexpr u32 kSdaAuxACallback = 0x5CF0u, kSdaAuxAContext = 0x5CF8u;
constexpr u32 kSdaAuxBCallback = 0x5CF4u, kSdaAuxBContext = 0x5CFCu;
constexpr u32 kSdaMode = 0x5D38u;

// The guest entries served on every call: must equal their rows of extract/native-owners.txt (native_file_api_tests).
struct Owners {
    struct Address {
        u32 address;
        const char* name;
        bool continuation;
    };
    static const Address* addresses(std::size_t& count) noexcept {
        static constexpr Address kAddresses[] = {{kRegisterCallback, "AXRegisterCallback", false},
                                                 {kRegisterAuxA, "AXRegisterAuxACallback", false},
                                                 {kRegisterAuxB, "AXRegisterAuxBCallback", false},
                                                 {kSetMode, "AXSetMode", false}};
        count = sizeof(kAddresses) / sizeof(kAddresses[0]);
        return kAddresses;
    }
};

inline void install(Cpu& cpu) {
    const auto own = [&cpu](u32 address, const char* name, auto&& serve) {
        cpu.install_hle(address, [serve](Cpu& c) {
            serve(c);
            return true;  // an owner never declines
        }, std::string("AX registration: ") + name);
    };
    own(kRegisterCallback, "AXRegisterCallback", [](Cpu& c) {
        const u32 sda = c.gpr[13];
        const u32 old = c.mem_read32(sda + kSdaFrameCallback);
        c.mem_write32(sda + kSdaFrameCallback, c.gpr[3]);
        c.gpr[3] = old;
    });
    own(kRegisterAuxA, "AXRegisterAuxACallback", [](Cpu& c) {
        const u32 sda = c.gpr[13];
        c.mem_write32(sda + kSdaAuxACallback, c.gpr[3]);
        c.mem_write32(sda + kSdaAuxAContext, c.gpr[4]);
    });
    own(kRegisterAuxB, "AXRegisterAuxBCallback", [](Cpu& c) {
        const u32 sda = c.gpr[13];
        c.mem_write32(sda + kSdaAuxBCallback, c.gpr[3]);
        c.mem_write32(sda + kSdaAuxBContext, c.gpr[4]);
    });
    own(kSetMode, "AXSetMode", [](Cpu& c) {
        const u32 sda = c.gpr[13];
        if (c.mem_read32(sda + kSdaMode) != c.gpr[3]) c.mem_write32(sda + kSdaMode, c.gpr[3]);
    });
}

}  // namespace gekko::ax_registration
