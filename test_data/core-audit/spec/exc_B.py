"""Independent semantic model (author B) of the Gekko exception / interrupt architecture.

Sources: ONLY docs/reference/ppc/gekko_user_manual.txt ("PAGE N" = the "===== PAGE N =====" marker)
and docs/reference/ppc/ppc_750cl.txt (cited as "750CL PAGE N"). No GekkoForge implementation was read.

Bit numbering follows the manual: bit 0 is the MSB of a 32-bit register, mask(n) = 1 << (31 - n).

Public surface
--------------
AREA, supports(word), step(state, word, partial=None)
    Instructions: sc, rfi, tw, twi, mtmsr, mfmsr (plus the pre-instruction IABR check and the
    post-instruction trace check applied to them).
take_pending(state, partial=None) -> name | None
    Applied at an instruction boundary (state.pc = next instruction to execute). Takes at most
    ONE asynchronous exception, highest priority first.
Entry helpers for use by other areas: enter_system_reset, enter_machine_check, enter_dsi,
    enter_isi, enter_external, enter_alignment, enter_program, enter_fp_unavailable,
    enter_decrementer, enter_syscall, enter_trace, enter_performance_monitor, enter_iabr,
    enter_thermal, hard_reset, check_iabr, trace_after.
Timer helpers: timer_advance(state, ticks), advance_bus_clocks(state, n), write_dec(state, value).

Undefined behaviour policy
--------------------------
* Anything that decides CONTROL FLOW (whether/which exception is taken, where execution goes)
  and is not pinned down by the manuals always raises UndefinedBehaviour.
* Register CONTENTS the manuals do not specify (e.g. SRR1[1-4,10-15] for arch-defined exceptions,
  whose "register settings" the Gekko manual defers to the PowerPC PEM, which is not a source here)
  raise UndefinedBehaviour in strict mode (the default).  With partial=True (or module-level
  PARTIAL_DEFAULT = True) the model completes the operation, writes 0 as an arbitrary
  placeholder into the unspecified bits, and records them in
      state.spec_undefined : dict  register-name -> mask of bits a comparator must ignore
  ('srr0', 'srr1', 'dsisr', 'msr', 'gpr', 'fpr', 'reserve_addr' ...).
  step() resets state.spec_undefined; an exception entry overwrites the srr0/srr1 marks it owns.
* In strict mode the raise happens before any state is modified.
"""
from __future__ import annotations

import copy as _copy

from gekko_state import MASK32, MASK64, State  # noqa: F401  (State used by the self-test)

# Cross-check history: every "CROSSCHECK FIX" comment marks a change made while reconciling this
# model with the independent model exc_A (crosscheck_exc.py, 2026-09-14); each cites the page
# that decided it.

AREA = "exc"
PARTIAL_DEFAULT = False


class UndefinedBehaviour(Exception):
    """Behaviour the manuals leave undefined, conflicting, or implementation-specific."""


def _m(bit: int) -> int:
    return 1 << (31 - bit)


# ---------------------------------------------------------------------------------------------
# MSR bits - Table 4-4 "MSR Bit Settings", PAGE 164-165; Table 2-1 "Additional MSR Bits" PAGE 58
# ---------------------------------------------------------------------------------------------
MSR_POW = _m(13)   # PAGE 164
MSR_ILE = _m(15)   # PAGE 164
MSR_EE = _m(16)    # PAGE 164
MSR_PR = _m(17)    # PAGE 164
MSR_FP = _m(18)    # PAGE 164
MSR_ME = _m(19)    # PAGE 164
MSR_FE0 = _m(20)   # PAGE 164
MSR_SE = _m(21)    # PAGE 164
MSR_BE = _m(22)    # PAGE 164
MSR_FE1 = _m(23)   # PAGE 164
MSR_B24 = _m(24)   # PAGE 164 "Reserved. This bit corresponds to the AL bit of the POWER architecture."
MSR_IP = _m(25)    # PAGE 165
MSR_IR = _m(26)    # PAGE 165
MSR_DR = _m(27)    # PAGE 165
MSR_B28 = _m(28)   # PAGE 165 "Reserved. Full function"
MSR_PM = _m(29)    # PAGE 165
MSR_RI = _m(30)    # PAGE 165
MSR_LE = _m(31)    # PAGE 165

# Reserved MSR bits: 0 (full), 1-4 (partial), 5-9 (full), 10-12 (partial), 14 (impl-specific),
# 24 (reserved), 28 (full) - Table 4-4 PAGE 164-165.
MSR_RESERVED = (0xFFF80000 | _m(14) | MSR_B24 | MSR_B28)
assert MSR_RESERVED == 0xFFFA0088
MSR_FULL_FUNCTION_RESERVED = _m(0) | 0x07C00000 | MSR_B28   # PAGE 164/165 "Full function"

# SRR1 layout - PAGE 166 steps 2/3: "SRR1[1-4, 10-15] are loaded with information specific to the
# exception type." / "SRR1[5-9, 16-31] are loaded with a copy of the corresponding MSR bits."
# Tables 4-7/4-11/4-12/4-13 (PAGE 169, 177, 178) add bit 0 "Loaded with equivalent MSR bits".
SRR1_COPY = _m(0) | 0x07C00000 | 0x0000FFFF
SRR1_CAUSE = 0x78000000 | 0x003F0000
assert SRR1_COPY | SRR1_CAUSE == MASK32 and SRR1_COPY & SRR1_CAUSE == 0
SRR1_COPY_MC = 0x0000FFFF   # Table 4-10 PAGE 173 "0-9 Cleared", "16-31 MSR[16-31]"

# rfi - PAGE 532 "MSR[0,5-9,16-23, 25-27, 30-31] <- SRR1[0,5-9,16-23, 25-27, 30-31]", "MSR[13] <- b'0'"
RFI_RESTORE = _m(0) | 0x07C00000 | 0x0000FF00 | 0x00000070 | 0x00000003
assert RFI_RESTORE == 0x87C0FF73

# Exception vector offsets - Table 4-2 PAGE 159-160
VEC = {
    "system_reset": 0x00100, "machine_check": 0x00200, "dsi": 0x00300, "isi": 0x00400,
    "external": 0x00500, "alignment": 0x00600, "program": 0x00700, "trap": 0x00700,
    "fp_unavailable": 0x00800, "decrementer": 0x00900, "syscall": 0x00C00, "trace": 0x00D00,
    "performance_monitor": 0x00F00, "iabr": 0x01300, "thermal": 0x01700,
}

# HID0 machine-check enables - Table 4-9 PAGE 172
HID0_EMCP = _m(0)
HID0_EBA = _m(2)
HID0_EBD = _m(3)

# Machine-check SRR1 cause bits - Table 4-10 PAGE 173
MC_CAUSE_BITS = {
    "dma": _m(10),             # "Set when a DMA or locked cache error happens."
    "l2": _m(11),              # "Set when an L2 data cache double bit error is detected"
    "mcp": _m(12),             # "Set when MCP signal is asserted"
    "tea": _m(13),             # "Set when TEA signal is asserted"
    "data_parity": _m(14),     # "Set when a data bus parity error is detected"
    "address_parity": _m(15),  # "Set when an address bus parity error is detected"
}

# IABR - Table 2-3 PAGE 62: 0-29 address, 30 BE, 31 TE
IABR_BE = _m(30)
IABR_TE = _m(31)


# ---------------------------------------------------------------------------------------------
# infrastructure
# ---------------------------------------------------------------------------------------------
def _partial(partial):
    return PARTIAL_DEFAULT if partial is None else bool(partial)


def _undef_dict(state):
    d = getattr(state, "spec_undefined", None)
    if d is None:
        d = {}
        state.spec_undefined = d
    return d


def _mark(state, reg, mask):
    d = _undef_dict(state)
    if mask:
        d[reg] = mask
    else:
        d.pop(reg, None)


def vector_address(msr: int, name: str) -> int:
    # PAGE 166 step 5 / PAGE 168: "If IP is cleared, exceptions are vectored to the physical address
    # 0x000n_nnnn. If IP is set, exceptions are vectored to the physical address 0xFFFn_nnnn."
    base = 0xFFF00000 if msr & MSR_IP else 0x00000000
    return (base | VEC[name]) & MASK32


