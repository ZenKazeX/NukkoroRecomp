// Native pose math - see pose_math.h. Register-order transliteration of the float arithmetic (the order IS the
// semantics for bit-exact results); fused PPC operations map to std::fma / std::fmaf:
//   fmadd(s)  a*c+b      fmsub(s)  a*c-b      fnmsub(s) -(a*c-b)
#include "motion/pose_math.h"
#include "math/msl.h"
#include <cmath>
#include <cstring>
#include <limits>

namespace shadow::motion {

namespace {
inline float fmadds(float a, float c, float b) { return std::fmaf(a, c, b); }
inline float fmsubs(float a, float c, float b) { return std::fmaf(a, c, -b); }
inline float fnmsubs(float a, float c, float b) { return -std::fmaf(a, c, -b); }
inline double fmadd(double a, double c, double b) { return std::fma(a, c, b); }
inline double fmsub(double a, double c, double b) { return std::fma(a, c, -b); }
inline double fnmsub(double a, double c, double b) { return -std::fma(a, c, -b); }
inline std::uint32_t hi(double d) { std::uint64_t u; std::memcpy(&u, &d, 8); return std::uint32_t(u >> 32); }
inline double from_words(std::uint32_t h, std::uint32_t l) { const std::uint64_t u = (std::uint64_t(h) << 32) | l; double d; std::memcpy(&d, &u, 8); return d; }
inline double bitsd(std::uint64_t u) { double d; std::memcpy(&d, &u, 8); return d; }
// fctiwz: round toward zero to signed 32 bits, saturating; NaN -> INT_MIN (PowerPC rule)
inline std::int32_t fctiwz(double v) {
    if (std::isnan(v)) return std::numeric_limits<std::int32_t>::min();
    if (v >= 2147483647.0) return std::numeric_limits<std::int32_t>::max();
    if (v <= -2147483648.0) return std::numeric_limits<std::int32_t>::min();
    return std::int32_t(v);
}

}  // namespace

double game_sin(double x) { return math::msl::sin(x); }                // 803B2AFC (math/msl.h)

double game_cos(double x) { return math::msl::cos(x); }                // 803B2594 (math/msl.h)

void sincos_f32(float radians, float& s, float& c) {                 // 80031568
    s = float(game_sin(double(radians)));
    c = float(game_cos(double(radians)));
}

QuantisedAngle quantise_angle(float radians) {                       // 80420B9C / 80420DC0 per axis
    const float turns = (65536.0f * radians) / 6.28318548f;
    const std::int32_t i = fctiwz(double(turns));
    if (i == 0) return {false, 0.0f};
    const float u = float(std::uint32_t(i) & 0xFFFFu);
    return {true, (6.28318548f * u) / 65536.0f};
}

// ------------------------------------------------------------------------------------------------ matrices
Mtx34 mtx_identity() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}; }

void mtx_translate(Mtx34& m, const Vec3& v) {                       // 8040C5EC
    for (int r = 0; r < 3; ++r) {
        float f0 = m[4 * r + 1] * v[1];
        f0 = fmadds(m[4 * r + 0], v[0], f0);
        f0 = fmadds(m[4 * r + 2], v[2], f0);
        m[4 * r + 3] = m[4 * r + 3] + f0;
    }
}

void mtx_scale(Mtx34& m, const Vec3& v) {                           // 8040C7E0
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) m[4 * r + c] = m[4 * r + c] * v[c];
}

void mtx_multiply(Mtx34& m, const Mtx34& R) {                       // 8040CB14 (via the 8040CD04 copy)
    const Mtx34 T = m;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            float f = T[4 * r + 1] * R[4 + c];
            f = fmadds(T[4 * r + 0], R[c], f);
            m[4 * r + c] = fmadds(T[4 * r + 2], R[8 + c], f);
        }
        float f = T[4 * r + 1] * R[7];
        f = fmadds(T[4 * r + 0], R[3], f);
        f = fmadds(T[4 * r + 2], R[11], f);
        m[4 * r + 3] = T[4 * r + 3] + f;
    }
}

