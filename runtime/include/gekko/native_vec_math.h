#pragma once

// The game's 3-vector helpers (0x8040DDC4 .. 0x8040E048), owned natively (docs/agents/native-math.md).
//
// Original: Metrowerks C++ small functions on {float x, y, z} in guest memory, scalar single arithmetic only (lfs, fadds,
// fsubs, fmuls, fmadds, fmsubs, stfs), run in FPSCR[NI] mode (OSInit sets it). Every function loads its operands in a fixed
// order and stores in a fixed order; Sub, Add, Scale, Dot and Cross read all operands before the first store, the two
// in-place forms (AddTo 0x8040E048, SubFrom 0x8040DE54) store x before they read y and z of the destination, so an
// operand that overlaps the destination sees the original's order. The natives keep that order and use the host's IEEE
// operations through shadow::math (NaN rules, fused multiply-add, the NI flush of denormal operands and results).
//
//   0x8040DDC4 Sub      r3 = r4 - r5            0x8040DE20 Add      r3 = r4 + r5
//   0x8040DDF8 Scale    r3 = r4 * f1            0x8040DFE0 Dot      f1 = r3 . r4 (y*y', then fused x, then fused z)
//   0x8040E008 Cross    r3 = r4 x r5            0x8040DFA0 Length   f1 = |r3|
//   0x8040E048 AddTo    r3 += r4                0x8040DE54 SubFrom  r3 -= r4
//
// Length ends in the game's sqrt (0x8000FEE4 -> 0x8000FF04): for a positive finite single the original's frsqrte seed,
// three Newton steps and frsp give exactly the host's correctly rounded single root (all 2,139,095,039 positive finite
// singles, docs: native_se_math.cpp); a negative or NaN argument gives MSL's __float_nan (0x7FFFFFFF), +-0
// return the argument; +infinity runs the Newton chain into inf * 0 and gives the default NaN (0x7FC00000).
//
// Not kept: the f0..f4 temporaries and the FPSCR status bits (no code reads them; docs/NATIVE_MSL_MATH.md census).

#include "gekko/types.h"

#include <cstddef>

namespace gekko {
class Cpu;
}

namespace gekko::vec_math {

constexpr u32 kSub = 0x8040DDC4u;
constexpr u32 kScale = 0x8040DDF8u;
constexpr u32 kAdd = 0x8040DE20u;
constexpr u32 kSubFrom = 0x8040DE54u;
constexpr u32 kLength = 0x8040DFA0u;
constexpr u32 kDot = 0x8040DFE0u;
constexpr u32 kCross = 0x8040E008u;
constexpr u32 kAddTo = 0x8040E048u;

// The guest entries served here: equal to this section of extract/native-owners.txt (native_file_api_tests).
struct Owners {
    struct Address {
        u32 address;
        const char* name;
    };
    static const Address* addresses(std::size_t& count) noexcept {
        static constexpr Address kAddresses[] = {{kSub, "Vec3 Sub"},       {kScale, "Vec3 Scale"},
                                                 {kAdd, "Vec3 Add"},       {kSubFrom, "Vec3 SubFrom"},
                                                 {kLength, "Vec3 Length"}, {kDot, "Vec3 Dot"},
                                                 {kCross, "Vec3 Cross"},   {kAddTo, "Vec3 AddTo"}};
        count = sizeof(kAddresses) / sizeof(kAddresses[0]);
        return kAddresses;
    }
};

void serve_sub(Cpu& cpu) noexcept;
void serve_scale(Cpu& cpu) noexcept;
void serve_add(Cpu& cpu) noexcept;
void serve_sub_from(Cpu& cpu) noexcept;
void serve_length(Cpu& cpu) noexcept;
void serve_dot(Cpu& cpu) noexcept;
void serve_cross(Cpu& cpu) noexcept;
void serve_add_to(Cpu& cpu) noexcept;

// Installs the owners on the CPU.
void install(Cpu& cpu);

}  // namespace gekko::vec_math
