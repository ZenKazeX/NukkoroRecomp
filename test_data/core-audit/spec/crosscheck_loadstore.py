"""Differential cross-check of the two independent "loadstore" specification models.

    python crosscheck_loadstore.py [--n N] [--seed S] [--jobs J] [--models loadstore_A,loadstore_B]
                                   [--path DIR] [--show K] [--no-edge] [--mutants]

For every mnemonic both models support it draws random + deterministic edge execution vectors
(boundary EAs around 4K / 256M / 2^32 wrap, misaligned operands, aliasing rA/rB/rD/rS, rA=0,
lmw/string register ranges, every XER[25-31] count class, MSR LE/ILE/IP/DR/EE/ME/PR patterns,
HID0[DCE], HID2[LCE/DCHERR/DCHEE/WPE], RESERVE, random FPSCR (every RN/NI/enable/sticky
pattern - loadstore must not touch it), and the optional environment hooks
data_wimg / data_phys / dcache_hit / wpar), runs both models from identical cloned states and
compares
  - the outcome class: ok / partial (defined parts applied, rest named undefined) /
    invalid (UB, state untouched) / unmodelled (depends on state the neutral State lacks,
    untouched) / unsupported
  - the undefined-location set, normalised to {field: bitmask}
  - the full State.arch_dict(), mem.log, mem.bytes and pending, with the union of both
    models' undefined bits masked out
  - untouched-ness of invalid / unmodelled outcomes, and a PAGE citation in every UB message.
It also sweeps the opcode space (all primary opcodes; every 10-bit XO of groups 4 and 31 with
field noise and Rc) comparing supports()/mnemonic().

Contracts normalised:
  A-style (loadstore_A, loadstore): exc.undefined == ('all',) -> invalid (or unmodelled if
      the exception is an instance of mod.Unmodelled); otherwise exc.apply_defined(state) and
      exc.undefined names locations ('gpr[N]', 'cr0', 'dsisr', 'srr1[...]', 'hid2[DCHERR]',
      'reserve_addr', 'machine_check(pc,srr0,srr1,msr,exception)').
  B-style (loadstore_B): PartiallyUndefined (state updated, exc.masks {field: mask});
      Unmodelled / plain UndefinedBehaviour -> untouched.
"""
from __future__ import annotations

import argparse
import collections
import importlib
import multiprocessing as mp
import os
import random
import re
import sys
import types

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from gekko_state import MASK32, State  # noqa: E402

M = MASK32

# ------------------------------------------------------------------------------------------
# Catalogue - written from the manual's encoding images (x'....' headers of Chapter 12),
# checked against the manual text, NOT copied from either model.  name -> (primary, xo|None)
# ------------------------------------------------------------------------------------------
CAT = {
    "lwz": (32, None), "lwzu": (33, None), "lbz": (34, None), "lbzu": (35, None),
    "stw": (36, None), "stwu": (37, None), "stb": (38, None), "stbu": (39, None),
    "lhz": (40, None), "lhzu": (41, None), "lha": (42, None), "lhau": (43, None),
    "sth": (44, None), "sthu": (45, None), "lmw": (46, None), "stmw": (47, None),
    "lwarx": (31, 20), "lwzx": (31, 23), "lwzux": (31, 55), "lbzx": (31, 87), "lbzux": (31, 119),
    "stwcx.": (31, 150), "stwx": (31, 151), "stwux": (31, 183), "stbx": (31, 215),
    "stbux": (31, 247), "lhzx": (31, 279), "lhzux": (31, 311), "lhax": (31, 343),
    "lhaux": (31, 375), "sthx": (31, 407), "sthux": (31, 439), "lswx": (31, 533),
    "lwbrx": (31, 534), "stswx": (31, 661), "stwbrx": (31, 662), "lswi": (31, 597),
    "stswi": (31, 725), "lhbrx": (31, 790), "sthbrx": (31, 918), "dcbz": (31, 1014),
    "dcbz_l": (4, 1014),
}
UPDATE = {"lwzu", "lbzu", "lhzu", "lhau", "stwu", "stbu", "sthu", "lwzux", "lbzux", "lhzux",
          "lhaux", "stwux", "stbux", "sthux"}
DFORM = {n for n, (op, xo) in CAT.items() if xo is None}

# bit masks used by the generators (big-endian bit n = 1 << (31 - n))
MSR_LE, MSR_ILE, MSR_IP, MSR_DR, MSR_EE, MSR_ME, MSR_PR = 0x1, 0x10000, 0x40, 0x10, 0x8000, 0x1000, 0x4000
HID0_DCE = 0x00004000
HID2_WPE, HID2_LCE, HID2_DCHERR, HID2_DCHEE = 0x40000000, 0x10000000, 0x00800000, 0x00080000

