#pragma once

// PowerPC 750CL ("Gekko") instruction semantics, expressed once.
//
// These helpers hold the parts of the architecture that are easy to get subtly
// wrong: carry and overflow generation, the condition register encoding, the
// rotate-and-mask forms, the shift edge cases, and the Gekko graphics
// quantisation. The interpreter in cpu.cpp implements the same rules inline;
// the statically recompiled code emitted by tools/ppcrecomp calls these.
//
// runtime/tests/ppc_recomp_tests.cpp proves the two agree: it runs every
// covered instruction encoding through the interpreter and through the
// recompiler-emitted form with randomised operands and compares the whole
// architected state afterwards.
//
// Nothing here touches host state the guest cannot see. Every function takes
// the Cpu whose architected registers it is defined to modify.

#include <algorithm>
#include <cmath>

#if defined(_MSC_VER)
#include <intrin.h>  // _BitScanReverse, _lzcnt_u32 (cntlzw)
#endif
#if defined(__AVX2__) || defined(__FMA__)
#include <immintrin.h>  // _mm_fmadd_sd (fused multiply-add)
#endif

#include "gekko/cpu.h"
#include "gekko/types.h"

namespace gekko {
namespace ppc {

// ---------------------------------------------------------------------------
// Bit helpers
// ---------------------------------------------------------------------------

GEKKO_ALWAYS_INLINE u32 rotl32(u32 value, u32 amount) noexcept {
    amount &= 31u;
    return amount == 0 ? value : ((value << amount) | (value >> (32u - amount)));
}

// The MB/ME mask of the rotate-and-mask forms. Constant for a given encoding,
// so the recompiler folds this at generation time and emits the literal.
constexpr u32 mask_from(u32 mb, u32 me) noexcept {
    const u32 begin = 0xffffffffu >> mb;
    const u32 end = (me >= 31u) ? 0u : (0xffffffffu >> (me + 1u));
    const u32 mask = begin ^ end;
    return (me < mb) ? ~mask : mask;
}

constexpr s32 sign_extend(u32 value, u32 bits) noexcept {
    const u32 shift = 32u - bits;
    return static_cast<s32>(value << shift) >> shift;
}

// Host round-to-nearest to single precision: NaNs keep their sign and the top
// 22 payload bits and become quiet. Not a model of any guest instruction on
// its own; the floating-point helpers below use it after their NaN and
// non-IEEE handling.
inline double round_to_single(double value) noexcept {
    return static_cast<double>(static_cast<float>(value));
}

// ---------------------------------------------------------------------------
// Condition register and XER
// ---------------------------------------------------------------------------

inline void set_cr_field(Cpu& c, u32 field, u32 value) noexcept {
    const u32 shift = 28u - 4u * field;
    c.cr = (c.cr & ~(0xfu << shift)) | ((value & 0xfu) << shift);
}

inline u32 cr_field(const Cpu& c, u32 field) noexcept {
    return (c.cr >> (28u - 4u * field)) & 0xfu;
}

inline void update_cr0(Cpu& c, u32 value) noexcept {
    const s32 signed_value = static_cast<s32>(value);
    u32 flags = signed_value < 0 ? 8u : (signed_value > 0 ? 4u : 2u);
    if ((c.xer & kXerSO) != 0u) {
        flags |= 1u;
    }
    set_cr_field(c, 0, flags);
}

inline void update_cr1(Cpu& c) noexcept { set_cr_field(c, 1, (c.fpscr >> 28) & 0xfu); }

inline void set_carry(Cpu& c, bool carry) noexcept {
    c.xer = carry ? (c.xer | kXerCA) : (c.xer & ~kXerCA);
}

inline void set_overflow(Cpu& c, bool overflow) noexcept {
    if (overflow) {
        c.xer |= kXerOV | kXerSO;
    } else {
        c.xer &= ~kXerOV;
    }
}

GEKKO_ALWAYS_INLINE void cmp(Cpu& c, u32 field, s32 lhs, s32 rhs) noexcept {
    u32 flags = lhs < rhs ? 8u : (lhs > rhs ? 4u : 2u);
    if ((c.xer & kXerSO) != 0u) {
        flags |= 1u;
    }
    set_cr_field(c, field, flags);
}

GEKKO_ALWAYS_INLINE void cmpl(Cpu& c, u32 field, u32 lhs, u32 rhs) noexcept {
    u32 flags = lhs < rhs ? 8u : (lhs > rhs ? 4u : 2u);
    if ((c.xer & kXerSO) != 0u) {
        flags |= 1u;
    }
    set_cr_field(c, field, flags);
}

GEKKO_ALWAYS_INLINE bool cr_bit(const Cpu& c, u32 bit) noexcept { return ((c.cr >> (31u - bit)) & 1u) != 0u; }

GEKKO_ALWAYS_INLINE void set_cr_bit(Cpu& c, u32 bit, u32 value) noexcept {
    const u32 mask = 1u << (31u - bit);
    c.cr = (c.cr & ~mask) | ((value & 1u) << (31u - bit));
}

// ---------------------------------------------------------------------------
// Integer arithmetic that produces carry or overflow
// ---------------------------------------------------------------------------
//
// Every form below is one 32-bit sum x + y + carry_in (manual Chapter 12):
//   add/addc/adde/addze/addme:     x = rA, y = rB | 0 | 0xFFFFFFFF
//   subf/subfc/subfe/subfze/subfme: x = ~rA, y = rB | 0 | 0xFFFFFFFF
// XER[OV] is signed overflow of that sum: x and y have the same sign and the
// result's sign differs, i.e. bit 31 of (x ^ value) & (y ^ value). With
// y == 0 this folds to (x ^ value) & ~x; with y == 0xFFFFFFFF to
// (x ^ value) & x. runtime/tests/ppc_recomp_tests.cpp checks every form
// against exact 64-bit signed arithmetic.

inline u32 addc(Cpu& c, u32 a, u32 b, bool oe) noexcept {
    const u64 result = static_cast<u64>(a) + b;
    const u32 value = static_cast<u32>(result);
    set_carry(c, result > 0xffffffffull);
    if (oe) {
        set_overflow(c, ((a ^ value) & (b ^ value) & 0x80000000u) != 0u);
    }
    return value;
}

inline u32 adde(Cpu& c, u32 a, u32 b, bool oe) noexcept {
    const u64 carry = (c.xer & kXerCA) != 0u ? 1ull : 0ull;
    const u64 result = static_cast<u64>(a) + b + carry;
    const u32 value = static_cast<u32>(result);
    set_carry(c, result > 0xffffffffull);
    if (oe) {
        set_overflow(c, ((a ^ value) & (b ^ value) & 0x80000000u) != 0u);
    }
    return value;
}

inline u32 addze(Cpu& c, u32 a, bool oe) noexcept {
    const u64 carry = (c.xer & kXerCA) != 0u ? 1ull : 0ull;
    const u64 result = static_cast<u64>(a) + carry;
    const u32 value = static_cast<u32>(result);
    set_carry(c, result > 0xffffffffull);
    if (oe) {
        set_overflow(c, ((a ^ value) & ~a & 0x80000000u) != 0u);
    }
    return value;
}

inline u32 addme(Cpu& c, u32 a, bool oe) noexcept {
    const u64 carry = (c.xer & kXerCA) != 0u ? 1ull : 0ull;
    const u64 result = static_cast<u64>(a) + 0xffffffffull + carry;
    const u32 value = static_cast<u32>(result);
    set_carry(c, result > 0xffffffffull);
    if (oe) {
        // y = -1: only a negative rA can wrap to a non-negative result.
        set_overflow(c, ((a ^ value) & a & 0x80000000u) != 0u);
    }
    return value;
}

inline u32 add_oe(Cpu& c, u32 a, u32 b) noexcept {
    const u32 value = a + b;
    set_overflow(c, ((a ^ value) & (b ^ value) & 0x80000000u) != 0u);
    return value;
}

inline u32 subfc(Cpu& c, u32 a, u32 b, bool oe) noexcept {
    const u32 complement = ~a;
    const u64 result = static_cast<u64>(complement) + b + 1ull;
    const u32 value = static_cast<u32>(result);
    set_carry(c, result > 0xffffffffull);
    if (oe) {
        set_overflow(c, ((complement ^ value) & (b ^ value) & 0x80000000u) != 0u);
    }
    return value;
}

inline u32 subfe(Cpu& c, u32 a, u32 b, bool oe) noexcept {
    const u64 carry = (c.xer & kXerCA) != 0u ? 1ull : 0ull;
    const u32 complement = ~a;
    const u64 result = static_cast<u64>(complement) + b + carry;
    const u32 value = static_cast<u32>(result);
    set_carry(c, result > 0xffffffffull);
    if (oe) {
        set_overflow(c, ((complement ^ value) & (b ^ value) & 0x80000000u) != 0u);
    }
    return value;
}

inline u32 subfze(Cpu& c, u32 a, bool oe) noexcept {
    const u64 carry = (c.xer & kXerCA) != 0u ? 1ull : 0ull;
    const u32 complement = ~a;
    const u64 result = static_cast<u64>(complement) + carry;
    const u32 value = static_cast<u32>(result);
    set_carry(c, result > 0xffffffffull);
    if (oe) {
        // y = 0: overflow needs a non-negative ~rA (a negative rA), so the mask
        // is rA itself - not addze's ~rA.
        set_overflow(c, ((complement ^ value) & a & 0x80000000u) != 0u);
    }
    return value;
}

inline u32 subfme(Cpu& c, u32 a, bool oe) noexcept {
    const u64 carry = (c.xer & kXerCA) != 0u ? 1ull : 0ull;
    const u32 complement = ~a;
    const u64 result = static_cast<u64>(complement) + 0xffffffffull + carry;
    const u32 value = static_cast<u32>(result);
    set_carry(c, result > 0xffffffffull);
    if (oe) {
        set_overflow(c, ((complement ^ value) & complement & 0x80000000u) != 0u);
    }
    return value;
}

inline u32 subf_oe(Cpu& c, u32 a, u32 b) noexcept {
    const u32 value = b - a;
    set_overflow(c, ((b ^ a) & (b ^ value) & 0x80000000u) != 0u);
    return value;
}

inline u32 neg_oe(Cpu& c, u32 a) noexcept {
    set_overflow(c, a == 0x80000000u);
    return 0u - a;
}

inline u32 subfic(Cpu& c, u32 a, u32 imm) noexcept {
    const u32 complement = ~a;
    const u64 result = static_cast<u64>(complement) + imm + 1ull;
    set_carry(c, result > 0xffffffffull);
    return static_cast<u32>(result);
}

inline u32 addic(Cpu& c, u32 a, u32 imm) noexcept {
    const u64 result = static_cast<u64>(a) + imm;
    set_carry(c, result > 0xffffffffull);
    return static_cast<u32>(result);
}

inline u32 mulhw(u32 a, u32 b) noexcept {
    const s64 product =
        static_cast<s64>(static_cast<s32>(a)) * static_cast<s64>(static_cast<s32>(b));
    return static_cast<u32>(static_cast<u64>(product) >> 32);
}

inline u32 mulhwu(u32 a, u32 b) noexcept {
    return static_cast<u32>((static_cast<u64>(a) * b) >> 32);
}

GEKKO_ALWAYS_INLINE u32 mullw(Cpu& c, u32 a, u32 b, bool oe) noexcept {
    const s64 product =
        static_cast<s64>(static_cast<s32>(a)) * static_cast<s64>(static_cast<s32>(b));
    const u32 value = static_cast<u32>(product);
    if (oe) {
        set_overflow(c, product != static_cast<s64>(static_cast<s32>(value)));
    }
    return value;
}

inline u32 mulli(u32 a, s32 imm) noexcept {
    const s64 product = static_cast<s64>(static_cast<s32>(a)) * imm;
    return static_cast<u32>(product);
}

inline u32 divw(Cpu& c, u32 a, u32 b, bool oe) noexcept {
    const s32 dividend = static_cast<s32>(a);
    const s32 divisor = static_cast<s32>(b);
    s32 value = 0;
    if (divisor == 0 || (a == 0x80000000u && divisor == -1)) {
        if (oe) set_overflow(c, true);
        value = (divisor == 0) ? (dividend < 0 ? -1 : 0) : -1;
    } else {
        value = dividend / divisor;
        if (oe) set_overflow(c, false);
    }
    return static_cast<u32>(value);
}

inline u32 divwu(Cpu& c, u32 a, u32 b, bool oe) noexcept {
    if (b == 0) {
        if (oe) set_overflow(c, true);
        return 0;
    }
    if (oe) set_overflow(c, false);
    return a / b;
}

// cntlzw: leading zeros of a 32-bit value, 32 for zero (manual PAGE 369).
// One host instruction instead of MSVC's 16-pass unrolled shift loop. LZCNT is
// emitted only when the TU is compiled for AVX2 (every AVX2 host has LZCNT); on
// an SSE2/SSE4.2/AVX build the F3 0F BD encoding would silently execute as BSR
// on a pre-LZCNT CPU, so those use BSR with an explicit zero test. The
// reference interpreter keeps its own bit loop as the independent oracle.
GEKKO_ALWAYS_INLINE u32 cntlzw(u32 value) noexcept {
#if defined(_MSC_VER) && defined(__AVX2__)
    return static_cast<u32>(_lzcnt_u32(value));
#elif defined(_MSC_VER)
    unsigned long index = 0;
    return _BitScanReverse(&index, value) != 0 ? 31u - static_cast<u32>(index) : 32u;
#elif defined(__GNUC__) || defined(__clang__)
    return value == 0u ? 32u : static_cast<u32>(__builtin_clz(value));
#else
    u32 count = 0;
    while (count < 32 && (value & 0x80000000u) == 0u) {
        ++count;
        value <<= 1;
    }
    return count;
#endif
}

// ---------------------------------------------------------------------------
// lwarx / stwcx. reservation (manual PAGE 139, section 3.3.5.3)
// ---------------------------------------------------------------------------
//
// lwarx loads a word and sets the reservation on the aligned 32-byte block. The
// reservation is nonspecific for this processor: stwcx. never compares the
// address, it only asks whether a reservation exists, stores only if it does,
// and cancels it either way. CR0 = 00 || stored || XER[SO].

inline u32 lwarx(Cpu& c, u32 ea) {
    const u32 value = c.mem_read32(ea);
    c.reservation = true;
    c.reservation_address = ea & ~31u;
    return value;
}

inline void stwcx(Cpu& c, u32 ea, u32 value) {
    u32 flags = (c.xer & kXerSO) != 0u ? 1u : 0u;
    if (c.reservation) {
        c.mem_write32(ea, value);
        flags |= 2u;
    }
    c.reservation = false;
    set_cr_field(c, 0u, flags);
}

inline u32 slw(u32 s, u32 b) noexcept {
    const u32 shift = b & 63u;
    return shift >= 32u ? 0u : (s << shift);
}

inline u32 srw(u32 s, u32 b) noexcept {
    const u32 shift = b & 63u;
    return shift >= 32u ? 0u : (s >> shift);
}

inline u32 sraw(Cpu& c, u32 s, u32 b) noexcept {
    const u32 shift = b & 63u;
    const s32 value = static_cast<s32>(s);
    if (shift >= 32u) {
        set_carry(c, value < 0);
        return value < 0 ? 0xffffffffu : 0u;
    }
    set_carry(c, value < 0 && (s & ((1u << shift) - 1u)) != 0u);
    return static_cast<u32>(value >> shift);
}

GEKKO_ALWAYS_INLINE u32 srawi(Cpu& c, u32 s, u32 shift) noexcept {
    const s32 value = static_cast<s32>(s);
    set_carry(c, value < 0 && (s & ((1u << shift) - 1u)) != 0u);
    return static_cast<u32>(value >> shift);
}

GEKKO_ALWAYS_INLINE bool trap_taken(u32 to, s32 a, s32 b) noexcept {
    return ((to & 0x10u) && a < b) || ((to & 0x08u) && a > b) || ((to & 0x04u) && a == b) ||
           ((to & 0x02u) && static_cast<u32>(a) < static_cast<u32>(b)) ||
           ((to & 0x01u) && static_cast<u32>(a) > static_cast<u32>(b));
}

// ---------------------------------------------------------------------------
// Floating point
// ---------------------------------------------------------------------------

// A double-format signalling NaN: exponent all ones, quiet bit (fraction bit
// 51) clear, fraction nonzero. Tested on the stored bits, never on a double
// that has been through a host conversion.
constexpr bool is_signalling_nan(u64 bits) noexcept {
    return (bits & 0x7ff8000000000000ull) == 0x7ff0000000000000ull &&
           (bits & 0x0007ffffffffffffull) != 0u;
}

// Sets FPSCR exception bits by the usual rule (PAGE 473, mtfsf: "FX is set when
// an exception bit changes from 0 to 1"; FEX and VX are summaries): FX only on
// a 0 -> 1 transition, VX = OR of the VX* bits, FEX = OR of each exception
// summary bit ANDed with its enable (VX&VE, OX&OE, UX&UE, ZX&ZE, XX&XE; bits
// 2-6 shifted right by 22 land on bits 24-28).
constexpr u32 fpscr_raise(u32 fpscr, u32 exceptions) noexcept {
    u32 value = fpscr | exceptions;
    if ((exceptions & ~fpscr) != 0u) value |= kFpscrFX;
    value = (value & ~kFpscrVX) | ((value & kFpscrVXAny) != 0u ? kFpscrVX : 0u);
    value = (value & ~kFpscrFEX) | ((((value >> 22) & value) & 0xf8u) != 0u ? kFpscrFEX : 0u);
    return value;
}

// The exception half of an unordered compare (PAGE 397 fcmpo, 398 fcmpu, 503-506
// ps_cmpo0/1 and ps_cmpu0/1): an SNaN operand sets VXSNAN, plus VXVC for the
// ordered forms when VE = 0; otherwise a QNaN operand sets VXVC for the ordered
// forms only. An unordered compare of QNaNs changes no exception bit. Out of
// line: no in-game compare is known to see a NaN, and it keeps fp_compare small.
GEKKO_NOINLINE inline void fp_compare_unordered(Cpu& c, u64 a_bits, u64 b_bits,
                                                bool ordered) noexcept {
    u32 exceptions = 0u;
    if (is_signalling_nan(a_bits) || is_signalling_nan(b_bits)) {
        exceptions = kFpscrVXSNAN;
        if (ordered && (c.fpscr & kFpscrVE) == 0u) exceptions |= kFpscrVXVC;
    } else if (ordered) {
        exceptions = kFpscrVXVC;
    }
    if (exceptions != 0u) c.fpscr = fpscr_raise(c.fpscr, exceptions);
}

// fcmpu/fcmpo (ordered = XO 32) and ps_cmpu0/1, ps_cmpo0/1 (ordered = XO 32/96)
// share one definition: the result code c goes to CR field crfD and to FPCC
// (FPSCR bits 16-19) only - FPSCR[C] (bit 15) is not an affected bit. The
// operands are the stored FPR bits (ps0 or ps1). Ordered tests first, so a
// non-NaN compare decides on one host compare; NaN falls through all three.
inline void fp_compare(Cpu& c, u32 field, u64 a_bits, u64 b_bits, bool ordered) noexcept {
    const double va = bits_to_double(a_bits);
    const double vb = bits_to_double(b_bits);
    u32 flags;
    if (va < vb) {
        flags = 8u;
    } else if (va > vb) {
        flags = 4u;
    } else if (va == vb) {
        flags = 2u;
    } else {
        flags = 1u;
        fp_compare_unordered(c, a_bits, b_bits, ordered);
    }
    set_cr_field(c, field, flags);
    c.fpscr = (c.fpscr & ~kFpscrFPCC) | (flags << 12);
}

// fctiw / fctiwz (PAGE 399/400). A NaN or an operand below -2^31 gives
// 0x80000000; above 2^31 - 1 gives 0x7FFFFFFF - except in non-IEEE mode, where
// Table 2-22 (PAGE 85, "Single INT") places 0x80000000 for every invalid
// conversion. Positive overflow is invalid only when the ROUNDED value exceeds
// 2^31 - 1: fctiwz truncates (invalid from 2^31), fctiw rounds to nearest
// (invalid from 2147483647.5, whose tie rounds to the even 2^31). The negative
// side already gives 0x80000000 in both modes.
inline u64 fctiw(const Cpu& c, u64 bits, bool truncate) noexcept {
    const double value = bits_to_double(bits);
    s32 converted;
    if (value != value) {
        converted = static_cast<s32>(0x80000000u);
    } else if (value > 2147483647.0) {
        const bool invalid = truncate ? value >= 2147483648.0 : value >= 2147483647.5;
        converted = (invalid && (c.fpscr & kFpscrNI) != 0u) ? static_cast<s32>(0x80000000u)
                                                            : 2147483647;
    } else if (value < -2147483648.0) {
        converted = static_cast<s32>(0x80000000u);
    } else {
        converted = truncate ? static_cast<s32>(std::trunc(value))
                             : static_cast<s32>(std::nearbyint(value));
    }
    return 0xfff8000000000000ull | static_cast<u32>(converted);
}

// ---------------------------------------------------------------------------
// Floating-point arithmetic (audit fpu-1, fpu-2, fpu-3, paired-single-3)
// ---------------------------------------------------------------------------
//
// One definition for fadd fsub fmul fdiv fsqrt frsqrte, fadds fsubs fmuls
// fdivs fsqrts fres, the multiply-add family (fmadd fmsub fnmadd fnmsub, their
// single forms, ps_madd ps_msub ps_nmadd ps_nmsub ps_madds0 ps_madds1), frsp,
// and ps_add ps_sub ps_mul ps_div ps_res ps_rsqrte ps_muls0/1 ps_sum0/1. Every
// value is passed and returned as stored FPR bits. The result is, in order:
//
// 1. A NaN operand: the first NaN of frA, frB, frC (PAGE 85 Table 2-22 note 1,
//    "Prioritize according to Chapter 3"), made quiet with its sign and
//    payload kept (single-precision results then keep the top payload bits).
//    fnmadd/fnmsub do not negate it: "QNaNs propagate with no effect on their
//    sign bit" (PAGE 412-415, 522-525).
// 2. FPSCR[NI] = 1 (PAGE 84 Table 2-21): a denormalised operand is replaced by
//    a zero of its sign. Single-precision instructions count 0 < |x| < 2^-126
//    as denormalised, double-precision instructions the subnormal range.
// 3. An invalid operation (VE = 0) yields the default QNaN 0x7FF8000000000000:
//    "QNaNs that are generated as the result of a disabled invalid operation
//    exception have a sign bit of zero" (PAGE 415). x86 generates
//    0xFFF8000000000000. Never negated.
// 4. The multiply-add family rounds ONCE, the exact a*c +- b: "These
//    instructions combine multiply and add operations without an intermediate
//    rounding operation" (PAGE 97; PAGE 403/404, 508-510). Double forms are a
//    genuinely fused multiply-add. Single forms take the fused double s and
//    round it to single; that can differ from rounding the exact value only
//    when s lies exactly on a single-precision midpoint and the exact value
//    does not, and the sign of the exact residual a*c +- b - s then decides.
//    Every other single-precision instruction rounds its double result to
//    single. For single-precision operands, the only ones PAGE 82 defines,
//    that is rounding the exact value: a sum, product, quotient or square root
//    of 24-bit mantissas that is inexact in double is never a midpoint.
// 5. FPSCR[NI] = 1 (PAGE 85 Table 2-22): a denormalised result is a zero of its
//    sign. Tininess is decided before the final rounding, on |v| < 2^-126
//    (single) or < 2^-1022 (double), where v is the exact value for the
//    multiply-add family and for double mul/div, and the double result
//    otherwise.
// 6. fnmadd/fnmsub negate the rounded result ("rounded ... then negated",
//    PAGE 412-415).
//
// Unchanged and still unmodelled: the FPSCR status and exception bits these
// instructions affect (FPRF FR FI FX OX UX ZX XX VX*), FPSCR[RN], and the
// VE = 1 / ZE = 1 "no result" rules. fres and frsqrte remain 1/b and
// 1/sqrt(b), inside the permitted estimate.
//
// Cost. Each helper is the host operation, a test on the value classes that
// is false for finite operands and results away from zero, and one
// never-taken branch into fp_arith_slow, which recomputes from the operand
// bits by the rules above and reads FPSCR[NI]. "Tiny" is 0 < |x| <= 2^-125
// (single) / 2^-1021 (double): (x << 1) - 1 wraps +-0 to all ones, so one
// unsigned compare of the minimum over the tested values rejects tiny values
// and never zeros, the common operands. A NaN result is `r != r`.
// fadd/fsub and the single and paired sums test only their result, against a
// wider bound: a denormalised operand d cannot change round(x + d) unless
// |x| < 2^-71 (single) / 2^-967 (double), because |d| < ulp(x) / 4 above
// that, so every affected result is <= 2^-70 / 2^-966.

constexpr u64 kFpSignBit = 0x8000000000000000ull;
constexpr u64 kFpQuietBit = 0x0008000000000000ull;
constexpr u64 kFpDefaultQNaN = 0x7ff8000000000000ull;

// fp_arith_slow operation codes; kFpSingle marks a single-precision result.
enum FpOp : u32 {
    kFpAdd = 0u,     // frA + frB
    kFpSub = 1u,     // frA - frB
    kFpMul = 2u,     // frA * frC
    kFpDiv = 3u,     // frA / frB
    kFpSqrt = 4u,    // sqrt(frB)
    kFpRes = 5u,     // 1 / frB (fres, ps_res)
    kFpRsqrte = 6u,  // 1 / sqrt(frB) (frsqrte, ps_rsqrte)
    kFpRound = 7u,   // frB (frsp: single rounding only)
    kFpMAdd = 8u,    // frA * frC + frB
    kFpMSub = 9u,    // frA * frC - frB
    kFpNMAdd = 10u,  // -(frA * frC + frB)
    kFpNMSub = 11u,  // -(frA * frC - frB)
    kFpSingle = 0x10u,
};

// a * c + b rounded once. /arch:AVX2 builds (the generated shards) get the
// FMA instruction; others the correctly rounded C library fma.
GEKKO_ALWAYS_INLINE double fused_multiply_add(double a, double c, double b) noexcept {
#if defined(__AVX2__) || defined(__FMA__)
    return _mm_cvtsd_f64(_mm_fmadd_sd(_mm_set_sd(a), _mm_set_sd(c), _mm_set_sd(b)));
#else
    return std::fma(a, c, b);
#endif
}

namespace fp_detail {

// (bits << 1) - 1: magnitude order with +-0 wrapped to all ones.
GEKKO_ALWAYS_INLINE u64 tiny_key(u64 bits) noexcept { return (bits << 1) - 1u; }
GEKKO_ALWAYS_INLINE u64 lower(u64 x, u64 y) noexcept { return y < x ? y : x; }
// (key >> 53) < bound  <=>  0 < |x| <= 2^(bound - 1023)
constexpr u32 kTinySingle = 898u;  // 2^-125
constexpr u32 kTinyDouble = 2u;    // 2^-1021
constexpr u32 kSumSingle = 953u;   // 2^-70
constexpr u32 kSumDouble = 57u;    // 2^-966

GEKKO_ALWAYS_INLINE u64 to_single(double value) noexcept {
    return double_to_bits(static_cast<double>(static_cast<float>(value)));
}

// The sign of a sum of doubles, exactly: Shewchuk's grow-expansion keeps the
// sum as nonoverlapping components ordered by magnitude (two-sum per step,
// exact under round-to-nearest without overflow); its sign is the sign of the
// largest nonzero component.
inline int expansion_sign(const double* terms, int count) noexcept {
    double parts[8];
    int size = 0;
    for (int k = 0; k < count; ++k) {
        double q = terms[k];
        for (int i = 0; i < size; ++i) {
            const double sum = q + parts[i];
            const double virt = sum - q;
            parts[i] = (q - (sum - virt)) + (parts[i] - virt);
            q = sum;
        }
        parts[size++] = q;
    }
    for (int i = size - 1; i >= 0; --i) {
        if (parts[i] != 0.0) return parts[i] > 0.0 ? 1 : -1;
    }
    return 0;
}

// sign(x * y + z - w), exactly, for finite doubles.
//   z - w = d + e exactly (two-sum). z and w are first quartered (exact: both
//   are at least 2^970) when z - w could overflow; the exponent absorbs it.
//   |x*y| lies in [2^E, 2^(E+2)) with E = ilogb(x) + ilogb(y). If |d| >= 2^(E+3)
//   the product cannot reach the difference. Otherwise divide everything by
//   2^E: x' = x / 2^ilogb(x) and y' in [1, 2), so x'*y' = p + ep exactly (fma),
//   and d' = d / 2^E is below 8. If |d'| < 1/2 the product decides. Else d' is
//   at least 1/2, so d' and e' are exact unless e' underflows - and then
//   e' matters only if x'*y' + d' cancels exactly, which leaves sign(e).
GEKKO_NOINLINE inline int exact_sign_product_sum(double x, double y, double z, double w) noexcept {
    int exponent = 0;
    if (std::signbit(z) != std::signbit(w) && std::fabs(z) >= 0x1p970 && std::fabs(w) >= 0x1p970) {
        z *= 0.25;
        w *= 0.25;
        exponent = -2;
    }
    const double d = z - w;
    const double virt = d - z;
    const double e = (z - (d - virt)) + (-w - virt);
    const int difference_sign = d > 0.0 ? 1 : (d < 0.0 ? -1 : (e > 0.0 ? 1 : (e < 0.0 ? -1 : 0)));
    if (x == 0.0 || y == 0.0) return difference_sign;
    const int product_sign = std::signbit(x) != std::signbit(y) ? -1 : 1;
    const int ex = std::ilogb(x);
    const int ey = std::ilogb(y);
    exponent += ex + ey;
    if (d == 0.0) return product_sign;  // e == 0 too: z - w is exactly zero
    // |x*y| < 2^(E+2) <= |d| / 2 < |z - w|. Below 2^-1074 every nonzero
    // difference of doubles already dominates.
    if (exponent + 3 < -1074 || std::fabs(d) >= std::scalbn(1.0, exponent + 3)) {
        return difference_sign;
    }
    const double xs = std::scalbn(x, -ex);
    const double ys = std::scalbn(y, -ey);
    const double ds = std::scalbn(d, -exponent);
    if (std::fabs(ds) < 0.5) return product_sign;
    const double es = std::scalbn(e, -exponent);
    const double p = xs * ys;
    const double ep = fused_multiply_add(xs, ys, -p);
    const double terms[4] = {es, ep, ds, p};
    const int sign = expansion_sign(terms, 4);
    return sign != 0 ? sign : (e > 0.0 ? 1 : (e < 0.0 ? -1 : 0));
}

}  // namespace fp_detail

// Every case the inline helpers below do not finish: NaN operands or results,
// tiny operands or results, single-precision midpoints of the fused forms.
// a/b/c are frA/frB/frC (0 when the operation has no such operand).
GEKKO_NOINLINE inline u64 fp_arith_slow(u32 fpscr, u32 op, u64 a_bits, u64 b_bits,
                                        u64 c_bits) noexcept {
    const bool single = (op & kFpSingle) != 0u;
    const u32 kind = op & 0xfu;
    const bool fused = kind >= kFpMAdd;
    const bool uses_a = kind <= kFpDiv || fused;
    const bool uses_b = kind != kFpMul;
    const bool uses_c = kind == kFpMul || fused;

    // 1. NaN operands, priority frA, frB, frC.
    const auto is_nan = [](u64 bits) { return (bits << 1) > 0xffe0000000000000ull; };
    u64 nan = 0u;
    if (uses_a && is_nan(a_bits)) {
        nan = a_bits;
    } else if (uses_b && is_nan(b_bits)) {
        nan = b_bits;
    } else if (uses_c && is_nan(c_bits)) {
        nan = c_bits;
    }
    if (nan != 0u) {
        nan |= kFpQuietBit;
        return single ? double_to_bits(round_to_single(bits_to_double(nan))) : nan;
    }

    // 2. Non-IEEE mode: denormalised operands become zeros of their sign.
    const bool non_ieee = (fpscr & kFpscrNI) != 0u;
    if (non_ieee) {
        const u64 bound = single ? 0x7020000000000000ull : 0x0020000000000000ull;
        const auto zero_tiny = [bound](u64 bits) {
            const u64 magnitude = bits << 1;
            return (magnitude != 0u && magnitude < bound) ? (bits & kFpSignBit) : bits;
        };
        a_bits = zero_tiny(a_bits);
        b_bits = zero_tiny(b_bits);
        c_bits = zero_tiny(c_bits);
    }
    const double va = bits_to_double(a_bits);
    const double vb = bits_to_double(b_bits);
    const double vc = bits_to_double(c_bits);
    const double addend = (kind == kFpMSub || kind == kFpNMSub) ? -vb : vb;
    double r;
    switch (kind) {
        case kFpAdd: r = va + vb; break;
        case kFpSub: r = va - vb; break;
        case kFpMul: r = va * vc; break;
        case kFpDiv: r = va / vb; break;
        case kFpSqrt: r = std::sqrt(vb); break;
        case kFpRes: r = 1.0 / vb; break;
        case kFpRsqrte: r = 1.0 / std::sqrt(vb); break;
        case kFpRound: r = vb; break;
        default: r = fused_multiply_add(va, vc, addend); break;
    }

    // 3. An invalid operation generates the default QNaN.
    if (r != r) return kFpDefaultQNaN;

    // Sign of (exact value - r); called only with r finite, so every operand
    // it reads is finite.
    const auto residual = [&]() -> int {
        if (fused) return fp_detail::exact_sign_product_sum(va, vc, addend, r);
        if (single) return 0;
        if (kind == kFpMul) return fp_detail::exact_sign_product_sum(va, vc, 0.0, r);
        if (kind == kFpDiv) {
            // a/b - r = -(r*b - a) / b
            const int sign = fp_detail::exact_sign_product_sum(r, vb, 0.0, va);
            return std::signbit(vb) ? sign : -sign;
        }
        return 0;
    };

    u64 out;
    const double tiny = single ? 0x1p-126 : 0x1p-1022;
    const double magnitude = std::fabs(r);
    bool flush = false;
    if (non_ieee && magnitude <= tiny) {
        // 5. |v| < tiny. r is v correctly rounded and tiny is a double, so only
        // |r| == tiny can hide a v just below it.
        flush = magnitude < tiny || (std::signbit(r) ? residual() > 0 : residual() < 0);
    }
    if (flush) {
        out = double_to_bits(r) & kFpSignBit;
    } else if (single) {
        double rounded = round_to_single(r);
        if (fused && magnitude != 0.0 && magnitude <= 0x1p128) {
            // 4. r exactly on a single midpoint: an odd multiple of half the
            // single ulp 2^(e-23), e = max(ilogb(r), -126). (Comparing how r's
            // double neighbours round is not a midpoint test: a neighbour of
            // r may itself be the midpoint.) The exact value then rounds the
            // way its residual points, which the neighbour on that side shows.
            const int e = std::max(std::ilogb(r), -126);
            if (std::fabs(std::fmod(std::scalbn(r, 24 - e), 2.0)) == 1.0) {
                const int sign = residual();
                if (sign != 0) rounded = round_to_single(std::nextafter(r, sign > 0 ? HUGE_VAL : -HUGE_VAL));
            }
        }
        out = double_to_bits(rounded);
    } else {
        out = double_to_bits(r);
    }
    // 6.
    return (kind == kFpNMAdd || kind == kFpNMSub) ? (out ^ kFpSignBit) : out;
}

// ---- double-precision results ----------------------------------------------

GEKKO_ALWAYS_INLINE u64 fp_add(Cpu& c, u64 a, u64 b) noexcept {
    const double r = bits_to_double(a) + bits_to_double(b);
    if ((fp_detail::tiny_key(double_to_bits(r)) >> 53) < fp_detail::kSumDouble || r != r) {
        return fp_arith_slow(c.fpscr, kFpAdd, a, b, 0u);
    }
    return double_to_bits(r);
}

GEKKO_ALWAYS_INLINE u64 fp_sub(Cpu& c, u64 a, u64 b) noexcept {
    const double r = bits_to_double(a) - bits_to_double(b);
    if ((fp_detail::tiny_key(double_to_bits(r)) >> 53) < fp_detail::kSumDouble || r != r) {
        return fp_arith_slow(c.fpscr, kFpSub, a, b, 0u);
    }
    return double_to_bits(r);
}

GEKKO_ALWAYS_INLINE u64 fp_mul(Cpu& c, u64 a, u64 cc) noexcept {
    const double r = bits_to_double(a) * bits_to_double(cc);
    const u64 key = fp_detail::lower(fp_detail::lower(fp_detail::tiny_key(a), fp_detail::tiny_key(cc)),
                                     fp_detail::tiny_key(double_to_bits(r)));
    if ((key >> 53) < fp_detail::kTinyDouble || r != r) {
        return fp_arith_slow(c.fpscr, kFpMul, a, 0u, cc);
    }
    return double_to_bits(r);
}

GEKKO_ALWAYS_INLINE u64 fp_div(Cpu& c, u64 a, u64 b) noexcept {
    const double r = bits_to_double(a) / bits_to_double(b);
    const u64 key = fp_detail::lower(fp_detail::lower(fp_detail::tiny_key(a), fp_detail::tiny_key(b)),
                                     fp_detail::tiny_key(double_to_bits(r)));
    if ((key >> 53) < fp_detail::kTinyDouble || r != r) {
        return fp_arith_slow(c.fpscr, kFpDiv, a, b, 0u);
    }
    return double_to_bits(r);
}

// sqrt(b) is tiny only for a tiny b.
GEKKO_ALWAYS_INLINE u64 fp_sqrt(Cpu& c, u64 b) noexcept {
    const double r = std::sqrt(bits_to_double(b));
    if ((fp_detail::tiny_key(b) >> 53) < fp_detail::kTinyDouble || r != r) {
        return fp_arith_slow(c.fpscr, kFpSqrt, 0u, b, 0u);
    }
    return double_to_bits(r);
}

// 1/sqrt(b) is never below 2^-1021 for a finite b.
GEKKO_ALWAYS_INLINE u64 fp_rsqrte(Cpu& c, u64 b) noexcept {
    const double r = 1.0 / std::sqrt(bits_to_double(b));
    if ((fp_detail::tiny_key(b) >> 53) < fp_detail::kTinyDouble || r != r) {
        return fp_arith_slow(c.fpscr, kFpRsqrte, 0u, b, 0u);
    }
    return double_to_bits(r);
}

// fmadd/fmsub/fnmadd/fnmsub frD,frA,frC,frB.
GEKKO_ALWAYS_INLINE u64 fp_fused(Cpu& c, u32 kind, u64 a, u64 cc, u64 b) noexcept {
    const u64 addend = (kind == kFpMSub || kind == kFpNMSub) ? (b ^ kFpSignBit) : b;
    const double r = fused_multiply_add(bits_to_double(a), bits_to_double(cc), bits_to_double(addend));
    u64 key = fp_detail::lower(fp_detail::tiny_key(a), fp_detail::tiny_key(cc));
    key = fp_detail::lower(fp_detail::lower(key, fp_detail::tiny_key(b)),
                           fp_detail::tiny_key(double_to_bits(r)));
    if ((key >> 53) < fp_detail::kTinyDouble || r != r) {
        return fp_arith_slow(c.fpscr, kind, a, b, cc);
    }
    return (kind >= kFpNMAdd) ? (double_to_bits(r) ^ kFpSignBit) : double_to_bits(r);
}
GEKKO_ALWAYS_INLINE u64 fp_madd(Cpu& c, u64 a, u64 cc, u64 b) noexcept { return fp_fused(c, kFpMAdd, a, cc, b); }
GEKKO_ALWAYS_INLINE u64 fp_msub(Cpu& c, u64 a, u64 cc, u64 b) noexcept { return fp_fused(c, kFpMSub, a, cc, b); }
GEKKO_ALWAYS_INLINE u64 fp_nmadd(Cpu& c, u64 a, u64 cc, u64 b) noexcept { return fp_fused(c, kFpNMAdd, a, cc, b); }
GEKKO_ALWAYS_INLINE u64 fp_nmsub(Cpu& c, u64 a, u64 cc, u64 b) noexcept { return fp_fused(c, kFpNMSub, a, cc, b); }

// ---- single-precision results (group 59, paired singles, frsp) --------------

GEKKO_ALWAYS_INLINE u64 fps_add(Cpu& c, u64 a, u64 b) noexcept {
    const double r = bits_to_double(a) + bits_to_double(b);
    if ((fp_detail::tiny_key(double_to_bits(r)) >> 53) < fp_detail::kSumSingle || r != r) {
        return fp_arith_slow(c.fpscr, kFpAdd | kFpSingle, a, b, 0u);
    }
    return fp_detail::to_single(r);
}

GEKKO_ALWAYS_INLINE u64 fps_sub(Cpu& c, u64 a, u64 b) noexcept {
    const double r = bits_to_double(a) - bits_to_double(b);
    if ((fp_detail::tiny_key(double_to_bits(r)) >> 53) < fp_detail::kSumSingle || r != r) {
        return fp_arith_slow(c.fpscr, kFpSub | kFpSingle, a, b, 0u);
    }
    return fp_detail::to_single(r);
}

GEKKO_ALWAYS_INLINE u64 fps_mul(Cpu& c, u64 a, u64 cc) noexcept {
    const double r = bits_to_double(a) * bits_to_double(cc);
    const u64 key = fp_detail::lower(fp_detail::lower(fp_detail::tiny_key(a), fp_detail::tiny_key(cc)),
                                     fp_detail::tiny_key(double_to_bits(r)));
    if ((key >> 53) < fp_detail::kTinySingle || r != r) {
        return fp_arith_slow(c.fpscr, kFpMul | kFpSingle, a, 0u, cc);
    }
    return fp_detail::to_single(r);
}

GEKKO_ALWAYS_INLINE u64 fps_div(Cpu& c, u64 a, u64 b) noexcept {
    const double r = bits_to_double(a) / bits_to_double(b);
    const u64 key = fp_detail::lower(fp_detail::lower(fp_detail::tiny_key(a), fp_detail::tiny_key(b)),
                                     fp_detail::tiny_key(double_to_bits(r)));
    if ((key >> 53) < fp_detail::kTinySingle || r != r) {
        return fp_arith_slow(c.fpscr, kFpDiv | kFpSingle, a, b, 0u);
    }
    return fp_detail::to_single(r);
}

// sqrt(b) <= 2^-125 needs b <= 2^-250, itself tiny.
GEKKO_ALWAYS_INLINE u64 fps_sqrt(Cpu& c, u64 b) noexcept {
    const double r = std::sqrt(bits_to_double(b));
    if ((fp_detail::tiny_key(b) >> 53) < fp_detail::kTinySingle || r != r) {
        return fp_arith_slow(c.fpscr, kFpSqrt | kFpSingle, 0u, b, 0u);
    }
    return fp_detail::to_single(r);
}

GEKKO_ALWAYS_INLINE u64 fps_res(Cpu& c, u64 b) noexcept {
    const double r = 1.0 / bits_to_double(b);
    const u64 key = fp_detail::lower(fp_detail::tiny_key(b), fp_detail::tiny_key(double_to_bits(r)));
    if ((key >> 53) < fp_detail::kTinySingle || r != r) {
        return fp_arith_slow(c.fpscr, kFpRes | kFpSingle, 0u, b, 0u);
    }
    return fp_detail::to_single(r);
}

GEKKO_ALWAYS_INLINE u64 fps_rsqrte(Cpu& c, u64 b) noexcept {
    const double r = 1.0 / std::sqrt(bits_to_double(b));
    const u64 key = fp_detail::lower(fp_detail::tiny_key(b), fp_detail::tiny_key(double_to_bits(r)));
    if ((key >> 53) < fp_detail::kTinySingle || r != r) {
        return fp_arith_slow(c.fpscr, kFpRsqrte | kFpSingle, 0u, b, 0u);
    }
    return fp_detail::to_single(r);
}

// frsp. A NaN needs nothing extra: the host keeps the sign and frB[0-34] and
// sets the quiet bit, as the round-to-single model does.
GEKKO_ALWAYS_INLINE u64 fps_round(Cpu& c, u64 b) noexcept {
    if ((fp_detail::tiny_key(b) >> 53) < fp_detail::kTinySingle) {
        return fp_arith_slow(c.fpscr, kFpRound | kFpSingle, 0u, b, 0u);
    }
    return fp_detail::to_single(bits_to_double(b));
}

// fmadds/fmsubs/fnmadds/fnmsubs, ps_madd/ps_msub/ps_nmadd/ps_nmsub and
// ps_madds0/1 frD,frA,frC,frB (one half). The fused double is a single
// midpoint exactly when its 29 mantissa bits below the single-precision lsb
// are 1 followed by zeros (in the single normal range; below it the tiny test
// already routes to the slow path).
GEKKO_ALWAYS_INLINE u64 fps_fused(Cpu& c, u32 kind, u64 a, u64 cc, u64 b) noexcept {
    const u64 addend = (kind == kFpMSub || kind == kFpNMSub) ? (b ^ kFpSignBit) : b;
    const double r = fused_multiply_add(bits_to_double(a), bits_to_double(cc), bits_to_double(addend));
    const u64 r_bits = double_to_bits(r);
    u64 key = fp_detail::lower(fp_detail::tiny_key(a), fp_detail::tiny_key(cc));
    key = fp_detail::lower(fp_detail::lower(key, fp_detail::tiny_key(b)), fp_detail::tiny_key(r_bits));
    // (64-bit mask: a u32 cast made MSVC spill r_bits to the stack in halves.)
    if ((key >> 53) < fp_detail::kTinySingle || (r_bits & 0x1fffffffull) == 0x10000000ull || r != r) {
        return fp_arith_slow(c.fpscr, kind | kFpSingle, a, b, cc);
    }
    const u64 out = fp_detail::to_single(r);
    return (kind >= kFpNMAdd) ? (out ^ kFpSignBit) : out;
}
GEKKO_ALWAYS_INLINE u64 fps_madd(Cpu& c, u64 a, u64 cc, u64 b) noexcept { return fps_fused(c, kFpMAdd, a, cc, b); }
GEKKO_ALWAYS_INLINE u64 fps_msub(Cpu& c, u64 a, u64 cc, u64 b) noexcept { return fps_fused(c, kFpMSub, a, cc, b); }
GEKKO_ALWAYS_INLINE u64 fps_nmadd(Cpu& c, u64 a, u64 cc, u64 b) noexcept { return fps_fused(c, kFpNMAdd, a, cc, b); }
GEKKO_ALWAYS_INLINE u64 fps_nmsub(Cpu& c, u64 a, u64 cc, u64 b) noexcept { return fps_fused(c, kFpNMSub, a, cc, b); }

inline u32 field_mask8(u32 fm) noexcept {
    u32 mask = 0;
    for (u32 index = 0; index < 8; ++index) {
        if ((fm & (0x80u >> index)) != 0u) {
            mask |= 0xfu << (28u - 4u * index);
        }
    }
    return mask;
}

// ---------------------------------------------------------------------------
// Gekko paired-single quantised load and store
// ---------------------------------------------------------------------------

// GQR type 0 (and the reserved types 1-3, which both engines treat as type 0)
// moves the single-precision word unchanged: LOAD_SCALE "is applied only to
// integer numbers" (PAGE 493) and for floating point "the addition of the
// STORE_SCALE field to the exponent does not take place" (PAGE 497). That is
// the callee-saved FPR save/restore in every FP prologue and epilogue (GQR0),
// so psq_load/psq_store carry no scale work and no call beyond the memory
// accessors on that path. The integer types 4-7 go through the out-of-line
// psq_*_quantised functions. psq_load/psq_store stay out of line themselves
// (as MSVC already chose for the old, larger body): ~8,700 call sites.

// 2^n, exact for the |n| <= 32 of a 6-bit two's-complement GQR scale.
constexpr double exact_power_of_two(int n) noexcept {
    double value = 1.0;
    for (; n > 0; --n) value *= 2.0;
    for (; n < 0; ++n) value *= 0.5;
    return value;
}

// Scale factors indexed by the RAW 6-bit GQR scale field. Every entry is an
// exact power of two, so `x * table[raw]` is bit-identical to the
// std::ldexp(1.0, +-S) factor both engines used to compute per element.
struct GqrScaleTable {
    double factor[64];
    constexpr explicit GqrScaleTable(int sign) noexcept : factor{} {
        for (u32 raw = 0; raw < 64u; ++raw) {
            factor[raw] = exact_power_of_two(sign * sign_extend(raw, 6));
        }
    }
};
inline constexpr GqrScaleTable kGqrLoadScale{-1};  // F = I * 2^-S (PAGE 110)
inline constexpr GqrScaleTable kGqrStoreScale{1};  // I = ROUND(F * 2^S) (PAGE 112)

// Clamp to [lo, hi] for the quantised store, total over every double. The
// adjusted overflow values are lo for negative and hi for positive overflow,
// and "the converted value produced when the input operand is +Inf or NaN is
// the same as the adjusted result value for overflow of positive values"
// (PAGE 111). A NaN fails both tests and yields hi, so the integer conversion
// that follows always sees an in-range value (a NaN reaching static_cast<u8>
// is undefined behaviour, and on x64 it wrote 0).
constexpr double quantise_saturate(double value, double lo, double hi) noexcept {
    if (value < lo) return lo;
    if (value <= hi) return value;
    return hi;
}

inline float dequantise_element(Cpu& c, u32 address, u32 type, double factor) {
    switch (type) {
        case 4: return static_cast<float>(static_cast<double>(c.mem_read8(address)) * factor);
        case 5: return static_cast<float>(static_cast<double>(c.mem_read16(address)) * factor);
        case 6:
            return static_cast<float>(static_cast<double>(static_cast<s8>(c.mem_read8(address))) *
                                      factor);
        default:  // 7
            return static_cast<float>(static_cast<double>(static_cast<s16>(c.mem_read16(address))) *
                                      factor);
    }
}

inline void quantise_element(Cpu& c, u32 address, float value, u32 type, double factor) {
    const double scaled = static_cast<double>(value) * factor;
    switch (type) {
        case 4: c.mem_write8(address, static_cast<u8>(quantise_saturate(scaled, 0.0, 255.0))); break;
        case 5:
            c.mem_write16(address, static_cast<u16>(quantise_saturate(scaled, 0.0, 65535.0)));
            break;
        case 6:
            c.mem_write8(address,
                         static_cast<u8>(static_cast<s8>(quantise_saturate(scaled, -128.0, 127.0))));
            break;
        default:  // 7
            c.mem_write16(address, static_cast<u16>(static_cast<s16>(
                                       quantise_saturate(scaled, -32768.0, 32767.0))));
            break;
    }
}

// Integer GQR types 4-7: one byte for 4/6, two for 5/7 (PAGE 493 `c`).
GEKKO_NOINLINE inline void psq_load_quantised(Cpu& c, u32 d, u32 address, bool w, u32 type,
                                              u32 scale_field) {
    const double factor = kGqrLoadScale.factor[scale_field & 0x3fu];
    c.fpr[d].bits[0] =
        double_to_bits(static_cast<double>(dequantise_element(c, address, type, factor)));
    if (w) {
        c.fpr[d].bits[1] = double_to_bits(1.0);
    } else {
        c.fpr[d].bits[1] = double_to_bits(static_cast<double>(
            dequantise_element(c, address + (type & 1u) + 1u, type, factor)));
    }
}

GEKKO_NOINLINE inline void psq_store_quantised(Cpu& c, u32 d, u32 address, bool w, u32 type,
                                               u32 scale_field) {
    const double factor = kGqrStoreScale.factor[scale_field & 0x3fu];
    quantise_element(c, address, static_cast<float>(bits_to_double(c.fpr[d].bits[0])), type,
                     factor);
    if (!w) {
        quantise_element(c, address + (type & 1u) + 1u,
                         static_cast<float>(bits_to_double(c.fpr[d].bits[1])), type, factor);
    }
}

// A listed GX vertex site that is a psq_st (RenderWare's PTank vertex
// writers, docs/native-time-20261005/GX_DATA_PLANE.md): the elements psq_store
// would write, in the order it writes them, appended to the open immediate
// primitive instead of stored to the pipe page.
inline void quantise_vertex(Cpu& c, u32 site, float value, u32 type, double factor) {
    const double scaled = static_cast<double>(value) * factor;
    switch (type) {
        case 4: c.gx_vertex(site, static_cast<u8>(quantise_saturate(scaled, 0.0, 255.0)), 1u); break;
        case 5: c.gx_vertex(site, static_cast<u16>(quantise_saturate(scaled, 0.0, 65535.0)), 2u); break;
        case 6:
            c.gx_vertex(site, static_cast<u8>(static_cast<s8>(quantise_saturate(scaled, -128.0, 127.0))), 1u);
            break;
        default:  // 7
            c.gx_vertex(site, static_cast<u16>(static_cast<s16>(quantise_saturate(scaled, -32768.0, 32767.0))),
                        2u);
            break;
    }
}

GEKKO_NOINLINE inline void psq_store_gx(Cpu& c, u32 site, u32 d, bool w, u32 gqr_index) {
    const u32 gqr = c.spr[SPR_GQR0 + gqr_index];
    const u32 type = gqr & 7u;
    if (type < 4u) {
        c.gx_vertex(site, double_to_single_bits_flush(c.fpr[d].bits[0]), 4u);
        if (!w) c.gx_vertex(site, double_to_single_bits_flush(c.fpr[d].bits[1]), 4u);
        return;
    }
    const double factor = kGqrStoreScale.factor[(gqr >> 8) & 0x3fu];
    quantise_vertex(c, site, static_cast<float>(bits_to_double(c.fpr[d].bits[0])), type, factor);
    if (!w) quantise_vertex(c, site, static_cast<float>(bits_to_double(c.fpr[d].bits[1])), type, factor);
}

GEKKO_NOINLINE inline void psq_load(Cpu& c, u32 d, u32 address, bool w, u32 gqr_index) {
    const u32 gqr = c.spr[SPR_GQR0 + gqr_index];
    const u32 type = (gqr >> 16) & 7u;
    if (type >= 4u) {
        psq_load_quantised(c, d, address, w, type, gqr >> 24);
        return;
    }
    // Type 0 moves the word by the lfs bit expansion: a denorm loads as it is
    // (PAGE 111) and a signalling NaN stays signalling (audit fpu-4).
    c.fpr[d].bits[0] = single_to_double_bits(c.mem_read32(address));
    if (w) {
        c.fpr[d].bits[1] = double_to_bits(1.0);
    } else {
        c.fpr[d].bits[1] = single_to_double_bits(c.mem_read32(address + 4u));
    }
}

GEKKO_NOINLINE inline void psq_store(Cpu& c, u32 d, u32 address, bool w, u32 gqr_index) {
    const u32 gqr = c.spr[SPR_GQR0 + gqr_index];
    const u32 type = gqr & 7u;
    if (type >= 4u) {
        psq_store_quantised(c, d, address, w, type, gqr >> 8);
        return;
    }
    // Type 0 stores the word by bit selection, except that a denorm stores a
    // zero (PAGE 112; audit paired-single-8). The old narrowing cast wrote the
    // denormal and quieted signalling NaNs.
    c.mem_write32(address, double_to_single_bits_flush(c.fpr[d].bits[0]));
    if (!w) {
        c.mem_write32(address + 4u, double_to_single_bits_flush(c.fpr[d].bits[1]));
    }
}

// R16 (2026-10-20): psq_l / psq_st specialised for ONE GQR value K, emitted by
// ppcrecomp --psq-sites only behind an exact runtime guard
// (`c.spr[SPR_GQR0 + i] == K`, else the generic psq_load / psq_store above).
// With K a template constant, the type and scale fold away, so the work left
// is exactly the generic path's element arithmetic: the same expressions, in
// the same element order, through the same quantise_saturate / casts /
// single_to_double_bits / double_to_single_bits_flush. The pair is read or
// written through the CPU's FM accessors, element by element in the generic
// helper's order (FM step 2: the served-span shortcut over the page table is
// gone); every access the flat window does not serve is decided by the same
// *_slow path the generic helper's accessors reach.
template <u32 K>
struct PsqLoadK {
    static constexpr u32 type = (K >> 16) & 7u;
    static constexpr u32 size = type == 5u || type == 7u ? 2u : (type >= 4u ? 1u : 4u);
};
template <u32 K>
struct PsqStoreK {
    static constexpr u32 type = K & 7u;
    static constexpr u32 size = type == 5u || type == 7u ? 2u : (type >= 4u ? 1u : 4u);
};

template <u32 K>
GEKKO_ALWAYS_INLINE void psq_load_k(Cpu& c, u32 d, u32 address, bool w, u32 gqr_index) {
    using P = PsqLoadK<K>;
    (void)gqr_index;
    if constexpr (P::type >= 4u) {
        constexpr double factor = kGqrLoadScale.factor[(K >> 24) & 0x3fu];
        c.fpr[d].bits[0] = double_to_bits(static_cast<double>(dequantise_element(c, address, P::type, factor)));
        c.fpr[d].bits[1] = w ? double_to_bits(1.0)
                             : double_to_bits(static_cast<double>(
                                   dequantise_element(c, address + P::size, P::type, factor)));
    } else {
        c.fpr[d].bits[0] = single_to_double_bits(c.mem_read32(address));
        c.fpr[d].bits[1] = w ? double_to_bits(1.0) : single_to_double_bits(c.mem_read32(address + 4u));
    }
}

template <u32 K>
GEKKO_ALWAYS_INLINE void psq_store_k(Cpu& c, u32 d, u32 address, bool w, u32 gqr_index) {
    using P = PsqStoreK<K>;
    (void)gqr_index;
    if constexpr (P::type >= 4u) {
        constexpr double factor = kGqrStoreScale.factor[(K >> 8) & 0x3fu];
        quantise_element(c, address, static_cast<float>(bits_to_double(c.fpr[d].bits[0])), P::type, factor);
        if (!w) {
            quantise_element(c, address + P::size, static_cast<float>(bits_to_double(c.fpr[d].bits[1])), P::type,
                             factor);
        }
    } else {
        c.mem_write32(address, double_to_single_bits_flush(c.fpr[d].bits[0]));
        if (!w) c.mem_write32(address + 4u, double_to_single_bits_flush(c.fpr[d].bits[1]));
    }
}

// ---------------------------------------------------------------------------
// Special purpose registers, routed exactly as the interpreter routes them
// ---------------------------------------------------------------------------

inline u32 read_spr(const Cpu& c, u32 number) noexcept {
    switch (number) {
        case SPR_XER: return c.xer;
        case SPR_LR: return c.lr;
        case SPR_CTR: return c.ctr;
        case SPR_TBL_READ: return static_cast<u32>(c.time_base());
        case SPR_TBU_READ: return static_cast<u32>(c.time_base() >> 32);
        case SPR_DEC: return c.decrementer();
        default: return c.spr[number];
    }
}

inline void write_spr(Cpu& c, u32 number, u32 value) noexcept {
    switch (number) {
        case SPR_XER: c.xer = value; break;
        case SPR_LR: c.lr = value; break;
        case SPR_CTR: c.ctr = value; break;
        case SPR_TBL_WRITE: c.set_time_base((c.time_base() & 0xffffffff00000000ull) | value); break;
        case SPR_TBU_WRITE:
            c.set_time_base((c.time_base() & 0x00000000ffffffffull) | (static_cast<u64>(value) << 32));
            break;
        case SPR_DEC: c.set_decrementer(value); break;
        case SPR_HID2: c.write_hid2(value); break;
        default:
            // The BATs are plain storage here: GuestMemory's views decide
            // nothing by them (docs/memory-architecture-20261006/README.md D2).
            c.spr[number] = value;
            break;
    }
}

}  // namespace ppc
}  // namespace gekko
