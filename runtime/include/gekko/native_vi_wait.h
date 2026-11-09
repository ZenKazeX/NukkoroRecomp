#pragma once

// VIWaitForRetrace (main.dol, PAL GUPP8P), owned natively in the single-guest-thread configuration (the lifted build;
// extract/native-owners-r2.txt). The original's 14 words (the retrace count is the SDA word r13 + 0x5BCC, the retrace queue
// r13 + 0x5BD4):
//
//   0x8037ECD4 mflr r0 ; stw r0,4(r1) ; stwu r1,-16(r1) ; stw r31,12(r1) ; stw r30,8(r1)
//   0x8037ECE8 bl OSDisableInterrupts                      ; r3 = the previous MSR[EE]
//   0x8037ECEC lwz r30,0x5BCC(r13) ; mr r31,r3
//   0x8037ECF4 addi r3,r13,0x5BD4 ; bl OSSleepThread       ; sleep until the retrace queue is woken
//   0x8037ECFC lwz r0,0x5BCC(r13) ; cmplw r30,r0 ; beq 0x8037ECF4
//   0x8037ED08 mr r3,r31 ; bl OSRestoreInterrupts
//   0x8037ED10 lwz r0,0x14(r1) ; lwz r31,12(r1) ; lwz r30,8(r1) ; addi r1,r1,16 ; mtlr r0 ; blr
//
// Native: the same frame, the same two interrupt gates (served through the gates' own owners, so r3 / r4 / r5 / CR0 and the
// deliverable-interrupt rule are theirs) and the native OSSleepThread. Two resume rows exist because the gates can hand back
// to the dispatcher: an interrupt that is deliverable at OSDisableInterrupts is taken at the gate's entry and the function
// continues at 0x8037ECEC when the gate returns; an interrupt that the restore makes deliverable is taken before 0x8037ED10.
// Registers on return: r0 = the saved LR, r1, LR, r30 and r31 as the caller had them, r3 / r4 / r5 / CR0 from the restore gate.

#include "gekko/types.h"

namespace gekko {
class Cpu;
}

namespace gekko::vi_wait {

constexpr u32 kWaitForRetrace = 0x8037ECD4u;
constexpr u32 kAfterDisable = 0x8037ECECu;   // the row after OSDisableInterrupts
constexpr u32 kAfterRestore = 0x8037ED10u;   // the row after OSRestoreInterrupts

// Installed by os_threads::install_waits (the single-thread configuration only: the reference build keeps the guest body).
void install(Cpu& cpu);

}  // namespace gekko::vi_wait
