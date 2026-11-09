#pragma once

// CRI's audio stream ring without ARAM: no ARAM block, no ARQ, no DMA (docs/ARAM.md, retirement step 3).
//
// What the game does (main.dol, PAL): CRI's ADX layer streams music, voices and movie audio. Its ring setup
// fn_803D069C takes one 256 KiB ARAM block (ARAlloc 0x40000 at 0x803D06E0) unless the game supplied one (it does not)
// and keeps its base at 0x805C4D24; the ring finalize fn_803D0530 gives it back (ARFree at 0x803D0658, then checks
// the size it got back) - never reached in play. The stream server (0x803CEEE8 <- 0x803C5974 <- 0x803C2460)
// decodes into the game's memory and moves each chunk into the ring with ARQPostRequest (0x803CF0D0 for the ring's
// steady feed, 0x803CF21C for the 0x2000-byte start-up fill), then spins on the chunk's busy word until the request's
// completion callback (0x803CF32C / 0x803CF260, run by ARQ's interrupt service routine at the DSP DMA interrupt)
// clears it. AX voices play the ring by its ARAM addresses (base + offset), which CRI writes into their parameter
// blocks.
//
// Native here: the ring lives in a native arena (SoundArena; sound addresses at kSoundBase, past the GCAX banks'
// span). Two call sites are lowered (extract/lift-sites-native.txt): the ARAlloc returns the arena's sound
// address; the ARFree releases it and reports the size, as ARFree does. The stream server's transfers belong to the
// native ADX output (gekko/native_adx_output.h) since 2026-11-03: copy() puts each chunk into the ring and the
// output completes it at once (until then two lowered ARQPostRequest sites handed the completion to the DSP DMA
// interrupt). The ARQ queue is never touched. The voices' addresses stay the game's.

#include <string>

#include "gekko/sound_arena.h"
#include "gekko/types.h"

namespace gekko {

class Cpu;
class GuestMemory;

class NativeStreamRing {
public:
    NativeStreamRing(Cpu& cpu, GuestMemory& memory) noexcept
        : cpu_(cpu), arena_(memory, kSoundBase, kSoundSpan, kArenaBase, "native stream ring") {}

    void install();

    static constexpr u32 kSoundBase = 0x02000000u;  // past the GCAX banks' span (NativeSoundBanks)
    static constexpr u32 kSoundSpan = 0x01000000u;
    static constexpr u32 kArenaBase = 0x95000000u;  // guest window, after the banks' arena

    const SoundArena& arena() const noexcept { return arena_; }

    // The lowered sites (scaffold::stream_ring_reserve / _release).
    u32 reserve(Cpu& c, u32 bytes);
    u32 release(Cpu& c, u32 size_out);
    // A transfer into the ring: the native ADX output (docs/CRI_SERVERS.md 4c) completes it itself.
    bool copy(u32 source, u32 destination, u32 bytes, std::string& why);
    // A savestate made before ARAM was retired: its ring [base, base + bytes) (ARAM's bytes in `image`) taken over
    // at the same addresses (docs/ARAM.md "Old savestates").
    bool adopt(u32 base, u32 bytes, const u8* image, std::string& why) { return arena_.adopt(base, bytes, image, why); }
    SoundArena& arena() noexcept { return arena_; }

    struct Stats {
        u64 reserves = 0, releases = 0;
        u64 posts = 0;
        u64 post_bytes = 0;
        u32 reserved = 0;
        u32 committed = 0;
    };
    const Stats& stats() const noexcept { return stats_; }

private:
    void stop(Cpu& c, const std::string& why);

    Cpu& cpu_;
    SoundArena arena_;
    Stats stats_;
};

}  // namespace gekko