void mtx_rotate_z(Mtx34& m, float a) {                              // 8040C85C (8040C110)
    float s, c; sincos_f32(a, s, c);
    mtx_multiply(m, {c, -s, 0, 0, s, c, 0, 0, 0, 0, 1, 0});
}
void mtx_rotate_y(Mtx34& m, float a) {                              // 8040C898 (8040C188)
    float s, c; sincos_f32(a, s, c);
    mtx_multiply(m, {c, 0, s, 0, 0, 1, 0, 0, -s, 0, c, 0});
}
void mtx_rotate_x(Mtx34& m, float a) {                              // 8040C8D4 (8040C200)
    float s, c; sincos_f32(a, s, c);
    mtx_multiply(m, {1, 0, 0, 0, 0, c, -s, 0, 0, s, c, 0});
}

void mtx_invert(Mtx34& m) {                                         // 8040CD24
    const float f8 = m[4], f4 = m[10], f5 = m[8];
    float f0 = f8 * f4;
    float f3 = m[6];
    const float f7 = m[9], f6 = m[5];
    float f2 = f3 * f7;
    float f10 = m[1];
    float f1 = fmsubs(f3, f5, f0);
    float f9 = m[0];
    float f12 = f6 * f5;
    float f11 = m[2];
    float f23 = fmsubs(f6, f4, f2);
    f1 = f10 * f1;
    f2 = fmsubs(f8, f7, f12);
    f1 = fmadds(f9, f23, f1);
    float f24 = fmadds(f11, f2, f1);
    if (0.0f == f24) return;
    const float f26 = m[7];
    f0 = f11 * f7;
    f1 = f3 * f5;
    float f13 = f11 * f26;
    float f27 = m[3];
    const float f28 = 1.0f / f24;
    const float f25 = m[11];
    const float f30 = f10 * f26;
    f0 = fnmsubs(f10, f4, f0);
    f24 = f28 * f23;
    float f22 = f10 * f3;
    f23 = f7 * f13;
    f12 = f11 * f6;
    m[0] = f24;
    float f29 = f28 * f0;
    f0 = f11 * f5;
    f24 = f22 - f12;
    f1 = fnmsubs(f8, f4, f1);
    m[1] = f29;
    f0 = fmsubs(f9, f4, f0);
    float f31 = f28 * f24;
    f24 = f10 * f5;
    f29 = f27 * f6;
    f23 = fmadds(f25, f22, f23);
    m[2] = f31;
    f24 = fnmsubs(f9, f7, f24);
    f31 = f27 * f3;
    f23 = fmadds(f4, f29, f23);
    f22 = f9 * f3;
    f13 = f5 * f13;
    f23 = fnmsubs(f7, f31, f23);
    f3 = f11 * f8;
    f11 = f27 * f8;
    f27 = fnmsubs(f25, f12, f23);
    f12 = fmadds(f25, f22, f13);
    f13 = f22 - f3;
    f27 = fnmsubs(f4, f30, f27);
    f12 = fmadds(f4, f11, f12);
    f13 = -f13;
    f27 = -f27;
    f12 = fnmsubs(f5, f31, f12);
    f22 = f9 * f6;
    float f6b = f5 * f30;
    f27 = f28 * f27;
    f23 = f10 * f8;
    f6b = fmadds(f25, f22, f6b);
    f10 = f28 * f1;
    m[3] = f27;
    const float f8b = f22 - f23;
    f1 = fmadds(f7, f11, f6b);
    f9 = f9 * f26;
    m[4] = f10;
    f3 = fnmsubs(f25, f3, f12);
    const float f6c = f28 * f0;
    f0 = fnmsubs(f5, f29, f1);
    f1 = fnmsubs(f4, f9, f3);
    f3 = f28 * f13;
    m[5] = f6c;
    f0 = fnmsubs(f25, f23, f0);
    f1 = f28 * f1;
    m[6] = f3;
    f3 = f28 * f2;
    f0 = fnmsubs(f7, f9, f0);
    m[7] = f1;
    f2 = f28 * f24;
    f1 = f28 * f8b;
    m[8] = f3;
    f0 = -f0;
    m[9] = f2;
    f0 = f28 * f0;
    m[10] = f1;
    m[11] = f0;
}

