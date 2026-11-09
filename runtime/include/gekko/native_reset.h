#pragma once

// The console's reset and boot controls, native: what the OS learned from the
// processor interface's reset register and reset-switch state, and what it
// asked of them, with no processor interface underneath.
//
// docs/native-time-20261005/STAGE6_PI_MI.md (D5). On the console the reset
// register (PI 0x24) carried the boot's reset code and, written, reset or
// rebooted the machine; the PI cause register's RSWST bit was the front-panel
// reset button (1 = released). Here:
//
//   OSResetSystem 0x80376FB8      the game asks the console to restart, hot-
//                                 reset or shut down: on the PC the program
//                                 ends (a guest-requested exit naming the
//                                 request). Reached from the game's reset
//                                 sequence (a reset-button release, which the
//                                 PC cannot produce), __check_pad3 (the boot
//                                 pad-3 combo word, 0 here) and the C
//                                 runtime's exit. On the console it ended in
//                                 Reset's `b .` after the PI writes - here,
//                                 before, that froze the game.
//   OSGetResetCode 0x803771B8     a PC process always starts cold: 0 (no reset
//                                 code; bit 31, OS_RESETCODE_RESTART, clear -
//                                 the only bit its one caller, OSInit's
//                                 ClearArena, tests)
//   OSGetResetButtonState 0x803772E4
//                                 FALSE: the PC has no reset button. The PI
//                                 model never set RSWST, so until stage 6 the
//                                 OS saw the button HELD (characterised in the
//                                 design); the game's reset handler now takes
//                                 its normal path. Refused when the boot
//                                 information asks for the kiosk auto-reset
//                                 (0x800030E3 & 0x1F, 0 on the PC).
//   the memory interface's OnReset 0x803769A0
//                                 refused (stop): it is a record of the OS
//                                 reset queue, which only OSResetSystem walks.
//
// With these owned, Reset, __OSDoHotReset, KillThreads,
// __OSCallResetFunctions and the OS reboot path lose every caller and the
// generator prunes them. No state: nothing here is remembered between calls.

#include <cstddef>

#include "gekko/types.h"

namespace gekko {

class Cpu;

class NativeReset final {
public:
    explicit NativeReset(Cpu& cpu) noexcept : cpu_(cpu) {}

    // Installs the owners. Before the guest boots.
    void install();

    struct Stats {
        u64 reset_code_reads = 0;    // OSGetResetCode
        u64 reset_button_polls = 0;  // OSGetResetButtonState
    };
    const Stats& stats() const noexcept { return stats_; }

    // Every guest address the generator must route to an owner here
    // (extract/native-owners.txt).
    struct Address {
        u32 address;
        const char* name;
        bool continuation;
    };
    static const Address* addresses(std::size_t& count) noexcept;

private:
    Cpu& cpu_;
    Stats stats_;
};

}  // namespace gekko
