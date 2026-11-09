#pragma once

// Two small SDK routines, owned natively (docs/agents/os-other.md, step 5; removal plan service os.other).
//
//   OSGetConsoleType 0x80370E04 (no arguments): r3 = the boot-info pointer at r13+0x5A20; when it is non-null r3 =
//       the word at +0x2C (the console type the loader left); when either is zero r3 = 0x10000002 (retail hardware).
//       Only r3 and CR0 change. Its callers are the debug console routines (0x803AF5D4, 0x803B4130, 0x803B57FC).
//   0x80378320 (a data-word callback at 0x805EEC10, in the DVD file system's static data): a lone `blr`. It reads and
//       writes nothing, so the owner returns at once.
//
// Not kept: CR0 (scratch in both).

#include "gekko/types.h"

#include <cstddef>

namespace gekko {
class Cpu;
}

namespace gekko::os_report {
class DebugConsole;
}

namespace gekko::os_misc {

constexpr u32 kGetConsoleType = 0x80370E04u;
constexpr u32 kEmptyCallback = 0x80378320u;
// audio.dsp (called by the unidentified audio code 0x80385A9C): `lwz r3, 0x5DD0(r13); blr`.
constexpr u32 kLoadCell5DD0 = 0x8038C384u;
constexpr u32 kCell5DD0 = 0x5DD0u;
// 0x8037177C: OSReport("%s\n", r3) (the format is the r13+0x26DC string "%s\n"); called by the 0x80384680 init path.
constexpr u32 kReportString = 0x8037177Cu;
constexpr u32 kReportStringFormat = 0x26DCu;
constexpr u32 kBootInfoCell = 0x5A20u;   // r13-relative: the pointer to the loader's boot-info block
constexpr u32 kConsoleTypeOffset = 0x2Cu;
constexpr u32 kRetailConsole = 0x10000002u;

// Leaves the original's result in r3.
void serve_get_console_type(Cpu& cpu);
void serve_load_cell_5dd0(Cpu& cpu);
// The text goes to the debug console exactly as the native OSReport sends it (a retail console prints nothing).
void serve_report_string(Cpu& cpu, os_report::DebugConsole& console);

// Must equal this owner's rows of extract/native-owners.txt (native_file_api_tests).
struct Owners {
    struct Address {
        u32 address;
        const char* name;
    };
    static const Address* addresses(std::size_t& count) noexcept {
        static constexpr Address kAddresses[] = {{kGetConsoleType, "OSGetConsoleType"},
                                                 {kEmptyCallback, "empty callback (DVD data word)"},
                                                 {kLoadCell5DD0, "r13+0x5DD0 getter"},
                                                 {kReportString, "OSReport(\"%s\n\", r3)"}};
        count = 4;
        return kAddresses;
    }
};

void install(Cpu& cpu, os_report::DebugConsole& console);

}  // namespace gekko::os_misc
