"""Independent semantic model, author A, area "loadstore".

Integer load/store instructions of the IBM Gekko, derived ONLY from
docs/reference/ppc/gekko_user_manual.txt (cited "PAGE N" = the "===== PAGE N =====" marker)
with ppc_750cl.txt used as a cross-check (cited "750CL PAGE N").  No GekkoForge source was
consulted.

Covered
  D-form   32 lwz, 33 lwzu, 34 lbz, 35 lbzu, 36 stw, 37 stwu, 38 stb, 39 stbu,
           40 lhz, 41 lhzu, 42 lha, 43 lhau, 44 sth, 45 sthu, 46 lmw, 47 stmw
  op 31 X  23 lwzx, 55 lwzux, 87 lbzx, 119 lbzux, 279 lhzx, 311 lhzux, 343 lhax, 375 lhaux,
           151 stwx, 183 stwux, 215 stbx, 247 stbux, 407 sthx, 439 sthux,
           597 lswi, 533 lswx, 725 stswi, 661 stswx, 20 lwarx, 150 stwcx., 1014 dcbz,
           (bonus, same family) 534 lwbrx, 790 lhbrx, 662 stwbrx, 918 sthbrx
  op 4  X  1014 dcbz_l
NOT covered: FP / paired-single loads and stores (other areas), dcbf/dcbi/dcbt/dcbst.

Model scope (NOT manual claims - boundaries of the neutral State):
  * gekko_state.Memory is addressed by effective address; no BAT/page-table translation,
    no DSI/protection model (harness precondition: the access translates without a DSI).
    RECONCILED (crosscheck_loadstore.py, disagreement class C2/C7/C9): the original
    "pages are taken as caching-allowed" assumption was WRONG - the W/I attributes decide
    dcbz/dcbz_l alignment (PAGE 142, PAGE 175, Table 5-4 PAGE 193) and RESERVE_ADDR is a
    physical address (PAGE 450), neither of which the neutral State carries.  Optional
    harness hooks (same names as loadstore_B):
        state.data_wimg(ea) -> WIMG nibble (W=8 I=4 M=2 G=1) of a translated data access
        state.data_phys(ea) -> physical address of a translated data access
        state.dcache_hit(ea) -> bool, 32-byte L1 block containing ea is in the d-cache
        state.wpar           -> WPAR value (write gather pipe, PAGE 314)
    With MSR[DR]=0 "the WIMG bits are automatically generated as 0b0011" (PAGE 134) and
    PA = EA, so no hook is needed.  With MSR[DR]=1 an outcome that depends on a missing
    hook raises Unmodelled (state untouched).
  * No cache contents are modelled unless state.dcache_hit is attached.  For dcbz/dcbz_l
    "all bytes are cleared" (PAGE 383/384) is the same whether the block hit or missed, so
    the zeroing is modelled; the hit-dependent effects (dcbz_l: HID2[DCHERR], machine check,
    PAGE 311) are reported as undefined when the hook is absent.
  * Memory is the CPU-visible view.  dcbz_l "is not forwarded to the L2 cache nor the 60x
    bus" (PAGE 384): the block is zeroed in the locked cache only.  This flat model shows
    the CPU-visible result (zeros at the block) as a write to Memory.
  * Memory-access granularity in mem.log follows the pseudocode MEM(x, y) operands: one
    access per MEM(EA, n) (lmw/stmw: one 4-byte access per register in ascending order;
    string instructions: one 1-byte access per byte, in EA order); dcbz/dcbz_l log one
    32-byte write of zero at the aligned block start (the manual defines the cleared set,
    not an access sequence).

Undefined behaviour is never invented: step() raises UndefinedBehaviour WITHOUT touching
the state (not even mem.log); the exception carries the parts the manual does define
(apply_defined()) and the list of locations it leaves undefined.  step_with_markers()
applies the defined parts and returns {location: UNDEFINED}.  Exceptions this area can
raise (alignment, program) are reported the same way, because the exception-specific SRR1
bits / DSISR are deferred to the Programming Environments manual, which is not in the source
set; SRR0, SRR1[5-9,16-31], MSR named bits, DAR, and the vector ARE defined and applied.
"""
from __future__ import annotations

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gekko_state import MASK32, State  # noqa: E402

AREA = "loadstore"
M = MASK32


# --------------------------------------------------------------------------------------
# Undefined-behaviour plumbing
# --------------------------------------------------------------------------------------
class _UndefinedMarker:
    __slots__ = ()

    def __repr__(self):
        return "UNDEFINED"


UNDEFINED = _UndefinedMarker()


class UndefinedBehaviour(Exception):
    """Raised when the manual leaves (part of) the result undefined.

    what      : text including the PAGE citation
    defined   : a plan dict of what the manual DOES define (see _commit for the keys)
    undefined : tuple of location names left undefined ('all' = boundedly undefined /
                invalid form: nothing at all is defined, not even whether an exception
                is taken)
    """

    def __init__(self, what, defined=None, undefined=("all",)):
        super().__init__(what)
        self.what = what
        self.defined = defined or {}
        self.undefined = tuple(undefined)

    def apply_defined(self, state):
        _commit(state, self.defined)


class Unmodelled(UndefinedBehaviour):
    """The manual defines the result, but it depends on information the neutral State does not
    carry (WIMG of a translated page, physical address, L1 cache occupancy, WPAR / gather FIFO).
    State untouched; undefined == ('all',)."""

    def __init__(self, what):
        super().__init__(what, None, ("all",))


def _all_undefined(what):
    raise UndefinedBehaviour(what, None, ("all",))


def _commit(state, plan):
    """Apply a plan.  Keys (all optional):
    mem          : list of ('r', addr, width) / ('w', addr, width, value), applied in order
    gpr          : {index: value}
    cr, cr_mask  : CR bits under the mask are written
    reserve, reserve_addr
    hid2, hid2_mask
    srr0 ; srr1, srr1_mask ; msr, msr_mask ; dar
    pc           : next PC
    exception    : name or None
    """
    for op in plan.get("mem", ()):
        if op[0] == "r":
            state.mem.read(op[1] & M, op[2])
        else:
            state.mem.write(op[1] & M, op[2], op[3])
    for i, v in plan.get("gpr", {}).items():
        state.gpr[i] = v & M
    if "cr" in plan:
        m = plan.get("cr_mask", M)
        state.cr = ((state.cr & ~m) | (plan["cr"] & m)) & M
    if "reserve" in plan:
        state.reserve = bool(plan["reserve"])
    if "reserve_addr" in plan:
        state.reserve_addr = plan["reserve_addr"] & M
    if "hid2" in plan:
        m = plan.get("hid2_mask", M)
        state.hid2 = ((state.hid2 & ~m) | (plan["hid2"] & m)) & M
    if "srr0" in plan:
        state.srr0 = plan["srr0"] & M
    if "srr1" in plan:
        m = plan.get("srr1_mask", M)
        state.srr1 = ((state.srr1 & ~m) | (plan["srr1"] & m)) & M
    if "msr" in plan:
        m = plan.get("msr_mask", M)
        state.msr = ((state.msr & ~m) | (plan["msr"] & m)) & M
    if "dar" in plan:
        state.dar = plan["dar"] & M
    if "pc" in plan:
        state.pc = plan["pc"] & M
    if "exception" in plan:
        state.exception = plan["exception"]


# --------------------------------------------------------------------------------------
# Register bit layouts (big-endian bit numbering: bit 0 = MSB, so bit n = 1 << (31 - n))
# --------------------------------------------------------------------------------------
def _bit(n):
    return 1 << (31 - n)


# MSR bits, Table 4-4 PAGE 164-165
MSR_POW = _bit(13)
MSR_ILE = _bit(15)   # "When an exception occurs, this bit is copied into MSR[LE]" PAGE 164
MSR_EE = _bit(16)
MSR_PR = _bit(17)
MSR_FP = _bit(18)
MSR_ME = _bit(19)
MSR_FE0 = _bit(20)
MSR_SE = _bit(21)
MSR_BE = _bit(22)
MSR_FE1 = _bit(23)
MSR_IP = _bit(25)    # "1 Exceptions are vectored to the physical address 0xFFFn_nnnn" PAGE 165
MSR_IR = _bit(26)
MSR_DR = _bit(27)
MSR_PM = _bit(29)
MSR_RI = _bit(30)
MSR_LE = _bit(31)    # "1 The processor runs in little-endian mode." PAGE 165
MSR_NAMED = (MSR_POW | MSR_ILE | MSR_EE | MSR_PR | MSR_FP | MSR_ME | MSR_FE0 | MSR_SE | MSR_BE
             | MSR_FE1 | MSR_IP | MSR_IR | MSR_DR | MSR_PM | MSR_RI | MSR_LE)

# HID0[DCE] = bit 17 "Data cache enable ... DCE is zero at power-up." PAGE 64
HID0_DCE = _bit(17)
# HID2: bit 3 LCE "0 ... dcbz_l instruction is illegal" PAGE 67; bit 8 DCHERR "dcbz_l cache hit
# error (sticky)", bit 12 DCHEE "dcbz_l cache hit error enable" PAGE 68
HID2_WPE = _bit(1)   # "WPE Write pipe enable" PAGE 67 / PAGE 314
HID2_LCE = _bit(3)
HID2_DCHERR = _bit(8)
HID2_DCHEE = _bit(12)

# WIMG nibble as written "WIMG = x1xx" (I is the second bit) PAGE 138; "W = 1 or I = 1" PAGE 193
WIMG_W = 0b1000
WIMG_I = 0b0100
# "For real addressing mode ... the WIMG bits are automatically generated as 0b0011" PAGE 134
DEFAULT_REAL_MODE_WIMG = 0b0011

# SRR1 on an alignment / program exception.  RECONCILED (class C1):
#   "SRR1[5-9, 16-31] are loaded with a copy of the corresponding MSR bits. Depending on the
#   implementation, reserved bits may not be copied." PAGE 166, resolved for Gekko by Table 4-4
#   "Note: Full function reserved bits are saved in SRR1 when an exception occurs; partial
#   function reserved bits are not saved." PAGE 165.
#   -> bit 28 ("Reserved. Full function" PAGE 165) and bit 29 (PM, a Gekko-defined bit, not
#      reserved on Gekko, PAGE 165) ARE copied (the original A was right, B wrong);
#   -> bit 24 ("Reserved. This bit corresponds to the AL bit of the POWER architecture."
#      PAGE 164, neither full nor partial function, and absent from the sc RTL copy list
#      "MSR[0,5-9, 16-23, 25-27, 30-31]" PAGE 537) is UNDEFINED (B was right, A wrong).
#   SRR1[0-4,10-15] stay undefined (exception-specific, PAGE 166, deferred to the PEM).
SRR1_COPY_MASK = 0x07C0FF7F
SRR1_UNDEFINED_MASK = 0xF83F0080

# CR0 = CR bits 0-3 in the order LT GT EQ SO: "CR0[LT GT EQ S0] = 0b00 || store_performed ||
# XER[SO]" PAGE 565; "CR bits 0-2 ... CR bit 3 receives a copy of ... XER[SO]" PAGE 339
CR0_MASK = 0xF0000000
CR0_EQ = _bit(2)
CR0_SO = _bit(3)
# XER[SO]: the manual names the bit but defers the XER layout to the Programming
# Environments manual (not in the source set).  INTERPRETATION: SO = XER[0].
XER_SO = _bit(0)


