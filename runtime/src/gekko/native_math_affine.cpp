// The native owners of the game's 3x4 matrix / vector leaves 0x8040C57C .. 0x8040DF7C (gekko/native_math_affine.h).
//
// Every owner has two paths. The fast path is plain single-precision host arithmetic in the original's operation order
// (the fused steps as one-rounding fma), with one validity test per value: any operand, intermediate or result that is NaN
// or a nonzero magnitude below 2^-100 sends the whole call to the exact path, the original's instruction sequence on the
// guest's operands through ppc::fps_* (NI flush, NaN rules, default NaN). The fps_* helpers never write FPSCR in
// arithmetic, so an operation is a pure value function, and for operands and results that are zero or normal and at least
// 2^-100 it equals the correctly rounded single result, i.e. the host float operation (the same argument as the Mtn owners,
// docs/agents/opt-mtn.md; the contract test checks it by running the original words). Stores follow all loads in the fast
// path; the framed routines (concat, the inverses) are served there only when the output equals or is disjoint from each
// input, where the original's interleaved order gives the same words (checked on its load / store trace); anything else
// runs the exact path in the original's order.

#include "gekko/native_math_affine.h"

#include "gekko/cpu.h"
#include "gekko/gekko_estimate.h"
#include "gekko/ppc_ops.h"

#if defined(__AVX2__) || defined(__FMA__)
#include <immintrin.h>
#else
#include <cmath>
#endif

