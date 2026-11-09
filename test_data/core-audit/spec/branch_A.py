"""Independent semantic model, author A, area "branch".

Branch and system instructions of the IBM Gekko, derived ONLY from
docs/reference/ppc/gekko_user_manual.txt (cited "PAGE N" = the "===== PAGE N =====" marker)
with ppc_750cl.txt used as a cross-check (cited "750CL PAGE N").

Covered
  primary 18      b / ba / bl / bla
  primary 16      bc / bca / bcl / bcla (every BO encoding, z/y bits, CTR decrement)
  primary 19      bclr / bclrl (XO 16), bcctr / bcctrl (XO 528), rfi (XO 50)
  primary 17      sc
  primary 3       twi
  primary 31      tw (XO 4), mfmsr (XO 83), mtmsr (XO 146), mfspr (XO 339),
                  mftb (XO 371, identical to mfspr on Gekko), mtspr (XO 467)
  SPRs modelled   XER LR CTR DSISR DAR DEC SRR0 SRR1 SPRG0-3 TBL/TBU HID0 HID2 GQR0-7;
                  SPR numbers absent from the manual's tables are modelled too (program
                  exception), as are privilege exceptions for every supervisor SPR, the
                  PVR/HID1 mtspr no-op (PAGE 90) and SDA/USDA (PAGE 61).  supports() is True for
                  every mfspr/mftb/mtspr word; a defined access to a register State has no field
                  for (BATs, SDR1, PMCs, ...) raises NotModelled (a NotImplementedError).

  CROSS-CHECK FIXES (crosscheck_branch.py against branch_B, 2026-09-14) are marked "XCHK:".

Conventions
  * step() never mutates `state` when it raises.  Execution happens on a register shadow.
  * UndefinedBehaviour.partial is None when NOTHING is defined (e.g. reserved instruction
    field bits set, PAGE 87).  Otherwise it is a State-like object carrying every defined
    result, and UndefinedBehaviour.undefined maps location -> bitmask (int) or True (whole
    location / non-register effect).  Undefined bits are zeroed in `partial`.
    apply_defined(state) copies partial into state; step_with_markers() does step() and
    returns {} or the undefined map after applying the defined parts.
  * Asynchronous interrupts made recognisable by mtmsr / rfi are NOT delivered by step()
    (that belongs to the exception area's take_pending); enabled_pending(state) reports them.
  * The post-instruction trace exception (MSR[SE], or MSR[BE] for a branch) IS taken by step()
    (exception 'trace', vector 0xD00, SRR0 = the next instruction), because it is part of the
    completion of the traced instruction (PAGE 161, PAGE 176).
  * Undefined-location keys: register names ('msr', 'srr0', 'xer', 'cr', 'hid0', 'hid2'),
    'gpr[N]', 'gqr[N]'; non-register annotations 'pending.decrementer', 'power_mode',
    'le_effective_point' (value True).
  * Time does not pass inside step(): TB and DEC only change by mtspr.
"""
from __future__ import annotations

import copy
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gekko_state import MASK32, State  # noqa: E402

AREA = "branch"
M = MASK32


# ======================================================================================
# Undefined-behaviour plumbing
# ======================================================================================
class UndefinedBehaviour(Exception):
    def __init__(self, what, partial=None, undefined=None):
        super().__init__(what)
        self.what = what
        self.partial = partial
        self.undefined = dict(undefined) if undefined else {"all": True}

    def apply_defined(self, state):
        if self.partial is not None:
            _commit(state, self.partial)


class NotModelled(NotImplementedError):
    """Defined by the manual, but needs a register gekko_state.State does not carry."""


_REG_FIELDS = ("gpr", "fpr", "cr", "xer", "lr", "ctr", "msr", "fpscr", "srr0", "srr1", "dar",
               "dsisr", "dec", "tb", "hid0", "hid2", "gqr", "sprg", "pc", "reserve",
               "reserve_addr", "pending", "exception")


def _shadow(state):
    s = copy.copy(state)          # memory object shared: no instruction here touches memory
    s.gpr = list(state.gpr)
    s.fpr = [list(p) for p in state.fpr]
    s.gqr = list(state.gqr)
    s.sprg = list(state.sprg)
    s.pending = dict(state.pending)
    return s


def _commit(dst, src):
    for f in _REG_FIELDS:
        v = getattr(src, f)
        if isinstance(v, list):
            v = copy.deepcopy(v)
        elif isinstance(v, dict):
            v = dict(v)
        setattr(dst, f, v)


class _Ctx:
    def __init__(self):
        self.undefined = {}
        self.why = []

    def undef(self, loc, mask, why):
        if mask is True or self.undefined.get(loc) is True:
            self.undefined[loc] = True
        else:
            self.undefined[loc] = self.undefined.get(loc, 0) | mask
        self.why.append(why)


def _full_ub(what):
    raise UndefinedBehaviour(what, None, {"all": True})


# ======================================================================================
# Bit helpers (big-endian numbering: bit 0 = MSB, PAGE 340 "x[n] n is a bit ... within x")
# ======================================================================================
def _b(i):
    return 1 << (31 - i)


def _field(w, a, b):
    return (w >> (31 - b)) & ((1 << (b - a + 1)) - 1)


def _exts(v, bits):
    v &= (1 << bits) - 1
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


# MSR bits, Table 4-4 PAGE 164-165
MSR_POW, MSR_ILE, MSR_EE, MSR_PR, MSR_FP, MSR_ME = _b(13), _b(15), _b(16), _b(17), _b(18), _b(19)
MSR_FE0, MSR_SE, MSR_BE, MSR_FE1, MSR_IP, MSR_IR = _b(20), _b(21), _b(22), _b(23), _b(25), _b(26)
MSR_DR, MSR_PM, MSR_RI, MSR_LE = _b(27), _b(29), _b(30), _b(31)
# Reserved MSR bits, PAGE 164: 0 full, 1-4 partial, 5-9 full, 10-12 partial, 14 impl-specific,
# 24 reserved (AL), 28 full.
MSR_RSVD_ALL = 0xFFFA0088
# bits whose written value mtmsr cannot define: partial function / impl-specific / plain reserved
MSR_RSVD_NOT_FULL = 0x783A0080  # 1-4, 10-12, 14, 24
# SRR1 bits that receive MSR on exception entry: PAGE 163 "MSR[5-9, 16-31] are placed into the
# corresponding bit positions of SRR1", PAGE 537 adds bit 0, PAGE 165 note "Full function reserved
# bits are saved in SRR1" (0, 5-9, 28).  Bit 24 (not full function) handled separately.
SRR1_SAVE = 0x87C0FF7F
SRR1_INFO = 0x783F0000   # bits 1-4, 10-15: PAGE 166 step 2 / PAGE 537 "set to undefined values"
SRR1_B24 = _b(24)
# rfi restore mask, PAGE 532: MSR[0,5-9,16-23,25-27,30-31] <- SRR1[...]
RFI_MASK = 0x87C0FF73
# Bits an implementation "may" also restore on rfi (PAGE 532 note): 24, 28, 29 (PM, PAGE 58)
RFI_MAYBE = _b(24) | _b(28) | _b(29)

HID0_DOZE_NAP_SLEEP = _b(8) | _b(9) | _b(10)       # PAGE 63-64
HID0_RSVD = _b(4) | _b(5) | _b(6) | _b(12) | _b(13) | _b(14) | _b(27) | _b(30)  # PAGE 63-66
HID0_FLASH = _b(20) | _b(21)                         # ICFI/DCFI self-clearing, PAGE 65

HID2_RW = 0xF00F0000      # LSQE WPE PSE LCE (0-3), enables 12-15, PAGE 67-68
HID2_RO = 0x0F000000      # DMAQL 4-7 "(read only)", PAGE 68
HID2_STICKY = 0x00F00000  # 8-11 "(sticky)", PAGE 68
HID2_RSVD = 0x0000FFFF    # 16-31 "Reserved", PAGE 68

GQR_RSVD = 0xC0F8C0F8     # 0-1, 8-12, 16-17, 24-28 reserved, Table 2-16 PAGE 78
XER_DEF = 0xF000FF7F      # 0-3 (mcrxr PAGE 460), 16-23 (PAGE 57), 25-31 (lswx PAGE 449)


