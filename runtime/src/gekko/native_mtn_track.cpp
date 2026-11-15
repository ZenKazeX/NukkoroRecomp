// The Mtn track sampler, seeks, initialiser and Hermite interpolators, native. Contract: include/gekko/native_mtn_track.h.

#include "gekko/native_mtn_track.h"

#include "gekko/cpu.h"
#include "gekko/gekko_estimate.h"
#include "gekko/native_guest_call.h"
#include "gekko/ppc_ops.h"

namespace gekko::mtn_track {
namespace {
using F = u64;  // an FPR as the original holds it: the double bits of a single value

constexpr u32 kEps = 0x38D1B717u;    // [r2 + 0x16E0] = 9.99999975e-05 (.sdata2)
constexpr u32 kZero = 0x00000000u;   // [r2 + 0x16D0], [r2 + 0x16F8]
constexpr u32 kOne = 0x3F800000u;    // [r2 + 0x16E8], [r2 + 0x1700]
constexpr u32 kTwo = 0x40000000u;    // [r2 + 0x16EC]
constexpr u32 kThree = 0x40400000u;  // [r2 + 0x16F0]
constexpr u32 kSix = 0x40C00000u;    // [r2 + 0x16F4]
constexpr u32 kCtxTime = kContext + 0x0Cu;
constexpr u32 kCtxRate = kContext + 0x08u;
constexpr u32 kCtxInterp = kContext + 0x14u;
// The instruction after the sampler's bctrl: guest_call's never-fetched return address (a value, not an entry).
constexpr u32 kLink = 0x80416338u;  // liveness-roots: value-only
constexpr u64 kSign = 0x8000000000000000ull;

GEKKO_ALWAYS_INLINE F L(u32 word) noexcept { return single_to_double_bits(word); }
GEKKO_ALWAYS_INLINE F LM(Cpu& c, u32 address) { return single_to_double_bits(c.mem_read32(address)); }
GEKKO_ALWAYS_INLINE void SM(Cpu& c, u32 address, F value) { c.mem_write32(address, double_to_single_bits(value)); }

// fcmpo cr0, a, b -> "LT": the NaN case raises the original's exception bits and is not less (bge is then taken).
GEKKO_ALWAYS_INLINE bool less_ordered(Cpu& c, F a, F b) noexcept {
    const double da = bits_to_double(a), db = bits_to_double(b);
    if (da < db) return true;
    if (da >= db) return false;
    ppc::fp_compare_unordered(c, a, b, true);
    return false;
}
// fcmpo cr0, a, b -> "GT" (ble / bgt after it): a NaN is not greater.
GEKKO_ALWAYS_INLINE bool greater_ordered(Cpu& c, F a, F b) noexcept {
    const double da = bits_to_double(a), db = bits_to_double(b);
    if (da > db) return true;
    if (da <= db) return false;
    ppc::fp_compare_unordered(c, a, b, true);
    return false;
}
// fcmpu cr0, a, b -> "EQ" (bne is taken when it is not equal, a NaN included).
GEKKO_ALWAYS_INLINE bool equal_unordered(Cpu& c, F a, F b) noexcept {
    const double da = bits_to_double(a), db = bits_to_double(b);
    if (da == db) return true;
    if (da < db || da > db) return false;
    ppc::fp_compare_unordered(c, a, b, false);
    return false;
}

// 16-bit packed value: 0 is +0.0; otherwise bit placement only.
GEKKO_ALWAYS_INLINE u32 half_bits(u32 h) noexcept {
    return h == 0u ? 0u : ((h & 0x8000u) << 16) | (((h & 0x7FFFu) << 13) + 0x38000000u);
}

// Decodes the packed key at `stream` into the four floats at `key` (time, in-tangent, out-tangent, value) in the original's store
// order. Returns the header word; `value_at` receives the address of the value word (after the out-tangent word, when there is
// one), which the caller steps over or not as the original does.
GEKKO_ALWAYS_INLINE u32 decode_key(Cpu& c, u32 stream, u32 key, u32& value_at) {
    u32 p = stream + 4u;
    const u32 h = c.mem_read16(stream);
    c.mem_write32(key, float_to_bits(static_cast<float>(h & 0x3FFFu)));
    c.mem_write32(key + 4u, half_bits(c.mem_read16(stream + 2u)));
    u32 out_bits;
    if ((h & 0x8000u) != 0u) {
        out_bits = c.mem_read32(key + 4u);  // lfs of the stored in-tangent
    } else {
        out_bits = half_bits(c.mem_read16(p));
        p += 2u;
    }
    c.mem_write32(key + 8u, out_bits);
    c.mem_write32(key + 0xCu, half_bits(c.mem_read16(p)));
    value_at = p;
    return h;
}

GEKKO_ALWAYS_INLINE void copy_key(Cpu& c, u32 dst, u32 src) {  // lfs / stfs pairs: bit-exact word copies
    const u32 w0 = c.mem_read32(src);
    c.mem_write32(dst, w0);
    const u32 w1 = c.mem_read32(src + 4u);
    c.mem_write32(dst + 4u, w1);
    const u32 w2 = c.mem_read32(src + 8u);
    c.mem_write32(dst + 8u, w2);
    const u32 w3 = c.mem_read32(src + 0xCu);
    c.mem_write32(dst + 0xCu, w3);
}

// ---- the interpolators (the original's operation order, register roles in comments) ----------------------------------------
F hermite_value_exact(Cpu& c, u32 a, u32 b, F t) noexcept {
    const F a_time = LM(c, a), b_time = LM(c, b);
    const F s = ppc::fps_sub(c, t, a_time);                    // f9 = t - A.time
    const F one = L(kOne);
    const F dt = ppc::fps_sub(c, b_time, a_time);              // f1
    const F s2 = ppc::fps_mul(c, s, s);                        // f8
    const F inv = ppc::fps_div(c, one, dt);                    // f12
    const F b_val = LM(c, b + 0xCu), a_val = LM(c, a + 0xCu), a_out = LM(c, a + 8u), b_in = LM(c, b + 4u);
    const F inv2 = ppc::fps_mul(c, inv, inv);                  // f10
    F f4 = ppc::fps_mul(c, s2, s);
    F f0 = ppc::fps_mul(c, L(kThree), s2);
    const F f11 = ppc::fps_mul(c, s2, inv);
    f4 = ppc::fps_mul(c, inv2, f4);
    const F f10 = ppc::fps_mul(c, f0, inv2);
    f0 = ppc::fps_mul(c, L(kTwo), f4);
    const F f8 = ppc::fps_sub(c, f4, f11);
    F f2 = ppc::fps_mul(c, f0, inv);
    f4 = ppc::fps_sub(c, f8, f11);
    f0 = f2 ^ kSign;                                           // fneg
    f2 = ppc::fps_sub(c, f2, f10);
    f4 = ppc::fps_add(c, s, f4);
    f0 = ppc::fps_add(c, f0, f10);
    f2 = ppc::fps_add(c, one, f2);
    f0 = ppc::fps_mul(c, b_val, f0);
    f0 = ppc::fps_madd(c, a_val, f2, f0);
    f0 = ppc::fps_madd(c, a_out, f4, f0);
    return ppc::fps_madd(c, b_in, f8, f0);
}

F hermite_slope_exact(Cpu& c, u32 a, u32 b, F t) noexcept {
    const F a_time = LM(c, a), b_time = LM(c, b);
    const F s = ppc::fps_sub(c, t, a_time);                    // f9
    const F six = L(kSix);                                     // f4
    const F dt = ppc::fps_sub(c, b_time, a_time);              // f0
    const F one = L(kOne);                                     // f8
    const F two = L(kTwo);                                     // f2
    const F s2 = ppc::fps_mul(c, s, s);                        // f10
    const F inv = ppc::fps_div(c, one, dt);                    // f11
    const F three = L(kThree);                                 // f0
    const F b_val = LM(c, b + 0xCu), a_val = LM(c, a + 0xCu), a_out = LM(c, a + 8u), b_in = LM(c, b + 4u);
    const F inv2 = ppc::fps_mul(c, inv, inv);                  // f12
    F f7 = ppc::fps_mul(c, six, s2);
    const F f4a = ppc::fps_mul(c, six, s);
    const F f2a = ppc::fps_mul(c, two, s);
    f7 = ppc::fps_mul(c, f7, inv2);
    const F f0a = ppc::fps_mul(c, three, s2);
    const F f9 = ppc::fps_mul(c, f4a, inv2);
    f7 = ppc::fps_mul(c, inv, f7);
    const F f10 = ppc::fps_mul(c, f2a, inv);
    const F f4b = ppc::fps_mul(c, f0a, inv2);
    F f0 = f7 ^ kSign;                                         // fneg
    const F f2 = ppc::fps_sub(c, f7, f9);
    const F f7b = ppc::fps_sub(c, f4b, f10);
    f0 = ppc::fps_add(c, f0, f9);
    const F f4c = ppc::fps_sub(c, f7b, f10);
    f0 = ppc::fps_mul(c, b_val, f0);
    const F f1 = ppc::fps_add(c, one, f4c);
    f0 = ppc::fps_madd(c, a_val, f2, f0);
    f0 = ppc::fps_madd(c, a_out, f1, f0);
    return ppc::fps_madd(c, b_in, f7b, f0);
}

// ---- the fast path of the interpolators ---------------------------------------------------------------------------------------
// The translated code's fps_* never touch FPSCR in arithmetic (fp_arith_slow takes the FPSCR by value): an operation is a value
// function. For operands and results that are neither NaN nor tiny (a nonzero magnitude below 2^-125) that value is the correctly
// rounded single result, i.e. plain float arithmetic in the same operation order (a fused multiply-add as fmaf: its one midpoint
// case is resolved by the slow path to the same correctly rounded value). Whenever any operand, any intermediate or the result is
// NaN or (conservatively) has a nonzero magnitude below 2^-100, the whole evaluation is redone through ppc::fps_* (the exact path
// above). The tiny test is a superset of the original's slow-path trigger, so it can only send more calls to the exact path.
GEKKO_ALWAYS_INLINE bool tiny_or_nan(float x) noexcept {
    const u32 m = float_to_bits(x) & 0x7FFFFFFFu;
    return (m - 1u) < 0x0D7FFFFFu || m > 0x7F800000u;  // 0 < |x| < 2^-100, or NaN
}
#if defined(__AVX2__) || defined(__FMA__)
GEKKO_ALWAYS_INLINE float fmaf1(float a, float c, float b) noexcept {
    return _mm_cvtss_f32(_mm_fmadd_ss(_mm_set_ss(a), _mm_set_ss(c), _mm_set_ss(b)));
}
#else
GEKKO_ALWAYS_INLINE float fmaf1(float a, float c, float b) noexcept { return std::fma(a, c, b); }
#endif

// Returns false (nothing usable) when the exact path has to run.
bool hermite_value_fast(Cpu& c, u32 a, u32 b, float t, float& out) noexcept {
    const float a_time = bits_to_float(c.mem_read32(a)), b_time = bits_to_float(c.mem_read32(b));
    const float b_val = bits_to_float(c.mem_read32(b + 0xCu)), a_val = bits_to_float(c.mem_read32(a + 0xCu));
    const float a_out = bits_to_float(c.mem_read32(a + 8u)), b_in = bits_to_float(c.mem_read32(b + 4u));
    bool bad = tiny_or_nan(a_time) | tiny_or_nan(b_time) | tiny_or_nan(b_val) | tiny_or_nan(a_val) | tiny_or_nan(a_out) |
               tiny_or_nan(b_in) | tiny_or_nan(t);
    const float s = t - a_time;                  bad |= tiny_or_nan(s);
    const float dt = b_time - a_time;            bad |= tiny_or_nan(dt);
    const float s2 = s * s;                      bad |= tiny_or_nan(s2);
    const float inv = 1.0f / dt;                 bad |= tiny_or_nan(inv);
    const float inv2 = inv * inv;                bad |= tiny_or_nan(inv2);
    float f4 = s2 * s;                           bad |= tiny_or_nan(f4);
    float f0 = 3.0f * s2;                        bad |= tiny_or_nan(f0);
    const float f11 = s2 * inv;                  bad |= tiny_or_nan(f11);
    f4 = inv2 * f4;                              bad |= tiny_or_nan(f4);
    const float f10 = f0 * inv2;                 bad |= tiny_or_nan(f10);
    f0 = 2.0f * f4;                              bad |= tiny_or_nan(f0);
    const float f8 = f4 - f11;                   bad |= tiny_or_nan(f8);
    float f2 = f0 * inv;                         bad |= tiny_or_nan(f2);
    f4 = f8 - f11;                               bad |= tiny_or_nan(f4);
    f0 = -f2;
    f2 = f2 - f10;                               bad |= tiny_or_nan(f2);
    f4 = s + f4;                                 bad |= tiny_or_nan(f4);
    f0 = f0 + f10;                               bad |= tiny_or_nan(f0);
    f2 = 1.0f + f2;                              bad |= tiny_or_nan(f2);
    f0 = b_val * f0;                             bad |= tiny_or_nan(f0);
    f0 = fmaf1(a_val, f2, f0);                   bad |= tiny_or_nan(f0);
    f0 = fmaf1(a_out, f4, f0);                   bad |= tiny_or_nan(f0);
    const float r = fmaf1(b_in, f8, f0);         bad |= tiny_or_nan(r);
    out = r;
    return !bad;
}

bool hermite_slope_fast(Cpu& c, u32 a, u32 b, float t, float& out) noexcept {
    const float a_time = bits_to_float(c.mem_read32(a)), b_time = bits_to_float(c.mem_read32(b));
    const float b_val = bits_to_float(c.mem_read32(b + 0xCu)), a_val = bits_to_float(c.mem_read32(a + 0xCu));
    const float a_out = bits_to_float(c.mem_read32(a + 8u)), b_in = bits_to_float(c.mem_read32(b + 4u));
    bool bad = tiny_or_nan(a_time) | tiny_or_nan(b_time) | tiny_or_nan(b_val) | tiny_or_nan(a_val) | tiny_or_nan(a_out) |
               tiny_or_nan(b_in) | tiny_or_nan(t);
    const float s = t - a_time;                  bad |= tiny_or_nan(s);
    const float dt = b_time - a_time;            bad |= tiny_or_nan(dt);
    const float s2 = s * s;                      bad |= tiny_or_nan(s2);
    const float inv = 1.0f / dt;                 bad |= tiny_or_nan(inv);
    const float inv2 = inv * inv;                bad |= tiny_or_nan(inv2);
    float f7 = 6.0f * s2;                        bad |= tiny_or_nan(f7);
    const float f4a = 6.0f * s;                  bad |= tiny_or_nan(f4a);
    const float f2a = 2.0f * s;                  bad |= tiny_or_nan(f2a);
    f7 = f7 * inv2;                              bad |= tiny_or_nan(f7);
    const float f0a = 3.0f * s2;                 bad |= tiny_or_nan(f0a);
    const float f9 = f4a * inv2;                 bad |= tiny_or_nan(f9);
    f7 = inv * f7;                               bad |= tiny_or_nan(f7);
    const float f10 = f2a * inv;                 bad |= tiny_or_nan(f10);
    const float f4b = f0a * inv2;                bad |= tiny_or_nan(f4b);
    float f0 = -f7;
    const float f2 = f7 - f9;                    bad |= tiny_or_nan(f2);
    const float f7b = f4b - f10;                 bad |= tiny_or_nan(f7b);
    f0 = f0 + f9;                                bad |= tiny_or_nan(f0);
    const float f4c = f7b - f10;                 bad |= tiny_or_nan(f4c);
    f0 = b_val * f0;                             bad |= tiny_or_nan(f0);
    const float f1 = 1.0f + f4c;                 bad |= tiny_or_nan(f1);
    f0 = fmaf1(a_val, f2, f0);                   bad |= tiny_or_nan(f0);
    f0 = fmaf1(a_out, f1, f0);                   bad |= tiny_or_nan(f0);
    const float r = fmaf1(b_in, f7b, f0);        bad |= tiny_or_nan(r);
    out = r;
    return !bad;
}

// The fast path serves a time that is a single value (every caller passes an lfs result); anything else is the exact path.
GEKKO_ALWAYS_INLINE bool single_value(F t, float& out) noexcept {
    out = static_cast<float>(bits_to_double(t));
    return static_cast<double>(out) == bits_to_double(t);
}
F hermite_value(Cpu& c, u32 a, u32 b, F t) noexcept {
    float tf, r;
    if (single_value(t, tf) && hermite_value_fast(c, a, b, tf, r)) return single_to_double_bits(float_to_bits(r));
    return hermite_value_exact(c, a, b, t);
}
F hermite_slope(Cpu& c, u32 a, u32 b, F t) noexcept {
    float tf, r;
    if (single_value(t, tf) && hermite_slope_fast(c, a, b, tf, r)) return single_to_double_bits(float_to_bits(r));
    return hermite_slope_exact(c, a, b, t);
}

// The call through the context's interpolator pointer (the value interpolator at +0x14).
F interpolate(Cpu& c, u32 a, u32 b, F t) {
    const u32 target = c.mem_read32(kCtxInterp);
    if (target == kHermiteValue) return hermite_value(c, a, b, t);
    if (target == kHermiteSlope) return hermite_slope(c, a, b, t);
    c.fpr[1].bits[0] = t;
    u32 result = 0;
    if (!guest_call(c, target, kLink, {a, b}, result)) return t;  // the run is stopped
    return c.fpr[1].bits[0];
}

// ---- 0x804162AC ----------------------------------------------------------------------------------------------------------------
F track_sample(Cpu& c, u32 st) {
    const s32 count = static_cast<s16>(c.mem_read16(st + 2u));
    s32 index = static_cast<s16>(c.mem_read16(st));
    u32 flags = c.mem_read32(st + 4u);
    u32 stream = c.mem_read32(st + 8u);
    if ((flags & 1u) != 0u) return LM(c, st + 0x10u);
    const u32 a = st + 0x20u, b = st + 0x30u;
    const F eps = L(kEps);
    const F t = LM(c, kCtxTime);
    if (less_ordered(c, t, ppc::fps_add(c, eps, LM(c, a)))) return LM(c, a + 0xCu);
    const F b_time0 = LM(c, b);
    if (less_ordered(c, t, ppc::fps_sub(c, b_time0, eps))) {
        if ((flags & 8u) != 0u) return LM(c, st + 0x10u);
        return interpolate(c, a, b, t);
    }
    if (less_ordered(c, t, ppc::fps_add(c, eps, b_time0))) return LM(c, b + 0xCu);

    // advance: the loop of 0x80416354 .. 0x80416520
    bool in_window = false;  // left the loop through "t < B.time + eps"
    while (index < count) {
        copy_key(c, a, b);
        u32 value_at;
        decode_key(c, stream, b, value_at);
        stream = value_at + 2u;
        index = static_cast<s16>(index + 1);  // r7 + 1; the loop test extends the low half word
        if (less_ordered(c, t, ppc::fps_add(c, eps, LM(c, b)))) {
            in_window = true;
            break;
        }
    }

    c.mem_write16(st, static_cast<u16>(index));
    c.mem_write32(st + 8u, stream);
    if (in_window) {
        c.mem_write32(st + 4u, c.mem_read32(st + 4u) & ~1u);
        const F b_time = LM(c, b);
        if (!less_ordered(c, t, ppc::fps_sub(c, b_time, eps))) return LM(c, b + 0xCu);
        const F zero = L(kZero);
        if (equal_unordered(c, zero, LM(c, a + 8u)) && equal_unordered(c, zero, LM(c, b + 4u)) &&
            equal_unordered(c, LM(c, a + 0xCu), LM(c, b + 0xCu))) {
            c.mem_write32(st + 4u, c.mem_read32(st + 4u) | 8u);
            const F value = LM(c, b + 0xCu);
            SM(c, st + 0x10u, value);
            return value;
        }
        c.mem_write32(st + 4u, c.mem_read32(st + 4u) & ~8u);
        return interpolate(c, a, b, LM(c, kCtxTime));
    }

    // the stream is exhausted
    const F b_out = LM(c, b + 8u);
    if (equal_unordered(c, L(kZero), b_out)) {
        c.mem_write32(st + 4u, c.mem_read32(st + 4u) | 1u);
        const F value = LM(c, b + 0xCu);
        SM(c, st + 0x10u, value);
        return value;
    }
    const F dt = ppc::fps_sub(c, LM(c, kCtxTime), LM(c, b));
    return ppc::fps_madd(c, b_out, dt, LM(c, b + 0xCu));
}

// ---- 0x80415300: advance the track to the context time (forward) ----------------------------------------------------------------
void seek_forward(Cpu& c, u32 st) {
    const F eps = L(kEps);
    const F t = LM(c, kCtxTime);
    const u32 a = st + 0x20u, b = st + 0x30u;
    if (less_ordered(c, t, ppc::fps_add(c, eps, LM(c, b)))) return;  // nothing to do: no store
    u32 stream = c.mem_read32(st + 8u);
    s32 index = static_cast<s16>(c.mem_read16(st));
    const s32 count = static_cast<s16>(c.mem_read16(st + 2u));
    while (index < count) {
        copy_key(c, a, b);
        u32 value_at;
        decode_key(c, stream, b, value_at);
        stream = value_at + 2u;
        index = static_cast<s16>(index + 1);
        if (less_ordered(c, t, ppc::fps_add(c, eps, LM(c, b)))) break;
    }
    c.mem_write16(st, static_cast<u16>(index));
    c.mem_write32(st + 8u, stream);
}

// ---- 0x80415170: step the track back to the context time ------------------------------------------------------------------------
void seek_backward(Cpu& c, u32 st) {
    const F eps = L(kEps);
    const u32 a = st + 0x20u, b = st + 0x30u;
    const F t0 = LM(c, kCtxTime);
    if (greater_ordered(c, t0, ppc::fps_sub(c, LM(c, a), eps))) return;  // ble not taken: leave, no store
    u32 stream = c.mem_read32(st + 8u);
    s32 index = static_cast<s16>(c.mem_read16(st));
    while (index > -1) {
        copy_key(c, b, a);
        u32 value_at;
        const u32 h = decode_key(c, stream, a, value_at);
        index = static_cast<s16>(index - 1);
        stream -= ((h >> 14) & 1u) != 0u ? 6u : 8u;
        const F f0 = ppc::fps_sub(c, LM(c, a), eps);
        if (greater_ordered(c, LM(c, kCtxTime), f0)) break;
    }
    c.mem_write16(st, static_cast<u16>(index));
    c.mem_write32(st + 8u, stream);
}

// ---- 0x80414B80: reset a track to its first (or last) key, then seek to the context time ----------------------------------------
void track_init(Cpu& c, u32 st) {
    const u32 rec = c.mem_read32(st + 0xCu);
    c.mem_write32(st + 4u, 0u);
    if (less_ordered(c, LM(c, kCtxRate), L(kZero))) {
        // reverse playback: start at the last key of the record
        const s32 count = static_cast<s16>(c.mem_read16(st + 2u));
        c.mem_write16(st, static_cast<u16>(count - 1));
        const u32 first_header = c.mem_read16(rec + 8u);
        const s32 tail = static_cast<s8>(c.mem_read8(rec + 6u));
        const u32 size = c.mem_read32(rec);
        const u32 key_bytes = ((first_header >> 14) & 1u) != 0u ? 6u : 8u;
        c.mem_write32(st + 8u, rec + size - static_cast<u32>(tail) - key_bytes);
        const u32 stream = c.mem_read32(st + 8u);
        u32 value_at;
        const u32 h = decode_key(c, stream, st + 0x30u, value_at);
        c.mem_write32(st + 8u, c.mem_read32(st + 8u) - (((h >> 14) & 1u) != 0u ? 6u : 8u));
        copy_key(c, st + 0x20u, st + 0x30u);
        c.mem_write16(st, static_cast<u16>(static_cast<s16>(c.mem_read16(st)) - 1));
        seek_backward(c, st);
        return;
    }
    c.mem_write16(st, 0u);
    c.mem_write32(st + 8u, rec + 8u);
    u32 value_at;
    decode_key(c, rec + 8u, st + 0x20u, value_at);
    c.mem_write32(st + 8u, value_at + 2u);
    copy_key(c, st + 0x30u, st + 0x20u);
    c.mem_write16(st, static_cast<u16>(static_cast<s16>(c.mem_read16(st)) + 1));
    seek_forward(c, st);
}

// ---- the typed samplers: three tracks sampled into a vector -----------------------------------------------------------------------
// The sampling function is whatever the context set up at [r13 + 0x5F78]: the forward sampler is served inline, any other
// (the reverse and slope samplers) is called as guest code from the original function's return point `link`.
F sample_through_pointer(Cpu& c, u32 track, u32 link) {
    const u32 target = c.mem_read32(c.gpr[13] + 0x5F78u);
    if (target == kTrackSample) return track_sample(c, track);
    u32 result = 0;
    if (!guest_call(c, target, link, {track}, result)) return c.fpr[1].bits[0];  // the run is stopped
    return c.fpr[1].bits[0];
}

// out[i] = sample(track i) for every track whose stream pointer is set; the tracks start at obj + base, 0x40 apart.
template <u32 Link>
void sample_vec3(Cpu& c, u32 obj, u32 base, bool reciprocal) {
    const u32 tracks = obj + base;
    for (u32 i = 0; i < 3; ++i) {
        const u32 track = tracks + 0x40u * i;
        if (c.mem_read32(track + 8u) == 0u) continue;
        F f1 = sample_through_pointer(c, track, Link);
        c.fpr[1].bits[0] = f1;
        if (reciprocal) f1 = ppc::fps_div(c, L(kOne), f1);
        SM(c, obj + 4u * i, f1);
    }
}

// The vector samplers' return addresses after their sampler calls (values handed to guest_call, never entries).
constexpr u32 kLinkVec3Reciprocal = 0x8042138Cu;  // liveness-roots: value-only
constexpr u32 kLinkVec3 = 0x80421430u;            // liveness-roots: value-only
constexpr u32 kLinkVec3At20 = 0x804214C0u;        // liveness-roots: value-only
constexpr u32 kLinkVec3Scaled = 0x8042154Cu;      // liveness-roots: value-only
}  // namespace

void serve_track_sample(Cpu& cpu) { cpu.fpr[1].bits[0] = track_sample(cpu, cpu.gpr[3]); }
void serve_hermite_value(Cpu& cpu) noexcept { cpu.fpr[1].bits[0] = hermite_value(cpu, cpu.gpr[3], cpu.gpr[4], cpu.fpr[1].bits[0]); }
void serve_hermite_slope(Cpu& cpu) noexcept { cpu.fpr[1].bits[0] = hermite_slope(cpu, cpu.gpr[3], cpu.gpr[4], cpu.fpr[1].bits[0]); }
void serve_seek_forward(Cpu& cpu) { seek_forward(cpu, cpu.gpr[3]); }
void serve_seek_backward(Cpu& cpu) { seek_backward(cpu, cpu.gpr[3]); }
void serve_track_init(Cpu& cpu) { track_init(cpu, cpu.gpr[3]); }
void state_init(Cpu& cpu, u32 state, u32 record) {
    cpu.mem_write32(state + 4u, 0u);
    cpu.mem_write16(state + 2u, static_cast<u16>(cpu.mem_read16(record + 4u)));
    cpu.mem_write32(state + 0xCu, record);
    track_init(cpu, state);
}
void serve_sample_vec3_reciprocal(Cpu& cpu) { sample_vec3<kLinkVec3Reciprocal>(cpu, cpu.gpr[3], 0x10u, true); }
void serve_sample_vec3(Cpu& cpu) { sample_vec3<kLinkVec3>(cpu, cpu.gpr[3], 0x10u, false); }
void serve_sample_vec3_at_20(Cpu& cpu) { sample_vec3<kLinkVec3At20>(cpu, cpu.gpr[3], 0x20u, false); }
void serve_sample_vec3_scaled(Cpu& cpu) {
    const u32 obj = cpu.gpr[3];
    sample_vec3<kLinkVec3Scaled>(cpu, obj, 0x20u, false);
    const F scale = LM(cpu, obj + 0x10u);
    if (greater_ordered(cpu, scale, L(kZero))) {
        SM(cpu, obj, ppc::fps_mul(cpu, LM(cpu, obj), scale));
        SM(cpu, obj + 4u, ppc::fps_mul(cpu, LM(cpu, obj + 4u), LM(cpu, obj + 0x10u)));
        SM(cpu, obj + 8u, ppc::fps_mul(cpu, LM(cpu, obj + 8u), LM(cpu, obj + 0x10u)));
    }
}

void install(Cpu& cpu) {
    cpu.install_hle(kTrackSample, [](Cpu& c) { serve_track_sample(c); return true; }, "Mtn TrackSample (native)");
    cpu.install_hle(kHermiteValue, [](Cpu& c) { serve_hermite_value(c); return true; }, "Mtn HermiteValue (native)");
    cpu.install_hle(kHermiteSlope, [](Cpu& c) { serve_hermite_slope(c); return true; }, "Mtn HermiteSlope (native)");
    cpu.install_hle(kSeekForward, [](Cpu& c) { serve_seek_forward(c); return true; }, "Mtn TrackSeekForward (native)");
    cpu.install_hle(kSeekBackward, [](Cpu& c) { serve_seek_backward(c); return true; }, "Mtn TrackSeekBackward (native)");
    cpu.install_hle(kTrackInit, [](Cpu& c) { serve_track_init(c); return true; }, "Mtn TrackInit (native)");
    cpu.install_hle(kSampleVec3Reciprocal, [](Cpu& c) { serve_sample_vec3_reciprocal(c); return true; },
                    "Mtn SampleVec3Reciprocal (native)");
    cpu.install_hle(kSampleVec3, [](Cpu& c) { serve_sample_vec3(c); return true; }, "Mtn SampleVec3 (native)");
    cpu.install_hle(kSampleVec3At20, [](Cpu& c) { serve_sample_vec3_at_20(c); return true; }, "Mtn SampleVec3At20 (native)");
    cpu.install_hle(kSampleVec3Scaled, [](Cpu& c) { serve_sample_vec3_scaled(c); return true; }, "Mtn SampleVec3Scaled (native)");
}

}  // namespace gekko::mtn_track