def _field(w, a, b):
    """w[a..b], big-endian bit numbers inclusive.  Field positions Table 12-2 PAGE 338-339:
    OPCD 0-5, rD/rS 6-10, rA 11-15, rB/NB 16-20, XO 21-30, Rc 31, d 16-31."""
    return (w >> (31 - b)) & ((1 << (b - a + 1)) - 1)


def _exts(v, bits):
    # EXTS(x): "Result of extending x on the left with sign bits" PAGE 341
    v &= (1 << bits) - 1
    return (v - (1 << bits) if v >> (bits - 1) else v) & M


# --------------------------------------------------------------------------------------
# Exceptions raised by this area
# --------------------------------------------------------------------------------------
def _exception(state, name, offset, why, dar=None, extra_undefined=()):
    """Take an instruction-caused exception (alignment 0x600 / program 0x700).

    Defined parts:
      * The exception is "taken without completing the exception-causing instruction"
        PAGE 161 -> no memory access, no GPR/CR/reservation change.
      * SRR0: "This may be the address of the instruction that caused the exception or the
        next one (as in the case of a system call, trace, or trap exception)" PAGE 163.
        INTERPRETATION: alignment/program(illegal) are not in the 'next one' list ->
        SRR0 = CIA.
      * "SRR1[5-9, 16-31] are loaded with a copy of the corresponding MSR bits" PAGE 166;
        bits 5-9 are "Reserved. Full function" and "Full function reserved bits are saved in
        SRR1" PAGE 165 -> defined mask SRR1_COPY_MASK = 0x07C0FF7F (bit 24 undefined, see
        the constant's comment; RECONCILED class C1).
      * New MSR from Table 4-6 PAGE 168 (Alignment and Program rows):
        POW EE PR FP FE0 SE BE FE1 IR DR PM RI = 0, ILE ME IP unaltered ("-- Bit is not
        altered"), LE = ILE ("Bit is copied from the MSR[ILE]").
        RECONCILED (class C1): the table legend "Reserved bits are read as if written as 0."
        PAGE 168 defines the reserved bits of the new MSR: whatever is stored, a read returns
        the value a write of 0 would give, so the architected value is 0.  The original A
        left them undefined ("msr[reserved]"); B (all reserved bits 0) was right.
      * PC = vector offset + base from MSR[IP]: 0x000n_nnnn or 0xFFFn_nnnn PAGE 166;
        offsets Table 4-2 PAGE 159 (Alignment 00600, Program 00700).
      * DAR (alignment): "After a DSI or an alignment exception, DAR is set to the effective
        address (EA) generated by the faulting instruction." PAGE 59
    Undefined in the source set (deferred to the Programming Environments manual):
      SRR1[0-4,10-15] ("loaded with information specific to the exception type" PAGE 166),
      SRR1[24] (see SRR1_COPY_MASK), DSISR for alignment.
    """
    msr = state.msr
    new_msr = (msr & (MSR_ILE | MSR_ME | MSR_IP)) | (MSR_LE if msr & MSR_ILE else 0)
    base = 0xFFF00000 if msr & MSR_IP else 0x00000000
    plan = {
        "srr0": state.pc,
        "srr1": msr & SRR1_COPY_MASK, "srr1_mask": SRR1_COPY_MASK,
        "msr": new_msr, "msr_mask": M,          # reserved bits -> 0, PAGE 168 legend
        "pc": base | offset,
        "exception": name,
    }
    undefined = ["srr1[0-4,10-15,24]"]
    if dar is not None:
        plan["dar"] = dar
        undefined.append("dsisr")
    undefined.extend(extra_undefined)
    raise UndefinedBehaviour(why, plan, tuple(undefined))


def _alignment(state, why, ea, extra_undefined=()):
    _exception(state, "alignment", 0x00600, why, dar=ea & M, extra_undefined=extra_undefined)


# --------------------------------------------------------------------------------------
# Common checks
# --------------------------------------------------------------------------------------
def _rc_reserved(name, word, page):
    # X-form load/store encodings show bit 31 as 0 (encoding diagram PAGE {page}); "If
    # instructions are encoded with incorrectly set bits in reserved fields, the results on
    # execution can be said to be boundedly undefined." PAGE 87.  For stores additionally
    # "it defines integer store instructions with the CR update option enabled (Rc ... = 1)
    # to be an invalid form" PAGE 103.
    if word & 1:
        _all_undefined(f"{name} with Rc=1: invalid form / reserved bit set, encoding PAGE {page}, "
                       f"PAGE 103, boundedly undefined PAGE 87")


def _little_endian_single(state, name, page):
    # "The default byte and bit ordering is big-endian. See 'Byte Ordering' in Chapter 3 ...
    # of the PowerPC Microprocessor Family: The Programming Environments manual" PAGE 89.
    # Little-endian (MSR[LE]=1) data-access semantics for single-register accesses are only
    # stated to be supported ("Gekko supports misaligned, single-register load and store
    # accesses in little-endian mode" PAGE 105); the byte/address mapping is deferred to a
    # manual not in the source set -> undefined here.
    if state.msr & MSR_LE:
        _all_undefined(f"{name} with MSR[LE]=1: little-endian byte ordering deferred to the "
                       f"Programming Environments manual PAGE 89 (instruction PAGE {page})")


def _ea_d(state, ra, word, update):
    d = _exts(_field(word, 16, 31), 16)
    if update:
        base = state.gpr[ra]              # "EA <- (rA) + EXTS(d)" e.g. PAGE 453
    else:
        base = 0 if ra == 0 else state.gpr[ra]   # "(rA|0)" PAGE 341
    # "Effective address computations ... use 32-bit unsigned binary arithmetic. A carry from
    # bit 0 is ignored." PAGE 89
    return (base + d) & M


def _ea_x(state, ra, rb, update):
    base = state.gpr[ra] if update else (0 if ra == 0 else state.gpr[ra])
    return (base + state.gpr[rb]) & M


def _next_pc(state):
    # "For instructions which do not branch, the next instruction address is CIA + 4." PAGE 342
    return (state.pc + 4) & M


def _getbe(state, ea, width):
    # MEM(x, y): "Contents of y bytes of memory starting at address x" PAGE 341; default
    # big-endian byte order PAGE 89; operands wrap "from the maximum effective address through
    # effective address 0" PAGE 89 (Memory masks each byte address).  Unlogged peek used only
    # while PLANNING; the logged read happens in _commit.
    return state.mem._get(ea & M, width)


# --------------------------------------------------------------------------------------
# Environment queries (RECONCILED, classes C2 / C7 / C9 - see module doc)
# --------------------------------------------------------------------------------------
def _wimg(state, ea, what):
    if not state.msr & MSR_DR:
        # "For real addressing mode (... MSR[DR] = 0 ... for data access), the WIMG bits are
        # automatically generated as 0b0011" PAGE 134; "when address translation is disabled
        # (real addressing mode), the default WIMG bits cause the I bit to be cleared" PAGE 138
        return DEFAULT_REAL_MODE_WIMG
    hook = getattr(state, "data_wimg", None)
    if hook is None:
        raise Unmodelled(f"{what} with MSR[DR]=1: the W/I bits of the translated page decide the "
                         f"outcome (PAGE 142, PAGE 175, Table 5-4 PAGE 193) but the neutral State "
                         f"carries no BAT/PTE (attach state.data_wimg)")
    return int(hook(ea & M)) & 0xF


def _phys(state, ea):
    if not state.msr & MSR_DR:
        return ea & M                      # real addressing mode: PA = EA, PAGE 134/138
    hook = getattr(state, "data_phys", None)
    return None if hook is None else int(hook(ea & M)) & M


def _pipe_check(state, ea, nbytes, what):
    """Write gather pipe PAGE 314: "For a non-cacheable store instruction to the address
    specified in WPAR, the operand will be stored sequentially in the buffer" (a 128-byte FIFO
    the neutral State does not carry).  Enabled only by HID2[WPE]=1 ("The write gather pipe is
    disabled at power-on as HID2[WPE] = 0" PAGE 314).  WPAR is not part of the neutral State:
    without a state.wpar attribute no store is taken to address the pipe (harness assumption,
    as in loadstore_B).  The address match is stated both as "upper 26 bits" (PAGE 314, 9.4.1)
    and "bits 0-26" (PAGE 314, 9.4.2); the wider 26-bit (64-byte) window is used."""
    if not (state.hid2 & HID2_WPE) or nbytes <= 0:
        return
    if not state.msr & MSR_DR:
        return                             # 0b0011 is cacheable (PAGE 134) -> never gathered
    wpar = getattr(state, "wpar", None)
    if wpar is None:
        return
    hook = getattr(state, "data_wimg", None)
    if hook is not None and not int(hook(ea & M)) & WIMG_I:
        return                             # cacheable store: not gathered
    gb = wpar & 0xFFFFFFC0
    for k in range(nbytes):
        pa = _phys(state, (ea + k) & M)
        if pa is None or (pa & 0xFFFFFFC0) == gb:
            raise Unmodelled(f"{what}: store may enter the write gather pipe FIFO (HID2[WPE]=1, "
                             f"WPAR set) PAGE 314")


def _reservation_w_check(state, ea, name):
    # Table 5-4 PAGE 193: "lwarx or stwcx. with W = 1 | Reservation instruction to write-through
    # segment or block | DSI exception DSISR[5] =1"; but PAGE 103: "Executing lwarx and stwcx. to
    # a page marked write-through does not cause a DSI exception if the W bit is set" and
    # PAGE 119: "If the W bit is set, executing lwarx and stwcx. to a page marked write-through
    # does not cause a DSI exception".  Contradictory -> undefined when W = 1.  Its priority
    # relative to the misalignment alignment exception is not listed in Table 4-3 PAGE 162, so
    # this check precedes the alignment check.  (COMMON-MODE gap: neither original model
    # checked W for lwarx/stwcx.; added to both.)
    if _wimg(state, ea, name) & WIMG_W:
        _all_undefined(f"{name} to a write-through (W=1) page: DSI per Table 5-4 PAGE 193 vs no DSI "
                       f"per PAGE 103 / PAGE 119")