# ======================================================================================
# Exception entry (only the parts these manuals define)
# ======================================================================================
def _enter_exception(sh, ctx, cia, vector, name, srr0, srr0_why=None):
    old = sh.msr
    if srr0 is None:
        sh.srr0 = 0
        ctx.undef("srr0", True, srr0_why)
    else:
        sh.srr0 = srr0 & M                                    # PAGE 537 "SRR0 <-iea CIA + 4"
    und = SRR1_INFO                                           # PAGE 166 step 2 / PAGE 537
    msr_und = ctx.undefined.get("msr", 0)
    msr_und = M if msr_und is True else msr_und
    # XCHK: an MSR bit left undefined earlier in the same step (mtmsr writing reserved bit 24,
    # then a trace entry) is copied to SRR1 undefined as well - this model zeroes undefined bits
    # in the shadow, so `old` alone would under-report it.
    if (old | msr_und) & SRR1_B24:
        # PAGE 166 step 3 "Depending on the implementation, reserved bits may not be copied";
        # when MSR[24]=0 copied and "read as if written as 0" (PAGE 168) agree.
        und |= SRR1_B24
    sh.srr1 = old & SRR1_SAVE
    ctx.undef("srr1", und, "SRR1[1-4,10-15] exception-specific / undefined PAGE 166, PAGE 537")
    # Table 4-6 PAGE 168: POW EE PR FP FE0 SE BE FE1 IR DR PM RI cleared; ILE ME IP unaltered;
    # LE <- ILE; "Reserved bits are read as if written as 0."
    new = old & (MSR_ILE | MSR_ME | MSR_IP)
    if old & MSR_ILE:
        new |= MSR_LE
    if old & MSR_RSVD_ALL:
        ctx.undef("msr", old & MSR_RSVD_ALL,
                  "reserved MSR bits across exception entry not listed in Table 4-6 PAGE 168")
    sh.msr = new
    # PAGE 166 step 5 / PAGE 164 IP: 0x000n_nnnn or 0xFFFn_nnnn
    sh.pc = ((0xFFF00000 if old & MSR_IP else 0) + vector) & M
    sh.exception = name


def _program(sh, ctx, cia, kind):
    # Illegal-instruction / privileged-instruction type program exception, vector 0x00700
    # (PAGE 175).
    # XCHK (was: SRR0 undefined): SRR0 = CIA.  PAGE 46: "(excluding the trap and system call
    # exceptions) the address of the faulting instruction is provided to the exception handler
    # and ... neither the faulting instruction nor subsequent instructions ... will complete";
    # PAGE 161: program exceptions are "taken without completing the exception-causing
    # instruction"; PAGE 163 names only "a system call, trace, or trap exception" as the cases
    # that save "the next one".  Illegal and privileged program exceptions are none of those.
    _enter_exception(sh, ctx, cia, 0x700, "program", cia)


def _trap(sh, ctx, cia, kind):
    # Trap type program exception, vector 0x00700 (PAGE 114 "the system trap type program
    # exception is taken", PAGE 175).
    # XCHK (was: exception name 'program'): named 'trap' - Table 4-3 PAGE 162 "trap exception
    # condition", PAGE 163 "trap exception"; gekko_state lists 'trap' as a distinct name.
    # SRR0 stays undefined: PAGE 163 "the next one (as in the case of a system call, trace, or
    # trap exception)" vs PAGE 161 "Program ... taken without completing the exception-causing
    # instruction", and PAGE 175 defers the register settings to the PEM (not available).
    _enter_exception(sh, ctx, cia, 0x700, "trap", None,
                     f"SRR0 for {kind}: CIA+4 (PAGE 163) vs not completed (PAGE 161); settings "
                     "deferred to PEM PAGE 175")


# ======================================================================================
# Branches
# ======================================================================================
def _bo_bits(bo):
    # BO[0] is the MSB of the 5-bit field (PAGE 359 figure: BO occupies bits 6-10)
    return (bo >> 4) & 1, (bo >> 3) & 1, (bo >> 2) & 1, (bo >> 1) & 1


def _h_b(sh, w, ctx, cia):
    # PAGE 358: NIA <-iea (AA ? 0 : CIA) + EXTS(LI || 0b00); if LK then LR <-iea CIA + 4
    li = _field(w, 6, 29)
    disp = _exts(li << 2, 26)
    aa, lk = (w >> 1) & 1, w & 1
    tgt = disp & M if aa else (cia + disp) & M        # PAGE 340 "<-iea ... 32-bit ... address"
    if lk:
        sh.lr = (cia + 4) & M
    sh.pc = tgt


def _cond(sh, bo, bi, decrement_allowed=True):
    b0, b1, b2, b3 = _bo_bits(bo)
    if not b2:
        # PAGE 359 "if not BO[2] then CTR <- CTR - 1"; PAGE 363 "the entire 32-bit CTR is
        # decremented" (wraps 0 -> 0xFFFFFFFF)
        sh.ctr = (sh.ctr - 1) & M
    ctr_ok = b2 or ((sh.ctr != 0) ^ b3)                 # PAGE 359 ctr_ok
    crbit = (sh.cr >> (31 - bi)) & 1
    cond_ok = b0 or (crbit == b1)                       # PAGE 359 cond_ok (z/y bits ignored)
    return bool(ctr_ok and cond_ok)


def _h_bc(sh, w, ctx, cia):
    # PAGE 359-360
    bo, bi = _field(w, 6, 10), _field(w, 11, 15)
    disp = _exts(_field(w, 16, 29) << 2, 16)
    aa, lk = (w >> 1) & 1, w & 1
    taken = _cond(sh, bo, bi)
    nia = (cia + 4) & M
    if taken:
        nia = disp & M if aa else (cia + disp) & M
    # LR update regardless of the outcome: PAGE 360 "Other registers altered: Link Register (LR)
    # (if LK = 1)" and "If LK = 1, the effective address of the instruction following the branch
    # instruction is placed into the link register."
    if lk:
        sh.lr = (cia + 4) & M
    sh.pc = nia


def _h_bclr(sh, w, ctx, cia):
    # PAGE 363-364; bits 16-20 reserved (figure PAGE 363) -> boundedly undefined PAGE 87
    if _field(w, 16, 20):
        _full_ub("bclr with nonzero reserved bits 16-20: boundedly undefined PAGE 87, PAGE 363")
    bo, bi = _field(w, 6, 10), _field(w, 11, 15)
    lk = w & 1
    target = sh.lr & 0xFFFFFFFC     # PAGE 364 "LR[0-29] || 0b00", read BEFORE the LR update
    taken = _cond(sh, bo, bi)
    if lk:
        sh.lr = (cia + 4) & M
    sh.pc = target if taken else (cia + 4) & M


def _h_bcctr(sh, w, ctx, cia):
    # PAGE 361-362
    if _field(w, 16, 20):
        _full_ub("bcctr with nonzero reserved bits 16-20: boundedly undefined PAGE 87, PAGE 361")
    bo, bi = _field(w, 6, 10), _field(w, 11, 15)
    if not (bo >> 2) & 1:
        _full_ub("bcctr with BO[2]=0: 'the instruction form is invalid' PAGE 362; Gekko 'provides "
                 "limited support for instructions represented in an invalid form' PAGE 87")
    lk = w & 1
    b0, b1 = (bo >> 4) & 1, (bo >> 3) & 1
    crbit = (sh.cr >> (31 - bi)) & 1
    cond_ok = b0 or (crbit == b1)                       # PAGE 361 cond_ok
    target = sh.ctr & 0xFFFFFFFC    # PAGE 361 text "CTR[0-29] || 0b00" (RTL "CTR || 0b00")
    if lk:
        sh.lr = (cia + 4) & M       # PAGE 362 "Other registers altered: LR (if LK = 1)"
    sh.pc = target if cond_ok else (cia + 4) & M


# ======================================================================================
# System linkage
# ======================================================================================
def _h_sc(sh, w, ctx, cia):
    # Figure PAGE 537: 17 | 0...0 (6-29) | 1 (30) | 0 (31)
    if w != 0x44000002:
        # XCHK (was: bit 30 = 0 -> illegal-instruction program exception).  PAGE 88 "The
        # following primary opcodes have unused extended opcodes. 4, 17, ..." says SOME opcode-17
        # words are illegal, but neither PAGE 537 nor Table A-1 PAGE 583 identifies an
        # extended-opcode field for primary 17 (every bit 6-31 is drawn as a fixed 0/1), so a
        # malformed word is either illegal (program exception) or an sc with "incorrectly set
        # bits in reserved fields" (boundedly undefined, PAGE 87): not decidable -> undefined.
        _full_ub("opcode-17 word other than 0x44000002: illegal (PAGE 88) or boundedly "
                 "undefined (PAGE 87, PAGE 537)")
    # PAGE 537: SRR0 <-iea CIA + 4; SRR1 <- MSR bits; MSR per exception; NIA <- base + 0xC00
    _enter_exception(sh, ctx, cia, 0xC00, "syscall", cia + 4)


