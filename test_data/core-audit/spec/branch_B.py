"""Independent specification model, area "branch", author B.

Sources: ONLY docs/reference/ppc/gekko_user_manual.txt (and ppc_750cl.txt where noted).
"PAGE N" = the "===== PAGE N =====" marker in gekko_user_manual.txt that precedes the text.
Big-endian bit numbering in comments: bit 0 = 0x80000000, bit 31 = 0x00000001.

Instructions covered
    b ba bl bla, bc bca bcl bcla, bclr bclrl, bcctr bcctrl, sc, rfi, tw, twi,
    mfmsr, mtmsr, mfspr, mtspr (and mftb, which Gekko decodes identically to mfspr, PAGE 120).

Contract
    step(state, word) executes the instruction at state.pc.  It raises
        UndefinedBehaviour  - the manual leaves the outcome (control flow / whether an exception
                              is taken / a whole register value) undefined or self-contradictory;
        NotModelled         - (subclass of UndefinedBehaviour) the behaviour is defined but needs
                              a register gekko_state.State does not carry (BATs, SDR1, PMCs, ...).
    When step raises, `state` is left untouched (execution happens on a clone first).
    Where only SOME BITS of the result are undefined, step still produces a deterministic
    reference value and records the undefined bits in state.undefined_bits, a dict of
        'gpr[N]' / 'msr' / 'srr0' / 'srr1' / 'xer' / 'cr' / 'hid0' / 'hid2' / 'gqr[N]' -> int mask
        'pending.decrementer'                                                      -> True
        'le_effective_point' / 'power_mode' (annotations: no register of this step) -> True
    A comparator must ignore those bits.  undefined_bits(state, word) returns the same dict
    without modifying state.
    The post-instruction trace exception (MSR[SE], or MSR[BE] after a branch) is taken inside step:
    exception 'trace', vector 0xD00, SRR0 = the next instruction (PAGE 161/163/176).

CROSS-CHECK FIXES (crosscheck_branch.py against branch_A, 2026-09-14) are marked "XCHK:".

Exception naming: 'program' (illegal / privileged type), 'trap' (trap type program exception,
same vector 0x700), 'syscall'.  EXCEPTION_ALIASES maps 'trap' -> 'program' for comparators
that do not distinguish the program-exception subtypes.

Not in this area (left to the exception area's take_pending): delivery of asynchronous
exceptions that become enabled by mtmsr (PAGE 475) or rfi (PAGE 532).  step only records
the new MSR; it never consumes state.pending.
"""
from __future__ import annotations

try:
    from gekko_state import State, MASK32  # noqa: F401
except ImportError:  # allow running from another cwd
    import os
    import sys
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from gekko_state import State, MASK32  # noqa: F401

AREA = "branch"
M = 0xFFFFFFFF
EXCEPTION_ALIASES = {"trap": "program"}


class UndefinedBehaviour(Exception):
    """The manual leaves this behaviour undefined, implementation-specific, or contradictory."""


class NotModelled(UndefinedBehaviour):
    """Defined by the manual but needs state that gekko_state.State does not carry."""


def _bit(b: int) -> int:
    """Big-endian bit b of a 32-bit register."""
    return 1 << (31 - b)


def _bits(*ranges) -> int:
    m = 0
    for r in ranges:
        lo, hi = (r, r) if isinstance(r, int) else r
        for b in range(lo, hi + 1):
            m |= _bit(b)
    return m


# ----------------------------------------------------------------------------------------------
# MSR layout - Table 4-4 PAGE 164-165, Table 2-1 PAGE 58
MSR_POW = _bit(13)   # PAGE 164 "13 POW Power management enable"
MSR_ILE = _bit(15)   # PAGE 164 "15 ILE Exception little-endian mode"
MSR_EE = _bit(16)    # PAGE 164
MSR_PR = _bit(17)    # PAGE 164 "1 The processor can only execute user-level instructions."
MSR_FP = _bit(18)    # PAGE 164
MSR_ME = _bit(19)    # PAGE 164
MSR_FE0 = _bit(20)   # PAGE 164
MSR_SE = _bit(21)    # PAGE 164 single-step trace enable
MSR_BE = _bit(22)    # PAGE 164 branch trace enable
MSR_FE1 = _bit(23)   # PAGE 164
MSR_IP = _bit(25)    # PAGE 165 exception prefix
MSR_IR = _bit(26)    # PAGE 165
MSR_DR = _bit(27)    # PAGE 165
MSR_PM = _bit(29)    # PAGE 165 (Gekko-specific)
MSR_RI = _bit(30)    # PAGE 165
MSR_LE = _bit(31)    # PAGE 165
# PAGE 164-165: bit 0, 5-9, 28 "Reserved. Full function"; 1-4, 10-12 "Reserved. Partial function";
# 14 "Reserved. Implementation-specific"; 24 "Reserved. This bit corresponds to the AL bit".
MSR_RES_FULL = _bits(0, (5, 9), 28)
MSR_RES_ALL = _bits((0, 12), 14, 24, 28)
# Reserved bits whose storage the manual does not settle.  Full function bits 0 and 5-9 are
# saved to SRR1 (PAGE 165 note) and restored by rfi (PAGE 532 RTL), so they are treated as real
# storage.
# XCHK (was: bit 28 uncertain too, because rfi's RTL omits it).  Table 4-4 PAGE 165 "28 -
# Reserved. Full function" and the PAGE 165 note "Full function reserved bits are saved in SRR1
# when an exception occurs" make bit 28 real storage, exactly like 0 and 5-9; whether rfi RESTORES
# it is a separate question handled by RFI_UNCERTAIN, not a question of what mtmsr/mfmsr store.
MSR_RES_UNCERTAIN = _bits((1, 4), (10, 12), 14, 24)

# SRR1 on exception entry - PAGE 166 steps 2 and 3
SRR1_EXC_INFO = _bits((1, 4), (10, 15))   # "SRR1[1-4, 10-15] are loaded with information specific to the exception type"
SRR1_MSR_COPY = _bits(0, (5, 9), (16, 31))  # "SRR1[5-9, 16-31] are loaded with a copy of the corresponding MSR bits"
# PAGE 166 "Depending on the implementation, reserved bits may not be copied." Full-function reserved
# bits ARE saved (PAGE 165 note) -> only reserved bit 24 inside the copy range is uncertain.
SRR1_COPY_UNCERTAIN = _bit(24)

# rfi restore list - PAGE 532 "MSR[0,5-9,16-23, 25-27, 30-31] <- SRR1[0,5-9,16-23, 25-27, 30-31]"
RFI_RESTORE = _bits(0, (5, 9), (16, 23), (25, 27), (30, 31))
# PAGE 532 "an implementation may define additional MSR bits, and in this case, may also cause them
# to be ... restored to MSR from SRR1 on an rfi" -> PM (29) and full-function bit 28 uncertain.
# XCHK (was: bit 24 left unaltered as defined).  PAGE 166 step 3 copies MSR[16-31] - bit 24 included -
# into SRR1 "Depending on the implementation, reserved bits may not be copied", and the PAGE 532 note
# lets an implementation restore what it saves; Table 4-4 PAGE 164 gives bit 24 no function ("This bit
# corresponds to the AL bit of the POWER architecture").  Whether rfi restores it is not stated.
RFI_UNCERTAIN = _bits(24, 28, 29)

# Exception vectors - Table 4-2 PAGE 159
VEC_PROGRAM = 0x00700
VEC_SYSCALL = 0x00C00
VEC_TRACE = 0x00D00

# XER - PAGE 57 "XER[16-23] is implemented so that they can be read with mfspr[XER] and written";
# XER[0-3] used by mcrxr (PAGE 460 area), XER[25-31] byte count (lswx, PAGE 553 area).
# The manual never lists XER's reserved bits; bits 4-15 and 24 are the ones it does not describe as
# implemented, so a nonzero write/read there is uncertain.
# XCHK (was: bits 3-15).  mcrxr PAGE 458 "CR[4 * crfD, 4 * crfD + 3] <- XER[0-3]", "XER[0-3] <- 0b0000"
# and "Other registers altered: XER[0-3]" read and write XER bit 3 as a register bit, so it is storage.
XER_UNCERTAIN = _bits((4, 15), 24)

