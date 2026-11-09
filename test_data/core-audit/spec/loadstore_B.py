"""loadstore_B.py - independent Gekko specification model, area "loadstore", author B.

Sources (ONLY): docs/reference/ppc/gekko_user_manual.txt, cited "PAGE N" (the "===== PAGE N ====="
marker that precedes the text), and docs/reference/ppc/ppc_750cl.txt, cited "750CL PAGE N".
No GekkoForge implementation, test, audit or project document was consulted.

Instructions covered
    D-form : lwz lwzu lbz lbzu lhz lhzu lha lhau stw stwu stb stbu sth sthu lmw stmw
    X-form : lwzx lwzux lbzx lbzux lhzx lhzux lhax lhaux stwx stwux stbx stbux sthx sthux
             lhbrx lwbrx sthbrx stwbrx (byte-reverse forms, included for completeness)
             lswi lswx stswi stswx lwarx stwcx. dcbz
    op 4   : dcbz_l

Outcome contract of step(state, word)
    * normal completion: registers/memory updated, state.pc += 4, state.exception = None.
    * a precise exception this area can raise (alignment 0x600, program 0x700): SRR0/SRR1/MSR/PC
      (and DAR for alignment) are updated, state.exception is set, and then PartiallyUndefined is
      raised because the on-disk manuals do not define every bit that the exception writes.
    * UndefinedBehaviour: the manual leaves the result undefined / invalid form / contradictory.
      The state is NOT modified.
    * Unmodelled (subclass of UndefinedBehaviour): the manual defines the result, but it depends on
      information the neutral gekko_state.State does not carry (WIMG bits of a translated page,
      L1 cache occupancy, WPAR).  The state is NOT modified.
    * PartiallyUndefined (subclass of UndefinedBehaviour): the state HAS been updated with every
      defined effect; exc.masks = {field: bitmask} names the bits whose value is undefined.
      Field keys: 'srr1', 'dsisr', 'cr', 'hid2', 'reserve_addr', 'gpr[N]', and (dcbz_l machine check,
      added in reconciliation) 'pc', 'srr0', 'msr', 'exception' (whole field undefined).

Modelling boundary (read before comparing against an implementation)
    * state.mem is addressed by effective address; translation, DSI and page R/C recording are not
      modelled (gekko_state has no BAT/SR/page table).  PAGE 89: EA arithmetic is 32-bit unsigned,
      carry ignored, multi-byte operands wrap from 0xFFFFFFFF to 0.
    * Optional hooks a harness MAY attach to the State object:
          state.data_wimg(ea) -> int   WIMG nibble (W=8, I=4, M=2, G=1) of a translated data access
          state.data_phys(ea) -> int   physical address of a translated data access
          state.dcache_hit(ea) -> bool whether the 32-byte L1 block holding ea is in the L1 d-cache
          state.wpar : int             WPAR value (write gather pipe)
      With MSR[DR]=0 the default WIMG is 0b0011 (PAGE 138, PAGE 308) and PA = EA, so no hook is needed.
    * ASSUMPTION (not a manual rule): if state has no 'wpar' attribute, no store is taken to address
      the write gather pipe (PAGE 314); WPAR is not part of the neutral state.
    * Memory access logging granularity: one mem.read/mem.write per architectural MEM(EA, n) term of
      the manual's RTL, in RTL order; dcbz/dcbz_l clear a 32-byte block and are logged as ONE
      32-byte write of 0 at the block base (the manual gives no intra-block order).  Byte-reverse
      instructions perform ONE access of the operand at EA (their RTL concatenates bytes, which is not
      a temporal order).
"""
from __future__ import annotations

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from gekko_state import MASK32, State  # noqa: E402

AREA = "loadstore"


class UndefinedBehaviour(Exception):
    """Behaviour the manual leaves undefined / invalid / contradictory.  State untouched unless the
    subclass PartiallyUndefined is raised."""

    def __init__(self, what: str, masks: dict | None = None):
        super().__init__(what)
        self.what = what
        self.masks = dict(masks or {})


class Unmodelled(UndefinedBehaviour):
    """Defined by the manual but dependent on state absent from gekko_state.State.  State untouched."""


class PartiallyUndefined(UndefinedBehaviour):
    """State updated with all defined effects; self.masks lists undefined bits per field."""


# --------------------------------------------------------------------------------------------------
# Bit helpers (PowerPC big-endian bit numbering: bit 0 is the MSB of a 32-bit register)
# --------------------------------------------------------------------------------------------------
def _B(k: int) -> int:
    return 1 << (31 - k)


# MSR bits, Table 4-4 PAGE 164-165
MSR_POW = _B(13)
MSR_ILE = _B(15)
MSR_EE = _B(16)
MSR_PR = _B(17)
MSR_FP = _B(18)
MSR_ME = _B(19)
MSR_IP = _B(25)
MSR_IR = _B(26)
MSR_DR = _B(27)
MSR_RI = _B(30)
MSR_LE = _B(31)

# HID0[DCE] bit 17, PAGE 64
HID0_DCE = _B(17)
# HID2 bits, Table 2-6 PAGE 67-68: WPE bit 1, LCE bit 3, DCHERR bit 8, DCHEE bit 12
HID2_WPE = _B(1)
HID2_LCE = _B(3)
HID2_DCHERR = _B(8)
HID2_DCHEE = _B(12)

# XER[SO] is bit 0; XER[25-31] is the string byte count (PAGE 41, PAGE 449, PAGE 562)
XER_SO = _B(0)
XER_COUNT = 0x7F

# WIMG nibble as written "WIMG = x1xx" (I is the second bit) PAGE 138; W = 1 or I = 1 PAGE 193
WIMG_W = 0b1000
WIMG_I = 0b0100
DEFAULT_REAL_MODE_WIMG = 0b0011  # PAGE 308 "default WIMG bits (0b0011)"; PAGE 138 I cleared in real mode

# SRR1 bits copied from MSR on an exception.  PAGE 163/166 say MSR[5-9,16-31]; the sc/rfi RTL
# (PAGE 537, PAGE 533) says MSR[0,5-9,16-23,25-27,30-31].
# RECONCILED (crosscheck_loadstore.py class C1): intersecting was too wide.  PAGE 166 step 3 adds
# "Depending on the implementation, reserved bits may not be copied", and Gekko resolves that in
# Table 4-4 PAGE 165: "Note: Full function reserved bits are saved in SRR1 when an exception occurs;
# partial function reserved bits are not saved."  Bit 28 is "Reserved. Full function" (PAGE 165) ->
# copied; bit 29 is PM, a Gekko-defined (not reserved) bit inside PAGE 166's 16-31 -> copied (PAGE 537's
# NOTE: "An implementation may define additional MSR bits, and in this case, may also cause them to be
# saved to SRR1").  Bit 24 is "Reserved. This bit corresponds to the AL bit of the POWER architecture."
# (PAGE 164), neither full nor partial function, and not in PAGE 537's list -> stays undefined.
SRR1_COPY_MASK = 0x07C0FF7F          # bits 5-9, 16-23, 25-31
SRR1_UNDEFINED_MASK = 0xF83F0080     # bits 0, 1-4, 10-15 (exception-specific, PAGE 166), 24

BLOCK_MASK = ~31 & MASK32            # 32-byte L1 cache block, PAGE 25 "32-byte (eight-word) cache block"


def _exts16(v: int) -> int:
    v &= 0xFFFF
    return (v - 0x10000) & MASK32 if v & 0x8000 else v


# --------------------------------------------------------------------------------------------------
# Decode table.  Encodings: Appendix A / Chapter 12 (opcode, xo) and the x'....' images of each entry.
# --------------------------------------------------------------------------------------------------
# value: (mnemonic, kind, width, update, byterev, page); kind "loada" = sign-extending (lha family)
_DFORM = {
    32: ("lwz", "load", 4, False, False, 452),    # x'8000 0000' PAGE 452
    33: ("lwzu", "load", 4, True, False, 453),    # x'8400 0000' PAGE 453
    34: ("lbz", "load", 1, False, False, 426),    # x'8800 0000' PAGE 426
    35: ("lbzu", "load", 1, True, False, 427),    # x'8C00 0000' PAGE 427
    40: ("lhz", "load", 2, False, False, 443),    # x'A000 0000' PAGE 443
    41: ("lhzu", "load", 2, True, False, 444),    # x'A400 0000' PAGE 444
    42: ("lha", "loada", 2, False, False, 438),   # x'A800 0000' PAGE 438
    43: ("lhau", "loada", 2, True, False, 439),   # x'AC00 0000' PAGE 439
    36: ("stw", "store", 4, False, False, 563),   # x'9000 0000' PAGE 563
    37: ("stwu", "store", 4, True, False, 566),   # x'9400 0000' PAGE 566
    38: ("stb", "store", 1, False, False, 542),   # x'9800 0000' PAGE 542
    39: ("stbu", "store", 1, True, False, 543),   # x'9C00 0000' PAGE 543
    44: ("sth", "store", 2, False, False, 555),   # x'B000 0000' PAGE 555
    45: ("sthu", "store", 2, True, False, 557),   # x'B400 0000' PAGE 557
    46: ("lmw", "lmw", 4, False, False, 447),     # x'B800 0000' PAGE 447
    47: ("stmw", "stmw", 4, False, False, 560),   # x'BC00 0000' PAGE 560
}

