#pragma once

// OSPanic 0x8037324C, owned natively (docs/agents/os-other.md, step 3; removal plan service os.other).
//
// Original behaviour (main.dol, PAL GUPP8P), OSPanic(file r3, line r4, message r5, varargs ...): it masks interrupts
// (OSDisableInterrupts), prints the message through the C library's vprintf (0x803AA414), then OSReport's
//   " in \"%s\" on line %d.\n"  (file, line)  and  "\nAddress:      Back Chain    LR Save\n",
// walks up to 17 stack frames printing "0x%08x:   0x%08x    0x%08x\n" (frame, back chain, saved LR) and calls
// PPCHalt 0x80370B1C, which is `sync` followed by an endless loop. It never returns. The text goes to the debug UART
// (InitializeUART, owned: a retail console has none, so nothing is written on the console either).
//
// The game reaches it from one place: 0x80050FF4, called by the RenderWare event handler at 0x80051A9C, which passes
// the file name cell r2-0x79A8, line 0x318 and the message "The End." - the program's own end, not a failure: the
// original stops the console there. The PC equivalent is the program ending, as OSResetSystem does
// (native_reset.cpp): a call whose return address is the one inside that routine (0x80051018) is a guest-requested
// exit (StopReason::HleExit); any other caller (none exists in the game; the SDK's own panics are owned elsewhere or
// stop earlier) is a fault, the console halt the original would reach. Either way the stop names the file, line and
// message the original would have printed (the message's format string is quoted as it stands, with no varargs).
//
// Not kept: the interrupt mask and the printed text (nothing observes either once the program ends).

#include "gekko/types.h"

#include <cstddef>
#include <string>

namespace gekko {
class Cpu;
}

namespace gekko::os_panic {

constexpr u32 kOSPanic = 0x8037324Cu;
constexpr u32 kPpcHalt = 0x80370B1Cu;  // PPCHalt: `sync`, then an endless loop; never returns
constexpr u32 kHaltWrapper = 0x80379720u;  // a frame, `bl PPCHalt`, the frame popped: the MSL abort / exit paths' halt (never returns)
constexpr u32 kGameEndReturn = 0x80051018u;  // after 0x80050FF4's `bl OSPanic`

// What the original would have printed first: "<file>:<line>: <message>" (strings read from guest memory, bounded).
std::string describe(Cpu& cpu);

// Requests the stop; true when it is the game's own program end (HleExit).
bool serve_panic(Cpu& cpu);

// Must equal this owner's row of extract/native-owners.txt (native_file_api_tests).
struct Owners {
    struct Address {
        u32 address;
        const char* name;
    };
    static const Address* addresses(std::size_t& count) noexcept {
        static constexpr Address kAddresses[] = {{kOSPanic, "OSPanic"}, {kPpcHalt, "PPCHalt"},
                                                  {kHaltWrapper, "the halt wrapper 0x80379720"}};
        count = 3;
        return kAddresses;
    }
};

// PPCHalt 0x80370B1C (cpu.config): the original executes `sync` and loops forever, so the console hangs. EXPLICIT
// DIFFERENCE: the owner stops the run with an ExecutionFault naming the return address instead of spinning; nothing the
// program could observe continues after the halt in either case.
void serve_halt(Cpu& cpu);

void install(Cpu& cpu);

}  // namespace gekko::os_panic
