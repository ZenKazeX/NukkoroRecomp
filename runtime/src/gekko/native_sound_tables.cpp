// The native owner of the game's sound-effect table lookup (gekko/native_sound_tables.h).

#include "gekko/native_sound_tables.h"

#include "gekko/cpu.h"
#include "gekko/guest_memory.h"

#include <array>
#include <cstdio>

namespace gekko::sound_tables {

namespace {

// AudioDataStageCity.bin row 90 as shipped (bytes +8..+0xF; +0x10/+0x14 are zero) and its correction.
struct Erratum {
    u32 rows;        // the table's row count
    u32 row;         // the defective row's index
    u32 id;
    std::array<u8, 8> original;  // +8..+0xF: bank, program, pitch/volume, +0xC, duration, +0xE, +0xF
    u8 bank, program;            // program 0xFF: disabled (the lookup refuses the row: no sound)
};
constexpr Erratum kErrata[] = {
    {91u, 90u, 0x7160u, {{0x00, 0x00, 0x00, 0x00, 0x7B, 0x00, 0x00, 0x00}}, 0u, 0xFFu},  // SE_Z1090_WARPHOLE
};

void correct(GuestMemory& memory, u32 table, u32 row, Report& report) noexcept {
    const u32 count = memory.peek32(table);
    const u32 first = memory.peek32(table + 4u);
    for (const Erratum& e : kErrata) {
        if (count != e.rows || row != first + e.row * kRowBytes || memory.peek32(row + kRowId) != e.id ||
            memory.peek32(row + kRowLive) != 0u || memory.peek32(row + kRowLimit) != 0u)
            continue;
        bool original = true;
        for (u32 i = 0; i < e.original.size(); ++i) original = original && memory.peek8(row + kRowBank + i) == e.original[i];
        if (!original) continue;
        memory.write8(row + kRowBank, e.bank);
        memory.write8(row + kRowProgram, e.program);
        ++report.errata_applied;
        std::fprintf(stderr, "[sound-tables] corrected row %u (id 0x%04X): bank %u program %d (shipped unfilled)\n",
                     e.row, e.id, e.bank, static_cast<int>(static_cast<s8>(e.program)));
    }
}

}  // namespace

int slot_of(u32 id) noexcept {
    // 0x801D8838 and its jump table 0x8053E188.
    switch ((id >> 12) & 0xFu) {
        case 0xEu: return 0;
        case 1u: return 1;
        case 2u: return 2;
        case 3u: return 3;
        case 4u: return 4;
        case 5u: return 5;
        case 6u: return 6;
        case 7u: case 8u: case 9u: return 7;
        default: return -1;
    }
}

void serve_lookup(Cpu& cpu, Report& report) noexcept {
    cpu.gpr[3] = lookup(cpu.guest_memory(), cpu.gpr[3], cpu.gpr[4], report);
}

u32 lookup(GuestMemory& memory, u32 manager, u32 id, Report& report) noexcept {
    ++report.lookups;
    u32 result = 0;
    const int slot = slot_of(id);
    const u32 table = slot < 0 ? 0u : manager + static_cast<u32>(slot) * 16u;
    if (table != 0u) {
        const s32 count = static_cast<s32>(memory.peek32(table));
        const u32 first = memory.peek32(table + 4u);
        const u32 wanted = id & 0xFFFFu;
        u32 row = 0;
        for (s32 i = 0; i < count; ++i) {
            const u32 candidate = first + static_cast<u32>(i) * kRowBytes;
            if (memory.peek32(candidate + kRowId) == wanted) {
                row = candidate;
                break;
            }
        }
        if (row == 0u) {
            ++report.missing;
        } else {
            correct(memory, table, row, report);
            const s32 limit = static_cast<s32>(memory.peek32(row + kRowLimit));
            const s32 live = static_cast<s32>(memory.peek32(row + kRowLive));
            if (static_cast<s8>(memory.peek8(row + kRowProgram)) < 0) {
                ++report.refused_disabled;
            } else if (limit != 0 && live >= limit) {
                ++report.refused_limit;
            } else {
                if (limit != 0) memory.write32(row + kRowLive, static_cast<u32>(live + 1));
                result = row;
                ++report.found;
            }
        }
    } else {
        ++report.missing;
    }
    return result;
}

}  // namespace gekko::sound_tables
