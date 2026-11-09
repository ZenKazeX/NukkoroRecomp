#include "gekko/cpu.h"
#include "gekko/ppc_ops.h"
#include "gekko/savestate.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "gekko/cpu_diag.h"
#include "gekko/indirect_call_trace.h"
#include "gekko/audio_audit.h"
#include "gekko/ppc_instruction_traits.h"
#include <limits>
#include <chrono>
#include <thread>

#include "gekko/host_stats.h"

namespace gekko {
// Guest-thread owned; see host_stats.h.
HostPageStats& host_page_stats() noexcept {
    static HostPageStats stats;
    return stats;
}
}  // namespace gekko

namespace gekko {
#if defined(SHADOW_CPU_REFERENCE_BUILD)
namespace {

constexpr u32 kCoreClock = 486000000u;
constexpr u32 kBusClock = 162000000u;

inline u32 field_rd(u32 i) { return (i >> 21) & 31u; }
inline u32 field_ra(u32 i) { return (i >> 16) & 31u; }
inline u32 field_rb(u32 i) { return (i >> 11) & 31u; }
inline u32 field_rc_reg(u32 i) { return (i >> 6) & 31u; }
inline bool field_rc(u32 i) { return (i & 1u) != 0u; }
inline bool field_oe(u32 i) { return ((i >> 10) & 1u) != 0u; }
inline s32 field_simm(u32 i) { return static_cast<s16>(i & 0xffffu); }
inline u32 field_uimm(u32 i) { return i & 0xffffu; }

inline u32 rotl32(u32 value, u32 amount) {
    amount &= 31u;
    return amount == 0 ? value : ((value << amount) | (value >> (32u - amount)));
}

inline u32 ppc_mask(u32 mb, u32 me) {
    const u32 begin = 0xffffffffu >> mb;
    const u32 end = (me >= 31u) ? 0u : (0xffffffffu >> (me + 1u));
    const u32 mask = begin ^ end;
    return (me < mb) ? ~mask : mask;
}

inline s32 sign_extend(u32 value, u32 bits) {
    const u32 shift = 32u - bits;
    return static_cast<s32>(value << shift) >> shift;
}

// Signed interpretation of the 6-bit GQR scale field.
inline int gqr_scale(u32 raw) {
    return sign_extend(raw & 0x3fu, 6);
}

inline bool is_signalling_nan(u64 bits) {
    const bool all_ones = ((bits >> 52) & 0x7ffu) == 0x7ffu;
    const bool quiet = ((bits >> 51) & 1u) != 0u;
    const bool fraction = (bits & 0x000fffffffffffffull) != 0u;
    return all_ones && !quiet && fraction;
}

// fcmpu/fcmpo (gekko_user_manual.txt PAGE 397-398) and ps_cmpu0/1, ps_cmpo0/1
// (PAGE 503-506), one definition for all six: returns the result code written
// to CR field crfD and FPCC. Exception bits: an SNaN operand sets VXSNAN, plus
// VXVC for the ordered forms when VE = 0; otherwise a QNaN sets VXVC for the
// ordered forms only. FX is set only when an exception bit goes 0 -> 1, VX and
// FEX are recomputed as summaries (PAGE 473). FPSCR[C] is not written.
u32 compare_floating(u64 a_bits, u64 b_bits, bool ordered, u32& fpscr) {
    const double va = bits_to_double(a_bits);
    const double vb = bits_to_double(b_bits);
    u32 code;
    if (std::isnan(va) || std::isnan(vb)) {
        code = 1u;
        u32 raised = 0u;
        if (is_signalling_nan(a_bits) || is_signalling_nan(b_bits)) {
            raised = kFpscrVXSNAN;
            if (ordered && (fpscr & kFpscrVE) == 0u) raised |= kFpscrVXVC;
        } else if (ordered) {
            raised = kFpscrVXVC;
        }
        if (raised != 0u) {
            if ((fpscr & raised) != raised) fpscr |= kFpscrFX;
            fpscr |= raised;
            if ((fpscr & kFpscrVXAny) != 0u) {
                fpscr |= kFpscrVX;
            } else {
                fpscr &= ~kFpscrVX;
            }
            const bool enabled = ((fpscr & kFpscrVX) != 0u && (fpscr & kFpscrVE) != 0u) ||
                                 ((fpscr & kFpscrOX) != 0u && (fpscr & kFpscrOE) != 0u) ||
                                 ((fpscr & kFpscrUX) != 0u && (fpscr & kFpscrUE) != 0u) ||
                                 ((fpscr & kFpscrZX) != 0u && (fpscr & kFpscrZE) != 0u) ||
                                 ((fpscr & kFpscrXX) != 0u && (fpscr & kFpscrXE) != 0u);
            fpscr = enabled ? (fpscr | kFpscrFEX) : (fpscr & ~kFpscrFEX);
        }
    } else if (va < vb) {
        code = 8u;
    } else if (va > vb) {
        code = 4u;
    } else {
        code = 2u;
    }
    fpscr = (fpscr & ~kFpscrFPCC) | (code << 12);
    return code;
}

// Saturation for psq_st to an integer GQR type (PAGE 111): out-of-range values
// take the adjusted overflow value for their sign, and +Inf or NaN take the
// POSITIVE overflow value. Converting a NaN straight to an integer is undefined.
inline double quantise_saturate(double value, double lo, double hi) {
    if (std::isnan(value)) return hi;
    return std::clamp(value, lo, hi);
}

// ---- floating-point arithmetic: the reference formulation -------------------
//
// Audit fpu-1, fpu-2, fpu-3, paired-single-3. The rules are the ones written
// above ppc::fp_arith_slow (runtime/include/gekko/ppc_ops.h), from
// gekko_user_manual.txt PAGE 82-85 (Tables 2-21/2-22), 97, 403-415 and
// 508-510. This formulation shares no code with that one: no fast path, no
// class-key tests, library classification, and the exact residual comes from
// integer arithmetic instead of two-sum/fma expansions.

enum class RefFp { Add, Sub, Mul, Div, Sqrt, Res, Rsqrte, Round, MAdd, MSub, NMAdd, NMSub };

// A finite double as mantissa * 2^exponent.
void reference_decompose(double value, bool& negative, u64& mantissa, int& exponent) {
    const u64 bits = double_to_bits(value);
    negative = (bits >> 63) != 0u;
    const u32 biased = static_cast<u32>(bits >> 52) & 0x7ffu;
    const u64 fraction = bits & 0x000fffffffffffffull;
    if (biased == 0u) {
        mantissa = fraction;
        exponent = -1074;
    } else {
        mantissa = fraction | 0x0010000000000000ull;
        exponent = static_cast<int>(biased) - 1075;
    }
}

// 4,352 bits: a product of two mantissas spans exponents -2148..2048 and a
// double -1074..1024.
struct ReferenceBig {
    u64 limb[68]{};
};

void reference_add_shifted(ReferenceBig& big, u64 high, u64 low, int shift) {
    const int word = shift / 64;
    const int bit = shift % 64;
    const u64 parts[3] = {low << bit, bit == 0 ? high : ((high << bit) | (low >> (64 - bit))),
                          bit == 0 ? 0u : (high >> (64 - bit))};
    u64 carry = 0u;
    for (int index = word; index < 68; ++index) {
        const u64 add = index - word < 3 ? parts[index - word] : 0u;
        if (index - word >= 3 && carry == 0u) break;
        const u64 first = big.limb[index] + add;
        const u64 carry_first = first < add ? 1u : 0u;
        const u64 second = first + carry;
        const u64 carry_second = second < carry ? 1u : 0u;
        big.limb[index] = second;
        carry = carry_first + carry_second;
    }
}

// sign(x * y + z - w) for finite doubles, by exact integer sums.
int reference_exact_sign(double x, double y, double z, double w) {
    struct Term {
        bool negative;
        u64 high, low;
        int exponent;
    };
    Term terms[3];
    int count = 0;
    {
        bool nx, ny;
        u64 mx, my;
        int ex, ey;
        reference_decompose(x, nx, mx, ex);
        reference_decompose(y, ny, my, ey);
        if (mx != 0u && my != 0u) {
            const u64 x0 = mx & 0xffffffffu, x1 = mx >> 32, y0 = my & 0xffffffffu, y1 = my >> 32;
            const u64 p00 = x0 * y0, p01 = x0 * y1, p10 = x1 * y0, p11 = x1 * y1;
            const u64 middle = (p00 >> 32) + (p01 & 0xffffffffu) + (p10 & 0xffffffffu);
            terms[count++] = {nx != ny, p11 + (p01 >> 32) + (p10 >> 32) + (middle >> 32),
                              (middle << 32) | (p00 & 0xffffffffu), ex + ey};
        }
    }
    for (const double value : {z, -w}) {
        bool negative;
        u64 mantissa;
        int exponent;
        reference_decompose(value, negative, mantissa, exponent);
        if (mantissa != 0u) terms[count++] = {negative, 0u, mantissa, exponent};
    }
    if (count == 0) return 0;
    int lowest = terms[0].exponent;
    for (int index = 1; index < count; ++index) lowest = std::min(lowest, terms[index].exponent);
    ReferenceBig positive, negative;
    for (int index = 0; index < count; ++index) {
        reference_add_shifted(terms[index].negative ? negative : positive, terms[index].high,
                              terms[index].low, terms[index].exponent - lowest);
    }
    for (int index = 67; index >= 0; --index) {
        if (positive.limb[index] != negative.limb[index]) {
            return positive.limb[index] > negative.limb[index] ? 1 : -1;
        }
    }
    return 0;
}

// Rounds the double v to single precision. `direction()` is the sign of
// (exact value - v); it is asked only for a v exactly between two singles.
template <typename Direction>
double reference_round_single(double v, bool exact, const Direction& direction) {
    const float nearest = static_cast<float>(v);
    if (!exact || !std::isfinite(v) || static_cast<double>(nearest) == v) {
        return static_cast<double>(nearest);
    }
    const double magnitude = std::fabs(v);
    const float rounded = static_cast<float>(magnitude);
    double low, high;
    if (std::isinf(rounded)) {
        low = static_cast<double>(std::numeric_limits<float>::max());
        high = 0x1p128;  // round-to-nearest overflows from FLT_MAX + ulp/2
    } else if (static_cast<double>(rounded) < magnitude) {
        low = rounded;
        const float next = std::nextafter(rounded, std::numeric_limits<float>::infinity());
        high = std::isinf(next) ? 0x1p128 : static_cast<double>(next);
    } else {
        high = rounded;
        low = std::nextafter(rounded, 0.0f);
    }
    if (magnitude - low != high - magnitude) {
        return static_cast<double>(nearest);
    }
    const int sign = direction();
    if (sign == 0) {
        return static_cast<double>(nearest);  // the exact value is the midpoint: ties to even
    }
    const bool upward = std::signbit(v) ? sign < 0 : sign > 0;
    return std::copysign(static_cast<double>(static_cast<float>(upward ? high : low)), v);
}

// fres / frsqrte as the hardware tests state them: fres built as a single
// (23-bit fraction, single exponent 253 - e, denormal by shifting when NI is
// clear), frsqrte as a double with the exponent's oddness choosing the table
// half. Written apart from ppc::estimate's arithmetic; only the segment
// values (ppc::estimate::kReciprocal / kReciprocalRoot) are shared.
u64 reference_estimate(u32 fpscr, RefFp kind, bool single, u64 b_bits) {
    const double b = bits_to_double(b_bits);
    const bool negative = (b_bits >> 63) != 0u;
    const u32 biased = static_cast<u32>(b_bits >> 52) & 0x7ffu;
    if (kind == RefFp::Res) {
        const bool non_ieee = (fpscr & kFpscrNI) != 0u;
        if (biased <= 0x37eu) {
            if ((b_bits << 1) == 0u) return double_to_bits(negative ? -HUGE_VAL : HUGE_VAL);
            const double largest = static_cast<double>(std::numeric_limits<float>::max());
            return double_to_bits(negative ? -largest : largest);
        }
        if (biased >= (non_ieee ? 0x47du : 0x494u)) {
            if (std::isnan(b)) return b_bits | 0x0008000000000000ull;
            return negative ? 0x8000000000000000ull : 0u;
        }
        const u32 fraction = static_cast<u32>((b_bits & 0x000fffffffffffffull) >> 29);
        const u32 key = fraction >> 18;
        const s64 doubled_base = 2 * ppc::estimate::kReciprocal[key].base;
        const s64 doubled_dec = -ppc::estimate::kReciprocal[key].slope;
        const u32 mantissa = static_cast<u32>(doubled_base + doubled_dec * static_cast<s64>((fraction >> 8) & 0x3ffu)) >> 1;
        const int exponent = 253 - (static_cast<int>(biased) - 0x380);
        u32 single_bits = negative ? 0x80000000u : 0u;
        if (exponent > 0) {
            single_bits |= (static_cast<u32>(exponent) << 23) | mantissa;
        } else if (!non_ieee) {
            single_bits |= ((1u << 23) | mantissa) >> (1u + static_cast<u32>(-exponent));
        }
        float result;
        std::memcpy(&result, &single_bits, sizeof(result));
        return double_to_bits(static_cast<double>(result));
    }
    // frsqrte
    u64 result_bits;
    if ((b_bits << 1) == 0u) {
        result_bits = double_to_bits(negative ? -HUGE_VAL : HUGE_VAL);
    } else if (std::isinf(b)) {
        result_bits = negative ? 0x7ff8000000000000ull : 0u;
    } else if (std::isnan(b)) {
        result_bits = b_bits | 0x0008000000000000ull;
    } else if (negative) {
        result_bits = 0x7ff8000000000000ull;
    } else {
        // b = m * 2^p with m in [1, 2); a denormal is renormalised.
        int p = 0;
        const double m = std::frexp(b, &p) * 2.0;
        p -= 1;
        const u64 mantissa_bits = double_to_bits(m) & 0x000fffffffffffffull;
        const bool even_p = (p & 1) == 0;  // biased exponent odd
        const int i = static_cast<int>(mantissa_bits >> 37);
        const ppc::estimate::Segment& segment = ppc::estimate::kReciprocalRoot[i / 2048 + (even_p ? 16 : 0)];
        const s64 estimate = segment.base + segment.slope * (i % 2048);
        // 1/sqrt(2^p) = 2^(-p/2): result exponent floor(-p/2) - 1 when the
        // fraction comes from the 2^26-scaled segment of the odd half.
        const int result_exponent = even_p ? -(p / 2) - 1 : -((p + 1) / 2);
        result_bits = (static_cast<u64>(result_exponent + 1023) << 52) | (static_cast<u64>(estimate) << 26);
    }
    if (!single || std::isnan(bits_to_double(result_bits))) {
        return single ? double_to_bits(static_cast<double>(static_cast<float>(bits_to_double(result_bits))))
                      : result_bits;
    }
    const double rounded = static_cast<double>(static_cast<float>(bits_to_double(result_bits)));
    if ((fpscr & kFpscrNI) != 0u && rounded != 0.0 && std::fabs(rounded) < 0x1p-126) {
        return double_to_bits(std::copysign(0.0, rounded));
    }
    return double_to_bits(rounded);
}

u64 reference_fp(u32 fpscr, RefFp kind, bool single, u64 a_bits, u64 b_bits, u64 c_bits) {
    if (kind == RefFp::Res || kind == RefFp::Rsqrte) {
        if (std::isnan(bits_to_double(b_bits))) {
            const u64 quiet = b_bits | 0x0008000000000000ull;
            return single ? double_to_bits(static_cast<double>(static_cast<float>(bits_to_double(quiet))))
                          : quiet;
        }
        return reference_estimate(fpscr, kind, single, b_bits);
    }
    const bool fused = kind == RefFp::MAdd || kind == RefFp::MSub || kind == RefFp::NMAdd ||
                       kind == RefFp::NMSub;
    const bool has_a = fused || kind == RefFp::Add || kind == RefFp::Sub || kind == RefFp::Mul ||
                       kind == RefFp::Div;
    const bool has_b = kind != RefFp::Mul;
    const bool has_c = fused || kind == RefFp::Mul;

    // NaN operands, frA before frB before frC: quieted, sign kept, not negated.
    const u64 operands[3] = {a_bits, b_bits, c_bits};
    const bool present[3] = {has_a, has_b, has_c};
    for (int index = 0; index < 3; ++index) {
        if (present[index] && std::isnan(bits_to_double(operands[index]))) {
            const u64 quiet = operands[index] | 0x0008000000000000ull;
            return single ? double_to_bits(static_cast<double>(static_cast<float>(bits_to_double(quiet))))
                          : quiet;
        }
    }

    // FPSCR[NI], Table 2-21: denormalised operands are zeros of their sign.
    const bool non_ieee = (fpscr & kFpscrNI) != 0u;
    const auto operand = [&](u64 bits) {
        const double value = bits_to_double(bits);
        if (non_ieee) {
            const bool denormal = single ? (value != 0.0 && std::fabs(value) < 0x1p-126)
                                         : std::fpclassify(value) == FP_SUBNORMAL;
            if (denormal) return std::copysign(0.0, value);
        }
        return value;
    };
    const double va = operand(a_bits), vb = operand(b_bits), vc = operand(c_bits);
    const double addend = (kind == RefFp::MSub || kind == RefFp::NMSub) ? -vb : vb;
    double v = 0.0;
    switch (kind) {
        case RefFp::Add: v = va + vb; break;
        case RefFp::Sub: v = va - vb; break;
        case RefFp::Mul: v = va * vc; break;
        case RefFp::Div: v = va / vb; break;
        case RefFp::Sqrt: v = std::sqrt(vb); break;
        case RefFp::Round: v = vb; break;
        default: v = std::fma(va, vc, addend); break;
    }
    if (std::isnan(v)) return 0x7ff8000000000000ull;  // generated QNaN, sign 0

    // sign(exact - v) where the exact value is defined (fused; double mul/div).
    const auto direction = [&]() {
        if (fused) return reference_exact_sign(va, vc, addend, v);
        if (single) return 0;
        if (kind == RefFp::Mul) return reference_exact_sign(va, vc, 0.0, v);
        if (kind == RefFp::Div) {
            const int remainder = reference_exact_sign(-v, vb, va, 0.0);  // a - v*b
            return std::signbit(vb) ? -remainder : remainder;
        }
        return 0;
    };

    double result;
    const double smallest_normal = single ? 0x1p-126 : 0x1p-1022;
    const bool tiny = non_ieee && std::isfinite(v) &&
                      (std::fabs(v) < smallest_normal ||
                       (std::fabs(v) == smallest_normal &&
                        (std::signbit(v) ? direction() > 0 : direction() < 0)));
    if (tiny) {
        result = std::copysign(0.0, v);  // Table 2-22: denormalised result -> zero
    } else if (single) {
        result = reference_round_single(v, fused, direction);
    } else {
        result = v;
    }
    if (kind == RefFp::NMAdd || kind == RefFp::NMSub) result = -result;
    return double_to_bits(result);
}

}  // namespace
#endif

// C2: zero-initialised static storage (.bss), never written.
Cpu::Cpu(GuestMemory& memory)
    : memory_(memory),
      fm_window_(memory.fm_window()),
      fm_null_window_(memory.fm_null_window()) {
    fm_register(this);  // FM: the fault handler can now resolve this machine's windows (flat_memory.cpp)
    set_msr(msr_);  // C2: select the page table for the constructed MSR (0: none)
    // Journalling, the poll watch and the access trace re-decide whether the
    // page table may serve. Registered under this Cpu's own identity: another
    // Cpu on the same GuestMemory keeps its own listener, and the destructor
    // removes only this one (audit concurrency-4).
    memory_.add_translation_listener(this, [this]() { select_memory_window(); });
    history_.resize(256);
    reset();
}

Cpu::~Cpu() {
    fm_unregister(this);
    memory_.remove_translation_listener(this);
}

void Cpu::reset() {
    // A reset Cpu must execute exactly like a freshly constructed one (audit
    // state-determinism-6). Everything below is either architected state or
    // execution bookkeeping that decides WHEN guest-visible events happen or
    // what the per-run reports say; all of it gets its constructor value.
    // Host wiring and configuration survive: guest memory, the fifo pipe, attached
    // recompiled code, the idle handler, HLE hooks, verification/trace/history
    // switches and ring capacities.
    std::fill(std::begin(gpr), std::end(gpr), 0u);
    for (auto& reg : fpr) {
        reg.bits[0] = 0;
        reg.bits[1] = 0;
    }
    spr.fill(0);
    sr.fill(0);
    pc = npc = 0;
    lr = ctr = xer = cr = fpscr = 0;
    stop_ = CpuStop{};
    interrupt_lines_ = 0u;
    reservation = false;  // hard reset clears the reservation flag
    reservation_address = 0;
    history_head_ = 0;
    history_size_ = 0;

    // Game time restarts at zero with nothing scheduled (the owners of the
    // other events schedule them again when the machine starts); the time base
    // and decrementer views restart with it.
    clock_ = GameClock{};
    end_poll_tracking();
    dec_value_ = 0;
    dec_written_at_ = 0;
    time_base_offset_ = 0;
    // Dispatch state a previous run (or a request_stop outside run) left.
    exit_pending_ = 0u;
    pending_native_call_.reset();
    recognise_interrupt_ = false;
    stop_requested_.store(false, std::memory_order_relaxed);
    // A host stop posted for the run that ended is not part of a fresh
    // machine. Claimed like a post (2 -> 1), so a concurrent poster cannot
    // write the detail while it is cleared; a post still in progress
    // completes and stops the next run, as if posted after the reset.
    u8 posted = 2u;
    if (host_stop_state_.compare_exchange_strong(posted, u8{1}, std::memory_order_acq_rel)) {
        host_stop_reason_ = StopReason::Running;
        host_stop_detail_.clear();
        host_stop_state_.store(u8{0}, std::memory_order_release);
    }
    verify_armed_entry_ = 0u;
    verify_replaying_ = false;
    memory_fault_ = MemoryFault{};
    // Exception FPR context.
    interrupt_fpr_depth_ = 0;
    interrupt_fpr_resume_ = 0;
    std::fill(std::begin(interrupt_fpr_), std::end(interrupt_fpr_), FpReg{});
    interrupt_fpscr_ = 0;
    interrupt_fpr_context_switches_ = 0;
    // Per-run reports keyed to the instruction timeline that just restarted.
    recompiled_calls = 0;
    interpreter_exits = 0;
    dispatch_blocked_interrupt = 0;
    dispatch_blocked_hle = 0;
    dispatch_no_native_code = 0;
    dispatch_entered_mid_function = 0;
    fallback_totals_.fill(0);
    fallback_sites_.clear();
    strand_pc_ = 0;
    strand_lr_ = 0;
    strand_start_instruction_ = 0;
    interpreter_strands_.clear();
    interpreted_instructions_ = 0;
    interpreted_ns_ = 0;
    verify_ = VerifyReport{};
    for (HleEntry& entry : hle_) entry.hits = 0;
    hle_slow_calls_ = 0;

    // Gekko processor version: 0x00083214 on retail GameCube hardware.
    spr[SPR_PVR] = 0x00083214u;
    spr[SPR_HID0] = 0x0011c464u;
    spr[SPR_HID1] = 0x80000000u;
    // HID2 = LSQE | WPE | PSE (PAGE 67 Table 2-6): paired singles, quantised
    // load/store and the write gather pipe enabled. LCE (0x10000000) is clear,
    // as at power-on (PAGE 310), so dcbz_l raises the illegal-instruction
    // program exception until the guest sets it (audit paired-single-10).
    spr[SPR_HID2] = 0xe0000000u;
    spr[SPR_L2CR] = 0x80000000u;
    // DEC is a view of the game clock (decrementer()); its spr[] slot is never
    // read or written. Power-on value, with its sign edge armed.
    set_decrementer(0xffffffffu);

    // The IPL's block address translation values, as the guest may read them
    // back. Translation does not use them (GuestMemory's views, README D2).
    spr[SPR_IBAT0U] = 0x80001fffu;
    spr[SPR_IBAT0U + 1] = 0x00000002u;
    spr[SPR_DBAT0U] = 0x80001fffu;
    spr[SPR_DBAT0U + 1] = 0x00000002u;
    spr[SPR_DBAT0U + 2] = 0xc0001fffu;
    spr[SPR_DBAT0U + 3] = 0x0000002au;

    set_msr(kMsrFP | kMsrME | kMsrIR | kMsrDR | kMsrRI);

    // The machine was just reset (this also re-derives the FIFO
    // routes), and the fault latch was just cleared.
    select_memory_window();
    rebuild_hle_slots();
}

void Cpu::set_entry(u32 address) {
    pending_native_call_.reset();
    pc = address;
    npc = address + 4u;
}

void Cpu::set_history_depth(std::size_t depth) {
    history_.assign(depth, TraceEntry{0, 0});
    history_head_ = 0;
    history_size_ = 0;
}

std::vector<Cpu::TraceEntry> Cpu::history() const {
    std::vector<TraceEntry> result;
    if (history_.empty()) {
        return result;
    }
    result.reserve(history_size_);
    const std::size_t capacity = history_.size();
    const std::size_t start = (history_head_ + capacity - history_size_) % capacity;
    for (std::size_t index = 0; index < history_size_; ++index) {
        result.push_back(history_[(start + index) % capacity]);
    }
    return result;
}

void Cpu::rebuild_hle_slots() noexcept {
    hle_index_.clear();
    hle_index_.reserve(hle_.size());
    for (std::size_t i = 0; i < hle_.size(); ++i) {
        hle_index_[hle_[i].address] = i;
    }
    // The old 8-address hot list was permanently incomplete once System
    // installed its 11 unconditional hooks, so every translated call paid an
    // out-of-line try_hle_slow and a binary search (audit
    // dispatch-exceptions-3). A slot is exact for any hook count: it names
    // the single hooked address that maps to it, or sends the whole slot to
    // the slow path when two hooks share it.
    //
    // While verifying, every thunk must reach try_hle_slow (the per-call cut);
    // while a data-access fault is latched, every call must unwind. Both fill
    // every slot with kHleSlotAll, so the inline test needs no extra flag.
    const bool all = verifying_ || memory_fault_.latched;
    hle_slots_all_ = all;
    hle_shared_.clear();
    std::fill(std::begin(hle_slots_), std::end(hle_slots_), all ? kHleSlotAll : kHleSlotEmpty);
    if (all) return;
    for (std::size_t i = 0; i < hle_.size(); ++i) {
        const HleEntry& entry = hle_[i];
        const u32 index = (entry.address >> 2) & kHleSlotMask;
        u32& slot = hle_slots_[index];
        const bool reserved_value = entry.address == kHleSlotEmpty || entry.address == kHleSlotAll;
        slot = (slot == kHleSlotEmpty && !reserved_value) ? entry.address : kHleSlotAll;
        hle_slot_entry_[index] = static_cast<u32>(i);  // read only while the slot names this address
    }
    // Each shared slot's hooks, listed together: one pass over the hooks, then grouped by slot (install_hle rebuilds
    // per hook, so this stays linear in the hook count).
    std::vector<std::pair<u32, u32>> shared;  // (slot, hle_ index)
    for (std::size_t i = 0; i < hle_.size(); ++i) {
        const u32 index = (hle_[i].address >> 2) & kHleSlotMask;
        if (hle_slots_[index] == kHleSlotAll) shared.emplace_back(index, static_cast<u32>(i));
    }
    std::sort(shared.begin(), shared.end());
    for (std::size_t k = 0; k < shared.size();) {
        const u32 index = shared[k].first, first = static_cast<u32>(hle_shared_.size());
        for (; k < shared.size() && shared[k].first == index; ++k) hle_shared_.push_back(shared[k].second);
        hle_slot_entry_[index] = (first << 8) | (static_cast<u32>(hle_shared_.size()) - first);
    }
}

void Cpu::install_hle(u32 address, HleHandler handler, std::string name) {
    for (auto& entry : hle_) {
        if (entry.address == address) {
            entry.handler = std::move(handler);
            entry.name = std::move(name);
            rebuild_hle_slots();
            return;
        }
    }
    hle_.push_back(HleEntry{address, std::move(handler), std::move(name)});
    std::sort(hle_.begin(), hle_.end(),
              [](const HleEntry& a, const HleEntry& b) { return a.address < b.address; });
    rebuild_hle_slots();
}

const Cpu::HleEntry* Cpu::find_hle(u32 address) const {
    // A slot that names exactly this address holds its entry index (rebuild_hle_slots); any other slot value - empty,
    // another address, kHleSlotAll - leaves the answer to the exact map.
    const u32 slot = (address >> 2) & kHleSlotMask;
    const u32 named = hle_slots_[slot];
    // The sentinels (1, 3) are not word-aligned and never name an address: a pc equal to one goes to the map.
    if (named == address && named > kHleSlotAll) {
        return &hle_[hle_slot_entry_[slot]];
    }
    // Any slot but kHleSlotAll is exact (rebuild_hle_slots): empty, or naming the one OTHER hooked address of this
    // slot, proves this address is not hooked. The dispatcher asks for every pc it enters, so the common unhooked
    // answer must not pay the hash (2026-11-04: find_hle 0.074 ms/frame). Verification and a latched fault fill every
    // slot with kHleSlotAll, so both still take the exact map below.
    if (named != kHleSlotAll) {
        return nullptr;
    }
    // A shared slot: its few hooks, exactly (rebuild_hle_slots); the verify / fault fill has no lists - the map.
    if (!hle_slots_all_) {
        const u32 packed = hle_slot_entry_[slot];
        for (u32 k = packed >> 8, end = (packed >> 8) + (packed & 0xFFu); k < end; ++k)
            if (hle_[hle_shared_[k]].address == address) return &hle_[hle_shared_[k]];
        return nullptr;
    }
    const auto it = hle_index_.find(address);
    if (it == hle_index_.end()) {
        return nullptr;
    }
    return &hle_[it->second];
}

bool Cpu::serve_owner(u32 address) {
    const HleEntry* entry = find_hle(address);
    return entry != nullptr && entry->handler(*this);
}

bool Cpu::has_hle(u32 address) const { return find_hle(address) != nullptr; }

const std::string* Cpu::hle_name(u32 address) const {
    const HleEntry* entry = find_hle(address);
    return entry ? &entry->name : nullptr;
}

bool Cpu::try_hle_slow(u32 address) {
    // Per-call verification cut. Every generated entry thunk calls this first,
    // which makes it the one runtime choke point every direct native call
    // passes through. Under --verify-recomp the dispatcher must be the
    // boundary of every guest call, so the differential oracle verifies each
    // call-to-call segment instead of one giant subtree per dispatch: the
    // direct native-to-native transfers (interior targets, loop heads) had
    // silently reduced the oracle to 633 dispatch boundaries on a
    // 400M-instruction route, of which 34 unverifiable device-touching
    // subtrees carried essentially every instruction. The armed flag marks the
    // one entry the dispatcher itself chose; any other thunk reached while
    // verifying is a nested call and hands back to become its own verified
    // segment. Returns are deliberately not cut - a callee's return lands in
    // the caller's resume thunk inside the same segment - which keeps segment
    // count near the guest's call count rather than exploding per transfer.
    // A hooked address is ALWAYS cut before its handler while verifying, armed
    // or not. Handlers carry host state the journal cannot roll back (the AX
    // mixer's voice fractions, probe counters), so the oracle must never run
    // one inside a pass that is later discarded or replayed. This used to run
    // the handler first and cut afterwards, and the dispatcher's hook path and
    // the replay's step() then ran it again - up to three times per call. The
    // cut hands the call to the dispatcher (Cpu::run), which runs the handler
    // exactly once outside the oracle and verifies a declined call's
    // translated body as its own segment.
    ++hle_slow_calls_;
    // A latched data-access fault halts the machine: a call made by the native
    // code still unwinding from the fault must not run its callee (or a
    // handler). Returning "served" makes the thunk return at once, and the
    // caller's `if (c.exiting()) return;` unwinds further. pc is left as the
    // fault recorded it. rebuild_hle_slots routes every call here while
    // latched, so the inline filter pays nothing for this.
    if (memory_fault_.latched) {
        return true;
    }
#if defined(SHADOW_CPU_REFERENCE_BUILD)
    if (verifying_ && !verify_replaying_ && handler_depth_ == 0u) {
        if (verify_armed_entry_ == address && address != 0u &&
            (hle_.empty() || find_hle(address) == nullptr)) {
            verify_armed_entry_ = 0u;
            return false;
        }
        // Fast mode outside a checked pass: cut only a call that is sampled; every other call runs as it would
        // without the verifier (its hook below included - no pass will be discarded or replayed around it).
        if (!verify_sampling_.enabled || verify_in_pass_) {
            leave_to_dispatcher(address, FallbackReason::VerifyCut, address);
            return true;
        }
        // A hook outside the scope is never a checked pass (the dispatcher serves it), so it is no arrival.
        const bool hooked = !hle_.empty() && find_hle(address) != nullptr;
        if ((!hooked || in_verify_scope(address)) && verify_sample_arrival(address)) {
            verify_sampled_entry_ = address;
            leave_to_dispatcher(address, FallbackReason::VerifyCut, address);
            return true;
        }
    }
#endif
    if (!hle_.empty()) {
        const HleEntry* entry = find_hle(address);
#if defined(SHADOW_CPU_REFERENCE_BUILD)
        // Fast verification serves unsampled owners here, inline: the guest calls the handler makes are part of its
        // effect and must not be sampled and cut away from it (the dispatcher's hook path has the same guard).
        ++handler_depth_;
        const bool served = entry != nullptr && entry->handler(*this);
        --handler_depth_;
#else
        const bool served = entry != nullptr && entry->handler(*this);
#endif
        if (served) {
            ++const_cast<HleEntry*>(entry)->hits;
            // Same contract as the interpreter: a handler that fully served the
            // call has left results in architected state and execution resumes
            // at LR.
            if ((exit_pending_ & kExitLeave) == 0u && !stopped()) pc = lr;
            return true;
        }
    }
    return false;
}

const LiftSiteRecord* Cpu::find_lift_site(u32 address) const noexcept {
    const LiftSiteRecord* const end = lift_sites_ + lift_site_count_;
    const LiftSiteRecord* site = std::lower_bound(
        lift_sites_, end, address, [](const LiftSiteRecord& record, u32 key) { return record.address < key; });
    return site != end && site->address == address ? site : nullptr;
}

// The cut a translated pass takes at a lowered call site: the same rules as
// the per-call cut before an owner (try_hle_slow). A site calls a native
// subsystem whose state the oracle's journal cannot roll back, so while
// verifying it never runs inside a pass that may be replayed: the pass ends
// here and the dispatcher runs the site once (run_lift_site_at_pc). While a
// data-access fault is latched the site must not run at all.
bool Cpu::lift_site_cut_slow(u32 site) {
    if (memory_fault_.latched) {
        return true;
    }
#if defined(SHADOW_CPU_REFERENCE_BUILD)
    if (verifying_ && !verify_replaying_ && handler_depth_ == 0u && (!verify_sampling_.enabled || verify_in_pass_)) {
        leave_to_dispatcher(site, FallbackReason::VerifyCut, site);
        return true;
    }
#else
    (void)site;
#endif
    return false;
}

bool Cpu::run_lift_site_at_pc() {
    if (lift_site_count_ == 0u || !maybe_lift_site(pc)) return false;
    const LiftSiteRecord* const site = find_lift_site(pc);
    if (site == nullptr) return false;
    ++lift_sites_dispatched_;
    site->lower(*this);
    if (site->continuation == LiftSiteContinuation::Sequential) pc = site->address + 4u;
    npc = pc;
    return true;
}

void Cpu::serve_native_owner(u32 address) {
    // The owner's slot names exactly this address (rebuild_hle_slots; verification and a latched fault fill every slot
    // with kHleSlotAll, so neither reaches here): call the handler straight from the flat slot table - no find_hle,
    // no second filter, no try_hle_slow frame. The accounting is try_hle_slow's, in the same order.
    const u32 slot = (address >> 2) & kHleSlotMask;
    if (hle_slots_[slot] == address && address > kHleSlotAll && !hle_slots_all_) {
        HleEntry& entry = hle_[hle_slot_entry_[slot]];
        ++hle_slow_calls_;
#if defined(SHADOW_CPU_REFERENCE_BUILD)
        ++handler_depth_;
        const bool served = entry.handler(*this);
        --handler_depth_;
#else
        const bool served = entry.handler(*this);
#endif
        if (served) {
            ++entry.hits;
            if ((exit_pending_ & kExitLeave) == 0u && !stopped()) pc = lr;
            return;
        }
    } else if (try_hle(address)) {
        return;
    }
    char detail[192];
    std::snprintf(detail, sizeof(detail),
                  "native owner of 0x%08X is missing or declined; the original body is not in this image",
                  address);
    request_stop(StopReason::ExecutionFault, detail);
    leave_to_dispatcher(address, FallbackReason::NotRecompiled, address);
}

void Cpu::request_stop(StopReason reason, const std::string& detail) {
    // Raise the stop flag first, ahead of the first-writer-wins guard.
    // needs_dispatch() - the only thing a translated loop back edge tests -
    // reads it, so a guest loop that makes no calls and has interrupts masked
    // still notices the stop and leaves through leave_to_dispatcher(Yield) with
    // guest state committed.
    stop_requested_.store(true, std::memory_order_relaxed);
    if (stop_.reason != StopReason::Running) {
        return;
    }
    stop_.reason = reason;
    stop_.address = pc;
    stop_.detail = detail;
}

void Cpu::request_stop_from_host(StopReason reason, std::string detail) {
    // First poster wins; the request is complete (state 2) before the flag
    // the guest polls goes up, so the dispatcher never sees a half-written one.
    u8 expected = 0u;
    if (!host_stop_state_.compare_exchange_strong(expected, u8{1}, std::memory_order_acq_rel)) {
        return;
    }
    host_stop_reason_ = reason;
    host_stop_detail_ = std::move(detail);
    host_stop_state_.store(u8{2}, std::memory_order_release);
    stop_requested_.store(true, std::memory_order_release);
}

void Cpu::serve_host_stop() {
    std::string detail = std::move(host_stop_detail_);
    const StopReason reason = host_stop_reason_;
    host_stop_detail_.clear();
    host_stop_state_.store(u8{0}, std::memory_order_release);
    detail += "; guest ";
    detail += guest_location();
    request_stop(reason, detail);
}

std::string Cpu::guest_location(unsigned max_frames) {
    char row[96];
    std::snprintf(row, sizeof(row), "pc=0x%08X lr=0x%08X ctr=0x%08X r1=0x%08X back chain:", pc, lr,
                  ctr, gpr[1]);
    std::string out = row;
    // PowerPC EABI: [sp] is the caller's sp, and a function saves its LR in
    // its caller's frame at +4. Stops at a null, misaligned or descending link.
    u32 sp = gpr[1];
    for (unsigned frame = 0; frame < max_frames; ++frame) {
        const u32 back = read_guest_u32(sp);
        if (back == 0u || (back & 3u) != 0u || back <= sp) break;
        std::snprintf(row, sizeof(row), " 0x%08X", read_guest_u32(back + 4u));
        out += row;
        sp = back;
    }
    return out;
}

void Cpu::set_cr_field(u32 field, u32 value) {
    const u32 shift = 28u - 4u * field;
    cr = (cr & ~(0xfu << shift)) | ((value & 0xfu) << shift);
}

u32 Cpu::cr_field(u32 field) const { return (cr >> (28u - 4u * field)) & 0xfu; }

#if defined(SHADOW_CPU_REFERENCE_BUILD)
void Cpu::update_cr0(u32 value) {
    const s32 signed_value = static_cast<s32>(value);
    u32 flags = signed_value < 0 ? 8u : (signed_value > 0 ? 4u : 2u);
    if ((xer & kXerSO) != 0u) {
        flags |= 1u;
    }
    set_cr_field(0, flags);
}

void Cpu::update_cr1() { set_cr_field(1, (fpscr >> 28) & 0xfu); }

void Cpu::set_xer_carry(bool carry) {
    xer = carry ? (xer | kXerCA) : (xer & ~kXerCA);
}

void Cpu::set_xer_overflow(bool overflow) {
    if (overflow) {
        xer |= kXerOV | kXerSO;
    } else {
        xer &= ~kXerOV;
    }
}
#endif

// ---------------------------------------------------------------------------
// Address translation
// ---------------------------------------------------------------------------

bool Cpu::translate(u32 effective, bool is_write, bool is_instruction, u32& address) {
    (void)is_write;
    const bool enabled = is_instruction ? ((msr_ & kMsrIR) != 0u) : ((msr_ & kMsrDR) != 0u);
    if (!enabled) return false;
    address = effective;
    return true;
}

u8* Cpu::memory_access(u32 address, u32 size, bool writing) {
    // While GuestMemory journals (the oracle, the poll-point watch) every
    // access goes through its read* / write*, which record the journal.
    if (size == 0u || memory_.journaling()) return nullptr;
    const u8* const host = memory_.peek_pointer(address, size);
    if (host == nullptr) return nullptr;
    if (memory_.access_traced()) memory_.trace_access(address, size, writing);
    return const_cast<u8*>(host);
}

// Cold. Split out of the eight accessors so the success path does not carry the
// std::string temporary's /GS cookie, __security_check_cookie call and unwind
// state.
GEKKO_NOINLINE void Cpu::translation_fault_read(u32 effective) {
    take_memory_fault(effective, false);
}

GEKKO_NOINLINE void Cpu::translation_fault_write(u32 effective) {
    take_memory_fault(effective, true);
}

// A data access that cannot be served: the guest turned data translation off
// (audit memory-model-3), or - with unmapped-access stops on, as production
// runs - the window has nothing at the address (Cpu::unmapped_access: a null
// pointer, a released range, a retired device page).
//
// Before this, each fault rewrote DAR unconditionally (request_stop's
// first-writer-wins guarded only the stop report), the accessor returned 0 or
// dropped the store, and the translated code kept running to its next back
// edge or call boundary: a later fault overwrote DAR, and later stores and
// MMIO accesses committed guest effects after the faulting instruction.
//
// The hardware takes a precise DSI here. That is not reproducible in this
// core: generated bodies publish pc only at back edges, calls and returns,
// so the faulting instruction's address does not exist at the access, and
// unwinding the native frames
// precisely would need a check after every guest access in the 48 shards.
// What the core does instead, all of it outside the leaf fast paths:
//   * the FIRST fault latches DAR, DSISR and memory_fault_; nothing rewrites
//     them until reset() or load_state() replaces the machine;
//   * the host page table is dropped and refused while latched, so every
//     later guest access in the unwinding native code reaches *_slow, which
//     suppresses it (reads 0, stores dropped: no RAM, MMIO or FIFO
//     effect); the FIFO routes are emptied for the same reason;
//   * every HLE slot is forced to the slow path, where a latched fault makes
//     every call unwind without running its callee;
//   * the stop is raised (and run() re-raises it after a clear_stop) with the
//     faulting effective address in its detail.
// Register-only instructions between the fault and the next boundary still
// execute; they cannot reach memory or a device.
void Cpu::take_memory_fault(u32 effective, bool writing) {
    latch_memory_fault(effective, writing, false);
    restate_memory_fault();
}

void Cpu::latch_memory_fault(u32 effective, bool writing, bool unmapped) {
    if (memory_fault_.latched) return;
    memory_fault_.latched = true;
    memory_fault_.write = writing;
    memory_fault_.unmapped = unmapped;
    memory_fault_.effective = effective;
    memory_fault_.pc = pc;
    spr[SPR_DAR] = effective;
    spr[SPR_DSISR] = kDsisrNoTranslation | (writing ? kDsisrStore : 0u);
    select_memory_window();
    rebuild_hle_slots();
}

void Cpu::restate_memory_fault() {
    char detail[160];
    if (memory_fault_.unmapped) {
        std::snprintf(detail, sizeof(detail),
                      "data %s at 0x%08X reached no memory of the game's address window (pc 0x%08X)",
                      memory_fault_.write ? "write" : "read", memory_fault_.effective, memory_fault_.pc);
    } else {
        std::snprintf(detail, sizeof(detail),
                      "data %s at 0x%08X with data translation off: real-mode (GameCube physical) addressing "
                      "does not exist here (pc 0x%08X)",
                      memory_fault_.write ? "write" : "read", memory_fault_.effective, memory_fault_.pc);
    }
    const u32 saved_pc = pc;
    // The stop report names the pc the fault was taken at, even when run()
    // restates it later from a different pc.
    pc = memory_fault_.pc;
    request_stop(memory_fault_.unmapped ? StopReason::ExecutionFault : StopReason::TranslationFault, detail);
    pc = saved_pc;
}

void Cpu::select_memory_window() noexcept {
    // The journal (the oracle and the poll watch) and an access trace must see
    // every access; a latched data fault must reach the suppressing slow path.
    serving_pages_ = !memory_.journaling() && !memory_.access_traced() && !memory_fault_.latched;
    set_msr(msr_);
    ++host_page_stats().invalidations;
}

u8 Cpu::mem_read8_slow(u32 effective) {
    if (memory_fault_.latched) return 0;  // halted by a data fault: suppressed
    u32 address = 0;
    if (!translate(effective, false, false, address)) {
        translation_fault_read(effective);
        return 0;
    }
    if (const u8* host = memory_access(address, 1u, false)) {
        return *host;
    }
    if (unserved(address, 1u)) {
        unmapped_access(effective, 1u, false, 0u);
        return 0;
    }
    return memory_.read8(address);
}

u16 Cpu::mem_read16_slow(u32 effective) {
    if (memory_fault_.latched) return 0;  // halted by a data fault: suppressed
    u32 address = 0;
    if (!translate(effective, false, false, address)) {
        translation_fault_read(effective);
        return 0;
    }
    if (const u8* host = memory_access(address, 2u, false)) {
        return load_be16(host);
    }
    if (unserved(address, 2u)) {
        unmapped_access(effective, 2u, false, 0u);
        return 0;
    }
    return memory_.read16(address);
}

u32 Cpu::mem_read32_slow(u32 effective) {
    if (memory_fault_.latched) return 0;  // halted by a data fault: suppressed
    u32 address = 0;
    if (!translate(effective, false, false, address)) {
        translation_fault_read(effective);
        return 0;
    }
    if (const u8* host = memory_access(address, 4u, false)) {
        return load_be32(host);
    }
    if (unserved(address, 4u)) {
        unmapped_access(effective, 4u, false, 0u);
        return 0;
    }
    return memory_.read32(address);
}

u64 Cpu::mem_read64_slow(u32 effective) {
    if (memory_fault_.latched) return 0;  // halted by a data fault: suppressed
    u32 address = 0;
    if (!translate(effective, false, false, address)) {
        translation_fault_read(effective);
        return 0;
    }
    if (const u8* host = memory_access(address, 8u, false)) {
        return load_be64(host);
    }
    if (unserved(address, 8u)) {
        unmapped_access(effective, 8u, false, 0u);
        return 0;
    }
    return memory_.read64(address);
}

void Cpu::mem_write8_slow(u32 effective, u8 value) {
    if (memory_fault_.latched) return;  // halted by a data fault: suppressed
    u32 address = 0;
    if (!translate(effective, true, false, address)) {
        translation_fault_write(effective);
        return;
    }
    if (u8* host = memory_access(address, 1u, true)) {
        *host = value;
        return;
    }
    if ((address & kGxPipePageMask) == kGxPipePage && gx_sink_ != nullptr) {
        unlisted_gx_store(address, value, 1u);
        return;
    }
    if (unserved(address, 1u)) {
        unmapped_access(effective, 1u, true, value);
        return;
    }
    memory_.write8(address, value);
}

void Cpu::mem_write16_slow(u32 effective, u16 value) {
    if (memory_fault_.latched) return;  // halted by a data fault: suppressed
    u32 address = 0;
    if (!translate(effective, true, false, address)) {
        translation_fault_write(effective);
        return;
    }
    if (u8* host = memory_access(address, 2u, true)) {
        store_be16(host, value);
        return;
    }
    if ((address & kGxPipePageMask) == kGxPipePage && gx_sink_ != nullptr) {
        unlisted_gx_store(address, value, 2u);
        return;
    }
    if (unserved(address, 2u)) {
        unmapped_access(effective, 2u, true, value);
        return;
    }
    memory_.write16(address, value);
}

void Cpu::mem_write32_slow(u32 effective, u32 value) {
    if (memory_fault_.latched) return;  // halted by a data fault: suppressed
    u32 address = 0;
    if (!translate(effective, true, false, address)) {
        translation_fault_write(effective);
        return;
    }
    if (u8* host = memory_access(address, 4u, true)) {
        store_be32(host, value);
        return;
    }
    if ((address & kGxPipePageMask) == kGxPipePage && gx_sink_ != nullptr) {
        unlisted_gx_store(address, value, 4u);
        return;
    }
    if (unserved(address, 4u)) {
        unmapped_access(effective, 4u, true, value);
        return;
    }
    memory_.write32(address, value);
}

void Cpu::mem_write64_slow(u32 effective, u64 value) {
    if (memory_fault_.latched) return;  // halted by a data fault: suppressed
    u32 address = 0;
    if (!translate(effective, true, false, address)) {
        translation_fault_write(effective);
        return;
    }
    if (u8* host = memory_access(address, 8u, true)) {
        store_be64(host, value);
        return;
    }
    if ((address & kGxPipePageMask) == kGxPipePage && gx_sink_ != nullptr) {
        unlisted_gx_store(address, value, 8u);
        return;
    }
    if (unserved(address, 8u)) {
        unmapped_access(effective, 8u, true, value);
        return;
    }
    memory_.write64(address, value);
}

// ---------------------------------------------------------------------------
// GX pipe sites (gekko/gx_sites.h, extract/gx-sites.txt)
// ---------------------------------------------------------------------------

const GxSiteRecord* Cpu::find_gx_site(u32 address) const noexcept {
    const GxSiteRecord* const end = gx_sites_ + gx_site_count_;
    const GxSiteRecord* site = std::lower_bound(
        gx_sites_, end, address, [](const GxSiteRecord& record, u32 key) { return record.address < key; });
    return site != end && site->address == address ? site : nullptr;
}

bool Cpu::gx_site_enter(u32 site) {
    // What the site meant is a host effect: a verified segment that reaches a
    // live site is not replayed, as with platform sites and wait points.
    memory_.journal_note_host_effect();
    // A latched data fault suppresses every later guest access (memory_fault).
    if (memory_fault_.latched) return false;
    if (gx_sink_ == nullptr) {
        char detail[192];
        std::snprintf(detail, sizeof(detail),
                      "GX pipe site 0x%08X was reached and no native GX subsystem is installed", site);
        request_stop(StopReason::ExecutionFault, detail);
        return false;
    }
    return true;
}

void Cpu::gx_draw_sync_token(u32 site, u16 token, bool interrupt) {
    if (gx_site_enter(site)) gx_sink_->draw_sync_token(site, token, interrupt);
}

void Cpu::gx_draw_done(u32 site, u32 value) {
    if (gx_site_enter(site)) gx_sink_->draw_done(site, value);
}

void Cpu::gx_copy_trigger(u32 site, u32 value) {
    if (gx_site_enter(site)) gx_sink_->copy_trigger(site, value);
}

void Cpu::gx_primitive_opcode(u32 site, u8 opcode) {
    if (gx_site_enter(site)) gx_sink_->primitive_opcode(site, opcode);
}

void Cpu::gx_primitive_count(u32 site, u16 count) {
    if (gx_site_enter(site)) gx_sink_->primitive_count(site, count);
}

void Cpu::gx_dead_site(u32 site) {
    char detail[160];
    std::snprintf(detail, sizeof(detail),
                  "GX pipe site 0x%08X is dead in this build (lifted renderer: no immediate GX) and was reached",
                  site);
    request_stop(StopReason::ExecutionFault, detail);
}

void Cpu::gx_vertex(u32 site, u32 value, u32 bytes) {
    if (gx_site_enter(site)) gx_sink_->vertex(site, value, bytes);
}

void Cpu::execute_gx_site(const GxSiteRecord& site, u32 instruction) {
    // The listed word at this address, executed as the generator emits it
    // (tools/ppcrecomp/main.cpp, --gx-sites): the effective address, the
    // family's call or nothing, an update form's base update.
    const u32 primary = instruction >> 26;
    const u32 d = (instruction >> 21) & 31u;
    const u32 a = (instruction >> 16) & 31u;
    const bool paired = primary == 60u || primary == 61u;
    const bool update = primary == 37u || primary == 39u || primary == 45u || primary == 53u ||
                        primary == 55u || primary == 61u;
    const u32 displacement = paired ? static_cast<u32>(static_cast<s32>(instruction << 20) >> 20)
                                    : static_cast<u32>(static_cast<s32>(static_cast<s16>(instruction & 0xffffu)));
    const u32 address = (a == 0u && !update ? 0u : gpr[a]) + displacement;
    switch (site.family) {
        case GxSiteFamily::Dead: break;
        case GxSiteFamily::DrawSyncToken: gx_draw_sync_token(site.address, static_cast<u16>(gpr[d]), false); break;
        case GxSiteFamily::DrawSyncTokenIrq: gx_draw_sync_token(site.address, static_cast<u16>(gpr[d]), true); break;
        case GxSiteFamily::DrawDone: gx_draw_done(site.address, gpr[d]); break;
        case GxSiteFamily::CopyTrigger: gx_copy_trigger(site.address, gpr[d]); break;
        case GxSiteFamily::PrimitiveOpcode: gx_primitive_opcode(site.address, static_cast<u8>(gpr[d])); break;
        case GxSiteFamily::PrimitiveCount: gx_primitive_count(site.address, static_cast<u16>(gpr[d])); break;
        case GxSiteFamily::Vertex:
            switch (primary) {
                case 38u: gx_vertex(site.address, gpr[d] & 0xffu, 1u); break;
                case 44u: gx_vertex(site.address, gpr[d] & 0xffffu, 2u); break;
                case 36u: gx_vertex(site.address, gpr[d], 4u); break;
                case 52u: gx_vertex(site.address, double_to_single_bits(fpr[d].bits[0]), 4u); break;
                default:  // 60, psq_st
                    ppc::psq_store_gx(*this, site.address, d, ((instruction >> 15) & 1u) != 0u,
                                      (instruction >> 12) & 7u);
                    break;
            }
            break;
    }
    if (update && a != 0u) {
        gpr[a] = address;
    }
}

void Cpu::unlisted_gx_store(u32 address, u64 value, u32 bytes) {
    ++unlisted_gx_stores_;
    char detail[400];
    std::snprintf(detail, sizeof(detail),
                  "a store the GX site list does not hold reached the write-gather pipe: 0x%08X, "
                  "value 0x%llX (%u bytes), guest pc 0x%08X lr 0x%08X, host code %p "
                  "(map it through the linker map); the inventory missed it - regenerate "
                  "extract/gx-sites.txt (tools/ppcrecomp/gx_sites.py)",
                  address, static_cast<unsigned long long>(value), bytes, pc, lr, cold_store_return_);
    request_stop(StopReason::ExecutionFault, detail);
}

// Stage 6 (docs/native-time-20261005/STAGE6_PI_MI.md, D1): every GameCube
// device is native, so a guest access guest memory cannot serve is a defect. The
// report names what the page was and what replaced it.
namespace {

struct RetiredPage {
    u32 physical;
    u32 size;
    const char* device;
    const char* replaced_by;
};

constexpr RetiredPage kRetiredPages[] = {
    {0x0C000000u, 0x1000u, "the command processor", "NativeGxSync (stage 4) and the lowered GX sites (phase 4)"},
    {0x0C001000u, 0x1000u, "the pixel engine", "NativeGxSync (stage 4)"},
    {0x0C002000u, 0x1000u, "the video interface", "NativeVideo (stage 2) and the listed platform sites"},
    {0x0C003000u, 0x1000u, "the processor interface",
     "NativeInterrupts (interrupt bookkeeping and delivery) and NativeReset (stage 6)"},
    {0x0C004000u, 0x1000u, "the memory interface",
     "nothing: the PC has no memory-protection hardware (stage 6; the OS's stores are dead platform sites)"},
    {0x0C005000u, 0x1000u, "the DSP interface and ARAM controller", "NativeAudio (stage 1)"},
    {0x0C006000u, 0x0400u, "the disc interface", "the native file API (native assets)"},
    {0x0C006400u, 0x0400u, "the serial interface", "the lifted peripheral layer (L1)"},
    {0x0C006800u, 0x0400u, "the external interface", "NativeCard and NativeIpl (stage 5)"},
    {0x0C006C00u, 0x0400u, "the audio interface", "NativeAudio (stage 1)"},
    {0x0C008000u, 0x1000u, "the write-gather pipe", "the lowered GX sites (phase 4)"},
    {0x08000000u, 0x00400000u, "the embedded frame buffer", "the native renderer"},
};

}  // namespace

void Cpu::unmapped_access(u32 address, u32 bytes, bool write, u64 value) {
    ++unmapped_accesses_stopped_;
    // What the address was on the GameCube, so a stale hardware or MEM1
    // assumption names itself; none of it exists here.
    const char* what = nullptr;
    const char* owner = nullptr;
    if ((address & 0xC0000000u) == 0xC0000000u) {
        const u32 hardware = address & 0x3FFFFFFFu;
        for (const RetiredPage& page : kRetiredPages) {
            if (hardware >= page.physical && hardware - page.physical < page.size) {
                what = page.device;
                owner = page.replaced_by;
                break;
            }
        }
        if (what == nullptr && hardware >= 0x0C000000u && hardware < 0x0C010000u) {
            what = "an unassigned page of the GameCube's device-register window";
            owner = "nothing: no device ever answered there";
        }
        if (what == nullptr) {
            what = "a GameCube uncached-mirror address";
            owner = "nothing: the game's address window has no mirrors (docs/memory-architecture-20261006)";
        }
    } else if (address < 0x80000000u) {
        what = "an address below the program image (a null pointer or a GameCube physical address)";
        owner = "nothing: the game's address window has no physical addresses (docs/memory-architecture-20261006)";
    } else if (const GuestMemory::Region* region = memory_.region_at(address)) {
        what = GuestMemory::kind_name(region->kind);
        owner = "a range of the region that is reserved but not committed";
    }
    if (what == nullptr) {
        what = "no region of the game's address window";
        owner = "nothing (memory nothing allocated, or that was released)";
    }
    char detail[512];
    if (write) {
        std::snprintf(detail, sizeof(detail),
                      "a guest %u-byte store of 0x%llX to 0x%08X reached %s, which this machine does not have "
                      "(its owner is %s); ",
                      bytes, static_cast<unsigned long long>(value), address, what, owner);
    } else {
        std::snprintf(detail, sizeof(detail),
                      "a guest %u-byte load from 0x%08X reached %s, which this machine does not have (its owner "
                      "is %s); ",
                      bytes, address, what, owner);
    }
    request_stop(StopReason::ExecutionFault, std::string(detail) + "guest " + guest_location());
    // Contain the unwinding native code: every later access is suppressed and
    // no call runs until reset() or load_state() replaces the machine.
    latch_memory_fault(address, write, true);
}

// Host diagnostic peeks (audit memory-model-4). These used to be the
// architected mem_read8/mem_read32: a trace read past the end of mapped memory
// wrote DAR and stopped the machine with a TranslationFault the guest never
// caused, and a non-RAM address ran a device read or logged an unmapped
// access. A peek translates with the current MSR - pure - and reads only
// mapped guest memory. Each byte is translated on its own, so a read
// straddling a region boundary is exact.
bool Cpu::peek_guest_u8(u32 effective, u8& value) {
    u32 address = 0;
    if (!translate(effective, false, false, address)) return false;
    const u8* const host = memory_.peek_pointer(address, 1u);
    if (host == nullptr) return false;
    value = *host;
    return true;
}

u32 Cpu::read_guest_u32(u32 effective) {
    u32 value = 0;
    for (u32 index = 0; index < 4u; ++index) {
        u8 byte = 0;
        if (!peek_guest_u8(effective + index, byte)) return 0;
        value = (value << 8) | byte;
    }
    return value;
}

void Cpu::write_guest_u32(u32 effective, u32 value) { mem_write32(effective, value); }

u8 Cpu::read_guest_u8(u32 effective) {
    u8 byte = 0;
    return peek_guest_u8(effective, byte) ? byte : u8{0};
}

std::string Cpu::read_guest_string(u32 effective, u32 limit) {
    std::string result;
    for (u32 index = 0; index < limit; ++index) {
        u8 byte = 0;
        if (!peek_guest_u8(effective + index, byte) || byte == 0) {
            break;
        }
        result.push_back(static_cast<char>(byte));
    }
    return result;
}

// ---------------------------------------------------------------------------
// Exceptions and timers
// ---------------------------------------------------------------------------

void Cpu::return_from_interrupt(u32 site) noexcept {
    const u32 saved = spr[SPR_SRR1];  // the same steps as the rfi instruction (case 50 of the interpreter)
    set_msr(((msr_ & ~0x87c0ff73u) | (saved & 0x87c0ff73u)) & ~kMsrPOW);
    restore_interrupt_fpr();
    recognise_interrupt_ = true;
    leave_to_dispatcher(spr[SPR_SRR0] & ~3u, FallbackReason::ReturnFromInterrupt, site);
}

void Cpu::note_exception_resume(u32 srr0) noexcept {
    if (interrupt_fpr_depth_ == 1u) {
        interrupt_fpr_resume_ = srr0 & ~3u;
    }
}

void Cpu::restore_interrupt_fpr() noexcept {
    if (!interrupt_fpr_shadow_) {
        // Depth is still tracked so savestates and nesting behave identically
        // whichever way the switch is set; only the copy-back is suppressed.
        if (interrupt_fpr_depth_ != 0u) --interrupt_fpr_depth_;
        return;
    }
    if (interrupt_fpr_depth_ == 0u) {
        return;
    }
    --interrupt_fpr_depth_;
    // Nested sc/rfi inside a VI handler used to restore here and then let
    // the outer handler trash f1-f4 before the interrupted wrap resumed.
    if (interrupt_fpr_depth_ != 0u) {
        return;
    }
    // ...and only restore it when this rfi genuinely resumes the code that was
    // interrupted.
    //
    // The snapshot exists to undo the register clobbering an exception handler
    // does on the way through: a handler is free to use FPRs that the hardware
    // would have made it fault for. That is sound only while the handler
    // returns to its own interruptee.
    //
    // CORRECTION 2026-09-03. This comment used to justify itself with "this
    // runtime does not raise the floating-point-unavailable exception
    // (Exception::FpUnavailable is declared and never raised)". That is false,
    // and has been since the FP-unavailable exception was added to fix the Gun
    // Fortress freeze. Measured with the conformance census (cpu_diag.h):
    // FpUnavailable is raised 1,400 times on the Gun Fortress 2e9 route and
    // once on boot 15e9.
    //
    // The snapshot itself is NOT dead - deleting it was proposed and is
    // refuted. Measured outcomes of the branch below:
    //     GF 2e9    restore performed 6,106   skipped (context switch) 3,978
    //     boot 15e9 restore performed 4,643   skipped (context switch) 5,786
    // `restore performed` equals the system-call count exactly on both routes,
    // so every sc produces one restore. See
    // docs/cpu-renovation/GEKKO_ISA_CONFORMANCE_2026-09-03.md item B-1/O-6.
    //
    // The GameCube OS reschedules from inside its interrupt handlers, and this
    // game is multithreaded. When the handler picks a different thread it
    // rewrites SRR0, and the snapshot then belongs to a thread that is no
    // longer running - copying it back stamps the interrupted thread's
    // registers onto the newly scheduled one.
    //
    // That is a silent, delayed corruption. It cost Gun Fortress a hard freeze
    // at about four seconds in: EffectParticle*::vf2 (0x80434824) resumed with
    // f1..f4 = {-1.0, 0.0, 0.6, 0.0} where it had loaded {-pi, 2.0, 0.0, pi},
    // so its rotation-wrap loop stepped by f2*f4 = 0.0, never converged, and
    // the guest span there for ever - no GX submission, no XFB, presenter
    // frozen on the last frame while interrupt-driven audio starved.
    //
    // Discarding the snapshot is always safe: the registers then simply stay as
    // whoever ran last left them, which is exactly what the hardware does.
    if ((spr[SPR_SRR0] & ~3u) != interrupt_fpr_resume_) {
        ++interrupt_fpr_context_switches_;
        ++cpu_diag().fpr_restore_skipped_context_switch;
        return;
    }
    ++cpu_diag().fpr_restore_performed;
    // Measurement only (CPU-2): does this restore actually change anything?
    {
        u64 differing = 0;
        for (std::size_t index = 0; index < sizeof(fpr) / sizeof(fpr[0]); ++index) {
            if (std::memcmp(&fpr[index], &interrupt_fpr_[index], sizeof(fpr[0])) != 0) {
                ++differing;
            }
        }
        if (differing != 0u || fpscr != interrupt_fpscr_) {
            ++cpu_diag().fpr_restore_changed;
            cpu_diag().fpr_restore_changed_registers += differing;
        }
    }
    std::memcpy(fpr, interrupt_fpr_, sizeof(fpr));
    fpscr = interrupt_fpscr_;
}

CpuDiagCounters& cpu_diag() {
    static CpuDiagCounters counters;
    return counters;
}

std::atomic<u64> g_non_native_ppc_executions{0};

bool reference_execution_available() noexcept {
#if defined(SHADOW_CPU_REFERENCE_BUILD)
    return true;
#else
    return false;
#endif
}

void Cpu::raise_exception(Exception vector, u32 srr1_bits) {
    exception_entry(vector);
    // Bits the exception itself reports (the illegal-instruction program
    // exception's bit 12) are part of the architected entry: the OS entry
    // saves SRR1 into the OSContext.
    spr[SPR_SRR1] |= srr1_bits;
    enter_os_exception(vector);
}

void Cpu::exception_entry(Exception vector) {
    pending_native_call_.reset();
    {
        // Conformance census. Counting only - see cpu_diag.h.
        CpuDiagCounters& diag = cpu_diag();
        const std::size_t slot = static_cast<std::size_t>(vector) >> 8;
        if (slot < CpuDiagCounters::kVectorSlots) ++diag.exceptions_by_vector[slot];
        ++diag.exceptions_total;
    }
    if (interrupt_fpr_depth_ == 0u && interrupt_fpr_shadow_) {
        std::memcpy(interrupt_fpr_, fpr, sizeof(fpr));
        interrupt_fpscr_ = fpscr;
    }
    ++interrupt_fpr_depth_;
    spr[SPR_SRR0] = pc;
    if (interrupt_fpr_depth_ == 1u) {
        interrupt_fpr_resume_ = pc & ~3u;
    }
    spr[SPR_SRR1] = msr_ & 0x87c0ffffu;
    u32 msr = msr_;
    msr &= ~(kMsrPOW | kMsrEE | kMsrPR | kMsrFP | kMsrFE0 | kMsrSE | kMsrBE | kMsrFE1 | kMsrIR |
             kMsrDR | kMsrPM | kMsrRI);
    if ((msr & kMsrILE) != 0u) {
        msr |= kMsrLE;
    } else {
        msr &= ~kMsrLE;
    }
    // C2: one write of the final value (exception entry clears DR, so the
    // page table switches to the null table here; the OS entry turns
    // translation back on before any guest instruction runs).
    set_msr(msr);
}

void Cpu::enter_native_interrupt() noexcept {
    pending_native_call_.reset();
    ++cpu_diag().native_interrupts_entered;
    if (interrupt_fpr_depth_ == 0u && interrupt_fpr_shadow_) {
        std::memcpy(interrupt_fpr_, fpr, sizeof(fpr));
        interrupt_fpscr_ = fpscr;
    }
    ++interrupt_fpr_depth_;
    spr[SPR_SRR0] = pc;
    if (interrupt_fpr_depth_ == 1u) {
        interrupt_fpr_resume_ = pc & ~3u;
    }
    spr[SPR_SRR1] = msr_ & 0x87c0ffffu;
    u32 msr = msr_;
    msr &= ~(kMsrPOW | kMsrEE | kMsrPR | kMsrFP | kMsrFE0 | kMsrSE | kMsrBE | kMsrFE1 | kMsrPM |
             kMsrRI);
    if ((msr & kMsrILE) != 0u) {
        msr |= kMsrLE;
    } else {
        msr &= ~kMsrLE;
    }
    set_msr(msr | kMsrIR | kMsrDR);
}

// ---------------------------------------------------------------------------
// Game time (docs/native-time-20261005/DESIGN.md)
// ---------------------------------------------------------------------------
//
// The time base and the decrementer are views of the game clock. Neither ticks:
// the clock moves only at a wait point, so both are exact at every guest
// instruction boundary by construction.

void Cpu::set_time_base(u64 value) noexcept {
    time_base_offset_ = value - clock_.now();
}

void Cpu::set_decrementer(u32 value) noexcept {
    dec_value_ = value;
    dec_written_at_ = clock_.now();
    // DEC = value - (t - written). Its bit 0 (the MSB) goes 0 -> 1 exactly when
    // it steps from 0x00000000 to 0xFFFFFFFF, value + 1 ticks after the write
    // whatever the sign of value (gekko_user_manual.txt PAGE 159 Table 4-2).
    clock_.schedule(GameEvent::Decrementer, clock_.now() + static_cast<u64>(value) + 1u);
}

void Cpu::latch_decrementer() noexcept {
    CpuDiagCounters& diag = cpu_diag();
    ++diag.dec_edge_total;
    if (!dec_delivery_) {
        // No consumer (set_decrementer_delivery): the edge is counted and dropped.
        ++diag.dec_edge_dropped_no_consumer;
    } else {
        interrupt_lines_ = static_cast<u8>(interrupt_lines_ | kInterruptDecrementer);
        if ((msr_ & kMsrEE) != 0u) {
            exit_pending_ = static_cast<u8>(exit_pending_ | kExitInterrupt);
        } else {
            ++diag.dec_edge_suppressed_ee_clear;
        }
    }
    // The register keeps counting; its next 0 -> 1 edge is a full wrap later.
    const u64 edge = clock_.deadline(GameEvent::Decrementer);
    clock_.schedule(GameEvent::Decrementer, edge + (u64{1} << 32));
}

void Cpu::write_hid2(u32 value) {
    if ((value & kHid2PairedSingleEnables) != kHid2PairedSingleEnables) {
        char detail[192];
        std::snprintf(detail, sizeof(detail),
                      "HID2 write 0x%08X would clear the paired-single enables LSQE|PSE (pc 0x%08X); "
                      "they are a machine invariant, the write is refused",
                      value, pc);
        request_stop(StopReason::ExecutionFault, detail);
        return;
    }
    spr[SPR_HID2] = value;
}

// Audit dispatch-exceptions-1/-2: the one place either engine enters 0x500 or
// 0x900 for an asynchronous condition. Priority is gekko_user_manual.txt
// PAGE 162 Table 4-3: external interrupt (3) before decrementer (5). The
// external line is a level and stays asserted until the controller drops it;
// the decrementer condition is an edge latch and is consumed by being taken.
void Cpu::take_pending_interrupt() {
    // The native line and the external line are both external interrupts to
    // the OS: when both are pending, its priority table decides (the
    // comparison NativeInterrupts installs), not the order of the lines.
    const bool external = (interrupt_lines_ & kInterruptExternal) != 0u;
    const bool native = (interrupt_lines_ & kInterruptNative) != 0u;
    if (native && (!external || (native_interrupt_first_ && native_interrupt_first_(*this)))) {
        // A native source (gekko/native_interrupts.h). Its handler enters the
        // OS's context protocol itself and sets pc/npc; no vector is fetched.
        if (!native_interrupt_handler_) {
            interrupt_lines_ = static_cast<u8>(interrupt_lines_ & ~kInterruptNative);
            request_stop(StopReason::ExecutionFault,
                         "a native interrupt is pending and no native interrupt handler is installed");
            return;
        }
        native_interrupt_handler_(*this);
        return;
    }
    if (external) {
        raise_exception(Exception::ExternalInterrupt);
    } else {
        interrupt_lines_ = static_cast<u8>(interrupt_lines_ & ~kInterruptDecrementer);
        ++cpu_diag().dec_pending_taken;
        raise_exception(Exception::Decrementer);
    }
    pc = npc;
}

// ---------------------------------------------------------------------------
// Platform sites (extract/platform-sites.txt)
// ---------------------------------------------------------------------------

u16 Cpu::platform_read16(u32 site, u32 effective) {
    // The platform's answer is a host effect: a verified segment that reaches
    // a site is not replayed, as with a wait point.
    memory_.journal_note_host_effect();
    // A latched data fault suppresses every later guest access (memory_fault).
    if (memory_fault_.latched) return 0u;
    if (!platform_read_) {
        char detail[192];
        std::snprintf(detail, sizeof(detail),
                      "platform site 0x%08X reads 0x%08X and no native platform subsystem is installed",
                      site, effective);
        request_stop(StopReason::ExecutionFault, detail);
        return 0u;
    }
    return platform_read_(*this, site, effective);
}

void Cpu::platform_write16(u32 site, u32 effective, u16 value) {
    memory_.journal_note_host_effect();
    if (memory_fault_.latched) return;
    if (!platform_write_) {
        char detail[192];
        std::snprintf(detail, sizeof(detail),
                      "platform site 0x%08X writes 0x%04X to 0x%08X and no native platform subsystem is installed",
                      site, value, effective);
        request_stop(StopReason::ExecutionFault, detail);
        return;
    }
    platform_write_(*this, site, effective, value);
}

u32 Cpu::platform_value(u32 site) {
    memory_.journal_note_host_effect();
    if (memory_fault_.latched) return 0u;
    if (!platform_value_) {
        char detail[160];
        std::snprintf(detail, sizeof(detail),
                      "platform site 0x%08X loads a console constant and no native platform subsystem is installed",
                      site);
        request_stop(StopReason::ExecutionFault, detail);
        return 0u;
    }
    return platform_value_(*this, site);
}

const PlatformSiteRecord* Cpu::find_platform_site(u32 address) const noexcept {
    const PlatformSiteRecord* const end = platform_site_table_ + platform_site_table_count_;
    const PlatformSiteRecord* site = std::lower_bound(
        platform_site_table_, end, address,
        [](const PlatformSiteRecord& record, u32 key) { return record.address < key; });
    return site != end && site->address == address ? site : nullptr;
}

void Cpu::execute_platform_site(const PlatformSiteRecord& site, u32 instruction) {
    // The listed word at this address, executed as the generator emits it
    // (tools/ppcrecomp/main.cpp, --platform-sites): the effective address,
    // then the native platform's answer (lhz), its effect (sth) or nothing (a
    // dead sth). Only the D-form lhz / sth without update is ever listed, and
    // li (addi rD,0): the native platform's value.
    const u32 d = (instruction >> 21) & 31u;
    if ((instruction >> 26) == 14u) {
        gpr[d] = platform_value(site.address);
        return;
    }
    const u32 a = (instruction >> 16) & 31u;
    const u32 effective =
        (a == 0u ? 0u : gpr[a]) + static_cast<u32>(static_cast<s32>(static_cast<s16>(instruction & 0xffffu)));
    if ((instruction >> 26) == 40u) {
        gpr[d] = platform_read16(site.address, effective);
    } else if (!site.dead) {
        platform_write16(site.address, effective, static_cast<u16>(gpr[d]));
    }
}

// ---------------------------------------------------------------------------
// Differential verification of recompiled code against the interpreter
// ---------------------------------------------------------------------------

struct Cpu::ArchitectedState {
    u32 gpr[32];
    FpReg fpr[32];
    u32 pc, npc, lr, ctr, xer, cr, msr, fpscr;
    std::array<u32, 1024> spr;
    std::array<u32, 16> sr;
    // The time-base and decrementer views of the game clock (Cpu::time_base,
    // Cpu::decrementer): the same four words the instruction clock used to
    // occupy, so the block keeps its size.
    u64 time_base_offset, dec_written_at, dec_value, reserved_clock;
    bool external_interrupt, pending_decrementer;
    // Savestate version 6. These two occupy what was the struct's tail padding
    // through version 5, so the serialised block is still exactly the same
    // size; Cpu::load_state ignores the bytes for older versions.
    bool reservation;
    u32 reservation_address;
};

struct Cpu::InterruptFprState {
    u32 depth;
    u32 resume;
    FpReg registers[32];
    u32 fpscr;
};

// Savestate version 10: the game clock - its time and every scheduled
// deadline (gekko/game_clock.h) - and the data-fault latch. Host-private like
// InterruptFprState and deliberately not in ArchitectedState: the clock is
// not architected state, it is the schedule that decides when guest-visible
// events happen. The latch rides along so a state saved from a halted machine
// reloads halted.
struct Cpu::TimelineState {
    u64 now;
    u64 deadlines[static_cast<std::size_t>(GameEvent::Count)];
    u32 fault_effective;
    u32 fault_pc;
    u8 fault_latched;
    u8 fault_write;
    u8 fault_unmapped;  // M1: was reserved (0 = translation off)
    u8 reserved[5];
};

void Cpu::capture_interrupt_fpr_state(InterruptFprState& state) const noexcept {
    state.depth = interrupt_fpr_depth_;
    state.resume = interrupt_fpr_resume_;
    std::copy(std::begin(interrupt_fpr_), std::end(interrupt_fpr_),
              std::begin(state.registers));
    state.fpscr = interrupt_fpscr_;
}

void Cpu::restore_interrupt_fpr_state(const InterruptFprState& state) noexcept {
    interrupt_fpr_depth_ = state.depth;
    interrupt_fpr_resume_ = state.resume;
    std::copy(std::begin(state.registers), std::end(state.registers),
              std::begin(interrupt_fpr_));
    interrupt_fpscr_ = state.fpscr;
}

void Cpu::capture_state(ArchitectedState& state) const {
    std::copy(std::begin(gpr), std::end(gpr), std::begin(state.gpr));
    std::copy(std::begin(fpr), std::end(fpr), std::begin(state.fpr));
    state.pc = pc;
    state.npc = npc;
    state.lr = lr;
    state.ctr = ctr;
    state.xer = xer;
    state.cr = cr;
    state.msr = msr_;
    state.fpscr = fpscr;
    state.spr = spr;
    state.sr = sr;
    state.spr[SPR_DEC] = 0u;  // the decrementer is a clock view, below
    state.time_base_offset = time_base_offset_;
    state.dec_written_at = dec_written_at_;
    state.dec_value = dec_value_;
    state.reserved_clock = 0u;
    // The two bools are the two bits of interrupt_lines_.
    state.external_interrupt = (interrupt_lines_ & kInterruptExternal) != 0u;
    state.pending_decrementer = (interrupt_lines_ & kInterruptDecrementer) != 0u;
    state.reservation = reservation;
    state.reservation_address = reservation_address;
}

void Cpu::restore_state(const ArchitectedState& state) {
    std::copy(std::begin(state.gpr), std::end(state.gpr), std::begin(gpr));
    std::copy(std::begin(state.fpr), std::end(state.fpr), std::begin(fpr));
    pc = state.pc;
    npc = state.npc;
    lr = state.lr;
    ctr = state.ctr;
    xer = state.xer;
    cr = state.cr;
    set_msr(state.msr);
    fpscr = state.fpscr;
    spr = state.spr;
    sr = state.sr;
    spr[SPR_DEC] = 0u;  // never read; the decrementer is a clock view
    time_base_offset_ = state.time_base_offset;
    dec_written_at_ = state.dec_written_at;
    dec_value_ = static_cast<u32>(state.dec_value);
    // The native line is not architected state: NativeInterrupts derives it
    // from its sources, which a restore does not touch, so it is kept. (The
    // --verify-recomp rollback before every replay used to drop it: a raised,
    // unmasked ARAM source was never delivered again and the ARQ waiter hung.
    // A savestate load re-derives it when the native interrupts load.)
    interrupt_lines_ = static_cast<u8>((state.external_interrupt ? kInterruptExternal : 0u) |
                                       (state.pending_decrementer ? kInterruptDecrementer : 0u) |
                                       (interrupt_lines_ & kInterruptNative));
    reservation = state.reservation;
    reservation_address = state.reservation_address;
    // A restored machine may have a different fault latch and MSR: every memoised page is suspect.
    select_memory_window();
}

void Cpu::save_state(std::vector<u8>& out) const {
    // The CPU block is this struct's raw bytes and has been 4872 bytes since
    // version 3. Growing it would mis-parse every existing .gekkostate: add new
    // fields inside the padding (as version 6 did) or as a separate block.
    static_assert(sizeof(ArchitectedState) == 4872u,
                  "ArchitectedState is the on-disk CPU savestate block");
    // Reuse the --verify-recomp snapshot so the savestate and the recompiler
    // oracle can never disagree about what "the architected state" is.
    ArchitectedState snapshot{};
    capture_state(snapshot);
    state::put_block(out, &snapshot, sizeof(snapshot));
    // Version 5: this state is host-private, but it is semantically required.
    // An exception snapshots the interrupted thread's FPR file here and an rfi
    // restores it only if the guest did not schedule a different thread. A
    // savestate taken between those two events must carry the snapshot, depth,
    // and expected resume PC or the later rfi restores stale/zero host state.
    InterruptFprState interrupt_snapshot{};
    capture_interrupt_fpr_state(interrupt_snapshot);
    state::put_block(out, &interrupt_snapshot, sizeof(interrupt_snapshot));
    // Version 10: the game clock and the data-fault latch; version 11: five
    // deadlines (GameEvent::Retrace).
    static_assert(sizeof(TimelineState) == 64u, "TimelineState is an on-disk savestate block");
    TimelineState timeline{};
    timeline.now = clock_.now();
    for (std::size_t index = 0; index < clock_.deadlines().size(); ++index) {
        timeline.deadlines[index] = clock_.deadlines()[index];
    }
    timeline.fault_effective = memory_fault_.effective;
    timeline.fault_pc = memory_fault_.pc;
    timeline.fault_latched = memory_fault_.latched ? 1u : 0u;
    timeline.fault_write = memory_fault_.write ? 1u : 0u;
    timeline.fault_unmapped = memory_fault_.unmapped ? 1u : 0u;
    state::put_block(out, &timeline, sizeof(timeline));
}

bool Cpu::load_state(const u8*& cursor, const u8* end, u32 save_version) {
    // savestate.cpp refuses every version before 11 (native game time with the
    // native video: the TimelineState holds the Retrace deadline).
    if (save_version < 11u) return false;
    ArchitectedState snapshot{};
    if (!state::get_block(cursor, end, &snapshot, sizeof(snapshot))) return false;
    InterruptFprState interrupt_snapshot{};
    if (!state::get_block(cursor, end, &interrupt_snapshot, sizeof(interrupt_snapshot))) {
        return false;
    }
    TimelineState timeline{};
    if (!state::get_block(cursor, end, &timeline, sizeof(timeline))) return false;
    memory_fault_.latched = timeline.fault_latched != 0u;
    memory_fault_.write = timeline.fault_write != 0u;
    memory_fault_.unmapped = timeline.fault_unmapped != 0u;
    memory_fault_.effective = timeline.fault_effective;
    memory_fault_.pc = timeline.fault_pc;
    // HID2 LSQE|PSE are machine invariants (Cpu::write_hid2); a state
    // without them cannot be resumed by code that never tests them.
    if ((snapshot.spr[SPR_HID2] & kHid2PairedSingleEnables) != kHid2PairedSingleEnables) return false;
    // After the latch: restore_state's host-page drop re-derives the FIFO
    // routes from the restored BATs under the restored latch.
    restore_state(snapshot);
    clock_.set_now(timeline.now);
    for (std::size_t index = 0; index < static_cast<std::size_t>(GameEvent::Count); ++index) {
        clock_.schedule(static_cast<GameEvent>(index), timeline.deadlines[index]);
    }
    rebuild_hle_slots();
    restore_interrupt_fpr_state(interrupt_snapshot);
    // A restored machine has no stop pending. Its next instruction boundary is
    // a dispatch (R1), so the interpreter's recognition-point memory of the
    // discarded timeline must not carry over.
    clear_stop();
    stop_requested_.store(false, std::memory_order_relaxed);
    pending_native_call_.reset();
    recognise_interrupt_ = false;
    end_poll_tracking();
    return true;
}

void Cpu::set_verify_recompiled(bool enabled, std::size_t max_messages, u64 max_calls) {
#if defined(SHADOW_CPU_REFERENCE_BUILD)
    verifying_ = enabled;
    verify_message_limit_ = max_messages;
    verify_call_limit_ = max_calls;
    if (!enabled) {
        std::vector<GuestMemory::JournalEntry>().swap(verify_native_writes_scratch_);
        std::vector<std::pair<u32, u8>>().swap(verify_native_bytes_scratch_);
    }
#else
    (void)max_messages;
    (void)max_calls;
    verifying_ = false;
    if (enabled) {
        request_stop(StopReason::ExecutionFault,
                     "reference verification is unavailable in the native runtime");
    }
#endif
    // try_hle's inline filter must send every call to try_hle_slow while
    // verifying (the per-call cut) and only hooked ones otherwise.
    rebuild_hle_slots();
}

namespace {

// Dispatcher entry for an address that has no declared entry point: run the
// translated function that owns c.pc from exactly that instruction. Shared by
// the verified mid-function dispatch and the body of a declined hook.
void enter_translated_range(Cpu& c) {
    const RecompiledCode::Range* const range = c.recompiled()->find_range(c.pc);
    range->at(c, c.pc);
    c.finish_transfers();  // R12
}

// The CpuDiagCounters fields a discarded native pass can advance: exception
// delivery, decrementer edges and the FPR shadow restore. The whole struct
// used to be copied in and assigned back per segment - about 18 KB of arrays,
// so each segment paid allocations proportional to the route. Not captured,
// because no native pass writes them: the interpreter-origin ledger (only
// Cpu::execute, which native code never reaches) and the dispatcher's own
// timing counters (only Cpu::run).
class VerifyDiagSnapshot {
public:
    explicit VerifyDiagSnapshot(const CpuDiagCounters& diag)
        : exceptions_by_vector_(diag.exceptions_by_vector),
          exceptions_total_(diag.exceptions_total),
          dec_edge_total_(diag.dec_edge_total),
          dec_edge_suppressed_ee_clear_(diag.dec_edge_suppressed_ee_clear),
          dec_edge_dropped_no_consumer_(diag.dec_edge_dropped_no_consumer),
          dec_pending_dropped_ee_clear_(diag.dec_pending_dropped_ee_clear),
          dec_pending_taken_(diag.dec_pending_taken),
          fpr_restore_performed_(diag.fpr_restore_performed),
          fpr_restore_skipped_context_switch_(diag.fpr_restore_skipped_context_switch),
          fpr_restore_changed_(diag.fpr_restore_changed),
          fpr_restore_changed_registers_(diag.fpr_restore_changed_registers) {}