def _h_rfi(sh, w, ctx, cia):
    if w != 0x4C000064:
        _full_ub("rfi with nonzero reserved bits 6-20/31: boundedly undefined PAGE 87, PAGE 532")
    if sh.msr & MSR_PR:
        _program(sh, ctx, cia, "privileged-instruction (rfi, 'supervisor-level' PAGE 532)")
        return
    old, s1 = sh.msr, sh.srr1
    # PAGE 532: MSR[0,5-9,16-23,25-27,30-31] <- SRR1[...]; MSR[13] <- 0; other bits unassigned
    new = (old & ~RFI_MASK & ~MSR_POW) | (s1 & RFI_MASK)
    diff = (old ^ s1) & RFI_MAYBE
    if diff:
        # PAGE 532 "an implementation may define additional MSR bits, and ... may also cause
        # them to be ... restored to MSR from SRR1 on an rfi"
        ctx.undef("msr", diff, "rfi restore of MSR[24,28,29] implementation-optional PAGE 532")
        new &= ~diff
    sh.msr = new & M
    sh.pc = sh.srr0 & 0xFFFFFFFC                        # PAGE 532 "SRR0[0-29] || 0b00"


# ======================================================================================
# Traps
# ======================================================================================
def _trap_cond(to, a, b):
    sa, sb = _exts(a, 32), _exts(b, 32)
    return bool(((to & 0x10) and sa < sb) or        # TO[0]  PAGE 578
                ((to & 0x08) and sa > sb) or        # TO[1]
                ((to & 0x04) and a == b) or         # TO[2]
                ((to & 0x02) and a < b) or          # TO[3] <U
                ((to & 0x01) and a > b))            # TO[4] >U


def _h_tw(sh, w, ctx, cia):
    if w & 1:
        _full_ub("tw with nonzero reserved bit 31: boundedly undefined PAGE 87, PAGE 578")
    to, ra, rb = _field(w, 6, 10), _field(w, 11, 15), _field(w, 16, 20)
    if _trap_cond(to, sh.gpr[ra], sh.gpr[rb]):
        # PAGE 578 "the system trap handler is invoked"; PAGE 114 "system trap type program
        # exception is taken"
        _trap(sh, ctx, cia, "trap (tw)")
        return
    sh.pc = (cia + 4) & M                               # PAGE 114 "execution continues normally"


def _h_twi(sh, w, ctx, cia):
    to, ra = _field(w, 6, 10), _field(w, 11, 15)
    simm = _exts(_field(w, 16, 31), 16) & M             # PAGE 579 EXTS(SIMM)
    if _trap_cond(to, sh.gpr[ra], simm):
        _trap(sh, ctx, cia, "trap (twi)")
        return
    sh.pc = (cia + 4) & M


# ======================================================================================
# MSR moves
# ======================================================================================
def _h_mtmsr(sh, w, ctx, cia):
    if _field(w, 11, 20) or (w & 1):
        _full_ub("mtmsr with nonzero reserved bits: boundedly undefined PAGE 87, PAGE 475")
    if sh.msr & MSR_PR:
        _program(sh, ctx, cia, "privileged-instruction (mtmsr, 'supervisor-level' PAGE 475)")
        return
    old = sh.msr
    new = sh.gpr[_field(w, 6, 10)]                      # PAGE 475 "MSR <- (rS)"
    bad = new & MSR_RSVD_NOT_FULL
    if bad:
        ctx.undef("msr", bad, "writing partial-function/implementation-specific/reserved MSR "
                  "bits 1-4,10-12,14,24 PAGE 164")
        new &= ~bad
    if (new & MSR_POW) and not (old & MSR_POW) and (sh.hid0 & HID0_DOZE_NAP_SLEEP):
        # PAGE 164 "Power management functions are implementation-dependent"; PAGE 475 not
        # execution-synchronizing for POW
        ctx.undef("power_mode", True, "MSR[POW] set with HID0[DOZE|NAP|SLEEP] PAGE 164, PAGE 475")
    if (new ^ old) & MSR_LE:
        ctx.undef("le_effective_point", True,
                  "mtmsr not execution synchronizing w.r.t. LE PAGE 475")
    if (new ^ old) & MSR_SE:
        # XCHK (was: 'trace' marker whenever the new SE = 1).  PAGE 161 trace exceptions are
        # "generated following execution and completion of an instruction while trace mode is
        # enabled"; PAGE 475 makes EE/RI "effective as soon as the instruction completes" but says
        # nothing about SE.  Whether an mtmsr that changes SE is itself traced decides pc, srr0,
        # srr1, msr and exception -> the whole outcome is undefined.  Unchanged SE=1 is an
        # ordinary traced instruction (step()).
        _full_ub("mtmsr changing MSR[SE]: whether the mtmsr itself is traced PAGE 161, 164, 475")
    sh.msr = new & M
    sh.pc = (cia + 4) & M


def _h_mfmsr(sh, w, ctx, cia):
    if _field(w, 11, 20) or (w & 1):
        _full_ub("mfmsr with nonzero reserved bits: boundedly undefined PAGE 87, PAGE 461")
    if sh.msr & MSR_PR:
        _program(sh, ctx, cia, "privileged-instruction (mfmsr, 'supervisor-level' PAGE 461)")
        return
    d = _field(w, 6, 10)
    # XCHK (was: all bits defined).  PAGE 461 "rD <- MSR", but the bits whose storage this model
    # already calls undefined on write (reserved partial-function 1-4, 10-12, implementation-
    # specific 14 and bit 24, Table 4-4 PAGE 164) cannot be defined on read: a value held there
    # came from an undefined write.  Full-function reserved bits 0, 5-9, 28 stay defined
    # (PAGE 165 note "Full function reserved bits are saved in SRR1").
    und = sh.msr & MSR_RSVD_NOT_FULL
    if und:
        ctx.undef(f"gpr[{d}]", und, "mfmsr of MSR bits with undefined storage PAGE 164")
    sh.gpr[d] = sh.msr & ~und & M
    sh.pc = (cia + 4) & M


# ======================================================================================
# SPR moves
# ======================================================================================
def _rng(a, b):
    return set(range(a, b + 1))


_COMMON = ({1, 8, 9, 18, 19, 22, 25, 26, 27, 282} | _rng(272, 275) | _rng(528, 543) |
           _rng(912, 923) | _rng(936, 943) | _rng(952, 959) | {1008, 1009, 1010, 1013, 1017} |
           _rng(1019, 1022))
# Table 12-10 PAGE 463-465 (mfspr) + TBL/TBU 268/269 readable by mfspr: PAGE 117 note 2
# "specifying TBR 268 for TBL and SPR 269 for TBU", PAGE 120 mftb == mfspr on Gekko.
MF_DEFINED = frozenset(_COMMON | {287, 268, 269})
# Table 12-13 PAGE 477-479 (mtspr)
# XCHK (was: 287 absent -> illegal-instruction program exception in supervisor mode).  PAGE 90
# "An mtspr instruction executing in supervisor mode (MSR[PR] = 0) with the SPR field specifying
# HID1 or PVR (read-only registers) executes as a no-op."  - the same bullet that states the
# invalid-SPR rule, so PVR is not an invalid SPR field for mtspr on Gekko.
MT_DEFINED = frozenset(_COMMON | {284, 285, 287})
MT_NOOP = frozenset({287, 1009})

MF_MODELLED = frozenset({1, 8, 9, 18, 19, 22, 26, 27, 268, 269, 920, 1008} | _rng(272, 275) |
                        _rng(912, 919))
MT_MODELLED = frozenset({1, 8, 9, 18, 19, 22, 26, 27, 284, 285, 920, 1008} | _rng(272, 275) |
                        _rng(912, 919))

# XCHK (was: all bits of a read defined).  Bits this model calls undefined when WRITTEN cannot be
# defined when READ back (the stored value came from an undefined write): XER bits outside
# 0-3,16-23,25-31 (PAGE 57 "XER[16-23] is implemented so that they can be read"), GQR reserved
# bits (Table 2-16 PAGE 78), HID2 16-31 "Reserved" (PAGE 68), HID0 "Reserved"/"Not used" bits
# (PAGE 63-66).
READ_UNDEFINED = {1: ~XER_DEF & M, 920: HID2_RSVD, 1008: HID0_RSVD}
READ_UNDEFINED.update({n: GQR_RSVD for n in range(912, 920)})


