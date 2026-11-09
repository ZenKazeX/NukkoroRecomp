"""Reconciled Gekko specification model, area "int".

Sources: docs/reference/ppc/gekko_user_manual.txt ("PAGE N") and ppc_750cl.txt ("750CL PAGE N")
only.  No GekkoForge implementation or audit document was consulted.

PROVENANCE
  Two independently written models, int_A.py and int_B.py, were cross-checked by
  crosscheck_int.py (decode sweep over the whole primary / group-19 / group-31 opcode space,
  a deterministic boundary sweep, and a seeded random sweep with aliasing registers, dirty
  reserved bits, every SO/OV/CA combination, random CR, and random FPSCR/MSR/FPR/SPR state
  that integer instructions must not disturb).  The full arch_dict, mem.log, mem.bytes,
  pending, the outcome class and the set of undefined locations were compared.
  Result: 0 disagreements in either direction, so NEITHER model needed a correction; this
  module re-exports int_A (whose UB contract - raise before touching state, defined parts in
  exc.defined - is the more composable one) unchanged.  The cross-check's detection power is
  itself validated by 24 injected single-point mutants (crosscheck_int.py --mutants), each of
  which is caught independently by the boundary sweep and by the random sweep.

COVERED (62 mnemonics + o/. variants): mulli subfic cmpli cmpi addic addic. addi addis rlwimi
  rlwinm rlwnm ori oris xori xoris andi. andis.; mcrf crand crandc creqv crnand crnor cror crorc
  crxor; add addc adde addme addze subf subfc subfe subfme subfze neg mullw mulhw mulhwu divw
  divwu; cmp cmpl and andc or orc xor nand nor eqv extsb extsh cntlzw slw srw sraw srawi mcrxr
  mfcr mtcrf.  NOT covered: tw/twi (exception area).

UNDEFINED BEHAVIOUR (both models agreed, manual-sanctioned)
  * Nonzero reserved field -> whole instruction boundedly undefined, state untouched:
    "If instructions are encoded with incorrectly set bits in reserved fields, the results on
    execution can be said to be boundedly undefined." PAGE 87; "the instruction form is
    invalid" PAGE 337.  Reserved fields per the encoding diagrams: cmp/cmpl bits 9,31
    (PAGE 365/367); cmpi/cmpli bit 9 (PAGE 366/368); mcrf 9-10,14-20,31 (PAGE 456); CR logical
    bit 31 (PAGE 370-377); mcrxr 9-20,31 (PAGE 458); mfcr 11-20,31 (PAGE 459); mtcrf 11,20,31
    (PAGE 470); cntlzw/extsb/extsh 16-20 (PAGE 369/392/393); addme/addze/subfme/subfze/neg
    16-20 (PAGE 352/353/573/574/487); mulhw/mulhwu bit 21 (PAGE 482/483).
  * cmp/cmpl/cmpi/cmpli with L = 1: "If L = 1 the instruction form is invalid." PAGE 365-368;
    "Gekko provides limited support for instructions represented in an invalid form." PAGE 87.
  * divw 0x8000_0000 / -1 or <anything> / 0, divwu <anything> / 0: "the contents of rD are
    undefined, as are the contents of the LT, GT, and EQ bits of the CR0 field (if Rc = 1). In
    this case, if OE = 1 then OV is set." PAGE 385 / PAGE 386.  Defined parts (pc + 4, OV|SO
    when OE = 1, CR0[SO] = XER[SO] when Rc = 1, every other bit) are carried in exc.defined.

REMAINING AMBIGUITIES (shared interpretations; the corpus is silent or garbled, both authors
independently chose the same reading; none is a disagreement)
  1. XER bit positions SO = XER[0], OV = XER[1], CA = XER[2]: the corpus names the bits
     (PAGE 41) and places them in XER[0-3] (mcrxr PAGE 458) and the byte count in XER[25-31]
     (lswx), but defers the layout to "Section 2.1.5, XER Register" of the Programming
     Environments manual (PAGE 345 NOTE), which is not in the corpus.
  2. OE = 1 with no overflow clears OV but leaves SO set.  PAGE 92 "The summary overflow bit
     (SO) and overflow bit (OV) ... are set to reflect an overflow condition of a 32-bit
     result" read literally could also clear SO; the "summary" name (PAGE 41, PAGE 339) and
     mcrxr being the explicit clear (PAGE 458) support stickiness, and the manual delegates the
     definition to the PEM XER section.  Not modelled as UB because the manual delegates rather
     than leaves it undefined.
  3. CR0[SO] for an OE = 1 instruction copies XER[SO] AFTER that instruction's own update
     ('o' records the overflow "in XER[OV] and CR0[SO]" PAGE 340).
  4. Garbled pseudocode resolved by the prose: mulli "rD <- prod[16-48]" (33 bits) vs "low-order
     32-bits of the 48-bit product" PAGE 484; mullw "prod[16-48]" PAGE 485; mulhw
     "prod[0-63] <- (rA * (rB; rD <- prod" PAGE 482; extsh prose "Bit 48 of rS" vs pseudocode
     rS[16] PAGE 393; sraw prose "(shift amounts between 0-31)" vs "shift amounts from 32 to 63
     give a result of 32 sign bits" PAGE 539 (the rB[26] pseudocode reconciles both).
  5. nand PAGE 486 has an empty "Affected:" line; the Rc definition PAGE 339 is applied.
  6. CA / OV of the subf family and addme/subfme: carry out of bit 0 and signed
     representability of the pseudocode sum "not(rA) + (rB) + 1", "(rA) + XER[CA] + 0xFFFF_FFFF"
     (PAGE 352, 569-574); the corpus defers the exact rule to PEM Chapter 3 (PAGE 570 NOTE).
"""
from __future__ import annotations

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import int_A as _impl  # noqa: E402
from int_A import (  # noqa: E402,F401
    AREA, UNDEFINED, UndefinedBehaviour, XER_CA, XER_OV, XER_SO, mnemonic, step, step_with_markers, supports,
)

__all__ = ["AREA", "UNDEFINED", "UndefinedBehaviour", "XER_SO", "XER_OV", "XER_CA",
           "supports", "mnemonic", "step", "step_with_markers"]


if __name__ == "__main__":
    import runpy
    import subprocess

    here = os.path.dirname(os.path.abspath(__file__))
    rc = 0
    for script in ("int_A.py", "int_B.py"):
        try:
            runpy.run_path(os.path.join(here, script), run_name="__main__")
        except SystemExit as e:
            if e.code:
                print("self-test %s FAILED (exit %r)" % (script, e.code))
                rc = 1
    # differential re-check of this reconciled module against the independent int_B
    r = subprocess.run([sys.executable, os.path.join(here, "crosscheck_int.py"), "--models", "int,int_B",
                        "--n", "60000"], cwd=here)
    rc |= r.returncode
    print("int.py reconciled self-check:", "PASS" if rc == 0 else "FAIL")
    sys.exit(rc)