    void restore(CpuDiagCounters& diag) const {
        diag.exceptions_by_vector = exceptions_by_vector_;
        diag.exceptions_total = exceptions_total_;
        diag.dec_edge_total = dec_edge_total_;
        diag.dec_edge_suppressed_ee_clear = dec_edge_suppressed_ee_clear_;
        diag.dec_edge_dropped_no_consumer = dec_edge_dropped_no_consumer_;
        diag.dec_pending_dropped_ee_clear = dec_pending_dropped_ee_clear_;
        diag.dec_pending_taken = dec_pending_taken_;
        diag.fpr_restore_performed = fpr_restore_performed_;
        diag.fpr_restore_skipped_context_switch = fpr_restore_skipped_context_switch_;
        diag.fpr_restore_changed = fpr_restore_changed_;
        diag.fpr_restore_changed_registers = fpr_restore_changed_registers_;
    }
    u64 exceptions_total() const noexcept { return exceptions_total_; }

private:
    std::array<u64, CpuDiagCounters::kVectorSlots> exceptions_by_vector_;
    u64 exceptions_total_;
    u64 dec_edge_total_;
    u64 dec_edge_suppressed_ee_clear_;
    u64 dec_edge_dropped_no_consumer_;
    u64 dec_pending_dropped_ee_clear_;
    u64 dec_pending_taken_;
    u64 fpr_restore_performed_;
    u64 fpr_restore_skipped_context_switch_;
    u64 fpr_restore_changed_;
    u64 fpr_restore_changed_registers_;
};

}  // namespace


bool Cpu::serve_hle_for_verify(u32 address) {
    const HleEntry* entry = find_hle(address);
    if (entry == nullptr) return false;
    const u32 entry_pc = pc;
    ++handler_depth_;
    const bool served = entry->handler(*this);
    --handler_depth_;
    if (!served) return false;
    ++const_cast<HleEntry*>(entry)->hits;
    // A native owner leaves pc at the entry: the call resumes at the LR it was entered with. A handler that ran
    // translated code ends where that code ended - at the return address when it returned, mid-body when an interrupt
    // was recognised inside it (pc is where it resumes; the pass is skipped as one that did not return).
    if (!stopped() && pc == entry_pc) pc = lr;
    return true;
}

void Cpu::set_verify_scope(std::vector<u32> entries, bool all_segments) {
    verify_all_segments_ = all_segments;
    std::sort(entries.begin(), entries.end());
    entries.erase(std::unique(entries.begin(), entries.end()), entries.end());
    verify_scope_ = std::move(entries);
    verify_entry_stats_.clear();
    verify_coverage_.assign(verify_scope_.empty() ? 0u : (kVerifyCoverageEnd - kVerifyCoverageBase) / 4u / 64u, 0u);
}

bool Cpu::in_verify_scope(u32 entry) const noexcept {
    return std::binary_search(verify_scope_.begin(), verify_scope_.end(), entry);
}

void Cpu::set_verify_r4_dead(std::vector<u32> entries) {
    std::sort(entries.begin(), entries.end());
    verify_r4_dead_ = std::move(entries);
}

void Cpu::set_verify_ranges(std::vector<std::pair<u32, u32>> ranges) {
    std::sort(ranges.begin(), ranges.end());
    verify_ranges_ = std::move(ranges);
}

// A segment the oracle checks: any (all segments), a scoped entry, or one that starts inside a scoped function's range
// (its post-call continuations - what entry scoping cannot reach - without the rest of the game's segments).
bool Cpu::verify_candidate(u32 pc) const noexcept {
    if (verify_all_segments_ || in_verify_scope(pc)) return true;
    auto it = std::upper_bound(verify_ranges_.begin(), verify_ranges_.end(), std::pair<u32, u32>{pc, ~0u});
    return it != verify_ranges_.begin() && pc < std::prev(it)->second;
}

void Cpu::set_verify_sampling(const VerifySampling& sampling, const std::vector<u32>& known) {
    verify_sampling_ = sampling;
    // Sampling decides at the cut whether a call becomes a segment; without a scope (--verify-recomp) every
    // dispatch is a segment and there is nothing to sample.
    verify_sampling_.enabled = sampling.enabled && !verify_scope_.empty();
    verify_sampling_counts_ = {};
    const std::size_t slots = verify_sampling_.enabled ? (kVerifyCoverageEnd - kVerifyCoverageBase) / 4u : 0u;
    verify_arrivals_.assign(slots, 0u);
    verify_sampled_n_.assign(slots, 0u);
    verify_known_.assign(slots, 0u);
    for (const u32 entry : known) {
        if (slots == 0u || entry < kVerifyCoverageBase || entry >= kVerifyCoverageEnd || (entry & 3u) != 0u) continue;
        u8& flag = verify_known_[(entry - kVerifyCoverageBase) >> 2];
        verify_sampling_counts_.known_entries += flag == 0u;
        flag = 1u;
    }
    verify_tokens_ = 0.0;
    verify_refill_at_ = verify_started_at_ = std::chrono::steady_clock::now();
    if (verify_sampling_.enabled) verify_call_limit_ = ~0ull;
}

Cpu::VerifySamplingReport Cpu::verify_sampling_report() const {
    VerifySamplingReport report = verify_sampling_counts_;
    report.wall_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - verify_started_at_).count();
    return report;
}

