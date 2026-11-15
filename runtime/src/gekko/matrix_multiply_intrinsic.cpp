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
// One row of a matrix as four words (the fourth is the flags / padding word).
GEKKO_ALWAYS_INLINE __m128i load_row(Cpu& c, u32 address) {
    return _mm_setr_epi32(static_cast<int>(c.mem_read32(address)), static_cast<int>(c.mem_read32(address + 4u)),
                          static_cast<int>(c.mem_read32(address + 8u)), static_cast<int>(c.mem_read32(address + 12u)));
}
// |bits| - 1 per lane, unsigned: 0 < |v| <= 2^-125 (fp_detail::kTinySingle's range; a zero gives all ones) is exactly
// the lanes below 0x01000000. The running minimum over many values is one test at the end.
GEKKO_ALWAYS_INLINE __m128i tiny_key(__m128i bits) noexcept {
    return _mm_sub_epi32(_mm_and_si128(bits, _mm_set1_epi32(0x7FFFFFFF)), _mm_set1_epi32(1));
}
GEKKO_ALWAYS_INLINE __m128i tiny_key(__m128 value) noexcept { return tiny_key(_mm_castps_si128(value)); }
// True when a counted lane of `key` (a running minimum of tiny_key) belongs to a tiny value.
GEKKO_ALWAYS_INLINE bool any_tiny(__m128i key, int lanes) noexcept {
    const __m128i small = _mm_cmpeq_epi32(_mm_min_epu32(key, _mm_set1_epi32(0x00FFFFFF)), key);
    return (_mm_movemask_ps(_mm_castsi128_ps(small)) & lanes) != 0;
}
// Row I of the product: the three-term chain over b's rows with a's component I of each column (ax, ay, az hold the
// x / y / z of the four rows of a). The pos row (Pos) starts with a fused step over b's pos row.
template <int I, bool Pos, class Step>
GEKKO_ALWAYS_INLINE __m128 chain(__m128 b0, __m128 b1, __m128 b2, __m128 b3, __m128 ax, __m128 ay, __m128 az,
                                 Step& step) noexcept {
    const __m128 sx = _mm_permute_ps(ax, _MM_SHUFFLE(I, I, I, I));
    const __m128 sy = _mm_permute_ps(ay, _MM_SHUFFLE(I, I, I, I));
    const __m128 sz = _mm_permute_ps(az, _MM_SHUFFLE(I, I, I, I));
    __m128 acc = Pos ? step(_mm_fmadd_ps(b0, sx, b3)) : step(_mm_mul_ps(b0, sx));
    acc = step(_mm_fmadd_ps(b1, sy, acc));
    return step(_mm_fmadd_ps(b2, sz, acc));
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

// The body in single precision, one row of the output per vector. Every output element is a three-term chain,
//   right / up / at rows:  (b.right * a.row.x)  then  fma(b.up, a.row.y)  then  fma(b.at, a.row.z)
//   pos row:               fma(b.right, a.pos.x, b.pos)  then  fma(b.up, a.pos.y)  then  fma(b.at, a.pos.z)
// (the ordered body's ps_muls / ps_madds halves: x and y through its first pass, z through the second), here as float
// lanes: a product is one rounding, a fused step is one rounding (_mm_fmadd_ps), which is what ppc::fps_mul / fps_fused
// return when the operation stays on their fast path (a fused double repaired at a single-precision midpoint is the
// once-rounded value, the same one). The second pass also leaves a sum in ps1 of f8 / f11 / f12 / f13 (1.0 times the a
// row's components, added one after the other); it is computed too, because those registers are left as the body
// leaves them.
// When no operand and no intermediate or final value is tiny (0 < |v| <= 2^-125: the helpers' slow-path range, where
// the non-IEEE flush bit matters) and no output is a NaN (every NaN operand makes one), the helpers change no FPSCR
// bit, so the order of the operations is not observable and the result is the ordered body's. Every input is read
// before anything is written; an output that overlaps an input or the stack frame, or a call off that range, returns
// false having written nothing.
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
    const __m128i ai0 = load_row(c, a), ai1 = load_row(c, a + 0x10u), ai2 = load_row(c, a + 0x20u),
                  ai3 = load_row(c, a + 0x30u);
    const __m128i bi0 = load_row(c, b), bi1 = load_row(c, b + 0x10u), bi2 = load_row(c, b + 0x20u),
                  bi3 = load_row(c, b + 0x30u);
    // Tiny operands (lane 3, the flags word, does not count).
    __m128i key = _mm_min_epu32(_mm_min_epu32(tiny_key(ai0), tiny_key(ai1)), _mm_min_epu32(tiny_key(ai2), tiny_key(ai3)));
    key = _mm_min_epu32(key, _mm_min_epu32(_mm_min_epu32(tiny_key(bi0), tiny_key(bi1)),
                                           _mm_min_epu32(tiny_key(bi2), tiny_key(bi3))));
    if (any_tiny(key, 7)) {
        ++off_fast_path_;
        return false;
    }
    // b's rows with the flags word zeroed (a flags word read as a float is usually a denormal: no arithmetic on it).
    const __m128 zero = _mm_setzero_ps();
    const __m128 b0 = _mm_blend_ps(_mm_castsi128_ps(bi0), zero, 8), b1 = _mm_blend_ps(_mm_castsi128_ps(bi1), zero, 8),
                 b2 = _mm_blend_ps(_mm_castsi128_ps(bi2), zero, 8), b3 = _mm_blend_ps(_mm_castsi128_ps(bi3), zero, 8);
    // a transposed: ax = (right.x, up.x, at.x, pos.x), ay and az likewise (aw = the flags words).
    __m128 ax = _mm_castsi128_ps(ai0), ay = _mm_castsi128_ps(ai1), az = _mm_castsi128_ps(ai2), aw = _mm_castsi128_ps(ai3);
    _MM_TRANSPOSE4_PS(ax, ay, az, aw);
    (void)aw;
    __m128i chain_key = _mm_set1_epi32(-1);
    const auto step = [&chain_key](__m128 value) {
        chain_key = _mm_min_epu32(chain_key, tiny_key(value));
        return value;
    };
    __m128 row[4];
    row[0] = chain<0, false>(b0, b1, b2, zero, ax, ay, az, step);
    row[1] = chain<1, false>(b0, b1, b2, zero, ax, ay, az, step);
    row[2] = chain<2, false>(b0, b1, b2, zero, ax, ay, az, step);
    row[3] = chain<3, true>(b0, b1, b2, b3, ax, ay, az, step);
    // The second pass's ps1 sums: right / up / at: (z + (y + x)); pos: (z + (y + (x + 1.0))).
    const __m128 x_plus_one = _mm_blend_ps(ax, _mm_add_ps(ax, _mm_set1_ps(1.0f)), 8);
    const __m128 sum_yx = step(_mm_add_ps(ay, x_plus_one));
    const __m128 sum = step(_mm_add_ps(az, sum_yx));
    if (any_tiny(chain_key, 15)) {
        ++off_fast_path_;
        return false;
    }
    // A NaN anywhere reaches an output or a sum.
    const __m128 unordered = _mm_or_ps(_mm_or_ps(_mm_cmpunord_ps(row[0], row[0]), _mm_cmpunord_ps(row[1], row[1])),
                                       _mm_or_ps(_mm_cmpunord_ps(row[2], row[2]), _mm_cmpunord_ps(row[3], row[3])));
    if ((_mm_movemask_ps(unordered) & 7) != 0 || (_mm_movemask_ps(_mm_cmpunord_ps(sum, sum)) & 15) != 0) {
        ++off_fast_path_;
        return false;
    }
    alignas(16) float r[4][4];
    alignas(16) float j[4];
    alignas(16) float xs[4], ys[4], zs[4], up_z[4], at_z[4];
    for (u32 i = 0; i < 4u; ++i)
        _mm_store_ps(r[i], row[i]);
    _mm_store_ps(j, sum);
    _mm_store_ps(xs, ax);
    _mm_store_ps(ys, ay);
    _mm_store_ps(zs, az);
    _mm_store_ps(up_z, b1);
    _mm_store_ps(at_z, b2);

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
    // The output: three words per row; the padding words untouched, the flags word last.
    for (u32 row_index = 0; row_index < 4u; ++row_index)
        for (u32 k = 0; k < 3u; ++k)
            c.mem_write32(out + 0x10u * row_index + 4u * k, float_to_bits(r[row_index][k]));
    const u32 r0 = c.mem_read32(a + 0x0Cu);
    const u32 r4 = r0 & c.mem_read32(b + 0x0Cu);
    c.mem_write32(out + 0x0Cu, r4);

    // The registers as the body leaves them (singles held as doubles).
    const auto set = [&c](u32 n, float p0, float p1) {
        c.fpr[n].bits[0] = double_to_bits(static_cast<double>(p0));
        c.fpr[n].bits[1] = double_to_bits(static_cast<double>(p1));
    };
    set(0u, xs[0], ys[0]);
    set(1u, zs[0], 1.0f);
    set(2u, xs[1], ys[1]);
    set(3u, zs[1], 1.0f);
    set(4u, xs[2], ys[2]);
    set(5u, zs[2], 1.0f);
    set(6u, xs[3], ys[3]);
    set(7u, zs[3], 1.0f);
    set(8u, r[2][2], j[2]);
    set(9u, up_z[2], 1.0f);
    set(10u, at_z[2], 1.0f);
    set(11u, r[3][2], j[3]);
    set(12u, r[0][2], j[0]);
    set(13u, r[1][2], j[1]);
    c.fpr[30].bits[0] = f30_0;
    c.fpr[30].bits[1] = single_to_double_bits(f30_s1);
    c.fpr[31].bits[0] = f31_0;
    c.fpr[31].bits[1] = single_to_double_bits(f31_s1);
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