_XFORM31 = {
    23: ("lwzx", "load", 4, False, False, 455),      # x'7C00 002E' PAGE 455
    55: ("lwzux", "load", 4, True, False, 454),     # x'7C00 006E' PAGE 454
    87: ("lbzx", "load", 1, False, False, 429),      # x'7C00 00AE' PAGE 429
    119: ("lbzux", "load", 1, True, False, 428),    # x'7C00 00EE' PAGE 428
    279: ("lhzx", "load", 2, False, False, 446),     # x'7C00 022E' PAGE 446
    311: ("lhzux", "load", 2, True, False, 445),     # x'7C00 026E' PAGE 445
    343: ("lhax", "loada", 2, False, False, 441),    # x'7C00 02AE' PAGE 441
    375: ("lhaux", "loada", 2, True, False, 440),    # x'7C00 02EE' PAGE 440
    151: ("stwx", "store", 4, False, False, 568),    # x'7C00 012E' PAGE 568
    183: ("stwux", "store", 4, True, False, 567),    # x'7C00 016E' PAGE 567
    215: ("stbx", "store", 1, False, False, 545),    # x'7C00 01AE' PAGE 545
    247: ("stbux", "store", 1, True, False, 544),    # x'7C00 01EE' PAGE 544
    407: ("sthx", "store", 2, False, False, 559),    # x'7C00 032E' PAGE 559
    439: ("sthux", "store", 2, True, False, 558),    # x'7C00 036E' PAGE 558
    790: ("lhbrx", "load", 2, False, True, 442),     # x'7C00 062C' PAGE 442
    534: ("lwbrx", "load", 4, False, True, 451),     # x'7C00 042C' PAGE 451
    918: ("sthbrx", "store", 2, False, True, 556),   # x'7C00 072C' PAGE 556
    662: ("stwbrx", "store", 4, False, True, 564),   # x'7C00 052C' PAGE 564
    597: ("lswi", "lswi", 1, False, False, 448),     # x'7C00 04AA' PAGE 448
    533: ("lswx", "lswx", 1, False, False, 449),     # x'7C00 042A' PAGE 449
    725: ("stswi", "stswi", 1, False, False, 561),   # x'7C00 05AA' PAGE 561
    661: ("stswx", "stswx", 1, False, False, 562),   # x'7C00 052A' PAGE 562
    20: ("lwarx", "lwarx", 4, False, False, 450),    # x'7C00 0028' PAGE 450
    150: ("stwcx.", "stwcx", 4, False, False, 565),  # x'7C00 012D' PAGE 565
    1014: ("dcbz", "dcbz", 32, False, False, 383),   # x'7C00 07EC' PAGE 383
}

_DCBZ_L = ("dcbz_l", "dcbz_l", 32, False, False, 384)  # x'1000 07EC' PAGE 384, opcode 4 xo 1014


def _decode(word: int):
    word &= MASK32
    op = word >> 26
    if op in _DFORM:
        return _DFORM[op]
    xo = (word >> 1) & 0x3FF
    if op == 31:
        return _XFORM31.get(xo)
    if op == 4 and xo == 1014:
        return _DCBZ_L
    return None


def supports(word: int) -> bool:
    """True for every encoding whose primary/extended opcode names an instruction of this area,
    including its invalid forms (step() then raises UndefinedBehaviour)."""
    return _decode(word) is not None


def mnemonic(word: int) -> str | None:
    d = _decode(word)
    return d[0] if d else None


# --------------------------------------------------------------------------------------------------
# Exception entry (only the exceptions this area can cause: alignment, program)
# --------------------------------------------------------------------------------------------------
def _take_exception(state: State, name: str, offset: int, dar: int | None, why: str,
                    extra_masks: dict | None = None) -> None:
    old = state.msr & MASK32
    # SRR0 = the instruction that caused it: PAGE 163 "the address of the instruction that caused the
    # exception or the next one (as in the case of a system call, trace, or trap exception)".
    state.srr0 = state.pc & MASK32
    # SRR1: MSR copy (intersection of PAGE 166 and PAGE 537), other bits undefined (see constants).
    state.srr1 = old & SRR1_COPY_MASK
    # New MSR, Table 4-6 PAGE 168: ILE, ME, IP not altered; LE <- ILE; every other bit cleared
    # ("Reserved bits are read as if written as 0").
    new = old & (MSR_ILE | MSR_ME | MSR_IP)
    if old & MSR_ILE:
        new |= MSR_LE
    state.msr = new
    # Vector: PAGE 166 step 5, IP=0 -> 0x000n_nnnn, IP=1 -> 0xFFFn_nnnn; offsets Table 4-2 PAGE 159.
    state.pc = ((0xFFF00000 if old & MSR_IP else 0x00000000) | offset) & MASK32
    masks = {"srr1": SRR1_UNDEFINED_MASK}
    if dar is not None:
        # PAGE 59: "After a DSI or an alignment exception, DAR is set to the effective address (EA)
        # generated by the faulting instruction."
        state.dar = dar & MASK32
        # DSISR contents for alignment are defined only in the PEM (not on disk): PAGE 59 refers out.
        masks["dsisr"] = MASK32
    if extra_masks:
        masks.update(extra_masks)
    state.exception = name
    raise PartiallyUndefined(f"{name} exception taken ({why}); SRR1 exception bits / DSISR not "
                             f"defined by the on-disk manuals PAGE 166, PAGE 59", masks)


def _alignment(state: State, ea: int, why: str, extra_masks: dict | None = None) -> None:
    _take_exception(state, "alignment", 0x00600, ea, why, extra_masks)


# --------------------------------------------------------------------------------------------------
# Environment queries
# --------------------------------------------------------------------------------------------------
def _wimg(state: State, ea: int, what: str) -> int:
    if not (state.msr & MSR_DR):
        return DEFAULT_REAL_MODE_WIMG  # PAGE 308 / PAGE 138
    hook = getattr(state, "data_wimg", None)
    if hook is None:
        raise Unmodelled(f"{what} with MSR[DR]=1: W/I bits of the translated page decide the alignment "
                         f"exception (PAGE 122, PAGE 193) but the neutral State has no BAT/PTE")
    return int(hook(ea & MASK32)) & 0xF


def _phys(state: State, ea: int):
    if not (state.msr & MSR_DR):
        return ea & MASK32  # real addressing mode, PAGE 138
    hook = getattr(state, "data_phys", None)
    if hook is None:
        return None
    return int(hook(ea & MASK32)) & MASK32


def _pipe_check(state: State, ea: int, nbytes: int, what: str) -> None:
    """Write gather pipe (PAGE 314): with HID2[WPE]=1 a non-cacheable store to the WPAR gather address is
    buffered in a FIFO that is not part of the neutral state."""
    if not (state.hid2 & HID2_WPE) or nbytes <= 0:
        return
    if not (state.msr & MSR_DR):
        return  # default WIMG 0b0011 is cacheable (PAGE 138), so the pipe is never used
    wpar = getattr(state, "wpar", None)
    if wpar is None:
        return  # ASSUMPTION documented in the module docstring
    hook = getattr(state, "data_wimg", None)
    if hook is not None and not (int(hook(ea & MASK32)) & WIMG_I):
        return  # cacheable store: not gathered
    # PAGE 314 says both "upper 26 bits" and "bits 0-26"; use the wider 26-bit match (64-byte window).
    gb = wpar & 0xFFFFFFC0
    for k in range(nbytes):
        pa = _phys(state, (ea + k) & MASK32)
        if pa is None or (pa & 0xFFFFFFC0) == gb:
            raise Unmodelled(f"{what}: store may enter the write gather pipe FIFO (HID2[WPE]=1, WPAR set) "
                             f"PAGE 314")


def _crosses(ea: int, n: int) -> bool:
    """String operand boundary rule of PAGE 105 (and 750CL PAGE 103)."""
    last = (ea + n - 1) & MASK32
    if ea & 3:
        return (ea >> 12) != (last >> 12)   # non-word-aligned crossing a 4-Kbyte boundary
    return (ea >> 28) != (last >> 28)       # word-aligned crossing a 256-Mbyte boundary


def _finish(state: State) -> None:
    state.pc = (state.pc + 4) & MASK32
    state.exception = None


# --------------------------------------------------------------------------------------------------
# step
# --------------------------------------------------------------------------------------------------
def step(state: State, word: int) -> None:
    word &= MASK32
    dec = _decode(word)
    if dec is None:
        raise ValueError(f"word {word:08X} is not in area {AREA}")
    name, kind, width, update, byterev, page = dec
    indexed = (word >> 26) in (31, 4)
    if kind in ("load", "loada", "store"):
        _single(state, word, name, kind, width, update, indexed, byterev, page)
    elif kind in ("lmw", "stmw"):
        _multiple(state, word, name, kind == "lmw", page)
    elif kind in ("lswi", "lswx", "stswi", "stswx"):
        _string(state, word, name, kind.startswith("l"), kind.endswith("i"), page)
    elif kind == "lwarx":
        _lwarx(state, word)
    elif kind == "stwcx":
        _stwcx(state, word)
    elif kind == "dcbz":
        _dcbz(state, word)
    elif kind == "dcbz_l":
        _dcbz_l(state, word)
    else:  # pragma: no cover
        raise AssertionError(kind)