u32 Cpu::verify_arrivals(u32 address) const noexcept {
    if (verify_arrivals_.empty() || address < kVerifyCoverageBase || address >= kVerifyCoverageEnd) return 0u;
    return verify_arrivals_[(address - kVerifyCoverageBase) >> 2];
}

// Single-precision multiply family (fmuls, fmadds.., ps_mul*, ps_madd*..): frC must be a single-precision operand
// (Gekko manual PAGE 82). Counts, per instruction, the executions whose frC was not exactly a single, and of those the
// ones where rounding frC to a 25-bit significand (24 fraction bits, ties up - the rule Dolphin applies, unproven
// here) would change the operand. Observation only: no result is altered.
void Cpu::note_single_multiply_frc(u64 c) {
    if (!frc_probe_) return;
    const u64 exponent = (c >> 52) & 0x7FFu, fraction = c & 0x000FFFFFFFFFFFFFull;
    if (exponent == 0x7FFu || (exponent == 0u && fraction == 0u)) return;  // inf / NaN / zero: no significand
    double value;
    std::memcpy(&value, &c, sizeof value);
    const bool single = static_cast<double>(static_cast<float>(value)) == value;
    if (single) return;
    // Significand bits below the 24th fraction bit, with the subnormal shift: a normal double keeps bits 51..28.
    u64 low_mask = 0x0FFFFFFFull;
    if (exponent == 0u) {
        unsigned shift = 0u;  // leading zeros of the 52-bit fraction (subnormal: rare)
        while (((fraction << shift) & (1ull << 51)) == 0u) ++shift;
        ++shift;  // the normalising shift moves the leading 1 into the implicit bit
        low_mask = (shift >= 28u) ? 0ull : (low_mask >> shift);
    }
    FrcProbe& p = frc_probe_sites_[pc];
    ++p.wide;
    if ((fraction & low_mask) != 0u) ++p.rounding_changes;
}

