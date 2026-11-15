// The Mtn pose pass, native. Contract: include/gekko/native_mtn_pose.h.

#include "gekko/native_mtn_pose.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#include "gekko/cpu.h"
#include "gekko/gekko_estimate.h"
#include "gekko/native_guest_call.h"
#include "gekko/native_math_affine.h"
#include "gekko/native_mtn_track.h"
#include "gekko/native_se_math.h"
#include "gekko/native_vec_math.h"
#include "gekko/ppc_ops.h"
#include "math/matrix.h"
#include "math/msl.h"

namespace gekko::mtn_pose {
namespace {
using F = u64;  // an FPR as the original holds it: the double bits of a single value

// .sdata2 (r2 = 0x805FA780): the words of the pinned DOL (the contract test compares them with main.dol).
constexpr u32 kOne = 0x3F800000u;       // r2 + 0x1700 and + 0x1730 (and + 0x16B0)
constexpr u32 kZero = 0x00000000u;      // r2 + 0x1728
constexpr u32 kTwo = 0x40000000u;       // r2 + 0x172C
constexpr u32 kHalf = 0x3F000000u;      // r2 + 0x1738
constexpr u32 kNearOne = 0x3F7FF972u;   // r2 + 0x1734
constexpr u32 kTurn = 0x47800000u;      // r2 + 0x171C = 65536.0
constexpr u32 kTwoPi = 0x40C90FDBu;     // r2 + 0x1720
constexpr u32 kEpsilon = 0x34000000u;   // 0x805EEDA0 = 2^-23
constexpr u64 kMagic = 0x4330000080000000ull;  // lfd r2 + 0x1708
constexpr u64 kSign = 0x8000000000000000ull;
constexpr u32 kScratch = 0x805E23A0u;   // the pass scratch: +0xF0 matrices (0x30), +0x1B0 vectors (0xC), +0x1E0 / +0x1F0 / +0x200
                                        // flags, +0x210 weights
constexpr u32 kIdentity = 0x805E17E0u;  // the identity matrix 0x8040C748 copies
constexpr u32 kChannelWeights = 0x805E25B0u;

GEKKO_ALWAYS_INLINE F L(u32 word) noexcept { return single_to_double_bits(word); }
GEKKO_ALWAYS_INLINE F LM(Cpu& c, u32 address) { return single_to_double_bits(c.mem_read32(address)); }
GEKKO_ALWAYS_INLINE void SM(Cpu& c, u32 address, F value) { c.mem_write32(address, double_to_single_bits(value)); }
GEKKO_ALWAYS_INLINE shadow::math::SubnormalMode mode_of(const Cpu& c) noexcept {
    return (c.fpscr & kFpscrNI) != 0u ? shadow::math::SubnormalMode::Flush : shadow::math::SubnormalMode::Preserve;
}
GEKKO_ALWAYS_INLINE float as_float(F v) noexcept { return static_cast<float>(bits_to_double(v)); }
GEKKO_ALWAYS_INLINE F from_float(float f) noexcept { return single_to_double_bits(float_to_bits(f)); }

// fcmpo cr0, a, b: "LT" / "GT" (the NaN case raises the original's exception bits and is neither).
GEKKO_ALWAYS_INLINE bool lt_ordered(Cpu& c, F a, F b) noexcept {
    const double da = bits_to_double(a), db = bits_to_double(b);
    if (da < db) return true;
    if (da >= db) return false;
    ppc::fp_compare_unordered(c, a, b, true);
    return false;
}
GEKKO_ALWAYS_INLINE bool gt_ordered(Cpu& c, F a, F b) noexcept {
    const double da = bits_to_double(a), db = bits_to_double(b);
    if (da > db) return true;
    if (da <= db) return false;
    ppc::fp_compare_unordered(c, a, b, true);
    return false;
}
// fcmpu cr0, a, b -> "EQ" (bne is taken when it is not equal, a NaN included).
GEKKO_ALWAYS_INLINE bool eq_unordered(Cpu& c, F a, F b) noexcept {
    const double da = bits_to_double(a), db = bits_to_double(b);
    if (da == db) return true;
    if (da < db || da > db) return false;
    ppc::fp_compare_unordered(c, a, b, false);
    return false;
}

// 0x8040D198 (and the retired copy site): twelve words, forward.
GEKKO_ALWAYS_INLINE void copy12(Cpu& c, u32 dst, u32 src) {
    for (u32 i = 0; i < 12u; ++i) c.mem_write32(dst + 4u * i, c.mem_read32(src + 4u * i));
}
// 0x800091D0 / 0x800091EC: three floats through the FPRs (lfs, lfs, stfs, lfs, stfs, stfs).
GEKKO_ALWAYS_INLINE void copy3(Cpu& c, u32 dst, u32 src) {
    const F x = LM(c, src), y = LM(c, src + 4u);
    SM(c, dst, x);
    const F z = LM(c, src + 8u);
    SM(c, dst + 4u, y);
    SM(c, dst + 8u, z);
}

// The matrix stack object [r13 + 0x5F50]: +0 base, +8 index; entries 0x30 bytes.
GEKKO_ALWAYS_INLINE u32 stack_object(Cpu& c) { return c.mem_read32(c.gpr[13] + 0x5F50u); }
GEKKO_ALWAYS_INLINE u32 stack_top(Cpu& c) {  // 0x80412750
    const u32 obj = stack_object(c);
    return c.mem_read32(obj) + c.mem_read32(obj + 8u) * 0x30u;
}
GEKKO_ALWAYS_INLINE void stack_push(Cpu& c) {  // 0x8040D254
    const u32 obj = stack_object(c);
    const u32 index = c.mem_read32(obj + 8u), base = c.mem_read32(obj);
    c.mem_write32(obj + 8u, index + 1u);
    copy12(c, base + (index + 1u) * 0x30u, base + index * 0x30u);
}
GEKKO_ALWAYS_INLINE void stack_pop(Cpu& c) {  // 0x8040D294
    const u32 obj = stack_object(c);
    const s32 index = static_cast<s32>(c.mem_read32(obj + 8u));
    if (index > 0) c.mem_write32(obj + 8u, static_cast<u32>(index - 1));
}

// Calls a guest function from the original function's return point `link`; false when the run is stopped.
// guest_call lowers r1 by 0x10 for its own frame, the original's `bl` does not: r1 is raised by 0x10 first so the callee's frame lands at the
// address it has in the original (the callee frames are the deepest stack words, and the state report hashes them).
GEKKO_ALWAYS_INLINE bool call(Cpu& c, u32 target, u32 link, std::initializer_list<u32> args) {
    u32 result = 0;
    c.gpr[1] += 0x10u;
    const bool ok = guest_call(c, target, link, args, result);
    c.gpr[1] -= 0x10u;
    return ok;
}

// 16-bit turn quantisation: the angle * 65536 / 2pi as fctiwz gives it.
GEKKO_ALWAYS_INLINE u32 turns(Cpu& c, F angle) {
    F v = ppc::fps_mul(c, L(kTurn), angle);
    v = ppc::fps_div(c, v, L(kTwoPi));
    return static_cast<u32>(ppc::fctiw(c, v, true));
}
// radians of the low 16 bits of a non-zero turn count, as the magic-double sequence gives them.
GEKKO_ALWAYS_INLINE F radians(Cpu& c, u32 count) {
    const u64 d = 0x4330000000000000ull | (static_cast<u64>((count & 0xFFFFu) ^ 0x80000000u));
    F f = ppc::fps_sub(c, d, kMagic);
    f = ppc::fps_mul(c, L(kTwoPi), f);
    return ppc::fps_div(c, f, L(kTurn));
}

// ---- the quaternion chain ---------------------------------------------------------------------------------------------------------
struct Quat {
    F x, y, z, w;
};

// 0x804121BC. dst / src 0 = the stack top (copied through a frame local of the original).
void matrix_normalize(Cpu& c, u32 dst, u32 src) {
    const u32 frame = c.gpr[1] - 0x80u;
    const u32 dst_arg = dst;
    if (src == 0u) {
        copy12(c, frame + 0x38u, stack_top(c));
        src = frame + 0x38u;
    }
    if (dst == 0u) dst = frame + 8u;
    if (dst != src) copy12(c, dst, src);
    const F thr = L(kEpsilon), one = L(kOne);
    for (u32 col = 0; col < 12u; col += 4u) {
        const F x = LM(c, dst + col), y = LM(c, dst + 0x10u + col), z = LM(c, dst + 0x20u + col);
        const F x2 = ppc::fps_mul(c, x, x);
        const F y2 = ppc::fps_mul(c, y, y);
        const F z2 = ppc::fps_mul(c, z, z);
        const F s = ppc::fps_add(c, z2, ppc::fps_add(c, x2, y2));
        const F len = se_math::detail::guest_sqrt(c, s);
        if (!gt_ordered(c, len, thr)) continue;
        const F inv = ppc::fps_div(c, one, len);
        SM(c, dst + col, ppc::fps_mul(c, LM(c, dst + col), inv));
        SM(c, dst + 0x10u + col, ppc::fps_mul(c, LM(c, dst + 0x10u + col), inv));
        SM(c, dst + 0x20u + col, ppc::fps_mul(c, LM(c, dst + 0x20u + col), inv));
    }
    if (dst_arg == 0u) copy12(c, stack_top(c), dst);
}

// 0x8042193C: the quaternion of a 3x4 rotation (x y z w). `big` is the element byte offsets of the sums and the difference for
// the branch of the largest diagonal element: out[axis] = 0.5 s, the others inv * (m[a] + m[b]) and w = inv * (m[c] - m[d]).
Quat matrix_to_quat(Cpu& c, u32 m) {
    const F f4 = LM(c, m), f3 = LM(c, m + 0x14u), f5 = LM(c, m + 0x28u);
    F f0 = ppc::fps_add(c, f4, f3);
    const F f2 = L(kOne);
    f0 = ppc::fps_add(c, f5, f0);
    const F trace1 = ppc::fps_add(c, f2, f0);
    Quat q;
    if (!lt_ordered(c, trace1, f2)) {
        const F s = se_math::detail::guest_sqrt(c, trace1);
        const F twice = ppc::fps_mul(c, L(kTwo), s);
        const F w = ppc::fps_mul(c, L(kHalf), s);
        const F inv = ppc::fps_div(c, L(kOne), twice);
        const F a = ppc::fps_sub(c, LM(c, m + 0x24u), LM(c, m + 0x18u));
        const F b = ppc::fps_sub(c, LM(c, m + 8u), LM(c, m + 0x20u));
        const F d = ppc::fps_sub(c, LM(c, m + 0x10u), LM(c, m + 4u));
        q.x = ppc::fps_mul(c, inv, a);
        q.y = ppc::fps_mul(c, inv, b);
        q.z = ppc::fps_mul(c, inv, d);
        q.w = w;
        return q;
    }
    // one of x, y, z is the largest: t = 1 + big - other1 - other2 (the original's subtraction order), s = sqrt t, big' = 0.5 s,
    // inv = 1 / (4 * big'); the other two = inv * (sum), w = inv * (difference)
    const auto largest = [&](F big, F o1, F o2, u32 sa1, u32 sb1, u32 sa2, u32 sb2, u32 da, u32 db, F& out_big, F& out1, F& out2,
                             F& out_w) {
        F t = ppc::fps_sub(c, big, o1);
        t = ppc::fps_sub(c, t, o2);
        const F s = se_math::detail::guest_sqrt(c, ppc::fps_add(c, f2, t));
        const F half = ppc::fps_mul(c, L(kHalf), s);
        const F four_half = ppc::fps_mul(c, L(0x40800000u), half);
        const F v1 = ppc::fps_add(c, LM(c, m + sa1), LM(c, m + sb1));
        const F inv = ppc::fps_div(c, L(kOne), four_half);
        const F v2 = ppc::fps_add(c, LM(c, m + sa2), LM(c, m + sb2));
        const F v3 = ppc::fps_sub(c, LM(c, m + da), LM(c, m + db));
        out_big = half;
        out1 = ppc::fps_mul(c, inv, v1);
        out2 = ppc::fps_mul(c, inv, v2);
        out_w = ppc::fps_mul(c, inv, v3);
    };
    if (gt_ordered(c, f4, f3)) {
        if (gt_ordered(c, f4, f5)) {  // x
            largest(f4, f3, f5, 0x10u, 4u, 0x20u, 8u, 0x24u, 0x18u, q.x, q.y, q.z, q.w);
            return q;
        }
        // z (0x804219F4)
        largest(f5, f4, f3, 8u, 0x20u, 0x18u, 0x24u, 0x10u, 4u, q.z, q.x, q.y, q.w);
        return q;
    }
    if (gt_ordered(c, f3, f5)) {  // y
        largest(f3, f5, f4, 0x24u, 0x18u, 4u, 0x10u, 8u, 0x20u, q.y, q.z, q.x, q.w);
        return q;
    }
    largest(f5, f4, f3, 8u, 0x20u, 0x18u, 0x24u, 0x10u, 4u, q.z, q.x, q.y, q.w);  // z (0x80421AD4)
    return q;
}

// 0x80421774: shortest-arc slerp of a and b by t (a weighted by t, b by 1 - t); the weights go through the game's acosf / sinf.
Quat quat_slerp(Cpu& c, const Quat& a, const Quat& b, F t) {
    F f1 = ppc::fps_mul(c, a.y, b.y);
    f1 = ppc::fps_madd(c, a.x, b.x, f1);
    f1 = ppc::fps_madd(c, a.z, b.z, f1);
    f1 = ppc::fps_madd(c, a.w, b.w, f1);
    F sign = L(kOne);  // f22
    if (lt_ordered(c, f1, L(kZero))) {
        f1 ^= kSign;
        sign ^= kSign;
    }
    F wa, wb;  // f22 and f23
    if (lt_ordered(c, f1, L(kNearOne))) {
        const shadow::math::SubnormalMode mode = mode_of(c);
        const F theta = from_float(shadow::math::msl::acosf(as_float(f1), mode));
        F s = from_float(shadow::math::msl::sinf(as_float(theta), mode));
        const F inv = ppc::fps_div(c, L(kOne), s);
        F x = ppc::fps_sub(c, L(kOne), t);
        x = ppc::fps_mul(c, x, theta);
        x = from_float(shadow::math::msl::sinf(as_float(x), mode));
        wb = ppc::fps_mul(c, inv, x);
        x = ppc::fps_mul(c, t, theta);
        x = from_float(shadow::math::msl::sinf(as_float(x), mode));
        x = ppc::fps_mul(c, inv, x);
        wa = ppc::fps_mul(c, sign, x);
    } else {
        wa = ppc::fps_mul(c, sign, t);
        wb = ppc::fps_sub(c, L(kOne), t);
    }
    Quat r;
    F x = ppc::fps_mul(c, a.x, wa), y = ppc::fps_mul(c, a.y, wa), z = ppc::fps_mul(c, a.z, wa);
    r.x = ppc::fps_madd(c, b.x, wb, x);
    F w = ppc::fps_mul(c, a.w, wa);
    r.y = ppc::fps_madd(c, b.y, wb, y);
    r.z = ppc::fps_madd(c, b.z, wb, z);
    r.w = ppc::fps_madd(c, b.w, wb, w);
    return r;
}

// 0x80421BB0
void quat_to_matrix(Cpu& c, u32 out, const Quat& q) {
    const F f13 = q.w, f7 = L(kTwo), f10 = q.x, f11 = q.y, f12 = q.z;
    const F f2 = ppc::fps_mul(c, f7, f13);
    const F f1 = ppc::fps_mul(c, f7, f10);
    const F f0 = L(kOne);
    const F f6 = ppc::fps_mul(c, f10, f11);
    const F f5 = ppc::fps_mul(c, f13, f12);
    const F f31 = ppc::fps_msub(c, f2, f13, f0);
    const F zero = L(kZero);
    const F f9 = ppc::fps_mul(c, f10, f12);
    const F f8 = ppc::fps_mul(c, f13, f11);
    const F f3 = ppc::fps_add(c, f6, f5);
    const F e00 = ppc::fps_madd(c, f1, f10, f31);
    F f2b = ppc::fps_sub(c, f9, f8);
    const F f1b = ppc::fps_sub(c, f6, f5);
    F f5b = ppc::fps_mul(c, f7, f11);
    SM(c, out, e00);
    const F e10 = ppc::fps_mul(c, f7, f3);
    f2b = ppc::fps_mul(c, f7, f2b);
    const F f3b = ppc::fps_add(c, f9, f8);
    SM(c, out + 0x10u, e10);
    const F f9b = ppc::fps_mul(c, f11, f12);
    const F f8b = ppc::fps_mul(c, f13, f10);
    const F e01 = ppc::fps_mul(c, f7, f1b);
    SM(c, out + 0x20u, f2b);
    const F f1c = ppc::fps_mul(c, f7, f12);
    F f4c = ppc::fps_add(c, f9b, f8b);
    F f2c = ppc::fps_sub(c, f9b, f8b);
    SM(c, out + 4u, e01);
    f5b = ppc::fps_madd(c, f5b, f11, f31);
    f4c = ppc::fps_mul(c, f7, f4c);
    const F e02 = ppc::fps_mul(c, f7, f3b);
    SM(c, out + 0x14u, f5b);
    f2c = ppc::fps_mul(c, f7, f2c);
    const F e22 = ppc::fps_madd(c, f1c, f12, f31);
    SM(c, out + 0x24u, f4c);
    SM(c, out + 8u, e02);
    SM(c, out + 0x18u, f2c);
    SM(c, out + 0x28u, e22);
    SM(c, out + 0x2Cu, zero);
    SM(c, out + 0x1Cu, zero);
    SM(c, out + 0xCu, zero);
}

// 0x8042167C
void matrix_blend(Cpu& c, u32 out, u32 a, u32 b, F t) {
    const Quat qa = matrix_to_quat(c, a);
    const Quat qb = matrix_to_quat(c, b);
    const Quat q = quat_slerp(c, qa, qb, t);
    quat_to_matrix(c, out, q);
    matrix_normalize(c, out, out);
}

GEKKO_ALWAYS_INLINE Quat load_quat(Cpu& c, u32 at) { return {LM(c, at), LM(c, at + 4u), LM(c, at + 8u), LM(c, at + 0xCu)}; }
GEKKO_ALWAYS_INLINE void store_quat(Cpu& c, u32 at, const Quat& q) {
    SM(c, at, q.x);
    SM(c, at + 4u, q.y);
    SM(c, at + 8u, q.z);
    SM(c, at + 0xCu, q.w);
}

// ---- the matrix leaves the passes use ---------------------------------------------------------------------------------------------
// 0x8040CB14 (retired: its direct callers are lift sites): left = left * right, the shadow::math composition.
void compose(Cpu& c, u32 left, u32 right) {
    float l[12], r[12];
    for (u32 i = 0; i < 12u; ++i) {
        l[i] = bits_to_float(c.mem_read32(left + 4u * i));
        r[i] = bits_to_float(c.mem_read32(right + 4u * i));
    }
    shadow::math::compose_affine_matrix(l, r, mode_of(c));
    for (u32 i = 0; i < 12u; ++i) c.mem_write32(left + 4u * i, float_to_bits(l[i]));
}
GEKKO_ALWAYS_INLINE void translate_by(Cpu& c, u32 m, u32 v) {  // the owner of 0x8040C5EC
    c.gpr[3] = m;
    c.gpr[4] = v;
    math_affine::serve_translate_by(c);
}
GEKKO_ALWAYS_INLINE void scale_columns(Cpu& c, u32 m, u32 v) {  // the owner of 0x8040C7E0
    c.gpr[3] = m;
    c.gpr[4] = v;
    math_affine::serve_scale_columns(c);
}
// Return addresses of the original calls (the link a guest callee returns to; values only, never entered).
constexpr u32 kLinkCallback = 0x804197C4u;  // liveness-roots: value-only
constexpr u32 kLinkPost = 0x80419814u;  // liveness-roots: value-only

// 0x8040C85C / C898 / C8D4: the top matrix times a rotation about Z / Y / X by `angle` radians. The originals build the rotation matrix
// (0x8040C110 / C188 / C200: sin and cos of the angle by sincosf, each rounded to single, then twelve stores) and compose it with the top
// (0x8040CB14, the retired lift site). Done here without the guest calls: same sin / cos (shadow::math::msl in the caller's NI mode), same
// matrix words, same composition; the callees' frames and the temporary matrix are dead.
enum class Axis { Z, Y, X };
void rotate(Cpu& c, Axis axis, F angle) {
    const shadow::math::SubnormalMode mode = mode_of(c);
    const float a = as_float(angle);
    const float s = shadow::math::msl::sinf(a, mode);
    const float co = shadow::math::msl::cosf(a, mode);
    const float ns = bits_to_float(float_to_bits(s) ^ 0x80000000u);  // fneg
    const float zero = 0.0f, one = 1.0f;
    float r[12];
    switch (axis) {
    case Axis::Z: {
        const float v[12] = {co, ns, zero, zero, s, co, zero, zero, zero, zero, one, zero};
        std::memcpy(r, v, sizeof r);
        break;
    }
    case Axis::Y: {
        const float v[12] = {co, zero, s, zero, zero, one, zero, zero, ns, zero, co, zero};
        std::memcpy(r, v, sizeof r);
        break;
    }
    default: {
        const float v[12] = {one, zero, zero, zero, zero, co, ns, zero, zero, s, co, zero};
        std::memcpy(r, v, sizeof r);
        break;
    }
    }
    const u32 top = stack_top(c);
    float l[12];
    for (u32 i = 0; i < 12u; ++i) l[i] = bits_to_float(c.mem_read32(top + 4u * i));
    shadow::math::compose_affine_matrix(l, r, mode);
    for (u32 i = 0; i < 12u; ++i) c.mem_write32(top + 4u * i, float_to_bits(l[i]));
}

// The three Euler counts applied to the stack top in the order Z, Y, X (a zero count: no rotation).
void euler_rotations(Cpu& c, u32 ix, u32 iy, u32 iz) {
    if (iz != 0u) rotate(c, Axis::Z, radians(c, iz));
    if (iy != 0u) rotate(c, Axis::Y, radians(c, iy));
    if (ix != 0u) rotate(c, Axis::X, radians(c, ix));
}

// The original's frame is `size` below the caller's r1; guest calls made inside use it.
struct FrameScope {
    Cpu& c;
    u32 saved;
    u32 frame;
    FrameScope(Cpu& cpu, u32 size) : c(cpu), saved(cpu.gpr[1]), frame(cpu.gpr[1] - size) { c.gpr[1] = frame; }
    ~FrameScope() { c.gpr[1] = saved; }
    FrameScope(const FrameScope&) = delete;
    FrameScope& operator=(const FrameScope&) = delete;
};

// The pass scratch: flag arrays and the index list walk shared by the four callbacks.
GEKKO_ALWAYS_INLINE u32 flag_a(u32 idx) { return kScratch + 0x1E0u + (idx << 2); }
GEKKO_ALWAYS_INLINE u32 flag_b(u32 idx) { return kScratch + 0x1F0u + (idx << 2); }
GEKKO_ALWAYS_INLINE u32 flag_c(u32 idx) { return kScratch + 0x200u + (idx << 2); }
GEKKO_ALWAYS_INLINE u32 weight_at(u32 idx) { return kScratch + 0x210u + (idx << 2); }
GEKKO_ALWAYS_INLINE u32 matrix_at(u32 idx) { return kScratch + 0xF0u + idx * 0x30u; }
GEKKO_ALWAYS_INLINE u32 vector_at(u32 idx) { return kScratch + 0x1B0u + idx * 0xCu; }
constexpr u32 kNone = 0xFFFFFFFFu;

// Data the original is undefined on (see the header: preconditions). The owner keeps going exactly as before (it reads the same guest
// addresses) and says so once per kind on stderr, so a run that meets one is visible.
void note_unsupported(unsigned kind) {
    static bool seen[4] = {false, false, false, false};
    static const char* const text[4] = {
        "a bone index above 3 (the pass scratch has four slots; the original overwrites its own flag arrays)",
        "a channel index above 3 (the weight table has four entries)",
        "a pass accumulator the original reads without any write (a list without a processed root: a stale stack local)",
        "a table channel without a record (its frame slot is never saved; the original restores whatever the slot holds)"};
    if (seen[kind]) return;
    seen[kind] = true;
    std::fprintf(stderr, "[mtn_pose] precondition not met: %s\n", text[kind]);
}

GEKKO_ALWAYS_INLINE s32 list_count(Cpu& c, u32 list) { return static_cast<s32>(c.mem_read32(list)); }
GEKKO_ALWAYS_INLINE u32 list_index(Cpu& c, u32 list, s32 i) {
    const u32 idx = c.mem_read32(c.mem_read32(list + 4u) + 4u * static_cast<u32>(i));
    if (idx != 0xFFFFFFFFu && idx > 3u) note_unsupported(0);
    return idx;
}

void clear_flags(Cpu& c, u32 list) {
    for (s32 i = 0; i < list_count(c, list); ++i) {
        const u32 idx = list_index(c, list, i);
        if (idx == kNone) break;
        c.mem_write32(flag_b(idx), 0u);
        c.mem_write32(flag_c(idx), 0u);
    }
}

// The ordering pass: flag_a[idx] = "a later unprocessed child exists" (or a root once one was found); returns whether one was.
bool order_pass(Cpu& c, u32 list) {
    bool found = false;
    for (s32 i = 0; i < list_count(c, list); ++i) {
        const u32 idx = list_index(c, list, i);
        if (idx == kNone) break;
        if (found && c.mem_read32(c.mem_read32(list + 0xCu) + (idx << 2)) == kNone) {
            c.mem_write32(flag_a(idx), 1u);
            continue;
        }
        c.mem_write32(flag_a(idx), 0u);
        const s32 n = list_count(c, list);
        for (s32 j = i + 1; j < n; ++j) {
            const u32 v = list_index(c, list, j);
            if (v == kNone) break;
            if (c.mem_read32(flag_b(v)) == 0u && c.mem_read32(c.mem_read32(list + 0xCu) + (v << 2)) == idx) {
                c.mem_write32(flag_a(idx), 1u);
                found = true;
                break;
            }
        }
    }
    return found;
}

// ---- 0x8041EC24 --------------------------------------------------------------------------------------------------------------------
void rotation_pass(Cpu& c, u32 args) {
    FrameScope fs(c, 0xC0u);
    const u32 fr = fs.frame;
    const u32 list = c.mem_read32(args), base = c.mem_read32(args + 4u), pairs = args + 8u;
    clear_flags(c, list);
    stack_push(c);
    bool found, acc_written = false;
    do {
        found = order_pass(c, list);
        for (s32 j = 0; j < list_count(c, list); ++j) {
            const u32 idx = list_index(c, list, j);
            if (idx == kNone) break;
            if (c.mem_read32(flag_b(idx)) != 0u) continue;
            const u32 angles = c.mem_read32(pairs + idx * 8u);
            const F x = LM(c, angles), y = LM(c, angles + 4u), z = LM(c, angles + 8u);
            const F fx = ppc::fps_mul(c, L(kTurn), x), fy = ppc::fps_mul(c, L(kTurn), y), fz = ppc::fps_mul(c, L(kTurn), z);
            const u32 ix = static_cast<u32>(ppc::fctiw(c, ppc::fps_div(c, fx, L(kTwoPi)), true));
            const u32 iy = static_cast<u32>(ppc::fctiw(c, ppc::fps_div(c, fy, L(kTwoPi)), true));
            const u32 iz = static_cast<u32>(ppc::fctiw(c, ppc::fps_div(c, fz, L(kTwoPi)), true));
            if (c.mem_read32(flag_a(idx)) != 0u) {
                if (c.mem_read32(flag_c(idx)) != 0u) continue;
                copy12(c, stack_top(c), kIdentity);
                euler_rotations(c, ix, iy, iz);
                copy12(c, matrix_at(idx), stack_top(c));
                c.mem_write32(flag_c(idx), 1u);
                continue;
            }
            c.mem_write32(flag_b(idx), 1u);
            const u32 parent = c.mem_read32(c.mem_read32(list + 0xCu) + (idx << 2));
            u32 dst;
            if (parent == kNone) {
                dst = fr + 8u;
                acc_written = true;
            } else {
                dst = matrix_at(parent);
                c.mem_write32(flag_c(parent), 1u);
            }
            u32 current;
            if (c.mem_read32(flag_c(idx)) != 0u) {
                current = matrix_at(idx);
            } else {
                copy12(c, stack_top(c), kIdentity);
                euler_rotations(c, ix, iy, iz);
                copy12(c, fr + 0x38u, stack_top(c));
                current = fr + 0x38u;
            }
            const u32 flags = c.mem_read32(c.mem_read32(pairs + idx * 8u + 4u));
            if ((flags & 1u) != 0u) {
                if (j == 0) matrix_normalize(c, dst, base);
                matrix_blend(c, dst, current, dst, LM(c, weight_at(idx)));
            } else {
                copy12(c, dst, current);
            }
        }
    } while (found);
    if (!acc_written) note_unsupported(2);
    stack_pop(c);
    compose(c, stack_top(c), fr + 8u);
}

// ---- 0x8041E5D0 / 0x8041E8EC / 0x8041F174: the vector passes ---------------------------------------------------------------------
enum class VecKind { Scale, ScaleLengths, Translation, ScaleBank };  // ScaleBank: 0x8041FA14 (round 4), see fold_scale_bank

// 0x80412378: the column lengths of the 3x4 matrix `m` (0: the stack top) into out[0..2].
void column_lengths(Cpu& c, u32 out, u32 m) {
    if (m == 0u) m = stack_top(c);
    for (u32 col = 0; col < 12u; col += 4u) {
        const F x = LM(c, m + col), y = LM(c, m + 0x10u + col), z = LM(c, m + 0x20u + col);
        const F x2 = ppc::fps_mul(c, x, x);
        const F y2 = ppc::fps_mul(c, y, y);
        const F z2 = ppc::fps_mul(c, z, z);
        const F s = ppc::fps_add(c, z2, ppc::fps_add(c, x2, y2));
        SM(c, out + col, se_math::detail::guest_sqrt(c, s));
    }
}

void vector_pass(Cpu& c, u32 args, VecKind kind) {
    FrameScope fs(c, kind == VecKind::Scale ? 0x40u : 0x50u);
    const u32 fr = fs.frame;
    const u32 list = c.mem_read32(args), base_matrix = c.mem_read32(args + 4u), pairs = args + 8u;
    if (kind != VecKind::Translation) {
        SM(c, fr + 0x10u, L(kOne));
        SM(c, fr + 0xCu, L(kOne));
        SM(c, fr + 8u, L(kOne));
    }
    clear_flags(c, list);
    bool found, acc_written = kind != VecKind::Translation;
    do {
        found = order_pass(c, list);
        for (s32 j = 0; j < list_count(c, list); ++j) {
            const u32 idx = list_index(c, list, j);
            if (idx == kNone) break;
            if (c.mem_read32(flag_b(idx)) != 0u) continue;
            u32 source = c.mem_read32(pairs + idx * 8u);
            if (c.mem_read32(flag_a(idx)) != 0u) {
                if (c.mem_read32(flag_c(idx)) != 0u) continue;
                copy3(c, vector_at(idx), source);
                c.mem_write32(flag_c(idx), 1u);
                continue;
            }
            c.mem_write32(flag_b(idx), 1u);
            const u32 parent = c.mem_read32(c.mem_read32(list + 0xCu) + (idx << 2));
            u32 dst;
            if (parent == kNone) {
                dst = kind == VecKind::Translation ? fr + 0x14u : fr + 8u;
                acc_written = true;
            } else {
                dst = vector_at(parent);
                c.mem_write32(flag_c(parent), 1u);
            }
            if (c.mem_read32(flag_c(idx)) != 0u) source = vector_at(idx);
            const u32 flags = c.mem_read32(c.mem_read32(pairs + idx * 8u + 4u));
            if ((flags & 1u) == 0u) {
                copy3(c, dst, source);
                continue;
            }
            u32 base = dst;
            if (j == 0) {
                if (kind == VecKind::Scale) {
                    base = base_matrix + 0x30u;
                } else if (kind == VecKind::ScaleLengths || kind == VecKind::ScaleBank) {
                    column_lengths(c, fr + 8u, base_matrix);
                    base = fr + 8u;
                } else {
                    const F tx = LM(c, base_matrix + 0xCu);
                    SM(c, fr + 8u, tx);
                    const F ty = LM(c, base_matrix + 0x1Cu);
                    SM(c, fr + 0xCu, ty);
                    const F tz = LM(c, base_matrix + 0x2Cu);
                    SM(c, fr + 0x10u, tz);
                    base = fr + 8u;
                }
            }
            const F w = LM(c, weight_at(idx));
            const F inv = ppc::fps_sub(c, L(kOne), w);
            for (u32 k = 0; k < 12u; k += 4u) {
                F f0 = LM(c, base + k);
                const F f1 = LM(c, source + k);
                f0 = ppc::fps_mul(c, inv, f0);
                f0 = ppc::fps_madd(c, w, f1, f0);
                SM(c, dst + k, f0);
            }
        }
    } while (found);
    if (!acc_written) note_unsupported(2);
    if (kind == VecKind::Translation) {
        translate_by(c, stack_top(c), fr + 0x14u);
        return;
    }
    const F one = L(kOne);
    if (kind == VecKind::ScaleBank) {  // 0x8041FD1C: the accumulated scale applied to every bank matrix, then to the top
        if (eq_unordered(c, one, LM(c, fr + 8u)) && eq_unordered(c, one, LM(c, fr + 0xCu)) && eq_unordered(c, one, LM(c, fr + 0x10u))) return;
        stack_push(c);
        for (s32 i = 0; i < list_count(c, list); ++i) {
            if (c.mem_read32(c.mem_read32(list + 4u) + 4u * static_cast<u32>(i)) == kNone) break;
            const u32 bank = kScratch + 0x30u * static_cast<u32>(i);
            copy12(c, stack_top(c), bank);
            scale_columns(c, stack_top(c), fr + 8u);
            copy12(c, bank, stack_top(c));
        }
        stack_pop(c);
        scale_columns(c, stack_top(c), fr + 8u);
        return;
    }
    if (!(eq_unordered(c, one, LM(c, fr + 8u)) && eq_unordered(c, one, LM(c, fr + 0xCu)) && eq_unordered(c, one, LM(c, fr + 0x10u))))
        scale_columns(c, stack_top(c), fr + 8u);
}

// ---- 0x80420B9C: one bone's Euler angles applied to the stack top -------------------------------------------------------------------
void euler_bone(Cpu& c, u32 args) {
    FrameScope fs(c, 0x30u);
    const u32 list = c.mem_read32(args);
    const u32 first = c.mem_read32(c.mem_read32(list + 4u));
    const u32 angles = c.mem_read32(args + 8u + (first << 3));
    const F fz0 = LM(c, angles + 8u), fx0 = LM(c, angles), fy0 = LM(c, angles + 4u);
    const u32 iz = static_cast<u32>(ppc::fctiw(c, ppc::fps_div(c, ppc::fps_mul(c, L(kTurn), fz0), L(kTwoPi)), true));
    const u32 ix = static_cast<u32>(ppc::fctiw(c, ppc::fps_div(c, ppc::fps_mul(c, L(kTurn), fx0), L(kTwoPi)), true));
    const u32 iy = static_cast<u32>(ppc::fctiw(c, ppc::fps_div(c, ppc::fps_mul(c, L(kTurn), fy0), L(kTwoPi)), true));
    euler_rotations(c, ix, iy, iz);
}

// ---- round 4: the NodeWalk callbacks 0x80420B28 / 0x80420D10 / 0x8041FA14 / 0x804202E0 / 0x8041FDDC, run inside the native walk ---------
// Switch: on in the default build, off without its CMake option (GEKKO_MTN_FOLD=1, or the build option SHADOW_MTN_FOLD=ON -> GEKKO_MTN_FOLD_DEFAULT=1).
#ifndef GEKKO_MTN_FOLD_DEFAULT
#define GEKKO_MTN_FOLD_DEFAULT 0
#endif
bool g_fold = GEKKO_MTN_FOLD_DEFAULT != 0;
std::atomic<unsigned long long> g_fold_hits[kFoldKinds];
std::atomic<unsigned long long> g_fold_back[kFoldKinds];
constexpr const char* kFoldNames[kFoldKinds] = {"scale callback", "translate callback", "scale bank",
                                                "translation bank", "rotation bank"};
constexpr u32 kBankTemp = kScratch + 0xC0u;  // the fifth matrix of the bank: the base bone accumulator

void print_fold_stats() {
    for (unsigned k = 0; k < kFoldKinds; ++k)
        std::fprintf(stderr, "[mtn_fold] %-30s folded %llu handed back %llu\n", kFoldNames[k], g_fold_hits[k].load(), g_fold_back[k].load());
}

// What the fold can prove before it writes anything: the bone indices of the list are 0..3 up to the first -1 (the pass scratch has
// four slots), the first entry exists and is a root (so the pass accumulator is written before it is read: the original reads a stale
// stack local otherwise), and, for the passes that read the base matrix through a pointer, the pointer is not null.
bool fold_list_ok(Cpu& c, u32 args, bool needs_base) {
    const u32 list = c.mem_read32(args);
    if (needs_base && c.mem_read32(args + 4u) == 0u) return false;
    const s32 n = list_count(c, list);
    if (n < 1) return false;
    const u32 indices = c.mem_read32(list + 4u), parents = c.mem_read32(list + 0xCu);
    for (s32 i = 0; i < n; ++i) {
        const u32 idx = c.mem_read32(indices + 4u * static_cast<u32>(i));
        if (idx == kNone) break;
        if (idx > 3u) return false;
    }
    const u32 first = c.mem_read32(indices);
    if (first == kNone || first > 3u) return false;
    return c.mem_read32(parents + (first << 2)) == kNone;
}

// 0x80420B28: the first bone vector scales the stack top unless it is exactly (1, 1, 1).
void fold_scale_callback(Cpu& c, u32 args) {
    const u32 first = c.mem_read32(c.mem_read32(c.mem_read32(args) + 4u));
    const u32 v = c.mem_read32(args + 8u + (first << 3));
    const F one = L(kOne);
    if (eq_unordered(c, one, LM(c, v)) && eq_unordered(c, one, LM(c, v + 4u)) && eq_unordered(c, one, LM(c, v + 8u))) return;
    scale_columns(c, stack_top(c), v);
}

// 0x80420D10: the first bone vector translates the stack top.
void fold_translate_callback(Cpu& c, u32 args) {
    const u32 first = c.mem_read32(c.mem_read32(c.mem_read32(args) + 4u));
    translate_by(c, stack_top(c), c.mem_read32(args + 8u + (first << 3)));
}

// 0x80412438 with r3 = 0: the 3x3 part of the stack top becomes that of src (the translation column is copied through and back).
void copy_3x3_to_top(Cpu& c, u32 src) {
    static const u32 offs[9] = {0x00u, 0x10u, 0x20u, 0x04u, 0x14u, 0x24u, 0x08u, 0x18u, 0x28u};
    u32 v[9];
    for (unsigned k = 0; k < 9u; ++k) v[k] = c.mem_read32(src + offs[k]);
    const u32 top = stack_top(c);
    for (unsigned k = 0; k < 9u; ++k) c.mem_write32(top + offs[k], v[k]);
}

// 0x8041FDDC: bank rotation. Pass 1 applies every bone Euler angles to its bank matrix (kScratch + 0x30 * position); the first bone
// (when its flag word has bit 0) also to the base accumulator kBankTemp through the base matrix rotation part. Pass 2 is the
// parent-ordered blend (MatrixBlend against the base accumulator for the first position, else against the parent accumulator).
void fold_rotation_bank(Cpu& c, u32 args) {
    FrameScope fs(c, 0xD0u);
    const u32 fr = fs.frame;
    const u32 list = c.mem_read32(args), base = c.mem_read32(args + 4u), pairs = args + 8u;
    clear_flags(c, list);
    stack_push(c);
    for (s32 i = 0; i < list_count(c, list); ++i) {
        const u32 idx = list_index(c, list, i);
        if (idx == kNone) break;
        const u32 angles = c.mem_read32(pairs + idx * 8u);
        const u32 flags = c.mem_read32(c.mem_read32(pairs + idx * 8u + 4u));
        if ((flags & 1u) != 0u && i == 0) {
            copy12(c, fr + 0x38u, base);
            SM(c, fr + 0x44u, L(kZero));
            SM(c, fr + 0x54u, L(kZero));
            SM(c, fr + 0x64u, L(kZero));
            copy12(c, stack_top(c), kBankTemp);
            compose(c, stack_top(c), fr + 0x38u);
            copy12(c, kBankTemp, stack_top(c));
        }
        const u32 bank = kScratch + 0x30u * static_cast<u32>(i);
        copy12(c, stack_top(c), bank);
        const F x = LM(c, angles), y = LM(c, angles + 4u), z = LM(c, angles + 8u);
        const u32 iz = turns(c, z), ix = turns(c, x), iy = turns(c, y);
        euler_rotations(c, ix, iy, iz);
        copy12(c, bank, stack_top(c));
    }
    bool found, acc_written = false;
    do {
        found = order_pass(c, list);
        for (s32 j = 0; j < list_count(c, list); ++j) {
            const u32 idx = list_index(c, list, j);
            if (idx == kNone) break;
            if (c.mem_read32(flag_b(idx)) != 0u) continue;
            const u32 bank = kScratch + 0x30u * static_cast<u32>(j);
            if (c.mem_read32(flag_a(idx)) != 0u) {
                if (c.mem_read32(flag_c(idx)) != 0u) continue;
                copy12(c, matrix_at(idx), bank);
                c.mem_write32(flag_c(idx), 1u);
                continue;
            }
            c.mem_write32(flag_b(idx), 1u);
            const u32 parent = c.mem_read32(c.mem_read32(list + 0xCu) + (idx << 2));
            u32 dst;
            if (parent == kNone) {
                dst = fr + 8u;
                acc_written = true;
            } else {
                dst = matrix_at(parent);
                c.mem_write32(flag_c(parent), 1u);
            }
            const u32 src = c.mem_read32(flag_c(idx)) != 0u ? matrix_at(idx) : bank;
            const u32 flags = c.mem_read32(c.mem_read32(pairs + idx * 8u + 4u));
            if ((flags & 1u) != 0u) {
                const u32 other = j == 0 ? kBankTemp : dst;
                matrix_blend(c, dst, src, other, LM(c, weight_at(idx)));
            } else {
                copy12(c, dst, src);
            }
        }
    } while (found);
    if (!acc_written) note_unsupported(2);
    stack_pop(c);
    copy_3x3_to_top(c, fr + 8u);
}

// 0x804202E0: bank translation. Pass 1 translates every bank matrix by its bone vector (the first bone also the base accumulator
// kBankTemp, by the base matrix translation column); pass 2 blends the bank matrices translation columns over the bone tree and
// finally sets the stack top translation column to the root result.
void fold_translation_bank(Cpu& c, u32 args) {
    FrameScope fs(c, 0xA0u);
    const u32 fr = fs.frame;
    const u32 list = c.mem_read32(args), base = c.mem_read32(args + 4u), pairs = args + 8u;
    clear_flags(c, list);
    stack_push(c);
    for (s32 i = 0; i < list_count(c, list); ++i) {
        const u32 idx = list_index(c, list, i);
        if (idx == kNone) break;
        const u32 source = c.mem_read32(pairs + idx * 8u);
        const u32 flags = c.mem_read32(c.mem_read32(pairs + idx * 8u + 4u));
        if ((flags & 1u) != 0u && i == 0) {
            const F tx = LM(c, base + 0xCu);
            SM(c, fr + 8u, tx);
            const F ty = LM(c, base + 0x1Cu);
            SM(c, fr + 0xCu, ty);
            const F tz = LM(c, base + 0x2Cu);
            SM(c, fr + 0x10u, tz);
            copy12(c, stack_top(c), kBankTemp);
            translate_by(c, stack_top(c), fr + 8u);
            copy12(c, kBankTemp, stack_top(c));
        }
        const u32 bank = kScratch + 0x30u * static_cast<u32>(i);
        copy12(c, stack_top(c), bank);
        translate_by(c, stack_top(c), source);
        copy12(c, bank, stack_top(c));
    }
    bool found, acc_written = false;
    do {
        found = order_pass(c, list);
        for (s32 j = 0; j < list_count(c, list); ++j) {
            const u32 idx = list_index(c, list, j);
            if (idx == kNone) break;
            if (c.mem_read32(flag_b(idx)) != 0u) continue;
            const u32 bank = kScratch + 0x30u * static_cast<u32>(j);
            const F x = LM(c, bank + 0xCu), y = LM(c, bank + 0x1Cu), z = LM(c, bank + 0x2Cu);
            SM(c, fr + 0x20u, x);
            SM(c, fr + 0x24u, y);
            SM(c, fr + 0x28u, z);
            if (c.mem_read32(flag_a(idx)) != 0u) {
                if (c.mem_read32(flag_c(idx)) != 0u) continue;
                copy3(c, vector_at(idx), fr + 0x20u);
                c.mem_write32(flag_c(idx), 1u);
                continue;
            }
            c.mem_write32(flag_b(idx), 1u);
            const u32 parent = c.mem_read32(c.mem_read32(list + 0xCu) + (idx << 2));
            u32 dst;
            if (parent == kNone) {
                dst = fr + 0x2Cu;
                acc_written = true;
            } else {
                dst = vector_at(parent);
                c.mem_write32(flag_c(parent), 1u);
            }
            const u32 source = c.mem_read32(flag_c(idx)) != 0u ? vector_at(idx) : fr + 0x20u;
            const u32 flags = c.mem_read32(c.mem_read32(pairs + idx * 8u + 4u));
            if ((flags & 1u) == 0u) {
                copy3(c, dst, source);
                continue;
            }
            u32 from = dst;
            if (j == 0) {
                const F bx = LM(c, kScratch + 0xCCu);
                SM(c, fr + 0x14u, bx);
                const F by = LM(c, kScratch + 0xDCu);
                SM(c, fr + 0x18u, by);
                const F bz = LM(c, kScratch + 0xECu);
                SM(c, fr + 0x1Cu, bz);
                from = fr + 0x14u;
            }
            const F w = LM(c, weight_at(idx));
            const F inv = ppc::fps_sub(c, L(kOne), w);
            for (u32 k = 0; k < 12u; k += 4u) {
                F f0 = LM(c, from + k);
                const F f1 = LM(c, source + k);
                f0 = ppc::fps_mul(c, inv, f0);
                f0 = ppc::fps_madd(c, w, f1, f0);
                SM(c, dst + k, f0);
            }
        }
    } while (found);
    if (!acc_written) note_unsupported(2);
    stack_pop(c);
    copy12(c, fr + 0x38u, stack_top(c));
    SM(c, fr + 0x44u, LM(c, fr + 0x2Cu));
    SM(c, fr + 0x54u, LM(c, fr + 0x30u));
    SM(c, fr + 0x64u, LM(c, fr + 0x34u));
    copy12(c, stack_top(c), fr + 0x38u);
}

// Runs a callback inside the walk when the fold is on and the case is provable; false = the caller enters the guest body.
bool fold_callback(Cpu& c, u32 fn, u32 args) {
    unsigned kind;
    switch (fn) {
    case kFoldScaleCallback: kind = 0; break;
    case kFoldTranslateCallback: kind = 1; break;
    case kFoldScaleBank: kind = 2; break;
    case kFoldTranslationBank: kind = 3; break;
    case kFoldRotationBank: kind = 4; break;
    default: return false;
    }
    if (!g_fold) return false;
    if (kind >= 2u && !fold_list_ok(c, args, kind != 2u)) {
        g_fold_back[kind].fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    g_fold_hits[kind].fetch_add(1, std::memory_order_relaxed);
    switch (kind) {
    case 0: fold_scale_callback(c, args); break;
    case 1: fold_translate_callback(c, args); break;
    case 2: vector_pass(c, args, VecKind::ScaleBank); break;
    case 3: fold_translation_bank(c, args); break;
    default: fold_rotation_bank(c, args); break;
    }
    return true;
}

// ---- round 5: the MotionUpdate tree (0x80416EF4): shell + the sampler pick (0x804198F0) + the bone pass (0x80419094) native ---------------
// The other phases (0x8041BB8C per channel, 0x8041AFAC, 0x8041A33C, 0x80419A9C, 0x80418618) are entered as guest code, unchanged.
// Switch: on by default since the build option SHADOW_MTN_TREE defaults ON (GEKKO_MTN_TREE_DEFAULT=1); GEKKO_MTN_TREE=0/1 overrides. The hook declines
// (the guest body runs) whenever the switch is off; the native path has no input it must refuse (every pointer is read where the
// original reads it), so there is no other hand-back.
#ifndef GEKKO_MTN_TREE_DEFAULT
#define GEKKO_MTN_TREE_DEFAULT 0
#endif
bool g_tree = GEKKO_MTN_TREE_DEFAULT != 0;
std::atomic<unsigned long long> g_tree_hits[kTreeKinds];
constexpr const char* kTreeNames[kTreeKinds] = {"motion update", "sampler pick (channels)", "bone pass", "declined"};

void print_tree_stats() {
    for (unsigned k = 0; k < kTreeKinds; ++k) std::fprintf(stderr, "[mtn_tree] %-26s %llu\n", kTreeNames[k], g_tree_hits[k].load());
}

// Values stored for the guest (the sampler triples) and the guest return addresses of the original calls; never entered by this code.
constexpr u32 kForwardSlope = 0x80416020u;   // liveness-roots: value-only
constexpr u32 kForwardBoth = 0x80415D0Cu;    // liveness-roots: value-only
constexpr u32 kReverseValue = 0x80415A30u;   // liveness-roots: value-only
constexpr u32 kReverseSlope = 0x8041579Cu;   // liveness-roots: value-only
constexpr u32 kReverseBoth = 0x80415488u;    // liveness-roots: value-only
constexpr u32 kLinkSamplerA = 0x80419A30u;   // liveness-roots: value-only
constexpr u32 kLinkSamplerB = 0x80419A68u;   // liveness-roots: value-only
constexpr u32 kLinkChannel = 0x80416F74u;    // liveness-roots: value-only
constexpr u32 kLinkPhaseA = 0x80416FA4u;     // liveness-roots: value-only
constexpr u32 kLinkPhaseB = 0x80416FACu;     // liveness-roots: value-only
constexpr u32 kLinkPhaseC = 0x80416FB4u;     // liveness-roots: value-only
constexpr u32 kLinkPhaseD = 0x80416FCCu;     // liveness-roots: value-only
constexpr u32 kPhaseChannel = 0x8041BB8Cu;
constexpr u32 kPhaseA = 0x8041AFACu;
constexpr u32 kPhaseB = 0x8041A33Cu;
constexpr u32 kPhaseC = 0x80419A9Cu;
constexpr u32 kPhaseD = 0x80418618u;
constexpr u32 kChannelStride = 0x130u;

GEKKO_ALWAYS_INLINE F neg_word(Cpu& c, u32 address) { return LM(c, address) ^ kSign; }  // lfs, fneg

// 0x80412438 with r3 != 0: the 3x3 part of dst becomes that of src.
void copy_3x3(Cpu& c, u32 dst, u32 src) {
    static const u32 offs[9] = {0x00u, 0x10u, 0x20u, 0x04u, 0x14u, 0x24u, 0x08u, 0x18u, 0x28u};
    u32 v[9];
    for (unsigned k = 0; k < 9u; ++k) v[k] = c.mem_read32(src + offs[k]);
    for (unsigned k = 0; k < 9u; ++k) c.mem_write32(dst + offs[k], v[k]);
}

// 0x80412520(out, 0): the translation column of the stack top into out[0..2] (through a frame copy of the top).
void top_translation_to(Cpu& c, u32 out) {
    const u32 top = stack_top(c);
    const F x = LM(c, top + 0xCu);
    SM(c, out, x);
    const F y = LM(c, top + 0x1Cu);
    SM(c, out + 4u, y);
    const F z = LM(c, top + 0x2Cu);
    SM(c, out + 8u, z);
}

// 0x804215E0(out, a, b, t): rotation blend a -> b by t (MatrixBlend), translation (1 - t) * b + t * a, copied to out.
void matrix_lerp(Cpu& c, u32 out, u32 a, u32 b, F t) {
    FrameScope fs(c, 0x60u);
    const u32 tmp = fs.frame + 8u;
    matrix_blend(c, tmp, a, b, t);
    const F one = L(kOne);
    const F inv = ppc::fps_sub(c, one, t);
    for (u32 off = 0xCu; off <= 0x2Cu; off += 0x10u) {
        F f0 = LM(c, b + off);
        const F f1 = LM(c, a + off);
        f0 = ppc::fps_mul(c, inv, f0);
        f0 = ppc::fps_madd(c, t, f1, f0);
        SM(c, tmp + off, f0);
    }
    copy12(c, out, tmp);
}

bool run_sampler_entry(Cpu& c, u32 fn, u32 arg, u32 link) {
    c.gpr[3] = arg;
    switch (fn) {
    case mtn_track::kSampleVec3Reciprocal: mtn_track::serve_sample_vec3_reciprocal(c); return !c.exiting();
    case mtn_track::kSampleVec3: mtn_track::serve_sample_vec3(c); return !c.exiting();
    case mtn_track::kSampleVec3At20: mtn_track::serve_sample_vec3_at_20(c); return !c.exiting();
    case mtn_track::kSampleVec3Scaled: mtn_track::serve_sample_vec3_scaled(c); return !c.exiting();
    default: return call(c, fn, link, {arg});
    }
}

// 0x804198F0: per active channel pick the sampler triple by the sign of the rate, load the sampling context, run the channel callbacks.
bool sampler_pick(Cpu& c, u32 m) {
    FrameScope fs(c, 0x40u);
    const u32 fr = fs.frame;
    for (s32 i = 0; i < static_cast<s32>(c.mem_read32(m + 0x20u)); ++i) {
        const u32 idx = c.mem_read32(c.mem_read32(m + 0x24u) + 4u * static_cast<u32>(i));
        if (idx == kNone) break;
        const u32 ch = c.mem_read32(m + 0x10u) + idx * kChannelStride;
        if ((c.mem_read32(ch + 0x90u) & 4u) == 0u) continue;
        g_tree_hits[1].fetch_add(1, std::memory_order_relaxed);
        const F rate = LM(c, ch + 0xA8u), zero = L(kZero);
        const double da = bits_to_double(rate), db = bits_to_double(zero);
        const u32 slot = c.gpr[13];
        if (da > db || da == db) {  // fcmpo, cror: rate >= 0
            c.mem_write32(slot + 0x5F78u, mtn_track::kTrackSample);
            c.mem_write32(slot + 0x5F7Cu, kForwardSlope);
            c.mem_write32(slot + 0x5F80u, kForwardBoth);
        } else {
            if (!(da < db)) ppc::fp_compare_unordered(c, rate, zero, true);  // the first compare of a NaN
            if (da < db) {
                c.mem_write32(slot + 0x5F78u, kReverseValue);
                c.mem_write32(slot + 0x5F7Cu, kReverseSlope);
                c.mem_write32(slot + 0x5F80u, kReverseBoth);
            } else {
                ppc::fp_compare_unordered(c, rate, zero, true);  // the second
            }
        }
        if ((c.mem_read32(m + 0x14u) & 8u) != 0u) {
            const u32 a0 = c.mem_read32(ch + 0xA0u), a1 = c.mem_read32(ch + 0xA4u), a2 = c.mem_read32(ch + 0xA8u), a3 = c.mem_read32(ch + 0xACu);
            c.mem_write32(fr + 8u, a0);
            c.mem_write32(fr + 0xCu, a1);
            c.mem_write32(fr + 0x10u, a2);
            const u32 count = static_cast<u32>(ppc::fctiw(c, L(a3), true));
            c.mem_write32(fr + 0x14u, a3);
            const F f = ppc::fps_sub(c, 0x4330000000000000ull | static_cast<u64>(count ^ 0x80000000u), kMagic);
            SM(c, fr + 0x14u, f);
            const F w0 = LM(c, fr + 8u), w1 = LM(c, fr + 0xCu), w2 = LM(c, fr + 0x10u), w3 = LM(c, fr + 0x14u);
            SM(c, mtn_track::kContext, w0);
            SM(c, mtn_track::kContext + 4u, w1);
            SM(c, mtn_track::kContext + 8u, w2);
            SM(c, mtn_track::kContext + 0xCu, w3);
        } else {
            for (u32 k = 0; k < 16u; k += 4u) {
                const F w = LM(c, ch + 0xA0u + k);
                SM(c, mtn_track::kContext + k, w);
            }
        }
        for (u32 e = c.mem_read32(ch + 0x124u); e != 0u; e = c.mem_read32(e + 8u)) {
            const u32 fn = c.mem_read32(e);
            if (fn != 0u && !run_sampler_entry(c, fn, e + 0x10u, kLinkSamplerA)) return false;
        }
        if ((c.mem_read32(ch + 0x90u) & 8u) != 0u) {
            for (u32 e = c.mem_read32(ch + 0x124u); e != 0u; e = c.mem_read32(e + 8u)) {
                const u32 fn = c.mem_read32(e + 4u);
                if (fn != 0u && !run_sampler_entry(c, fn, e + 0x10u, kLinkSamplerB)) return false;
            }
        }
    }
    return true;
}

// ---- round 6, candidate D: the pose rebuild phase 0x80418618 (last phase of MotionUpdate) ---------------------------------------------
// Hand-written native body. Switch: on in the default build, off without its CMake option (GEKKO_MTN_PHASE_D=1, or the build option SHADOW_MTN_PHASE_D=ON ->
// GEKKO_MTN_PHASE_D_DEFAULT=1). It is an HLE hook on 0x80418618 (declines to the guest body when off or not provable) and the native
// MotionUpdate calls it directly. Hand-back before any write: a bone index above 3 in the channel table (the pass scratch has four
// slots), or a first active channel (flag 4) whose partner entry is not -1 (the original then reads an unwritten frame local).
#ifndef GEKKO_MTN_PHASE_D_DEFAULT
#define GEKKO_MTN_PHASE_D_DEFAULT 0
#endif
bool g_phase_d = GEKKO_MTN_PHASE_D_DEFAULT != 0;
std::atomic<unsigned long long> g_phase_d_hits[3];  // folded, handed back, channels visited
constexpr u32 kNegOne = 0xBF800000u;                 // r2 + 0x1704
constexpr u32 kLinkResetHook = 0x80419044u;          // liveness-roots: value-only
constexpr u32 kResetHook = 0x8041DDBCu;

void print_phase_d_stats() {
    std::fprintf(stderr, "[mtn_phase_d] folded %llu handed back %llu channels %llu\n", g_phase_d_hits[0].load(), g_phase_d_hits[1].load(),
                 g_phase_d_hits[2].load());
}

// The ordering pass of 0x80418618: like order_pass, over the motion channel table (+0x24) with the partner table (+0x2C) as parents.
bool order_pass_m(Cpu& c, u32 m) {
    bool found = false;
    for (s32 i = 0; i < static_cast<s32>(c.mem_read32(m + 0x20u)); ++i) {
        const u32 idx = c.mem_read32(c.mem_read32(m + 0x24u) + 4u * static_cast<u32>(i));
        if (idx == kNone) break;
        if (found && c.mem_read32(c.mem_read32(m + 0x2Cu) + (idx << 2)) == kNone) {
            c.mem_write32(flag_a(idx), 1u);
            continue;
        }
        c.mem_write32(flag_a(idx), 0u);
        const s32 n = static_cast<s32>(c.mem_read32(m + 0x20u));
        for (s32 j = i + 1; j < n; ++j) {
            const u32 v = c.mem_read32(c.mem_read32(m + 0x24u) + 4u * static_cast<u32>(j));
            if (v == kNone) break;
            if (c.mem_read32(flag_b(v)) == 0u && c.mem_read32(c.mem_read32(m + 0x2Cu) + (v << 2)) == idx) {
                c.mem_write32(flag_a(idx), 1u);
                found = true;
                break;
            }
        }
    }
    return found;
}

// 0x804125CC(out, 0, vec): out = the 3x3 part of the stack top applied to vec (three fused sums), through a temporary.
void rotate_by_top(Cpu& c, u32 out, u32 vec) {
    const u32 mat = stack_top(c);
    const F v0 = LM(c, vec), v1 = LM(c, vec + 4u), v2 = LM(c, vec + 8u);
    F r[3];
    for (u32 row = 0; row < 3u; ++row) {
        const u32 b = mat + 0x10u * row;
        F f0 = ppc::fps_mul(c, v1, LM(c, b + 4u));
        f0 = ppc::fps_madd(c, v0, LM(c, b), f0);
        f0 = ppc::fps_madd(c, v2, LM(c, b + 8u), f0);
        r[row] = f0;
    }
    SM(c, out, r[0]);
    SM(c, out + 4u, r[1]);
    SM(c, out + 8u, r[2]);
}

GEKKO_ALWAYS_INLINE bool head_ok(Cpu& c, u32 m) {  // the motion's "has two-part pose" test: halfword 1 and a low flag nibble
    const u32 head = c.mem_read32(c.mem_read32(m + 0xCu) + 0xCu);
    return c.mem_read16(head + 4u) == 1u && (c.mem_read16(head + 6u) & 7u) != 0u;
}

void clear_flags_m(Cpu& c, u32 m) {
    for (s32 i = 0; i < static_cast<s32>(c.mem_read32(m + 0x20u)); ++i) {
        const u32 idx = c.mem_read32(c.mem_read32(m + 0x24u) + 4u * static_cast<u32>(i));
        if (idx == kNone) break;
        c.mem_write32(flag_b(idx), 0u);
        c.mem_write32(flag_c(idx), 0u);
    }
}

bool phase_d_ok(Cpu& c, u32 m) {
    const s32 n = static_cast<s32>(c.mem_read32(m + 0x20u));
    const u32 table = c.mem_read32(m + 0x24u);
    bool seen_active = false;
    for (s32 i = 0; i < n; ++i) {
        const u32 idx = c.mem_read32(table + 4u * static_cast<u32>(i));
        if (idx == kNone) break;
        if (idx > 3u) return false;
        const u32 partner = c.mem_read32(c.mem_read32(m + 0x2Cu) + (idx << 2));
        if (partner != kNone && partner > 3u) return false;
        const u32 ch = c.mem_read32(m + 0x10u) + idx * kChannelStride;
        if (!seen_active && (c.mem_read32(ch + 0x90u) & 4u) != 0u) {
            seen_active = true;
            if (c.mem_read32(c.mem_read32(m + 0x2Cu) + (idx << 2)) != kNone) return false;
        }
    }
    return true;
}

bool phase_d(Cpu& c, u32 m) {
    FrameScope fs(c, 0x90u);
    const u32 fr = fs.frame;
    bool any_kind0 = false;  // r26
    if (head_ok(c, m)) {
        const F tx = LM(c, c.mem_read32(m + 4u) + 0xCu);
        SM(c, m + 0x130u, tx);
        const F ty = LM(c, c.mem_read32(m + 4u) + 0x1Cu);
        SM(c, m + 0x134u, ty);
        const F tz = LM(c, c.mem_read32(m + 4u) + 0x2Cu);
        SM(c, m + 0x138u, tz);
        if ((c.mem_read32(m + 0x14u) & 1u) != 0u) {
            SM(c, m + 0x140u, ppc::fps_sub(c, LM(c, m + 0x130u), LM(c, m + 0xF0u)));
            SM(c, m + 0x144u, ppc::fps_sub(c, LM(c, m + 0x134u), LM(c, m + 0xF4u)));
            SM(c, m + 0x148u, ppc::fps_sub(c, LM(c, m + 0x138u), LM(c, m + 0xF8u)));
            copy3(c, m + 0xF0u, m + 0x130u);
        } else {
            const F f2 = LM(c, m + 0x130u), f1 = LM(c, m + 0xF0u);
            SM(c, m + 0x140u, ppc::fps_sub(c, f2, f1));
            SM(c, m + 0x144u, L(kZero));
            SM(c, m + 0x148u, ppc::fps_sub(c, LM(c, m + 0x138u), LM(c, m + 0xF8u)));
            SM(c, m + 0xF0u, LM(c, m + 0x130u));
            SM(c, m + 0xF8u, LM(c, m + 0x138u));
        }
        copy3(c, m + 0x110u, m + 0xF0u);
        copy3(c, m + 0x120u, m + 0x100u);
        copy12(c, m + 0xC0u, m + 0x90u);
    }
    stack_push(c);
    copy12(c, stack_top(c), kIdentity);
    translate_by(c, stack_top(c), m + 0xF0u);
    compose(c, stack_top(c), m + 0x90u);
    c.gpr[3] = stack_top(c);
    math_affine::serve_invert_in_place(c);
    for (s32 i = 0; i < static_cast<s32>(c.mem_read32(m + 0x20u)); ++i) {
        const u32 idx = c.mem_read32(c.mem_read32(m + 0x24u) + 4u * static_cast<u32>(i));
        if (idx == kNone) break;
        const u32 ch = c.mem_read32(m + 0x10u) + idx * kChannelStride;
        if (i != 0 && (c.mem_read32(ch + 0x90u) & 4u) == 0u) continue;
        g_phase_d_hits[2].fetch_add(1, std::memory_order_relaxed);
        stack_push(c);
        compose(c, stack_top(c), ch);
        copy12(c, ch, stack_top(c));
        stack_pop(c);
        if ((c.mem_read32(ch + 0x90u) & 0x1000000u) != 0u) copy12(c, ch + 0x30u, ch);
        else copy_3x3(c, ch, ch + 0x30u);
        if (head_ok(c, m)) {
            if ((c.mem_read32(m + 0x14u) & 1u) != 0u) {
                SM(c, ch + 0xF0u, ppc::fps_sub(c, LM(c, ch + 0xE0u), LM(c, ch + 0xD0u)));
                SM(c, ch + 0xF4u, ppc::fps_sub(c, LM(c, ch + 0xE4u), LM(c, ch + 0xD4u)));
                SM(c, ch + 0xF8u, ppc::fps_sub(c, LM(c, ch + 0xE8u), LM(c, ch + 0xD8u)));
            } else {
                SM(c, ch + 0xF0u, ppc::fps_sub(c, LM(c, ch + 0xE0u), LM(c, ch + 0xD0u)));
                SM(c, ch + 0xF4u, L(kZero));
                SM(c, ch + 0xF8u, ppc::fps_sub(c, LM(c, ch + 0xE8u), LM(c, ch + 0xD8u)));
            }
            copy3(c, ch + 0xD0u, ch + 0xE0u);
        }
        const u32 params = c.mem_read32(ch + 0x120u);
        if ((c.mem_read32(params) & 1u) == 0u) continue;
        if (i == 0) {
            stack_push(c);
            compose(c, stack_top(c), m + 0x30u);
            copy12(c, m + 0x30u, stack_top(c));
            stack_pop(c);
            if ((c.mem_read32(m + 0x14u) & 0x10000u) != 0u) copy12(c, m + 0x60u, m + 0x30u);
            else copy_3x3(c, m + 0x30u, m + 0x60u);
        }
        const s32 kind = static_cast<s32>(c.mem_read32(c.mem_read32(ch + 0x120u) + 8u));
        if (kind == 1) {
            const u32 partner = c.mem_read32(c.mem_read32(m + 0x2Cu) + (idx << 2));
            if (partner == kNone) continue;
            const u32 ch2 = c.mem_read32(m + 0x10u) + partner * kChannelStride;
            if ((c.mem_read32(ch2 + 0x90u) & 4u) == 0u) continue;
            const u32 rec = c.mem_read32(c.mem_read32(m + 0xCu) + 0x18u) + (idx << 5);
            F t = LM(c, c.mem_read32(ch + 0x120u) + 0x10u);
            if (c.mem_read32(rec) != 0u) {
                const F f1 = LM(c, rec + 8u), f0 = LM(c, rec + 0xCu);
                t = ppc::fps_madd(c, t, f1, f0);
            }
            copy3(c, fr + 0x44u, ch2 + 0xF0u);
            copy3(c, fr + 0x38u, ch + 0xF0u);
            stack_push(c);
            copy12(c, stack_top(c), ch2);
            {
                const F a0 = LM(c, fr + 0x38u), b0 = LM(c, fr + 0x44u), a1 = LM(c, fr + 0x3Cu), b1 = LM(c, fr + 0x48u);
                const F a2 = LM(c, fr + 0x40u), b2 = LM(c, fr + 0x4Cu);
                const F d0 = ppc::fps_sub(c, a0, b0), d1 = ppc::fps_sub(c, a1, b1), d2 = ppc::fps_sub(c, a2, b2);
                SM(c, fr + 0x2Cu, ppc::fps_mul(c, t, d0));
                SM(c, fr + 0x30u, ppc::fps_mul(c, t, d1));
                SM(c, fr + 0x34u, ppc::fps_mul(c, t, d2));
            }
            translate_by(c, stack_top(c), fr + 0x2Cu);
            copy12(c, ch2, stack_top(c));
            copy12(c, stack_top(c), ch);
            {
                const F inv = ppc::fps_sub(c, L(kOne), t);
                const F a0 = LM(c, fr + 0x44u), b0 = LM(c, fr + 0x38u), a1 = LM(c, fr + 0x48u), b1 = LM(c, fr + 0x3Cu);
                const F a2 = LM(c, fr + 0x4Cu), b2 = LM(c, fr + 0x40u);
                const F d0 = ppc::fps_sub(c, a0, b0), d1 = ppc::fps_sub(c, a1, b1), d2 = ppc::fps_sub(c, a2, b2);
                SM(c, fr + 0x2Cu, ppc::fps_mul(c, inv, d0));
                SM(c, fr + 0x30u, ppc::fps_mul(c, inv, d1));
                SM(c, fr + 0x34u, ppc::fps_mul(c, inv, d2));
            }
            translate_by(c, stack_top(c), fr + 0x2Cu);
            copy12(c, ch, stack_top(c));
            stack_pop(c);
            {
                const F r1 = LM(c, ch + 0xA4u), r0 = LM(c, ch + 0xA0u), f3 = LM(c, ch2 + 0xA8u);
                F f2 = ppc::fps_sub(c, r1, r0);
                const F q1 = LM(c, ch2 + 0xA4u), q0 = LM(c, ch2 + 0xA0u);
                f2 = ppc::fps_mul(c, f3, f2);
                const F f0 = ppc::fps_sub(c, q1, q0);
                SM(c, ch + 0xA8u, ppc::fps_div(c, f2, f0));
            }
        } else if (kind == 0) {
            if ((c.mem_read32(m + 0x14u) & 4u) != 0u && head_ok(c, m)) any_kind0 = true;
        }
    }
    if (any_kind0) {
        copy3(c, fr + 0x14u, m + 0x140u);
        rotate_by_top(c, fr + 0x14u, fr + 0x14u);
        stack_push(c);
        clear_flags_m(c, m);
        bool found;
        do {
            found = order_pass_m(c, m);
            for (s32 j = 0; j < static_cast<s32>(c.mem_read32(m + 0x20u)); ++j) {
                const u32 idx = c.mem_read32(c.mem_read32(m + 0x24u) + 4u * static_cast<u32>(j));
                if (idx == kNone) break;
                if (c.mem_read32(flag_b(idx)) != 0u) continue;
                const u32 ch = c.mem_read32(m + 0x10u) + idx * kChannelStride;
                copy3(c, fr + 0x20u, ch + 0xF0u);
                copy12(c, stack_top(c), ch);
                rotate_by_top(c, fr + 0x20u, fr + 0x20u);
                if (c.mem_read32(flag_a(idx)) != 0u) {
                    if (c.mem_read32(flag_c(idx)) != 0u) continue;
                    copy3(c, vector_at(idx), fr + 0x20u);
                    c.mem_write32(flag_c(idx), 1u);
                    continue;
                }
                c.mem_write32(flag_b(idx), 1u);
                const u32 parent = c.mem_read32(c.mem_read32(m + 0x2Cu) + (idx << 2));
                u32 dst;
                if (parent == kNone) {
                    dst = vector_at(idx);
                } else {
                    dst = vector_at(parent);
                    c.mem_write32(flag_c(parent), 1u);
                }
                const u32 src = c.mem_read32(flag_c(idx)) != 0u ? vector_at(idx) : fr + 0x20u;
                const u32 params = c.mem_read32(ch + 0x120u);
                if ((c.mem_read32(params) & 1u) == 0u || parent == kNone) {
                    copy3(c, dst, src);
                    continue;
                }
                const u32 rec = c.mem_read32(c.mem_read32(m + 0xCu) + 0x18u) + (idx << 5);
                F f3 = LM(c, params + 0x10u);
                if (c.mem_read32(rec) != 0u) {
                    const F f1 = LM(c, rec + 8u), f0 = LM(c, rec + 0xCu);
                    f3 = ppc::fps_madd(c, f3, f1, f0);
                }
                const F f2 = ppc::fps_sub(c, L(kOne), f3);
                for (u32 k = 0; k < 12u; k += 4u) {
                    F f0 = LM(c, dst + k);
                    const F f1 = LM(c, src + k);
                    f0 = ppc::fps_mul(c, f2, f0);
                    f0 = ppc::fps_madd(c, f3, f1, f0);
                    SM(c, dst + k, f0);
                }
            }
        } while (found);
        for (s32 j = 0; j < static_cast<s32>(c.mem_read32(m + 0x20u)); ++j) {
            const u32 idx = c.mem_read32(c.mem_read32(m + 0x24u) + 4u * static_cast<u32>(j));
            if (idx == kNone) break;
            const u32 ch = c.mem_read32(m + 0x10u) + idx * kChannelStride;
            if ((c.mem_read32(ch + 0x90u) & 4u) == 0u) continue;
            if (j == 0) {
                copy12(c, stack_top(c), kIdentity);
                translate_by(c, stack_top(c), fr + 0x14u);
                compose(c, stack_top(c), m + 0x30u);
                copy12(c, m + 0x30u, stack_top(c));
            }
            if (c.mem_read32(c.mem_read32(m + 0x2Cu) + (idx << 2)) == kNone) copy3(c, fr + 8u, vector_at(idx));
            {
                const F a0 = LM(c, fr + 0x14u), b0 = LM(c, fr + 8u), a1 = LM(c, fr + 0x18u), b1 = LM(c, fr + 0xCu);
                const F a2 = LM(c, fr + 0x1Cu), b2 = LM(c, fr + 0x10u);
                SM(c, fr + 0x20u, ppc::fps_sub(c, a0, b0));
                SM(c, fr + 0x24u, ppc::fps_sub(c, a1, b1));
                SM(c, fr + 0x28u, ppc::fps_sub(c, a2, b2));
            }
            copy12(c, stack_top(c), kIdentity);
            translate_by(c, stack_top(c), fr + 0x20u);
            compose(c, stack_top(c), ch);
            copy12(c, ch, stack_top(c));
        }
        stack_pop(c);
    }
    stack_pop(c);
    for (s32 i = 0; i < static_cast<s32>(c.mem_read32(m + 0x20u)); ++i) {
        const u32 ch = c.mem_read32(m + 0x10u) + 0x130u * static_cast<u32>(i);
        c.mem_write16(ch + 0x110u, 0u);
        SM(c, ch + 0x100u, L(kNegOne));
        SM(c, ch + 0x104u, L(kNegOne));
        SM(c, ch + 0x108u, LM(c, ch + 0xA8u));
        SM(c, ch + 0x10Cu, L(kNegOne));
        if ((c.mem_read32(ch + 0x94u) & 0x2000u) != 0u && !call(c, kResetHook, kLinkResetHook, {c.mem_read32(m + 0xCu), static_cast<u32>(i)})) return false;
        c.mem_write32(ch + 0x94u, 0u);
    }
    c.mem_write32(m + 0x14u, c.mem_read32(m + 0x14u) & 0x00FFFFFFu);
    for (u32 k = 0; k < 12u; k += 4u) c.mem_write32(m + 0x150u + k, 0u);
    return true;
}

bool serve_phase_d_hook(Cpu& c) {
    if (!g_phase_d) return false;
    if (!phase_d_ok(c, c.gpr[3])) {
        g_phase_d_hits[1].fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    g_phase_d_hits[0].fetch_add(1, std::memory_order_relaxed);
    phase_d(c, c.gpr[3]);
    return true;
}

// ---- round 6, candidate C: the time advance phase 0x80419A9C (third phase of MotionUpdate) ---------------------------------------------
// Hand-written native body. Switch: on in the default build, off without its CMake option (GEKKO_MTN_PHASE_C=1, or the build option SHADOW_MTN_PHASE_C=ON ->
// GEKKO_MTN_PHASE_C_DEFAULT=1). An HLE hook on 0x80419A9C (declines to the guest body when off or not provable) that the native MotionUpdate
// also calls directly. Hand-back before any write: a null parameter block or partner entry the walk would dereference at address 0.
#ifndef GEKKO_MTN_PHASE_C_DEFAULT
#define GEKKO_MTN_PHASE_C_DEFAULT 0
#endif
bool g_phase_c = GEKKO_MTN_PHASE_C_DEFAULT != 0;
std::atomic<unsigned long long> g_phase_c_hits[3];  // folded, handed back, channels visited
constexpr u32 kEps4 = 0x38D1B717u;                  // r2 + 0x16FC = 1e-4
constexpr u32 kPhaseFinish = 0x8041DC68u;
constexpr u32 kPhaseRelease = 0x80414290u;
constexpr u32 kPhaseInstall = 0x8041A6BCu;
constexpr u32 kPhaseWrap = 0x8041ABF8u;
constexpr u32 kPhaseMoved = 0x8041BF20u;
constexpr u32 kLinkFinish = 0x80419C2Cu;    // liveness-roots: value-only
constexpr u32 kLinkRelease = 0x80419C58u;   // liveness-roots: value-only
constexpr u32 kLinkInstall = 0x80419D5Cu;   // liveness-roots: value-only
constexpr u32 kLinkWrapA = 0x80419FE0u;     // liveness-roots: value-only
constexpr u32 kLinkWrapB = 0x80419FFCu;     // liveness-roots: value-only
constexpr u32 kLinkWrapC = 0x8041A174u;     // liveness-roots: value-only
constexpr u32 kLinkWrapD = 0x8041A190u;     // liveness-roots: value-only
constexpr u32 kLinkMoved = 0x8041A318u;     // liveness-roots: value-only

void print_phase_c_stats() {
    std::fprintf(stderr, "[mtn_phase_c] folded %llu handed back %llu channels %llu\n", g_phase_c_hits[0].load(), g_phase_c_hits[1].load(),
                 g_phase_c_hits[2].load());
}

// Test hook, compiled ONLY into the contract test (GEKKO_MTN_TEST_HOOKS is defined on the mtn_pose_contract_tests target, which compiles this file
// itself); the game's phase C is the code without it (wrap_cap_hit() is the constant false and its call sites vanish). With the hook: stop the phase,
// leaving memory as it is, when the wrap loops have arrived at their loop test more than `cap` times in total since the cap was set (0 = unlimited). The
// ORIGINAL body is frozen at the same arrival, so the contract can compare the state of the loops that never terminate (a time so large that
// `time - length == time` in single precision, or an infinity).
#ifdef GEKKO_MTN_TEST_HOOKS
std::atomic<unsigned long long> g_wrap_cap{0}, g_wrap_seen{0};
GEKKO_ALWAYS_INLINE bool wrap_cap_hit() noexcept {
    const unsigned long long cap = g_wrap_cap.load(std::memory_order_relaxed);
    return cap != 0ull && g_wrap_seen.fetch_add(1, std::memory_order_relaxed) + 1ull > cap;
}
#else
constexpr bool wrap_cap_hit() noexcept { return false; }
#endif

// fcmpo a, b; cror eq, gt, eq; the branch on eq: "a >= b" (a NaN raises the exception bits and is false).
bool ge_cror(Cpu& c, F a, F b) {
    const double da = bits_to_double(a), db = bits_to_double(b);
    if (da > db || da == db) return true;
    if (!(da < db)) ppc::fp_compare_unordered(c, a, b, true);
    return false;
}
// the mirror, cror eq, lt, eq: "a <= b"
bool le_cror(Cpu& c, F a, F b) {
    const double da = bits_to_double(a), db = bits_to_double(b);
    if (da < db || da == db) return true;
    if (!(da > db)) ppc::fp_compare_unordered(c, a, b, true);
    return false;
}

GEKKO_ALWAYS_INLINE F fabs_single(Cpu& c, F v) { return ppc::fps_round(c, v & 0x7FFFFFFFFFFFFFFFull); }  // 0x8000A980: fabs then frsp
GEKKO_ALWAYS_INLINE F dt_value(Cpu& c) { return LM(c, c.gpr[13] + 0x2910u); }  // the frame time in the small data area
GEKKO_ALWAYS_INLINE void flag_or(Cpu& c, u32 address, u32 bits) { c.mem_write32(address, c.mem_read32(address) | bits); }
GEKKO_ALWAYS_INLINE void flag_clear(Cpu& c, u32 address, u32 bits) { c.mem_write32(address, c.mem_read32(address) & ~bits); }

void clamp_time(Cpu& c, u32 ch) {
    u32 p = c.mem_read32(ch + 0x120u);
    F f1 = LM(c, p + 0x10u);
    if (lt_ordered(c, f1, L(kZero))) SM(c, p + 0x10u, L(kZero));
    p = c.mem_read32(ch + 0x120u);
    f1 = LM(c, p + 0x10u);
    if (gt_ordered(c, f1, L(kOne))) SM(c, p + 0x10u, L(kOne));
}

// 0x80419B7C..0x80419D5C: the channel `idx` has run out: clear it and the channels before it in the table, release their links, drop them from
// the secondary table (+0x28), compact the channel table (+0x24) and, when it is left empty, install the next motion (0x8041A6BC).
bool channel_finish(Cpu& c, u32 m, u32 idx, u32 ch, u32& outer) {
    flag_or(c, ch + 0x90u, 0x800000u);
    s32 found = -1;  // r26
    for (s32 j = 0; j < static_cast<s32>(c.mem_read32(m + 0x20u)); ++j) {
        const u32 other = c.mem_read32(c.mem_read32(m + 0x24u) + 4u * static_cast<u32>(j));
        if (other == kNone) break;
        if (found == -1) {
            if (other == idx) {
                found = j;
            } else {
                const u32 off = other * kChannelStride;
                u32 base = c.mem_read32(m + 0x10u);
                const u32 params = c.mem_read32(base + off + 0x120u);
                c.mem_write32(params, c.mem_read32(params) & ~2u);
                base = c.mem_read32(m + 0x10u);
                flag_clear(c, base + off + 0x90u, 1u);
                base = c.mem_read32(m + 0x10u);
                flag_clear(c, base + off + 0x90u, 2u);
                base = c.mem_read32(m + 0x10u);
                flag_clear(c, base + off + 0x90u, 4u);
                base = c.mem_read32(m + 0x10u);
                if ((c.mem_read32(base + off + 0x90u) & 0x10u) != 0u) {
                    if (!call(c, kPhaseFinish, kLinkFinish, {c.mem_read32(m + 0xCu), other})) return false;
                    base = c.mem_read32(m + 0x10u);
                    flag_clear(c, base + off + 0x90u, 0x10u);
                }
                const u32 half = c.mem_read16(c.mem_read32(m + 0x10u) + off + 0x128u);
                if (half != 0u && !call(c, kPhaseRelease, kLinkRelease, {half})) return false;
                const s32 count = static_cast<s32>(c.mem_read32(m + 0x20u));
                if (count > 0) {
                    s32 k = 0, ctr = count;
                    u32 byte = 0;
                    for (;;) {
                        const u32 table = c.mem_read32(m + 0x28u);
                        const u32 entry = c.mem_read32(table + byte);
                        if (entry == kNone) break;
                        if (other != entry) {
                            ++k;
                            byte += 4u;
                            if (--ctr == 0) break;
                            continue;
                        }
                        c.mem_write32(table + byte, 0xFFFFFFFFu);
                        u32 at = static_cast<u32>(k) * 4u;
                        while (k < static_cast<s32>(c.mem_read32(m + 0x20u)) - 1) {
                            const u32 t2 = c.mem_read32(m + 0x28u);
                            const u32 next = at + 4u;
                            ++k;
                            const u32 slot = t2 + at;
                            at += 4u;
                            c.mem_write32(slot, c.mem_read32(slot + 4u));
                            c.mem_write32(c.mem_read32(m + 0x28u) + next, 0xFFFFFFFFu);
                        }
                        break;
                    }
                }
            }
            // 0x80419CDC: the entry's own partner link and flag
            c.mem_write32(c.mem_read32(m + 0x2Cu) + (other << 2), 0xFFFFFFFFu);
            const u32 p3 = c.mem_read32(c.mem_read32(m + 0x10u) + other * kChannelStride + 0x120u);
            c.mem_write32(p3, c.mem_read32(p3) & ~1u);
        }
        c.mem_write32(c.mem_read32(m + 0x24u) + 4u * static_cast<u32>(j), 0xFFFFFFFFu);
        if (found != -1) c.mem_write32(c.mem_read32(m + 0x24u) + 4u * static_cast<u32>(j - found), other);
    }
    outer = 0;  // the original leaves r31 (the outer index) at 0 here
    if (c.mem_read32(c.mem_read32(m + 0x24u) + 4u) == kNone && !call(c, kPhaseInstall, kLinkInstall, {c.mem_read32(m + 0xCu)})) return false;
    return true;
}

// kind 1 (follow a partner channel): copy its flags, warp the time to the partner's range.
void channel_follow(Cpu& c, u32 m, u32 idx, u32 ch) {
    clamp_time(c, ch);
    const u32 partner = c.mem_read32(c.mem_read32(m + 0x2Cu) + (idx << 2));
    if (partner == kNone) return;
    const u32 ch2 = c.mem_read32(m + 0x10u) + partner * kChannelStride;
    const u32 fl2 = c.mem_read32(ch2 + 0x90u);
    if ((fl2 & 4u) == 0u) return;
    if ((fl2 & 1u) != 0u) flag_or(c, ch + 0x90u, 1u);
    else flag_clear(c, ch + 0x90u, 1u);
    if ((c.mem_read32(ch2 + 0x90u) & 2u) != 0u) flag_or(c, ch + 0x90u, 2u);
    else flag_clear(c, ch + 0x90u, 2u);
    if ((c.mem_read32(ch2 + 0x94u) & 0x400u) != 0u) flag_or(c, ch + 0x94u, 0x400u);
    else flag_clear(c, ch + 0x94u, 0x400u);
    const F f3 = LM(c, ch2 + 0xA0u);
    const F f1a = LM(c, ch2 + 0xACu);
    const F f0a = LM(c, ch2 + 0xA4u);
    const F f2a = ppc::fps_sub(c, f1a, f3);
    const F f4 = LM(c, ch + 0xA0u);
    const F f1b = ppc::fps_sub(c, f0a, f3);
    const F f3b = LM(c, ch + 0xA4u);
    const F f5 = LM(c, ch2 + 0xA8u);
    const F zero = L(kZero);
    const F f1c = ppc::fps_div(c, f2a, f1b);
    F f2b = ppc::fps_sub(c, f3b, f4);
    const bool rate_ge = ge_cror(c, f5, zero);  // fcmpo f5, f0 ; cror
    f2b = ppc::fps_madd(c, f2b, f1c, f4);
    if (rate_ge) {
        F f0 = LM(c, ch + 0xACu);
        if (lt_ordered(c, f2b, f0)) {
            f0 = ppc::fps_sub(c, f3b, f0);
            SM(c, ch + 0xA8u, f0);
            f0 = LM(c, ch + 0xA0u);
            const F f1 = LM(c, ch + 0xA8u);
            f0 = ppc::fps_sub(c, f2b, f0);
            f0 = ppc::fps_add(c, f1, f0);
            SM(c, ch + 0xA8u, f0);
        } else {
            f0 = ppc::fps_sub(c, f2b, f0);
            SM(c, ch + 0xA8u, f0);
        }
    } else if (lt_ordered(c, f5, zero)) {  // fcmpo f5, f0 ; bge
        F f0 = LM(c, ch + 0xACu);
        if (gt_ordered(c, f2b, f0)) {
            f0 = ppc::fps_sub(c, f4, f0);
            SM(c, ch + 0xA8u, f0);
            f0 = LM(c, ch + 0xA4u);
            const F f1 = LM(c, ch + 0xA8u);
            f0 = ppc::fps_sub(c, f2b, f0);
            f0 = ppc::fps_add(c, f1, f0);
            SM(c, ch + 0xA8u, f0);
        } else {
            f0 = ppc::fps_sub(c, f2b, f0);
            SM(c, ch + 0xA8u, f0);
        }
    }
    const F dt = dt_value(c);
    if (gt_ordered(c, dt, L(kEps4))) SM(c, ch + 0xA8u, ppc::fps_div(c, LM(c, ch + 0xA8u), dt));
    flag_clear(c, ch + 0x90u, 0x20000u);
}

// The wrap of the parameter past its end (rate > 0) or start (rate < 0): with a two-part pose the position change is accumulated in the
// channel's matrix; the guest helper 0x8041ABF8 evaluates the channel at the two ends.
bool wrap_forward(Cpu& c, u32 m, u32 ch, bool& moved, u32 fr) {
    if (head_ok(c, m)) {
        const F a0 = LM(c, ch + 0xA0u), a1 = LM(c, ch + 0xA4u), a2 = LM(c, ch + 0xA8u), a3 = LM(c, ch + 0xACu);
        SM(c, fr + 0x5Cu, a0);
        SM(c, fr + 0x60u, a1);
        SM(c, fr + 0x64u, a2);
        SM(c, fr + 0x68u, a3);
        SM(c, fr + 0x68u, a1);
        if (!call(c, kPhaseWrap, kLinkWrapA, {fr + 0x50u, ch, fr + 0x5Cu, moved ? 1u : 0u})) return false;
        const F f0 = LM(c, fr + 0x5Cu);
        SM(c, fr + 0x68u, f0);
        if (!call(c, kPhaseWrap, kLinkWrapB, {fr + 0x44u, ch, fr + 0x5Cu, 1u})) return false;
        c.gpr[3] = fr + 0x2Cu;
        c.gpr[4] = fr + 0x50u;
        c.gpr[5] = fr + 0x44u;
        vec_math::serve_sub(c);
        copy3(c, fr + 0x38u, fr + 0x2Cu);
        if ((c.mem_read32(m + 0x14u) & 1u) == 0u) SM(c, fr + 0x3Cu, L(kZero));
        stack_push(c);
        copy12(c, stack_top(c), ch);
        for (;;) {
            if (wrap_cap_hit()) return false;
            const F cur = LM(c, ch + 0xACu), end = LM(c, ch + 0xA4u);
            if (!ge_cror(c, cur, end)) break;
            F len = ppc::fps_sub(c, end, LM(c, ch + 0xA0u));
            len = ppc::fps_sub(c, LM(c, ch + 0xACu), len);
            SM(c, ch + 0xACu, len);
            c.gpr[3] = fr + 0x20u;
            c.gpr[4] = ch + 0xD0u;
            c.gpr[5] = fr + 0x38u;
            vec_math::serve_sub(c);
            copy3(c, ch + 0xD0u, fr + 0x20u);
            translate_by(c, stack_top(c), fr + 0x38u);
        }
        copy12(c, ch, stack_top(c));
        stack_pop(c);
    } else {
        for (;;) {
            if (wrap_cap_hit()) return false;
            const F cur = LM(c, ch + 0xACu), end = LM(c, ch + 0xA4u);
            if (!ge_cror(c, cur, end)) break;
            F len = ppc::fps_sub(c, end, LM(c, ch + 0xA0u));
            len = ppc::fps_sub(c, LM(c, ch + 0xACu), len);
            SM(c, ch + 0xACu, len);
        }
    }
    flag_or(c, ch + 0x90u, 0x400000u);
    moved = true;
    return true;
}

bool wrap_backward(Cpu& c, u32 m, u32 ch, bool& moved, u32 fr) {
    if (head_ok(c, m)) {
        const F start = LM(c, ch + 0xA0u);
        SM(c, fr + 0x5Cu, start);
        SM(c, fr + 0x60u, LM(c, ch + 0xA4u));
        SM(c, fr + 0x64u, LM(c, ch + 0xA8u));
        SM(c, fr + 0x68u, LM(c, ch + 0xACu));
        SM(c, fr + 0x68u, start);
        if (!call(c, kPhaseWrap, kLinkWrapC, {fr + 0x44u, ch, fr + 0x5Cu, moved ? 1u : 0u})) return false;
        const F f0 = LM(c, fr + 0x60u);
        SM(c, fr + 0x68u, f0);
        if (!call(c, kPhaseWrap, kLinkWrapD, {fr + 0x50u, ch, fr + 0x5Cu, 1u})) return false;
        c.gpr[3] = fr + 0x14u;
        c.gpr[4] = fr + 0x44u;
        c.gpr[5] = fr + 0x50u;
        vec_math::serve_sub(c);
        copy3(c, fr + 0x38u, fr + 0x14u);
        if ((c.mem_read32(m + 0x14u) & 1u) == 0u) SM(c, fr + 0x3Cu, L(kZero));
        stack_push(c);
        copy12(c, stack_top(c), ch);
        for (;;) {
            if (wrap_cap_hit()) return false;
            const F cur = LM(c, ch + 0xACu), st = LM(c, ch + 0xA0u);
            if (!le_cror(c, cur, st)) break;
            F len = ppc::fps_sub(c, LM(c, ch + 0xA4u), st);
            len = ppc::fps_add(c, LM(c, ch + 0xACu), len);
            SM(c, ch + 0xACu, len);
            c.gpr[3] = fr + 8u;
            c.gpr[4] = ch + 0xD0u;
            c.gpr[5] = fr + 0x38u;
            vec_math::serve_sub(c);
            copy3(c, ch + 0xD0u, fr + 8u);
            translate_by(c, stack_top(c), fr + 0x38u);
        }
        copy12(c, ch, stack_top(c));
        stack_pop(c);
    } else {
        for (;;) {
            if (wrap_cap_hit()) return false;
            const F cur = LM(c, ch + 0xACu), st = LM(c, ch + 0xA0u);
            if (!le_cror(c, cur, st)) break;
            F len = ppc::fps_sub(c, LM(c, ch + 0xA4u), st);
            len = ppc::fps_add(c, LM(c, ch + 0xACu), len);
            SM(c, ch + 0xACu, len);
        }
    }
    flag_or(c, ch + 0x90u, 0x400000u);
    moved = true;
    return true;
}

// 0x80419F04: advance the playback time and handle the end (loop / wrap for the ping-pong flag, stop otherwise).
bool channel_advance(Cpu& c, u32 m, u32 ch, bool& moved, u32 fr) {
    const u32 flags = c.mem_read32(ch + 0x90u);
    if ((flags & 0x40000u) != 0u) moved = true;
    if ((flags & 0x20000u) != 0u) return true;
    const F rate = LM(c, ch + 0xA8u);
    F time = ppc::fps_madd(c, rate, dt_value(c), LM(c, ch + 0xACu));
    SM(c, ch + 0xACu, time);
    const F zero = L(kZero);
    if ((c.mem_read32(ch + 0x90u) & 2u) != 0u) {
        F span = ppc::fps_sub(c, LM(c, ch + 0xA4u), LM(c, ch + 0xA0u));
        span = fabs_single(c, span);
        if (lt_ordered(c, span, L(kEps4))) {  // a zero-length range: pin to the end
            SM(c, ch + 0xACu, LM(c, ch + 0xA4u));
            return true;
        }
        const F r = LM(c, ch + 0xA8u);
        if (gt_ordered(c, r, zero)) {
            if (!ge_cror(c, LM(c, ch + 0xACu), LM(c, ch + 0xA4u))) return true;
            return wrap_forward(c, m, ch, moved, fr);
        }
        if (bits_to_double(r) < bits_to_double(zero)) {  // the same fcmpo, branch bge: rate < 0
            if (!le_cror(c, LM(c, ch + 0xACu), LM(c, ch + 0xA0u))) return true;
            return wrap_backward(c, m, ch, moved, fr);
        }
        return true;
    }
    const F r = LM(c, ch + 0xA8u);
    if (ge_cror(c, r, zero)) {
        const F cur = LM(c, ch + 0xACu), end = LM(c, ch + 0xA4u);
        if (!ge_cror(c, cur, end)) return true;
        SM(c, ch + 0xACu, end);
        flag_or(c, ch + 0x90u, 0x400000u);
        return true;
    }
    if (lt_ordered(c, r, zero)) {
        const F cur = LM(c, ch + 0xACu), st = LM(c, ch + 0xA0u);
        if (!le_cror(c, cur, st)) return true;
        SM(c, ch + 0xACu, st);
        flag_or(c, ch + 0x90u, 0x400000u);
    }
    return true;
}

bool phase_c_ok(Cpu& c, u32 m) {
    const s32 n = static_cast<s32>(c.mem_read32(m + 0x20u));
    const u32 table = c.mem_read32(m + 0x24u);
    for (s32 i = 0; i < n; ++i) {
        const u32 idx = c.mem_read32(table + 4u * static_cast<u32>(i));
        if (idx == kNone) break;
        const u32 ch = c.mem_read32(m + 0x10u) + idx * kChannelStride;
        if ((c.mem_read32(ch + 0x90u) & 4u) != 0u && c.mem_read32(ch + 0x120u) == 0u) return false;
    }
    return true;
}

bool phase_c(Cpu& c, u32 m) {
    FrameScope fs(c, 0xA0u);
    const u32 fr = fs.frame;
    for (s32 i = 0; i < static_cast<s32>(c.mem_read32(m + 0x20u)); ++i) {
        const u32 idx = c.mem_read32(c.mem_read32(m + 0x24u) + 4u * static_cast<u32>(i));
        if (idx == kNone) break;
        const u32 ch = c.mem_read32(m + 0x10u) + idx * kChannelStride;
        if ((c.mem_read32(ch + 0x90u) & 4u) == 0u) continue;
        g_phase_c_hits[2].fetch_add(1, std::memory_order_relaxed);
        bool moved = false;
        const u32 params = c.mem_read32(ch + 0x120u);
        if ((c.mem_read32(params) & 1u) != 0u) {
            const s32 kind = static_cast<s32>(c.mem_read32(params + 8u));
            if (kind == 1) {
                channel_follow(c, m, idx, ch);
            } else if (kind == 0) {
                F f3 = LM(c, params + 0x1Cu);
                const F dt = dt_value(c);
                F f1 = LM(c, params + 0x20u);
                f1 = ppc::fps_madd(c, f3, dt, f1);
                SM(c, params + 0x20u, f1);
                const u32 p2 = c.mem_read32(ch + 0x120u);
                F f2 = LM(c, p2 + 0x20u);
                f1 = LM(c, p2 + 0x18u);
                f1 = ppc::fps_div(c, f2, f1);
                SM(c, p2 + 0x10u, f1);
                const u32 p3 = c.mem_read32(ch + 0x120u);
                f1 = LM(c, p3 + 0x10u);
                if (gt_ordered(c, f1, L(kNearOne))) {
                    u32 outer = static_cast<u32>(i);
                    if (!channel_finish(c, m, idx, ch, outer)) return false;
                    i = static_cast<s32>(outer);
                }
            } else {
                clamp_time(c, ch);
            }
        }
        if (!channel_advance(c, m, ch, moved, fr)) return false;
        if (moved && !call(c, kPhaseMoved, kLinkMoved, {m, idx})) return false;
    }
    return true;
}

bool serve_phase_c_hook(Cpu& c) {
    if (!g_phase_c) return false;
    if (!phase_c_ok(c, c.gpr[3])) {
        g_phase_c_hits[1].fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    g_phase_c_hits[0].fetch_add(1, std::memory_order_relaxed);
    phase_c(c, c.gpr[3]);
    return true;
}

// ---- round 7, candidate B: the clip start phase 0x8041A33C (second phase of MotionUpdate) and the tree below it ---------------------------
// Hand-written native bodies for 0x8041A33C (per-channel start / state flags), 0x8041BDE4 (range clamp flags), 0x8041ADAC (start a clip on a
// channel), 0x8041C1C0 (bind), 0x8041C74C (reset the track state arrays), 0x8041C30C (bind the key records of a node tree) and 0x8041E498
// (node search), with 0x80414E88 (track state init: mtn_track::state_init), 0x8000540C / 0x800054F4 (memset / memcpy of Mtn-owned memory) and
// 0x8000A980 (fabs) inlined. The clip database and allocator side stays GUEST code called where the original calls it: 0x80414368 / 0x80414290
// (clip cache acquire / release), 0x80412E3C (query), 0x80413644 / 0x8041342C / 0x8041174C (clip lookup), 0x8041352C (record cursor), and the
// phases 0x8041A508 / 0x8041A6BC / 0x8041A934 / 0x8041ABF8 / 0x8041DEC4. HLE hook on 0x8041A33C (declines to the guest body when off); the
// native MotionUpdate calls it directly. Switch: off by default (GEKKO_MTN_PHASE_B=1, or the build option SHADOW_MTN_PHASE_B=ON ->
// GEKKO_MTN_PHASE_B_DEFAULT=1). Hand-back before any write: an active channel (flag 4) without a parameter block.
#ifndef GEKKO_MTN_PHASE_B_DEFAULT
#define GEKKO_MTN_PHASE_B_DEFAULT 0
#endif
bool g_phase_b = GEKKO_MTN_PHASE_B_DEFAULT != 0;
std::atomic<unsigned long long> g_phase_b_hits[5];  // phase runs, handed back, clips started, nodes bound, searches
constexpr u32 kNegEps4 = 0xB8D1B717u;               // r2 + 0x1714
constexpr u32 kClipAcquire = 0x80414368u;
constexpr u32 kClipRelease = 0x80414290u;
constexpr u32 kClipQuery = 0x80412E3Cu;
constexpr u32 kClipLookup = 0x80413644u;
constexpr u32 kClipHandle = 0x8041342Cu;
constexpr u32 kClipInfo = 0x8041174Cu;
constexpr u32 kRecordCursor = 0x8041352Cu;
constexpr u32 kBindCallback = 0x8041DEC4u;
constexpr u32 kPhaseStartA = 0x8041A934u;
constexpr u32 kPhaseInstallA = 0x8041A508u;
constexpr u32 kLinkAcquire = 0x8041ADD8u;      // liveness-roots: value-only
constexpr u32 kLinkReleaseA = 0x8041ADECu;     // liveness-roots: value-only
constexpr u32 kLinkQuery = 0x8041AEECu;        // liveness-roots: value-only
constexpr u32 kLinkReleaseB = 0x8041AEB4u;     // liveness-roots: value-only
constexpr u32 kLinkLookup = 0x8041C20Cu;       // liveness-roots: value-only
constexpr u32 kLinkHandle = 0x8041C26Cu;       // liveness-roots: value-only
constexpr u32 kLinkInfo = 0x8041C274u;         // liveness-roots: value-only
constexpr u32 kLinkCursor = 0x8041C398u;       // liveness-roots: value-only
constexpr u32 kLinkBindCb = 0x8041A474u;       // liveness-roots: value-only
constexpr u32 kLinkWrapE = 0x8041A4A0u;        // liveness-roots: value-only
constexpr u32 kLinkStartA = 0x8041A4BCu;       // liveness-roots: value-only
constexpr u32 kLinkInstallB = 0x8041A4E0u;     // liveness-roots: value-only
constexpr u32 kLinkInstallC = 0x8041A4F4u;     // liveness-roots: value-only

void print_phase_b_stats() {
    std::fprintf(stderr, "[mtn_phase_b] runs %llu handed back %llu clips started %llu nodes bound %llu searches %llu\n", g_phase_b_hits[0].load(),
                 g_phase_b_hits[1].load(), g_phase_b_hits[2].load(), g_phase_b_hits[3].load(), g_phase_b_hits[4].load());
}

// a guest call that wants the callee's r3
GEKKO_ALWAYS_INLINE bool call_r(Cpu& c, u32 target, u32 link, std::initializer_list<u32> args, u32& result) {
    c.gpr[1] += 0x10u;
    const bool ok = guest_call(c, target, link, args, result);
    c.gpr[1] -= 0x10u;
    return ok;
}

// 0x8000540C / 0x800054F4: byte fills and copies of Mtn-owned memory (non-overlapping)
void mem_fill0(Cpu& c, u32 dst, u32 n) {
    for (u32 i = 0; i < n; ++i) c.mem_write8(dst + i, 0u);
}
void mem_copy(Cpu& c, u32 dst, u32 src, u32 n) {
    for (u32 i = 0; i < n; ++i) c.mem_write8(dst + i, c.mem_read8(src + i));
}

// 0x8041E498: the node with key `key` in the tree below `node` (children at +0x24 / siblings at +0x28, key = first word of [node + 0xC]).
u32 find_node(Cpu& c, u32 node, u32 key, unsigned depth = 0) {
    g_phase_b_hits[4].fetch_add(1, std::memory_order_relaxed);
    if (depth > 200u) return 0u;
    if (key == c.mem_read32(c.mem_read32(node + 0xCu))) return node;
    for (u32 side = 0x24u; side <= 0x28u; side += 4u) {
        const u32 first = c.mem_read32(node + side);
        if (first == 0u) continue;
        if (key == c.mem_read32(c.mem_read32(first + 0xCu))) return first;
        if (c.mem_read32(first + 0x24u) != 0u) {
            const u32 r = find_node(c, c.mem_read32(first + 0x24u), key, depth + 1u);
            if (r != 0u) return r;
        }
        if (c.mem_read32(first + 0x28u) != 0u) {
            const u32 r = find_node(c, c.mem_read32(first + 0x28u), key, depth + 1u);
            if (r != 0u) return r;
        }
        // the original continues with the sibling section only when the child section found nothing: both sections run in turn
    }
    return 0u;
}

// 0x8041C74C: reset the track state arrays of the node tree for channel slot `idx`.
void reset_tracks(Cpu& c, u32 node, u32 idx, unsigned depth = 0) {
    if (depth > 200u) return;
    const u32 flags = c.mem_read32(node);
    u32 list = c.mem_read32(node + 8u);
    if ((flags & 0x400u) != 0u) {
        const u32 entry = c.mem_read32(list + 0x10u + (idx << 3));
        const F one = L(kOne);
        SM(c, entry + 8u, one);
        SM(c, entry + 4u, one);
        SM(c, entry, one);
        mem_fill0(c, entry + 0x10u, 0x40u);
        mem_fill0(c, entry + 0x50u, 0x40u);
        mem_fill0(c, entry + 0x90u, 0x40u);
        list = c.mem_read32(list + 4u);
    }
    if ((c.mem_read32(node) & 7u) != 0u) {
        const u32 entry = c.mem_read32(list + 0x10u + (idx << 3));
        mem_copy(c, entry, c.mem_read32(node + 0xCu) + 0x10u, 0xCu);
        mem_fill0(c, entry + 0x20u, 0x40u);
        mem_fill0(c, entry + 0x60u, 0x40u);
        mem_fill0(c, entry + 0xA0u, 0x40u);
    }
    const u32 f2 = c.mem_read32(node);
    list = c.mem_read32(list + 4u);
    if ((f2 & 0x40u) == 0u && (f2 & 0x38u) != 0u) {
        const u32 entry = c.mem_read32(list + 0x10u + (idx << 3));
        mem_copy(c, entry, c.mem_read32(node + 0xCu) + 0x20u, 0x10u);
        mem_fill0(c, entry + 0x20u, 0x40u);
        mem_fill0(c, entry + 0x60u, 0x40u);
        mem_fill0(c, entry + 0xA0u, 0x40u);
    }
    const u32 f3 = c.mem_read32(node);
    const u32 next = c.mem_read32(list + 4u);
    if ((f3 & 0x380u) != 0u) {
        const u32 entry = c.mem_read32(next + 0x10u + (idx << 3));
        mem_copy(c, entry, c.mem_read32(node + 0xCu) + 0x30u, 0xCu);
        mem_fill0(c, entry + 0x10u, 0x40u);
        mem_fill0(c, entry + 0x50u, 0x40u);
        mem_fill0(c, entry + 0x90u, 0x40u);
    }
    if (c.mem_read32(node + 0x24u) != 0u) reset_tracks(c, c.mem_read32(node + 0x24u), idx, depth + 1u);
    if (c.mem_read32(node + 0x28u) != 0u) reset_tracks(c, c.mem_read32(node + 0x28u), idx, depth + 1u);
}

// the record cursor callback of 0x8041C30C: the next key record (the first call of a group passes the clip id, the following calls 0)
GEKKO_ALWAYS_INLINE bool next_record(Cpu& c, u32 arg, u32& record) { return call_r(c, kRecordCursor, kLinkCursor, {arg}, record); }

// one bind step: take a record from the cursor and initialise the track state `state` from it (0x8041352C then 0x80414E88)
bool bind_track(Cpu& c, u32 state, u32& cursor_arg) {
    u32 record;
    if (!next_record(c, cursor_arg, record)) return false;
    mtn_track::state_init(c, state, record);
    cursor_arg = 0u;
    return true;
}
// the same without a track (the record is consumed)
bool skip_record(Cpu& c, u32& cursor_arg) {
    u32 record;
    if (!next_record(c, cursor_arg, record)) return false;
    cursor_arg = 0u;
    return true;
}

// 0x8041C30C(obj, node, idx, cursor_arg, weight): bind the key records of the node tree to the channel slot `idx`.
bool bind_nodes(Cpu& c, u32 obj, u32 node, u32 idx, u32 cursor, F weight, unsigned depth = 0) {
    if (depth > 200u) return true;
    FrameScope fs(c, 0x30u);
    g_phase_b_hits[3].fetch_add(1, std::memory_order_relaxed);
    const u32 key = c.mem_read32(node);
    const u32 found = find_node(c, c.mem_read32(obj + 0xCu), key);
    u32 list = 0u;
    if (found != 0u) list = c.mem_read32(found + 8u);
    u32 hw = c.mem_read16(node + 6u);
    if ((hw & 0x400u) != 0u) {
        if (found != 0u && (c.mem_read32(found) & 0x400u) != 0u) {
            const u32 entry = c.mem_read32(list + 0x10u + (idx << 3));
            const u32 state = entry + 0x10u;
            mem_copy(c, entry, c.mem_read32(found + 0xCu) + 0x30u, 0xCu);
            if (!bind_track(c, state, cursor)) return false;
            u32 zero = 0u;
            if (!bind_track(c, state + 0x40u, zero)) return false;
            zero = 0u;
            if (!bind_track(c, state + 0x80u, zero)) return false;
            list = c.mem_read32(list + 4u);
            cursor = 0u;
        } else {
            if (!skip_record(c, cursor)) return false;
            u32 zero = 0u;
            if (!skip_record(c, zero)) return false;
            zero = 0u;
            if (!skip_record(c, zero)) return false;
            cursor = 0u;
        }
    }
    hw = c.mem_read16(node + 6u);
    if ((hw & 7u) != 0u) {
        if (found != 0u && (c.mem_read32(found) & 7u) != 0u) {
            const u32 entry = c.mem_read32(list + 0x10u + (idx << 3));
            SM(c, entry + 0x10u, weight);
            const u32 state = entry + 0x20u;
            mem_copy(c, entry, c.mem_read32(found + 0xCu) + 0x10u, 0xCu);
            if ((c.mem_read16(node + 6u) & 1u) != 0u && !bind_track(c, state, cursor)) return false;
            if ((c.mem_read16(node + 6u) & 2u) != 0u && !bind_track(c, state + 0x40u, cursor)) return false;
            if ((c.mem_read16(node + 6u) & 4u) != 0u && !bind_track(c, state + 0x80u, cursor)) return false;
        } else {
            if ((hw & 1u) != 0u && !skip_record(c, cursor)) return false;
            if ((c.mem_read16(node + 6u) & 2u) != 0u && !skip_record(c, cursor)) return false;
            if ((c.mem_read16(node + 6u) & 4u) != 0u && !skip_record(c, cursor)) return false;
        }
    }
    if (list != 0u) list = c.mem_read32(list + 4u);
    hw = c.mem_read16(node + 6u);
    if ((hw & 0x40u) == 0u && (hw & 0x38u) != 0u) {
        if (found != 0u && (c.mem_read32(found) & 0x38u) != 0u) {
            const u32 entry = c.mem_read32(list + 0x10u + (idx << 3));
            const u32 state = entry + 0x20u;
            mem_copy(c, entry, c.mem_read32(found + 0xCu) + 0x20u, 0x10u);
            if ((c.mem_read16(node + 6u) & 8u) != 0u && !bind_track(c, state, cursor)) return false;
            if ((c.mem_read16(node + 6u) & 0x10u) != 0u && !bind_track(c, state + 0x40u, cursor)) return false;
            if ((c.mem_read16(node + 6u) & 0x20u) != 0u && !bind_track(c, state + 0x80u, cursor)) return false;
        } else {
            if ((hw & 8u) != 0u && !skip_record(c, cursor)) return false;
            if ((c.mem_read16(node + 6u) & 0x10u) != 0u && !skip_record(c, cursor)) return false;
            if ((c.mem_read16(node + 6u) & 0x20u) != 0u && !skip_record(c, cursor)) return false;
        }
    }
    if (list != 0u) list = c.mem_read32(list + 4u);
    hw = c.mem_read16(node + 6u);
    if ((hw & 0x380u) != 0u) {
        if (found != 0u && (c.mem_read32(found) & 0x380u) != 0u) {
            const u32 entry = c.mem_read32(list + 0x10u + (idx << 3));
            const u32 state = entry + 0x10u;
            mem_copy(c, entry, c.mem_read32(found + 0xCu) + 0x30u, 0xCu);
            if ((c.mem_read16(node + 6u) & 0x80u) != 0u && !bind_track(c, state, cursor)) return false;
            if ((c.mem_read16(node + 6u) & 0x100u) != 0u && !bind_track(c, state + 0x40u, cursor)) return false;
            if ((c.mem_read16(node + 6u) & 0x200u) != 0u && !bind_track(c, state + 0x80u, cursor)) return false;
        } else {
            if ((hw & 0x80u) != 0u && !skip_record(c, cursor)) return false;
            if ((c.mem_read16(node + 6u) & 0x100u) != 0u && !skip_record(c, cursor)) return false;
            if ((c.mem_read16(node + 6u) & 0x200u) != 0u && !skip_record(c, cursor)) return false;
        }
    }
    const u32 a = c.mem_read32(node + 0x48u);
    if (a != 0u && !bind_nodes(c, obj, a, idx, cursor, weight, depth + 1u)) return false;
    const u32 b = c.mem_read32(node + 0x4Cu);
    if (b != 0u && !bind_nodes(c, obj, b, idx, cursor, weight, depth + 1u)) return false;
    return true;
}

// 0x8041C1C0(obj, idx, clip): bind clip `clip` to channel slot `idx`; 1 when the clip was found.
bool clip_bind(Cpu& c, u32 obj, u32 idx, u32 clip, u32& found_out) {
    FrameScope fs(c, 0x30u);
    const u32 off = idx * kChannelStride;
    const u32 ch = c.mem_read32(obj + 0x10u) + off;
    F weight = L(kNegOne);
    reset_tracks(c, c.mem_read32(obj + 0xCu), idx);
    u32 hit;
    if (!call_r(c, kClipLookup, kLinkLookup, {ch + 0xA0u, clip}, hit)) return false;
    if (hit == 0u) {
        const u32 chb = c.mem_read32(obj + 0x10u) + off;
        SM(c, chb + 0x98u, L(kZero));
        SM(c, c.mem_read32(obj + 0x10u) + off + 0x9Cu, L(kOne));
        found_out = 0u;
        return true;
    }
    {
        const u32 chb = c.mem_read32(obj + 0x10u) + off;
        const F a = LM(c, ch + 0xA0u), b = LM(c, ch + 0xA4u);
        SM(c, chb + 0x98u, a);
        SM(c, c.mem_read32(obj + 0x10u) + off + 0x9Cu, b);
    }
    for (u32 k = 0; k < 16u; k += 4u) {  // 0x804167AC: the sampling context from the channel's four floats
        const F w = LM(c, ch + 0xA0u + k);
        SM(c, mtn_track::kContext + k, w);
    }
    u32 handle;
    if (!call_r(c, kClipHandle, kLinkHandle, {clip}, handle)) return false;
    u32 info;
    if (!call_r(c, kClipInfo, kLinkInfo, {handle}, info)) return false;
    if (info == 0u) {
        found_out = 0u;
        return true;
    }
    const u32 root = c.mem_read32(obj + 8u);
    if (handle != c.mem_read32(root)) {
        F f1 = fabs_single(c, LM(c, info + 8u));
        if (gt_ordered(c, f1, L(kEps4))) {
            const u32 root2 = c.mem_read32(obj + 8u);
            f1 = fabs_single(c, LM(c, root2 + 8u));
            if (gt_ordered(c, f1, L(kEps4))) {
                const u32 root3 = c.mem_read32(obj + 8u);
                const F f0 = LM(c, info + 8u);
                f1 = LM(c, root3 + 8u);
                weight = ppc::fps_div(c, f1, f0);
            }
        }
    }
    if (!bind_nodes(c, obj, info + 0x2Cu, idx, clip, weight)) return false;
    found_out = 1u;
    return true;
}

// 0x8041BDE4(a = the channel's four floats, b = its limit floats, lo, hi): clamp the limits and report which ends are reached.
u32 range_flags(Cpu& c, u32 a, u32 b, F f1, F f2) {
    F f0, f3, f4;
    u32 r0 = 0u;
    f3 = LM(c, a + 8u);
    f4 = LM(c, b + 8u);
    f0 = L(kEps4);
    f3 = ppc::fps_mul(c, f3, f4);
    if (lt_ordered(c, f3, f0)) r0 |= 1u;
    SM(c, a + 8u, f4);
    f0 = L(kZero);
    f3 = LM(c, b);
    if (lt_ordered(c, f3, f0)) {
        f3 = LM(c, a);
    } else {
        if (gt_ordered(c, f1, f3)) SM(c, b, f1);
        f3 = LM(c, b);
    }
    f1 = LM(c, b + 4u);
    f0 = L(kZero);
    if (lt_ordered(c, f1, f0)) {
        f2 = LM(c, a + 4u);
    } else {
        if (lt_ordered(c, f2, f1)) SM(c, b + 4u, f2);
        f2 = LM(c, b + 4u);
    }
    if (lt_ordered(c, f2, f3)) f2 = f3;
    f1 = LM(c, b + 0xCu);
    f0 = L(kZero);
    if (lt_ordered(c, f1, f0)) {
        f1 = LM(c, a + 0xCu);
        if (lt_ordered(c, f1, f3)) {
            f0 = L(kEps4);
            f1 = f3;
            r0 |= 0x10u;
            if (lt_ordered(c, f4, f0)) r0 |= 1u;
        }
        if (gt_ordered(c, f1, f2)) {
            f0 = L(kNegEps4);
            f1 = f2;
            r0 |= 0x10u;
            if (gt_ordered(c, f4, f0)) r0 |= 1u;
        }
    } else {
        bool set_one = false;
        if (lt_ordered(c, f1, f3)) f1 = f3;
        if (gt_ordered(c, f1, f2)) f1 = f2;
        f0 = L(kEps4);
        if (lt_ordered(c, f4, f0)) {
            f0 = LM(c, a + 0xCu);
            if (gt_ordered(c, f1, f0)) set_one = true;
        }
        if (!set_one) {
            f0 = L(kNegEps4);
            if (gt_ordered(c, f4, f0)) {
                f0 = LM(c, a + 0xCu);
                if (lt_ordered(c, f1, f0)) set_one = true;
                else goto skip_one;
            } else {
                goto skip_one;
            }
        }
        r0 |= 1u;
    skip_one:
        r0 |= 0x10u;
    }
    SM(c, a, f3);
    SM(c, a + 4u, f2);
    SM(c, a + 0xCu, f1);
    return r0;
}

// 0x8041ADAC(m, idx): start the clip that channel `idx` asks for.
bool clip_start(Cpu& c, u32 m, u32 idx) {
    FrameScope fs(c, 0x20u);
    g_phase_b_hits[2].fetch_add(1, std::memory_order_relaxed);
    const u32 ch = c.mem_read32(m + 0x10u) + idx * kChannelStride;
    if (!call(c, kClipAcquire, kLinkAcquire, {c.mem_read16(ch + 0x110u)})) return false;
    if ((c.mem_read32(ch + 0x90u) & 4u) != 0u && !call(c, kClipRelease, kLinkReleaseA, {c.mem_read16(ch + 0x128u)})) return false;
    mem_fill0(c, ch + 0xD0u, 0xCu);
    if ((c.mem_read32(ch + 0x94u) & 2u) == 0u) copy3(c, ch + 0xB0u, m + 0xF0u);
    else flag_or(c, ch + 0x90u, 0x100000u);
    if ((c.mem_read32(ch + 0x94u) & 4u) == 0u) {
        copy3(c, ch + 0xC0u, m + 0x100u);
        copy12(c, ch + 0x60u, m + 0x90u);
    } else {
        flag_or(c, ch + 0x90u, 0x200000u);
    }
    flag_or(c, ch + 0x90u, 4u);
    flag_or(c, ch + 0x90u, 0x10000u);
    flag_or(c, ch + 0x90u, 0x20000u);
    flag_or(c, ch + 0x90u, 0x80000u);
    flag_or(c, ch + 0x90u, 0x1000000u);
    u32 found;
    if (!clip_bind(c, m, idx, c.mem_read16(ch + 0x110u), found)) return false;
    if (found == 0u) {
        if (!call(c, kClipRelease, kLinkReleaseB, {c.mem_read16(ch + 0x110u)})) return false;
        c.mem_write16(ch + 0x128u, 0u);
        return true;
    }
    c.mem_write16(ch + 0x128u, static_cast<u16>(c.mem_read16(ch + 0x110u)));
    u32 q;
    if ((c.mem_read32(ch + 0x94u) & 0x800u) != 0u) {
        if (!call_r(c, kClipQuery, kLinkQuery, {c.mem_read16(ch + 0x128u), 2u, 0u, 0u, 0u}, q)) return false;
        if (q != 0u) flag_clear(c, ch + 0x90u, 8u);
        else flag_or(c, ch + 0x90u, 8u);
    } else {
        if (!call_r(c, kClipQuery, kLinkQuery, {c.mem_read16(ch + 0x128u), 2u, 0u, 0u, 0u}, q)) return false;
        if (q != 0u) flag_or(c, ch + 0x90u, 8u);
        else flag_clear(c, ch + 0x90u, 8u);
    }
    if ((c.mem_read32(ch + 0x94u) & 0x1000u) == 0u) {
        if (!call_r(c, kClipQuery, kLinkQuery, {c.mem_read16(ch + 0x128u), 1u, 0u, 0u, 0u}, q)) return false;
        if (q != 0u) flag_or(c, ch + 0x90u, 2u);
        else flag_clear(c, ch + 0x90u, 2u);
    }
    return true;
}

bool phase_b_ok(Cpu& c, u32 m) {
    const s32 n = static_cast<s32>(c.mem_read32(m + 0x20u));
    const u32 table = c.mem_read32(m + 0x24u);
    for (s32 i = 0; i < n; ++i) {
        const u32 idx = c.mem_read32(table + 4u * static_cast<u32>(i));
        if (idx == kNone) break;
        const u32 ch = c.mem_read32(m + 0x10u) + idx * kChannelStride;
        if ((c.mem_read32(ch + 0x94u) & 1u) != 0u && (c.mem_read32(ch + 0x90u) & 4u) != 0u && c.mem_read32(ch + 0x120u) == 0u) return false;
        if ((c.mem_read32(ch + 0x94u) & 1u) != 0u && c.mem_read16(ch + 0x110u) != 0u && c.mem_read32(ch + 0x120u) == 0u) return false;
    }
    return true;
}

// 0x8041A33C(m): per channel with a start request run it, derive the state flags, then install / start the pending phases.
bool phase_b(Cpu& c, u32 m) {
    FrameScope fs(c, 0x30u);
    u32 any_start = 0u, any_param = 0u;  // r30, r29
    for (s32 i = 0; i < static_cast<s32>(c.mem_read32(m + 0x20u)); ++i) {
        const u32 idx = c.mem_read32(c.mem_read32(m + 0x24u) + 4u * static_cast<u32>(i));
        if (idx == kNone) break;
        const u32 ch = c.mem_read32(m + 0x10u) + idx * kChannelStride;
        if ((c.mem_read32(ch + 0x94u) & 1u) == 0u) continue;
        if (c.mem_read16(ch + 0x110u) != 0u) {
            if (!clip_start(c, m, idx)) return false;
            if ((c.mem_read32(c.mem_read32(ch + 0x120u)) & 1u) != 0u) any_param = 1u;
            any_start = 1u;
        }
        if ((c.mem_read32(ch + 0x90u) & 4u) == 0u) continue;
        const u32 f94 = c.mem_read32(ch + 0x94u);
        if ((f94 & 0x78u) != 0u) {
            const u32 a = ch + 0xA0u, b = ch + 0x100u;
            if ((f94 & 0x40u) == 0u) SM(c, b + 8u, LM(c, a + 8u));
            if ((c.mem_read32(ch + 0x94u) & 8u) == 0u) SM(c, b, L(kNegOne));
            if ((c.mem_read32(ch + 0x94u) & 0x10u) == 0u) SM(c, b + 4u, L(kNegOne));
            if ((c.mem_read32(ch + 0x94u) & 0x20u) == 0u) SM(c, b + 0xCu, L(kNegOne));
            const F f1 = LM(c, ch + 0x98u), f2 = LM(c, ch + 0x9Cu);
            const u32 r = range_flags(c, a, b, f1, f2);
            if ((r & 1u) != 0u) flag_or(c, ch + 0x90u, 0x40000u);
            if ((r & 0x10u) != 0u) flag_or(c, ch + 0x90u, 0x20000u);
        }
        if ((c.mem_read32(ch + 0x94u) & 0x2000u) != 0u) {
            if (!call(c, kBindCallback, kLinkBindCb, {c.mem_read32(m + 0xCu), idx})) return false;
            flag_or(c, ch + 0x90u, 0x10u);
        }
        if ((c.mem_read32(ch + 0x90u) & 0x10000u) != 0u) {
            if (!call(c, kPhaseWrap, kLinkWrapE, {ch + 0xD0u, ch, ch + 0xA0u, 0u})) return false;
            if ((c.mem_read32(c.mem_read32(ch + 0x120u)) & 1u) != 0u && !call(c, kPhaseStartA, kLinkStartA, {m, idx})) return false;
        }
    }
    if (any_param != 0u) return call(c, kPhaseInstallA, kLinkInstallB, {c.mem_read32(m + 0xCu)});
    if (any_start != 0u) return call(c, kPhaseInstall, kLinkInstallC, {c.mem_read32(m + 0xCu)});
    return true;
}

bool serve_phase_b_hook(Cpu& c) {
    if (!g_phase_b) return false;
    if (!phase_b_ok(c, c.gpr[3])) {
        g_phase_b_hits[1].fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    g_phase_b_hits[0].fetch_add(1, std::memory_order_relaxed);
    phase_b(c, c.gpr[3]);
    return true;
}

bool node_walk(Cpu& c, u32 motion, u32 node);

// 0x80419094: the bone pass: the root matrix chain, the per-channel local matrices, the blended bank, the channel weights, then NodeWalk.
bool bone_pass(Cpu& c, u32 m) {
    g_tree_hits[2].fetch_add(1, std::memory_order_relaxed);
    FrameScope fs(c, 0x160u);
    const u32 fr = fs.frame;
    translate_by(c, stack_top(c), m + 0xF0u);
    compose(c, stack_top(c), m + 0x90u);
    const u32 head = c.mem_read32(c.mem_read32(m + 0xCu) + 0xCu);
    if (c.mem_read16(head + 4u) == 1u && (c.mem_read16(head + 6u) & 7u) != 0u) {
        bool x_flag = false, y_flag = false;  // r27, r28
        for (s32 i = 0; i < static_cast<s32>(c.mem_read32(m + 0x20u)); ++i) {
            const u32 idx = c.mem_read32(c.mem_read32(m + 0x24u) + 4u * static_cast<u32>(i));
            if (idx == kNone) break;
            const u32 ch = c.mem_read32(m + 0x10u) + idx * kChannelStride;
            const u32 flags = c.mem_read32(ch + 0x90u);
            if ((flags & 4u) == 0u) continue;
            if ((flags & 0x100000u) != 0u) x_flag = true;
            if ((flags & 0x200000u) != 0u) y_flag = true;
            copy3(c, ch + 0xE0u, c.mem_read32(ch + 0x124u) + 0x10u);
            if ((c.mem_read32(ch + 0x90u) & 1u) == 0u && (c.mem_read32(ch + 0x94u) & 0x400u) == 0u) continue;
            stack_push(c);
            if ((c.mem_read32(ch + 0x90u) & 0x80000u) != 0u) {
                SM(c, fr + 8u, neg_word(c, ch + 0xE0u));
                if ((c.mem_read32(m + 0x14u) & 1u) != 0u) SM(c, fr + 0xCu, neg_word(c, ch + 0xE4u));
                else SM(c, fr + 0xCu, L(kZero));
                SM(c, fr + 0x10u, neg_word(c, ch + 0xE8u));
                copy12(c, stack_top(c), kIdentity);
                translate_by(c, stack_top(c), ch + 0xB0u);
                compose(c, stack_top(c), ch + 0x60u);
                translate_by(c, stack_top(c), fr + 8u);
                top_translation_to(c, ch + 0xB0u);
            } else {
                const F ax = LM(c, ch + 0xD0u), bx = LM(c, ch + 0xE0u);
                SM(c, fr + 8u, ppc::fps_sub(c, ax, bx));
                if ((c.mem_read32(m + 0x14u) & 1u) != 0u) {
                    const F ay = LM(c, ch + 0xD4u), by = LM(c, ch + 0xE4u);
                    SM(c, fr + 0xCu, ppc::fps_sub(c, ay, by));
                } else {
                    SM(c, fr + 0xCu, L(kZero));
                }
                const F az = LM(c, ch + 0xD8u), bz = LM(c, ch + 0xE8u);
                SM(c, fr + 0x10u, ppc::fps_sub(c, az, bz));
                copy12(c, stack_top(c), ch);
                translate_by(c, stack_top(c), fr + 8u);
                copy12(c, ch, stack_top(c));
            }
            stack_pop(c);
        }
        copy12(c, fr + 0x104u, stack_top(c));
        for (s32 i = 0; i < static_cast<s32>(c.mem_read32(m + 0x20u)); ++i) {
            const u32 idx = c.mem_read32(c.mem_read32(m + 0x24u) + 4u * static_cast<u32>(i));
            if (idx == kNone) break;
            const u32 ch = c.mem_read32(m + 0x10u) + idx * kChannelStride;
            if (i != 0 && (c.mem_read32(ch + 0x90u) & 4u) == 0u) continue;
            stack_push(c);
            if ((c.mem_read32(ch + 0x90u) & 0x80000u) != 0u) {
                copy12(c, stack_top(c), kIdentity);
                translate_by(c, stack_top(c), ch + 0xB0u);
                compose(c, stack_top(c), ch + 0x60u);
            } else {
                if (x_flag || y_flag) {
                    copy12(c, fr + 0xA4u, stack_top(c));
                    if (x_flag) {
                        const F tx = LM(c, m + 0x110u);
                        SM(c, fr + 0xB0u, tx);
                        const F ty = LM(c, m + 0x114u);
                        SM(c, fr + 0xC0u, ty);
                        const F tz = LM(c, m + 0x118u);
                        SM(c, fr + 0xD0u, tz);
                    }
                    if (y_flag) {
                        stack_push(c);
                        copy12(c, stack_top(c), kIdentity);
                        compose(c, stack_top(c), m + 0xC0u);
                        copy12(c, fr + 0x74u, stack_top(c));
                        stack_pop(c);
                        copy_3x3(c, fr + 0xA4u, fr + 0x74u);
                        c.mem_write32(ch + 0x90u, c.mem_read32(ch + 0x90u) | 0x80000u);
                        c.mem_write32(ch + 0x90u, c.mem_read32(ch + 0x90u) | 0x1000000u);
                    }
                    copy12(c, stack_top(c), fr + 0xA4u);
                }
                compose(c, stack_top(c), ch);
            }
            copy12(c, ch, stack_top(c));
            copy12(c, fr + 0x104u, ch);
            copy12(c, kScratch + 0x30u * static_cast<u32>(i), fr + 0x104u);
            stack_pop(c);
            if (i != 0) {
                const F t = LM(c, c.mem_read32(ch + 0x120u) + 0x10u);
                matrix_lerp(c, fr + 0x104u, fr + 0x104u, fr + 0xD4u, t);
            } else if ((c.mem_read32(c.mem_read32(ch + 0x120u)) & 1u) != 0u) {
                stack_push(c);
                if (x_flag || y_flag) {
                    copy12(c, fr + 0x44u, stack_top(c));
                    if (x_flag) {
                        const F tx = LM(c, m + 0x110u);
                        SM(c, fr + 0x50u, tx);
                        const F ty = LM(c, m + 0x114u);
                        SM(c, fr + 0x60u, ty);
                        const F tz = LM(c, m + 0x118u);
                        SM(c, fr + 0x70u, tz);
                    }
                    if (y_flag) {
                        stack_push(c);
                        copy12(c, stack_top(c), kIdentity);
                        compose(c, stack_top(c), m + 0xC0u);
                        copy12(c, fr + 0x14u, stack_top(c));
                        stack_pop(c);
                        copy_3x3(c, fr + 0x44u, fr + 0x14u);
                        c.mem_write32(m + 0x14u, c.mem_read32(m + 0x14u) | 0x10000u);
                    }
                    copy12(c, stack_top(c), fr + 0x44u);
                }
                compose(c, stack_top(c), m + 0x30u);
                copy12(c, m + 0x30u, stack_top(c));
                copy12(c, kBankTemp, m + 0x30u);
                stack_pop(c);
                const F t = LM(c, c.mem_read32(ch + 0x120u) + 0x10u);
                matrix_lerp(c, fr + 0x104u, fr + 0x104u, m + 0x30u, t);
            }
            copy12(c, fr + 0xD4u, fr + 0x104u);
        }
        copy12(c, stack_top(c), fr + 0x104u);
    }
    for (s32 i = 0; i < static_cast<s32>(c.mem_read32(m + 0x20u)); ++i) {
        const u32 idx = c.mem_read32(c.mem_read32(m + 0x24u) + 4u * static_cast<u32>(i));
        if (idx == kNone) break;
        const u32 ch = c.mem_read32(m + 0x10u) + idx * kChannelStride;
        const F v = LM(c, c.mem_read32(ch + 0x120u) + 0x10u);
        SM(c, kChannelWeights + (idx << 2), v);
    }
    return node_walk(c, m, c.mem_read32(m + 0xCu));
}

// 0x80416EF4: the whole per-frame motion update.
bool motion_update(Cpu& c, u32 m) {
    if (m == 0u) return true;
    FrameScope fs(c, 0x20u);
    stack_push(c);
    copy12(c, stack_top(c), kIdentity);
    c.mem_write32(m + 0x14u, c.mem_read32(m + 0x14u) & 0xFF00FFFFu);
    for (s32 i = 0; i < static_cast<s32>(c.mem_read32(m + 0x20u)); ++i) {
        const u32 idx = c.mem_read32(c.mem_read32(m + 0x24u) + 4u * static_cast<u32>(i));
        if (idx == kNone) break;
        const u32 ch = c.mem_read32(m + 0x10u) + idx * kChannelStride;
        c.mem_write32(ch + 0x90u, c.mem_read32(ch + 0x90u) & 0xFFFFu);
        if (!call(c, kPhaseChannel, kLinkChannel, {m, idx})) return false;
        if ((c.mem_read32(ch + 0x90u) & 4u) != 0u) {
            const F v = LM(c, ch + 0xACu);
            SM(c, ch + 0x12Cu, v);
        }
    }
    if (!call(c, kPhaseA, kLinkPhaseA, {m})) return false;
    if (g_phase_b && phase_b_ok(c, m)) {
        g_phase_b_hits[0].fetch_add(1, std::memory_order_relaxed);
        if (!phase_b(c, m)) return false;
    } else {
        if (g_phase_b) g_phase_b_hits[1].fetch_add(1, std::memory_order_relaxed);
        if (!call(c, kPhaseB, kLinkPhaseB, {m})) return false;
    }
    if (g_phase_c && phase_c_ok(c, m)) {
        g_phase_c_hits[0].fetch_add(1, std::memory_order_relaxed);
        if (!phase_c(c, m)) return false;
    } else {
        if (g_phase_c) g_phase_c_hits[1].fetch_add(1, std::memory_order_relaxed);
        if (!call(c, kPhaseC, kLinkPhaseC, {m})) return false;
    }
    if (!sampler_pick(c, m)) return false;
    if (!bone_pass(c, m)) return false;
    if (g_phase_d && phase_d_ok(c, m)) {
        g_phase_d_hits[0].fetch_add(1, std::memory_order_relaxed);
        if (!phase_d(c, m)) return false;
    } else {
        if (g_phase_d) g_phase_d_hits[1].fetch_add(1, std::memory_order_relaxed);
        if (!call(c, kPhaseD, kLinkPhaseD, {m})) return false;
    }
    stack_pop(c);
    return true;
}

bool serve_motion_update_hook(Cpu& c) {
    if (!g_tree) {
        g_tree_hits[3].fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    g_tree_hits[0].fetch_add(1, std::memory_order_relaxed);
    motion_update(c, c.gpr[3]);
    return true;
}

// ---- 0x804196F0: the recursive bone-tree walk ------------------------------------------------------------------------------------------
bool run_callback(Cpu& c, u32 fn, u32 arg, u32 link) {
    switch (fn) {
    case kRotationPass: rotation_pass(c, arg); return !c.exiting();
    case kScalePass: vector_pass(c, arg, VecKind::Scale); return !c.exiting();
    case kScaleLengthsPass: vector_pass(c, arg, VecKind::ScaleLengths); return !c.exiting();
    case kTranslationPass: vector_pass(c, arg, VecKind::Translation); return !c.exiting();
    case kEulerBone: euler_bone(c, arg); return !c.exiting();
    default:
        if (fold_callback(c, fn, arg)) return !c.exiting();
        return call(c, fn, link, {arg});
    }
}

bool node_walk(Cpu& c, u32 motion, u32 node) {
    FrameScope fs(c, 0x40u);
    const u32 sp = fs.frame;
    bool saved_any = false;
    u32 entry = c.mem_read32(node + 8u);
    if (c.mem_read32(node + 0x28u) != 0u) stack_push(c);
    for (s32 i = 0; i < static_cast<s32>(c.mem_read32(motion + 0x20u)); ++i) {
        const u32 idx = c.mem_read32(c.mem_read32(motion + 0x24u) + 4u * static_cast<u32>(i));
        if (idx == kNone) break;
        if (idx > 3u) note_unsupported(0);
        const u32 record = c.mem_read32(node + 0x18u) + (idx << 5);
        if (c.mem_read32(record) == 0u) {
            note_unsupported(3);
            continue;
        }
        const F f1 = LM(c, record + 8u), f0 = LM(c, record + 0xCu);
        const F f2 = LM(c, c.mem_read32(node + 0x14u) + idx * 0x2Cu + 0x10u);
        const F f3 = LM(c, kChannelWeights + (idx << 2));
        saved_any = true;
        const F next = ppc::fps_madd(c, f2, f1, f0);
        SM(c, sp + 0x14u + (idx << 2), f3);
        SM(c, kChannelWeights + (idx << 2), next);
    }
    while (entry != 0u) {
        const u32 fn = c.mem_read32(entry);
        if (fn != 0u && !run_callback(c, fn, entry + 8u, kLinkCallback)) return false;
        entry = c.mem_read32(entry + 4u);
    }
    const u32 post = c.mem_read32(node + 0x1Cu);
    if (post != 0u) {
        c.mem_write32(sp + 0x10u, c.mem_read32(c.mem_read32(node + 0xCu)));
        c.mem_write32(sp + 8u, stack_top(c));
        c.mem_write32(sp + 0xCu, c.mem_read32(node + 0x20u));
            if (!call(c, post, kLinkPost, {sp + 8u})) return false;
    }
    if (c.mem_read32(node + 0x10u) != 0u) copy12(c, c.mem_read32(node + 0x10u), stack_top(c));
    if ((c.mem_read32(motion + 0x14u) & 2u) != 0u) {
        if (c.mem_read32(node + 0x28u) != 0u) stack_pop(c);
        return true;
    }
    const u32 child = c.mem_read32(node + 0x24u);
    if (child != 0u && !node_walk(c, motion, child)) return false;
    if (saved_any) {
        for (s32 i = 0; i < static_cast<s32>(c.mem_read32(motion + 0x20u)); ++i) {
            const u32 idx = c.mem_read32(c.mem_read32(motion + 0x24u) + 4u * static_cast<u32>(i));
            if (idx == kNone) break;
            SM(c, kChannelWeights + (idx << 2), LM(c, sp + 0x14u + (idx << 2)));
        }
    }
    const u32 sibling = c.mem_read32(node + 0x28u);
    if (sibling != 0u) {
        stack_pop(c);
        if (!node_walk(c, motion, sibling)) return false;
    }
    return true;
}

}  // namespace

void set_fold_enabled(bool enabled) noexcept { g_fold = enabled; }
bool fold_enabled() noexcept { return g_fold; }
unsigned long long fold_hits(unsigned kind) noexcept { return kind < kFoldKinds ? g_fold_hits[kind].load() : 0ull; }
unsigned long long fold_handed_back(unsigned kind) noexcept { return kind < kFoldKinds ? g_fold_back[kind].load() : 0ull; }
void serve_fold_scale_callback(Cpu& cpu) { fold_scale_callback(cpu, cpu.gpr[3]); }
void serve_fold_translate_callback(Cpu& cpu) { fold_translate_callback(cpu, cpu.gpr[3]); }
void serve_fold_scale_bank(Cpu& cpu) { vector_pass(cpu, cpu.gpr[3], VecKind::ScaleBank); }
void serve_fold_translation_bank(Cpu& cpu) { fold_translation_bank(cpu, cpu.gpr[3]); }
void serve_fold_rotation_bank(Cpu& cpu) { fold_rotation_bank(cpu, cpu.gpr[3]); }

void set_tree_enabled(bool enabled) noexcept { g_tree = enabled; }
bool tree_enabled() noexcept { return g_tree; }
unsigned long long tree_hits(unsigned kind) noexcept { return kind < kTreeKinds ? g_tree_hits[kind].load() : 0ull; }
#ifdef GEKKO_MTN_TEST_HOOKS
void set_wrap_loop_cap(unsigned long long cap) noexcept {
    g_wrap_seen.store(0, std::memory_order_relaxed);
    g_wrap_cap.store(cap, std::memory_order_relaxed);
}
#endif
void set_phase_b_enabled(bool enabled) noexcept { g_phase_b = enabled; }
bool phase_b_enabled() noexcept { return g_phase_b; }
unsigned long long phase_b_hits(unsigned kind) noexcept { return kind < 5u ? g_phase_b_hits[kind].load() : 0ull; }
void serve_phase_b(Cpu& cpu) { phase_b(cpu, cpu.gpr[3]); }
bool serve_phase_b_hand_back(Cpu& cpu) { return !serve_phase_b_hook(cpu); }
void serve_bind_nodes(Cpu& cpu) { bind_nodes(cpu, cpu.gpr[3], cpu.gpr[4], cpu.gpr[5], cpu.gpr[6], cpu.fpr[1].bits[0]); }
void serve_reset_tracks(Cpu& cpu) { reset_tracks(cpu, cpu.gpr[3], cpu.gpr[4]); }
void serve_find_node(Cpu& cpu) { cpu.gpr[3] = find_node(cpu, cpu.gpr[3], cpu.gpr[4]); }
void serve_range_flags(Cpu& cpu) { cpu.gpr[3] = range_flags(cpu, cpu.gpr[3], cpu.gpr[4], cpu.fpr[1].bits[0], cpu.fpr[2].bits[0]); }
void serve_clip_bind(Cpu& cpu) {
    u32 found = 0;
    clip_bind(cpu, cpu.gpr[3], cpu.gpr[4], cpu.gpr[5], found);
    cpu.gpr[3] = found;
}
void set_phase_c_enabled(bool enabled) noexcept { g_phase_c = enabled; }
bool phase_c_enabled() noexcept { return g_phase_c; }
unsigned long long phase_c_hits(unsigned kind) noexcept { return kind < 3u ? g_phase_c_hits[kind].load() : 0ull; }
void serve_phase_c(Cpu& cpu) { phase_c(cpu, cpu.gpr[3]); }
bool serve_phase_c_hand_back(Cpu& cpu) { return !serve_phase_c_hook(cpu); }
void set_phase_d_enabled(bool enabled) noexcept { g_phase_d = enabled; }
bool phase_d_enabled() noexcept { return g_phase_d; }
unsigned long long phase_d_hits(unsigned kind) noexcept { return kind < 3u ? g_phase_d_hits[kind].load() : 0ull; }
void serve_phase_d(Cpu& cpu) { phase_d(cpu, cpu.gpr[3]); }
bool serve_phase_d_hand_back(Cpu& cpu) { return !serve_phase_d_hook(cpu); }
void serve_motion_update(Cpu& cpu) { motion_update(cpu, cpu.gpr[3]); }
void serve_sampler_pick(Cpu& cpu) { sampler_pick(cpu, cpu.gpr[3]); }
void serve_bone_pass(Cpu& cpu) { bone_pass(cpu, cpu.gpr[3]); }

void serve_node_walk(Cpu& cpu) { node_walk(cpu, cpu.gpr[3], cpu.gpr[4]); }
void serve_rotation_pass(Cpu& cpu) { rotation_pass(cpu, cpu.gpr[3]); }
void serve_scale_pass(Cpu& cpu) { vector_pass(cpu, cpu.gpr[3], VecKind::Scale); }
void serve_scale_lengths_pass(Cpu& cpu) { vector_pass(cpu, cpu.gpr[3], VecKind::ScaleLengths); }
void serve_translation_pass(Cpu& cpu) { vector_pass(cpu, cpu.gpr[3], VecKind::Translation); }
void serve_euler_bone(Cpu& cpu) { euler_bone(cpu, cpu.gpr[3]); }
void serve_matrix_blend(Cpu& cpu) {
    matrix_blend(cpu, cpu.gpr[3], cpu.gpr[4], cpu.gpr[5], cpu.fpr[1].bits[0]);
}
void serve_quat_slerp(Cpu& cpu) {
    const Quat r = quat_slerp(cpu, load_quat(cpu, cpu.gpr[4]), load_quat(cpu, cpu.gpr[5]), cpu.fpr[1].bits[0]);
    store_quat(cpu, cpu.gpr[3], r);
    cpu.gpr[3] = 1u;
}
void serve_matrix_to_quat(Cpu& cpu) { store_quat(cpu, cpu.gpr[3], matrix_to_quat(cpu, cpu.gpr[4])); }
void serve_quat_to_matrix(Cpu& cpu) { quat_to_matrix(cpu, cpu.gpr[3], load_quat(cpu, cpu.gpr[4])); }
void serve_matrix_normalize(Cpu& cpu) { matrix_normalize(cpu, cpu.gpr[3], cpu.gpr[4]); }

void install(Cpu& cpu) {
    bool stats = false, tree_stats = false, phase_d_stats = false, phase_c_stats = false, phase_b_stats = false;
    if (const char* v = std::getenv("GEKKO_MTN_PHASE_B"); v != nullptr && v[0] != 0) {
        g_phase_b = v[0] == '1';
        phase_b_stats = g_phase_b;
    }
    if (const char* v = std::getenv("GEKKO_MTN_PHASE_B_STATS"); v != nullptr && v[0] == '1') phase_b_stats = true;
    if (phase_b_stats) std::atexit(print_phase_b_stats);
    // Switch off (the shipped default): no hook at all, the guest body runs untouched. The hook is installed only when the switch is on at start-up.
    if (g_phase_b) cpu.install_hle(kPhaseB, [](Cpu& c) { return serve_phase_b_hook(c); }, "Mtn clip start phase (native, hand-back)");
    if (const char* v = std::getenv("GEKKO_MTN_PHASE_C"); v != nullptr && v[0] != 0) {
        g_phase_c = v[0] == '1';
        phase_c_stats = g_phase_c;
    }
    if (const char* v = std::getenv("GEKKO_MTN_PHASE_C_STATS"); v != nullptr && v[0] == '1') phase_c_stats = true;
    if (phase_c_stats) std::atexit(print_phase_c_stats);
    cpu.install_hle(kPhaseC, [](Cpu& c) { return serve_phase_c_hook(c); }, "Mtn time advance phase (native, hand-back)");
    if (const char* v = std::getenv("GEKKO_MTN_PHASE_D"); v != nullptr && v[0] != 0) {
        g_phase_d = v[0] == '1';
        phase_d_stats = g_phase_d;
    }
    if (const char* v = std::getenv("GEKKO_MTN_PHASE_D_STATS"); v != nullptr && v[0] == '1') phase_d_stats = true;
    if (phase_d_stats) std::atexit(print_phase_d_stats);
    cpu.install_hle(kPhaseD, [](Cpu& c) { return serve_phase_d_hook(c); }, "Mtn pose rebuild phase (native, hand-back)");
    if (const char* v = std::getenv("GEKKO_MTN_TREE"); v != nullptr && v[0] != 0) {
        g_tree = v[0] == '1';
        tree_stats = g_tree;
    }
    if (const char* v = std::getenv("GEKKO_MTN_TREE_STATS"); v != nullptr && v[0] == '1') tree_stats = true;
    if (tree_stats) std::atexit(print_tree_stats);
    cpu.install_hle(kMotionUpdate, [](Cpu& c) { return serve_motion_update_hook(c); }, "Mtn MotionUpdate (native tree, hand-back)");
    if (const char* v = std::getenv("GEKKO_MTN_FOLD"); v != nullptr && v[0] != 0) {
        g_fold = v[0] == '1';
        stats = g_fold;  // an explicit run-time enable also records the hit counts (stderr, at exit)
    }
    if (const char* v = std::getenv("GEKKO_MTN_FOLD_STATS"); v != nullptr && v[0] == '1') stats = true;
    if (stats) std::atexit(print_fold_stats);
    cpu.install_hle(kNodeWalk, [](Cpu& c) { serve_node_walk(c); return true; }, "Mtn NodeWalk (native)");
    cpu.install_hle(kRotationPass, [](Cpu& c) { serve_rotation_pass(c); return true; }, "Mtn RotationPass (native)");
    cpu.install_hle(kScalePass, [](Cpu& c) { serve_scale_pass(c); return true; }, "Mtn ScalePass (native)");
    cpu.install_hle(kScaleLengthsPass, [](Cpu& c) { serve_scale_lengths_pass(c); return true; }, "Mtn ScaleLengthsPass (native)");
    cpu.install_hle(kTranslationPass, [](Cpu& c) { serve_translation_pass(c); return true; }, "Mtn TranslationPass (native)");
    cpu.install_hle(kEulerBone, [](Cpu& c) { serve_euler_bone(c); return true; }, "Mtn EulerBone (native)");
    cpu.install_hle(kMatrixBlend, [](Cpu& c) { serve_matrix_blend(c); return true; }, "Mtn MatrixBlend (native)");
    cpu.install_hle(kQuatSlerp, [](Cpu& c) { serve_quat_slerp(c); return true; }, "Mtn QuatSlerp (native)");
    cpu.install_hle(kMatrixToQuat, [](Cpu& c) { serve_matrix_to_quat(c); return true; }, "Mtn MatrixToQuat (native)");
    cpu.install_hle(kQuatToMatrix, [](Cpu& c) { serve_quat_to_matrix(c); return true; }, "Mtn QuatToMatrix (native)");
    cpu.install_hle(kMatrixNormalize, [](Cpu& c) { serve_matrix_normalize(c); return true; }, "Mtn MatrixNormalize (native)");
}

}  // namespace gekko::mtn_pose
