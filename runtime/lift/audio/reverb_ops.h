#pragma once

// The reverb's four-lane single-precision helpers (AVX2 builds), shared by reverb.cpp and the contract test
// (reverb_ops_tests): every lane computes what the scalar helpers of reverb.cpp compute for one channel.
//
// Two families. The REFERENCE ops (flushing operands, double-precision add / multiply, one rounding) are the first
// vector formulation and stay as the oracle. The CLEAN ops take operands that are already flush-fixed (+-0, a normal
// number, an infinity or a NaN: the results of the ops themselves) and do the same arithmetic without the operand
// flush and, for add / multiply, without the double round trip:
//   * flush(flush(x)) == flush(x), so dropping an operand flush on a value that an earlier op produced changes nothing;
//   * a float add of two floats is the correctly rounded sum; (double)a + (double)b rounded once to float is the same
//     number (double rounding is innocuous when the wide format has >= 2p + 2 bits: 53 >= 50), NaN and infinity cases
//     included. The flush decision of the reference is taken on the double sum: the sum of two flush-fixed floats is a
//     multiple of 2^-149, exact in double and in float when it is below the smallest normal, so deciding on the float
//     result gives the same answer;
//   * a float multiply is the correctly rounded product too, but a product just below the smallest normal can round UP
//     to exactly the smallest normal (kept by a float-domain test, flushed by the double-domain test of the reference):
//     the result's magnitude == smallest normal is the only ambiguous case, and those lanes are redecided from the
//     exact double product (mul_boundary).
// Rounding mode: nearest (the host default, as everywhere in the vector reverb).

#if defined(__AVX2__)
#include <immintrin.h>

#include <cmath>
#include <cstdint>

#include "math/matrix.h"

#if defined(_MSC_VER)
#define SHADOW_REVERB_OPS_INLINE __forceinline
#define SHADOW_REVERB_OPS_NOINLINE __declspec(noinline)
#else
#define SHADOW_REVERB_OPS_INLINE inline __attribute__((always_inline))
#define SHADOW_REVERB_OPS_NOINLINE __attribute__((noinline))
#endif

