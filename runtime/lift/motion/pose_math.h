// Native pose math (phase 12): the exact arithmetic the original animation evaluator uses.
//   Mtx34: 3 rows x 4 columns, row-major, translation in column 3 (the original's matrix layout is the same algebra).
//   sin / cos / acos: the game's double-precision library, math/msl.h (one native copy for every user; a host libm
//   is NOT used: it differs in the last bit for some inputs).
// Each function names the original it transcribes. Pure functions on values: no global state, no guest memory.
#pragma once
#include <array>
#include <cstdint>

namespace shadow::motion {

using Vec3 = std::array<float, 3>;
using Mtx34 = std::array<float, 12>;

Mtx34 mtx_identity();                                   // 8040C748 (copy of the runtime identity)
void mtx_translate(Mtx34& m, const Vec3& v);            // 8040C5EC  m = m * T(v)
void mtx_scale(Mtx34& m, const Vec3& v);                // 8040C7E0  m = m * S(v)
void mtx_multiply(Mtx34& m, const Mtx34& r);            // 8040CB14  m = m * r
void mtx_rotate_z(Mtx34& m, float radians);             // 8040C85C  m = m * Rz
void mtx_rotate_y(Mtx34& m, float radians);             // 8040C898  m = m * Ry
void mtx_rotate_x(Mtx34& m, float radians);             // 8040C8D4  m = m * Rx
void mtx_invert(Mtx34& m);                              // 8040CD24  in place; unchanged when the determinant is 0
void mtx_set_rotation(Mtx34& dst, const Mtx34& src);    // 80412438  dst 3x3 = src 3x3 (translation kept)
Vec3 mtx_translation(const Mtx34& m);                   // 80412520
Vec3 mtx_rotate_vector(const Mtx34& m, const Vec3& v);  // 804125CC  R(m) * v
inline Vec3 vec_sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }   // 8040DDC4

void sincos_f32(float radians, float& s, float& c);     // 80031568 (double sin / cos, then frsp)
double game_sin(double x);                              // 803B2AFC
double game_cos(double x);                              // 803B2594

// Angle quantisation of the Euler stage (80420B9C / 80420DC0): radians -> signed 32-bit turn units (65536 per turn,
// truncated) -> nonzero ? (u16 units) * 2pi / 65536 : no rotation.
struct QuantisedAngle { bool apply; float radians; };
QuantisedAngle quantise_angle(float radians);

// ---- phase 13: the blend math (multi-channel evaluators and channel-matrix blend)
using Quat = std::array<float, 4>;                      // x, y, z, w
float game_sqrtf(float x);                              // 8000FF04 (Newton on frsqrte; frsqrte modelled exactly as the
                                                        // reference / translated runtime does: 1 / sqrt in double)
double game_acos(double x);                             // 803AF678 (fdlibm acos, fused; sqrt = 803B2D2C)
float game_acosf(float x);                              // 801B2150 (acos then frsp)
float game_sinf(float x);                               // 8000FFE8 (803B2AFC then frsp)
Quat mtx_to_quat(const Mtx34& m);                       // 8042193C
Quat quat_slerp(const Quat& a, const Quat& b, float t); // 80421774: a weighted by t, b by 1 - t (shortest arc)
Mtx34 quat_to_mtx(const Quat& q);                       // 80421BB0 (translation 0)
void mtx_normalize_columns(Mtx34& m);                   // 804121BC (3x3 columns to unit length when > FLT_EPSILON)
Vec3 mtx_column_lengths(const Mtx34& m);                // 80412378
Mtx34 mtx_blend(const Mtx34& a, const Mtx34& b, float w);   // 804215E0: rotation slerp(a, b, w) (8042167C), translation
                                                            // w * a + (1 - w) * b

}  // namespace shadow::motion