def _enter(state, name, srr0, *, partial, srr0_undef=None, cause=0, cause_defined=SRR1_CAUSE,
           cause_reason=None, copy_mask=SRR1_COPY, clear_me=False, extra=(), extra_undef=(),
           sc_rtl=False):
    """Generic exception entry (PAGE 166 "Steps for Exception Processing").

    extra: iterable of (attr, value) committed after the SRR updates.
    extra_undef: iterable of (register-name, mask, reason) for unspecified contents.
    """
    partial = _partial(partial)
    msr = state.msr & MASK32
    undef = []  # (reg, mask, reason)
    if srr0_undef:
        undef.append(("srr0", MASK32, srr0_undef))
    missing = SRR1_CAUSE & ~cause_defined & MASK32
    if copy_mask == SRR1_COPY_MC:
        missing = 0  # Table 4-10 PAGE 173 fully specifies bits 0-15 for machine check
    if missing:
        undef.append(("srr1", missing, cause_reason or
                      f"{name}: SRR1[1-4,10-15] contents not given (deferred to PEM ch.6) PAGE 166"))
    srr1_undef = 0
    msr_undef = 0
    if msr & MSR_RESERVED:
        # PAGE 166 step 3 "Depending on the implementation, reserved bits may not be copied." ;
        # Table 4-4 note PAGE 165 "Full function reserved bits are saved in SRR1 ... partial
        # function reserved bits are not saved." ; Table 4-6 note PAGE 168 "Reserved bits are read as
        # if written as 0." - the new MSR image of a set reserved bit is not specified.
        msr_undef = msr & MSR_RESERVED
        srr1_undef |= msr & MSR_B24 & copy_mask      # bit 24 is not labelled full-function
        if sc_rtl:
            srr1_undef |= msr & MSR_B28 & copy_mask  # PAGE 537 RTL omits bit 28 from the copy
        undef.append(("msr", msr_undef,
                      f"{name}: reserved MSR bits 0x{msr & MSR_RESERVED:08X} set at entry PAGE 165/168"))
        if srr1_undef:
            undef.append(("srr1", srr1_undef, f"{name}: SRR1 copy of reserved MSR bits PAGE 166/537"))
    for item in extra_undef:
        undef.append(item)
    if undef and not partial:
        raise UndefinedBehaviour("; ".join(r for _, _, r in undef))

    # ---- commit ---------------------------------------------------------------------------
    # PAGE 166 step 1: SRR0 loaded with an exception-dependent instruction address.
    state.srr0 = srr0 & MASK32
    # PAGE 166 steps 2-3.  Unspecified cause bits get 0 as placeholder (masked).
    state.srr1 = ((msr & copy_mask) | (cause & cause_defined & SRR1_CAUSE)) & ~srr1_undef & MASK32
    for attr, value in extra:
        setattr(state, attr, value)
    # Table 4-6 PAGE 168 (and Tables 4-7, 4-10..4-13): POW,EE,PR,FP,FE0,SE,BE,FE1,IR,DR,PM,RI <- 0;
    # ILE and IP unaltered; ME unaltered except machine check (ME <- 0); LE <- ILE.
    keep = MSR_ILE | MSR_IP | (0 if clear_me else MSR_ME)
    new = msr & keep
    if msr & MSR_ILE:
        new |= MSR_LE
    state.msr = new & MASK32
    # PAGE 166 step 5: fetch resumes at vector computed with the NEW MSR[IP] (IP is unaltered).
    state.pc = vector_address(state.msr, name)
    state.exception = name
    # marks: an entry owns srr0/srr1/msr marks
    reg_masks = {"srr0": 0, "srr1": 0, "msr": 0}
    for reg, mask, _ in undef:
        reg_masks[reg] = reg_masks.get(reg, 0) | mask
    for reg, mask in reg_masks.items():
        _mark(state, reg, mask)
    return name


# ---------------------------------------------------------------------------------------------
# entry helpers
# ---------------------------------------------------------------------------------------------
def enter_system_reset(state, partial=None):
    """Soft reset (SRESET). Table 4-7 PAGE 169."""
    # SRR0: "Set to the effective address of the instruction that the processor would have attempted
    # to execute next if no exception conditions were present." (PAGE 169) -> state.pc at boundary.
    # SRR1: 0 MSR, 1-4 cleared, 5-9 MSR, 10-15 cleared, 16-31 MSR (PAGE 169) -> fully defined.
    return _enter(state, "system_reset", state.pc, partial=partial, cause=0, cause_defined=SRR1_CAUSE)


def enter_machine_check(state, sources=None, partial=None):
    """Machine check with MSR[ME] = 1 (Table 4-10 PAGE 173). `sources`: iterable of MC_CAUSE_BITS
    keys already qualified by the caller, or None if unknown."""
    undef = []
    cause = 0
    if sources is None:
        undef.append(("srr1", 0x003F0000, "machine check source unknown: SRR1[10-15] PAGE 173"))
    else:
        for s in sources:
            cause |= MC_CAUSE_BITS[s]
    return _enter(state, "machine_check", state.pc, partial=partial,
                  # Table 4-10 PAGE 173 "On a best-effort basis Gekko can set this to an EA of some
                  # instruction that was executing or about to be executing"
                  srr0_undef="machine check SRR0 is best-effort PAGE 173",
                  cause=cause, cause_defined=SRR1_CAUSE, copy_mask=SRR1_COPY_MC, clear_me=True,
                  extra_undef=undef)


def enter_dsi(state, ea, dsisr_causes=(), partial=None):
    """DSI (PAGE 174). SRR0 = address of the faulting instruction (state.pc = CIA before commit):
    PAGE 46 "(excluding the trap and system call exceptions) the address of the faulting instruction
    is provided to the exception handler"; PAGE 161 "taken without completing the exception-causing
    instruction"."""
    bits = {"page_fault": _m(1), "protection": _m(4), "direct_store": _m(5), "ear": _m(11)}
    # DSISR[1] page fault, DSISR[4] protection (Table 5-3 PAGE 192); DSISR[5] direct-store
    # (Table 5-4 PAGE 193); DSISR[11] eciwx/ecowx with EAR[E]=0 (Table 4-3 PAGE 162).
    dsisr = 0
    for c in dsisr_causes:
        dsisr |= bits[c]
    # CROSSCHECK FIX (exc crosscheck 2026-09-14, disagreement class "DSI DSISR/SRR1 unnamed bits"):
    # (1) DSISR bits 1/4/5/11 of causes that did NOT occur were defined as 0, and (2) SRR1[1-4,10-15]
    # were defined as 0 (the default cause_defined).  The tables only say what is SET for the cause
    # that occurred ("D access: DSI exception DSISR[1] =1" Table 5-3 PAGE 192) and explicitly defer
    # the rest: "refer to ... Chapter 6, 'Exceptions,' in the PowerPC Microprocessor Family: The
    # Programming Environments manual for a complete description of the SRR1 and DSISR bit settings
    # for these exceptions." PAGE 193.  -> only the occurring cause bits are defined; SRR1 cause
    # bits undefined (this model's own policy for PEM-deferred settings, module docstring).
    return _enter(state, "dsi", state.pc, partial=partial, cause_defined=0,
                  cause_reason="dsi: SRR1[1-4,10-15] deferred to PEM PAGE 193",
                  extra=(("dar", ea & MASK32), ("dsisr", dsisr)),   # DAR = EA: PAGE 59
                  extra_undef=[("dsisr", MASK32 & ~dsisr,
                                "DSISR bits other than the occurring cause bits not given PAGE 192-193")])