void mtx_set_rotation(Mtx34& dst, const Mtx34& src) {               // 80412438
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) dst[4 * r + c] = src[4 * r + c];
}
Vec3 mtx_translation(const Mtx34& m) { return {m[3], m[7], m[11]}; }   // 80412520

Vec3 mtx_rotate_vector(const Mtx34& m, const Vec3& v) {             // 804125CC
    Vec3 o;
    for (int r = 0; r < 3; ++r) {
        float f0 = v[1] * m[4 * r + 1];
        f0 = fmadds(v[0], m[4 * r + 0], f0);
        o[r] = fmadds(v[2], m[4 * r + 2], f0);
    }
    return o;
}


// ------------------------------------------------------------------------------------------------ phase 13: blend math
namespace {
inline float qnan_f() { const std::uint32_t u = 0x7FFFFFFFu; float f; std::memcpy(&f, &u, 4); return f; }   // 805EED78
}  // namespace

float game_sqrtf(float xf) {                                          // 8000FF04
    const double x = xf;
    if (x > 0.0) {
        double y = 1.0 / std::sqrt(x);                                // frsqrte (reference model)
        for (int i = 0; i < 2; ++i) {
            double f0 = y * y;
            y = 0.5 * y;
            f0 = fnmsub(x, f0, 3.0);
            y = y * f0;
        }
        double f0 = y * y;
        y = 0.5 * y;
        f0 = fnmsub(x, f0, 3.0);
        f0 = y * f0;
        return float(x * f0);
    }
    if (x < 0.0) return qnan_f();
    if (std::isnan(x)) return qnan_f();
    return xf;                                                        // +-0
}

double game_acos(double x) { return math::msl::acos(x); }              // 803AF678 (math/msl.h)

float game_acosf(float x) { return float(game_acos(double(x))); }    // 801B2150
float game_sinf(float x) { return float(game_sin(double(x))); }       // 8000FFE8

Quat mtx_to_quat(const Mtx34& m) {                                    // 8042193C
    Quat q{};
    const float m0 = m[0], m5 = m[5], m10 = m[10];
    float f0 = m0 + m5;
    f0 = m10 + f0;
    const float t = 1.0f + f0;
    if (t >= 1.0f) {
        const float s = game_sqrtf(t);
        const float f2 = 2.0f * s;
        const float w = 0.5f * s;
        const float f7 = 1.0f / f2;
        q[0] = f7 * (m[9] - m[6]);
        q[1] = f7 * (m[2] - m[8]);
        q[2] = f7 * (m[4] - m[1]);
        q[3] = w;
        return q;
    }
    if (m0 > m5) {
        if (m0 > m10) {                                               // x major
            float g = m0 - m5; g = g - m10;
            const float s = game_sqrtf(1.0f + g);
            const float f7 = 0.5f * s;
            const float f5 = 1.0f / (4.0f * f7);
            q[0] = f7; q[1] = f5 * (m[4] + m[1]); q[2] = f5 * (m[8] + m[2]); q[3] = f5 * (m[9] - m[6]);
            return q;
        }
    } else if (m5 > m10) {                                            // y major
        float g = m5 - m10; g = g - m0;
        const float s = game_sqrtf(1.0f + g);
        const float f7 = 0.5f * s;
        const float f5 = 1.0f / (4.0f * f7);
        q[1] = f7; q[2] = f5 * (m[9] + m[6]); q[0] = f5 * (m[1] + m[4]); q[3] = f5 * (m[2] - m[8]);
        return q;
    }
    {                                                                 // z major
        float g = m10 - m0; g = g - m5;
        const float s = game_sqrtf(1.0f + g);
        const float f7 = 0.5f * s;
        const float f5 = 1.0f / (4.0f * f7);
        q[2] = f7; q[0] = f5 * (m[2] + m[8]); q[1] = f5 * (m[6] + m[9]); q[3] = f5 * (m[4] - m[1]);
    }
    return q;
}

