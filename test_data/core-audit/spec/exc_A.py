"""Independent semantic model, author A, area "exc".

Exception and interrupt architecture of the IBM Gekko, derived ONLY from
docs/reference/ppc/gekko_user_manual.txt (cited "PAGE N" = the "===== PAGE N =====" marker)
with ppc_750cl.txt used as a cross-check (cited "750CL PAGE N").

Bit numbering is big-endian (bit 0 = MSB of a 32-bit value) exactly as in the manual.

Public surface
--------------
  AREA, supports(word), step(state, word), step_with_markers(state, word)
  take_pending(state), take_pending_with_markers(state)       -- between instructions
  exception entry helpers (each atomic: commit everything, or raise UndefinedBehaviour and
  touch nothing):
      take_hard_reset, take_system_reset, take_machine_check, take_dsi, take_isi,
      take_external, take_alignment, take_program, take_fp_unavailable, take_decrementer,
      take_syscall, take_trace, take_performance_monitor, take_iabr, take_thermal
  condition helpers: vector_address, trace_condition, iabr_match, fp_unavailable_condition,
      fp_enabled_exceptions_enabled, select_instruction_exception, select_async_exception
  timer helpers: advance_timebase, write_dec, bus_clocks_to_ticks

Instructions covered by step(): rfi, sc, tw, twi, mtmsr (mtmsr because the manual defines
its exception-recognition timing, PAGE 475), mfmsr (added by the exc crosscheck, PAGE 461).
step() first applies the IABR dispatch check when the state carries an `iabr` attribute
(Table 4-3 PAGE 162, PAGE 177).

Cross-check history: every "CROSSCHECK FIX" comment marks a change made while reconciling this
model with the independent model exc_B (crosscheck_exc.py, 2026-09-14); each cites the page that
decided it.

state.pending keys understood by take_pending (all optional):
  'hard_reset'  bool  HRESET/POR event                          (edge, consumed when taken)
  'machine_check' iterable of cause names from
                 {'dma','l2dp','mcp','tea','dp','ap'}            (events, consumed)
  'soft_reset'  bool  SRESET ("effectively edge-sensitive" PAGE 169, consumed)
  'external'    bool  level of the INT pin ("level-sensitive" PAGE 262, NOT consumed)
  'performance_monitor' bool  qualified PMC condition incl. MMCR0[ENINT] (caller-owned)
  'decrementer' bool  latched DEC 0->1 MSB transition (model-owned, consumed when taken)
  'thermal'     bool  qualified TAU condition incl. THRMn[TIE] (caller-owned)

state.exception names: 'hard_reset', 'system_reset', 'machine_check', 'checkstop', 'dsi',
  'isi', 'external', 'alignment', 'program' (illegal / privileged / FP-enabled), 'trap'
  (trap-type program exception, same vector 0x700), 'fp_unavailable', 'decrementer',
  'syscall', 'trace', 'performance_monitor', 'iabr', 'thermal'.
A checkstopped state carries state.checkstop = True (attribute added by this model).

Undefined behaviour is never invented: the operation raises UndefinedBehaviour WITHOUT
touching the state.  The exception carries .defined (a State with every defined part
applied; undefined bits hold a documented placeholder) and .undefined = {location: mask}
(location 'all' = boundedly undefined / invalid form; 'gpr' / 'fpr' = every element;
'pending.decrementer' = the latch).  apply_defined(state) copies the defined parts.

STRICT (module flag, default True): the manual gives SRR1[1-4,10-15] explicitly only for
system reset (Table 4-7), machine check (4-10), performance monitor (4-11), IABR (4-12) and
thermal (4-13), and bits 1/3/4 for ISI (Table 5-3).  For external, decrementer, DSI,
alignment, FP unavailable, sc and trace it defers to "The Programming Environments" manual,
which is NOT one of the permitted sources.  With STRICT these bits are reported undefined
(placeholder 0).  With STRICT = False they are defined as 0 "by analogy" with every table
the manual does give for this field (and with the sc pseudo-code "SRR1[1-4, 10-15] <- 0",
PAGE 537).  The program exception cause bits are undefined in BOTH modes (four causes share
vector 0x700 and the manual never gives their SRR1 encoding).
"""
from __future__ import annotations

import copy
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gekko_state import MASK32, MASK64, State  # noqa: E402

AREA = "exc"
STRICT = True
M = MASK32


# ======================================================================================
# Undefined-behaviour plumbing
# ======================================================================================
class UndefinedBehaviour(Exception):
    """Raised when the manual leaves (part of) the result undefined.

    what      : text including the PAGE citation(s)
    defined   : State with all defined parts applied (None for an invalid form)
    undefined : dict {location: mask}; 'all' means boundedly undefined
    """

    def __init__(self, what, defined=None, undefined=None):
        super().__init__(what)
        self.what = what
        self.defined = defined
        self.undefined = dict(undefined or {"all": M})

    def apply_defined(self, state):
        if self.defined is not None and "all" not in self.undefined:
            _commit(self.defined, state)


_FIELDS = ("gpr", "fpr", "cr", "xer", "lr", "ctr", "msr", "fpscr", "srr0", "srr1", "dar",
           "dsisr", "dec", "tb", "hid0", "hid2", "gqr", "sprg", "pc", "reserve",
           "reserve_addr", "exception", "pending")


def _work_copy(state):
    mem = state.mem
    state.mem = None
    try:
        c = copy.deepcopy(state)
    finally:
        state.mem = mem
    c.mem = mem
    if not hasattr(c, "checkstop"):
        c.checkstop = False
    return c


def _commit(src, dst):
    for k in _FIELDS:
        setattr(dst, k, copy.deepcopy(getattr(src, k)))
    dst.checkstop = getattr(src, "checkstop", False)
    # CROSSCHECK FIX (exc crosscheck 2026-09-14): IABR is an exception-area register (hard reset
    # zeroes it, Table 4-8 PAGE 170; step() consults it, PAGE 177).  It was not committed, so a
    # hard reset left a stale IABR in the caller's state.
    if hasattr(src, "iabr"):
        dst.iabr = getattr(src, "iabr") & M


class _Ctx:
    """Transaction: mutate a work copy, collect undefined masks, then commit or raise."""

    def __init__(self, state):
        self.orig = state
        self.st = _work_copy(state)
        self.undef = {}
        self.notes = []

    def mark(self, loc, mask, note):
        if mask:
            self.undef[loc] = self.undef.get(loc, 0) | mask
            if note not in self.notes:
                self.notes.append(note)

    def finish(self):
        if self.undef:
            raise UndefinedBehaviour("; ".join(self.notes), self.st, self.undef)
        _commit(self.st, self.orig)


def _invalid_form(what):
    # "If instructions are encoded with incorrectly set bits in reserved fields, the results on
    # execution can be said to be boundedly undefined." PAGE 87
    raise UndefinedBehaviour(what + " (boundedly undefined, PAGE 87)", None, {"all": M})


# ======================================================================================
# Bit helpers (big-endian numbering, PAGE 164 Figure 4-3 / Table 4-4)
# ======================================================================================
def B(n):
    return 1 << (31 - n)


def R(a, b):
    """Mask of big-endian bits a..b inclusive."""
    v = 0
    for n in range(a, b + 1):
        v |= B(n)
    return v


# MSR bits, Table 4-4 PAGE 164-165
POW = B(13)
ILE = B(15)
EE = B(16)
PR = B(17)
FP = B(18)
ME = B(19)
FE0 = B(20)
SE = B(21)
BE = B(22)
FE1 = B(23)
IP = B(25)
IR = B(26)
DR = B(27)
PM = B(29)
RI = B(30)
LE = B(31)
# Reserved: 0 (full), 1-4 (partial), 5-9 (full), 10-12 (partial), 14 (impl.-specific),
# 24 (AL), 28 (full)  -- Table 4-4 PAGE 164-165
MSR_RESERVED = B(0) | R(1, 4) | R(5, 9) | R(10, 12) | B(14) | B(24) | B(28)

# "SRR1[1-4, 10-15] are loaded with information specific to the exception type."
# "SRR1[5-9, 16-31] are loaded with a copy of the corresponding MSR bits." PAGE 166
# Bit 0 "Loaded with equivalent MSR bits" Table 4-7 PAGE 169, Table 4-11/4-12 PAGE 177,
# Table 4-13 PAGE 178.
SRR1_SPECIFIC = R(1, 4) | R(10, 15)
SRR1_COPY = B(0) | R(5, 9) | R(16, 31)

# Table 4-6 "MSR Setting Due to Exception" PAGE 168: POW EE PR FP FE0 SE BE FE1 IR DR PM RI
# cleared; ILE, IP "not altered"; ME not altered except machine check (cleared); LE <- ILE.
_ENTRY_CLEAR = POW | EE | PR | FP | FE0 | SE | BE | FE1 | IR | DR | PM | RI | LE

# Vector offsets, Table 4-2 PAGE 159 (+ thermal row PAGE 160)
VECTORS = {
    "system_reset": 0x00100, "hard_reset": 0x00100, "machine_check": 0x00200,
    "dsi": 0x00300, "isi": 0x00400, "external": 0x00500, "alignment": 0x00600,
    "program": 0x00700, "trap": 0x00700, "fp_unavailable": 0x00800,
    "decrementer": 0x00900, "syscall": 0x00C00, "trace": 0x00D00,
    "performance_monitor": 0x00F00, "iabr": 0x01300, "thermal": 0x01700,
}
# 0x00E00 floating-point assist: "not implemented in Gekko" PAGE 176 -> no entry.
# System management interrupt 0x01400 exists on the 750CL (750CL PAGE 159) but is absent
# from the Gekko Table 4-2 (PAGE 159-160) and Table 4-3 (PAGE 162) -> not modelled.


def vector_address(msr, name):
    # "If IP is cleared, exceptions are vectored to the physical address 0x000n_nnnn. If IP is
    # set, exceptions are vectored to the physical address 0xFFFn_nnnn." PAGE 166 step 5,
    # PAGE 165 (MSR[IP]).  The sc page says "base address determined by the new setting of
    # MSR[IP]" PAGE 537; IP is "not altered" on entry (Table 4-6 PAGE 168) so old == new.
    base = 0xFFF00000 if (msr & IP) else 0x00000000
    return (base | VECTORS[name]) & M


