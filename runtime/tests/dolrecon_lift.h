#pragma once

// C++ reconstruction of leaf functions of main.dol (docs/DOLRECON.md "C++ reconstruction").
//
// tools/dolrecon/lift_cpp.py turns verified assembly of leaf functions (no calls, no indirect jumps, every branch
// inside the function) into C++ that runs on its own locals - no Cpu object, no decode, no dispatcher. Each function
// reads the registers it uses from a State, computes on locals, and writes back what it changed; memory goes through
// the small big-endian interface below. dolrecon_lift_tests.cpp proves every function against the ORIGINAL
// instructions executed by the reference interpreter on randomized states (registers, CR, XER, CTR and every byte
// written).

#include <cstddef>
#include <cstdint>
#include <vector>

#include "dolrecon_fp.h"

namespace dolrecon {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s8 = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

// Guest memory as the function sees it: big-endian values at 32-bit guest addresses.
struct Mem {
    virtual ~Mem() = default;
    virtual u8 r8(u32 address) = 0;
    virtual u16 r16(u32 address) = 0;
    virtual u32 r32(u32 address) = 0;
    virtual u64 r64(u32 address) = 0;
    virtual void w8(u32 address, u8 value) = 0;
    virtual void w16(u32 address, u16 value) = 0;
    virtual void w32(u32 address, u32 value) = 0;
    virtual void w64(u32 address, u64 value) = 0;
};

// The architectural state a lifted function can read or change. f holds the double-format bits of ps0 of each FPR,
// p those of ps1 (Gekko's lfs writes both halves; lfd, fmr, fneg, fabs, fnabs write ps0 only). xer is the whole
// register (SO bit 31, OV 30, CA 29). r2 / r13 are read as the small-data bases (EABI: set once by __init_registers).
struct State {
    u32 r[32];
    u64 f[32];
    u64 p[32];
    u32 cr;
    u32 xer;
    u32 ctr;
    u32 lr;
    u32 fpscr;
    u32 gqr[8];  // GQR0-7 (paired-single quantization)
    // Not architecture: the lifted code's run control. fault != 0 unwinds every lifted frame (no lifted callee at an
    // address, the budget ran out, a return to somewhere other than the entry LR); budget counts entries and back edges.
    u32 fault;
    u32 fault_address;
    s64 budget;
    struct TraceEvent* trace_events;  // non-null: record every function entry (divergence localization)
    std::size_t trace_count, trace_capacity;
};

// A function entered by a branch, with the state its body starts from (r1, r3..r10, f1). The harness records the
// same events from the interpreter; on a difference the first differing event names where behaviour first parted.
struct TraceEvent {
    u32 address;
    u32 r1;
    u32 r[8];
    u64 f1;
};
inline void trace_enter(State& s, u32 address) {
    if (s.trace_events == nullptr || s.trace_count >= s.trace_capacity) return;
    TraceEvent& e = s.trace_events[s.trace_count++];
    e.address = address;
    e.r1 = s.r[1];
    for (unsigned i = 0; i < 8; ++i) e.r[i] = s.r[3 + i];
    e.f1 = s.f[1];
}

inline constexpr u32 kFaultBudget = 1u, kFaultNoTarget = 2u, kFaultReturnElsewhere = 3u;

inline constexpr u32 kSdaBase = 0x805EC500u;   // r13, recovered from __init_registers' operands (PAL)
inline constexpr u32 kSda2Base = 0x805FA780u;  // r2

struct Lifted {
    u32 address;
    const char* name;
    void (*fn)(Mem&, State&);
    u32 gpr_in, gpr_out;  // bit n = rN read before written / written
    u32 fpr_in, fpr_out, ps1_out;
    u32 base_regs;        // GPRs used as a load / store base (the test points them at memory)
    u32 index_regs;       // GPRs used as an index (rB of indexed forms; the test gives them small offsets)
    u32 uses_ctr_loop;    // a CTR-decrementing branch: the test keeps CTR small
    u32 instructions;
    u32 calls;            // calls other functions (direct, indirect or tail)
};

extern const Lifted kLifted[];  // sorted by address
extern const std::size_t kLiftedCount;
// The lifted function at `target`, else s.fault = kFaultNoTarget (never a guess).
void dispatch(Mem& m, State& s, u32 target);

// ---- helpers the generated code uses (the architecture's definitions, no host floating point) -----------------
inline u32 rotl32(u32 v, u32 n) { n &= 31u; return n ? (v << n) | (v >> (32u - n)) : v; }
inline u32 mask32(u32 mb, u32 me) {
    const u32 a = 0xFFFFFFFFu >> mb, b = me >= 31u ? 0xFFFFFFFFu : ~(0xFFFFFFFFu >> (me + 1u));
    return mb <= me ? (a & b) : (a | b);
}
inline u32 crf_signed(s32 a, s32 b, u32 xer) { return (a < b ? 8u : a > b ? 4u : 2u) | ((xer >> 31) & 1u); }
inline u32 crf_unsigned(u32 a, u32 b, u32 xer) { return (a < b ? 8u : a > b ? 4u : 2u) | ((xer >> 31) & 1u); }
inline void set_crf(u32& cr, u32 field, u32 value) {
    const u32 shift = (7u - field) * 4u;
    cr = (cr & ~(0xFu << shift)) | ((value & 0xFu) << shift);
}
inline void set_ca(u32& xer, bool ca) { xer = ca ? (xer | 0x20000000u) : (xer & ~0x20000000u); }
inline u32 ca_of(u32 xer) { return (xer >> 29) & 1u; }
inline u32 cntlz(u32 v) { u32 n = 0; while (n < 32u && !(v & 0x80000000u)) { ++n; v <<= 1; } return n; }
inline u32 bswap16(u32 v) { return ((v & 0xFFu) << 8) | ((v >> 8) & 0xFFu); }
inline u32 bswap32(u32 v) { return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24); }

