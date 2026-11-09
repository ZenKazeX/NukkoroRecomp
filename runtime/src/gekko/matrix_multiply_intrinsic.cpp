#include "gekko/matrix_multiply_intrinsic.h"

#include <cstdio>
#include <cstring>

#if defined(__AVX2__)
#include <immintrin.h>
#endif

#include "gekko/cpu.h"
#include "gekko/ppc_ops.h"

namespace gekko {

namespace {

// A paired-single register held in host registers: ps0, ps1 as stored FPR bits.
struct Ps {
    u64 p0, p1;
};

constexpr u64 kOne = 0x3FF0000000000000ull;  // the ps1 a psq_l with W = 1 loads
constexpr u32 kEntry = 0x8047FC08u;

// The state a call can change, for --verify-matrix-multiply.
struct Snapshot {
    u32 gpr[32], lr, ctr, xer, cr, fpscr;
    u64 fpr[32][2];
    u8 bytes[4][64];  // out, a, b (64 bytes each), the stack frame (0x30)
};

void window_addresses(const Cpu& c, u32 base[4], u32 size[4]) {
    base[0] = c.gpr[3];
    base[1] = c.gpr[4];
    base[2] = c.gpr[5];
    base[3] = c.gpr[1] - 0x30u;
    size[0] = size[1] = size[2] = 64u;
    size[3] = 0x30u;
}

void take(Cpu& c, const u32 base[4], const u32 size[4], Snapshot& s) {
    std::memcpy(s.gpr, c.gpr, sizeof(s.gpr));
    s.lr = c.lr;
    s.ctr = c.ctr;
    s.xer = c.xer;
    s.cr = c.cr;
    s.fpscr = c.fpscr;
    for (u32 i = 0; i < 32u; ++i) {
        s.fpr[i][0] = c.fpr[i].bits[0];
        s.fpr[i][1] = c.fpr[i].bits[1];
    }
    for (u32 w = 0; w < 4u; ++w)
        for (u32 i = 0; i < size[w]; ++i)
            s.bytes[w][i] = c.mem_read8(base[w] + i);
}

void put(Cpu& c, const u32 base[4], const u32 size[4], const Snapshot& s) {
    std::memcpy(c.gpr, s.gpr, sizeof(s.gpr));
    c.lr = s.lr;
    c.ctr = s.ctr;
    c.xer = s.xer;
    c.cr = s.cr;
    c.fpscr = s.fpscr;
    for (u32 i = 0; i < 32u; ++i) {
        c.fpr[i].bits[0] = s.fpr[i][0];
        c.fpr[i].bits[1] = s.fpr[i][1];
    }
    // Overlapping windows hold the same bytes: every window was read from one state.
    for (u32 w = 0; w < 4u; ++w)
        for (u32 i = 0; i < size[w]; ++i)
            c.mem_write8(base[w] + i, s.bytes[w][i]);
}

// The ordered body's steps, each the guest instruction's effect on one paired-single value (forced inline: as
// lambdas MSVC called them, which made the ordered body slower than the translated one).
GEKKO_ALWAYS_INLINE Ps ps_load(Cpu& c, u32 address) {  // psq_l W = 0
    return Ps{single_to_double_bits(c.mem_read32(address)), single_to_double_bits(c.mem_read32(address + 4u))};
}
GEKKO_ALWAYS_INLINE Ps ps_load1(Cpu& c, u32 address) {  // psq_l W = 1
    return Ps{single_to_double_bits(c.mem_read32(address)), kOne};
}
GEKKO_ALWAYS_INLINE void ps_store(Cpu& c, u32 address, const Ps& v) {  // psq_st W = 0
    c.mem_write32(address, double_to_single_bits_flush(v.p0));
    c.mem_write32(address + 4u, double_to_single_bits_flush(v.p1));
}
GEKKO_ALWAYS_INLINE void ps_store1(Cpu& c, u32 address, const Ps& v) {  // psq_st W = 1
    c.mem_write32(address, double_to_single_bits_flush(v.p0));
}
// ps_muls0 d,a,c (ps0 of c) and ps_madds0/1 d,a,c,b (ps0/ps1 of c), each half the same helper as the guest's.
GEKKO_ALWAYS_INLINE Ps ps_muls(Cpu& c, const Ps& x, u64 s) {
    const u64 r0 = ppc::fps_mul(c, x.p0, s);
    return Ps{r0, ppc::fps_mul(c, x.p1, s)};
}
GEKKO_ALWAYS_INLINE Ps ps_madds(Cpu& c, const Ps& x, u64 s, const Ps& y) {
    const u64 r0 = ppc::fps_madd(c, x.p0, s, y.p0);
    return Ps{r0, ppc::fps_madd(c, x.p1, s, y.p1)};
}

// [x, x + xn) and [y, y + yn) share a byte (guest addresses wrap).
bool overlaps(u32 x, u32 xn, u32 y, u32 yn) noexcept { return x - y < yn || y - x < xn; }

#if defined(__AVX2__)
// fp_detail::tiny_key's test, (key >> 53) < kTinySingle with key = (bits << 1) - 1, per lane: 0 < |v| <= 2^-125,
// i.e. 0 < magnitude bits <= kTinySingle << 52.
GEKKO_ALWAYS_INLINE __m256i tiny_lanes(__m256d v) noexcept {
    const __m256i m = _mm256_and_si256(_mm256_castpd_si256(v), _mm256_set1_epi64x(0x7FFFFFFFFFFFFFFFll));
    const __m256i nonzero = _mm256_cmpgt_epi64(m, _mm256_setzero_si256());
    const __m256i above = _mm256_cmpgt_epi64(m, _mm256_set1_epi64x(static_cast<long long>(
                                                     u64{ppc::fp_detail::kTinySingle} << 52)));
    return _mm256_andnot_si256(above, nonzero);
}

// One ps_muls/ps_madds half per lane: x * s + add rounded once, then to single, as ppc::fps_fused's fast path
// computes it. `slow` collects every lane where fps_fused or fps_mul could leave that path - a tiny operand or
// result, a NaN result (every NaN operand makes one), a fused double on a single-precision midpoint. A product is
// the lane with add = -0.0: x * s + -0 is x * s exactly, signed zeros included; its midpoint test is stricter than
// fps_mul's (none), which only sends a call to the ordered path.
GEKKO_ALWAYS_INLINE __m256d fused_lanes(__m256d x, __m256d s, __m256d add, __m256i& slow) noexcept {
    const __m256d r = _mm256_fmadd_pd(x, s, add);
    const __m256i bits = _mm256_castpd_si256(r);
    const __m256i midpoint = _mm256_cmpeq_epi64(_mm256_and_si256(bits, _mm256_set1_epi64x(0x1FFFFFFFll)),
                                                _mm256_set1_epi64x(0x10000000ll));
    const __m256i nan = _mm256_castpd_si256(_mm256_cmp_pd(r, r, _CMP_UNORD_Q));
    slow = _mm256_or_si256(slow, _mm256_or_si256(_mm256_or_si256(tiny_lanes(x), tiny_lanes(s)),
                                                 _mm256_or_si256(tiny_lanes(add), tiny_lanes(r))));
    slow = _mm256_or_si256(slow, _mm256_or_si256(midpoint, nan));
    return _mm256_cvtps_pd(_mm256_cvtpd_ps(r));
}
#endif

}  // namespace

bool MatrixMultiplyIntrinsic::serve(Cpu& c) {
    // psq_l loads type 0-3 from bits 16-18 of GQR0, psq_st stores from bits 0-2.
    const u32 gqr0 = c.spr[SPR_GQR0];
    if ((c.msr() & 0x2000u) == 0u || ((gqr0 >> 16) & 7u) >= 4u || (gqr0 & 7u) >= 4u) {
        ++declines_;
        return false;
    }
    ++calls_;
    return verify_ ? verify(c) : execute(c);
}

bool MatrixMultiplyIntrinsic::execute(Cpu& c) {
    if (run_wide(c)) {
        ++wide_;
        return true;
    }
    return run(c);
}

// The body four lanes at a time. Each ps_muls/ps_madds half is one lane of three steps (right, up, at terms):
//   lanes 0-3   f12 (out.right.xy), f13 (out.up.xy)          a.right / a.up  x, y, z  times b.right/up/at .xy
//   lanes 4-7   f30 (out.at.xy), f31 (out.pos.xy)            a.at / a.pos, f31 starting from b.pos.xy
//   lanes 8-11  f12', f13' = (out.right.z, sum), (out.up.z, sum)   the second pass: (b.*.z, 1.0)
//   lanes 12-15 f8', f11'  = (out.at.z, sum), (out.pos.z, sum)     f11' starting from (b.pos.z, 1.0)
// When no lane leaves the helpers' fast path, the fast path changes no FPSCR bit, so the order of the operations is
// not observable and the result is the ordered body's. Every input is read before anything is written; an output
// that overlaps an input or the stack frame, or a lane off the fast path, returns false having written nothing.
bool MatrixMultiplyIntrinsic::run_wide(Cpu& c) {
#if defined(__AVX2__)
    const u32 out = c.gpr[3];
    const u32 a = c.gpr[4];
    const u32 b = c.gpr[5];
    const u32 sp = c.gpr[1] - 0x30u;
    if (overlaps(out, 64u, a, 64u) || overlaps(out, 64u, b, 64u) || overlaps(out, 64u, sp, 0x30u) ||
        overlaps(a, 64u, sp, 0x30u) || overlaps(b, 64u, sp, 0x30u)) {
        ++overlapping_;
        return false;
    }
    // The FPR values the psq_l loads make, as stored bits (index = word offset / 4; 3, 7, 11, 15 unused).
    u64 av[16], bv[16];
    for (u32 i = 0; i < 16u; ++i) {
        if ((i & 3u) == 3u) continue;
        av[i] = single_to_double_bits(c.mem_read32(a + 4u * i));
        bv[i] = single_to_double_bits(c.mem_read32(b + 4u * i));
    }
    const auto d = [](u64 bits) { return bits_to_double(bits); };
    const double one = 1.0;
    const double nz = -0.0;
    // _mm256_set_pd takes lanes high to low.
    const auto pair2 = [&](double x0, double x1, double y0, double y1) { return _mm256_set_pd(y1, y0, x1, x0); };
    const __m256d right_xy = pair2(d(bv[0]), d(bv[1]), d(bv[0]), d(bv[1]));
    const __m256d up_xy = pair2(d(bv[4]), d(bv[5]), d(bv[4]), d(bv[5]));
    const __m256d at_xy = pair2(d(bv[8]), d(bv[9]), d(bv[8]), d(bv[9]));
    const __m256d right_z = pair2(d(bv[2]), one, d(bv[2]), one);
    const __m256d up_z = pair2(d(bv[6]), one, d(bv[6]), one);
    const __m256d at_z = pair2(d(bv[10]), one, d(bv[10]), one);
    // a's rows: right 0, up 4, at 8, pos 12; component k at + k.
    const auto rows = [&](u32 r0, u32 r1, u32 k) {
        return pair2(d(av[r0 + k]), d(av[r0 + k]), d(av[r1 + k]), d(av[r1 + k]));
    };
    const __m256d zero2 = _mm256_set1_pd(nz);
    const __m256d pos_xy = pair2(nz, nz, d(bv[12]), d(bv[13]));
    const __m256d pos_z = pair2(nz, nz, d(bv[14]), one);
    __m256i slow = _mm256_setzero_si256();
    __m256d v0 = fused_lanes(right_xy, rows(0u, 4u, 0u), zero2, slow);
    __m256d v1 = fused_lanes(right_xy, rows(8u, 12u, 0u), pos_xy, slow);
    __m256d v2 = fused_lanes(right_z, rows(0u, 4u, 0u), zero2, slow);
    __m256d v3 = fused_lanes(right_z, rows(8u, 12u, 0u), pos_z, slow);
    v0 = fused_lanes(up_xy, rows(0u, 4u, 1u), v0, slow);
    v1 = fused_lanes(up_xy, rows(8u, 12u, 1u), v1, slow);
    v2 = fused_lanes(up_z, rows(0u, 4u, 1u), v2, slow);
    v3 = fused_lanes(up_z, rows(8u, 12u, 1u), v3, slow);
    v0 = fused_lanes(at_xy, rows(0u, 4u, 2u), v0, slow);
    v1 = fused_lanes(at_xy, rows(8u, 12u, 2u), v1, slow);
    v2 = fused_lanes(at_z, rows(0u, 4u, 2u), v2, slow);
    v3 = fused_lanes(at_z, rows(8u, 12u, 2u), v3, slow);
    if (!_mm256_testz_si256(slow, slow)) {
        ++off_fast_path_;
        return false;
    }
    alignas(32) u64 r[4][4];
    _mm256_store_si256(reinterpret_cast<__m256i*>(r[0]), _mm256_castpd_si256(v0));
    _mm256_store_si256(reinterpret_cast<__m256i*>(r[1]), _mm256_castpd_si256(v1));
    _mm256_store_si256(reinterpret_cast<__m256i*>(r[2]), _mm256_castpd_si256(v2));
    _mm256_store_si256(reinterpret_cast<__m256i*>(r[3]), _mm256_castpd_si256(v3));

    // The prologue's stack words.
    const u64 f31_0 = c.fpr[31].bits[0], f31_1 = c.fpr[31].bits[1];
    const u64 f30_0 = c.fpr[30].bits[0], f30_1 = c.fpr[30].bits[1];
    const u32 f31_s0 = double_to_single_bits_flush(f31_0), f31_s1 = double_to_single_bits_flush(f31_1);
    const u32 f30_s0 = double_to_single_bits_flush(f30_0), f30_s1 = double_to_single_bits_flush(f30_1);
    c.mem_write32(sp, c.gpr[1]);
    c.mem_write64(sp + 0x20u, f31_0);
    c.mem_write32(sp + 0x28u, f31_s0);
    c.mem_write32(sp + 0x2Cu, f31_s1);
    c.mem_write64(sp + 0x10u, f30_0);
    c.mem_write32(sp + 0x18u, f30_s0);
    c.mem_write32(sp + 0x1Cu, f30_s1);
    // The output: x, y from the first pass, z from the second; flags; the padding words untouched.
    const auto put_word = [&c](u32 address, u64 bits) { c.mem_write32(address, double_to_single_bits_flush(bits)); };
    put_word(out + 0x00u, r[0][0]);
    put_word(out + 0x04u, r[0][1]);
    put_word(out + 0x08u, r[2][0]);
    put_word(out + 0x10u, r[0][2]);
    put_word(out + 0x14u, r[0][3]);
    put_word(out + 0x18u, r[2][2]);
    put_word(out + 0x20u, r[1][0]);
    put_word(out + 0x24u, r[1][1]);
    put_word(out + 0x28u, r[3][0]);
    put_word(out + 0x30u, r[1][2]);
    put_word(out + 0x34u, r[1][3]);
    put_word(out + 0x38u, r[3][2]);
    const u32 r0 = c.mem_read32(a + 0x0Cu);
    const u32 r4 = r0 & c.mem_read32(b + 0x0Cu);
    c.mem_write32(out + 0x0Cu, r4);

    // The registers as the body leaves them.
    const auto set = [&c](u32 n, u64 p0, u64 p1) {
        c.fpr[n].bits[0] = p0;
        c.fpr[n].bits[1] = p1;
    };
    set(0u, av[0], av[1]);
    set(1u, av[2], kOne);
    set(2u, av[4], av[5]);
    set(3u, av[6], kOne);
    set(4u, av[8], av[9]);
    set(5u, av[10], kOne);
    set(6u, av[12], av[13]);
    set(7u, av[14], kOne);
    set(8u, r[3][0], r[3][1]);
    set(9u, bv[6], kOne);
    set(10u, bv[10], kOne);
    set(11u, r[3][2], r[3][3]);
    set(12u, r[2][0], r[2][1]);
    set(13u, r[2][2], r[2][3]);
    set(30u, f30_0, single_to_double_bits(f30_s1));
    set(31u, f31_0, single_to_double_bits(f31_s1));
    c.gpr[0] = r0;
    c.gpr[4] = r4;
    return true;
#else
    (void)c;
    return false;
#endif
}

bool MatrixMultiplyIntrinsic::verify(Cpu& c) {
    const RecompiledCode* code = c.recompiled();
    const RecompiledCode::Range* body = code != nullptr ? code->find_range(kEntry) : nullptr;
    if (body == nullptr)
        return execute(c);
    u32 base[4], size[4];
    window_addresses(c, base, size);
    static Snapshot before, native, guest;
    take(c, base, size, before);
    const u32 pc = c.pc;
    execute(c);
    take(c, base, size, native);
    put(c, base, size, before);
    body->at(c, kEntry);  // the translated body itself (the entry thunk would come back to this hook)
    c.pc = pc;            // as a served call leaves it
    take(c, base, size, guest);
    ++verified_;
    if (std::memcmp(&native, &guest, sizeof(native)) != 0) {
        if (++differed_ <= 8u) {
            std::fprintf(stderr, "[matrix-multiply] native differs from the guest body: out %08X a %08X b %08X\n",
                         base[0], base[1], base[2]);
            for (u32 i = 0; i < 32u; ++i)
                if (native.fpr[i][0] != guest.fpr[i][0] || native.fpr[i][1] != guest.fpr[i][1])
                    std::fprintf(stderr, "  f%u native %016llX %016llX guest %016llX %016llX\n", i,
                                 static_cast<unsigned long long>(native.fpr[i][0]),
                                 static_cast<unsigned long long>(native.fpr[i][1]),
                                 static_cast<unsigned long long>(guest.fpr[i][0]),
                                 static_cast<unsigned long long>(guest.fpr[i][1]));
            for (u32 i = 0; i < 32u; ++i)
                if (native.gpr[i] != guest.gpr[i])
                    std::fprintf(stderr, "  r%u native %08X guest %08X\n", i, native.gpr[i], guest.gpr[i]);
            if (native.fpscr != guest.fpscr)
                std::fprintf(stderr, "  fpscr native %08X guest %08X\n", native.fpscr, guest.fpscr);
            for (u32 w = 0; w < 4u; ++w)
                if (std::memcmp(native.bytes[w], guest.bytes[w], size[w]) != 0)
                    std::fprintf(stderr, "  memory window %u (%08X) differs\n", w, base[w]);
        }
    }
    return true;
}

bool MatrixMultiplyIntrinsic::run(Cpu& c) {
    const u32 out = c.gpr[3];
    const u32 a = c.gpr[4];
    const u32 b = c.gpr[5];
    const u32 sp = c.gpr[1] - 0x30u;

    // Prologue: stwu r1,-0x30; f31 and f30 saved by stfd and psq_st.
    const Ps f31_in{c.fpr[31].bits[0], c.fpr[31].bits[1]};
    const Ps f30_in{c.fpr[30].bits[0], c.fpr[30].bits[1]};
    c.mem_write32(sp, c.gpr[1]);
    c.mem_write64(sp + 0x20u, f31_in.p0);
    ps_store(c, sp + 0x28u, f31_in);
    c.mem_write64(sp + 0x10u, f30_in.p0);
    ps_store(c, sp + 0x18u, f30_in);

    // The body, in the guest's order (0x8047FC1C .. 0x8047FCE8).
    Ps f8 = ps_load(c, b + 0x00u);
    const Ps f0 = ps_load(c, a + 0x00u);
    const Ps f2 = ps_load(c, a + 0x10u);
    Ps f12 = ps_muls(c, f8, f0.p0);
    Ps f9 = ps_load(c, b + 0x10u);
    Ps f13 = ps_muls(c, f8, f2.p0);
    const Ps f4 = ps_load(c, a + 0x20u);
    const Ps f6 = ps_load(c, a + 0x30u);
    Ps f30 = ps_muls(c, f8, f4.p0);
    Ps f11 = ps_load(c, b + 0x30u);
    f12 = ps_madds(c, f9, f0.p1, f12);
    Ps f10 = ps_load(c, b + 0x20u);
    f13 = ps_madds(c, f9, f2.p1, f13);
    Ps f31 = ps_madds(c, f8, f6.p0, f11);
    const Ps f1 = ps_load1(c, a + 0x08u);
    f30 = ps_madds(c, f9, f4.p1, f30);
    const Ps f3 = ps_load1(c, a + 0x18u);
    f12 = ps_madds(c, f10, f1.p0, f12);
    const Ps f5 = ps_load1(c, a + 0x28u);
    f31 = ps_madds(c, f9, f6.p1, f31);
    const Ps f7 = ps_load1(c, a + 0x38u);
    f13 = ps_madds(c, f10, f3.p0, f13);
    f8 = ps_load1(c, b + 0x08u);
    ps_store(c, out + 0x00u, f12);
    f30 = ps_madds(c, f10, f5.p0, f30);
    f31 = ps_madds(c, f10, f7.p0, f31);
    f11 = ps_load1(c, b + 0x38u);
    ps_store(c, out + 0x10u, f13);
    f12 = ps_muls(c, f8, f0.p0);
    f9 = ps_load1(c, b + 0x18u);
    ps_store(c, out + 0x20u, f30);
    f13 = ps_muls(c, f8, f2.p0);
    f10 = ps_load1(c, b + 0x28u);
    f12 = ps_madds(c, f9, f0.p1, f12);
    const u32 r0 = c.mem_read32(a + 0x0Cu);
    u32 r4 = c.mem_read32(b + 0x0Cu);
    f11 = ps_madds(c, f8, f6.p0, f11);
    f30 = ps_muls(c, f8, f4.p0);
    ps_store(c, out + 0x30u, f31);
    f13 = ps_madds(c, f9, f2.p1, f13);
    r4 = r0 & r4;
    f11 = ps_madds(c, f9, f6.p1, f11);
    c.mem_write32(out + 0x0Cu, r4);
    f12 = ps_madds(c, f10, f1.p0, f12);
    f8 = ps_madds(c, f9, f4.p1, f30);
    f13 = ps_madds(c, f10, f3.p0, f13);
    ps_store1(c, out + 0x08u, f12);
    f11 = ps_madds(c, f10, f7.p0, f11);
    f8 = ps_madds(c, f10, f5.p0, f8);
    ps_store1(c, out + 0x18u, f13);
    ps_store1(c, out + 0x28u, f8);
    ps_store1(c, out + 0x38u, f11);

    // Epilogue: psq_l then lfd restore f31 and f30 (ps1 by way of its single-precision save); r1 back.
    const Ps f31_saved = ps_load(c, sp + 0x28u);
    const u64 f31_p0 = c.mem_read64(sp + 0x20u);
    const Ps f30_saved = ps_load(c, sp + 0x18u);
    const u64 f30_p0 = c.mem_read64(sp + 0x10u);

    const Ps regs[14] = {f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13};
    for (u32 i = 0; i < 14u; ++i) {
        c.fpr[i].bits[0] = regs[i].p0;
        c.fpr[i].bits[1] = regs[i].p1;
    }
    c.fpr[31].bits[0] = f31_p0;
    c.fpr[31].bits[1] = f31_saved.p1;
    c.fpr[30].bits[0] = f30_p0;
    c.fpr[30].bits[1] = f30_saved.p1;
    c.gpr[0] = r0;
    c.gpr[4] = r4;
    return true;
}

}  // namespace gekko