def _single(state, word, name, kind, width, update, indexed, byterev, page):
    rd = (word >> 21) & 31   # rD or rS
    ra = (word >> 16) & 31
    rb = (word >> 11) & 31
    # X-form bit 31 is reserved (encoding "... xo 0") for every load/store except stwcx.;
    # PAGE 119 "the Rc bit must be zero for most load and store instructions"; PAGE 103 store with Rc=1
    # is an invalid form; PAGE 87 set reserved bits give boundedly-undefined results.
    if indexed and (word & 1):
        raise UndefinedBehaviour(f"{name} with bit 31 (Rc) set: invalid form PAGE 119, PAGE 103, PAGE 87")
    if update:
        # Loads: PAGE {page} "If rA = 0 or rA = rD, the instruction form is invalid" (also PAGE 102).
        # Stores: PAGE {page} "If rA = 0, the instruction form is invalid" (also PAGE 103); rS = rA valid.
        if ra == 0:
            raise UndefinedBehaviour(f"{name} with rA=0: invalid form PAGE {page}")
        if kind != "store" and ra == rd:
            raise UndefinedBehaviour(f"{name} with rA=rD: invalid form PAGE {page}")
    if state.msr & MSR_LE:
        # PAGE 105: single-register accesses are allowed in little-endian mode, but the byte ordering
        # rules live in the PEM "Byte Ordering" chapter (PAGE 89 refers out) - not on disk.
        raise UndefinedBehaviour(f"{name} with MSR[LE]=1: little-endian byte ordering not specified in the "
                                 f"on-disk manuals PAGE 89, PAGE 105")
    # EA: non-update "if rA = 0 then b <- 0 else b <- (rA)"; update "EA <- (rA) + ..." PAGE {page}, PAGE 89
    base = state.gpr[ra] if (update or ra != 0) else 0
    off = state.gpr[rb] if indexed else _exts16(word)
    ea = (base + off) & MASK32
    if kind == "store":
        _pipe_check(state, ea, width, name)
        val = state.gpr[rd] & ((1 << (8 * width)) - 1)   # rS[24-31] / rS[16-31] / rS PAGE {page}
        if byterev:
            # sthbrx MEM(EA,2) <- rS[24-31] || rS[16-23] PAGE 556; stwbrx PAGE 564
            val = int.from_bytes(val.to_bytes(width, "big")[::-1], "big")
        # PAGE 103: "If rS = rA, the contents of register rS are copied to the target memory element,
        # then the generated EA is placed into rA (rS)."
        state.mem.write(ea, width, val)
        if update:
            state.gpr[ra] = ea                             # rA <- EA PAGE {page}
    else:
        val = state.mem.read(ea, width)                    # big-endian MEM(EA, n) PAGE 81/89
        if byterev:
            # lhbrx rD <- (16)0 || MEM(EA+1,1) || MEM(EA,1) PAGE 442; lwbrx PAGE 451
            val = int.from_bytes(val.to_bytes(width, "big")[::-1], "big")
        if kind == "loada":
            val = _exts16(val)                             # lha: rD <- EXTS(MEM(EA, 2)) PAGE 438
        # lbz/lhz/lwz: rD <- (24)0 || MEM(EA,1) / (16)0 || MEM(EA,2) / MEM(EA,4) PAGE 426/443/452
        state.gpr[rd] = val & MASK32
        if update:
            state.gpr[ra] = ea                             # rA <- EA after the load PAGE {page}
    _finish(state)


def _multiple(state, word, name, load, page):
    r = (word >> 21) & 31
    ra = (word >> 16) & 31
    if load:
        # PAGE 447: "If rA is in the range of registers specified to be loaded, including the case in
        # which rA=0, the instruction form is invalid."  rA in [rD,31] is invalid.  Whether rA=0 is
        # invalid by itself (rD>0) is ambiguous: for lmw r0 is in the range only when rD=0, so the
        # "including rA=0" clause is either redundant or an independent rule -> undefined.
        if ra >= r:
            raise UndefinedBehaviour(f"lmw with rA in rD..r31: invalid form PAGE 447, PAGE 105")
        if ra == 0:
            raise UndefinedBehaviour("lmw with rA=0: 'including the case in which rA=0' is ambiguous PAGE 447")
    # stmw: no invalid form is stated (PAGE 560); rA inside rS..r31 simply stores its value.
    base = state.gpr[ra] if ra != 0 else 0
    ea = (base + _exts16(word)) & MASK32                 # EA <- b + EXTS(d) PAGE 447 / PAGE 560
    # Alignment priorities Table 4-3 PAGE 162: (2) lmw/stmw not word-aligned before (4) MSR[LE] set.
    if ea & 3:
        # PAGE 175 "The operand of lmw, stmw, lwarx, or stwcx. is not word-aligned."  Chapter 12
        # (PAGE 447/560) allows "either ... alignment exception handler is invoked or the results are
        # boundedly undefined"; Chapter 4 resolves it for Gekko.  Table 5-4 PAGE 194 qualifies with
        # "Translation enabled", but PAGE 193 says alignment conditions also occur when MSR[DR]=0, so
        # the exception is taken in both modes.
        _alignment(state, ea, f"{name} operand not word-aligned PAGE 175")
    if state.msr & MSR_LE:
        _alignment(state, ea, f"{name} with MSR[LE]=1 PAGE 175, PAGE 193")
    n = 32 - r                                           # n = (32 - rD) PAGE 447
    if not load:
        _pipe_check(state, ea, 4 * n, name)
    # do while r <= 31: GPR(r) <-> MEM(EA,4); r <- r+1; EA <- EA+4   (ascending order) PAGE 447/560
    for k in range(n):
        a = (ea + 4 * k) & MASK32
        if load:
            state.gpr[r + k] = state.mem.read(a, 4)
        else:
            state.mem.write(a, 4, state.gpr[r + k])
    _finish(state)


def _string(state, word, name, load, immediate, page):
    rd = (word >> 21) & 31
    ra = (word >> 16) & 31
    rb = (word >> 11) & 31          # NB for the immediate forms
    if word & 1:
        raise UndefinedBehaviour(f"{name} with bit 31 set: reserved bit PAGE 119, PAGE 87")
    if immediate:
        n = rb if rb != 0 else 32                        # if NB = 0 then n <- 32 PAGE 448 / PAGE 561
        ea = state.gpr[ra] if ra != 0 else 0             # EA is (rA|0) - no index PAGE 448 / PAGE 561
    else:
        n = state.xer & XER_COUNT                        # n <- XER[25-31] PAGE 449 / PAGE 562
        ea = ((state.gpr[ra] if ra != 0 else 0) + state.gpr[rb]) & MASK32
    nr = (n + 3) // 4                                    # nr = CEIL(n/4)
    rng = {(rd + k) % 32 for k in range(nr)}             # registers wrap through r0 PAGE 448/449
    if load:
        if ra == 0:
            # same "including the case in which rA=0" clause as lmw: ambiguous whether rA=0 alone is invalid
            raise UndefinedBehaviour(f"{name} with rA=0: 'including the case in which rA=0' is ambiguous "
                                     f"PAGE {page}")
        if immediate:
            if ra in rng:
                raise UndefinedBehaviour("lswi with rA in the loaded range: invalid form PAGE 448")
        else:
            # PAGE 449: "If rD = rA or rD = rB, the instruction form is invalid"
            if rd == ra or rd == rb:
                raise UndefinedBehaviour("lswx with rD=rA or rD=rB: invalid form PAGE 449")
            # PAGE 449: rA or rB in range -> "either the system illegal instruction error handler is
            # invoked or the results are boundedly undefined"; Chapter 4 does not resolve it.
            if ra in rng or rb in rng:
                raise UndefinedBehaviour("lswx with rA or rB in the loaded range: illegal-or-boundedly-"
                                         "undefined PAGE 449")
    if state.msr & MSR_LE:
        # RECONCILED (class C6): the zero-length case is NOT undefined.  Table 5-4 PAGE 194: "lmw, stmw,
        # lswi, lswx, stswi, or stswx instruction attempted while MSR[LE] = 1 -> Alignment exception"
        # conditions on the instruction being attempted, and PAGE 198 itself calls the zero-length case an
        # access: "Accesses generated by an lswx or stswx instruction with a zero length".
        # PAGE 175 "A multiple or string access is attempted with MSR[LE] set"; Table 4-3 PAGE 162 (4)
        _alignment(state, ea, f"{name} with MSR[LE]=1 PAGE 175")
    if n > 0 and _crosses(ea, n):
        # PAGE 105 (750CL PAGE 103): "A non-word-aligned string operation that crosses a 4-Kbyte
        # boundary, or a word-aligned string operation that crosses a 256-Mbyte boundary always causes
        # an alignment exception."  The Gekko alignment condition list (PAGE 175, PAGE 159) omits it and
        # Chapter 12 (PAGE 448) says "may be invoked".  Contradictory -> undefined.
        raise UndefinedBehaviour(f"{name} operand crossing a 4K (misaligned) / 256M (aligned) boundary: "
                                 f"PAGE 105 vs PAGE 175 vs PAGE {page}")
    if not load:
        _pipe_check(state, ea, n, name)
    if load and n == 0:
        # PAGE 449 "If n = 0, the contents of rD are undefined."  No memory access (PAGE 198 item 6).
        _finish(state)
        raise PartiallyUndefined("lswx with XER[25-31]=0: rD undefined PAGE 449", {f"gpr[{rd}]": MASK32})
    # Loop RTL PAGE 448/449/561/562: bytes left to right, one MEM(EA,1) per byte, EA ascending.
    r = (rd - 1) % 32
    i = 0
    for k in range(n):
        if i == 0:
            r = (r + 1) % 32
            if load:
                state.gpr[r] = 0                          # GPR(r) <- 0 clears unfilled low bytes
        a = (ea + k) & MASK32
        if load:
            b = state.mem.read(a, 1)
            state.gpr[r] = (state.gpr[r] | (b << (24 - i))) & MASK32   # GPR(r)[i,i+7] <- MEM(EA,1)
        else:
            state.mem.write(a, 1, (state.gpr[r] >> (24 - i)) & 0xFF)   # MEM(EA,1) <- GPR(r)[i,i+7]
        i += 8
        if i == 32:
            i = 0
    # stswx n = 0: "If n = 0, no bytes are stored." PAGE 562
    _finish(state)


def _reservation_w_check(state, ea, name):
    """COMMON-MODE gap (neither original model checked it; added to A and B).  Table 5-4 PAGE 193:
    "lwarx or stwcx. with W = 1 | Reservation instruction to write-through segment or block | DSI exception
    DSISR[5] =1"; PAGE 103: "Executing lwarx and stwcx. to a page marked write-through does not cause a DSI
    exception if the W bit is set"; PAGE 119 says the same.  Contradictory -> undefined for W = 1.  The
    priority against the misalignment alignment exception is not listed in Table 4-3 PAGE 162, so this check
    comes first.  With MSR[DR]=1 and no data_wimg hook the W bit is unknown -> Unmodelled."""
    if _wimg(state, ea, name) & WIMG_W:
        raise UndefinedBehaviour(f"{name} to a write-through (W=1) page: DSI per Table 5-4 PAGE 193 vs no DSI "
                                 f"per PAGE 103 / PAGE 119")