def _spr_number(w):
    # PAGE 462 "n <- spr[5-9] || spr[0-4]"; spr[0-4] = instruction bits 11-15 (low half of n),
    # spr[5-9] = bits 16-20 (high half), PAGE 465 note.
    return (_field(w, 16, 20) << 5) | _field(w, 11, 15)


def _spr_supervisor(n):
    # PAGE 463 / PAGE 477: "SPR[0] = 1 if and only if reading [writing] the register is
    # supervisor-level".  SPR[0] = instruction bit 11 = bit 0x10 of n.
    return bool(n & 0x10)


def _store_masked(ctx, loc, value, rsvd_mask, why):
    bad = value & rsvd_mask
    if bad:
        ctx.undef(loc, bad, why)
    return value & ~bad & M


def _h_spr(sh, w, ctx, cia, write):
    n = _spr_number(w)
    reg = _field(w, 6, 10)
    defined = MT_DEFINED if write else MF_DEFINED
    if write and 936 <= n <= 942:
        # XCHK (was: unsupported).  Table 12-13 PAGE 478 lists UMMCR0/UPMC1-4/USIA/UMMCR1 as
        # "User" mtspr encodings, while Table 2-53 PAGE 118 gives them "mfspr" only (and 750CL
        # PAGE 51: "Attempting to write to a user-read-only performance monitor register causes a
        # program exception, regardless of the MSR[PR] setting").  The Gekko manual contradicts
        # itself -> whether an exception is taken is undefined.
        _full_ub(f"mtspr to user read-only PM SPR {n}: Table 12-13 PAGE 478 vs Table 2-53 PAGE 118")
    if n not in defined:
        # PAGE 175 "Gekko fully decodes the SPR field ... If an undefined SPR is specified, a
        # program exception is taken."
        _program(sh, ctx, cia, f"illegal-instruction (undefined SPR {n})")
        return
    if (sh.msr & MSR_PR) and _spr_supervisor(n):
        # PAGE 463 / PAGE 477 "privileged instruction type program exception"
        _program(sh, ctx, cia, f"privileged-instruction (SPR {n} with MSR[PR]=1)")
        return
    if write:
        v = sh.gpr[reg]
        if n == 1:
            sh.xer = _store_masked(ctx, "xer", v, ~XER_DEF & M,
                                   "XER bits outside 0-3,16-23,25-31 not described PAGE 57")
        elif n == 8:
            sh.lr = v
        elif n == 9:
            sh.ctr = v
        elif n == 18:
            sh.dsisr = v
        elif n == 19:
            sh.dar = v
        elif n == 22:
            old = sh.dec
            sh.dec = v
            if not (old & 0x80000000) and (v & 0x80000000):
                # XCHK (was: pending["decrementer"] = True).  Table 4-2 PAGE 159 "when the
                # most-significant bit of the DEC register changes from 0 to 1" vs Table 4-3
                # PAGE 162 "Decrementer passes through zero" and PAGE 176 "a decrementer exception
                # condition occurs (for example, the decrementer register has completed
                # decrementing)": whether a SOFTWARE write that sets the MSB requests the
                # exception is not stated -> the pending flag is undefined.
                ctx.undef("pending.decrementer", True,
                          "mtspr DEC setting the MSB: exception requested or not PAGE 159/162/176")
            elif sh.pending.get("decrementer"):
                # XCHK (common-mode, both models kept it pending): PAGE 164 EE=0 "delays
                # recognition of ... decrementer exception conditions", but no page says whether
                # rewriting DEC withdraws a condition that is already waiting -> undefined.
                ctx.undef("pending.decrementer", True,
                          "mtspr DEC while a decrementer condition is pending PAGE 164/176")
        elif n == 26:
            sh.srr0 = v
        elif n == 27:
            sh.srr1 = v
        elif 272 <= n <= 275:
            sh.sprg[n - 272] = v
        elif n == 284:
            # PAGE 477 "SPRs TBL and TBU are treated as separate 32-bit registers; setting one
            # leaves the other unaltered."
            sh.tb = (sh.tb & 0xFFFFFFFF00000000) | v
        elif n == 285:
            sh.tb = (sh.tb & 0x00000000FFFFFFFF) | (v << 32)
        elif 912 <= n <= 919:
            sh.gqr[n - 912] = _store_masked(ctx, f"gqr[{n - 912}]", v, GQR_RSVD,
                                            "GQR reserved bits Table 2-16 PAGE 78")
        elif n == 920:
            old = sh.hid2
            new = (v & HID2_RW) | (old & HID2_RO)       # DMAQL "(read only)" PAGE 68
            sdiff = (v ^ old) & HID2_STICKY
            if sdiff:
                ctx.undef("hid2", sdiff, "software write to sticky HID2 error bits PAGE 68")
            new |= old & HID2_STICKY & ~sdiff
            new |= _store_masked(ctx, "hid2", v & HID2_RSVD, HID2_RSVD,
                                 "HID2 reserved bits 16-31 PAGE 68")
            sh.hid2 = new & M
        elif n == 1008:
            nv = _store_masked(ctx, "hid0", v, HID0_RSVD, "HID0 reserved/not-used bits PAGE 63-66")
            flash = nv & HID0_FLASH
            if flash:
                ctx.undef("hid0", flash, "ICFI/DCFI 'hardware automatically resets these bits "
                          "in the next cycle' PAGE 65")
                nv &= ~flash
            sh.hid0 = nv
        elif n in MT_NOOP:
            pass        # PAGE 90 "... specifying HID1 or PVR (read-only registers) executes as a no-op"
        elif n == 959:
            # PAGE 61 "Gekko does not implement the sampled data address register (SDA) or the
            # user-level, read-only USDA registers. However, ... those registers can be written to
            # by boot code without causing an exception." (supervisor, checked above)
            pass
        elif n == 943:
            if sh.msr & MSR_PR:
                # PAGE 61 covers "boot code" only; Table 12-13 PAGE 478 lists USDA "User"
                _full_ub("mtspr USDA with MSR[PR]=1: PAGE 61 covers boot code only, PAGE 478 User")
        else:
            raise NotModelled(f"mtspr SPR {n}: register not carried by gekko_state.State")
    else:
        if n == 1:
            v = sh.xer
        elif n == 8:
            v = sh.lr
        elif n == 9:
            v = sh.ctr
        elif n == 18:
            v = sh.dsisr
        elif n == 19:
            v = sh.dar
        elif n == 22:
            v = sh.dec
        elif n == 26:
            v = sh.srr0
        elif n == 27:
            v = sh.srr1
        elif 272 <= n <= 275:
            v = sh.sprg[n - 272]
        elif n == 268:
            v = sh.tb & M                               # PAGE 468 TBL
        elif n == 269:
            v = (sh.tb >> 32) & M                       # PAGE 468 TBU
        elif 912 <= n <= 919:
            v = sh.gqr[n - 912]
        elif n == 920:
            v = sh.hid2
        elif n == 1008:
            v = sh.hid0
        elif n in (943, 959):
            # XCHK (was: unsupported).  PAGE 61 "Gekko does not implement the sampled data address
            # register (SDA) or the user-level, read-only USDA registers" - the value read is not
            # specified.
            _full_ub(f"mfspr SPR {n} (SDA/USDA not implemented, read value unspecified PAGE 61)")
        else:
            raise NotModelled(f"mfspr SPR {n}: register not carried by gekko_state.State")
        und = v & READ_UNDEFINED.get(n, 0)
        if und:
            ctx.undef(f"gpr[{reg}]", und, f"mfspr SPR {n}: reserved bits with undefined storage "
                      "PAGE 57/63-68/78")
        sh.gpr[reg] = v & ~und & M                      # PAGE 462 "rD <- SPR(n)"
    if w & 1:
        # PAGE 175 "mtspr and mfspr with the record bit (Rc) set ... In Gekko, the appropriate
        # condition register (CR) should be treated as undefined."
        # XCHK (was: also on the program-exception paths).  PAGE 161: program exceptions are
        # "taken without completing the exception-causing instruction", so the undefined CR is a
        # result of COMPLETED execution only.
        ctx.undef("cr", 0xF0000000, "mtspr/mfspr with Rc=1: CR undefined PAGE 175")
        sh.cr &= 0x0FFFFFFF
    sh.pc = (cia + 4) & M


