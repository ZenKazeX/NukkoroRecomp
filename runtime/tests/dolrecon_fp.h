#pragma once

// Gekko scalar floating point for lifted code (docs/DOLRECON.md "C++ reconstruction", tier 2).
//
// The rules (Gekko user's manual PAGE 82-85 Tables 2-21 / 2-22, 97, 397-415): IEEE double arithmetic rounded to
// nearest; a NaN operand wins in the order frA, frB, frC and is returned quieted (sign kept, not negated); an invalid
// operation produces the default QNaN 0x7FF8000000000000; with FPSCR[NI] a denormalised operand is a zero of its sign
// (single-precision instructions: below 2^-126) and a result below the smallest normal is a zero of its sign;
// single-precision instructions round the double result to single, and a fused multiply-add that lands exactly on a
// midpoint between two singles is resolved by the exact value; fnmadd / fnmsub negate the rounded result. Arithmetic
// does not update FPSCR's status bits in this model; compares set FPCC and the invalid-operation bits.
//
// This is an independent formulation of those rules, verified against the reference interpreter by
// dolrecon_lift_tests: the exact sign of x*y + z - w is computed here with one signed accumulator of 32-bit limbs.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace dolrecon::fp {

using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s64 = std::int64_t;

inline constexpr u32 kNI = 0x00000004u, kFX = 0x80000000u, kFEX = 0x40000000u, kVX = 0x20000000u,
                     kOX = 0x10000000u, kUX = 0x08000000u, kZX = 0x04000000u, kXX = 0x02000000u,
                     kVXSNAN = 0x01000000u, kVXVC = 0x00080000u, kFPCC = 0x0000F000u, kVE = 0x00000080u,
                     kOE = 0x00000040u, kUE = 0x00000020u, kZE = 0x00000010u, kXE = 0x00000008u,
                     kVXAny = 0x01F80700u;

inline double d(u64 b) { double v; std::memcpy(&v, &b, 8); return v; }
inline u64 b(double v) { u64 r; std::memcpy(&r, &v, 8); return r; }

// ---- exact sign of x*y + z - w (finite doubles) --------------------------------------------------------------
// Every finite double is m * 2^e with m < 2^53, e >= -1074. Terms are placed by bit position relative to 2^-2148 (the
// smallest product exponent) into signed 64-bit cells holding 32 bits each; carries are resolved at the end.
struct Exact {
    static constexpr int kBase = -2148, kCells = 140;  // 4480 bits
    s64 cell[kCells] = {};
    void add_bits(u64 lo, u64 hi, int exponent, int sign) {  // sign * (hi:lo) * 2^exponent
        const int shift = exponent - kBase;
        const int word = shift / 32, bit = shift % 32;
        // the 128-bit value as 32-bit chunks; cell word+k gets chunk k's low bits shifted up by `bit` plus the bits that
        // spill over from chunk k-1
        const u32 chunks[5] = {u32(lo), u32(lo >> 32), u32(hi), u32(hi >> 32), 0u};
        u32 prev = 0;
        for (int k = 0; k < 5; ++k) {
            const u32 cur = chunks[k];
            const u32 part = bit == 0 ? cur : (u32(u64(cur) << bit) | (prev >> (32 - bit)));
            if (word + k < kCells) cell[word + k] += sign * s64(part);
            prev = cur;
        }
    }
    int sign() {
        s64 carry = 0;
        for (int k = 0; k < kCells; ++k) {  // normalise: each cell to [0, 2^32), carry upward (floor division)
            s64 v = cell[k] + carry;
            s64 q = v >> 32;  // arithmetic shift = floor(v / 2^32)
            cell[k] = v - (q << 32);
            carry = q;
        }
        if (carry != 0) return carry > 0 ? 1 : -1;
        for (int k = kCells - 1; k >= 0; --k)
            if (cell[k] != 0) return 1;
        return 0;
    }
};

