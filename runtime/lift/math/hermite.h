#pragma once
#include "math/floating_point.h"

namespace shadow::math {
// Times, values and independently authored incoming/outgoing slopes are values,
// not pointers into an asset image. Keys remain owned by their animation track.
struct HermiteKey {
    float time;
    float incoming_slope;
    float outgoing_slope;
    float value;
};

// Evaluate a segment, or its derivative with respect to time. Extrapolation is
// intentional; neither operation clamps time or changes the simulation clock.
// Preserves the recovered single-precision rounding and fused accumulation
// order. Requires round-to-nearest, host DAZ/FTZ off. Equal key times retain
// arithmetic division-by-zero behavior; no fabricated constant segment.
float hermite_value(const HermiteKey& left, const HermiteKey& right, float time,
                    SubnormalMode mode) noexcept;
float hermite_derivative(const HermiteKey& left, const HermiteKey& right, float time,
                         SubnormalMode mode) noexcept;
struct HermiteSample { float value, derivative; };
// Shares the segment powers when both outputs are required.
HermiteSample evaluate_hermite_segment(const HermiteKey& left, const HermiteKey& right, float time,
                                      SubnormalMode mode) noexcept;
}  // namespace shadow::math