# HID0 - Table 2-4 PAGE 63-66
HID0_ICE = _bit(16)   # PAGE 64
HID0_DCE = _bit(17)   # PAGE 64
HID0_ICFI = _bit(20)  # PAGE 65
HID0_DCFI = _bit(21)  # PAGE 65
# PAGE 63 "4 BCLK Reserved. Must set to 0." "5 - Not used" "6 ECLK Reserved. Must set to 0."
# PAGE 64 "12-14 - Not used"; PAGE 66 "27 - Not used", "30 - Not used"
HID0_UNCERTAIN = _bits((4, 6), (12, 14), 27, 30)

# HID2 - Table 2-6 PAGE 67-68
HID2_DMAQL = _bits((4, 7))     # PAGE 68 "4-7 DMAQL DMA queue length (read only)"
HID2_STICKY = _bits((8, 11))   # PAGE 68 "(sticky)" error bits; how software clears them is not stated
HID2_RESERVED = _bits((16, 31))  # PAGE 68 "16-31 - Reserved."

# GQR - Table 2-16 PAGE 78: reserved 0-1, 8-12, 16-17, 24-28
GQR_RESERVED = _bits((0, 1), (8, 12), (16, 17), (24, 28))

DEC_MSB = _bit(0)


# ----------------------------------------------------------------------------------------------
# SPR table.  n = spr[5-9] || spr[0-4] (PAGE 462 "n <- spr[5-9] || spr[0-4]"; PAGE 117 "the high-order
# five bits appearing in bits 16-20 of the instruction and the low-order five bits in bits 11-15").
# Access per Table 12-10 (mfspr, PAGE 463-465), Table 12-13 (mtspr, PAGE 477-479), Table 2-52/2-53
# (PAGE 115-118).  'U' user, 'S' supervisor, None = not a valid SPR for that instruction.
_SPR = {}


def _spr(n, name, rd, wr):
    _SPR[n] = (name, rd, wr)


_spr(1, "XER", "U", "U")
_spr(8, "LR", "U", "U")
_spr(9, "CTR", "U", "U")
_spr(18, "DSISR", "S", "S")
_spr(19, "DAR", "S", "S")
_spr(22, "DEC", "S", "S")
_spr(25, "SDR1", "S", "S")
_spr(26, "SRR0", "S", "S")
_spr(27, "SRR1", "S", "S")
for _i in range(4):
    _spr(272 + _i, "SPRG%d" % _i, "S", "S")
_spr(282, "EAR", "S", "S")
# PAGE 117 note 2: "The TB registers can be read in user mode using either the mftb or mfspr instruction
# and specifying TBR 268 for TBL and SPR 269 for TBU"; written with mtspr 284/285 (Table 12-13 PAGE 477).
# Table 2-52 PAGE 116: 268/269 "mfspr" only, 284/285 "mtspr" only.
_spr(268, "TBL", "U", None)
_spr(269, "TBU", "U", None)
_spr(284, "TBL", None, "S")
_spr(285, "TBU", None, "S")
# PAGE 116 "PVR 287 ... Supervisor (OEA) mfspr"; PAGE 90 "An mtspr instruction executing in supervisor mode
# (MSR[PR] = 0) with the SPR field specifying HID1 or PVR (read-only registers) executes as a no-op."
_spr(287, "PVR", "S", "S")
for _i in range(4):
    _spr(528 + 2 * _i, "IBAT%dU" % _i, "S", "S")
    _spr(529 + 2 * _i, "IBAT%dL" % _i, "S", "S")
    _spr(536 + 2 * _i, "DBAT%dU" % _i, "S", "S")
    _spr(537 + 2 * _i, "DBAT%dL" % _i, "S", "S")
for _i in range(8):
    _spr(912 + _i, "GQR%d" % _i, "S", "S")
_spr(920, "HID2", "S", "S")
_spr(921, "WPAR", "S", "S")
_spr(922, "DMAU", "S", "S")
_spr(923, "DMAL", "S", "S")
for _n, _nm in ((936, "UMMCR0"), (937, "UPMC1"), (938, "UPMC2"), (939, "USIA"), (940, "UMMCR1"),
                (941, "UPMC3"), (942, "UPMC4")):
    _spr(_n, _nm, "U", "U")  # write access CONFLICTS between tables, see _mtspr
_spr(943, "USDA", "U", "U")
for _n, _nm in ((952, "MMCR0"), (953, "PMC1"), (954, "PMC2"), (955, "SIA"), (956, "MMCR1"),
                (957, "PMC3"), (958, "PMC4"), (959, "SDA")):
    _spr(_n, _nm, "S", "S")
_spr(1008, "HID0", "S", "S")
_spr(1009, "HID1", "S", "S")
_spr(1010, "IABR", "S", "S")
_spr(1013, "DABR", "S", "S")  # Table 2-53 PAGE 117 says "User" but Tables 12-10/12-13 and the SPR[0] rule say Supervisor
_spr(1017, "L2CR", "S", "S")
_spr(1019, "ICTC", "S", "S")
_spr(1020, "THRM1", "S", "S")
_spr(1021, "THRM2", "S", "S")
_spr(1022, "THRM3", "S", "S")

# SPRs the neutral State carries
_MODELLED = {1, 8, 9, 18, 19, 22, 26, 27, 272, 273, 274, 275, 268, 269, 284, 285,
             912, 913, 914, 915, 916, 917, 918, 919, 920, 1008}


# ----------------------------------------------------------------------------------------------
# field helpers
def _op(word):
    return (word >> 26) & 0x3F


def _xo(word):  # bits 21-30
    return (word >> 1) & 0x3FF


def _f5(word, first_bit):  # 5-bit field starting at big-endian bit first_bit
    return (word >> (27 - first_bit)) & 0x1F


def _exts(value, nbits):
    value &= (1 << nbits) - 1
    return value - (1 << nbits) if value & (1 << (nbits - 1)) else value


def _s32(v):
    return _exts(v, 32)


def _cr_bit(st, bi):
    return (st.cr >> (31 - bi)) & 1


def _und_or(und, key, mask):
    if mask:
        und[key] = und.get(key, 0) | mask


# ----------------------------------------------------------------------------------------------
# exception entry
def _enter_exception(st, und, srr0, vector, name, srr0_mask=0):
    old = st.msr
    # PAGE 166 step 1 "SRR0 is loaded with an instruction address that depends on the type of exception"
    st.srr0 = srr0 & M
    _und_or(und, "srr0", srr0_mask)
    # PAGE 166 step 2/3; reference value: exception-specific bits 0 (their content is defined only in the
    # Programming Environments manual Ch.6, which PAGE 175/176 defer to and which is not a source here)
    st.srr1 = old & SRR1_MSR_COPY
    _und_or(und, "srr1", SRR1_EXC_INFO | (old & SRR1_COPY_UNCERTAIN))
    # Table 4-6 PAGE 168: POW EE PR FP FE0 SE BE FE1 IR DR PM RI cleared; ILE ME IP "not altered";
    # LE "copied from the MSR[ILE]"; "Reserved bits are read as if written as 0."
    new = old & (MSR_ILE | MSR_ME | MSR_IP)
    if old & MSR_ILE:
        new |= MSR_LE
    st.msr = new
    _und_or(und, "msr", old & MSR_RES_ALL)
    # PAGE 166 step 5 / PAGE 168: IP=0 -> 0x000n_nnnn, IP=1 -> 0xFFFn_nnnn
    st.pc = ((0xFFF00000 if old & MSR_IP else 0) + vector) & M
    st.exception = name


def _program(st, und, cia):
    """Illegal-instruction or privileged-instruction type program exception.

    PAGE 46 "(excluding the trap and system call exceptions) the address of the faulting instruction is
    provided to the exception handler"; PAGE 161 "The exception is then taken without completing the
    exception-causing instruction."  Vector 0x700 (PAGE 175).  Illegal vs privileged differ only in
    SRR1[1-4,10-15], which are don't-care here, so both collapse to one model."""
    _enter_exception(st, und, cia, VEC_PROGRAM, "program")


def _trap(st, und, cia):
    # PAGE 113-114 "the system trap type program exception is taken"; vector 0x700 PAGE 175.
    # SRR0: PAGE 46 excludes trap from "the address of the faulting instruction is provided" and PAGE 163
    # says "or the next one (as in the case of a system call, trace, or trap exception)"; the register
    # settings themselves are deferred to the PEM (PAGE 175).  Reference = CIA+4, whole SRR0 undefined.
    _enter_exception(st, und, cia + 4, VEC_PROGRAM, "trap", srr0_mask=M)