inline void split(double v, int& sign, u64& m, int& e) {
    const u64 x = b(v);
    sign = (x >> 63) ? -1 : 1;
    const u32 be = u32(x >> 52) & 0x7FFu;
    m = x & 0xFFFFFFFFFFFFFull;
    if (be != 0) m |= 1ull << 52;
    e = (be == 0 ? 1 : int(be)) - 1075;
}

inline int exact_sign(double x, double y, double z, double w) {
    Exact acc;
    int sx, sy, sz, sw, ex, ey, ez, ew;
    u64 mx, my, mz, mw;
    split(x, sx, mx, ex);
    split(y, sy, my, ey);
    if (mx != 0 && my != 0) {
        // 53 x 53 -> 106 bits by 32-bit halves
        const u64 a0 = mx & 0xFFFFFFFFu, a1 = mx >> 32, b0 = my & 0xFFFFFFFFu, b1 = my >> 32;
        const u64 p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
        const u64 mid = (p00 >> 32) + (p01 & 0xFFFFFFFFu) + (p10 & 0xFFFFFFFFu);
        const u64 lo = (mid << 32) | (p00 & 0xFFFFFFFFu);
        const u64 hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
        acc.add_bits(lo, hi, ex + ey, sx * sy);
    }
    split(z, sz, mz, ez);
    if (mz != 0) acc.add_bits(mz, 0, ez, sz);
    split(w, sw, mw, ew);
    if (mw != 0) acc.add_bits(mw, 0, ew, -sw);
    return acc.sign();
}

enum class Op { Add, Sub, Mul, Div, Sqrt, Res, Rsqrte, Round, MAdd, MSub, NMAdd, NMSub };

// Rounds the double v to single; `dir` is sign(exact - v), used only when v is exactly between two singles.
template <typename Dir>
inline double to_single(double v, bool exact_known, const Dir& dir) {
    const float f = float(v);
    if (!exact_known || !std::isfinite(v) || double(f) == v) return double(f);
    const double mag = std::fabs(v);
    float below = float(mag), above;
    if (std::isinf(below) || double(below) > mag) {  // float(mag) rounded up (or overflowed)
        above = below;
        below = std::isinf(above) ? std::numeric_limits<float>::max() : std::nextafter(above, 0.0f);
    } else {
        above = std::nextafter(below, std::numeric_limits<float>::infinity());
    }
    const double lo = double(below), hi = std::isinf(above) ? 0x1p128 : double(above);
    if (mag - lo != hi - mag) return double(f);  // not a midpoint: the double rounding is innocuous
    const int s = dir();
    if (s == 0) return double(f);  // exactly the midpoint: ties to even, as the host conversion does
    const bool up = (v < 0) ? s < 0 : s > 0;  // away from zero when the exact magnitude is above v's
    return std::copysign(double(float(up ? hi : lo)), v);
}