u32 Cpu::verify_checks(u32 address) const noexcept {
    if (verify_sampled_n_.empty() || address < kVerifyCoverageBase || address >= kVerifyCoverageEnd) return 0u;
    return verify_sampled_n_[(address - kVerifyCoverageBase) >> 2];
}

// One arrival at a candidate entry: whether to check it. Counters first (no clock read for the arrivals that are
// sampled out - nearly all of them), then the CPU budget.
bool Cpu::verify_sample_arrival(u32 entry) {
    if (entry < kVerifyCoverageBase || entry >= kVerifyCoverageEnd || (entry & 3u) != 0u) return false;
    if (!verify_candidate(entry)) return false;
    const u32 slot = (entry - kVerifyCoverageBase) >> 2;
    u32& arrivals = verify_arrivals_[slot];
    verify_sampling_counts_.entries_seen += arrivals == 0u;
    if (arrivals != ~0u) ++arrivals;
    ++verify_sampling_counts_.arrivals;
    u8& taken = verify_sampled_n_[slot];
    const VerifySampling& s = verify_sampling_;
    const bool want = verify_known_[slot] != 0u
                          ? s.known_period != 0u && arrivals % s.known_period == 0u
                          : taken < s.first_per_entry || (s.period != 0u && arrivals % s.period == 0u);
    if (!want) {
        ++verify_sampling_counts_.sampled_out;
        return false;
    }
    const auto now = std::chrono::steady_clock::now();
    verify_tokens_ = std::min(s.burst_seconds * s.cpu_share,
                              verify_tokens_ + std::chrono::duration<double>(now - verify_refill_at_).count() *
                                                   s.cpu_share);
    verify_refill_at_ = now;
    if (verify_tokens_ <= 0.0) {
        // An entry still owed its first checks keeps them: `taken` did not advance.
        ++verify_sampling_counts_.over_budget;
        return false;
    }
    if (taken != 0xFFu) ++taken;
    ++verify_sampling_counts_.sampled;
    return true;
}

// The dispatcher's decision for a candidate entry: strict checks everything; fast checks a call try_hle_slow already
// sampled (and cut for), or samples this arrival.
bool Cpu::verify_take_sample(u32 entry) {
    if (!verify_sampling_.enabled) return true;
    if (verify_sampled_entry_ != 0u) {
        const bool cut_for_it = verify_sampled_entry_ == entry;
        verify_sampled_entry_ = 0u;
        if (cut_for_it) return true;
    }
    return verify_sample_arrival(entry);
}

void Cpu::set_verify_owner_fixture(u32 address, int mode) {
#if defined(SHADOW_CPU_REFERENCE_BUILD)
    install_hle(
        address,
        [mode](Cpu& c) {
            enter_translated_range(c);
            if (mode == 2) {
                // The last byte written outside the dead stack (an observable byte), or r3.
                const auto& journal = c.guest_memory().journal();
                const u32 sp = c.gpr[1];
                bool flipped = false;
                for (auto it = journal.rbegin(); it != journal.rend() && !flipped; ++it) {
                    const u32 at = it->address + it->size - 1u;
                    if (at < sp && sp - at <= 0x10000u) continue;
                    if (u8* p = c.guest_memory().host_pointer(at, 1u)) {
                        *p ^= 1u;
                        flipped = true;
                    }
                }
                if (!flipped) c.gpr[3] ^= 1u;
            }
            return true;
        },
        mode == 2 ? "verify-player fixture owner (corrupt)" : "verify-player fixture owner (exact)");
#else
    (void)address;
    (void)mode;
    request_stop(StopReason::ExecutionFault, "the verify-player fixture is a reference-runner tool");
#endif
}

#if defined(SHADOW_CPU_REFERENCE_BUILD)
namespace {
// The native pass of a hooked scoped entry: the hook as try_hle runs it, or - declined - the translated body.
void run_owner_pass(Cpu& c) {
    const u32 entry = c.pc;
    if (c.serve_hle_for_verify(entry)) return;
    c.pc = entry;
    if (c.recompiled()->find_range(entry) == nullptr) {
        c.request_stop(StopReason::ExecutionFault, "native owner declined; the original body is not in this image");
        return;
    }
    enter_translated_range(c);
}
}  // namespace
#endif

void Cpu::verify_scoped_dispatch(RecompiledCode::Fn native, u32 entry, bool owner) {
#if defined(SHADOW_CPU_REFERENCE_BUILD)
    const u64 verified0 = verify_.verified, mismatches0 = verify_.mismatches, device0 = verify_.skipped_device,
              exit0 = verify_.skipped_exit, instructions0 = verify_.verified_instructions;
    const auto began = verify_sampling_.enabled ? std::chrono::steady_clock::now()
                                                : std::chrono::steady_clock::time_point{};
    verify_owner_pass_ = owner;
    verify_dispatch(owner ? &run_owner_pass : native, entry);
    verify_owner_pass_ = false;
    if (verify_sampling_.enabled) {
        const double spent = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
        verify_tokens_ -= spent;
        verify_sampling_counts_.verifier_seconds += spent;
    }
    VerifyEntryStats& s = verify_entry_stats_[entry];
    const u64 mismatches = verify_.mismatches - mismatches0, verified = verify_.verified - verified0;
    s.verified += verified;
    s.mismatches += mismatches;
    s.skipped_device += verify_.skipped_device - device0;
    s.skipped_exit += verify_.skipped_exit - exit0;
    s.instructions += verify_.verified_instructions - instructions0;
    if (owner) {
        s.owner_verified += verified;
        s.owner_mismatches += mismatches;
    }
    if (mismatches != 0u && s.first_mismatch_tick == 0u) s.first_mismatch_tick = clock_.now();
#else
    (void)native;
    (void)entry;
    (void)owner;
#endif
}

#if defined(SHADOW_CPU_REFERENCE_BUILD)

