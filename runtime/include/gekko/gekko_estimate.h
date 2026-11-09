#pragma once

// The Gekko reciprocal estimates: fres, ps_res, frsqrte and ps_rsqrte.
//
// These are not divisions. The 750CL looks a 5-bit segment of the mantissa up
// in a table, interpolates linearly over the next ten (fres) or eleven
// (frsqrte, with the exponent's low bit choosing the half) and scales by the
// operand's exponent. The result has about twelve good bits and differs from
// 1/x and 1/sqrt(x) in the low places; guest code refines it with Newton
// steps, but a caller that stores, compares or feeds the raw estimate onward
// sees those bits, so an exact reciprocal is a different value.
//
// Source of the segments and of every special case: the hardware-verified
// definitions in hwtests cputest/reciprocal.cpp (Common/FloatUtils.h
// frsqrte_expected / fres_expected), which compare against the real fres and
// frsqrte for every operand whose low 32 bits are zero. The instruction pages
// of the Gekko manual give only the accuracy bound. The tables here are that
// definition re-expressed as (base, slope) over the double's fraction:
// frsqrte's halves are ordered by the exponent's low bit, fres's base is
// halved with the rounding folded into the slope term.
//
// Nothing here reads the machine: each result is a pure function of the
// operand bits and, for fres, FPSCR[NI].

#include <cstdint>
#include <cstring>

#include "gekko/types.h"