// lfs: single -> double format, bit-exact (PowerPC architecture "Load Floating-Point Single": no rounding, NaN payloads
// and signalling bits kept - unlike a host float -> double conversion, which quiets signalling NaNs).
inline u64 single_to_double_bits(u32 w) {
    const u64 sign = u64(w >> 31) << 63;
    const u32 exp = (w >> 23) & 0xFFu, frac = w & 0x7FFFFFu;
    if (exp == 0xFFu) return sign | (u64(0x7FF) << 52) | (u64(frac) << 29);
    if (exp != 0u) return sign | (u64(exp - 127u + 1023u) << 52) | (u64(frac) << 29);
    if (frac == 0u) return sign;
    // denormal: normalise
    int e = -126;
    u32 m = frac;
    while (!(m & 0x800000u)) { m <<= 1; --e; }
    return sign | (u64(e + 1023) << 52) | (u64(m & 0x7FFFFFu) << 29);
}
// stfs: double -> single format, the architecture's store conversion for EVERY double (PowerPC Table 2-41 "Store"):
// exponent > 896 or +-0: bit selection (sign, top exponent bit, then 30 bits from frS[5..34]: truncation, NaNs kept,
// no saturation); 874..896: denormalise by shifting the mantissa (truncate to the single denormal grid); below 874,
// double denormals included: every mantissa bit shifts out and the store keeps the sign alone.
inline u32 double_to_single_bits(u64 d) {
    const u32 exp11 = u32(d >> 52) & 0x7FFu;
    if (exp11 > 896u || (d << 1) == 0u) return u32((d >> 32) & 0xC0000000u) | u32((d >> 29) & 0x3FFFFFFFu);
    const u32 sign = u32(d >> 32) & 0x80000000u;
    if (exp11 < 874u) return sign;
    const u32 mantissa = 0x80000000u | u32((d >> 21) & 0x7FFFFFFFu);
    return sign | (mantissa >> (905u - exp11));
}

}  // namespace dolrecon
