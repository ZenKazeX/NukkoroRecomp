#pragma once

// The game's 3x4 matrix / vector leaves (main.dol, PAL GUPP8P), owned natively (docs/agents/os-other.md step 15; docs/agents/
// opt-matrix.md). Matrices are rows 0x00 / 0x10 / 0x20 of x y z translation (single floats). Every floating-point operation is
// the original's, in the original's operand roles and order: a plain-float fast path with an exact ppc::fps_* fallback for any
// NaN or tiny value (fused multiply-adds with one rounding, single results), every stored float the original's stfs word.
//
//   0x8040C6CC point(r3 = out, r4 = m, r5 = p):  out = m x [p 1]
//       row i: ((m[i][1] * p.y  then  + m[i][0] * p.x  then  + m[i][2] * p.z)  +  m[i][3] added last as add(m[i][3], acc)
//       The point and the matrix are read in full before the first store, so out may alias either.
//   0x8040C668 rotate_vector(r3 = out, r4 = m, r5 = p): the same without the translation column and its add.
//   0x8040C5EC translate_by(r3 = m, r4 = v):     m[i][3] = m[i][3] + (m[i][0] * v.x + (m[i][1] * v.y ...)) i.e. the
//       translation column moved by the rotation part x v; v is read first, rows are read and stored one at a time.
//   0x8040C57C translate_by_floats(r3 = m, f1 = x, f2 = y, f3 = z): the same with the vector in f1..f3.
//   0x8040C7E0 scale_columns(r3 = m, r4 = v):    m[i][c] = m[i][c] * v[c] for the rotation part (c = 0..2, i = 0..2); v first.
//   0x8040C770 scale_floats(r3 = m, f1, f2, f3): the same with the factors in f1..f3.
//   0x8040DF7C squared_length(r3 = v) -> f1 (both halves) = (z*z) + ((x*x) + (y*y)), single operations.
//   0x8040C94C concat(r3 = out, r4 = a, r5 = b):  out = a x b (each rotation / translation element: m1*p1, then fma m0*p0, then
//       fma m2*p2, the translation column + a's translation last).
//   0x8040CD24 invert_in_place(r3 = m): cofactor inverse; the determinant (compared with the constant [r2 + 0x163C]) equal: m is
//       left as it is. 0x8040CF48 invert(r3 = out, r4 = m): the same into out; determinant equal: the 12-word copy (CTR = 0).
//
// What a call leaves, as the original's: the stored words, FPSCR[FPCC] and cr0 of the determinant compare, the dead frame the
// framed routines (concat, both inverses) write below SP (back chain, LR word, the callee-saved FPR save area f23 / f22 / f21 .. f31)
// and the restore's ps1 rounding. NOT kept: the volatile registers the original uses as scratch (r0, r4.., f0..f13, cr1..) and
// nothing else is written (no non-volatile register, LR unchanged).

#include "gekko/types.h"

#include <cstddef>

namespace gekko {
class Cpu;
}

namespace gekko::math_affine {

constexpr u32 kPoint = 0x8040C6CCu;
constexpr u32 kTranslateBy = 0x8040C5ECu;
constexpr u32 kScaleColumns = 0x8040C7E0u;
constexpr u32 kRotateVector = 0x8040C668u;
constexpr u32 kTranslateFloats = 0x8040C57Cu;
constexpr u32 kScaleFloats = 0x8040C770u;
constexpr u32 kSquaredLength = 0x8040DF7Cu;
constexpr u32 kConcat = 0x8040C94Cu;
constexpr u32 kInvertInPlace = 0x8040CD24u;
constexpr u32 kInvert = 0x8040CF48u;

void serve_point(Cpu& cpu);
void serve_translate_by(Cpu& cpu);
void serve_scale_columns(Cpu& cpu);
void serve_rotate_vector(Cpu& cpu);
void serve_translate_floats(Cpu& cpu);
void serve_scale_floats(Cpu& cpu);
void serve_squared_length(Cpu& cpu);
void serve_concat(Cpu& cpu);
void serve_invert_in_place(Cpu& cpu);
void serve_invert(Cpu& cpu);
// The original sequences alone (no fast path): the reference opt_intrinsics_diff_tests checks the three owners against.
void serve_point_exact(Cpu& cpu);
void serve_translate_by_exact(Cpu& cpu);
void serve_scale_columns_exact(Cpu& cpu);

// The two paths of each owner, exposed for the contract test (it counts which calls the fast path accepts). A fast function
// returns true when it served the call (words stored), false when nothing was stored or compared and the exact path must run.
namespace detail {
bool point_fast(Cpu& cpu, u32 out, u32 m, u32 p);
bool rotate_vector_fast(Cpu& cpu, u32 out, u32 m, u32 p);
bool translate_by_fast(Cpu& cpu, u32 m, float x, float y, float z);
bool scale_columns_fast(Cpu& cpu, u32 m, float x, float y, float z);
bool squared_length_fast(Cpu& cpu, u32 v, u64& result);
bool concat_fast(Cpu& cpu, u32 out, u32 a, u32 b);
bool invert_in_place_fast(Cpu& cpu, u32 r2, u32 m);
bool invert_fast(Cpu& cpu, u32 r2, u32 out, u32 m);
}  // namespace detail

struct Owners {
    struct Address {
        u32 address;
        const char* name;
    };
    static const Address* addresses(std::size_t& count) noexcept {
        static constexpr Address kAddresses[] = {{kPoint, "matrix 3x4 point transform"},
                                                 {kTranslateBy, "matrix 3x4 translate by rotated vector"},
                                                 {kScaleColumns, "matrix 3x4 scale columns"},
                                                 {kRotateVector, "matrix 3x4 rotate vector"},
                                                 {kTranslateFloats, "matrix 3x4 translate by floats"},
                                                 {kScaleFloats, "matrix 3x4 scale columns by floats"},
                                                 {kSquaredLength, "vec3 squared length"},
                                                 {kConcat, "matrix 3x4 concat"},
                                                 {kInvertInPlace, "matrix 3x4 invert in place"},
                                                 {kInvert, "matrix 3x4 invert"}};
        count = 10;
        return kAddresses;
    }
};

void install(Cpu& cpu);

}  // namespace gekko::math_affine
