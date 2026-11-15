#pragma once

// The microcode's linear SRC over a flat window (native_audio_mixer.cpp render_voice): 160 outputs of
//   t = frac + (i + 1) * ratio;  k = t >> 16;  f = t & 0xFFFF;  s0 = w[k + 2];  d = sat16(w[k + 3] - s0);
//   y = sat16(s0 + round16(d * f))   with   round16(p) = (p + (bit 16 of p ? 0x8000 : 0x7FFF)) >> 16
// (IRAM 0x0753..0x0813). `window` is the flat window (the voice's 4 history samples, the source, then silence); the
// caller guarantees frac + 160 * ratio < 2^27, so k + 3 stays inside the window it built. The scalar function is the
// reference; the AVX2 one computes eight outputs per step in 32-bit lanes and is bit-identical: |d| <= 32768 and
// f <= 65535 give |d * f| < 2^31, and adding the rounding constant (<= 0x8000) cannot leave the range either. One
// 32-bit gather at window[k + 2] reads the two neighbouring samples (s0 the low half, s1 the high half).

#include <algorithm>
#include <cstdint>

#if defined(__AVX2__)
#include <immintrin.h>
#endif

namespace shadow::audio {

inline std::int32_t src_sat16(std::int64_t v) noexcept {
    return v > 32767 ? 32767 : (v < -32768 ? -32768 : static_cast<std::int32_t>(v));
}

inline void linear_src_scalar(const std::int16_t* window, std::uint32_t frac, std::uint32_t ratio, std::int32_t* y,
                              std::uint32_t outputs) noexcept {
    for (std::uint32_t i = 0; i < outputs; ++i) {
        const std::uint64_t t = static_cast<std::uint64_t>(frac) + static_cast<std::uint64_t>(i + 1u) * ratio;
        const std::size_t k = static_cast<std::size_t>(t >> 16);
        const std::int64_t f = static_cast<std::int64_t>(t & 0xFFFFu);
        const std::int32_t s0 = window[k + 2u];
        const std::int32_t d = src_sat16(static_cast<std::int64_t>(window[k + 3u]) - s0);
        const std::int64_t p = static_cast<std::int64_t>(d) * f;
        y[i] = src_sat16(s0 + ((p + ((p & 0x10000) ? 0x8000 : 0x7FFF)) >> 16));
    }
}

#if defined(__AVX2__)
// `outputs` must be a multiple of 8.
inline void linear_src_avx2(const std::int16_t* window, std::uint32_t frac, std::uint32_t ratio, std::int32_t* y,
                            std::uint32_t outputs) noexcept {
    const __m256i lane_steps = _mm256_setr_epi32(1, 2, 3, 4, 5, 6, 7, 8);
    const __m256i ratio_v = _mm256_set1_epi32(static_cast<int>(ratio));
    __m256i phase = _mm256_add_epi32(_mm256_set1_epi32(static_cast<int>(frac)), _mm256_mullo_epi32(lane_steps, ratio_v));
    const __m256i phase_step = _mm256_slli_epi32(ratio_v, 3);
    const __m256i fraction_mask = _mm256_set1_epi32(0xFFFF);
    const __m256i one = _mm256_set1_epi32(1);
    const __m256i round_base = _mm256_set1_epi32(0x7FFF);
    const __m256i sat_low = _mm256_set1_epi32(-32768);
    const __m256i sat_high = _mm256_set1_epi32(32767);
    const int* const base = reinterpret_cast<const int*>(window + 2);
    for (std::uint32_t i = 0; i < outputs; i += 8) {
        const __m256i k = _mm256_srli_epi32(phase, 16);
        const __m256i f = _mm256_and_si256(phase, fraction_mask);
        const __m256i pair = _mm256_i32gather_epi32(base, k, 2);
        const __m256i s0 = _mm256_srai_epi32(_mm256_slli_epi32(pair, 16), 16);
        const __m256i s1 = _mm256_srai_epi32(pair, 16);
        const __m256i d = _mm256_min_epi32(sat_high, _mm256_max_epi32(sat_low, _mm256_sub_epi32(s1, s0)));
        const __m256i p = _mm256_mullo_epi32(d, f);
        const __m256i half = _mm256_add_epi32(round_base, _mm256_and_si256(_mm256_srli_epi32(p, 16), one));
        const __m256i rounded = _mm256_srai_epi32(_mm256_add_epi32(p, half), 16);
        const __m256i value = _mm256_min_epi32(sat_high, _mm256_max_epi32(sat_low, _mm256_add_epi32(s0, rounded)));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(y + i), value);
        phase = _mm256_add_epi32(phase, phase_step);
    }
}
#endif

}  // namespace shadow::audio
