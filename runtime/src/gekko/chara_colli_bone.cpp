#include "gekko/chara_colli_bone.h"

#include <array>

#include "gekko/cpu.h"
#include "gekko/native_se_math.h"
#include "gekko/ppc_ops.h"

namespace gekko::chara_colli_bone {

namespace {

// A float as the guest stores it (the stfs word), and as an FPR holds it (lfs).
using W = u32;
using F = u64;
struct V3 {
    W x, y, z;
};
using M34 = std::array<W, 12>;  // rows of x y z translation

GEKKO_ALWAYS_INLINE F L(W w) noexcept { return single_to_double_bits(w); }
GEKKO_ALWAYS_INLINE W S(F f) noexcept { return double_to_single_bits(f); }
GEKKO_ALWAYS_INLINE W rt(W w) noexcept { return S(L(w)); }  // a copy through an FPR (lfs / stfs)
constexpr u64 kSign = 0x8000000000000000ull;

// The r2-relative constants the guest code loads (all in .sdata2).
constexpr u32 kOne = 0x1638u;            // 1.0f
constexpr u32 kZero = 0x163Cu;           // 0.0f
constexpr u32 kEulerZero = 0xFFFF8208u;  // r2 - 0x7DF8: 0.0f, 0x8001E070's angle test
constexpr u32 kCapsuleK = 0xFFFF8300u;   // r2 - 0x7D00: the capsule axis's x and y
constexpr u32 kCylinderK = 0xFFFF8330u;  // r2 - 0x7CD0: the cylinder's
constexpr u32 kSphereZero = 0xFFFF8358u; // r2 - 0x7CA8: 0x80035500's rotation test
constexpr u32 kEpsilon = 0x167Cu;        // 1e-6f, Normalize's bound
constexpr u32 kZeroVector = 0x805E1860u; // Normalize's fallback (BSS)

constexpr u32 kSphere = 0x8051DE14u, kCapsule = 0x8051DC88u, kCylinder = 0x8051DD04u;

struct Ctx {
    Cpu& c;
    // The lowest FPR a guest callee on the path saves with psq_st (32: none): 0x8040CF48 f21, 0x8040C94C f23,
    // 0x8040CB14 and 0x800C3C3C f30, 0x80031568 f31.
    u32 low = 32u;
    void saves(u32 f) noexcept {
        if (f < low)
            low = f;
    }
    // Their psq_l / lfd restores: ps1 rounded to single.
    void round_saved() const {
        for (u32 f = low; f < 32u; ++f)
            c.fpr[f].bits[1] = L(double_to_single_bits_flush(c.fpr[f].bits[1]));
    }
    W k(u32 offset) const { return c.mem_read32(c.gpr[2] + offset); }
    W rd(u32 address) const { return c.mem_read32(address); }
    void wr(u32 address, W value) const { c.mem_write32(address, value); }
    V3 rd3(u32 a) const { return {rd(a), rd(a + 4u), rd(a + 8u)}; }
    void wr3(u32 a, const V3& v) const {
        wr(a, v.x);
        wr(a + 4u, v.y);
        wr(a + 8u, v.z);
    }
    M34 rd34(u32 a) const {
        M34 m;
        for (u32 i = 0; i < 12u; ++i)
            m[i] = rd(a + 4u * i);
        return m;
    }
    void wr34(u32 a, const M34& m) const {
        for (u32 i = 0; i < 12u; ++i)
            wr(a + 4u * i, m[i]);
    }
    F add(F a, F b) const { return ppc::fps_add(c, a, b); }
    F sub(F a, F b) const { return ppc::fps_sub(c, a, b); }
    F mul(F a, F b) const { return ppc::fps_mul(c, a, b); }
    F div(F a, F b) const { return ppc::fps_div(c, a, b); }
    F madd(F a, F cc, F b) const { return ppc::fps_madd(c, a, cc, b); }
    F msub(F a, F cc, F b) const { return ppc::fps_msub(c, a, cc, b); }
    F nmsub(F a, F cc, F b) const { return ppc::fps_nmsub(c, a, cc, b); }
    // fcmpu / fcmpo into cr0 and FPSCR[FPCC]: the result's LT / EQ bits.
    bool lt(F a, F b, bool ordered) const {
        ppc::fp_compare(c, 0u, a, b, ordered);
        return ppc::cr_bit(c, 0u);
    }
    bool eq(F a, F b, bool ordered) const {
        ppc::fp_compare(c, 0u, a, b, ordered);
        return ppc::cr_bit(c, 2u);
    }
};

GEKKO_ALWAYS_INLINE M34 copy34(const M34& m) noexcept {  // 0x8040D198 / 0x8040CD04 (lfsx / stfsx)
    M34 r;
    for (u32 i = 0; i < 12u; ++i)
        r[i] = rt(m[i]);
    return r;
}
GEKKO_ALWAYS_INLINE V3 copy3(const V3& v) noexcept { return {rt(v.x), rt(v.y), rt(v.z)}; }  // 0x800091D0 / EC

// 0x800169F8 (the Range shapes' vf 0x0C): +0x2C through a frame copy, then 0x800091EC.
V3 local_position(const Ctx& x, u32 child) { return copy3(copy3(x.rd3(child + 0x2Cu))); }

// 0x8040C084: [I | v].
M34 translation(const Ctx& x, const V3& v) {
    const W one = rt(x.k(kOne)), zero = rt(x.k(kZero));
    return {one, zero, zero, rt(v.x), zero, one, zero, rt(v.y), zero, zero, one, rt(v.z)};
}

// 0x8040C94C / 0x8040CB14: a x b.
M34 mul34(const Ctx& x, const M34& a, const M34& b) {
    M34 r;
    for (u32 i = 0; i < 3u; ++i) {
        const u32 row = 4u * i;
        for (u32 j = 0; j < 3u; ++j)
            r[row + j] = S(x.madd(L(a[row + 2u]), L(b[8u + j]),
                                  x.madd(L(a[row]), L(b[j]), x.mul(L(a[row + 1u]), L(b[4u + j])))));
        r[row + 3u] = S(x.add(L(a[row + 3u]), x.madd(L(a[row + 2u]), L(b[11]),
                                                     x.madd(L(a[row]), L(b[3]), x.mul(L(a[row + 1u]), L(b[7]))))));
    }
    return r;
}

// 0x8040C6CC: m x [p 1].
V3 point(const Ctx& x, const M34& m, const V3& p) {
    const F px = L(p.x), py = L(p.y), pz = L(p.z);
    const auto row = [&](u32 r) {
        return S(x.add(L(m[r + 3u]), x.madd(L(m[r + 2u]), pz, x.madd(L(m[r]), px, x.mul(L(m[r + 1u]), py)))));
    };
    return {row(0u), row(4u), row(8u)};
}

// 0x8040C668: m's rotation x v.
V3 rotate(const Ctx& x, const M34& m, const V3& v) {
    const F vx = L(v.x), vy = L(v.y), vz = L(v.z);
    const auto row = [&](u32 r) {
        return S(x.madd(L(m[r + 2u]), vz, x.madd(L(m[r]), vx, x.mul(L(m[r + 1u]), vy))));
    };
    return {row(0u), row(4u), row(8u)};
}

// 0x80031568: sin and cos of a single angle, MSL's double routines (math/msl.h) rounded to single.
void sincos(Ctx& x, W angle, W& s, W& co) {
    const F a = L(angle);
    s = S(ppc::fps_round(x.c, se_math::detail::msl_sin(x.c, a)));
    co = S(ppc::fps_round(x.c, se_math::detail::msl_cos(x.c, a)));
}

// 0x8040C110 (z), 0x8040C188 (y), 0x8040C200 (x).
M34 rotation(Ctx& x, u32 axis, W angle) {
    W s, co;
    sincos(x, angle, s, co);
    const W one = rt(x.k(kOne)), zero = rt(x.k(kZero)), ns = S(L(s) ^ kSign), cs = rt(co), ss = rt(s);
    switch (axis) {
    case 2u: return {cs, ns, zero, zero, ss, cs, zero, zero, zero, zero, one, zero};
    case 1u: return {cs, zero, ss, zero, zero, one, zero, zero, ns, zero, cs, zero};
    default: return {one, zero, zero, zero, zero, cs, ns, zero, zero, ss, cs, zero};
    }
}

// 0x8001E070 (m, angles x y z at `angles`, order): m = m x R per non-zero angle, in the order's sequence.
void euler(Ctx& x, M34& m, u32 angles, u32 order) {
    static constexpr u32 kSequence[3][3] = {{2u, 1u, 0u}, {2u, 0u, 1u}, {1u, 0u, 2u}};
    const s32 o = static_cast<s32>(order);
    if (o < 0 || o > 2)
        return;
    for (const u32 axis : kSequence[o]) {
        const W angle = x.rd(angles + 4u * axis);
        if (x.eq(L(x.k(kEulerZero)), L(angle), false))
            continue;
        m = mul34(x, copy34(m), rotation(x, axis, angle));  // 0x8040CB14
        x.saves(30u);
    }
}

// 0x8040CF48: the cofactor inverse; a singular matrix is copied.
M34 inverse(Ctx& x, const M34& w) {
    F m[12];
    for (u32 i = 0; i < 12u; ++i)
        m[i] = L(w[i]);
    const F m6m8 = x.mul(m[6], m[8]), m4m10 = x.mul(m[4], m[10]);
    const F c0 = x.msub(m[5], m[10], x.mul(m[6], m[9]));  // m5 m10 - m6 m9
    const F c8 = x.msub(m[4], m[9], x.mul(m[5], m[8]));   // m4 m9 - m5 m8
    const F det = x.madd(m[2], c8, x.madd(m[0], c0, x.mul(m[1], x.sub(m6m8, m4m10))));
    if (x.eq(L(x.k(kZero)), det, false))
        return copy34(w);
    const F inv = x.div(L(x.k(kOne)), det);
    const F neg = kSign;
    M34 r;
    r[0] = S(x.mul(inv, c0));
    r[1] = S(x.mul(inv, x.nmsub(m[1], m[10], x.mul(m[2], m[9]))));
    r[2] = S(x.mul(inv, x.sub(x.mul(m[1], m[6]), x.mul(m[2], m[5]))));
    r[3] = S(x.mul(inv, neg ^ x.nmsub(m[10], x.mul(m[1], m[7]),
                                      x.nmsub(m[11], x.mul(m[2], m[5]),
                                              x.nmsub(m[9], x.mul(m[3], m[6]),
                                                      x.madd(m[10], x.mul(m[3], m[5]),
                                                             x.madd(m[11], x.mul(m[1], m[6]),
                                                                    x.mul(m[9], x.mul(m[2], m[7])))))))));
    r[4] = S(x.mul(inv, neg ^ x.sub(m4m10, m6m8)));
    r[5] = S(x.mul(inv, x.msub(m[0], m[10], x.mul(m[2], m[8]))));
    r[6] = S(x.mul(inv, neg ^ x.sub(x.mul(m[0], m[6]), x.mul(m[2], m[4]))));
    r[7] = S(x.mul(inv, x.nmsub(m[10], x.mul(m[0], m[7]),
                                x.nmsub(m[11], x.mul(m[2], m[4]),
                                        x.nmsub(m[8], x.mul(m[3], m[6]),
                                                x.madd(m[10], x.mul(m[3], m[4]),
                                                       x.madd(m[11], x.mul(m[0], m[6]),
                                                              x.mul(m[8], x.mul(m[2], m[7])))))))));
    r[8] = S(x.mul(inv, c8));
    r[9] = S(x.mul(inv, x.nmsub(m[0], m[9], x.mul(m[1], m[8]))));
    r[10] = S(x.mul(inv, x.sub(x.mul(m[0], m[5]), x.mul(m[1], m[4]))));
    r[11] = S(x.mul(inv, neg ^ x.nmsub(m[9], x.mul(m[0], m[7]),
                                       x.nmsub(m[11], x.mul(m[1], m[4]),
                                               x.nmsub(m[8], x.mul(m[3], m[5]),
                                                       x.madd(m[9], x.mul(m[3], m[4]),
                                                              x.madd(m[11], x.mul(m[0], m[5]),
                                                                     x.mul(m[8], x.mul(m[1], m[7])))))))));
    return r;
}

// 0x8040DFA0: |v| through the game's sqrt (0x8000FF04).
F length(Ctx& x, const V3& v) {
    const F vx = L(v.x), vy = L(v.y), vz = L(v.z);
    const F xx = x.mul(vx, vx), yy = x.mul(vy, vy), zz = x.mul(vz, vz);
    return se_math::detail::guest_sqrt(x.c, x.add(zz, x.add(xx, yy)));
}

V3 sub3(Ctx& x, const V3& a, const V3& b) {  // 0x8040DDC4
    return {S(x.sub(L(a.x), L(b.x))), S(x.sub(L(a.y), L(b.y))), S(x.sub(L(a.z), L(b.z)))};
}
V3 add3(Ctx& x, const V3& a, const V3& b) {  // 0x8040DE20
    return {S(x.add(L(a.x), L(b.x))), S(x.add(L(a.y), L(b.y))), S(x.add(L(a.z), L(b.z)))};
}

// 0x8040DEC4 in place.
void normalize(Ctx& x, V3& v) {
    const F len = length(x, v);
    if (x.lt(len, L(x.k(kEpsilon)), true)) {
        v = copy3(x.rd3(kZeroVector));
        return;
    }
    v = {S(x.div(L(v.x), len)), S(x.div(L(v.y), len)), S(x.div(L(v.z), len))};
}

// A capsule or cylinder whose +0x50 is final: centre, inverse, axis ends and direction (0x8002F5BC / 0x8003286C).
void finish_oriented(Ctx& x, u32 child, const M34& m, bool capsule) {
    x.wr(child + 4u, rt(m[3]));  // 0x8040D1CC (row, 3)
    x.wr(child + 8u, rt(m[7]));
    x.wr(child + 0x0Cu, rt(m[11]));
    x.wr34(child + 0x80u, copy34(inverse(x, m)));
    const W k = rt(x.k(capsule ? kCapsuleK : kCylinderK));
    const V3 axis = copy3(rotate(x, m, {k, k, rt(x.rd(child + 0x38u))}));
    const V3 centre = x.rd3(child + 4u);
    x.wr3(child + 0xB0u, copy3(sub3(x, centre, axis)));
    x.wr3(child + 0xBCu, copy3(add3(x, centre, axis)));
    V3 direction = copy3(axis);  // 0x8040DF38: a copy normalised, copied out
    normalize(x, direction);
    x.wr3(child + 0xC8u, copy3(copy3(direction)));
    x.saves(21u);  // 0x8040CF48
}

// vf 0x54 (0x8002F664 / 0x8003291C / 0x800354A0): re-posed on `bone`.
void repose(Ctx& x, u32 child, u32 cvt, u32 bone) {
    const M34 b = x.rd34(bone);
    const V3 local = local_position(x, child);
    if (cvt == kSphere) {
        x.wr3(child + 4u, copy3(point(x, b, local)));
        return;
    }
    M34 m = copy34(mul34(x, b, translation(x, local)));  // 0x8040C94C
    x.saves(23u);
    euler(x, m, child + 0x40u, x.rd(child + 0x4Cu));
    x.wr34(child + 0x50u, m);
    finish_oriented(x, child, m, cvt == kCapsule);
}

// vf 0x50 (0x8002F774 / 0x80032A2C / 0x80035500): re-derived from the object's position, rotation and order.
void rederive(Ctx& x, u32 child, u32 cvt, u32 position, u32 rotation_angles, u32 order) {
    if (cvt == kSphere) {
        bool rotated = false;
        if ((x.c.mem_read8(child + 0x18u) & 1u) != 0u) {
            const F zero = L(x.k(kSphereZero));
            rotated = !x.eq(zero, L(x.rd(rotation_angles)), false) ||
                      !x.eq(zero, L(x.rd(rotation_angles + 4u)), false) ||
                      !x.eq(zero, L(x.rd(rotation_angles + 8u)), false);
        }
        if (!rotated) {
            const V3 local = local_position(x, child);
            x.wr3(child + 4u, copy3(add3(x, x.rd3(position), local)));
            return;
        }
        M34 m = copy34(translation(x, x.rd3(position)));
        euler(x, m, rotation_angles, order);
        x.wr3(child + 4u, copy3(point(x, m, local_position(x, child))));
        return;
    }
    M34 m = copy34(translation(x, x.rd3(position)));
    euler(x, m, rotation_angles, order);
    m = mul34(x, copy34(m), translation(x, local_position(x, child)));  // 0x8040CB14
    x.saves(30u);
    euler(x, m, child + 0x40u, x.rd(child + 0x4Cu));
    x.wr34(child + 0x50u, m);
    finish_oriented(x, child, m, cvt == kCapsule);
}

// The child list (sentinel this+0x64, first this+0x68, next node+4, child node+8): the Range shapes this code
// covers (any angle: the native sine / cosine, math/msl.h, cover every argument).
bool gather(const Ctx& x, u32 self, u32 (&children)[32], u32 (&vtables)[32], u32& count) {
    const u32 sentinel = self + 0x64u;
    count = 0;
    for (u32 node = x.rd(sentinel + 4u); node != sentinel; node = x.rd(node + 4u)) {
        if (count == 32u)
            return false;
        const u32 child = x.rd(node + 8u);
        const u32 cvt = x.rd(child);
        if (cvt != kSphere && cvt != kCapsule && cvt != kCylinder)
            return false;
        children[count] = child;
        vtables[count++] = cvt;
    }
    return true;
}

}  // namespace

bool update(Cpu& c, u32 self, bool* wide) {
    Ctx x{c};
    const bool dirty = c.mem_read8(self + 0x28u) != 0u;
    // The contract, checked before anything is written.
    u32 children[32], vtables[32], count = 0;
    if (!gather(x, self, children, vtables, count))
        return false;
    if (count == 0u)
        return false;

    if (dirty) {  // vf 0x8C
        const u32 order = x.rd(self + 0x58u);
        for (u32 i = 0; i < count; ++i)
            rederive(x, children[i], vtables[i], self + 0x40u, self + 0x4Cu, order);
        c.mem_write8(self + 0x29u, 1u);
        c.mem_write8(self + 0x28u, 0u);
    }
    // vf 0x90
    const u32 bones = x.rd(self + 0x6Cu);
    for (u32 i = 0; i < count; ++i)
        if (const u32 bone = x.rd(bones + 4u * i); bone != 0u)
            repose(x, children[i], vtables[i], bone);
    c.mem_write8(self + 0x29u, 1u);
    // vf 0x84: the first child counted unconditionally, then every enabled child (the first again).
    const V3 centre = x.rd3(self + 0x40u);
    const auto reach = [&](u32 child) {
        const F r = L(x.rd(child + 0x10u));
        return x.add(length(x, sub3(x, x.rd3(child + 4u), centre)), r);
    };
    F radius = reach(children[0]);
    for (u32 i = 0; i < count; ++i) {
        if (c.mem_read8(children[i] + 0x19u) == 0u)
            continue;
        const F candidate = reach(children[i]);
        if (x.lt(radius, candidate, true))
            radius = candidate;
    }
    x.wr(self + 8u, S(radius));
    x.saves(30u);  // 0x800C3C3C
    x.round_saved();
    if (wide != nullptr)
        *wide = x.low <= 21u;
    return true;
}

bool rederive_children(Cpu& c, u32 self) {
    Ctx x{c};
    u32 children[32], vtables[32], count = 0;
    if (!gather(x, self, children, vtables, count))
        return false;
    const u32 order = x.rd(self + 0x58u);
    for (u32 i = 0; i < count; ++i)
        rederive(x, children[i], vtables[i], self + 0x40u, self + 0x4Cu, order);
    c.mem_write8(self + 0x29u, 1u);
    c.mem_write8(self + 0x28u, 0u);
    x.round_saved();
    return true;
}

bool rederive_shape(Cpu& c, u32 self) {
    Ctx x{c};
    const u32 shape = x.rd(self + 0x5Cu);  // vf 0x28 (0x800C0BD0)
    const u32 cvt = x.rd(shape);
    if (cvt != kSphere && cvt != kCapsule && cvt != kCylinder)
        return false;
    rederive(x, shape, cvt, self + 0x40u, self + 0x4Cu, x.rd(self + 0x58u));
    c.mem_write8(self + 0x29u, 1u);
    c.mem_write8(self + 0x28u, 0u);
    x.round_saved();
    return true;
}

}  // namespace gekko::chara_colli_bone
