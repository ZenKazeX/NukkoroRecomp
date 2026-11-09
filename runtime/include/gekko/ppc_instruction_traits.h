#pragma once

#include "gekko/types.h"

namespace gekko {

// One canonical description of the instruction classes that require MSR[FP].
// The interpreter and the static translator must consult the same predicate:
// otherwise the differential verifier disagrees at the exception boundary and,
// more seriously, one execution engine can use the guest FPR file while Gekko
// hardware would raise the 0x800 floating-point-unavailable exception.
constexpr bool instruction_requires_fp(u32 instruction) noexcept {
    const u32 primary = instruction >> 26;
    switch (primary) {
        case 4:
            // dcbz_l shares primary 4 with the paired-single group but is a
            // cache instruction, not a floating-point instruction.
            return ((instruction >> 1) & 0x3ffu) != 1014u;
        case 31:
            // Indexed scalar floating-point loads/stores.
            switch ((instruction >> 1) & 0x3ffu) {
                case 535:  // lfsx
                case 567:  // lfsux
                case 599:  // lfdx
                case 631:  // lfdux
                case 663:  // stfsx
                case 695:  // stfsux
                case 727:  // stfdx
                case 759:  // stfdux
                case 983:  // stfiwx
                    return true;
                default:
                    return false;
            }
        case 48: case 49: case 50: case 51:
        case 52: case 53: case 54: case 55:
        case 56: case 57: case 59: case 60: case 61: case 63:
            return true;
        default:
            return false;
    }
}

// The HID2 enable bits an instruction needs, 0 for none (audit
// paired-single-10). gekko_user_manual.txt PAGE 90: "the dcbz_l instruction is
// illegal when HID2[LCE] = 0, the psq_l, psq_lu, psq_st and psq_stu
// instructions are illegal when HID2[PSQE] = 0 or HID2[PSE] = 0, and all other
// paired single instructions are illegal when HID2[PSE] = 0"; each pseudocode
// (PAGE 493-532) tests this first. Primary 4 holds only the Gekko extensions:
// dcbz_l and the paired-single group, indexed psq_lx/psq_stx included.
// The masks are HID2 bits: LSQE 0x80000000, PSE 0x20000000, LCE 0x10000000.
constexpr u32 instruction_required_hid2(u32 instruction) noexcept {
    switch (instruction >> 26) {
        case 4:
            return ((instruction >> 1) & 0x3ffu) == 1014u ? 0x10000000u : 0x20000000u;
        case 56: case 57: case 60: case 61:
            return 0xa0000000u;
        default:
            return 0u;
    }
}

// HID2 LSQE | PSE. Set at reset and kept by every HID2 write the machine
// accepts: Cpu::write_hid2 refuses a value that would clear either (change C,
// 2026-10-05; every guest HID2 writer keeps both, tools/platform_hid2_writers.py).
// Paired-single instructions are therefore always enabled and neither engine
// tests these two bits.
constexpr u32 kHid2PairedSingleEnables = 0xa0000000u;

// The HID2 enable an instruction still tests at run time - the predicate the
// interpreter and the static translator share: LCE for dcbz_l, which the guest
// sets and clears, and nothing for any other instruction.
constexpr u32 instruction_checked_hid2(u32 instruction) noexcept {
    return instruction_required_hid2(instruction) & ~kHid2PairedSingleEnables;
}

}  // namespace gekko
