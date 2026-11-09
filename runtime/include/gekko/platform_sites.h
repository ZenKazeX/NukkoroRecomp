#pragma once

// The listed platform sites, as the generator lowered them.
//
// extract/platform-sites.txt (docs/native-time-20261005/STAGE2_VI.md D5,
// STAGE4_GX_SYNC.md D6, STAGE6_PI_MI.md D4): a register access of guest code
// whose own subsystem is not native, pinned by its DOL word. The generator
// emits, in place of the bus access:
//
//   a read (lhz)      Cpu::platform_read16 - the native platform's answer
//   a write (sth)     Cpu::platform_write16 - the native platform's effect
//   a dead write      nothing: the register it programmed has no consumer on
//                     this platform (the memory interface's, stage 6)
//   a value (li)      Cpu::platform_value - a console constant of the platform
//                     written into game code, the native platform's value
//                     (the video field's length, B2 one time base)
//
// The interpreter (the reference oracle's replay) applies the same table to the
// same words (Cpu::execute), so the two engines agree; there is no register
// behind any of these addresses, and an access no table lists stops the
// production machine (Cpu::set_unmapped_access_stops).

#include "gekko/types.h"

namespace gekko {

// One listed site: the instruction's address, its DOL word, and whether the
// generator dropped it (kind `dead`, a store only).
struct PlatformSiteRecord {
    u32 address;
    u32 word;
    bool dead;
};

}  // namespace gekko
