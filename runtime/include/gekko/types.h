#pragma once

// Core scalar types and big-endian helpers shared by the whole Gekko runtime.
// Guest memory always stores original big-endian bytes; the host is little-endian.

#include <cstdint>
#include <cstring>

#if defined(_MSC_VER)
#include <cstdlib>  // _byteswap_ushort / _byteswap_ulong / _byteswap_uint64
#endif

// CPU-03: an inline the compiler may not decline.
//
// The generated recomp shards are 4-5 MB single translation units. MSVC applies
// a per-TU inline budget, exhausts it, and then emits out-of-line CALLS even for
// one-instruction helpers - measured on recomp_030.cpp: 548 calls to
// ppc::rotl32, 153 to ppc::cr_bit, 678 to double_to_bits. `inline` is a hint the
// compiler is free to ignore; this is not.
//
// Use ONLY on genuinely tiny leaf functions, where inlining makes the code
// smaller than the call site it replaces. Forcing a large body at hundreds of
// thousands of sites trades I-cache for nothing.
#if defined(_MSC_VER)
#define GEKKO_ALWAYS_INLINE __forceinline
#define GEKKO_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define GEKKO_ALWAYS_INLINE inline __attribute__((always_inline))
#define GEKKO_NOINLINE __attribute__((noinline))
#else
#define GEKKO_ALWAYS_INLINE inline
#define GEKKO_NOINLINE
#endif

// L0r0-01: one host code-generation model per image.
//
// The inline guest-semantics helpers in these headers are not the same function
// under every host instruction set: ppc_ops.h lowers cntlzw to LZCNT under
// __AVX2__ and to BSR otherwise, and fused_multiply_add to the FMA3 instruction
// under __AVX2__/__FMA__ and to the C library fma otherwise. GEKKO_NOINLINE
// members (Cpu::mem_read32 ...) are emitted as a COMDAT by every TU that calls
// them. Two TUs of one image compiled for different /arch or /fp models
// therefore violate the one-definition rule, and MSVC silently keeps the first
// COMDAT it meets - the executables took Cpu::mem_read32 for all 373,463 shard
// call sites from a baseline-SSE2 shadow_main.obj. Record the model in every
// object so the linker refuses such an image (LNK2038) instead. The values are
// exactly the macros the headers branch on; /arch:SSE4.2 defines none of its
// own, so it shares "SSE" with the default (CMake sets one flag set for all
// consumers - CMakeLists.txt shadow_host_isa - and ctest
// native_host_isa_consistency compares the flags themselves).
#if defined(_MSC_VER)
#if defined(__AVX512F__)
#pragma detect_mismatch("gekko_host_isa", "AVX512")
#elif defined(__AVX2__)
#pragma detect_mismatch("gekko_host_isa", "AVX2")
#elif defined(__AVX__)
#pragma detect_mismatch("gekko_host_isa", "AVX")
#else
#pragma detect_mismatch("gekko_host_isa", "SSE")
#endif
#if defined(__AVX2__) || defined(__FMA__)
#pragma detect_mismatch("gekko_host_fma", "1")
#else
#pragma detect_mismatch("gekko_host_fma", "0")
#endif
#if defined(_M_FP_FAST)
#pragma detect_mismatch("gekko_host_fp", "fast")
#elif defined(_M_FP_STRICT)
#pragma detect_mismatch("gekko_host_fp", "strict")
#else
#pragma detect_mismatch("gekko_host_fp", "precise")  // /fp:precise, explicit or the default
#endif
#if defined(_M_FP_CONTRACT)
#pragma detect_mismatch("gekko_host_fp_contract", "1")
#else
#pragma detect_mismatch("gekko_host_fp_contract", "0")
#endif
#endif

