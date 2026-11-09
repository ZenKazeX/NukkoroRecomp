#pragma once
// Shared by guest_call_tests (reference build: the interpreter runs the words written by the test) and guest_call_translated_tests
// (production build: the generator's translation of a real game function found in the recompiled table).
//
// The translated target is NOT a literal address: a literal in a test is a liveness root (extract/liveness-roots.txt), which would keep
// that function's body compiled in the shipping executable for the sake of a test. The rule instead: scan the recompiled table in
// order; the first functions whose game words are exactly one of the two leaf patterns below are the targets. Any such function of the
// game does (they are everywhere and will stay: a `li r3,K ; blr` or `mr r3,r4 ; blr` accessor). When a pattern has no match the test
// says so and fails, it never falls back to an address. The expectation comes from the words themselves, not from a copy of the
// function's behaviour:
//   li r3,K ; blr   -> r3 = K sign-extended (the first argument and r4 are ignored)
//   mr r3,r4 ; blr  -> r3 = the second argument
// Both are leaves with no frame, so the callee leaves every register but r3 alone, and guest_call restores the rest regardless.
#include "gekko/types.h"

namespace guest_call_cases {
constexpr gekko::u32 kBlr = 0x4E800020u;
constexpr gekko::u32 kLiR3 = 0x38600000u;   // li r3,K (K in the low 16 bits)
constexpr gekko::u32 kMrR3R4 = 0x7C832378u;  // mr r3,r4
constexpr gekko::u32 kFirstArgument = 0x0000AAAAu, kSecondArgument = 0x00001234u;
// The immediates the interpreted twin writes (li r3,K ; blr).
constexpr gekko::u32 kImmediates[] = {0u, 1u, 0x7FFFu, 0xFFFFu, 0x8000u};
inline gekko::u32 sign_extend16(gekko::u32 k) { return static_cast<gekko::u32>(static_cast<gekko::s32>(static_cast<gekko::s16>(k & 0xFFFFu))); }
}  // namespace guest_call_cases