# ======================================================================================
# Generic exception entry (PAGE 166 "4.3.2 Steps for Exception Processing")
# ======================================================================================
def _enter(ctx, name, srr0, specific_bits=0, specific_undef=0, specific_note="",
           machine_check=False, syscall=False):
    st = ctx.st
    msr = st.msr & M
    res = msr & MSR_RESERVED
    # CROSSCHECK FIX (exc crosscheck 2026-09-14, disagreement class "SRR1 copy of reserved MSR
    # bits"): previously EVERY copied reserved bit (0, 5-9, 24, 28) was reported undefined.  The
    # Gekko-specific note resolves PAGE 166 step 3 ("Depending on the implementation, reserved
    # bits may not be copied") for the full-function bits: "Note: Full function reserved bits are
    # saved in SRR1 when an exception occurs; partial function reserved bits are not saved."
    # (Table 4-4 note, PAGE 165).  Bits 0, 5-9 and 28 are "Reserved. Full function" (PAGE 164/165)
    # -> copied, defined.  Bit 24 is only "Reserved. This bit corresponds to the AL bit of the
    # POWER architecture." (PAGE 164), neither full nor partial -> the PAGE 166 "may not be
    # copied" stays unresolved -> undefined.  For sc, the RTL "SRR1[0,5-9, 16-23, 25-27, 30-31]
    # <- MSR[...]" (PAGE 537) omits bit 28, contradicting the PAGE 165 note -> bit 28 undefined
    # for sc only.  (Partial-function bits 1-4/10-12 and bit 14 fall in the exception-specific
    # range SRR1[1-4,10-15] and are never copies.)
    if machine_check:
        # Table 4-10 PAGE 173: SRR1 "0-9 Cleared", 10-15 cause bits, "16-31 MSR[16-31]"
        srr1 = (msr & R(16, 31)) | (specific_bits & R(10, 15))
        res_copied = res & B(24)
    else:
        # step 2/3 PAGE 166
        srr1 = (msr & SRR1_COPY) | (specific_bits & SRR1_SPECIFIC)
        res_copied = res & (B(24) | (B(28) if syscall else 0))
    new = msr & ~_ENTRY_CLEAR
    if machine_check:
        new &= ~ME                      # Table 4-6 PAGE 168 / Table 4-10 PAGE 173: ME 0
    if msr & ILE:
        new |= LE                       # "LE Set to value of ILE" Table 4-6 PAGE 168
    if res:
        # Table 4-4 note PAGE 165: "Full function reserved bits are saved in SRR1 ... partial
        # function reserved bits are not saved" vs Table 4-6 note PAGE 168: "Reserved bits are
        # read as if written as 0" and PAGE 166 step 3: "Depending on the implementation,
        # reserved bits may not be copied."
        ctx.mark("msr", res, "MSR reserved bits set at exception entry: new MSR value of "
                             "those bits undefined (PAGE 165, PAGE 168)")
        ctx.mark("srr1", res_copied, "MSR reserved bit 24 (or bit 28 for sc) set at exception "
                                     "entry: SRR1 copy undefined (PAGE 164, PAGE 166 step 3, "
                                     "PAGE 537)")
    ctx.mark("srr1", specific_undef & (R(10, 15) if machine_check else SRR1_SPECIFIC),
             specific_note)
    st.srr0 = srr0 & M
    st.srr1 = srr1 & M
    st.msr = new & M
    st.pc = vector_address(new, name)   # PAGE 166 step 5
    st.exception = name


def _unspecified_specific(ctx_note_exc, page):
    """SRR1[1-4,10-15] for exceptions whose register settings the manual defers to the
    Programming Environments manual.  Returns (undefined_mask, note)."""
    if STRICT:
        return SRR1_SPECIFIC, ("SRR1[1-4,10-15] for %s: manual defers register settings to "
                               "The Programming Environments manual (%s)" % (ctx_note_exc, page))
    return 0, ""


# --------------------------------------------------------------------------------------
# Individual exception entry, each on a context (internal) + atomic public wrapper
# --------------------------------------------------------------------------------------
def _hard_reset(ctx):
    st = ctx.st
    # "Gekko immediately branches to 0xFFF0_0100 without attempting to reach a recoverable
    # state" PAGE 170; Table 4-8 "Settings Caused by Hard Reset" PAGE 170-171
    st.gpr = [0] * 32
    ctx.mark("gpr", M, "hard reset: GPRs Unknown (Table 4-8 PAGE 170)")
    st.fpr = [[0, 0] for _ in range(32)]
    ctx.mark("fpr", MASK64, "hard reset: FPRs Unknown (Table 4-8 PAGE 170)")
    st.fpscr = 0            # FPSCR 00000000
    st.cr = 0               # CR All 0s
    st.msr = 0x00000040     # MSR 00000040 (only IP set)
    st.xer = 0              # XER 00000000
    st.hid0 = 0             # HID0 00000000
    st.hid2 = 0             # HID2 00000000
    st.gqr = [0] * 8        # GQRn 00000000
    st.tb = 0               # TBU/TBL 00000000 (PAGE 171)
    st.lr = 0
    st.ctr = 0
    st.srr0 = 0
    st.srr1 = 0             # Gekko: SRR1 00000000 (PAGE 171); 750CL PAGE 170 says 00000040
    st.sprg = [0] * 4       # SPRGs 00000000
    st.dsisr = 0
    st.dar = 0
    st.dec = 0xFFFFFFFF     # DEC FFFFFFFF (PAGE 171; also 750CL PAGE 180 "Timer Facilities")
    st.reserve = False      # "Reservation Address Unknown (reservation flag cleared)" PAGE 171
    st.reserve_addr = 0
    ctx.mark("reserve_addr", M, "hard reset: reservation address Unknown (PAGE 171)")
    # CROSSCHECK FIX (exc crosscheck 2026-09-14): "IABR All 0s (break point disabled)"
    # Table 4-8 PAGE 170 was not applied.
    st.iabr = 0
    st.pc = 0xFFF00100
    st.exception = "hard_reset"
    # "Once Gekko enters a checkstop state, only a hard reset can clear the processor from the
    # checkstop state." PAGE 302
    st.checkstop = False
    # "all operations cease, and the machine state is lost" / "internal logic to be reset"
    # PAGE 170 -> the model-owned decrementer latch is reset with DEC; pin levels
    # (external / PM / thermal) are system-owned and left as the caller set them.
    st.pending["decrementer"] = False
    st.pending["hard_reset"] = False
    # "SRESET asserted coincident to, or after the assertion of, HRESET will also have no
    # effect on the operation or state of the machine." PAGE 169
    st.pending["soft_reset"] = False
    st.pending["machine_check"] = ()


def take_hard_reset(state):
    ctx = _Ctx(state)
    _hard_reset(ctx)
    ctx.finish()
    return "hard_reset"


def _system_reset(ctx):
    st = ctx.st
    # Table 4-7 PAGE 169: SRR0 "the instruction that the processor would have attempted to
    # execute next"; SRR1 0/5-9/16-31 MSR, 1-4 and 10-15 Cleared; MSR per table (ME, IP, ILE
    # unchanged; LE <- ILE); vector "either 0x0000_0100 or 0xFFF0_0100" PAGE 169.
    _enter(ctx, "system_reset", st.pc, specific_bits=0)
    st.pending["soft_reset"] = False


def take_system_reset(state):
    ctx = _Ctx(state)
    _system_reset(ctx)
    ctx.finish()
    return "system_reset"


# Table 4-10 PAGE 173 / Table 2-2 PAGE 60
MC_CAUSE_BITS = {"dma": B(10), "l2dp": B(11), "mcp": B(12), "tea": B(13), "dp": B(14),
                 "ap": B(15)}
HID0_EMCP = B(0)   # Table 4-9 PAGE 172
HID0_EBA = B(2)
HID0_EBD = B(3)


def _machine_check(ctx, causes):
    """Returns 'machine_check', 'checkstop' or None (all causes masked by HID0)."""
    st = ctx.st
    qualified = []
    for c in causes:
        if c not in MC_CAUSE_BITS:
            raise ValueError("unknown machine check cause %r" % (c,))
        if c == "mcp" and not (st.hid0 & HID0_EMCP):
            # "0 Masks MCP. Asserting MCP does not generate a machine check exception or a
            # checkstop." Table 4-9 PAGE 172; "If HID0[EMCP] is cleared, Gekko ignores the
            # interrupt condition." PAGE 262
            continue
        if c == "ap" and not (st.hid0 & HID0_EBA):
            continue    # "0 Prevents address parity checking." PAGE 172
        if c == "dp" and not (st.hid0 & HID0_EBD):
            continue    # "0 Parity checking is disabled." PAGE 172
        # tea / l2dp / dma: enable qualification lives outside HID0[0-3] (L2CR, HID2) and is
        # the caller's responsibility (PAGE 172 "Certain machine check conditions ...").
        qualified.append(c)
    # MCP is "negative edge-sensitive" PAGE 262; TEA "asserted for one cycle only" PAGE 261:
    # the causes are events and are consumed whether taken, checkstopped, or masked.
    st.pending["machine_check"] = ()
    if not qualified:
        return None
    if not (st.msr & ME):
        # "If MSR[ME] = 0 and a machine check occurs, the processor enters the checkstop
        # state." PAGE 173; "instruction processing is suspended ... The contents of all
        # latches are frozen" PAGE 173
        st.checkstop = True
        st.exception = "checkstop"
        return "checkstop"
    bits = 0
    for c in qualified:
        bits |= MC_CAUSE_BITS[c]
    # SRR0: "On a best-effort basis Gekko can set this to an EA of some instruction that was
    # executing or about to be executing" Table 4-10 PAGE 173 -> undefined; placeholder = pc.
    _enter(ctx, "machine_check", st.pc, specific_bits=bits, machine_check=True)
    ctx.mark("srr0", M, "machine check SRR0 is best-effort (Table 4-10 PAGE 173)")
    return "machine_check"


def take_machine_check(state, causes):
    ctx = _Ctx(state)
    r = _machine_check(ctx, causes)
    ctx.finish()
    return r


# DSISR bits named by the manual: Table 5-3 PAGE 192, Table 5-4 PAGE 193, Table 4-3 PAGE 162
DSI_CAUSE_DSISR = {"page_fault": B(1), "protection": B(4), "direct_store": B(5),
                   "eciwx": B(11)}
# ISI SRR1 bits: Table 5-3 PAGE 192, Table 5-4 PAGE 193
ISI_CAUSE_SRR1 = {"page_fault": B(1), "protection": B(4), "no_execute": B(3),
                  "direct_store": B(3), "guarded": B(3)}


def _dsi(ctx, ea, causes):
    st = ctx.st
    # SRR0: instruction-caused, precise: "the address of the faulting instruction is provided
    # to the exception handler" PAGE 46; "taken without completing the exception-causing
    # instruction" PAGE 161.  state.pc = address of the faulting instruction.
    mask, note = _unspecified_specific("DSI", "PAGE 174")
    srr0 = st.pc
    _enter(ctx, "dsi", srr0, 0, mask, note)
    # "After a DSI or an alignment exception, DAR is set to the effective address (EA)
    # generated by the faulting instruction." PAGE 60
    st.dar = ea & M
    bits = 0
    for c in causes:
        bits |= DSI_CAUSE_DSISR[c]
    st.dsisr = bits
    # only the cause bits themselves are given; every other DSISR bit is PEM-defined
    ctx.mark("dsisr", M & ~bits, "DSISR bits other than the named cause bits are not given "
                                 "by the manual (PAGE 192-193, PAGE 60)")