namespace gekko::math_affine {

namespace {
using F = u64;
// lfs / stfs: a float word as the FPR holds it, and back.
GEKKO_ALWAYS_INLINE F L(u32 word) noexcept { return single_to_double_bits(word); }
GEKKO_ALWAYS_INLINE u32 S(F value) noexcept { return double_to_single_bits(value); }
GEKKO_ALWAYS_INLINE float LF(Cpu& c, u32 address) { return bits_to_float(c.mem_read32(address)); }

// 0 < |x| < 2^-100, or NaN: the fast path declines (a superset of the original's slow-path trigger at 2^-125).
GEKKO_ALWAYS_INLINE bool TN(float x) noexcept {
    const u32 m = float_to_bits(x) & 0x7FFFFFFFu;
    return (m - 1u) < 0x0D7FFFFFu || m > 0x7F800000u;
}
#if defined(__AVX2__) || defined(__FMA__)
GEKKO_ALWAYS_INLINE float FMA(float a, float c, float b) noexcept {
    return _mm_cvtss_f32(_mm_fmadd_ss(_mm_set_ss(a), _mm_set_ss(c), _mm_set_ss(b)));
}
#else
GEKKO_ALWAYS_INLINE float FMA(float a, float c, float b) noexcept { return std::fma(a, c, b); }
#endif
// A value in an FPR that the fast path may take as a float: a single (what lfs / frsp / the single arithmetic leave there).
GEKKO_ALWAYS_INLINE bool single_value(F bits, float& out) noexcept {
    const double d = bits_to_double(bits);
    out = static_cast<float>(d);
    return static_cast<double>(out) == d;
}

// acc = m1 * p1; acc = m0 * p0 + acc; acc = m2 * p2 + acc: the original's order (fmuls, fmadds, fmadds).
GEKKO_ALWAYS_INLINE F dot(Cpu& c, F m0, F m1, F m2, F p0, F p1, F p2) {
    F acc = ppc::fps_mul(c, m1, p1);
    acc = ppc::fps_madd(c, m0, p0, acc);
    return ppc::fps_madd(c, m2, p2, acc);
}
GEKKO_ALWAYS_INLINE float dotf(float m0, float m1, float m2, float p0, float p1, float p2, bool& bad) noexcept {
    float acc = m1 * p1;
    bad |= TN(acc);
    acc = FMA(m0, p0, acc);
    bad |= TN(acc);
    acc = FMA(m2, p2, acc);
    bad |= TN(acc);
    return acc;
}

constexpr u64 kSign = 0x8000000000000000ull;
GEKKO_ALWAYS_INLINE u32 mem(Cpu& c, u32 address) noexcept { return c.mem_read32(address); }
GEKKO_ALWAYS_INLINE void store(Cpu& c, u32 address, F value) noexcept { c.mem_write32(address, S(value)); }
GEKKO_ALWAYS_INLINE bool plain_span(u32 a, u32 b) noexcept {  // two 48-byte matrices: disjoint
    const s32 d = static_cast<s32>(a - b);
    return d >= 48 || d <= -48;
}

// ---- the callee-saved FPR frame of the three framed routines -------------------------------------------------------------
// The original opens a frame (stwu), optionally stores LR into the caller's frame word SP+4, saves f<first>..f31 with
// stfd + psq_st (ps0 as a double at +0x10*k, then ps0 / ps1 as two singles at +8) and restores them with psq_l + lfd. The
// owner writes the same words (guest memory below SP, observable in a state hash) and applies the restore's one visible
// effect: ps1 rounded to a single (GQR0 type 0; the other types go through the generic helpers, as the original's code does).
struct Frame {
    Cpu& c;
    u32 base;
    u32 first;
    GEKKO_ALWAYS_INLINE Frame(Cpu& cpu, u32 size, u32 first_saved, bool saves_lr) : c(cpu), base(cpu.gpr[1] - size), first(first_saved) {
        const u32 sp = c.gpr[1];
        c.mem_write32(base, sp);
        if (saves_lr) c.mem_write32(sp + 4u, c.lr);
        const bool plain = (c.spr[SPR_GQR0] & 7u) < 4u;
        for (u32 d = first; d < 32u; ++d) {
            const u32 at = base + 0x10u * (d - first + 1u);
            c.mem_write64(at, c.fpr[d].bits[0]);
            if (plain) {
                c.mem_write32(at + 8u, double_to_single_bits_flush(c.fpr[d].bits[0]));
                c.mem_write32(at + 12u, double_to_single_bits_flush(c.fpr[d].bits[1]));
            } else {
                ppc::psq_store(c, d, at + 8u, false, 0u);
            }
        }
    }
    GEKKO_ALWAYS_INLINE ~Frame() {
        const bool plain = ((c.spr[SPR_GQR0] >> 16) & 7u) < 4u;
        for (u32 d = first; d < 32u; ++d) {
            if (plain) {
                c.fpr[d].bits[1] = single_to_double_bits(double_to_single_bits_flush(c.fpr[d].bits[1]));
            } else {
                const u64 ps0 = c.fpr[d].bits[0];
                ppc::psq_load(c, d, base + 0x10u * (d - first + 1u) + 8u, false, 0u);
                c.fpr[d].bits[0] = ps0;
            }
        }
    }
};

// ---- the exact paths (the original's instruction sequences on the guest's operands) -------------------------------------------
void point_exact(Cpu& c, u32 out, u32 m, u32 p) {
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

void rotate_vector_exact(Cpu& c, u32 out, u32 m, u32 p) {
    const F px = L(c.mem_read32(p)), py = L(c.mem_read32(p + 4u)), pz = L(c.mem_read32(p + 8u));
    u32 result[3];
    for (u32 row = 0; row < 3u; ++row) {
        const u32 r = m + 0x10u * row;
        result[row] = S(dot(c, L(c.mem_read32(r)), L(c.mem_read32(r + 4u)), L(c.mem_read32(r + 8u)), px, py, pz));
    }
    for (u32 row = 0; row < 3u; ++row)
        c.mem_write32(out + 4u * row, result[row]);
}

void translate_by_exact(Cpu& c, u32 m, F vx, F vy, F vz) {
    for (u32 row = 0; row < 3u; ++row) {
        const u32 r = m + 0x10u * row;
        const F acc = dot(c, L(c.mem_read32(r)), L(c.mem_read32(r + 4u)), L(c.mem_read32(r + 8u)), vx, vy, vz);
        c.mem_write32(r + 0xCu, S(ppc::fps_add(c, L(c.mem_read32(r + 0xCu)), acc)));
    }
}

void scale_columns_exact(Cpu& c, u32 m, F v0, F v1, F v2) {
    for (u32 row = 0; row < 3u; ++row) {
        const u32 r = m + 0x10u * row;
        c.mem_write32(r, S(ppc::fps_mul(c, L(c.mem_read32(r)), v0)));
        c.mem_write32(r + 4u, S(ppc::fps_mul(c, L(c.mem_read32(r + 4u)), v1)));
        c.mem_write32(r + 8u, S(ppc::fps_mul(c, L(c.mem_read32(r + 8u)), v2)));
    }
}

void concat_exact(Cpu& c) {
    const u32 a3 = c.gpr[3], a4 = c.gpr[4], a5 = c.gpr[5];
    F f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f23, f24, f25, f26, f27, f28, f29, f30, f31;
    f31 = L(mem(c, a5 + 16u));
    f0 = L(mem(c, a4 + 4u));
    f11 = L(mem(c, a5 + 20u));
    f4 = L(mem(c, a5 + 28u));
    f2 = ppc::fps_mul(c, f0, f31);
    f7 = L(mem(c, a5 + 24u));
    f1 = ppc::fps_mul(c, f0, f11);
    f30 = L(mem(c, a5 + 0u));
    f3 = ppc::fps_mul(c, f0, f4);
    f6 = L(mem(c, a4 + 0u));
    f12 = L(mem(c, a5 + 4u));
    f0 = ppc::fps_mul(c, f0, f7);
    f24 = L(mem(c, a4 + 20u));
    f2 = ppc::fps_madd(c, f6, f30, f2);
    f5 = L(mem(c, a5 + 12u));
    f1 = ppc::fps_madd(c, f6, f12, f1);
    f8 = L(mem(c, a5 + 8u));
    f29 = L(mem(c, a5 + 32u));
    f25 = ppc::fps_madd(c, f6, f5, f3);
    f28 = L(mem(c, a4 + 8u));
    f26 = ppc::fps_madd(c, f6, f8, f0);
    f13 = L(mem(c, a5 + 36u));
    f10 = ppc::fps_mul(c, f24, f31);
    f3 = ppc::fps_madd(c, f28, f29, f2);
    f6 = L(mem(c, a5 + 44u));
    f27 = ppc::fps_madd(c, f28, f13, f1);
    f1 = L(mem(c, a4 + 36u));
    f2 = ppc::fps_mul(c, f24, f11);
    f0 = L(mem(c, a4 + 16u));
    f9 = L(mem(c, a5 + 40u));
    f23 = ppc::fps_mul(c, f24, f4);
    store(c, a3 + 0u, f3);
    f3 = ppc::fps_mul(c, f24, f7);
    f25 = ppc::fps_madd(c, f28, f6, f25);
    f24 = L(mem(c, a4 + 12u));
    f28 = ppc::fps_madd(c, f28, f9, f26);
    store(c, a3 + 4u, f27);
    f26 = ppc::fps_madd(c, f0, f30, f10);
    f10 = L(mem(c, a4 + 24u));
    f27 = ppc::fps_madd(c, f0, f12, f2);
    store(c, a3 + 8u, f28);
    f25 = ppc::fps_add(c, f24, f25);
    f2 = L(mem(c, a4 + 32u));
    f24 = ppc::fps_madd(c, f0, f5, f23);
    f23 = L(mem(c, a4 + 28u));
    f28 = ppc::fps_madd(c, f0, f8, f3);
    store(c, a3 + 12u, f25);
    f26 = ppc::fps_madd(c, f10, f29, f26);
    f3 = L(mem(c, a4 + 40u));
    f25 = ppc::fps_mul(c, f1, f31);
    f0 = L(mem(c, a4 + 44u));
    f24 = ppc::fps_madd(c, f10, f6, f24);
    store(c, a3 + 16u, f26);
    f31 = ppc::fps_madd(c, f10, f13, f27);
    f26 = ppc::fps_mul(c, f1, f11);
    f27 = ppc::fps_mul(c, f1, f4);
    f11 = ppc::fps_madd(c, f2, f30, f25);
    store(c, a3 + 20u, f31);
    f4 = ppc::fps_madd(c, f10, f9, f28);
    f7 = ppc::fps_mul(c, f1, f7);
    f1 = ppc::fps_madd(c, f2, f12, f26);
    f5 = ppc::fps_madd(c, f2, f5, f27);
    store(c, a3 + 24u, f4);
    f10 = ppc::fps_add(c, f23, f24);
    f2 = ppc::fps_madd(c, f2, f8, f7);
    f4 = ppc::fps_madd(c, f3, f6, f5);
    f5 = ppc::fps_madd(c, f3, f29, f11);
    store(c, a3 + 28u, f10);
    f1 = ppc::fps_madd(c, f3, f13, f1);
    f2 = ppc::fps_madd(c, f3, f9, f2);
    store(c, a3 + 32u, f5);
    f0 = ppc::fps_add(c, f0, f4);
    store(c, a3 + 36u, f1);
    store(c, a3 + 40u, f2);
    store(c, a3 + 44u, f0);
}

void invert_in_place_exact(Cpu& c) {
    const u32 a2 = c.gpr[2], a3 = c.gpr[3];
    F f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f22, f23, f24, f25, f26, f27, f28, f29, f30, f31;
    f8 = L(mem(c, a3 + 16u));
    f4 = L(mem(c, a3 + 40u));
    f5 = L(mem(c, a3 + 32u));
    f0 = ppc::fps_mul(c, f8, f4);
    f3 = L(mem(c, a3 + 24u));
    f7 = L(mem(c, a3 + 36u));
    f6 = L(mem(c, a3 + 20u));
    f2 = ppc::fps_mul(c, f3, f7);
    f10 = L(mem(c, a3 + 4u));
    f1 = ppc::fps_msub(c, f3, f5, f0);
    f9 = L(mem(c, a3 + 0u));
    f12 = ppc::fps_mul(c, f6, f5);
    f11 = L(mem(c, a3 + 8u));
    f23 = ppc::fps_msub(c, f6, f4, f2);
    f0 = L(mem(c, a2 + 5692u));
    f1 = ppc::fps_mul(c, f10, f1);
    f2 = ppc::fps_msub(c, f8, f7, f12);
    f1 = ppc::fps_madd(c, f9, f23, f1);
    f24 = ppc::fps_madd(c, f11, f2, f1);
    ppc::fp_compare(c, 0u, f0, f24, false);
    if ((c.cr & 0x20000000u) != 0u) return;  // determinant equal to the constant: the matrix is left as it is
    f26 = L(mem(c, a3 + 28u));
    f0 = ppc::fps_mul(c, f11, f7);
    f12 = L(mem(c, a2 + 5688u));
    f1 = ppc::fps_mul(c, f3, f5);
    f13 = ppc::fps_mul(c, f11, f26);
    f27 = L(mem(c, a3 + 12u));
    f28 = ppc::fps_div(c, f12, f24);
    f25 = L(mem(c, a3 + 44u));
    f30 = ppc::fps_mul(c, f10, f26);
    f0 = ppc::fps_nmsub(c, f10, f4, f0);
    f24 = ppc::fps_mul(c, f28, f23);
    f22 = ppc::fps_mul(c, f10, f3);
    f23 = ppc::fps_mul(c, f7, f13);
    f12 = ppc::fps_mul(c, f11, f6);
    store(c, a3 + 0u, f24);
    f29 = ppc::fps_mul(c, f28, f0);
    f0 = ppc::fps_mul(c, f11, f5);
    f24 = ppc::fps_sub(c, f22, f12);
    f1 = ppc::fps_nmsub(c, f8, f4, f1);
    store(c, a3 + 4u, f29);
    f0 = ppc::fps_msub(c, f9, f4, f0);
    f31 = ppc::fps_mul(c, f28, f24);
    f24 = ppc::fps_mul(c, f10, f5);
    f29 = ppc::fps_mul(c, f27, f6);
    f23 = ppc::fps_madd(c, f25, f22, f23);
    store(c, a3 + 8u, f31);
    f24 = ppc::fps_nmsub(c, f9, f7, f24);
    f31 = ppc::fps_mul(c, f27, f3);
    f23 = ppc::fps_madd(c, f4, f29, f23);
    f22 = ppc::fps_mul(c, f9, f3);
    f13 = ppc::fps_mul(c, f5, f13);
    f23 = ppc::fps_nmsub(c, f7, f31, f23);
    f3 = ppc::fps_mul(c, f11, f8);
    f11 = ppc::fps_mul(c, f27, f8);
    f27 = ppc::fps_nmsub(c, f25, f12, f23);
    f12 = ppc::fps_madd(c, f25, f22, f13);
    f13 = ppc::fps_sub(c, f22, f3);
    f27 = ppc::fps_nmsub(c, f4, f30, f27);
    f12 = ppc::fps_madd(c, f4, f11, f12);
    f13 = f13 ^ kSign;
    f27 = f27 ^ kSign;
    f12 = ppc::fps_nmsub(c, f5, f31, f12);
    f22 = ppc::fps_mul(c, f9, f6);
    f6 = ppc::fps_mul(c, f5, f30);
    f27 = ppc::fps_mul(c, f28, f27);
    f23 = ppc::fps_mul(c, f10, f8);
    f6 = ppc::fps_madd(c, f25, f22, f6);
    f10 = ppc::fps_mul(c, f28, f1);
    store(c, a3 + 12u, f27);
    f8 = ppc::fps_sub(c, f22, f23);
    f1 = ppc::fps_madd(c, f7, f11, f6);
    f9 = ppc::fps_mul(c, f9, f26);
    store(c, a3 + 16u, f10);
    f3 = ppc::fps_nmsub(c, f25, f3, f12);
    f6 = ppc::fps_mul(c, f28, f0);
    f0 = ppc::fps_nmsub(c, f5, f29, f1);
    f1 = ppc::fps_nmsub(c, f4, f9, f3);
    f3 = ppc::fps_mul(c, f28, f13);
    store(c, a3 + 20u, f6);
    f0 = ppc::fps_nmsub(c, f25, f23, f0);
    f1 = ppc::fps_mul(c, f28, f1);
    store(c, a3 + 24u, f3);
    f3 = ppc::fps_mul(c, f28, f2);
    f0 = ppc::fps_nmsub(c, f7, f9, f0);
    store(c, a3 + 28u, f1);
    f2 = ppc::fps_mul(c, f28, f24);
    f1 = ppc::fps_mul(c, f28, f8);
    store(c, a3 + 32u, f3);
    f0 = f0 ^ kSign;
    store(c, a3 + 36u, f2);
    f0 = ppc::fps_mul(c, f28, f0);
    store(c, a3 + 40u, f1);
    store(c, a3 + 44u, f0);
}

void invert_exact(Cpu& c) {
    const u32 a2 = c.gpr[2], a3 = c.gpr[3], a4 = c.gpr[4];
    F f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f21, f22, f23, f24, f25, f26, f27, f28, f29, f30, f31;
    f2 = L(mem(c, a4 + 32u));
    f0 = L(mem(c, a4 + 24u));
    f5 = L(mem(c, a4 + 16u));
    f1 = L(mem(c, a4 + 40u));
    f13 = ppc::fps_mul(c, f0, f2);
    f4 = L(mem(c, a4 + 36u));
    f21 = ppc::fps_mul(c, f5, f1);
    f3 = L(mem(c, a4 + 20u));
    f8 = ppc::fps_mul(c, f0, f4);
    f6 = L(mem(c, a4 + 4u));
    f10 = ppc::fps_mul(c, f3, f2);
    f7 = L(mem(c, a4 + 0u));
    f9 = ppc::fps_sub(c, f13, f21);
    f12 = L(mem(c, a4 + 8u));
    f23 = ppc::fps_msub(c, f3, f1, f8);
    f8 = L(mem(c, a2 + 5692u));
    f11 = ppc::fps_msub(c, f5, f4, f10);
    f9 = ppc::fps_mul(c, f6, f9);
    f9 = ppc::fps_madd(c, f7, f23, f9);
    f22 = ppc::fps_madd(c, f12, f11, f9);
    ppc::fp_compare(c, 0u, f8, f22, false);
    if ((c.cr & 0x20000000u) != 0u) {
        // determinant equal to the constant: the original calls the 12-word copy 0x8040CD04 (lfsx / stfsx, one word at a time)
        for (u32 i = 0; i < 12u; ++i) c.mem_write32(a3 + 4u * i, S(L(c.mem_read32(a4 + 4u * i))));
        c.ctr = 0u;
        return;
    }
    f10 = L(mem(c, a4 + 28u));
    f25 = ppc::fps_mul(c, f12, f4);
    f9 = L(mem(c, a2 + 5688u));
    f24 = ppc::fps_sub(c, f21, f13);
    f13 = ppc::fps_mul(c, f12, f10);
    f8 = L(mem(c, a4 + 12u));
    f27 = ppc::fps_div(c, f9, f22);
    f9 = L(mem(c, a4 + 44u));
    f29 = ppc::fps_mul(c, f6, f10);
    f25 = ppc::fps_nmsub(c, f6, f1, f25);
    f26 = ppc::fps_mul(c, f27, f23);
    f21 = ppc::fps_mul(c, f6, f0);
    f23 = ppc::fps_mul(c, f4, f13);
    f28 = ppc::fps_mul(c, f12, f3);
    store(c, a3 + 0u, f26);
    f30 = ppc::fps_mul(c, f27, f25);
    f25 = ppc::fps_mul(c, f12, f2);
    f26 = ppc::fps_sub(c, f21, f28);
    f24 = f24 ^ kSign;
    store(c, a3 + 4u, f30);
    f25 = ppc::fps_msub(c, f7, f1, f25);
    f30 = ppc::fps_mul(c, f27, f26);
    f26 = ppc::fps_mul(c, f6, f2);
    f31 = ppc::fps_mul(c, f8, f3);
    f23 = ppc::fps_madd(c, f9, f21, f23);
    store(c, a3 + 8u, f30);
    f26 = ppc::fps_nmsub(c, f7, f4, f26);
    f30 = ppc::fps_mul(c, f8, f0);
    f23 = ppc::fps_madd(c, f1, f31, f23);
    f21 = ppc::fps_mul(c, f7, f0);
    f22 = ppc::fps_mul(c, f2, f13);
    f13 = ppc::fps_nmsub(c, f4, f30, f23);
    f12 = ppc::fps_mul(c, f12, f5);
    f0 = ppc::fps_mul(c, f8, f5);
    f13 = ppc::fps_nmsub(c, f9, f28, f13);
    f23 = ppc::fps_madd(c, f9, f21, f22);
    f8 = ppc::fps_sub(c, f21, f12);
    f13 = ppc::fps_nmsub(c, f1, f29, f13);
    f23 = ppc::fps_madd(c, f1, f0, f23);
    f8 = f8 ^ kSign;
    f13 = f13 ^ kSign;
    f23 = ppc::fps_nmsub(c, f2, f30, f23);
    f22 = ppc::fps_mul(c, f7, f3);
    f3 = ppc::fps_mul(c, f2, f29);
    f13 = ppc::fps_mul(c, f27, f13);
    f6 = ppc::fps_mul(c, f6, f5);
    f3 = ppc::fps_madd(c, f9, f22, f3);
    f7 = ppc::fps_mul(c, f7, f10);
    store(c, a3 + 12u, f13);
    f5 = ppc::fps_nmsub(c, f9, f12, f23);
    f0 = ppc::fps_madd(c, f4, f0, f3);
    f3 = ppc::fps_mul(c, f27, f24);
    f5 = ppc::fps_nmsub(c, f1, f7, f5);
    f1 = ppc::fps_nmsub(c, f2, f31, f0);
    f0 = ppc::fps_sub(c, f22, f6);
    store(c, a3 + 16u, f3);
    f2 = ppc::fps_mul(c, f27, f25);
    f3 = ppc::fps_nmsub(c, f9, f6, f1);
    f1 = ppc::fps_mul(c, f27, f8);
    store(c, a3 + 20u, f2);
    f5 = ppc::fps_mul(c, f27, f5);
    f3 = ppc::fps_nmsub(c, f4, f7, f3);
    store(c, a3 + 24u, f1);
    f2 = ppc::fps_mul(c, f27, f11);
    f1 = ppc::fps_mul(c, f27, f26);
    store(c, a3 + 28u, f5);
    f3 = f3 ^ kSign;
    f0 = ppc::fps_mul(c, f27, f0);
    store(c, a3 + 32u, f2);
    f2 = ppc::fps_mul(c, f27, f3);
    store(c, a3 + 36u, f1);
    store(c, a3 + 40u, f0);
    store(c, a3 + 44u, f2);
}


// fcmpu cr0, a, b -> EQ (cr0 and FPSCR[FPCC] set as the original does).
GEKKO_ALWAYS_INLINE bool compare_equal(Cpu& c, float a, float b) {
    ppc::fp_compare(c, 0u, single_to_double_bits(float_to_bits(a)), single_to_double_bits(float_to_bits(b)), false);
    return (c.cr & 0x20000000u) != 0u;
}
// The out-of-place inverse's singular branch: the 12-word forward copy 0x8040CD04 (CTR ends at 0).
bool zero_det(Cpu& c, u32 out, u32 in) {
    for (u32 i = 0; i < 12u; ++i) c.mem_write32(out + 4u * i, S(L(c.mem_read32(in + 4u * i))));
    c.ctr = 0u;
    return true;
}

}  // namespace

namespace detail {

// ---- the fast paths ----------------------------------------------------------------------------------------------------------
// true: served (words stored); false: nothing was stored or compared, run the exact path.
bool point_fast(Cpu& c, u32 out, u32 m, u32 p) {
    const float px = LF(c, p), py = LF(c, p + 4u), pz = LF(c, p + 8u);
    bool bad = TN(px) | TN(py) | TN(pz);
    float result[3];
    for (u32 row = 0; row < 3u; ++row) {
        const u32 r = m + 0x10u * row;
        const float m0 = LF(c, r), m1 = LF(c, r + 4u), m2 = LF(c, r + 8u), m3 = LF(c, r + 0xCu);
        bad |= TN(m0) | TN(m1) | TN(m2) | TN(m3);
        const float acc = dotf(m0, m1, m2, px, py, pz, bad);
        result[row] = m3 + acc;
        bad |= TN(result[row]);
    }
    if (bad) return false;
    for (u32 row = 0; row < 3u; ++row)
        c.mem_write32(out + 4u * row, float_to_bits(result[row]));
    return true;
}

bool rotate_vector_fast(Cpu& c, u32 out, u32 m, u32 p) {
    const float px = LF(c, p), py = LF(c, p + 4u), pz = LF(c, p + 8u);
    bool bad = TN(px) | TN(py) | TN(pz);
    float result[3];
    for (u32 row = 0; row < 3u; ++row) {
        const u32 r = m + 0x10u * row;
        const float m0 = LF(c, r), m1 = LF(c, r + 4u), m2 = LF(c, r + 8u);
        bad |= TN(m0) | TN(m1) | TN(m2);
        result[row] = dotf(m0, m1, m2, px, py, pz, bad);
    }
    if (bad) return false;
    for (u32 row = 0; row < 3u; ++row)
        c.mem_write32(out + 4u * row, float_to_bits(result[row]));
    return true;
}

bool translate_by_fast(Cpu& c, u32 m, float vx, float vy, float vz) {
    bool bad = TN(vx) | TN(vy) | TN(vz);
    float result[3];
    for (u32 row = 0; row < 3u; ++row) {
        const u32 r = m + 0x10u * row;
        const float m0 = LF(c, r), m1 = LF(c, r + 4u), m2 = LF(c, r + 8u), m3 = LF(c, r + 0xCu);
        bad |= TN(m0) | TN(m1) | TN(m2) | TN(m3);
        const float acc = dotf(m0, m1, m2, vx, vy, vz, bad);
        result[row] = m3 + acc;
        bad |= TN(result[row]);
    }
    if (bad) return false;
    for (u32 row = 0; row < 3u; ++row)
        c.mem_write32(m + 0x10u * row + 0xCu, float_to_bits(result[row]));
    return true;
}

bool scale_columns_fast(Cpu& c, u32 m, float v0, float v1, float v2) {
    bool bad = TN(v0) | TN(v1) | TN(v2);
    const float v[3] = {v0, v1, v2};
    float result[9];
    for (u32 row = 0; row < 3u; ++row) {
        const u32 r = m + 0x10u * row;
        for (u32 col = 0; col < 3u; ++col) {
            const float x = LF(c, r + 4u * col);
            bad |= TN(x);
            result[3u * row + col] = x * v[col];
            bad |= TN(result[3u * row + col]);
        }
    }
    if (bad) return false;
    for (u32 row = 0; row < 3u; ++row)
        for (u32 col = 0; col < 3u; ++col)
            c.mem_write32(m + 0x10u * row + 4u * col, float_to_bits(result[3u * row + col]));
    return true;
}

bool squared_length_fast(Cpu& c, u32 v, u64& result) {
    const float x = LF(c, v), y = LF(c, v + 4u), z = LF(c, v + 8u);
    const float xx = x * x, yy = y * y, zz = z * z;
    const float xy = xx + yy;
    const float r = zz + xy;
    if (TN(x) | TN(y) | TN(z) | TN(xx) | TN(yy) | TN(zz) | TN(xy) | TN(r)) return false;
    result = single_to_double_bits(float_to_bits(r));
    return true;
}

bool concat_fast(Cpu& c, u32 a3, u32 a4, u32 a5) {
    bool bad = false;
    float res[12];
    float f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f23, f24, f25, f26, f27, f28, f29, f30, f31;
    f31 = LF(c, a5 + 16u); bad |= TN(f31);
    f0 = LF(c, a4 + 4u); bad |= TN(f0);
    f11 = LF(c, a5 + 20u); bad |= TN(f11);
    f4 = LF(c, a5 + 28u); bad |= TN(f4);
    f2 = f0 * f31; bad |= TN(f2);
    f7 = LF(c, a5 + 24u); bad |= TN(f7);
    f1 = f0 * f11; bad |= TN(f1);
    f30 = LF(c, a5 + 0u); bad |= TN(f30);
    f3 = f0 * f4; bad |= TN(f3);
    f6 = LF(c, a4 + 0u); bad |= TN(f6);
    f12 = LF(c, a5 + 4u); bad |= TN(f12);
    f0 = f0 * f7; bad |= TN(f0);
    f24 = LF(c, a4 + 20u); bad |= TN(f24);
    f2 = FMA(f6, f30, f2); bad |= TN(f2);
    f5 = LF(c, a5 + 12u); bad |= TN(f5);
    f1 = FMA(f6, f12, f1); bad |= TN(f1);
    f8 = LF(c, a5 + 8u); bad |= TN(f8);
    f29 = LF(c, a5 + 32u); bad |= TN(f29);
    f25 = FMA(f6, f5, f3); bad |= TN(f25);
    f28 = LF(c, a4 + 8u); bad |= TN(f28);
    f26 = FMA(f6, f8, f0); bad |= TN(f26);
    f13 = LF(c, a5 + 36u); bad |= TN(f13);
    f10 = f24 * f31; bad |= TN(f10);
    f3 = FMA(f28, f29, f2); bad |= TN(f3);
    f6 = LF(c, a5 + 44u); bad |= TN(f6);
    f27 = FMA(f28, f13, f1); bad |= TN(f27);
    f1 = LF(c, a4 + 36u); bad |= TN(f1);
    f2 = f24 * f11; bad |= TN(f2);
    f0 = LF(c, a4 + 16u); bad |= TN(f0);
    f9 = LF(c, a5 + 40u); bad |= TN(f9);
    f23 = f24 * f4; bad |= TN(f23);
    res[0] = f3;
    f3 = f24 * f7; bad |= TN(f3);
    f25 = FMA(f28, f6, f25); bad |= TN(f25);
    f24 = LF(c, a4 + 12u); bad |= TN(f24);
    f28 = FMA(f28, f9, f26); bad |= TN(f28);
    res[1] = f27;
    f26 = FMA(f0, f30, f10); bad |= TN(f26);
    f10 = LF(c, a4 + 24u); bad |= TN(f10);
    f27 = FMA(f0, f12, f2); bad |= TN(f27);
    res[2] = f28;
    f25 = f24 + f25; bad |= TN(f25);
    f2 = LF(c, a4 + 32u); bad |= TN(f2);
    f24 = FMA(f0, f5, f23); bad |= TN(f24);
    f23 = LF(c, a4 + 28u); bad |= TN(f23);
    f28 = FMA(f0, f8, f3); bad |= TN(f28);
    res[3] = f25;
    f26 = FMA(f10, f29, f26); bad |= TN(f26);
    f3 = LF(c, a4 + 40u); bad |= TN(f3);
    f25 = f1 * f31; bad |= TN(f25);
    f0 = LF(c, a4 + 44u); bad |= TN(f0);
    f24 = FMA(f10, f6, f24); bad |= TN(f24);
    res[4] = f26;
    f31 = FMA(f10, f13, f27); bad |= TN(f31);
    f26 = f1 * f11; bad |= TN(f26);
    f27 = f1 * f4; bad |= TN(f27);
    f11 = FMA(f2, f30, f25); bad |= TN(f11);
    res[5] = f31;
    f4 = FMA(f10, f9, f28); bad |= TN(f4);
    f7 = f1 * f7; bad |= TN(f7);
    f1 = FMA(f2, f12, f26); bad |= TN(f1);
    f5 = FMA(f2, f5, f27); bad |= TN(f5);
    res[6] = f4;
    f10 = f23 + f24; bad |= TN(f10);
    f2 = FMA(f2, f8, f7); bad |= TN(f2);
    f4 = FMA(f3, f6, f5); bad |= TN(f4);
    f5 = FMA(f3, f29, f11); bad |= TN(f5);
    res[7] = f10;
    f1 = FMA(f3, f13, f1); bad |= TN(f1);
    f2 = FMA(f3, f9, f2); bad |= TN(f2);
    res[8] = f5;
    f0 = f0 + f4; bad |= TN(f0);
    res[9] = f1;
    res[10] = f2;
    res[11] = f0;
    if (bad) return false;
    for (u32 i = 0; i < 12u; ++i) c.mem_write32(a3 + 4u * i, float_to_bits(res[i]));
    return true;
}

bool invert_in_place_fast(Cpu& c, u32 a2, u32 a3) {
    bool bad = false;
    float res[12];
    float f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f22, f23, f24, f25, f26, f27, f28, f29, f30, f31;
    f8 = LF(c, a3 + 16u); bad |= TN(f8);
    f4 = LF(c, a3 + 40u); bad |= TN(f4);
    f5 = LF(c, a3 + 32u); bad |= TN(f5);
    f0 = f8 * f4; bad |= TN(f0);
    f3 = LF(c, a3 + 24u); bad |= TN(f3);
    f7 = LF(c, a3 + 36u); bad |= TN(f7);
    f6 = LF(c, a3 + 20u); bad |= TN(f6);
    f2 = f3 * f7; bad |= TN(f2);
    f10 = LF(c, a3 + 4u); bad |= TN(f10);
    f1 = FMA(f3, f5, -f0); bad |= TN(f1);
    f9 = LF(c, a3 + 0u); bad |= TN(f9);
    f12 = f6 * f5; bad |= TN(f12);
    f11 = LF(c, a3 + 8u); bad |= TN(f11);
    f23 = FMA(f6, f4, -f2); bad |= TN(f23);
    f0 = LF(c, a2 + 5692u); bad |= TN(f0);
    f1 = f10 * f1; bad |= TN(f1);
    f2 = FMA(f8, f7, -f12); bad |= TN(f2);
    f1 = FMA(f9, f23, f1); bad |= TN(f1);
    f24 = FMA(f11, f2, f1); bad |= TN(f24);
    if (bad) return false;  // nothing has been written or compared yet
    if (compare_equal(c, f0, f24)) return true;
    f26 = LF(c, a3 + 28u); bad |= TN(f26);
    f0 = f11 * f7; bad |= TN(f0);
    f12 = LF(c, a2 + 5688u); bad |= TN(f12);
    f1 = f3 * f5; bad |= TN(f1);
    f13 = f11 * f26; bad |= TN(f13);
    f27 = LF(c, a3 + 12u); bad |= TN(f27);
    f28 = f12 / f24; bad |= TN(f28);
    f25 = LF(c, a3 + 44u); bad |= TN(f25);
    f30 = f10 * f26; bad |= TN(f30);
    f0 = -FMA(f10, f4, -f0); bad |= TN(f0);
    f24 = f28 * f23; bad |= TN(f24);
    f22 = f10 * f3; bad |= TN(f22);
    f23 = f7 * f13; bad |= TN(f23);
    f12 = f11 * f6; bad |= TN(f12);
    res[0] = f24;
    f29 = f28 * f0; bad |= TN(f29);
    f0 = f11 * f5; bad |= TN(f0);
    f24 = f22 - f12; bad |= TN(f24);
    f1 = -FMA(f8, f4, -f1); bad |= TN(f1);
    res[1] = f29;
    f0 = FMA(f9, f4, -f0); bad |= TN(f0);
    f31 = f28 * f24; bad |= TN(f31);
    f24 = f10 * f5; bad |= TN(f24);
    f29 = f27 * f6; bad |= TN(f29);
    f23 = FMA(f25, f22, f23); bad |= TN(f23);
    res[2] = f31;
    f24 = -FMA(f9, f7, -f24); bad |= TN(f24);
    f31 = f27 * f3; bad |= TN(f31);
    f23 = FMA(f4, f29, f23); bad |= TN(f23);
    f22 = f9 * f3; bad |= TN(f22);
    f13 = f5 * f13; bad |= TN(f13);
    f23 = -FMA(f7, f31, -f23); bad |= TN(f23);
    f3 = f11 * f8; bad |= TN(f3);
    f11 = f27 * f8; bad |= TN(f11);
    f27 = -FMA(f25, f12, -f23); bad |= TN(f27);
    f12 = FMA(f25, f22, f13); bad |= TN(f12);
    f13 = f22 - f3; bad |= TN(f13);
    f27 = -FMA(f4, f30, -f27); bad |= TN(f27);
    f12 = FMA(f4, f11, f12); bad |= TN(f12);
    f13 = -f13;
    f27 = -f27;
    f12 = -FMA(f5, f31, -f12); bad |= TN(f12);
    f22 = f9 * f6; bad |= TN(f22);
    f6 = f5 * f30; bad |= TN(f6);
    f27 = f28 * f27; bad |= TN(f27);
    f23 = f10 * f8; bad |= TN(f23);
    f6 = FMA(f25, f22, f6); bad |= TN(f6);
    f10 = f28 * f1; bad |= TN(f10);
    res[3] = f27;
    f8 = f22 - f23; bad |= TN(f8);
    f1 = FMA(f7, f11, f6); bad |= TN(f1);
    f9 = f9 * f26; bad |= TN(f9);
    res[4] = f10;
    f3 = -FMA(f25, f3, -f12); bad |= TN(f3);
    f6 = f28 * f0; bad |= TN(f6);
    f0 = -FMA(f5, f29, -f1); bad |= TN(f0);
    f1 = -FMA(f4, f9, -f3); bad |= TN(f1);
    f3 = f28 * f13; bad |= TN(f3);
    res[5] = f6;
    f0 = -FMA(f25, f23, -f0); bad |= TN(f0);
    f1 = f28 * f1; bad |= TN(f1);
    res[6] = f3;
    f3 = f28 * f2; bad |= TN(f3);
    f0 = -FMA(f7, f9, -f0); bad |= TN(f0);
    res[7] = f1;
    f2 = f28 * f24; bad |= TN(f2);
    f1 = f28 * f8; bad |= TN(f1);
    res[8] = f3;
    f0 = -f0;
    res[9] = f2;
    f0 = f28 * f0; bad |= TN(f0);
    res[10] = f1;
    res[11] = f0;
    if (bad) return false;
    for (u32 i = 0; i < 12u; ++i) c.mem_write32(a3 + 4u * i, float_to_bits(res[i]));
    return true;
}

bool invert_fast(Cpu& c, u32 a2, u32 a3, u32 a4) {
    bool bad = false;
    float res[12];
    float f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f21, f22, f23, f24, f25, f26, f27, f28, f29, f30, f31;
    f2 = LF(c, a4 + 32u); bad |= TN(f2);
    f0 = LF(c, a4 + 24u); bad |= TN(f0);
    f5 = LF(c, a4 + 16u); bad |= TN(f5);
    f1 = LF(c, a4 + 40u); bad |= TN(f1);
    f13 = f0 * f2; bad |= TN(f13);
    f4 = LF(c, a4 + 36u); bad |= TN(f4);
    f21 = f5 * f1; bad |= TN(f21);
    f3 = LF(c, a4 + 20u); bad |= TN(f3);
    f8 = f0 * f4; bad |= TN(f8);
    f6 = LF(c, a4 + 4u); bad |= TN(f6);
    f10 = f3 * f2; bad |= TN(f10);
    f7 = LF(c, a4 + 0u); bad |= TN(f7);
    f9 = f13 - f21; bad |= TN(f9);
    f12 = LF(c, a4 + 8u); bad |= TN(f12);
    f23 = FMA(f3, f1, -f8); bad |= TN(f23);
    f8 = LF(c, a2 + 5692u); bad |= TN(f8);
    f11 = FMA(f5, f4, -f10); bad |= TN(f11);
    f9 = f6 * f9; bad |= TN(f9);
    f9 = FMA(f7, f23, f9); bad |= TN(f9);
    f22 = FMA(f12, f11, f9); bad |= TN(f22);
    if (bad) return false;  // nothing has been written or compared yet
    if (compare_equal(c, f8, f22)) return zero_det(c, a3, a4);
    f10 = LF(c, a4 + 28u); bad |= TN(f10);
    f25 = f12 * f4; bad |= TN(f25);
    f9 = LF(c, a2 + 5688u); bad |= TN(f9);
    f24 = f21 - f13; bad |= TN(f24);
    f13 = f12 * f10; bad |= TN(f13);
    f8 = LF(c, a4 + 12u); bad |= TN(f8);
    f27 = f9 / f22; bad |= TN(f27);
    f9 = LF(c, a4 + 44u); bad |= TN(f9);
    f29 = f6 * f10; bad |= TN(f29);
    f25 = -FMA(f6, f1, -f25); bad |= TN(f25);
    f26 = f27 * f23; bad |= TN(f26);
    f21 = f6 * f0; bad |= TN(f21);
    f23 = f4 * f13; bad |= TN(f23);
    f28 = f12 * f3; bad |= TN(f28);
    res[0] = f26;
    f30 = f27 * f25; bad |= TN(f30);
    f25 = f12 * f2; bad |= TN(f25);
    f26 = f21 - f28; bad |= TN(f26);
    f24 = -f24;
    res[1] = f30;
    f25 = FMA(f7, f1, -f25); bad |= TN(f25);
    f30 = f27 * f26; bad |= TN(f30);
    f26 = f6 * f2; bad |= TN(f26);
    f31 = f8 * f3; bad |= TN(f31);
    f23 = FMA(f9, f21, f23); bad |= TN(f23);
    res[2] = f30;
    f26 = -FMA(f7, f4, -f26); bad |= TN(f26);
    f30 = f8 * f0; bad |= TN(f30);
    f23 = FMA(f1, f31, f23); bad |= TN(f23);
    f21 = f7 * f0; bad |= TN(f21);
    f22 = f2 * f13; bad |= TN(f22);
    f13 = -FMA(f4, f30, -f23); bad |= TN(f13);
    f12 = f12 * f5; bad |= TN(f12);
    f0 = f8 * f5; bad |= TN(f0);
    f13 = -FMA(f9, f28, -f13); bad |= TN(f13);
    f23 = FMA(f9, f21, f22); bad |= TN(f23);
    f8 = f21 - f12; bad |= TN(f8);
    f13 = -FMA(f1, f29, -f13); bad |= TN(f13);
    f23 = FMA(f1, f0, f23); bad |= TN(f23);
    f8 = -f8;
    f13 = -f13;
    f23 = -FMA(f2, f30, -f23); bad |= TN(f23);
    f22 = f7 * f3; bad |= TN(f22);
    f3 = f2 * f29; bad |= TN(f3);
    f13 = f27 * f13; bad |= TN(f13);
    f6 = f6 * f5; bad |= TN(f6);
    f3 = FMA(f9, f22, f3); bad |= TN(f3);
    f7 = f7 * f10; bad |= TN(f7);
    res[3] = f13;
    f5 = -FMA(f9, f12, -f23); bad |= TN(f5);
    f0 = FMA(f4, f0, f3); bad |= TN(f0);
    f3 = f27 * f24; bad |= TN(f3);
    f5 = -FMA(f1, f7, -f5); bad |= TN(f5);
    f1 = -FMA(f2, f31, -f0); bad |= TN(f1);
    f0 = f22 - f6; bad |= TN(f0);
    res[4] = f3;
    f2 = f27 * f25; bad |= TN(f2);
    f3 = -FMA(f9, f6, -f1); bad |= TN(f3);
    f1 = f27 * f8; bad |= TN(f1);
    res[5] = f2;
    f5 = f27 * f5; bad |= TN(f5);
    f3 = -FMA(f4, f7, -f3); bad |= TN(f3);
    res[6] = f1;
    f2 = f27 * f11; bad |= TN(f2);
    f1 = f27 * f26; bad |= TN(f1);
    res[7] = f5;
    f3 = -f3;
    f0 = f27 * f0; bad |= TN(f0);
    res[8] = f2;
    f2 = f27 * f3; bad |= TN(f2);
    res[9] = f1;
    res[10] = f0;
    res[11] = f2;
    if (bad) return false;
    for (u32 i = 0; i < 12u; ++i) c.mem_write32(a3 + 4u * i, float_to_bits(res[i]));
    return true;
}


}  // namespace detail

// ---- the owners --------------------------------------------------------------------------------------------------------------
void serve_point(Cpu& c) {
    const u32 out = c.gpr[3], m = c.gpr[4], p = c.gpr[5];
    if (!detail::point_fast(c, out, m, p)) point_exact(c, out, m, p);
}

void serve_rotate_vector(Cpu& c) {
    const u32 out = c.gpr[3], m = c.gpr[4], p = c.gpr[5];
    if (!detail::rotate_vector_fast(c, out, m, p)) rotate_vector_exact(c, out, m, p);
}

void serve_translate_by(Cpu& c) {
    const u32 m = c.gpr[3], v = c.gpr[4];
    const float vy = LF(c, v + 4u), vx = LF(c, v), vz = LF(c, v + 8u);
    if (!detail::translate_by_fast(c, m, vx, vy, vz)) {
        translate_by_exact(c, m, L(c.mem_read32(v)), L(c.mem_read32(v + 4u)), L(c.mem_read32(v + 8u)));
    }
}

void serve_translate_floats(Cpu& c) {
    const u32 m = c.gpr[3];
    const F x = c.fpr[1].bits[0], y = c.fpr[2].bits[0], z = c.fpr[3].bits[0];
    float fx, fy, fz;
    if (single_value(x, fx) && single_value(y, fy) && single_value(z, fz) && detail::translate_by_fast(c, m, fx, fy, fz)) return;
    translate_by_exact(c, m, x, y, z);
}

void serve_scale_columns(Cpu& c) {
    const u32 m = c.gpr[3], v = c.gpr[4];
    if (!detail::scale_columns_fast(c, m, LF(c, v), LF(c, v + 4u), LF(c, v + 8u))) {
        scale_columns_exact(c, m, L(c.mem_read32(v)), L(c.mem_read32(v + 4u)), L(c.mem_read32(v + 8u)));
    }
}

void serve_scale_floats(Cpu& c) {
    const u32 m = c.gpr[3];
    const F x = c.fpr[1].bits[0], y = c.fpr[2].bits[0], z = c.fpr[3].bits[0];
    float fx, fy, fz;
    if (single_value(x, fx) && single_value(y, fy) && single_value(z, fz) && detail::scale_columns_fast(c, m, fx, fy, fz)) return;
    scale_columns_exact(c, m, x, y, z);
}

void serve_squared_length(Cpu& c) {
    const u32 v = c.gpr[3];
    F result;
    if (!detail::squared_length_fast(c, v, result)) {
        const F fx = L(c.mem_read32(v)), fy = L(c.mem_read32(v + 4u)), fz = L(c.mem_read32(v + 8u));
        const F a = ppc::fps_mul(c, fx, fx), b = ppc::fps_mul(c, fy, fy), d = ppc::fps_mul(c, fz, fz);
        result = ppc::fps_add(c, d, ppc::fps_add(c, a, b));
    }
    c.fpr[1].bits[0] = result;
    c.fpr[1].bits[1] = result;
}

void serve_concat(Cpu& c) {
    const u32 a3 = c.gpr[3], a4 = c.gpr[4], a5 = c.gpr[5];
    Frame frame(c, 0xA0u, 23u, false);
    const bool ok = (a3 == a4 || plain_span(a3, a4)) && (a3 == a5 || plain_span(a3, a5));
    if (!(ok && detail::concat_fast(c, a3, a4, a5))) concat_exact(c);
}

void serve_invert_in_place(Cpu& c) {
    const u32 a2 = c.gpr[2], a3 = c.gpr[3];
    Frame frame(c, 0xB0u, 22u, false);
    if (!detail::invert_in_place_fast(c, a2, a3)) invert_in_place_exact(c);
}

void serve_invert(Cpu& c) {
    const u32 a2 = c.gpr[2], a3 = c.gpr[3], a4 = c.gpr[4];
    Frame frame(c, 0xC0u, 21u, true);
    const bool ok = a3 == a4 || plain_span(a3, a4);
    if (!(ok && detail::invert_fast(c, a2, a3, a4))) invert_exact(c);
}

void install(Cpu& cpu) {
    cpu.install_hle(kPoint, [](Cpu& c) { serve_point(c); return true; }, "matrix 3x4 point (native)");
    cpu.install_hle(kTranslateBy, [](Cpu& c) { serve_translate_by(c); return true; }, "matrix 3x4 translate-by (native)");
    cpu.install_hle(kScaleColumns, [](Cpu& c) { serve_scale_columns(c); return true; }, "matrix 3x4 scale columns (native)");
    cpu.install_hle(kRotateVector, [](Cpu& c) { serve_rotate_vector(c); return true; }, "matrix 3x4 rotate vector (native)");
    cpu.install_hle(kTranslateFloats, [](Cpu& c) { serve_translate_floats(c); return true; }, "matrix 3x4 translate by floats (native)");
    cpu.install_hle(kScaleFloats, [](Cpu& c) { serve_scale_floats(c); return true; }, "matrix 3x4 scale columns by floats (native)");
    cpu.install_hle(kSquaredLength, [](Cpu& c) { serve_squared_length(c); return true; }, "vec3 squared length (native)");
    cpu.install_hle(kConcat, [](Cpu& c) { serve_concat(c); return true; }, "matrix 3x4 concat (native)");
    cpu.install_hle(kInvertInPlace, [](Cpu& c) { serve_invert_in_place(c); return true; }, "matrix 3x4 invert in place (native)");
    cpu.install_hle(kInvert, [](Cpu& c) { serve_invert(c); return true; }, "matrix 3x4 invert (native)");
}

// The original sequences alone, without the fast path: the reference opt-intrinsics' differential test checks the owners against.
void serve_point_exact(Cpu& c) { point_exact(c, c.gpr[3], c.gpr[4], c.gpr[5]); }
void serve_translate_by_exact(Cpu& c) {
    const u32 m = c.gpr[3], v = c.gpr[4];
    translate_by_exact(c, m, L(c.mem_read32(v)), L(c.mem_read32(v + 4u)), L(c.mem_read32(v + 8u)));
}
void serve_scale_columns_exact(Cpu& c) {
    const u32 m = c.gpr[3], v = c.gpr[4];
    scale_columns_exact(c, m, L(c.mem_read32(v)), L(c.mem_read32(v + 4u)), L(c.mem_read32(v + 8u)));
}

}  // namespace gekko::math_affine
