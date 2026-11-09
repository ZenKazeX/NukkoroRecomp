#pragma once

// The game's file API, served natively.
//
// The game, RenderWare and CRI were linked against the Nintendo DVD SDK, so the
// file API they call is that SDK's exported entry points: DVDOpen, DVDClose,
// DVDReadAsyncPrio, DVDGetCommandBlockStatus, ... Every one of them that any
// surviving guest code can reach is owned here (extract/native-owners.txt): the
// generator emits only a thunk, the original body - path walk, command queue,
// drive state machine, DI registers, alarms, interrupt handler - is not in the
// image at all, and nothing underneath pretends to be a drive.
//
//     game / RenderWare / CRI  (translated guest code, unchanged)
//        |  the file API's entry points (native owners)
//     NativeFileApi
//        |  path -> entry           AssetNamespace (immutable, indexed, 64-bit)
//        |  entry -> guest token    AssetAddressSpace (the 32-bit handle the
//        |                          guest stores in DVDFileInfo.startAddr)
//        |  token -> bytes          AssetSystem: one copy from a mapped host
//        |                          file straight into guest memory
//     host filesystem
//
// ---------------------------------------------------------------------------
// THE CONTRACT (what the game can observe; docs/native-asset-pipeline-20261005/)
// ---------------------------------------------------------------------------
//
//  * Results: every return value the API defines, the DVDFileInfo / command
//    block / DVDDir / DVDDirEntry fields its callers read, the bytes a read
//    delivers (past end-of-file reads as zero, as both live issuers rely on).
//  * Completion: a read is complete when its entry point returns. The game's
//    callback - DVDReadAsyncPrio's, DVDCancelAsync's, a raw read's - runs
//    before the entry point returns to its caller, with (result, block): the
//    order the port has always delivered and the game's loaders are built on.
//  * Failure: a path that names nothing returns FALSE (the SDK's warning is
//    logged natively); a directory is refused by DVDOpen; a read the host
//    cannot serve completes FATAL (-1) with the callback told -1; a read
//    outside the file stops the machine where the SDK would OSPanic.
//
// What is deliberately NOT reproduced: guest instruction counts or any other
// GameCube timing, the SDK's thread queue and its wake-ups, the command
// queue, the drive states the port can never be in (BUSY, WAITING, cover,
// retry, pause), the DI registers, the fatal-error screen, the FST in guest
// memory. The console reboot path (__OSBootDol's disc boot) has no native
// equivalent and stops the machine at its first file-API call.
//
// Timing: an owner retires no guest instructions. Only the guest code the
// API calls back into (the game's callbacks) retires its own.

#include <cstddef>
#include <string>
#include <vector>

#include "gekko/asset_mount.h"
#include "gekko/types.h"

namespace gekko {

class GuestMemory;
class Cpu;

class NativeFileApi {
public:
    NativeFileApi(Cpu& cpu, GuestMemory& memory, assets::AssetSystem& assets) noexcept
        : cpu_(cpu), memory_(memory), assets_(assets) {}

    // Installs every owner and the callback continuation. Must run after the
    // asset system is mounted and before the guest boots.
    void install();

    // Every address the generator must route here: owner rows first, then the
    // continuation entry inside a row (the source of extract/native-owners.txt).
    struct Address {
        u32 address;
        const char* name;
        bool continuation;
    };
    static const Address* addresses(std::size_t& count) noexcept;

    struct Stats {
        u64 opens = 0;
        u64 open_misses = 0;        // a path that names nothing (the SDK's warning)
        u64 open_directories = 0;   // DVDOpen of a directory (refused)
        u64 closes = 0;
        u64 cancels = 0;
        u64 reads = 0;              // every read entry point
        u64 read_bytes = 0;
        u64 reads_failed = 0;       // the host could not produce the bytes
        u64 reads_refused = 0;      // a malformed request (completes FATAL)
        u64 reads_unmapped = 0;     // a token that names no asset: MUST be zero
        u64 reads_out_of_range = 0; // a request leaving its asset: MUST be zero
        u64 identity_reads = 0;     // DVDReadDiskID / DVDInquiryAsync
        u64 callbacks = 0;          // guest callbacks the API made
        u64 status_queries = 0;     // DVDGetCommandBlockStatus / DVDGetDriveStatus
        u64 anomalies = 0;          // a block in a state the API never produces
        u64 read_ns = 0;            // host time inside the read path
        u64 read_ns_max = 0;
    };
    Stats stats() const noexcept { return stats_; }
    // The first few paths DVDOpen could not resolve, for the exit
    // report (the SDK printed each one as an OSReport warning).
    const std::vector<std::string>& missed_paths() const noexcept { return missed_; }

    // Where a read's bytes come from: an asset (by guest token), or the title
    // block the drive-identity entry points return.
    enum class Source : u8 { Asset = 0, Identity };

    // The one read path. Copies `length` bytes for `token` into guest memory
    // at `address`, writes the completion fields of `block` and, when
    // `callback` is non-zero, calls it back with (result, block) - leaving to
    // the dispatcher with the caller's frame on the guest stack; the
    // continuation then returns `return_value` to the caller. Without a
    // callback the entry point sets r3 itself.
    struct ReadOutcome {
        s32 result = -1;          // bytes delivered, or -1
        bool refused = false;     // a malformed request (completed FATAL)
        bool called_back = false; // a callback is in progress: r3 belongs to it
    };
    ReadOutcome read(Cpu& c, u32 block, u32 address, u32 length, u32 token, u32 callback,
                     u32 command, Source source, u32 return_value);

    // A native device's read (CRI's file device, DISC_SYSTEM_MAP.md cut 3): `length` bytes of the asset at
    // `token` into guest memory at `address`, past the asset's end zero-filled, with the same counters, asset
    // trace and renderer notification as the read path - and no DVD command block. False when the bytes could
    // not be delivered (counted).
    bool device_read(Cpu& c, u32 token, u32 address, u32 length);

    // Told of every delivered read on the reading thread, after the copy: the asset, its path, the file offset of
    // the first byte, the guest address and the length (gekko/prs_ahead.h). Unset: nobody.
    using ReadObserver =
        std::function<void(assets::AssetId id, const std::string& path, u64 offset, u32 address, u64 length)>;
    void set_read_observer(ReadObserver observer) { read_observer_ = std::move(observer); }

private:
    void count_read_time(u64 ns) noexcept;
    bool copy_asset(u32 token, u32 address, u32 length, std::string& resolved, u64& relative);

    Cpu& cpu_;
    GuestMemory& memory_;
    assets::AssetSystem& assets_;
    bool auto_fatal_messaging_ = false;  // DVDSetAutoFatalMessaging's flag (no fatal screen exists)
    bool auto_invalidation_ = true;      // DVDSetAutoInvalidation's flag (the SDK's default)
    Stats stats_;
    std::vector<std::string> missed_;
    ReadObserver read_observer_;
};

}  // namespace gekko
