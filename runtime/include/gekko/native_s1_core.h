#pragma once

// S1 core: the layout manager's one-player per-frame scan (main.dol, PAL GUPP8P), served natively with a hand-back.
// Evidence and the data layout: docs/re/core.md, docs/DRAW_DISTANCE.md. Names are this project's own.
//
//   0x800CAC08 LayoutScan(r3 = manager, r4 = point): the game's layout manager walks its 0x800 slots (0x20 bytes each at
//   [manager + 0x18]) every frame, with one viewport's point (a vec3 at r4). Per slot (record = [slot], flags = [record + 0x18]):
//     - [manager + 0x428] bit 0 ("restricted") and a record without flag 0x8000: the slot is skipped;
//     - flag 0x2 (live): counted into [manager + 8] (zeroed at the start), nothing else;
//     - flag 0x1 (dormant) with a type ([slot + 0x10] >= 0): the spawn test 0x800CA8D8: flag 0x20000 -> no; otherwise the
//       range test 0x800CA970 (flag 0x80800 -> in range; else |pos - point|^2 <= (100 * range)^2), and an object in range is
//       created through its type's factory;
//     - neither flag 0x1 nor 0x2, saved flags ([record + 0x1C]) bit 0: respawn bookkeeping (flags 0x40 / 0x800000 restore the
//       saved flags and relink the slot's group through 0x800CAE24; flag 0x10000 sets 0x40 once the point is out of range).
//   The first two cases and a dormant slot that stays out of range change nothing but the live count. This owner serves exactly
//   those frames: it scans the 0x800 slots without writing, and when no slot needs the creating or the bookkeeping paths it
//   writes the count, the frames the original's prologue and its 0x800CA8D8 calls leave, and returns. If any slot needs the
//   others (a spawn, a restore, the 0x40 mark, a null table/slot oddity) it DECLINES before writing anything and the original
//   body runs the whole frame from its start (the scan is idempotent: it only rewrites [manager + 8] from zero).
//
// What a served call leaves, as the original's (with 0x800CA970 served by NativeObjectRange, as in the game): [manager + 8];
// the frame words of its own prologue ([r1 - 0x20] back chain, [r1 + 4] caller's LR slot of the new frame, r27..r31 at
// [r1 - 0x14 .. r1 - 4]); and, when a spawn test ran, the 0x800CA8D8 prologue words of its last call ([r1 - 0x40] back chain,
// [r1 - 0x1C] = 0x800CAD8C, r29..r31 at [r1 - 0x2C .. r1 - 0x24] - the last tested slot). r27..r31, r1 and LR are as at entry.
// Not kept (by the ABI): r0, r3..r12, CR0, CTR, XER and the dead stack below those frames. No floating-point register or FPSCR
// is touched (the range test is the same host-float arithmetic as NativeObjectRange::in_range).

#include "gekko/types.h"

#include <cstddef>

namespace gekko {
class Cpu;
}

namespace gekko::s1_core {

constexpr u32 kLayoutScan = 0x800CAC08u;

struct Stats {
    u64 calls = 0;
    u64 served = 0;
    u64 declined_spawn = 0;    // a slot in range (spawn) or a respawn path
    u64 declined_other = 0;    // a null record
    u64 slots_scanned = 0;
};

// Serves one call. True: served (state written). False: nothing written, the original body must run.
bool serve_layout_scan(Cpu& cpu) noexcept;

const Stats& stats() noexcept;
void set_enabled(bool enabled) noexcept;  // GEKKO_S1_CORE_SCAN=1 enables (off by default: no measurable gain)
void install(Cpu& cpu);

}  // namespace gekko::s1_core