# ----------------------------------------------------------------------------------------------
# branches
def _ctr_cond(st, bo, bi, allow_ctr=True):
    # PAGE 359 / PAGE 363 RTL:
    #   if not BO[2] then CTR <- CTR - 1
    #   ctr_ok <- BO[2] | ((CTR != 0) xor BO[3])
    #   cond_ok <- BO[0] | (CR[BI] == BO[1])
    # BO[0]=0x10 ... BO[4]=0x01 (BO occupies bits 6-10, BO[0] is its MSB).  z bits and the y bit (BO[4])
    # are ignored (Table 12-6 PAGE 359 "z indicates a bit that is ignored"; y is only a prediction hint).
    if not (bo & 0x04):
        # PAGE 363 "the entire 32-bit CTR is decremented"
        st.ctr = (st.ctr - 1) & M
    ctr_ok = bool(bo & 0x04) or ((st.ctr != 0) != bool(bo & 0x02))
    cond_ok = bool(bo & 0x10) or (_cr_bit(st, bi) == ((bo >> 3) & 1))
    return ctr_ok and cond_ok


def _b(st, word, und):
    cia = st.pc
    li = word & 0x03FFFFFC                      # LI (bits 6-29) || 0b00, PAGE 358
    disp = _exts(li, 26)                        # EXTS(LI || 0b00)
    aa, lk = (word >> 1) & 1, word & 1
    # PAGE 358 "if AA = 1 then NIA <- EXTS(LI || 0b00) else NIA <- CIA + EXTS(LI || 0b00)"
    st.pc = (disp if aa else cia + disp) & M
    if lk:  # PAGE 358 "if LK = 1 then LR <- CIA + 4"
        st.lr = (cia + 4) & M


def _bc(st, word, und):
    cia = st.pc
    bo, bi = _f5(word, 6), _f5(word, 11)
    disp = _exts(word & 0xFFFC, 16)            # EXTS(BD || 0b00), BD = bits 16-29, PAGE 359
    aa, lk = (word >> 1) & 1, word & 1
    taken = _ctr_cond(st, bo, bi)
    # PAGE 359-360 "If AA = 0, the branch target address is the sum of BD || 0b00 sign-extended and the
    # address of this instruction. If AA = 1, the branch target address is the value BD || 0b00 sign-extended."
    st.pc = ((disp if aa else cia + disp) if taken else cia + 4) & M
    # PAGE 360 "If LK = 1, the effective address of the instruction following the branch instruction is
    # placed into the link register." / "Affected: Link Register (LR) (if LK = 1)" - conditioned on LK
    # only (the RTL's indentation is lost in the text; the prose is unconditional on the branch outcome).
    if lk:
        st.lr = (cia + 4) & M


def _bclr(st, word, und):
    cia = st.pc
    if _f5(word, 16):
        # encoding PAGE 363 "19 BO BI 0 0 0 0 0 16 LK" (bits 16-20 Reserved); PAGE 87 "If instructions are
        # encoded with incorrectly set bits in reserved fields, the results on execution can be said to be
        # boundedly undefined."
        raise UndefinedBehaviour("bclr with nonzero reserved bits 16-20 PAGE 87/363")
    bo, bi, lk = _f5(word, 6), _f5(word, 11), word & 1
    target = st.lr & 0xFFFFFFFC                 # PAGE 363 "NIA <- LR[0-29] || 0b00" (read before LR update)
    taken = _ctr_cond(st, bo, bi)
    st.pc = target if taken else (cia + 4) & M
    if lk:  # PAGE 364 "If LK = 1, then the effective address of the instruction following ... into the link register"
        st.lr = (cia + 4) & M


def _bcctr(st, word, und):
    cia = st.pc
    if _f5(word, 16):
        raise UndefinedBehaviour("bcctr with nonzero reserved bits 16-20 PAGE 87/361")
    bo, bi, lk = _f5(word, 6), _f5(word, 11), word & 1
    if not (bo & 0x04):
        # PAGE 362 "If the 'decrement and test CTR' option is specified (BO[2] = 0), the instruction form is
        # invalid."  PAGE 87 "Gekko provides limited support for instructions represented in an invalid form."
        raise UndefinedBehaviour("bcctr invalid form BO[2]=0 PAGE 362")
    # PAGE 361 "cond_ok <- BO[0] | (CR[BI] == BO[1]); if cond_ok then NIA <- CTR || 0b00"
    # "The branch target address is CTR[0-29] || 0b00."  CTR is never decremented (no RTL for it).
    cond_ok = bool(bo & 0x10) or (_cr_bit(st, bi) == ((bo >> 3) & 1))
    target = st.ctr & 0xFFFFFFFC
    st.pc = target if cond_ok else (cia + 4) & M
    if lk:  # PAGE 361 "If LK = 1, the effective address of the instruction following ... into the link register."
        st.lr = (cia + 4) & M


# ----------------------------------------------------------------------------------------------
# system linkage
def _sc(st, word, und):
    cia = st.pc
    if word != 0x44000002:
        # encoding PAGE 537 / Appendix A PAGE 583: "0 1 0 0 0 1 0 ... 0 1 0" - every bit fixed.
        raise UndefinedBehaviour("sc-opcode word with nonzero reserved bits or bit30=0 PAGE 87/537")
    # PAGE 537 "SRR0 <- CIA + 4"; "SRR1[0,5-9,16-23,25-27,30-31] <- MSR[...]"; "bits 1-4 and 10-15 of SRR1
    # are set to undefined values" (RTL says "<- 0": reference 0, bits masked).  PAGE 166 step 3 copies
    # MSR[16-31] (so PM bit 29 and full-function bit 28 are saved, PAGE 165).
    # PAGE 537 "The exception causes the next instruction to be fetched from offset 0xC00 from the physical
    # base address determined by the new setting of MSR[IP]" - IP is not altered (Table 4-6 PAGE 168).
    _enter_exception(st, und, cia + 4, VEC_SYSCALL, "syscall")


def _rfi(st, word, und):
    cia = st.pc
    if word != 0x4C000064:
        # encoding PAGE 532 "19 00000 00000 00000 00000 50 0" - bits 6-20 and 31 reserved
        raise UndefinedBehaviour("rfi with nonzero reserved bits PAGE 87/532")
    if st.msr & MSR_PR:
        # PAGE 532 "This is a supervisor-level, context synchronizing instruction."; PAGE 90 privileged list
        _program(st, und, cia)
        return
    old = st.msr
    # PAGE 532 "MSR[0,5-9,16-23,25-27,30-31] <- SRR1[...]", "MSR[13] <- b'0'"; other bits unaltered.
    new = (old & ~RFI_RESTORE & ~MSR_POW) | (st.srr1 & RFI_RESTORE)
    st.msr = new & M
    _und_or(und, "msr", (st.srr1 ^ old) & RFI_UNCERTAIN)
    # PAGE 532 "NIA <- SRR0[0-29] || 0b00"
    st.pc = st.srr0 & 0xFFFFFFFC


# ----------------------------------------------------------------------------------------------
# traps
def _trap_cond(a, b, to):
    # PAGE 578 tw / PAGE 579 twi:
    #   if (a < b) & TO[0] then TRAP; if (a > b) & TO[1]; if (a = b) & TO[2];
    #   if (a <U b) & TO[3]; if (a >U b) & TO[4]
    sa, sb = _s32(a), _s32(b)
    ua, ub = a & M, b & M
    return bool(((to & 0x10) and sa < sb) or ((to & 0x08) and sa > sb) or ((to & 0x04) and ua == ub)
                or ((to & 0x02) and ua < ub) or ((to & 0x01) and ua > ub))


def _tw(st, word, und):
    cia = st.pc
    if word & 1:
        raise UndefinedBehaviour("tw with reserved bit 31 set PAGE 87/578")
    to, ra, rb = _f5(word, 6), _f5(word, 11), _f5(word, 16)
    if _trap_cond(st.gpr[ra], st.gpr[rb], to):
        _trap(st, und, cia)
    else:
        st.pc = (cia + 4) & M


def _twi(st, word, und):
    cia = st.pc
    to, ra = _f5(word, 6), _f5(word, 11)
    simm = _exts(word & 0xFFFF, 16) & M        # PAGE 579 "EXTS(SIMM)"
    if _trap_cond(st.gpr[ra], simm, to):
        _trap(st, und, cia)
    else:
        st.pc = (cia + 4) & M