void Cpu::verify_dispatch(RecompiledCode::Fn native, u32 entry) {
    // A segment that reached a live GX site noted a host effect (gx_site_enter)
    // and is not replayed; one with only dead sites is device-free and is
    // replayed: the interpreter drops the same stores (Cpu::execute).
    ArchitectedState before;
    capture_state(before);
    InterruptFprState before_interrupt_fpr;
    capture_interrupt_fpr_state(before_interrupt_fpr);
    const auto before_deadlines = clock_.deadlines();
    const u64 before_fpr_context_switches = interrupt_fpr_context_switches_;
    const VerifyDiagSnapshot before_diag(cpu_diag());
    const u64 before_cuts = fallback_totals_[static_cast<std::size_t>(FallbackReason::VerifyCut)];

    // No time passes inside a segment (the game clock moves only at wait
    // points, which run outside the oracle), so neither pass can see an event.
    memory_.journal_begin();
    exit_pending_ = native_entry_exit_state();
    verify_in_pass_ = true;
    native(*this);
    finish_transfers();  // R12: the published chain is part of this segment, as its tail calls were
    verify_in_pass_ = false;
    // An interior entry has no try_hle and never consumes the arm; clear it so
    // it cannot outlive this pass.
    verify_armed_entry_ = 0u;
    const bool native_exited = exit_pending_ != 0u;
    const bool native_left = (exit_pending_ & kExitLeave) != 0u;
    exit_pending_ = 0u;
    const bool touched_device = memory_.journal_touched_device() || memory_.journal_overflowed();
    // The three ends the replay can find again (no instruction count exists):
    // the function returned (a taken bclr), it raised its own exception and
    // handed back at the vector, or it stopped on the per-call cut at its
    // first nested callee.
    const u64 native_exceptions = cpu_diag().exceptions_total - before_diag.exceptions_total();
    const bool returned = !native_exited && !stopped();
    const bool faulted = native_left && !stopped() && native_exceptions != 0u;
    const bool cut = native_left && !stopped() && native_exceptions == 0u &&
                     fallback_totals_[static_cast<std::size_t>(FallbackReason::VerifyCut)] -
                             before_cuts == 1u;
    // A pass that handed back at its own entry with no end of its own (an
    // instruction the generator refuses at the head of the range) must reach
    // the dispatcher as a hand-back, so the dispatcher takes its
    // trap-first-instruction step() retry instead of re-dispatching the same
    // address. Never after a stop, and never after a cut or a fault: those
    // ran at least the transfer or the faulting instruction. A jump table
    // that loops back into its own entry (0x80043CB8, the bctr loop's case
    // rows) is cut there after a whole iteration, which need not change the
    // pc, r1, LR and MSR the dispatcher's stall test compares; taken for a
    // stall, its entry instruction was stepped unverified.
    const auto keep_stalled_hand_back = [&]() {
        exit_pending_ = (native_left && !stopped() && pc == entry && leave_site_ == entry && !cut && !faulted)
                            ? kExitLeave
                            : u8{0};
    };
    const u32 native_end_pc = pc;
    const u32 native_end_lr = lr;
    const u32 native_end_sp = gpr[1];

    ArchitectedState after_native;
    capture_state(after_native);
    InterruptFprState after_native_interrupt_fpr;
    capture_interrupt_fpr_state(after_native_interrupt_fpr);
    const auto after_native_deadlines = clock_.deadlines();
    std::vector<GuestMemory::JournalEntry>& native_writes = verify_native_writes_scratch_;
    native_writes.clear();
    if (!touched_device && (returned || faulted || cut)) {
        native_writes.assign(memory_.journal().begin(), memory_.journal().end());
    }
    verify_.native_journal_peak_entries =
        std::max<u64>(verify_.native_journal_peak_entries, native_writes.size());
    // The bytes the native pass left behind, so the interpreter's can be
    // compared against them once memory has been rewound and replayed. Sorted
    // and unique: one comparison per byte however often it was stored.
    std::vector<std::pair<u32, u8>>& native_bytes = verify_native_bytes_scratch_;
    native_bytes.clear();
    std::size_t native_byte_count = 0;
    for (const GuestMemory::JournalEntry& write : native_writes) {
        native_byte_count += write.size;
    }
    native_bytes.reserve(native_byte_count);
    for (const GuestMemory::JournalEntry& write : native_writes) {
        for (u32 index = 0; index < write.size; ++index) {
            native_bytes.emplace_back(write.address + index, u8{0});
        }
    }
    std::sort(native_bytes.begin(), native_bytes.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    native_bytes.erase(
        std::unique(native_bytes.begin(), native_bytes.end(),
                    [](const auto& a, const auto& b) { return a.first == b.first; }),
        native_bytes.end());
    for (auto& touched : native_bytes) {
        touched.second = memory_.peek8(touched.first);
    }
    verify_.unique_native_bytes_peak =
        std::max<u64>(verify_.unique_native_bytes_peak, native_bytes.size());
    verify_.journal_scratch_peak_bytes = std::max<u64>(
        verify_.journal_scratch_peak_bytes,
        static_cast<u64>(memory_.journal_capacity()) * sizeof(GuestMemory::JournalEntry) +
            static_cast<u64>(native_writes.capacity()) * sizeof(GuestMemory::JournalEntry) +
            static_cast<u64>(native_bytes.capacity()) * sizeof(native_bytes[0]));

    // Only a segment that returned, faulted or reached its first nested call
    // has an end the interpreter can find again; device traffic (and a wait
    // point's events) cannot be repeated. Either way the native result stands.
    if (touched_device || !(returned || faulted || cut)) {
        memory_.journal_end();
        if (touched_device) {
            ++verify_.skipped_device;
        } else {
            ++verify_.skipped_exit;
        }
        if (verify_.messages.size() < verify_message_limit_) {
            char row[224];
            std::snprintf(row, sizeof(row), "0x%08X not verified (%s)", entry,
                          touched_device ? "hardware access or a wait point"
                                         : "handed back without returning, faulting or calling");
            verify_.messages.push_back(row);
        }
        keep_stalled_hand_back();
        return;
    }

    memory_.journal_rollback();
    restore_state(before);
    restore_interrupt_fpr_state(before_interrupt_fpr);
    for (std::size_t index = 0; index < before_deadlines.size(); ++index) {
        clock_.schedule(static_cast<GameEvent>(index), before_deadlines[index]);
    }
    interrupt_fpr_context_switches_ = before_fpr_context_switches;
    before_diag.restore(cpu_diag());

    memory_.journal_begin();
    verify_replaying_ = true;
    // The native pass began right after a dispatch whose R1 check took nothing,
    // and native code recognises again only at R2-R5. Starting the replay with
    // the step() flag clear gives the interpreter exactly those points.
    recognise_interrupt_ = false;
    // ---- compare ----
    // What differs between an interpreter state and the native pass's end, as
    // text (empty: the same registers). At most six differences are spelled out.
    using Deadlines = std::array<u64, static_cast<std::size_t>(GameEvent::Count)>;
    const auto register_differences = [&](const ArchitectedState& interpreter,
                                          const InterruptFprState& interpreter_fpr,
                                          const Deadlines& interpreter_deadlines) {
        std::string text;
        char buffer[192];
        int differences = 0;
        const auto note = [&](const char* what, u64 a, u64 b) {
            if (++differences > 6) return;
            char detail[192];
            std::snprintf(detail, sizeof(detail), "%s%s=[i 0x%llX n 0x%llX]", text.empty() ? "" : " ",
                          what, static_cast<unsigned long long>(a), static_cast<unsigned long long>(b));
            text += detail;
        };
        // A native owner's pass is held to the call's contract, not to the original's scratch registers: r1-r4,
        // r13-r31, f1, f14-f31, LR, MSR, CR2-CR4, FPSCR's control byte (RN NI XE ZE UE OE VE; no game code reads
        // the status bits, docs/NATIVE_MSL_MATH.md) and where the call ends. Translated passes compare everything.
        const bool owner = verify_owner_pass_;
        // r4 is a return register only for a 64-bit result: an entry whose every caller overwrites r4 before reading
        // it (set_verify_r4_dead, proven from the binary) is not held to it.
        const bool r4_dead = owner && std::binary_search(verify_r4_dead_.begin(), verify_r4_dead_.end(), entry);
        const auto volatile_gpr = [owner, r4_dead](int r) {
            return owner && (r == 0 || (r >= 5 && r <= 12) || (r == 4 && r4_dead));
        };
        const auto volatile_fpr = [owner](int r) { return owner && (r == 0 || (r >= 2 && r <= 13)); };
        for (int index = 0; index < 32; ++index) {
            if (!volatile_gpr(index) && interpreter.gpr[index] != after_native.gpr[index]) {
                std::snprintf(buffer, sizeof(buffer), "r%d", index);
                note(buffer, interpreter.gpr[index], after_native.gpr[index]);
            }
            if (volatile_fpr(index)) continue;
            if (interpreter.fpr[index].bits[0] != after_native.fpr[index].bits[0]) {
                std::snprintf(buffer, sizeof(buffer), "f%d ps0", index);
                note(buffer, interpreter.fpr[index].bits[0], after_native.fpr[index].bits[0]);
            }
            if (interpreter.fpr[index].bits[1] != after_native.fpr[index].bits[1]) {
                std::snprintf(buffer, sizeof(buffer), "f%d ps1", index);
                note(buffer, interpreter.fpr[index].bits[1], after_native.fpr[index].bits[1]);
            }
        }
        if (interpreter.pc != after_native.pc) note("pc", interpreter.pc, after_native.pc);
        if (interpreter.lr != after_native.lr) note("lr", interpreter.lr, after_native.lr);
        if (!owner && interpreter.ctr != after_native.ctr) note("ctr", interpreter.ctr, after_native.ctr);
        if ((owner ? (interpreter.cr ^ after_native.cr) & 0x00FFF000u : interpreter.cr ^ after_native.cr) != 0u)
            note("cr", interpreter.cr, after_native.cr);
        if (!owner && interpreter.xer != after_native.xer) note("xer", interpreter.xer, after_native.xer);
        if (interpreter.msr != after_native.msr) note("msr", interpreter.msr, after_native.msr);
        if ((owner ? (interpreter.fpscr ^ after_native.fpscr) & 0xFFu : interpreter.fpscr ^ after_native.fpscr) != 0u)
            note("fpscr", interpreter.fpscr, after_native.fpscr);
        for (std::size_t index = 0; index < interpreter.spr.size(); ++index) {
            if (interpreter.spr[index] != after_native.spr[index]) {
                std::snprintf(buffer, sizeof(buffer), "spr%zu", index);
                note(buffer, interpreter.spr[index], after_native.spr[index]);
            }
        }
        if (interpreter.time_base_offset != after_native.time_base_offset)
            note("time_base_offset", interpreter.time_base_offset, after_native.time_base_offset);
        if (interpreter.dec_written_at != after_native.dec_written_at)
            note("dec_written_at", interpreter.dec_written_at, after_native.dec_written_at);
        if (interpreter.dec_value != after_native.dec_value)
            note("dec_value", interpreter.dec_value, after_native.dec_value);
        for (std::size_t index = 0; index < after_native_deadlines.size(); ++index) {
            if (interpreter_deadlines[index] != after_native_deadlines[index]) {
                std::snprintf(buffer, sizeof(buffer), "deadline_%s",
                              game_event_name(static_cast<GameEvent>(index)));
                note(buffer, interpreter_deadlines[index], after_native_deadlines[index]);
            }
        }
        if (interpreter.pending_decrementer != after_native.pending_decrementer)
            note("pending_decrementer", interpreter.pending_decrementer, after_native.pending_decrementer);
        if (interpreter.external_interrupt != after_native.external_interrupt)
            note("external_interrupt", interpreter.external_interrupt, after_native.external_interrupt);
        if (interpreter.reservation != after_native.reservation)
            note("reservation", interpreter.reservation, after_native.reservation);
        if (interpreter.reservation_address != after_native.reservation_address)
            note("reservation_address", interpreter.reservation_address, after_native.reservation_address);
        if (interpreter_fpr.depth != after_native_interrupt_fpr.depth)
            note("interrupt_fpr_depth", interpreter_fpr.depth, after_native_interrupt_fpr.depth);
        if (interpreter_fpr.resume != after_native_interrupt_fpr.resume)
            note("interrupt_fpr_resume", interpreter_fpr.resume, after_native_interrupt_fpr.resume);
        if (interpreter_fpr.fpscr != after_native_interrupt_fpr.fpscr)
            note("interrupt_fpscr", interpreter_fpr.fpscr, after_native_interrupt_fpr.fpscr);
        for (int index = 0; index < 32; ++index) {
            if (interpreter_fpr.registers[index].bits[0] != after_native_interrupt_fpr.registers[index].bits[0]) {
                std::snprintf(buffer, sizeof(buffer), "interrupt_f%d ps0", index);
                note(buffer, interpreter_fpr.registers[index].bits[0],
                     after_native_interrupt_fpr.registers[index].bits[0]);
            }
            if (interpreter_fpr.registers[index].bits[1] != after_native_interrupt_fpr.registers[index].bits[1]) {
                std::snprintf(buffer, sizeof(buffer), "interrupt_f%d ps1", index);
                note(buffer, interpreter_fpr.registers[index].bits[1],
                     after_native_interrupt_fpr.registers[index].bits[1]);
            }
        }
        for (std::size_t index = 0; index < interpreter.sr.size(); ++index) {
            if (interpreter.sr[index] != after_native.sr[index]) {
                std::snprintf(buffer, sizeof(buffer), "sr%zu", index);
                note(buffer, interpreter.sr[index], after_native.sr[index]);
            }
        }
        return text;
    };
    // Every byte the native pass wrote must hold the native value now, and
    // every byte the interpreter wrote must be among them (a narrower native
    // store would otherwise verify whenever the leading bytes agreed).
    // --verify-player owner passes: a native owner's subtree runs other native owners where the replay runs their
    // original words, and those leave different scratch in the dead stack below the r1 the call returns with - bytes
    // nothing can read (the CharaColli verify and the savestate A/B exclude the same window). Translated passes keep
    // the every-byte comparison.
    const auto dead_stack = [&](u32 address) {
        return verify_owner_pass_ && address < native_end_sp && native_end_sp - address <= 0x10000u;
    };
    const auto memory_differences = [&](const std::vector<GuestMemory::JournalEntry>& interpreter_writes) {
        char buffer[192];
        for (const auto& touched : native_bytes) {
            if (dead_stack(touched.first)) continue;
            const u8 current = memory_.peek8(touched.first);
            if (current != touched.second) {
                std::snprintf(buffer, sizeof(buffer), "memory 0x%08X: interpreter 0x%02X native 0x%02X",
                              touched.first, current, touched.second);
                return std::string(buffer);
            }
        }
        for (const GuestMemory::JournalEntry& write : interpreter_writes) {
            for (u32 index = 0; index < write.size; ++index) {
                const u32 address = write.address + index;
                if (dead_stack(address)) continue;
                const auto covered = std::lower_bound(
                    native_bytes.begin(), native_bytes.end(), address,
                    [](const auto& byte, u32 at) { return byte.first < at; });
                if (covered == native_bytes.end() || covered->first != address) {
                    // An owner pass that left a byte alone matches an original that stored the value it already held
                    // (a memoised native owner skips the store): the byte's value before the call - the interpreter
                    // journal's first `previous` for it - must equal its value now.
                    if (verify_owner_pass_) {
                        u8 before = 0u;
                        bool found = false;
                        for (const GuestMemory::JournalEntry& first : interpreter_writes) {
                            if (address >= first.address && address < first.address + first.size) {
                                const u32 shift = 8u * (first.size - 1u - (address - first.address));
                                before = static_cast<u8>(first.previous >> shift);
                                found = true;
                                break;
                            }
                        }
                        if (found && before == memory_.peek8(address)) continue;
                    }
                    std::snprintf(buffer, sizeof(buffer), "memory 0x%08X written by the interpreter only",
                                  address);
                    return std::string(buffer);
                }
            }
        }
        return std::string();
    };

    // The replay ends where the native pass ended (no instruction count says
    // when):
    //   * returned - at the first taken bclr, either LK, that lands at the
    //     native return pc with the native r1. A callee entered through an
    //     interior thunk is not cut, and its blr lands just after its call
    //     site instead. The generator lowers blrl (an indirect call through
    //     LR, like the OS switch-thread callback) as the same
    //     `c.pc = target; return;` as blr.
    //   * faulted - at the vector, once as many exceptions were taken.
    //   * cut - at an arrival at the cut entry, but not simply the first one
    //     with the native LR and r1. Native code crosses a row entry's thunk
    //     (its try_hle: the cut) on a call, on a bctr and on a transfer
    //     between records, but passes a row entry absorbed into the running
    //     record inline - fall-through or goto - with no cut: case 1's row
    //     runs into case 2's entry, and the loop's bctr later reaches case 2
    //     through the thunk. The running record is invisible here, so the
    //     replay ends at the first arrival, after at least one instruction
    //     (the arm is consumed by the dispatched entry, so a jump table that
    //     loops back into it - 0x80043CB8 - is cut at its second arrival),
    //     where every register and every written byte equals the native end.
    //     Exact when the native pass is right (its own arrival matches), and
    //     an earlier arrival matches only when nothing differs.
    // A bound stops a replay that never gets there (a mismatch by definition:
    // the native pass did).
    constexpr u64 kVerifyReplayCap = 50000000;
    const auto replay_exceptions = [&]() {
        return cpu_diag().exceptions_total - before_diag.exceptions_total();
    };
    bool cut_arrived = false;
    std::string first_cut_arrival;
    const auto cut_reached = [&]() {
        if (pc != native_end_pc || lr != native_end_lr || gpr[1] != native_end_sp ||
            replay_exceptions() != 0u) {
            return false;
        }
        ArchitectedState now;
        capture_state(now);
        InterruptFprState now_interrupt_fpr;
        capture_interrupt_fpr_state(now_interrupt_fpr);
        std::string differences = register_differences(now, now_interrupt_fpr, clock_.deadlines());
        if (differences.empty()) {
            if (memory_.journal_touched_device() || memory_.journal_overflowed()) {
                differences = "the interpreter touched hardware or overflowed the write journal";
            } else {
                differences = memory_differences(memory_.journal());
                if (differences.empty()) return true;
            }
        }
        if (!cut_arrived) {
            cut_arrived = true;
            first_cut_arrival = differences;
        }
        return false;
    };
    bool replay_ended = false;
    u64 replayed = 0;
    while (!stopped() && !replay_ended && replayed < kVerifyReplayCap) {
        const u32 at = pc;
        u32 fetch = 0;
        const bool at_return = returned && translate(at, false, true, fetch) &&
                               (memory_.peek32(fetch) & 0xFC0007FEu) == 0x4C000020u;
        if (!verify_coverage_.empty() && at >= kVerifyCoverageBase && at < kVerifyCoverageEnd) {
            const u32 bit = (at - kVerifyCoverageBase) >> 2;
            verify_coverage_[bit >> 6] |= u64{1} << (bit & 63u);
        }
        step();
        ++replayed;
        if (returned) {
            replay_ended = at_return && pc != at + 4u && pc == native_end_pc && gpr[1] == native_end_sp;
        } else if (cut) {
            replay_ended = cut_reached();
        } else {
            replay_ended = replay_exceptions() == native_exceptions && pc == native_end_pc;
        }
    }
    ArchitectedState after_interpreter;
    capture_state(after_interpreter);
    InterruptFprState after_interpreter_interrupt_fpr;
    capture_interrupt_fpr_state(after_interpreter_interrupt_fpr);
    const auto after_interpreter_deadlines = clock_.deadlines();

    verify_replaying_ = false;
    // The replay's journal_begin reset both flags. The native pass was
    // device-free and within the journal limit, so a replay that reached
    // MMIO/unmapped memory, or recorded more writes than the journal holds,
    // did something the native pass did not.
    const bool interpreter_touched_device = memory_.journal_touched_device();
    const bool interpreter_overflowed = memory_.journal_overflowed();
    const std::vector<GuestMemory::JournalEntry>& interpreter_writes = memory_.journal();
    verify_.interpreter_journal_peak_entries =
        std::max<u64>(verify_.interpreter_journal_peak_entries, interpreter_writes.size());
    memory_.journal_end();

    std::string mismatch;
    const auto append = [&](const std::string& part) {
        if (part.empty()) return;
        if (!mismatch.empty()) mismatch += " ";
        mismatch += part;
    };
    if (!replay_ended && !stopped()) {
        append(returned ? "the interpreter did not return within the replay bound"
               : cut    ? "the interpreter did not reach the native pass's cut state within the replay bound"
                        : "the interpreter did not reach the native pass's exception within the replay bound");
        if (cut_arrived) append("(first arrival at the cut: " + first_cut_arrival + ")");
    }
    // A cut that ended was compared, registers and memory, at its arrival.
    if (!(cut && replay_ended)) {
        append(register_differences(after_interpreter, after_interpreter_interrupt_fpr,
                                    after_interpreter_deadlines));
        if (interpreter_touched_device) {
            append("interpreter touched hardware (MMIO or unmapped) that the native pass did not");
        }
        if (interpreter_overflowed) {
            append("interpreter overflowed the write journal the native pass stayed within");
        }
        if (mismatch.empty()) mismatch = memory_differences(interpreter_writes);
    }

    if (mismatch.empty()) {
        ++verify_.verified;
        verify_.verified_instructions += replayed;
    } else {
        ++verify_.mismatches;
        if (verify_.mismatch_messages.size() < 32u) {
            char row[512];
            std::snprintf(row, sizeof(row),
                          "0x%08X after %llu interpreted instructions (pc i 0x%08X n 0x%08X): %s", entry,
                          static_cast<unsigned long long>(replayed), after_interpreter.pc,
                          after_native.pc, mismatch.c_str());
            verify_.mismatch_messages.push_back(row);
        }
        // A failed oracle is not a valid state to continue from.
        request_stop(StopReason::ExecutionFault, "recompiler verification mismatch: " + mismatch);
    }
    // The interpreter's result is the reference, so it is what the machine keeps.
    keep_stalled_hand_back();
}
#endif

const char* fallback_reason_name(FallbackReason reason) noexcept {
    switch (reason) {
        case FallbackReason::NotRecompiled: return "target has no native code";
        case FallbackReason::SystemCall: return "sc (system call vector)";
        case FallbackReason::ReturnFromInterrupt: return "rfi";
        case FallbackReason::Trap: return "trap taken";
        case FallbackReason::UnexpectedReturn: return "callee returned elsewhere";
        case FallbackReason::EndOfFunction: return "ran past the analysed function";
        case FallbackReason::Yield: return "stop, interrupt or wait-point yield";
        case FallbackReason::VerifyCut: return "per-call verification cut";
        default: return "none";
    }
}

void Cpu::record_fallback(u32 target, FallbackReason reason, u32 site) {
    const u64 key = (static_cast<u64>(static_cast<u8>(reason)) << 32) | target;
    auto it = fallback_sites_.find(key);
    if (it == fallback_sites_.end()) {
        FallbackSite entry;
        entry.target = target;
        entry.caller = site;
        entry.reason = reason;
        entry.count = 1;
        entry.first_dispatch = recompiled_calls;
        fallback_sites_.emplace(key, entry);
        return;
    }
    ++it->second.count;
    if (it->second.caller == 0) {
        it->second.caller = site;
    }
}

std::vector<Cpu::FallbackSite> Cpu::fallback_sites() const {
    std::vector<FallbackSite> sites;
    sites.reserve(fallback_sites_.size());
    for (const auto& entry : fallback_sites_) {
        sites.push_back(entry.second);
    }
    // Boot order, so the audit works through the path the game actually takes.
    std::sort(sites.begin(), sites.end(), [](const FallbackSite& a, const FallbackSite& b) {
        return a.first_dispatch < b.first_dispatch;
    });
    return sites;
}

void Cpu::idle_wait() {
    // A listed polling loop took its back edge (cpu.h): only an event - a
    // retrace, an audio frame, an alarm, a completed transfer - can change
    // what it polls, so the game clock advances to its next event and
    // delivers it. The generated loop then re-reads what it polls (or its
    // needs_dispatch() check takes a now-deliverable interrupt).
    end_poll_tracking();
    if (idle_work_hook_ && idle_work_hook_(*this) && needs_dispatch()) return;  // the loop dispatches into the work
    wait_for_event();
}

void Cpu::capture_poll_registers(PollRegisters& out) const noexcept {
    std::memcpy(out.gpr, gpr, sizeof(out.gpr));
    out.cr = cr;
    out.xer = xer;
    out.ctr = ctr;
    out.lr = lr;
    out.fpscr = fpscr;
    out.msr = msr_;
    for (std::size_t index = 0; index < 32u; ++index) {
        out.fpr[2u * index] = fpr[index].bits[0];
        out.fpr[2u * index + 1u] = fpr[index].bits[1];
    }
}

void Cpu::end_poll_tracking() noexcept {
    memory_.watch_end();
    poll_site_ = 0;
}

void Cpu::poll_point(u32 site) {
    CpuDiagCounters& diag = cpu_diag();
    ++diag.poll_points;
    PollRegisters now;
    capture_poll_registers(now);
    if (poll_site_ != site || std::memcmp(&now, &poll_registers_, sizeof(now)) != 0) {
        // Another loop, or this iteration changed a register: progress.
        memory_.watch_end();
        poll_site_ = site;
        poll_registers_ = now;
        return;
    }
    if (verifying_) {
        // The oracle's segment journal is in use; a register fixed point is
        // taken as the wait (the segment is then not replayed).
        wait_for_event();
        return;
    }
    if (!memory_.watch_active()) {
        // The registers came back unchanged (or the last watch saw real work):
        // watch the next iteration's writes.
        ++diag.poll_watches;
        memory_.watch_begin(kPollWatchLimit);
        return;
    }
    if (!memory_.watch_unchanged()) {
        // Unchanged registers, but memory moved: progress. Watch again.
        memory_.watch_begin(kPollWatchLimit);
        return;
    }
    // A fixed point: nothing but an event can end this loop.
    ++diag.poll_fixed_points;
    memory_.watch_end();
    wait_for_event();
}

void Cpu::wait_for_event() {
    // Delivering events changes host state a verified segment cannot replay.
    memory_.journal_note_host_effect();
    if (idle_handler_) {
        idle_handler_(*this);
        return;
    }
    request_stop(StopReason::ExecutionFault,
                 "the guest is idle and no game clock owner is installed to advance time");
}

// RD1: the dispatch table's 32-bit origin: a byte of this image's read-only
// data, so no function can share its address (a function anchor did: the
// linker's identical-COMDAT folding merged it with empty test stubs, 15:2x)
// and no slot's distance is 0.
static const unsigned char kDispatchAnchor = 0;

void RecompiledCode::install(const Entry* entries, std::size_t count) {
    entries_ = entries;
    count_ = count;
    slots_.clear();
    low_ = 0;
    high_ = 0;
    std::size_t refused = 0;
    bool have_translated = false;
    for (std::size_t index = 0; index < count; ++index) {
        const u32 address = entries[index].address;
        if (address < GuestMemory::kImageBase) {
            // A real-mode alias from a generated tree older than the native
            // exception entry: nothing can ever run there.
            ++refused;
            continue;
        }
        if (!have_translated) {
            low_ = address;
            high_ = address + 4u;
            have_translated = true;
            continue;
        }
        low_ = std::min(low_, address);
        high_ = std::max(high_, address + 4u);
    }
    // RD1: 32-bit slots, each the function's distance from kDispatchAnchor (a
    // read-only byte of this image, never a function), 0 = absent.
    // Every entry is checked to round-trip exactly; one that would not (a
    // function more than 2 GiB from the anchor, impossible within one image) stops the program here instead of dispatching
    // anywhere else.
    anchor_ = reinterpret_cast<std::intptr_t>(&kDispatchAnchor);
    if (have_translated) slots_.assign((high_ - low_) >> 2, 0);
    for (std::size_t index = 0; index < count; ++index) {
        const u32 address = entries[index].address;
        if (address < GuestMemory::kImageBase) continue;
        const Fn fn = entries[index].fn;
        s32 delta = 0;
        if (fn != nullptr) {
            const std::intptr_t distance = reinterpret_cast<std::intptr_t>(fn) - anchor_;
            delta = static_cast<s32>(distance);
            if (distance == 0 || static_cast<std::intptr_t>(delta) != distance ||
                reinterpret_cast<Fn>(anchor_ + static_cast<std::intptr_t>(delta)) != fn) {
                std::fprintf(stderr, "[dispatch-table] entry 0x%08X does not fit a 32-bit slot\n", address);
                std::fflush(stderr);
                std::abort();
            }
        }
        slots_[(address - low_) >> 2] = delta;
    }
    if (refused != 0u) {
        std::fprintf(stderr,
                     "[dispatch-table] %zu entries below the program image refused: real-mode aliases from "
                     "a stale generated tree (regenerate; no guest code runs in real mode)\n",
                     refused);
        std::fflush(stderr);
    }
    // The dense span is proportional to high_ - low_, so one entry far from the
    // code region multiplies the committed footprint. The healthy footprint is
    // ~17 MiB; a table an order of magnitude past that is a placement
    // regression, not a bigger program.
    constexpr std::size_t kExpectedCeilingBytes = 256u * 1024u * 1024u;
    if (table_bytes() > kExpectedCeilingBytes) {
        std::fprintf(stderr,
                     "[dispatch-table] WARNING: lookup tables commit %.1f MiB (dense span 0x%08X-0x%08X). An "
                     "entry is stretching the dense span; give its region its own table instead.\n",
                     static_cast<double>(table_bytes()) / (1024.0 * 1024.0), low_, high_);
        std::fflush(stderr);
    }
}

// ---------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------

void Cpu::handoff_native_call(u32 target, u32 site) noexcept {
    leave_to_dispatcher(target, FallbackReason::Yield, site);
    if (recompiled_ != nullptr && recompiled_->lookup(target) != nullptr)
        pending_native_call_ = target;
}

bool Cpu::finish_transfers() {
    // R12. Each published entry runs only after the body that published it has
    // returned, so the host stack holds one body frame at a time however long
    // the chain is. Nothing is tested between entries - not the budget, a stop
    // or an interrupt - because the tail call this replaces tested nothing
    // either: the entered body's own recognition points decide (cpu.h,
    // transfer_to).
    while ((exit_pending_ & kExitTransfer) != 0u) {
        exit_pending_ = static_cast<u8>(exit_pending_ & ~kExitTransfer);
        const RecompiledCode::Fn entry = transfer_fn_;
        ++native_transfers_driven;
        entry(*this);
    }
    return exiting();
}

#if defined(SHADOW_CPU_REFERENCE_BUILD)
void Cpu::run() { (void)run_steps(~0ull); }

u64 Cpu::run_steps(u64 max_steps) {
    u64 steps = 0;
#else
void Cpu::run() {
#endif
    // A stop request served by an earlier run ends with that run; a new run
    // starts with none (the report itself is cleared by clear_stop()).
    stop_requested_.store(stopped(), std::memory_order_relaxed);
    // A latched data-access fault halts the machine until reset() or
    // load_state() replaces it. A caller that cleared the stop report - the
    // savestate hotkey loop clears every stop after servicing a request - must
    // not resume guest code that already ran past a failed access.
    if (memory_fault_.latched) {
        restate_memory_fault();
    }
    // 2026-09-05 forensic timing (--profile): wall inside this loop, split into
    // native call trees and the dispatcher's own work between them.
    CpuDiagCounters& diag = cpu_diag();
    const bool timing = diag.dispatch_timing;
    const auto run_begin = timing ? std::chrono::steady_clock::now()
                                  : std::chrono::steady_clock::time_point{};
#if defined(SHADOW_CPU_REFERENCE_BUILD)
    while (steps < max_steps && !stopped()) {
        ++steps;
#else
    while (!stopped()) {
#endif
        // A watch never crosses a dispatch (its journal would collect whatever
        // runs next, and the verifier journals segments itself). The tracked
        // site and registers stay: a loop that hands back - an interrupt, or a
        // per-call cut under --verify-recomp on EVERY iteration - is still the
        // same loop when it comes back, and forgetting it each time hid the
        // fixed point forever (stage1/verify-6).
        memory_.watch_end();
        // A stop a host thread posted (request_stop_from_host): served here,
        // on the guest thread, with the guest state committed.
        if (host_stop_state_.load(std::memory_order_acquire) == 2u) {
            serve_host_stop();
            break;
        }
        ++diag.dispatch_iterations;
        const bool entering_native_call = pending_native_call_ && *pending_native_call_ == pc &&
                                          recompiled_ != nullptr && recompiled_->lookup(pc) != nullptr;
        pending_native_call_.reset();
        // Recognition point R1 (cpu.h, interrupt_lines_). A deliverable
        // external or decrementer condition is taken here: the OS's exception
        // entry is native (os_exception_entry.cpp), so the very next dispatch
        // below enters the handler as native code - or, when the entry is
        // refused (no OS vector installed), the machine stops right here and
        // nothing runs after the exception. A condition that is pending while
        // EE = 0 is left latched - it is delayed, never discarded (audit
        // dispatch-exceptions-1: sc, trap and FP-unavailable hand-backs used to
        // arrive here with EE already cleared and threw the decrementer away).
        if (!entering_native_call && interrupt_lines_ != 0u && (msr_ & kMsrEE) != 0u) {
            ++dispatch_blocked_interrupt;
            const auto raise_begin = timing ? std::chrono::steady_clock::now()
                                            : std::chrono::steady_clock::time_point{};
            take_pending_interrupt();
            if (timing) {
                diag.interrupt_raise_ns += static_cast<u64>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - raise_begin)
                        .count());
            }
            if (stopped()) break;  // a refused OS entry: nothing runs after the exception
        }
        // A pass that ended at a lowered call site (Cpu::lift_site_cut, under
        // --verify-recomp) or a resumption exactly at one: the site runs once,
        // here, outside the oracle, and dispatch follows its continuation - the same
        // order as an owner call served by the hook path below.
        if (!entering_native_call && run_lift_site_at_pc()) {
            continue;
        }
        const bool hooked = !entering_native_call && recompiled_ != nullptr && !hle_.empty() && find_hle(pc) != nullptr;
        const RecompiledCode::Range* hooked_range = hooked ? recompiled_->find_range(pc) : nullptr;
        // A hooked address is entered through its owning range (the hook runs
        // once, then the range) or - a native owner, whose original body is not
        // in the image - through its declared entry thunk, which serves the
        // hook itself (Cpu::serve_native_owner). A hooked address with neither
        // has no translated code and is refused below: a hook is never served
        // as a hidden fallback for missing code.
        const bool hooked_owner = hooked && hooked_range == nullptr && recompiled_->lookup(pc) != nullptr;
        if (hooked && hooked_range == nullptr && !hooked_owner) {
            ++dispatch_blocked_hle;
        } else if (recompiled_ != nullptr && recompiled_->lookup(pc) == nullptr) {
            ++dispatch_no_native_code;
        }
        if (recompiled_ != nullptr && (!hooked || hooked_range != nullptr || hooked_owner)) {
            RecompiledCode::Fn native = recompiled_->lookup(pc);
            if (hooked_range != nullptr) {
                // A hook at a translated address must not force step() to
                // interpret its first instruction when the hook declines.
                // Run it once, then enter the owning native range directly:
                // the public function thunk would call the hook a second
                // time, while an interior thunk would not call it at all.
                // Not used while verifying: a reference build under
                // --verify-recomp takes the once-only hook path below instead.
                native = +[](Cpu& c) {
                    const u32 entry = c.pc;
                    const RecompiledCode::Range* owned = c.recompiled()->find_range(entry);
                    c.npc = entry + 4u;
                    if (c.try_hle(entry)) return;
                    owned->at(c, entry);
                };
            }
            const RecompiledCode::Range* range = nullptr;
            if (native == nullptr) {
                // Not a declared entry point. The guest computed this address -
                // a jump table, a function pointer into the middle of a routine,
                // an SRR0 that is not a block boundary - so find the translated
                // function that owns it and start there. This is the alternative
                // to interpreting from an address the recompiler did not
                // anticipate; it runs the same translated instructions.
                range = recompiled_->find_range(pc);
            }
            if (native != nullptr || range != nullptr) {
#if defined(SHADOW_CPU_REFERENCE_BUILD)
                close_interpreter_strand();
#endif
                ++recompiled_calls;
                const u32 entry = pc;
                // The no-progress test below asks where this pass gave up.
                leave_site_ = kNoLeaveSite;
#if defined(SHADOW_CPU_REFERENCE_BUILD)
                // Every kind of finished attempt counts against the limit;
                // omitting the "too long" skips used to let a route full of
                // oversized segments spin on the limit check forever.
                const u64 checked = verify_.verified + verify_.skipped_device +
                                    verify_.skipped_exit + verify_.skipped_hle +
                                    verify_.mismatches;
                // Arm the per-call cut protocol for this entry whether or not
                // this particular dispatch is verified: once the limit is
                // reached, execution must still proceed segment-by-segment or
                // an unarmed entry thunk would cut to the dispatcher with
                // nothing retired and spin.
                verify_armed_entry_ = verifying_ ? pc : 0u;
                if (verifying_ && (hooked_range != nullptr || hooked_owner) && !verify_scope_.empty() &&
                    in_verify_scope(pc) && checked < verify_call_limit_ && verify_take_sample(pc)) {
                    // --verify-player: a native owner at a scoped entry is what the oracle is for - its pass is
                    // compared with the original words, not skipped as an HLE call.
                    verify_armed_entry_ = 0u;
                    verify_scoped_dispatch(nullptr, pc, true);
                } else if (verifying_ && (hooked_range != nullptr || hooked_owner)) {
                    // A hooked translated address. Handlers keep host state the
                    // journal cannot roll back (the AX mixer's voice fractions,
                    // probe counters), and a served call contains no translated
                    // code for the two engines to compare, so while verifying
                    // the handler runs exactly once, here, outside the oracle -
                    // the same order the hook lambda above uses. try_hle_slow
                    // cuts before every hooked thunk so nested calls arrive
                    // here too, and the replay never invokes handlers. A
                    // declined call's translated body is then verified as a
                    // segment of its own, from the state the handler left.
                    const HleEntry* const hook = find_hle(pc);
                    exit_pending_ = native_entry_exit_state();
                    npc = pc + 4u;
                    ++handler_depth_;
                    const bool served = hook->handler(*this);
                    --handler_depth_;
                    if (served) {
                        ++const_cast<HleEntry*>(hook)->hits;
                        if ((exit_pending_ & kExitLeave) == 0u && !stopped()) pc = lr;
                        // Fast mode: a served call is either an owner sampled out (counted there) or out of scope.
                        if (checked < verify_call_limit_ && !verify_sampling_.enabled) {
                            ++verify_.skipped_hle;
                            if (verify_.messages.size() < verify_message_limit_) {
                                char row[224];
                                std::snprintf(row, sizeof(row),
                                              "0x%08X not verified (HLE handler served the call)",
                                              entry);
                                verify_.messages.push_back(row);
                            }
                        }
                    } else if (hooked_owner) {
                        // A native owner has no translated body to verify
                        // instead: declining is the same stop its thunk takes.
                        request_stop(StopReason::ExecutionFault,
                                     "native owner declined; the original body is not in this image");
                    } else if (!stopped() && (exit_pending_ & kExitLeave) == 0u && pc == entry) {
                        // at() has no try_hle, so nested calls must cut on
                        // their own.
                        verify_armed_entry_ = 0u;
                        if (checked < verify_call_limit_ && verify_scope_.empty()) {
                            verify_dispatch(&enter_translated_range, pc);
                        } else if (checked < verify_call_limit_ && verify_candidate(pc) &&
                                   verify_take_sample(pc)) {
                            verify_scoped_dispatch(&enter_translated_range, pc, false);
                        } else {
                            enter_translated_range(*this);
                        }
                    }
                } else if (verifying_ && checked < verify_call_limit_ && !verify_scope_.empty() &&
                           verify_candidate(pc) && verify_take_sample(pc)) {
                    if (native == nullptr) ++dispatch_entered_mid_function;
                    verify_scoped_dispatch(native != nullptr ? native : &enter_translated_range, pc, false);
                } else if (verifying_ && checked < verify_call_limit_ && verify_scope_.empty()) {
                    // A dispatch with no declared entry is verified like any
                    // other: its cold mid-block entry and resumed suffix are
                    // exactly what the oracle exists to
                    // check, and they used to run unjournalled and uncounted.
                    if (native == nullptr) ++dispatch_entered_mid_function;
                    verify_dispatch(native != nullptr ? native : &enter_translated_range, pc);
                } else
#endif
                {
                    exit_pending_ = native_entry_exit_state();
                    const auto native_begin = timing ? std::chrono::steady_clock::now()
                                                     : std::chrono::steady_clock::time_point{};
                    if (native != nullptr) {
                        native(*this);
                    } else {
                        ++dispatch_entered_mid_function;
                        range->at(*this, pc);
                    }
                    // R12: the transfers the entered code published before
                    // returning to the dispatcher (the chain its tail calls
                    // used to run inside this call).
                    finish_transfers();
                    if (timing) {
                        ++diag.native_calls_timed;
                        diag.native_ns += static_cast<u64>(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - native_begin)
                                .count());
                    }
                }
                const u8 exit_state = exit_pending_;
                [[maybe_unused]] const bool gave_up = (exit_state & kExitLeave) != 0u;
                // Native code unwound at a recognition point because an interrupt
                // became deliverable inside it (R2: after a call returned). pc is
                // that point; the R1 check at the top of the loop takes it.
                const bool interrupt_unwind = exit_state == kExitInterrupt;
                if (exit_state == 0u) ++diag.native_normal_returns;
                if (interrupt_unwind) {
                    ++interpreter_exits;
                    ++fallback_totals_[static_cast<std::size_t>(FallbackReason::Yield)];
                    if (record_fallbacks_) record_fallback(pc, FallbackReason::Yield, 0u);
                }
                exit_pending_ = 0u;
                // A pass that gave up at the very instruction it was dispatched
                // to (leave site == entry) executed nothing: dispatching it
                // again would repeat it forever, so the instruction is executed
                // directly instead (the reference interpreter) or the machine
                // stops (production). Every other hand-back made progress,
                // whatever it changed: a loop that gives up at its bctr and
                // resumes at its own entry, a thread that blocked while the OS
                // switched to a new thread created with the same routine, a
                // stop or an interrupt at a loop edge. (The instruction clock
                // answered this exactly - retired nothing; the leave site does
                // now. Comparing r1/LR/MSR took a loop that changed only other
                // registers for a stall: difftest F1b bnd shape 13.) A
                // verification cut names the thunk it cut at, not an
                // instruction that gave up.
                const bool no_progress = gave_up && pc == entry && leave_site_ == entry &&
                                         leave_reason_ != FallbackReason::VerifyCut && !stopped() &&
                                         !interrupt_pending() && !stop_requested();
#if defined(SHADOW_CPU_REFERENCE_BUILD)
                if ((entering_native_call && interrupt_pending()) || !no_progress)
#else
                if (!no_progress || interrupt_unwind || (entering_native_call && interrupt_pending()))
#endif
                {
                    continue;
                }
            }
        }
#if defined(SHADOW_CPU_REFERENCE_BUILD)
        // Audit tag: why native dispatch was not possible here (cpu_diag.h).
        diag.interpreter_cause =
            recompiled_ == nullptr                                   ? u8{4}
            : (!hle_.empty() && find_hle(pc) != nullptr)             ? u8{1}
            : (recompiled_->lookup(pc) == nullptr &&
               recompiled_->find_range(pc) == nullptr)                ? u8{2}
                                                                     : u8{3};
        open_interpreter_strand();
        // Arriving here IS the hand-back a leave_to_dispatcher asked for: this
        // loop is the dispatcher, and no native frame is on the stack to
        // unwind. Leaving the request latched made it permanent in a build
        // with no translated code attached - the next HLE handler to serve a
        // call then found kExitLeave still set and never resumed at its link
        // address. The native path clears the same request after every
        // dispatch, a few lines above; this is the interpreted path's copy.
        exit_pending_ = 0u;
        step();
#else
        if (!stopped()) {
            const bool has_entry = recompiled_ != nullptr && recompiled_->lookup(pc) != nullptr;
            const bool has_range = recompiled_ != nullptr && recompiled_->find_range(pc) != nullptr;
            char detail[192];
            std::snprintf(detail, sizeof(detail),
                          "native execution cannot continue at 0x%08X (LR=0x%08X CTR=0x%08X entry=%u range=%u); PPC interpretation is forbidden",
                          pc, lr, ctr, has_entry ? 1u : 0u, has_range ? 1u : 0u);
            request_stop(StopReason::ExecutionFault, detail);
        }
        break;
#endif
    }
#if defined(SHADOW_CPU_REFERENCE_BUILD)
    close_interpreter_strand();
#endif
    stop_requested_.store(false, std::memory_order_relaxed);
    if (timing) {
        diag.run_ns += static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - run_begin)
                .count());
    }