# --------------------------------------------------------------------------------------
# Single-register loads
#   name: (width, 'z' zero-extend | 'a' algebraic | 'br' byte-reverse, update, page)
# --------------------------------------------------------------------------------------
def _load(name, width, ext, update, indexed, page):
    def run(state, word):
        rd, ra, rb = _field(word, 6, 10), _field(word, 11, 15), _field(word, 16, 20)
        if indexed:
            _rc_reserved(name, word, page)
        if update and (ra == 0 or ra == rd):
            # "If rA = 0, or rA = rD, the instruction form is invalid." (e.g. PAGE 453);
            # "load with update instructions with operand rA = 0 or rA = rD as invalid forms"
            # PAGE 102
            _all_undefined(f"{name} with rA=0 or rA=rD: invalid form PAGE {page}, PAGE 102")
        _little_endian_single(state, name, page)
        ea = _ea_x(state, ra, rb, update) if indexed else _ea_d(state, ra, word, update)
        raw = _getbe(state, ea, width)
        if ext == "z":
            # lbz: "rD <- (24)0 || MEM(EA, 1)"; lhz: "rD <- (16)0 || MEM(EA, 2)";
            # lwz: "rD <- MEM(EA, 4)"  PAGE 426/443/452
            val = raw
        elif ext == "a":
            # lha: "rD <- EXTS(MEM(EA, 2))" ... "filled with a copy of the most-significant bit
            # of the loaded half word" PAGE 438
            val = _exts(raw, 16)
        else:
            # lhbrx: "rD <- (16)0 || MEM(EA + 1, 1) || MEM(EA, 1)" PAGE 442
            # lwbrx: "rD <- MEM(EA + 3, 1) || MEM(EA + 2, 1) || MEM(EA + 1, 1) || MEM(EA, 1)"
            # PAGE 451.  Logged as one access to "the half word/word in memory addressed by
            # EA" (text of PAGE 442/451); latency equals other loads PAGE 102.
            val = int.from_bytes(raw.to_bytes(width, "big"), "little")
        gpr = {rd: val}
        if update:
            gpr[ra] = ea                  # "rA <- EA" / "EA is placed into rA." PAGE 427
        return {"mem": [("r", ea, width)], "gpr": gpr, "pc": _next_pc(state), "exception": None}
    return run


# --------------------------------------------------------------------------------------
# Single-register stores
# --------------------------------------------------------------------------------------
def _store(name, width, byterev, update, indexed, page):
    def run(state, word):
        rs, ra, rb = _field(word, 6, 10), _field(word, 11, 15), _field(word, 16, 20)
        if indexed:
            _rc_reserved(name, word, page)
        if update and ra == 0:
            # "If rA = 0, the instruction form is invalid." (e.g. PAGE 566); PAGE 103
            _all_undefined(f"{name} with rA=0: invalid form PAGE {page}, PAGE 103")
        _little_endian_single(state, name, page)
        ea = _ea_x(state, ra, rb, update) if indexed else _ea_d(state, ra, word, update)
        _pipe_check(state, ea, width, name)   # RECONCILED class C9, PAGE 314
        # Value is taken from rS BEFORE the update: "If rS = rA, the contents of register rS
        # are copied to the target memory element, then the generated EA is placed into rA
        # (rS)." PAGE 103; pseudocode order "MEM(EA, 4) <- rS ; rA <- EA" PAGE 566
        src = state.gpr[rs]
        if width == 1:
            val = src & 0xFF              # "MEM(EA, 1) <- rS[24-31]" PAGE 542
        elif width == 2:
            val = src & 0xFFFF            # "MEM(EA, 2) <- rS[16-31]" PAGE 555
        else:
            val = src                     # "MEM(EA, 4) <- rS" PAGE 563
        if byterev:
            # sthbrx "MEM(EA, 2) <- rS[24-31] || rS[16-23]" PAGE 556;
            # stwbrx "MEM(EA, 4) <- rS[24-31] || rS[16-23] || rS[8-15] || rS[0-7]" PAGE 564
            val = int.from_bytes(val.to_bytes(width, "big"), "little")
        plan = {"mem": [("w", ea, width, val)], "pc": _next_pc(state), "exception": None}
        if update:
            plan["gpr"] = {ra: ea}        # "EA is placed into rA." PAGE 543
        return plan
    return run


# --------------------------------------------------------------------------------------
# lmw / stmw
# --------------------------------------------------------------------------------------
def _lmw(state, word):
    rd, ra = _field(word, 6, 10), _field(word, 11, 15)
    # "If rA is in the range of registers specified to be loaded, including the case in which
    # rA = 0, the instruction form is invalid." PAGE 447 (also PAGE 105).
    # INTERPRETATION: rA = 0 is invalid for every rD (literal reading of "including the case").
    if ra == 0 or ra >= rd:
        _all_undefined("lmw with rA=0 or rA in rD..r31: invalid form PAGE 447")
    ea = _ea_d(state, ra, word, False)    # "EA <- b + EXTS(d)" PAGE 447
    if state.msr & MSR_LE:
        # "lmw, stmw, lswi, lswx, stswi, or stswx instruction attempted while MSR[LE] = 1 ->
        # Alignment exception" PAGE 194; "A multiple or string access is attempted with
        # MSR[LE] set." PAGE 175
        _alignment(state, "lmw with MSR[LE]=1 -> alignment PAGE 175/194", ea)
    if ea & 3:
        # "EA must be a multiple of four. If it is not, either the system alignment exception
        # handler is invoked or the results are boundedly undefined." PAGE 447; Gekko resolves
        # it: "The operand of lmw, stmw, lwarx, or stwcx. is not word-aligned." -> alignment
        # PAGE 175 (Table 4-2 PAGE 159, Table 6-1 "Poor means that an alignment exception
        # occurs" PAGE 232)
        _alignment(state, "lmw EA not a multiple of 4 -> alignment PAGE 175", ea)
    mem, gpr = [], {}
    r, a = rd, ea
    while r <= 31:                        # "do while r <= 31 / GPR(r) <- MEM(EA, 4)" PAGE 447
        mem.append(("r", a, 4))
        gpr[r] = _getbe(state, a, 4)
        r += 1
        a = (a + 4) & M                   # "EA <- EA + 4" (32-bit, PAGE 89)
    return {"mem": mem, "gpr": gpr, "pc": _next_pc(state), "exception": None}


def _stmw(state, word):
    rs, ra = _field(word, 6, 10), _field(word, 11, 15)
    # No invalid form is stated for stmw (PAGE 560): rA may be 0 or inside rS..r31.
    ea = _ea_d(state, ra, word, False)    # "EA <- b + EXTS(d)" PAGE 560
    if state.msr & MSR_LE:
        _alignment(state, "stmw with MSR[LE]=1 -> alignment PAGE 175/194", ea)
    if ea & 3:
        # "EA must be a multiple of four. If it is not, either ... alignment ... or boundedly
        # undefined" PAGE 560; Gekko: alignment PAGE 175
        _alignment(state, "stmw EA not a multiple of 4 -> alignment PAGE 175", ea)
    _pipe_check(state, ea, 4 * (32 - rs), "stmw")   # RECONCILED class C9, PAGE 314
    mem = []
    r, a = rs, ea
    while r <= 31:                        # "MEM(EA, 4) <- GPR(r)" ascending r PAGE 560
        mem.append(("w", a, 4, state.gpr[r]))
        r += 1
        a = (a + 4) & M
    return {"mem": mem, "pc": _next_pc(state), "exception": None}


# --------------------------------------------------------------------------------------
# String instructions
# --------------------------------------------------------------------------------------
def _string_regs(r0, n):
    nr = (n + 3) // 4                     # "nr = CEIL(n / 4)" PAGE 448
    return [(r0 + k) % 32 for k in range(nr)]   # "wraps around to r0" PAGE 448


def _string_boundary(name, ea, n, page):
    # "A non-word-aligned string operation that crosses a 4-Kbyte boundary, or a word-aligned
    # string operation that crosses a 256-Mbyte boundary always causes an alignment
    # exception." PAGE 105
    # CONFLICT: the Gekko alignment-cause lists (PAGE 159, PAGE 175, priority Table 4-3
    # PAGE 162) do not contain this cause, Table 6-1 rates string boundary crossings "Good"
    # (no exception) PAGE 232, and the instruction page says only "may be invoked" PAGE {page}.
    # -> whether the instruction executes or takes an alignment exception is undefined.
    if n == 0:
        return
    last = ea + n - 1                     # unwrapped: a wrap past 0xFFFFFFFF also crosses
    if (ea >> 28) != (last >> 28) or ((ea & 3) and (ea >> 12) != (last >> 12)):
        _all_undefined(f"{name}: string operand crosses a 4K (non-word-aligned) or 256M boundary: "
                       f"PAGE 105 says alignment, PAGE 175/232 do not, PAGE {page} 'may be invoked'")


def _load_string(name, state, rd, ea, n, page):
    mem, gpr = [], {}
    r = (rd - 1) % 32                     # "r <- rD - 1" PAGE 448
    i = 0
    a = ea
    cur = None
    while n > 0:                          # pseudocode PAGE 448 / PAGE 449
        if i == 0:
            if cur is not None:
                gpr[r] = cur
            r = (r + 1) % 32              # "r <- r + 1 (mod 32)"
            cur = 0                       # "GPR(r) <- 0" / "GPR(r) <- (32)0"
        mem.append(("r", a, 1))
        cur |= _getbe(state, a, 1) << (24 - i)   # "GPR(r)[i, i + 7] <- MEM(EA, 1)"
        i += 8
        if i == 32:
            i = 0
        a = (a + 1) & M
        n -= 1
    if cur is not None:
        gpr[r] = cur                      # unfilled low-order bytes stay cleared PAGE 448
    return {"mem": mem, "gpr": gpr, "pc": _next_pc(state), "exception": None}


def _store_string(state, rs, ea, n):
    mem = []
    r = (rs - 1) % 32                     # "r <- rS - 1" PAGE 561
    i = 0
    a = ea
    while n > 0:                          # pseudocode PAGE 561 / PAGE 562
        if i == 0:
            r = (r + 1) % 32
        mem.append(("w", a, 1, (state.gpr[r] >> (24 - i)) & 0xFF))  # "MEM(EA,1) <- GPR(r)[i,i+7]"
        i += 8
        if i == 32:
            i = 0
        a = (a + 1) & M
        n -= 1
    return {"mem": mem, "pc": _next_pc(state), "exception": None}


def _lswi(state, word):
    _rc_reserved("lswi", word, 448)
    rd, ra, nb = _field(word, 6, 10), _field(word, 11, 15), _field(word, 16, 20)
    n = 32 if nb == 0 else nb             # "if NB = 0 then n <- 32 else n <- NB" PAGE 448
    # "If rA is in the range of registers specified to be loaded, including the case in which
    # rA = 0, the instruction form is invalid." PAGE 448.  INTERPRETATION: rA = 0 is invalid
    # regardless of whether r0 is among the loaded registers.
    if ra == 0 or ra in _string_regs(rd, n):
        _all_undefined("lswi with rA=0 or rA in the loaded range: invalid form PAGE 448")
    ea = state.gpr[ra]                    # "EA is (rA|0)" PAGE 448 (rA != 0 here)
    if state.msr & MSR_LE:
        _alignment(state, "lswi with MSR[LE]=1 -> alignment PAGE 175/194", ea)
    _string_boundary("lswi", ea, n, 448)
    return _load_string("lswi", state, rd, ea, n, 448)