def take_dsi(state, ea, causes):
    ctx = _Ctx(state)
    _dsi(ctx, ea, causes)
    ctx.finish()
    return "dsi"


def _isi(ctx, cause):
    st = ctx.st
    bits = ISI_CAUSE_SRR1[cause]
    # "an attempt to fetch the next instruction fails" PAGE 174; "The state saved by the
    # processor for each of these exceptions contains information that identifies the address
    # of the failing instruction." PAGE 193.  state.pc = address whose fetch failed.
    if STRICT:
        und = SRR1_SPECIFIC & ~bits
        note = ("ISI SRR1[1-4,10-15] other than the Table 5-3 cause bit are not given "
                "(PAGE 192-193)")
    else:
        und, note = 0, ""
    _enter(ctx, "isi", st.pc, bits, und, note)


def take_isi(state, cause):
    ctx = _Ctx(state)
    _isi(ctx, cause)
    ctx.finish()
    return "isi"


def _async_next(ctx, name, specific_defined, page):
    st = ctx.st
    # SRR0 = "the effective address of the instruction that the processor would have
    # attempted to execute next if no exception conditions were present" Table 4-11 PAGE 177,
    # Table 4-13 PAGE 178; rfi PAGE 532 "the address of the instruction that would have been
    # executed next had the exception not occurred".  Between instructions that is state.pc.
    if specific_defined:
        _enter(ctx, name, st.pc, 0, 0, "")
    else:
        mask, note = _unspecified_specific(name, page)
        _enter(ctx, name, st.pc, 0, mask, note)


def take_external(state):
    ctx = _Ctx(state)
    _async_next(ctx, "external", False, "PAGE 174")
    ctx.finish()
    return "external"


def take_decrementer(state):
    ctx = _Ctx(state)
    _async_next(ctx, "decrementer", False, "PAGE 176")
    ctx.st.pending["decrementer"] = False
    ctx.finish()
    return "decrementer"


def take_performance_monitor(state):
    ctx = _Ctx(state)
    _async_next(ctx, "performance_monitor", True, "Table 4-11 PAGE 177")
    ctx.finish()
    return "performance_monitor"


def take_thermal(state):
    ctx = _Ctx(state)
    _async_next(ctx, "thermal", True, "Table 4-13 PAGE 178")
    ctx.finish()
    return "thermal"


def _alignment(ctx, ea):
    st = ctx.st
    mask, note = _unspecified_specific("alignment", "PAGE 175")
    _enter(ctx, "alignment", st.pc, 0, mask, note)      # faulting instruction, PAGE 46
    st.dar = ea & M                                       # PAGE 60
    st.dsisr = 0
    ctx.mark("dsisr", M, "alignment DSISR contents are not given by the manual (PAGE 60, "
                         "PAGE 175)")


def take_alignment(state, ea):
    ctx = _Ctx(state)
    _alignment(ctx, ea)
    ctx.finish()
    return "alignment"


PROGRAM_KINDS = ("illegal", "privileged", "trap", "fp_enabled")


def _program(ctx, kind):
    st = ctx.st
    if kind not in PROGRAM_KINDS:
        raise ValueError(kind)
    if kind == "trap":
        # "This may be the address of the instruction that caused the exception or the next
        # one (as in the case of a system call, trace, or trap exception)." PAGE 163
        # (identical 750CL PAGE 162); "(excluding the trap and system call exceptions) the
        # address of the faulting instruction is provided" PAGE 46.
        srr0 = st.pc + 4
        name = "trap"
    else:
        # illegal / privileged / FP-enabled: faulting instruction, PAGE 46, PAGE 161; FP
        # enabled exceptions "are treated as precise" PAGE 175.
        srr0 = st.pc
        name = "program"
    _enter(ctx, name, srr0, 0, SRR1_SPECIFIC,
           "program exception SRR1 cause bits are never given by the manual (PAGE 175 defers "
           "to The Programming Environments manual)")
    if kind == "trap":
        # CROSSCHECK FIX (exc crosscheck 2026-09-14, disagreement class "trap SRR0"): SRR0 was
        # defined as CIA+4.  The only statements are two overview sentences - "This may be the
        # address of the instruction that caused the exception or the next one (as in the case
        # of a system call, trace, or trap exception)" PAGE 163 and "(excluding the trap and
        # system call exceptions) the address of the faulting instruction is provided" PAGE 46 -
        # while the program-exception section itself states "Chapter 6, 'Exceptions' in the
        # PowerPC Microprocessor Family: The Programming Environments manual describes register
        # settings for this exception." PAGE 175, and tw/twi only say "the system trap handler
        # is invoked" PAGE 578-579.  Unlike sc (explicit RTL "SRR0 <-iea CIA + 4" PAGE 537) no
        # register-level statement exists, and trap shares the precise dispatch/execution class
        # that is "taken without completing the exception-causing instruction" PAGE 161.
        # Ambiguous -> SRR0 undefined, placeholder CIA+4.
        ctx.mark("srr0", M, "trap SRR0: overview text says 'the next one' (PAGE 163, PAGE 46) "
                            "but register settings are deferred to the PEM (PAGE 175)")


def take_program(state, kind):
    ctx = _Ctx(state)
    _program(ctx, kind)
    ctx.finish()
    return ctx.orig.exception


def _fp_unavailable(ctx):
    st = ctx.st
    mask, note = _unspecified_specific("FP unavailable", "PAGE 175")
    _enter(ctx, "fp_unavailable", st.pc, 0, mask, note)  # faulting instruction, PAGE 46


def take_fp_unavailable(state):
    ctx = _Ctx(state)
    _fp_unavailable(ctx)
    ctx.finish()
    return "fp_unavailable"


def _syscall(ctx):
    st = ctx.st
    msr = st.msr
    # sc, PAGE 537:
    #   SRR0 <-iea CIA + 4
    #   SRR1[1-4, 10-15] <- 0   ... prose: "bits 1-4 and 10-15 of SRR1 are set to undefined
    #                               values"
    #   SRR1[0,5-9,16-23,25-27,30-31] <- MSR[same]
    #   NIA <- base_ea + 0xC00
    if STRICT:
        mask = SRR1_SPECIFIC
        note = ("sc: 'bits 1-4 and 10-15 of SRR1 are set to undefined values' (PAGE 537; the "
                "pseudo-code on the same page writes 0)")
    else:
        mask, note = 0, ""
    _enter(ctx, "syscall", st.pc + 4, 0, mask, note, syscall=True)
    # CROSSCHECK FIX (exc crosscheck 2026-09-14, disagreement class "sc SRR1[PM]"): SRR1[PM] was
    # reported undefined.  The sc NOTE only says the implementation decides: "An implementation
    # may define additional MSR bits, and in this case, may also cause them to be saved to SRR1
    # from MSR on an exception" PAGE 537.  Gekko defines PM as such a bit ("Gekko-specific;
    # defined as reserved by the PowerPC architecture" PAGE 165) and its exception-processing
    # steps decide it for every exception: "SRR1[5-9, 16-31] are loaded with a copy of the
    # corresponding MSR bits." PAGE 166 step 3 (PM is not a reserved bit on Gekko, so the "may
    # not be copied" clause does not apply).  -> SRR1[29] = MSR[29], defined.
    del msr


def take_syscall(state):
    ctx = _Ctx(state)
    _syscall(ctx)
    ctx.finish()
    return "syscall"


def _trace(ctx, next_pc):
    # "Trace exceptions are generated following execution and completion of an instruction"
    # PAGE 161; SRR0 is "the next one (as in the case of a system call, trace, or trap
    # exception)" PAGE 163.  next_pc = address the completed instruction transfers control to.
    mask, note = _unspecified_specific("trace", "PAGE 176")
    _enter(ctx, "trace", next_pc, 0, mask, note)


def take_trace(state, next_pc):
    ctx = _Ctx(state)
    _trace(ctx, next_pc)
    ctx.finish()
    return "trace"


def _iabr(ctx):
    st = ctx.st
    # "The instruction that triggers the instruction address breakpoint exception is not
    # executed before the exception handler is invoked." PAGE 177; Table 4-12 PAGE 177-178:
    # SRR0 = instruction that would have been executed next = the matched instruction.
    _enter(ctx, "iabr", st.pc, 0, 0, "")


def take_iabr(state):
    ctx = _Ctx(state)
    _iabr(ctx)
    ctx.finish()
    return "iabr"


# ======================================================================================
# Condition helpers
# ======================================================================================
def trace_condition(msr_at_execution, kind):
    """kind: 'branch' (b/bc/bclr/bcctr, Table 2-46 PAGE 113), 'rfi', 'isync', 'sc', 'other'.
    Only for an instruction that 'caused no other exception' (PAGE 164)."""
    if msr_at_execution & SE:
        # "generates a single-step trace exception upon the successful execution of every
        # instruction except rfi, isync, and sc" PAGE 164; "does not take trace exceptions on
        # the isync instruction" PAGE 176
        if kind not in ("rfi", "isync", "sc"):
            return True
    if msr_at_execution & BE and kind == "branch":
        # "generates a branch type trace exception when a branch instruction executes
        # successfully" PAGE 164; "MSR[BE] = 1 and the currently completing instruction is a
        # branch" PAGE 176 (taken or not: both 'complete').
        return True
    return False


def iabr_match(state, iabr):
    # Table 2-3 PAGE 62: 0-29 address, 30 BE, 31 TE; Table 4-2 PAGE 159: "IABR[0-29] matches
    # EA[0-29] of the next instruction to complete, IABR[TE] matches MSR[IR], and IABR[BE] = 1"
    if not (iabr & B(30)):
        return False
    if bool(iabr & B(31)) != bool(state.msr & IR):
        return False
    return (iabr & ~3 & M) == (state.pc & ~3 & M)


def fp_unavailable_condition(msr):
    # MSR[FP] "0 The processor prevents dispatch of floating-point instructions, including
    # floating-point loads, stores, and moves." PAGE 164; PAGE 175
    return not (msr & FP)


def fp_enabled_exceptions_enabled(msr):
    # "IEEE floating-point enabled exceptions ... are ignored when both MSR[FE0] and MSR[FE1]
    # are cleared. If either bit is set, all IEEE enabled floating-point exceptions are taken"
    # PAGE 166; Table 4-5 PAGE 165-166
    return bool(msr & (FE0 | FE1))