# ----------------------------------------------------------------------------------------------
# MSR / SPR moves
def _mfmsr(st, word, und):
    cia = st.pc
    if (word >> 11) & 0x3FF or word & 1:
        raise UndefinedBehaviour("mfmsr with nonzero reserved bits 11-20/31 PAGE 87/461")
    if st.msr & MSR_PR:  # PAGE 461 "This is a supervisor-level instruction."
        _program(st, und, cia)
        return
    d = _f5(word, 6)
    st.gpr[d] = st.msr & M                      # PAGE 461 "rD <- MSR"
    _und_or(und, "gpr[%d]" % d, st.msr & MSR_RES_UNCERTAIN)
    st.pc = (cia + 4) & M


def _mtmsr(st, word, und):
    cia = st.pc
    if (word >> 11) & 0x3FF or word & 1:
        raise UndefinedBehaviour("mtmsr with nonzero reserved bits 11-20/31 PAGE 87/475")
    if st.msr & MSR_PR:  # PAGE 475 "This is a supervisor-level instruction."
        _program(st, und, cia)
        return
    v = st.gpr[_f5(word, 6)] & M
    old = st.msr
    if (old ^ v) & MSR_SE:
        # XCHK (was: no trace question at all).  PAGE 161 trace exceptions are "generated following
        # execution and completion of an instruction while trace mode is enabled"; PAGE 475 makes EE and
        # RI "effective as soon as the instruction completes" but is silent on SE.  Whether this mtmsr is
        # itself traced decides pc/srr0/srr1/msr/exception -> whole outcome undefined.
        raise UndefinedBehaviour("mtmsr changing MSR[SE]: whether the mtmsr itself is traced PAGE 161/164/475")
    st.msr = v                                  # PAGE 475 "MSR <- (rS)"
    # PAGE 168 "Reserved bits are read as if written as 0." vs PAGE 165 full-function/partial-function notes
    _und_or(und, "msr", v & MSR_RES_UNCERTAIN)
    # XCHK (was: not recorded).  PAGE 475 "It is also an execution synchronizing instruction except with
    # respect to alterations to the POW and LE bits": the point at which a changed LE takes effect for the
    # following instructions is not defined.  PAGE 63-64 HID0 DOZE/NAP/SLEEP "invoked by setting MSR[POW]
    # while this bit is set"; PAGE 164 "Power management functions are implementation-dependent".  Neither
    # changes this step's registers, so they are annotations, not register masks.
    if (old ^ v) & MSR_LE:
        und["le_effective_point"] = True
    if (v & MSR_POW) and not (old & MSR_POW) and (st.hid0 & _bits(8, 9, 10)):
        und["power_mode"] = True
    st.pc = (cia + 4) & M


def _spr_number(word):
    # PAGE 462 "n <- spr[5-9] || spr[0-4]"; spr field = instruction bits 11-20, spr[0-4] = bits 11-15.
    lo = _f5(word, 11)   # spr[0-4]
    hi = _f5(word, 16)   # spr[5-9]
    return (hi << 5) | lo


def _mfspr(st, word, und):
    cia = st.pc
    d, n = _f5(word, 6), _spr_number(word)
    info = _SPR.get(n)
    access = info[1] if info else None
    if access is None:
        # PAGE 57 "Gekko fully decodes the SPR field of the instruction. If the SPR specified is undefined, the
        # illegal instruction program exception occurs."  (PAGE 90, PAGE 175 agree.)  In user mode with
        # SPR[0]=1 PAGE 463 allows privileged-or-illegal type: identical in this model.
        _program(st, und, cia)
        return
    if access == "S" and st.msr & MSR_PR:
        # PAGE 463 "Execution of this instruction specifying a defined and supervisor-level register when
        # MSR[PR] = 1 will result in a privileged instruction type program exception."
        _program(st, und, cia)
        return
    name = info[0]
    key = "gpr[%d]" % d
    if n == 1:
        val = st.xer
        _und_or(und, key, val & XER_UNCERTAIN)
    elif n == 8:
        val = st.lr
    elif n == 9:
        val = st.ctr
    elif n == 18:
        val = st.dsisr
    elif n == 19:
        val = st.dar
    elif n == 22:
        val = st.dec
    elif n == 26:
        val = st.srr0
    elif n == 27:
        val = st.srr1
    elif 272 <= n <= 275:
        val = st.sprg[n - 272]
    elif n == 268:      # PAGE 117 note 2 / PAGE 468 "if n = 268 then rD <- TBL"
        val = st.tb & M
    elif n == 269:      # PAGE 468 "else if n = 269 then rD <- TBU"
        val = (st.tb >> 32) & M
    elif 912 <= n <= 919:
        val = st.gqr[n - 912]
        _und_or(und, key, val & GQR_RESERVED)
    elif n == 920:
        val = st.hid2
        _und_or(und, key, val & HID2_RESERVED)
    elif n == 1008:
        val = st.hid0
        _und_or(und, key, val & HID0_UNCERTAIN)
    elif n in (943, 959):
        # PAGE 61 "Gekko does not implement the sampled data address register (SDA) or the user-level,
        # read-only USDA registers" - the value read back is not specified.
        raise UndefinedBehaviour("mfspr %s: register not implemented, read value unspecified PAGE 61" % name)
    else:
        raise NotModelled("mfspr %s (SPR %d) needs state not in gekko_state.State" % (name, n))
    st.gpr[d] = val & M                         # PAGE 462 "rD <- SPR(n)"
    if word & 1:
        # PAGE 175 "The UISA defines mtspr and mfspr with the record bit (Rc) set as causing a program exception
        # or giving a boundedly-undefined result. In Gekko, the appropriate condition register (CR) should be
        # treated as undefined."  Interpreted as the Rc target field CR0.
        _und_or(und, "cr", 0xF0000000)
    st.pc = (cia + 4) & M


def _mtspr(st, word, und):
    cia = st.pc
    s, n = _f5(word, 6), _spr_number(word)
    info = _SPR.get(n)
    access = info[2] if info else None
    if 936 <= n <= 942:
        # Table 12-13 PAGE 478 lists UMMCR0/UPMCn/USIA as "User" mtspr encodings, but Table 2-53 PAGE 118 marks
        # them "mfspr" only (and ppc_750cl.txt says writes to user-read-only PM registers cause a program
        # exception): contradictory.
        raise UndefinedBehaviour("mtspr to user read-only PM register SPR %d: Table 12-13 PAGE 478 vs "
                                 "Table 2-53 PAGE 118 conflict" % n)
    if access is None:
        # PAGE 57 / PAGE 90 "Any mtspr, mfspr, or mftb instruction with an invalid SPR (or TBR) field causes an
        # illegal type program exception."  (PAGE 477 privileged-or-illegal choice collapses in this model.)
        _program(st, und, cia)
        return
    if access == "S" and st.msr & MSR_PR:
        # PAGE 477 "specifying a defined and supervisor-level register when MSR[PR] = 1 results in a privileged
        # instruction type program exception."
        _program(st, und, cia)
        return
    v = st.gpr[s] & M
    name = info[0]
    if n == 1:
        st.xer = v
        _und_or(und, "xer", v & XER_UNCERTAIN)
    elif n == 8:
        st.lr = v
    elif n == 9:
        st.ctr = v
    elif n == 18:
        st.dsisr = v
    elif n == 19:
        st.dar = v
    elif n == 22:
        old = st.dec
        st.dec = v
        if not (old & DEC_MSB) and (v & DEC_MSB):
            # Table 4-2 PAGE 159: decrementer exception "when the most-significant bit of the DEC register
            # changes from 0 to 1"; Table 4-3 PAGE 162 "Decrementer passes through zero".  Whether a software
            # write that sets the MSB requests the exception is not stated -> pending flag undefined.
            und["pending.decrementer"] = True
        elif st.pending.get("decrementer"):
            # XCHK (common-mode, both models kept it pending): PAGE 164 EE=0 "delays recognition of ...
            # decrementer exception conditions", but whether rewriting DEC withdraws a waiting condition is
            # not stated -> pending flag undefined.
            und["pending.decrementer"] = True
    elif n == 26:
        st.srr0 = v
    elif n == 27:
        st.srr1 = v
    elif 272 <= n <= 275:
        st.sprg[n - 272] = v
    elif n == 284:
        # PAGE 477 "SPRs TBL and TBU are treated as separate 32-bit registers; setting one leaves the other unaltered."
        st.tb = (st.tb & ~M) | v
    elif n == 285:
        st.tb = (v << 32) | (st.tb & M)
    elif 912 <= n <= 919:
        st.gqr[n - 912] = v
        _und_or(und, "gqr[%d]" % (n - 912), v & GQR_RESERVED)
    elif n == 920:
        old = st.hid2
        # PAGE 68 "4-7 DMAQL DMA queue length (read only)" -> write does not change it.
        st.hid2 = (v & ~HID2_DMAQL) | (old & HID2_DMAQL)
        # PAGE 68 sticky error bits: how a write affects them is not stated.
        _und_or(und, "hid2", ((v ^ old) & HID2_STICKY) | (v & HID2_RESERVED))
    elif n == 1008:
        new = v
        # PAGE 65 "Once the L1 flash invalidate bits are set through a mtspr operations, hardware automatically
        # resets these bits in the next cycle (provided that the corresponding cache enable bits are set in
        # HID0)."  and "The bit is cleared when the invalidation operation begins (usually the next cycle
        # after the write operation to the register)" - "usually": reference cleared, bit undefined.
        m = 0
        if v & HID0_ICFI:
            m |= HID0_ICFI
            if v & HID0_ICE:
                new &= ~HID0_ICFI
        if v & HID0_DCFI:
            m |= HID0_DCFI
            if v & HID0_DCE:
                new &= ~HID0_DCFI
        st.hid0 = new & M
        _und_or(und, "hid0", m | (v & HID0_UNCERTAIN))
    elif n in (287, 1009):
        # PAGE 90 "An mtspr instruction executing in supervisor mode (MSR[PR] = 0) with the SPR field specifying
        # HID1 or PVR (read-only registers) executes as a no-op."  (supervisor guaranteed by the check above)
        pass
    elif n == 959:
        # PAGE 61 "for compatibility with processors that do, those registers can be written to by boot code
        # without causing an exception. SDA is SPR 959; USDA is SPR 943."
        pass
    elif n == 943:
        if st.msr & MSR_PR:
            raise UndefinedBehaviour("mtspr USDA in user mode: PAGE 61 only covers boot code, Table 12-13 "
                                     "PAGE 478 lists it User")
    else:
        raise NotModelled("mtspr %s (SPR %d) needs state not in gekko_state.State" % (name, n))
    if word & 1:
        _und_or(und, "cr", 0xF0000000)          # PAGE 175, see _mfspr
    st.pc = (cia + 4) & M


