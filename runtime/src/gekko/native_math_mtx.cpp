// The native owner of the Dolphin SDK's PSMTXConcat 0x8037976C (gekko/native_math_mtx.h).
//
// Two paths, as in native_math_affine.cpp. The fast path is plain single-precision host arithmetic in the original's operation
// order: per lane one product, then the fused steps as one-rounding fma (ps_muls0/1 and ps_madds0/1 are lanes of fmuls / fmadds
// with a chosen operand half). Any input, constant, intermediate or result that is NaN or a nonzero magnitude below 2^-100
// sends the whole call to the exact path, before anything is written. Above that bound ppc::fps_mul / fps_madd are the
// correctly rounded single operations (the FPSCR[NI] flush and the NaN rules only act below it or on NaN), so the host
// operations give the same bits. The fast path also needs plain quantisation (GQR0 load and store types 0..3, which move
// singles unchanged) and a, b, ab and the constant clear of the 0x40-byte frame below SP: the original writes the frame
// before its loads and restores f14 / f15 / f31 from it between its stores, so an overlap changes what it reads.
// Aliasing between a, b and ab needs nothing: the original reads all 24 input words before its first store.
// The exact path is the original's instruction sequence on the guest's registers through ppc::psq_load_inl / psq_store_inl
// (GQR0 at the call) and ppc::fps_mul / fps_madd, the helpers the generated translation of this body used.

#include "gekko/native_math_mtx.h"

#include "gekko/cpu.h"
#include "gekko/ppc_ops.h"

#if defined(__AVX2__) || defined(__FMA__)
#include <immintrin.h>
#else
#include <cmath>
#endif