# Table 4-3 PAGE 162 (+ trace row PAGE 163).  Fetch (ISI) precedes dispatch/execution
# exceptions, which precede the post-instruction trace.
INSTRUCTION_PRIORITY = (
    "isi",           # instruction fetch 0
    "iabr",          # dispatch/execution 0
    "program",       # 1 illegal / privileged / trap
    "syscall",       # 2
    "fp_unavailable",  # 3
    "program_fp",    # 4 floating-point enabled (lowest program)
    "dsi_ecx",       # 5 eciwx/ecowx with EAR[E] = 0
    "alignment",     # 6
    "dsi_bat",       # 7
    "dsi_segment",   # 8
    "dsi_tlb",       # 9
    "dsi_dabr",      # 10
    "trace",         # 11 post-instruction
)


def select_instruction_exception(conditions):
    """Highest-priority instruction-related exception among `conditions` (PAGE 161-163)."""
    conds = set(conditions)
    for name in INSTRUCTION_PRIORITY:
        if name in conds:
            return name
    return None


# Table 4-3 PAGE 162 asynchronous priorities (0 highest)
ASYNC_PRIORITY = ("hard_reset", "machine_check", "soft_reset", "external",
                  "performance_monitor", "decrementer", "thermal")
_MASKABLE = ("external", "performance_monitor", "decrementer", "thermal")


def select_async_exception(pending, msr):
    for name in ASYNC_PRIORITY:
        v = pending.get(name)
        if not v:
            continue
        if name in _MASKABLE and not (msr & EE):
            # "When MSR[EE] = 0, recognition of these exception conditions is delayed." PAGE 166
            continue
        return name
    return None


# ======================================================================================
# take_pending: applied between instructions
# ======================================================================================
def _take_pending(ctx):
    st = ctx.st
    p = st.pending
    if p.get("hard_reset"):
        # priority 0 "taken immediately regardless of other pending exceptions" PAGE 160
        _hard_reset(ctx)
        return "hard_reset"
    if getattr(st, "checkstop", False):
        # "instruction processing is suspended and generally cannot resume without the
        # processor being reset" PAGE 173; "only a hard reset can clear" PAGE 302
        st.exception = "checkstop"
        return "checkstop"
    if p.get("machine_check"):
        r = _machine_check(ctx, tuple(p["machine_check"]))
        if r is not None:
            return r
    if p.get("soft_reset"):
        # priority 2, nonmaskable ("System reset exceptions cannot be masked." PAGE 166)
        _system_reset(ctx)
        return "system_reset"
    if not (st.msr & EE):
        # PAGE 164 EE=0: "The processor delays recognition of external interrupts and
        # decrementer exception conditions"; PAGE 61 (NOTE): EE also masks PM and thermal.
        return None
    if p.get("smi"):
        # CROSSCHECK FIX (exc crosscheck 2026-09-14, disagreement class "pending smi"): the key
        # was silently ignored.  The manual names a system management interrupt as a Gekko
        # asynchronous maskable exception ("Setting MSR[EE] masks ... also the Gekko-specific
        # system management, performance monitor, and thermal management exceptions" PAGE 58;
        # Table 1-4 PAGE 47; PAGE 160-161) yet gives it no vector ("Reserved 01400-016FF"
        # Table 4-2 PAGE 159) and no row in Table 4-3 PAGE 162.  Masked by EE=0 (defined, above)
        # and below the nonmaskable classes (PAGE 160 item 4); once recognisable its vector and
        # its priority against external/PM/decrementer/thermal are not given -> control flow
        # undefined.
        raise UndefinedBehaviour("system management interrupt recognised: no Gekko vector "
                                 "(Table 4-2 PAGE 159) or priority (Table 4-3 PAGE 162)",
                                 None, {"all": M})
    if p.get("external"):
        _async_next(ctx, "external", False, "PAGE 174")
        # INT "is level-sensitive" and "Negation—Should not occur until interrupt is taken"
        # PAGE 262: the level is system-owned, NOT cleared here.
        return "external"
    if p.get("performance_monitor"):
        # "The priority of the performance monitor interrupt lies between the external
        # interrupt and the decrementer interrupt" PAGE 177
        _async_next(ctx, "performance_monitor", True, "Table 4-11 PAGE 177")
        return "performance_monitor"
    if p.get("decrementer"):
        _async_next(ctx, "decrementer", False, "PAGE 176")
        # the condition is a transition event (Table 4-2 PAGE 159 "changes from 0 to 1");
        # taking the exception consumes it.
        p["decrementer"] = False
        return "decrementer"
    if p.get("thermal"):
        _async_next(ctx, "thermal", True, "Table 4-13 PAGE 178")
        return "thermal"
    return None


def take_pending(state):
    """Recognise and take the highest-priority pending asynchronous exception at an
    instruction boundary.  Returns its name, 'checkstop', or None."""
    ctx = _Ctx(state)
    r = _take_pending(ctx)
    ctx.finish()
    return r


def take_pending_with_markers(state):
    try:
        r = take_pending(state)
        return r, {}
    except UndefinedBehaviour as u:
        u.apply_defined(state)
        return state.exception, u.undefined


# ======================================================================================
# Decrementer / time base
# ======================================================================================
DEC_TB_BUS_CLOCK_DIVISOR = 4


def bus_clocks_to_ticks(bus_clocks):
    # "the decrementer register is decremented and the time base is incremented at a speed
    # that is one-fourth the speed of the bus clock" PAGE 60; "decremented at one fourth the
    # bus clock rate" PAGE 176
    #
    # CROSSCHECK FIX (exc crosscheck 2026-09-14, disagreement class "bus clocks not a multiple
    # of 4"): this used to floor (7 -> 1).  The manual gives only the rate, never the phase of
    # the divide-by-four prescaler relative to an arbitrary starting bus clock, so 7 bus clocks
    # yield 1 or 2 ticks depending on that phase (flooring is not even additive: 7+7 -> 2 but
    # 14 -> 3).  Any window of 4k consecutive bus clocks contains exactly k prescaler edges, so
    # multiples of four are defined for every phase.
    if bus_clocks < 0:
        raise ValueError("bus_clocks")
    if bus_clocks % DEC_TB_BUS_CLOCK_DIVISOR:
        raise UndefinedBehaviour("DEC/TB prescaler phase not given for %d bus clocks (PAGE 60, "
                                 "PAGE 176)" % bus_clocks, None, {"all": M})
    return bus_clocks // DEC_TB_BUS_CLOCK_DIVISOR


def advance_timebase(state, ticks):
    """Advance TB and DEC together by `ticks` timer ticks.  Returns True when a decrementer
    exception condition arose (the latch is set in state.pending['decrementer'])."""
    if ticks < 0:
        raise ValueError("ticks")
    if getattr(state, "checkstop", False):
        # CROSSCHECK FIX (common-mode, found by manual review during the exc crosscheck
        # 2026-09-14; exc_B had the same omission): "When a processor is in checkstop state,
        # instruction processing is suspended ... The contents of all latches are frozen within
        # two cycles upon entering checkstop state." PAGE 173 -> TB, DEC and the decrementer
        # condition do not advance.
        return False
    # TB: "64-bit register" with TBU/TBL halves PAGE 41/PAGE 60, incremented (carry TBL->TBU)
    state.tb = (state.tb + ticks) & MASK64
    old = state.dec & M
    state.dec = (old - ticks) & M
    # Table 4-2 PAGE 159: "when the most-significant bit of the DEC register changes from 0
    # to 1"; Table 4-3 PAGE 162: "Decrementer passes through zero".  Counting down by one,
    # the only 0->1 MSB change is 0x00000000 -> 0xFFFFFFFF, which happens within `ticks`
    # steps iff ticks > old (unsigned); any ticks >= 2**32 necessarily crosses.
    fired = ticks > old
    if fired:
        # The condition stays pending while MSR[EE] = 0: "When MSR[EE] = 0, recognition of
        # these exception conditions is delayed" PAGE 166; mtmsr PAGE 475: "if MSR[EE] = 0
        # and an external or decrementer exception is pending, executing an mtmsr instruction
        # that sets MSR[EE] = 1 will cause the ... exception to be taken before the next
        # instruction is executed".
        state.pending["decrementer"] = True
    return fired


def write_dec(state, value):
    """DEC write (mtspr).  Raises UndefinedBehaviour where the manual is silent."""
    value &= M
    ctx = _Ctx(state)
    st = ctx.st
    old = st.dec & M
    st.dec = value
    if not (old & 0x80000000) and (value & 0x80000000):
        # Table 4-2 PAGE 159 ("MSB of the DEC register changes from 0 to 1") would fire on a
        # write; Table 4-3 PAGE 162 ("Decrementer passes through zero") would not.
        ctx.mark("pending.decrementer", 1, "DEC write changing MSB 0->1: whether it raises a "
                                           "decrementer condition (PAGE 159 vs PAGE 162)")
    if st.pending.get("decrementer"):
        ctx.mark("pending.decrementer", 1, "DEC write while a decrementer exception is "
                                           "pending: cancellation not described (PAGE 176)")
    ctx.finish()


# ======================================================================================
# Instructions: rfi, sc, tw, twi, mtmsr
# ======================================================================================
def _f(w, a, b):
    return (w >> (31 - b)) & ((1 << (b - a + 1)) - 1)


def _s32(v):
    v &= M
    return v - (1 << 32) if v & 0x80000000 else v


def _decode(word):
    word &= M
    op = word >> 26
    xo = (word >> 1) & 0x3FF
    if op == 19 and xo == 50:
        return "rfi"
    if op == 17:
        # CROSSCHECK FIX (exc crosscheck 2026-09-14, disagreement class "supports op 17 with
        # bit 30 = 0"): only bit-30-set words were claimed.  "The class is determined by
        # examining the primary opcode and the extended opcode, if any." PAGE 87 - opcode 17 has
        # no extended opcode, so every opcode-17 word is the sc instruction; a word that does not
        # match the fixed "17 00000 00000 000000000000000 1 0" layout (PAGE 537) is an invalid
        # form and is rejected as boundedly undefined in _exec_sc.
        return "sc"
    if op == 31 and xo == 4:
        return "tw"
    if op == 3:
        return "twi"
    if op == 31 and xo == 146:
        return "mtmsr"
    if op == 31 and xo == 83:
        # CROSSCHECK FIX (exc crosscheck 2026-09-14, coverage class "mfmsr"): added so the
        # reconciled model covers the MSR read that model B covers (PAGE 461).
        return "mfmsr"
    return None


def supports(word):
    return _decode(word) is not None


def mnemonic(word):
    return _decode(word)


# rfi, PAGE 532: MSR[0,5-9,16-23,25-27,30-31] <- SRR1[same]; MSR[13] <- 0;
# NIA <- SRR0[0-29] || 0b00
RFI_COPY = B(0) | R(5, 9) | R(16, 23) | R(25, 27) | R(30, 31)