# ======================================================================================
# Decode / dispatch
# ======================================================================================
# (handler, is_branch (PAGE 58 list, BE trace), exempt_from_SE_trace (PAGE 164: rfi, sc))
def _decode(word):
    w = word & M
    op = w >> 26
    if op == 18:
        return _h_b, True, False, "b"
    if op == 16:
        return _h_bc, True, False, "bc"
    if op == 17:
        return _h_sc, False, True, "sc"
    if op == 3:
        return _h_twi, False, False, "twi"
    if op == 19:
        xo = _field(w, 21, 30)
        if xo == 16:
            return _h_bclr, True, False, "bclr"
        if xo == 528:
            return _h_bcctr, True, False, "bcctr"
        if xo == 50:
            return _h_rfi, False, True, "rfi"
        return None
    if op == 31:
        xo = _field(w, 21, 30)
        if xo == 4:
            return _h_tw, False, False, "tw"
        if xo == 83:
            return _h_mfmsr, False, False, "mfmsr"
        if xo == 146:
            return _h_mtmsr, False, False, "mtmsr"
        # XCHK (was: supports() False for defined-but-unmodelled SPRs, which also hid their
        # fully defined privileged-instruction program exception in user mode): every SPR word is
        # supported; an access that needs an absent register raises NotModelled from step().
        if xo in (339, 371):   # PAGE 120 "Gekko ignores ... bit 25 and treating both identically"
            return (lambda sh, ww, ctx, cia: _h_spr(sh, ww, ctx, cia, False)), False, False, "mfspr"
        if xo == 467:
            return (lambda sh, ww, ctx, cia: _h_spr(sh, ww, ctx, cia, True)), False, False, "mtspr"
    return None


def supports(word):
    return _decode(word) is not None


def mnemonic(word):
    d = _decode(word)
    return d[3] if d else None


def step(state, word):
    d = _decode(word)
    if d is None:
        raise NotImplementedError(f"branch_A does not model {word & M:#010x}")
    handler, is_branch, se_exempt, _name = d
    cia = state.pc & M
    old_msr = state.msr
    sh = _shadow(state)
    sh.exception = None
    ctx = _Ctx()
    handler(sh, word & M, ctx, cia)          # full-UB raises before anything is committed
    if sh.exception is None:
        # PAGE 164 SE: trace "upon the successful execution of every instruction except rfi,
        # isync, and sc. Successful execution means that the instruction caused no other
        # exception."; BE: "when a branch instruction executes successfully".
        # XCHK (was: a 'trace' marker with the pre-trace state as the partial result).  The entry
        # itself is given by the manual: PAGE 176 "Each instruction considered during trace mode
        # completes before a trace exception is taken" and "instruction fetching resumes as offset
        # 0x00D00 from the base address indicated by MSR[IP]"; Table 4-6 PAGE 168 "Trace
        # exception" MSR row; PAGE 166 steps 2-3 for SRR1 (bits 1-4, 10-15 exception-specific ->
        # undefined); SRR0 = the instruction that executes next, PAGE 163 "the next one (as in the
        # case of a system call, trace, or trap exception)", 750CL PAGE 166 step 1 "Normally, this
        # is the instruction that would have been completed next had the exception not been
        # taken", consistent with PAGE 161 (trace is post-instruction, after completion).
        if (old_msr & MSR_SE and not se_exempt) or (old_msr & MSR_BE and is_branch):
            _enter_exception(sh, ctx, cia, 0xD00, "trace", sh.pc)
    if ctx.undefined:
        raise UndefinedBehaviour("; ".join(ctx.why), sh, ctx.undefined)
    _commit(state, sh)


def step_with_markers(state, word):
    try:
        step(state, word)
        return {}
    except UndefinedBehaviour as u:
        u.apply_defined(state)
        return dict(u.undefined)


def enabled_pending(state):
    """Asynchronous exceptions that must be taken before the next instruction (PAGE 475 mtmsr,
    PAGE 532 rfi), in Table 4-3 priority order (PAGE 162: external before decrementer)."""
    out = []
    if state.msr & MSR_EE:
        if state.pending.get("external"):
            out.append("external")
        if state.pending.get("decrementer"):
            out.append("decrementer")
    return out


# ======================================================================================
# Self test: vectors hand-derived from the manual text
# ======================================================================================
def _xo31(xo, a=0, b=0, c=0, rc=0):
    return (31 << 26) | (a << 21) | (b << 16) | (c << 11) | (xo << 1) | rc


def _sprw(xo, reg, n, rc=0):
    return (31 << 26) | (reg << 21) | ((n & 0x1F) << 16) | (((n >> 5) & 0x1F) << 11) | (xo << 1) | rc


