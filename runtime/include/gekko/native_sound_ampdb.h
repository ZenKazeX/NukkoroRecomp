#pragma once

// The GCAX dB -> linear amplitude search 0x8043C3F0 (main.dol, PAL GUPP8P), owned natively (docs/re/sound.md "Owner 0x8043C3F0").
//
// The original is a linear scan over 0x401 entries {f32 amplitude, f32 dB} at 0x8056C150 (8 bytes each, dB ascending from -96.0 to
// 0.0). Input f1 (a double image of a single). Per entry: lfs f0 = dB; fcmpo cr0, f0, f1; "ble" continues (dB <= f1, and also
// an unordered compare); the first entry with dB > f1 returns f1 = its amplitude; when none does, f1 = the single at r2 + 0x1740.
//
// A call leaves, as the original: f1 and f0 (both slots, as lfs writes them), r0 = 0x401, r3 = 8 * index (0x2008 when no entry
// matched), r4 = table base, r5 = base + 8 * (index, or 0x400 when none matched), CTR = 0x401 - index (0 when none matched),
// CR0 and FPSCR[FPCC] of the last fcmpo, and the FPSCR exception bits an unordered fcmpo raises (a NaN input compares unordered
// with every entry). It reads the table from guest memory once (at the first call) and declines (the guest body runs) if the
// keys are not a non-decreasing, NaN-free sequence.
//
// op3 (2026-11-13): revived from tools/re/withdrawn_ampdb (withdrawn under shared-machine timing only) as a hook with a hand-back, not
// a native-owners row: the guest body stays, so the switch can turn the owner off. CMake SHADOW_OP3_AMPDB (default OFF) compiles it
// in default-on; GEKKO_OP3_AMPDB=0/1 overrides at run time.
#include "gekko/types.h"

namespace gekko {
class Cpu;
}

namespace gekko::sound_ampdb {

constexpr u32 kSearch = 0x8043C3F0u;
constexpr u32 kTable = 0x8056C150u;
constexpr u32 kEntries = 0x401u;
constexpr u32 kEndConstant = 0x1740u;  // r2 + 0x1740: the single returned when no entry matches

// Host model of the scan on bits: the number of entries the original steps over (the index of the first entry whose dB
// exceeds the input, kEntries when none does). `db_bits` are the single words of the dB column. Plain linear scan (the model).
u32 scan_index_linear(const u32* db_bits, u64 f1_bits) noexcept;
// The owner's search: binary search over a verified non-decreasing, NaN-free column; the same value as the scan.
u32 scan_index_binary(const u32* db_bits, u64 f1_bits) noexcept;

// Serves the call; false = declined (switch off or table not usable), the guest body then runs.
bool serve_search(Cpu& cpu);
void set_enabled(bool enabled) noexcept;
bool enabled() noexcept;

void install(Cpu& cpu);

}  // namespace gekko::sound_ampdb
