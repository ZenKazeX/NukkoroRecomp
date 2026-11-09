#pragma once

// Three leaf routines of the game's 3x4 matrix family (main.dol, PAL GUPP8P), owned natively (docs/agents/os-other.md,
// step 15; the 0x8040C* family of docs/NATIVE_MATH_METHOD.md). Matrices are rows 0x00 / 0x10 / 0x20 of x y z translation
// (single floats). Every floating-point operation is the original's, in the original's operand roles and order, on the
// guest's operands (ppc::fps_*: fused multiply-adds with one rounding, single results, the FPSCR sticky bits the
// original's instructions set), every stored float the original's stfs word.
//
//   0x8040C6CC point(r3 = out, r4 = m, r5 = p):  out = m x [p 1]
//       row i: ((m[i][1] * p.y  then  + m[i][0] * p.x  then  + m[i][2] * p.z)  +  m[i][3] added last as add(m[i][3], acc)
//       The point and the matrix are read in full before the first store, so out may alias either.
//   0x8040C5EC translate_by(r3 = m, r4 = v):     m[i][3] = m[i][3] + (m[i][0] * v.x + (m[i][1] * v.y ...)) i.e. the
//       translation column moved by the rotation part x v; v is read first, rows are read and stored one at a time.
//   0x8040C7E0 scale_columns(r3 = m, r4 = v):    m[i][c] = m[i][c] * v[c] for the rotation part (c = 0..2, i = 0..2); v first.
//
// What a call leaves, as the original's: the stored words and FPSCR's sticky / FPRF bits. NOT kept: f0..f7, the volatile FPRs
// the original uses as scratch (the ABI makes them dead across the call; the owner leaves them alone), and nothing else
// is written (no stack, no non-volatile register, no CR, LR unchanged). Docs: step 15.

#include "gekko/types.h"

#include <cstddef>

namespace gekko {
class Cpu;
}

namespace gekko::math_affine {

constexpr u32 kPoint = 0x8040C6CCu;
constexpr u32 kTranslateBy = 0x8040C5ECu;
constexpr u32 kScaleColumns = 0x8040C7E0u;

void serve_point(Cpu& cpu);
void serve_translate_by(Cpu& cpu);
void serve_scale_columns(Cpu& cpu);

struct Owners {
    struct Address {
        u32 address;
        const char* name;
    };
    static const Address* addresses(std::size_t& count) noexcept {
        static constexpr Address kAddresses[] = {{kPoint, "matrix 3x4 point transform"},
                                                 {kTranslateBy, "matrix 3x4 translate by rotated vector"},
                                                 {kScaleColumns, "matrix 3x4 scale columns"}};
        count = 3;
        return kAddresses;
    }
};

void install(Cpu& cpu);

}  // namespace gekko::math_affine
