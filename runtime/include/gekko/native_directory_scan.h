#pragma once

// The game's directory scan 0x80044ED4, native over the asset namespace (docs/DISC_SYSTEM_MAP.md, "Cut 2").
//
//   count = scan(r4 = out, r5 = path, r6 = name filter or 0)
//
// The original opened the directory with the DVD SDK, read every entry, and for each one whose name holds the
// filter (strstr) wrote a 0x108-byte record into a heap block of `cap` records (cap: the word at -0x7BF0(r13)):
//   +0x000  the name and its NUL (strcpy: the record's other bytes keep what the heap block held)
//   +0x100  directories: -1; files: the length DVDOpen of the bare name reports (it resolves from the root: the
//           game never changes directory), -1 when that open fails
//   +0x104  1 directory, 2 file
// It then allocated a block of exactly `count` records, set *out to it, copied the records in, freed the first
// block and returned the count. A directory that does not open set *out = 0 and returned 0.
//
// Here the entries come from the namespace in its own order (the order the DVD SDK's owners served) and nothing
// opens, reads or closes anything: no DVDDir, no DVDFileInfo, no name pool. The two allocations and the free are
// the game's own heap functions, called in the original's order on a frame laid out like the original's, so the
// heap and every byte the game can read afterwards equal the original's. Its only caller is the movie list
// (0x803313E0, from StageSequence::Manager), which scans "." for ".sfd" names.
//
// Refused (the machine stops, naming it) rather than reproduced: more matching entries than `cap` (the original
// overran its block), a name of 0x100 bytes or more (strcpy overran the record), a failed allocation, a file too
// large for the original's 32-bit length, and a heap call that does not return to the scan.

#include <cstddef>

#include "gekko/types.h"

namespace gekko {

class Cpu;
namespace assets {
class AssetSystem;
}

class NativeDirectoryScan final {
public:
    NativeDirectoryScan(Cpu& cpu, const assets::AssetSystem& assets) noexcept : cpu_(cpu), assets_(assets) {}

    // Installs the owner. Before the guest boots; the namespace is read at each call.
    void install();

    struct Stats {
        u64 scans = 0;    // calls
        u64 records = 0;  // records returned
        u64 missing = 0;  // paths that are not a directory
    };
    const Stats& stats() const noexcept { return stats_; }

    // Every guest address the generator must route to an owner here (extract/native-owners.txt).
    struct Address {
        u32 address;
        const char* name;
        bool continuation;
    };
    static const Address* addresses(std::size_t& count) noexcept;

private:
    Cpu& cpu_;
    const assets::AssetSystem& assets_;
    Stats stats_;
};

}  // namespace gekko
