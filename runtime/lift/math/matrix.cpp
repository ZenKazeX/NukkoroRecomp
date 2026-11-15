#include "math/matrix.h"

#include <cstdint>
#include <cstring>
#include <cmath>
#if defined(__AVX2__) || defined(__FMA__)
#include <immintrin.h>
#endif

namespace shadow::math {

namespace {
using namespace detail;
float dot(const float* left, const float* right, unsigned column, SubnormalMode mode) noexcept {
    float result = multiply(left[1], right[4 + column], mode);
    result = fused(left[0], right[column], result, mode);
    return fused(left[2], right[8 + column], result, mode);
}

#if defined(__AVX2__) || defined(__FMA__)
// Accumulate lane validity and make one decision after the SIMD calculation.
// The same exceptional operands/results still take the scalar contract, but
// normal matrices no longer execute a scalar branch after every operation.
__m128 ordinary_mask(__m128 x) noexcept {
    const __m128 magnitude = _mm_and_ps(x, _mm_castsi128_ps(_mm_set1_epi32(0x7fffffff)));
    const __m128 normal = _mm_cmpgt_ps(magnitude, _mm_set1_ps(kMinNormal));
    const __m128 zeros = _mm_cmpeq_ps(magnitude, _mm_setzero_ps());
    const __m128 finite = _mm_cmplt_ps(magnitude, _mm_castsi128_ps(_mm_set1_epi32(0x7f800000)));
    return _mm_and_ps(_mm_or_ps(normal, zeros), finite);
}
bool compose_ordinary(const float* left, const float* right, float* output) noexcept {
    const __m128 r0 = _mm_loadu_ps(right), r1 = _mm_loadu_ps(right + 4), r2 = _mm_loadu_ps(right + 8);
    __m128 valid = _mm_and_ps(ordinary_mask(r0), _mm_and_ps(ordinary_mask(r1), ordinary_mask(r2)));
    for (unsigned row = 0; row < 3; ++row) {
        const __m128 l = _mm_loadu_ps(left + row * 4);
        valid = _mm_and_ps(valid, ordinary_mask(l));
        __m128 result = _mm_mul_ps(_mm_shuffle_ps(l, l, _MM_SHUFFLE(1,1,1,1)), r1);
        valid = _mm_and_ps(valid, ordinary_mask(result));
        result = _mm_fmadd_ps(_mm_shuffle_ps(l, l, _MM_SHUFFLE(0,0,0,0)), r0, result);
        valid = _mm_and_ps(valid, ordinary_mask(result));
        result = _mm_fmadd_ps(_mm_shuffle_ps(l, l, _MM_SHUFFLE(2,2,2,2)), r2, result);
        valid = _mm_and_ps(valid, ordinary_mask(result));
        const __m128 translated = _mm_add_ps(result, _mm_shuffle_ps(l, l, _MM_SHUFFLE(3,3,3,3)));
        // Do not add zero to the linear columns: that can change a signed zero.
        result = _mm_blend_ps(result, translated, 8);
        valid = _mm_and_ps(valid, ordinary_mask(result));
        _mm_storeu_ps(output + row * 4, result);
    }
    return _mm_movemask_ps(valid) == 15;
}

// The disjoint-spans form (left and right do not overlap by even one float): both operands are loaded before the
// first store, so no copy is needed, and the product is computed a row per vector. Returns false having written
// nothing when the contract's slow cases may arise; the caller then runs the general code below, from the same inputs.
//  - Preserve (no flush): every operation is the IEEE operation the scalar contract performs, denormals included, so
//    only an output NaN (the contract's NaN rules: operand payload priority, the default NaN) needs the scalar code.
//    An input NaN or an invalid operation (inf * 0, inf - inf) always reaches an output as a NaN.
//  - Flush: an operand or an intermediate that is zero or a normal finite value above 2^-126, as for ordinary_mask.
// The running minimum / maximum of |bits| - 1 (unsigned) and |bits| answer all those tests at once at the end.
struct RangeCheck {
    __m128i low = _mm_set1_epi32(-1);  // min of (|bits| - 1): 0 < |v| <= kMinNormal gives a value below 0x00800000
    __m128i high = _mm_setzero_si128();  // max of |bits|: a NaN or an infinity reaches 0x7F800000
    void note(__m128 value) noexcept {
        const __m128i magnitude = _mm_and_si128(_mm_castps_si128(value), _mm_set1_epi32(0x7FFFFFFF));
        low = _mm_min_epu32(low, _mm_sub_epi32(magnitude, _mm_set1_epi32(1)));
        high = _mm_max_epi32(high, magnitude);
    }
    __m128 pass(__m128 value) noexcept {
        note(value);
        return value;
    }
    bool ordinary() const noexcept {
        const __m128i small = _mm_cmpeq_epi32(_mm_min_epu32(low, _mm_set1_epi32(0x007FFFFF)), low);  // low <= 0x7FFFFF
        const __m128i large = _mm_cmpgt_epi32(high, _mm_set1_epi32(0x7F7FFFFF));
        return _mm_movemask_ps(_mm_castsi128_ps(_mm_or_si128(small, large))) == 0;
    }
};

bool compose_disjoint(float* left, const float* right, SubnormalMode mode) noexcept {
    const __m128 r0 = _mm_loadu_ps(right), r1 = _mm_loadu_ps(right + 4), r2 = _mm_loadu_ps(right + 8);
    const __m128 l[3] = {_mm_loadu_ps(left), _mm_loadu_ps(left + 4), _mm_loadu_ps(left + 8)};
    __m128 result[3];
    if (mode == SubnormalMode::Preserve) {
        for (unsigned row = 0; row < 3; ++row) {
            const __m128 x = _mm_shuffle_ps(l[row], l[row], _MM_SHUFFLE(0, 0, 0, 0));
            const __m128 y = _mm_shuffle_ps(l[row], l[row], _MM_SHUFFLE(1, 1, 1, 1));
            const __m128 z = _mm_shuffle_ps(l[row], l[row], _MM_SHUFFLE(2, 2, 2, 2));
            const __m128 w = _mm_shuffle_ps(l[row], l[row], _MM_SHUFFLE(3, 3, 3, 3));
            __m128 sum = _mm_mul_ps(y, r1);
            sum = _mm_fmadd_ps(x, r0, sum);
            sum = _mm_fmadd_ps(z, r2, sum);
            // Do not add zero to the linear columns: that can change a signed zero.
            result[row] = _mm_blend_ps(sum, _mm_add_ps(sum, w), 8);
        }
        const __m128 unordered = _mm_or_ps(_mm_cmpunord_ps(result[0], result[0]),
                                           _mm_or_ps(_mm_cmpunord_ps(result[1], result[1]), _mm_cmpunord_ps(result[2], result[2])));
        if (_mm_movemask_ps(unordered) != 0) return false;
    } else {
        RangeCheck check;
        check.note(r0), check.note(r1), check.note(r2);
        for (unsigned row = 0; row < 3; ++row) {
            const __m128 x = _mm_shuffle_ps(l[row], l[row], _MM_SHUFFLE(0, 0, 0, 0));
            const __m128 y = _mm_shuffle_ps(l[row], l[row], _MM_SHUFFLE(1, 1, 1, 1));
            const __m128 z = _mm_shuffle_ps(l[row], l[row], _MM_SHUFFLE(2, 2, 2, 2));
            const __m128 w = _mm_shuffle_ps(l[row], l[row], _MM_SHUFFLE(3, 3, 3, 3));
            check.note(l[row]);
            __m128 sum = check.pass(_mm_mul_ps(y, r1));
            sum = check.pass(_mm_fmadd_ps(x, r0, sum));
            sum = check.pass(_mm_fmadd_ps(z, r2, sum));
            const __m128 translated = check.pass(_mm_add_ps(sum, w));
            result[row] = _mm_blend_ps(sum, translated, 8);
        }
        if (!check.ordinary()) return false;
    }
    _mm_storeu_ps(left, result[0]);
    _mm_storeu_ps(left + 4, result[1]);
    _mm_storeu_ps(left + 8, result[2]);
    return true;
}
#endif
}  // namespace

void copy_affine_matrix(void* destination, const void* source) noexcept {
    if (destination == source) return;
    const auto distance = reinterpret_cast<std::uintptr_t>(destination) -
                          reinterpret_cast<std::uintptr_t>(source);
    if (distance < kAffineMatrixBytes) {
        // A later source element can be overwritten by an earlier destination
        // element. Keep that defined order; a whole-span memmove would differ.
        auto* output = static_cast<unsigned char*>(destination);
        const auto* input = static_cast<const unsigned char*>(source);
        for (std::size_t i = 0; i < kAffineMatrixBytes; i += sizeof(float)) {
            std::uint32_t element;
            std::memcpy(&element, input + i, sizeof(element));
            std::memcpy(output + i, &element, sizeof(element));
        }
    } else {
        std::memmove(destination, source, kAffineMatrixBytes);
    }
}

void compose_affine_matrix(float* left, const float* right, SubnormalMode mode) noexcept {
#if defined(__AVX2__) || defined(__FMA__)
    {
        const auto distance = reinterpret_cast<std::uintptr_t>(left) - reinterpret_cast<std::uintptr_t>(right);
        // |left - right| >= 48 bytes: the spans share no float.
        if (distance + (kAffineMatrixBytes - 1) > 2 * (kAffineMatrixBytes - 1) && compose_disjoint(left, right, mode))
            return;
    }
#endif
    float a[12], b[12], output[12];
    std::memcpy(a, left, sizeof(a));
    std::memcpy(b, right, sizeof(b));
    // Only the first output precedes acquisition of the right translation.
    // Preserve that effect when float spans overlap at a translation element.
    const auto distance = reinterpret_cast<std::uintptr_t>(left) - reinterpret_cast<std::uintptr_t>(right);
    if (distance == 3 * sizeof(float) || distance == 7 * sizeof(float) || distance == 11 * sizeof(float))
        b[distance / sizeof(float)] = dot(a, b, 0, mode);
#if defined(__AVX2__) || defined(__FMA__)
    if (compose_ordinary(a, b, output)) {
        std::memcpy(left, output, sizeof(output));
        return;
    }
#endif
    for (unsigned row = 0; row < 3; ++row) for (unsigned column = 0; column < 4; ++column) {
        float result = dot(a + row * 4, b, column, mode);
        if (column == 3) result = add(a[row * 4 + 3], result, mode);
        output[row * 4 + column] = result;
    }
    std::memcpy(left, output, sizeof(output));
}

}  // namespace shadow::math