ADDR_BASES = [0x80001000, 0x80001001, 0x80001002, 0x80001003, 0x80000FFC, 0x80000FFD, 0x80000FFE,
              0x80000FF0, 0x8FFFFFFC, 0x8FFFFFFE, 0x8FFFFFE0, 0xFFFFFFFC, 0xFFFFFFFD, 0xFFFFFFFF,
              0x00000000, 0x00000004, 0x7FFFFFFC, 0xCC003000, 0xE0000000, 0xE0000FFF, 0x80400000]
SMALL = [0, 1, 2, 3, 4, 8, 0x1F, 0x20, 0x21, 0xFFFFFFFF, 0xFFFFFFFC, 0xFFFFFFFE, 0x7FFF, 0x8000,
         0xFFFF8000, 0x1000, 0x10000000, 0x80000000]
BOUND16 = [0, 1, 2, 3, 4, 8, 0x1F, 0x20, 0x7FFF, 0x7FFC, 0x8000, 0x8001, 0xFFFF, 0xFFFC, 0xFFF8,
           0xFFFE, 0x0FFF, 0x1000, 0xF000]


# ------------------------------------------------------------------------------------------
# Environment hooks (deterministic, deep-copyable)
# ------------------------------------------------------------------------------------------
class WimgHook:
    def __init__(self, table):
        self.table = list(table)

    def __call__(self, ea):
        return self.table[(ea >> 28) & 15]

    def __repr__(self):
        return "Wimg%s" % ["%X" % v for v in self.table]


class PhysHook:
    def __init__(self, mode, k):
        self.mode, self.k = mode, k

    def __call__(self, ea):
        if self.mode == "mask":
            return ea & 0x3FFFFFFF
        if self.mode == "xor":
            return (ea ^ self.k) & M
        return ea

    def __repr__(self):
        return "Phys(%s,%08X)" % (self.mode, self.k)


class HitHook:
    def __init__(self, mode, seed):
        self.mode, self.seed = mode, seed

    def __call__(self, ea):
        if self.mode in (True, False):
            return self.mode
        return bool((((ea >> 5) * 2654435761) ^ self.seed) >> 11 & 1)

    def __repr__(self):
        return "Hit(%s)" % self.mode


WIMG_CHOICES = [0b0010, 0b0011, 0b0000, 0b0001, 0b0100, 0b0110, 0b1000, 0b1010, 0b1100]


# ------------------------------------------------------------------------------------------
# Generators
# ------------------------------------------------------------------------------------------
def rand_addr(r):
    k = r.random()
    if k < 0.45:
        return (r.choice(ADDR_BASES) + r.choice((0, 0, 0, 1, 2, 3, 4, -4, 8, 0x20, -1))) & M
    if k < 0.60:
        return r.choice(SMALL)
    if k < 0.75:
        return (0x80000000 | r.getrandbits(20)) & M
    return r.getrandbits(32)


def rand_reg(r):
    return r.choice((0, 1, 2, 3, 4, 5, 29, 30, 31)) if r.random() < 0.5 else r.randrange(32)


def gen_word(r, name):
    op, xo = CAT[name]
    rd, ra, rb = rand_reg(r), rand_reg(r), rand_reg(r)
    k = r.random()
    if k < 0.15:
        ra = rd
    elif k < 0.25:
        rb = rd
    elif k < 0.35:
        rb = ra
    elif k < 0.40:
        ra = 0
    if name in ("lmw", "stmw") and r.random() < 0.6:
        rd = r.choice((25, 27, 28, 29, 30, 31))
        if r.random() < 0.3:
            ra = r.randrange(rd, 32)
    if name in ("lswi", "stswi") and r.random() < 0.5:
        rb = r.choice((0, 1, 3, 4, 5, 7, 8, 31))
    if xo is None:
        low = r.choice(BOUND16) if r.random() < 0.6 else r.getrandbits(16)
        w = (op << 26) | (rd << 21) | (ra << 16) | low
    else:
        rc = 1 if r.random() < (0.5 if name in ("stwcx.", "lwarx") else 0.10) else 0
        if name in ("dcbz", "dcbz_l") and r.random() < 0.85:
            rd = 0
        w = (op << 26) | (rd << 21) | (ra << 16) | (rb << 11) | (xo << 1) | rc
    return w & M


