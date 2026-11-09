"""Reconciled specification model, area "exc" (Gekko exception / interrupt architecture).

This is the corrected author-A model (exc_A.py) after a differential cross-check against the
independently written author-B model (exc_B.py) with crosscheck_exc.py: 0 disagreements over
220,000 vectors per model pair (exc_A vs exc_B seed 1, exc vs exc_B seed 7) plus a 12,293-word
supports() sweep, with 12/12 seeded single-point mutants of exc_B detected (logs under
scratch/core-audit/audio-r2/exc-crosscheck/).  Sources: ONLY
docs/reference/ppc/gekko_user_manual.txt ("PAGE N") and ppc_750cl.txt ("750CL PAGE N").

Contract (unchanged from exc_A): every operation is atomic - it commits, or raises
UndefinedBehaviour without touching the state; u.defined carries every defined part and
u.undefined = {location: mask} ('all' = boundedly undefined / invalid form; a mask on 'pc' or
'exception' = control flow undefined).  STRICT is forced True here: SRR1[1-4,10-15] for
exceptions whose register settings the Gekko manual defers to the PEM are reported undefined.
Toggle exc_A.STRICT (not exc.STRICT) if the non-strict placeholders are ever wanted.

Disagreement classes found and how they were resolved (details + quotes at each
"CROSSCHECK FIX" comment in exc_A.py / exc_B.py):
  A wrong  trap SRR0 defined CIA+4        -> ambiguous: PAGE 163/46 overview vs PAGE 175 deferral
                                              -> SRR0 undefined (placeholder CIA+4)
  A wrong  SRR1 copy of reserved bits      -> PAGE 165 note: full-function bits (0,5-9,28) are
           all undefined                      saved; only bit 24 (unclassified, PAGE 164) and, for
                                              sc, bit 28 (PAGE 537 RTL) undefined
  A wrong  sc SRR1[PM] undefined           -> copied: PAGE 166 step 3 decides PAGE 537's "may"
  A wrong  step() ignores IABR             -> IABR is dispatch priority 0, instruction not
                                              executed (Table 4-3 PAGE 162, PAGE 177)
  A wrong  trace taken although IABR       -> undefined: 750CL PAGE 180 "instruction breakpoint
           matches the next instruction          takes priority over trace", trace fate unstated
  A wrong  mtmsr altering POW/LE defined   -> undefined: "execution synchronizing ... except with
                                              respect to alterations to the POW and LE bits" PAGE 475
  A wrong  hard reset leaves IABR          -> "IABR All 0s" Table 4-8 PAGE 170
  A wrong  bus clocks floor (7 -> 1 tick)  -> prescaler phase not given (PAGE 60/176) -> undefined
                                              unless a multiple of 4
  A wrong  pending smi ignored             -> undefined once recognisable (no vector PAGE 159, no
                                              priority PAGE 162; EE-masked PAGE 58)
  A gap    opcode 17 with bit 30 = 0 unsupported, mfmsr absent -> supported (PAGE 87, PAGE 461)
  B wrong  step() not atomic on raise      -> module contract; transactional step
  B wrong  rfi PM mismatch / reserved bits, mtmsr reserved bits raised as control flow
                                           -> contents-only: MSR bits marked (PAGE 532, 164-168)
  B wrong  mtmsr BE-only change undefined  -> defined: BE traces branches only (PAGE 164, 113)
  B wrong  checkstop tested before HRESET  -> "only a hard reset can clear" PAGE 302
  B wrong  ISI/DSI non-occurring cause bits defined 0, DSI SRR1 cause bits defined 0
                                           -> PAGE 193 defers "a complete description of the
                                              SRR1 and DSISR bit settings" -> undefined
  B wrong  smi raised above resets / with EE = 0 -> PAGE 160 item 4, PAGE 58
  both     TB/DEC advanced in checkstop (common-mode, manual review) -> frozen, PAGE 173
Harness-only normalisations (not model changes): B's helper entry functions do not consume
pending conditions (A's take_* do) - pending is not compared for helpers; exception names and
machine-check cause spellings are mapped.

Remaining documented ambiguities (reported as UndefinedBehaviour by both models):
  - SRR1[1-4,10-15] for external, decrementer, DSI, alignment, program, FP unavailable, sc,
    trace (register settings deferred to the PEM, PAGE 174-176; sc prose vs RTL PAGE 537);
    ISI/DSI bits other than the occurring cause (PAGE 193); all of DSISR for alignment
  - trap SRR0 (above); machine-check SRR0 "best-effort" (Table 4-10 PAGE 173)
  - new-MSR value of any set reserved MSR bit at exception entry / rfi / mtmsr (PAGE 165 vs
    PAGE 168); SRR1 copy of MSR bit 24 (and bit 28 for sc)
  - rfi restoring MSR[PM] when SRR1[PM] != MSR[PM] (PAGE 532 "may")
  - mtmsr altering SE (own trace), POW or LE (PAGE 475, PAGE 90)
  - trace coinciding with an IABR match on the next instruction (750CL PAGE 180)
  - DEC write that changes the MSB 0->1, or while a decrementer condition is pending
    (PAGE 159 vs PAGE 162/176); bus-clock counts not a multiple of 4 (PAGE 60)
  - a recognisable system management interrupt (PAGE 58 vs Table 4-2/4-3)
  - hard reset: GPRs, FPRs, reservation address "Unknown" (Table 4-8 PAGE 170-171)
  - invalid forms (reserved bits set; sc with bit 30 clear): boundedly undefined, PAGE 87
Residual common-mode interpretations both models share WITHOUT raising (flag, do not treat as
settled if an implementation differs): branch trace (MSR[BE]) fires for not-taken branches too
("a branch instruction executes successfully" PAGE 164 does not distinguish); machine-check
SRR1[10] is 0 when no DMA/locked-cache error although only bits 11-15 say "otherwise zero"
(Table 4-10 PAGE 173); SRESET/external/PM/thermal are levels or edges as annotated in exc_A;
the IABR "mtspr immediately preceding a context-synchronizing instruction" caveat (PAGE 178) is
not representable in the state container and is not modelled.
"""
from __future__ import annotations

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import exc_A as _A  # noqa: E402

