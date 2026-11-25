#pragma once

// Shadow's air-shoe jets: all four flames, as the game was built to show them - a DELIBERATE DEVIATION from the
// retail game, which shows three (the user's choice, 2026-11-23; docs/effects/particles.md "Jet").
//
// The original (PlayerEffect_cpp::Jet, vt 0x80522AC0; the same bytes in the PAL, USA and Japan main.dol):
//  * a Jet holds four flame clumps at +0x5C / +0x60 / +0x64 / +0x68, one per shoe node of the table 0x804B368C
//    (7, 8, 9, 10); every loop over them - the constructor's attach loop, the update (0x800A57B8), the fade
//    (0x800A56F4) and the destructor (0x800A5AA4) - runs four slots and skips an empty one;
//  * the constructor 0x800A5B6C loads the template SHADOW_FOOTEFF_R.DFF into +0x5C, then clones it three times into
//    +0x60, +0x64 and - the slip - +0x5C again (0x800A5C58 `stw r3,0x5C(r30)`) instead of +0x68. So the fourth slot
//    stays empty, node 10 never gets a flame, and the template clump is leaked.
//
// The fix, owned here: RpClumpClone 0x804580E0, only when called from that third clone (LR 0x800A5C58) of a Jet
// under construction. It makes the third clone as before (returned in r3, stored to +0x5C by the original's store)
// and a fourth one from the same template, stored to +0x68 before the original continues. The constructor's own loop
// then adds the fourth clump to the world and takes its frame, exactly as for the other three; the update places it
// on node 10 and the destructor destroys it. The template stays leaked, as in retail. Every other call of
// RpClumpClone runs the original (the hook declines).
//
// SHADOW_RETAIL_SHOE_JETS=1 leaves the hook out: the retail three jets (comparisons against retail captures, tapes
// recorded with the retail behaviour - the extra clone is one more allocation in the game's heap).

namespace gekko {

class Cpu;

namespace shoe_jets {

void install(Cpu& cpu);

// The hook's own figures: Jets given their fourth flame, and RpClumpClone calls passed to the original.
struct Stats {
    unsigned long long fourth_jets = 0;
    unsigned long long passed = 0;
};
const Stats& stats() noexcept;

}  // namespace shoe_jets

}  // namespace gekko