def _exec_rfi(ctx, word):
    st = ctx.st
    # layout "19 00000 00000 00000 50 0" PAGE 532
    if word & (R(6, 20) | B(31)):
        _invalid_form("rfi with nonzero reserved bits")
    if st.msr & PR:
        # "This is a supervisor-level, context synchronizing instruction." PAGE 532; privileged
        # list PAGE 90
        _program(ctx, "privileged")
        return
    old = st.msr & M
    s1 = st.srr1 & M
    new = (old & ~RFI_COPY) | (s1 & RFI_COPY)
    new &= ~POW                                             # "MSR[13] <- b'0'" PAGE 532
    if (old ^ s1) & PM:
        # PM is a Gekko-defined bit (PAGE 61) outside the rfi list; "an implementation may
        # define additional MSR bits, and in this case, may also cause them to be ... restored
        # to MSR from SRR1 on an rfi" PAGE 532.  Placeholder: pseudo-code (not restored).
        ctx.mark("msr", PM, "rfi: restoration of MSR[PM] is 'may' (PAGE 532)")
    res = (old & MSR_RESERVED & ~RFI_COPY) | (s1 & MSR_RESERVED & RFI_COPY)
    ctx.mark("msr", res, "rfi with MSR/SRR1 reserved bits set: 'Reserved bits are read as if "
                         "written as 0' (PAGE 168) vs full-function reserved bits (PAGE 165)")
    st.msr = new & M
    st.pc = st.srr0 & ~3 & M
    st.exception = None
    # No trace: SE excludes rfi (PAGE 164); rfi is not in Table 2-46 branch list (PAGE 113).
    # A newly enabled pending exception is taken by take_pending() with SRR0 = this pc
    # ("the address of the instruction that would have been executed next" PAGE 532).


def _exec_sc(ctx, word):
    # layout "17 00000 00000 000000000000000 1 0" PAGE 537
    if word & (R(6, 29) | B(31)) or not (word & B(30)):
        _invalid_form("sc with nonzero reserved bits or bit 30 clear")
    _syscall(ctx)
    # no trace for sc (PAGE 164) - it takes an exception anyway (PAGE 161)


def _trap_hit(a, b, to):
    # tw PAGE 578 / twi PAGE 579: TO[0] lt, TO[1] gt, TO[2] eq, TO[3] <U, TO[4] >U;
    # TO[0] is instruction bit 6 (value 0b10000)
    sa, sb = _s32(a), _s32(b)
    ua, ub = a & M, b & M
    return ((sa < sb and to & 0x10) or (sa > sb and to & 0x08) or (sa == sb and to & 0x04)
            or (ua < ub and to & 0x02) or (ua > ub and to & 0x01))


def _after_completion(ctx, msr_at_execution, kind, next_pc):
    """Instruction completed without exception: advance pc and apply trace (PAGE 161)."""
    st = ctx.st
    st.pc = next_pc & M
    st.exception = None
    if trace_condition(msr_at_execution, kind):
        if iabr_match(st, getattr(st, "iabr", 0)):
            # CROSSCHECK FIX (exc crosscheck 2026-09-14, disagreement class "trace + IABR at the
            # same boundary"): the trace was taken unconditionally.  Gekko's own text orders
            # them only implicitly (trace is post-instruction, IABR is dispatch priority 0 of
            # the NEXT instruction, Table 4-3 PAGE 162-163; synchronous exceptions "are taken in
            # strict program order" PAGE 160), while the 750CL cross-check states the opposite
            # outcome explicitly: "If trace and instruction breakpoint conditions occur
            # simultaneously, instruction breakpoint takes priority over trace" (750CL PAGE 180)
            # and never says whether the trace is then discarded or re-signalled.  Which
            # exception the step ends in is therefore undefined.
            for loc in ("pc", "srr0", "srr1", "msr"):
                ctx.mark(loc, M, "trace and IABR at the same boundary (PAGE 162, 750CL PAGE 180)")
            ctx.mark("exception", 1, "trace and IABR at the same boundary (PAGE 162, "
                                     "750CL PAGE 180)")
            return
        _trace(ctx, next_pc)


def _exec_tw(ctx, word, immediate):
    st = ctx.st
    to = _f(word, 6, 10)
    a = st.gpr[_f(word, 11, 15)] & M
    if immediate:
        b = _f(word, 16, 31)
        b = (b - 0x10000 if b & 0x8000 else b) & M        # EXTS(SIMM) PAGE 579
    else:
        if word & B(31):
            _invalid_form("tw with bit 31 set")          # layout "31 TO A B 4 0" PAGE 578
        b = st.gpr[_f(word, 16, 20)] & M
    if _trap_hit(a, b, to):
        # "the system trap handler is invoked" PAGE 578-579 -> trap-type program exception
        _program(ctx, "trap")
        # trace "is forgotten for that instruction" PAGE 161
        return
    # "If the tested conditions are not met, instruction execution continues normally."
    # PAGE 113-114
    _after_completion(ctx, st.msr, "other", st.pc + 4)


def _exec_mtmsr(ctx, word):
    st = ctx.st
    # layout "31 S 00000 00000 146 0" PAGE 475
    if word & (R(11, 20) | B(31)):
        _invalid_form("mtmsr with nonzero reserved bits")
    if st.msr & PR:
        _program(ctx, "privileged")                      # "supervisor-level" PAGE 475
        return
    old = st.msr & M
    new = st.gpr[_f(word, 6, 10)] & M
    # "MSR <- (rS)" PAGE 475
    st.msr = new
    ctx.mark("msr", new & MSR_RESERVED, "mtmsr writing MSR reserved bits: 'Reserved bits are "
                                        "read as if written as 0' (PAGE 168) vs Table 4-4 "
                                        "full-function reserved bits (PAGE 165)")
    if (old ^ new) & (POW | LE):
        # CROSSCHECK FIX (exc crosscheck 2026-09-14, disagreement class "mtmsr altering POW/LE"):
        # this was treated as a plain MSR write.  "It is also an execution synchronizing
        # instruction except with respect to alterations to the POW and LE bits." PAGE 475 - for
        # exactly these two bits the manual withdraws the guarantee that "all preceding
        # instructions have completed execution and cannot cause an exception before the
        # instruction executes" (PAGE 90), deferring to a PEM section that is not a permitted
        # source; POW additionally lets "the processor ... enter a power-saving mode when
        # additional conditions are present" (Table 2-1 PAGE 58), i.e. stop fetching.  The
        # step's outcome (including whether execution continues at CIA+4) is undefined.
        _after_completion(ctx, old, "other", st.pc + 4)
        for loc in ("pc", "srr0", "srr1", "msr"):
            ctx.mark(loc, M, "mtmsr altering MSR[POW] or MSR[LE] (PAGE 475, PAGE 58)")
        ctx.mark("exception", 1, "mtmsr altering MSR[POW] or MSR[LE] (PAGE 475, PAGE 58)")
        return
    if (old ^ new) & SE:
        # mtmsr "is also an execution synchronizing instruction ... but does not ensure
        # subsequent instructions execute in the newly established environment" PAGE 90;
        # whether the SE value written by this very instruction governs its own trace is not
        # stated.  Placeholder: SE in effect when the instruction executed.
        _after_completion(ctx, old, "other", st.pc + 4)
        for loc in ("pc", "srr0", "srr1", "msr"):
            ctx.mark(loc, M, "mtmsr changing MSR[SE]: trace of the mtmsr itself not "
                             "described (PAGE 90, PAGE 164, PAGE 475)")
        ctx.mark("exception", 1, "mtmsr changing MSR[SE]: trace of the mtmsr itself not "
                                 "described (PAGE 90, PAGE 164, PAGE 475)")
        return
    # EE/RI "effective as soon as the instruction completes" PAGE 475 -> a pending external
    # or decrementer is taken by take_pending() before the next instruction.
    _after_completion(ctx, old, "other", st.pc + 4)


def _exec_mfmsr(ctx, word):
    # CROSSCHECK ADDITION (exc crosscheck 2026-09-14): mfmsr, PAGE 461.
    st = ctx.st
    # layout "31 D 00000 00000 83 0" PAGE 461
    if word & (R(11, 20) | B(31)):
        _invalid_form("mfmsr with nonzero reserved bits")
    if st.msr & PR:
        # "This is a supervisor-level instruction." PAGE 461; privileged list PAGE 90
        _program(ctx, "privileged")
        return
    msr = st.msr & M
    st.gpr[_f(word, 6, 10)] = msr                         # "rD <- MSR" PAGE 461
    _after_completion(ctx, msr, "other", st.pc + 4)


def step(state, word):
    """Execute the covered instruction `word` located at state.pc.  Atomic."""
    name = _decode(word)
    if name is None:
        raise ValueError("unsupported word 0x%08X" % (word & M))
    if getattr(state, "checkstop", False):
        state.exception = "checkstop"                    # PAGE 173
        return
    ctx = _Ctx(state)
    ctx.st.exception = None
    word &= M
    if iabr_match(ctx.st, getattr(ctx.st, "iabr", 0)):
        # CROSSCHECK FIX (exc crosscheck 2026-09-14, disagreement class "IABR before dispatch"):
        # step() ignored IABR.  "Instruction address breakpoint" is priority 0 of the
        # instruction dispatch/execution exceptions (Table 4-3 PAGE 162), above program
        # (illegal/privileged/trap) and system call; "The instruction that triggers the
        # instruction address breakpoint exception is not executed before the exception handler
        # is invoked." PAGE 177.  Checked before the reserved-field test because the matched
        # instruction is never executed (boundedly undefined results are "results on
        # execution", PAGE 87).
        _iabr(ctx)
        ctx.finish()
        return
    if name == "rfi":
        _exec_rfi(ctx, word)
    elif name == "sc":
        _exec_sc(ctx, word)
    elif name == "tw":
        _exec_tw(ctx, word, False)
    elif name == "twi":
        _exec_tw(ctx, word, True)
    elif name == "mtmsr":
        _exec_mtmsr(ctx, word)
    elif name == "mfmsr":
        _exec_mfmsr(ctx, word)
    ctx.finish()


def step_with_markers(state, word):
    """Apply the defined parts; return {location: undefined mask} ({} when fully defined)."""
    try:
        step(state, word)
        return {}
    except UndefinedBehaviour as u:
        u.apply_defined(state)
        return u.undefined


