#pragma once

// Fast MSL sin / cos / sincos: the SAME double results as shadow::math::msl::sin / cos (runtime/lift/math/msl.cpp,
// proven there against the original words of sin, cos, __ieee754_rem_pio2 and the two kernels:
// docs/NATIVE_MSL_MATH.md), for every input bit pattern.
//
// Header only, inline, no tables in memory except the 32-word npio2_hw. Needs FMA3 (the game image is /arch:AVX2 and
// the host ISA gate already requires it). Arguments with |x| > 2^19 pi/2 (the __kernel_rem_pio2 path), infinities and
// NaNs are handed to msl::sin / msl::cos, so they are identical by construction.
//
// What is the same as the library, and why (each point is checked by the test, not only argued):
//  * sin is odd and cos is even in the library's own operation order: kernel_sin(-x, -y) = -kernel_sin(x, y) and
//    kernel_cos(-x, -y) = kernel_cos(x, y) (every operation is symmetric under round-to-nearest, and rem_pio2 works on
//    |x| and negates its results). So the sign of x is applied once at the end and the reduction sees |x|.
//  * |x| < 2^-27: fctiwz(x) == 0 is always true there, the library's test collapses to the exponent compare.
//  * kernel_cos(x, 0): x * y is +-0, and z * r (> 0, not tiny) minus +-0 is z * r, so that step is dropped.
//  * n = fctiwz(fma(invpio2, |x|, 0.5)) cannot saturate for |x| <= 2^19 pi/2, so a plain cvttsd2si gives it.
//  * the rare cancellation re-checks of rem_pio2 (2nd / 3rd iteration) are kept verbatim.
// Nothing is reordered or contracted differently: every fused operation of the library is the same fused operation.

#include "math/msl.h"

#include <bit>
#include <cstdint>
#include <immintrin.h>