def _run_tests():
    C = 0x80003100
    PROG = {"pc": 0x700, "exception": "program"}
    vectors = []

    def T(name, word, setup=None, expect=None, ub=None):
        vectors.append((name, word, setup or {}, expect or {}, ub))

    def prog_expect(msr_before, extra=None, trap=False, vector=0x700, srr0=C):
        e = dict(PROG)
        e["msr"] = (msr_before & (MSR_ILE | MSR_ME | MSR_IP)) | (MSR_LE if msr_before & MSR_ILE else 0)
        e["pc"] = (0xFFF00000 if msr_before & MSR_IP else 0) + vector
        # XCHK: illegal/privileged SRR0 = CIA (PAGE 46/161); trap SRR0 undefined (zeroed in partial)
        e["srr0"] = 0 if trap else srr0
        e["srr1"] = msr_before & 0x87C0FF7F
        if trap:
            e["exception"] = "trap"
        if extra:
            e.update(extra)
        return e

    def trap_expect(msr_before, extra=None):
        return prog_expect(msr_before, extra, trap=True)

    def trace_expect(msr_before, nia, extra=None):
        e = prog_expect(msr_before, extra, vector=0xD00, srr0=nia)
        e["exception"] = "trace"
        return e

    PROG_UB = {"srr1": 0x783F0000}
    TRAP_UB = {"srr0": True, "srr1": 0x783F0000}
    base_msr = 0x00002032

    # ---------------- b / ba / bl / bla  (PAGE 358) ----------------
    T("b +8", 0x48000008, expect={"pc": 0x80003108})
    T("b -4", 0x4BFFFFFC, expect={"pc": 0x800030FC})
    T("bl +0x100 sets LR", 0x48000101, expect={"pc": 0x80003200, "lr": 0x80003104})
    T("ba 0x100", 0x48000102, expect={"pc": 0x00000100})
    T("bla LI all ones sign-extends", 0x4BFFFFFF, expect={"pc": 0xFFFFFFFC, "lr": 0x80003104})
    T("b max positive LI", 0x49FFFFFC, expect={"pc": 0x820030FC})
    T("b most negative LI", 0x4A000000, expect={"pc": 0x7E003100})
    T("b wraps below 0", 0x4BFFFFF8, setup={"pc": 0}, expect={"pc": 0xFFFFFFF8})
    T("bl at 0xFFFFFFFC wraps LR and NIA", 0x48000005, setup={"pc": 0xFFFFFFFC},
      expect={"pc": 0, "lr": 0})
    T("bl with MSR[BE] -> trace entry, SRR0 = target", 0x48000101, setup={"msr": base_msr | MSR_BE},
      expect=trace_expect(base_msr | MSR_BE, 0x80003200, {"lr": 0x80003104}), ub=PROG_UB)
    T("bc not taken with MSR[BE] -> trace, SRR0 = CIA+4, IP prefix", 0x41820010,
      setup={"msr": base_msr | MSR_BE | MSR_IP, "cr": 0},
      expect=trace_expect(base_msr | MSR_BE | MSR_IP, C + 4), ub=PROG_UB)

    # ---------------- bc family (PAGE 359-360) ----------------
    T("beq taken", 0x41820010, setup={"cr": 0x20000000}, expect={"pc": C + 0x10})
    T("beq not taken", 0x41820010, setup={"cr": 0}, expect={"pc": C + 4})
    T("bne taken", 0x40820010, setup={"cr": 0}, expect={"pc": C + 0x10})
    T("beq with y hint bit (BO=13)", 0x41A20010, setup={"cr": 0x20000000}, expect={"pc": C + 0x10})
    T("bdnz ctr 1->0 not taken", 0x42000010, setup={"ctr": 1}, expect={"pc": C + 4, "ctr": 0})
    T("bdnz ctr 0 wraps, taken", 0x42000010, setup={"ctr": 0},
      expect={"pc": C + 0x10, "ctr": 0xFFFFFFFF})
    T("bdnz ctr 2->1 taken", 0x42000010, setup={"ctr": 2}, expect={"pc": C + 0x10, "ctr": 1})
    T("bdz ctr 1->0 taken", 0x42400010, setup={"ctr": 1}, expect={"pc": C + 0x10, "ctr": 0})
    T("bdz ctr 0 wraps not taken", 0x42400010, setup={"ctr": 0},
      expect={"pc": C + 4, "ctr": 0xFFFFFFFF})
    T("bdnzt LT set ctr5 taken", 0x41000010, setup={"cr": 0x80000000, "ctr": 5},
      expect={"pc": C + 0x10, "ctr": 4})
    T("bdnzt LT clear not taken, ctr still decrements", 0x41000010, setup={"ctr": 5},
      expect={"pc": C + 4, "ctr": 4})
    T("bdnzf ctr 1->0 not taken", 0x40000010, setup={"ctr": 1}, expect={"pc": C + 4, "ctr": 0})
    T("bdzf ctr 1->0 cond false taken", 0x40400010, setup={"ctr": 1},
      expect={"pc": C + 0x10, "ctr": 0})
    T("bdzt BI=31 taken", 0x415F0010, setup={"cr": 1, "ctr": 1}, expect={"pc": C + 0x10, "ctr": 0})
    T("bdzt BI=31 cond false", 0x415F0010, setup={"cr": 0xFFFFFFFE, "ctr": 1},
      expect={"pc": C + 4, "ctr": 0})
    T("branch always BO=20 ctr untouched", 0x42800010, setup={"ctr": 0, "cr": 0xFFFFFFFF},
      expect={"pc": C + 0x10})
    T("branch always BO=31 (z bits set)", 0x43E00010, setup={"ctr": 7}, expect={"pc": C + 0x10})
    T("BO=11010 (1z01y z=1) ctr1->0 taken", 0x43400010, setup={"ctr": 1},
      expect={"pc": C + 0x10, "ctr": 0})
    T("BO=11001 (1z00y y=1) ctr3->2 taken", 0x43200010, setup={"ctr": 3},
      expect={"pc": C + 0x10, "ctr": 2})
    T("BO=00110 (001zy z=1) cond false taken, no ctr", 0x40C20010, setup={"ctr": 9},
      expect={"pc": C + 0x10})
    T("bcl not taken still sets LR", 0x41820011, setup={"cr": 0}, expect={"pc": C + 4, "lr": C + 4})
    T("bca negative absolute", 0x4280FFFE, expect={"pc": 0xFFFFFFFC})
    T("bc most negative BD", 0x42808000, expect={"pc": 0x7FFFB100})
    T("bcla max positive BD", 0x42807FFF, expect={"pc": 0x00007FFC, "lr": C + 4})

    # ---------------- bclr (PAGE 363-364) ----------------
    T("blr clears low bits", 0x4E800020, setup={"lr": 0x80001237}, expect={"pc": 0x80001234})
    T("blrl target read before LR update", 0x4E800021, setup={"lr": 0x80004000},
      expect={"pc": 0x80004000, "lr": C + 4})
    T("bdnzlr ctr1->0 not taken", 0x4E000020, setup={"ctr": 1, "lr": 0x80005000},
      expect={"pc": C + 4, "ctr": 0})
    T("beqlrl not taken sets LR", 0x4D820021, setup={"lr": 0x80005000},
      expect={"pc": C + 4, "lr": C + 4})
    T("bdzlrl taken", 0x4E400021, setup={"ctr": 1, "lr": 0x80005000},
      expect={"pc": 0x80005000, "ctr": 0, "lr": C + 4})
    T("bclr reserved bits 16-20", 0x4E800820, ub="full")

    # ---------------- bcctr (PAGE 361-362) ----------------
    T("bctr clears low bits", 0x4E800420, setup={"ctr": 0x80002003},
      expect={"pc": 0x80002000})
    T("bctrl", 0x4E800421, setup={"ctr": 0x80002000}, expect={"pc": 0x80002000, "lr": C + 4})
    T("bnectr not taken", 0x4C820420, setup={"cr": 0x20000000, "ctr": 0x80002000},
      expect={"pc": C + 4})
    T("bnectrl not taken sets LR", 0x4C820421, setup={"cr": 0x20000000, "ctr": 0x80002000},
      expect={"pc": C + 4, "lr": C + 4})
    T("bcctr BO[2]=0 invalid form", 0x4E000420, setup={"ctr": 5}, ub="full")
    T("bcctr reserved bits", 0x4E800C20, ub="full")

    # ---------------- sc (PAGE 537, PAGE 166-168) ----------------
    m = 0x0000B032
    T("sc", 0x44000002, setup={"msr": m},
      expect={"srr0": C + 4, "srr1": 0x0000B032, "msr": 0x00001000, "pc": 0xC00,
              "exception": "syscall"}, ub={"srr1": 0x783F0000})
    m = MSR_ILE | MSR_IP | MSR_EE
    T("sc IP=1 ILE=1", 0x44000002, setup={"msr": m},
      expect={"srr0": C + 4, "srr1": 0x00008040, "msr": 0x00010041, "pc": 0xFFF00C00,
              "exception": "syscall"}, ub={"srr1": 0x783F0000})
    T("sc with MSR[SE] no trace, PM saved and cleared", 0x44000002, setup={"msr": 0x00001436},
      expect={"srr0": C + 4, "srr1": 0x00001436, "msr": 0x00001000, "pc": 0xC00,
              "exception": "syscall"}, ub={"srr1": 0x783F0000})
    T("opcode 17 bit30=0: illegal or boundedly undefined", 0x44000000, setup={"msr": base_msr},
      ub="full")
    T("sc reserved bits", 0x44000012, ub="full")
    T("sc bit 31 set", 0x44000003, ub="full")

    # ---------------- rfi (PAGE 532) ----------------
    T("rfi basic", 0x4C000064, setup={"msr": 0x1000, "srr0": 0x80001237, "srr1": 0x0000B032},
      expect={"msr": 0x0000B032, "pc": 0x80001234})
    T("rfi clears POW", 0x4C000064, setup={"msr": 0x00041000, "srr1": 0x00001000, "srr0": 0x100},
      expect={"msr": 0x00001000, "pc": 0x100})
    T("rfi leaves unassigned bit 1", 0x4C000064, setup={"msr": 0x40000000, "srr1": 0, "srr0": 4},
      expect={"msr": 0x40000000, "pc": 4})
    T("rfi PM differs -> undefined bit 29", 0x4C000064,
      setup={"msr": 0x1000, "srr1": 0x00001004, "srr0": 8},
      expect={"msr": 0x00001000, "pc": 8}, ub={"msr": 0x4})
    T("rfi SRR1 all ones", 0x4C000064, setup={"msr": 0, "srr1": 0xFFFFFFFF, "srr0": 0xFFFFFFFF},
      expect={"msr": 0x87C0FF73, "pc": 0xFFFFFFFC}, ub={"msr": 0x8C})
    T("rfi in user mode", 0x4C000064, setup={"msr": base_msr | MSR_PR, "srr1": 0, "srr0": 0},
      expect=prog_expect(base_msr | MSR_PR), ub=PROG_UB)
    T("rfi reserved bit 31", 0x4C000065, ub="full")
    T("rfi with MSR[SE] not traced", 0x4C000064,
      setup={"msr": MSR_SE, "srr1": 0x2032, "srr0": 0x80000000},
      expect={"msr": 0x2032, "pc": 0x80000000})

    # ---------------- tw / twi (PAGE 578-579) ----------------
    T("twi eq traps", 0x0C830005, setup={"gpr": {3: 5}}, expect=trap_expect(base_msr), ub=TRAP_UB)
    T("twi eq no trap", 0x0C830006, setup={"gpr": {3: 5}}, expect={"pc": C + 4})
    T("twi lt signed traps", 0x0E030000, setup={"gpr": {3: 0xFFFFFFFF}},
      expect=trap_expect(base_msr), ub=TRAP_UB)
    T("twi <U no trap for -1 vs 0", 0x0C430000, setup={"gpr": {3: 0xFFFFFFFF}},
      expect={"pc": C + 4})
    T("twi >U traps for -1 vs 0", 0x0C230000, setup={"gpr": {3: 0xFFFFFFFF}},
      expect=trap_expect(base_msr), ub=TRAP_UB)
    T("twi <U SIMM sign-extended", 0x0C43FFFF, setup={"gpr": {3: 0x7FFFFFFF}},
      expect=trap_expect(base_msr), ub=TRAP_UB)
    T("twi > signed vs -1", 0x0D03FFFF, setup={"gpr": {3: 0x7FFFFFFF}},
      expect=trap_expect(base_msr), ub=TRAP_UB)
    T("twi TO=0 never traps", 0x0C000000, expect={"pc": C + 4})
    T("trap (tw 31,0,0) with IP/ILE", 0x7FE00008, setup={"msr": 0x00013032},
      expect=trap_expect(0x00013032), ub=TRAP_UB)
    T("tw lt|gt aliasing rA=rB no trap", 0x7F042008, setup={"gpr": {4: 0x12345678}},
      expect={"pc": C + 4})
    T("tw lt|eq signed traps", 0x7E832008, setup={"gpr": {3: 0x80000000, 4: 0x7FFFFFFF}},
      expect=trap_expect(base_msr), ub=TRAP_UB)
    T("tw >U traps", 0x7C232008, setup={"gpr": {3: 0x80000000, 4: 0x7FFFFFFF}},
      expect=trap_expect(base_msr), ub=TRAP_UB)
    T("tw gt|<U no trap", 0x7D432008, setup={"gpr": {3: 0x80000000, 4: 0x7FFFFFFF}},
      expect={"pc": C + 4})
    T("tw reserved bit 31", 0x7FE00009, ub="full")
    T("tw no trap with SE -> trace entry", 0x7C000008, setup={"msr": base_msr | MSR_SE},
      expect=trace_expect(base_msr | MSR_SE, C + 4), ub=PROG_UB)
    T("tw trapping with SE -> trap only (trace forgotten, PAGE 161)", 0x7FE00008,
      setup={"msr": base_msr | MSR_SE}, expect=trap_expect(base_msr | MSR_SE), ub=TRAP_UB)

    # ---------------- mtmsr / mfmsr (PAGE 461, 475) ----------------
    T("mtmsr r3", 0x7C600124, setup={"msr": 0x1000, "gpr": {3: 0x0000B032}},
      expect={"msr": 0x0000B032, "pc": C + 4})
    T("mfmsr r5", 0x7CA000A6, setup={"msr": 0x0000B032}, expect={"gpr": {5: 0x0000B032}, "pc": C + 4})
    T("mtmsr user mode", 0x7C600124, setup={"msr": base_msr | MSR_PR, "gpr": {3: 0}},
      expect=prog_expect(base_msr | MSR_PR), ub=PROG_UB)
    T("mfmsr user mode", 0x7CA000A6, setup={"msr": base_msr | MSR_PR},
      expect=prog_expect(base_msr | MSR_PR), ub=PROG_UB)
    T("mtmsr partial-function bit 1", 0x7C600124, setup={"gpr": {3: 0x40002032}},
      expect={"msr": 0x00002032, "pc": C + 4}, ub={"msr": 0x40000000})
    T("mtmsr full-function bit 0 stored", 0x7C600124, setup={"gpr": {3: 0x80002032}},
      expect={"msr": 0x80002032, "pc": C + 4})
    T("mtmsr LE change", 0x7C600124, setup={"gpr": {3: 0x00002033}},
      expect={"msr": 0x00002033, "pc": C + 4}, ub={"le_effective_point": True})
    T("mtmsr POW with DOZE", 0x7C600124, setup={"gpr": {3: 0x00042032}, "hid0": 0x00800000},
      expect={"msr": 0x00042032, "pc": C + 4}, ub={"power_mode": True})
    T("mtmsr POW without power mode", 0x7C600124, setup={"gpr": {3: 0x00042032}},
      expect={"msr": 0x00042032, "pc": C + 4})
    T("mtmsr reserved bit 12", 0x7C680124, ub="full")
    T("mtmsr EE=1 with pending does not deliver", 0x7C600124,
      setup={"msr": 0x2032, "gpr": {3: 0xA032}, "pending": {"external": True, "decrementer": False}},
      expect={"msr": 0xA032, "pc": C + 4})
    T("mtmsr setting SE: traced or not undefined", 0x7C600124, setup={"gpr": {3: 0x2432}}, ub="full")
    T("mtmsr clearing SE: traced or not undefined", 0x7C600124,
      setup={"msr": 0x2432, "gpr": {3: 0x2032}}, ub="full")
    T("mtmsr SE stays 1: traced with the NEW MSR in SRR1", 0x7C600124,
      setup={"msr": 0x2432, "gpr": {3: 0xA432}},
      expect=trace_expect(0xA432, C + 4), ub=PROG_UB)
    T("mtmsr bit 24 then trace: SRR1[24] undefined", 0x7C600124,
      setup={"msr": 0x2432, "gpr": {3: 0x24B2}},
      expect=trace_expect(0x2432, C + 4), ub={"msr": 0x80, "srr1": 0x783F0080})
    T("mfmsr partial-function bits read undefined", 0x7CA000A6, setup={"msr": 0x48002032},
      expect={"gpr": {5: 0x00002032}, "pc": C + 4}, ub={"gpr[5]": 0x48000000})
    T("mfmsr full-function bits 0/28 read defined", 0x7CA000A6, setup={"msr": 0x8000203A},
      expect={"gpr": {5: 0x8000203A}, "pc": C + 4})

    # ---------------- mtspr / mfspr (PAGE 462-479) ----------------
    T("mtlr r0 (0x7C0803A6)", 0x7C0803A6, setup={"gpr": {0: 0x80001234}},
      expect={"lr": 0x80001234, "pc": C + 4})
    T("mflr r0 (0x7C0802A6)", 0x7C0802A6, setup={"lr": 0x80004444},
      expect={"gpr": {0: 0x80004444}, "pc": C + 4})
    T("mtctr r3", 0x7C6903A6, setup={"gpr": {3: 0xDEADBEEF}}, expect={"ctr": 0xDEADBEEF, "pc": C + 4})
    T("mtxer defined bits", 0x7C8103A6, setup={"gpr": {4: 0xF000FF7F}},
      expect={"xer": 0xF000FF7F, "pc": C + 4})
    T("mtxer reserved bit 10", 0x7C8103A6, setup={"gpr": {4: 0x20200000}},
      expect={"xer": 0x20000000, "pc": C + 4}, ub={"xer": 0x00200000})
    T("mfxer user mode ok", 0x7C6102A6, setup={"msr": base_msr | MSR_PR, "xer": 0x20000000},
      expect={"gpr": {3: 0x20000000}, "pc": C + 4})
    T("mtsrr0 r3", 0x7C7A03A6, setup={"gpr": {3: 0x80001000}}, expect={"srr0": 0x80001000, "pc": C + 4})
    T("mfsrr1 r3", 0x7C7B02A6, setup={"srr1": 0x0000B032}, expect={"gpr": {3: 0xB032}, "pc": C + 4})
    T("mtsprg2 r5", 0x7CB243A6, setup={"gpr": {5: 0x11112222}},
      expect={"sprg": [0, 0, 0x11112222, 0], "pc": C + 4})
    T("mtdsisr / mtdar", 0x7C7203A6, setup={"gpr": {3: 0x42000000}},
      expect={"dsisr": 0x42000000, "pc": C + 4})
    T("mtdar", 0x7C7303A6, setup={"gpr": {3: 0x80000123}}, expect={"dar": 0x80000123, "pc": C + 4})
    T("mfhid2 r3", 0x7C78E2A6, setup={"hid2": 0xE0000000}, expect={"gpr": {3: 0xE0000000}, "pc": C + 4})
    T("mtgqr0 legal bits", 0x7C10E3A6, setup={"gpr": {0: 0x00040004}},
      expect={"gqr": [0x00040004, 0, 0, 0, 0, 0, 0, 0], "pc": C + 4})
    T("mtgqr7 reserved bit 0", 0x7C17E3A6, setup={"gpr": {0: 0x80070007}},
      expect={"gqr": [0, 0, 0, 0, 0, 0, 0, 0x00070007], "pc": C + 4}, ub={"gqr[7]": 0x80000000})
    T("mtdec MSB 0->1: pending undefined", 0x7C7603A6, setup={"gpr": {3: 0x80000000}, "dec": 0x10},
      expect={"dec": 0x80000000, "pc": C + 4}, ub={"pending.decrementer": True})
    T("mtdec MSB stays 0", 0x7C7603A6, setup={"gpr": {3: 0x7FFFFFFF}, "dec": 0x10},
      expect={"dec": 0x7FFFFFFF, "pc": C + 4})
    T("mtdec while a decrementer condition is pending", 0x7C7603A6,
      setup={"gpr": {3: 0x7FFFFFFF}, "dec": 0x10, "pending": {"external": False, "decrementer": True}},
      expect={"dec": 0x7FFFFFFF, "pc": C + 4}, ub={"pending.decrementer": True})
    T("mfxer reserved bits read undefined, bit 3 defined", 0x7C6102A6, setup={"xer": 0x1F20FF7F},
      expect={"gpr": {3: 0x1000FF7F}, "pc": C + 4}, ub={"gpr[3]": 0x0F200000})
    T("mfgqr1 reserved bits read undefined", 0x7C71E2A6, setup={"gqr": [0, 0xFFFFFFFF, 0, 0, 0, 0, 0, 0]},
      expect={"gpr": {3: 0x3F073F07}, "pc": C + 4}, ub={"gpr[3]": 0xC0F8C0F8})
    T("mtpvr supervisor no-op (PAGE 90)", _sprw(467, 3, 287), setup={"gpr": {3: 0x12345678}},
      expect={"pc": C + 4})
    T("mthid1 supervisor no-op (PAGE 90)", _sprw(467, 3, 1009), expect={"pc": C + 4})
    T("mtpvr user -> privileged program", _sprw(467, 3, 287), setup={"msr": base_msr | MSR_PR},
      expect=prog_expect(base_msr | MSR_PR), ub=PROG_UB)
    T("mtsda supervisor no-op (PAGE 61)", _sprw(467, 3, 959), expect={"pc": C + 4})
    T("mtusda user: undefined", _sprw(467, 3, 943), setup={"msr": base_msr | MSR_PR}, ub="full")
    T("mfusda: read value unspecified", _sprw(339, 3, 943), ub="full")
    T("mtummcr0: table conflict", _sprw(467, 3, 936), setup={"msr": base_msr | MSR_PR}, ub="full")
    T("mfibat0u user -> privileged program", _sprw(339, 3, 528), setup={"msr": base_msr | MSR_PR},
      expect=prog_expect(base_msr | MSR_PR), ub=PROG_UB)
    T("mfibat0u supervisor not modelled", _sprw(339, 3, 528), ub="notmodelled")
    T("mfspr Rc=1 with undefined SPR: CR untouched", _sprw(339, 3, 0, rc=1), setup={"cr": 0xF0000000},
      expect=prog_expect(base_msr), ub=PROG_UB)
    T("mttbl", 0x7C7C43A6, setup={"gpr": {3: 0xAABBCCDD}, "tb": 0x1122334455667788},
      expect={"tb": 0x11223344AABBCCDD, "pc": C + 4})
    T("mttbu", 0x7C7D43A6, setup={"gpr": {3: 0xAABBCCDD}, "tb": 0x1122334455667788},
      expect={"tb": 0xAABBCCDD55667788, "pc": C + 4})
    T("mftb r3 (XO 371, TBL)", 0x7C6C42E6, setup={"tb": 0x1122334455667788},
      expect={"gpr": {3: 0x55667788}, "pc": C + 4})
    T("mfspr 269 (TBU) user mode", 0x7C6D42A6, setup={"tb": 0x1122334455667788, "msr": base_msr | MSR_PR},
      expect={"gpr": {3: 0x11223344}, "pc": C + 4})
    T("mftb XO371 with SPR 8 == mflr", 0x7C0802E6, setup={"lr": 0x1234},
      expect={"gpr": {0: 0x1234}, "pc": C + 4})
    T("mfspr 284 undefined for mfspr", _sprw(339, 3, 284), expect=prog_expect(base_msr), ub=PROG_UB)
    T("mtspr 268 undefined for mtspr", _sprw(467, 3, 268), expect=prog_expect(base_msr), ub=PROG_UB)
    T("mtsprg0 user mode privileged", 0x7C7043A6, setup={"msr": base_msr | MSR_PR},
      expect=prog_expect(base_msr | MSR_PR), ub=PROG_UB)
    T("mfspr SPR 0 undefined", 0x7C6002A6, expect=prog_expect(base_msr), ub=PROG_UB)
    T("mtlr Rc=1 CR0 undefined", 0x7C0803A7, setup={"gpr": {0: 0x80001234}, "cr": 0xF2345678},
      expect={"lr": 0x80001234, "cr": 0x02345678, "pc": C + 4}, ub={"cr": 0xF0000000})
    T("mthid0 ICFI", 0x7C70FBA6, setup={"gpr": {3: 0x00110C00}},
      expect={"hid0": 0x00110000, "pc": C + 4}, ub={"hid0": 0x00000C00})
    T("mthid2 PSE off, DMAQL read-only", 0x7C78E3A6, setup={"gpr": {3: 0xAF000000}, "hid2": 0xE0000000},
      expect={"hid2": 0xA0000000, "pc": C + 4})
    T("mthid2 sticky bit write differs", 0x7C78E3A6, setup={"gpr": {3: 0xE0800000}, "hid2": 0xE0000000},
      expect={"hid2": 0xE0000000, "pc": C + 4}, ub={"hid2": 0x00800000})

    # ---------------- decode / supports ----------------
    checks_ok = True
    for w, exp in ((0x48000000, True), (0x40000000, True), (0x4E800020, True), (0x4E800420, True),
                   (0x4C000064, True), (0x44000002, True), (0x0C000000, True), (0x7FE00008, True),
                   (0x7C0803A6, True), (_sprw(467, 3, 528), True), (_sprw(339, 3, 287), True),
                   (0x4C000020 | (33 << 1), False), (0x7C000000, False), (0x38600000, False)):
        if supports(w) != exp:
            print(f"FAIL supports({w:#010x}) != {exp}")
            checks_ok = False

    fails = 0
    for name, word, setup, expect, ub in vectors:
        st = State()
        for k, v in setup.items():
            if k == "gpr":
                for i, x in v.items():
                    st.gpr[i] = x
            else:
                setattr(st, k, v)
        st.pc = setup.get("pc", C)
        before = _shadow(st)
        exp = _shadow(st)
        exp.exception = None
        for k, v in expect.items():
            if k == "gpr":
                for i, x in v.items():
                    exp.gpr[i] = x
            else:
                setattr(exp, k, v)
        try:
            step(st, word)
            raised = None
        except UndefinedBehaviour as u:
            raised = u
        except NotModelled:
            ok = ub == "notmodelled" and all(getattr(st, f) == getattr(before, f) for f in _REG_FIELDS)
            if not ok:
                print(f"FAIL {name}: unexpected NotModelled or state mutated")
                fails += 1
            continue
        if ub == "notmodelled":
            print(f"FAIL {name}: expected NotModelled")
            fails += 1
            continue
        if ub is None:
            if raised is not None:
                print(f"FAIL {name}: unexpected UB: {raised.what} {raised.undefined}")
                fails += 1
                continue
            got = st
        else:
            if raised is None:
                print(f"FAIL {name}: expected UndefinedBehaviour")
                fails += 1
                continue
            unchanged = all(getattr(st, f) == getattr(before, f) for f in _REG_FIELDS)
            if not unchanged:
                print(f"FAIL {name}: state mutated on UB")
                fails += 1
                continue
            if ub == "full":
                if raised.partial is not None or raised.undefined != {"all": True}:
                    print(f"FAIL {name}: expected full UB, got {raised.undefined}")
                    fails += 1
                continue
            if raised.undefined != ub:
                print(f"FAIL {name}: undefined map {raised.undefined} != {ub}")
                fails += 1
                continue
            got = raised.partial
        bad = [f for f in _REG_FIELDS if getattr(got, f) != getattr(exp, f)]
        if bad:
            print(f"FAIL {name}: " + ", ".join(
                f"{f}: got {getattr(got, f)!r} want {getattr(exp, f)!r}" for f in bad
                if not isinstance(getattr(got, f), list)) + " " +
                ", ".join(f for f in bad if isinstance(getattr(got, f), list)))
            fails += 1
    total = len(vectors)
    print(f"branch_A self-test: {total - fails}/{total} vectors passed; supports checks "
          f"{'ok' if checks_ok else 'FAILED'}")
    return fails == 0 and checks_ok


if __name__ == "__main__":
    sys.exit(0 if _run_tests() else 1)
