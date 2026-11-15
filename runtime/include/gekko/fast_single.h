#pragma once

// Plain single-precision arithmetic for the host intrinsics that implement the game's float routines (3x4 matrix
// leaves, vector helpers, collision distance tests).
//
// The exact route is ppc::fps_* on FPR bit patterns (double-format values, NaN / denormal / non-IEEE handling, a
// midpoint repair for fused results). For the operands the game really feeds these routines, that route returns
// exactly what the host's own single-precision instruction returns:
//   - an operand that is zero or a normal finite single (no NaN, infinity or denormal: those take the exact route),
//   - a result that is zero or a normal finite single of magnitude >= 2^-100 (no overflow, no underflow towards the
//     non-IEEE flush boundary 2^-126, no NaN).
// Then add / sub / mul / div are the correctly rounded float operations (a double operation followed by a rounding to
// single, as the exact route does, is the same value: the double has more than twice the single's precision plus
// two bits), and a fused multiply-add is `fmaf` (the exact route's double fma + rounding + midpoint repair is the
// once-rounded result). None of these changes an FPSCR bit, so the fast route has no state effect either.
//
// Usage: collect every operand and every result in a Check, compute in float, and take the exact route (the
// untouched original code) when `check.bad` is non-zero. Nothing may be written before the decision.
//
// The fused operation needs FMA hardware (/arch:AVX2 builds); without it `kAvailable` is false and callers must not
// take the fast route.

#include "gekko/cpu.h"
#include "gekko/ppc_ops.h"
#include "gekko/types.h"

#if defined(__AVX2__) || defined(__FMA__)
#include <immintrin.h>
#define GEKKO_FAST_SINGLE 1
#else
#define GEKKO_FAST_SINGLE 0
#endif

namespace gekko::fast {

inline constexpr bool kAvailable = GEKKO_FAST_SINGLE != 0;

// 1 when `word` is not zero / a normal finite single.
GEKKO_ALWAYS_INLINE u32 bad_word(u32 word) noexcept {
    const u32 magnitude = word & 0x7FFFFFFFu;
    return static_cast<u32>((magnitude - 0x00800000u) >= 0x7F000000u) & static_cast<u32>(magnitude != 0u);
}

// 1 when a result is not zero / a normal finite single with |r| >= 2^-100.
GEKKO_ALWAYS_INLINE u32 bad_result(float value) noexcept {
    const u32 magnitude = float_to_bits(value) & 0x7FFFFFFFu;
    return static_cast<u32>((magnitude - 0x0D800000u) >= (0x7F800000u - 0x0D800000u)) &
           static_cast<u32>(magnitude != 0u);
}

struct Check {
    u32 bad = 0;
    // An operand word as a float.
    GEKKO_ALWAYS_INLINE float in(u32 word) noexcept {
        bad |= bad_word(word);
        return bits_to_float(word);
    }
    // A computed value that must stay ordinary.
    GEKKO_ALWAYS_INLINE float out(float value) noexcept {
        bad |= bad_result(value);
        return value;
    }
    GEKKO_ALWAYS_INLINE bool ok() const noexcept { return bad == 0u; }
};

// a * c + b, rounded once.
GEKKO_ALWAYS_INLINE float fma(float a, float c, float b) noexcept {
#if GEKKO_FAST_SINGLE
    return _mm_cvtss_f32(_mm_fmadd_ss(_mm_set_ss(a), _mm_set_ss(c), _mm_set_ss(b)));
#else
    return a * c + b;  // never used: kAvailable is false
#endif
}

// The CR field / FPCC of an unordered or ordered compare of two non-NaN values (ppc::fp_compare's non-NaN arms).
GEKKO_ALWAYS_INLINE u32 compare_flags(float a, float b) noexcept { return a < b ? 8u : (a > b ? 4u : 2u); }
GEKKO_ALWAYS_INLINE void put_compare(Cpu& c, u32 field, u32 flags) noexcept {
    ppc::set_cr_field(c, field, flags);
    c.fpscr = (c.fpscr & ~kFpscrFPCC) | (flags << 12);
}
GEKKO_ALWAYS_INLINE bool nan_word(u32 word) noexcept { return (word << 1) > 0xFF000000u; }

// The FPR image of a float result (a single is held as the same value in double format).
GEKKO_ALWAYS_INLINE u64 fpr_of(float value) noexcept { return double_to_bits(static_cast<double>(value)); }

// fcmpu / fcmpo cr0 on single words, with the register writes deferred: every compare overwrites cr0 and FPCC, so
// only the last one executed is observable. NaN words flush what is pending and take ppc::fp_compare.
class LazyCompare {
public:
    explicit LazyCompare(Cpu& c) noexcept : c_(c) {}
    LazyCompare(const LazyCompare&) = delete;
    LazyCompare& operator=(const LazyCompare&) = delete;
    ~LazyCompare() { flush(); }
    void flush() noexcept {
        if (pending_) {
            put_compare(c_, 0u, flags_);
            pending_ = false;
        }
    }
    // cr0 after fcmp(lfs(a), lfs(b)): returns the EQ bit.
    bool equal(u32 a, u32 b, bool ordered) noexcept {
        if (nan_word(a) || nan_word(b)) {
            flush();
            ppc::fp_compare(c_, 0u, single_to_double_bits(a), single_to_double_bits(b), ordered);
            return ppc::cr_bit(c_, 2u);
        }
        flags_ = compare_flags(bits_to_float(a), bits_to_float(b));
        pending_ = true;
        return flags_ == 2u;
    }

private:
    Cpu& c_;
    u32 flags_ = 0;
    bool pending_ = false;
};

}  // namespace gekko::fast
