#pragma once

// The Sonicteam effect system's per-frame particle update (main.dol, PAL GUPP8P), owned natively
// (docs/agents/opt-effects.md). Names come from this repository's own evidence (the vtable layout, the call graph and what each
// function does to memory); where the external reference agrees it is noted in that document.
//
// Objects (guest memory, big-endian words):
//   particle system S (an EffectParticle-like object, 0x13C bytes)
//     +0x018 flags  +0x0FC unit list head  +0x100 unit list tail  +0x108 definition flags (0x100: no age limit)
//     +0x110 unit count  +0x114 gravity  +0x118 drag  +0x124 -> {+0x0C float: age limit}  +0x134 / +0x138 pool words
//   unit (0x7C bytes; a vtable object: slot +0x08 is its deleting destructor)
//     +0x04 position  +0x1C velocity (3 floats each)  +0x4C flags (bit 0: finished)  +0x50 age  +0x64 prev  +0x68 next
//     +0x6C script cursor (0: no script)  +0x70 command list head  +0x74 command list tail
//   command (a vtable object; slot +0x08 deleting destructor(this, 1), slot +0x0C update(this, dt) -> 1 alive / 0 finished)
//     +0x04 data record (byte +2: end the unit when the command finishes)  +0x08 prev  +0x0C next  +0x10 started
//     +0x14 -> the unit's flags word  +0x18 -> the value the command moves (0: the command is spent)
//     +0x1C elapsed  +0x20 duration  +0x24 rate: an int (alpha ramp), a float (scalar) or three floats (vector)
//
//   0x80423F20 alpha ramp update      *target (a 10.22 fixed-point alpha, clamped to [0x3FFFFF, 0x3FC00000]) += int(rate * dt)
//   0x80425B20 scalar update          *target += rate * dt (one fused multiply-add)
//   0x80425544 vector update          target[0..2] += rate[0..2] * dt (also the 2D scale command at 0x80424958: same code)
//   0x80428044 unit step(dt)          ages units, runs each live unit's script and command list, deletes the finished ones,
//                                     then moves them: position += velocity * dt, the drag blend and the gravity pull
//
// Every floating-point operation is the original's, in its operand roles and order, on the guest's operands (ppc::fps_*: the
// FPSCR[NI] policy, one rounding for the fused forms, the NaN rules), every stored float the original's stfs word. Loads and stores
// keep the original's order, so aliased operands see what the original sees. The unit step calls the game only where the game's
// own code is the contract: the script step 0x80429224 (only when a unit has a script cursor), the update of a command class other
// than the three above, and the deleting destructors.
//
// NOT kept (dead across the call by the ABI, or console scratch): f0..f13, r0, r3..r12 other than a command update's r3 result,
// ctr, cr, the stack words the original's frame holds, and FPSCR's FPCC bits (every compare sets them; no code reads them). The
// FPSCR sticky bits the original's arithmetic and unordered compares set are kept. A cleared MSR[FP] is not checked (the game
// never clears it).

#include "gekko/types.h"

#include <cstddef>

namespace gekko {
class Cpu;
}

namespace gekko::effect_update {

constexpr u32 kAlphaRamp = 0x80423F20u;
constexpr u32 kScalar = 0x80425B20u;
constexpr u32 kVector = 0x80425544u;
constexpr u32 kVector2D = 0x80424958u;
constexpr u32 kUnitStep = 0x80428044u;
// The particle renderer's vertex fill (installed as a hook over its translated body, NOT a native-owner row: the body's tail is
// kept as guest code and entered at kPTankTail, the return address after the unlock call).
constexpr u32 kPTankFill = 0x80434824u;
constexpr u32 kPTankTail = 0x80434A78u;  // liveness-roots: value-only

// The guest entries served here: equal to this section of extract/native-owners.txt (native_file_api_tests).
struct Owners {
    struct Address {
        u32 address;
        const char* name;
    };
    static const Address* addresses(std::size_t& count) noexcept {
        static constexpr Address kAddresses[] = {{kAlphaRamp, "effect command: alpha ramp update"},
                                                 {kScalar, "effect command: scalar update"},
                                                 {kVector, "effect command: vector update"},
                                                 {kVector2D, "effect command: 2D scale update"},
                                                 {kUnitStep, "effect particle system: unit step"}};
        count = sizeof(kAddresses) / sizeof(kAddresses[0]);
        return kAddresses;
    }
};

void serve_alpha_ramp(Cpu& cpu);
void serve_scalar(Cpu& cpu);
void serve_vector(Cpu& cpu);
void serve_unit_step(Cpu& cpu);
// The particle renderer vertex fill 0x80434824: everything before the draw (see native_effect_update.cpp). False = declined (a
// rotation that would wrap for more than 65536 turns): nothing but idempotent memory was touched and the original body runs.
bool serve_ptank_fill(Cpu& cpu);

// Installs the owners on the CPU.
void install(Cpu& cpu);

}  // namespace gekko::effect_update