def _lswx(state, word):
    _rc_reserved("lswx", word, 449)
    rd, ra, rb = _field(word, 6, 10), _field(word, 11, 15), _field(word, 16, 20)
    n = state.xer & 0x7F                  # "n <- XER[25-31]" PAGE 449
    # "If rD = rA or rD = rB, the instruction form is invalid; If rD and rA both specify
    # GPR0, the form is invalid." PAGE 449
    if rd == ra or rd == rb:
        _all_undefined("lswx with rD=rA or rD=rB: invalid form PAGE 449")
    # "If rA or rB is in the range of registers specified to be loaded, including the case in
    # which rA = 0, either the system illegal instruction error handler is invoked or the
    # results are boundedly undefined." PAGE 449.  INTERPRETATION as for lswi: rA = 0 always.
    regs = _string_regs(rd, n)
    if ra == 0 or ra in regs or rb in regs:
        _all_undefined("lswx with rA=0 or rA/rB in the loaded range: illegal or boundedly "
                       "undefined PAGE 449")
    ea = _ea_x(state, ra, rb, False)      # "EA <- b + (rB)" PAGE 449
    if state.msr & MSR_LE:
        # "instruction attempted while MSR[LE] = 1" PAGE 194 - applies even when n = 0.
        # Confirmed (class C6, B was wrong): Table 5-4 PAGE 194 conditions on the instruction
        # being attempted, not on a byte being moved, and PAGE 198 itself calls the zero-length
        # case an access: "Accesses generated by an lswx or stswx instruction with a zero length".
        _alignment(state, "lswx with MSR[LE]=1 -> alignment PAGE 175/194", ea)
    if n == 0:
        # "rD <- undefined" / "If n = 0, the contents of rD are undefined." PAGE 449
        raise UndefinedBehaviour("lswx with XER[25-31]=0: rD undefined PAGE 449",
                                 {"pc": _next_pc(state), "exception": None}, (f"gpr[{rd}]",))
    _string_boundary("lswx", ea, n, 449)
    return _load_string("lswx", state, rd, ea, n, 449)


def _stswi(state, word):
    _rc_reserved("stswi", word, 561)
    rs, ra, nb = _field(word, 6, 10), _field(word, 11, 15), _field(word, 16, 20)
    n = 32 if nb == 0 else nb             # PAGE 561
    ea = 0 if ra == 0 else state.gpr[ra]  # "if rA = 0 then EA <- 0 else EA <- (rA)" PAGE 561
    # No invalid form is stated for stswi (PAGE 561).
    if state.msr & MSR_LE:
        _alignment(state, "stswi with MSR[LE]=1 -> alignment PAGE 175/194", ea)
    _string_boundary("stswi", ea, n, 561)
    _pipe_check(state, ea, n, "stswi")    # RECONCILED class C9, PAGE 314
    return _store_string(state, rs, ea, n)


def _stswx(state, word):
    _rc_reserved("stswx", word, 562)
    rs, ra, rb = _field(word, 6, 10), _field(word, 11, 15), _field(word, 16, 20)
    n = state.xer & 0x7F                  # "n <- XER[25-31]" PAGE 562
    ea = _ea_x(state, ra, rb, False)
    if state.msr & MSR_LE:
        _alignment(state, "stswx with MSR[LE]=1 -> alignment PAGE 175/194", ea)
    if n == 0:
        # "If n = 0, no bytes are stored." PAGE 562
        return {"pc": _next_pc(state), "exception": None}
    _string_boundary("stswx", ea, n, 562)
    _pipe_check(state, ea, n, "stswx")    # RECONCILED class C9, PAGE 314
    return _store_string(state, rs, ea, n)


# --------------------------------------------------------------------------------------
# lwarx / stwcx.
# --------------------------------------------------------------------------------------
def _lwarx(state, word):
    rd, ra, rb = _field(word, 6, 10), _field(word, 11, 15), _field(word, 16, 20)
    extra = ()
    if word & 1:
        # "If Rc is set, the instruction form is invalid for sync and lwarx instructions. If
        # Gekko encounters one of these invalid instruction forms, it sets CR0 to an undefined
        # value." PAGE 119 -> everything else as for Rc=0
        extra = ("cr0",)
    if state.msr & MSR_LE:
        _all_undefined("lwarx with MSR[LE]=1: byte ordering deferred PAGE 89")
    ea = _ea_x(state, ra, rb, False)      # "EA <- b + (rB)" PAGE 450
    _reservation_w_check(state, ea, "lwarx")
    if ea & 3:
        # "EA must be a multiple of four. If it is not, either ... alignment ... or boundedly
        # undefined" PAGE 450; Gekko: "If the location is not word-aligned, an alignment
        # exception occurs." PAGE 119, PAGE 175
        # Rc=1 on the exception path: CR0 stays undefined (class C8).  PAGE 161 says the
        # exception "is then taken without completing the exception-causing instruction", but
        # PAGE 119 says Gekko "sets CR0 to an undefined value" when it "encounters" the invalid
        # form, without excluding the exception case -> ambiguous -> undefined (B was wrong).
        _alignment(state, "lwarx EA not a multiple of 4 -> alignment PAGE 119/175", ea, extra)
    # "RESERVE <- 1 ; RESERVE_ADDR <- physical_addr(EA) ; rD <- MEM(EA,4)" PAGE 450; "The physical
    # address computed from EA is associated with the reservation" PAGE 450.
    # RECONCILED (class C7): the original A stored EA; with MSR[DR]=1 the physical address is not
    # derivable from the neutral State (B was right) -> state.data_phys hook or undefined.
    # The 32-byte granularity ("reservations are made on behalf of aligned 32-byte sections"
    # PAGE 103/119) governs snooping, not the stored address.
    plan = {"mem": [("r", ea, 4)], "gpr": {rd: _getbe(state, ea, 4)},
            "reserve": True, "pc": _next_pc(state), "exception": None}
    pa = _phys(state, ea)
    undefined = list(extra)
    if pa is None:
        undefined.append("reserve_addr")
    else:
        plan["reserve_addr"] = pa
    if undefined:
        raise UndefinedBehaviour("lwarx: " + ", ".join(undefined) + " undefined (CR0: Rc=1 PAGE 119; "
                                 "reserve_addr: physical_addr(EA) with MSR[DR]=1 PAGE 450)",
                                 plan, tuple(undefined))
    return plan


def _stwcx(state, word):
    rs, ra, rb = _field(word, 6, 10), _field(word, 11, 15), _field(word, 16, 20)
    extra = ()
    if not word & 1:
        # "The stwcx. instruction is the only load/store instruction with a valid form if Rc
        # is set. If Rc is zero, executing stwcx. sets CR0 to an undefined value." PAGE 119
        extra = ("cr0",)
    if state.msr & MSR_LE:
        _all_undefined("stwcx. with MSR[LE]=1: byte ordering deferred PAGE 89")
    ea = _ea_x(state, ra, rb, False)      # "EA <- b + (rB)" PAGE 565
    _reservation_w_check(state, ea, "stwcx.")
    if ea & 3:
        # PAGE 565 "either ... alignment ... or boundedly undefined"; Gekko: alignment
        # PAGE 119 ("If the location is not word-aligned, an alignment exception occurs."),
        # PAGE 175.  Taken without completing -> RESERVE and CR0 untouched PAGE 161.
        _alignment(state, "stwcx. EA not a multiple of 4 -> alignment PAGE 119/175", ea, extra)
    so = CR0_SO if state.xer & XER_SO else 0
    if state.reserve:
        # "if RESERVE then MEM(EA, 4) <- rS ; CR0 <- 0b00 || 0b1 || XER[SO] ; RESERVE <- 0"
        # PAGE 565.  No address comparison: "Because the hardware doesn't compare reservation
        # address when executing the stwcx. instruction" PAGE 565; "The stwcx. instruction does
        # not check the reservation for a matching address." PAGE 139
        # PAGE 314 "The eieio, stwcx, and sync instructions have no effect on the write gather
        # pipe" does not say whether the stwcx. store itself is gathered -> Unmodelled when it
        # could be (class C9, as loadstore_B).
        _pipe_check(state, ea, 4, "stwcx.")
        plan = {"mem": [("w", ea, 4, state.gpr[rs])], "cr": CR0_EQ | so, "cr_mask": CR0_MASK,
                "reserve": False}
    else:
        # "else CR0 <- 0b00 || 0b0 || XER[SO]" ; "it leaves the reserved bit cleared" PAGE 565
        plan = {"cr": so, "cr_mask": CR0_MASK, "reserve": False}
    plan["pc"] = _next_pc(state)
    plan["exception"] = None
    if extra:
        d = {k: v for k, v in plan.items() if k not in ("cr", "cr_mask")}
        raise UndefinedBehaviour("stwcx with Rc=0: CR0 undefined PAGE 119", d, extra)
    return plan


# --------------------------------------------------------------------------------------
# dcbz / dcbz_l
# --------------------------------------------------------------------------------------
def _dcbz_common(state, word, name, page):
    # Encoding: bits 6-10 are 0 and bit 31 is 0 ("31 0 0 0 0 0 A B 1014 0" PAGE 383,
    # "4 00000 A B 1014 0" PAGE 384); nonzero reserved bits -> boundedly undefined PAGE 87
    if _field(word, 6, 10) or (word & 1):
        _all_undefined(f"{name}: nonzero reserved field (encoding PAGE {page}) -> boundedly "
                       f"undefined PAGE 87")


def _dcbz_ea_block(state, word):
    ra, rb = _field(word, 11, 15), _field(word, 16, 20)
    ea = _ea_x(state, ra, rb, False)      # "EA is the sum (rA|0) + (rB)." PAGE 383/384
    # "the cache block containing the byte addressed by EA" PAGE 383; "32-byte (eight-word)
    # cache block" PAGE 25; "Both the instruction and data caches have 32-byte cache blocks"
    # PAGE 129 -> block = EA with the low 5 bits cleared.
    return ea, ea & ~31 & M


def _dcbz(state, word):
    _dcbz_common(state, word, "dcbz", 383)
    if state.msr & MSR_LE:
        _all_undefined("dcbz with MSR[LE]=1: little-endian address/byte mapping deferred PAGE 89")
    ea, block = _dcbz_ea_block(state, word)
    if not state.hid0 & HID0_DCE:
        # "An attempt is made to execute dcbz or dcbz_l when the data cache is disabled." ->
        # alignment PAGE 175; "if the cache is disabled, an alignment exception occurs",
        # priority 1 PAGE 122.  DAR = the EA generated by the instruction PAGE 59.
        _alignment(state, "dcbz with HID0[DCE]=0 -> alignment PAGE 175/122", ea)
    # RECONCILED (class C2): priority 2 "Page marked write-through or cache Inhibited-Alignment
    # exception" PAGE 122; "If the page containing the byte addressed by the EA is caching-
    # inhibited or write-through, then the system alignment exception handler is invoked."
    # PAGE 142; PAGE 175.  (PAGE 383 allows either clearing or the handler; Chapters 3/4
    # resolve it for Gekko.)  The original A assumed caching-allowed pages - wrong.
    if _wimg(state, ea, "dcbz") & (WIMG_W | WIMG_I):
        _alignment(state, "dcbz to a write-through or caching-inhibited page -> alignment "
                   "PAGE 142/175", ea)
    # "all bytes are cleared" PAGE 383 (HID2[LCE]=1 changes only which cache partition is used,
    # PAGE 311 section 9.2.2.1; the lock state does not matter: "The instruction executes
    # regardless of whether the cache is locked" PAGE 122)
    return {"mem": [("w", block, 32, 0)], "pc": _next_pc(state), "exception": None}


