#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#if defined(__AVX2__) || defined(__FMA__)
#include <immintrin.h>
#endif

namespace shadow::math {
enum class SubnormalMode { Preserve, Flush };
namespace detail {
constexpr float kMinNormal = 0x1p-126f;
constexpr std::uint32_t kSign = std::uint32_t{1} << 31;
inline std::uint32_t bits(float value) noexcept {
    std::uint32_t result;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}
inline float value(std::uint32_t word) noexcept {
    float result;
    std::memcpy(&result, &word, sizeof(result));
    return result;
}
inline bool nan(float x) noexcept { return (bits(x) & ~kSign) > 0x7f800000u; }
inline float quiet(float x) noexcept { return value(bits(x) | 0x00400000u); }
inline float zero(float x) noexcept { return value(bits(x) & kSign); }
inline float operand(float x, SubnormalMode mode) noexcept {
    return mode == SubnormalMode::Flush && (bits(x) & ~kSign) < 0x00800000u ? zero(x) : x;
}
inline float multiply(float a, float c, SubnormalMode mode) noexcept {
    if (nan(a)) return quiet(a);
    if (nan(c)) return quiet(c);
    const double exact = double(operand(a, mode)) * double(operand(c, mode));
    if (std::isnan(exact)) return value(0x7fc00000u);
    if (mode == SubnormalMode::Flush && std::fabs(exact) < kMinNormal)
        return std::copysign(0.0f, float(exact));
    return float(exact);
}
inline float fused(float a, float c, float b, SubnormalMode mode) noexcept {
    // The accumulator has priority over the second product operand.
    if (nan(a)) return quiet(a);
    if (nan(b)) return quiet(b);
    if (nan(c)) return quiet(c);
    a = operand(a, mode); b = operand(b, mode); c = operand(c, mode);
#if defined(__AVX2__) || defined(__FMA__)
    const float rounded = _mm_cvtss_f32(_mm_fmadd_ss(_mm_set_ss(a), _mm_set_ss(c), _mm_set_ss(b)));
#else
    const float rounded = std::fma(a, c, b);
#endif
    if (nan(rounded)) return value(0x7fc00000u);
    if (mode == SubnormalMode::Flush && std::fabs(rounded) <= kMinNormal) {
        // Two float significands multiply exactly in a double. Two-sum's
        // residual resolves a value just below the normal boundary that the
        // double sum rounded onto it; no general extended-precision engine.
        const double product = double(a) * double(c);
        const double sum = product + double(b);
        const double moved = sum - product;
        const double residual = (product - (sum - moved)) + (double(b) - moved);
        if (std::fabs(sum) < kMinNormal || (std::fabs(sum) == kMinNormal &&
            (std::signbit(sum) ? residual > 0 : residual < 0))) return zero(rounded);
    }
    return rounded;
}
inline float add(float a, float b, SubnormalMode mode) noexcept {
    if (nan(a)) return quiet(a);
    if (nan(b)) return quiet(b);
    const double sum = double(operand(a, mode)) + double(operand(b, mode));
    if (std::isnan(sum)) return value(0x7fc00000u);
    if (mode == SubnormalMode::Flush && std::fabs(sum) < kMinNormal)
        return std::copysign(0.0f, float(sum));
    return float(sum);
}

inline float subtract(float a, float b, SubnormalMode mode) noexcept {
    if (nan(a)) return quiet(a);
    if (nan(b)) return quiet(b);
    const double result = double(operand(a, mode)) - double(operand(b, mode));
    if (std::isnan(result)) return value(0x7fc00000u);
    if (mode == SubnormalMode::Flush && std::fabs(result) < kMinNormal)
        return std::copysign(0.0f, float(result));
    return float(result);
}
inline float divide(float a, float b, SubnormalMode mode) noexcept {
    if (nan(a)) return quiet(a);
    if (nan(b)) return quiet(b);
    const double result = double(operand(a, mode)) / double(operand(b, mode));
    if (std::isnan(result)) return value(0x7fc00000u);
    if (mode == SubnormalMode::Flush && std::fabs(result) < kMinNormal)
        return std::copysign(0.0f, float(result));
    return float(result);
}

// Double fdiv. Flush (FPSCR[NI]): a denormal operand reads as a zero of its sign, and a quotient whose exact value
// is below 2^-1022 is a zero of its sign (tininess before rounding). A NaN operand is quieted (frA first); an
// invalid quotient is the default QNaN 0x7FF8000000000000.
inline std::uint64_t bits(double value) noexcept {
    std::uint64_t result;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}
inline double value64(std::uint64_t word) noexcept {
    double result;
    std::memcpy(&result, &word, sizeof(result));
    return result;
}
inline bool nan(double x) noexcept { return (bits(x) << 1) > 0xffe0000000000000ull; }
inline double operand(double x, SubnormalMode mode) noexcept {
    return mode == SubnormalMode::Flush && (bits(x) << 1) < 0x0020000000000000ull
               ? value64(bits(x) & 0x8000000000000000ull) : x;
}
inline double divide(double a, double b, SubnormalMode mode) noexcept {
    if (nan(a)) return value64(bits(a) | 0x0008000000000000ull);
    if (nan(b)) return value64(bits(b) | 0x0008000000000000ull);
    a = operand(a, mode);
    b = operand(b, mode);
    const double result = a / b;
    if (result != result) return value64(0x7ff8000000000000ull);
    constexpr double kMinNormal64 = 0x1p-1022;
    if (mode == SubnormalMode::Flush && std::fabs(result) <= kMinNormal64) {
        // On the boundary the exact |a / b| < 2^-1022 decides: |a| 2^1022 < |b| (power-of-two scaling is exact).
        if (std::fabs(result) < kMinNormal64 || std::ldexp(std::fabs(a), 1022) < std::fabs(b))
            return value64(bits(result) & 0x8000000000000000ull);
    }
    return result;
}
}  // namespace detail
}  // namespace shadow::math
