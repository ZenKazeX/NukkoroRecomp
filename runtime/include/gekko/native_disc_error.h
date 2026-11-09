#pragma once

// The game's disc-error screen module (0x8032D138-0x8032E284), with no drive underneath.
// docs/DISC_SYSTEM_MAP.md, "Cut 1".
//
// On the console the game watched the drive every frame (the drive check 0x8032D444, called from game main at
// 0x800511F0) and, on a drive error, took over the frame with the "please close the Disc Cover" screen
// 0x8032D4CC: SE and rumble stopped, the frame buffer saved to an ARAM backup (0x8032E200 reserved it) and
// restored after. RenderWare's file system reported its own drive errors through a callback the game installed
// (0x8032D3D4, stored at the RtGCNFS device's +0x5C), and the loaders asked 0x8032D138 whether the screen held
// them (it returned the screen's thread queue while it was up).
//
// The native file API has no drive: DVDGetDriveStatus is 0 on every call (native_file_api.cpp). Then
//   - the drive check only reserves the backup (read by nothing but the screen) and stores 0 to the word at
//     0x5760(r13), which no reachable code makes non-zero: the call at 0x800511F0 is lowered to nothing
//     (extract/lift-sites.txt), and so is the module init's call at 0x8005126C (it fills the thread word and
//     queues only the screen's paths read);
//   - the screen, its backup and its text drawing lose every caller and the generator prunes them.
// What stays reachable is owned here:
//
//   0x8032D138 the loaders' query (0x80043D70, 0x80045694): 0, the screen never holds a load. The original
//              returns the queue only while the word at 0x5758(r13) is set, and only the screen sets it.
//   0x8032D3D4 RtGCNFS's drive-error callback (r3 = the RtGCNFS drive state): 1. With drive status 0 RtGCNFS
//              passes 7 (its "no error" case) or does not call it. States 2..6 are drive errors the native
//              drive cannot produce; one is refused (the machine stops, naming it) rather than ignored.
//
// No state.

#include <cstddef>

#include "gekko/types.h"

namespace gekko {

class Cpu;

class NativeDiscError final {
public:
    explicit NativeDiscError(Cpu& cpu) noexcept : cpu_(cpu) {}

    // Installs the owners. Before the guest boots.
    void install();

    // Every guest address the generator must route to an owner here (extract/native-owners.txt).
    struct Address {
        u32 address;
        const char* name;
        bool continuation;
    };
    static const Address* addresses(std::size_t& count) noexcept;

private:
    Cpu& cpu_;
};

}  // namespace gekko
