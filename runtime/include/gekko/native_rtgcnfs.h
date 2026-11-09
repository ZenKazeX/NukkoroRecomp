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
// points, from the same stack depth, with the original's frames (2026-11-04): read's 0x40 (r23..r31 saved by
// _savegpr_23) and the transfer's 0x20 below it, its state in r23..r31 as the original allocates them, so the yield
// carries it in the thread context and every stack byte is the original's. It hands the CPU to the game's
// OSYieldThread and resumes at one of two continuation entries inside its extent (0x8047BC64, 0x8047BC68). Open,
// close, sync and abort write their original frames too (docs/RESOURCE_AUDIT_2026-11-04.md F2).
// A savestate taken inside a yield resumes. Each transfer still calls the file system's drive-error callback
// (fs+0x5C) with RenderWare's "no error" state 7, as the original's drive poll did.
//
// Of the DVDFileInfo RenderWare embedded at +0x40 only the asset token (+0x70) and the length (+0x74) are kept,
// and the transfer result (+0x6C) the read-complete callback stored. Refused (the machine stops, naming it): a read
// the host cannot deliver, and a resume whose frame (back chains, return site) is not the read's.

#include <cstddef>

#include <functional>
#include <optional>

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
        u64 background_reads = 0;   // TBGRead's chunked reads served inline (GEKKO_NATIVE_BG_READ=1)
        u64 background_chunks = 0;  // ... their RwFread chunks
        u64 sync_reads = 0;         // reads for a native caller (read_sync)
        u64 yields_elided = 0;      // ... their yields continued at once (no other thread of the caller's priority)
    };
    const Stats& stats() const noexcept { return stats_; }

    // Every guest address the generator must route to an owner here (extract/native-owners.txt).
    struct Address {
        u32 address;
        const char* name;
        bool continuation;
    };
    static const Address* addresses(std::size_t& count) noexcept;

    // TBGRead's chunked read of a whole file into `destination` for a native caller (W2,
    // docs/NATIVE_STREAMING_REPLACEMENT_PLAN.md): the read loop 0x80045150's work after its allocation - the inline
    // reads of GEKKO_NATIVE_BG_READ=1 - returning what the loop returns (the sum of chunk x RwFread's count, the last
    // chunk rounded up to 32), or nothing when the run was stopped. Only with GEKKO_NATIVE_BG_READ=1.
    // A synchronous read for a native caller (Phase 4: NativeOne's Open): read's own code - its frame below the
    // caller's, the transfers, the cache, the drive-error callback, the memmove of a cache serve - with each yield
    // (two per chunk) continued at once instead of handed to OSYieldThread 0x80378A08. Only for a caller that checked
    // no other thread of its priority is ready: the yield would then return at once, and what is not done is
    // OSYieldThread's own bookkeeping (its frames, the current thread's context save, the run-queue round trip).
    // Entered as the file system's read slot is: r1 = the caller's frame, LR = its return site, the caller's
    // non-volatile registers. Returns read's result, or nothing when the run stopped.
    std::optional<u32> read_sync(Cpu& c, u32 file, u32 destination, u32 count);
    // Continues the read after yields that were elided (OSYieldThread would return at once): the continuation entries, inline.
    void drain(Cpu& c);

    bool chunked_read_installed() const noexcept { return static_cast<bool>(chunked_read_); }
    std::optional<u32> chunked_read(Cpu& c, u32 file, u32 destination, u32 chunk) {
        return chunked_read_(c, file, destination, chunk);
    }

private:
    std::function<std::optional<u32>(Cpu&, u32, u32, u32)> chunked_read_;
    // The read state machine's three entries (kRead, its chunk and transfer continuations), for read_sync.
    std::function<void(Cpu&)> read_entry_, read_chunk_, read_transfer_;
    struct Pending {
        u32 sp, resume;
    };
    bool sync_ = false;
    std::optional<Pending> pending_;
    Cpu& cpu_;
    const assets::AssetSystem& assets_;
    NativeFileApi& files_;
    Stats stats_;
};

}  // namespace gekko
