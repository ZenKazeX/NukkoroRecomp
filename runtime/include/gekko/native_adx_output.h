#pragma once

// CRI's ADX output to the AX voices ("rna" handles of ADXGC) as native C++20: reconstruction step 2,
// docs/CRI_SERVERS.md section 4c.
//
// The original (PAL DOL): 0x803CEEB4 runs every used handle of the table at 0x805C3E4C (16 x 0xEC; byte +0 = used)
// through 0x803CEF10, called each vsync server pass by 0x803C5964. Per handle (flags byte +1, signed channel count
// byte +3; per-channel words at +0x08 voice, +0x18 the voice's ring base, +0x28 transfer owner, +0x30 input joint
// (decoded PCM), +0x38 output joint (the AX ring), chunks at +0x40 / +0x50 (stride 8), busy words +0x60 / +0x70,
// transfer records +0xA8 (stride 0x20)):
//   flag 2 -> 0x803CF408 "consume": the voice's play position (the last channel's AX parameter block word +0x1B2,
//     less its ring base) into +0xE8 and a 32-entry history (0x805C2D88 index, 0x805C2D8C), then the samples played
//     since the cursor +4 (modulo 0x1000, rounded down to 0x800) leave every output joint (GetChunk data, PutChunk
//     free) and the cursor advances.
//   flag 1 -> play: per channel with a voice and no transfer in flight, a free chunk of the output (<= 0x2000) and as
//     much decoded input, both cut to their common length rounded down to 32 bytes (SJ_SplitChunk, the rest
//     UngetChunk'ed); nothing to move ends the whole call; the two lengths differing is a `b .` hang. Then the chunk
//     pair is recorded, +0x68 = bytes / 2, busy = 1, and ARQPostRequest copies input -> output; the server spins
//     until the completion 0x803CF32C (input chunk PutChunk free, output chunk PutChunk data, busy = 0, at the last
//     channel +0x6C += +0x68) has run in the DSP DMA interrupt.
//   else flag 2 and +0x7C < +0x20 -> silence: the same with the zero buffer *(0x805C2D7C) as the source, busy words
//     +0x70, +0x78 = bytes / 2, completion 0x803CF260 (output chunk PutChunk data, at the last channel +0x7C += +0x78).
//
// Native: each transfer is the copy into the stream ring's arena (NativeStreamRing::copy) followed at once by its
// completion's work - the wait for an interrupt disappears, so the vsync server no longer needs a thread context to
// spin in. The transfer record is filled as ARQPostRequest fills it. The joints are called through NativeSj's direct
// interface; a joint that is neither SJMEM nor SJRBF, or one whose error callback is guest code, stops the run (the
// game's ADX output only creates SJRBF joints). The original's two `b .` hangs (a position outside the ring, unequal
// chunk lengths) stop the run with the reason. DCFlushRange has no effect here and is not called. The handles stay
// in guest memory (the ADX layer reads and writes them).

#include <cstddef>
#include <functional>
#include <string>
#include <utility>

#include "gekko/types.h"

namespace gekko {

class Cpu;
class NativeSj;

class NativeAdxOutput {
public:
    // The transfer: `bytes` from guest `source` to the ring address `destination` (NativeStreamRing::copy in the
    // runtime; a guest-memory copy in the oracle test). False with a reason when it cannot be made.
    using Copy = std::function<bool(Cpu&, u32 source, u32 destination, u32 bytes, std::string& why)>;
    NativeAdxOutput(Cpu& cpu, NativeSj& sj, Copy copy) : cpu_(cpu), sj_(sj), copy_(std::move(copy)) {}
    void install();

    struct Stats {
        u64 passes = 0, handles = 0, consumes = 0, consumed_samples = 0, transfers = 0, transfer_bytes = 0,
            silence_transfers = 0, silence_bytes = 0;
    };
    const Stats& stats() const noexcept { return stats_; }

    struct Address {
        u32 address;
        const char* name;
        bool continuation;
    };
    static const Address* addresses(std::size_t& count) noexcept;

private:
    void exec(Cpu& c, u32 handle);
    void consume(Cpu& c, u32 handle);
    void complete(Cpu& c, u32 request, bool silence);
    bool fail(Cpu& c, const char* what, u32 handle);

    Cpu& cpu_;
    NativeSj& sj_;
    Copy copy_;
    Stats stats_;
};

}  // namespace gekko
