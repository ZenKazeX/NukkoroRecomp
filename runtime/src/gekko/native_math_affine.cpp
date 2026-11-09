// The native owners of the 3x4 matrix leaves 0x8040C6CC / C5EC / C7E0 (gekko/native_math_affine.h).

#include "gekko/native_math_affine.h"

#include "gekko/cpu.h"
#include "gekko/gekko_estimate.h"
#include "gekko/ppc_ops.h"

namespace gekko::math_affine {

namespace {
using F = u64;
// lfs / stfs: a float word as the FPR holds it, and back.
GEKKO_ALWAYS_INLINE F L(u32 word) noexcept { return single_to_double_bits(word); }
GEKKO_ALWAYS_INLINE u32 S(F value) noexcept { return double_to_single_bits(value); }

// acc = m1 * p1; acc = m0 * p0 + acc; acc = m2 * p2 + acc: the original's order (fmuls, fmadds, fmadds).
GEKKO_ALWAYS_INLINE F dot(Cpu& c, F m0, F m1, F m2, F p0, F p1, F p2) {
    F acc = ppc::fps_mul(c, m1, p1);
    acc = ppc::fps_madd(c, m0, p0, acc);
    return ppc::fps_madd(c, m2, p2, acc);
}
}  // namespace

void serve_point(Cpu& c) {
    const u32 out = c.gpr[3], m = c.gpr[4], p = c.gpr[5];
    const F px = L(c.mem_read32(p)), py = L(c.mem_read32(p + 4u)), pz = L(c.mem_read32(p + 8u));
    u32 result[3];
    for (u32 row = 0; row < 3u; ++row) {
        const u32 r = m + 0x10u * row;
        const F acc = dot(c, L(c.mem_read32(r)), L(c.mem_read32(r + 4u)), L(c.mem_read32(r + 8u)), px, py, pz);
        result[row] = S(ppc::fps_add(c, L(c.mem_read32(r + 0xCu)), acc));
    }
    // all reads precede the first store in the original: out may alias m or p
    for (u32 row = 0; row < 3u; ++row)
        c.mem_write32(out + 4u * row, result[row]);
}

void serve_translate_by(Cpu& c) {
    const u32 m = c.gpr[3], v = c.gpr[4];
    const F vy = L(c.mem_read32(v + 4u)), vx = L(c.mem_read32(v)), vz = L(c.mem_read32(v + 8u));
    for (u32 row = 0; row < 3u; ++row) {
        const u32 r = m + 0x10u * row;
        const F acc = dot(c, L(c.mem_read32(r)), L(c.mem_read32(r + 4u)), L(c.mem_read32(r + 8u)), vx, vy, vz);
        c.mem_write32(r + 0xCu, S(ppc::fps_add(c, L(c.mem_read32(r + 0xCu)), acc)));
    }
}

void serve_scale_columns(Cpu& c) {
    const u32 m = c.gpr[3], v = c.gpr[4];
    const F v0 = L(c.mem_read32(v)), v1 = L(c.mem_read32(v + 4u)), v2 = L(c.mem_read32(v + 8u));
    for (u32 row = 0; row < 3u; ++row) {
        const u32 r = m + 0x10u * row;
        c.mem_write32(r, S(ppc::fps_mul(c, L(c.mem_read32(r)), v0)));
        c.mem_write32(r + 4u, S(ppc::fps_mul(c, L(c.mem_read32(r + 4u)), v1)));
        c.mem_write32(r + 8u, S(ppc::fps_mul(c, L(c.mem_read32(r + 8u)), v2)));
    }
}

void install(Cpu& cpu) {
    cpu.install_hle(kPoint, [](Cpu& c) { serve_point(c); return true; }, "matrix 3x4 point (native)");
    cpu.install_hle(kTranslateBy, [](Cpu& c) { serve_translate_by(c); return true; }, "matrix 3x4 translate-by (native)");
    cpu.install_hle(kScaleColumns, [](Cpu& c) { serve_scale_columns(c); return true; }, "matrix 3x4 scale columns (native)");
}

}  // namespace gekko::math_affine
