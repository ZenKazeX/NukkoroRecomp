// The console's reset and boot controls, native. Contract and rationale:
// include/gekko/native_reset.h and docs/native-time-20261005/STAGE6_PI_MI.md
// (sections 1-2: the disassembly and the measurements these follow).

#include "gekko/native_reset.h"

#include <cstdio>
#include <string>

#include "gekko/cpu.h"

namespace gekko {

namespace {

// ---- the entry points the generator routes here (extract/native-owners.txt)
constexpr u32 kMemOnReset = 0x803769A0u;            // the memory interface's OnReset (OS reset queue record)
constexpr u32 kResetSystem = 0x80376FB8u;           // OSResetSystem(reset, resetCode, forceMenu)
constexpr u32 kGetResetCode = 0x803771B8u;          // OSGetResetCode()
constexpr u32 kGetResetButtonState = 0x803772E4u;   // OSGetResetButtonState()

const NativeReset::Address kAddresses[] = {
    {kMemOnReset, "the memory interface's OnReset", false},
    {kResetSystem, "OSResetSystem", false},
    {kGetResetCode, "OSGetResetCode", false},
    {kGetResetButtonState, "OSGetResetButtonState", false},
};

// The boot information's GameChoice byte (low memory 0x800030E3): its low five
// bits ask OSGetResetButtonState to fake a press after that many minutes (a
// kiosk / demo-disc feature). The native bootstrap writes 0.
constexpr u32 kBootGameChoice = 0x800030E3u;

void stop(Cpu& c, const std::string& why) {
    c.request_stop(StopReason::ExecutionFault, "native reset: " + why);
}

const char* reset_kind(u32 reset) noexcept {
    switch (reset) {
        case 0u: return "restart";
        case 1u: return "hot reset";
        case 2u: return "shut down";
        default: return "reset (unknown kind)";
    }
}

}  // namespace

const NativeReset::Address* NativeReset::addresses(std::size_t& count) noexcept {
    count = sizeof(kAddresses) / sizeof(kAddresses[0]);
    return kAddresses;
}

void NativeReset::install() {
    Cpu& cpu = cpu_;
    const auto own = [&cpu](u32 address, const char* name, auto&& serve) {
        cpu.install_hle(address, [serve](Cpu& c) {
            serve(c);
            return true;  // an owner never declines
        }, std::string("native reset: ") + name);
    };
    own(kResetSystem, "OSResetSystem", [](Cpu& c) {
        char detail[256];
        std::snprintf(detail, sizeof(detail),
                      "the game asked the console to %s (OSResetSystem(%u, 0x%08X, %u) from LR 0x%08X): the PC "
                      "has no console to reset, the program ends",
                      reset_kind(c.gpr[3]), c.gpr[3], c.gpr[4], c.gpr[5], c.lr);
        c.request_stop(StopReason::HleExit, detail);
    });
    own(kGetResetCode, "OSGetResetCode", [this](Cpu& c) {
        ++stats_.reset_code_reads;
        c.gpr[3] = 0u;  // a cold boot: no reset code, not a restart
    });
    own(kGetResetButtonState, "OSGetResetButtonState", [this](Cpu& c) {
        ++stats_.reset_button_polls;
        const u32 game_choice = c.mem_read8(kBootGameChoice) & 0x1Fu;
        if (game_choice != 0u) {
            stop(c, "OSGetResetButtonState: the boot information asks for the kiosk auto-reset after " +
                        std::to_string(game_choice) + " minutes (0x800030E3), which this program does not provide");
            return;
        }
        c.gpr[3] = 0u;  // the PC has no reset button: never pressed
    });
    own(kMemOnReset, "the memory interface's OnReset", [](Cpu& c) {
        stop(c, "the memory interface's OnReset ran: it is a record of the OS reset queue, which only "
                "OSResetSystem walks, and OSResetSystem ends the program");
    });
}

}  // namespace gekko