_A.STRICT = True
STRICT = True

AREA = _A.AREA
UndefinedBehaviour = _A.UndefinedBehaviour

_PUBLIC = (
    # instructions
    "supports", "mnemonic", "step", "step_with_markers",
    # asynchronous recognition
    "take_pending", "take_pending_with_markers",
    # exception entry helpers
    "take_hard_reset", "take_system_reset", "take_machine_check", "take_dsi", "take_isi",
    "take_external", "take_alignment", "take_program", "take_fp_unavailable",
    "take_decrementer", "take_syscall", "take_trace", "take_performance_monitor", "take_iabr",
    "take_thermal",
    # condition helpers
    "vector_address", "trace_condition", "iabr_match", "fp_unavailable_condition",
    "fp_enabled_exceptions_enabled", "select_instruction_exception", "select_async_exception",
    # timers
    "advance_timebase", "write_dec", "bus_clocks_to_ticks",
    # constants
    "VECTORS", "PROGRAM_KINDS", "MC_CAUSE_BITS", "DSI_CAUSE_DSISR", "ISI_CAUSE_SRR1",
    "INSTRUCTION_PRIORITY", "ASYNC_PRIORITY", "SRR1_SPECIFIC", "SRR1_COPY", "MSR_RESERVED",
    "RFI_COPY", "POW", "ILE", "EE", "PR", "FP", "ME", "FE0", "SE", "BE", "FE1", "IP", "IR", "DR",
    "PM", "RI", "LE", "HID0_EMCP", "HID0_EBA", "HID0_EBD", "B", "R",
)
for _name in _PUBLIC:
    globals()[_name] = getattr(_A, _name)
del _name

__all__ = ("AREA", "STRICT", "UndefinedBehaviour") + _PUBLIC


if __name__ == "__main__":
    import runpy

    # The reconciled model's hand-derived vectors are exc_A's (corrected) self-test.
    runpy.run_path(os.path.join(os.path.dirname(os.path.abspath(__file__)), "exc_A.py"),
                   run_name="__main__")