def enter_isi(state, fetch_ea, cause, partial=None):
    """ISI (PAGE 174). SRR0 = address whose fetch failed (PAGE 174 "an attempt to fetch the next
    instruction fails"; PAGE 46 faulting-instruction address)."""
    # Table 5-3 PAGE 192: page fault -> SRR1[1]; block/page protection -> SRR1[4];
    # no-execute -> SRR1[3]; Table 5-3/5-4 PAGE 192-193: direct-store and guarded fetch -> SRR1[3].
    bits = {"page_fault": _m(1), "protection": _m(4), "no_execute": _m(3),
            "direct_store": _m(3), "guarded": _m(3)}
    # CROSSCHECK FIX (exc crosscheck 2026-09-14, disagreement class "ISI SRR1 unnamed bits"): SRR1
    # bits 1/3/4 of causes that did NOT occur were defined as 0.  Table 5-3 PAGE 192 only gives the
    # bit set for the occurring condition ("I access: ISI exception SRR1[1] = 1") and PAGE 193 defers
    # "a complete description of the SRR1 and DSISR bit settings" to the PEM. -> only bits[cause].
    return _enter(state, "isi", fetch_ea, partial=partial, cause=bits[cause], cause_defined=bits[cause],
                  cause_reason="isi: SRR1[1-4,10-15] other than the cause bit not given PAGE 192-193")


def enter_external(state, partial=None):
    # SRR0 = next instruction: PAGE 532 (rfi) "the value placed into SRR0 by the exception processing
    # mechanism is the address of the instruction that would have been executed next"; PAGE 46
    # "their handling is postponed until the next instruction ... completes execution".
    return _enter(state, "external", state.pc, partial=partial, cause_defined=0,
                  cause_reason="external: register settings deferred to PEM ch.6 PAGE 174")


def enter_decrementer(state, partial=None):
    return _enter(state, "decrementer", state.pc, partial=partial, cause_defined=0,
                  cause_reason="decrementer: register settings deferred to PEM ch.6 PAGE 176")


def enter_alignment(state, ea, partial=None):
    # SRR0 = CIA (PAGE 46/161); DAR = EA (PAGE 59); DSISR and SRR1 cause bits not given (PAGE 175).
    return _enter(state, "alignment", state.pc, partial=partial, cause_defined=0,
                  cause_reason="alignment: SRR1 cause bits not given PAGE 175",
                  extra=(("dar", ea & MASK32), ("dsisr", 0)),
                  extra_undef=[("dsisr", MASK32, "alignment DSISR contents not given PAGE 175")])


def enter_program(state, kind, partial=None):
    """kind in {'illegal', 'privileged', 'trap', 'fp_enabled'} (Table 4-3 PAGE 162)."""
    if kind not in ("illegal", "privileged", "trap", "fp_enabled"):
        raise ValueError(kind)
    cia = state.pc
    if kind == "trap":
        # PAGE 163 "This may be the address of the instruction that caused the exception or the next
        # one (as in the case of a system call, trace, or trap exception)" vs PAGE 175 "Chapter 6 ...
        # describes register settings for this exception" (source not available) -> SRR0 undefined.
        # Placeholder: CIA+4 (literal PAGE 163 reading), masked.
        return _enter(state, "trap", cia + 4, partial=partial, cause_defined=0,
                      srr0_undef="trap SRR0: PAGE 163 says 'the next one', register settings deferred PAGE 175",
                      cause_reason="program(trap): SRR1 cause encoding deferred to PEM PAGE 175")
    return _enter(state, "program", cia, partial=partial, cause_defined=0,
                  cause_reason=f"program({kind}): SRR1 cause encoding deferred to PEM PAGE 175")


def enter_fp_unavailable(state, partial=None):
    # SRR0 = CIA (PAGE 161 dispatch/execution class, PAGE 46); settings deferred PAGE 175.
    return _enter(state, "fp_unavailable", state.pc, partial=partial, cause_defined=0,
                  cause_reason="fp_unavailable: register settings deferred to PEM PAGE 175")


def enter_syscall(state, partial=None):
    # PAGE 537: "SRR0 <-iea CIA + 4"; "SRR1[0,5-9, 16-23, 25-27, 30-31] <- MSR[...]"; text: "bits 1-4
    # and 10-15 of SRR1 are set to undefined values" (RTL shows 0) -> cause bits undefined.
    # PM (bit 29) is copied: PAGE 537 "An implementation may define additional MSR bits, and ... cause
    # them to be saved to SRR1" + Gekko tables PAGE 169/177/178 copy 16-31.
    return _enter(state, "syscall", state.pc + 4, partial=partial, cause_defined=0, sc_rtl=True,
                  cause_reason="syscall: SRR1[1-4,10-15] 'set to undefined values' PAGE 537")


def enter_trace(state, next_pc, partial=None):
    # SRR0 = next instruction: PAGE 163 "the next one (as in the case of a system call, trace ...)".
    return _enter(state, "trace", next_pc, partial=partial, cause_defined=0,
                  cause_reason="trace: SRR1 cause bits not given PAGE 176")


def enter_performance_monitor(state, partial=None):
    # Table 4-11 PAGE 177: SRR0 next; SRR1 1-4,10-15 cleared -> fully defined.
    return _enter(state, "performance_monitor", state.pc, partial=partial, cause=0)


def enter_iabr(state, partial=None):
    # Table 4-12 PAGE 177-178: SRR0 = instruction that would execute next (the matched instruction,
    # which "is not executed before the exception handler is invoked" PAGE 177); SRR1 cause cleared.
    return _enter(state, "iabr", state.pc, partial=partial, cause=0)


def enter_thermal(state, partial=None):
    # Table 4-13 PAGE 178: SRR0 next; SRR1 cause cleared -> fully defined.
    return _enter(state, "thermal", state.pc, partial=partial, cause=0)


def checkstop(state):
    # PAGE 173 "instruction processing is suspended ... The contents of all latches are frozen"
    state.checkstop = True
    state.exception = "checkstop"
    return "checkstop"


def hard_reset(state, partial=None):
    """Table 4-8 PAGE 170-171: state just before fetching the first reset-handler instruction."""
    unknown = [("gpr", MASK32, "GPRs Unknown PAGE 170"), ("fpr", MASK64, "FPRs Unknown PAGE 170"),
               ("reserve_addr", MASK32, "Reservation address Unknown PAGE 171")]
    if not _partial(partial):
        raise UndefinedBehaviour("; ".join(r for _, _, r in unknown))
    state.fpscr = 0          # PAGE 170
    state.cr = 0             # PAGE 170 "All 0s"
    state.msr = 0x00000040   # PAGE 170 "00000040 (only IP set)"
    state.xer = 0            # PAGE 170
    state.hid0 = 0           # PAGE 170
    state.hid2 = 0           # PAGE 170
    state.gqr = [0] * 8      # PAGE 170
    state.iabr = 0           # PAGE 170 "All 0s (break point disabled)"
    state.tb = 0             # PAGE 171 TBU/TBL 00000000
    state.lr = 0             # PAGE 171
    state.ctr = 0            # PAGE 171
    state.srr0 = 0           # PAGE 171
    state.srr1 = 0           # PAGE 171 (Gekko manual; 750CL PAGE 170 says 00000040 - Gekko wins)
    state.sprg = [0] * 4     # PAGE 171
    state.dsisr = 0          # PAGE 171
    state.dar = 0            # PAGE 171
    state.dec = 0xFFFFFFFF   # PAGE 171
    state.reserve = False    # PAGE 171 "reservation flag cleared"
    state.pc = 0xFFF00100    # PAGE 170 "Gekko immediately branches to 0xFFF0_0100"
    state.checkstop = False
    for k in ("hard_reset", "soft_reset", "machine_check", "decrementer"):
        # PAGE 170 "all operations cease, and the machine state is lost"; PAGE 169 SRESET coincident
        # with HRESET "no effect". External INT is a pin level and is left to the caller.
        if k in state.pending:
            state.pending[k] = False
    state.exception = "hard_reset"
    state.spec_undefined = {"gpr": MASK32, "fpr": MASK64, "reserve_addr": MASK32}
    return "hard_reset"


