#pragma once

// The end of the game's own memory in this run (the first address past it).
//
// The game was made for a console with 24 MiB: its image and its heap end at 0x81800000, and a run without mods
// has exactly that (GuestMemory::kLegacyHeapEnd), byte for byte as before. A run whose mods replace the game's files
// is given more (GuestMemory::extend_legacy_heap): the mods' models, stages and tables are larger than the disc's, and
// several of them together did not fit - the game's heap ran out and its code read through the missing allocation
// (docs/RELEASE_VALIDATION.md, 2026-11-26). The game asks its operating system for the arena's bounds and builds its
// heap from them, so a higher end is simply a larger heap.
//
// Everything that tests "is this a pointer into the game's memory" compares with this value, not with the console's
// constant: a pointer into the added room is as good as any other.

#include "gekko/types.h"

namespace gekko {

constexpr u32 kConsoleMemoryEnd = 0x81800000u;
inline u32 g_game_memory_end = kConsoleMemoryEnd;  // written once, before the game's first instruction
inline u32 game_memory_end() noexcept { return g_game_memory_end; }

}  // namespace gekko
