#pragma once

// MetroTRK's remaining entries, owned natively (docs/agents/axfx-effects.md, "Step G: debug.trk").
//
// The PC build has no debugger: the boot information names none (BI2 monitor kind 0), the native boot (native_boot.h)
// does not run InitMetroTRK / TRKInitializeNub, and InitMetroTRK / InitMetroTRK_BBA are stop owners (native_ipl.h).
// What was left of the debugger library in the build:
//
//   0x80370BF0  DBInit: three stores, no inputs: *(r13 + 0x5A18) = 0x80000040 (__DBInterface), *(0x80000048) =
//               0x370C60 (the physical address of the debugger exception destination 0x80370C60, a mfmsr / mtmsr
//               stub that falls into DBExceptionDestination 0x80370C18), *(r13 + 0x5A1C) = 1 (the verbosity flag).
//               Entered only by the original-boot capture tool and a bootstrap-image test; owning it exactly (same
//               stores) lets the generator drop DBExceptionDestination, whose address DBInit formed.
//   0x803A051C / 0x803A05A0 / 0x803A065C  the console file's close, write and read procedures that MSL's stdio objects
//               (stdin / stdout / stderr, data words at 0x80568F0C..) name: they talk to the debugger through the TRK
//               message layer when the nub is up (*(u32*)0x805A4F08 != 0; write and read also need the console flag
//               byte at 0x805A54A8). Without a debugger they return 1 (__io_error) and touch nothing. The owners do
//               exactly that; if the nub were up (it never is: nothing starts it) they stop the run.
//
// The console write routine 0x803AF5A8 reaches 0x803A05A0 only on development hardware after a UART write; with the
// printf family owned (native_os_report.h) no print reaches it; in the Westopolis replay none of the three procedures
// runs (guest watch of the two cells, Step G).

#include "gekko/types.h"

#include <cstddef>

namespace gekko {
class Cpu;
}

namespace gekko::trk {

constexpr u32 kDbInit = 0x80370BF0u;
constexpr u32 kConsoleClose = 0x803A051Cu;
constexpr u32 kConsoleWrite = 0x803A05A0u;
constexpr u32 kConsoleRead = 0x803A065Cu;

constexpr u32 kDbInterface = 0x80000040u;         // __DBInterface
constexpr u32 kDbExceptionHookPhysical = 0x80000048u;
constexpr u32 kDbExceptionDestinationPhysical = 0x00370C60u;
constexpr u32 kSdaDbInterface = 0x5A18u;           // r13-relative
constexpr u32 kSdaDbVerbosity = 0x5A1Cu;
constexpr u32 kNubState = 0x805A4F08u;             // u32: the TRK nub's state (0: not started)
constexpr u32 kConsoleEnabled = 0x805A54A8u;       // u8: the console redirect flag

void serve_db_init(Cpu& cpu);
void serve_console_close(Cpu& cpu);   // r3 = 1
void serve_console_write(Cpu& cpu);   // r3 = 1
void serve_console_read(Cpu& cpu);    // r3 = 1

void install(Cpu& cpu);

struct Owners {
    struct Address {
        u32 address;
        const char* name;
    };
    static const Address* addresses(std::size_t& count) noexcept {
        static constexpr Address kAddresses[] = {{kDbInit, "DBInit"},
                                                 {kConsoleClose, "TRK console close"},
                                                 {kConsoleWrite, "TRK console write"},
                                                 {kConsoleRead, "TRK console read"}};
        count = sizeof(kAddresses) / sizeof(kAddresses[0]);
        return kAddresses;
    }
};

}  // namespace gekko::trk
