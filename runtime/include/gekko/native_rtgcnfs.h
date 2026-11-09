#pragma once

// RenderWare's GameCube file system RtGCNFS (the RtFS device every RwFopen / RwFread of the game goes through),
// native over the asset namespace. docs/RTGCNFS_SPEC.md (the per-function specification from the disassembly) and
// docs/DISC_SYSTEM_MAP.md, "Cut 4".
//
// The device's state is RenderWare's own, in guest memory, with its layout: the RtFile records (0x90 bytes, the
// array at fs+0x58) and the RtFileSystem object. Owned here are the device functions that touched the disc:
//
//   open   0x8047B874  the path from RenderWare's own helpers (_rtGetFileNameFromDeviceName, the fs's device name,
//                      _rtFSBuildFileName), resolved in the namespace instead of DVDOpen
//   close  0x8047B988  status CLOSED (DVDClose had nothing left to do)
//   read   0x8047BC60  the whole read: 64-bit position, the 0x800-byte cache, 32-aligned direct transfers; each
//                      transfer copies from the asset at once instead of DVDReadAsyncPrio + its callback
//   sync   0x8047C1A4  the status with a drive that is always ready and transfers that are always complete
//   abort  0x8047C3D4  nothing in flight: TRUE
//
// The thread-scheduling points stay exactly where they were: the original yields (OSYieldThread) at the top of every
// chunk of a synchronous read and once in every transfer, while it waited on the drive. Here read yields at the same
// points, from the same stack depth: it keeps its state in a tagged frame where its own frame was, hands the CPU to
// the game's OSYieldThread, and resumes at one of two continuation entries inside its extent (0x8047BC64, 0x8047BC68).
// A savestate taken inside a yield resumes. Each transfer still calls the file system's drive-error callback
// (fs+0x5C) with RenderWare's "no error" state 7, as the original's drive poll did.
//
// Of the DVDFileInfo RenderWare embedded at +0x40 only the asset token (+0x70) and the length (+0x74) are kept,
// and the transfer result (+0x6C) the read-complete callback stored. Refused (the machine stops, naming it): a read
// the host cannot deliver, and a frame that does not resume with its tag.

#include <cstddef>

#include "gekko/types.h"

namespace gekko {

class Cpu;
class NativeFileApi;
namespace assets {
class AssetSystem;
}

class NativeRtGcnFs final {
public:
    NativeRtGcnFs(Cpu& cpu, const assets::AssetSystem& assets, NativeFileApi& files) noexcept
        : cpu_(cpu), assets_(assets), files_(files) {}

    // Installs the owners. After the file API, before the guest boots.
    void install();

    struct Stats {
        u64 opens = 0;
        u64 open_misses = 0;
        u64 reads = 0;
        u64 transfers = 0;
        u64 bytes = 0;
        u64 yields = 0;
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
    NativeFileApi& files_;
    Stats stats_;
};

}  // namespace gekko
