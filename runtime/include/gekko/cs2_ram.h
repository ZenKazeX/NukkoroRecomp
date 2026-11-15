#pragma once

// cs2/ram (2026-11-13): host-side RAM savings that change no byte the game reads (docs/agents/cs2-ram.md).
//
// One switch for all of them: GEKKO_CS2_RAM=0 turns every one off (the previous behaviour), so a same-tree run with the
// switch off is the base for the gates. On by default.
//   delta   mount-time replacement files kept as patches over the mapped original instead of full private copies
//   trim    pages of host files read once at the mount (txd scans, tree reads) leave the working set when done
//   lazy    the button glyphs' scaled sheets are made when a sheet is first served, not for all 27 at the mount

#include <cstdlib>

namespace gekko::cs2_ram {

inline bool enabled() noexcept {
    static const bool on = [] {
        const char* setting = std::getenv("GEKKO_CS2_RAM");
        return setting == nullptr || setting[0] != '0';
    }();
    return on;
}

}  // namespace gekko::cs2_ram