def _dcbz_l(state, word):
    _dcbz_common(state, word, "dcbz_l", 384)
    if not state.hid2 & HID2_LCE:
        # "If HID2[LCE] = 0 then the invalid instruction error handler is envolked." PAGE 384;
        # "dcbz_l instruction is illegal" PAGE 67; "execution of dcbz_l causes an illegal
        # instruction exception" PAGE 311; program exception vector 00700 PAGE 159/175.
        # Program (priority 1) precedes alignment (priority 6) Table 4-3 PAGE 162.
        _exception(state, "program", 0x00700, "dcbz_l with HID2[LCE]=0 -> illegal instruction "
                   "program exception PAGE 384/311")
    if state.msr & MSR_LE:
        _all_undefined("dcbz_l with MSR[LE]=1: little-endian address/byte mapping deferred PAGE 89")
    ea, block = _dcbz_ea_block(state, word)
    if not state.hid0 & HID0_DCE:
        _alignment(state, "dcbz_l with HID0[DCE]=0 -> alignment PAGE 175", ea)
    # RECONCILED (class C2): "The dcbz_l instruction causes an alignment exception if the page or
    # the block of the effective address is marked as write-thriugh or cache-inhibited." PAGE 311
    if _wimg(state, ea, "dcbz_l") & (WIMG_W | WIMG_I):
        _alignment(state, "dcbz_l to a write-through or caching-inhibited page -> alignment "
                   "PAGE 311/175", ea)
    # NOTE (class C4): dcbz_l has no supervisor restriction.  The text extraction of PAGE 384
    # reads "VEA Y es X", which cannot place the "Yes"; gekko_user_manual.pdf page 384 laid out
    # with pdftotext -layout puts "Yes" under "Gekko Specific" with "Supervisor Level" blank
    # (contrast dcbi PAGE 379, "Yes" under Supervisor Level, "This is a supervisor-level
    # instruction.").  loadstore_B's MSR[PR]=1 -> undefined was wrong.
    # Miss: "allocated in the 'locked' data cache ... All bytes are cleared"; hit: "all bytes
    # are cleared and the block is marked M" PAGE 384 -> the zeroing is defined either way.
    plan = {"mem": [("w", block, 32, 0)], "pc": _next_pc(state), "exception": None}
    # On a hit "Gekko sets HID2[DCHERR]=1. In addition, when the situation happens with
    # HID2[DCHEE] = MSR[EE] = MSR[ME] = 1, and HID2[DCHERR] = 0, Gekko also sets SRR1[10]=1 and
    # raises machine check." PAGE 311.  The machine check's SRR0 is set "On a best-effort basis"
    # (Table 4-10 PAGE 173) and when it is taken relative to this instruction is not stated, so
    # pc/srr0/srr1/msr/exception are undefined when it can fire.  RECONCILED (class C3): the
    # memory clear stays defined in that case too (PAGE 384 hit bullet), B's untouched
    # "Unmodelled" was wrong.
    armed = bool(state.hid2 & HID2_DCHEE) and bool(state.msr & MSR_EE) and bool(state.msr & MSR_ME) \
        and not state.hid2 & HID2_DCHERR
    mc = "machine_check(pc,srr0,srr1,msr,exception)"
    hook = getattr(state, "dcache_hit", None)
    if hook is None:
        if state.hid2 & HID2_DCHERR:
            # hit sets DCHERR=1, which is sticky and already 1; no machine check -> defined.
            return plan
        undefined = ["hid2[DCHERR]"]
        if armed:
            undefined.append(mc)
            plan = {"mem": plan["mem"]}
        raise UndefinedBehaviour("dcbz_l: cache hit/miss not modelled; hit sets HID2[DCHERR] "
                                 "(and may raise machine check) PAGE 311", plan, tuple(undefined))
    # RECONCILED (class C5): with state.dcache_hit attached the hit/miss is known (as loadstore_B).
    if not bool(hook(ea & M)):
        return plan
    plan["hid2"], plan["hid2_mask"] = HID2_DCHERR, HID2_DCHERR     # "sets HID2[DCHERR]=1" PAGE 311
    if armed:
        raise UndefinedBehaviour("dcbz_l cache hit with HID2[DCHEE]=MSR[EE]=MSR[ME]=1: machine check "
                                 "PAGE 311, SRR0 best-effort Table 4-10 PAGE 173",
                                 {"mem": plan["mem"], "hid2": HID2_DCHERR, "hid2_mask": HID2_DCHERR},
                                 (mc,))
    return plan


# --------------------------------------------------------------------------------------
# Decode
# --------------------------------------------------------------------------------------
_PRIMARY = {
    32: ("lwz", _load("lwz", 4, "z", False, False, 452)),
    33: ("lwzu", _load("lwzu", 4, "z", True, False, 453)),
    34: ("lbz", _load("lbz", 1, "z", False, False, 426)),
    35: ("lbzu", _load("lbzu", 1, "z", True, False, 427)),
    40: ("lhz", _load("lhz", 2, "z", False, False, 443)),
    41: ("lhzu", _load("lhzu", 2, "z", True, False, 444)),
    42: ("lha", _load("lha", 2, "a", False, False, 438)),
    43: ("lhau", _load("lhau", 2, "a", True, False, 439)),
    36: ("stw", _store("stw", 4, False, False, False, 563)),
    37: ("stwu", _store("stwu", 4, False, True, False, 566)),
    38: ("stb", _store("stb", 1, False, False, False, 542)),
    39: ("stbu", _store("stbu", 1, False, True, False, 543)),
    44: ("sth", _store("sth", 2, False, False, False, 555)),
    45: ("sthu", _store("sthu", 2, False, True, False, 557)),
    46: ("lmw", _lmw),
    47: ("stmw", _stmw),
}

# primary 31, XO = bits 21-30 (Table A / encodings PAGE 426-568)
_X31 = {
    23: ("lwzx", _load("lwzx", 4, "z", False, True, 455)),
    55: ("lwzux", _load("lwzux", 4, "z", True, True, 454)),
    87: ("lbzx", _load("lbzx", 1, "z", False, True, 429)),
    119: ("lbzux", _load("lbzux", 1, "z", True, True, 428)),
    279: ("lhzx", _load("lhzx", 2, "z", False, True, 446)),
    311: ("lhzux", _load("lhzux", 2, "z", True, True, 445)),
    343: ("lhax", _load("lhax", 2, "a", False, True, 441)),
    375: ("lhaux", _load("lhaux", 2, "a", True, True, 440)),
    534: ("lwbrx", _load("lwbrx", 4, "br", False, True, 451)),
    790: ("lhbrx", _load("lhbrx", 2, "br", False, True, 442)),
    151: ("stwx", _store("stwx", 4, False, False, True, 568)),
    183: ("stwux", _store("stwux", 4, False, True, True, 567)),
    215: ("stbx", _store("stbx", 1, False, False, True, 545)),
    247: ("stbux", _store("stbux", 1, False, True, True, 544)),
    407: ("sthx", _store("sthx", 2, False, False, True, 559)),
    439: ("sthux", _store("sthux", 2, False, True, True, 558)),
    662: ("stwbrx", _store("stwbrx", 4, True, False, True, 564)),
    918: ("sthbrx", _store("sthbrx", 2, True, False, True, 556)),
    597: ("lswi", _lswi),
    533: ("lswx", _lswx),
    725: ("stswi", _stswi),
    661: ("stswx", _stswx),
    20: ("lwarx", _lwarx),
    150: ("stwcx.", _stwcx),
    1014: ("dcbz", _dcbz),
}


def _decode(word):
    word &= M
    op = word >> 26
    if op in _PRIMARY:
        return _PRIMARY[op]
    xo = _field(word, 21, 30)
    if op == 31:
        return _X31.get(xo)
    if op == 4 and xo == 1014:
        # "dcbz_l 000100 00000 A B 1111110110 0" (Table A) / PAGE 384
        return ("dcbz_l", _dcbz_l)
    return None


def supports(word):
    return _decode(word) is not None


def mnemonic(word):
    d = _decode(word)
    return d[0] if d else None


def step(state, word):
    d = _decode(word)
    if d is None:
        raise ValueError(f"loadstore_A: word {word & M:08X} is not in area {AREA}")
    plan = d[1](state, word & M)          # may raise UndefinedBehaviour before any change
    _commit(state, plan)


def step_with_markers(state, word):
    """Like step(), but on UndefinedBehaviour applies the defined parts and returns
    {location: UNDEFINED}; returns {} when everything is defined."""
    try:
        step(state, word)
        return {}
    except UndefinedBehaviour as u:
        u.apply_defined(state)
        return {loc: UNDEFINED for loc in u.undefined}


