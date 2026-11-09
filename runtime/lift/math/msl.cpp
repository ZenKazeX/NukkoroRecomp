// The game's MSL math library, native (msl.h, docs/NATIVE_MSL_MATH.md). Each routine names the original it
// transcribes. Its operation order is the compiled one, the order IS the semantics: fmadd a*c+b, fmsub a*c-b and
// fnmsub -(a*c-b) round once. Everything below runs on finite values unless a comment says otherwise. The sites a
// NaN or an infinity reaches use nan_add / nan_sub, which follow the original CPU's NaN rules instead of the host's
// (x64 generates 0xFFF8000000000000 and may pick either NaN operand).

#include "math/msl.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#if defined(__AVX2__) || defined(__FMA__)
#include <immintrin.h>
#endif

namespace shadow::math::msl {
namespace {

using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s32 = std::int32_t;

u64 bits(double d) noexcept {
    u64 u;
    std::memcpy(&u, &d, 8);
    return u;
}
double value(u64 u) noexcept {
    double d;
    std::memcpy(&d, &u, 8);
    return d;
}
u32 hi(double d) noexcept { return u32(bits(d) >> 32); }
u32 lo(double d) noexcept { return u32(bits(d)); }
double words(u32 h, u32 l) noexcept { return value((u64(h) << 32) | l); }
double with_hi(double d, u32 h) noexcept { return words(h, lo(d)); }

double fmadd(double a, double c, double b) noexcept {
#if defined(__AVX2__) || defined(__FMA__)
    return _mm_cvtsd_f64(_mm_fmadd_sd(_mm_set_sd(a), _mm_set_sd(c), _mm_set_sd(b)));
#else
    return std::fma(a, c, b);
#endif
}
double fmsub(double a, double c, double b) noexcept { return fmadd(a, c, -b); }
double fnmsub(double a, double c, double b) noexcept { return -fmadd(a, c, -b); }

// fctiwz: toward zero, saturating; NaN gives INT_MIN.
s32 fctiwz(double v) noexcept {
    if (v != v) return std::numeric_limits<s32>::min();
    if (v >= 2147483647.0) return std::numeric_limits<s32>::max();
    if (v <= -2147483648.0) return std::numeric_limits<s32>::min();
    return s32(v);
}

// The original CPU's NaN rules for a + b and a - b (ppc_ops.h rule 1 and 3): the first NaN operand, quieted; an invalid
// sum (inf - inf) gives the default QNaN 0x7FF8000000000000, sign 0.
constexpr u64 kQuiet = 0x0008000000000000ull, kDefaultNaN = 0x7FF8000000000000ull;
bool is_nan(double d) noexcept { return (bits(d) << 1) > 0xFFE0000000000000ull; }
double nan_add(double a, double b) noexcept {
    if (is_nan(a)) return value(bits(a) | kQuiet);
    if (is_nan(b)) return value(bits(b) | kQuiet);
    const double r = a + b;
    return r != r ? value(kDefaultNaN) : r;
}
double nan_sub(double a, double b) noexcept {
    if (is_nan(a)) return value(bits(a) | kQuiet);
    if (is_nan(b)) return value(bits(b) | kQuiet);
    const double r = a - b;
    return r != r ? value(kDefaultNaN) : r;
}

// ---- constants, as stored in main.dol (r2 = 805FA780)
const double kZero = 0.0;
// __ieee754_rem_pio2 (r2 + 0x1450..0x1498)
const double kPio2_1 = value(0x3FF921FB54400000ull), kPio2_1t = value(0x3DD0B4611A626331ull);
const double kPio2_2 = value(0x3DD0B4611A600000ull), kPio2_2t = value(0x3BA3198A2E037073ull);
const double kPio2_3 = value(0x3BA3198A2E000000ull), kPio2_3t = value(0x397B839A252049C1ull);
const double kInvPio2 = value(0x3FE45F306DC9C883ull), kTwo24 = 0x1p24;
constexpr u32 kNpio2Hw[32] = {                                   // 805154A0
    0x3FF921FBu, 0x400921FBu, 0x4012D97Cu, 0x401921FBu, 0x401F6A7Au, 0x4022D97Cu, 0x4025FDBBu, 0x402921FBu,
    0x402C463Au, 0x402F6A7Au, 0x4031475Cu, 0x4032D97Cu, 0x40346B9Cu, 0x4035FDBBu, 0x40378FDBu, 0x403921FBu,
    0x403AB41Bu, 0x403C463Au, 0x403DD85Au, 0x403F6A7Au, 0x40407E4Cu, 0x4041475Cu, 0x4042106Cu, 0x4042D97Cu,
    0x4043A28Cu, 0x40446B9Cu, 0x404534ACu, 0x4045FDBBu, 0x4046C6CBu, 0x40478FDBu, 0x404858EBu, 0x404921FBu};
// __kernel_rem_pio2 (r2 + 0x14E8..0x1520, init_jk 80515520, PIo2 80515530, two_over_pi 80515398)
const double kTwon24 = 0x1p-24, kEight = 8.0, kEighth = 0.125, kHalf = 0.5, kOne = 1.0;
constexpr int kTwoOverPi[66] = {
    0xA2F983, 0x6E4E44, 0x1529FC, 0x2757D1, 0xF534DD, 0xC0DB62, 0x95993C, 0x439041, 0xFE5163, 0xABDEBB, 0xC561B7,
    0x246E3A, 0x424DD2, 0xE00649, 0x2EEA09, 0xD1921C, 0xFE1DEB, 0x1CB129, 0xA73EE8, 0x8235F5, 0x2EBB44, 0x84E99C,
    0x7026B4, 0x5F7E41, 0x3991D6, 0x398353, 0x39F49C, 0x845F8B, 0xBDF928, 0x3B1FF8, 0x97FFDE, 0x05980F, 0xEF2F11,
    0x8B5A0A, 0x6D1F6D, 0x367ECF, 0x27CB09, 0xB74F46, 0x3F669E, 0x5FEA2D, 0x7527BA, 0xC7EBE5, 0xF17B3D, 0x0739F7,
    0x8A5292, 0xEA6BFB, 0x5FB11F, 0x8D5D08, 0x560330, 0x46FC7B, 0x6BABF0, 0xCFBC20, 0x9AF436, 0x1DA9E3, 0x91615E,
    0xE61B08, 0x659985, 0x5F14A0, 0x68408D, 0xFFD880, 0x4D7327, 0x310606, 0x1556CA, 0x73A8C9, 0x60E27B, 0xC08C6B};
const double kPio2Pieces[8] = {
    value(0x3FF921FB40000000ull), value(0x3E74442D00000000ull), value(0x3CF8469880000000ull),
    value(0x3B78CC5160000000ull), value(0x39F01B8380000000ull), value(0x387A252040000000ull),
    value(0x36E3822280000000ull), value(0x3569F31D00000000ull)};
// scalbn (r2 + 0x15F8..0x1610), floor (r2 + 0x15D8)
const double kTwo54 = 0x1p54, kTwom54 = 0x1p-54, kHuge = value(0x7E37E43C8800759Cull),
             kTiny = value(0x01A56E1FC2F8F359ull);
// __kernel_cos (r2 + 0x14A8..0x14E0)
const double kC1 = value(0x3FA555555555554Cull), kC2 = value(0xBF56C16C16C15177ull),
             kC3 = value(0x3EFA01A019CB1590ull), kC4 = value(0xBE927E4F809C52ADull),
             kC5 = value(0x3E21EE9EBDB4B1C4ull), kC6 = value(0xBDA8FAE9BE8838D4ull);
// __kernel_sin (r2 + 0x1528..0x1558)
const double kS1 = value(0xBFC5555555555549ull), kS2 = value(0x3F8111111110F8A6ull),
             kS3 = value(0xBF2A01A019C161D5ull), kS4 = value(0x3EC71DE357B1FE7Dull),
             kS5 = value(0xBE5AE5E68A2B9CEBull), kS6 = value(0x3DE5D93A5ACFD57Cull);
// atan (r2 + 0x1598.., atanhi / atanlo / aT at 805155D8)
const double kAtanHi[4] = {value(0x3FDDAC670561BB4Full), value(0x3FE921FB54442D18ull),
                           value(0x3FEF730BD281F69Bull), value(0x3FF921FB54442D18ull)};
const double kAtanLo[4] = {value(0x3C7A2B7F222F65E2ull), value(0x3C81A62633145C07ull),
                           value(0x3C7007887AF0CBBDull), value(0x3C91A62633145C07ull)};
const double kAT[11] = {value(0x3FD555555555550Dull), value(0xBFC999999998EBC4ull), value(0x3FC24924920083FFull),
                        value(0xBFBC71C6FE231671ull), value(0x3FB745CDC54C206Eull), value(0xBFB3B0F2AF749A6Dull),
                        value(0x3FB10D66A0D03D51ull), value(0xBFADDE2D52DEFD9Aull), value(0x3FA97B4B24760DEBull),
                        value(0xBFA2B4442C6A6C2Full), value(0x3F90AD3AE322DA11ull)};
// __ieee754_atan2 (r2 + 0x1230..)
const double kPi = value(0x400921FB54442D18ull), kPiO2 = value(0x3FF921FB54442D18ull),
             kPiO4 = value(0x3FE921FB54442D18ull), k3PiO4 = value(0x4002D97C7F3321D2ull),
             kPiLo = value(0x3CA1A62633145C07ull);
// acos (r2 + 0x1120..)
const double kAPio2Hi = value(0x3FF921FB54442D18ull), kAPio2Lo = value(0x3C91A62633145C07ull);
const double kPS0 = value(0x3FC5555555555555ull), kPS1 = value(0xBFD4D61203EB6F7Dull),
             kPS2 = value(0x3FC9C1550E884455ull), kPS3 = value(0xBFA48228B5688F3Bull),
             kPS4 = value(0x3F49EFE07501B288ull), kPS5 = value(0x3F023DE10DFDF709ull);
const double kQS1 = value(0xC0033A271C8A2D4Bull), kQS2 = value(0x40002AE59C598AC8ull),
             kQS3 = value(0xBFE6066C1B8D0159ull), kQS4 = value(0x3FB3B8C5B12E9282ull);
const double kFloatNaN = value(0x7FFFFFFFE0000000ull);  // MSL __float_nan 805EED78 (0x7FFFFFFF) as a double

double copysign(double x, double y) noexcept {          // 803B256C: |x| with y's sign
    return words((hi(y) & 0x80000000u) | (hi(x) & 0x7FFFFFFFu), lo(x));
}

}  // namespace

// ---- scalbn 803B283C: MSL's classification first (a NaN or infinity and a zero return x itself), then fdlibm.
// Flush: the denormal x the compare kept reads as a zero in x * 2^54, and the final scaling is a zero: it is reached
// only for a new exponent field <= 0, so its exact value is below 2^-1022 even where it rounds up onto 2^-1022.
double detail::scalbn(double x, int n, SubnormalMode mode) noexcept {
    u32 hx = hi(x);
    if ((hx & 0x7FF00000u) == 0x7FF00000u) return x;
    if (x == kZero) return x;
    s32 k = s32((hx & 0x7FF00000u) >> 20);
    if (k == 0) {
        x = math::detail::operand(x, mode) * kTwo54;
        hx = hi(x);
        k = s32((hx & 0x7FF00000u) >> 20) - 54;
        if (n < -50000) return kTiny * x;
    }
    if (k == 0x7FF) return x + x;  // unreachable: classified above
    k = k + n;
    if (k > 0x7FE) return kHuge * copysign(kHuge, x);
    if (k > 0) return with_hi(x, (hx & 0x800FFFFFu) | (u32(k) << 20));
    if (k <= -54) {
        if (n > 50000) return kHuge * copysign(kHuge, x);
        return kTiny * copysign(kTiny, x);
    }
    k += 54;
    const double r = kTwom54 * with_hi(x, (hx & 0x800FFFFFu) | (u32(k) << 20));
    return mode == SubnormalMode::Flush ? copysign(0.0, r) : r;
}

// ---- floor 803B2668 (fdlibm; huge + x > 0 is fcmpo, false for a NaN, which keeps x).
double detail::floor(double x) noexcept {
    s32 i0 = s32(hi(x));
    u32 i1 = lo(x);
    const s32 j0 = ((i0 >> 20) & 0x7FF) - 0x3FF;
    if (j0 < 20) {
        if (j0 < 0) {
            if (kHuge + x > 0.0) {
                if (i0 >= 0) {
                    i0 = 0;
                    i1 = 0;
                } else if (((i0 & 0x7FFFFFFF) | s32(i1)) != 0) {
                    i0 = s32(0xBFF00000u);
                    i1 = 0;
                }
            }
        } else {
            const s32 i = 0x000FFFFF >> j0;
            if (((i0 & i) | s32(i1)) == 0) return x;
            if (kHuge + x > 0.0) {
                if (i0 < 0) i0 += 0x00100000 >> j0;
                i0 &= ~i;
                i1 = 0;
            }
        }
    } else if (j0 > 51) {
        return j0 == 0x400 ? nan_add(x, x) : x;
    } else {
        const u32 i = 0xFFFFFFFFu >> (j0 - 20);
        if ((i1 & i) == 0) return x;
        if (kHuge + x > 0.0) {
            if (i0 < 0) {
                if (j0 == 20) {
                    i0 += 1;
                } else {
                    const u32 j = i1 + (1u << (52 - j0));
                    if (j < i1) i0 += 1;
                    i1 = j;
                }
            }
            i1 &= ~i;
        }
    }
    return words(u32(i0), i1);
}

namespace {

// The reduction only ever scales normal values (z, one) by small exponents, so scalbn's mode is irrelevant there.
constexpr SubnormalMode kNormal = SubnormalMode::Preserve;

// ---- __kernel_rem_pio2 803B1108, as its one caller uses it: prec 2 (jk = init_jk[2] = 4), two_over_pi.
// x[0..nx-1] are x's 24-bit pieces, e0 the exponent of x[0]; y = x mod pi/2 in two doubles; returns n & 7.
int kernel_rem_pio2(const double* x, double* y, int e0, int nx) noexcept {
    constexpr int jk = 4, jp = jk;
    double f[20], fq[20], q[20];
    s32 iq[20];
    const int jx = nx - 1;
    int jv = (e0 - 3) / 24;
    if (jv < 0) jv = 0;
    int q0 = e0 - 24 * (jv + 1);
    for (int i = 0, j = jv - jx; i <= jx + jk; ++i, ++j) f[i] = j < 0 ? kZero : double(kTwoOverPi[j]);
    for (int i = 0; i <= jk; ++i) {
        double fw = 0.0;
        for (int j = 0; j <= jx; ++j) fw = fmadd(x[j], f[jx + i - j], fw);
        q[i] = fw;
    }
    int jz = jk, n, ih, carry;
    double z;
    for (;;) {  // recompute
        // Distill q[] into iq[] in reverse order.
        z = q[jz];
        for (int i = 0, j = jz; j > 0; ++i, --j) {
            const double fw = double(fctiwz(kTwon24 * z));
            iq[i] = fctiwz(fnmsub(kTwo24, fw, z));
            z = fw + q[j - 1];
        }
        z = detail::scalbn(z, q0, kNormal);
        z = fnmsub(kEight, detail::floor(kEighth * z), z);
        n = fctiwz(z);
        z = z - double(n);
        ih = 0;
        if (q0 > 0) {
            const s32 i = iq[jz - 1] >> (24 - q0);
            n += i;
            iq[jz - 1] -= i << (24 - q0);
            ih = iq[jz - 1] >> (23 - q0);
        } else if (q0 == 0) {
            ih = iq[jz - 1] >> 23;
        } else if (z >= kHalf) {
            ih = 2;
        }
        carry = 0;
        if (ih > 0) {
            n += 1;
            for (int i = 0; i < jz; ++i) {
                const s32 j = iq[i];
                if (carry == 0) {
                    if (j != 0) {
                        carry = 1;
                        iq[i] = 0x1000000 - j;
                    }
                } else {
                    iq[i] = 0xFFFFFF - j;
                }
            }
            if (q0 == 1) iq[jz - 1] &= 0x7FFFFF;
            else if (q0 == 2) iq[jz - 1] &= 0x3FFFFF;
            if (ih == 2) {
                z = kOne - z;
                if (carry != 0) z = z - detail::scalbn(kOne, q0, kNormal);
            }
        }
        if (z != kZero) break;
        // Check whether recomputation is needed.
        s32 j = 0;
        for (int i = jz - 1; i >= jk; --i) j |= iq[i];
        if (j != 0) break;
        int k = 1;
        while (iq[jk - k] == 0) ++k;  // k = the number of terms needed
        for (int i = jz + 1; i <= jz + k; ++i) {
            f[jx + i] = double(kTwoOverPi[jv + i]);
            double fw = 0.0;
            for (int jj = 0; jj <= jx; ++jj) fw = fmadd(x[jj], f[jx + i - jj], fw);
            q[i] = fw;
        }
        jz += k;
    }
    // Chop off zero terms.
    if (z == 0.0) {
        jz -= 1;
        q0 -= 24;
        while (iq[jz] == 0) {
            --jz;
            q0 -= 24;
        }
    } else {
        z = detail::scalbn(z, -q0, kNormal);
        if (z >= kTwo24) {
            const double fw = double(fctiwz(kTwon24 * z));
            iq[jz] = fctiwz(fnmsub(kTwo24, fw, z));
            jz += 1;
            q0 += 24;
            iq[jz] = fctiwz(fw);
        } else {
            iq[jz] = fctiwz(z);
        }
    }
    // Convert the integer bit chunks to floating point.
    double fw = detail::scalbn(kOne, q0, kNormal);
    for (int i = jz; i >= 0; --i) {
        q[i] = fw * double(iq[i]);
        fw = fw * kTwon24;
    }
    // fq[] = PIo2[] * q[].
    for (int i = jz; i >= 0; --i) {
        double s = 0.0;
        for (int k = 0; k <= jp && k <= jz - i; ++k) s = fmadd(kPio2Pieces[k], q[i + k], s);
        fq[jz - i] = s;
    }
    // Compress fq[] into y[] (prec 2).
    double sum = 0.0;
    for (int i = jz; i >= 0; --i) sum = sum + fq[i];
    y[0] = ih == 0 ? sum : -sum;
    double rest = fq[0] - sum;
    for (int i = 1; i <= jz; ++i) rest = rest + fq[i];
    y[1] = ih == 0 ? rest : -rest;
    return n & 7;
}

}  // namespace

// ---- __ieee754_rem_pio2 803B0C74
detail::Reduced detail::rem_pio2(double x) noexcept {
    const u32 hx = hi(x);
    const s32 ix = s32(hx & 0x7FFFFFFFu);
    if (ix <= 0x3FE921FB) return {0, x, 0.0};
    if (ix < 0x4002D97C) {  // |x| < 3pi/4, special case with n = +-1
        if (s32(hx) > 0) {
            double z = x - kPio2_1;
            if (ix != 0x3FF921FB) {
                const double y0 = z - kPio2_1t;
                return {1, y0, (z - y0) - kPio2_1t};
            }
            z = z - kPio2_2;
            const double y0 = z - kPio2_2t;
            return {1, y0, (z - y0) - kPio2_2t};
        }
        double z = x + kPio2_1;
        if (ix != 0x3FF921FB) {
            const double y0 = z + kPio2_1t;
            return {-1, y0, (z - y0) + kPio2_1t};
        }
        z = z + kPio2_2;
        const double y0 = z + kPio2_2t;
        return {-1, y0, (z - y0) + kPio2_2t};
    }
    if (ix <= 0x413921FB) {  // |x| ~<= 2^19 (pi/2), medium size
        double t = std::fabs(x);
        const s32 n = fctiwz(fmadd(kInvPio2, t, kHalf));
        const double fn = double(n);
        double r = fnmsub(kPio2_1, fn, t);
        double w = kPio2_1t * fn;
        double y0 = r - w;
        if (!(n < 32 && s32(kNpio2Hw[n - 1]) != ix)) {
            const s32 j = ix >> 20;
            s32 i = j - s32((hi(y0) >> 20) & 0x7FFu);
            if (i > 16) {  // 2nd iteration, good to 118 bits
                t = r;
                w = kPio2_2 * fn;
                r = t - w;
                w = fmsub(kPio2_2t, fn, (t - r) - w);
                y0 = r - w;
                i = j - s32((hi(y0) >> 20) & 0x7FFu);
                if (i > 49) {  // 3rd iteration, 151 bits
                    t = r;
                    w = kPio2_3 * fn;
                    r = t - w;
                    w = fmsub(kPio2_3t, fn, (t - r) - w);
                    y0 = r - w;
                }
            }
        }
        const double y1 = (r - y0) - w;
        if (s32(hx) < 0) return {-n, -y0, -y1};
        return {n, y0, y1};
    }
    if (ix >= 0x7FF00000) {  // infinity or NaN
        const double nan = nan_sub(x, x);
        return {0, nan, nan};
    }
    // All other (large) arguments: x's 24-bit pieces.
    const s32 e0 = (ix >> 20) - 1046;  // e0 = ilogb(z) - 23
    double z = words(u32(ix - (e0 << 20)), lo(x));
    double tx[3];
    for (int i = 0; i < 2; ++i) {
        tx[i] = double(fctiwz(z));
        z = (z - tx[i]) * kTwo24;
    }
    tx[2] = z;
    int nx = 3;
    while (tx[nx - 1] == kZero) --nx;  // skip zero terms
    double y[2];
    const int n = kernel_rem_pio2(tx, y, e0, nx);
    if (s32(hx) < 0) return {-n, -y[0], -y[1]};
    return {n, y[0], y[1]};
}

// ---- __kernel_sin 803B1F5C
double detail::kernel_sin(double x, double y, int iy) noexcept {
    const s32 ix = s32(hi(x) & 0x7FFFFFFFu);
    if (ix < 0x3E400000 && fctiwz(x) == 0) return x;  // |x| < 2^-27, generate inexact
    const double z = x * x;
    const double r5 = fmadd(kS6, z, kS5);
    const double v = z * x;
    const double r4 = fmadd(z, r5, kS4);
    double r = fmadd(z, r4, kS3);
    r = fmadd(z, r, kS2);
    if (iy == 0) return fmadd(v, fmadd(z, r, kS1), x);
    double t = v * r;
    t = fmsub(kHalf, y, t);
    t = fmsub(z, t, y);
    t = fnmsub(kS1, v, t);
    return x - t;
}

// ---- __kernel_cos 803B1014
double detail::kernel_cos(double x, double y) noexcept {
    const s32 ix = s32(hi(x) & 0x7FFFFFFFu);
    if (ix < 0x3E400000 && fctiwz(x) == 0) return kOne;  // |x| < 2^-27
    const double z = x * x;
    const double r5 = fmadd(kC6, z, kC5);
    const double r4 = fmadd(z, r5, kC4);
    const double r3 = fmadd(z, r4, kC3);
    const double r2 = fmadd(z, r3, kC2);
    const double r = z * fmadd(z, r2, kC1);
    if (ix < 0x3FD33333) {  // |x| < 0.3
        double t = x * y;
        t = fmsub(z, r, t);
        t = fmsub(kHalf, z, t);
        return kOne - t;
    }
    const double qx = ix > 0x3FE90000 ? 0.28125 : words(u32(ix) - 0x00200000u, 0);  // x/4, or 0.28125
    double t = x * y;
    const double hz = fmsub(kHalf, z, qx);
    t = fmsub(z, r, t);
    const double a = kOne - qx;
    t = hz - t;
    return a - t;
}

// ---- sin 803B2AFC, cos 803B2594
double sin(double x) noexcept {
    const s32 ix = s32(hi(x) & 0x7FFFFFFFu);
    if (ix <= 0x3FE921FB) return detail::kernel_sin(x, kZero, 0);
    if (ix >= 0x7FF00000) return nan_sub(x, x);  // sin(Inf or NaN) is NaN
    const detail::Reduced r = detail::rem_pio2(x);
    switch (r.n & 3) {
        case 0: return detail::kernel_sin(r.y0, r.y1, 1);
        case 1: return detail::kernel_cos(r.y0, r.y1);
        case 2: return -detail::kernel_sin(r.y0, r.y1, 1);
        default: return -detail::kernel_cos(r.y0, r.y1);
    }
}

double cos(double x) noexcept {
    const s32 ix = s32(hi(x) & 0x7FFFFFFFu);
    if (ix <= 0x3FE921FB) return detail::kernel_cos(x, kZero);
    if (ix >= 0x7FF00000) return nan_sub(x, x);
    const detail::Reduced r = detail::rem_pio2(x);
    switch (r.n & 3) {
        case 0: return detail::kernel_cos(r.y0, r.y1);
        case 1: return -detail::kernel_sin(r.y0, r.y1, 1);
        case 2: return -detail::kernel_cos(r.y0, r.y1);
        default: return detail::kernel_sin(r.y0, r.y1, 1);
    }
}

// ---- __kernel_tan 803B1FFC (r2 + 0x1560..0x1590, T at 80515570), tan 803B2BD4 (its kernel y at r2 + 0x1620)
namespace {
const double kT[13] = {value(0x3FD5555555555563ull), value(0x3FC111111110FE7Aull), value(0x3FABA1BA1BB341FEull),
                       value(0x3F9664F48406D637ull), value(0x3F8226E3E96E8493ull), value(0x3F6D6D22C9560328ull),
                       value(0x3F57DBC8FEE08315ull), value(0x3F4344D8F2F26501ull), value(0x3F3026F71A8D1068ull),
                       value(0x3F147E88A03792A6ull), value(0x3F12B80F32F0A7E9ull), value(0xBEF375CBDB605373ull),
                       value(0x3EFB2A7074BF7AD4ull)};
const double kPio4 = value(0x3FE921FB54442D18ull), kPio4Lo = value(0x3C81A62633145C07ull);
}  // namespace

double detail::kernel_tan(double x, double y, int iy) noexcept {
    const u32 hx = hi(x);
    const s32 ix = s32(hx & 0x7FFFFFFFu);
    if (ix < 0x3E300000 && fctiwz(x) == 0) {  // |x| < 2^-28
        if (((u32(ix) | lo(x)) | u32(iy + 1)) == 0u) return kOne / std::fabs(x);
        if (iy == 1) return x;
        return -kOne / x;
    }
    if (ix >= 0x3FE59428) {  // |x| >= 0.6744
        if (s32(hx) < 0) {
            x = -x;
            y = -y;
        }
        const double w = kPio4Lo - y;
        const double z = kPio4 - x;
        x = z + w;
        y = 0.0;
    }
    const double z = x * x;
    const double w = z * z;
    double v7 = fmadd(w, kT[12], kT[10]);
    const double r12 = fmadd(w, kT[11], kT[9]);
    v7 = fmadd(w, v7, kT[8]);
    const double r11 = fmadd(w, r12, kT[7]);
    double v6 = fmadd(w, v7, kT[6]);
    const double r7 = fmadd(w, r11, kT[5]);
    const double v5 = fmadd(w, v6, kT[4]);
    v6 = fmadd(w, r7, kT[3]);
    const double v4 = fmadd(w, v5, kT[2]);
    const double r = fmadd(w, v6, kT[1]);
    const double v = z * v4;
    const double s = z * x;
    double t = fmadd(s, r + v, y);
    double rr = fmadd(z, t, y);
    rr = fmadd(kT[0], s, rr);
    const double ww = x + rr;
    if (ix >= 0x3FE59428) {
        const double vi = double(iy);
        const double sign = double(1 - s32((hx >> 30) & 2u));
        t = ww * ww / (ww + vi) - rr;
        return sign * fnmsub(2.0, x - t, vi);  // (1 - ((hx >> 30) & 2)) * (v - 2 (x - (w w / (w + v) - r)))
    }
    if (iy == 1) return ww;
    // -1 / (x + r) accurately: z = w and t = a with their low words cleared.
    const double a = -kOne / ww;
    const double zh = words(hi(ww), 0u), th = words(hi(a), 0u);
    const double vv = rr - (zh - x);
    const double sum = fmadd(th, zh, kOne);
    return fmadd(a, fmadd(th, vv, sum), th);
}

double tan(double x) noexcept {
    const s32 ix = s32(hi(x) & 0x7FFFFFFFu);
    if (ix <= 0x3FE921FB) return detail::kernel_tan(x, kZero, 1);
    if (ix >= 0x7FF00000) return nan_sub(x, x);
    const detail::Reduced r = detail::rem_pio2(x);
    return detail::kernel_tan(r.y0, r.y1, 1 - ((r.n & 1) << 1));
}

// ---- atan 803B2210
double atan(double x) noexcept {
    const u32 hx = hi(x);
    const s32 ix = s32(hx & 0x7FFFFFFFu);
    if (ix >= 0x44100000) {  // |x| >= 2^66
        if (ix > 0x7FF00000 || (ix == 0x7FF00000 && lo(x) != 0u)) return nan_add(x, x);
        if (s32(hx) > 0) return kAtanHi[3] + kAtanLo[3];
        return -kAtanHi[3] - kAtanLo[3];
    }
    int id;
    double v = x;
    if (ix < 0x3FDC0000) {  // |x| < 0.4375
        if (ix < 0x3E200000 && kHuge + x > kOne) return x;  // |x| < 2^-29, raise inexact
        id = -1;
    } else {
        const double a = std::fabs(x);
        if (ix < 0x3FF30000) {      // |x| < 1.1875
            if (ix < 0x3FE60000) {  // 7/16 <= |x| < 11/16
                id = 0;
                v = fmsub(2.0, a, kOne) / (2.0 + a);
            } else {  // 11/16 <= |x| < 19/16
                id = 1;
                v = (a - kOne) / (kOne + a);
            }
        } else if (ix < 0x40038000) {  // |x| < 2.4375
            id = 2;
            v = (a - 1.5) / fmadd(1.5, a, kOne);
        } else {  // 2.4375 <= |x| < 2^66
            id = 3;
            v = -1.0 / a;
        }
    }
    const double z = v * v;
    const double w = z * z;
    const double f8 = fmadd(w, kAT[10], kAT[8]);
    double f3 = fmadd(w, kAT[9], kAT[7]);
    const double f7 = fmadd(w, f8, kAT[6]);
    double f2 = fmadd(w, f3, kAT[5]);
    f3 = fmadd(w, f7, kAT[4]);
    double f1 = fmadd(w, f2, kAT[3]);
    f2 = fmadd(w, f3, kAT[2]);
    const double f0 = fmadd(w, f1, kAT[1]);
    f1 = fmadd(w, f2, kAT[0]);
    const double s2 = w * f0, s1 = z * f1;
    if (id < 0) return fnmsub(v, s1 + s2, v);  // x - x (s1 + s2)
    double t = fmsub(v, s1 + s2, kAtanLo[id]);
    t = t - v;
    t = kAtanHi[id] - t;
    return s32(hx) < 0 ? -t : t;
}

// ---- __ieee754_atan2 803AFAEC. The quotient y / x is the one mode-dependent operation (a denormal operand reads
// as zero, a tiny quotient flushes); every other value here is a constant, an operand or atan's result.
double atan2(double y, double x, SubnormalMode mode) noexcept {
    const u32 hx = hi(x), lx = lo(x), hy = hi(y), ly = lo(y);
    const u32 ix = hx & 0x7FFFFFFFu, iy = hy & 0x7FFFFFFFu;
    if ((ix | ((lx | (0u - lx)) >> 31)) > 0x7FF00000u || (iy | ((ly | (0u - ly)) >> 31)) > 0x7FF00000u)
        return nan_add(x, y);  // x or y is NaN
    if (((hx - 0x3FF00000u) | lx) == 0u) return atan(y);  // x = 1.0
    const u32 m = ((hy >> 31) & 1u) | ((hx >> 30) & 2u);  // 2 * sign(x) + sign(y)
    if ((iy | ly) == 0u) {                                 // y = 0
        if (m == 2u) return kPi;
        if (m == 3u) return -kPi;
        return y;
    }
    if ((ix | lx) == 0u) return s32(hy) < 0 ? -kPiO2 : kPiO2;  // x = 0
    if (ix == 0x7FF00000u) {                                      // x = Inf
        if (iy == 0x7FF00000u) {
            const double both[4] = {kPiO4, -kPiO4, k3PiO4, -k3PiO4};
            return both[m];
        }
        const double finite_y[4] = {kZero, -kZero, kPi, -kPi};
        return finite_y[m];
    }
    if (iy == 0x7FF00000u) return s32(hy) < 0 ? -kPiO2 : kPiO2;  // y = Inf
    const s32 k = s32(iy - ix) >> 20;
    double z;
    if (k > 60) {
        z = kPiO2;  // |y / x| > 2^60
    } else if (s32(hx) < 0 && k < -60) {
        z = kZero;  // |y| / x < -2^-60
    } else {
        z = atan(std::fabs(math::detail::divide(y, x, mode)));
    }
    switch (m) {
        case 0: return z;
        case 1: return -z;
        case 2: return kPi - (z - kPiLo);
        default: return (z - kPiLo) - kPi;
    }
}

// ---- acos 803AF678. Its square roots (the wrapper 803B2F98 around the software sqrt 803B2D2C, replaced by
// math/scalar.h) see z in (0, 0.75]: positive and normal, so the correctly rounded std::sqrt is the same bits.
double acos(double x) noexcept {
    const u32 hx = hi(x);
    const s32 ix = s32(hx & 0x7FFFFFFFu);
    if (ix >= 0x3FF00000) {  // |x| >= 1, or NaN
        if (((u32(ix) - 0x3FF00000u) | lo(x)) == 0u) return s32(hx) > 0 ? 0.0 : kPi;
        return kFloatNaN;
    }
    if (ix < 0x3FE00000) {  // |x| < 0.5
        if (ix <= 0x3C600000) return kAPio2Hi;
        const double z = x * x;
        double f4 = fmadd(kPS5, z, kPS4);
        const double f9 = fmadd(z, f4, kPS3);
        double f5 = fmadd(kQS4, z, kQS3);
        const double f8 = fmadd(z, f9, kPS2);
        f4 = fmadd(z, f5, kQS2);
        f5 = fmadd(z, f8, kPS1);
        const double f3 = fmadd(z, f4, kQS1);
        f4 = fmadd(z, f5, kPS0);
        const double q = fmadd(z, f3, kOne);
        const double r = (z * f4) / q;
        double t = fnmsub(x, r, kAPio2Lo);
        t = x - t;
        return kAPio2Hi - t;
    }
    if (s32(hx) < 0) {  // x < -0.5
        const double z = kHalf * (kOne + x);
        const double s = std::sqrt(z);
        double f4 = fmadd(kPS5, z, kPS4);
        double f8 = fmadd(z, f4, kPS3);
        double f2 = fmadd(kQS4, z, kQS3);
        f8 = fmadd(z, f8, kPS2);
        double f5 = fmadd(z, f2, kQS2);
        const double f7 = fmadd(z, f8, kPS1);
        f4 = fmadd(z, f5, kQS1);
        f5 = fmadd(z, f7, kPS0);
        const double f3 = fmadd(z, f4, kOne);
        const double r = (z * f5) / f3;
        f2 = fmsub(r, s, kAPio2Lo);
        return fnmsub(2.0, s + f2, kPi);
    }
    const double z = kHalf * (kOne - x);  // x > 0.5
    const double s = std::sqrt(z);
    const double df = words(hi(s), 0);
    double f3 = fmadd(kPS5, z, kPS4);
    double f5 = fmadd(z, f3, kPS3);
    f3 = fnmsub(df, df, z);
    const double f8 = fmadd(z, f5, kPS2);
    f5 = fmadd(kQS4, z, kQS3);
    const double f7 = fmadd(z, f8, kPS1);
    f5 = fmadd(z, f5, kQS2);
    double f6 = fmadd(z, f7, kPS0);
    f5 = fmadd(z, f5, kQS1);
    double t = s + df;
    f6 = z * f6;
    const double f4 = fmadd(z, f5, kOne);
    t = f3 / t;
    const double r = f6 / f4;
    t = fmadd(r, s, t);
    t = df + t;
    return 2.0 * t;
}

// ---- __ieee754_pow 803B0444 (fdlibm e_pow.c as MSL compiled it; wrapper 803B2D0C). Constants: r2-relative doubles
// 805FBAB8..805FBBC0, bp / dp_h / dp_l at 80515368. The fused steps are the compiled ones. Every intermediate of the
// log / exp evaluation is finite and normal or zero for finite y and normal x, except: a denormal x (scaled by 2^53,
// a multiply whose operand Flush reads as zero), a denormal y (its products: the same), and p_l + p_h (a sum of
// two products that can overflow with opposite signs: the original CPU's NaN rules). The square root of y = 0.5 is the
// software sqrt 803B2D2C (through 803B2F98), exact like std::sqrt for every x >= 0 that reaches it.
namespace {
const double kPowBp[2] = {1.0, 1.5};
const double kPowDpH[2] = {0.0, value(0x3FE2B80340000000ull)};
const double kPowDpL[2] = {0.0, value(0x3E4CFDEB43CFD006ull)};
const double kPowInf = value(0x7FF0000000000000ull), kPowThird = value(0x3FD5555555555555ull),
             kPowIvln2H = value(0x3FF7154760000000ull), kPowIvln2L = value(0x3E54AE0BF85DDF44ull),
             kPowIvln2 = value(0x3FF71547652B82FEull), kPowTwo53 = 0x1p53,
             kPowL1 = value(0x3FE3333333333303ull), kPowL2 = value(0x3FDB6DB6DB6FABFFull),
             kPowL3 = value(0x3FD55555518F264Dull), kPowL4 = value(0x3FD17460A91D4101ull),
             kPowL5 = value(0x3FCD864A93C9DB65ull), kPowL6 = value(0x3FCA7E284A454EEFull),
             kPowCpH = value(0x3FEEC709E0000000ull), kPowCpL = value(0xBE3E2FE0145B01F5ull),
             kPowCp = value(0x3FEEC709DC3A03FDull), kPowOvt = value(0x3C971547652B82FEull),
             kPowLg2H = value(0x3FE62E4300000000ull), kPowLg2 = value(0x3FE62E42FEFA39EFull),
             kPowLg2L = value(0xBE205C610CA86C39ull), kPowP1 = value(0x3FC555555555553Eull),
             kPowP2 = value(0xBF66C16C16BEBD93ull), kPowP3 = value(0x3F11566AAF25DE2Cull),
             kPowP4 = value(0xBEBBBD41C5D26BF1ull), kPowP5 = value(0x3E66376972BEA4D0ull);
double low_zero(double d) noexcept { return words(hi(d), 0); }
}  // namespace

double pow(double x, double y, SubnormalMode mode) noexcept {
    const s32 hx = s32(hi(x)), hy = s32(hi(y));
    const u32 lx = lo(x), ly = lo(y);
    s32 ix = hx & 0x7FFFFFFF;
    const s32 iy = hy & 0x7FFFFFFF;
    if ((u32(iy) | ly) == 0u) return kOne;
    if (ix > 0x7FF00000 || (ix == 0x7FF00000 && lx != 0) || iy > 0x7FF00000 || (iy == 0x7FF00000 && ly != 0))
        return nan_add(x, y);
    s32 yisint = 0;  // 0: y not an integer, 1: odd, 2: even
    if (hx < 0) {
        if (iy >= 0x43400000) {
            yisint = 2;
        } else if (iy >= 0x3FF00000) {
            const s32 k = (iy >> 20) - 0x3FF;
            if (k > 20) {
                const u32 j = ly >> (52 - k);
                if ((j << (52 - k)) == ly) yisint = 2 - s32(j & 1u);
            } else if (ly == 0) {
                const s32 j = iy >> (20 - k);
                if ((j << (20 - k)) == iy) yisint = 2 - (j & 1);
            }
        }
    }
    if (ly == 0) {
        if (iy == 0x7FF00000) {  // y is +-inf
            if (((u32(ix) - 0x3FF00000u) | lx) == 0u) return nan_sub(y, y);
            if (ix >= 0x3FF00000) return hy >= 0 ? y : kZero;
            return hy < 0 ? -y : kZero;
        }
        if (iy == 0x3FF00000) return hy < 0 ? math::detail::divide(kOne, x, mode) : x;
        if (hy == 0x40000000) {
            const double xx = math::detail::operand(x, mode) * math::detail::operand(x, mode);
            return mode == SubnormalMode::Flush && std::fabs(xx) < 0x1p-1022 ? copysign(0.0, xx) : xx;
        }
        if (hy == 0x3FE00000 && hx >= 0) return std::sqrt(x);
    }
    double ax = std::fabs(x);
    if (lx == 0 && (ix == 0x7FF00000 || ix == 0 || ix == 0x3FF00000)) {
        double z = ax;
        if (hy < 0) z = math::detail::divide(kOne, z, mode);
        if (hx < 0) {
            if (((u32(ix) - 0x3FF00000u) | u32(yisint)) == 0u) {
                z = math::detail::divide(nan_sub(z, z), nan_sub(z, z), mode);
            } else if (yisint == 1) {
                z = -z;
            }
        }
        return z;
    }
    const s32 xpositive = (hx >> 31) + 1;
    if ((xpositive | yisint) == 0) return kFloatNaN;  // MSL: errno = EDOM, __float_nan
    double t1, t2;
    if (iy > 0x41E00000) {  // |y| > 2^31
        if (iy > 0x43F00000) {
            if (ix <= 0x3FEFFFFF) return hy < 0 ? kPowInf : kZero;
            if (ix >= 0x3FF00000) return hy > 0 ? kPowInf : kZero;
        }
        if (ix < 0x3FEFFFFF) return hy < 0 ? kPowInf : kZero;
        if (ix > 0x3FF00000) return hy > 0 ? kPowInf : kZero;
        const double t = x - kOne;  // MSL: x, where fdlibm has |x|
        const double f4 = fnmsub(0.25, t, kPowThird);
        const double f3 = fnmsub(t, f4, kHalf);
        const double w = (t * t) * f3;
        const double u = kPowIvln2H * t;
        const double v = fmsub(kPowIvln2L, t, kPowIvln2 * w);
        t1 = low_zero(u + v);
        t2 = v - (t1 - u);
    } else {
        s32 n = 0;
        if (ix < 0x00100000) {
            ax = math::detail::operand(ax, mode) * kPowTwo53;
            n -= 53;
            ix = s32(hi(ax));
        }
        n += (ix >> 20) - 0x3FF;
        const s32 j = ix & 0x000FFFFF;
        ix = j | 0x3FF00000;
        int k;
        if (j <= 0x3988E) {
            k = 0;
        } else if (j < 0xBB67A) {
            k = 1;
        } else {
            k = 0;
            n += 1;
            ix -= 0x00100000;
        }
        ax = with_hi(ax, u32(ix));
        const double bp = kPowBp[k];
        double u = ax - bp;
        const double v1 = kOne / (ax + bp);
        const double ss = u * v1;
        const double s_h = low_zero(ss);
        const double t_h0 = words(u32(((ix >> 1) | 0x20000000) + 0x00080000 + (k << 18)), 0);
        const double t_l0 = ax - (t_h0 - bp);
        const double s2a = ss * ss;
        u = fnmsub(s_h, t_h0, u);                         // u - s_h * t_h
        const double s_l = v1 * fnmsub(s_h, t_l0, u);    // v * (that - s_h * t_l)
        double r = fmadd(s2a, fmadd(s2a, fmadd(s2a, fmadd(s2a, fmadd(kPowL6, s2a, kPowL5), kPowL4), kPowL3), kPowL2),
                         kPowL1);
        r = (s2a * s2a) * r;
        r = fmadd(s_l, s_h + ss, r);
        const double s2 = s_h * s_h;
        const double t_h = low_zero((3.0 + s2) + r);
        const double t_l = r - ((t_h - 3.0) - s2);
        const double uu = s_h * t_h;
        const double vv = fmadd(s_l, t_h, t_l * ss);
        const double p_h = low_zero(uu + vv);
        const double p_l = vv - (p_h - uu);
        const double z_h = kPowCpH * p_h;
        const double z_l = kPowDpL[k] + fmadd(kPowCpL, p_h, kPowCp * p_l);
        const double t = words(0x43300000u, u32(n) ^ 0x80000000u) - words(0x43300000u, 0x80000000u);
        t1 = low_zero(t + ((z_h + z_l) + kPowDpH[k]));
        t2 = z_l - (((t1 - t) - kPowDpH[k]) - z_h);
    }
    const double s = (xpositive | (yisint - 1)) == 0 ? -kOne : kOne;  // x < 0 and y odd
    const double yo = math::detail::operand(y, mode);
    const double y1 = math::detail::operand(low_zero(y), mode);
    const double p_l = fmadd(t1, yo - y1, yo * t2);
    double p_h = y1 * t1;
    const double z = nan_add(p_l, p_h);
    s32 j = s32(hi(z));
    const s32 i = s32(lo(z));
    if (j >= 0x40900000) {
        if (((j - 0x40900000) | i) != 0) return kHuge * (kHuge * s);
        if (kPowOvt + p_l > z - p_h) return kHuge * (kHuge * s);
    } else if ((j & 0x7FFFFFFF) >= 0x4090CC00) {
        if (((j - s32(0xC090CC00u)) | i) != 0) return kTiny * (kTiny * s);
        if (p_l <= z - p_h) return kTiny * (kTiny * s);
    }
    const s32 ia = j & 0x7FFFFFFF;
    s32 k = (ia >> 20) - 0x3FF;
    s32 nn = 0;
    if (ia > 0x3FE00000) {
        nn = j + (0x00100000 >> (((j >> 20) & 0x7FF) - 0x3FE));
        k = ((nn & 0x7FFFFFFF) >> 20) - 0x3FF;
        const double t = words(u32(nn & ~(0x000FFFFF >> k)), 0);
        nn = ((nn & 0x000FFFFF) | 0x00100000) >> (20 - k);
        if (j < 0) nn = -nn;
        p_h = p_h - t;
    }
    const double t = low_zero(p_l + p_h);
    const double u = kPowLg2H * t;
    const double v = fmadd(kPowLg2, p_l - (t - p_h), t * kPowLg2L);
    double zz = u + v;
    const double w = v - (zz - u);
    const double tt = zz * zz;
    const double f0 = fmadd(zz, w, w);
    const double t1b = zz - tt * fmadd(tt, fmadd(tt, fmadd(tt, fmadd(kPowP5, tt, kPowP4), kPowP3), kPowP2), kPowP1);
    const double r = (zz * t1b) / (t1b - 2.0) - f0;
    zz = kOne - (r - zz);
    j = s32(hi(zz)) + (nn << 20);
    if ((j >> 20) <= 0) {
        zz = detail::scalbn(zz, nn, mode);
    } else {
        zz = with_hi(zz, u32(j));
    }
    return s * zz;
}

// ---- the float wrappers: the double routine, then frsp.
float round_to_single(double x, SubnormalMode mode) noexcept {
    if (mode == SubnormalMode::Flush && x == x && std::fabs(x) < 0x1p-126) return std::signbit(x) ? -0.0f : 0.0f;
    return float(x);
}
float sinf(float x, SubnormalMode mode) noexcept { return round_to_single(sin(double(x)), mode); }
float cosf(float x, SubnormalMode mode) noexcept { return round_to_single(cos(double(x)), mode); }
float atan2f(float y, float x, SubnormalMode mode) noexcept {
    return round_to_single(atan2(double(y), double(x), mode), mode);
}
float acosf(float x, SubnormalMode mode) noexcept { return round_to_single(acos(double(x)), mode); }
float atanf(float x, SubnormalMode mode) noexcept { return round_to_single(atan(double(x)), mode); }
float tanf(float x, SubnormalMode mode) noexcept { return round_to_single(tan(double(x)), mode); }

const int* detail::two_over_pi() noexcept { return kTwoOverPi; }
const double* detail::pio2_pieces() noexcept { return kPio2Pieces; }

}  // namespace shadow::math::msl
