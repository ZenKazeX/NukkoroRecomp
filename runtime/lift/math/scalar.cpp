#include "math/scalar.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#endif

namespace shadow::math {
namespace {
double from_bits(std::uint64_t bits) noexcept {
    double value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}
}  // namespace

SquareRootResult square_root(double value) noexcept {
    std::uint64_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    const std::uint64_t magnitude = bits & 0x7fffffffffffffffull;
    if (magnitude >= 0x7ff0000000000000ull) {
        if (magnitude > 0x7ff0000000000000ull)
            return {from_bits(bits | 0x0008000000000000ull), true};
        return {bits >> 63 ? from_bits(0x7ff8000000000000ull) : value, true};
    }
    if ((bits >> 63) && magnitude)
        return {from_bits(0x7fffffffe0000000ull), true};
#if defined(_M_X64) || defined(__x86_64__)
    const __m128d input = _mm_set_sd(value);
    return {_mm_cvtsd_f64(_mm_sqrt_sd(input, input)), false};
#else
    return {std::sqrt(value), false};
#endif
}

}  // namespace shadow::math