#if defined(SHADOW_CPU_REFERENCE_BUILD)
    return steps;
#endif
}

// A "strand" is one uninterrupted run of interpreted instructions: the guest
// arrived at an address with no native code, and every instruction from there
// until the dispatcher next finds native code again is interpreted. Attributing
// the whole strand to the address that started it is what turns "15 million
// instructions were interpreted" into "this function is not translated".
#if defined(SHADOW_CPU_REFERENCE_BUILD)
void Cpu::open_interpreter_strand() {
    if (strand_pc_ != 0u) return;
    strand_pc_ = pc == 0u ? 4u : pc;  // 0 is the "not stranded" sentinel
    strand_lr_ = lr;
    strand_start_instruction_ = interpreted_instructions_;
    strand_start_ns_ = std::chrono::steady_clock::now();
}

void Cpu::close_interpreter_strand() {
    if (strand_pc_ == 0u) return;
    const u64 instructions = interpreted_instructions_ - strand_start_instruction_;
    const u32 origin = strand_pc_;
    const u32 origin_lr = strand_lr_;
    const auto started = strand_start_ns_;
    strand_pc_ = 0u;
    if (instructions == 0u) return;
    const u64 elapsed = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started)
            .count());
    interpreted_ns_ += elapsed;
    InterpreterStrand& strand = interpreter_strands_[origin];
    if (strand.entries == 0u) {
        strand.address = origin;
        strand.first_dispatch = recompiled_calls;
        strand.link = origin_lr;
    }
    ++strand.entries;
    strand.instructions += instructions;
    strand.host_ns += elapsed;
    strand.last_link = origin_lr;
    if (instructions > strand.longest_instructions) {
        strand.longest_instructions = instructions;
    }
}

#endif

std::vector<Cpu::InterpreterStrand> Cpu::interpreter_strands() const {
    std::vector<InterpreterStrand> strands;
    strands.reserve(interpreter_strands_.size());
    for (const auto& entry : interpreter_strands_) strands.push_back(entry.second);
    std::sort(strands.begin(), strands.end(),
              [](const InterpreterStrand& a, const InterpreterStrand& b) {
                  return a.instructions > b.instructions;
              });
    return strands;
}

#if defined(SHADOW_CPU_REFERENCE_BUILD)
void Cpu::step() {
    // Asynchronous exceptions are recognised at exactly the instruction
    // boundaries native code can hand back at (cpu.h, interrupt_lines_): after
    // an R2/R3/R4/R5 instruction (recognise_interrupt_), and immediately before
    // an mtmsr (R4, the emitted pre-write check). Cpu::run adds R1 before every
    // step it makes. Recognising at every boundary here instead - what this
    // function used to do for the external line, and tick_time_base did for the
    // decrementer - put the --verify-recomp replay at vectors the native pass
    // could never reach (audit oracle-reference-5).
    if (interrupt_lines_ != 0u && (msr_ & kMsrEE) != 0u) {
        bool boundary = recognise_interrupt_;
        if (!boundary) {
            u32 fetch = 0;
            boundary = translate(pc, false, true, fetch) &&
                       // primary 31, extended 146 (mtmsr), decoded as the
                       // generator and op_group31 decode it
                       (memory_.peek32(fetch) & 0xFC0007FEu) == 0x7C000124u;
        }
        if (boundary) {
            take_pending_interrupt();
            if (stopped()) {  // a refused OS entry: the instruction at pc does not run
                recognise_interrupt_ = false;
                return;
            }
        }
    }
    recognise_interrupt_ = false;

    u32 address = 0;
    if (!translate(pc, false, true, address)) {
        request_stop(StopReason::TranslationFault,
                     "instruction fetch with MSR[IR] off: real mode does not exist here");
        return;
    }
    const u32 instruction = memory_.read32(address);

    if (!history_.empty()) {
        history_[history_head_] = TraceEntry{pc, instruction};
        const std::size_t history_capacity = history_.size();
        history_head_ = (history_capacity & (history_capacity - 1u)) == 0u
                            ? (history_head_ + 1u) & (history_capacity - 1u)
                            : (history_head_ + 1u) % history_capacity;
        if (history_size_ < history_.size()) {
            ++history_size_;
        }
    }

    npc = pc + 4u;

    // The --verify-recomp replay re-executes the guest instructions the native
    // pass translated. It never invokes a handler: while verifying, every
    // handler runs exactly once, from the dispatcher, and native passes cut
    // before hooked thunks. A replay that starts at a hooked address is the
    // body of a call whose handler declined; one that branches to a hooked
    // address without a call mirrors native code, which runs no handler there.
    // Invoking it here ran host side effects again (AX voice advance) and
    // compared a serving handler with itself.
    if (const HleEntry* entry = verify_replaying_ ? nullptr : find_hle(pc)) {
        if (entry->handler(*this)) {
            // A handler that fully served the call left its results in guest
            // state; execution resumes at LR, as on the generated thunk path.
            if ((exit_pending_ & kExitLeave) == 0u && !stopped()) pc = lr;
            npc = pc;
            // R2: the served thunk returns to its caller's exiting() check.
            recognise_interrupt_ = true;
            return;
        }
    }

    // A lowered call site (extract/lift-sites.txt): what the generated code
    // does at the same word - the generated lowering and its continuation.
    // Never inside a replay: every translated pass ends before a
    // site while verifying (lift_site_cut_slow), so a replayed segment holds none.
    if (!verify_replaying_ && run_lift_site_at_pc()) {
        return;
    }

    // A Gekko extension HID2 disables is an illegal instruction (PAGE 90):
    // program exception 0x700, SRR1 bit 12, the instruction does not retire.
    // Only dcbz_l's LCE can be clear (LSQE|PSE are machine invariants); same
    // predicate as the static translator (audit paired-single-10, change C).
    const u32 hid2_required = instruction_checked_hid2(instruction);
    if ((spr[SPR_HID2] & hid2_required) != hid2_required) {
        raise_exception(Exception::Program, kSrr1IllegalInstruction);
        pc = npc;
        return;
    }

    // Gekko raises 0x800 before an FP instruction retires whenever MSR[FP] is
    // clear. This must use the same classifier as the static translator so the
    // interpreter remains a real differential oracle for lazy FPU switching.
    if ((msr_ & kMsrFP) == 0u && instruction_requires_fp(instruction)) {
        raise_exception(Exception::FpUnavailable);
        pc = npc;
        return;
    }

    execute(instruction);

    pc = npc;
    // The interpreter's own diagnostic count (strands). Not a clock: no time
    // passes between two instructions, here or in native code.
    ++interpreted_instructions_;
}

void Cpu::unimplemented(u32 instruction, const char* form) {
    char buffer[192];
    std::snprintf(buffer, sizeof(buffer),
                  "unimplemented %s instruction 0x%08X (primary %u, extended %u) at 0x%08X", form,
                  instruction, instruction >> 26, (instruction >> 1) & 0x3ffu, pc);
    stop_.instruction = instruction;
    request_stop(StopReason::UnimplementedInstruction, buffer);
}

void Cpu::execute(u32 instruction) {
    {
        // TOTAL_NON_NATIVE_PPC_EXECUTIONS (cpu_diag.h). This is the only
        // function that executes a guest instruction's semantics outside
        // generated native code; nothing may reach the opcode switch below
        // without passing this line.
        CpuDiagCounters& diag = cpu_diag();
        g_non_native_ppc_executions.fetch_add(1, std::memory_order_relaxed);
        if (verify_replaying_) {
            // Cause 5, the oracle's own replay, is a scalar. Recording it per
            // pc filled all 256 origin slots within a few segments, after which
            // every replayed instruction walked the whole table and the real
            // interpretation origins (causes 1-4) could no longer be recorded.
            ++diag.verify_replay_executions;
        } else {
            const u8 cause = diag.interpreter_cause;
            bool recorded = false;
            for (auto& origin : diag.non_native_origins) {
                if (origin.count == 0u) {
                    origin.pc = pc;
                    origin.instruction = instruction;
                    origin.lr = lr;
                    origin.cause = cause;
                    origin.count = 1;
                    origin.first_interpreted = interpreted_instructions_;
                    origin.first_ns = static_cast<u64>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count());
                    origin.thread_hash = std::hash<std::thread::id>{}(std::this_thread::get_id());
                    recorded = true;
                    break;
                }
                if (origin.pc == pc && origin.cause == cause) {
                    ++origin.count;
                    recorded = true;
                    break;
                }
            }
            if (!recorded) ++diag.non_native_origins_overflow;
        }
    }
    const u32 opcode = instruction >> 26;
    // A listed GX pipe site (gekko/gx_sites.h): the same meaning the generated
    // code has, never a store to the pipe page.
    if (gx_site_count_ != 0u &&
        ((opcode >= 36u && opcode <= 39u) || opcode == 44u || opcode == 45u ||
         (opcode >= 52u && opcode <= 55u) || opcode == 60u || opcode == 61u)) {
        if (const GxSiteRecord* site = find_gx_site(pc); site != nullptr && site->word == instruction) {
            execute_gx_site(*site, instruction);
            return;
        }
    }
    // A listed platform site (gekko/platform_sites.h): the same meaning the
    // generated code has - the native platform's answer, its effect or nothing
    // for a dead store - never a bus access.
    if (platform_site_table_count_ != 0u && (opcode == 40u || opcode == 44u || opcode == 14u)) {
        if (const PlatformSiteRecord* site = find_platform_site(pc); site != nullptr && site->word == instruction) {
            execute_platform_site(*site, instruction);
            return;
        }
    }
    switch (opcode) {
        case 3: {  // twi
            const s32 a = static_cast<s32>(gpr[field_ra(instruction)]);
            const s32 b = field_simm(instruction);
            const u32 to = field_rd(instruction);
            const bool trap = ((to & 0x10u) && a < b) || ((to & 0x08u) && a > b) ||
                              ((to & 0x04u) && a == b) ||
                              ((to & 0x02u) && static_cast<u32>(a) < static_cast<u32>(b)) ||
                              ((to & 0x01u) && static_cast<u32>(a) > static_cast<u32>(b));
            if (trap) {
                raise_exception(Exception::Program);
            }
            break;
        }
        case 4:
            op_group4(instruction);
            break;
        case 7: {  // mulli
            const s64 product = static_cast<s64>(static_cast<s32>(gpr[field_ra(instruction)])) *
                                field_simm(instruction);
            gpr[field_rd(instruction)] = static_cast<u32>(product);
            break;
        }
        case 8: {  // subfic
            const u32 a = gpr[field_ra(instruction)];
            const u32 imm = static_cast<u32>(field_simm(instruction));
            const u32 complement = ~a;
            const u64 result = static_cast<u64>(complement) + imm + 1ull;
            gpr[field_rd(instruction)] = static_cast<u32>(result);
            set_xer_carry(result > 0xffffffffull);
            break;
        }
        case 10: {  // cmpli
            const u32 a = gpr[field_ra(instruction)];
            const u32 b = field_uimm(instruction);
            u32 flags = a < b ? 8u : (a > b ? 4u : 2u);
            if ((xer & kXerSO) != 0u) {
                flags |= 1u;
            }
            set_cr_field((instruction >> 23) & 7u, flags);
            break;
        }
        case 11: {  // cmpi
            const s32 a = static_cast<s32>(gpr[field_ra(instruction)]);
            const s32 b = field_simm(instruction);
            u32 flags = a < b ? 8u : (a > b ? 4u : 2u);
            if ((xer & kXerSO) != 0u) {
                flags |= 1u;
            }
            set_cr_field((instruction >> 23) & 7u, flags);
            break;
        }
        case 12:
        case 13: {  // addic, addic.
            const u32 a = gpr[field_ra(instruction)];
            const u32 imm = static_cast<u32>(field_simm(instruction));
            const u64 result = static_cast<u64>(a) + imm;
            gpr[field_rd(instruction)] = static_cast<u32>(result);
            set_xer_carry(result > 0xffffffffull);
            if (opcode == 13) {
                update_cr0(static_cast<u32>(result));
            }
            break;
        }
        case 14: {  // addi / li
            const u32 ra = field_ra(instruction);
            gpr[field_rd(instruction)] =
                (ra == 0 ? 0u : gpr[ra]) + static_cast<u32>(field_simm(instruction));
            break;
        }
        case 15: {  // addis / lis
            const u32 ra = field_ra(instruction);
            gpr[field_rd(instruction)] =
                (ra == 0 ? 0u : gpr[ra]) + (static_cast<u32>(field_uimm(instruction)) << 16);
            break;
        }
        case 16: {  // bc
            const u32 bo = field_rd(instruction);
            const u32 bi = field_ra(instruction);
            bool condition_ok = true;
            if ((bo & 0x04u) == 0u) {
                --ctr;
                const bool ctr_zero = ctr == 0;
                condition_ok = ((bo & 0x02u) != 0u) ? ctr_zero : !ctr_zero;
            }
            if ((bo & 0x10u) == 0u) {
                const bool bit_set = ((cr >> (31u - bi)) & 1u) != 0u;
                condition_ok = condition_ok && (bit_set == (((bo & 0x08u) != 0u)));
            }
            const u32 target =
                (static_cast<u32>(sign_extend(instruction & 0xfffcu, 16)) +
                 (((instruction >> 1) & 1u) ? 0u : pc));
            if ((instruction & 1u) != 0u) {
                lr = pc + 4u;
            }
            if (condition_ok) {
                npc = target;
                // R3: a taken backward jump is a translated back edge.
                if ((instruction & 1u) == 0u && target <= pc) recognise_interrupt_ = true;
            }
            break;
        }
        case 17:  // sc
            // A system call resumes at the instruction after sc, unlike the
            // fault-style exceptions that re-execute the offending instruction.
            system_call(pc);
            break;
        case 18: {  // b
            const u32 target = static_cast<u32>(sign_extend(instruction & 0x03fffffcu, 26)) +
                               (((instruction >> 1) & 1u) ? 0u : pc);
            if ((instruction & 1u) != 0u) {
                lr = pc + 4u;
            } else if (target <= pc) {
                recognise_interrupt_ = true;  // R3: a translated back edge
            }
            npc = target;
            break;
        }
        case 19:
            op_group19(instruction);
            break;
        case 20: {  // rlwimi
            const u32 rs = field_rd(instruction);
            const u32 ra = field_ra(instruction);
            const u32 mask = ppc_mask((instruction >> 6) & 31u, (instruction >> 1) & 31u);
            const u32 rotated = rotl32(gpr[rs], field_rb(instruction));
            gpr[ra] = (rotated & mask) | (gpr[ra] & ~mask);
            if (field_rc(instruction)) {
                update_cr0(gpr[ra]);
            }
            break;
        }
        case 21: {  // rlwinm
            const u32 mask = ppc_mask((instruction >> 6) & 31u, (instruction >> 1) & 31u);
            gpr[field_ra(instruction)] =
                rotl32(gpr[field_rd(instruction)], field_rb(instruction)) & mask;
            if (field_rc(instruction)) {
                update_cr0(gpr[field_ra(instruction)]);
            }
            break;
        }
        case 23: {  // rlwnm
            const u32 mask = ppc_mask((instruction >> 6) & 31u, (instruction >> 1) & 31u);
            gpr[field_ra(instruction)] =
                rotl32(gpr[field_rd(instruction)], gpr[field_rb(instruction)] & 31u) & mask;
            if (field_rc(instruction)) {
                update_cr0(gpr[field_ra(instruction)]);
            }
            break;
        }
        case 24:  // ori
            gpr[field_ra(instruction)] = gpr[field_rd(instruction)] | field_uimm(instruction);
            break;
        case 25:  // oris
            gpr[field_ra(instruction)] =
                gpr[field_rd(instruction)] | (field_uimm(instruction) << 16);
            break;
        case 26:  // xori
            gpr[field_ra(instruction)] = gpr[field_rd(instruction)] ^ field_uimm(instruction);
            break;
        case 27:  // xoris
            gpr[field_ra(instruction)] =
                gpr[field_rd(instruction)] ^ (field_uimm(instruction) << 16);
            break;
        case 28:  // andi.
            gpr[field_ra(instruction)] = gpr[field_rd(instruction)] & field_uimm(instruction);
            update_cr0(gpr[field_ra(instruction)]);
            break;
        case 29:  // andis.
            gpr[field_ra(instruction)] =
                gpr[field_rd(instruction)] & (field_uimm(instruction) << 16);
            update_cr0(gpr[field_ra(instruction)]);
            break;
        case 31:
            op_group31(instruction);
            break;
        case 32:
        case 33: {  // lwz, lwzu
            const u32 ra = field_ra(instruction);
            const u32 address =
                (ra == 0 && opcode == 32 ? 0u : gpr[ra]) + static_cast<u32>(field_simm(instruction));
            gpr[field_rd(instruction)] = mem_read32(address);
            if (opcode == 33) {
                gpr[ra] = address;
            }
            break;
        }
        case 34:
        case 35: {  // lbz, lbzu
            const u32 ra = field_ra(instruction);
            const u32 address =
                (ra == 0 && opcode == 34 ? 0u : gpr[ra]) + static_cast<u32>(field_simm(instruction));
            gpr[field_rd(instruction)] = mem_read8(address);
            if (opcode == 35) {
                gpr[ra] = address;
            }
            break;
        }
        case 36:
        case 37: {  // stw, stwu
            const u32 ra = field_ra(instruction);
            const u32 address =
                (ra == 0 && opcode == 36 ? 0u : gpr[ra]) + static_cast<u32>(field_simm(instruction));
            mem_write32(address, gpr[field_rd(instruction)]);
            if (opcode == 37) {
                gpr[ra] = address;
            }
            break;
        }
        case 38:
        case 39: {  // stb, stbu
            const u32 ra = field_ra(instruction);
            const u32 address =
                (ra == 0 && opcode == 38 ? 0u : gpr[ra]) + static_cast<u32>(field_simm(instruction));
            mem_write8(address, static_cast<u8>(gpr[field_rd(instruction)]));
            if (opcode == 39) {
                gpr[ra] = address;
            }
            break;
        }
        case 40:
        case 41: {  // lhz, lhzu
            const u32 ra = field_ra(instruction);
            const u32 address =
                (ra == 0 && opcode == 40 ? 0u : gpr[ra]) + static_cast<u32>(field_simm(instruction));
            gpr[field_rd(instruction)] = mem_read16(address);
            if (opcode == 41) {
                gpr[ra] = address;
            }
            break;
        }
        case 42:
        case 43: {  // lha, lhau
            const u32 ra = field_ra(instruction);
            const u32 address =
                (ra == 0 && opcode == 42 ? 0u : gpr[ra]) + static_cast<u32>(field_simm(instruction));
            gpr[field_rd(instruction)] = static_cast<u32>(static_cast<s32>(static_cast<s16>(mem_read16(address))));
            if (opcode == 43) {
                gpr[ra] = address;
            }
            break;
        }
        case 44:
        case 45: {  // sth, sthu
            const u32 ra = field_ra(instruction);
            const u32 address =
                (ra == 0 && opcode == 44 ? 0u : gpr[ra]) + static_cast<u32>(field_simm(instruction));
            mem_write16(address, static_cast<u16>(gpr[field_rd(instruction)]));
            if (opcode == 45) {
                gpr[ra] = address;
            }
            break;
        }
        case 46: {  // lmw
            const u32 ra = field_ra(instruction);
            u32 address = (ra == 0 ? 0u : gpr[ra]) + static_cast<u32>(field_simm(instruction));
            for (u32 reg = field_rd(instruction); reg < 32; ++reg, address += 4u) {
                gpr[reg] = mem_read32(address);
            }
            break;
        }
        case 47: {  // stmw
            const u32 ra = field_ra(instruction);
            u32 address = (ra == 0 ? 0u : gpr[ra]) + static_cast<u32>(field_simm(instruction));
            for (u32 reg = field_rd(instruction); reg < 32; ++reg, address += 4u) {
                mem_write32(address, gpr[reg]);
            }
            break;
        }
        case 48:
        case 49: {  // lfs, lfsu
            const u32 ra = field_ra(instruction);
            const u32 address =
                (ra == 0 && opcode == 48 ? 0u : gpr[ra]) + static_cast<u32>(field_simm(instruction));
            // D.6 bit expansion (types.h): an SNaN is not quieted (audit fpu-4).
            const u64 value = single_to_double_bits(mem_read32(address));
            fpr[field_rd(instruction)].bits[0] = value;
            fpr[field_rd(instruction)].bits[1] = value;
            if (opcode == 49) {
                gpr[ra] = address;
            }
            break;
        }
        case 50:
        case 51: {  // lfd, lfdu
            const u32 ra = field_ra(instruction);
            const u32 address =
                (ra == 0 && opcode == 50 ? 0u : gpr[ra]) + static_cast<u32>(field_simm(instruction));
            fpr[field_rd(instruction)].bits[0] = mem_read64(address);
            if (opcode == 51) {
                gpr[ra] = address;
            }
            break;
        }
        case 52:
        case 53: {  // stfs, stfsu
            const u32 ra = field_ra(instruction);
            const u32 address =
                (ra == 0 && opcode == 52 ? 0u : gpr[ra]) + static_cast<u32>(field_simm(instruction));
            // D.7 store conversion (types.h): bit selection, truncation.
            mem_write32(address, double_to_single_bits(fpr[field_rd(instruction)].bits[0]));
            if (opcode == 53) {
                gpr[ra] = address;
            }
            break;
        }
        case 54:
        case 55: {  // stfd, stfdu
            const u32 ra = field_ra(instruction);
            const u32 address =
                (ra == 0 && opcode == 54 ? 0u : gpr[ra]) + static_cast<u32>(field_simm(instruction));
            mem_write64(address, fpr[field_rd(instruction)].bits[0]);
            if (opcode == 55) {
                gpr[ra] = address;
            }
            break;
        }
        case 56:
            op_psq_load(instruction, false, false);
            break;
        case 57:
            op_psq_load(instruction, false, true);
            break;
        case 59:
            op_group59(instruction);
            break;
        case 60:
            op_psq_store(instruction, false, false);
            break;
        case 61:
            op_psq_store(instruction, false, true);
            break;
        case 63:
            op_group63(instruction);
            break;
        default:
            unimplemented(instruction, "primary");
            break;
    }
}

