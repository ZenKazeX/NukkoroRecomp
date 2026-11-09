"""Reconciled specification model, area "branch".

This is branch_A.py after the differential cross-check against the independently written
branch_B.py (crosscheck_branch.py, 2026-09-14).  Both models were corrected in place (every change
is marked "XCHK:" with its page citation) until they agreed on every vector; this module re-exports
the corrected A so that there is exactly one implementation of the semantics.

Sources: docs/reference/ppc/gekko_user_manual.txt ("PAGE N"), ppc_750cl.txt ("750CL PAGE N") as a
cross-check only.  No GekkoForge implementation was consulted.

Contract (same as branch_A)
    supports(word) -> bool          True for b, bc, bclr, bcctr, sc, rfi, tw, twi, mfmsr, mtmsr, and
                                    every mfspr / mftb (XO 371) / mtspr word.
    step(state, word)               executes at state.pc; never mutates state when it raises.
      UndefinedBehaviour            .partial is None  -> the whole outcome is undefined
                                    otherwise .apply_defined(state) installs every defined result
                                    and .undefined maps location -> bitmask or True:
                                    'msr' 'srr0' 'srr1' 'cr' 'xer' 'hid0' 'hid2' 'gpr[N]' 'gqr[N]'
                                    (bits zeroed in .partial) and annotations 'pending.decrementer',
                                    'le_effective_point', 'power_mode'.
      NotModelled                   (a NotImplementedError) a defined access to a register
                                    gekko_state.State does not carry (BATs, SDR1, PMCs, ...).
    enabled_pending(state)          asynchronous exceptions made takeable (not delivered by step).

Disagreement classes found and how they were resolved (details at the XCHK comments)
  1  trace (MSR[SE] / MSR[BE]):  A = defined effects + 'trace' marker, B = whole step undefined.
     BOTH changed: the trace exception is taken inside step - PAGE 176 "Each instruction considered
     during trace mode completes before a trace exception is taken", vector 0xD00, Table 4-6 PAGE 168,
     SRR0 = next instruction (PAGE 163 "the next one (as in the case of a system call, trace, or trap
     exception)"), SRR1[1-4,10-15] undefined (PAGE 166).
  2  illegal/privileged program exception SRR0: A undefined, B = CIA.  B right: PAGE 46 "(excluding
     the trap and system call exceptions) the address of the faulting instruction is provided".
  3  trap exception name: A 'program', B 'trap'.  B right: PAGE 162/163 "trap exception" (vector
     0x700 unchanged; SRR0 stays undefined in both - PAGE 161 vs PAGE 163, settings deferred PAGE 175).
  4  opcode-17 word with bit 30 = 0: A illegal program exception, B undefined.  B right: PAGE 88 says
     opcode 17 has unused extended opcodes but no page identifies its extended-opcode field, so it is
     illegal (PAGE 88) or boundedly undefined (PAGE 87) - not decidable.
  5  mtspr PVR (287) in supervisor mode: A illegal program exception, B no-op.  B right: PAGE 90 "An
     mtspr instruction executing in supervisor mode ... specifying HID1 or PVR ... executes as a no-op".
  6  mfspr/mtspr Rc=1 on the program-exception path: A CR0 undefined, B CR untouched.  B right:
     PAGE 161 program exceptions are "taken without completing the exception-causing instruction".
  7  mtspr DEC setting the MSB: A pending=True, B pending undefined.  B right: PAGE 159 "MSB ... changes
     from 0 to 1" vs PAGE 162 "passes through zero" / PAGE 176 "completed decrementing".
  8  read-back of reserved bits (mfmsr, mfspr XER/GQR/HID0/HID2): A defined, B undefined.  B right: the
     bits whose write both models call undefined cannot be defined when read (PAGE 57 "XER[16-23] is
     implemented so that they can be read", PAGE 164, PAGE 63-68, PAGE 78).
  9  MSR bit 28 on mtmsr/mfmsr: B undefined, A stored.  A right: Table 4-4 PAGE 165 "28 - Reserved.
     Full function", note "Full function reserved bits are saved in SRR1".
  10 XER bit 3 on mtspr/mfspr: B undefined, A defined.  A right: mcrxr PAGE 458 "XER[0-3] <- 0b0000".
  11 rfi and MSR bit 24: B unaltered/defined, A undefined when SRR1[24] differs.  A right: PAGE 166 step 3
     may copy bit 24 into SRR1, PAGE 532 note lets an implementation restore what it saves.
  12 mtmsr LE change / POW with DOZE|NAP|SLEEP: A annotations, B nothing.  A right: PAGE 475 "execution
     synchronizing except with respect to alterations to the POW and LE bits".
  13 mtmsr changing SE: A marker only when the new SE = 1, B nothing.  Both changed to whole-outcome
     undefined: PAGE 161 "while trace mode is enabled" vs PAGE 475 silent on SE.
  14 coverage: A did not support unmodelled SPR words (hiding their defined user-mode privileged
     program exception), SDA/USDA (PAGE 61), HID1 no-op (PAGE 90) or the UMMCR0/UPMCn/USIA mtspr table
     conflict (Table 12-13 PAGE 478 "User" vs Table 2-53 PAGE 118 "mfspr").  A extended to B's model.
  15 exception entry after an MSR bit made undefined in the same step (mtmsr bit 24, then trace):
     A under-reported SRR1[24].  A fixed.
  common-mode (both agreed, manual silent): a DEC write while a decrementer condition is already
     pending - pending flag now undefined in both (PAGE 164/176).

Remaining documented ambiguities (both models raise UndefinedBehaviour / mark undefined)
  trap SRR0 (PAGE 161 vs 163, PAGE 175); reserved-field bits in bclr/bcctr/rfi/tw/mfmsr/mtmsr and any
  opcode-17 word but 0x44000002 (PAGE 87/88); bcctr with BO[2]=0 (PAGE 362 invalid form); SRR1[1-4,
  10-15] on every exception (PAGE 166, PAGE 537 "set to undefined values"); reserved MSR bits across
  exception entry (Table 4-6 PAGE 168); rfi restore of MSR[24,28,29] (PAGE 532 note); mtmsr changing SE;
  mtspr DEC MSB 0->1 or while pending; HID0 ICFI/DCFI "usually the next cycle" (PAGE 65); HID2 sticky
  bits (PAGE 68); UMMCR0/UPMCn/USIA writes; SDA/USDA reads and user-mode USDA writes (PAGE 61);
  CR0 after mfspr/mtspr with Rc=1 (PAGE 175).
"""
from __future__ import annotations

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import branch_A as _impl  # noqa: E402
from branch_A import (  # noqa: E402,F401
    AREA, NotModelled, UndefinedBehaviour, enabled_pending, mnemonic, step, step_with_markers, supports,
)

__all__ = ["AREA", "NotModelled", "UndefinedBehaviour", "enabled_pending", "mnemonic", "step",
           "step_with_markers", "supports"]


def _self_test():
    ok = _impl._run_tests()
    # A short in-process differential run against branch_B (the full run is crosscheck_branch.py).
    import crosscheck_branch as xc
    import random
    import branch_B
    r = random.Random(1)
    bad = 0
    n = 3000
    this = sys.modules[__name__]
    for _ in range(n):
        name = r.choice(xc.NAMES)
        w = xc.gen_word(r, name) & xc.M
        res = xc.compare(this, branch_B, xc.gen_state(r, name, w), w)
        if res[2]:
            bad += 1
            if bad <= 3:
                print("DISAGREE", name, "%08X" % w, res[2])
    print("branch (reconciled) quick differential vs branch_B: %d vectors, %d disagreements" % (n, bad))
    return ok and bad == 0


if __name__ == "__main__":
    sys.exit(0 if _self_test() else 1)