inline u64 arith(u32 fpscr, Op op, bool single, u64 a, u64 bb, u64 c) {
    const bool fused = op == Op::MAdd || op == Op::MSub || op == Op::NMAdd || op == Op::NMSub;
    const bool use_a = fused || op == Op::Add || op == Op::Sub || op == Op::Mul || op == Op::Div;
    const bool use_b = op != Op::Mul;
    const bool use_c = fused || op == Op::Mul;
    const u64 ops[3] = {a, bb, c};
    const bool used[3] = {use_a, use_b, use_c};
    for (int k = 0; k < 3; ++k)
        if (used[k] && std::isnan(d(ops[k]))) {
            const u64 q = ops[k] | 0x0008000000000000ull;
            return single ? b(double(float(d(q)))) : q;
        }
    const bool ni = (fpscr & kNI) != 0;
    const auto in = [&](u64 x) {
        const double v = d(x);
        if (ni && v != 0.0 && (single ? std::fabs(v) < 0x1p-126 : std::fpclassify(v) == FP_SUBNORMAL))
            return std::copysign(0.0, v);
        return v;
    };
    const double va = in(a), vb = in(bb), vc = in(c);
    const double addend = (op == Op::MSub || op == Op::NMSub) ? -vb : vb;
    double v;
    switch (op) {
        case Op::Add: v = va + vb; break;
        case Op::Sub: v = va - vb; break;
        case Op::Mul: v = va * vc; break;
        case Op::Div: v = va / vb; break;
        case Op::Sqrt: v = std::sqrt(vb); break;
        case Op::Res: v = 1.0 / vb; break;
        case Op::Rsqrte: v = 1.0 / std::sqrt(vb); break;
        case Op::Round: v = vb; break;
        default: v = std::fma(va, vc, addend); break;
    }
    if (std::isnan(v)) return 0x7FF8000000000000ull;
    // sign(exact - v), defined where the exact value differs from the rounded double: fused ops; double mul / div.
    const auto dir = [&]() -> int {
        if (fused) return exact_sign(va, vc, addend, v);
        if (single) return 0;
        if (op == Op::Mul) return exact_sign(va, vc, 0.0, v);
        if (op == Op::Div) {  // sign(a/b - v) = sign(a - v*b) * sign(b)
            const int r = exact_sign(-v, vb, va, 0.0);
            return std::signbit(vb) ? -r : r;
        }
        return 0;
    };
    const double min_normal = single ? 0x1p-126 : 0x1p-1022;
    double r;
    if (ni && std::isfinite(v) &&
        (std::fabs(v) < min_normal || (std::fabs(v) == min_normal && (v < 0 ? dir() > 0 : dir() < 0)))) {
        r = std::copysign(0.0, v);
    } else if (single) {
        r = to_single(v, fused, dir);
    } else {
        r = v;
    }
    if (op == Op::NMAdd || op == Op::NMSub) r = -r;
    return b(r);
}

inline bool snan(u64 x) {
    return ((x >> 52) & 0x7FF) == 0x7FF && !((x >> 51) & 1) && (x & 0xFFFFFFFFFFFFFull) != 0;
}

// fcmpu / fcmpo: the CR field code (LT GT EQ UN = 8 4 2 1); FPCC and the invalid-operation bits of FPSCR.
inline u32 compare(u64 a, u64 bb, bool ordered, u32& fpscr) {
    const double va = d(a), vb = d(bb);
    u32 code;
    if (std::isnan(va) || std::isnan(vb)) {
        code = 1;
        u32 raised = 0;
        if (snan(a) || snan(bb)) {
            raised = kVXSNAN;
            if (ordered && !(fpscr & kVE)) raised |= kVXVC;
        } else if (ordered) {
            raised = kVXVC;
        }
        if (raised) {
            if ((fpscr & raised) != raised) fpscr |= kFX;
            fpscr |= raised;
            fpscr = (fpscr & kVXAny) ? (fpscr | kVX) : (fpscr & ~kVX);
            const bool enabled = ((fpscr & kVX) && (fpscr & kVE)) || ((fpscr & kOX) && (fpscr & kOE)) ||
                                 ((fpscr & kUX) && (fpscr & kUE)) || ((fpscr & kZX) && (fpscr & kZE)) ||
                                 ((fpscr & kXX) && (fpscr & kXE));
            fpscr = enabled ? (fpscr | kFEX) : (fpscr & ~kFEX);
        }
    } else {
        code = va < vb ? 8u : va > vb ? 4u : 2u;
    }
    fpscr = (fpscr & ~kFPCC) | (code << 12);
    return code;
}

// fctiw / fctiwz: 0xFFF80000_xxxxxxxx with the 32-bit integer; NaN and too-small -> 0x80000000; too large -> 0x7FFFFFFF,
// or 0x80000000 under FPSCR[NI] when the rounded value exceeds 2^31 - 1.
inline u64 fctiw(u32 fpscr, u64 x, bool truncate) {
    const double v = d(x);
    std::int32_t r;
    if (std::isnan(v)) {
        r = std::int32_t(0x80000000u);
    } else if (v > 2147483647.0) {
        const double rounded = truncate ? std::trunc(v) : std::nearbyint(v);
        r = ((fpscr & kNI) && rounded > 2147483647.0) ? std::int32_t(0x80000000u) : 2147483647;
    } else if (v < -2147483648.0) {
        r = std::int32_t(0x80000000u);
    } else {
        r = truncate ? std::int32_t(std::trunc(v)) : std::int32_t(std::nearbyint(v));
    }
    return 0xFFF8000000000000ull | u32(r);
}