# ----------------------------------------------------------------------------------------------
# decode
_BRANCHES = ("b", "bc", "bclr", "bcctr")


def _decode(word):
    op = _op(word)
    if op == 18:
        return "b", _b
    if op == 16:
        return "bc", _bc
    if op == 17:
        return "sc", _sc
    if op == 3:
        return "twi", _twi
    if op == 19:
        xo = _xo(word)
        if xo == 16:
            return "bclr", _bclr
        if xo == 528:
            return "bcctr", _bcctr
        if xo == 50:
            return "rfi", _rfi
    if op == 31:
        xo = _xo(word)
        if xo == 4:
            return "tw", _tw
        if xo == 83:
            return "mfmsr", _mfmsr
        if xo == 146:
            return "mtmsr", _mtmsr
        if xo in (339, 371):
            # PAGE 120 "Gekko ignores the extended opcode differences between mftb and mfspr by ignoring bit 25
            # and treating both instructions identically."
            return "mfspr", _mfspr
        if xo == 467:
            return "mtspr", _mtspr
    return None, None


def supports(word: int) -> bool:
    return _decode(word & M)[0] is not None


def _execute(state, word):
    word &= M
    name, fn = _decode(word)
    if fn is None:
        raise ValueError("branch_B does not handle word 0x%08X" % word)
    work = state.clone()
    work.exception = None
    und = {}
    pre_msr = state.msr
    fn(work, word, und)
    if work.exception is None:
        # PAGE 164 "SE ... generates a single-step trace exception upon the successful execution of every
        # instruction except rfi, isync, and sc"; "BE ... generates a branch type trace exception when a branch
        # instruction executes successfully."
        # XCHK (was: UndefinedBehaviour for the whole step, "trace register settings deferred to the PEM").
        # The Gekko manual does give the entry: PAGE 176 "Each instruction considered during trace mode
        # completes before a trace exception is taken", vector 0x00D00 from the MSR[IP] base; Table 4-6 PAGE 168
        # MSR row "Trace exception"; PAGE 166 steps 2-3 for SRR1; SRR0 = the next instruction, PAGE 163 "the next
        # one (as in the case of a system call, trace, or trap exception)" and 750CL PAGE 166 step 1 "Normally,
        # this is the instruction that would have been completed next".  Only SRR1[1-4,10-15] are unstated.
        # The instruction's own effects are defined, so they are no longer discarded.
        if (pre_msr & MSR_SE and name not in ("rfi", "sc")) or (pre_msr & MSR_BE and name in _BRANCHES):
            _enter_exception(work, und, work.pc, VEC_TRACE, "trace")
    return work, und


def undefined_bits(state, word: int) -> dict:
    return _execute(state, word)[1]


def step(state, word: int) -> None:
    work, und = _execute(state, word)
    for k, v in work.__dict__.items():
        if k != "mem":          # no instruction in this area touches memory; keep the caller's Memory object
            setattr(state, k, v)
    state.undefined_bits = und