namespace shadow::audio::vec {

using V = __m128;
using Mode = math::SubnormalMode;
constexpr Mode kFlushMode = Mode::Flush;
constexpr float kMinNormalValue = 0x1p-126f;

SHADOW_REVERB_OPS_INLINE V abs_mask() noexcept { return _mm_castsi128_ps(_mm_set1_epi32(0x7fffffff)); }
SHADOW_REVERB_OPS_INLINE V min_normal() noexcept { return _mm_set1_ps(kMinNormalValue); }
// Signed zero where |x| < the smallest normal (the sign bit is kept), x otherwise. NaN compares false: kept.
SHADOW_REVERB_OPS_INLINE V flush(V x) noexcept {
    const V small = _mm_cmplt_ps(_mm_and_ps(x, abs_mask()), min_normal());
    return _mm_andnot_ps(_mm_and_ps(small, abs_mask()), x);
}
template <Mode M>
SHADOW_REVERB_OPS_INLINE V operand(V x) noexcept {
    if constexpr (M == kFlushMode) return flush(x); else return x;
}

// ---- reference ops (operand flush, double precision) ----
// rounded<M>(double) for four lanes: the flush decision is taken on the double, as in the scalar helper.
template <Mode M>
SHADOW_REVERB_OPS_INLINE V rounded(__m256d value) noexcept {
    const V narrow = _mm256_cvtpd_ps(value);
    if constexpr (M != kFlushMode) return narrow;
    const __m256d small = _mm256_cmp_pd(_mm256_andnot_pd(_mm256_set1_pd(-0.0), value),
                                        _mm256_set1_pd(double(kMinNormalValue)), _CMP_LT_OQ);
    // The four 64-bit masks to four 32-bit masks (dwords 0, 2, 4, 6 of the 256-bit mask).
    const V mask = _mm_castsi128_ps(_mm256_castsi256_si128(_mm256_permutevar8x32_epi32(
        _mm256_castpd_si256(small), _mm256_setr_epi32(0, 2, 4, 6, 0, 2, 4, 6))));
    return _mm_andnot_ps(_mm_and_ps(mask, abs_mask()), narrow);
}
template <Mode M>
SHADOW_REVERB_OPS_INLINE V add(V a, V b) noexcept {
    return rounded<M>(_mm256_add_pd(_mm256_cvtps_pd(operand<M>(a)), _mm256_cvtps_pd(operand<M>(b))));
}
template <Mode M>
SHADOW_REVERB_OPS_INLINE V mul(V a, V b) noexcept {
    return rounded<M>(_mm256_mul_pd(_mm256_cvtps_pd(operand<M>(a)), _mm256_cvtps_pd(operand<M>(b))));
}
// A fused result of exactly the smallest normal magnitude needs the exact residual test (rare): the scalar rule,
// for each flagged lane.
SHADOW_REVERB_OPS_NOINLINE inline V boundary(V value, V a, V b, V c, int lanes) noexcept {
    alignas(16) float v[4], x[4], y[4], z[4];
    _mm_store_ps(v, value); _mm_store_ps(x, a); _mm_store_ps(y, b); _mm_store_ps(z, c);
    for (int lane = 0; lane < 4; ++lane) {
        if (!(lanes & (1 << lane))) continue;
        const double product = double(x[lane]) * double(y[lane]);
        const double sum = product + double(z[lane]);
        const double moved = sum - product;
        const double residual = (product - (sum - moved)) + (double(z[lane]) - moved);
        if (std::fabs(sum) < kMinNormalValue || (std::fabs(sum) == kMinNormalValue &&
            (std::signbit(sum) ? residual > 0 : residual < 0))) v[lane] = std::copysign(0.0f, v[lane]);
    }
    return _mm_load_ps(v);
}
// The fused multiply-add of clean operands: the result flush and the exact boundary rule, no operand flush.
template <Mode M>
SHADOW_REVERB_OPS_INLINE V fused_clean(V a, V b, V c) noexcept {
    V value = _mm_fmadd_ps(a, b, c);
    if constexpr (M != kFlushMode) return value;
    const V magnitude = _mm_and_ps(value, abs_mask());
    const int exact = _mm_movemask_ps(_mm_cmpeq_ps(magnitude, min_normal()));
    const V small = _mm_cmplt_ps(magnitude, min_normal());
    value = _mm_andnot_ps(_mm_and_ps(small, abs_mask()), value);
    if (exact != 0) value = boundary(value, a, b, c, exact);
    return value;
}
template <Mode M>
SHADOW_REVERB_OPS_INLINE V fused(V a, V b, V c) noexcept {
    return fused_clean<M>(operand<M>(a), operand<M>(b), operand<M>(c));
}

// ---- clean ops (operands flush-fixed already) ----
template <Mode M>
SHADOW_REVERB_OPS_INLINE V add_clean(V a, V b) noexcept {
    const V sum = _mm_add_ps(a, b);
    if constexpr (M != kFlushMode) return sum; else return flush(sum);
}
// A product of magnitude == the smallest normal: redecided from the exact double product (flush iff it is below).
SHADOW_REVERB_OPS_NOINLINE inline V mul_boundary(V value, V a, V b, int lanes) noexcept {
    alignas(16) float v[4], x[4], y[4];
    _mm_store_ps(v, value); _mm_store_ps(x, a); _mm_store_ps(y, b);
    for (int lane = 0; lane < 4; ++lane) {
        if (!(lanes & (1 << lane))) continue;
        if (std::fabs(double(x[lane]) * double(y[lane])) < double(kMinNormalValue)) v[lane] = std::copysign(0.0f, v[lane]);
    }
    return _mm_load_ps(v);
}
template <Mode M>
SHADOW_REVERB_OPS_INLINE V mul_clean(V a, V b) noexcept {
    V product = _mm_mul_ps(a, b);
    if constexpr (M != kFlushMode) return product;
    const V magnitude = _mm_and_ps(product, abs_mask());
    const int exact = _mm_movemask_ps(_mm_cmpeq_ps(magnitude, min_normal()));
    const V small = _mm_cmplt_ps(magnitude, min_normal());
    product = _mm_andnot_ps(_mm_and_ps(small, abs_mask()), product);
    if (exact != 0) product = mul_boundary(product, a, b, exact);
    return product;
}

// ---- speculation (flush mode) ----
// Lanes whose value is not zero and not above the smallest normal's magnitude: exactly the values for which the flush /
// boundary rule of the clean ops can act. A chain of ops none of whose results is suspect gives, unflushed (plain fmadd /
// add / multiply), the same bits as the same chain through the clean ops: flush(x) == x for every result.
SHADOW_REVERB_OPS_INLINE V suspect(V x) noexcept {
    const V magnitude = _mm_and_ps(x, abs_mask());
    return _mm_and_ps(_mm_cmple_ps(magnitude, min_normal()), _mm_cmpneq_ps(magnitude, _mm_setzero_ps()));
}

template <Mode M>
SHADOW_REVERB_OPS_INLINE __m128i truncate(V x) noexcept {
    // cvtt gives INT32_MIN for NaN and for every out-of-range value, the flush mode's authored result; the standard
    // mode saturates positive overflow instead.
    const __m128i converted = _mm_cvttps_epi32(x);
    if constexpr (M == kFlushMode) return converted;
    const V overflow = _mm_cmpge_ps(x, _mm_set1_ps(0x1p31f));
    return _mm_blendv_epi8(converted, _mm_set1_epi32(INT32_MAX), _mm_castps_si128(overflow));
}

}  // namespace shadow::audio::vec
#endif