def fields(w):
    return (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31


def exts16(v):
    v &= 0xFFFF
    return (v - 0x10000) & M if v & 0x8000 else v


def candidate_eas(st, name, w):
    rd, ra, rb = fields(w)
    base0 = st.gpr[ra] if ra else 0
    out = set()
    if name in DFORM:
        out.add((base0 + exts16(w)) & M)
        out.add((st.gpr[ra] + exts16(w)) & M)
    elif name in ("lswi", "stswi"):
        out.add(base0)
    else:
        out.add((base0 + st.gpr[rb]) & M)
        out.add((st.gpr[ra] + st.gpr[rb]) & M)
    return out


def prefill(r, st, name, w):
    for ea in candidate_eas(st, name, w):
        if r.random() < 0.9:
            lo = (ea - 40) & M
            st.mem.load(lo, bytes(r.getrandbits(8) for _ in range(40 + 136)))
    st.mem.log.clear()


def gen_state(r, name, w):
    st = State()
    for i in range(32):
        st.gpr[i] = rand_addr(r) if r.random() < 0.7 else r.choice(SMALL + [r.getrandbits(32)])
        st.fpr[i] = [r.getrandbits(64), r.getrandbits(64)]
    st.cr = r.getrandbits(32) if r.random() < 0.7 else r.choice((0, M, 0x0FFFFFFF, 0xF0000000))
    cnt = r.choice((0, 0, 1, 2, 3, 4, 5, 7, 8, 9, 31, 32, 33, 64, 0x7F, r.getrandbits(7)))
    st.xer = ((r.randrange(8) << 29) | (r.getrandbits(22) << 7 if r.random() < 0.5 else 0) | cnt) & M
    st.fpscr = r.getrandbits(32) if r.random() < 0.7 else r.choice((0, 1, 2, 3, 4, 0xF8, M))
    if r.random() < 0.3:
        msr = r.getrandbits(32)
    else:
        msr = r.choice((0x2032, 0x2022, 0x2002, 0x0032, 0xB032, 0x9022, 0x4032))
        for bit, p in ((MSR_LE, 0.15), (MSR_ILE, 0.3), (MSR_IP, 0.3), (MSR_EE, 0.3), (MSR_ME, 0.4),
                       (MSR_PR, 0.2)):
            if r.random() < p:
                msr ^= bit
    st.msr = msr & M
    st.lr, st.ctr = r.getrandbits(32), r.getrandbits(32)
    st.srr0, st.srr1, st.dar, st.dsisr = (r.getrandbits(32) for _ in range(4))
    st.dec, st.tb = r.getrandbits(32), r.getrandbits(64)
    st.hid0 = (r.getrandbits(32) & ~HID0_DCE) | (HID0_DCE if r.random() < 0.75 else 0)
    h2 = r.getrandbits(32) & ~(HID2_LCE | HID2_DCHERR | HID2_DCHEE | HID2_WPE)
    for bit, p in ((HID2_LCE, 0.7), (HID2_DCHERR, 0.4), (HID2_DCHEE, 0.5), (HID2_WPE, 0.4)):
        if r.random() < p:
            h2 |= bit
    st.hid2 = h2 & M
    st.gqr = [r.getrandbits(32) for _ in range(8)]
    st.sprg = [r.getrandbits(32) for _ in range(4)]
    st.pc = r.choice((0x80003100, 0xFFFFFFFC, 0x7FFFFFFC, 0)) if r.random() < 0.2 else (r.getrandbits(30) << 2)
    st.reserve = r.random() < 0.5
    st.reserve_addr = r.getrandbits(32)
    st.pending = {"external": r.random() < 0.5, "decrementer": r.random() < 0.5}
    st.exception = r.choice((None, None, "program", "dsi", "alignment"))
    prefill(r, st, name, w)
    # hooks
    if r.random() < 0.55:
        if r.random() < 0.4:
            st.data_wimg = WimgHook([r.choice((0b0010, 0b0011))] * 16)
        else:
            st.data_wimg = WimgHook([r.choice(WIMG_CHOICES) for _ in range(16)])
    if r.random() < 0.5:
        st.data_phys = PhysHook(r.choice(("id", "mask", "xor")), r.getrandbits(32) & ~0xFFF)
    if r.random() < 0.5:
        st.dcache_hit = HitHook(r.choice((True, False, "hash")), r.getrandbits(32))
    if r.random() < 0.35:
        eas = sorted(candidate_eas(st, name, w))
        pa = eas[0]
        ph = getattr(st, "data_phys", None)
        if ph is not None and r.random() < 0.5:
            pa = ph(pa)
        st.wpar = ((pa & 0xFFFFFFC0) | r.getrandbits(6)) if r.random() < 0.7 else r.getrandbits(32)
    return st


def hooks_repr(st):
    return {k: repr(getattr(st, k)) for k in ("data_wimg", "data_phys", "dcache_hit", "wpar") if hasattr(st, k)}


# ------------------------------------------------------------------------------------------
# Contract normalisation
# ------------------------------------------------------------------------------------------
MSR_NAMED = 0x0005FF77
A_LOCATIONS = {
    "srr1[0-4,10-15]": {"srr1": 0xF83F0000},          # original model A
    "srr1[0-4,10-15,24]": {"srr1": 0xF83F0080},       # reconciled
    "msr[reserved]": {"msr": ~MSR_NAMED & M},          # original model A
    "dsisr": {"dsisr": M},
    "cr0": {"cr": 0xF0000000},
    "hid2[DCHERR]": {"hid2": HID2_DCHERR},
    "reserve_addr": {"reserve_addr": M},
    "machine_check(pc,srr0,srr1,msr,exception)": {"pc": M, "srr0": M, "srr1": M, "msr": M, "exception": 1},
}
B_FIELDS = {"srr1", "dsisr", "cr", "hid2", "reserve_addr", "pc", "srr0", "msr", "exception"}


def _merge(dst, src):
    for k, v in src.items():
        dst[k] = dst.get(k, 0) | v


def norm_a(undefined):
    out = {}
    for loc in undefined:
        m = re.fullmatch(r"gpr\[(\d+)\]", loc)
        if m:
            _merge(out, {"gpr[%d]" % int(m.group(1)): M})
        elif loc in A_LOCATIONS:
            _merge(out, A_LOCATIONS[loc])
        else:
            raise RuntimeError("unknown A-style undefined location %r" % loc)
    return out


def norm_b(masks):
    out = {}
    for k, v in masks.items():
        if not (k in B_FIELDS or re.fullmatch(r"gpr\[\d+\]", k)):
            raise RuntimeError("unknown B-style mask field %r" % k)
        _merge(out, {k: (1 if k == "exception" else v & M)})
    return out


def run_model(mod, st, w):
    """-> (outcome, {field: mask}, message)"""
    try:
        mod.step(st, w)
        return ("ok", {}, "")
    except mod.UndefinedBehaviour as u:
        what = getattr(u, "what", str(u))
        unm = getattr(mod, "Unmodelled", None)
        part = getattr(mod, "PartiallyUndefined", None)
        if hasattr(u, "masks"):                              # B-style
            if part is not None and isinstance(u, part):
                return ("partial", norm_b(u.masks), what)
            if unm is not None and isinstance(u, unm):
                return ("unmodelled", {}, what)
            return ("invalid", {}, what)
        if "all" in u.undefined:                             # A-style
            if unm is not None and isinstance(u, unm):
                return ("unmodelled", {}, what)
            return ("invalid", {}, what)
        u.apply_defined(st)
        return ("partial", norm_a(u.undefined), what)
    except ValueError as e:
        return ("unsupported", {}, str(e))


def snapshot(st, und=None):
    d = st.arch_dict()
    d = {k: ([list(x) for x in v] if k == "fpr" else (list(v) if isinstance(v, list) else v)) for k, v in d.items()}
    for f, m in (und or {}).items():
        g = re.fullmatch(r"gpr\[(\d+)\]", f)
        if g:
            d["gpr"][int(g.group(1))] &= ~m & M
        elif f == "exception":
            d["exception"] = "<undefined>"
        else:
            d[f] = d[f] & ~m & M
    d["mem.log"] = list(st.mem.log)
    d["mem.bytes"] = dict(st.mem.bytes)
    d["pending"] = dict(st.pending)
    return d


def diff_fields(a, b):
    out = []
    for k in a:
        if a[k] != b[k]:
            if k == "gpr":
                out.extend("gpr[%d]" % i for i in range(32) if a[k][i] != b[k][i])
            else:
                out.append(k)
    return out


def classify(fs):
    return tuple(sorted(set("gpr" if f.startswith("gpr[") else f for f in fs)))


def compare(name, w, base, mA, mB):
    pre = snapshot(base)
    sA, sB = base.clone(), base.clone()
    oA = run_model(mA, sA, w)
    oB = run_model(mB, sB, w)
    probs = []
    if oA[0] != oB[0]:
        probs.append(("outcome", oA[0], oB[0]))
    else:
        if oA[2] and "PAGE" not in oA[2]:
            probs.append(("A-no-page",))
        if oB[2] and "PAGE" not in oB[2]:
            probs.append(("B-no-page",))
        if oA[0] in ("invalid", "unmodelled", "unsupported"):
            if snapshot(sA) != pre:
                probs.append(("A-modified-on-" + oA[0], classify(diff_fields(pre, snapshot(sA)))))
            if snapshot(sB) != pre:
                probs.append(("B-modified-on-" + oB[0], classify(diff_fields(pre, snapshot(sB)))))
        else:
            if oA[1] != oB[1]:
                probs.append(("undefined-set", tuple(sorted((k, "%X" % v) for k, v in oA[1].items())),
                              tuple(sorted((k, "%X" % v) for k, v in oB[1].items()))))
            und = dict(oA[1])
            _merge(und, oB[1])
            dA, dB = snapshot(sA, und), snapshot(sB, und)
            if dA != dB:
                probs.append(("state", classify(diff_fields(dA, dB))))
    if not probs:
        return None
    rd, ra, rb = fields(w)
    detail = {
        "word": "%08X" % w, "rd/ra/rb": (rd, ra, rb),
        "pre": {"msr": "%08X" % base.msr, "hid0": "%08X" % base.hid0, "hid2": "%08X" % base.hid2,
                "xer": "%08X" % base.xer, "reserve": base.reserve, "gpr[ra]": "%08X" % base.gpr[ra],
                "gpr[rb]": "%08X" % base.gpr[rb], "hooks": hooks_repr(base)},
        "A": (oA[0], oA[2][:160]), "B": (oB[0], oB[2][:160]),
    }
    if oA[0] == oB[0] and oA[0] in ("ok", "partial"):
        und = dict(oA[1]); _merge(und, oB[1])
        dA, dB = snapshot(sA, und), snapshot(sB, und)
        detail["diff"] = {f: (str(dA.get(f, dA["gpr"]))[:120], str(dB.get(f, dB["gpr"]))[:120])
                          for f in classify(diff_fields(dA, dB)) if f != "gpr"}
        detail["gpr_diff"] = {i: ("%08X" % sA.gpr[i], "%08X" % sB.gpr[i]) for i in range(32) if dA["gpr"][i] != dB["gpr"][i]}
        detail["undef"] = ({k: "%X" % v for k, v in oA[1].items()}, {k: "%X" % v for k, v in oB[1].items()})
    key = (name, tuple(probs))
    return key, detail


# ------------------------------------------------------------------------------------------
# Mutation self-validation against loadstore_B.py (single-point plausible defects)
# ------------------------------------------------------------------------------------------
MUTANTS = [
    ("stwu stores the updated EA", "        state.mem.write(ea, width, val)\n        if update:",
     "        if update:\n            val = ea & ((1 << (8 * width)) - 1)\n        state.mem.write(ea, width, val)\n        if update:"),
    ("lha zero-extends", "            val = _exts16(val)", "            pass"),
    ("update load does not write rA",
     "            state.gpr[ra] = ea                             # rA <- EA after the load", "            pass  #"),
    ("non-update (rA|0) ignored", "base = state.gpr[ra] if (update or ra != 0) else 0", "base = state.gpr[ra]"),
    ("lmw descending", "for k in range(n):\n        a = (ea + 4 * k) & MASK32",
     "for k in reversed(range(n)):\n        a = (ea + 4 * k) & MASK32"),
    ("dcbz block mask 8 bytes", "BLOCK_MASK = ~31 & MASK32", "BLOCK_MASK = ~7 & MASK32"),
    ("string partial register not cleared", "                state.gpr[r] = 0", "                pass"),
    ("lswi NB=0 means 0", "n = rb if rb != 0 else 32", "n = rb"),
    ("stwcx. compares reservation address", "    if state.reserve:\n        _pipe_check",
     "    if state.reserve and state.reserve_addr == ea:\n        _pipe_check"),
    ("stwcx. SO not copied", "    so = 1 if state.xer & XER_SO else 0", "    so = 0"),
    ("stwcx. success keeps the reservation", "        cr0 = 0b0010 | so\n        state.reserve = False", "        cr0 = 0b0010 | so"),
    ("exception vector ignores MSR[IP]", "state.pc = ((0xFFF00000 if old & MSR_IP else 0x00000000) | offset) & MASK32",
     "state.pc = offset & MASK32"),
    ("exception SRR0 = CIA+4", "    state.srr0 = state.pc & MASK32", "    state.srr0 = (state.pc + 4) & MASK32"),
    ("new MSR keeps EE", "    new = old & (MSR_ILE | MSR_ME | MSR_IP)", "    new = old & (MSR_ILE | MSR_ME | MSR_IP | MSR_EE)"),
    ("DAR not set on alignment", "        state.dar = dar & MASK32", "        pass"),
    ("dcbz DCE check skipped", "    if not (state.hid0 & HID0_DCE):\n        # PAGE 140", "    if False:\n        # PAGE 140"),
    ("lswx count from 8 bits", "n = state.xer & XER_COUNT", "n = state.xer & 0xFF"),
    ("lbz sign-extends the byte", "        if kind == \"loada\":",
     "        if width == 1 and val & 0x80:\n            val = (val - 0x100) & MASK32\n        if kind == \"loada\":"),
    ("SRR1 bit 28 not copied (class C1 regression)", "SRR1_COPY_MASK = 0x07C0FF7F", "SRR1_COPY_MASK = 0x07C0FF77"),
    ("zero-length string with LE=1 completes (class C6 regression)",
     "        _alignment(state, ea, f\"{name} with MSR[LE]=1 PAGE 175\")",
     "        if n:\n            _alignment(state, ea, f\"{name} with MSR[LE]=1 PAGE 175\")"),
    ("lwarx ignores W bit (common-mode gap regression)", "    if _wimg(state, ea, name) & WIMG_W:", "    if False:"),
    ("sthbrx not reversed", "        if byterev:\n            # sthbrx", "        if False:\n            # sthbrx"),
    ("pc not wrapped", "    state.pc = (state.pc + 4) & MASK32", "    state.pc = (state.pc + 4)"),
    ("dcbz_l LCE check inverted", "    if not (state.hid2 & HID2_LCE):", "    if state.hid2 & HID2_LCE:"),
    ("string boundary rule dropped", "    if n > 0 and _crosses(ea, n):", "    if False:"),
]


def load_model(spec):
    if "@" not in spec:
        return importlib.import_module(spec)
    base, idx = spec.split("@")
    key = "%s_mut%s" % (base, idx)
    if key in sys.modules:
        return sys.modules[key]
    desc, old, new = MUTANTS[int(idx)]
    path = None
    for d in sys.path:
        p = os.path.join(d, base + ".py")
        if os.path.exists(p):
            path = p
            break
    with open(path, encoding="utf-8") as fh:
        src = fh.read()
    if old not in src:
        raise RuntimeError("mutant %s (%s): anchor text not found" % (idx, desc))
    src = src.replace(old, new, 1).replace('if __name__ == "__main__":', "if False:")
    mod = types.ModuleType(key)
    mod.__file__ = path
    sys.modules[key] = mod
    exec(compile(src, key, "exec"), mod.__dict__)
    return mod


# ------------------------------------------------------------------------------------------
# Sweeps
# ------------------------------------------------------------------------------------------
def worker(args):
    seed, count, modA, modB, extra = args
    for p in extra:
        if p not in sys.path:
            sys.path.insert(0, p)
    mA, mB = load_model(modA), load_model(modB)
    names = sorted(CAT)
    r = random.Random(seed)
    classes, counts, outcomes = collections.OrderedDict(), collections.Counter(), collections.Counter()
    for _ in range(count):
        name = r.choice(names)
        w = gen_word(r, name)
        base = gen_state(r, name, w)
        res = compare(name, w, base, mA, mB)
        if res is None:
            # outcome coverage bookkeeping (re-run cheaply only on A)
            outcomes[(name, run_model(mA, base.clone(), w)[0])] += 1
            continue
        key, detail = res
        counts[key] += 1
        classes.setdefault(key, detail)
    return classes, counts, outcomes


EDGE_REGS = [(3, 4, 5), (3, 3, 5), (3, 4, 3), (3, 4, 4), (3, 0, 5), (0, 0, 0), (31, 30, 31),
             (29, 30, 5), (0, 31, 1), (30, 0, 30), (5, 4, 0), (28, 31, 29)]
EDGE_OFF = [0, 4, 0xFFFC, 0x7FFF, 0x8000, 1, 3, 0x20]
EDGE_MSR = [0x2022, 0x2032, 0x2023, 0x12033, 0x1B032, 0x9022, 0x2062, 0x6022]
EDGE_XER = [0, 1, 3, 4, 5, 8, 32, 0x7F, 0x80000081, 0xE0000000]
EDGE_HID = [(HID0_DCE, 0xF0000000), (0, 0xF0000000), (HID0_DCE, 0xE0000000), (HID0_DCE, 0xF0800000),
            (HID0_DCE, 0xF0080000), (HID0_DCE, 0xB0080000)]
EDGE_HOOKS = ["none", "wimg-cache", "wimg-I", "wimg-W", "all-hit", "all-miss", "phys-mask", "wpar"]


def edge_state(name, w, base, off, msr, xer, hid, reserve, hook):
    st = State()
    rd, ra, rb = fields(w)
    for i in range(32):
        st.gpr[i] = (0x01010101 * (i + 1) + 0x10) & M
    st.gpr[ra] = base
    if name not in DFORM and name not in ("lswi", "stswi"):
        st.gpr[rb] = off if rb != ra else base
    st.msr, st.xer = msr, xer
    st.hid0, st.hid2 = hid
    st.cr = 0x5A5A5A5A
    st.fpscr = 0xFFFFFFFF if (off & 1) else 0x00000003
    st.reserve, st.reserve_addr = reserve, 0x12345678
    st.pc = 0x80003100 if not (off & 2) else 0xFFFFFFFC
    for ea in candidate_eas(st, name, w):
        st.mem.load((ea - 40) & M, bytes(((ea + k) * 37 + 11) & 0xFF for k in range(176)))
    if hook == "wimg-cache":
        st.data_wimg = WimgHook([0b0010] * 16)
    elif hook == "wimg-I":
        st.data_wimg = WimgHook([0b0110] * 16)
    elif hook == "wimg-W":
        st.data_wimg = WimgHook([0b1010] * 16)
    elif hook in ("all-hit", "all-miss"):
        st.data_wimg = WimgHook([0b0011] * 16)
        st.data_phys = PhysHook("id", 0)
        st.dcache_hit = HitHook(hook == "all-hit", 0)
    elif hook == "phys-mask":
        st.data_wimg = WimgHook([0b0010] * 8 + [0b0110] * 8)
        st.data_phys = PhysHook("mask", 0)
    elif hook == "wpar":
        st.data_wimg = WimgHook([0b0110] * 16)
        st.data_phys = PhysHook("id", 0)
        st.wpar = base & 0xFFFFFFC0
    return st


def edge_worker(args):
    names, modA, modB, extra = args
    for p in extra:
        if p not in sys.path:
            sys.path.insert(0, p)
    mA, mB = load_model(modA), load_model(modB)
    classes, counts = collections.OrderedDict(), collections.Counter()
    n = 0
    for name in names:
        op, xo = CAT[name]
        i = 0
        for (rd, ra, rb) in EDGE_REGS:
            for base in ADDR_BASES:
                for off in EDGE_OFF:
                    for msr in EDGE_MSR:
                        i += 1
                        xer = EDGE_XER[i % len(EDGE_XER)]
                        hid = EDGE_HID[(i // 3) % len(EDGE_HID)]
                        hook = EDGE_HOOKS[(i // 7) % len(EDGE_HOOKS)]
                        reserve = bool(i % 2)
                        if xo is None:
                            w = (op << 26) | (rd << 21) | (ra << 16) | off
                        else:
                            rc = (i >> 1) & 1
                            d = 0 if name in ("dcbz", "dcbz_l") and (i % 5) else rd
                            nbf = rb if name not in ("lswi", "stswi") else (off & 31)
                            w = (op << 26) | (d << 21) | (ra << 16) | (nbf << 11) | (xo << 1) | rc
                        w &= M
                        st = edge_state(name, w, base, exts16(off) if xo is None else off, msr, xer, hid, reserve, hook)
                        n += 1
                        res = compare(name, w, st, mA, mB)
                        if res is not None:
                            key, detail = res
                            counts[key] += 1
                            classes.setdefault(key, detail)
    return n, classes, counts


def decode_sweep(mA, mB, seed):
    r = random.Random(seed ^ 0x5EED)
    n, bad = 0, []
    words = []
    for op in range(64):
        for _ in range(256):
            words.append((op << 26) | r.getrandbits(26))
    for op in (4, 31):
        for xo in range(1024):
            for variant in range(6):
                noise = r.getrandbits(15) << 11 if variant & 1 else 0
                words.append((op << 26) | noise | (xo << 1) | (variant >> 1 & 1))
    for w in words:
        n += 1
        if mA.supports(w) != mB.supports(w) or mA.mnemonic(w) != mB.mnemonic(w):
            bad.append("%08X A=%s B=%s" % (w, mA.mnemonic(w), mB.mnemonic(w)))
    return n, bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--n", type=int, default=240000)
    ap.add_argument("--seed", type=int, default=20260914)
    ap.add_argument("--jobs", type=int, default=max(1, min(12, (os.cpu_count() or 2) - 2)))
    ap.add_argument("--models", default="loadstore_A,loadstore_B")
    ap.add_argument("--path", action="append", default=[])
    ap.add_argument("--show", type=int, default=4)
    ap.add_argument("--no-edge", action="store_true")
    ap.add_argument("--mutants", action="store_true")
    a = ap.parse_args()
    extra = [os.path.abspath(p) for p in a.path]
    for p in extra:
        sys.path.insert(0, p)
    modA, modB = a.models.split(",")

    if a.mutants:
        missed = 0
        for i, (desc, _, _) in enumerate(MUTANTS):
            spec = "%s@%d" % (modB, i)
            per = max(1, a.n // a.jobs)
            tasks = [(a.seed * 31 + j, per, modA, spec, extra) for j in range(a.jobs)]
            with mp.Pool(a.jobs) as pool:
                res = pool.map(worker, tasks)
            nbad = sum(sum(ct.values()) for _, ct, _ in res)
            print("mutant %2d %-58s detected by %d of %d random vectors" % (i, desc, nbad, per * a.jobs))
            missed += nbad == 0
        print("MUTANTS missed: %d of %d" % (missed, len(MUTANTS)))
        return 1 if missed else 0

    mA, mB = load_model(modA), load_model(modB)
    for nm, (op, xo) in CAT.items():
        w = (op << 26) | ((xo << 1) if xo is not None else 0) | (1 if nm == "stwcx." else 0)
        for mod in (mA, mB):
            if not mod.supports(w) or mod.mnemonic(w) != nm:
                print("CATALOGUE MISMATCH %s %s -> %r" % (mod.__name__, nm, mod.mnemonic(w)))

    nd, bad = decode_sweep(mA, mB, a.seed)
    print("decode sweep: %d words, %d supports/mnemonic disagreements" % (nd, len(bad)))
    for b in bad[:10]:
        print("   ", b)

    pool = mp.Pool(a.jobs)
    ne, eclasses, ecounts = 0, collections.OrderedDict(), collections.Counter()
    if not a.no_edge:
        names = sorted(CAT)
        chunks = [names[j::a.jobs] for j in range(a.jobs)]
        for n_, cl, ct in pool.map(edge_worker, [(c, modA, modB, extra) for c in chunks if c]):
            ne += n_
            for k, v in cl.items():
                eclasses.setdefault(k, v)
            ecounts.update(ct)
        print("edge sweep: %d vectors, %d disagreeing, %d classes" % (ne, sum(ecounts.values()), len(eclasses)))
        byname = collections.defaultdict(list)
        for k in eclasses:
            byname[k[0]].append(k)
        for nm in sorted(byname):
            for k in byname[nm][: a.show]:
                print("  [%d] %s\n       %s" % (ecounts[k], k, eclasses[k]))
            if len(byname[nm]) > a.show:
                print("  ... %d more edge classes for %s" % (len(byname[nm]) - a.show, nm))

    per = a.n // a.jobs
    tasks = [(a.seed * 7919 + j, per + (1 if j < a.n % a.jobs else 0), modA, modB, extra) for j in range(a.jobs)]
    results = pool.map(worker, tasks)
    pool.close()
    classes, counts, outcomes = collections.OrderedDict(), collections.Counter(), collections.Counter()
    for cl, ct, oc in results:
        for k, v in cl.items():
            classes.setdefault(k, v)
        counts.update(ct)
        outcomes.update(oc)
    print("outcome coverage of agreeing random vectors (ok/partial/invalid/unmodelled):")
    line = ["%s %d/%d/%d/%d" % (nm, outcomes[(nm, "ok")], outcomes[(nm, "partial")], outcomes[(nm, "invalid")],
                                 outcomes[(nm, "unmodelled")]) for nm in sorted(CAT)]
    for i in range(0, len(line), 5):
        print("   " + "; ".join(line[i:i + 5]))
    total_bad = sum(counts.values())
    print("random sweep: %d vectors, %d disagreeing, %d classes" % (a.n, total_bad, len(classes)))
    byname = collections.defaultdict(list)
    for k in classes:
        byname[k[0]].append(k)
    for nm in sorted(byname):
        for k in byname[nm][: a.show]:
            print("  [%d] %s\n       %s" % (counts[k], k, classes[k]))
        if len(byname[nm]) > a.show:
            print("  ... %d more classes for %s" % (len(byname[nm]) - a.show, nm))
    dis = total_bad + sum(ecounts.values()) + len(bad)
    print("TOTAL vectors %d (decode %d + edge %d + random %d); disagreements %d" % (nd + ne + a.n, nd, ne, a.n, dis))
    return 0 if dis == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