# ---------------------------------------------------------------------------------------------
# asynchronous recognition between instructions
# ---------------------------------------------------------------------------------------------
def take_pending(state, partial=None):
    """Recognise and take the highest-priority pending asynchronous exception at an instruction
    boundary. state.pending keys: hard_reset, machine_check, soft_reset, external,
    performance_monitor, decrementer, thermal (smi is rejected).

    Priority (Table 4-3 PAGE 162): 0 hard reset, 1 machine check, 2 soft reset, 3 external,
    4 performance monitor, 5 decrementer, 6 thermal.
    """
    p = state.pending
    if p.get("hard_reset"):
        # PAGE 160 "Has highest priority and is taken immediately regardless of other pending
        # exceptions or recoverability."
        return hard_reset(state, partial)
    # CROSSCHECK FIX (exc crosscheck 2026-09-14, disagreement class "hard reset in checkstop"): the
    # checkstop test came first and returned None, so a pending HRESET could never leave checkstop.
    # "Once Gekko enters a checkstop state, only a hard reset can clear the processor from the
    # checkstop state." PAGE 302 -> hard reset is tested first (above).  Also report 'checkstop'
    # (return value and state.exception) exactly as step() already does for a checkstopped state
    # (PAGE 173 "instruction processing is suspended").
    if getattr(state, "checkstop", False):
        state.exception = "checkstop"
        return "checkstop"
    mc = p.get("machine_check")
    if mc:
        sources = None if mc is True else set(mc)
        if sources is not None:
            # Table 4-9 PAGE 172: EMCP=0 "Masks MCP. Asserting MCP does not generate a machine check
            # exception or a checkstop."; EBA/EBD=0 prevent address/data parity checking.
            if not state.hid0 & HID0_EMCP:
                sources.discard("mcp")
            if not state.hid0 & HID0_EBA:
                sources.discard("address_parity")
            if not state.hid0 & HID0_EBD:
                sources.discard("data_parity")
        if sources is None or sources:
            if not state.msr & MSR_ME:
                # PAGE 166 step 5 / PAGE 173 "If MSR[ME] = 0 and a machine check occurs, the processor
                # enters the checkstop state."
                p["machine_check"] = False
                return checkstop(state)
            # PAGE 160 "Taken immediately regardless of recoverability."  Not maskable by EE.
            name = enter_machine_check(state, sources, partial)
            p["machine_check"] = False  # interpretation: the reported condition is consumed
            return name
        p["machine_check"] = False
    if p.get("soft_reset"):
        # PAGE 161 priority 2; PAGE 166 "System reset exceptions cannot be masked."
        # PAGE 169 "SRESET is an effectively edge-sensitive signal" -> consumed when taken.
        name = enter_system_reset(state, partial)
        p["soft_reset"] = False
        return name
    if not state.msr & MSR_EE:
        # PAGE 164 "EE 0 The processor delays recognition of external interrupts and decrementer
        # exception conditions."; PAGE 58 "Setting MSR[EE] masks not only ... external interrupt and
        # decrementer exceptions but also the Gekko-specific system management, performance monitor,
        # and thermal management exceptions."  Conditions stay pending (PAGE 475 mtmsr text).
        return None
    if p.get("smi"):
        # Table 4-2 PAGE 159 "Reserved 01400-016FF"; SMI only exists on the 750CL (750CL PAGE 159).
        # CROSSCHECK FIX (exc crosscheck 2026-09-14, disagreement class "pending smi"): this raised
        # before every other test, i.e. even when a hard reset, machine check or soft reset (all of
        # higher, defined priority: PAGE 160 item 1 vs item 4 "Maskable asynchronous exceptions
        # (external, decrementer, thermal management, system management, ...)") was taken, and even
        # when EE = 0 masks it ("Setting MSR[EE] masks ... also the Gekko-specific system management
        # ... exceptions" PAGE 58).  Only a recognisable SMI has no vector / priority.
        raise UndefinedBehaviour("system management interrupt: vector 0x1400 reserved on Gekko PAGE 159/162")
    if p.get("external"):
        # PAGE 174 "The INT signal is expected to remain asserted until Gekko takes the external
        # interrupt exception" -> level input; the model does NOT clear it (the system negates INT).
        return enter_external(state, partial)
    if p.get("performance_monitor"):
        # PAGE 177 "between the external interrupt and the decrementer interrupt"; level condition
        # owned by PMC/MMCR0 logic (not cleared here).
        return enter_performance_monitor(state, partial)
    if p.get("decrementer"):
        # Table 4-2 PAGE 159 "when the most-significant bit of the DEC register changes from 0 to 1"
        # -> an edge; the latch is consumed when the exception is taken (interpretation).
        name = enter_decrementer(state, partial)
        p["decrementer"] = False
        return name
    if p.get("thermal"):
        # Table 4-3 PAGE 162 priority 6; level from THRM1/2 (not cleared here).
        return enter_thermal(state, partial)
    return None


# ---------------------------------------------------------------------------------------------
# decrementer / time base
# ---------------------------------------------------------------------------------------------
def timer_advance(state, ticks: int) -> int:
    """Advance TB and DEC by `ticks` counter steps.
    PAGE 60 "In Gekko, the decrementer register is decremented and the time base is incremented at a
    speed that is one-fourth the speed of the bus clock." -> both move by the same count.
    Returns the number of MSB 0->1 transitions of DEC (Table 4-2 PAGE 159); any transition sets the
    decrementer latch (several before recognition collapse into one pending condition)."""
    if ticks < 0:
        raise ValueError("ticks must be >= 0")
    if getattr(state, "checkstop", False):
        # CROSSCHECK FIX (common-mode, found by manual review during the exc crosscheck 2026-09-14;
        # exc_A had the same omission): "The contents of all latches are frozen within two cycles upon
        # entering checkstop state." PAGE 173 -> TB/DEC frozen, no decrementer condition.
        return 0
    old = state.dec & MASK32
    state.tb = (state.tb + ticks) & MASK64                    # TB is 64-bit, PAGE 41
    state.dec = (old - ticks) & MASK32                        # DEC is a 32-bit decrementing counter, PAGE 41
    # MSB goes 0->1 only when the count passes 0x00000000 -> 0xFFFFFFFF
    # (Table 4-3 PAGE 162 "Decrementer passes through zero").
    crossings = 0 if ticks <= old else 1 + (ticks - old - 1) // (1 << 32)
    if crossings:
        state.pending["decrementer"] = True
    return crossings


