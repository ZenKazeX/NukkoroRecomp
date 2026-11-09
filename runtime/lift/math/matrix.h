#pragma once

#include <cstddef>
#include "math/floating_point.h"

namespace shadow::math {

inline constexpr std::size_t kAffineMatrixBytes = 12 * sizeof(float);

// Copy twelve float object representations. Both complete spans must be valid.
// This operation is independent of their byte order and does not evaluate the
// floats (including NaNs). Forward overlapping elements retain sequential copy
// semantics; disjoint buffers, self-copy and backward overlap are also valid.
// Unaligned storage is permitted. No CPU, address space or allocation is owned.
void copy_affine_matrix(void* destination, const void* source) noexcept;

// Compose row-major 3x4 affine matrices into left. Each span contains twelve
// floats; self-aliasing and partial float-element overlap are supported.
// Product order is y, fused x, fused z, then a separate translation addition.
// Requires round-to-nearest with host DAZ/FTZ disabled. Flush mode explicitly
// zeros subnormal operands and exact results before rounding, retaining sign.
void compose_affine_matrix(float* left, const float* right, SubnormalMode mode) noexcept;

}  // namespace shadow::math