# ==============================================================================================
# self-test: vectors derived by hand from the manual text (expected values written out literally)
if __name__ == "__main__":
    import traceback

    PC = 0x80003100

    def enc_spr(op_xo, reg, n, rc=0):
        return (31 << 26) | (reg << 21) | ((n & 0x1F) << 16) | (((n >> 5) & 0x1F) << 11) | (op_xo << 1) | rc

    def mfspr(d, n, rc=0):
        return enc_spr(339, d, n, rc)

    def mtspr(s, n, rc=0):
        return enc_spr(467, s, n, rc)

    SUP = 0x00003032  # FP|ME|IR|DR|RI supervisor
    USR = SUP | MSR_PR

    results = {"pass": 0, "fail": 0}

    def T(name, word, setup=None, expect=None, und=None, raises=None):
        st = State()
        st.pc = PC
        st.msr = SUP
        for k, v in (setup or {}).items():
            if k.startswith("gpr"):
                st.gpr[int(k[4:-1])] = v
            elif k.startswith("gqr"):
                st.gqr[int(k[4:-1])] = v
            elif k.startswith("sprg"):
                st.sprg[int(k[5:-1])] = v
            else:
                setattr(st, k, v)
        before = st.clone()
        try:
            try:
                step(st, word)
            except UndefinedBehaviour as e:
                if raises is not None and isinstance(e, raises):
                    # state must be untouched
                    assert st.arch_dict() == before.arch_dict(), "state modified on raise"
                    results["pass"] += 1
                    return
                raise
            assert raises is None, "expected %s" % raises.__name__
            exp = {"pc": (PC + 4) & M, "exception": None}
            exp.update(expect or {})
            for k, v in exp.items():
                if k.startswith("gpr"):
                    got = st.gpr[int(k[4:-1])]
                elif k.startswith("gqr"):
                    got = st.gqr[int(k[4:-1])]
                elif k.startswith("sprg"):
                    got = st.sprg[int(k[5:-1])]
                else:
                    got = getattr(st, k)
                assert got == v, "%s: got %r expected %r" % (k, got, v)
            # everything not in expect must be unchanged
            for k, v in before.arch_dict().items():
                if k in exp or k in ("gpr", "gqr", "sprg"):
                    continue
                assert getattr(st, k) == v, "%s changed unexpectedly: %r -> %r" % (k, v, getattr(st, k))
            for arr in ("gpr", "gqr", "sprg"):
                for i, v in enumerate(getattr(before, arr)):
                    if "%s[%d]" % (arr, i) not in exp:
                        assert getattr(st, arr)[i] == v, "%s[%d] changed" % (arr, i)
            assert st.undefined_bits == (und or {}), "undefined_bits %r expected %r" % (st.undefined_bits, und)
            results["pass"] += 1
        except Exception:
            results["fail"] += 1
            print("FAIL:", name)
            traceback.print_exc()

    # ---- table self-consistency: SPR[0]=1 iff supervisor (PAGE 463 / PAGE 477) -----------------
    for n_, (nm, rd, wr) in _SPR.items():
        for acc in (rd, wr):
            if acc is not None and nm not in ("DABR",):
                assert (acc == "S") == bool(n_ & 0x10), (n_, nm, acc)
    assert mfspr(0, 8) == 0x7C0802A6 and mtspr(0, 8) == 0x7C0803A6 and mtspr(0, 9) == 0x7C0903A6
    assert MSR_RES_ALL & (MSR_POW | MSR_ILE | MSR_EE | MSR_PR | MSR_FP | MSR_ME | MSR_FE0 | MSR_SE | MSR_BE
                          | MSR_FE1 | MSR_IP | MSR_IR | MSR_DR | MSR_PM | MSR_RI | MSR_LE) == 0
    assert SRR1_EXC_INFO | SRR1_MSR_COPY == M and SRR1_EXC_INFO & SRR1_MSR_COPY == 0
    assert RFI_RESTORE == 0x87C0FF73 and SRR1_EXC_INFO == 0x783F0000

    # ---- b / ba / bl / bla  (PAGE 358) ----------------------------------------------------------
    T("b +0x100", 0x48000100, expect={"pc": 0x80003200})
    T("b -4", 0x4BFFFFFC, expect={"pc": 0x800030FC})
    T("b most negative LI", 0x4A000000, expect={"pc": 0x7E003100})
    T("b most positive LI", 0x49FFFFFC, expect={"pc": 0x820030FC})
    T("bl +0x100 sets LR", 0x48000101, expect={"pc": 0x80003200, "lr": 0x80003104})
    T("ba 0x100", 0x48000102, expect={"pc": 0x00000100})
    T("bla sign-extended absolute", 0x4BFFFFFF, expect={"pc": 0xFFFFFFFC, "lr": 0x80003104})
    T("b wraps past 0xFFFFFFFF", 0x48000020, setup={"pc": 0xFFFFFFF0}, expect={"pc": 0x00000010})
    T("bl at top of memory: LR wraps", 0x48000011, setup={"pc": 0xFFFFFFFC}, expect={"pc": 0x0000000C, "lr": 0})
    # PAGE 360 "bcl 20,31" style: bl to next instruction
    T("bcl 20,31,$+4", 0x429F0005, expect={"pc": 0x80003104, "lr": 0x80003104})

    # ---- bc  (PAGE 359-360, Table 12-6) -----------------------------------------------------------
    T("beq taken (BO=12 BI=2)", 0x41820010, setup={"cr": 0x20000000}, expect={"pc": 0x80003110})
    T("beq not taken", 0x41820010, setup={"cr": 0xDFFFFFFF})
    T("bne taken (BO=4)", 0x40820010, setup={"cr": 0x00000000}, expect={"pc": 0x80003110})
    T("blt cr7 (BI=28)", 0x419C0010, setup={"cr": 0x00000008}, expect={"pc": 0x80003110})
    T("bdnz ctr 2->1 taken", 0x42000010, setup={"ctr": 2}, expect={"pc": 0x80003110, "ctr": 1})
    T("bdnz ctr 1->0 not taken", 0x42000010, setup={"ctr": 1}, expect={"ctr": 0})
    T("bdnz ctr 0 wraps -> taken", 0x42000010, setup={"ctr": 0}, expect={"pc": 0x80003110, "ctr": 0xFFFFFFFF})
    T("bdz ctr 1->0 taken (BO=18)", 0x42400010, setup={"ctr": 1}, expect={"pc": 0x80003110, "ctr": 0})
    T("bdz ctr 2->1 not taken", 0x42400010, setup={"ctr": 2}, expect={"ctr": 1})
    T("bdnzt taken (BO=8)", 0x41000010, setup={"ctr": 5, "cr": 0x80000000}, expect={"pc": 0x80003110, "ctr": 4})
    T("bdnzt cond false: not taken, ctr still decremented", 0x41000010, setup={"ctr": 5}, expect={"ctr": 4})
    T("bdnzf ctr->0 not taken (BO=0)", 0x40000010, setup={"ctr": 1}, expect={"ctr": 0})
    T("bdzf ctr->0 cond false taken (BO=2)", 0x40400010, setup={"ctr": 1}, expect={"pc": 0x80003110, "ctr": 0})
    T("bdzt ctr->0 cond true taken (BO=10)", 0x41400010, setup={"ctr": 1, "cr": 0x80000000},
      expect={"pc": 0x80003110, "ctr": 0})
    T("branch always BO=20, ctr untouched", 0x42800010, setup={"ctr": 0}, expect={"pc": 0x80003110})
    T("branch always BO=31 (z,y set)", 0x43E00010, setup={"ctr": 7}, expect={"pc": 0x80003110})
    T("001zy with z=1 (BO=6) cond false taken, no ctr", 0x40C20010, setup={"ctr": 0, "cr": 0},
      expect={"pc": 0x80003110})
    T("1z00y with z=1 (BO=24) ignores CR", 0x43000010, setup={"ctr": 2, "cr": 0}, expect={"pc": 0x80003110, "ctr": 1})
    T("bnel not taken still sets LR", 0x40820011, setup={"cr": 0x20000000}, expect={"lr": 0x80003104})
    T("bca absolute negative", 0x4280FFF2, expect={"pc": 0xFFFFFFF0})
    T("bc negative displacement", 0x4280FFF8, expect={"pc": 0x800030F8})
    T("bcla absolute taken", 0x42801003, expect={"pc": 0x00001000, "lr": 0x80003104})

    # ---- bclr (PAGE 363-364) ---------------------------------------------------------------------
    T("blr clears low bits", 0x4E800020, setup={"lr": 0x80001237}, expect={"pc": 0x80001234})
    T("blrl target is OLD LR", 0x4E800021, setup={"lr": 0x80001000}, expect={"pc": 0x80001000, "lr": 0x80003104})
    T("bdnzlr ctr->0 not taken", 0x4E000020, setup={"ctr": 1, "lr": 0x80001000}, expect={"ctr": 0})
    T("bdnzlr ctr->1 taken", 0x4E000020, setup={"ctr": 2, "lr": 0x80001000}, expect={"ctr": 1, "pc": 0x80001000})
    T("bltlr not taken", 0x4D800020, setup={"lr": 0x80001000, "cr": 0x7FFFFFFF})
    T("bltlrl not taken sets LR", 0x4D800021, setup={"lr": 0x80001000}, expect={"lr": 0x80003104})
    T("bclr reserved bits -> UB", 0x4E800820, raises=UndefinedBehaviour)

    # ---- bcctr (PAGE 361-362) --------------------------------------------------------------------
    T("bctr clears low bits", 0x4E800420, setup={"ctr": 0x8000AAAB}, expect={"pc": 0x8000AAA8})
    T("bctrl", 0x4E800421, setup={"ctr": 0x80004000}, expect={"pc": 0x80004000, "lr": 0x80003104})
    T("bctrl with CTR==LR alias value", 0x4E800421, setup={"ctr": 0x80003104, "lr": 0x80003104},
      expect={"pc": 0x80003104, "lr": 0x80003104})
    T("bnectr not taken", 0x4C820420, setup={"ctr": 0x80004000, "cr": 0x20000000})
    T("bnectrl not taken sets LR, no ctr change", 0x4C820421, setup={"ctr": 0, "cr": 0x20000000},
      expect={"lr": 0x80003104})
    T("bcctr BO=16 invalid form -> UB", 0x4E000420, setup={"ctr": 5}, raises=UndefinedBehaviour)
    T("bcctr BO=0 invalid form -> UB", 0x4C000420, raises=UndefinedBehaviour)

    # ---- sc (PAGE 537, Table 4-6 PAGE 168) ---------------------------------------------------------
    T("sc from user mode", 0x44000002, setup={"msr": 0x0000F032},
      expect={"srr0": 0x80003104, "srr1": 0x0000F032, "msr": 0x00001000, "pc": 0x00000C00, "exception": "syscall"},
      und={"srr1": 0x783F0000})
    T("sc IP ILE SE PM FE0 LE", 0x44000002, setup={"msr": 0x00010C45},
      expect={"srr0": 0x80003104, "srr1": 0x00000C45, "msr": 0x00010041, "pc": 0xFFF00C00, "exception": "syscall"},
      und={"srr1": 0x783F0000})
    T("sc with reserved MSR bit 24 and 5 set", 0x44000002, setup={"msr": 0x04003032 | 0x80},
      expect={"srr0": 0x80003104, "srr1": 0x040030B2, "msr": 0x00001000, "pc": 0x00000C00, "exception": "syscall"},
      und={"srr1": 0x783F0080, "msr": 0x04000080})
    T("sc malformed -> UB", 0x44000003, raises=UndefinedBehaviour)

    # ---- rfi (PAGE 532) ------------------------------------------------------------------------------
    T("rfi restores and aligns", 0x4C000064, setup={"msr": 0x00001000, "srr1": 0x0004F032, "srr0": 0x80001237},
      expect={"msr": 0x0000F032, "pc": 0x80001234})
    T("rfi leaves ILE, clears ME from SRR1", 0x4C000064, setup={"msr": 0x00011000, "srr1": 0, "srr0": 0},
      expect={"msr": 0x00010000, "pc": 0})
    T("rfi clears POW", 0x4C000064, setup={"msr": 0x00041000, "srr1": 0x1000, "srr0": 0x100},
      expect={"msr": 0x00001000, "pc": 0x100})
    T("rfi PM differs -> bit undefined", 0x4C000064, setup={"msr": 0x1000, "srr1": 0x1004, "srr0": 0x100},
      expect={"msr": 0x1000, "pc": 0x100}, und={"msr": 0x4})
    T("rfi restores full-function bit 0; bit 24 restore unstated", 0x4C000064,
      setup={"msr": 0x1000, "srr1": 0x80001080, "srr0": 0x100}, expect={"msr": 0x80001000, "pc": 0x100},
      und={"msr": 0x80})
    T("rfi in user mode -> program", 0x4C000064, setup={"msr": USR, "srr0": 0x100},
      expect={"srr0": PC, "srr1": USR, "msr": 0x00001000, "pc": 0x00000700, "exception": "program"},
      und={"srr1": 0x783F0000})

    # ---- tw / twi (PAGE 578-579) ---------------------------------------------------------------------
    trap_exp = {"srr0": PC + 4, "srr1": SUP, "msr": 0x00001000, "pc": 0x00000700, "exception": "trap"}
    trap_und = {"srr0": M, "srr1": 0x783F0000}
    T("tweqi r3,0 traps", 0x0C830000, setup={"gpr[3]": 0}, expect=trap_exp, und=trap_und)
    T("tweqi r3,0 no trap", 0x0C830000, setup={"gpr[3]": 1})
    T("twgti signed: 0x80000000 > 0 false", 0x0D030000, setup={"gpr[3]": 0x80000000})
    T("twlgti unsigned: 0x80000000 >u 0 traps", 0x0C230000, setup={"gpr[3]": 0x80000000}, expect=trap_exp, und=trap_und)
    T("twllti r3 <u EXTS(-1)", 0x0C43FFFF, setup={"gpr[3]": 5}, expect=trap_exp, und=trap_und)
    T("twlti r3 < -1 false for 5", 0x0E03FFFF, setup={"gpr[3]": 5})
    T("tw lt: -1 < 0 traps", (31 << 26) | (0x10 << 21) | (3 << 16) | (4 << 11) | 8,
      setup={"gpr[3]": 0xFFFFFFFF, "gpr[4]": 0}, expect=trap_exp, und=trap_und)
    T("tw ltu: 0xFFFFFFFF <u 0 false", (31 << 26) | (0x02 << 21) | (3 << 16) | (4 << 11) | 8,
      setup={"gpr[3]": 0xFFFFFFFF, "gpr[4]": 0})
    T("trap = tw 31,0,0 always", 0x7FE00008, expect=trap_exp, und=trap_und)
    T("tw 0 never", 0x7C000008)
    T("tw rA==rB same register eq traps", (31 << 26) | (4 << 21) | (5 << 16) | (5 << 11) | 8,
      setup={"gpr[5]": 0x1234}, expect=trap_exp, und=trap_und)
    T("tw with bit31 -> UB", 0x7FE00009, raises=UndefinedBehaviour)
    T("twi trap in user mode (UISA, not privileged)", 0x0FE00000, setup={"msr": USR},
      expect=dict(trap_exp, srr1=USR), und=trap_und)

    # ---- mfmsr / mtmsr (PAGE 461, 475) ------------------------------------------------------------
    T("mfmsr r5", 0x7CA000A6, expect={"gpr[5]": SUP})
    T("mfmsr user -> program", 0x7CA000A6, setup={"msr": USR},
      expect={"srr0": PC, "srr1": USR, "msr": 0x1000, "pc": 0x700, "exception": "program"}, und={"srr1": 0x783F0000})
    T("mtmsr r6", 0x7CC00124, setup={"gpr[6]": 0x0000B032}, expect={"msr": 0x0000B032})
    T("mtmsr to user mode", 0x7CC00124, setup={"gpr[6]": USR}, expect={"msr": USR})
    T("mtmsr reserved bit 24 undefined", 0x7CC00124, setup={"gpr[6]": 0x000030B2}, expect={"msr": 0x000030B2},
      und={"msr": 0x80})
    T("mtmsr user -> program", 0x7CC00124, setup={"msr": USR, "gpr[6]": 0},
      expect={"srr0": PC, "srr1": USR, "msr": 0x1000, "pc": 0x700, "exception": "program"}, und={"srr1": 0x783F0000})
    T("mtmsr reserved field -> UB", 0x7CC10124, raises=UndefinedBehaviour)

    # ---- mfspr / mtspr (PAGE 462-465, 476-479) -----------------------------------------------------
    T("mtlr r0", 0x7C0803A6, setup={"gpr[0]": 0x12345678}, expect={"lr": 0x12345678})
    T("mflr r0 user mode ok", 0x7C0802A6, setup={"msr": USR, "lr": 0xCAFEBABE}, expect={"gpr[0]": 0xCAFEBABE})
    T("mtctr r3", mtspr(3, 9), setup={"gpr[3]": 0xFFFFFFFF}, expect={"ctr": 0xFFFFFFFF})
    T("mfctr r3", mfspr(3, 9), setup={"ctr": 7}, expect={"gpr[3]": 7})
    T("mtxer bits 0-2,16-23,25-31 defined", mtspr(4, 1), setup={"gpr[4]": 0xE000FF7F}, expect={"xer": 0xE000FF7F})
    T("mtxer reserved bit 5 undefined", mtspr(4, 1), setup={"gpr[4]": 0x04000000}, expect={"xer": 0x04000000},
      und={"xer": 0x04000000})
    T("mfxer", mfspr(4, 1), setup={"xer": 0x20000005}, expect={"gpr[4]": 0x20000005})
    T("mfspr DSISR user -> privileged program", mfspr(3, 18), setup={"msr": USR},
      expect={"srr0": PC, "srr1": USR, "msr": 0x1000, "pc": 0x700, "exception": "program"}, und={"srr1": 0x783F0000})
    T("mtspr SRR0", mtspr(3, 26), setup={"gpr[3]": 0x80001235}, expect={"srr0": 0x80001235})
    T("mtspr SRR1", mtspr(3, 27), setup={"gpr[3]": 0xFFFFFFFF}, expect={"srr1": 0xFFFFFFFF})
    T("mfspr SRR1", mfspr(3, 27), setup={"srr1": 0x0000B032}, expect={"gpr[3]": 0x0000B032})
    T("mtspr SPRG3", mtspr(3, 275), setup={"gpr[3]": 0xDEADBEEF}, expect={"sprg[3]": 0xDEADBEEF})
    T("mfspr SPRG0", mfspr(7, 272), setup={"sprg[0]": 0x55}, expect={"gpr[7]": 0x55})
    T("mtspr DAR", mtspr(3, 19), setup={"gpr[3]": 0xFFF}, expect={"dar": 0xFFF})
    T("mtspr DSISR", mtspr(3, 18), setup={"gpr[3]": 0x42000000}, expect={"dsisr": 0x42000000})
    T("mtspr DEC MSB 0->1 pending undefined", mtspr(3, 22), setup={"gpr[3]": 0x80000000, "dec": 5},
      expect={"dec": 0x80000000}, und={"pending.decrementer": True})
    T("mtspr DEC no MSB change", mtspr(3, 22), setup={"gpr[3]": 0x100, "dec": 0x80000000}, expect={"dec": 0x100})
    T("mfspr DEC", mfspr(3, 22), setup={"dec": 0x12345}, expect={"gpr[3]": 0x12345})
    T("mtspr TBL (284) keeps TBU", mtspr(3, 284), setup={"gpr[3]": 0xAAAAAAAA, "tb": 0x1111111122222222},
      expect={"tb": 0x11111111AAAAAAAA})
    T("mtspr TBU (285) keeps TBL", mtspr(3, 285), setup={"gpr[3]": 0xAAAAAAAA, "tb": 0x1111111122222222},
      expect={"tb": 0xAAAAAAAA22222222})
    T("mfspr TBL (268) user", mfspr(3, 268), setup={"msr": USR, "tb": 0x1111111122222222}, expect={"gpr[3]": 0x22222222})
    T("mfspr TBU (269)", mfspr(3, 269), setup={"tb": 0x1111111122222222}, expect={"gpr[3]": 0x11111111})
    T("mftb TBU (XO 371)", enc_spr(371, 3, 269), setup={"tb": 0x1111111122222222}, expect={"gpr[3]": 0x11111111})
    T("mtspr TBL user -> program", mtspr(3, 284), setup={"msr": USR},
      expect={"srr0": PC, "srr1": USR, "msr": 0x1000, "pc": 0x700, "exception": "program"}, und={"srr1": 0x783F0000})
    T("mfspr 284 invalid -> illegal program", mfspr(3, 284),
      expect={"srr0": PC, "srr1": SUP, "msr": 0x1000, "pc": 0x700, "exception": "program"}, und={"srr1": 0x783F0000})
    T("mtspr 268 invalid -> illegal program", mtspr(3, 268),
      expect={"srr0": PC, "srr1": SUP, "msr": 0x1000, "pc": 0x700, "exception": "program"}, und={"srr1": 0x783F0000})
    T("mfspr SPR 0 invalid", mfspr(3, 0),
      expect={"srr0": PC, "srr1": SUP, "msr": 0x1000, "pc": 0x700, "exception": "program"}, und={"srr1": 0x783F0000})
    T("mtspr GQR5", mtspr(3, 917), setup={"gpr[3]": 0x00070007}, expect={"gqr[5]": 0x00070007})
    T("mtspr GQR0 reserved bits undefined", mtspr(3, 912), setup={"gpr[3]": 0xFFFFFFFF},
      expect={"gqr[0]": 0xFFFFFFFF}, und={"gqr[0]": 0xC0F8C0F8})
    T("mfspr GQR7", mfspr(3, 919), setup={"gqr[7]": 0x3F070000}, expect={"gpr[3]": 0x3F070000})
    T("mtspr HID2 keeps DMAQL", mtspr(3, 920), setup={"gpr[3]": 0xA0000000, "hid2": 0x0A000000},
      expect={"hid2": 0xAA000000})
    T("mtspr HID2 sticky change undefined", mtspr(3, 920), setup={"gpr[3]": 0xE0000000, "hid2": 0xE0800000},
      expect={"hid2": 0xE0000000}, und={"hid2": 0x00800000})
    T("mfspr HID2", mfspr(3, 920), setup={"hid2": 0xE0000000}, expect={"gpr[3]": 0xE0000000})
    T("mtspr HID0 ICFI auto-clears with ICE", mtspr(3, 1008), setup={"gpr[3]": 0x00008800},
      expect={"hid0": 0x00008000}, und={"hid0": 0x00000800})
    T("mtspr HID0 plain", mtspr(3, 1008), setup={"gpr[3]": 0x0011C004}, expect={"hid0": 0x0011C004})
    T("mtspr PVR supervisor no-op", mtspr(3, 287), setup={"gpr[3]": 0x12345678})
    T("mtspr HID1 supervisor no-op", mtspr(3, 1009), setup={"gpr[3]": 0x12345678})
    T("mtspr HID1 user -> program", mtspr(3, 1009), setup={"msr": USR},
      expect={"srr0": PC, "srr1": USR, "msr": 0x1000, "pc": 0x700, "exception": "program"}, und={"srr1": 0x783F0000})
    T("mfspr LR with Rc=1: CR0 undefined", mfspr(3, 8, rc=1), setup={"lr": 0x44}, expect={"gpr[3]": 0x44},
      und={"cr": 0xF0000000})
    T("mfspr IBAT0U not modelled", mfspr(3, 528), raises=NotModelled)
    T("mfspr IBAT0U user still privileged program", mfspr(3, 528), setup={"msr": USR},
      expect={"srr0": PC, "srr1": USR, "msr": 0x1000, "pc": 0x700, "exception": "program"}, und={"srr1": 0x783F0000})
    T("mtspr UMMCR0 conflict -> UB", mtspr(3, 936), raises=UndefinedBehaviour)
    T("mfspr SDA -> UB", mfspr(3, 959), raises=UndefinedBehaviour)

    # ---- trace modes ------------------------------------------------------------------------------
    T("b with MSR[SE] -> trace, SRR0 = target", 0x48000100, setup={"msr": SUP | MSR_SE},
      expect={"srr0": 0x80003200, "srr1": 0x00003432, "msr": 0x00001000, "pc": 0x00000D00, "exception": "trace"},
      und={"srr1": 0x783F0000})
    T("bc not taken with MSR[BE] -> trace, SRR0 = CIA+4", 0x41820010, setup={"msr": SUP | MSR_BE},
      expect={"srr0": 0x80003104, "srr1": 0x00003232, "msr": 0x00001000, "pc": 0x00000D00, "exception": "trace"},
      und={"srr1": 0x783F0000})
    T("bl with BE and IP|ILE -> trace at 0xFFF00D00, LE from ILE", 0x48000101,
      setup={"msr": SUP | MSR_BE | MSR_IP | MSR_ILE},
      expect={"lr": 0x80003104, "srr0": 0x80003200, "srr1": 0x00003272, "msr": 0x00011041, "pc": 0xFFF00D00,
              "exception": "trace"}, und={"srr1": 0x783F0000})
    T("tw trapping under SE: trap only", 0x7FE00008, setup={"msr": SUP | MSR_SE},
      expect=dict(trap_exp, srr1=SUP | MSR_SE), und=trap_und)
    T("mtmsr setting SE -> UB", 0x7CC00124, setup={"gpr[6]": SUP | MSR_SE}, raises=UndefinedBehaviour)
    T("mtmsr SE unchanged: traced with new MSR", 0x7CC00124, setup={"msr": SUP | MSR_SE, "gpr[6]": 0xB432},
      expect={"srr0": 0x80003104, "srr1": 0x0000B432, "msr": 0x00001000, "pc": 0x00000D00, "exception": "trace"},
      und={"srr1": 0x783F0000})
    T("mtmsr LE change annotated", 0x7CC00124, setup={"gpr[6]": SUP | MSR_LE}, expect={"msr": SUP | MSR_LE},
      und={"le_effective_point": True})
    T("mtmsr POW with NAP annotated", 0x7CC00124, setup={"gpr[6]": SUP | MSR_POW, "hid0": _bit(9)},
      expect={"msr": SUP | MSR_POW}, und={"power_mode": True})
    T("mtmsr full-function bit 28 stored defined", 0x7CC00124, setup={"gpr[6]": SUP | 0x8}, expect={"msr": SUP | 0x8})
    T("mfmsr full-function bit 28 read defined", 0x7CA000A6, setup={"msr": SUP | 0x8}, expect={"gpr[5]": SUP | 0x8})
    T("mtxer bit 3 defined (mcrxr PAGE 458)", mtspr(4, 1), setup={"gpr[4]": 0x10000000}, expect={"xer": 0x10000000})
    T("mtdec while decrementer pending", mtspr(3, 22), setup={"gpr[3]": 0x100, "dec": 0x200,
                                                            "pending": {"external": False, "decrementer": True}},
      expect={"dec": 0x100}, und={"pending.decrementer": True})
    T("mtlr with MSR[BE] fine (not a branch)", 0x7C0803A6, setup={"msr": SUP | MSR_BE, "gpr[0]": 1},
      expect={"lr": 1})
    T("rfi with MSR[SE] exempt", 0x4C000064, setup={"msr": 0x1400, "srr1": 0x1000, "srr0": 0x10},
      expect={"msr": 0x1000, "pc": 0x10})

    # ---- supports -----------------------------------------------------------------------------------
    for w in (0x48000000, 0x40000000, 0x44000002, 0x0C000000, 0x4E800020, 0x4E800420, 0x4C000064,
              0x7C000008, 0x7C0000A6, 0x7C000124, 0x7C0802A6, 0x7C0803A6, 0x7C0C42E6):
        assert supports(w), hex(w)
    for w in (0x00000000, 0x4C000000, 0x7C000038, 0x38600000, 0x4C00012C):
        assert not supports(w), hex(w)

    print("branch_B self-test: %d passed, %d failed" % (results["pass"], results["fail"]))
    raise SystemExit(1 if results["fail"] else 0)