# ======================================================================================
# Self-test: hand-derived vectors
# ======================================================================================
if __name__ == "__main__":
    import traceback

    RESULTS = []

    def vec(title):
        def deco(fn):
            RESULTS.append((title, fn))
            return fn
        return deco

    def mk(msr=0x00002032, pc=0x80003100):
        s = State()
        s.msr = msr
        s.pc = pc
        return s

    def expect_ub(fn, *a):
        try:
            fn(*a)
        except UndefinedBehaviour as u:
            return u
        raise AssertionError("expected UndefinedBehaviour")

    def nonstrict(fn):
        def run():
            global STRICT
            saved = STRICT
            STRICT = False
            try:
                fn()
            finally:
                STRICT = saved
        return run

    # ---- vector addresses --------------------------------------------------------------
    @vec("V01 vector external IP=0 -> 0x00000500 (PAGE 159, PAGE 166)")
    def _():
        assert vector_address(0, "external") == 0x500

    @vec("V02 vector every exception with IP=1 -> 0xFFF0_nnnn (PAGE 165)")
    def _():
        assert vector_address(IP, "external") == 0xFFF00500
        assert vector_address(IP, "iabr") == 0xFFF01300
        assert vector_address(IP, "thermal") == 0xFFF01700
        assert vector_address(0, "syscall") == 0x00000C00
        assert vector_address(0, "performance_monitor") == 0x00000F00

    # ---- external interrupt ------------------------------------------------------------
    @vec("V03 external, EE=1, non-strict: SRR0=pc SRR1=MSR new MSR=0 pc=0x500")
    @nonstrict
    def _():
        s = mk(msr=0x0000A032, pc=0x80001234)
        s.pending["external"] = True
        assert take_pending(s) == "external"
        assert (s.srr0, s.srr1, s.msr, s.pc) == (0x80001234, 0x0000A032, 0, 0x500)
        assert s.exception == "external" and s.pending["external"] is True  # level, PAGE 262

    @vec("V04 external strict: UB only on SRR1[1-4,10-15]; state untouched")
    def _():
        s = mk(msr=0x0000A032, pc=0x80001234)
        s.pending["external"] = True
        before = s.arch_dict()
        u = expect_ub(take_pending, s)
        assert s.arch_dict() == before
        assert u.undefined == {"srr1": SRR1_SPECIFIC}
        assert u.defined.pc == 0x500 and u.defined.srr1 == 0x0000A032

    @vec("V05 external with EE=0 is not recognised")
    def _():
        s = mk(msr=0x00002032)
        s.pending["external"] = True
        before = s.arch_dict()
        assert take_pending(s) is None and s.arch_dict() == before

    @vec("V06 external with IP=1: pc 0xFFF00500, IP kept in new MSR")
    @nonstrict
    def _():
        s = mk(msr=0x0000A072)
        s.pending["external"] = True
        take_pending(s)
        assert s.pc == 0xFFF00500 and s.msr == 0x40 and s.srr1 == 0xA072

    @vec("V07 ILE=1: LE set on entry, SRR1[15] NOT a copy of ILE")
    @nonstrict
    def _():
        s = mk(msr=0x0001A032)
        s.pending["external"] = True
        take_pending(s)
        assert s.msr == (ILE | LE) and s.srr1 == 0x0000A032

    @vec("V08 ME preserved on non-machine-check entry; POW/PM/SE/BE/FE cleared")
    @nonstrict
    def _():
        s = mk(msr=POW | EE | ME | FE0 | SE | BE | FE1 | PM | RI | LE)
        s.pending["external"] = True
        take_pending(s)
        assert s.msr == ME
        # SRR1 copies bits 16-31 only: POW (bit 13) is not copied
        assert s.srr1 == (EE | ME | FE0 | SE | BE | FE1 | PM | RI | LE)

    # ---- decrementer / time base ---------------------------------------------------------
    @vec("V09 DEC 0 -1 tick-> FFFFFFFF fires; TB+1")
    def _():
        s = mk()
        s.dec, s.tb = 0, 7
        assert advance_timebase(s, 1) is True
        assert s.dec == 0xFFFFFFFF and s.tb == 8 and s.pending["decrementer"]

    @vec("V10 DEC 1 -1 tick-> 0 does not fire")
    def _():
        s = mk()
        s.dec = 1
        assert advance_timebase(s, 1) is False and s.dec == 0 and not s.pending["decrementer"]

    @vec("V11 DEC 0x80000000 -> 0x7FFFFFFF (MSB 1->0) does not fire")
    def _():
        s = mk()
        s.dec = 0x80000000
        assert advance_timebase(s, 1) is False and s.dec == 0x7FFFFFFF

    @vec("V12 DEC 5 -6 ticks-> FFFFFFFF fires; DEC 5 -5-> 0 does not")
    def _():
        s = mk()
        s.dec = 5
        assert advance_timebase(s, 6) is True and s.dec == 0xFFFFFFFF
        s2 = mk()
        s2.dec = 5
        assert advance_timebase(s2, 5) is False and s2.dec == 0

    @vec("V13 DEC FFFFFFFF -> FFFFFFFE (MSB stays 1) does not fire")
    def _():
        s = mk()
        s.dec = 0xFFFFFFFF
        assert advance_timebase(s, 1) is False and s.dec == 0xFFFFFFFE

    @vec("V14 TB carry TBL->TBU and 64-bit wrap")
    def _():
        s = mk()
        s.tb = 0x00000000FFFFFFFF
        advance_timebase(s, 1)
        assert s.tb == 0x0000000100000000
        s.tb = 0xFFFFFFFFFFFFFFFF
        advance_timebase(s, 2)
        assert s.tb == 1

    @vec("V15 bus clocks / 4 (PAGE 60); non-multiple of 4 -> prescaler phase undefined")
    def _():
        assert bus_clocks_to_ticks(8) == 2 and bus_clocks_to_ticks(0) == 0
        assert "all" in expect_ub(bus_clocks_to_ticks, 7).undefined

    @vec("V15b checkstop freezes TB/DEC (PAGE 173)")
    def _():
        s = mk()
        s.checkstop = True
        s.dec, s.tb = 0, 9
        assert advance_timebase(s, 5) is False
        assert (s.dec, s.tb, s.pending["decrementer"]) == (0, 9, False)

    @vec("V16 decrementer pending while EE=0 stays pending")
    def _():
        s = mk(msr=0x2032)
        s.dec = 0
        advance_timebase(s, 3)
        assert take_pending(s) is None and s.pending["decrementer"] is True

    @vec("V17 mtmsr sets EE -> pending decrementer taken before next instr, SRR0=pc+4")
    @nonstrict
    def _():
        s = mk(msr=0x2032, pc=0x80000100)
        s.pending["decrementer"] = True
        s.gpr[5] = 0xA032
        step(s, 0x7CA00124)                       # mtmsr r5
        assert s.msr == 0xA032 and s.pc == 0x80000104
        assert take_pending(s) == "decrementer"
        assert (s.srr0, s.srr1, s.msr, s.pc) == (0x80000104, 0xA032, 0, 0x900)
        assert s.pending["decrementer"] is False

    @vec("V18 priority external > decrementer; decrementer stays pending")
    @nonstrict
    def _():
        s = mk(msr=0xA032)
        s.pending["external"] = True
        s.pending["decrementer"] = True
        assert take_pending(s) == "external" and s.pending["decrementer"] is True

    @vec("V19 priority performance monitor > decrementer > thermal (PAGE 162/177)")
    @nonstrict
    def _():
        s = mk(msr=0xA032)
        s.pending.update(performance_monitor=True, decrementer=True, thermal=True)
        assert take_pending(s) == "performance_monitor" and s.pc == 0xF00
        s = mk(msr=0xA032)
        s.pending.update(decrementer=True, thermal=True)
        assert take_pending(s) == "decrementer"
        s = mk(msr=0xA032)
        s.pending.update(thermal=True)
        assert take_pending(s) == "thermal" and s.pc == 0x1700

    @vec("V20 PM / thermal SRR1 fully defined by Tables 4-11/4-13 even in STRICT")
    def _():
        s = mk(msr=0xA032, pc=0x80004000)
        s.pending["performance_monitor"] = True
        assert take_pending(s) == "performance_monitor"
        assert (s.srr0, s.srr1, s.msr) == (0x80004000, 0xA032, 0)

    @vec("V21 thermal/PM/decrementer masked by EE=0 (PAGE 61)")
    def _():
        s = mk(msr=0x2032)
        s.pending.update(performance_monitor=True, thermal=True, decrementer=True)
        assert take_pending(s) is None

    # ---- resets --------------------------------------------------------------------------
    @vec("V22 soft reset beats external and ignores EE=0; Table 4-7 fully defined")
    def _():
        s = mk(msr=0x00001032, pc=0x80002000)          # ME|IR|DR|RI, EE=0
        s.pending.update(soft_reset=True, external=True)
        assert take_pending(s) == "system_reset"
        assert (s.srr0, s.srr1, s.msr, s.pc) == (0x80002000, 0x1032, ME, 0x100)
        assert s.pending["soft_reset"] is False

    @vec("V23 soft reset with IP=1 -> 0xFFF00100")
    def _():
        s = mk(msr=0x40)
        s.pending["soft_reset"] = True
        take_pending(s)
        assert s.pc == 0xFFF00100 and s.msr == 0x40 and s.srr1 == 0x40

    @vec("V24 hard reset: Table 4-8, pc 0xFFF00100, GPR/FPR/reservation address undefined")
    def _():
        s = mk(msr=0xA032)
        s.gpr[3] = 99
        s.dec, s.tb, s.hid2, s.reserve = 5, 1234, 0xE0000000, True
        s.pending.update(hard_reset=True, soft_reset=True, external=True)
        u = expect_ub(take_pending, s)
        assert set(u.undefined) == {"gpr", "fpr", "reserve_addr"}
        d = u.defined
        assert (d.pc, d.msr, d.dec, d.tb, d.hid2, d.srr1, d.reserve) == \
            (0xFFF00100, 0x40, 0xFFFFFFFF, 0, 0, 0, False)
        assert d.pending["soft_reset"] is False and d.exception == "hard_reset"

    @vec("V25 hard reset clears checkstop (PAGE 302)")
    def _():
        s = mk()
        s.checkstop = True
        s.pending["hard_reset"] = True
        u = expect_ub(take_pending, s)
        assert u.defined.checkstop is False

    # ---- machine check -------------------------------------------------------------------
    @vec("V26 machine check TEA, ME=1: SRR1[13], SRR1[0-9]=0, ME cleared, SRR0 undefined")
    def _():
        s = mk(msr=0x0000B032)                           # EE|ME|FP|IR|DR|RI
        s.pending["machine_check"] = ("tea",)
        u = expect_ub(take_pending, s)
        assert u.undefined == {"srr0": M}
        d = u.defined
        assert d.srr1 == (0xB032 | B(13)) and d.msr == 0 and d.pc == 0x200
        assert d.pending["machine_check"] == ()

    @vec("V27 machine check with ME=0 -> checkstop, pc unchanged; step suspended")
    def _():
        s = mk(msr=0x2032, pc=0x80005000)
        s.pending["machine_check"] = ("tea",)
        assert take_pending(s) == "checkstop"
        assert s.checkstop and s.pc == 0x80005000 and s.msr == 0x2032
        step(s, 0x44000002)
        assert s.pc == 0x80005000 and s.exception == "checkstop"

    @vec("V28 MCP with HID0[EMCP]=0 ignored, lower priority external still taken")
    @nonstrict
    def _():
        s = mk(msr=0xB032)
        s.hid0 = 0
        s.pending.update(machine_check=("mcp",), external=True)
        assert take_pending(s) == "external" and s.pending["machine_check"] == ()

    @vec("V29 MCP with EMCP=1 -> SRR1[12]; AP needs EBA -> SRR1[15]; DP masked w/o EBD")
    def _():
        s = mk(msr=0x1032)
        s.hid0 = HID0_EMCP | HID0_EBA
        s.pending["machine_check"] = ("mcp", "ap", "dp")
        u = expect_ub(take_pending, s)
        assert u.defined.srr1 == (0x1032 | B(12) | B(15))

    # ---- sc ------------------------------------------------------------------------------
    @vec("V30 sc non-strict: SRR0=CIA+4, SRR1=MSR, MSR=0, pc=0xC00")
    @nonstrict
    def _():
        s = mk(msr=0x2032, pc=0x80003100)
        step(s, 0x44000002)
        assert (s.srr0, s.srr1, s.msr, s.pc, s.exception) == \
            (0x80003104, 0x2032, 0, 0xC00, "syscall")

    @vec("V31 sc strict: UB exactly SRR1[1-4,10-15] (PAGE 537 prose)")
    def _():
        s = mk()
        u = expect_ub(step, s, 0x44000002)
        assert u.undefined == {"srr1": SRR1_SPECIFIC} and u.defined.srr0 == 0x80003104

    @vec("V32 sc with ILE|IP|PR: new MSR = ILE|LE|IP, SRR1 keeps PR and IP")
    @nonstrict
    def _():
        s = mk(msr=ILE | IP | PR | 0x2032)
        step(s, 0x44000002)
        assert s.msr == (ILE | LE | IP) and s.pc == 0xFFF00C00
        assert s.srr1 == (IP | PR | 0x2032)

    @vec("V33 sc at 0xFFFFFFFC: SRR0 wraps to 0")
    @nonstrict
    def _():
        s = mk(pc=0xFFFFFFFC)
        step(s, 0x44000002)
        assert s.srr0 == 0

    @vec("V34 sc with reserved bit 31 set -> boundedly undefined")
    def _():
        u = expect_ub(step, mk(), 0x44000003)
        assert "all" in u.undefined

    @vec("V35 sc with MSR[PM]=1: SRR1[PM] copied (PAGE 166 step 3 decides the PAGE 537 'may')")
    @nonstrict
    def _():
        s = mk(msr=0x2032 | PM)
        step(s, 0x44000002)
        assert s.srr1 == (0x2032 | PM) and s.msr == 0

    @vec("V35b sc with MSR bit 28 set: SRR1[28] undefined (PAGE 537 RTL vs PAGE 165 note)")
    @nonstrict
    def _():
        u = expect_ub(step, mk(msr=0x2032 | B(28)), 0x44000002)
        assert u.undefined == {"msr": B(28), "srr1": B(28)}
        s = mk(msr=0xA032 | B(28))
        s.pending["external"] = True
        u = expect_ub(take_pending, s)
        assert u.undefined == {"msr": B(28)} and u.defined.srr1 == (0xA032 | B(28))

    @vec("V35c sc word with bit 30 clear: supported, boundedly undefined (PAGE 87, PAGE 537)")
    def _():
        assert supports(0x44000000)
        assert "all" in expect_ub(step, mk(), 0x44000000).undefined

    # ---- rfi -----------------------------------------------------------------------------
    @vec("V36 rfi: pc = SRR0 & ~3, MSR <- SRR1")
    def _():
        s = mk(msr=0x00000000, pc=0x00000C40)
        s.srr0, s.srr1 = 0x80001237, 0x0000A032
        step(s, 0x4C000064)
        assert s.pc == 0x80001234 and s.msr == 0xA032 and s.exception is None

    @vec("V37 rfi does not restore ILE (bit 15), restores LE, clears POW")
    def _():
        s = mk(msr=ILE | LE | POW)
        s.srr0, s.srr1 = 0x100, 0x00042032          # POW set in SRR1 too
        step(s, 0x4C000064)
        assert s.msr == (ILE | 0x2032)

    @vec("V38 rfi in user mode -> privileged program, SRR0 = rfi address")
    def _():
        s = mk(msr=PR | 0x2032, pc=0x80007000)
        s.srr0, s.srr1 = 0x1234, 0x5678
        u = expect_ub(step, s, 0x4C000064)
        d = u.defined
        assert u.undefined == {"srr1": SRR1_SPECIFIC}
        assert (d.srr0, d.pc, d.msr, d.exception) == (0x80007000, 0x700, 0, "program")
        assert d.srr1 == (PR | 0x2032)

    @vec("V39 rfi enabling EE with external pending: taken with SRR0 = rfi target")
    @nonstrict
    def _():
        s = mk(msr=0)
        s.srr0, s.srr1 = 0x80009000, 0xA032
        s.pending["external"] = True
        step(s, 0x4C000064)
        assert take_pending(s) == "external"
        assert s.srr0 == 0x80009000 and s.srr1 == 0xA032 and s.pc == 0x500

    @vec("V40 rfi with SRR1[PM] != MSR[PM] -> UB on MSR[PM] only")
    def _():
        s = mk(msr=0)
        s.srr0, s.srr1 = 0x100, PM | 0x2032
        u = expect_ub(step, s, 0x4C000064)
        assert u.undefined == {"msr": PM} and u.defined.msr == 0x2032

    @vec("V41 rfi with reserved bit 20 of word set -> boundedly undefined")
    def _():
        assert "all" in expect_ub(step, mk(msr=0), 0x4C000864).undefined

    @vec("V42 rfi with SE=1 takes no trace (PAGE 164)")
    def _():
        s = mk(msr=SE)
        s.srr0, s.srr1 = 0x2000, 0x2032
        step(s, 0x4C000064)
        assert s.pc == 0x2000 and s.exception is None

    # ---- tw / twi -----------------------------------------------------------------------
    @vec("V43 tw 4,r3,r4 equal -> trap: vector 0x700, name 'trap', SRR0 undefined "
         "(placeholder CIA+4; PAGE 163 vs PAGE 175)")
    def _():
        s = mk(pc=0x80001000)
        s.gpr[3] = s.gpr[4] = 0x1234
        u = expect_ub(step, s, 0x7C832008)
        d = u.defined
        assert (d.srr0, d.pc, d.msr, d.srr1, d.exception) == \
            (0x80001004, 0x700, 0, 0x2032, "trap")
        assert u.undefined == {"srr1": SRR1_SPECIFIC, "srr0": M}

    @vec("V44 tw 16 (lt): r3=-1 < r4=0 signed -> trap")
    def _():
        s = mk()
        s.gpr[3], s.gpr[4] = 0xFFFFFFFF, 0
        assert expect_ub(step, s, 0x7E832008).defined.exception == "trap"

    @vec("V45 tw 2 (ltu): 0xFFFFFFFF <U 0 false -> no trap, pc+4")
    def _():
        s = mk(pc=0x80001000)
        s.gpr[3], s.gpr[4] = 0xFFFFFFFF, 0
        step(s, 0x7C432008)
        assert s.pc == 0x80001004 and s.exception is None

    @vec("V46 tw 1 (gtu): 0xFFFFFFFF >U 0 -> trap")
    def _():
        s = mk()
        s.gpr[3], s.gpr[4] = 0xFFFFFFFF, 0
        assert expect_ub(step, s, 0x7C232008).defined.exception == "trap"

    @vec("V47 tw 0 never traps even when equal")
    def _():
        s = mk(pc=0x10)
        s.gpr[3] = s.gpr[4] = 5
        step(s, 0x7C032008)
        assert s.pc == 0x14

    @vec("V48 tw 24,r5,r5 (lt|gt, aliasing rA=rB) -> no trap")
    def _():
        s = mk(pc=0x20)
        s.gpr[5] = 0x80000000
        step(s, 0x7F052808)
        assert s.pc == 0x24

    @vec("V49 trap = tw 31,r0,r0 always traps")
    def _():
        assert expect_ub(step, mk(), 0x7FE00008).defined.pc == 0x700

    @vec("V50 tw with bit 31 set -> boundedly undefined")
    def _():
        assert "all" in expect_ub(step, mk(), 0x7FE00009).undefined

    @vec("V51 twi 8 (gt),r3,-1: r3=0 > -1 -> trap")
    def _():
        s = mk()
        s.gpr[3] = 0
        assert expect_ub(step, s, 0x0D03FFFF).defined.exception == "trap"

    @vec("V52 twi 1 (gtu),r3,-1: 0 >U 0xFFFFFFFF false -> no trap")
    def _():
        s = mk(pc=0x40)
        s.gpr[3] = 0
        step(s, 0x0C23FFFF)
        assert s.pc == 0x44

    @vec("V53 twi 6 (eq|ltu),r3,0x7FFF: r3=0x7FFF equal -> trap; r3=0x8000 -> none")
    def _():
        s = mk()
        s.gpr[3] = 0x7FFF
        assert expect_ub(step, s, 0x0CC37FFF).defined.exception == "trap"
        s = mk(pc=0x50)
        s.gpr[3] = 0x8000
        step(s, 0x0CC37FFF)
        assert s.pc == 0x54

    @vec("V54 twi 12 (gt|eq) r3=0x80000000 vs SIMM 0: signed less -> no trap")
    def _():
        s = mk(pc=0x60)
        s.gpr[3] = 0x80000000
        step(s, 0x0D830000)
        assert s.pc == 0x64

    # ---- trace --------------------------------------------------------------------------
    @vec("V55 SE=1, non-trapping twi -> trace, SRR0 = next instruction, SRR1 = MSR")
    @nonstrict
    def _():
        s = mk(msr=0x2032 | SE, pc=0x80001000)
        step(s, 0x0C030000)                              # twi 0,r3,0
        assert (s.srr0, s.srr1, s.msr, s.pc, s.exception) == \
            (0x80001004, 0x2032 | SE, 0, 0xD00, "trace")

    @vec("V56 SE=1 and trapping tw -> trap only, trace forgotten (PAGE 161)")
    def _():
        s = mk(msr=0x2032 | SE)
        assert expect_ub(step, s, 0x7FE00008).defined.exception == "trap"

    @vec("V57 BE=1 on non-branch tw -> no trace; trace_condition table")
    def _():
        s = mk(msr=0x2032 | BE, pc=0x100)
        step(s, 0x7C032008)
        assert s.pc == 0x104 and s.exception is None
        assert trace_condition(SE, "other") and trace_condition(SE, "branch")
        assert not trace_condition(SE, "sc") and not trace_condition(SE, "isync")
        assert not trace_condition(SE, "rfi")
        assert trace_condition(BE, "branch") and not trace_condition(BE, "other")

    @vec("V58 trace beats a simultaneously pending external (trace clears EE)")
    @nonstrict
    def _():
        s = mk(msr=0xA032 | SE, pc=0x80001000)
        s.pending["external"] = True
        step(s, 0x0C030000)
        assert s.exception == "trace" and take_pending(s) is None

    # ---- mtmsr --------------------------------------------------------------------------
    @vec("V59 mtmsr in user mode -> privileged program")
    def _():
        s = mk(msr=PR | 0xA032, pc=0x3000)
        u = expect_ub(step, s, 0x7CA00124)
        assert u.defined.exception == "program" and u.defined.srr0 == 0x3000

    @vec("V60 mtmsr then external pending: SRR0 = mtmsr+4")
    @nonstrict
    def _():
        s = mk(msr=0x2032, pc=0x3000)
        s.gpr[5] = 0xA032
        s.pending["external"] = True
        step(s, 0x7CA00124)
        assert take_pending(s) == "external" and s.srr0 == 0x3004

    @vec("V61 mtmsr changing SE -> UB (trace of mtmsr itself)")
    def _():
        s = mk(msr=0x2032)
        s.gpr[5] = 0x2032 | SE
        u = expect_ub(step, s, 0x7CA00124)
        assert set(u.undefined) >= {"pc", "srr0", "srr1", "msr", "exception"}

    @vec("V62 mtmsr writing reserved bit 28 -> UB on that bit only")
    def _():
        s = mk(msr=0x2032)
        s.gpr[5] = 0x2032 | B(28)
        u = expect_ub(step, s, 0x7CA00124)
        assert u.undefined == {"msr": B(28)} and u.defined.pc == s.pc + 4

    # ---- instruction exception helpers and priorities -----------------------------------
    @vec("V63 Table 4-3 instruction priorities")
    def _():
        sel = select_instruction_exception
        assert sel({"fp_unavailable", "program"}) == "program"
        assert sel({"syscall", "fp_unavailable"}) == "syscall"
        assert sel({"program_fp", "fp_unavailable"}) == "fp_unavailable"
        assert sel({"alignment", "dsi_bat"}) == "alignment"
        assert sel({"dsi_ecx", "alignment"}) == "dsi_ecx"
        assert sel({"iabr", "program"}) == "iabr"
        assert sel({"trace", "dsi_tlb"}) == "dsi_tlb"
        assert sel({"isi", "iabr"}) == "isi"
        assert sel(()) is None

    @vec("V64 async selector: soft_reset > external; EE gates maskables only")
    def _():
        assert select_async_exception({"soft_reset": True, "external": True}, 0) == "soft_reset"
        assert select_async_exception({"external": True, "decrementer": True}, EE) == "external"
        assert select_async_exception({"decrementer": True}, 0) is None
        assert select_async_exception({"hard_reset": True, "machine_check": ("tea",)}, 0) \
            == "hard_reset"

    @vec("V65 DSI page fault: DAR=EA, DSISR[1], SRR0=faulting instr")
    def _():
        s = mk(msr=0x2032, pc=0x80002000)
        u = expect_ub(take_dsi, s, 0xDEADBEEF, ["page_fault"])
        d = u.defined
        assert (d.dar, d.dsisr, d.srr0, d.pc, d.msr) == (0xDEADBEEF, B(1), 0x80002000, 0x300, 0)
        assert u.undefined["dsisr"] == M & ~B(1) and u.undefined["srr1"] == SRR1_SPECIFIC

    @vec("V66 ISI guarded -> SRR1[3]; SRR0 = fetch address")
    def _():
        s = mk(msr=0x2032, pc=0x90000000)
        u = expect_ub(take_isi, s, "guarded")
        d = u.defined
        assert d.srr1 == (0x2032 | B(3)) and d.srr0 == 0x90000000 and d.pc == 0x400
        assert u.undefined == {"srr1": SRR1_SPECIFIC & ~B(3)}

    @vec("V67 ISI page fault non-strict fully defined: SRR1[1]")
    @nonstrict
    def _():
        s = mk(msr=0xA032, pc=0x1000)
        take_isi(s, "page_fault")
        assert s.srr1 == (0xA032 | B(1)) and s.exception == "isi"

    @vec("V68 alignment: DAR=EA, DSISR undefined")
    @nonstrict
    def _():
        s = mk(pc=0x2000)
        u = expect_ub(take_alignment, s, 0x80000001)
        assert u.defined.dar == 0x80000001 and u.undefined == {"dsisr": M}
        assert u.defined.pc == 0x600 and u.defined.srr0 == 0x2000

    @vec("V69 FP unavailable non-strict: SRR0 = instr, vector 0x800; condition helpers")
    @nonstrict
    def _():
        s = mk(msr=0x0032, pc=0x2000)
        assert fp_unavailable_condition(s.msr) and not fp_unavailable_condition(FP)
        take_fp_unavailable(s)
        assert (s.srr0, s.pc, s.srr1) == (0x2000, 0x800, 0x32)
        assert fp_enabled_exceptions_enabled(FE0) and fp_enabled_exceptions_enabled(FE1)
        assert not fp_enabled_exceptions_enabled(FP)

    @vec("V70 program fp_enabled: SRR0 = faulting instruction (not +4)")
    def _():
        s = mk(msr=0x2932, pc=0x4000)                    # FE0|FE1 set
        u = expect_ub(take_program, s, "fp_enabled")
        assert u.defined.srr0 == 0x4000 and u.defined.exception == "program"

    @vec("V71 IABR match rules + take_iabr fully defined (Table 4-12)")
    def _():
        s = mk(msr=0x2032, pc=0x80001000)                # IR=1
        assert iabr_match(s, 0x80001000 | 3)
        assert not iabr_match(s, 0x80001000 | 2)          # TE=0 vs IR=1
        assert not iabr_match(s, 0x80001000 | 1)          # BE=0
        assert not iabr_match(s, 0x80001004 | 3)
        take_iabr(s)
        assert (s.srr0, s.srr1, s.msr, s.pc) == (0x80001000, 0x2032, 0, 0x1300)

    @vec("V72 MSR full-function reserved bit 5 at entry: UB on msr only, SRR1 copy defined "
         "(PAGE 165 note); unclassified bit 24: UB on msr and srr1")
    @nonstrict
    def _():
        s = mk(msr=0xA032 | B(5))
        s.pending["external"] = True
        u = expect_ub(take_pending, s)
        assert u.undefined == {"msr": B(5)} and u.defined.srr1 == (0xA032 | B(5))
        s = mk(msr=0xA032 | B(24))
        s.pending["external"] = True
        u = expect_ub(take_pending, s)
        assert u.undefined == {"msr": B(24), "srr1": B(24)}

    @vec("V73 write_dec: MSB 0->1 undefined; 1->1 and positive writes defined")
    def _():
        s = mk()
        s.dec = 0x10
        u = expect_ub(write_dec, s, 0x80000000)
        assert u.undefined == {"pending.decrementer": 1} and s.dec == 0x10
        s.dec = 0xFFFFFFFF
        write_dec(s, 0x80000000)
        assert s.dec == 0x80000000
        s.dec = 0
        write_dec(s, 5)
        assert s.dec == 5 and not s.pending["decrementer"]

    @vec("V74 external level re-taken after handler rfi re-enables EE")
    @nonstrict
    def _():
        s = mk(msr=0xA032, pc=0x80001000)
        s.pending["external"] = True
        take_pending(s)
        s.pc = 0x520                                   # handler returns
        step(s, 0x4C000064)
        assert s.msr == 0xA032 and s.pc == 0x80001000
        assert take_pending(s) == "external" and s.srr0 == 0x80001000

    @vec("V75 supports(): covered words only")
    def _():
        for w in (0x4C000064, 0x44000002, 0x7FE00008, 0x0C000000, 0x7CA00124, 0x7C0000A6,
                  0x44000000):
            assert supports(w)
        for w in (0x4C000020, 0x7C000010, 0x7C0002A6, 0x00000000):
            assert not supports(w)

    @vec("V76 IABR match beats a privileged rfi; instruction not executed (PAGE 162, 177)")
    def _():
        s = mk(msr=PR | 0x2032, pc=0x80003100)
        s.iabr = 0x80003100 | 3
        s.srr0, s.srr1 = 0x1234, 0x5678
        step(s, 0x4C000064)
        assert (s.exception, s.srr0, s.srr1, s.msr, s.pc) == \
            ("iabr", 0x80003100, PR | 0x2032, 0, 0x1300)

    @vec("V77 IABR match beats an invalid-form word (never executed)")
    def _():
        s = mk(pc=0x100)
        s.iabr = 0x100 | 3
        step(s, 0x7FE00009)
        assert s.exception == "iabr" and s.pc == 0x1300

    @vec("V78 SE trace with IABR on the next instruction -> control undefined (750CL PAGE 180)")
    def _():
        s = mk(msr=0x2032 | SE, pc=0x80001000)
        s.iabr = 0x80001004 | 3
        u = expect_ub(step, s, 0x0C030000)
        assert "pc" in u.undefined and "exception" in u.undefined

    @vec("V79 mtmsr altering POW or LE -> control undefined (PAGE 475)")
    def _():
        for bit in (POW, LE):
            s = mk(msr=0x2032)
            s.gpr[5] = 0x2032 | bit
            assert "pc" in expect_ub(step, s, 0x7CA00124).undefined

    @vec("V80 mfmsr: rD <- MSR, pc+4; user mode -> privileged (PAGE 461, PAGE 90)")
    def _():
        s = mk(msr=0xA032, pc=0x200)
        s.gpr[31] = 0x12345678
        step(s, 0x7FE000A6)
        assert s.gpr[31] == 0xA032 and s.pc == 0x204 and s.exception is None
        u = expect_ub(step, mk(msr=PR | 0xA032), 0x7FE000A6)
        assert u.defined.exception == "program"
        assert "all" in expect_ub(step, mk(), 0x7FE100A6).undefined

    @vec("V81 hard reset zeroes IABR (Table 4-8 PAGE 170)")
    def _():
        s = mk()
        s.iabr = 0x80001003
        s.pending["hard_reset"] = True
        u = expect_ub(take_pending, s)
        u.apply_defined(s)
        assert s.iabr == 0

    @vec("V82 smi pending: masked by EE=0, undefined once recognisable (PAGE 58, 159, 162)")
    def _():
        s = mk(msr=0x2032)
        s.pending["smi"] = True
        assert take_pending(s) is None
        s = mk(msr=0xA032)
        s.pending["smi"] = True
        assert "all" in expect_ub(take_pending, s).undefined

    fails = 0
    for title, fn in RESULTS:
        try:
            fn()
            print("PASS", title)
        except Exception:
            fails += 1
            print("FAIL", title)
            traceback.print_exc()
    print("%d vectors, %d failed" % (len(RESULTS), fails))
    sys.exit(1 if fails else 0)