namespace gekko {
namespace ppc {
namespace estimate {

struct Segment {
    s64 base;
    s64 slope;
};

// frsqrte / ps_rsqrte. Index 0-15: the biased exponent is even; 16-31: odd.
constexpr Segment kReciprocalRoot[] = {
    {0x1a7e800, -0x568}, {0x17cb800, -0x4f3}, {0x1552800, -0x48d},
    {0x130c000, -0x435}, {0x10f2000, -0x3e7}, {0x0eff000, -0x3a2},
    {0x0d2e000, -0x365}, {0x0b7c000, -0x32e}, {0x09e5000, -0x2fc},
    {0x0867000, -0x2d0}, {0x06ff000, -0x2a8}, {0x05ab800, -0x283},
    {0x046a000, -0x261}, {0x0339800, -0x243}, {0x0218800, -0x226},
    {0x0105800, -0x20b}, {0x3ffa000, -0x7a4}, {0x3c29000, -0x700},
    {0x38aa000, -0x670}, {0x3572000, -0x5f2}, {0x3279000, -0x584},
    {0x2fb7000, -0x524}, {0x2d26000, -0x4cc}, {0x2ac0000, -0x47e},
    {0x2881000, -0x43a}, {0x2665000, -0x3fa}, {0x2468000, -0x3c2},
    {0x2287000, -0x38e}, {0x20c1000, -0x35e}, {0x1f12000, -0x332},
    {0x1d79000, -0x30a}, {0x1bf4000, -0x2e6},
};

// fres / ps_res.
constexpr Segment kReciprocal[] = {
    {0x7ff800, 0x3e1}, {0x783800, 0x3a7}, {0x70ea00, 0x371}, {0x6a0800, 0x340},
    {0x638800, 0x313}, {0x5d6200, 0x2ea}, {0x579000, 0x2c4}, {0x520800, 0x2a0},
    {0x4cc800, 0x27f}, {0x47ca00, 0x261}, {0x430800, 0x245}, {0x3e8000, 0x22a},
    {0x3a2c00, 0x212}, {0x360800, 0x1fb}, {0x321400, 0x1e5}, {0x2e4a00, 0x1d1},
    {0x2aa800, 0x1be}, {0x272c00, 0x1ac}, {0x23d600, 0x19b}, {0x209e00, 0x18b},
    {0x1d8800, 0x17c}, {0x1a9000, 0x16e}, {0x17ae00, 0x15b}, {0x14f800, 0x15b},
    {0x124400, 0x143}, {0x0fbe00, 0x143}, {0x0d3800, 0x12d}, {0x0ade00, 0x12d},
    {0x088400, 0x11a}, {0x065000, 0x11a}, {0x041c00, 0x108}, {0x020c00, 0x106},
};

namespace detail {

constexpr u64 kSign = 1ull << 63;
constexpr u64 kFraction = (1ull << 52) - 1;
constexpr u64 kExponent = 0x7ffull << 52;
constexpr u64 kQuiet = 0x0008000000000000ull;
constexpr u64 kDefaultNaN = 0x7ff8000000000000ull;
constexpr u64 kInfinity = 0x7ff0000000000000ull;
constexpr u64 kLargestSingle = 0x47efffffe0000000ull;  // FLT_MAX as a double

inline u64 single_to_double_bits(u32 word) noexcept {
    float single;
    std::memcpy(&single, &word, sizeof(single));
    const double widened = static_cast<double>(single);  // exact, denormals included
    u64 result;
    std::memcpy(&result, &widened, sizeof(result));
    return result;
}

}  // namespace detail

// fres / ps_res of one operand. A zero gives the infinity of its sign, a NaN
// its quieted self, an infinity a zero of its sign. Operands whose reciprocal
// would overflow a single (biased exponent below 895, denormals included)
// give the largest single of their sign. At the other end the reciprocal is a
// single denormal: with FPSCR[NI] set it is a zero of the operand's sign from
// biased exponent 1149 on; with NI clear the segment value is shifted into
// denormal form, and only from exponent 1172 on is the result a zero.
inline u64 reciprocal(u64 operand, bool non_ieee) noexcept {
    using namespace detail;
    const u64 fraction = operand & kFraction;
    const u64 sign = operand & kSign;
    const u64 biased = (operand & kExponent) >> 52;

    if (fraction == 0u && biased == 0u) return sign | kInfinity;
    if (biased == 0x7ffu) {
        if (fraction == 0u) return sign;
        return operand | kQuiet;
    }
    if (biased < 895u) return sign | kLargestSingle;
    if (biased >= (non_ieee ? 1149u : 1172u)) return sign;

    const unsigned index = static_cast<unsigned>(fraction >> 37);  // 15 bits
    const Segment& segment = kReciprocal[index >> 10];
    const u64 mantissa =
        static_cast<u64>(segment.base - (segment.slope * static_cast<s64>(index & 1023u) + 1) / 2);  // 23 bits
    // Single exponent 253 - (biased - 896) = 1149 - biased.
    if (biased < 1149u) return sign | ((0x7fdull - biased) << 52) | (mantissa << 29);
    // NI clear, 1149 <= biased < 1172: a single denormal.
    const unsigned shift = 1u + static_cast<unsigned>(biased - 1149u);
    const u32 single = static_cast<u32>(sign >> 32) |
                       static_cast<u32>(((1ull << 23) | mantissa) >> shift);
    return single_to_double_bits(single);
}

// frsqrte / ps_rsqrte of one operand. A zero gives the infinity of its sign; a
// positive infinity +0; a negative operand (not a zero) or negative infinity
// the default NaN; a NaN its quieted self. A denormal is normalised before the
// lookup. The result is a double whose low 26 fraction bits are zero.
inline u64 reciprocal_root(u64 operand) noexcept {
    using namespace detail;
    s64 fraction = static_cast<s64>(operand & kFraction);
    const u64 sign = operand & kSign;
    s64 exponent = static_cast<s64>(operand & kExponent);

    if (fraction == 0 && exponent == 0) return sign | kInfinity;
    if (exponent == static_cast<s64>(kExponent)) {
        if (fraction == 0) return sign != 0u ? kDefaultNaN : 0u;
        return operand | kQuiet;
    }
    if (sign != 0u) return kDefaultNaN;

    if (exponent == 0) {
        do {
            exponent -= 1ll << 52;
            fraction <<= 1;
        } while ((fraction & (1ll << 52)) == 0);
        fraction &= static_cast<s64>(kFraction);
        exponent += 1ll << 52;
    }

    const s64 odd = exponent & (1ll << 52);
    exponent = ((0x3ffll << 52) - ((exponent - (0x3fell << 52)) / 2)) & static_cast<s64>(kExponent);

    const unsigned index = static_cast<unsigned>((odd | fraction) >> 37);  // 16 bits
    const Segment& segment = kReciprocalRoot[index >> 11];
    const s64 mantissa = segment.base + segment.slope * static_cast<s64>(index & 2047u);
    return static_cast<u64>(exponent) | (static_cast<u64>(mantissa) << 26);
}

}  // namespace estimate
}  // namespace ppc
}  // namespace gekko