// fsel: frC when frA >= 0 (and not a NaN), else frB.
inline u64 fsel(u64 a, u64 bb, u64 c) {
    const double va = d(a);
    return (va >= 0.0 && !std::isnan(va)) ? c : bb;
}

// ---- paired singles: quantized loads / stores (GQR) -------------------------------------------------------------
// GQRn: load type bits 16-18, load scale bits 24-29; store type bits 0-2, store scale bits 8-13 (signed 6-bit).
// Types 0-3 move single-precision words; 4 u8, 5 u16, 6 s8, 7 s16 scale by 2^-scale on load and 2^scale on store.
inline int gqr_scale(u32 six) { return int(six & 0x3Fu) - ((six & 0x20u) ? 64 : 0); }
inline u32 element_size(u32 type) { return (type == 4 || type == 6) ? 1u : (type == 5 || type == 7) ? 2u : 4u; }

inline u64 lfs_bits(u32 w) {  // the lfs expansion: exact, NaNs keep their signalling bit
    const u64 sign = u64(w >> 31) << 63;
    const u32 e = (w >> 23) & 0xFFu, f = w & 0x7FFFFFu;
    if (e == 0xFFu) return sign | (u64(0x7FF) << 52) | (u64(f) << 29);
    if (e != 0u) return sign | (u64(e - 127u + 1023u) << 52) | (u64(f) << 29);
    if (f == 0u) return sign;
    int x = -126;
    u32 m = f;
    while (!(m & 0x800000u)) { m <<= 1; --x; }
    return sign | (u64(x + 1023) << 52) | (u64(m & 0x7FFFFFu) << 29);
}

template <typename M>
inline u64 psq_load_element(M& m, u32 address, u32 gqr) {
    const u32 type = (gqr >> 16) & 7u;
    if (type < 4u) return lfs_bits(m.r32(address));
    const double factor = std::ldexp(1.0, -gqr_scale(gqr >> 24));
    double v;
    switch (type) {
        case 4: v = double(m.r8(address)); break;
        case 5: v = double(m.r16(address)); break;
        case 6: v = double(std::int8_t(m.r8(address))); break;
        default: v = double(std::int16_t(m.r16(address))); break;
    }
    return b(double(float(v * factor)));
}

// psq_st type 0: bit selection, except a nonzero value below the single normal range (exponent <= 896, double
// denormals included) stores a zero of its sign.
inline u32 psq_single_bits(u64 x) {
    const u32 e = u32(x >> 52) & 0x7FFu;
    if (e > 896u || (x << 1) == 0u) return u32((x >> 32) & 0xC0000000u) | u32((x >> 29) & 0x3FFFFFFFu);
    return u32(x >> 32) & 0x80000000u;
}

inline double saturate(double v, double lo, double hi) { return std::isnan(v) ? hi : (v < lo ? lo : v > hi ? hi : v); }

template <typename M>
inline void psq_store_element(M& m, u32 address, u64 x, u32 gqr) {
    const u32 type = gqr & 7u;
    if (type < 4u) {
        m.w32(address, psq_single_bits(x));
        return;
    }
    const double scaled = double(float(d(x))) * std::ldexp(1.0, gqr_scale(gqr >> 8));
    switch (type) {
        case 4: m.w8(address, std::uint8_t(saturate(scaled, 0.0, 255.0))); break;
        case 5: m.w16(address, std::uint16_t(saturate(scaled, 0.0, 65535.0))); break;
        case 6: m.w8(address, std::uint8_t(std::int8_t(saturate(scaled, -128.0, 127.0)))); break;
        default: m.w16(address, std::uint16_t(std::int16_t(saturate(scaled, -32768.0, 32767.0)))); break;
    }
}

// ps_sel lane: frC when frA >= 0 (and not a NaN), else frB.
inline u64 sel(u64 a, u64 bb, u64 c) { return fsel(a, bb, c); }

}  // namespace dolrecon::fp