def _lwarx(state, word):
    rd = (word >> 21) & 31
    ra = (word >> 16) & 31
    rb = (word >> 11) & 31
    if state.msr & MSR_LE:
        raise UndefinedBehaviour("lwarx with MSR[LE]=1: byte ordering not on disk PAGE 89, PAGE 105")
    ea = ((state.gpr[ra] if ra != 0 else 0) + state.gpr[rb]) & MASK32   # EA <- (rA|0) + (rB) PAGE 450
    _reservation_w_check(state, ea, "lwarx")
    if ea & 3:
        # PAGE 175; PAGE 119 "If the location is not word-aligned, an alignment exception occurs."
        # RECONCILED (class C8): with Rc=1 CR0 is undefined on this path too.  PAGE 161 ("taken without
        # completing the exception-causing instruction") and PAGE 119 ("If Gekko encounters one of these
        # invalid instruction forms, it sets CR0 to an undefined value") do not say which wins -> undefined.
        _alignment(state, ea, "lwarx operand not word-aligned PAGE 175",
                   {"cr": 0xF0000000} if word & 1 else None)
    pa = _phys(state, ea)
    # RTL PAGE 450: RESERVE <- 1; RESERVE_ADDR <- physical_addr(EA); rD <- MEM(EA,4)
    val = state.mem.read(ea, 4)
    state.gpr[rd] = val
    state.reserve = True
    masks = {}
    if pa is None:
        masks["reserve_addr"] = MASK32   # PA of a translated EA is not derivable from the neutral State
    else:
        state.reserve_addr = pa
    if word & 1:
        # PAGE 119: "If Rc is set, the instruction form is invalid for sync and lwarx instructions. If Gekko
        # encounters one of these invalid instruction forms, it sets CR0 to an undefined value."
        masks["cr"] = 0xF0000000
    _finish(state)
    if masks:
        raise PartiallyUndefined("lwarx: " + ", ".join(sorted(masks)) + " undefined PAGE 119 / PAGE 450", masks)


def _stwcx(state, word):
    rs = (word >> 21) & 31
    ra = (word >> 16) & 31
    rb = (word >> 11) & 31
    if state.msr & MSR_LE:
        raise UndefinedBehaviour("stwcx. with MSR[LE]=1: byte ordering not on disk PAGE 89, PAGE 105")
    ea = ((state.gpr[ra] if ra != 0 else 0) + state.gpr[rb]) & MASK32   # PAGE 565
    _reservation_w_check(state, ea, "stwcx.")
    if ea & 3:
        # PAGE 175: checked regardless of RESERVE - the condition is on the operand, not on the store.
        # RECONCILED (class C8): Rc=0 -> CR0 undefined on this path too (PAGE 119 vs PAGE 161, see _lwarx).
        _alignment(state, ea, "stwcx. operand not word-aligned PAGE 175",
                   None if word & 1 else {"cr": 0xF0000000})
    so = 1 if state.xer & XER_SO else 0
    if state.reserve:
        _pipe_check(state, ea, 4, "stwcx.")
        # PAGE 565: MEM(EA,4) <- rS; CR0 <- 0b00 || 0b1 || XER[SO]; RESERVE <- 0.
        # The reservation address is NOT compared: PAGE 565 "the hardware doesn't compare reservation
        # address"; PAGE 139 "any subsequent stwcx. ... regardless of address, will cancel the reservation".
        state.mem.write(ea, 4, state.gpr[rs])
        cr0 = 0b0010 | so
        state.reserve = False
    else:
        # PAGE 565: CR0 <- 0b00 || 0b0 || XER[SO]; no store; reservation stays clear.
        cr0 = so
    state.cr = ((state.cr & 0x0FFFFFFF) | (cr0 << 28)) & MASK32
    _finish(state)
    if not (word & 1):
        # PAGE 119: "If Rc is zero, executing stwcx. sets CR0 to an undefined value."
        raise PartiallyUndefined("stwcx with Rc=0: CR0 undefined PAGE 119", {"cr": 0xF0000000})


def _dcbz_common_checks(state, word, name, page):
    if (word >> 21) & 31 or (word & 1):
        # Encoding "31 00000 A B 1014 0" / "4 00000 A B 1014 0": reserved fields PAGE 383/384, PAGE 87
        raise UndefinedBehaviour(f"{name} with reserved bits 6-10 or 31 set PAGE {page}, PAGE 87")


def _dcbz(state, word):
    _dcbz_common_checks(state, word, "dcbz", 383)
    ra = (word >> 16) & 31
    rb = (word >> 11) & 31
    if state.msr & MSR_LE:
        raise UndefinedBehaviour("dcbz with MSR[LE]=1: little-endian EA conventions not on disk PAGE 89")
    ea = ((state.gpr[ra] if ra != 0 else 0) + state.gpr[rb]) & MASK32    # EA is (rA|0) + (rB) PAGE 383
    # Exception priorities PAGE 122: 1 cache disabled -> alignment; 2 W or I -> alignment; then DSI.
    if not (state.hid0 & HID0_DCE):
        # PAGE 140 "The dcbz instruction will cause an alignment exception when the data cache is disabled."
        _alignment(state, ea, "dcbz with HID0[DCE]=0 PAGE 140, PAGE 122")
    wimg = _wimg(state, ea, "dcbz")
    if wimg & (WIMG_W | WIMG_I):
        # PAGE 142 "If the page containing the byte addressed by the EA is caching-inhibited or
        # write-through, then the system alignment exception handler is invoked."  (PAGE 383 allows
        # either clearing memory or the handler; Chapters 3/4 resolve it for Gekko.)
        _alignment(state, ea, "dcbz to a write-through or caching-inhibited page PAGE 142")
    # PAGE 142 / PAGE 383: all bytes of the (32-byte, PAGE 25) block containing EA are cleared.
    state.mem.write(ea & BLOCK_MASK, 32, 0)
    _finish(state)


def _dcbz_l(state, word):
    _dcbz_common_checks(state, word, "dcbz_l", 384)
    if not (state.hid2 & HID2_LCE):
        # PAGE 311 "When HID2[LCE] = 0, execution of dcbz_l causes an illegal instruction exception";
        # PAGE 384 "the invalid instruction error handler is envolked"; PAGE 67 "dcbz_l instruction is
        # illegal".  Program exception, vector 0x700 (Table 4-2 PAGE 159), priority 1 (PAGE 162).
        _take_exception(state, "program", 0x00700, None, "dcbz_l with HID2[LCE]=0 PAGE 311")
    # RECONCILED (class C4): the original MSR[PR]=1 -> undefined rule is removed.  The text extraction of
    # PAGE 384 ("VEA Y es X") cannot place the "Yes", but gekko_user_manual.pdf page 384 laid out with
    # `pdftotext -layout` puts "Yes" under "Gekko Specific" with "Supervisor Level" blank; dcbi PAGE 379 has the
    # same flattened text but its "Yes" sits under "Supervisor Level" and its body says "This is a
    # supervisor-level instruction."  dcbz_l is therefore a user-level (VEA) instruction.
    ra = (word >> 16) & 31
    rb = (word >> 11) & 31
    if state.msr & MSR_LE:
        raise UndefinedBehaviour("dcbz_l with MSR[LE]=1: little-endian EA conventions not on disk PAGE 89")
    ea = ((state.gpr[ra] if ra != 0 else 0) + state.gpr[rb]) & MASK32    # EA is (rA|0) + (rB) PAGE 384
    if not (state.hid0 & HID0_DCE):
        # PAGE 175 "An attempt is made to execute dcbz or dcbz_l when the data cache is disabled."
        _alignment(state, ea, "dcbz_l with HID0[DCE]=0 PAGE 175")
    wimg = _wimg(state, ea, "dcbz_l")
    if wimg & (WIMG_W | WIMG_I):
        # PAGE 311 "causes an alignment exception if the page or the block of the effective address is
        # marked as write-through or cache-inhibited"
        _alignment(state, ea, "dcbz_l to a write-through or caching-inhibited page PAGE 311")
    # PAGE 311: a hit in the normal or locked cache sets HID2[DCHERR]=1 (sticky, PAGE 68); if
    # HID2[DCHEE]=MSR[EE]=MSR[ME]=1 and HID2[DCHERR]=0 it also raises machine check with SRR1[10]=1.
    mc_armed = bool(state.hid2 & HID2_DCHEE) and bool(state.msr & MSR_EE) and bool(state.msr & MSR_ME) \
        and not (state.hid2 & HID2_DCHERR)
    hook = getattr(state, "dcache_hit", None)
    hit = None if hook is None else bool(hook(ea & MASK32))
    # PAGE 384: in both the hit and the miss case "All bytes are cleared and the block is marked as M".
    # The block lives in the locked (or normal) L1 cache and is NOT forwarded to L2 or the bus
    # (PAGE 384); state.mem models the processor's load/store view, i.e. what later loads see.
    state.mem.write(ea & BLOCK_MASK, 32, 0)
    if hit:
        state.hid2 = (state.hid2 | HID2_DCHERR) & MASK32
    _finish(state)
    if mc_armed and hit is not False:
        # RECONCILED (class C3): the original raised Unmodelled / UndefinedBehaviour and left the state
        # untouched.  PAGE 384's hit bullet still says "all bytes are cleared and the block is marked M", and
        # PAGE 311 "Gekko sets HID2[DCHERR]=1" on a hit, so both stay defined when the machine check can fire;
        # only the machine check itself is undefined here: SRR0 "On a best-effort basis" (Table 4-10 PAGE 173)
        # and when it is taken relative to this instruction is not stated -> pc/srr0/srr1/msr/exception.
        masks = {"pc": MASK32, "srr0": MASK32, "srr1": MASK32, "msr": MASK32, "exception": 1}
        if hit is None:
            masks["hid2"] = HID2_DCHERR
        raise PartiallyUndefined("dcbz_l hit with HID2[DCHEE]=MSR[EE]=MSR[ME]=1 (or hit/miss unknown): machine "
                                 "check PAGE 311, SRR0 best-effort PAGE 173", masks)
    if hit is None and not (state.hid2 & HID2_DCHERR):
        raise PartiallyUndefined("dcbz_l: HID2[DCHERR] depends on L1 cache occupancy PAGE 311",
                                 {"hid2": HID2_DCHERR})


