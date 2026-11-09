#pragma once

// The AX ADPCM decoder of System::run_ax_mix: nibble address arithmetic and the continuous per-voice decoder,
// over plain values (a sound store's bytes, the coefficients, the voice's cursor). Moved here from system.cpp so the
// regression tests can compare it with the former per-sample loop.

#include "gekko/system.h"
#include "gekko/types.h"

#include <algorithm>
#include <cstddef>

namespace gekko {

// ---- ADPCM address arithmetic and the continuous decoder ------------------
//
// Original guest behaviour: the AX microcode addresses an ADPCM voice in
// NIBBLES, and a 16-nibble frame is one header byte (2 nibbles: coefficient
// pair in the top nibble, shift in the bottom) followed by 14 sample nibbles.
// Evidence: MISSING_UI_AND_VIDEO_PLAYBACK_REPAIR.md s5.4; the 16/14
// relationship is DSP microcode behaviour and is not provable from this
// image. So sample s of frame f lives at nibble f*16 + 2 + s, and a nibble
// address maps onto the decoded sample run as frame*14 + max(0, within-2):
// the header nibbles alias the frame's first sample.
//
// Native implementation: the resampler steps in SOURCE SAMPLES (the 16.16
// ratio is samples per output sample), so positions are converted to sample
// indices here rather than stepping the nibble address by a sample count
// (D07-F2: that played each frame's first sample three times and skipped
// ~12.5% of every span).
constexpr u32 kAdpcmNibblesPerFrame = 16u;
constexpr u32 kAdpcmSamplesPerFrame = 14u;
constexpr u32 kAdpcmHeaderNibbles = 2u;

inline u64 adpcm_nibble_to_sample(u64 nibble) noexcept {
    const u64 frame = nibble / kAdpcmNibblesPerFrame;
    const u64 within = nibble % kAdpcmNibblesPerFrame;
    return frame * kAdpcmSamplesPerFrame +
           (within < kAdpcmHeaderNibbles ? 0u : within - kAdpcmHeaderNibbles);
}

// Everything the decoder needs from the parameter block, read once per
// voice-frame on the guest thread. Loop context: AXPBADPCMLOOP at pb+180
// (pred_scale), pb+182 (yn1), pb+184 (yn2). Evidence: fn_80386474 syncs
// exactly that three-halfword group at +0xB4/+0xB6/+0xB8 under its own sync
// bit (0x100000), between the SRC group ending at +0xB2 and the LPF group
// (recomp_012.cpp, fn_80386474_body). The real DSP reloads the predictor
// history from it when a looping voice wraps; the 2026-08 mixer carried the
// running history across the wrap instead (D07-F3).
struct AdpcmDecodeContext {
    const u8* samples = nullptr;
    u32 sample_mask = 0;
    const s16* coefficients = nullptr;  // 8 pairs
    u32 end_address = 0;                // inclusive nibble address
    u32 loop_address = 0;
    bool looping = false;
    u32 loop_pred_scale = 0;
    s32 loop_yn1 = 0;
    s32 loop_yn2 = 0;
};

struct AdpcmDecodeStats {
    u64 samples = 0;
    u64 loop_context_reloads = 0;
};

// One sample, every case: a frame header, the end of a one-shot (silence, no advance) and a loop's wrap with its
// context reload. The predictor arithmetic is the same expression the 2026-08 mixer used.
inline s32 adpcm_decode_one(AxAdpcmVoiceCursor& cursor, const AdpcmDecodeContext& ctx,
                            AdpcmDecodeStats& stats) noexcept {
    s32 value = 0;
    // Bounded: every pass either consumes a header, decodes a nibble, or
    // wraps; a window where the loop lands inside its own header would
    // otherwise spin (loop == end == a header nibble).
    for (u32 pass = 0; pass < 4u; ++pass) {
        if (cursor.nibble > ctx.end_address) {
            if (!ctx.looping) {
                break;  // past the end of a one-shot: silence, no advance
            }
            cursor.nibble = ctx.loop_address;
            cursor.predictor = (ctx.loop_pred_scale >> 4) & 7u;
            cursor.scale = ctx.loop_pred_scale & 0xfu;
            cursor.yn1 = ctx.loop_yn1;
            cursor.yn2 = ctx.loop_yn2;
            ++stats.loop_context_reloads;
        }
        const u32 within = cursor.nibble & (kAdpcmNibblesPerFrame - 1u);
        if (within < kAdpcmHeaderNibbles) {
            const u8 header = ctx.samples[(cursor.nibble / 2u) & ctx.sample_mask];
            cursor.predictor = (header >> 4) & 7u;
            cursor.scale = header & 0xfu;
            cursor.nibble += kAdpcmHeaderNibbles - within;
            continue;
        }
        const u8 byte = ctx.samples[(cursor.nibble / 2u) & ctx.sample_mask];
        const s32 raw = ((cursor.nibble & 1u) == 0u) ? (byte >> 4) : (byte & 0xfu);
        const s32 nibble_value = (raw ^ 8) - 8;  // sign-extend 4 bits
        const s32 predicted = ctx.coefficients[cursor.predictor * 2u] * cursor.yn1 +
                              ctx.coefficients[cursor.predictor * 2u + 1u] * cursor.yn2;
        value = ((nibble_value * (1 << cursor.scale)) * 2048 + 1024 + predicted) >> 11;
        if (value > 32767) value = 32767;
        if (value < -32768) value = -32768;
        cursor.yn2 = cursor.yn1;
        cursor.yn1 = value;
        ++cursor.nibble;
        break;
    }
    return value;
}

// Decodes `count` samples at the cursor into the ring and advances it. The
// end/loop handling is inside the decoder, so the ring is the voice's
// playback stream in sample units: a looping voice wraps in place and a
// finished one-shot reads as zeros without the cursor moving.
//
// Between a frame's header and the frame's (or the sound's) end every sample takes adpcm_decode_one's decode branch
// with the frame's predictor and scale, so such a run is decoded without the per-sample header, end and loop tests;
// the samples at those boundaries go through adpcm_decode_one itself.
inline void adpcm_decode_forward(AxAdpcmVoiceCursor& cursor, s16* ring, hw::AudioFrameIndex64 ring_mask,
                                 const AdpcmDecodeContext& ctx, hw::AudioFrameIndex64 count,
                                 AdpcmDecodeStats& stats) noexcept {
    u64 produced = 0;
    while (produced < count) {
        const u32 within = cursor.nibble & (kAdpcmNibblesPerFrame - 1u);
        if (within >= kAdpcmHeaderNibbles && cursor.nibble <= ctx.end_address) {
            u64 run = u64(kAdpcmNibblesPerFrame - within);
            run = std::min<u64>(run, u64(ctx.end_address) - cursor.nibble + 1u);
            run = std::min<u64>(run, count - produced);
            const s32 c0 = ctx.coefficients[cursor.predictor * 2u];
            const s32 c1 = ctx.coefficients[cursor.predictor * 2u + 1u];
            const s32 gain = 1 << cursor.scale;
            u32 nibble = cursor.nibble;
            s32 yn1 = cursor.yn1, yn2 = cursor.yn2;
            hw::AudioFrameIndex64 decoded = cursor.decoded;
            for (u64 k = 0; k < run; ++k) {
                const u8 byte = ctx.samples[(nibble / 2u) & ctx.sample_mask];
                const s32 raw = ((nibble & 1u) == 0u) ? (byte >> 4) : (byte & 0xfu);
                const s32 nibble_value = (raw ^ 8) - 8;
                s32 value = ((nibble_value * gain) * 2048 + 1024 + (c0 * yn1 + c1 * yn2)) >> 11;
                if (value > 32767) value = 32767;
                if (value < -32768) value = -32768;
                yn2 = yn1;
                yn1 = value;
                ++nibble;
                ring[static_cast<std::size_t>(decoded & ring_mask)] = static_cast<s16>(value);
                ++decoded;
            }
            cursor.nibble = nibble;
            cursor.yn1 = yn1;
            cursor.yn2 = yn2;
            cursor.decoded = decoded;
            stats.samples += run;
            produced += run;
            continue;
        }
        const s32 value = adpcm_decode_one(cursor, ctx, stats);
        ring[static_cast<std::size_t>(cursor.decoded & ring_mask)] = static_cast<s16>(value);
        ++cursor.decoded;
        ++stats.samples;
        ++produced;
    }
}

}  // namespace gekko
