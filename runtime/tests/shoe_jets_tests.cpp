// liveness-roots: reference-interpreter-only
// Shadow's fourth shoe jet (gekko/native_shoe_jets.h), in the reference interpreter. The Jet constructor's clone
// sequence (0x800A5C30 .. 0x800A5C58, the PAL main.dol words: the same in USA and Japan) is written at its own
// addresses and run; RpClumpClone 0x804580E0 is a stand-in that returns its argument + 1, so the stores show which
// slot got a clone.
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_guest_call.h"
#include "gekko/native_shoe_jets.h"

#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace {
using namespace gekko;
constexpr u32 kFragment = 0x800A5C30u, kFragmentEnd = 0x800A5C5Cu, kRpClumpClone = 0x804580E0u;
constexpr u32 kJet = 0x81700000u, kTemplate = 0x81710000u, kLink = 0x81700F00u, kStack = 0x817E0000u;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void put(GuestMemory& m, u32 address, std::initializer_list<u32> words) {
    for (u32 w : words) { m.write32(address, w); address += 4; }
}

struct Slots {
    u32 s5c, s60, s64, s68;
};

// One run of the constructor's clone sequence on a fresh Jet; `marker`: the vtable word the constructor wrote at +0x18.
Slots run(bool install, u32 marker = 0x80522ACCu) {
    GuestMemory memory;
    Cpu cpu{memory};
    put(memory, kFragment, {
        0x807E005Cu,  // lwz r3,0x5C(r30)
        0x28030000u,  // cmplwi r3,0
        0x41820024u,  // beq 0x800A5C5C
        0x483B24A5u,  // bl RpClumpClone
        0x907E0060u,  // stw r3,0x60(r30)
        0x807E005Cu,  // lwz r3,0x5C(r30)
        0x483B2499u,  // bl RpClumpClone
        0x907E0064u,  // stw r3,0x64(r30)
        0x807E005Cu,  // lwz r3,0x5C(r30)
        0x483B248Du,  // bl RpClumpClone
        0x907E005Cu,  // stw r3,0x5C(r30)   <- the original's slip: the third clone over the template
    });
    // The test ends the fragment here. Its `bl`s overwrote LR (the real constructor saved it in its prologue), so it
    // returns through r31, which holds the link: mtlr r31 ; blr.
    put(memory, kFragmentEnd, {0x7FE803A6u, 0x4E800020u});
    put(memory, kRpClumpClone, {0x38630001u, 0x4E800020u});    // stand-in: addi r3,r3,1 ; blr
    memory.write32(kJet + 0x18u, marker);
    memory.write32(kJet + 0x5Cu, kTemplate);
    memory.write32(kJet + 0x60u, 0u);
    memory.write32(kJet + 0x64u, 0u);
    memory.write32(kJet + 0x68u, 0u);
    cpu.reset();
    cpu.gpr[1] = kStack;
    cpu.gpr[30] = kJet;
    cpu.gpr[31] = kLink;
    if (install)
        shoe_jets::install(cpu);
    u32 r = 0;
    require(guest_call(cpu, kFragment, kLink, {}, r), "the constructor's clone sequence did not return");
    return {memory.read32(kJet + 0x5Cu), memory.read32(kJet + 0x60u), memory.read32(kJet + 0x64u),
            memory.read32(kJet + 0x68u)};
}
}  // namespace

int main() {
    try {
        // Retail: three clones, the third over the template; the fourth slot empty.
        const Slots retail = run(false);
        require(retail.s60 == kTemplate + 1u && retail.s64 == kTemplate + 1u && retail.s5c == kTemplate + 1u,
                "retail: the three clones are not where the original stores them");
        require(retail.s68 == 0u, "retail: the fourth slot is not empty (the control does not show the original's slip)");

        // The fix: the same three, and a fourth clone of the same template in +0x68.
        const shoe_jets::Stats before = shoe_jets::stats();
        const Slots fixed = run(true);
        require(fixed.s60 == kTemplate + 1u && fixed.s64 == kTemplate + 1u && fixed.s5c == kTemplate + 1u,
                "fixed: the original three clones changed");
        require(fixed.s68 == kTemplate + 1u, "fixed: the fourth slot did not get a clone of the template");
        require(shoe_jets::stats().fourth_jets == before.fourth_jets + 1u, "fixed: the fourth jet was not counted");
        // The first two clones and the hook's own two nested clones pass to the original.
        require(shoe_jets::stats().passed == before.passed + 4u, "fixed: other clones did not pass to the original");

        // Not a Jet under construction (the marker the constructor writes before cloning is absent): untouched.
        const Slots other = run(true, 0u);
        require(other.s68 == 0u, "a clone from the same site outside a Jet was changed");

        // SHADOW_RETAIL_SHOE_JETS=1: the hook is not installed, retail behaviour.
        _putenv_s("SHADOW_RETAIL_SHOE_JETS", "1");
        const Slots switched = run(true);
        _putenv_s("SHADOW_RETAIL_SHOE_JETS", "");
        require(switched.s68 == 0u, "SHADOW_RETAIL_SHOE_JETS=1 did not keep the retail three jets");

        std::printf("PASS shoe jets: retail leaves +0x68 empty; the fix clones the fourth jet into +0x68, leaves the other "
                    "slots and every other clone as the original, and SHADOW_RETAIL_SHOE_JETS=1 keeps retail\n");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL shoe_jets_tests: %s\n", e.what());
        return 1;
    }
}
