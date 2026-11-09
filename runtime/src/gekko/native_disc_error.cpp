// The game's disc-error screen module with no drive underneath. Contract: include/gekko/native_disc_error.h and
// docs/DISC_SYSTEM_MAP.md ("Cut 1": the disassembly and the A/B these follow).

#include "gekko/native_disc_error.h"

#include <cstdio>
#include <string>

#include "gekko/cpu.h"

namespace gekko {

namespace {

// ---- the entry points the generator routes here (extract/native-owners.txt)
constexpr u32 kLoadHoldQuery = 0x8032D138u;  // the loaders' "does the screen hold me" query
constexpr u32 kDriveCallback = 0x8032D3D4u;  // RtGCNFS's drive-error callback (device +0x5C)

const NativeDiscError::Address kAddresses[] = {
    {kLoadHoldQuery, "the disc-error screen's load hold query", false},
    {kDriveCallback, "the RtGCNFS drive-error callback", false},
};

// RtGCNFS's drive states that open the screen (0x8032D3D4: 2..6; 7 and below 2 return at once).
constexpr u32 kFirstErrorState = 2u;
constexpr u32 kNoErrorState = 7u;

}  // namespace

const NativeDiscError::Address* NativeDiscError::addresses(std::size_t& count) noexcept {
    count = sizeof(kAddresses) / sizeof(kAddresses[0]);
    return kAddresses;
}

void NativeDiscError::install() {
    Cpu& cpu = cpu_;
    const auto own = [&cpu](u32 address, const char* name, auto&& serve) {
        cpu.install_hle(address, [serve](Cpu& c) {
            serve(c);
            return true;  // an owner never declines
        }, std::string("native disc error: ") + name);
    };
    own(kLoadHoldQuery, "load hold query", [](Cpu& c) { c.gpr[3] = 0u; });
    own(kDriveCallback, "drive-error callback", [](Cpu& c) {
        const u32 state = c.gpr[3];
        if (state >= kFirstErrorState && state < kNoErrorState) {
            char detail[192];
            std::snprintf(detail, sizeof(detail),
                          "native disc error: RtGCNFS reported drive state %u (LR 0x%08X), but the native file API "
                          "has no drive and reports none",
                          state, c.lr);
            c.request_stop(StopReason::ExecutionFault, detail);
            return;
        }
        c.gpr[3] = 1u;
    });
}

}  // namespace gekko
