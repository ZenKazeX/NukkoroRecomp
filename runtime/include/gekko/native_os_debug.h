#pragma once

// InitializeUART (0x803B57D8, sdk-identify PROVEN: docs/agents/sdk-identify.md), owned natively (docs/agents/axfx-effects.md,
// "Step C: os.debug"). The debug UART's bring-up, entered from one place: the console write routine 0x803AF5A8
// (MSL's __write_console as the game links it) calls it with the UART's first-use flag clear (small-data cell
// 0x5EA0), and sets that flag when it returns 0. Original behaviour, a function of three guest cells and the console type:
//
//   if (*(u32*)(r13 + 0x5EE4) == 0xA5FF005A)            return 0           (already brought up: nothing written)
//   if (OSGetConsoleType() & 0x10000000) == 0:          *(r13 + 0x5EE0) = 0;  return 2     (retail: no UART)
//   else  *(r13 + 0x5EE0) = 0xA5FF005A; *(r13 + 0x5ED8) = 0; *(r13 + 0x5EDC) = 1; return 0
//
// OSGetConsoleType (0x80370E04) is: type = BI2 pointer (r13 + 0x5A20) != 0 ? BI2->consoleType (+0x2C) : 0;
// type == 0 -> 0x10000002 (the development-hardware default when the block is absent). The owner reads the same cells.
// The UART enable word (0x5EE0) is what the native IPL module's WriteUARTN owner checks (native_ipl.h).
//
// What differs from the original (not observable): the original saves its link register into the caller's frame
// word at 4(r1) and pushes an 8-byte frame (back chain at -8(r1)); r0 and cr0 are scratch. The owner writes
// none of them.

#include "gekko/types.h"

#include <cstddef>

namespace gekko {
class Cpu;
}

namespace gekko::os_debug {

constexpr u32 kInitializeUart = 0x803B57D8u;

// Small-data cells (offsets from r13).
constexpr u32 kBi2Cell = 0x5A20u;        // the boot information 2 block (consoleType at +0x2C), or 0
constexpr u32 kUartBroughtUp = 0x5EE4u;  // compared against kUartEnabled: "already initialized"
constexpr u32 kUartEnabledCell = 0x5EE0u;
constexpr u32 kUartCell0 = 0x5ED8u;      // cleared by the bring-up
constexpr u32 kUartCell1 = 0x5EDCu;      // set to 1 by the bring-up
constexpr u32 kConsoleTypeOffset = 0x2Cu;
constexpr u32 kDefaultConsoleType = 0x10000002u;
constexpr u32 kDevelopmentBit = 0x10000000u;
constexpr u32 kUartEnabled = 0xA5FF005Au;

// OSGetConsoleType (0x80370E04) as a function of the guest cells it reads; reads only.
u32 console_type(Cpu& cpu);

void serve_initialize_uart(Cpu& cpu);  // r3 = 0 brought up / already up, 2 retail console

void install(Cpu& cpu);

// The guest entries these owners serve: must equal their rows of extract/native-owners.txt (native_file_api_tests).
struct Owners {
    struct Address {
        u32 address;
        const char* name;
    };
    static const Address* addresses(std::size_t& count) noexcept {
        static constexpr Address kAddresses[] = {{kInitializeUart, "InitializeUART"}};
        count = sizeof(kAddresses) / sizeof(kAddresses[0]);
        return kAddresses;
    }
};

}  // namespace gekko::os_debug
