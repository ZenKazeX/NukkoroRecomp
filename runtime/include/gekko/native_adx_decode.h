#pragma once

// The CRI ADX stereo 4-bit block decoder 0x803C5B2C (main.dol, PAL GUPP8P), owned natively (docs/agents/opt-adx.md).
//
// Identified from the words (the reference's source naming agrees): it decodes `n` samples per channel of a two-channel ADX
// stream whose blocks are 18 bytes per channel, channel 0 then channel 1 (a 0x24-byte pair), 4-bit ADPCM with two-tap
// prediction, into two separate 16-bit PCM buffers, with the scale word optionally masked by a running key.
// Called by the wrapper 0x803C5AC8 when the word at 0x805B8FB8 is zero (the other wrapper branch, 0x803C5D8C, is not owned).
//
//   r3 = source (block pair 0), r4 = n (samples per channel; blocks = (n + (n < 0)) / 2 pairs are processed, one pair
//   yields 32 samples per channel), r5 = PCM out channel 0, r6 = channel 0 history {s16 h1, s16 h2},
//   r7 = PCM out channel 1, r8 = channel 1 history, r9 / r10 = the two prediction coefficients (signed 16-bit, 12-bit shift),
//   stack (caller's frame): +8 key pointer (s16 key), +0xE s16 key multiplier, +0x12 s16 key addend.
//
// Per block pair: scale word of channel 0 (s16 at +0); if bit 0x8000 is set the decode stops (returns 2 * pairs done, the
// histories are NOT stored). Otherwise key = (key * mult + add) & 0x7FFF is stored, the same check and key step are done for
// channel 1 (scale at +0x12; its xor uses the key the channel 0 step stored), and scale = ((word ^ key_before_step) & 0x1FFF) + 1.
// Then 16 bytes per channel, high nibble first: sample = clamp16(nibble * scale + ((c1 * h1 + c2 * h2) >> 12)), the low
// nibble taken from the 16-entry word table at 0x80516B18 (the sign-extended nibble) as the original reads it. Results and
// histories are stored as 16-bit halfwords. Normal return: r3 = n.
//
// What a call leaves, as the original's: the PCM halfwords, the histories, the key halfword, r3, and CTR (0 once a pair was
// decoded). NOT kept: the volatile registers r0, r4..r12, cr0, and the original's stack frame below the caller's SP (the dead
// stmw/lmw save area of r19..r31). Everything is read and written in the original's order, so overlapping buffers behave
// as the original.

#include "gekko/types.h"

#include <cstddef>

namespace gekko {
class Cpu;
}

namespace gekko::adx_decode {

constexpr u32 kDecodeSte4 = 0x803C5B2Cu;
constexpr u32 kNibbleTable = 0x80516B18u;

void serve_decode_ste4(Cpu& cpu);

struct Owners {
    struct Address {
        u32 address;
        const char* name;
    };
    static const Address* addresses(std::size_t& count) noexcept {
        static constexpr Address kAddresses[] = {{kDecodeSte4, "ADX stereo 4-bit block decoder"}};
        count = 1;
        return kAddresses;
    }
};

void install(Cpu& cpu);

}  // namespace gekko::adx_decode