Quat quat_slerp(const Quat& a, const Quat& b, float t) {             // 80421774
    float dot = a[1] * b[1];
    dot = fmadds(a[0], b[0], dot);
    dot = fmadds(a[2], b[2], dot);
    dot = fmadds(a[3], b[3], dot);
    float wa = 1.0f, wb;
    if (dot < 0.0f) { dot = -dot; wa = -wa; }
    if (dot < 0.9999f /* 0x3F7FF972 */) {
        const float ang = game_acosf(dot);
        const float inv = 1.0f / game_sinf(ang);
        wb = inv * game_sinf((1.0f - t) * ang);
        wa = wa * (inv * game_sinf(t * ang));
    } else {
        wa = wa * t;
        wb = 1.0f - t;
    }
    return {fmadds(b[0], wb, a[0] * wa), fmadds(b[1], wb, a[1] * wa), fmadds(b[2], wb, a[2] * wa), fmadds(b[3], wb, a[3] * wa)};
}

Mtx34 quat_to_mtx(const Quat& q) {                                    // 80421BB0
    const float f13 = q[3], f10 = q[0], f11 = q[1], f12 = q[2];
    const float f7 = 2.0f;
    float f2 = f7 * f13;
    float f1 = f7 * f10;
    float f6 = f10 * f11;
    float f5 = f13 * f12;
    const float f31 = fmsubs(f2, f13, 1.0f);
    float f9 = f10 * f12;
    float f8 = f13 * f11;
    float f3 = f6 + f5;
    float f4 = fmadds(f1, f10, f31);
    f2 = f9 - f8;
    f1 = f6 - f5;
    f5 = f7 * f11;
    Mtx34 m{};
    m[0] = f4;
    f4 = f7 * f3;
    f2 = f7 * f2;
    f3 = f9 + f8;
    m[4] = f4;
    f9 = f11 * f12;
    f8 = f13 * f10;
    f6 = f7 * f1;
    m[8] = f2;
    f1 = f7 * f12;
    f4 = f9 + f8;
    f2 = f9 - f8;
    m[1] = f6;
    f5 = fmadds(f5, f11, f31);
    f4 = f7 * f4;
    f3 = f7 * f3;
    m[5] = f5;
    f2 = f7 * f2;
    f1 = fmadds(f1, f12, f31);
    m[9] = f4;
    m[2] = f3;
    m[6] = f2;
    m[10] = f1;
    m[11] = 0.0f; m[7] = 0.0f; m[3] = 0.0f;
    return m;
}

void mtx_normalize_columns(Mtx34& m) {                                // 804121BC
    for (int c = 0; c < 3; ++c) {
        float f1 = m[std::size_t(c)] * m[std::size_t(c)];
        float f0 = m[std::size_t(4 + c)] * m[std::size_t(4 + c)];
        const float f2 = m[std::size_t(8 + c)] * m[std::size_t(8 + c)];
        f0 = f1 + f0;
        f1 = f2 + f0;
        const float len = game_sqrtf(f1);
        if (len > 1.1920929e-07f /* 805EEDA0 */) {
            const float inv = 1.0f / len;
            for (int r = 0; r < 3; ++r) m[std::size_t(4 * r + c)] = m[std::size_t(4 * r + c)] * inv;
        }
    }
}

Vec3 mtx_column_lengths(const Mtx34& m) {                            // 80412378
    Vec3 out;
    for (int c = 0; c < 3; ++c) {
        float f1 = m[std::size_t(c)] * m[std::size_t(c)];
        float f0 = m[std::size_t(4 + c)] * m[std::size_t(4 + c)];
        const float f2 = m[std::size_t(8 + c)] * m[std::size_t(8 + c)];
        f0 = f1 + f0;
        out[std::size_t(c)] = game_sqrtf(f2 + f0);
    }
    return out;
}

Mtx34 mtx_blend(const Mtx34& a, const Mtx34& b, float w) {           // 804215E0 -> 8042167C
    Mtx34 t = quat_to_mtx(quat_slerp(mtx_to_quat(a), mtx_to_quat(b), w));
    mtx_normalize_columns(t);
    const float f2 = 1.0f - w;
    for (int r = 0; r < 3; ++r) {
        const float f0 = f2 * b[std::size_t(4 * r + 3)];
        t[std::size_t(4 * r + 3)] = fmadds(w, a[std::size_t(4 * r + 3)], f0);
    }
    return t;
}

}  // namespace shadow::motion