namespace gekko {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s8 = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

constexpr u16 bswap16(u16 value) noexcept {
    return static_cast<u16>((value >> 8) | (value << 8));
}

constexpr u32 bswap32(u32 value) noexcept {
    return ((value & 0x000000ffu) << 24) | ((value & 0x0000ff00u) << 8) |
           ((value & 0x00ff0000u) >> 8) | ((value & 0xff000000u) >> 24);
}

constexpr u64 bswap64(u64 value) noexcept {
    return (static_cast<u64>(bswap32(static_cast<u32>(value))) << 32) |
           static_cast<u64>(bswap32(static_cast<u32>(value >> 32)));
}

// Host byte order helpers.
//
// These are the innermost operation of every guest load and store, so they are
// written as an unaligned host access plus one byte-reversal instruction rather
// than as a byte gather: the guest performs unaligned accesses, so memcpy (not a
// cast) is the only well-defined way to read the host bytes, and MSVC lowers a
// fixed-size memcpy to a single mov. The results are bit-identical to the
// shift-and-or forms these replaced, for every input.
#if defined(_MSC_VER)
inline u16 host_bswap16(u16 value) noexcept { return _byteswap_ushort(value); }
inline u32 host_bswap32(u32 value) noexcept { return _byteswap_ulong(value); }
inline u64 host_bswap64(u64 value) noexcept { return _byteswap_uint64(value); }
#elif defined(__GNUC__) || defined(__clang__)
inline u16 host_bswap16(u16 value) noexcept { return __builtin_bswap16(value); }
inline u32 host_bswap32(u32 value) noexcept { return __builtin_bswap32(value); }
inline u64 host_bswap64(u64 value) noexcept { return __builtin_bswap64(value); }
#else
inline u16 host_bswap16(u16 value) noexcept { return bswap16(value); }
inline u32 host_bswap32(u32 value) noexcept { return bswap32(value); }
inline u64 host_bswap64(u64 value) noexcept { return bswap64(value); }
#endif

inline u16 load_be16(const u8* p) noexcept {
    u16 raw;
    std::memcpy(&raw, p, sizeof(raw));
    return host_bswap16(raw);
}

inline u32 load_be32(const u8* p) noexcept {
    u32 raw;
    std::memcpy(&raw, p, sizeof(raw));
    return host_bswap32(raw);
}

inline u64 load_be64(const u8* p) noexcept {
    u64 raw;
    std::memcpy(&raw, p, sizeof(raw));
    return host_bswap64(raw);
}

inline void store_be16(u8* p, u16 value) noexcept {
    const u16 raw = host_bswap16(value);
    std::memcpy(p, &raw, sizeof(raw));
}

inline void store_be32(u8* p, u32 value) noexcept {
    const u32 raw = host_bswap32(value);
    std::memcpy(p, &raw, sizeof(raw));
}

inline void store_be64(u8* p, u64 value) noexcept {
    const u64 raw = host_bswap64(value);
    std::memcpy(p, &raw, sizeof(raw));
}

// Bit casts between a value and its IEEE-754 bit pattern. They are the hottest helpers of the translated FPU and
// paired-single code, so they must compile to plain register moves under every optimisation level: a std::memcpy
// copy is expanded inline only where intrinsics are enabled (/Oi, part of /O2 but not of /O1), and became an
// out-of-line CRT memcpy call per operand in a recomp shard built at /O1 (2026-10-19: 638 calls in fn_803D3C5C,
// 38-52 % of the game thread in movies). __builtin_bit_cast is a compiler builtin under C++17 as well (MSVC >= 19.27,
// clang, GCC >= 11): identical code to the /O2 memcpy expansion, independent of /Oi.
#if defined(_MSC_VER) || defined(__clang__) || (defined(__GNUC__) && __GNUC__ >= 11)
#define GEKKO_BIT_CAST(To, value) __builtin_bit_cast(To, value)
#else
template <typename To, typename From>
inline To gekko_bit_cast_copy(const From& from) noexcept {
    static_assert(sizeof(To) == sizeof(From), "a bit cast keeps the size");
    To to;
    std::memcpy(&to, &from, sizeof(to));
    return to;
}
#define GEKKO_BIT_CAST(To, value) gekko_bit_cast_copy<To>(value)
#endif

inline float bits_to_float(u32 bits) noexcept {
    return GEKKO_BIT_CAST(float, bits);
}

inline u32 float_to_bits(float value) noexcept {
    return GEKKO_BIT_CAST(u32, value);
}

GEKKO_ALWAYS_INLINE double bits_to_double(u64 bits) noexcept {
    return GEKKO_BIT_CAST(double, bits);
}

GEKKO_ALWAYS_INLINE u64 double_to_bits(double value) noexcept {
    return GEKKO_BIT_CAST(u64, value);
}

// ---------------------------------------------------------------------------
// PowerPC single <-> double format conversions (audit fpu-4, paired-single-8)
// ---------------------------------------------------------------------------
//
// Every FPR holds a double-format value ("Architecturally, all single- and
// double-precision floating-point numbers are represented in double-precision
// format within Gekko", gekko_user_manual.txt PAGE 109). Loading or storing a
// single-precision word converts by bit selection, not by IEEE arithmetic
// (PowerPC Programming Environments manual D.6/D.7; PAGE 108 Table 2-41 lists
// "Store" for every NaN, no quieting). The host casts differ: cvtss2sd and
// cvtsd2ss set the quiet bit of a signalling NaN, and cvtsd2ss rounds and
// saturates where the store truncates. Shared by the recompiler's emitted
// code, ppc_ops.h and the reference interpreter; runtime/tests check them
// against a literal transcription of D.6/D.7.
//
// Both fast paths use the host conversion where it is provably identical, so
// the common case stays a cvt instruction and a compare. They rely on the
// host MXCSR having DAZ clear (denormal operands are not zeroed), which is the
// thread default and nothing in the runtime changes.

// lfs/lfsu/lfsx/lfsux, and psq_l of a type-0 GQR ("passed directly to the
// register without any conversion. This includes the case where the operand
// is a denorm", PAGE 111). D.6: a normal, zero or infinite word widens to the
// same value (denormals are normalised by shifting, which is the value-exact
// widening); a NaN keeps its sign and moves its 23 fraction bits up, so a
// signalling NaN stays signalling.
GEKKO_ALWAYS_INLINE u64 single_to_double_bits(u32 word) noexcept {
    const double value = static_cast<double>(bits_to_float(word));
    if (value != value) {
        return (static_cast<u64>(word & 0x80000000u) << 32) | 0x7ff0000000000000ull |
               (static_cast<u64>(word & 0x007fffffu) << 29);
    }
    return double_to_bits(value);
}

// The D.7 store conversion for everything the host cast cannot reproduce.
//   exponent > 896, or +-0: WORD[0-1] = frS[0-1], WORD[2-31] = frS[5-34]
//     (bit selection: truncation, NaNs unchanged, out-of-range values are not
//     saturated - Table 2-41 "Store");
//   874 <= exponent <= 896: denormalise by shifting the mantissa, 1 to 23
//     places (PAGE 109), i.e. truncate to the single denormal grid;
//   exponent < 874, double denormals included: every mantissa bit shifts out,
//     leaving the sign (Table 2-41 "Double Denormalized: Store zero").
GEKKO_NOINLINE inline u32 double_to_single_bits_slow(u64 bits) noexcept {
    const u32 exponent = static_cast<u32>(bits >> 52) & 0x7ffu;
    if (exponent > 896u || (bits << 1) == 0u) {
        return static_cast<u32>(((bits >> 32) & 0xc0000000u) | ((bits >> 29) & 0x3fffffffu));
    }
    const u32 sign = static_cast<u32>(bits >> 32) & 0x80000000u;
    if (exponent < 874u) {
        return sign;
    }
    const u32 mantissa = 0x80000000u | static_cast<u32>((bits >> 21) & 0x7fffffffu);
    return sign | (mantissa >> (905u - exponent));
}

// stfs/stfsu/stfsx/stfsux. A double the host narrows exactly (every value a
// single-precision instruction, lfs or frsp produces) is already the bit
// selection D.7 defines; anything else - a NaN, an inexact or out-of-range
// double, a double below the single range - takes the literal conversion.
GEKKO_ALWAYS_INLINE u32 double_to_single_bits(u64 bits) noexcept {
    const double value = bits_to_double(bits);
    const float narrowed = static_cast<float>(value);
    if (static_cast<double>(narrowed) == value) {
        return float_to_bits(narrowed);
    }
    return double_to_single_bits_slow(bits);
}

// psq_st to a type-0 GQR: "the value from the FPR is passed directly to the L1
// cache without any conversion, except when this operand is a denorm. In the
// case of a denorm, the value 0.0 is stored" (PAGE 112; PAGE 84 repeats it
// for ps1). A denorm is a nonzero value below the single normal range
// (exponent <= 896, double denormals included). The manual's 0.0 names no
// sign; the sign bit is kept, as the D.7 denormalisation keeps it.
GEKKO_NOINLINE inline u32 double_to_single_bits_flush_slow(u64 bits) noexcept {
    const u32 exponent = static_cast<u32>(bits >> 52) & 0x7ffu;
    if (exponent > 896u || (bits << 1) == 0u) {
        return static_cast<u32>(((bits >> 32) & 0xc0000000u) | ((bits >> 29) & 0x3fffffffu));
    }
    return static_cast<u32>(bits >> 32) & 0x80000000u;
}

// Fast path: an exactly narrowed value that is not a nonzero single denormal
// ((word << 1) - 1 wraps +-0 above the bound) is its own bit selection.
GEKKO_ALWAYS_INLINE u32 double_to_single_bits_flush(u64 bits) noexcept {
    const double value = bits_to_double(bits);
    const float narrowed = static_cast<float>(value);
    const u32 word = float_to_bits(narrowed);
    if (static_cast<double>(narrowed) == value && ((word << 1) - 1u) >= 0x00ffffffu) {
        return word;
    }
    return double_to_single_bits_flush_slow(bits);
}

}  // namespace gekko