namespace shadow::recovered::fast {
namespace trig_detail {

using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s32 = std::int32_t;

#if defined(_MSC_VER)
#define SHADOW_FAST_TRIG_INLINE __forceinline
#else
#define SHADOW_FAST_TRIG_INLINE inline __attribute__((always_inline))
#endif

#define SHADOW_FAST_TRIG_UNLIKELY(c) (c)

constexpr double dbl(u64 bits) noexcept { return std::bit_cast<double>(bits); }
SHADOW_FAST_TRIG_INLINE u64 bits_of(double d) noexcept { return std::bit_cast<u64>(d); }

// a * c + b, a * c - b, b - a * c: one rounding each (the library's fmadd / fmsub / -fnmsub).
SHADOW_FAST_TRIG_INLINE double fma_(double a, double c, double b) noexcept {
    return _mm_cvtsd_f64(_mm_fmadd_sd(_mm_set_sd(a), _mm_set_sd(c), _mm_set_sd(b)));
}
SHADOW_FAST_TRIG_INLINE double fms_(double a, double c, double b) noexcept {
    return _mm_cvtsd_f64(_mm_fmsub_sd(_mm_set_sd(a), _mm_set_sd(c), _mm_set_sd(b)));
}
SHADOW_FAST_TRIG_INLINE double fnma_(double a, double c, double b) noexcept {
    return _mm_cvtsd_f64(_mm_fnmadd_sd(_mm_set_sd(a), _mm_set_sd(c), _mm_set_sd(b)));
}
SHADOW_FAST_TRIG_INLINE double xor_sign(double v, u64 mask) noexcept { return std::bit_cast<double>(bits_of(v) ^ mask); }

inline constexpr double kPio2_1 = dbl(0x3FF921FB54400000ull), kPio2_1t = dbl(0x3DD0B4611A626331ull);
inline constexpr double kPio2_2 = dbl(0x3DD0B4611A600000ull), kPio2_2t = dbl(0x3BA3198A2E037073ull);
inline constexpr double kPio2_3 = dbl(0x3BA3198A2E000000ull), kPio2_3t = dbl(0x397B839A252049C1ull);
inline constexpr double kInvPio2 = dbl(0x3FE45F306DC9C883ull);
inline constexpr double kC1 = dbl(0x3FA555555555554Cull), kC2 = dbl(0xBF56C16C16C15177ull),
                        kC3 = dbl(0x3EFA01A019CB1590ull), kC4 = dbl(0xBE927E4F809C52ADull),
                        kC5 = dbl(0x3E21EE9EBDB4B1C4ull), kC6 = dbl(0xBDA8FAE9BE8838D4ull);
inline constexpr double kS1 = dbl(0xBFC5555555555549ull), kS2 = dbl(0x3F8111111110F8A6ull),
                        kS3 = dbl(0xBF2A01A019C161D5ull), kS4 = dbl(0x3EC71DE357B1FE7Dull),
                        kS5 = dbl(0xBE5AE5E68A2B9CEBull), kS6 = dbl(0x3DE5D93A5ACFD57Cull);
inline constexpr u32 kNpio2Hw[32] = {
    0x3FF921FBu, 0x400921FBu, 0x4012D97Cu, 0x401921FBu, 0x401F6A7Au, 0x4022D97Cu, 0x4025FDBBu, 0x402921FBu,
    0x402C463Au, 0x402F6A7Au, 0x4031475Cu, 0x4032D97Cu, 0x40346B9Cu, 0x4035FDBBu, 0x40378FDBu, 0x403921FBu,
    0x403AB41Bu, 0x403C463Au, 0x403DD85Au, 0x403F6A7Au, 0x40407E4Cu, 0x4041475Cu, 0x4042106Cu, 0x4042D97Cu,
    0x4043A28Cu, 0x40446B9Cu, 0x404534ACu, 0x4045FDBBu, 0x4046C6CBu, 0x40478FDBu, 0x404858EBu, 0x404921FBu};

inline constexpr u32 kSmallCut = 0x3FE921FBu;   // |x| <= pi/4 (high word): no reduction
inline constexpr u32 kMediumCut = 0x413921FBu;  // |x| <= 2^19 pi/2 (high word): the library's medium reduction
inline constexpr u64 kSignBit = 0x8000000000000000ull;

SHADOW_FAST_TRIG_INLINE u32 hi_word(double d) noexcept { return u32(bits_of(d) >> 32); }

// kernel_sin(x, y, iy = 1), kernel_sin(x, 0, 0), kernel_cos(x, y). x any sign.
SHADOW_FAST_TRIG_INLINE double kernel_sin1(double x, double y) noexcept {
    if ((hi_word(x) & 0x7FFFFFFFu) < 0x3E400000u) return x;
    const double z = x * x;
    const double r5 = fma_(kS6, z, kS5);
    const double v = z * x;
    const double r4 = fma_(z, r5, kS4);
    double r = fma_(z, r4, kS3);
    r = fma_(z, r, kS2);
    double t = v * r;
    t = fms_(0.5, y, t);
    t = fms_(z, t, y);
    t = fnma_(kS1, v, t);
    return x - t;
}
SHADOW_FAST_TRIG_INLINE double kernel_sin0(double x) noexcept {
    if ((hi_word(x) & 0x7FFFFFFFu) < 0x3E400000u) return x;
    const double z = x * x;
    const double r5 = fma_(kS6, z, kS5);
    const double v = z * x;
    const double r4 = fma_(z, r5, kS4);
    double r = fma_(z, r4, kS3);
    r = fma_(z, r, kS2);
    return fma_(v, fma_(z, r, kS1), x);
}
SHADOW_FAST_TRIG_INLINE double kernel_cos_poly(double z) noexcept {
    const double r5 = fma_(kC6, z, kC5);
    const double r4 = fma_(z, r5, kC4);
    const double r3 = fma_(z, r4, kC3);
    const double r2 = fma_(z, r3, kC2);
    return z * fma_(z, r2, kC1);
}
// y = 0 (the argument was not reduced).
SHADOW_FAST_TRIG_INLINE double kernel_cos0(double x) noexcept {
    const u32 ix = hi_word(x) & 0x7FFFFFFFu;
    if (ix < 0x3E400000u) return 1.0;
    const double z = x * x;
    const double zr = z * kernel_cos_poly(z);  // the library's fmsub(z, r, x * 0) = fmsub(z, r, +-0) = round(z r)
    if (ix < 0x3FD33333u) return 1.0 - fms_(0.5, z, zr);
    const double qx = ix > 0x3FE90000u ? 0.28125 : dbl(u64(ix - 0x00200000u) << 32);
    return (1.0 - qx) - (fms_(0.5, z, qx) - zr);
}
SHADOW_FAST_TRIG_INLINE double select_(bool pick_a, double a, double b) noexcept {
    const __m128d mask = _mm_castsi128_pd(_mm_set1_epi64x(-std::int64_t(pick_a)));
    return _mm_cvtsd_f64(_mm_blendv_pd(_mm_set_sd(b), _mm_set_sd(a), mask));
}
SHADOW_FAST_TRIG_INLINE double kernel_cos1(double x, double y) noexcept {
    const u32 ix = hi_word(x) & 0x7FFFFFFFu;
    if (ix < 0x3E400000u) return 1.0;
    const double z = x * x;
    const double r = kernel_cos_poly(z);
    const double t0 = x * y;
    const double zr = fms_(z, r, t0);
    // |x| < 0.3: 1 - (0.5 z - t).  Otherwise qx = x/4 (0.28125 above 0.78125): (1 - qx) - ((0.5 z - qx) - t).
    // Both are computed and one is selected (the reduced argument's size is unpredictable).
    const double qx = select_(ix > 0x3FE90000u, 0.28125, dbl(u64(ix - 0x00200000u) << 32));
    const double small = 1.0 - fms_(0.5, z, zr);
    const double big = (1.0 - qx) - (fms_(0.5, z, qx) - zr);
    return select_(ix < 0x3FD33333u, small, big);
}

struct Reduced {
    s32 n;  // x = n pi/2 + y0 + y1 for the argument |x| (n >= 1)
    double y0, y1;
};

// __ieee754_rem_pio2 for t = |x| with 0x3FE921FB < high(t) <= 0x413921FB, n and y as the library returns them for +x:
// the library's medium path verbatim. Entered only by the rare cases the inline path below does not cover.
#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
inline Reduced reduce_slow(double t, u32 ix) noexcept {
    if (ix < 0x4002D97Cu) {  // |x| < 3pi/4: n = 1
        double z = t - kPio2_1;
        if (ix != 0x3FF921FBu) {
            const double y0 = z - kPio2_1t;
            return {1, y0, (z - y0) - kPio2_1t};
        }
        z = z - kPio2_2;
        const double y0 = z - kPio2_2t;
        return {1, y0, (z - y0) - kPio2_2t};
    }
    const s32 n = _mm_cvttsd_si32(_mm_set_sd(fma_(kInvPio2, t, 0.5)));
    const double fn = double(n);
    double r = fnma_(kPio2_1, fn, t);
    double w = kPio2_1t * fn;
    double y0 = r - w;
    if (n >= 32 || kNpio2Hw[n - 1] == ix) {  // the library: !(n < 32 && npio2_hw[n - 1] != ix)
        const s32 j = s32(ix >> 20);
        s32 i = j - s32((hi_word(y0) >> 20) & 0x7FFu);
        if (i > 16) {  // 2nd iteration, good to 118 bits
            double tt = r;
            w = kPio2_2 * fn;
            r = tt - w;
            w = fms_(kPio2_2t, fn, (tt - r) - w);
            y0 = r - w;
            i = j - s32((hi_word(y0) >> 20) & 0x7FFu);
            if (i > 49) {  // 3rd iteration, 151 bits
                tt = r;
                w = kPio2_3 * fn;
                r = tt - w;
                w = fms_(kPio2_3t, fn, (tt - r) - w);
                y0 = r - w;
            }
        }
    }
    return {n, y0, (r - y0) - w};
}

// The same, with the common cases inline and branch-free:
//  * |x| < 3pi/4 (library: n = 1, y0 = (|x| - pio2_1) - pio2_1t) is the general formula with n = 1: fma(-pio2_1, 1, t)
//    is the plain difference and pio2_1t * 1 is exact, so the three results are the same bits. Only the high word
//    0x3FF921FB (the library continues with pio2_2) and the cancellation re-check go to reduce_slow.
//  * the re-check (n >= 32 or npio2_hw[n - 1] == ix, then i > 16) only matters when i > 16, which needs a cancellation
//    of 16+ bits; the inline path tests the combined condition without branching and calls reduce_slow when it holds.
SHADOW_FAST_TRIG_INLINE Reduced reduce(double t, u32 ix) noexcept {
    if (ix < 0x4002D97Cu) {  // |x| < 3pi/4: n = 1, y0 = (|x| - pio2_1) - pio2_1t
        if (ix == 0x3FF921FBu) return reduce_slow(t, ix);  // the library continues with pio2_2
        const double z = t - kPio2_1;
        const double y0 = z - kPio2_1t;
        return {1, y0, (z - y0) - kPio2_1t};
    }
    const s32 n = _mm_cvttsd_si32(_mm_set_sd(fma_(kInvPio2, t, 0.5)));
    const double fn = double(n);
    const double r = fnma_(kPio2_1, fn, t);
    const double w = kPio2_1t * fn;
    const double y0 = r - w;
    const s32 i = s32(ix >> 20) - s32((hi_word(y0) >> 20) & 0x7FFu);
    const bool table_hit = (n >= 32) | (kNpio2Hw[(n - 1) & 31] == ix);  // n >= 2 here
    if (SHADOW_FAST_TRIG_UNLIKELY(table_hit & (i > 16))) return reduce_slow(t, ix);
    return {n, y0, (r - y0) - w};
}
}  // namespace trig_detail

// sin(x): the bits of shadow::math::msl::sin(x) for every x.
inline double sin(double x) noexcept {
    using namespace trig_detail;
    const u64 b = bits_of(x);
    const u32 ix = u32(b >> 32) & 0x7FFFFFFFu;
    const u64 sign = b & kSignBit;
    const double t = std::bit_cast<double>(b & ~kSignBit);
    if (ix <= kSmallCut) return kernel_sin0(x);  // odd: no need for |x|
    if (ix > kMediumCut) return shadow::math::msl::sin(x);  // |x| > 2^19 pi/2, infinity, NaN
    const Reduced q = reduce(t, ix);
    const u64 flip = sign ^ (u64(q.n & 2) << 62);
    if (q.n & 1) return xor_sign(kernel_cos1(q.y0, q.y1), flip);
    return xor_sign(kernel_sin1(q.y0, q.y1), flip);
}

// cos(x): the bits of shadow::math::msl::cos(x) for every x.
inline double cos(double x) noexcept {
    using namespace trig_detail;
    const u64 b = bits_of(x);
    const u32 ix = u32(b >> 32) & 0x7FFFFFFFu;
    const double t = std::bit_cast<double>(b & ~kSignBit);
    if (ix <= kSmallCut) return kernel_cos0(x);  // even
    if (ix > kMediumCut) return shadow::math::msl::cos(x);
    const Reduced q = reduce(t, ix);
    const u64 flip = u64((q.n + 1) & 2) << 62;  // n&3: 0 +, 1 -, 2 -, 3 +
    if (q.n & 1) return xor_sign(kernel_sin1(q.y0, q.y1), flip);
    return xor_sign(kernel_cos1(q.y0, q.y1), flip);
}

// sin and cos of one argument with one reduction: s = msl::sin(x), c = msl::cos(x), bit for bit.
inline void sincos(double x, double& s, double& c) noexcept {
    using namespace trig_detail;
    const u64 b = bits_of(x);
    const u32 ix = u32(b >> 32) & 0x7FFFFFFFu;
    const u64 sign = b & kSignBit;
    const double t = std::bit_cast<double>(b & ~kSignBit);
    if (ix <= kSmallCut) {
        s = kernel_sin0(x);
        c = kernel_cos0(x);
        return;
    }
    if (ix > kMediumCut) {
        s = shadow::math::msl::sin(x);
        c = shadow::math::msl::cos(x);
        return;
    }
    const Reduced q = reduce(t, ix);
    const double ks = kernel_sin1(q.y0, q.y1);
    const double kc = kernel_cos1(q.y0, q.y1);
    const u64 sflip = sign ^ (u64(q.n & 2) << 62);
    const u64 cflip = u64((q.n + 1) & 2) << 62;
    if (q.n & 1) {
        s = xor_sign(kc, sflip);
        c = xor_sign(ks, cflip);
    } else {
        s = xor_sign(ks, sflip);
        c = xor_sign(kc, cflip);
    }
}

}  // namespace shadow::recovered::fast