# ======================================================================================
# Self-test: hand-derived vectors.  Expected values were worked out by hand from the manual
# text (literal numbers below), not computed with the helpers above.
# ======================================================================================
if __name__ == "__main__":
    PC0 = 0x80003100

    def D(op, s, a, d):
        return ((op << 26) | (s << 21) | (a << 16) | (d & 0xFFFF)) & M

    def X(op, s, a, b, xo, rc=0):
        return ((op << 26) | (s << 21) | (a << 16) | (b << 11) | (xo << 1) | rc) & M

    def X31(s, a, b, xo, rc=0):
        return X(31, s, a, b, xo, rc)

    MEMA = {0x80001000: bytes([0x11, 0x22, 0x33, 0x44, 0x80, 0x01, 0x7F, 0xFF,
                                0xFF, 0xFE, 0xAB, 0xCD, 0x01, 0x02, 0x03, 0x04])}
    DCE_ON = {"hid0": 0x00004000}

    # (name, setup, word, expect)
    # setup: gpr{}, mem{addr: bytes}, xer, cr, msr, hid0, hid2, reserve, reserve_addr
    # expect: gpr{} (all other GPRs must be unchanged), bytes{addr: bytes}, log[...], cr, pc,
    #         reserve, reserve_addr, exception, srr0, srr1 (low 0x07C0FFFF), msr (0x5FF77),
    #         dar, hid2, undef: set of undefined locations ({'all'} => state fully unchanged)
    V = [
        # ---------------- lwz family: EA, rA|0, sign of d, wrap  (PAGE 452-455, 89)
        ("lwz d(rA) BE", {"gpr": {4: 0x80001000}, "mem": MEMA}, D(32, 3, 4, 0),
         {"gpr": {3: 0x11223344}, "log": [("r", 0x80001000, 4, 0x11223344)], "pc": PC0 + 4}),
        ("lwz rA=0 is literal 0", {"gpr": {0: 0x80001000}, "mem": {0x00000100: b"\xDE\xAD\xBE\xEF"}},
         D(32, 3, 0, 0x100), {"gpr": {3: 0xDEADBEEF}, "log": [("r", 0x100, 4, 0xDEADBEEF)]}),
        ("lwz negative d", {"gpr": {4: 0x80001004}, "mem": MEMA}, D(32, 5, 4, 0xFFFC),
         {"gpr": {5: 0x11223344}, "log": [("r", 0x80001000, 4, 0x11223344)]}),
        ("lwz rA=rD non-update ok", {"gpr": {4: 0x80001008}, "mem": MEMA}, D(32, 4, 4, 0),
         {"gpr": {4: 0xFFFEABCD}}),
        ("lwz EA wraps below 0", {"gpr": {4: 0x00000004}, "mem": {0xFFFFFFFC: b"\x01\x02\x03\x04"}},
         D(32, 3, 4, 0xFFF8), {"gpr": {3: 0x01020304}, "log": [("r", 0xFFFFFFFC, 4, 0x01020304)]}),
        ("lwz operand wraps through 0", {"gpr": {4: 0xFFFFFFFE},
                                         "mem": {0xFFFFFFFE: b"\xAA\xBB", 0: b"\xCC\xDD"}},
         D(32, 3, 4, 0), {"gpr": {3: 0xAABBCCDD}, "log": [("r", 0xFFFFFFFE, 4, 0xAABBCCDD)]}),
        ("lwzu updates rA", {"gpr": {4: 0x80000FFC}, "mem": MEMA}, D(33, 3, 4, 8),
         {"gpr": {3: 0x80017FFF, 4: 0x80001004}, "log": [("r", 0x80001004, 4, 0x80017FFF)]}),
        ("lwzu rA=0 invalid", {"mem": MEMA}, D(33, 3, 0, 8), {"undef": {"all"}}),
        ("lwzu rA=rD invalid", {"gpr": {3: 0x80001000}, "mem": MEMA}, D(33, 3, 3, 0), {"undef": {"all"}}),
        ("lwzx rA=0", {"gpr": {0: 5, 5: 0x80001004}, "mem": MEMA}, X31(3, 0, 5, 23),
         {"gpr": {3: 0x80017FFF}}),
        ("lwzux", {"gpr": {4: 0x80000000, 5: 0x1008}, "mem": MEMA}, X31(3, 4, 5, 55),
         {"gpr": {3: 0xFFFEABCD, 4: 0x80001008}}),
        ("lwzux rB=rD ok, old rB used", {"gpr": {4: 0x80000000, 5: 0x100C}, "mem": MEMA}, X31(5, 4, 5, 55),
         {"gpr": {5: 0x01020304, 4: 0x8000100C}}),
        ("lwzux rA=rD invalid", {"gpr": {4: 0x80000000, 5: 0x1000}, "mem": MEMA}, X31(4, 4, 5, 55),
         {"undef": {"all"}}),
        ("lwzx Rc=1 reserved", {"gpr": {4: 0x80000000, 5: 0x1000}, "mem": MEMA}, X31(3, 4, 5, 23, 1),
         {"undef": {"all"}}),
        # ---------------- byte / half-word loads (PAGE 426-429, 438-446)
        ("lbz zero-extends 0x80", {"gpr": {4: 0x80001000}, "mem": MEMA}, D(34, 3, 4, 4),
         {"gpr": {3: 0x00000080}, "log": [("r", 0x80001004, 1, 0x80)]}),
        ("lbzu", {"gpr": {4: 0x80001000, 3: 0xFFFFFFFF}, "mem": MEMA}, D(35, 3, 4, 8),
         {"gpr": {3: 0x000000FF, 4: 0x80001008}}),
        ("lbzu rA=rD invalid", {"gpr": {4: 0x80001000}, "mem": MEMA}, D(35, 4, 4, 8), {"undef": {"all"}}),
        ("lbzx", {"gpr": {4: 0x80000000, 5: 0x100A}, "mem": MEMA}, X31(3, 4, 5, 87), {"gpr": {3: 0xAB}}),
        ("lbzux", {"gpr": {4: 0x80001010, 5: 0xFFFFFFF3}, "mem": MEMA}, X31(3, 4, 5, 119),
         {"gpr": {3: 0x44, 4: 0x80001003}}),
        ("lhz 0x8001 zero-extended", {"gpr": {4: 0x80001000}, "mem": MEMA}, D(40, 3, 4, 4),
         {"gpr": {3: 0x00008001}, "log": [("r", 0x80001004, 2, 0x8001)]}),
        ("lha 0x8001 sign-extended", {"gpr": {4: 0x80001000}, "mem": MEMA}, D(42, 3, 4, 4),
         {"gpr": {3: 0xFFFF8001}}),
        ("lha 0x7FFF positive", {"gpr": {4: 0x80001000}, "mem": MEMA}, D(42, 3, 4, 6),
         {"gpr": {3: 0x00007FFF}}),
        ("lhau d=-8", {"gpr": {4: 0x80001010}, "mem": MEMA}, D(43, 3, 4, 0xFFF8),
         {"gpr": {3: 0xFFFFFFFE, 4: 0x80001008}}),
        ("lhau rA=0 invalid", {"mem": MEMA}, D(43, 3, 0, 0), {"undef": {"all"}}),
        ("lhax rA=0", {"gpr": {6: 0x80001008}, "mem": MEMA}, X31(3, 0, 6, 343), {"gpr": {3: 0xFFFFFFFE}}),
        ("lhaux", {"gpr": {4: 0x80001000, 6: 10}, "mem": MEMA}, X31(3, 4, 6, 375),
         {"gpr": {3: 0xFFFFABCD, 4: 0x8000100A}}),
        ("lhzu", {"gpr": {4: 0x80001000}, "mem": MEMA}, D(41, 3, 4, 10), {"gpr": {3: 0xABCD, 4: 0x8000100A}}),
        ("lhzx", {"gpr": {4: 0x80001000, 5: 8}, "mem": MEMA}, X31(3, 4, 5, 279), {"gpr": {3: 0xFFFE}}),
        ("lhzux", {"gpr": {4: 0x80001000, 5: 2}, "mem": MEMA}, X31(3, 4, 5, 311), {"gpr": {3: 0x3344, 4: 0x80001002}}),
        ("lwbrx", {"gpr": {4: 0x80001000}, "mem": MEMA}, X31(3, 0, 4, 534),
         {"gpr": {3: 0x44332211}, "log": [("r", 0x80001000, 4, 0x11223344)]}),
        ("lhbrx", {"gpr": {4: 0x80001000}, "mem": MEMA}, X31(3, 0, 4, 790), {"gpr": {3: 0x00002211}}),
        ("lwz with MSR[LE]=1 undefined", {"gpr": {4: 0x80001000}, "mem": MEMA, "msr": 0x2033}, D(32, 3, 4, 0),
         {"undef": {"all"}}),
        # ---------------- single stores (PAGE 542-568, 103)
        ("stw BE", {"gpr": {3: 0xCAFEBABE, 4: 0x80002000}}, D(36, 3, 4, 4),
         {"bytes": {0x80002004: b"\xCA\xFE\xBA\xBE"}, "log": [("w", 0x80002004, 4, 0xCAFEBABE)]}),
        ("stw rA=0", {"gpr": {0: 0x80002000, 3: 0x01020304}}, D(36, 3, 0, 0x7FFC),
         {"bytes": {0x7FFC: b"\x01\x02\x03\x04"}, "log": [("w", 0x7FFC, 4, 0x01020304)]}),
        ("stwu rS=rA stores OLD value", {"gpr": {1: 0x80003000}}, D(37, 1, 1, 0xFFF8),
         {"gpr": {1: 0x80002FF8}, "bytes": {0x80002FF8: b"\x80\x00\x30\x00"},
          "log": [("w", 0x80002FF8, 4, 0x80003000)]}),
        ("stwu rA=0 invalid", {"gpr": {3: 1}}, D(37, 3, 0, 0x100), {"undef": {"all"}}),
        ("stwux rS=rA", {"gpr": {4: 0x80002000, 5: 0x10}}, X31(4, 4, 5, 183),
         {"gpr": {4: 0x80002010}, "bytes": {0x80002010: b"\x80\x00\x20\x00"}}),
        ("stwx rS=rB", {"gpr": {4: 0x80002000, 5: 0x20}}, X31(5, 4, 5, 151),
         {"bytes": {0x80002020: b"\x00\x00\x00\x20"}}),
        ("stwx Rc=1 invalid", {"gpr": {4: 0x80002000, 5: 0x20}}, X31(5, 4, 5, 151, 1), {"undef": {"all"}}),
        ("stb low byte only, neighbours kept", {"gpr": {3: 0x123456F0, 4: 0x80002000},
                                               "mem": {0x80001FFF: b"\x99\x99\x99"}}, D(38, 3, 4, 0),
         {"bytes": {0x80001FFF: b"\x99\xF0\x99"}, "log": [("w", 0x80002000, 1, 0xF0)]}),
        ("stbu", {"gpr": {3: 0xAB, 4: 0x80002000}}, D(39, 3, 4, 0xFFFF),
         {"gpr": {4: 0x80001FFF}, "bytes": {0x80001FFF: b"\xAB"}}),
        ("stbx rA=0", {"gpr": {3: 0x77, 5: 0x80002001}}, X31(3, 0, 5, 215), {"bytes": {0x80002001: b"\x77"}}),
        ("stbux", {"gpr": {3: 0x1FF, 4: 0x80002000, 5: 3}}, X31(3, 4, 5, 247),
         {"gpr": {4: 0x80002003}, "bytes": {0x80002003: b"\xFF"}}),
        ("sth low 16 bits", {"gpr": {3: 0xDEAD8001, 4: 0x80002000}}, D(44, 3, 4, 2),
         {"bytes": {0x80002002: b"\x80\x01"}, "log": [("w", 0x80002002, 2, 0x8001)]}),
        ("sthu", {"gpr": {3: 0x1234, 4: 0x80002000}}, D(45, 3, 4, 6),
         {"gpr": {4: 0x80002006}, "bytes": {0x80002006: b"\x12\x34"}}),
        ("sthx", {"gpr": {3: 0xFFFF5678, 4: 0x80002000, 5: 8}}, X31(3, 4, 5, 407), {"bytes": {0x80002008: b"\x56\x78"}}),
        ("sthux rA=0 invalid", {"gpr": {5: 8}}, X31(3, 0, 5, 439), {"undef": {"all"}}),
        ("stwbrx", {"gpr": {3: 0x11223344, 4: 0x80002000}}, X31(3, 0, 4, 662),
         {"bytes": {0x80002000: b"\x44\x33\x22\x11"}, "log": [("w", 0x80002000, 4, 0x44332211)]}),
        ("sthbrx", {"gpr": {3: 0x11223344, 4: 0x80002000}}, X31(3, 0, 4, 918),
         {"bytes": {0x80002000: b"\x44\x33"}}),
        # ---------------- lmw / stmw (PAGE 447, 560, 175)
        ("lmw r29 ascending", {"gpr": {1: 0x80001000}, "mem": MEMA}, D(46, 29, 1, 4),
         {"gpr": {29: 0x80017FFF, 30: 0xFFFEABCD, 31: 0x01020304},
          "log": [("r", 0x80001004, 4, 0x80017FFF), ("r", 0x80001008, 4, 0xFFFEABCD),
                  ("r", 0x8000100C, 4, 0x01020304)]}),
        ("lmw rA in range invalid", {"gpr": {30: 0x80001000}, "mem": MEMA}, D(46, 29, 30, 0), {"undef": {"all"}}),
        ("lmw rA=0 invalid (interp.)", {"mem": MEMA}, D(46, 30, 0, 0x1000), {"undef": {"all"}}),
        ("lmw misaligned -> alignment", {"gpr": {1: 0x80001001}, "msr": 0xB032}, D(46, 30, 1, 0),
         {"exception": "alignment", "pc": 0x00000600, "srr0": PC0, "srr1": 0x0000B032,
          "msr": 0x00001000, "dar": 0x80001001, "log": [],
          "undef": {"srr1[0-4,10-15,24]", "dsisr"}}),
        ("stmw r30", {"gpr": {1: 0x80003000, 30: 0xAAAAAAAA, 31: 0xBBBBBBBB}}, D(47, 30, 1, 0xFFF8),
         {"bytes": {0x80002FF8: b"\xAA\xAA\xAA\xAA\xBB\xBB\xBB\xBB"},
          "log": [("w", 0x80002FF8, 4, 0xAAAAAAAA), ("w", 0x80002FFC, 4, 0xBBBBBBBB)]}),
        ("stmw rA in range, rA=r31", {"gpr": {31: 0x80003000}}, D(47, 31, 31, 0),
         {"bytes": {0x80003000: b"\x80\x00\x30\x00"}}),
        ("stmw LE=1 -> alignment, IP=1 ILE=1", {"gpr": {1: 0x80003000}, "msr": 0x0001E051}, D(47, 31, 1, 0),
         {"exception": "alignment", "pc": 0xFFF00600, "srr0": PC0, "srr1": 0x0000E051,
          "msr": 0x00010041, "dar": 0x80003000, "log": [],
          "undef": {"srr1[0-4,10-15,24]", "dsisr"}}),
        # ---------------- strings (PAGE 448, 449, 561, 562, 105)
        ("lswi 7 bytes, partial clear", {"gpr": {4: 0x80001000, 5: 0xFFFFFFFF, 6: 0xFFFFFFFF}, "mem": MEMA},
         X31(5, 4, 7, 597),
         {"gpr": {5: 0x11223344, 6: 0x80017F00},
          "log": [("r", 0x80001000 + k, 1, v) for k, v in enumerate([0x11, 0x22, 0x33, 0x44, 0x80, 0x01, 0x7F])]}),
        ("lswi NB=0 -> 32 bytes, wraps r31->r0", {"gpr": {10: 0x80001000}, "mem": {0x80001000: bytes(range(1, 33))}},
         X31(30, 10, 0, 597),
         {"gpr": {30: 0x01020304, 31: 0x05060708, 0: 0x090A0B0C, 1: 0x0D0E0F10, 2: 0x11121314,
                  3: 0x15161718, 4: 0x191A1B1C, 5: 0x1D1E1F20}}),
        ("lswi rA in range invalid", {"gpr": {4: 0x80001000}, "mem": MEMA}, X31(3, 4, 5, 597), {"undef": {"all"}}),
        ("lswi rA=0 invalid (interp.)", {"mem": MEMA}, X31(3, 0, 4, 597), {"undef": {"all"}}),
        ("lswx n from XER[25-31]", {"gpr": {4: 0x80000000, 7: 0x100B}, "mem": MEMA, "xer": 0xE0000085},
         X31(8, 4, 7, 533), {"gpr": {8: 0xCD010203, 9: 0x04000000}}),
        ("lswx n=0 -> rD undefined", {"gpr": {4: 0x80001000, 7: 0, 8: 0x55}, "xer": 0x80000000},
         X31(8, 4, 7, 533), {"pc": PC0 + 4, "log": [], "undef": {"gpr[8]"}}),
        ("lswx rD=rB invalid", {"gpr": {4: 0x80001000}, "xer": 4}, X31(7, 4, 7, 533), {"undef": {"all"}}),
        ("lswx rB in range invalid", {"gpr": {4: 0x80001000, 9: 0}, "xer": 5}, X31(8, 4, 9, 533), {"undef": {"all"}}),
        ("stswi 6 bytes, rA inside source range ok", {"gpr": {3: 0x11223344, 4: 0x80002000}}, X31(3, 4, 6, 725),
         {"bytes": {0x80002000: b"\x11\x22\x33\x44\x80\x00"},
          "log": [("w", 0x80002000 + k, 1, v) for k, v in enumerate([0x11, 0x22, 0x33, 0x44, 0x80, 0x00])]}),
        ("stswi rA=0 -> EA 0", {"gpr": {31: 0xA1B2C3D4, 0: 0x5A5A5A5A}}, X31(31, 0, 5, 725),
         {"bytes": {0: b"\xA1\xB2\xC3\xD4\x5A"}}),
        ("stswx n=0 stores nothing", {"gpr": {4: 0x80002000, 5: 0}, "xer": 0x20000000}, X31(3, 4, 5, 661),
         {"log": [], "pc": PC0 + 4}),
        ("stswx 3 bytes", {"gpr": {3: 0xFEDCBA98, 4: 0x80002000, 5: 1}, "xer": 3}, X31(3, 4, 5, 661),
         {"bytes": {0x80002001: b"\xFE\xDC\xBA"}}),
        ("lswi LE=1 -> alignment", {"gpr": {4: 0x80001000}, "msr": 0x00003033}, X31(5, 4, 4, 597),
         {"exception": "alignment", "pc": 0x600, "dar": 0x80001000, "srr0": PC0, "srr1": 0x3033,
          "msr": 0x00001000, "undef": {"srr1[0-4,10-15,24]", "dsisr"}}),
        ("stswi misaligned crossing 4K undefined", {"gpr": {4: 0x80001FFE}}, X31(3, 4, 4, 725), {"undef": {"all"}}),
        ("stswi word-aligned crossing 4K defined", {"gpr": {3: 0x01020304, 4: 0x80001FFC, 5: 0x05060708}},
         X31(3, 4, 8, 725), {"bytes": {0x80001FFC: b"\x01\x02\x03\x04\x80\x00\x1F\xFC"}}),
        # ---------------- lwarx / stwcx. (PAGE 450, 565, 119, 139)
        ("lwarx sets reservation", {"gpr": {4: 0x80001000, 5: 4}, "mem": MEMA, "msr": 0x2022}, X31(3, 4, 5, 20),
         {"gpr": {3: 0x80017FFF}, "reserve": True, "reserve_addr": 0x80001004,
          "log": [("r", 0x80001004, 4, 0x80017FFF)]}),
        ("lwarx misaligned -> alignment, no reservation", {"gpr": {4: 0x80001000, 5: 2}, "mem": MEMA, "msr": 0x2022},
         X31(3, 4, 5, 20),
         {"exception": "alignment", "reserve": False, "dar": 0x80001002, "log": [], "pc": 0x600,
          "undef": {"srr1[0-4,10-15,24]", "dsisr"}}),
        ("lwarx Rc=1 -> CR0 undefined only", {"gpr": {4: 0x80001000, 5: 0}, "mem": MEMA, "msr": 0x2022}, X31(3, 4, 5, 20, 1),
         {"gpr": {3: 0x11223344}, "reserve": True, "reserve_addr": 0x80001000, "undef": {"cr0"}}),
        ("stwcx. with reservation stores, EQ set, SO copied",
         {"gpr": {3: 0x0BADF00D, 4: 0x80002000, 5: 8}, "reserve": True, "reserve_addr": 0x80002008,
          "xer": 0x80000000, "cr": 0x0FFFFFFF, "msr": 0x2022}, X31(3, 4, 5, 150, 1),
         {"bytes": {0x80002008: b"\x0B\xAD\xF0\x0D"}, "cr": 0x3FFFFFFF, "reserve": False,
          "reserve_addr": 0x80002008, "log": [("w", 0x80002008, 4, 0x0BADF00D)]}),
        ("stwcx. without reservation: no store, CR0=0",
         {"gpr": {3: 1, 4: 0x80002000, 5: 8}, "reserve": False, "cr": 0xF2345678, "msr": 0x2022}, X31(3, 4, 5, 150, 1),
         {"cr": 0x02345678, "log": [], "reserve": False}),
        ("stwcx. ignores reservation address", {"gpr": {3: 0x12345678, 0: 0x80004000}, "reserve": True,
                                               "reserve_addr": 0x80001000, "msr": 0x2022}, X31(3, 0, 0, 150, 1),
         {"bytes": {0x80004000: b"\x12\x34\x56\x78"}, "cr": 0x20000000, "reserve": False,
          "reserve_addr": 0x80001000}),
        ("stwcx Rc=0 -> CR0 undefined", {"gpr": {3: 7, 4: 0x80002000}, "reserve": True, "cr": 0x11111111, "msr": 0x2022},
         X31(3, 0, 4, 150, 0),
         {"bytes": {0x80002000: b"\x00\x00\x00\x07"}, "reserve": False, "undef": {"cr0"}, "cr": 0x11111111}),
        ("stwcx. misaligned -> alignment, reservation kept", {"gpr": {3: 7, 4: 0x80002001}, "reserve": True,
                                                              "cr": 0x11111111, "msr": 0x2022}, X31(3, 0, 4, 150, 1),
         {"exception": "alignment", "reserve": True, "cr": 0x11111111, "log": [], "dar": 0x80002001,
          "undef": {"srr1[0-4,10-15,24]", "dsisr"}}),
        # ---------------- dcbz / dcbz_l (PAGE 383, 384, 122, 175, 311)
        ("dcbz clears the 32-byte block containing EA", dict(DCE_ON, msr=0x2022, gpr={4: 0x80001000, 5: 0x13},
                                                             mem={0x80000FFF: b"\xEE" * 34}),
         X31(0, 4, 5, 1014),
         {"bytes": {0x80000FFF: b"\xEE" + bytes(32) + b"\xEE"}, "log": [("w", 0x80001000, 32, 0)],
          "pc": PC0 + 4}),
        ("dcbz rA=0", dict(DCE_ON, msr=0x2022, gpr={0: 0x1234, 5: 0x8000105F}, mem={0x80001040: b"\x01" * 33}),
         X31(0, 0, 5, 1014), {"bytes": {0x80001040: bytes(32) + b"\x01"}}),
        ("dcbz HID0[DCE]=0 -> alignment, DAR=EA (unaligned)", {"gpr": {4: 0x80001000, 5: 0x13},
                                                               "mem": {0x80001000: b"\xEE"}},
         X31(0, 4, 5, 1014),
         {"exception": "alignment", "pc": 0x600, "dar": 0x80001013, "bytes": {0x80001000: b"\xEE"},
          "log": [], "undef": {"srr1[0-4,10-15,24]", "dsisr"}}),
        ("dcbz reserved bits 6-10 set", dict(DCE_ON, gpr={4: 0x80001000}), X31(1, 0, 4, 1014), {"undef": {"all"}}),
        ("dcbz_l HID2[LCE]=0 -> program 0x700", {"gpr": {4: 0x80001000}, "hid2": 0xE0000000, "msr": 0x00009032},
         X(4, 0, 0, 4, 1014),
         {"exception": "program", "pc": 0x700, "srr0": PC0, "srr1": 0x9032, "msr": 0x00001000, "log": [],
          "undef": {"srr1[0-4,10-15,24]"}}),
        ("dcbz_l LCE=1, DCHERR already 1: defined", dict(DCE_ON, msr=0x2022, gpr={4: 0xE0000000, 5: 0x21},
                                                         hid2=0xF0800000),
         X(4, 0, 4, 5, 1014), {"log": [("w", 0xE0000020, 32, 0)], "hid2": 0xF0800000, "pc": PC0 + 4}),
        ("dcbz_l LCE=1, DCHERR=0: DCHERR undefined", dict(DCE_ON, msr=0x2022, gpr={4: 0xE0000000, 5: 0}, hid2=0xF0000000,
                                                          mem={0xE0000000: b"\x42"}),
         X(4, 0, 4, 5, 1014),
         {"bytes": {0xE0000000: b"\x00"}, "pc": PC0 + 4, "undef": {"hid2[DCHERR]"}}),
        ("dcbz_l LCE=1 DCE=0 -> alignment", {"gpr": {4: 0xE0000004}, "hid2": 0xF0000000},
         X(4, 0, 0, 4, 1014),
         {"exception": "alignment", "pc": 0x600, "dar": 0xE0000004, "log": [],
          "undef": {"srr1[0-4,10-15,24]", "dsisr"}}),
        # ---------------- reconciliation vectors (crosscheck_loadstore.py classes C1-C9)
        ("C1 SRR1 copies bits 28/29, bit 24 undefined; new MSR reserved bits read 0",
         {"gpr": {1: 0x80001001}, "msr": 0x8002B0BE}, D(46, 30, 1, 0),
         {"exception": "alignment", "pc": 0x600, "srr0": PC0, "srr1": 0x0000B03E, "msr_full": 0x00001000,
          "dar": 0x80001001, "log": [], "undef": {"srr1[0-4,10-15,24]", "dsisr"}}),
        ("C2 dcbz DR=1 without data_wimg -> unmodelled", dict(DCE_ON, gpr={4: 0x80001000}),
         X31(0, 0, 4, 1014), {"undef": {"all"}, "unmodelled": True}),
        ("C2 dcbz DR=1 cache-inhibited page -> alignment",
         dict(DCE_ON, gpr={4: 0xCC003000, 5: 0x13}, hooks={"data_wimg": lambda ea: 0b0110}),
         X31(0, 4, 5, 1014), {"exception": "alignment", "pc": 0x600, "dar": 0xCC003013, "log": [],
                              "undef": {"srr1[0-4,10-15,24]", "dsisr"}}),
        ("C2 dcbz DR=1 write-through page -> alignment",
         dict(DCE_ON, gpr={4: 0xCC003000, 5: 0x13}, hooks={"data_wimg": lambda ea: 0b1010}),
         X31(0, 4, 5, 1014), {"exception": "alignment", "pc": 0x600, "dar": 0xCC003013, "log": [],
                              "undef": {"srr1[0-4,10-15,24]", "dsisr"}}),
        ("C2 dcbz DR=1 caching-allowed page clears",
         dict(DCE_ON, gpr={4: 0x80001000, 5: 0x13}, hooks={"data_wimg": lambda ea: 0b0010}),
         X31(0, 4, 5, 1014), {"log": [("w", 0x80001000, 32, 0)]}),
        ("C2 dcbz_l DR=1 cache-inhibited -> alignment",
         dict(DCE_ON, gpr={4: 0xCC000000}, hid2=0xF0800000, hooks={"data_wimg": lambda ea: 0b0100}),
         X(4, 0, 0, 4, 1014), {"exception": "alignment", "pc": 0x600, "dar": 0xCC000000, "log": [],
                               "undef": {"srr1[0-4,10-15,24]", "dsisr"}}),
        ("C7 lwarx DR=1 no data_phys -> reserve_addr undefined",
         {"gpr": {4: 0x80001000, 5: 4}, "mem": MEMA, "reserve_addr": 0x55, "hooks": {"data_wimg": lambda ea: 0b0010}},
         X31(3, 4, 5, 20), {"gpr": {3: 0x80017FFF}, "reserve": True, "reserve_addr": 0x55, "undef": {"reserve_addr"}}),
        ("C7 lwarx DR=1 data_phys -> reserve_addr = PA",
         {"gpr": {4: 0x80001000, 5: 4}, "mem": MEMA,
          "hooks": {"data_wimg": lambda ea: 0b0010, "data_phys": lambda ea: ea & 0x3FFFFFFF}},
         X31(3, 4, 5, 20), {"gpr": {3: 0x80017FFF}, "reserve": True, "reserve_addr": 0x00001004}),
        ("W=1 lwarx contradictory -> undefined",
         {"gpr": {4: 0x80001000, 5: 4}, "mem": MEMA, "hooks": {"data_wimg": lambda ea: 0b1000}},
         X31(3, 4, 5, 20), {"undef": {"all"}}),
        ("W=1 stwcx. DR=1 without data_wimg -> unmodelled",
         {"gpr": {3: 1, 4: 0x80002000}, "reserve": True}, X31(3, 0, 4, 150, 1), {"undef": {"all"}, "unmodelled": True}),
        ("C6 lswx n=0 with LE=1 -> alignment",
         {"gpr": {4: 0x80001000, 7: 0, 8: 0x55}, "xer": 0, "msr": 0x2033}, X31(8, 4, 7, 533),
         {"exception": "alignment", "pc": 0x600, "dar": 0x80001000, "srr1": 0x2033, "log": [],
          "undef": {"srr1[0-4,10-15,24]", "dsisr"}}),
        ("C8 stwcx Rc=0 misaligned -> alignment, CR0 undefined",
         {"gpr": {3: 7, 4: 0x80002002}, "reserve": True, "cr": 0x11111111, "msr": 0x2022}, X31(3, 0, 4, 150, 0),
         {"exception": "alignment", "reserve": True, "cr": 0x11111111, "log": [], "dar": 0x80002002,
          "undef": {"srr1[0-4,10-15,24]", "dsisr", "cr0"}}),
        ("C5 dcbz_l known miss: DCHERR unchanged",
         dict(DCE_ON, msr=0x2022, gpr={5: 0xE0000021}, hid2=0xF0000000, hooks={"dcache_hit": lambda ea: False}),
         X(4, 0, 0, 5, 1014), {"log": [("w", 0xE0000020, 32, 0)], "hid2": 0xF0000000}),
        ("C5 dcbz_l known hit, not armed: DCHERR set",
         dict(DCE_ON, msr=0x2022, gpr={5: 0xE0000040}, hid2=0xF0000000, hooks={"dcache_hit": lambda ea: True}),
         X(4, 0, 0, 5, 1014), {"log": [("w", 0xE0000040, 32, 0)], "hid2": 0xF0800000}),
        ("C3/C5 dcbz_l known hit, armed: cleared, DCHERR set, machine check undefined",
         dict(DCE_ON, msr=0xB022, gpr={5: 0xE0000040}, hid2=0xF0080000, hooks={"dcache_hit": lambda ea: True}),
         X(4, 0, 0, 5, 1014), {"log": [("w", 0xE0000040, 32, 0)], "hid2": 0xF0880000, "nopc": True,
                               "undef": {"machine_check(pc,srr0,srr1,msr,exception)"}}),
        ("C3 dcbz_l no hook, armed: cleared, DCHERR + machine check undefined",
         dict(DCE_ON, msr=0xB022, gpr={5: 0xE0000040}, hid2=0xF0080000),
         X(4, 0, 0, 5, 1014), {"log": [("w", 0xE0000040, 32, 0)], "hid2": 0xF0080000, "nopc": True,
                               "undef": {"hid2[DCHERR]", "machine_check(pc,srr0,srr1,msr,exception)"}}),
        ("C4 dcbz_l with MSR[PR]=1 is legal",
         dict(DCE_ON, msr=0x6022, gpr={5: 0xE0000000}, hid2=0xF0800000), X(4, 0, 0, 5, 1014),
         {"log": [("w", 0xE0000000, 32, 0)], "hid2": 0xF0800000}),
        ("C9 stw possibly into the write gather pipe -> unmodelled",
         {"gpr": {3: 0x61000000, 4: 0xCC008000}, "hid2": 0xE0000000, "hooks": {"wpar": 0xCC008000}},
         D(36, 3, 4, 0), {"undef": {"all"}, "unmodelled": True}),
        ("C9 stw real mode is cacheable, never gathered",
         {"gpr": {3: 0x61000000, 4: 0xCC008000}, "hid2": 0xE0000000, "msr": 0x2022, "hooks": {"wpar": 0xCC008000}},
         D(36, 3, 4, 0), {"log": [("w", 0xCC008000, 4, 0x61000000)]}),
    ]

    fails = 0
    for name, setup, word, exp in V:
        s = State()
        s.pc = PC0
        for i, v in setup.get("gpr", {}).items():
            s.gpr[i] = v
        for a, b in setup.get("mem", {}).items():
            s.mem.load(a, b)
        for k in ("xer", "cr", "msr", "hid0", "hid2", "reserve", "reserve_addr"):
            if k in setup:
                setattr(s, k, setup[k])
        for k, v in setup.get("hooks", {}).items():
            setattr(s, k, v)
        before = s.clone()
        assert supports(word), name
        errs = []
        if exp.get("unmodelled"):
            try:
                step(s.clone(), word)
                errs.append("expected Unmodelled, completed")
            except Unmodelled:
                pass
            except UndefinedBehaviour:
                errs.append("expected Unmodelled, got plain UndefinedBehaviour")
        markers = step_with_markers(s, word)
        undef = exp.get("undef", set())
        if set(markers) != set(undef):
            errs.append(f"undefined {sorted(markers)} != {sorted(undef)}")
        if undef == {"all"}:
            if s.arch_dict() != before.arch_dict() or s.mem.bytes != before.mem.bytes or s.mem.log:
                errs.append("state changed on fully-undefined form")
        else:
            want_gpr = list(before.gpr)
            for i, v in exp.get("gpr", {}).items():
                want_gpr[i] = v
            if s.gpr != want_gpr:
                errs.append(f"gpr {[hex(x) for x in s.gpr]} != {[hex(x) for x in want_gpr]}")
            for a, b in exp.get("bytes", {}).items():
                got = bytes(s.mem.bytes.get((a + k) & M, 0) for k in range(len(b)))
                if got != b:
                    errs.append(f"mem@{a:08X} {got.hex()} != {b.hex()}")
            if "log" in exp and s.mem.log != exp["log"]:
                errs.append(f"log {s.mem.log} != {exp['log']}")
            if "exception" not in exp and "pc" not in exp and not exp.get("nopc") and s.pc != PC0 + 4:
                errs.append(f"pc {s.pc:08X} != CIA+4")
            if "exception" not in exp and s.exception is not None:
                errs.append(f"unexpected exception {s.exception}")
            for k in ("cr", "pc", "reserve", "reserve_addr", "exception", "srr0", "dar", "hid2"):
                if k in exp and getattr(s, k) != exp[k]:
                    errs.append(f"{k} {getattr(s, k)!r} != {exp[k]!r}")
            if "msr_full" in exp and s.msr != exp["msr_full"]:
                errs.append(f"msr {s.msr:08X} != {exp['msr_full']:08X} (all 32 bits)")
            if "srr1" in exp and (s.srr1 & 0x07C0FF7F) != exp["srr1"]:
                errs.append(f"srr1 {s.srr1:08X} != {exp['srr1']:08X}")
            if "msr" in exp and (s.msr & 0x0005FF77) != exp["msr"]:
                errs.append(f"msr {s.msr:08X} != {exp['msr']:08X}")
            if "exception" in exp and "log" not in exp and s.mem.log:
                errs.append("exception path touched memory")
        if errs:
            fails += 1
            print(f"FAIL {name} [{word:08X} {mnemonic(word)}]: " + "; ".join(errs))
    # decode sanity: unrelated words are not claimed
    for w, want in ((0x7C0802A6, False), (0x38600000, False), (0x7C0007EC, True), (0x100007EC, True),
                    (0x1000002A, False), (0x7C00012D, True), (0x7C00042A, True)):
        if supports(w) != want:
            fails += 1
            print(f"FAIL supports({w:08X}) != {want}")
    print(f"{len(V)} vectors, {fails} failures")
    sys.exit(1 if fails else 0)
