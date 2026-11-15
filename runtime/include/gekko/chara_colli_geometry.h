#pragma once

// The geometry of the CharaColli intrinsics' broad phase (gekko/chara_colli_intrinsic.h): RwV3d helpers that run the
// guest's single-precision operations on stored words, and the 0x8003C688 squared distance from a point to a segment.
// `distance_squared` takes a plain-float route when every operand and every intermediate is an ordinary single
// (gekko/fast_single.h) and the original ppc::fps_* sequence otherwise (`distance_squared_exact`, the unchanged
// original). Both leave identical registers: the result, CR0 and FPSCR[FPCC] of the last compare.

#include "gekko/cpu.h"
#include "gekko/fast_single.h"
#include "gekko/ppc_ops.h"
#include "gekko/types.h"

namespace gekko::chara_colli_geometry {

constexpr u32 kSegmentConstant = 0xFFFF8418u;  // r2 - 0x7BE8: the 0x8003C688 projection bound

// lfs and stfs: the guest's conversions at every float load and store.
GEKKO_ALWAYS_INLINE u64 lfs(u32 word) noexcept { return single_to_double_bits(word); }
GEKKO_ALWAYS_INLINE u32 stfs(u64 bits) noexcept { return double_to_single_bits(bits); }

// An RwV3d as three stored words (a helper's stfs results in the guest's dead frame).
struct V3 {
    u32 x, y, z;
};
GEKKO_ALWAYS_INLINE V3 read3(Cpu& c, u32 address) {
    const u32 x = c.mem_read32(address);
    const u32 y = c.mem_read32(address + 4u);
    return V3{x, y, c.mem_read32(address + 8u)};
}
// 0x800091EC (lfs, lfs, stfs, lfs, stfs, stfs): a copy through the FPRs.
GEKKO_ALWAYS_INLINE V3 copy(const V3& v) noexcept { return V3{stfs(lfs(v.x)), stfs(lfs(v.y)), stfs(lfs(v.z))}; }
// 0x8040DDC4 RwV3dSub(out, a, b): fsubs x, y, z.
GEKKO_ALWAYS_INLINE V3 sub(Cpu& c, const V3& a, const V3& b) {
    const u64 x = ppc::fps_sub(c, lfs(a.x), lfs(b.x));
    const u64 y = ppc::fps_sub(c, lfs(a.y), lfs(b.y));
    const u64 z = ppc::fps_sub(c, lfs(a.z), lfs(b.z));
    return V3{stfs(x), stfs(y), stfs(z)};
}
// 0x8040DE20 RwV3dAdd(out, a, b): fadds x, y, z.
GEKKO_ALWAYS_INLINE V3 add(Cpu& c, const V3& a, const V3& b) {
    const u64 x = ppc::fps_add(c, lfs(a.x), lfs(b.x));
    const u64 y = ppc::fps_add(c, lfs(a.y), lfs(b.y));
    const u64 z = ppc::fps_add(c, lfs(a.z), lfs(b.z));
    return V3{stfs(x), stfs(y), stfs(z)};
}
// 0x8040DDF8 RwV3dScale(out, v, f1): fmuls x, y, z.
GEKKO_ALWAYS_INLINE V3 scale(Cpu& c, const V3& v, u64 s) {
    const u64 x = ppc::fps_mul(c, lfs(v.x), s);
    const u64 y = ppc::fps_mul(c, lfs(v.y), s);
    const u64 z = ppc::fps_mul(c, lfs(v.z), s);
    return V3{stfs(x), stfs(y), stfs(z)};
}
// 0x8040DFE0 RwV3dDotProduct(a, b) -> f1: fmuls y, fmadds x, fmadds z.
GEKKO_ALWAYS_INLINE u64 dot(Cpu& c, const V3& a, const V3& b) {
    const u64 y = ppc::fps_mul(c, lfs(a.y), lfs(b.y));
    const u64 xy = ppc::fps_madd(c, lfs(a.x), lfs(b.x), y);
    return ppc::fps_madd(c, lfs(a.z), lfs(b.z), xy);
}
// 0x8040DF7C (|v|^2) -> f1: fmuls x, y, z; fadds x + y; fadds z + (x + y).
GEKKO_ALWAYS_INLINE u64 length_squared(Cpu& c, const V3& v) {
    const u64 x = ppc::fps_mul(c, lfs(v.x), lfs(v.x));
    const u64 y = ppc::fps_mul(c, lfs(v.y), lfs(v.y));
    const u64 z = ppc::fps_mul(c, lfs(v.z), lfs(v.z));
    const u64 xy = ppc::fps_add(c, x, y);
    return ppc::fps_add(c, z, xy);
}

// 0x8003C688 (centre, segment {a, b}, out = null) -> f1: the squared distance from the centre to the segment (the
// original sequence).
inline u64 distance_squared_exact(Cpu& c, const V3& centre, const V3& a, const V3& b) {
    const V3 d = copy(sub(c, b, a));
    const V3 w = copy(sub(c, centre, a));
    const u64 along = dot(c, w, d);  // f31
    ppc::fp_compare(c, 0u, along, lfs(c.mem_read32(c.gpr[2] + kSegmentConstant)), true);
    if (!ppc::cr_bit(c, 1u))  // not past a: |w|^2
        return length_squared(c, w);
    const u64 span = dot(c, d, d);
    ppc::fp_compare(c, 0u, along, span, true);
    if (ppc::cr_bit(c, 1u) || ppc::cr_bit(c, 2u))  // past b: 0x8040E1D0 (centre, b)
        return length_squared(c, sub(c, centre, b));
    const u64 t = ppc::fps_div(c, along, span);
    const V3 nearest = copy(add(c, a, scale(c, d, t)));
    return length_squared(c, sub(c, centre, nearest));
}
// The plain-float route of distance_squared. Returns false (nothing written) when an operand or an intermediate is
// not an ordinary single; `out` is the FPR image of f1.
GEKKO_ALWAYS_INLINE bool distance_squared_fast(Cpu& c, const V3& centre, const V3& a, const V3& b, u32 bound_word,
                                               u64& out) {
    fast::Check k;
    const float ax = k.in(a.x), ay = k.in(a.y), az = k.in(a.z);
    const float bx = k.in(b.x), by = k.in(b.y), bz = k.in(b.z);
    const float cx = k.in(centre.x), cy = k.in(centre.y), cz = k.in(centre.z);
    const float limit = k.in(bound_word);
    const float dx = k.out(bx - ax), dy = k.out(by - ay), dz = k.out(bz - az);
    const float wx = k.out(cx - ax), wy = k.out(cy - ay), wz = k.out(cz - az);
    float along = k.out(wy * dy);
    along = k.out(fast::fma(wx, dx, along));
    along = k.out(fast::fma(wz, dz, along));
    u32 flags = fast::compare_flags(along, limit);
    float vx, vy, vz;
    if (flags != 4u) {  // not past a: |w|^2
        vx = wx, vy = wy, vz = wz;
    } else {
        float span = k.out(dy * dy);
        span = k.out(fast::fma(dx, dx, span));
        span = k.out(fast::fma(dz, dz, span));
        flags = fast::compare_flags(along, span);
        if (flags != 8u) {  // past b: 0x8040E1D0 (centre, b)
            vx = k.out(cx - bx), vy = k.out(cy - by), vz = k.out(cz - bz);
        } else {
            const float t = k.out(along / span);
            const float nx = k.out(ax + k.out(dx * t)), ny = k.out(ay + k.out(dy * t)), nz = k.out(az + k.out(dz * t));
            vx = k.out(cx - nx), vy = k.out(cy - ny), vz = k.out(cz - nz);
        }
    }
    const float xx = k.out(vx * vx), yy = k.out(vy * vy), zz = k.out(vz * vz);
    const float sum = k.out(zz + k.out(xx + yy));
    if (!k.ok())
        return false;
    fast::put_compare(c, 0u, flags);
    out = fast::fpr_of(sum);
    return true;
}

GEKKO_ALWAYS_INLINE u64 distance_squared(Cpu& c, const V3& centre, const V3& a, const V3& b) {
    if constexpr (fast::kAvailable) {
        u64 out;
        if (distance_squared_fast(c, centre, a, b, c.mem_read32(c.gpr[2] + kSegmentConstant), out))
            return out;
    }
    return distance_squared_exact(c, centre, a, b);
}

}  // namespace gekko::chara_colli_geometry