namespace gekko::math_mtx {

namespace {
constexpr u32 kFrame = 0x40u;  // stwu r1, -0x40(r1)

GEKKO_ALWAYS_INLINE float LF(Cpu& c, u32 address) { return bits_to_float(c.mem_read32(address)); }
// 0 < |x| < 2^-100, or NaN: the fast path declines (the same test as native_math_affine.cpp).
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
// [x, x + size) and the frame [frame, frame + 0x40) share no byte (guest addresses wrap at 2^32).
GEKKO_ALWAYS_INLINE bool clear_of_frame(u32 x, u32 size, u32 frame) noexcept {
    return static_cast<u32>(x - frame) >= kFrame && static_cast<u32>(frame - x) >= size;
}

// ps_muls0 / ps_muls1 frD, frA, frC: both lanes of frA times one half of frC, each rounded to single.
GEKKO_ALWAYS_INLINE void muls(Cpu& c, u32 d, u32 a, u32 cc, u32 half) {
    const u64 p0 = ppc::fps_mul(c, c.fpr[a].bits[0], c.fpr[cc].bits[half]);
    const u64 p1 = ppc::fps_mul(c, c.fpr[a].bits[1], c.fpr[cc].bits[half]);
    c.fpr[d].bits[0] = p0;
    c.fpr[d].bits[1] = p1;
}
// ps_madds0 / ps_madds1 frD, frA, frC, frB: per lane frA * (one half of frC) + frB, one rounding.
GEKKO_ALWAYS_INLINE void madds(Cpu& c, u32 d, u32 a, u32 cc, u32 b, u32 half) {
    const u64 p0 = ppc::fps_madd(c, c.fpr[a].bits[0], c.fpr[cc].bits[half], c.fpr[b].bits[0]);
    const u64 p1 = ppc::fps_madd(c, c.fpr[a].bits[1], c.fpr[cc].bits[half], c.fpr[b].bits[1]);
    c.fpr[d].bits[0] = p0;
    c.fpr[d].bits[1] = p1;
}

// The original, 0x8037976C .. 0x80379834, one statement per instruction (r6 = 0x805EEC18 is not kept).
void concat_exact(Cpu& c) {
    const u32 a = c.gpr[3], b = c.gpr[4], ab = c.gpr[5];
    const u32 sp = c.gpr[1], fr = sp - kFrame;
    c.mem_write32(fr, sp);                          // stwu   r1, -0x40(r1)
    ppc::psq_load_inl(c, 0u, a + 0x00u, false, 0u);  // psq_l  f0, 0(r3)
    c.mem_write64(fr + 0x08u, c.fpr[14].bits[0]);   // stfd   f14, 8(r1)
    ppc::psq_load_inl(c, 6u, b + 0x00u, false, 0u);  // psq_l  f6, 0(r4)
    ppc::psq_load_inl(c, 7u, b + 0x08u, false, 0u);  // psq_l  f7, 8(r4)
    c.mem_write64(fr + 0x10u, c.fpr[15].bits[0]);   // stfd   f15, 0x10(r1)
    c.mem_write64(fr + 0x28u, c.fpr[31].bits[0]);   // stfd   f31, 0x28(r1)
    ppc::psq_load_inl(c, 8u, b + 0x10u, false, 0u);  // psq_l  f8, 0x10(r4)
    muls(c, 12u, 6u, 0u, 0u);                       // ps_muls0  f12, f6, f0
    ppc::psq_load_inl(c, 2u, a + 0x10u, false, 0u);  // psq_l  f2, 0x10(r3)
    muls(c, 13u, 7u, 0u, 0u);                       // ps_muls0  f13, f7, f0
    ppc::psq_load_inl(c, 31u, kUnit01, false, 0u);   // psq_l  f31, 0(r6)
    muls(c, 14u, 6u, 2u, 0u);                       // ps_muls0  f14, f6, f2
    ppc::psq_load_inl(c, 9u, b + 0x18u, false, 0u);  // psq_l  f9, 0x18(r4)
    muls(c, 15u, 7u, 2u, 0u);                       // ps_muls0  f15, f7, f2
    ppc::psq_load_inl(c, 1u, a + 0x08u, false, 0u);  // psq_l  f1, 8(r3)
    madds(c, 12u, 8u, 0u, 12u, 1u);                 // ps_madds1 f12, f8, f0, f12
    ppc::psq_load_inl(c, 3u, a + 0x18u, false, 0u);  // psq_l  f3, 0x18(r3)
    madds(c, 14u, 8u, 2u, 14u, 1u);                 // ps_madds1 f14, f8, f2, f14
    ppc::psq_load_inl(c, 10u, b + 0x20u, false, 0u); // psq_l  f10, 0x20(r4)
    madds(c, 13u, 9u, 0u, 13u, 1u);                 // ps_madds1 f13, f9, f0, f13
    ppc::psq_load_inl(c, 11u, b + 0x28u, false, 0u); // psq_l  f11, 0x28(r4)
    madds(c, 15u, 9u, 2u, 15u, 1u);                 // ps_madds1 f15, f9, f2, f15
    ppc::psq_load_inl(c, 4u, a + 0x20u, false, 0u);  // psq_l  f4, 0x20(r3)
    ppc::psq_load_inl(c, 5u, a + 0x28u, false, 0u);  // psq_l  f5, 0x28(r3)
    madds(c, 12u, 10u, 1u, 12u, 0u);                // ps_madds0 f12, f10, f1, f12
    madds(c, 13u, 11u, 1u, 13u, 0u);                // ps_madds0 f13, f11, f1, f13
    madds(c, 14u, 10u, 3u, 14u, 0u);                // ps_madds0 f14, f10, f3, f14
    madds(c, 15u, 11u, 3u, 15u, 0u);                // ps_madds0 f15, f11, f3, f15
    ppc::psq_store_inl(c, 12u, ab + 0x00u, false, 0u);  // psq_st f12, 0(r5)
    muls(c, 2u, 6u, 4u, 0u);                        // ps_muls0  f2, f6, f4
    madds(c, 13u, 31u, 1u, 13u, 1u);                // ps_madds1 f13, f31, f1, f13
    muls(c, 0u, 7u, 4u, 0u);                        // ps_muls0  f0, f7, f4
    ppc::psq_store_inl(c, 14u, ab + 0x10u, false, 0u);  // psq_st f14, 0x10(r5)
    madds(c, 15u, 31u, 3u, 15u, 1u);                // ps_madds1 f15, f31, f3, f15
    ppc::psq_store_inl(c, 13u, ab + 0x08u, false, 0u);  // psq_st f13, 8(r5)
    madds(c, 2u, 8u, 4u, 2u, 1u);                   // ps_madds1 f2, f8, f4, f2
    madds(c, 0u, 9u, 4u, 0u, 1u);                   // ps_madds1 f0, f9, f4, f0
    madds(c, 2u, 10u, 5u, 2u, 0u);                  // ps_madds0 f2, f10, f5, f2
    c.fpr[14].bits[0] = c.mem_read64(fr + 0x08u);   // lfd    f14, 8(r1)    (ps0 only)
    ppc::psq_store_inl(c, 15u, ab + 0x18u, false, 0u);  // psq_st f15, 0x18(r5)
    madds(c, 0u, 11u, 5u, 0u, 0u);                  // ps_madds0 f0, f11, f5, f0
    ppc::psq_store_inl(c, 2u, ab + 0x20u, false, 0u);   // psq_st f2, 0x20(r5)
    madds(c, 0u, 31u, 5u, 0u, 1u);                  // ps_madds1 f0, f31, f5, f0
    c.fpr[15].bits[0] = c.mem_read64(fr + 0x10u);   // lfd    f15, 0x10(r1)
    ppc::psq_store_inl(c, 0u, ab + 0x28u, false, 0u);   // psq_st f0, 0x28(r5)
    c.fpr[31].bits[0] = c.mem_read64(fr + 0x28u);   // lfd    f31, 0x28(r1)
    // addi r1, r1, 0x40; blr
}
}  // namespace

namespace detail {

bool concat_fast(Cpu& c, u32 a, u32 b, u32 ab) {
    const u32 gqr = c.spr[SPR_GQR0];
    if (((gqr >> 16) & 7u) >= 4u || (gqr & 7u) >= 4u) return false;  // a quantised psq_l / psq_st
    const u32 sp = c.gpr[1], fr = sp - kFrame;
    if (!(clear_of_frame(a, 48u, fr) && clear_of_frame(b, 48u, fr) && clear_of_frame(ab, 48u, fr) &&
          clear_of_frame(kUnit01, 8u, fr)))
        return false;
    bool bad = false;
    float x[12], y[12];
    for (u32 i = 0; i < 12u; ++i) {
        x[i] = LF(c, a + 4u * i);
        y[i] = LF(c, b + 4u * i);
        bad |= TN(x[i]) | TN(y[i]);
    }
    const u32 unit1_word = c.mem_read32(kUnit01 + 4u);
    const float unit[2] = {LF(c, kUnit01), bits_to_float(unit1_word)};
    bad |= TN(unit[0]) | TN(unit[1]);
    float out[12];
    for (u32 i = 0; i < 3u; ++i) {
        const float* row = x + 4u * i;
        for (u32 j = 0; j < 4u; ++j) {
            // the lane j of the original's register for row i: muls (b[0][j] * a[i][0]), then madds1 with a[i][1],
            // madds0 with a[i][2], and for columns 2 / 3 the madds1 with unit01 against a[i][3]
            float s = y[j] * row[0];
            bad |= TN(s);
            s = FMA(y[4u + j], row[1], s);
            bad |= TN(s);
            s = FMA(y[8u + j], row[2], s);
            bad |= TN(s);
            if (j >= 2u) {
                s = FMA(unit[j - 2u], row[3], s);
                bad |= TN(s);
            }
            out[4u * i + j] = s;
        }
    }
    if (bad) return false;
    // stwu and the three stfd; the stores; the lfd restores give back the saved ps0 (the frame overlaps nothing).
    c.mem_write32(fr, sp);
    c.mem_write64(fr + 0x08u, c.fpr[14].bits[0]);
    c.mem_write64(fr + 0x10u, c.fpr[15].bits[0]);
    c.mem_write64(fr + 0x28u, c.fpr[31].bits[0]);
    for (u32 i = 0; i < 12u; ++i) c.mem_write32(ab + 4u * i, float_to_bits(out[i]));
    c.fpr[14].bits[1] = single_to_double_bits(float_to_bits(out[5]));   // f14 = ab[1][0..1]
    c.fpr[15].bits[1] = single_to_double_bits(float_to_bits(out[7]));   // f15 = ab[1][2..3]
    c.fpr[31].bits[1] = single_to_double_bits(unit1_word);              // f31 = unit01 (psq_l, type 0)
    return true;
}

}  // namespace detail

void serve_concat(Cpu& c) {
    if (!detail::concat_fast(c, c.gpr[3], c.gpr[4], c.gpr[5])) concat_exact(c);
}

void serve_concat_exact(Cpu& c) { concat_exact(c); }

void install(Cpu& cpu) {
    cpu.install_hle(kConcat, [](Cpu& c) { serve_concat(c); return true; }, "PSMTXConcat (native)");
}

}  // namespace gekko::math_mtx
