#include "math/hermite.h"

namespace shadow::math {
namespace {
using namespace detail;
struct SegmentPowers { float elapsed, square, inverse, inverse_square; };
SegmentPowers powers(const HermiteKey& left, const HermiteKey& right, float time, SubnormalMode mode) noexcept {
    const float elapsed = subtract(time, left.time, mode);
    const float duration = subtract(right.time, left.time, mode);
    const float square = multiply(elapsed, elapsed, mode);
    const float inverse = divide(1.0f, duration, mode);
    return {elapsed, square, inverse, multiply(inverse, inverse, mode)};
}
float accumulate(const HermiteKey& left, const HermiteKey& right,
                 float left_value, float right_value, float left_slope, float right_slope,
                 SubnormalMode mode) noexcept {
    float result = multiply(right.value, right_value, mode);
    result = fused(left.value, left_value, result, mode);
    result = fused(left.outgoing_slope, left_slope, result, mode);
    return fused(right.incoming_slope, right_slope, result, mode);
}

float value_from_powers(const HermiteKey& left, const HermiteKey& right, const SegmentPowers& p,
                    SubnormalMode mode) noexcept {
    using namespace detail;
    const auto [elapsed, square, inverse, inverse_square] = p;
    const float cube = multiply(square, elapsed, mode);
    const float triple_square = multiply(3.0f, square, mode);
    const float square_over_duration = multiply(square, inverse, mode);
    const float cube_over_duration_squared = multiply(inverse_square, cube, mode);
    const float triple_normalized_square = multiply(triple_square, inverse_square, mode);
    const float double_cube = multiply(2.0f, cube_over_duration_squared, mode);
    const float right_slope = subtract(cube_over_duration_squared, square_over_duration, mode);
    const float double_normalized_cube = multiply(double_cube, inverse, mode);
    const float left_slope_tail = subtract(right_slope, square_over_duration, mode);
    const float left_value_tail = subtract(double_normalized_cube, triple_normalized_square, mode);
    const float left_slope = add(elapsed, left_slope_tail, mode);
    const float right_value = add(-double_normalized_cube, triple_normalized_square, mode);
    const float left_value = add(1.0f, left_value_tail, mode);
    return accumulate(left, right, left_value, right_value, left_slope, right_slope, mode);
}

float derivative_from_powers(const HermiteKey& left, const HermiteKey& right, const SegmentPowers& p,
                         SubnormalMode mode) noexcept {
    using namespace detail;
    const auto [elapsed, square, inverse, inverse_square] = p;
    const float six_square = multiply(6.0f, square, mode);
    const float six_elapsed = multiply(6.0f, elapsed, mode);
    const float double_elapsed = multiply(2.0f, elapsed, mode);
    const float six_normalized_square = multiply(six_square, inverse_square, mode);
    const float triple_square = multiply(3.0f, square, mode);
    const float six_elapsed_over_duration_squared = multiply(six_elapsed, inverse_square, mode);
    const float six_square_over_duration_cubed = multiply(inverse, six_normalized_square, mode);
    const float double_normalized_elapsed = multiply(double_elapsed, inverse, mode);
    const float triple_normalized_square = multiply(triple_square, inverse_square, mode);
    const float left_value = subtract(six_square_over_duration_cubed, six_elapsed_over_duration_squared, mode);
    const float right_slope = subtract(triple_normalized_square, double_normalized_elapsed, mode);
    const float right_value = add(-six_square_over_duration_cubed, six_elapsed_over_duration_squared, mode);
    const float left_slope_tail = subtract(right_slope, double_normalized_elapsed, mode);
    const float left_slope = add(1.0f, left_slope_tail, mode);
    return accumulate(left, right, left_value, right_value, left_slope, right_slope, mode);
}
}  // namespace

float hermite_value(const HermiteKey& left, const HermiteKey& right, float time, SubnormalMode mode) noexcept {
    return value_from_powers(left, right, powers(left, right, time, mode), mode);
}
float hermite_derivative(const HermiteKey& left, const HermiteKey& right, float time, SubnormalMode mode) noexcept {
    return derivative_from_powers(left, right, powers(left, right, time, mode), mode);
}
HermiteSample evaluate_hermite_segment(const HermiteKey& left, const HermiteKey& right, float time,
                                      SubnormalMode mode) noexcept {
    const auto p = powers(left, right, time, mode);
    return {value_from_powers(left, right, p, mode), derivative_from_powers(left, right, p, mode)};
}
}  // namespace shadow::math