void Cpu::op_group19(u32 instruction) {
    const u32 extended = (instruction >> 1) & 0x3ffu;
    switch (extended) {
        case 0: {  // mcrf
            set_cr_field((instruction >> 23) & 7u, cr_field((instruction >> 18) & 7u));
            break;
        }
        case 16: {  // bclr
            const u32 bo = field_rd(instruction);
            const u32 bi = field_ra(instruction);
            bool condition_ok = true;
            if ((bo & 0x04u) == 0u) {
                --ctr;
                const bool ctr_zero = ctr == 0;
                condition_ok = ((bo & 0x02u) != 0u) ? ctr_zero : !ctr_zero;
            }
            if ((bo & 0x10u) == 0u) {
                const bool bit_set = ((cr >> (31u - bi)) & 1u) != 0u;
                condition_ok = condition_ok && (bit_set == ((bo & 0x08u) != 0u));
            }
            const u32 target = lr & ~3u;
            if ((instruction & 1u) != 0u) {
                lr = pc + 4u;
            }
            if (condition_ok) {
                npc = target;
                // R2: native returns from the host frame to a caller's
                // exiting() check (or to Cpu::run).
                recognise_interrupt_ = true;
            }
            break;
        }
        case 50: {  // rfi
            const u32 saved = spr[SPR_SRR1];
            set_msr(((msr_ & ~0x87c0ff73u) | (saved & 0x87c0ff73u)) & ~kMsrPOW);
            restore_interrupt_fpr();
            npc = spr[SPR_SRR0] & ~3u;
            recognise_interrupt_ = true;  // R5: native hands to Cpu::run
            break;
        }
        case 150:  // isync
            break;
        case 528: {  // bcctr
            const u32 bo = field_rd(instruction);
            const u32 bi = field_ra(instruction);
            bool condition_ok = true;
            if ((bo & 0x10u) == 0u) {
                const bool bit_set = ((cr >> (31u - bi)) & 1u) != 0u;
                condition_ok = bit_set == ((bo & 0x08u) != 0u);
            }
            const u32 target = ctr & ~3u;
            if ((instruction & 1u) != 0u) {
                lr = pc + 4u;
                if (condition_ok && indirect_call_trace::enabled()) indirect_call_trace::record(pc, target);   // observation only
            }
            if (condition_ok) {
                npc = target;
            }
            break;
        }
        case 33:    // crnor
        case 129:   // crandc
        case 193:   // crxor
        case 225:   // crnand
        case 257:   // crand
        case 289:   // creqv
        case 417:   // crorc
        case 449: {  // cror
            const u32 destination = field_rd(instruction);
            const u32 a = (cr >> (31u - field_ra(instruction))) & 1u;
            const u32 b = (cr >> (31u - field_rb(instruction))) & 1u;
            u32 result = 0;
            switch (extended) {
                case 33: result = ~(a | b) & 1u; break;
                case 129: result = a & ~b & 1u; break;
                case 193: result = a ^ b; break;
                case 225: result = ~(a & b) & 1u; break;
                case 257: result = a & b; break;
                case 289: result = ~(a ^ b) & 1u; break;
                case 417: result = (a | ~b) & 1u; break;
                default: result = a | b; break;
            }
            const u32 mask = 1u << (31u - destination);
            cr = (cr & ~mask) | (result << (31u - destination));
            break;
        }
        default:
            unimplemented(instruction, "group-19");
            break;
    }
}

void Cpu::op_group31(u32 instruction) {
    const u32 extended = (instruction >> 1) & 0x3ffu;
    const u32 d = field_rd(instruction);
    const u32 a = field_ra(instruction);
    const u32 b = field_rb(instruction);
    const bool record = field_rc(instruction);
    const bool overflow_enabled = field_oe(instruction);

    const auto address_ab = [&]() -> u32 { return (a == 0 ? 0u : gpr[a]) + gpr[b]; };
    // lswi/lswx RTL (manual PAGES 448-449): r <- rD-1; i <- 0; while n > 0:
    // if i = 0 then { r <- r+1 (mod 32); GPR(r) <- 0 }; load the byte into
    // GPR(r)[i..i+7]; i <- i+8 (mod 32); n <- n-1. A register is cleared only
    // when a byte is about to load into it, so nothing past the last loaded
    // register changes. The EA is taken before any register is written.
    const auto load_string = [&](u32 address, u32 count, u32 first) {
        u32 reg = (first - 1u) & 31u;
        u32 shift = 0;
        for (u32 index = 0; index < count; ++index) {
            if (shift == 0) {
                reg = (reg + 1u) & 31u;
                gpr[reg] = 0;
                shift = 24;
            } else {
                shift -= 8;
            }
            gpr[reg] |= static_cast<u32>(mem_read8(address + index)) << shift;
        }
    };

    switch (extended) {
        case 0: {  // cmp
            const s32 lhs = static_cast<s32>(gpr[a]);
            const s32 rhs = static_cast<s32>(gpr[b]);
            u32 flags = lhs < rhs ? 8u : (lhs > rhs ? 4u : 2u);
            if ((xer & kXerSO) != 0u) flags |= 1u;
            set_cr_field((instruction >> 23) & 7u, flags);
            break;
        }
        case 32: {  // cmpl
            const u32 lhs = gpr[a];
            const u32 rhs = gpr[b];
            u32 flags = lhs < rhs ? 8u : (lhs > rhs ? 4u : 2u);
            if ((xer & kXerSO) != 0u) flags |= 1u;
            set_cr_field((instruction >> 23) & 7u, flags);
            break;
        }
        case 4: {  // tw
            const s32 lhs = static_cast<s32>(gpr[a]);
            const s32 rhs = static_cast<s32>(gpr[b]);
            const u32 to = d;
            const bool trap = ((to & 0x10u) && lhs < rhs) || ((to & 0x08u) && lhs > rhs) ||
                              ((to & 0x04u) && lhs == rhs) ||
                              ((to & 0x02u) && gpr[a] < gpr[b]) ||
                              ((to & 0x01u) && gpr[a] > gpr[b]);
            if (trap) {
                raise_exception(Exception::Program);
            }
            break;
        }
        case 8:
        case 520: {  // subfc
            const u32 complement = ~gpr[a];
            const u64 result = static_cast<u64>(complement) + gpr[b] + 1ull;
            const u32 value = static_cast<u32>(result);
            set_xer_carry(result > 0xffffffffull);
            if (overflow_enabled) {
                // Signed overflow of ~rA + rB + 1: same-sign operands, result
                // sign differs.
                set_xer_overflow(((complement ^ value) & (gpr[b] ^ value) & 0x80000000u) != 0u);
            }
            gpr[d] = value;
            if (record) update_cr0(value);
            break;
        }
        case 10:
        case 522: {  // addc
            const u64 result = static_cast<u64>(gpr[a]) + gpr[b];
            const u32 value = static_cast<u32>(result);
            set_xer_carry(result > 0xffffffffull);
            if (overflow_enabled) {
                set_xer_overflow(((gpr[a] ^ value) & (gpr[b] ^ value) & 0x80000000u) != 0u);
            }
            gpr[d] = value;
            if (record) update_cr0(value);
            break;
        }
        case 11: {  // mulhwu
            const u64 product = static_cast<u64>(gpr[a]) * gpr[b];
            gpr[d] = static_cast<u32>(product >> 32);
            if (record) update_cr0(gpr[d]);
            break;
        }
        case 19:  // mfcr
            gpr[d] = cr;
            break;
        case 20: {  // lwarx: load and reserve the aligned 32-byte block
            const u32 address = address_ab();
            gpr[d] = mem_read32(address);
            reservation = true;
            reservation_address = address & ~31u;
            break;
        }
        case 23:  // lwzx
            gpr[d] = mem_read32(address_ab());
            break;
        case 24: {  // slw
            const u32 shift = gpr[b] & 63u;
            gpr[a] = shift >= 32u ? 0u : (gpr[d] << shift);
            if (record) update_cr0(gpr[a]);
            break;
        }
        case 26: {  // cntlzw
            u32 count = 0;
            u32 value = gpr[d];
            while (count < 32 && (value & 0x80000000u) == 0u) {
                ++count;
                value <<= 1;
            }
            gpr[a] = count;
            if (record) update_cr0(gpr[a]);
            break;
        }
        case 28:  // and
            gpr[a] = gpr[d] & gpr[b];
            if (record) update_cr0(gpr[a]);
            break;
        case 40:
        case 552: {  // subf
            const u32 value = gpr[b] - gpr[a];
            if (overflow_enabled) {
                set_xer_overflow(((gpr[b] ^ gpr[a]) & (gpr[b] ^ value) & 0x80000000u) != 0u);
            }
            gpr[d] = value;
            if (record) update_cr0(value);
            break;
        }
        case 54:   // dcbst
        case 86:   // dcbf
        case 246:  // dcbtst
        case 278:  // dcbt
        case 470:  // dcbi
        case 982:  // icbi
        case 598:  // sync
        case 566:  // tlbsync
        case 854:  // eieio
        case 306:  // tlbie
        case 370:  // tlbia
            break;
        case 55: {  // lwzux
            // Update forms (manual Chapter 12): EA <- rA + rB; access MEM(EA);
            // rA <- EA. The EA is taken before any register is written and rA
            // is written last, exactly like the D-form updates above.
            const u32 address = gpr[a] + gpr[b];
            gpr[d] = mem_read32(address);
            gpr[a] = address;
            break;
        }
        case 60:  // andc
            gpr[a] = gpr[d] & ~gpr[b];
            if (record) update_cr0(gpr[a]);
            break;
        case 75: {  // mulhw
            const s64 product = static_cast<s64>(static_cast<s32>(gpr[a])) *
                                static_cast<s64>(static_cast<s32>(gpr[b]));
            gpr[d] = static_cast<u32>(static_cast<u64>(product) >> 32);
            if (record) update_cr0(gpr[d]);
            break;
        }
        case 83:  // mfmsr
            gpr[d] = msr_;
            break;
        case 87:  // lbzx
            gpr[d] = mem_read8(address_ab());
            break;
        case 104:
        case 616: {  // neg
            const u32 value = 0u - gpr[a];
            if (overflow_enabled) {
                set_xer_overflow(gpr[a] == 0x80000000u);
            }
            gpr[d] = value;
            if (record) update_cr0(value);
            break;
        }
        case 119: {  // lbzux
            const u32 address = gpr[a] + gpr[b];
            gpr[d] = mem_read8(address);
            gpr[a] = address;
            break;
        }
        case 124:  // nor
            gpr[a] = ~(gpr[d] | gpr[b]);
            if (record) update_cr0(gpr[a]);
            break;
        case 136:
        case 648: {  // subfe
            const u64 carry = (xer & kXerCA) != 0u ? 1ull : 0ull;
            const u32 complement = ~gpr[a];
            const u64 result = static_cast<u64>(complement) + gpr[b] + carry;
            const u32 value = static_cast<u32>(result);
            set_xer_carry(result > 0xffffffffull);
            if (overflow_enabled) {
                // Signed overflow of ~rA + rB + CA.
                set_xer_overflow(((complement ^ value) & (gpr[b] ^ value) & 0x80000000u) != 0u);
            }
            gpr[d] = value;
            if (record) update_cr0(value);
            break;
        }
        case 138:
        case 650: {  // adde
            const u64 carry = (xer & kXerCA) != 0u ? 1ull : 0ull;
            const u64 result = static_cast<u64>(gpr[a]) + gpr[b] + carry;
            const u32 value = static_cast<u32>(result);
            set_xer_carry(result > 0xffffffffull);
            if (overflow_enabled) {
                set_xer_overflow(((gpr[a] ^ value) & (gpr[b] ^ value) & 0x80000000u) != 0u);
            }
            gpr[d] = value;
            if (record) update_cr0(value);
            break;
        }
        case 144: {  // mtcrf
            const u32 crm = (instruction >> 12) & 0xffu;
            u32 mask = 0;
            for (u32 index = 0; index < 8; ++index) {
                if ((crm & (0x80u >> index)) != 0u) {
                    mask |= 0xfu << (28u - 4u * index);
                }
            }
            cr = (cr & ~mask) | (gpr[d] & mask);
            break;
        }
        case 146:  // mtmsr
            // Recognised before (step()) and after (here) the write, R4. An
            // EE 0 -> 1 write with a condition pending takes it before the next
            // instruction (gekko_user_manual.txt PAGE 475).
            set_msr(gpr[d]);
            recognise_interrupt_ = true;
            break;
        case 150: {  // stwcx.
            // Manual PAGE 139: stores only while a reservation exists (whatever
            // its address), cancels it either way, CR0 = 00 || stored || SO.
            const u32 address = address_ab();
            u32 flags = (xer & kXerSO) != 0u ? 1u : 0u;
            if (reservation) {
                mem_write32(address, gpr[d]);
                flags |= 2u;
            }
            reservation = false;
            set_cr_field(0, flags);
            break;
        }
        case 151:  // stwx
            mem_write32(address_ab(), gpr[d]);
            break;
        case 183: {  // stwux
            // rS == rA is valid (only rA == 0 is invalid): the ORIGINAL rS is
            // stored, so rA may only change after the store.
            const u32 address = gpr[a] + gpr[b];
            mem_write32(address, gpr[d]);
            gpr[a] = address;
            break;
        }
        case 200:
        case 712: {  // subfze
            const u64 carry = (xer & kXerCA) != 0u ? 1ull : 0ull;
            const u32 complement = ~gpr[a];
            const u64 result = static_cast<u64>(complement) + carry;
            const u32 value = static_cast<u32>(result);
            set_xer_carry(result > 0xffffffffull);
            if (overflow_enabled) {
                // ~rA + CA overflows only when ~rA >= 0 (rA negative) and the
                // result is negative: the mask is rA, not addze's ~rA.
                set_xer_overflow(((complement ^ value) & gpr[a] & 0x80000000u) != 0u);
            }
            gpr[d] = value;
            if (record) update_cr0(value);
            break;
        }
        case 202:
        case 714: {  // addze
            const u64 carry = (xer & kXerCA) != 0u ? 1ull : 0ull;
            const u64 result = static_cast<u64>(gpr[a]) + carry;
            const u32 value = static_cast<u32>(result);
            set_xer_carry(result > 0xffffffffull);
            if (overflow_enabled) {
                set_xer_overflow(((gpr[a] ^ value) & ~gpr[a] & 0x80000000u) != 0u);
            }
            gpr[d] = value;
            if (record) update_cr0(value);
            break;
        }
        case 210:  // mtsr
            sr[(instruction >> 16) & 15u] = gpr[d];
            break;
        case 242:  // mtsrin
            sr[(gpr[b] >> 28) & 15u] = gpr[d];
            break;
        case 215:  // stbx
            mem_write8(address_ab(), static_cast<u8>(gpr[d]));
            break;
        case 247: {  // stbux (manual PAGE 544)
            const u32 address = gpr[a] + gpr[b];
            mem_write8(address, static_cast<u8>(gpr[d]));
            gpr[a] = address;
            break;
        }
        case 232:
        case 744: {  // subfme
            const u64 carry = (xer & kXerCA) != 0u ? 1ull : 0ull;
            const u32 complement = ~gpr[a];
            const u64 result = static_cast<u64>(complement) + 0xffffffffull + carry;
            const u32 value = static_cast<u32>(result);
            set_xer_carry(result > 0xffffffffull);
            if (overflow_enabled) {
                // ~rA + (-1) + CA: only a negative ~rA can wrap non-negative.
                set_xer_overflow(((complement ^ value) & complement & 0x80000000u) != 0u);
            }
            gpr[d] = value;
            if (record) update_cr0(value);
            break;
        }
        case 234:
        case 746: {  // addme
            const u64 carry = (xer & kXerCA) != 0u ? 1ull : 0ull;
            const u64 result = static_cast<u64>(gpr[a]) + 0xffffffffull + carry;
            const u32 value = static_cast<u32>(result);
            set_xer_carry(result > 0xffffffffull);
            if (overflow_enabled) {
                set_xer_overflow(((gpr[a] ^ value) & gpr[a] & 0x80000000u) != 0u);
            }
            gpr[d] = value;
            if (record) update_cr0(value);
            break;
        }
        case 235:
        case 747: {  // mullw
            const s64 product = static_cast<s64>(static_cast<s32>(gpr[a])) *
                                static_cast<s64>(static_cast<s32>(gpr[b]));
            const u32 value = static_cast<u32>(product);
            if (overflow_enabled) {
                set_xer_overflow(product != static_cast<s64>(static_cast<s32>(value)));
            }
            gpr[d] = value;
            if (record) update_cr0(value);
            break;
        }
        case 266:
        case 778: {  // add
            const u32 value = gpr[a] + gpr[b];
            if (overflow_enabled) {
                set_xer_overflow(((gpr[a] ^ value) & (gpr[b] ^ value) & 0x80000000u) != 0u);
            }
            gpr[d] = value;
            if (record) update_cr0(value);
            break;
        }
        case 279:  // lhzx
            gpr[d] = mem_read16(address_ab());
            break;
        case 284:  // eqv
            gpr[a] = ~(gpr[d] ^ gpr[b]);
            if (record) update_cr0(gpr[a]);
            break;
        case 311: {  // lhzux
            const u32 address = gpr[a] + gpr[b];
            gpr[d] = mem_read16(address);
            gpr[a] = address;
            break;
        }
        case 316:  // xor
            gpr[a] = gpr[d] ^ gpr[b];
            if (record) update_cr0(gpr[a]);
            break;
        case 339: {  // mfspr
            const u32 number = ((instruction >> 16) & 0x1fu) | (((instruction >> 11) & 0x1fu) << 5);
            switch (number) {
                case SPR_XER: gpr[d] = xer; break;
                case SPR_LR: gpr[d] = lr; break;
                case SPR_CTR: gpr[d] = ctr; break;
                case SPR_TBL_READ: gpr[d] = static_cast<u32>(time_base()); break;
                case SPR_TBU_READ: gpr[d] = static_cast<u32>(time_base() >> 32); break;
                case SPR_DEC: gpr[d] = decrementer(); break;
                default: gpr[d] = spr[number]; break;
            }
            break;
        }
        case 343:  // lhax
            gpr[d] = static_cast<u32>(static_cast<s32>(static_cast<s16>(mem_read16(address_ab()))));
            break;
        case 371: {  // mftb
            const u32 number = ((instruction >> 16) & 0x1fu) | (((instruction >> 11) & 0x1fu) << 5);
            gpr[d] = number == SPR_TBU_READ ? static_cast<u32>(time_base() >> 32)
                                            : static_cast<u32>(time_base());
            break;
        }
        case 375: {  // lhaux
            const u32 address = gpr[a] + gpr[b];
            gpr[d] = static_cast<u32>(static_cast<s32>(static_cast<s16>(mem_read16(address))));
            gpr[a] = address;
            break;
        }
        case 407:  // sthx
            mem_write16(address_ab(), static_cast<u16>(gpr[d]));
            break;
        case 412:  // orc
            gpr[a] = gpr[d] | ~gpr[b];
            if (record) update_cr0(gpr[a]);
            break;
        case 439: {  // sthux: stores the original rS even when rS == rA
            const u32 address = gpr[a] + gpr[b];
            mem_write16(address, static_cast<u16>(gpr[d]));
            gpr[a] = address;
            break;
        }
        case 444:  // or / mr
            gpr[a] = gpr[d] | gpr[b];
            if (record) update_cr0(gpr[a]);
            break;
        case 459:
        case 971: {  // divwu
            const u32 divisor = gpr[b];
            u32 value = 0;
            if (divisor == 0) {
                if (overflow_enabled) set_xer_overflow(true);
            } else {
                value = gpr[a] / divisor;
                if (overflow_enabled) set_xer_overflow(false);
            }
            gpr[d] = value;
            if (record) update_cr0(value);
            break;
        }
        case 467: {  // mtspr
            const u32 number = ((instruction >> 16) & 0x1fu) | (((instruction >> 11) & 0x1fu) << 5);
            switch (number) {
                case SPR_XER: xer = gpr[d]; break;
                case SPR_LR: lr = gpr[d]; break;
                case SPR_CTR: ctr = gpr[d]; break;
                case SPR_TBL_WRITE:
                    set_time_base((time_base() & 0xffffffff00000000ull) | gpr[d]);
                    break;
                case SPR_TBU_WRITE:
                    set_time_base((time_base() & 0x00000000ffffffffull) |
                                  (static_cast<u64>(gpr[d]) << 32));
                    break;
                case SPR_DEC:
                    set_decrementer(gpr[d]);
                    set_decrementer_delivery(true);  // the guest arms the decrementer (see cpu.h)
                    break;
                case SPR_HID2: write_hid2(gpr[d]); break;
                default:
                    spr[number] = gpr[d];
                    // Rewriting a BAT changes what every memoised page means.
                    if (number >= SPR_IBAT0U && number <= SPR_DBAT3L) {
                        select_memory_window();
                    }
                    break;
            }
            break;
        }
        case 476:  // nand
            gpr[a] = ~(gpr[d] & gpr[b]);
            if (record) update_cr0(gpr[a]);
            break;
        case 491:
        case 1003: {  // divw
            const s32 dividend = static_cast<s32>(gpr[a]);
            const s32 divisor = static_cast<s32>(gpr[b]);
            s32 value = 0;
            if (divisor == 0 || (static_cast<u32>(dividend) == 0x80000000u && divisor == -1)) {
                if (overflow_enabled) set_xer_overflow(true);
                value = (divisor == 0) ? (dividend < 0 ? -1 : 0) : -1;
            } else {
                value = dividend / divisor;
                if (overflow_enabled) set_xer_overflow(false);
            }
            gpr[d] = static_cast<u32>(value);
            if (record) update_cr0(gpr[d]);
            break;
        }
        case 512: {  // mcrxr
            set_cr_field((instruction >> 23) & 7u, (xer >> 28) & 0xfu);
            xer &= 0x0fffffffu;
            break;
        }
        case 533: {  // lswx
            // n = XER[25-31]. Manual PAGE 449: exactly CEIL(n,4) registers
            // from rD (wrapping r31 -> r0) are altered, each cleared only when
            // its first byte is about to load. n == 0 leaves rD undefined; no
            // register is touched here.
            load_string(address_ab(), xer & 0x7fu, d);
            break;
        }
        case 534:  // lwbrx
            gpr[d] = bswap32(mem_read32(address_ab()));
            break;
        case 535: {  // lfsx
            const u64 value = single_to_double_bits(mem_read32(address_ab()));
            fpr[d].bits[0] = value;
            fpr[d].bits[1] = value;
            break;
        }
        case 536: {  // srw
            const u32 shift = gpr[b] & 63u;
            gpr[a] = shift >= 32u ? 0u : (gpr[d] >> shift);
            if (record) update_cr0(gpr[a]);
            break;
        }
        case 567: {  // lfsux
            const u32 address = gpr[a] + gpr[b];
            const u64 value = single_to_double_bits(mem_read32(address));
            fpr[d].bits[0] = value;
            fpr[d].bits[1] = value;
            gpr[a] = address;
            break;
        }
        case 595:  // mfsr
            gpr[d] = sr[(instruction >> 16) & 15u];
            break;
        case 597: {  // lswi
            // Manual PAGE 448: n = NB, or 32 when NB == 0. rD plays no part in
            // the count.
            u32 count = (instruction >> 11) & 31u;
            if (count == 0) count = 32u;
            load_string(a == 0 ? 0u : gpr[a], count, d);
            break;
        }
        case 599:  // lfdx
            fpr[d].bits[0] = mem_read64(address_ab());
            break;
        case 631: {  // lfdux
            const u32 address = gpr[a] + gpr[b];
            fpr[d].bits[0] = mem_read64(address);
            gpr[a] = address;
            break;
        }
        case 659:  // mfsrin
            gpr[d] = sr[(gpr[b] >> 28) & 15u];
            break;
        case 661: {  // stswx
            const u32 count = xer & 0x7fu;
            const u32 address = address_ab();
            u32 reg = d;
            u32 shift = 24;
            for (u32 index = 0; index < count; ++index) {
                mem_write8(address + index, static_cast<u8>(gpr[reg] >> shift));
                if (shift == 0) {
                    shift = 24;
                    reg = (reg + 1) & 31u;
                } else {
                    shift -= 8;
                }
            }
            break;
        }
        case 662:  // stwbrx
            mem_write32(address_ab(), bswap32(gpr[d]));
            break;
        case 663:  // stfsx
            mem_write32(address_ab(), double_to_single_bits(fpr[d].bits[0]));
            break;
        case 695: {  // stfsux
            const u32 address = gpr[a] + gpr[b];
            mem_write32(address, double_to_single_bits(fpr[d].bits[0]));
            gpr[a] = address;
            break;
        }
        case 725: {  // stswi
            u32 count = ((instruction >> 11) & 31u);
            if (count == 0) count = 32u;
            const u32 address = (a == 0 ? 0u : gpr[a]);
            u32 reg = d;
            u32 shift = 24;
            for (u32 index = 0; index < count; ++index) {
                mem_write8(address + index, static_cast<u8>(gpr[reg] >> shift));
                if (shift == 0) {
                    shift = 24;
                    reg = (reg + 1) & 31u;
                } else {
                    shift -= 8;
                }
            }
            break;
        }
        case 727:  // stfdx
            mem_write64(address_ab(), fpr[d].bits[0]);
            break;
        case 759: {  // stfdux
            const u32 address = gpr[a] + gpr[b];
            mem_write64(address, fpr[d].bits[0]);
            gpr[a] = address;
            break;
        }
        case 790:  // lhbrx
            gpr[d] = bswap16(mem_read16(address_ab()));
            break;
        case 792: {  // sraw
            // CA comes from the ORIGINAL rS (manual PAGE 539). rS is latched
            // before rA is written: with rA == rS the store below overwrites it.
            const u32 shift = gpr[b] & 63u;
            const u32 source = gpr[d];
            const s32 value = static_cast<s32>(source);
            if (shift >= 32u) {
                set_xer_carry(value < 0);
                gpr[a] = value < 0 ? 0xffffffffu : 0u;
            } else {
                set_xer_carry(value < 0 && (source & ((1u << shift) - 1u)) != 0u);
                gpr[a] = static_cast<u32>(value >> shift);
            }
            if (record) update_cr0(gpr[a]);
            break;
        }
        case 824: {  // srawi
            // Same rule as sraw (manual PAGE 540): the shifted-out bits are
            // those of the original rS, which rA == rS would overwrite.
            const u32 shift = field_rb(instruction);
            const u32 source = gpr[d];
            const s32 value = static_cast<s32>(source);
            set_xer_carry(value < 0 && (source & ((1u << shift) - 1u)) != 0u);
            gpr[a] = static_cast<u32>(value >> shift);
            if (record) update_cr0(gpr[a]);
            break;
        }
        case 918:  // sthbrx
            mem_write16(address_ab(), bswap16(static_cast<u16>(gpr[d])));
            break;
        case 922:  // extsh
            gpr[a] = static_cast<u32>(static_cast<s32>(static_cast<s16>(gpr[d])));
            if (record) update_cr0(gpr[a]);
            break;
        case 954:  // extsb
            gpr[a] = static_cast<u32>(static_cast<s32>(static_cast<s8>(gpr[d])));
            if (record) update_cr0(gpr[a]);
            break;
        case 983:  // stfiwx
            mem_write32(address_ab(), static_cast<u32>(fpr[d].bits[0] & 0xffffffffull));
            break;
        case 1014: {  // dcbz
            const u32 address = address_ab() & ~31u;
            for (u32 index = 0; index < 32u; index += 4u) {
                mem_write32(address + index, 0);
            }
            break;
        }
        case 310:  // eciwx
            gpr[d] = mem_read32(address_ab());
            break;
        case 438:  // ecowx
            mem_write32(address_ab(), gpr[d]);
            break;
        default:
            unimplemented(instruction, "group-31");
            break;
    }
}

