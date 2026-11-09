#pragma once

// The game's sound-effect tables, resolved natively (docs/GCAX_SOUND_MAP.md; phase 1 of the native sound path).
//
// Original behaviour (main.dol, PAL GUPP8P): PlaySE 0x801D5F14 resolves a sound id with the lookup 0x801D88CC
// (mgr, id). The id's nibble (id >> 12) & 0xF picks one of the manager's eight table slots through the helper
// 0x801D8838 and its jump table 0x8053E188: 0xE -> slot 0 (System), 1..6 -> slots 1..6 (Gadget, Weapon, Player,
// PlayerHero, PlayerDark, Enemy), 7/8/9 -> slot 7 (the stage's table), anything else -> none. A slot is 16 bytes at
// mgr + 16 * slot: +0 row count, +4 the first row, +8 the file buffer, +0xC the table parser. The table loader
// 0x801D8AFC reads an AudioData*.bin file and its parser lays the rows out there (0x18 bytes each). The lookup takes
// the first row whose +4 word equals id & 0xFFFF; a row whose program (+9, signed byte) is negative is disabled; a
// row with a nonzero limit (+0x14) counts the starts of the current frame in +0x10 and refuses a request at the
// limit (the count is increased here and cleared for every row at the start of each SE frame by 0x801D87EC; nothing
// decrements it). The row is returned; the SE manager then reads its GCAX bank (+8) and program (+9) and sends them
// as MIDI bank select / program change before the note.
//
// Native implementation: this owner serves 0x801D88CC on every call with exactly that contract
// (sound_table_contract_tests compares it with the original instructions), so neither the lookup nor its helper is
// generated; the native SE manager (gekko/native_se_manager.h) calls lookup() directly. The rows stay in the buffer
// the game loaded because the still-translated stage functions load and free them (SCAFFOLDING: deleted when sound
// phase 3 parses the tables natively).
//
// Data errata - DELIBERATE DEVIATIONS from the original game, each chosen by the user. The shipped tables have one
// defective row, corrected when it is resolved:
//   AudioDataStageCity.bin row 90, 0x7160 SE_Z1090_WARPHOLE: unfilled (bank 0, program 0, every field zero but the
//   default duration), so every Black Arms tank warp-in plays B0_SYSTEM program 0, the menu window-open sound. The
//   original does exactly this (Dolphin reference trace of the retail disc, docs/GCAX_SOUND_MAP.md: same voice, same
//   gains, present in Dolphin's DSP output). The user chose to fix this original-game bug (2026-10-26): the row is
//   disabled (program -1), the lookup refuses it and the warp-in is silent. A first correction to B71_CITY program 90
//   was withdrawn by the user.
// An erratum matches the whole original row and the table's row count, so any other data is left untouched.

#include "gekko/types.h"

#include <cstddef>

namespace gekko {
class Cpu;
class GuestMemory;
}

namespace gekko::sound_tables {

constexpr u32 kLookup = 0x801D88CCu;  // lookup(mgr, id) -> row or 0: served natively
constexpr u32 kRowBytes = 0x18u;
constexpr u32 kRowId = 0x4u;          // u32: the id this row answers (id & 0xFFFF)
constexpr u32 kRowBank = 0x8u;        // u8: GCAX bank
constexpr u32 kRowProgram = 0x9u;     // s8: program; negative = disabled
constexpr u32 kRowLive = 0x10u;       // s32: starts in the current SE frame
constexpr u32 kRowLimit = 0x14u;      // s32: maximum starts per frame, 0 = none

// The table slot an id belongs to (0..7), or -1 when the id has no table.
int slot_of(u32 id) noexcept;

struct Report {
    u64 lookups = 0, found = 0, refused_disabled = 0, refused_limit = 0, missing = 0;
    u64 errata_applied = 0;
};

// The lookup (r3 = manager, r4 = id): writes the row address (or 0) to r3 and returns; changes nothing else but the
// row's live count, as the original does, and a defective row's bank/program (errata above).
void serve_lookup(Cpu& cpu, Report& report) noexcept;

// The same lookup for a native caller (the native SE manager): `tables` is the manager's table block (mgr + 8).
u32 lookup(GuestMemory& memory, u32 tables, u32 id, Report& report) noexcept;

// The guest entries this owner serves on every call: must equal its rows of extract/native-owners.txt
// (native_file_api_tests).
struct Owners {
    struct Address {
        u32 address;
        const char* name;
    };
    static const Address* addresses(std::size_t& count) noexcept {
        static constexpr Address kAddresses[] = {{kLookup, "SoundTable lookup"}};
        count = sizeof(kAddresses) / sizeof(kAddresses[0]);
        return kAddresses;
    }
};

}  // namespace gekko::sound_tables
