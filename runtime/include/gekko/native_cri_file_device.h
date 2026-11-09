#pragma once

// CRI's "DVD" file device (gcCi, the cvFs device ADXF, Sofdec and the ADX stream server read through), native over
// the asset namespace. docs/DISC_SYSTEM_MAP.md, "Cut 3".
//
// The device is the interface table at 0x80569FC8, which gcCi_GetInterface 0x803CEA9C returns to cvFs; every entry
// is reached through that table. Its state is the game's own, in guest memory, and stays there with the same
// layout, so every field the CRI layers above can see, and every savestate, is unchanged:
//
//   0x805C1BB8  +0x00 the last command-block status   +0x04 (byte) the device state the last change left
//               +0x08 the cancel-in-progress word     +0x0C / +0x10 the error callback's object / function
//   0x805C1BCC  40 handles of 0x64 bytes:
//               +0x00 used   +0x02 state (0 idle, 1 done, 2 reading, 3 error)   +0x08 buffer
//               +0x0C the handle's last status   +0x10 sector length (0x800)   +0x14 file length (<= 0x7FFFFFFF)
//               +0x18 sectors   +0x1C position (sectors)   +0x20 bytes transferred   +0x24 sectors requested
//               +0x58 the file's asset token   +0x5C the file's length (the file identity reads use)
//   0x805C2B70  the path prefix (256 bytes; only GetInterface writes it, with zeros)
//
// The original kept a DVDFileInfo inside each handle (+0x28..+0x63) and drove the DVD SDK: DVDOpen per open and
// per size query, DVDReadAsyncPrio / DVDReadPrio per request, DVDGetCommandBlockStatus polled by the per-frame
// server, DVDCancel plus a 2-second OSGetTime spin per stop or close, and cache maintenance around every
// transfer. Here a request copies the bytes from the asset at once (as the native DVD read already did), the
// handle reads "reading" until the next server pass, exactly as before, and then completes with the original's
// bookkeeping (transfer count, position, the zero fill past the end of the file). Only +0x58 / +0x5C of the old
// DVDFileInfo are written; the SDK's other fields are gone.
//
// Refused (the machine stops, naming it) rather than reproduced: a read the host cannot deliver (the original
// marked the handle in error and, on a later stop or close, spun two seconds and reported a timeout), and a path
// that would overrun the original's 0x104-byte path buffer.

#include <cstddef>

#include "gekko/types.h"

namespace gekko {

class Cpu;
class NativeFileApi;
namespace assets {
class AssetSystem;
}

class NativeCriFileDevice final {
public:
    NativeCriFileDevice(Cpu& cpu, const assets::AssetSystem& assets, NativeFileApi& files) noexcept
        : cpu_(cpu), assets_(assets), files_(files) {}

    // Installs the owners. After the file API, before the guest boots.
    void install();

    struct Stats {
        u64 opens = 0;
        u64 open_misses = 0;
        u64 requests = 0;      // reads issued
        u64 completions = 0;   // reads the server completed
        u64 bytes = 0;
        u64 stops = 0;
        u64 errors = 0;        // error-callback reports
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
