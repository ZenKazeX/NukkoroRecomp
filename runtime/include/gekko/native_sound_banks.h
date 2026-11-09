#pragma once

// The GCAX sound banks without ARAM: no ARAM block, no ARQ, no DMA, no wait (docs/ARAM.md, retirement step 2).
//
// What the game does (main.dol, PAL; docs/GCAX_SOUND_MAP.md sections 1, 2 and 4): the GCAX init 0x8043F744 takes
// one 8 MiB ARAM block (ARAlloc at 0x8043F7A4, its size the caller's argument), uploads a 0x500-byte zero block at
// its start (0x8043BBBC at 0x8043F818: an ARQ request, then a wait on the request's busy byte), and carves the rest
// into 16 bank slots with gcaxNewAramBank 0x8043C1FC from a size table the game hard-codes (0x8043C2AC). Each bank
// load (gcaxLoadMPB 0x8043C0AC) uploads the .mlt's wave data into its slot with gcaxAramDma 0x8043BACC (an ARQ
// request the game waits for: the only gcaxSetLoad call, 0x801F2D50, passes callback -1, which gcaxLoadMPB turns
// into no callback and a synchronous load) and keeps the program data in main memory. At every note-on GCAX writes
// the voice's addresses as slot base + the wave's offset (nibbles for ADPCM, samples for PCM16, bytes for PCM8),
// and the DSP reads the samples there.
//
// Native here: the banks live in a native arena (a region of the guest window at kArenaBase, reserved to the game's
// own request; pages are committed as the banks' bytes land in them). The game sees a sound address space at
// kSoundBase, past the 16 MiB of ARAM, so the stream ring (still in ARAM until its own step) and the banks never
// share an address; the AX voice decode reads a voice whose addresses lie there straight from the arena (source()).
// The init stays the game's code with two call sites lowered (extract/lift-sites.txt): its ARAlloc returns the
// arena's sound address, its zero-block upload is a plain copy. gcaxAramDma is owned (a plain copy). Everything
// else - the slot allocator, the bank loader, the program data, the note-on address arithmetic - stays the game's.

#include <cstddef>
#include <string>

#include "gekko/sound_arena.h"
#include "gekko/types.h"

namespace gekko {

class Cpu;
class GuestMemory;

class NativeSoundBanks {
public:
    NativeSoundBanks(Cpu& cpu, GuestMemory& memory) noexcept
        : cpu_(cpu), arena_(memory, kSoundBase, kSoundSpan, kArenaBase, "native sound banks") {}

    void install();

    struct Address {
        u32 address;
        const char* name;
    };
    static const Address* addresses(std::size_t& count) noexcept;

    static constexpr u32 kSoundBase = 0x01000000u;  // the first address past the 16 MiB of ARAM (ARInit's size)
    static constexpr u32 kSoundSpan = 0x01000000u;  // the banks' sound addresses: [kSoundBase, + 16 MiB)
    static constexpr u32 kArenaBase = 0x94000000u;  // guest window, after the motion store's arena

    const SoundArena& arena() const noexcept { return arena_; }

    // The lowered sites (scaffold::sound_banks_reserve, _zero_block, _release).
    u32 reserve(Cpu& c, u32 bytes);
    u32 release(Cpu& c, u32 size_out);
    // A savestate made before ARAM was retired: its GCAX block [base, base + bytes) (ARAM's bytes in `image`) taken
    // over at the same addresses (docs/ARAM.md "Old savestates").
    bool adopt(u32 base, u32 bytes, const u8* image, std::string& why) { return arena_.adopt(base, bytes, image, why); }
    SoundArena& arena() noexcept { return arena_; }
    u32 copy_in(Cpu& c, u32 from, u32 to, u32 bytes, u32 callback, u32 argument, bool counted);

    struct Stats {
        u64 reserves = 0, releases = 0;
        u64 uploads = 0;          // bank wave data copied in
        u64 upload_bytes = 0;
        u32 reserved = 0;         // the arena's reserve (the game's request)
        u32 committed = 0;        // the arena's committed bytes
        u64 interrupt_points = 0; // uploads that ended where the original's wait took a due interrupt
    };
    const Stats& stats() const noexcept { return stats_; }

private:
    void upload(Cpu& c);
    void take_interrupts(Cpu& c);
    void stop(Cpu& c, const std::string& why);

    Cpu& cpu_;
    SoundArena arena_;
    Stats stats_;
};

}  // namespace gekko
