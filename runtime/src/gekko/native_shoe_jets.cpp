// Shadow's fourth shoe jet. Contract and evidence: include/gekko/native_shoe_jets.h.

#include "gekko/native_shoe_jets.h"

#include <cstdlib>

#include "gekko/cpu.h"
#include "gekko/native_guest_call.h"

namespace gekko::shoe_jets {

namespace {

constexpr u32 kRpClumpClone = 0x804580E0u;
constexpr u32 kThirdCloneReturn = 0x800A5C58u;  // the Jet constructor's `stw r3,0x5C(r30)` after its third clone
// The link of the two clones made here: an address inside the constructor that is never fetched (production returns
// into this C++), and not the third clone's site, so the nested calls pass through the hook to the original.
constexpr u32 kNestedLink = 0x800A5C4Cu;
constexpr u32 kJetMarker = 0x18u;               // the constructor writes 0x80522ACC there before it clones
constexpr u32 kJetMarkerValue = 0x80522ACCu;
constexpr u32 kFourthSlot = 0x68u;

Stats g_stats;

bool retail_requested() {
    const char* v = std::getenv("SHADOW_RETAIL_SHOE_JETS");
    return v != nullptr && v[0] == '1';
}

}  // namespace

const Stats& stats() noexcept { return g_stats; }

void install(Cpu& cpu) {
    if (retail_requested())
        return;
    cpu.install_hle(kRpClumpClone, [](Cpu& c) {
        // r30 is the Jet the constructor builds (it keeps `this` there from its entry); r3 the template clump.
        const u32 jet = c.gpr[30];
        const u32 clump_template = c.gpr[3];
        if (c.lr != kThirdCloneReturn || clump_template == 0u || c.mem_read32(jet + kJetMarker) != kJetMarkerValue ||
            c.mem_read32(jet + kFourthSlot) != 0u) {
            ++g_stats.passed;
            return false;  // any other clone: the original
        }
        u32 third = 0u, fourth = 0u;
        if (!guest_call(c, kRpClumpClone, kNestedLink, {clump_template}, third))
            return true;  // the run is stopped (guest_call requested it)
        if (!guest_call(c, kRpClumpClone, kNestedLink, {clump_template}, fourth))
            return true;
        c.mem_write32(jet + kFourthSlot, fourth);  // 0 if the clone failed: the slot stays empty, as in retail
        c.gpr[3] = third;                          // the original stores it to +0x5C when this returns
        if (fourth != 0u)
            ++g_stats.fourth_jets;
        return true;
    }, "shoe jets: the Jet constructor's fourth flame (RpClumpClone 0x804580E0 from 0x800A5C58)");
}

}  // namespace gekko::shoe_jets
