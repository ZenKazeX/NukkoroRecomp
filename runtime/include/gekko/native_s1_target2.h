#pragma once

// S1 target (round 11): the target registry's query layer 0x801E7000-0x801EB800 served natively, entry by entry, with a hand-back.
// Evidence, layouts and the call census: docs/re/target-tree.md, docs/re/target-search.md. Names are this project's own.
//
// Stage 1 entries (each a whole call tree of the original: the frames, the stores, the guest allocator calls in the original's order):
//   0x801E7C40 CreateFromRegistry(r3 = registry + 4 ...)  -> r3 = a new TargetSearchList (new 0x14, new 4N, N push_backs, prune 0x801E9498)
//   copy 0x801E8A68, dtor 0x801E8974, the flag filters 0x801E83D0 / 0x801E8448, same-target 0x801E8158 (stage 1);
//   sphere 0x801E82C4, view cone 0x801E84C0, range 0x801E8528 and nearest 0x801E879C (stage 1b: floating point, the Target's position
//   virtual devirtualised behind a vtable-word guard, every target of the query proven to be a Target)
//
// Rules (docs/agents/S1_GAMEPLAY_2026-11-12.md, amendment 5 and the round-11 conditions):
//  - every dead stack word of the original's frames is written, in order, with the original's values (the models are generated from the
//    DOL by tools/re/gen_frames.py for straight-line functions and written by hand for loops and branches);
//  - every `operator new` / `operator delete` goes through the guest allocator, in the original's order and with its sizes;
//  - a call is served only when proven (live holders, bounded sizes, a Target vtable word, no aliasing with the stack window, no
//    allocator failure path); otherwise it is handed back BEFORE any write and the original body runs;
//  - hooks are installed only when the switch is on: GEKKO_S1_TARGET2=1 (CMake option SHADOW_S1_TARGET2 makes it default on);
//    GEKKO_S1_TARGET2_STATS=1 prints the served / handed-back counts per entry and reason; GEKKO_S1_TARGET2_ALLOCTRACE=<file> logs every
//    operator new / delete of the run (kind, size or pointer, LR) for the heap-order proof (off vs on must be byte-identical).

#include "gekko/types.h"

namespace gekko {
class Cpu;
}

namespace gekko::s1_target2 {

enum class Entry : unsigned { Factory, CopyCtor, Dtor, FlagA, FlagB, SameTarget, Sphere, ViewCone, Range, Nearest, kCount };

enum class Reason : unsigned { None, Disabled, BadShape, ExpiredOrNull, TooLarge, Alias, KindOfTarget, kCount };

struct Stats {
    u64 served[static_cast<unsigned>(Entry::kCount)] = {};
    u64 declined[static_cast<unsigned>(Entry::kCount)][static_cast<unsigned>(Reason::kCount)] = {};
};

// Serve one call of an entry from the Cpu state at its entry address. True: served (state written, r3 set, the caller continues at LR).
bool serve(Cpu& cpu, Entry entry) noexcept;

const Stats& stats() noexcept;
void set_enabled(bool enabled) noexcept;
// The vtable-word guard of the devirtualised Target slot 2 (0x8053ECB0): on by default; off only for the contract test's negative control.
void set_guard_enabled(bool enabled) noexcept;
bool enabled() noexcept;
void install(Cpu& cpu);

}  // namespace gekko::s1_target2