def advance_bus_clocks(state, bus_clocks: int) -> int:
    # PAGE 60 / PAGE 176 "decremented at one fourth the bus clock rate". The phase of the /4 prescaler
    # is not specified.
    if bus_clocks % 4:
        raise UndefinedBehaviour("DEC/TB prescaler phase for a non-multiple of 4 bus clocks PAGE 60")
    return timer_advance(state, bus_clocks // 4)


def write_dec(state, value: int) -> None:
    """Software write of DEC (the mtspr itself belongs to the SPR area)."""
    value &= MASK32
    if state.pending.get("decrementer"):
        raise UndefinedBehaviour("effect of a DEC write on an already-pending decrementer condition PAGE 159/176")
    if not state.dec & 0x80000000 and value & 0x80000000:
        # Table 4-2 PAGE 159 "MSB ... changes from 0 to 1" vs Table 4-3 PAGE 162 "passes through zero"
        # and PAGE 176 "has completed decrementing": whether a write can raise it is not stated.
        raise UndefinedBehaviour("DEC write changing MSB 0->1: exception or not PAGE 159/162")
    state.dec = value


# ---------------------------------------------------------------------------------------------
# pre-/post-instruction checks
# ---------------------------------------------------------------------------------------------
def _iabr_matches(state, ea) -> bool:
    iabr = getattr(state, "iabr", 0) & MASK32
    # Table 4-2 PAGE 159 / PAGE 177: "IABR[0-29] matches EA[0-29] of the next instruction to complete,
    # IABR[TE] matches MSR[IR], and IABR[BE] = 1"
    return bool(iabr & IABR_BE) and bool(iabr & IABR_TE) == bool(state.msr & MSR_IR) and \
        (iabr & 0xFFFFFFFC) == (ea & 0xFFFFFFFC)


def check_iabr(state, partial=None) -> bool:
    """Call before executing the instruction at state.pc. Table 4-3 PAGE 162: IABR is priority 0 of
    the dispatch/execution exceptions."""
    if _iabr_matches(state, state.pc):
        enter_iabr(state, partial)
        return True
    return False


BRANCH_PRIMARY = (16, 18)   # bc, b   - Table 2-46 PAGE 113
BRANCH_XL = (16, 528)       # bclr, bcctr (primary 19)


def is_branch(word: int) -> bool:
    op = word >> 26
    if op in BRANCH_PRIMARY:
        return True
    return op == 19 and ((word >> 1) & 0x3FF) in BRANCH_XL


def trace_after(state, word, next_pc, partial=None, *, msr_before=None) -> bool:
    """Post-instruction trace check for an instruction that completed WITHOUT another exception.
    Must be called with state.pc already = next_pc. Returns True if trace was taken."""
    msr = state.msr if msr_before is None else msr_before
    op = word >> 26
    xo = (word >> 1) & 0x3FF
    is_rfi = op == 19 and xo == 50
    is_isync = op == 19 and xo == 150
    is_sc = op == 17
    # PAGE 164 SE: "trace exception upon the successful execution of every instruction except rfi,
    # isync, and sc."  PAGE 176 "does not take trace exceptions on the isync instruction."
    se = bool(msr & MSR_SE) and not (is_rfi or is_isync or is_sc)
    # PAGE 159 "a branch instruction is completing and MSR[BE] = 1"; branch instructions = b, bc, bclr,
    # bcctr (Table 2-46 PAGE 113). Not restricted to taken branches by the text (interpretation).
    be = bool(msr & MSR_BE) and is_branch(word)
    if not (se or be):
        return False
    if _iabr_matches(state, next_pc):
        # 750CL PAGE 180 "If trace and instruction breakpoint conditions occur simultaneously,
        # instruction breakpoint takes priority over trace" - fate of the trace is not stated.
        raise UndefinedBehaviour("trace and IABR at the same boundary: trace discarded or kept 750CL PAGE 180")
    enter_trace(state, next_pc, partial)
    return True


# ---------------------------------------------------------------------------------------------
# instructions
# ---------------------------------------------------------------------------------------------
def _decode(word):
    op = word >> 26
    xo = (word >> 1) & 0x3FF
    if op == 17:
        return "sc"
    if op == 3:
        return "twi"
    if op == 19 and xo == 50:
        return "rfi"
    if op == 31 and xo == 4:
        return "tw"
    if op == 31 and xo == 146:
        return "mtmsr"
    if op == 31 and xo == 83:
        return "mfmsr"
    return None


def supports(word: int) -> bool:
    return _decode(word & MASK32) is not None


def _exts32(v):
    v &= MASK32
    return v - (1 << 32) if v & 0x80000000 else v


def _trap_cond(to, a, b):
    # PAGE 578/579: TO[0] a<b, TO[1] a>b, TO[2] a=b, TO[3] a<U b, TO[4] a>U b.
    # TO[0] is bit 6 of the instruction = MSB (0x10) of the 5-bit field.
    sa, sb = _exts32(a), _exts32(b)
    ua, ub = a & MASK32, b & MASK32
    return bool(((to & 0x10) and sa < sb) or ((to & 0x08) and sa > sb) or ((to & 0x04) and sa == sb)
                or ((to & 0x02) and ua < ub) or ((to & 0x01) and ua > ub))


def step(state, word: int, partial=None) -> None:
    # CROSSCHECK FIX (exc crosscheck 2026-09-14, disagreement class "B:strict-raise-not-atomic"):
    # the module contract says "In strict mode the raise happens before any state is modified", but
    # step() cleared state.exception / spec_undefined, and for tw/twi/mtmsr/mfmsr wrote pc (and rD)
    # before trace_after() could raise.  The step now runs transactionally: any UndefinedBehaviour
    # restores the complete pre-step state.
    mem = state.mem
    state.mem = None
    try:
        saved = _copy.deepcopy(state.__dict__)
    finally:
        state.mem = mem
    try:
        _step(state, word, partial)
    except UndefinedBehaviour:
        state.__dict__.clear()
        state.__dict__.update(saved)
        state.mem = mem
        raise


def _or_mark(state, reg, mask):
    if mask:
        d = _undef_dict(state)
        d[reg] = d.get(reg, 0) | mask


def _step(state, word: int, partial=None) -> None:
    word &= MASK32
    state.exception = None
    state.spec_undefined = {}
    if getattr(state, "checkstop", False):
        state.exception = "checkstop"   # PAGE 173 instruction processing suspended
        return
    kind = _decode(word)
    if kind is None:
        raise ValueError(f"exc_B does not model word 0x{word:08X}")
    cia = state.pc & MASK32

    # IABR - dispatch/execution priority 0 (Table 4-3 PAGE 162); the instruction is not executed.
    if check_iabr(state, partial):
        return

    rd = (word >> 21) & 0x1F
    ra = (word >> 16) & 0x1F
    rb = (word >> 11) & 0x1F

    # Reserved fields: the format diagrams mark them "Reserved"; behaviour of such invalid forms is
    # not given in these manuals.
    reserved_bad = {
        "sc": (word & 0x03FFFFFD) != 0 or not (word & 2),     # PAGE 537 format: 17 0..0 1 0
        "rfi": (word & 0x03FFF801) != 0,                        # PAGE 532 format
        "tw": (word & 1) != 0,                                  # PAGE 578 format
        "twi": False,                                           # PAGE 579 format
        "mtmsr": (word & 0x001FF801) != 0,                      # PAGE 475 format
        "mfmsr": (word & 0x001FF801) != 0,                      # PAGE 461 format
    }[kind]
    if reserved_bad:
        raise UndefinedBehaviour(f"{kind}: nonzero reserved field in 0x{word:08X} (format diagram PAGE 461-579)")

    msr_before = state.msr & MASK32
    supervisor_only = kind in ("rfi", "mtmsr", "mfmsr")   # PAGE 90 supervisor-level list
    if supervisor_only and msr_before & MSR_PR:
        # PAGE 90 "An attempt by a user-level program to execute the supervisor-level instructions listed
        # below causes the privileged instruction (program exception) handler to be invoked."
        enter_program(state, "privileged", partial)
        return

    if kind == "sc":
        # PAGE 537; no trace for sc (PAGE 164).
        enter_syscall(state, partial)
        return

    if kind in ("tw", "twi"):
        a = state.gpr[ra]
        if kind == "tw":
            b = state.gpr[rb]
        else:
            b = (word & 0xFFFF) - (0x10000 if word & 0x8000 else 0)   # EXTS(SIMM), PAGE 579
        if _trap_cond(rd, a, b):
            # PAGE 578 "the system trap handler is invoked"; PAGE 90 "program exception trap handler".
            enter_program(state, "trap", partial)
            return
        state.pc = (cia + 4) & MASK32
        trace_after(state, word, state.pc, partial, msr_before=msr_before)
        return

    if kind == "mfmsr":
        state.gpr[rd] = msr_before   # PAGE 461 "rD <- MSR"
        state.pc = (cia + 4) & MASK32
        trace_after(state, word, state.pc, partial, msr_before=msr_before)
        return

    if kind == "mtmsr":
        value = state.gpr[rd] & MASK32   # PAGE 475 "MSR <- (rS)"
        # CROSSCHECK FIX (exc crosscheck 2026-09-14, disagreement class "mtmsr reserved bits"): this
        # raised even with partial=True, although only register CONTENTS are unspecified (the module
        # policy reserves always-raise for control flow).  The MSR image of a written reserved bit is
        # unspecified ("Reserved bits are read as if written as 0" PAGE 168 vs "Full function" reserved
        # bits, Table 4-4 PAGE 164-165); pc = CIA+4 is not affected. -> mark MSR bits in partial mode.
        msr_undef = value & MSR_RESERVED
        if (value ^ msr_before) & (MSR_POW | MSR_LE):
            # PAGE 475 "execution synchronizing instruction except with respect to alterations to the POW
            # and LE bits"; POW enters power management (chapter 10, not modelled).
            raise UndefinedBehaviour("mtmsr altering POW or LE PAGE 475")
        # CROSSCHECK FIX (exc crosscheck 2026-09-14, disagreement class "mtmsr altering BE"): a BE-only
        # change raised.  Branch trace fires only "when a branch instruction executes successfully"
        # (PAGE 164) / "a branch instruction is completing and MSR[BE] = 1" (Table 4-2 PAGE 159); mtmsr
        # is not a branch (Table 2-46 PAGE 113), so neither the old nor the new BE can trace the mtmsr
        # itself.  Only an SE change leaves the mtmsr's own trace undefined.
        if (value ^ msr_before) & MSR_SE:
            raise UndefinedBehaviour("mtmsr altering SE: whether the mtmsr itself is traced PAGE 164/475")
        if msr_undef and not _partial(partial):
            raise UndefinedBehaviour(f"mtmsr writing reserved MSR bits 0x{msr_undef:08X} PAGE 164-165/168")
        state.msr = value
        # PAGE 475 "alterations to the MSR[EE] and MSR[RI] bits are effective as soon as the instruction
        # completes" - take_pending at the next boundary sees the new EE.
        state.pc = (cia + 4) & MASK32
        trace_after(state, word, state.pc, partial, msr_before=msr_before)
        _or_mark(state, "msr", msr_undef)
        return

    if kind == "rfi":
        srr1 = state.srr1 & MASK32
        # CROSSCHECK FIX (exc crosscheck 2026-09-14, disagreement class "rfi PM / reserved bits"): both
        # cases raised even with partial=True although only the MSR CONTENTS of those bits are
        # unspecified; NIA <- SRR0[0-29] || 0b00 (PAGE 532) is unaffected. -> mark MSR bits.
        msr_undef = 0
        if (srr1 ^ msr_before) & MSR_PM:
            # PAGE 532 RTL restores 0,5-9,16-23,25-27,30-31 (not PM=29) but "an implementation may
            # define additional MSR bits, and ... restored to MSR from SRR1 on an rfi" - Gekko silent.
            msr_undef |= MSR_PM
        new = (msr_before & ~RFI_RESTORE) | (srr1 & RFI_RESTORE)   # PAGE 532
        new &= ~MSR_POW                                            # PAGE 532 "MSR[13] <- b'0'"
        msr_undef |= new & MSR_RESERVED                            # PAGE 164-165 / 168
        if msr_undef and not _partial(partial):
            raise UndefinedBehaviour(f"rfi: MSR bits 0x{msr_undef:08X} not specified (PM restore PAGE 532, "
                                     f"reserved bits PAGE 164-165/168)")
        state.msr = new & MASK32
        state.pc = state.srr0 & 0xFFFFFFFC                       # PAGE 532 "NIA <- SRR0[0-29] || 0b00"
        _or_mark(state, "msr", msr_undef)
        # No trace: SE excludes rfi (PAGE 164); rfi is not a branch instruction (Table 2-46 PAGE 113).
        # Pending exceptions enabled by the new MSR are taken by take_pending with SRR0 = this target
        # (PAGE 532).
        return


# =============================================================================================
# self-test: hand-derived vectors from the manual text
# =============================================================================================
if __name__ == "__main__":
    import copy

    results = []

    def check(name, cond):
        results.append((name, bool(cond)))
        if not cond:
            print("FAIL:", name)

    def raises(fn, *a, **k):
        try:
            fn(*a, **k)
        except UndefinedBehaviour:
            return True
        return False

    def mk(msr=0x00002032, pc=0x80003100):
        s = State()
        s.msr = msr
        s.pc = pc
        return s

    def enc_x31(rs, ra, rb, xo, rc=0):
        return (31 << 26) | (rs << 21) | (ra << 16) | (rb << 11) | (xo << 1) | rc

    SC = 0x44000002
    RFI = 0x4C000064

    # 1-2 vector prefix (PAGE 166)
    check("vec IP=0 external", vector_address(0x8000, "external") == 0x00000500)
    check("vec IP=1 external", vector_address(0x8040, "external") == 0xFFF00500)

    # 3 external delayed while EE=0 (PAGE 164)
    s = mk(0x2032); s.pending["external"] = True
    check("ext EE=0 not taken", take_pending(s, partial=True) is None and s.pc == 0x80003100
          and s.pending["external"])

    # 4 external taken EE=1 (partial)
    s = mk(0xA032); s.pending["external"] = True
    r = take_pending(s, partial=True)
    check("ext taken", r == "external" and s.srr0 == 0x80003100 and s.srr1 == 0xA032
          and s.msr == 0 and s.pc == 0x500 and s.spec_undefined.get("srr1") == SRR1_CAUSE)
    check("ext level not cleared", s.pending["external"] is True)

    # 5 strict: raise before mutating
    s = mk(0xA032); s.pending["external"] = True; before = copy.deepcopy(s.arch_dict())
    check("ext strict raises", raises(take_pending, s) and s.arch_dict() == before)

    # 6 decrementer pending while EE=0 persists, mtmsr EE=1 then taken (PAGE 475)
    s = mk(0x2032); s.pending["decrementer"] = True
    check("dec EE=0 pending kept", take_pending(s, partial=True) is None and s.pending["decrementer"])
    s.gpr[5] = 0xA032
    step(s, enc_x31(5, 0, 0, 146))
    check("mtmsr sets EE", s.msr == 0xA032 and s.pc == 0x80003104 and s.exception is None)
    r = take_pending(s, partial=True)
    check("dec taken after mtmsr, SRR0=next", r == "decrementer" and s.srr0 == 0x80003104
          and s.pc == 0x900 and s.srr1 == 0xA032)
    check("dec latch consumed", s.pending["decrementer"] is False)

    # 7 priorities (Table 4-3 PAGE 162)
    s = mk(0xA032); s.pending.update(external=True, decrementer=True)
    check("ext beats dec", take_pending(s, partial=True) == "external" and s.pending["decrementer"])
    check("dec stays pending after ext entry (EE=0)", take_pending(s, partial=True) is None)
    s = mk(0xA032); s.pending.update(performance_monitor=True, decrementer=True)
    check("pm beats dec", take_pending(s) == "performance_monitor")
    s = mk(0xA032); s.pending.update(performance_monitor=True, external=True)
    check("ext beats pm", take_pending(s, partial=True) == "external")
    s = mk(0xA032); s.pending.update(thermal=True, decrementer=True)
    check("dec beats thermal", take_pending(s, partial=True) == "decrementer")

    # 8 fully defined async entries work in strict mode (PAGE 177/178)
    s = mk(0x0000FF33); s.pending["thermal"] = True   # every non-reserved low bit except IP
    msr0 = s.msr
    check("thermal strict", take_pending(s) == "thermal" and s.srr1 == 0x0000FF33
          and s.msr == (MSR_ILE & msr0 | MSR_ME) and s.pc == 0x1700 and s.spec_undefined == {})
    s = mk(0x0000FF33 | MSR_ILE); s.pending["thermal"] = True
    take_pending(s)
    check("thermal ILE kept, LE<-ILE, srr1 excludes ILE", s.msr == (MSR_ILE | MSR_ME | MSR_LE)
          and s.srr1 == 0x0000FF33)
    s = mk(0xB032 | MSR_ILE); s.pending["performance_monitor"] = True
    take_pending(s)
    check("pm ILE->LE, ME kept", s.msr == (MSR_ILE | MSR_LE | MSR_ME) and s.srr1 == 0xB032 and s.pc == 0xF00)
    s = mk(0xA072); s.pending["performance_monitor"] = True
    take_pending(s)
    check("IP kept, vector high", s.msr == MSR_IP and s.pc == 0xFFF00F00)

    # 9 soft reset: nonmaskable, beats external, edge consumed (PAGE 161/169)
    s = mk(0x2032); s.pending.update(soft_reset=True, external=True)
    check("sreset ignores EE", take_pending(s) == "system_reset" and s.pc == 0x100
          and s.srr0 == 0x80003100 and s.srr1 == 0x2032 and not s.pending["soft_reset"])
    s = mk(0xA032); s.pending.update(soft_reset=True, external=True)
    check("sreset beats ext", take_pending(s) == "system_reset")

    # 10 machine check (Table 4-9/4-10 PAGE 172-173)
    s = mk(0x3032); s.hid0 = HID0_EMCP; s.pending["machine_check"] = {"mcp"}
    r = take_pending(s, partial=True)
    check("mc taken", r == "machine_check" and s.srr1 == (0x3032 | _m(12)) and s.msr == 0
          and s.pc == 0x200 and s.spec_undefined.get("srr0") == MASK32)
    s = mk(0x3032); s.hid0 = 0; s.pending["machine_check"] = {"mcp"}
    check("mc MCP masked by EMCP=0", take_pending(s, partial=True) is None and s.pc == 0x80003100)
    s = mk(0x2032); s.hid0 = HID0_EMCP; s.pending["machine_check"] = {"mcp"}
    before = copy.deepcopy(s.arch_dict())
    r = take_pending(s, partial=True)
    frozen = s.arch_dict(); frozen["exception"] = before["exception"]
    check("mc ME=0 checkstop", r == "checkstop" and s.checkstop and frozen == before)
    step(s, SC)
    check("checkstop suspends step", s.exception == "checkstop" and s.pc == 0x80003100)
    s = mk(0x3032); s.pending.update(machine_check={"tea"}, soft_reset=True)
    check("mc beats sreset", take_pending(s, partial=True) == "machine_check" and s.pending["soft_reset"])
    s = mk(0x3032); s.pending["machine_check"] = {"tea"}
    check("mc strict raises (SRR0 best effort)", raises(take_pending, s))

    # 11 hard reset (Table 4-8 PAGE 170-171)
    s = mk(0xA032); s.dec = 5; s.pending.update(hard_reset=True, external=True, decrementer=True)
    check("hreset strict raises", raises(take_pending, s))
    r = take_pending(s, partial=True)
    check("hreset partial", r == "hard_reset" and s.msr == 0x40 and s.pc == 0xFFF00100
          and s.dec == 0xFFFFFFFF and s.srr1 == 0 and s.hid2 == 0 and not s.pending["decrementer"])

    # 12 SMI is not a Gekko exception (PAGE 160)
    s = mk(0xA032); s.pending["smi"] = True
    check("smi raises", raises(take_pending, s, partial=True))

    # 13 reserved MSR bit at entry (PAGE 165/168)
    s = mk(0xA032 | MSR_B28); s.pending["thermal"] = True
    check("reserved msr at entry strict raises", raises(take_pending, s))

    # 14 sc (PAGE 537)
    s = mk(0x0000E032)   # EE|PR|FP|IR|DR|RI
    check("sc strict raises", raises(step, s, SC) and s.pc == 0x80003100)
    step(s, SC, partial=True)
    check("sc srr0=CIA+4", s.exception == "syscall" and s.srr0 == 0x80003104 and s.pc == 0xC00)
    check("sc srr1 copy, PR cleared", s.srr1 == 0xE032 and s.msr == 0)
    s = mk(0x0000A436)   # SE and PM set
    step(s, SC, partial=True)
    check("sc under SE: no trace, PM copied", s.exception == "syscall" and s.srr1 == 0xA436)
    check("sc bad reserved field", raises(step, mk(), 0x44000003, partial=True))

    # 15 rfi (PAGE 532)
    s = mk(0x00000000); s.srr0 = 0x80001237; s.srr1 = 0x0000A032
    step(s, RFI)
    check("rfi restore & NIA", s.msr == 0xA032 and s.pc == 0x80001234 and s.exception is None)
    s = mk(MSR_ILE | MSR_POW | MSR_ME); s.srr0 = 0x100; s.srr1 = 0x0000A032 | MSR_POW
    step(s, RFI)
    check("rfi POW<-0, ILE not restored", s.msr == (MSR_ILE | 0xA032))
    s = mk(0); s.srr0 = 0x100; s.srr1 = MSR_ILE | 0x2032
    step(s, RFI)
    check("rfi bit15 ILE not taken from SRR1", s.msr == 0x2032)
    s = mk(0x6032); s.srr0 = 0x100; s.srr1 = 0
    check("rfi user-mode strict raises", raises(step, s, RFI))
    step(s, RFI, partial=True)
    check("rfi user-mode -> program", s.exception == "program" and s.srr0 == 0x80003100 and s.pc == 0x700)
    s = mk(0x2032); s.srr0 = 0x80005000; s.srr1 = 0xA032; s.pending["external"] = True
    step(s, RFI)
    r = take_pending(s, partial=True)
    check("rfi enables pending ext, SRR0 = rfi target", r == "external" and s.srr0 == 0x80005000)
    s = mk(0x2432); s.srr0 = 0x80005000; s.srr1 = 0x2432
    step(s, RFI)
    check("rfi with SE: no trace", s.exception is None and s.pc == 0x80005000)
    s = mk(0x2032); s.srr1 = 0x2036
    before = copy.deepcopy(s.arch_dict())
    check("rfi PM mismatch strict raises atomically", raises(step, s, RFI) and s.arch_dict() == before)
    step(s, RFI, partial=True)   # CROSSCHECK FIX: contents-only UB -> marked, not raised
    check("rfi PM mismatch partial marks MSR[PM]", s.spec_undefined == {"msr": MSR_PM} and s.msr == 0x2032)

    # 16 mtmsr / mfmsr (PAGE 475 / 461)
    s = mk(0xA032); s.pending["external"] = True; s.gpr[3] = 0x2032
    step(s, enc_x31(3, 0, 0, 146))
    check("mtmsr clears EE -> ext not taken", take_pending(s, partial=True) is None)
    s = mk(0x6032); s.gpr[3] = 0x2032
    step(s, enc_x31(3, 0, 0, 146), partial=True)
    check("mtmsr user-mode privileged", s.exception == "program" and s.msr == 0)
    s = mk(0x2032); s.gpr[3] = 0x2032 | MSR_POW
    check("mtmsr POW raises", raises(step, s, enc_x31(3, 0, 0, 146), partial=True))
    s = mk(0x2032); s.gpr[3] = 0x2032 | _m(5)
    check("mtmsr reserved strict raises", raises(step, s, enc_x31(3, 0, 0, 146)) and s.msr == 0x2032)
    step(s, enc_x31(3, 0, 0, 146), partial=True)   # CROSSCHECK FIX: contents-only UB -> marked
    check("mtmsr reserved partial marks", s.spec_undefined == {"msr": _m(5)} and s.pc == 0x80003104)
    s = mk(0x2032); s.gpr[3] = 0x2432
    check("mtmsr SE change raises", raises(step, s, enc_x31(3, 0, 0, 146), partial=True))
    s = mk(0x2032); s.gpr[3] = 0x2232
    step(s, enc_x31(3, 0, 0, 146))                  # CROSSCHECK FIX: BE-only change is defined
    check("mtmsr BE change defined", s.msr == 0x2232 and s.pc == 0x80003104 and s.exception is None)
    s = mk(0x2432); s.gpr[3] = 1
    before = copy.deepcopy(s.arch_dict())
    check("tw SE trace strict raise is atomic", raises(step, s, enc_x31(4, 3, 4, 4)) and s.arch_dict() == before)
    s = mk(0x2032); s.checkstop = True; s.pending["hard_reset"] = True
    check("hard reset leaves checkstop (PAGE 302)", take_pending(s, partial=True) == "hard_reset"
          and not s.checkstop and s.pc == 0xFFF00100)
    s = mk(0x2032); s.pending.update(smi=True, soft_reset=True)
    check("smi below soft reset", take_pending(s) == "system_reset")
    s = mk(0x2032); s.pending["smi"] = True
    check("smi masked by EE=0", take_pending(s) is None)
    s = mk(0xA032); s.gpr[31] = 0x12345678
    step(s, enc_x31(31, 0, 0, 83))
    check("mfmsr", s.gpr[31] == 0xA032 and s.pc == 0x80003104)
    s = mk(0xA032)
    check("mfmsr reserved field raises", raises(step, s, enc_x31(31, 1, 0, 83)))

    # 17 tw / twi (PAGE 578/579)
    s = mk(0x2032)
    step(s, enc_x31(31, 0, 0, 4), partial=True)      # trap = tw 31,0,0
    check("trap unconditional", s.exception == "trap" and s.pc == 0x700 and s.spec_undefined.get("srr0") == MASK32)
    s = mk(0x2032); s.gpr[3] = 1; s.gpr[4] = 2
    step(s, enc_x31(4, 3, 4, 4))
    check("tweq not equal no trap", s.exception is None and s.pc == 0x80003104)
    s = mk(0x2032); s.gpr[3] = 0xFFFFFFFF; s.gpr[4] = 1
    step(s, enc_x31(16, 3, 4, 4), partial=True)
    check("tw lt signed -1<1 traps", s.exception == "trap")
    s = mk(0x2032); s.gpr[3] = 0xFFFFFFFF; s.gpr[4] = 1
    step(s, enc_x31(2, 3, 4, 4))
    check("tw ltu -1<U1 false", s.exception is None)
    s = mk(0x2032); s.gpr[3] = 5
    step(s, enc_x31(5, 3, 3, 4), partial=True)       # twlge r3,r3 (aliasing): eq true
    check("tw aliased eq traps", s.exception == "trap")
    s = mk(0x2032); s.gpr[7] = 0
    step(s, (3 << 26) | (8 << 21) | (7 << 16) | 0xFFFF, partial=True)   # twgti r7,-1
    check("twi gt 0>-1 traps", s.exception == "trap")
    s = mk(0x2032); s.gpr[7] = 0
    step(s, (3 << 26) | (1 << 21) | (7 << 16) | 0xFFFF)                 # gtu 0 >U 0xFFFFFFFF
    check("twi gtu false", s.exception is None)
    s = mk(0x2032); s.gpr[7] = 0x80000000
    step(s, (3 << 26) | (0x10 << 21) | (7 << 16) | 0x0000, partial=True)  # lt: INT_MIN < 0
    check("twi lt INT_MIN", s.exception == "trap")
    s = mk(0x2032)
    step(s, (3 << 26) | (0 << 21) | (0 << 16) | 0)
    check("twi TO=0 never traps", s.exception is None)
    s = mk(0x2032); s.gpr[3] = 7
    step(s, (3 << 26) | (4 << 21) | (3 << 16) | 7, partial=True)   # tweqi r3,7
    check("twi eq traps", s.exception == "trap")

    # 18 trace (PAGE 164/176)
    s = mk(0x2432)
    step(s, enc_x31(4, 3, 4, 4), partial=True)   # s.gpr all zero -> equal -> traps? TO=4 eq: 0==0
    check("trap taken instead of trace", s.exception == "trap")
    s = mk(0x2432); s.gpr[3] = 1
    step(s, enc_x31(4, 3, 4, 4), partial=True)
    check("SE trace after tw no-trap", s.exception == "trace" and s.srr0 == 0x80003104
          and s.pc == 0xD00 and s.srr1 == 0x2432 and s.msr == 0)
    s = mk(0x2432); s.gpr[3] = 1
    check("SE trace strict raises", raises(step, s, enc_x31(4, 3, 4, 4)))
    s = mk(0x2232); s.pc = 0x80000010   # BE only, branch "b" completed to target
    check("BE branch trace", trace_after(s, 0x48000100, 0x80000110, partial=True)
          and s.srr0 == 0x80000110 and s.exception == "trace")
    s = mk(0x2232)
    check("BE non-branch no trace", not trace_after(s, 0x60000000, 0x80003104, partial=True))
    s = mk(0x2432); s.iabr = 0x80003104 | IABR_BE | IABR_TE
    check("trace vs IABR raises", raises(trace_after, s, 0x60000000, 0x80003104, partial=True))

    # 19 IABR (PAGE 177-178, Table 2-3 PAGE 62)
    s = mk(0x6032); s.iabr = 0x80003100 | IABR_BE | IABR_TE
    step(s, RFI)   # user-mode rfi would be privileged, but IABR has priority 0
    check("IABR beats privileged", s.exception == "iabr" and s.srr0 == 0x80003100 and s.pc == 0x1300
          and s.srr1 == 0x6032 and s.spec_undefined == {})
    s = mk(0x2012); s.iabr = 0x80003100 | IABR_BE | IABR_TE   # IR=0, TE=1: mismatch
    step(s, enc_x31(31, 0, 0, 83))
    check("IABR TE mismatch no break", s.exception is None)
    s = mk(0x2032); s.iabr = 0x80003100 | IABR_TE              # BE=0
    step(s, enc_x31(31, 0, 0, 83))
    check("IABR BE=0 no break", s.exception is None)

    # 20 timers (PAGE 60, Table 4-2 PAGE 159)
    s = mk(); s.dec = 0; s.tb = 10
    n = timer_advance(s, 1)
    check("dec 0 -> FFFFFFFF latch", n == 1 and s.dec == 0xFFFFFFFF and s.tb == 11 and s.pending["decrementer"])
    s = mk(); s.dec = 5
    check("dec 5-5=0 no latch", timer_advance(s, 5) == 0 and s.dec == 0 and not s.pending["decrementer"])
    s = mk(); s.dec = 0x80000000
    check("dec MSB 1->0 no latch", timer_advance(s, 1) == 0 and s.dec == 0x7FFFFFFF)
    s = mk(); s.dec = 0xFFFFFFFF
    check("dec already negative no latch", timer_advance(s, 0x7FFFFFFF) == 0 and s.dec == 0x80000000)
    s = mk(); s.dec = 3; s.tb = MASK64
    n = timer_advance(s, 3 + 1 + (1 << 32))
    check("multiple crossings single latch, tb wraps", n == 2 and s.pending["decrementer"]
          and s.tb == 3 + (1 << 32) and s.dec == 0xFFFFFFFF)
    s = mk(); s.dec = 0; s.tb = 9; s.checkstop = True
    check("checkstop freezes TB/DEC (PAGE 173)", timer_advance(s, 5) == 0 and s.dec == 0 and s.tb == 9
          and not s.pending["decrementer"])
    s = mk(); s.dec = 100
    check("bus clocks /4",advance_bus_clocks(s, 8) == 0 and s.dec == 98 and s.tb == 2)
    check("bus clocks non-multiple raises", raises(advance_bus_clocks, s, 7))
    s = mk(); s.dec = 1
    check("write_dec MSB 0->1 raises", raises(write_dec, s, 0x80000000))
    s = mk(); s.dec = 0xFFFFFFF0; s.pending["decrementer"] = True
    check("write_dec with latch raises", raises(write_dec, s, 0x100))
    s = mk(); s.dec = 0xFFFFFFF0
    write_dec(s, 0x80000001)
    check("write_dec MSB stays 1 ok", s.dec == 0x80000001 and not s.pending["decrementer"])

    # 21 dec latch survives many EE=0 steps, taken when EE set by rfi
    s = mk(0x2032); s.dec = 0
    timer_advance(s, 1)
    s.gpr[1] = 1
    for _ in range(3):
        step(s, enc_x31(4, 1, 2, 4))    # tweq r1,r2: 1 != 0 no trap
        take_pending(s, partial=True)
    s.srr0 = 0x80002000; s.srr1 = 0xA032
    step(s, RFI)
    check("latched dec taken after rfi EE=1", take_pending(s, partial=True) == "decrementer"
          and s.srr0 == 0x80002000)

    # 22 synchronous entry helpers (partial)
    s = mk(0x2032)
    enter_dsi(s, 0x81234567, ["page_fault"], partial=True)
    check("dsi", s.exception == "dsi" and s.dar == 0x81234567 and s.dsisr == 0x40000000
          and s.srr0 == 0x80003100 and s.pc == 0x300 and s.spec_undefined["dsisr"] == 0xBFFFFFFF
          and s.spec_undefined["srr1"] == SRR1_CAUSE)   # CROSSCHECK FIX: PAGE 193 deferral
    s = mk(0x2032)
    check("dsi strict raises", raises(enter_dsi, s, 0x0, ["protection"]) and s.pc == 0x80003100)
    s = mk(0x2032)
    enter_isi(s, 0x80400000, "no_execute", partial=True)
    check("isi", s.srr0 == 0x80400000 and s.srr1 == (0x2032 | _m(3)) and s.pc == 0x400
          and s.spec_undefined["srr1"] == (SRR1_CAUSE & ~_m(3)))   # CROSSCHECK FIX: PAGE 193 deferral
    s = mk(0x2032)
    enter_alignment(s, 0x80000003, partial=True)
    check("alignment", s.dar == 0x80000003 and s.pc == 0x600 and s.srr0 == 0x80003100)
    s = mk(0x2032)
    enter_fp_unavailable(s, partial=True)
    check("fpu unavailable", s.pc == 0x800 and s.srr0 == 0x80003100 and s.msr == 0)
    s = mk(0x2072)
    enter_program(s, "fp_enabled", partial=True)
    check("program fp_enabled high vector", s.pc == 0xFFF00700 and s.msr == MSR_IP)

    passed = sum(1 for _, ok in results if ok)
    print(f"{passed}/{len(results)} vectors passed")
    raise SystemExit(0 if passed == len(results) else 1)