# ==================================================================================================
# Self-test: hand-derived vectors (expected values written out literally, not computed by the model)
# ==================================================================================================
if __name__ == "__main__":
    import traceback

    def D(op, rd, ra, d):
        return (op << 26) | (rd << 21) | (ra << 16) | (d & 0xFFFF)

    def X(op, rd, ra, rb, xo, rc=0):
        return (op << 26) | (rd << 21) | (ra << 16) | (rb << 11) | (xo << 1) | rc

    PC = 0x80003100

    def mk(msr=0x00002032, **kw):
        s = State()
        s.msr = msr
        s.pc = PC
        for k, v in kw.items():
            setattr(s, k, v)
        return s

    def snap(s):
        return (repr(s.arch_dict()), dict(s.mem.bytes), list(s.mem.log))

    def ok(s, w):
        step(s, w)
        assert s.exception is None, s.exception
        assert s.pc == PC + 4, hex(s.pc)
        return s

    def undef(s, w, cls=UndefinedBehaviour):
        before = snap(s)
        try:
            step(s, w)
        except PartiallyUndefined:
            raise AssertionError("expected a clean UndefinedBehaviour, got PartiallyUndefined")
        except cls:
            assert snap(s) == before, "state modified by an UndefinedBehaviour outcome"
            return
        raise AssertionError("expected UndefinedBehaviour")

    def partial(s, w):
        try:
            step(s, w)
        except PartiallyUndefined as e:
            return e
        raise AssertionError("expected PartiallyUndefined")

    tests = []

    def T(fn):
        tests.append(fn)
        return fn

    # ---- encodings: manual x'....' images with zero operands decode to the right mnemonic ----
    @T
    def t_encodings():
        table = {
            0x88000000: "lbz", 0x8C000000: "lbzu", 0x7C0000EE: "lbzux", 0x7C0000AE: "lbzx",
            0xA8000000: "lha", 0xAC000000: "lhau", 0x7C0002EE: "lhaux", 0x7C0002AE: "lhax",
            0x7C00062C: "lhbrx", 0xA0000000: "lhz", 0xA4000000: "lhzu", 0x7C00026E: "lhzux",
            0x7C00022E: "lhzx", 0xB8000000: "lmw", 0x7C0004AA: "lswi", 0x7C00042A: "lswx",
            0x7C000028: "lwarx", 0x7C00042C: "lwbrx", 0x80000000: "lwz", 0x84000000: "lwzu",
            0x7C00006E: "lwzux", 0x7C00002E: "lwzx", 0x98000000: "stb", 0x9C000000: "stbu",
            0x7C0001EE: "stbux", 0x7C0001AE: "stbx", 0xB0000000: "sth", 0x7C00072C: "sthbrx",
            0xB4000000: "sthu", 0x7C00036E: "sthux", 0x7C00032E: "sthx", 0xBC000000: "stmw",
            0x7C0005AA: "stswi", 0x7C00052A: "stswx", 0x90000000: "stw", 0x7C00052C: "stwbrx",
            0x7C00012D: "stwcx.", 0x94000000: "stwu", 0x7C00016E: "stwux", 0x7C00012E: "stwx",
            0x7C0007EC: "dcbz", 0x100007EC: "dcbz_l",
        }
        for w, m in table.items():
            assert mnemonic(w) == m, (hex(w), mnemonic(w), m)
        # not this area: addi, add, lfd, ps_add (op4 xo5 21), mfspr, opcode 4 xo10 1013
        for w in (0x38600001, 0x7C632214, 0xC8030000, 0x1000002A, 0x7C0802A6, 0x100007EA):
            assert not supports(w), hex(w)

    # ---- lwz family ----
    @T
    def t_lwz_basic():
        s = mk(); s.gpr[4] = 0x80001000
        s.mem.load(0x80001010, bytes([0xDE, 0xAD, 0xBE, 0xEF]))
        ok(s, D(32, 3, 4, 0x10))
        assert s.gpr[3] == 0xDEADBEEF and s.gpr[4] == 0x80001000
        assert s.mem.log == [("r", 0x80001010, 4, 0xDEADBEEF)]

    @T
    def t_lwz_ra0_absolute():
        s = mk(); s.gpr[0] = 0x12345678
        s.mem.load(0x00000100, bytes([1, 2, 3, 4]))
        ok(s, D(32, 3, 0, 0x100))
        assert s.gpr[3] == 0x01020304 and s.mem.log == [("r", 0x100, 4, 0x01020304)]

    @T
    def t_lwz_negative_disp():
        s = mk(); s.gpr[4] = 0x80001000
        s.mem.load(0x80000FFC, bytes([0xCA, 0xFE, 0xBA, 0xBE]))
        ok(s, D(32, 3, 4, -4))
        assert s.gpr[3] == 0xCAFEBABE and s.mem.log[0][1] == 0x80000FFC

    @T
    def t_lwz_rd_equals_ra_nonupdate():
        s = mk(); s.gpr[3] = 0x80002000
        s.mem.load(0x80002004, bytes([0, 0, 0, 7]))
        ok(s, D(32, 3, 3, 4))
        assert s.gpr[3] == 7

    @T
    def t_lwz_ea_wraps():
        s = mk(); s.gpr[4] = 0xFFFFFFFE
        s.mem.load(0xFFFFFFFE, bytes([0x11, 0x22])); s.mem.load(0, bytes([0x33, 0x44]))
        ok(s, D(32, 3, 4, 0))
        assert s.gpr[3] == 0x11223344 and s.mem.log == [("r", 0xFFFFFFFE, 4, 0x11223344)]

    @T
    def t_lwz_ea_wraps_by_displacement():
        s = mk(); s.gpr[4] = 0x00000002
        s.mem.load(0xFFFFFFFE, bytes([0, 0, 0, 0x55]))
        ok(s, D(32, 3, 4, -4))
        assert s.mem.log == [("r", 0xFFFFFFFE, 4, 0x55)] and s.gpr[3] == 0x55

    @T
    def t_lwzu():
        s = mk(); s.gpr[4] = 0x80001000
        s.mem.load(0x80001008, bytes([0, 0, 1, 0]))
        ok(s, D(33, 3, 4, 8))
        assert s.gpr[3] == 0x100 and s.gpr[4] == 0x80001008

    @T
    def t_lwzu_ra0_invalid():
        undef(mk(), D(33, 3, 0, 8))

    @T
    def t_lwzu_ra_eq_rd_invalid():
        s = mk(); s.gpr[3] = 0x80001000
        undef(s, D(33, 3, 3, 8))

    @T
    def t_lwzx_ra0():
        s = mk(); s.gpr[0] = 0xFFFFFFFF; s.gpr[5] = 0x80004000
        s.mem.load(0x80004000, bytes([9, 8, 7, 6]))
        ok(s, X(31, 3, 0, 5, 23))
        assert s.gpr[3] == 0x09080706

    @T
    def t_lwzux_rb_eq_ra():
        s = mk(); s.gpr[4] = 0x40000800
        s.mem.load(0x80001000, bytes([0, 0, 0, 0x2A]))
        ok(s, X(31, 3, 4, 4, 55))
        assert s.gpr[3] == 0x2A and s.gpr[4] == 0x80001000

    @T
    def t_lwzux_rb_eq_rd():
        s = mk(); s.gpr[4] = 0x80001000; s.gpr[3] = 0x20
        s.mem.load(0x80001020, bytes([0xAB, 0xCD, 0xEF, 0x01]))
        ok(s, X(31, 3, 4, 3, 55))
        assert s.gpr[3] == 0xABCDEF01 and s.gpr[4] == 0x80001020

    @T
    def t_lwzux_ra0_invalid():
        undef(mk(), X(31, 3, 0, 5, 55))

    @T
    def t_lwzx_rc_invalid():
        undef(mk(), X(31, 3, 4, 5, 23, rc=1))

    # ---- byte / half ----
    @T
    def t_lbz_zero_extends():
        s = mk(); s.gpr[3] = 0xFFFFFFFF; s.gpr[4] = 0x80000000
        s.mem.load(0x80000001, bytes([0xFF, 0x77]))
        ok(s, D(34, 3, 4, 1))
        assert s.gpr[3] == 0x000000FF and s.mem.log == [("r", 0x80000001, 1, 0xFF)]

    @T
    def t_lbzu_lbzux():
        s = mk(); s.gpr[4] = 0x80000000; s.gpr[6] = 3
        s.mem.load(0x80000000, bytes([0x10, 0x81, 0x82, 0x83, 0x84]))
        ok(s, D(35, 3, 4, 2))
        assert s.gpr[3] == 0x82 and s.gpr[4] == 0x80000002
        s.pc = PC
        ok(s, X(31, 5, 4, 6, 119))
        assert s.gpr[5] == 0 and s.gpr[4] == 0x80000005  # byte at 0x80000005 unset -> 0

    @T
    def t_lbzx():
        s = mk(); s.gpr[4] = 0x80000000; s.gpr[5] = 4
        s.mem.load(0x80000004, bytes([0x9C]))
        ok(s, X(31, 3, 4, 5, 87))
        assert s.gpr[3] == 0x9C

    @T
    def t_lbzux_ra_eq_rd_invalid():
        undef(mk(), X(31, 4, 4, 5, 119))

    @T
    def t_lhz_zero_extends():
        s = mk(); s.gpr[4] = 0x80000000
        s.mem.load(0x80000010, bytes([0x80, 0x01]))
        ok(s, D(40, 3, 4, 0x10))
        assert s.gpr[3] == 0x00008001 and s.mem.log == [("r", 0x80000010, 2, 0x8001)]

    @T
    def t_lha_sign_extends():
        s = mk(); s.gpr[4] = 0x80000000
        s.mem.load(0x80000010, bytes([0x80, 0x01, 0x7F, 0xFF]))
        ok(s, D(42, 3, 4, 0x10))
        assert s.gpr[3] == 0xFFFF8001
        s.pc = PC
        ok(s, D(42, 3, 4, 0x12))
        assert s.gpr[3] == 0x00007FFF

    @T
    def t_lhau_lhax_lhaux():
        s = mk(); s.gpr[4] = 0x80000000; s.gpr[5] = 2
        s.mem.load(0x80000000, bytes([0x00, 0x00, 0xFF, 0xFE, 0x12, 0x34]))
        ok(s, D(43, 3, 4, 2))
        assert s.gpr[3] == 0xFFFFFFFE and s.gpr[4] == 0x80000002
        s.pc = PC
        ok(s, X(31, 6, 4, 5, 343))
        assert s.gpr[6] == 0x00001234 and s.gpr[4] == 0x80000002
        s.pc = PC
        ok(s, X(31, 7, 0, 4, 343))        # lhax with rA=0: EA = r4 = 0x80000002
        assert s.gpr[7] == 0xFFFFFFFE
        s.pc = PC
        ok(s, X(31, 8, 4, 5, 375))
        assert s.gpr[8] == 0x00001234 and s.gpr[4] == 0x80000004

    @T
    def t_lhzu_lhzux_lhzx():
        s = mk(); s.gpr[4] = 0x80000000; s.gpr[5] = 0x10
        s.mem.load(0x80000010, bytes([0xFE, 0xDC, 0xBA, 0x98]))
        ok(s, X(31, 3, 0, 5, 279))           # lhzx rA=0 -> EA 0x10 (unset memory)
        assert s.gpr[3] == 0 and s.mem.log[-1] == ("r", 0x10, 2, 0)
        s.pc = PC
        ok(s, X(31, 3, 4, 5, 311))           # lhzux
        assert s.gpr[3] == 0xFEDC and s.gpr[4] == 0x80000010
        s.pc = PC
        ok(s, D(41, 6, 4, 2))                # lhzu
        assert s.gpr[6] == 0xBA98 and s.gpr[4] == 0x80000012

    # ---- stores ----
    @T
    def t_stw_big_endian():
        s = mk(); s.gpr[3] = 0x01020304; s.gpr[4] = 0x80001000
        ok(s, D(36, 3, 4, 0x20))
        assert [s.mem.bytes[0x80001020 + i] for i in range(4)] == [1, 2, 3, 4]
        assert s.mem.log == [("w", 0x80001020, 4, 0x01020304)]

    @T
    def t_stb_low_byte_only():
        s = mk(); s.gpr[3] = 0x12345678; s.gpr[4] = 0x80001000
        s.mem.load(0x80000FFF, bytes([0xAA, 0xAA, 0xAA]))
        ok(s, D(38, 3, 4, 0))
        assert s.mem.bytes[0x80000FFF] == 0xAA and s.mem.bytes[0x80001000] == 0x78 \
            and s.mem.bytes[0x80001001] == 0xAA
        assert s.mem.log == [("w", 0x80001000, 1, 0x78)]

    @T
    def t_sth_low_half():
        s = mk(); s.gpr[3] = 0x12345678; s.gpr[4] = 0x80001000
        ok(s, D(44, 3, 4, 2))
        assert s.mem.bytes[0x80001002] == 0x56 and s.mem.bytes[0x80001003] == 0x78
        assert s.mem.log == [("w", 0x80001002, 2, 0x5678)]

    @T
    def t_stwu_rs_eq_ra_stack_push():
        s = mk(); s.gpr[1] = 0x80400000
        ok(s, D(37, 1, 1, -8))
        assert s.mem.log == [("w", 0x803FFFF8, 4, 0x80400000)] and s.gpr[1] == 0x803FFFF8

    @T
    def t_stbu_ra0_invalid():
        undef(mk(), D(39, 3, 0, 1))

    @T
    def t_sthu_sthux_stbux_stwux():
        s = mk(); s.gpr[3] = 0xAABBCCDD; s.gpr[4] = 0x80000000; s.gpr[5] = 4
        ok(s, D(45, 3, 4, 2))
        assert s.mem.log[-1] == ("w", 0x80000002, 2, 0xCCDD) and s.gpr[4] == 0x80000002
        s.pc = PC
        ok(s, X(31, 3, 4, 5, 439))
        assert s.mem.log[-1] == ("w", 0x80000006, 2, 0xCCDD) and s.gpr[4] == 0x80000006
        s.pc = PC
        ok(s, X(31, 3, 4, 5, 247))
        assert s.mem.log[-1] == ("w", 0x8000000A, 1, 0xDD) and s.gpr[4] == 0x8000000A
        s.pc = PC
        ok(s, X(31, 4, 4, 5, 183))           # stwux rS = rA: stores old rA
        assert s.mem.log[-1] == ("w", 0x8000000E, 4, 0x8000000A) and s.gpr[4] == 0x8000000E

    @T
    def t_stwx_stbx_sthx_ra0():
        s = mk(); s.gpr[0] = 0xDEAD0000; s.gpr[3] = 0x11223344; s.gpr[5] = 0x80005000
        ok(s, X(31, 3, 0, 5, 151))
        assert s.mem.log[-1] == ("w", 0x80005000, 4, 0x11223344)
        s.pc = PC
        ok(s, X(31, 3, 0, 5, 215))
        assert s.mem.log[-1] == ("w", 0x80005000, 1, 0x44)
        s.pc = PC
        ok(s, X(31, 3, 0, 5, 407))
        assert s.mem.log[-1] == ("w", 0x80005000, 2, 0x3344)

    @T
    def t_sthx_rc_invalid():
        undef(mk(), X(31, 3, 4, 5, 407, rc=1))

    @T
    def t_stwux_ra0_invalid():
        undef(mk(), X(31, 3, 0, 5, 183))

    # ---- byte reverse ----
    @T
    def t_byte_reverse():
        s = mk(); s.gpr[4] = 0x80000000; s.gpr[5] = 0; s.gpr[3] = 0x11223344
        s.mem.load(0x80000000, bytes([0x01, 0x02, 0x03, 0x04]))
        ok(s, X(31, 6, 4, 5, 534))
        assert s.gpr[6] == 0x04030201 and s.mem.log[-1] == ("r", 0x80000000, 4, 0x01020304)
        s.pc = PC
        ok(s, X(31, 7, 4, 5, 790))
        assert s.gpr[7] == 0x00000201
        s.pc = PC
        ok(s, X(31, 3, 4, 5, 662))
        assert [s.mem.bytes[0x80000000 + i] for i in range(4)] == [0x44, 0x33, 0x22, 0x11]
        s.pc = PC
        ok(s, X(31, 3, 4, 5, 918))
        assert [s.mem.bytes[0x80000000 + i] for i in range(4)] == [0x44, 0x33, 0x22, 0x11]
        assert s.mem.log[-1] == ("w", 0x80000000, 2, 0x4433)

    # ---- little-endian single-register access ----
    @T
    def t_single_le_undefined():
        s = mk(msr=0x00002033); s.gpr[4] = 0x80000000
        undef(s, D(32, 3, 4, 0))

    # ---- lmw / stmw ----
    @T
    def t_lmw_order():
        s = mk(); s.gpr[3] = 0x80004000
        s.mem.load(0x80004010, bytes([0x11] * 4 + [0x22] * 4 + [0x33] * 4))
        ok(s, D(46, 29, 3, 0x10))
        assert (s.gpr[29], s.gpr[30], s.gpr[31]) == (0x11111111, 0x22222222, 0x33333333)
        assert s.mem.log == [("r", 0x80004010, 4, 0x11111111), ("r", 0x80004014, 4, 0x22222222),
                             ("r", 0x80004018, 4, 0x33333333)]

    @T
    def t_lmw_misaligned_alignment():
        s = mk(); s.gpr[3] = 0x80004000; s.dsisr = 0x5A5A5A5A
        e = partial(s, D(46, 29, 3, 0x11))
        assert s.exception == "alignment" and s.srr0 == PC and s.pc == 0x00000600
        assert s.msr == 0x00000000 and s.srr1 == 0x00002032 and s.dar == 0x80004011
        assert s.gpr[29] == 0 and s.mem.log == []
        assert e.masks == {"srr1": 0xF83F0080, "dsisr": 0xFFFFFFFF}

    @T
    def t_lmw_alignment_ip_ile():
        s = mk(msr=0x00019072); s.gpr[3] = 0x80004000
        partial(s, D(46, 30, 3, 2))
        assert s.pc == 0xFFF00600 and s.msr == 0x00011041 and s.srr1 == 0x00009072 and s.srr0 == PC

    @T
    def t_lmw_ra_in_range_invalid():
        s = mk(); s.gpr[30] = 0x80004000
        undef(s, D(46, 28, 30, 0))

    @T
    def t_lmw_ra0_undefined():
        undef(mk(), D(46, 28, 0, 0x100))

    @T
    def t_stmw_rs30_and_ra_in_range():
        s = mk(); s.gpr[1] = 0x80400000; s.gpr[30] = 0xAAAA0000; s.gpr[31] = 0xBBBB1111
        ok(s, D(47, 30, 1, 4))
        assert s.mem.log == [("w", 0x80400004, 4, 0xAAAA0000), ("w", 0x80400008, 4, 0xBBBB1111)]
        s2 = mk(); s2.gpr[30] = 0x80400000; s2.gpr[29] = 1; s2.gpr[31] = 3
        ok(s2, D(47, 29, 30, 0))
        assert s2.mem.log == [("w", 0x80400000, 4, 1), ("w", 0x80400004, 4, 0x80400000),
                              ("w", 0x80400008, 4, 3)]

    @T
    def t_stmw_rs0_all32():
        s = mk(); s.gpr = [0x100 + i for i in range(32)]; s.gpr[5] = 0x80400000
        ok(s, D(47, 0, 5, 0))
        assert len(s.mem.log) == 32 and s.mem.log[0] == ("w", 0x80400000, 4, 0x100) \
            and s.mem.log[31] == ("w", 0x8040007C, 4, 0x11F) and s.mem.log[5] == ("w", 0x80400014, 4, 0x80400000)

    @T
    def t_stmw_le_alignment():
        s = mk(msr=0x00002033); s.gpr[1] = 0x80400000
        partial(s, D(47, 31, 1, 0))
        assert s.exception == "alignment" and s.dar == 0x80400000 and s.mem.log == []
        assert s.msr == 0 and s.srr1 == 0x00002033

    # ---- lswi / lswx / stswi / stswx ----
    @T
    def t_lswi_partial_register():
        s = mk(); s.gpr[4] = 0x80006000; s.gpr[6] = 0xFFFFFFFF
        s.mem.load(0x80006000, bytes([0x11, 0x22, 0x33, 0x44, 0x55, 0x66]))
        ok(s, X(31, 5, 4, 6, 597))           # lswi r5, r4, NB=6
        assert s.gpr[5] == 0x11223344 and s.gpr[6] == 0x55660000
        assert s.mem.log == [("r", 0x80006000 + k, 1, [0x11, 0x22, 0x33, 0x44, 0x55, 0x66][k]) for k in range(6)]

    @T
    def t_lswi_nb0_is_32():
        s = mk(); s.gpr[4] = 0x80006000
        s.mem.load(0x80006000, bytes(range(1, 33)))
        ok(s, X(31, 24, 4, 0, 597))
        assert s.gpr[24] == 0x01020304 and s.gpr[31] == 0x1D1E1F20 and len(s.mem.log) == 32

    @T
    def t_lswi_wraps_to_r0():
        s = mk(); s.gpr[4] = 0x80006000
        s.mem.load(0x80006000, bytes([1, 2, 3, 4, 5, 6, 7, 8]))
        ok(s, X(31, 31, 4, 8, 597))
        assert s.gpr[31] == 0x01020304 and s.gpr[0] == 0x05060708

    @T
    def t_lswi_ra_in_range_invalid():
        s = mk(); s.gpr[4] = 0x80006000
        undef(s, X(31, 3, 4, 8, 597))

    @T
    def t_lswi_misaligned_no_cross_ok():
        s = mk(); s.gpr[4] = 0x80006001
        s.mem.load(0x80006001, bytes([0xA, 0xB, 0xC]))
        ok(s, X(31, 5, 4, 3, 597))
        assert s.gpr[5] == 0x0A0B0C00

    @T
    def t_lswi_boundary_rules():
        s = mk(); s.gpr[4] = 0x80000FFE
        undef(s, X(31, 5, 4, 4, 597))         # misaligned, crosses 4K
        s = mk(); s.gpr[4] = 0x8FFFFFFC
        undef(s, X(31, 5, 4, 8, 597))         # aligned, crosses 256M
        s = mk(); s.gpr[4] = 0x80000FFC
        s.mem.load(0x80000FFC, bytes([1, 2, 3, 4, 5, 6, 7, 8]))
        ok(s, X(31, 5, 4, 8, 597))            # aligned, crosses 4K only: defined
        assert s.gpr[5] == 0x01020304 and s.gpr[6] == 0x05060708

    @T
    def t_lswx_count_from_xer_low7():
        s = mk(xer=0xE0000005); s.gpr[4] = 0x80006000; s.gpr[5] = 0x10
        s.mem.load(0x80006010, bytes([0xA1, 0xA2, 0xA3, 0xA4, 0xA5]))
        ok(s, X(31, 7, 4, 5, 533))
        assert s.gpr[7] == 0xA1A2A3A4 and s.gpr[8] == 0xA5000000 and len(s.mem.log) == 5
        assert s.xer == 0xE0000005

    @T
    def t_lswx_zero_length():
        s = mk(xer=0x20000080); s.gpr[4] = 0x80006000; s.gpr[7] = 0x1234   # XER[25-31] = 0 (bit 24 set)
        e = partial(s, X(31, 7, 4, 5, 533))
        assert e.masks == {"gpr[7]": 0xFFFFFFFF} and s.mem.log == [] and s.pc == PC + 4 and s.exception is None

    @T
    def t_lswx_invalid_forms():
        s = mk(xer=4); s.gpr[4] = 0x80006000; s.gpr[5] = 0
        undef(s, X(31, 5, 4, 5, 533))         # rD = rB
        undef(s, X(31, 4, 4, 5, 533))         # rD = rA
        undef(s, X(31, 3, 4, 3, 533))         # rD = rB = 3
        s2 = mk(xer=8); s2.gpr[4] = 0x80006000; s2.gpr[8] = 0
        undef(s2, X(31, 7, 4, 8, 533))        # rB in range {7,8}
        s3 = mk(xer=8); s3.gpr[8] = 0x80006000; s3.gpr[2] = 0
        undef(s3, X(31, 7, 8, 2, 533))        # rA in range {7,8}

    @T
    def t_stswi_order():
        s = mk(); s.gpr[4] = 0x80007000; s.gpr[30] = 0x11223344; s.gpr[31] = 0x55667788
        ok(s, X(31, 30, 4, 7, 725))
        assert s.mem.log == [("w", 0x80007000 + k, 1, [0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77][k])
                             for k in range(7)]

    @T
    def t_stswi_ra0_nb0():
        s = mk(); s.gpr = [0x01010101 * (i + 1) & 0xFFFFFFFF for i in range(32)]
        ok(s, X(31, 28, 0, 0, 725))           # EA = 0, 32 bytes from r28..r31 then wrap r0..r3
        assert len(s.mem.log) == 32 and s.mem.log[0] == ("w", 0, 1, 0x1D) and s.mem.log[15] == ("w", 15, 1, 0x20)
        assert s.mem.log[16] == ("w", 16, 1, 0x01) and s.mem.log[31] == ("w", 31, 1, 0x04)

    @T
    def t_stswx_zero_and_wrap():
        s = mk(xer=0x80000000); s.gpr[4] = 0x80007000; s.gpr[5] = 0
        ok(s, X(31, 3, 4, 5, 661))
        assert s.mem.log == []
        s = mk(xer=6); s.gpr[4] = 0x80007000; s.gpr[5] = 2; s.gpr[31] = 0xA0B0C0D0; s.gpr[0] = 0xE0F00000
        ok(s, X(31, 31, 4, 5, 661))
        assert [e[3] for e in s.mem.log] == [0xA0, 0xB0, 0xC0, 0xD0, 0xE0, 0xF0]
        assert s.mem.log[0][1] == 0x80007002

    @T
    def t_string_le_alignment():
        s = mk(msr=0x00002033, xer=3); s.gpr[4] = 0x80007000
        partial(s, X(31, 3, 4, 5, 661))
        assert s.exception == "alignment" and s.pc == 0x600 and s.dar == 0x80007000

    # ---- lwarx / stwcx. ----
    @T
    def t_lwarx_real_mode():
        s = mk(msr=0x00002022); s.gpr[4] = 0x00003000; s.gpr[5] = 4
        s.mem.load(0x00003004, bytes([0, 0, 0, 9]))
        ok(s, X(31, 3, 4, 5, 20))
        assert s.gpr[3] == 9 and s.reserve is True and s.reserve_addr == 0x00003004
        assert s.mem.log == [("r", 0x3004, 4, 9)]

    @T
    def t_lwarx_translated_reserve_addr_masked():
        s = mk(); s.gpr[5] = 0x80003000
        undef(s, X(31, 3, 0, 5, 20), Unmodelled)          # DR=1, W bit unknown (reconciliation)
        s = mk(); s.gpr[5] = 0x80003000; s.data_wimg = lambda ea: 0b0010
        e = partial(s, X(31, 3, 0, 5, 20))
        assert e.masks == {"reserve_addr": 0xFFFFFFFF} and s.reserve is True and s.pc == PC + 4

    @T
    def t_lwarx_misaligned():
        s = mk(msr=0x00002022); s.gpr[5] = 0x80003002
        partial(s, X(31, 3, 0, 5, 20))
        assert s.exception == "alignment" and s.reserve is False and s.dar == 0x80003002 and s.mem.log == []

    @T
    def t_lwarx_rc1_cr0_undefined():
        s = mk(msr=0x00002022); s.gpr[5] = 0x3000
        e = partial(s, X(31, 3, 0, 5, 20, rc=1))
        assert e.masks == {"cr": 0xF0000000} and s.reserve is True

    @T
    def t_stwcx_success_ignores_address():
        s = mk(msr=0x00002022, cr=0x02345678, xer=0x80000000, reserve=True, reserve_addr=0x1000)
        s.gpr[3] = 0xFEEDFACE; s.gpr[5] = 0x2000
        ok(s, X(31, 3, 0, 5, 150, rc=1))
        assert s.mem.log == [("w", 0x2000, 4, 0xFEEDFACE)] and s.cr == 0x32345678 and s.reserve is False

    @T
    def t_stwcx_fail_no_store():
        s = mk(msr=0x00002022, cr=0xF2345678, xer=0x00000000, reserve=False)
        s.gpr[3] = 1; s.gpr[5] = 0x2000
        ok(s, X(31, 3, 0, 5, 150, rc=1))
        assert s.mem.log == [] and s.cr == 0x02345678 and s.reserve is False
        s = mk(msr=0x00002022, cr=0xF2345678, xer=0xC0000000, reserve=False); s.gpr[5] = 0x2000
        ok(s, X(31, 3, 0, 5, 150, rc=1))
        assert s.cr == 0x12345678

    @T
    def t_stwcx_rc0_and_misaligned():
        s = mk(msr=0x00002022, reserve=True); s.gpr[3] = 5; s.gpr[5] = 0x2000
        e = partial(s, X(31, 3, 0, 5, 150, rc=0))
        assert e.masks == {"cr": 0xF0000000} and s.mem.log == [("w", 0x2000, 4, 5)] and s.reserve is False
        s = mk(msr=0x00002022, reserve=False, cr=0x12345678); s.gpr[5] = 0x2001
        partial(s, X(31, 3, 0, 5, 150, rc=1))
        assert s.exception == "alignment" and s.cr == 0x12345678 and s.dar == 0x2001

    # ---- dcbz / dcbz_l ----
    @T
    def t_dcbz_block():
        s = mk(msr=0x00002022, hid0=HID0_DCE); s.gpr[4] = 0x80003000; s.gpr[5] = 0x13
        s.mem.load(0x80002FFF, bytes([0xAA] * 34))
        ok(s, X(31, 0, 4, 5, 1014))
        assert s.mem.bytes[0x80002FFF] == 0xAA and s.mem.bytes[0x80003020] == 0xAA
        assert all(s.mem.bytes[0x80003000 + i] == 0 for i in range(32))
        assert s.mem.log == [("w", 0x80003000, 32, 0)]

    @T
    def t_dcbz_cache_disabled():
        s = mk(msr=0x00002022, hid0=0); s.gpr[5] = 0x80003013
        partial(s, X(31, 0, 0, 5, 1014))
        assert s.exception == "alignment" and s.dar == 0x80003013 and s.mem.log == [] and s.pc == 0x600

    @T
    def t_dcbz_translated_needs_wimg():
        s = mk(hid0=HID0_DCE); s.gpr[5] = 0x80003000
        undef(s, X(31, 0, 0, 5, 1014), Unmodelled)
        s = mk(hid0=HID0_DCE); s.gpr[5] = 0xCC003000
        s.data_wimg = lambda ea: 0b0100
        partial(s, X(31, 0, 0, 5, 1014))
        assert s.exception == "alignment" and s.dar == 0xCC003000
        s = mk(hid0=HID0_DCE); s.gpr[5] = 0x80003008
        s.data_wimg = lambda ea: 0b0010
        ok(s, X(31, 0, 0, 5, 1014))
        assert s.mem.log == [("w", 0x80003000, 32, 0)]

    @T
    def t_dcbz_reserved_bits():
        undef(mk(msr=0x00002022, hid0=HID0_DCE), X(31, 1, 0, 5, 1014))
        undef(mk(msr=0x00002022, hid0=HID0_DCE), X(31, 0, 0, 5, 1014, rc=1))

    @T
    def t_dcbz_l_illegal_when_lce0():
        s = mk(msr=0x00002022, hid0=HID0_DCE, hid2=0xE0000000, dar=0x1234, dsisr=0x5678); s.gpr[5] = 0xE0000000
        e = partial(s, X(4, 0, 0, 5, 1014))
        assert s.exception == "program" and s.pc == 0x700 and s.srr0 == PC and s.srr1 == 0x00002022
        assert s.msr == 0 and s.dar == 0x1234 and s.dsisr == 0x5678 and s.mem.log == []
        assert e.masks == {"srr1": 0xF83F0080}

    @T
    def t_dcbz_l_miss_and_hit():
        s = mk(msr=0x00002022, hid0=HID0_DCE, hid2=0xF0000000); s.gpr[5] = 0xE0000021
        s.dcache_hit = lambda ea: False
        ok(s, X(4, 0, 0, 5, 1014))
        assert s.mem.log == [("w", 0xE0000020, 32, 0)] and s.hid2 == 0xF0000000
        s = mk(msr=0x00002022, hid0=HID0_DCE, hid2=0xF0000000); s.gpr[5] = 0xE0000040
        s.dcache_hit = lambda ea: True
        ok(s, X(4, 0, 0, 5, 1014))
        assert s.hid2 == 0xF0800000 and s.mem.log == [("w", 0xE0000040, 32, 0)]

    @T
    def t_dcbz_l_hit_machine_check_undefined():
        s = mk(msr=0x0000B022, hid0=HID0_DCE, hid2=0xF0080000); s.gpr[5] = 0xE0000040   # EE|ME, DCHEE
        s.dcache_hit = lambda ea: True
        e = partial(s, X(4, 0, 0, 5, 1014))
        assert e.masks == {"pc": 0xFFFFFFFF, "srr0": 0xFFFFFFFF, "srr1": 0xFFFFFFFF, "msr": 0xFFFFFFFF, "exception": 1}
        assert s.mem.log == [("w", 0xE0000040, 32, 0)] and s.hid2 == 0xF0880000
        s = mk(msr=0x0000B022, hid0=HID0_DCE, hid2=0xF0080000); s.gpr[5] = 0xE0000040   # no hook
        e = partial(s, X(4, 0, 0, 5, 1014))
        assert e.masks["hid2"] == 0x00800000 and s.mem.log == [("w", 0xE0000040, 32, 0)]
        s = mk(msr=0x0000B022, hid0=HID0_DCE, hid2=0xF0080000); s.gpr[5] = 0xE0000040
        s.dcache_hit = lambda ea: False                                                  # miss: no machine check
        ok(s, X(4, 0, 0, 5, 1014))
        assert s.hid2 == 0xF0080000

    @T
    def t_reconciled_classes():
        # C1: SRR1 copies MSR bits 28/29, bit 24 undefined; new MSR reserved bits cleared
        s = mk(msr=0x8002B0BE); s.gpr[1] = 0x80001001
        e = partial(s, D(46, 30, 1, 0))
        assert (s.srr1 & 0x07C0FF7F) == 0x0000B03E and s.msr == 0x00001000 and e.masks["srr1"] == 0xF83F0080
        # C4: dcbz_l is legal with MSR[PR]=1
        s = mk(msr=0x00006022, hid0=HID0_DCE, hid2=0xF0800000); s.gpr[5] = 0xE0000000
        ok(s, X(4, 0, 0, 5, 1014))
        assert s.mem.log == [("w", 0xE0000000, 32, 0)]
        # C6: zero-length lswx with MSR[LE]=1 takes the alignment exception
        s = mk(msr=0x00002033, xer=0); s.gpr[4] = 0x80001000; s.gpr[7] = 0
        partial(s, X(31, 8, 4, 7, 533))
        assert s.exception == "alignment" and s.pc == 0x600 and s.dar == 0x80001000 and s.mem.log == []
        # C8: stwcx. Rc=0 misaligned -> alignment with CR0 undefined
        s = mk(msr=0x00002022, reserve=True, cr=0x11111111); s.gpr[3] = 7; s.gpr[4] = 0x80002002
        e = partial(s, X(31, 3, 0, 4, 150, rc=0))
        assert e.masks.get("cr") == 0xF0000000 and s.exception == "alignment" and s.reserve is True
        # W=1 lwarx / stwcx.: contradictory manual -> undefined
        s = mk(); s.gpr[4] = 0x80001000; s.data_wimg = lambda ea: 0b1000
        undef(s, X(31, 3, 0, 4, 20))
        undef(s, X(31, 3, 0, 4, 150, rc=1))

    @T
    def t_dcbz_l_no_hook():
        s = mk(msr=0x00002022, hid0=HID0_DCE, hid2=0xF0000000); s.gpr[5] = 0xE0000000
        e = partial(s, X(4, 0, 0, 5, 1014))
        assert e.masks == {"hid2": 0x00800000} and s.mem.log == [("w", 0xE0000000, 32, 0)] and s.pc == PC + 4
        s = mk(msr=0x00002022, hid0=HID0_DCE, hid2=0xF0800000); s.gpr[5] = 0xE0000000   # DCHERR already sticky
        ok(s, X(4, 0, 0, 5, 1014))
        assert s.hid2 == 0xF0800000

    @T
    def t_dcbz_l_cache_disabled_alignment():
        s = mk(msr=0x00002022, hid0=0, hid2=0xF0000000); s.gpr[5] = 0xE0000004
        partial(s, X(4, 0, 0, 5, 1014))
        assert s.exception == "alignment" and s.dar == 0xE0000004

    # ---- write gather pipe hook ----
    @T
    def t_pipe():
        s = mk(hid2=0xE0000000); s.wpar = 0xCC008000; s.gpr[3] = 0x61000000; s.gpr[4] = 0xCC008000
        undef(s, D(36, 3, 4, 0), Unmodelled)
        s = mk(msr=0x00002022, hid2=0xE0000000); s.wpar = 0xCC008000; s.gpr[3] = 0x61000000; s.gpr[4] = 0xCC008000
        ok(s, D(36, 3, 4, 0))
        assert s.mem.log == [("w", 0xCC008000, 4, 0x61000000)]

    failed = 0
    for fn in tests:
        try:
            fn()
        except Exception:  # noqa: BLE001
            failed += 1
            print(f"FAIL {fn.__name__}")
            traceback.print_exc()
    print(f"{len(tests) - failed}/{len(tests)} self-tests passed")
    sys.exit(1 if failed else 0)
