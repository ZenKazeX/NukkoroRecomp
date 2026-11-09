#pragma once

// The game's MSL math library (fdlibm compiled for the original CPU), native: docs/NATIVE_MSL_MATH.md.
//
// Each function returns the original's result bits for every input. It is the game's own algorithm, in its own
// operation order: every multiply-add the compiler fused is fused (one rounding), and NaN results follow the
// original CPU's rules (the first NaN operand quieted, a generated NaN 0x7FF8000000000000). `mode` is the caller's
// FPSCR[NI]. The game runs with it set (OSInit 80370E68 calls PPCSetFpNonIEEEMode 80370BE8). Flush treats a
// denormal operand as zero and flushes a tiny result, as the hardware does. The double routines are
// mode-independent except atan2 (its y / x). The float wrappers' rounding (frsp) depends on the mode.
// No CPU state, guest addresses, allocation or game time.

#include "math/floating_point.h"

namespace shadow::math::msl {

double sin(double x) noexcept;                                   // 803B2AFC
double cos(double x) noexcept;                                   // 803B2594
double atan(double x) noexcept;                                  // 803B2210
double tan(double x) noexcept;                                   // 803B2BD4
double atan2(double y, double x, SubnormalMode mode) noexcept;   // __ieee754_atan2 803AFAEC
double acos(double x) noexcept;                                  // 803AF678 (its domain error returns MSL's
                                                                 // __float_nan word as a double, no errno)

// frsp: the double rounded to single as the original CPU does (Flush: |x| < 2^-126 becomes a zero of its sign).
float round_to_single(double x, SubnormalMode mode) noexcept;
float sinf(float x, SubnormalMode mode) noexcept;                // 8000FFE8: sin, then frsp
float cosf(float x, SubnormalMode mode) noexcept;                // 8000F3B0: cos, then frsp
float atan2f(float y, float x, SubnormalMode mode) noexcept;     // 80016260: atan2, then frsp
float acosf(float x, SubnormalMode mode) noexcept;               // 801B2150: acos, then frsp
float atanf(float x, SubnormalMode mode) noexcept;               // 8000F3F8: atan, then frsp
float tanf(float x, SubnormalMode mode) noexcept;                // 8000F3D4: tan, then frsp

namespace detail {
// The internal routines, exposed for the contract test (tests/native_msl_tests.cpp).
struct Reduced {
    int n;
    double y0, y1;
};
Reduced rem_pio2(double x) noexcept;            // __ieee754_rem_pio2 803B0C74: x - n pi/2 = y0 + y1
double kernel_sin(double x, double y, int iy) noexcept;  // 803B1F5C
double kernel_cos(double x, double y) noexcept;           // 803B1014
double kernel_tan(double x, double y, int iy) noexcept;  // 803B1FFC
double scalbn(double x, int n, SubnormalMode mode) noexcept;  // 803B283C (its two multiplies read the mode;
                                                              // the library's own calls never meet a denormal)
double floor(double x) noexcept;                // 803B2668
// The two-over-pi digits (80515398, 66 words) and pi/2 in 24-bit pieces (80515530, 8 doubles), as compiled
// in, for the test to compare with main.dol.
const int* two_over_pi() noexcept;
const double* pio2_pieces() noexcept;
}  // namespace detail

}  // namespace shadow::math::msl
