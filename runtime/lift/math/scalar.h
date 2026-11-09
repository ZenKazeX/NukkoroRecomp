#pragma once

namespace shadow::math {

struct SquareRootResult {
    double value;
    bool domain_error;
};

// Round-to-nearest square root for the game's scalar math API. Signed zero
// is preserved. Nonfinite inputs and negative nonzero inputs report a domain
// error; NaN payloads are retained and quieted. The caller owns error storage.
// Requires the process's normal IEEE environment (round-to-nearest, DAZ off).
SquareRootResult square_root(double value) noexcept;

}  // namespace shadow::math