// ---------------------------------------------------------------------------
// Floating point
// ---------------------------------------------------------------------------

void Cpu::op_group59(u32 instruction) {
    const u32 extended = (instruction >> 1) & 0x1fu;
    const u32 d = field_rd(instruction);
    const u32 a = field_ra(instruction);
    const u32 b = field_rb(instruction);
    const u32 c = field_rc_reg(instruction);
    RefFp kind;
    switch (extended) {
        case 18: kind = RefFp::Div; break;     // fdivs
        case 20: kind = RefFp::Sub; break;     // fsubs
        case 21: kind = RefFp::Add; break;     // fadds
        case 22: kind = RefFp::Sqrt; break;    // fsqrts
        case 24: kind = RefFp::Res; break;     // fres
        case 25: kind = RefFp::Mul; break;     // fmuls
        case 28: kind = RefFp::MSub; break;    // fmsubs
        case 29: kind = RefFp::MAdd; break;    // fmadds
        case 30: kind = RefFp::NMSub; break;   // fnmsubs
        case 31: kind = RefFp::NMAdd; break;   // fnmadds
        default:
            unimplemented(instruction, "group-59");
            return;
    }
    if (kind == RefFp::Mul || kind == RefFp::MAdd || kind == RefFp::MSub || kind == RefFp::NMAdd ||
        kind == RefFp::NMSub) {
        note_single_multiply_frc(fpr[c].bits[0]);
    }
    // One single-precision result, in ps0 and duplicated into ps1 (PAGE 83).
    const u64 result =
        reference_fp(fpscr, kind, true, fpr[a].bits[0], fpr[b].bits[0], fpr[c].bits[0]);
    fpr[d].bits[0] = result;
    fpr[d].bits[1] = result;
    if (field_rc(instruction)) {
        update_cr1();
    }
}

void Cpu::op_group63(u32 instruction) {
    const u32 short_extended = (instruction >> 1) & 0x1fu;
    const u32 d = field_rd(instruction);
    const u32 a = field_ra(instruction);
    const u32 b = field_rb(instruction);
    const u32 c = field_rc_reg(instruction);

    const bool is_a_form = short_extended >= 18u &&
                           (short_extended == 18 || short_extended == 20 || short_extended == 21 ||
                            short_extended == 22 || short_extended == 23 || short_extended == 25 ||
                            short_extended == 26 || short_extended == 28 || short_extended == 29 ||
                            short_extended == 30 || short_extended == 31);
    if (is_a_form) {
        const double va = bits_to_double(fpr[a].bits[0]);
        if (short_extended == 23) {  // fsel: a compare and a move
            const double vb = bits_to_double(fpr[b].bits[0]);
            const double vc = bits_to_double(fpr[c].bits[0]);
            fpr[d].bits[0] = double_to_bits((va >= 0.0 && !std::isnan(va)) ? vc : vb);
        } else {
            RefFp kind;
            switch (short_extended) {
                case 18: kind = RefFp::Div; break;
                case 20: kind = RefFp::Sub; break;
                case 21: kind = RefFp::Add; break;
                case 22: kind = RefFp::Sqrt; break;
                case 25: kind = RefFp::Mul; break;
                case 26: kind = RefFp::Rsqrte; break;
                case 28: kind = RefFp::MSub; break;
                case 29: kind = RefFp::MAdd; break;
                case 30: kind = RefFp::NMSub; break;
                default: kind = RefFp::NMAdd; break;
            }
            fpr[d].bits[0] =
                reference_fp(fpscr, kind, false, fpr[a].bits[0], fpr[b].bits[0], fpr[c].bits[0]);
        }
        if (field_rc(instruction)) {
            update_cr1();
        }
        return;
    }

    const u32 extended = (instruction >> 1) & 0x3ffu;
    switch (extended) {
        case 0:
        case 32: {  // fcmpu, fcmpo (ordered)
            const u32 flags =
                compare_floating(fpr[a].bits[0], fpr[b].bits[0], extended == 32, fpscr);
            set_cr_field((instruction >> 23) & 7u, flags);
            break;
        }
        case 12: {  // frsp
            fpr[d].bits[0] = reference_fp(fpscr, RefFp::Round, true, 0u, fpr[b].bits[0], 0u);
            if (field_rc(instruction)) update_cr1();
            break;
        }
        case 14:
        case 15: {  // fctiw, fctiwz
            const double value = bits_to_double(fpr[b].bits[0]);
            s32 converted;
            if (std::isnan(value)) {
                converted = static_cast<s32>(0x80000000u);
            } else if (value > 2147483647.0) {
                // Table 2-22 (PAGE 85): in non-IEEE mode an invalid conversion
                // (the rounded value exceeds 2^31 - 1) places 0x80000000.
                const double rounded = (extended == 15) ? std::trunc(value) : std::nearbyint(value);
                converted = ((fpscr & kFpscrNI) != 0u && rounded > 2147483647.0)
                                ? static_cast<s32>(0x80000000u)
                                : 2147483647;
            } else if (value < -2147483648.0) {
                converted = static_cast<s32>(0x80000000u);
            } else {
                converted = (extended == 15) ? static_cast<s32>(std::trunc(value))
                                             : static_cast<s32>(std::nearbyint(value));
            }
            fpr[d].bits[0] = 0xfff8000000000000ull | static_cast<u32>(converted);
            if (field_rc(instruction)) update_cr1();
            break;
        }
        case 38:  // mtfsb1
            fpscr |= 1u << (31u - d);
            if (field_rc(instruction)) update_cr1();
            break;
        case 70:  // mtfsb0
            fpscr &= ~(1u << (31u - d));
            if (field_rc(instruction)) update_cr1();
            break;
        case 40:  // fneg
            fpr[d].bits[0] = fpr[b].bits[0] ^ 0x8000000000000000ull;
            if (field_rc(instruction)) update_cr1();
            break;
        case 64:  // mcrfs
            set_cr_field((instruction >> 23) & 7u, (fpscr >> (28u - 4u * ((instruction >> 18) & 7u))) & 0xfu);
            break;
        case 72:  // fmr
            fpr[d].bits[0] = fpr[b].bits[0];
            if (field_rc(instruction)) update_cr1();
            break;
        case 134: {  // mtfsfi
            const u32 field = (instruction >> 23) & 7u;
            const u32 value = (instruction >> 12) & 0xfu;
            const u32 shift = 28u - 4u * field;
            fpscr = (fpscr & ~(0xfu << shift)) | (value << shift);
            if (field_rc(instruction)) update_cr1();
            break;
        }
        case 136:  // fnabs
            fpr[d].bits[0] = fpr[b].bits[0] | 0x8000000000000000ull;
            if (field_rc(instruction)) update_cr1();
            break;
        case 264:  // fabs
            fpr[d].bits[0] = fpr[b].bits[0] & 0x7fffffffffffffffull;
            if (field_rc(instruction)) update_cr1();
            break;
        case 583:  // mffs
            fpr[d].bits[0] = 0xfff8000000000000ull | fpscr;
            if (field_rc(instruction)) update_cr1();
            break;
        case 711: {  // mtfsf
            const u32 fm = (instruction >> 17) & 0xffu;
            const u32 value = static_cast<u32>(fpr[b].bits[0] & 0xffffffffull);
            u32 mask = 0;
            for (u32 index = 0; index < 8; ++index) {
                if ((fm & (0x80u >> index)) != 0u) {
                    mask |= 0xfu << (28u - 4u * index);
                }
            }
            fpscr = (fpscr & ~mask) | (value & mask);
            if (field_rc(instruction)) update_cr1();
            break;
        }
        default:
            unimplemented(instruction, "group-63");
            break;
    }
}

// ---------------------------------------------------------------------------
// Gekko paired singles
// ---------------------------------------------------------------------------

// The reference keeps std::ldexp as an independent formulation of the scale
// (ppc_ops.h uses an exact power-of-two table; ppc_recomp_tests proves the two
// bit-identical), but only for the integer types: type 0 never applies a scale
// (PAGE 493 / PAGE 497).
float Cpu::quantise_load(u32 address, u32 type, u32 scale) {
    const auto factor = [scale]() {
        return std::ldexp(1.0, -static_cast<int>(static_cast<s32>(scale)));
    };
    switch (type) {
        case 4: return static_cast<float>(static_cast<double>(mem_read8(address)) * factor());
        case 5: return static_cast<float>(static_cast<double>(mem_read16(address)) * factor());
        case 6:
            return static_cast<float>(static_cast<double>(static_cast<s8>(mem_read8(address))) * factor());
        case 7:
            return static_cast<float>(static_cast<double>(static_cast<s16>(mem_read16(address))) * factor());
        default: return bits_to_float(mem_read32(address));  // unused: op_psq_load moves type 0-3 words itself
    }
}

void Cpu::quantise_store(u32 address, float value, u32 type, u32 scale) {
    if (type < 4u) {  // unused: op_psq_store moves type 0-3 words itself
        mem_write32(address, float_to_bits(value));
        return;
    }
    const double factor = std::ldexp(1.0, static_cast<int>(static_cast<s32>(scale)));
    const double scaled = static_cast<double>(value) * factor;
    switch (type) {
        case 4:
            mem_write8(address, static_cast<u8>(quantise_saturate(scaled, 0.0, 255.0)));
            break;
        case 5:
            mem_write16(address, static_cast<u16>(quantise_saturate(scaled, 0.0, 65535.0)));
            break;
        case 6:
            mem_write8(address,
                       static_cast<u8>(static_cast<s8>(quantise_saturate(scaled, -128.0, 127.0))));
            break;
        default:  // 7
            mem_write16(address, static_cast<u16>(static_cast<s16>(
                                     quantise_saturate(scaled, -32768.0, 32767.0))));
            break;
    }
}

void Cpu::op_psq_load(u32 instruction, bool indexed, bool update) {
    const u32 d = field_rd(instruction);
    const u32 a = field_ra(instruction);
    u32 address;
    bool w;
    u32 gqr_index;
    if (indexed) {
        address = (a == 0 ? 0u : gpr[a]) + gpr[field_rb(instruction)];
        w = ((instruction >> 10) & 1u) != 0u;
        gqr_index = (instruction >> 7) & 7u;
    } else {
        address = (a == 0 ? 0u : gpr[a]) + static_cast<u32>(sign_extend(instruction & 0xfffu, 12));
        w = ((instruction >> 15) & 1u) != 0u;
        gqr_index = (instruction >> 12) & 7u;
    }

    const u32 gqr = spr[SPR_GQR0 + gqr_index];
    const u32 type = (gqr >> 16) & 7u;
    const u32 scale = static_cast<u32>(gqr_scale(gqr >> 24));
    const u32 element_size = (type == 4 || type == 6) ? 1u : (type == 5 || type == 7) ? 2u : 4u;

    // Type 0 (and the reserved 1-3, treated as type 0) loads the word by the
    // lfs bit expansion: denorms pass (PAGE 111), SNaNs stay signalling.
    const auto element = [&](u32 at) {
        return type < 4u ? single_to_double_bits(mem_read32(at))
                         : double_to_bits(static_cast<double>(quantise_load(at, type, scale)));
    };
    if (w) {
        fpr[d].bits[0] = element(address);
        fpr[d].bits[1] = double_to_bits(1.0);
    } else {
        const u64 first = element(address);
        fpr[d].bits[1] = element(address + element_size);
        fpr[d].bits[0] = first;
    }
    if (update && a != 0) {
        gpr[a] = address;
    }
}

void Cpu::op_psq_store(u32 instruction, bool indexed, bool update) {
    const u32 d = field_rd(instruction);
    const u32 a = field_ra(instruction);
    u32 address;
    bool w;
    u32 gqr_index;
    if (indexed) {
        address = (a == 0 ? 0u : gpr[a]) + gpr[field_rb(instruction)];
        w = ((instruction >> 10) & 1u) != 0u;
        gqr_index = (instruction >> 7) & 7u;
    } else {
        address = (a == 0 ? 0u : gpr[a]) + static_cast<u32>(sign_extend(instruction & 0xfffu, 12));
        w = ((instruction >> 15) & 1u) != 0u;
        gqr_index = (instruction >> 12) & 7u;
    }

    const u32 gqr = spr[SPR_GQR0 + gqr_index];
    const u32 type = gqr & 7u;
    const u32 scale = static_cast<u32>(gqr_scale(gqr >> 8));
    const u32 element_size = (type == 4 || type == 6) ? 1u : (type == 5 || type == 7) ? 2u : 4u;

    // Type 0 stores by bit selection, a denorm as a zero (PAGE 112).
    const auto element = [&](u32 at, u64 bits) {
        if (type < 4u) {
            mem_write32(at, double_to_single_bits_flush(bits));
        } else {
            quantise_store(at, static_cast<float>(bits_to_double(bits)), type, scale);
        }
    };
    element(address, fpr[d].bits[0]);
    if (!w) {
        element(address + element_size, fpr[d].bits[1]);
    }
    if (update && a != 0) {
        gpr[a] = address;
    }
}

void Cpu::op_group4(u32 instruction) {
    const u32 xo6 = (instruction >> 1) & 0x3fu;
    if (xo6 == 6) {
        op_psq_load(instruction, true, false);
        return;
    }
    if (xo6 == 38) {
        op_psq_load(instruction, true, true);
        return;
    }
    if (xo6 == 7) {
        op_psq_store(instruction, true, false);
        return;
    }
    if (xo6 == 39) {
        op_psq_store(instruction, true, true);
        return;
    }

    const u32 d = field_rd(instruction);
    const u32 a = field_ra(instruction);
    const u32 b = field_rb(instruction);
    const u32 c = field_rc_reg(instruction);
    const u32 short_extended = (instruction >> 1) & 0x1fu;

    const auto ps0 = [&](u32 reg) { return bits_to_double(fpr[reg].bits[0]); };
    const auto ps1 = [&](u32 reg) { return bits_to_double(fpr[reg].bits[1]); };
    // Both halves are single-precision results by the reference_fp rules;
    // both are computed from the operands before frD is written.
    const auto set_pair = [&](RefFp kind, u64 a0, u64 b0, u64 c0, u64 a1, u64 b1, u64 c1) {
        const u64 first = reference_fp(fpscr, kind, true, a0, b0, c0);
        const u64 second = reference_fp(fpscr, kind, true, a1, b1, c1);
        fpr[d].bits[0] = first;
        fpr[d].bits[1] = second;
        if (field_rc(instruction)) update_cr1();
    };
    const u64 a0 = fpr[a].bits[0], a1 = fpr[a].bits[1];
    const u64 b0 = fpr[b].bits[0], b1 = fpr[b].bits[1];
    const u64 c0 = fpr[c].bits[0], c1 = fpr[c].bits[1];
    switch (short_extended) {  // the multiplies: what frC held (observation only)
        case 12: case 14: note_single_multiply_frc(c0); break;
        case 13: case 15: note_single_multiply_frc(c1); break;
        case 25: case 28: case 29: case 30: case 31:
            note_single_multiply_frc(c0);
            note_single_multiply_frc(c1);
            break;
        default: break;
    }

    switch (short_extended) {
        case 10: {  // ps_sum0: frC(ps1) "is placed into" frD(ps1), unrounded (PAGE 530)
            const u64 sum = reference_fp(fpscr, RefFp::Add, true, a0, b1, 0u);
            fpr[d].bits[0] = sum;
            fpr[d].bits[1] = c1;
            if (field_rc(instruction)) update_cr1();
            return;
        }
        case 11: {  // ps_sum1: frC(ps0) "is placed into" frD(ps0), unrounded (PAGE 531)
            const u64 sum = reference_fp(fpscr, RefFp::Add, true, a0, b1, 0u);
            fpr[d].bits[0] = c0;
            fpr[d].bits[1] = sum;
            if (field_rc(instruction)) update_cr1();
            return;
        }
        case 12:  // ps_muls0: frA(ps0) and frA(ps1) times frC(ps0)
            set_pair(RefFp::Mul, a0, 0u, c0, a1, 0u, c0);
            return;
        case 13:  // ps_muls1
            set_pair(RefFp::Mul, a0, 0u, c1, a1, 0u, c1);
            return;
        case 14:  // ps_madds0 (PAGE 509)
            set_pair(RefFp::MAdd, a0, b0, c0, a1, b1, c0);
            return;
        case 15:  // ps_madds1 (PAGE 510)
            set_pair(RefFp::MAdd, a0, b0, c1, a1, b1, c1);
            return;
        case 18:  // ps_div
            set_pair(RefFp::Div, a0, b0, 0u, a1, b1, 0u);
            return;
        case 20:  // ps_sub
            set_pair(RefFp::Sub, a0, b0, 0u, a1, b1, 0u);
            return;
        case 21:  // ps_add
            set_pair(RefFp::Add, a0, b0, 0u, a1, b1, 0u);
            return;
        case 23: {  // ps_sel: frD "is set to the contents of" frC or frB (PAGE 528)
            const u64 v0 = (ps0(a) >= 0.0 && !std::isnan(ps0(a))) ? c0 : b0;
            const u64 v1 = (ps1(a) >= 0.0 && !std::isnan(ps1(a))) ? c1 : b1;
            fpr[d].bits[0] = v0;
            fpr[d].bits[1] = v1;
            if (field_rc(instruction)) update_cr1();
            return;
        }
        case 24:  // ps_res
            set_pair(RefFp::Res, 0u, b0, 0u, 0u, b1, 0u);
            return;
        case 25:  // ps_mul
            set_pair(RefFp::Mul, a0, 0u, c0, a1, 0u, c1);
            return;
        case 26:  // ps_rsqrte
            set_pair(RefFp::Rsqrte, 0u, b0, 0u, 0u, b1, 0u);
            return;
        case 28:  // ps_msub
            set_pair(RefFp::MSub, a0, b0, c0, a1, b1, c1);
            return;
        case 29:  // ps_madd (PAGE 508)
            set_pair(RefFp::MAdd, a0, b0, c0, a1, b1, c1);
            return;
        case 30:  // ps_nmsub
            set_pair(RefFp::NMSub, a0, b0, c0, a1, b1, c1);
            return;
        case 31:  // ps_nmadd
            set_pair(RefFp::NMAdd, a0, b0, c0, a1, b1, c1);
            return;
        default:
            break;
    }

    const u32 extended = (instruction >> 1) & 0x3ffu;
    switch (extended) {
        case 0:
        case 32:
        case 64:
        case 96: {  // ps_cmpu0 / ps_cmpo0 / ps_cmpu1 / ps_cmpo1
            const bool high = extended >= 64;
            const bool ordered = extended == 32 || extended == 96;
            const u32 flags = compare_floating(fpr[a].bits[high ? 1 : 0], fpr[b].bits[high ? 1 : 0],
                                               ordered, fpscr);
            set_cr_field((instruction >> 23) & 7u, flags);
            break;
        }
        case 40:  // ps_neg
            fpr[d].bits[0] = fpr[b].bits[0] ^ 0x8000000000000000ull;
            fpr[d].bits[1] = fpr[b].bits[1] ^ 0x8000000000000000ull;
            break;
        case 72:  // ps_mr
            fpr[d].bits[0] = fpr[b].bits[0];
            fpr[d].bits[1] = fpr[b].bits[1];
            break;
        case 136:  // ps_nabs
            fpr[d].bits[0] = fpr[b].bits[0] | 0x8000000000000000ull;
            fpr[d].bits[1] = fpr[b].bits[1] | 0x8000000000000000ull;
            break;
        case 264:  // ps_abs
            fpr[d].bits[0] = fpr[b].bits[0] & 0x7fffffffffffffffull;
            fpr[d].bits[1] = fpr[b].bits[1] & 0x7fffffffffffffffull;
            break;
        case 528: {  // ps_merge00
            const u64 first = fpr[a].bits[0];
            const u64 second = fpr[b].bits[0];
            fpr[d].bits[0] = first;
            fpr[d].bits[1] = second;
            break;
        }
        case 560: {  // ps_merge01
            const u64 first = fpr[a].bits[0];
            const u64 second = fpr[b].bits[1];
            fpr[d].bits[0] = first;
            fpr[d].bits[1] = second;
            break;
        }
        case 592: {  // ps_merge10
            const u64 first = fpr[a].bits[1];
            const u64 second = fpr[b].bits[0];
            fpr[d].bits[0] = first;
            fpr[d].bits[1] = second;
            break;
        }
        case 624: {  // ps_merge11
            const u64 first = fpr[a].bits[1];
            const u64 second = fpr[b].bits[1];
            fpr[d].bits[0] = first;
            fpr[d].bits[1] = second;
            break;
        }
        case 1014: {  // dcbz_l - zero a locked-cache line
            const u32 address = ((a == 0 ? 0u : gpr[a]) + gpr[b]) & ~31u;
            for (u32 index = 0; index < 32u; index += 4u) {
                mem_write32(address + index, 0);
            }
            break;
        }
        default:
            unimplemented(instruction, "group-4");
            break;
    }
    if (field_rc(instruction)) {
        update_cr1();
    }
}
#endif

}  // namespace gekko

