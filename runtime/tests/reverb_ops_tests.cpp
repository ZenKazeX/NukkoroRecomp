// Contract of the reverb's clean vector ops (runtime/lift/audio/reverb_ops.h): for operands that are flush-fixed
// already, add_clean / mul_clean / fused_clean give the same bits as the reference ops (operand flush + double
// precision + one rounding) in both modes, and the vector round-to-integer of the crossfeed equals the scalar
// `nearest` rule. Inputs: random values across the exponent range, zeros of both signs, infinities, NaNs, and crafted
// operands whose results land on the smallest-normal boundary (the one case where the float-domain flush decision and
// the double-domain one can differ). A check that cannot fail is no check: three deliberately wrong variants (a
// multiply that skips the boundary redecision, an add without the result flush, a rounding that truncates) must each
// be caught.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#if !defined(__AVX2__)
int main() {
    std::puts("reverb_ops_tests: skipped (no AVX2)");
    return 0;
}
#else
#include "audio/reverb_ops.h"

namespace {
namespace vec = shadow::audio::vec;
using shadow::math::SubnormalMode;
using vec::V;

constexpr float kMin = 0x1p-126f;

float from_bits(std::uint32_t bits) { float f; std::memcpy(&f, &bits, 4); return f; }
std::uint32_t bits_of(float f) { std::uint32_t b; std::memcpy(&b, &f, 4); return b; }

struct Rng {
    std::uint64_t s = 0x243F6A8885A308D3ull;
    std::uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
};

// A flush-fixed operand: zero, a normal, an infinity or a NaN (never a subnormal).
float clean(float f) { return (bits_of(f) & 0x7FFFFFFFu) < 0x00800000u ? from_bits(bits_of(f) & 0x80000000u) : f; }

float random_clean(Rng& rng) {
    const std::uint64_t r = rng.next();
    switch (r & 15u) {
        case 0: return from_bits(static_cast<std::uint32_t>(r >> 8) & 0x80000000u);          // +-0
        case 1: return (r & 0x100) ? INFINITY : -INFINITY;
        case 2: return from_bits(0x7FC00000u);  // NaN (one payload: which of two NaN operands an op returns is not defined by the arithmetic)
        case 3: case 4: case 5: {  // around the smallest normal, up to a few thousand ulps either side
            const std::uint32_t mant = static_cast<std::uint32_t>(r >> 16) & 0x7FFu;
            const std::uint32_t bits = 0x00800000u + mant;
            return (r & 0x100) ? from_bits(bits) : -from_bits(bits);
        }
        default: {  // any normal exponent, biased to the small half where products and sums reach the boundary
            const std::uint32_t exponent = 1u + static_cast<std::uint32_t>(r >> 24) % ((r & 0x200) ? 254u : 100u);
            const std::uint32_t bits = (exponent << 23) | (static_cast<std::uint32_t>(r >> 40) & 0x7FFFFFu) |
                                       ((r & 0x100) ? 0x80000000u : 0u);
            return from_bits(bits);
        }
    }
}

V make(const float (&v)[4]) { return _mm_setr_ps(v[0], v[1], v[2], v[3]); }
void store(V x, float (&v)[4]) { _mm_storeu_ps(v, x); }

// The wrong variants (negative controls).
template <SubnormalMode M>
V mul_without_boundary(V a, V b) {
    V product = _mm_mul_ps(a, b);
    if constexpr (M != SubnormalMode::Flush) return product;
    const V magnitude = _mm_and_ps(product, vec::abs_mask());
    const V small = _mm_cmplt_ps(magnitude, vec::min_normal());
    return _mm_andnot_ps(_mm_and_ps(small, vec::abs_mask()), product);
}
template <SubnormalMode M>
V add_without_flush(V a, V b) { return _mm_add_ps(a, b); }

// Wrong detector (negative control): misses a result of exactly the smallest normal's magnitude.
V suspect_below_only(V x) {
    const V magnitude = _mm_and_ps(x, vec::abs_mask());
    return _mm_and_ps(_mm_cmplt_ps(magnitude, vec::min_normal()), _mm_cmpneq_ps(magnitude, _mm_setzero_ps()));
}

struct Totals {
    std::uint64_t lanes = 0, mismatches = 0, boundary_lanes = 0, naive_would_differ = 0;
};

template <SubnormalMode M, typename Clean, typename Reference>
void compare(Totals& totals, const float (&a)[4], const float (&b)[4], Clean clean_op, Reference reference_op) {
    float got[4], want[4];
    store(clean_op(make(a), make(b)), got);
    store(reference_op(make(a), make(b)), want);
    for (int lane = 0; lane < 4; ++lane) {
        ++totals.lanes;
        if (std::memcmp(&got[lane], &want[lane], 4) != 0) ++totals.mismatches;
    }
}

// a * b where the exact product sits within a few ulps of the smallest normal.
void crafted_product(Rng& rng, float& a, float& b) {
    a = from_bits(((static_cast<std::uint32_t>(rng.next() % 40u) + 40u) << 23) | (static_cast<std::uint32_t>(rng.next()) & 0x7FFFFFu));
    float target = clean(kMin / a);
    const int steps = static_cast<int>(rng.next() % 9u) - 4;
    for (int k = 0; k < std::abs(steps); ++k) target = std::nextafter(target, steps > 0 ? INFINITY : -INFINITY);
    b = target;
    if (rng.next() & 1u) a = -a;
    if (rng.next() & 1u) b = -b;
}

// Two floats whose exact sum is a small multiple of 2^-149 around the smallest normal.
void crafted_sum(Rng& rng, float& a, float& b) {
    a = from_bits(0x00800000u + (static_cast<std::uint32_t>(rng.next()) & 0xFFFFFu));
    const std::uint32_t mant = static_cast<std::uint32_t>(rng.next()) & 0xFFFFFu;
    b = -from_bits(0x00800000u + mant);
    if (rng.next() & 1u) { a = -a; b = -b; }
}

std::int32_t scalar_nearest(float value, bool flush_mode) {  // the rule of reverb.cpp's nearest<M>
    if (std::isnan(value)) return INT32_MIN;
    if (value >= 0x1p31f) return flush_mode ? INT32_MIN : INT32_MAX;
    if (value <= -0x1p31f) return INT32_MIN;
    const auto integer = static_cast<std::int32_t>(value);
    const float fraction = value - float(integer);
    if (fraction > 0.5f || (fraction == 0.5f && (integer & 1))) return integer + 1;
    if (fraction < -0.5f || (fraction == -0.5f && (integer & 1))) return integer - 1;
    return integer;
}
template <bool Flush>
std::int32_t vector_nearest(float value) {
    const __m128 v = _mm_set1_ps(value);
    __m128i converted = _mm_cvtps_epi32(v);
    if constexpr (!Flush) {
        const __m128 overflow = _mm_cmpge_ps(v, _mm_set1_ps(0x1p31f));
        converted = _mm_blendv_epi8(converted, _mm_set1_epi32(INT32_MAX), _mm_castps_si128(overflow));
    }
    return _mm_cvtsi128_si32(converted);
}
std::int32_t truncating_nearest(float value) {  // wrong variant: truncation
    const __m128i converted = _mm_cvttps_epi32(_mm_set1_ps(value));
    return _mm_cvtsi128_si32(converted);
}

template <SubnormalMode M>
int run_mode(const char* name) {
    Rng rng;
    Totals add, mul, fused, add_wrong, mul_wrong;
    std::uint64_t spec_lanes = 0, spec_bad = 0, spec_mismatch = 0, spec_wrong_missed = 0;
    float a[4], b[4], c[4];
    for (int iteration = 0; iteration < 400000; ++iteration) {
        for (int lane = 0; lane < 4; ++lane) {
            a[lane] = random_clean(rng); b[lane] = random_clean(rng); c[lane] = random_clean(rng);
        }
        const int craft = iteration % 4;
        if (craft == 1) for (int lane = 0; lane < 4; ++lane) { crafted_product(rng, a[lane], b[lane]); c[lane] = 0.0f; }  // c = 0: the fused result is the rounded product
        if (craft == 2) for (int lane = 0; lane < 4; ++lane) crafted_sum(rng, a[lane], b[lane]);
        if (craft == 3) {  // a fused operand set whose result is near the boundary: c cancels the product
            for (int lane = 0; lane < 4; ++lane) {
                crafted_product(rng, a[lane], b[lane]);
                c[lane] = -clean(a[lane] * b[lane]);
                if (rng.next() & 1u) c[lane] = clean(std::nextafter(c[lane], (rng.next() & 1u) ? INFINITY : -INFINITY));
            }
        }
        compare<M>(add, a, b, [](V x, V y) { return vec::add_clean<M>(x, y); }, [](V x, V y) { return vec::add<M>(x, y); });
        compare<M>(mul, a, b, [](V x, V y) { return vec::mul_clean<M>(x, y); }, [](V x, V y) { return vec::mul<M>(x, y); });
        {
            float got[4], want[4];
            store(vec::fused_clean<M>(make(a), make(b), make(c)), got);
            store(vec::fused<M>(make(a), make(b), make(c)), want);
            for (int lane = 0; lane < 4; ++lane) { ++fused.lanes; if (std::memcmp(&got[lane], &want[lane], 4) != 0) ++fused.mismatches; }
        }
        if constexpr (M == SubnormalMode::Flush) {
            // Speculation: where the raw (unflushed) result is not suspect it must equal the clean op's result.
            float raw_fused[4], clean_fused[4], raw_add[4], clean_add[4], raw_mul[4], clean_mul[4], s_f[4], s_a[4], s_m[4], w_f[4], w_a[4], w_m[4];
            const V va = make(a), vb = make(b), vc = make(c);
            const V rf = _mm_fmadd_ps(va, vb, vc), ra = _mm_add_ps(va, vb), rm = _mm_mul_ps(va, vb);
            store(rf, raw_fused); store(vec::fused_clean<M>(va, vb, vc), clean_fused);
            store(ra, raw_add); store(vec::add_clean<M>(va, vb), clean_add);
            store(rm, raw_mul); store(vec::mul_clean<M>(va, vb), clean_mul);
            store(vec::suspect(rf), s_f); store(vec::suspect(ra), s_a); store(vec::suspect(rm), s_m);
            store(suspect_below_only(rf), w_f); store(suspect_below_only(ra), w_a); store(suspect_below_only(rm), w_m);
            const auto test = [&](const float (&raw)[4], const float (&cl)[4], const float (&s)[4], const float (&w)[4]) {
                for (int lane = 0; lane < 4; ++lane) {
                    ++spec_lanes;
                    std::uint32_t sb, wb; std::memcpy(&sb, &s[lane], 4); std::memcpy(&wb, &w[lane], 4);
                    const bool differs = std::memcmp(&raw[lane], &cl[lane], 4) != 0;
                    if (wb == 0u && differs) ++spec_wrong_missed;  // the blind detector would have accepted a differing result
                    if (sb != 0u) { ++spec_bad; continue; }
                    if (differs) ++spec_mismatch;
                }
            };
            test(raw_fused, clean_fused, s_f, w_f); test(raw_add, clean_add, s_a, w_a); test(raw_mul, clean_mul, s_m, w_m);
        }
        compare<M>(mul_wrong, a, b, [](V x, V y) { return mul_without_boundary<M>(x, y); }, [](V x, V y) { return vec::mul<M>(x, y); });
        compare<M>(add_wrong, a, b, [](V x, V y) { return add_without_flush<M>(x, y); }, [](V x, V y) { return vec::add<M>(x, y); });
        for (int lane = 0; lane < 4; ++lane) {  // how often the crafted operands reach the ambiguous case
            const float p = a[lane] * b[lane];
            if (std::fabs(p) == kMin) ++mul.boundary_lanes;
        }
    }
    const bool flush = M == SubnormalMode::Flush;
    // add_without_flush differs only in the flush mode (preserve keeps subnormals: the plain sum is right there);
    // mul_without_boundary likewise.
    const bool controls_caught = flush ? (mul_wrong.mismatches > 0 && add_wrong.mismatches > 0) : true;
    const bool spec_ok = !flush || (spec_mismatch == 0 && spec_bad > 0 && spec_wrong_missed > 0);
    const bool ok = add.mismatches == 0 && mul.mismatches == 0 && fused.mismatches == 0 && controls_caught &&
                    (!flush || mul.boundary_lanes > 0) && spec_ok;
    if (flush)
        std::printf("flush     speculation: %llu results, %llu suspect (take the exact path), %llu unflushed results differing from the clean op (must be 0); "
                    "negative control (detector blind to the smallest-normal magnitude) misses %llu\n",
                    static_cast<unsigned long long>(spec_lanes), static_cast<unsigned long long>(spec_bad),
                    static_cast<unsigned long long>(spec_mismatch), static_cast<unsigned long long>(spec_wrong_missed));
    std::printf("%-9s add %llu lanes, mul %llu lanes (%llu on the boundary), fused %llu lanes: %llu / %llu / %llu mismatches;"
                " negative controls (mul without boundary, add without flush) %llu / %llu mismatches: %s\n",
                name, static_cast<unsigned long long>(add.lanes), static_cast<unsigned long long>(mul.lanes),
                static_cast<unsigned long long>(mul.boundary_lanes), static_cast<unsigned long long>(fused.lanes),
                static_cast<unsigned long long>(add.mismatches), static_cast<unsigned long long>(mul.mismatches),
                static_cast<unsigned long long>(fused.mismatches), static_cast<unsigned long long>(mul_wrong.mismatches),
                static_cast<unsigned long long>(add_wrong.mismatches), ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}

int run_nearest() {
    Rng rng;
    std::uint64_t checked = 0, mismatches_flush = 0, mismatches_preserve = 0, wrong_caught = 0;
    const auto check = [&](float value) {
        ++checked;
        if (vector_nearest<true>(value) != scalar_nearest(value, true)) ++mismatches_flush;
        if (vector_nearest<false>(value) != scalar_nearest(value, false)) ++mismatches_preserve;
        if (truncating_nearest(value) != scalar_nearest(value, true)) ++wrong_caught;
    };
    for (int i = 0; i < 2000000; ++i) {
        const std::uint64_t r = rng.next();
        float value;
        switch (r & 7u) {
            case 0: value = static_cast<float>(static_cast<std::int32_t>(r >> 32)) * 0.5f; break;           // halves
            case 1: value = static_cast<float>(static_cast<std::int32_t>(r >> 40)) + 0.5f; break;           // ties
            case 2: value = static_cast<float>(static_cast<std::int32_t>(r >> 33)) * 1.5f; break;           // near +-2^31
            case 3: value = from_bits(static_cast<std::uint32_t>(r >> 16)); break;                          // any bits
            default: value = static_cast<float>(static_cast<std::int32_t>(r >> 40)) * (1.0f / 8.0f); break;
        }
        check(value);
    }
    for (const float v : {0.0f, -0.0f, 0.5f, -0.5f, 1.5f, 2.5f, -2.5f, 2147483520.0f, -2147483648.0f, 2147483648.0f,
                          -2147483904.0f, INFINITY, -INFINITY, from_bits(0x7FC00000u), from_bits(0xFFC00000u)})
        check(v);
    const bool ok = mismatches_flush == 0 && mismatches_preserve == 0 && wrong_caught > 0;
    std::printf("nearest   %llu values: %llu flush-mode / %llu preserve-mode mismatches; negative control (truncation) "
                "differs on %llu: %s\n",
                static_cast<unsigned long long>(checked), static_cast<unsigned long long>(mismatches_flush),
                static_cast<unsigned long long>(mismatches_preserve), static_cast<unsigned long long>(wrong_caught),
                ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}
}  // namespace

int main() {
    int failures = 0;
    failures += run_mode<SubnormalMode::Flush>("flush");
    failures += run_mode<SubnormalMode::Preserve>("preserve");
    failures += run_nearest();
    std::puts(failures == 0 ? "reverb_ops_tests: ok" : "reverb_ops_tests: FAILED");
    return failures == 0 ? 0 : 1;
}
#endif
