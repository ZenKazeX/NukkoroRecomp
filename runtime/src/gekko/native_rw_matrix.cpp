// RenderWare matrix routines, native. Contract: include/gekko/native_rw_matrix.h.

#include "gekko/native_rw_matrix.h"

#include <cstdlib>
#include <cstring>

#include "gekko/cpu.h"
#include "gekko/native_guest_call.h"
#include "gekko/native_rw_refusal.h"
#include "gekko/ppc_ops.h"

namespace gekko::rw_matrix {
namespace {

constexpr u32 kSdaMatrixModule = 0x6108u;  // r13-relative: the matrix module's offset in the engine instance
constexpr u32 kSdaEngine = 0x615Cu;
constexpr u32 kTypeMask = 3u, kOrthonormal = 3u, kInternalIdentity = 0x00020000u;
constexpr u64 kOne = 0x3FF0000000000000ull;  // 1.0 as the double a lfs of 1.0f leaves

// RwMatrix layout: right 0 (flags at +0xC), up 0x10, at 0x20, pos 0x30; each row x y z.
constexpr u32 kRight = 0x00u, kFlags = 0x0Cu, kUp = 0x10u, kAt = 0x20u, kPos = 0x30u;

// lfs / stfs: the CPU model conversions (stfs, unlike psq_st, does not flush a subnormal); fmuls / fmadds / fmsubs / fnmsubs / fnmadds: its single-precision helpers.
struct Ops {
    Cpu& c;
    u64 ld(u32 address) const { return single_to_double_bits(c.mem_read32(address)); }
    void st(u32 address, u64 value) const { c.mem_write32(address, double_to_single_bits(value)); }  // stfs: no flush
    u64 mul(u64 a, u64 b) const { return ppc::fps_mul(c, a, b); }
    u64 madd(u64 a, u64 m, u64 b) const { return ppc::fps_madd(c, a, m, b); }     // a * m + b
    u64 msub(u64 a, u64 m, u64 b) const { return ppc::fps_msub(c, a, m, b); }     // a * m - b
    u64 nmsub(u64 a, u64 m, u64 b) const { return ppc::fps_nmsub(c, a, m, b); }   // -(a * m - b)
    u64 nmadd(u64 a, u64 m, u64 b) const { return ppc::fps_nmadd(c, a, m, b); }   // -(a * m + b)
    u64 div(u64 a, u64 b) const { return ppc::fps_div(c, a, b); }
};

bool servable(const Cpu& c) {
    const u32 gqr0 = c.spr[SPR_GQR0];
    return (c.msr() & 0x2000u) != 0u && ((gqr0 >> 16) & 7u) < 4u && (gqr0 & 7u) < 4u;
}

}  // namespace


// ---- RwMatrixScale 0x804803E8 / RwMatrixTranslate 0x804805EC / RwMatrixTransform 0x8048077C ------------------------------
// r3 = matrix, r4 = the scale / translation vector (Transform: the other matrix), r5 = the combine operation as a signed int:
// 0 replace, 1 pre-concatenate, 2 post-concatenate. Any other operation raises a RenderWare error in the original and then
// dereferences a null matrix (Scale, Translate): the owner declines it, so the guest body runs and faults as before.
constexpr u32 kMatrixMultiply = 0x8047FC08u;
constexpr u32 kTransformEntry = 0x8048077Cu + 0u;  // the row address: the base of the link addresses
constexpr u32 kSda2One = 0x1B9Cu, kSda2Zero = 0x1B98u;  // r2-relative: the single constants 1.0 and 0.0 the replace case loads

// the copy of one matrix (sixteen words) in the original's interleaved schedule: word k + 1 is loaded before word k is stored
void copy_matrix_interleaved(Cpu& c, u32 dst, u32 src) {
    u32 current = c.mem_read32(src);
    u32 next = c.mem_read32(src + 4u);
    for (u32 k = 0; k < 14u; ++k) {
        c.mem_write32(dst + 4u * k, current);
        current = next;
        next = c.mem_read32(src + 4u * (k + 2u));
    }
    c.mem_write32(dst + 4u * 14u, current);
    c.mem_write32(dst + 4u * 15u, next);
}

bool serve_scale(Cpu& c) {
    if (!servable(c)) return rw_refusal::refuse(c, "RwMatrixScale", "the FPU is off or GQR0 is not the float format");
    const Ops o{c};
    const u32 matrix = c.gpr[3], scale = c.gpr[4];
    const s32 op = static_cast<s32>(c.gpr[5]);
    if (op < 0 || op > 2) return rw_refusal::refuse(c, "RwMatrixScale", "the combine operation is outside 0..2");
    if (op == 0) {
        const u64 one = o.ld(c.gpr[2] + kSda2One), zero = o.ld(c.gpr[2] + kSda2Zero);
        o.st(matrix + kAt + 8u, one);
        const u64 sx = o.ld(scale);
        o.st(matrix + kUp + 4u, one);
        const u64 sy = o.ld(scale + 4u);
        o.st(matrix + kRight, one);
        const u64 sz = o.ld(scale + 8u);
        for (const u32 offset : {kUp, kRight + 8u, kRight + 4u, kAt + 4u, kAt, kUp + 8u, kPos + 8u, kPos + 4u, kPos}) o.st(matrix + offset, zero);
        c.mem_write32(matrix + kFlags, c.mem_read32(matrix + kFlags) | kInternalIdentity | kOrthonormal);
        o.st(matrix + kRight, sx);
        o.st(matrix + kUp + 4u, sy);
        o.st(matrix + kAt + 8u, sz);
    } else {
        const u64 sx = o.ld(scale), sy = o.ld(scale + 4u), sz = o.ld(scale + 8u);
        // pre-concatenate (1) scales the rows by x, y, z; post-concatenate (2) scales the columns, and the translation too
        const u64 factors[3] = {sx, sy, sz};
        for (u32 row = 0; row < (op == 1 ? 3u : 4u); ++row)
            for (u32 col = 0; col < 3u; ++col) {
                const u32 address = matrix + 0x10u * row + 4u * col;
                o.st(address, o.mul(o.ld(address), factors[op == 1 ? row : col]));
            }
    }
    c.mem_write32(matrix + kFlags, c.mem_read32(matrix + kFlags) & ~(kInternalIdentity | kTypeMask));
    return true;
}

bool serve_translate(Cpu& c) {
    if (!servable(c)) return rw_refusal::refuse(c, "RwMatrixTranslate", "the FPU is off or GQR0 is not the float format");
    const Ops o{c};
    const u32 matrix = c.gpr[3], t = c.gpr[4];
    const s32 op = static_cast<s32>(c.gpr[5]);
    if (op < 0 || op > 2) return rw_refusal::refuse(c, "RwMatrixTranslate", "the combine operation is outside 0..2");
    if (op == 0) {
        const u64 one = o.ld(c.gpr[2] + kSda2One), zero = o.ld(c.gpr[2] + kSda2Zero);
        o.st(matrix + kAt + 8u, one);
        const u64 tx = o.ld(t);
        o.st(matrix + kUp + 4u, one);
        const u64 ty = o.ld(t + 4u);
        o.st(matrix + kRight, one);
        const u64 tz = o.ld(t + 8u);
        for (const u32 offset : {kUp, kRight + 8u, kRight + 4u, kAt + 4u, kAt, kUp + 8u, kPos + 8u, kPos + 4u, kPos}) o.st(matrix + offset, zero);
        c.mem_write32(matrix + kFlags, c.mem_read32(matrix + kFlags) | kInternalIdentity | kOrthonormal);
        o.st(matrix + kPos, tx);
        o.st(matrix + kPos + 4u, ty);
        o.st(matrix + kPos + 8u, tz);
    } else if (op == 1) {
        // pos.k += tx * right.k + ty * up.k + tz * at.k: ty * up (product), then tx * right fused, tz * at fused, then added to pos
        const u64 ty = o.ld(t + 4u), tx = o.ld(t), tz = o.ld(t + 8u);
        for (u32 k = 0; k < 3u; ++k) {
            u64 sum = o.mul(ty, o.ld(matrix + kUp + 4u * k));
            const u64 right = o.ld(matrix + kRight + 4u * k);
            const u64 at = o.ld(matrix + kAt + 4u * k);
            const u64 pos = o.ld(matrix + kPos + 4u * k);
            sum = o.madd(tx, right, sum);
            sum = o.madd(tz, at, sum);
            o.st(matrix + kPos + 4u * k, ppc::fps_add(c, pos, sum));
        }
    } else {
        const u64 tx = o.ld(t);
        const u64 pos_x = o.ld(matrix + kPos);
        const u64 ty = o.ld(t + 4u);
        o.st(matrix + kPos, ppc::fps_add(c, pos_x, tx));
        const u64 pos_y = o.ld(matrix + kPos + 4u);
        const u64 tz = o.ld(t + 8u);
        o.st(matrix + kPos + 4u, ppc::fps_add(c, pos_y, ty));
        const u64 pos_z = o.ld(matrix + kPos + 8u);
        o.st(matrix + kPos + 8u, ppc::fps_add(c, pos_z, tz));
    }
    c.mem_write32(matrix + kFlags, c.mem_read32(matrix + kFlags) & ~kInternalIdentity);
    return true;
}

bool serve_transform(Cpu& c) {
    if (!servable(c)) return rw_refusal::refuse(c, "RwMatrixTransform", "the FPU is off or GQR0 is not the float format");
    const u32 matrix = c.gpr[3], transform = c.gpr[4];
    const s32 op = static_cast<s32>(c.gpr[5]);
    if (op < 0 || op > 2) return rw_refusal::refuse(c, "RwMatrixTransform", "the combine operation is outside 0..2");
    if (op == 0) {
        copy_matrix_interleaved(c, matrix, transform);
        return true;
    }
    // the product goes to a private frame (the original's 0xA0 bytes) through RwMatrixMultiply, a RenderWare core function
    const u32 saved_sp = c.gpr[1];
    c.gpr[1] = saved_sp - 0xA0u;
    const u32 tmp = c.gpr[1] + (op == 1 ? 0x50u : 0x10u);
    u32 ignored = 0;
    const bool ok = op == 1 ? guest_call(c, kMatrixMultiply, kTransformEntry + 4u, {tmp, transform, matrix}, ignored)
                            : guest_call(c, kMatrixMultiply, kTransformEntry + 8u, {tmp, matrix, transform}, ignored);
    c.gpr[1] = saved_sp;
    if (!ok) return true;  // the run was stopped
    // the product's rows and flags; its three pad words hold stale stack bytes in the original, which are not copied
    static constexpr u32 kPadWords[3] = {0x1Cu, 0x2Cu, 0x3Cu};
    for (u32 w = 0; w < 16u; ++w) {
        const u32 offset = 4u * w;
        if (offset == kPadWords[0] || offset == kPadWords[1] || offset == kPadWords[2]) continue;
        c.mem_write32(matrix + offset, c.mem_read32(tmp + offset));
    }
    c.gpr[3] = matrix;
    return true;
}

// ---- RwMatrixRotateOneMinusCosineSine 0x8047FD24 ---------------------------------------------------------------------------
// r3 = matrix, r4 = unit axis, f1 = 1 - cos, f2 = sin, r5 = combine operation (as Scale). The rotation matrix is formed on the
// frame (right 0x50, up 0x60, at 0x70, pos 0x80; flags 3, pos 0) from single-precision chains in the original's order, then
// replaces the matrix (op 0, the flags word set to 3) or is multiplied in through RwMatrixMultiply (a RenderWare core function,
// run with guest_call) and copied back. Pad words (stale stack bytes in the original) are not written. An invalid operation raises
// a RenderWare error in the original and returns null: declined, so the guest body runs it.
// The original's f31 save / restore leaves f31's second slot rounded to a single, which is kept.
// The original saves f31 with an stfd and a psq_st and restores it with a psq_l and an lfd: the first slot comes back exact, the
// second is rounded to a single (a psq_l widening of the flushed psq_st conversion).
void settle_f31(Cpu& c) { c.fpr[31].bits[1] = single_to_double_bits(double_to_single_bits_flush(c.fpr[31].bits[1])); }

constexpr u32 kRotateEntry = 0x8047FD24u + 0u;  // the row address: the base of the link addresses

bool serve_rotate(Cpu& c) {
    if (!servable(c)) return rw_refusal::refuse(c, "RwMatrixRotateOneMinusCosineSine", "the FPU is off or GQR0 is not the float format");
    const s32 op = static_cast<s32>(c.gpr[5]);
    if (op < 0 || op > 2) return rw_refusal::refuse(c, "RwMatrixRotateOneMinusCosineSine", "the combine operation is outside 0..2");
    const Ops o{c};
    const u32 matrix = c.gpr[3], axis = c.gpr[4];
    const u64 k = c.fpr[1].bits[0], s = c.fpr[2].bits[0];  // f1, f2 as passed (doubles, rounded by each single operation)
    const u64 x = o.ld(axis), y = o.ld(axis + 4u), z = o.ld(axis + 8u);
    const u64 one = o.ld(c.gpr[2] + kSda2One), zero = o.ld(c.gpr[2] + kSda2Zero);
    const auto sub = [&](u64 a, u64 b) { return ppc::fps_sub(c, a, b); };
    const auto add = [&](u64 a, u64 b) { return ppc::fps_add(c, a, b); };
    const u64 x2 = o.mul(x, x), y2 = o.mul(y, y), z2 = o.mul(z, z);
    const u64 a = sub(one, x2), b = sub(one, y2), d = sub(one, z2);
    const u64 yz = o.mul(y, z), zx = o.mul(z, x), xy = o.mul(x, y);
    const u64 ak = o.mul(a, k), bk = o.mul(b, k), dk = o.mul(d, k);
    const u64 rx = sub(one, ak), uy = sub(one, bk), az = sub(one, dk);
    const u64 yzk = o.mul(yz, k);
    const u64 xs = o.mul(x, s);
    const u64 zxk = o.mul(zx, k);
    const u64 ys = o.mul(y, s);
    const u64 uz = add(yzk, xs), ay = sub(yzk, xs), rz = sub(zxk, ys), ax = add(zxk, ys);
    const u64 xyk = o.mul(xy, k);
    const u64 zs = o.mul(z, s);
    const u64 ry = add(xyk, zs), ux = sub(xyk, zs);
    // right (rx ry rz), up (ux uy uz), at (ax ay az), pos (0 0 0), as stfs'd singles
    const u64 words[4][3] = {{rx, ry, rz}, {ux, uy, uz}, {ax, ay, az}, {zero, zero, zero}};
    if (op == 0) {
        for (u32 row = 0; row < 4u; ++row)
            for (u32 col = 0; col < 3u; ++col) o.st(matrix + 0x10u * row + 4u * col, words[row][col]);
        c.mem_write32(matrix + kFlags, 3u);
    } else {
        const u32 saved_sp = c.gpr[1];
        c.gpr[1] = saved_sp - 0xB0u;
        const u32 rot = c.gpr[1] + 0x50u, tmp = c.gpr[1] + 0x10u;
        for (u32 row = 0; row < 4u; ++row)
            for (u32 col = 0; col < 3u; ++col) o.st(rot + 0x10u * row + 4u * col, words[row][col]);
        c.mem_write32(rot + kFlags, 3u);
        u32 ignored = 0;
        const bool ok = op == 1 ? guest_call(c, kMatrixMultiply, kRotateEntry + 4u, {tmp, rot, matrix}, ignored)
                                : guest_call(c, kMatrixMultiply, kRotateEntry + 8u, {tmp, matrix, rot}, ignored);
        c.gpr[1] = saved_sp;
        if (!ok) return true;  // the run was stopped
        for (u32 w = 0; w < 16u; ++w) {
            const u32 offset = 4u * w;
            if (offset == 0x1Cu || offset == 0x2Cu || offset == 0x3Cu) continue;
            c.mem_write32(matrix + offset, c.mem_read32(tmp + offset));
        }
    }
    settle_f31(c);
    c.gpr[3] = matrix;
    return true;
}

// ---- RwMatrixUpdate 0x8047FBF0 / RwMatrixOptimize 0x8047F9E0 -----------------------------------------------------------------
// Update (r3 = matrix): the flags word loses the identity and type bits (0xFFFDFFFC). Optimize (r3 = matrix, r4 = tolerances or
// null for the engine's default at [engine + module + 0xC]): the three tolerance tests (rows unit length, rows orthogonal, the
// matrix is the identity) in the original's single-precision order set the normal (1), orthogonal (2) and internal-identity
// (0x20000) flags. A comparison is the original's ordered "tolerance >= sum": false for a NaN. The original's f29..f31 save /
// restore (psq pairs) leaves their second slots rounded to a single, which is kept.
constexpr u32 kSdaModuleOffset = 0x6108u;

bool serve_update(Cpu& c) {
    const u32 matrix = c.gpr[3];
    c.mem_write32(matrix + kFlags, c.mem_read32(matrix + kFlags) & 0xFFFDFFFCu);
    return true;
}

bool serve_optimize(Cpu& c) {
    if (!servable(c)) return rw_refusal::refuse(c, "RwMatrixOptimize", "the FPU is off or GQR0 is not the float format");
    const Ops o{c};
    const u32 matrix = c.gpr[3];
    const u32 tolerances = c.gpr[4] != 0u ? c.gpr[4] : c.mem_read32(c.gpr[13] + kSdaEngine) + c.mem_read32(c.gpr[13] + kSdaModuleOffset) + 0xCu;
    const auto sub = [&](u64 a, u64 b) { return ppc::fps_sub(c, a, b); };
    const auto add = [&](u64 a, u64 b) { return ppc::fps_add(c, a, b); };
    const auto at = [&](u32 offset) { return o.ld(matrix + offset); };
    const u64 rx = at(0x00), ry = at(0x04), rz = at(0x08), ux = at(0x10), uy = at(0x14), uz = at(0x18), ax = at(0x20), ay = at(0x24),
              az = at(0x28), one = o.ld(c.gpr[2] + kSda2One);
    const auto not_below = [&](u64 tolerance, u64 value) {  // fcmpo tolerance, value; cror eq, gt, eq: tolerance >= value, ordered
        double t, v;
        std::memcpy(&t, &tolerance, 8);
        std::memcpy(&v, &value, 8);
        return t >= v;
    };
    const u64 tol0 = o.ld(tolerances), tol1 = o.ld(tolerances + 4u), tol2 = o.ld(tolerances + 8u);
    // rows unit length: |up|^2 - 1, |right|^2 - 1, |at|^2 - 1
    const u64 up_sq = add(o.mul(uz, uz), add(o.mul(ux, ux), o.mul(uy, uy)));
    const u64 right_sq = add(o.mul(rz, rz), add(o.mul(rx, rx), o.mul(ry, ry)));
    const u64 at_sq = add(o.mul(az, az), add(o.mul(ax, ax), o.mul(ay, ay)));
    const u64 e_up = sub(up_sq, one), e_right = sub(right_sq, one);
    u64 total = o.madd(e_right, e_right, o.mul(e_up, e_up));
    const u64 e_at = sub(at_sq, one);
    total = o.madd(e_at, e_at, total);
    const bool normal = not_below(tol0, total);
    // rows orthogonal: the three pairwise dot products squared
    const u64 d_ra = o.madd(az, rz, o.madd(ax, rx, o.mul(ay, ry)));          // at . right
    const u64 d_ua = o.madd(uz, az, o.madd(ux, ax, o.mul(uy, ay)));          // up . at
    const u64 d_ur = o.madd(rz, uz, o.madd(rx, ux, o.mul(ry, uy)));          // right . up
    u64 ortho = o.mul(d_ra, d_ra);
    ortho = o.madd(d_ua, d_ua, ortho);
    ortho = o.madd(d_ur, d_ur, ortho);
    const bool orthogonal = not_below(tol1, ortho);
    bool identity = false;
    if (normal && orthogonal) {
        const u64 px = at(0x30), py = at(0x34), pz = at(0x38);
        const u64 ey = sub(uy, one), ex = sub(rx, one), ez = sub(az, one);
        u64 diag_up = o.madd(ey, ey, o.mul(ux, ux));                         // (uy - 1)^2 + ux^2
        u64 diag_right = o.madd(ex, ex, o.mul(ry, ry));                      // (rx - 1)^2 + ry^2
        diag_right = add(o.mul(rz, rz), diag_right);
        const u64 diag_up_all = add(o.mul(uz, uz), diag_up);
        u64 at_part = add(o.mul(ax, ax), o.mul(ay, ay));
        at_part = o.madd(ez, ez, at_part);
        u64 sum = add(diag_right, diag_up_all);
        sum = add(at_part, sum);
        u64 pos = add(o.mul(px, px), o.mul(py, py));
        pos = add(o.mul(pz, pz), pos);
        sum = add(pos, sum);
        identity = not_below(tol2, sum);
    }
    u32 flags = c.mem_read32(matrix + kFlags) & ~1u;
    if (normal) flags |= 1u;
    flags &= ~2u;
    if (orthogonal) flags |= 2u;
    flags &= ~0x20000u;
    if (identity) flags |= 0x20000u;
    c.mem_write32(matrix + kFlags, flags);
    for (const u32 r : {29u, 30u, 31u}) c.fpr[r].bits[1] = single_to_double_bits(double_to_single_bits_flush(c.fpr[r].bits[1]));
    return true;
}

bool serve_invert(Cpu& c) {
    if (!servable(c)) return rw_refusal::refuse(c, "RwMatrixInvert", "the FPU is off or GQR0 is not the float format");
    const Ops o{c};
    const u32 dst = c.gpr[3], src = c.gpr[4];
    // the module globals: [engine + module offset + 4] holds the enabled optimisations
    const u32 module_offset = c.mem_read32(c.gpr[13] + kSdaMatrixModule);
    const u32 optimisations = c.mem_read32(c.mem_read32(c.gpr[13] + kSdaEngine) + module_offset + 4u);
    const u32 flags = c.mem_read32(src + kFlags);
    if ((flags & optimisations & kInternalIdentity) != 0u) {
        // an identity: all sixteen words are copied, flags and pads included, in the original's interleaved load / store
        // schedule (word 3, the flags, was loaded first and is stored after word 5 is loaded)
        const u32 flags_word = flags;
        u32 r0 = c.mem_read32(src + 0x00u);
        u32 r5 = c.mem_read32(src + 0x04u);
        c.mem_write32(dst + 0x00u, r0);
        r0 = c.mem_read32(src + 0x08u);
        c.mem_write32(dst + 0x04u, r5);
        u32 r6 = c.mem_read32(src + 0x10u);
        c.mem_write32(dst + 0x08u, r0);
        r5 = c.mem_read32(src + 0x14u);
        c.mem_write32(dst + 0x0Cu, flags_word);
        r0 = c.mem_read32(src + 0x18u);
        c.mem_write32(dst + 0x10u, r6);
        r6 = c.mem_read32(src + 0x1Cu);
        c.mem_write32(dst + 0x14u, r5);
        r5 = c.mem_read32(src + 0x20u);
        c.mem_write32(dst + 0x18u, r0);
        r0 = c.mem_read32(src + 0x24u);
        c.mem_write32(dst + 0x1Cu, r6);
        r6 = c.mem_read32(src + 0x28u);
        c.mem_write32(dst + 0x20u, r5);
        r5 = c.mem_read32(src + 0x2Cu);
        c.mem_write32(dst + 0x24u, r0);
        r0 = c.mem_read32(src + 0x30u);
        c.mem_write32(dst + 0x28u, r6);
        r6 = c.mem_read32(src + 0x34u);
        c.mem_write32(dst + 0x2Cu, r5);
        r5 = c.mem_read32(src + 0x38u);
        c.mem_write32(dst + 0x30u, r0);
        r0 = c.mem_read32(src + 0x3Cu);
        c.mem_write32(dst + 0x34u, r6);
        c.mem_write32(dst + 0x38u, r5);
        c.mem_write32(dst + 0x3Cu, r0);
        settle_f31(c);
        return true;
    }
    if ((flags & kTypeMask) == kOrthonormal) {
        // the transpose and the translation -(pos . axis), in the original's load / store order
        const u64 rx = o.ld(src + kRight);
        const u64 ux = o.ld(src + kUp);
        o.st(dst + kRight, rx);
        const u64 ax = o.ld(src + kAt);
        o.st(dst + kRight + 4u, ux);
        const u64 ry = o.ld(src + kRight + 4u);
        o.st(dst + kRight + 8u, ax);
        const u64 py = o.ld(src + kPos + 4u);
        const u64 uy = o.ld(src + kUp + 4u);
        o.st(dst + kUp, ry);
        u64 sum_x = o.mul(py, ry);
        const u64 ay = o.ld(src + kAt + 4u);
        u64 sum_y = o.mul(py, uy);
        o.st(dst + kUp + 4u, uy);
        const u64 px = o.ld(src + kPos);
        u64 sum_z = o.mul(py, ay);
        o.st(dst + kUp + 8u, ay);
        const u64 rz = o.ld(src + kRight + 8u);
        sum_x = o.madd(px, rx, sum_x);
        const u64 pz = o.ld(src + kPos + 8u);
        sum_y = o.madd(px, ux, sum_y);
        const u64 uz = o.ld(src + kUp + 8u);
        sum_z = o.madd(px, ax, sum_z);
        o.st(dst + kAt, rz);
        const u64 az = o.ld(src + kAt + 8u);
        sum_x = o.nmadd(pz, rz, sum_x);
        o.st(dst + kAt + 4u, uz);
        sum_y = o.nmadd(pz, uz, sum_y);
        sum_z = o.nmadd(pz, az, sum_z);
        o.st(dst + kAt + 8u, az);
        o.st(dst + kPos, sum_x);
        o.st(dst + kPos + 4u, sum_y);
        o.st(dst + kPos + 8u, sum_z);
        c.mem_write32(dst + kFlags, kOrthonormal);
        settle_f31(c);
        return true;
    }
    // the general inverse: cofactors of the right row stored in dst.right and re-read, the determinant, then the other rows
    {
        const u64 uz = o.ld(src + kUp + 8u);
        const u64 ay = o.ld(src + kAt + 4u);
        const u64 rz = o.ld(src + kRight + 8u);
        const u64 uy = o.ld(src + kUp + 4u);
        const u64 uzay = o.mul(uz, ay);
        const u64 az = o.ld(src + kAt + 8u);
        const u64 rzay = o.mul(rz, ay);
        const u64 ry = o.ld(src + kRight + 4u);
        const u64 rzuy = o.mul(rz, uy);
        const u64 c0 = o.msub(uy, az, uzay);
        const u64 c1 = o.nmsub(ry, az, rzay);
        const u64 ux = o.ld(src + kUp);
        const u64 c2 = o.msub(ry, uz, rzuy);
        const u64 rx = o.ld(src + kRight);
        o.st(dst + kRight, c0);
        const u64 ax = o.ld(src + kAt);
        o.st(dst + kRight + 4u, c1);
        o.st(dst + kRight + 8u, c2);
        const u64 d1 = o.ld(dst + kRight + 4u);
        const u64 d0 = o.ld(dst + kRight);
        const u64 t = o.mul(d1, ux);
        const u64 d2 = o.ld(dst + kRight + 8u);
        const u64 determinant = o.madd(d2, ax, o.madd(d0, rx, t));
        const u64 inverse = double_to_single_bits(determinant) != 0u ? o.div(kOne, determinant) : kOne;

        const u64 e0 = o.ld(dst + kRight);
        const u64 uz2 = o.ld(src + kUp + 8u);
        const u64 r0 = o.mul(e0, inverse);
        const u64 ax2 = o.ld(src + kAt);
        const u64 rz2 = o.ld(src + kRight + 8u);
        const u64 ux2 = o.ld(src + kUp);
        u64 k5 = o.mul(uz2, ax2);
        o.st(dst + kRight, r0);
        const u64 ry2 = o.ld(src + kRight + 4u);
        const u64 k4 = o.mul(rz2, ax2);
        const u64 e1 = o.ld(dst + kRight + 4u);
        const u64 k3 = o.mul(rz2, ux2);
        const u64 az2 = o.ld(src + kAt + 8u);
        u64 k2 = o.mul(ry2, ax2);
        const u64 r1 = o.mul(e1, inverse);
        const u64 rx2 = o.ld(src + kRight);
        k5 = o.nmsub(ux2, az2, k5);
        const u64 uy2 = o.ld(src + kUp + 4u);
        u64 k1 = o.mul(ry2, ux2);
        o.st(dst + kRight + 4u, r1);
        const u64 k10 = o.msub(rx2, az2, k4);
        const u64 ay2 = o.ld(src + kAt + 4u);
        const u64 e2 = o.ld(dst + kRight + 8u);
        u64 k6 = o.mul(uy2, ax2);
        const u64 k9 = o.nmsub(rx2, uz2, k3);
        const u64 py = o.ld(src + kPos + 4u);
        const u64 r2 = o.mul(e2, inverse);
        const u64 px = o.ld(src + kPos);
        const u64 u0 = o.mul(inverse, k5);
        const u64 pz = o.ld(src + kPos + 8u);
        k6 = o.msub(ux2, ay2, k6);
        o.st(dst + kRight + 8u, r2);
        const u64 u1 = o.mul(inverse, k10);
        o.st(dst + kUp, u0);
        const u64 u2 = o.mul(inverse, k9);
        k2 = o.nmsub(rx2, ay2, k2);
        o.st(dst + kUp + 4u, u1);
        const u64 a0 = o.mul(inverse, k6);
        k1 = o.msub(rx2, uy2, k1);
        o.st(dst + kUp + 8u, u2);
        const u64 a1 = o.mul(inverse, k2);
        o.st(dst + kAt, a0);
        const u64 a2 = o.mul(inverse, k1);
        o.st(dst + kAt + 4u, a1);
        o.st(dst + kAt + 8u, a2);
        // the translation, from the stored (re-read) rows
        const u64 pos_x = o.nmadd(pz, o.ld(dst + kAt), o.madd(px, o.ld(dst + kRight), o.mul(py, o.ld(dst + kUp))));
        o.st(dst + kPos, pos_x);
        const u64 pos_y = o.nmadd(pz, o.ld(dst + kAt + 4u), o.madd(px, o.ld(dst + kRight + 4u), o.mul(py, o.ld(dst + kUp + 4u))));
        o.st(dst + kPos + 4u, pos_y);
        const u64 pos_z = o.nmadd(pz, o.ld(dst + kAt + 8u), o.madd(px, o.ld(dst + kRight + 8u), o.mul(py, o.ld(dst + kUp + 8u))));
        o.st(dst + kPos + 8u, pos_z);
        c.mem_write32(dst + kFlags, 0u);
    }
    settle_f31(c);
    return true;
}

void install(Cpu& cpu) {
    cpu.install_hle(kInvert, [](Cpu& c) { return serve_invert(c); }, "RenderWare RwMatrixInvert");
    cpu.install_hle(kScale, [](Cpu& c) { return serve_scale(c); }, "RenderWare RwMatrixScale");
    cpu.install_hle(kTranslate, [](Cpu& c) { return serve_translate(c); }, "RenderWare RwMatrixTranslate");
    cpu.install_hle(kTransform, [](Cpu& c) { return serve_transform(c); }, "RenderWare RwMatrixTransform");
    cpu.install_hle(kUpdate, [](Cpu& c) { return serve_update(c); }, "RenderWare RwMatrixUpdate");
    cpu.install_hle(kOptimize, [](Cpu& c) { return serve_optimize(c); }, "RenderWare RwMatrixOptimize");
    cpu.install_hle(kRotate, [](Cpu& c) { return serve_rotate(c); }, "RenderWare RwMatrixRotateOneMinusCosineSine");
}

}  // namespace gekko::rw_matrix
