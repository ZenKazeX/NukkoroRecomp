"""Differential cross-check of the two independent "int" specification models.

    python crosscheck_int.py [--n N] [--seed S] [--jobs J] [--models int_A,int_B] [--show K]
                             [--mutants] [--no-edge]

    --mutants  applies each MUTANTS entry (a plausible single-point defect) to the second model
               and requires the edge sweep and the random sweep to each report it.

Last results (2026-09-14, seed 20260914):
    int_A vs int_B : decode 20,480 + edge 561,152 + random 400,000 = 981,632 vectors, 0 disagreements
    int_B vs int   : decode 20,480 + edge 561,152 + random 250,000 = 831,632 vectors, 0 disagreements
    --mutants      : 24 of 24 detected; edge sweep alone misses 0, random sweep (32k) alone misses 0

Draws N execution vectors (random + boundary operands, aliasing register indices, clean and
reserved-bit-dirty encodings, every XER/CR/FPSCR/MSR pattern class) for every mnemonic both
models support, runs both models from identical fresh states, and compares:
  - the outcome class (ok / invalid-form UB / partially-defined UB / unsupported)
  - the full State.arch_dict(), mem.log, mem.bytes and pending (undefined locations masked
    to 0 in both states, after normalising the two UB contracts)
  - the set of undefined locations (GPR indices + CR bit mask)
It also sweeps the whole opcode space (every primary opcode, every 10-bit extended opcode of
groups 19 and 31 with OE/Rc/field noise) comparing supports()/mnemonic() agreement.

UB contracts being normalised:
  int_A: step() raises WITHOUT touching state; exc.undefined == ('all',) for an invalid form,
         else exc.apply_defined(state) applies the defined parts and exc.undefined names
         'gpr[N]' / 'cr[0:2]'.
  int_B: exc.applied False -> nothing touched (invalid form); True -> defined parts applied,
         exc.gpr_undefined / exc.cr_undefined_mask name the rest.
  int (reconciled): same contract as int_A.
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

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from gekko_state import MASK32, State  # noqa: E402

M = MASK32

# ------------------------------------------------------------------------------------------
# Opcode catalogue, written from the manual encodings (NOT copied from either model):
# name -> (primary, ext kind, xo, reserved-bit ranges that a "clean" word keeps zero)
# ext kind: None = primary only; 'x10' = XO in bits 21-30; 'xo9' = XO in bits 22-30, OE bit 21.
# ------------------------------------------------------------------------------------------
CAT = {
    "mulli": (7, None, None, ()), "subfic": (8, None, None, ()),
    "cmpli": (10, None, None, ((9, 10),)), "cmpi": (11, None, None, ((9, 10),)),
    "addic": (12, None, None, ()), "addic.": (13, None, None, ()),
    "addi": (14, None, None, ()), "addis": (15, None, None, ()),
    "rlwimi": (20, None, None, ()), "rlwinm": (21, None, None, ()), "rlwnm": (23, None, None, ()),
    "ori": (24, None, None, ()), "oris": (25, None, None, ()), "xori": (26, None, None, ()),
    "xoris": (27, None, None, ()), "andi.": (28, None, None, ()), "andis.": (29, None, None, ()),
    "mcrf": (19, "x10", 0, ((9, 10), (14, 20), (31, 31))),
    "crnor": (19, "x10", 33, ((31, 31),)), "crandc": (19, "x10", 129, ((31, 31),)),
    "crxor": (19, "x10", 193, ((31, 31),)), "crnand": (19, "x10", 225, ((31, 31),)),
    "crand": (19, "x10", 257, ((31, 31),)), "creqv": (19, "x10", 289, ((31, 31),)),
    "crorc": (19, "x10", 417, ((31, 31),)), "cror": (19, "x10", 449, ((31, 31),)),
    "cmp": (31, "x10", 0, ((9, 10), (31, 31))), "cmpl": (31, "x10", 32, ((9, 10), (31, 31))),
    "mfcr": (31, "x10", 19, ((11, 20), (31, 31))), "mtcrf": (31, "x10", 144, ((11, 11), (20, 20), (31, 31))),
    "mcrxr": (31, "x10", 512, ((9, 20), (31, 31))),
    "slw": (31, "x10", 24, ()), "cntlzw": (31, "x10", 26, ((16, 20),)), "and": (31, "x10", 28, ()),
    "andc": (31, "x10", 60, ()), "nor": (31, "x10", 124, ()), "eqv": (31, "x10", 284, ()),
    "xor": (31, "x10", 316, ()), "orc": (31, "x10", 412, ()), "or": (31, "x10", 444, ()),
    "nand": (31, "x10", 476, ()), "srw": (31, "x10", 536, ()), "sraw": (31, "x10", 792, ()),
    "srawi": (31, "x10", 824, ()), "extsh": (31, "x10", 922, ((16, 20),)),
    "extsb": (31, "x10", 954, ((16, 20),)),
    "subfc": (31, "xo9", 8, ()), "addc": (31, "xo9", 10, ()), "mulhwu": (31, "xo9", 11, ((21, 21),)),
    "subf": (31, "xo9", 40, ()), "mulhw": (31, "xo9", 75, ((21, 21),)), "neg": (31, "xo9", 104, ((16, 20),)),
    "subfe": (31, "xo9", 136, ()), "adde": (31, "xo9", 138, ()), "subfze": (31, "xo9", 200, ((16, 20),)),
    "addze": (31, "xo9", 202, ((16, 20),)), "subfme": (31, "xo9", 232, ((16, 20),)),
    "addme": (31, "xo9", 234, ((16, 20),)), "mullw": (31, "xo9", 235, ()), "add": (31, "xo9", 266, ()),
    "divwu": (31, "xo9", 459, ()), "divw": (31, "xo9", 491, ()),
}

BOUND32 = [0, 1, 2, 3, 4, 5, 7, 8, 15, 16, 30, 31, 32, 33, 47, 48, 63, 64, 65, 0x7F, 0x80, 0xFF,
           0x7FFF, 0x8000, 0xFFFF, 0x10000, 0x7FFFFFFF, 0x80000000, 0x80000001, 0xFFFFFFFF,
           0xFFFFFFFE, 0xFFFFFFFD, 0xFFFF8000, 0xFFFF7FFF, 0x00008000, 0xFFFFFF80, 0xFFFFFF7F,
           0x40000000, 0xC0000000, 0x55555555, 0xAAAAAAAA, 0x0000FFFF, 0xFFFF0000, 0xFFFFFFC0,
           0xFFFFFFE0, 0xFFFFFFDF, 0x0000003F, 0x00000020, 0x0000001F, 0x7FFFFFFE, 0x80000002,
           0x46, 0x12345678, 0xDEADBEEF, 0x00010000, 0x00000100]
BOUND16 = [0, 1, 2, 0x7FFF, 0x8000, 0x8001, 0xFFFF, 0xFFFE, 0x00FF, 0xFF00, 0x001F, 0x0020, 0x003F]


def _setbits(w, a, b, v):
    width = b - a + 1
    sh = 31 - b
    mask = ((1 << width) - 1) << sh
    return (w & ~mask & M) | ((v << sh) & mask)


def _getbits(w, a, b):
    return (w >> (31 - b)) & ((1 << (b - a + 1)) - 1)


def rand_u32(r):
    k = r.random()
    if k < 0.40:
        return r.choice(BOUND32)
    if k < 0.50:
        return r.randrange(0, 70)                   # shift/rotate amounts incl. 32..63 and 64+
    if k < 0.55:
        return (r.getrandbits(26) << 6) | r.randrange(0, 64)   # high garbage, rB[26-31] varied
    if k < 0.60:
        return r.choice(BOUND32) ^ (1 << r.randrange(32))
    return r.getrandbits(32)


def rand_reg(r):
    return r.choice((0, 3, 4, 5)) if r.random() < 0.5 else r.randrange(32)


def gen_word(r, name):
    op, kind, xo, reserved = CAT[name]
    dirty = r.random() < 0.12
    w = (op << 26) | r.getrandbits(26)
    # register fields with aliasing bias
    w = _setbits(w, 6, 10, rand_reg(r))
    w = _setbits(w, 11, 15, rand_reg(r))
    if kind is None:
        if r.random() < 0.5:
            w = _setbits(w, 16, 31, r.choice(BOUND16))
        if name in ("rlwimi", "rlwinm", "rlwnm"):
            if name == "rlwnm":
                w = _setbits(w, 16, 20, rand_reg(r))
            if r.random() < 0.4:
                w = _setbits(w, 21, 25, r.choice((0, 1, 15, 16, 30, 31)))
                w = _setbits(w, 26, 30, r.choice((0, 1, 15, 16, 30, 31)))
    else:
        w = _setbits(w, 16, 20, rand_reg(r))
        if kind == "x10":
            w = _setbits(w, 21, 30, xo)
        else:
            w = _setbits(w, 22, 30, xo)
        if name == "srawi" and r.random() < 0.5:
            w = _setbits(w, 16, 20, r.choice((0, 1, 30, 31)))
    if not dirty:
        for a, b in reserved:
            w = _setbits(w, a, b, 0)
    return w & M


def gen_state(r):
    st = State()
    for i in range(32):
        st.gpr[i] = rand_u32(r)
        st.fpr[i] = [r.getrandbits(64), r.getrandbits(64)]
    k = r.random()
    if k < 0.3:
        st.cr = r.getrandbits(32)
    elif k < 0.6:
        st.cr = r.choice((0, M, 0x0FFFFFFF, 0xF0000000, 0x10000000, 0xE0000000))
    else:
        st.cr = r.getrandbits(32) & r.getrandbits(32)
    top = r.randrange(8) << 29                   # every SO/OV/CA combination
    low = r.getrandbits(29) if r.random() < 0.5 else r.choice((0, 0x7F, 0x1FFFFFFF, 0x10000000))
    st.xer = (top | low) & M
    # FPSCR / MSR / HID2 / FP registers are irrelevant to integer semantics but must survive:
    # every FPSCR mode (RN 0-3, NI, all enables, all sticky bits) is drawn.
    st.fpscr = r.getrandbits(32) if r.random() < 0.7 else r.choice((0, 3, 4, 0xF8, M, 0x80000000))
    st.msr = r.getrandbits(32) if r.random() < 0.5 else r.choice((0, 0x2032, 0x8000, 0x9032, 0xB032))
    st.lr, st.ctr = r.getrandbits(32), r.getrandbits(32)
    st.srr0, st.srr1, st.dar, st.dsisr = (r.getrandbits(32) for _ in range(4))
    st.dec, st.tb = r.getrandbits(32), r.getrandbits(64)
    st.hid0, st.hid2 = r.getrandbits(32), r.getrandbits(32)
    st.gqr = [r.getrandbits(32) for _ in range(8)]
    st.sprg = [r.getrandbits(32) for _ in range(4)]
    st.pc = r.choice((0x80003100, 0xFFFFFFFC, 0x7FFFFFFC, 0)) if r.random() < 0.2 else (r.getrandbits(30) << 2)
    st.reserve = r.random() < 0.5
    st.reserve_addr = r.getrandbits(32)
    st.pending = {"external": r.random() < 0.5, "decrementer": r.random() < 0.5}
    st.exception = r.choice((None, None, "program", "dsi"))
    return st


# ------------------------------------------------------------------------------------------
# Mutation self-validation: plausible single-point defects injected into a model's source text.
# Every mutant MUST be reported as a disagreement, otherwise the cross-check is not able to
# see that class of defect (--mutants).  Spec "int_B@7" = int_B.py with MUTANTS[7] applied.
# ------------------------------------------------------------------------------------------
MUTANTS = [
    ("SO cleared when no overflow", "    state.xer &= ~XER_OV & MASK32\n    if ov:",
     "    state.xer &= ~(XER_OV | XER_SO) & MASK32\n    if ov:"),
    ("srawi/sraw CA ignores sign", "_set_ca(state, sgn & (1 if", "_set_ca(state, (1 if"),
    ("divw floors instead of truncating", "q = abs(x) // abs(y)\n            if (x < 0) != (y < 0):\n                q = -q",
     "q = x // y"),
    ("addi/addis lose (rA|0)", "(ra if a else 0)", "ra"),
    ("slw ignores rB[26]", "_mask(0, 31 - n) if _bit(rb, 26) == 0 else 0", "_mask(0, 31 - n)"),
    ("mulhwu signed", "(ra * rb) >> 32", "((_s32(ra) * _s32(rb)) & MASK64) >> 32"),
    ("cmpli signed", "x, y = ra, imm", "x, y = _s32(ra), _exts16(imm)"),
    ("mtcrf CRM bit order reversed", "_bit(crm << 24, i)", "_bit(crm << 24, 7 - i)"),
    ("mcrxr does not clear XER[0-3]", "state.xer &= 0x0FFFFFFF", "pass"),
    ("CA from post-write aliasing read", "_set_ca(state, total_u >> 32)",
     "_set_ca(state, 1 if state.gpr[d] < (terms_u[0] & MASK32) else 0)"),
    ("OV range off by one", "_set_ov(state, not (-(1 << 31) <= total_s <= (1 << 31) - 1))",
     "_set_ov(state, not (-(1 << 31) <= total_s < (1 << 31) - 1))"),
    ("divw UB forgets OV", "            if oe:\n                _set_ov(state, True)", "            if oe:\n                pass"),
    ("cntlzw reserved field unchecked", "_reserved(name, 369, word, (16, 20))", "pass"),
    ("extsh sign from bit 15", "_bit(rs, 16)", "_bit(rs, 15)"),
    ("crandc operands swapped", "v = x & (1 - y)", "v = y & (1 - x)"),
    ("subfe carry-in forced 1", "[nra, rb, ca], [_s32(nra), _s32(rb), ca]", "[nra, rb, 1], [_s32(nra), _s32(rb), 1]"),
    ("pc does not wrap", "state.pc = (state.pc + 4) & MASK32", "state.pc = (state.pc + 4)"),
    ("mullw OV from unsigned product", "p = _s32(ra) * _s32(rb)", "p = ra * rb"),
    ("CR0 SO copied before OE update",
     "    if oe:\n        _set_ov(state, not (-(1 << 31) <= total_s <= (1 << 31) - 1))\n    if rc:\n        _cr0(state, result)",
     "    so_old = _so(state)\n    if oe:\n        _set_ov(state, not (-(1 << 31) <= total_s <= (1 << 31) - 1))\n    if rc:\n"
     "        _set_crf(state, 0, (((0b100 if _s32(result) < 0 else (0b010 if _s32(result) > 0 else 0b001)) << 1) | so_old))"),
    ("rlwimi reads rA after partial write", "            res = (r & m) | (ra & ~m & MASK32)",
     "            state.gpr[a] = r & m\n            res = (r & m) | (state.gpr[a] & ~m & MASK32)"),
    ("divwu x/0 not flagged", "            bad = (y == 0)\n", "            bad = False\n            y = y or 1\n"),
    ("cmp L=1 accepted",
     "        if _f(word, 10, 10):\n            raise UndefinedBehaviour(\"%s: L = 1 -> invalid form (PAGE %d, PAGE 337)\" % (name, page))\n        crf",
     "        crf"),
    ("addic. does not copy SO", "ca_affected=True, oe=False, rc=True)", "ca_affected=True, oe=False, rc=False)"),
    ("FPSCR clobbered by logic op", "    state.gpr[a] = res\n    if rc:\n        _cr0(state, res)",
     "    state.gpr[a] = res\n    state.fpscr = 0\n    if rc:\n        _cr0(state, res)"),
]


def load_model(spec):
    if "@" not in spec:
        return importlib.import_module(spec)
    base, idx = spec.split("@")
    key = "%s_mut%s" % (base, idx)
    if key in sys.modules:
        return sys.modules[key]
    import types
    desc, old, new = MUTANTS[int(idx)]
    with open(os.path.join(HERE, base + ".py"), encoding="utf-8") as fh:
        src = fh.read()
    if old not in src:
        raise RuntimeError("mutant %s (%s): anchor text not found" % (idx, desc))
    src = src.replace(old, new, 1).replace('if __name__ == "__main__":', "if False:")
    mod = types.ModuleType(key)
    mod.__file__ = os.path.join(HERE, base + ".py")
    sys.modules[key] = mod
    exec(compile(src, key, "exec"), mod.__dict__)
    return mod


# ------------------------------------------------------------------------------------------
# Model runners -> (outcome, undefined-gprs, undefined-cr-mask, message)
# ------------------------------------------------------------------------------------------
def run_model(mod, st, w):
    try:
        mod.step(st, w)
        return ("ok", frozenset(), 0, "")
    except mod.UndefinedBehaviour as u:
        what = getattr(u, "what", str(u))
        if hasattr(u, "applied"):                       # int_B contract
            if not u.applied:
                return ("invalid", frozenset(), 0, what)
            return ("partial", frozenset(u.gpr_undefined), u.cr_undefined_mask, what)
        if "all" in u.undefined:                        # int_A contract
            return ("invalid", frozenset(), 0, what)
        u.apply_defined(st)
        gprs, crm = set(), 0
        for loc in u.undefined:
            m = re.fullmatch(r"gpr\[(\d+)\]", loc)
            if m:
                gprs.add(int(m.group(1)))
            elif loc == "cr[0:2]":
                crm |= 0xE0000000
            else:
                raise RuntimeError("unknown undefined location %r" % loc)
        return ("partial", frozenset(gprs), crm, what)
    except ValueError as e:
        return ("unsupported", frozenset(), 0, str(e))


def snapshot(st, gprs=frozenset(), crm=0):
    d = st.arch_dict()
    d = {k: (list(map(list, v)) if k == "fpr" else (list(v) if isinstance(v, list) else v)) for k, v in d.items()}
    for g in gprs:
        d["gpr"][g] = 0
    d["cr"] &= ~crm & M
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


def classify_fields(fields):
    return tuple(sorted(set("gpr" if f.startswith("gpr[") else f for f in fields)))


def one_vector(r, names, mA, mB, outcomes=None):
    name = r.choice(names)
    w = gen_word(r, name)
    base = gen_state(r)
    if name in ("divw", "divwu") and r.random() < 0.25:
        # force the manual's undefined divisions (PAGE 385/386) often enough to be exercised
        ia, ib = _getbits(w, 11, 15), _getbits(w, 16, 20)
        if r.random() < 0.5:
            base.gpr[ib] = 0
        else:
            base.gpr[ia], base.gpr[ib] = 0x80000000, 0xFFFFFFFF
    pre = snapshot(base)
    sA, sB = base.clone(), base.clone()
    oA = run_model(mA, sA, w)
    oB = run_model(mB, sB, w)
    if outcomes is not None:
        outcomes[(name, oA[0])] += 1
    problems = []
    if oA[0] != oB[0]:
        problems.append(("outcome", oA[0], oB[0]))
    else:
        if oA[3] and "PAGE" not in oA[3]:
            problems.append(("A-no-page",))
        if oB[3] and "PAGE" not in oB[3]:
            problems.append(("B-no-page",))
        if oA[0] in ("invalid", "unsupported"):
            dA, dB = snapshot(sA), snapshot(sB)
            if dA != pre:
                problems.append(("A-modified-on-" + oA[0], tuple(diff_fields(pre, dA))))
            if dB != pre:
                problems.append(("B-modified-on-" + oB[0], tuple(diff_fields(pre, dB))))
        else:
            if oA[1] != oB[1] or oA[2] != oB[2]:
                problems.append(("undefined-set", (sorted(oA[1]), hex(oA[2])), (sorted(oB[1]), hex(oB[2]))))
            gprs, crm = oA[1] | oB[1], oA[2] | oB[2]
            dA, dB = snapshot(sA, gprs, crm), snapshot(sB, gprs, crm)
            if dA != dB:
                problems.append(("state", classify_fields(diff_fields(dA, dB))))
            # neither model may write a GPR it declares undefined
            for g in oA[1]:
                if sA.gpr[g] != base.gpr[g]:
                    problems.append(("A-invented-undefined", g))
            for g in oB[1]:
                if sB.gpr[g] != base.gpr[g]:
                    problems.append(("B-invented-undefined", g))
    if not problems:
        return None
    detail = {
        "name": name, "word": "%08X" % w,
        "pre": {k: pre[k] for k in ("gpr", "cr", "xer", "pc")},
        "A": (oA[0], sorted(oA[1]), hex(oA[2]), oA[3]), "B": (oB[0], sorted(oB[1]), hex(oB[2]), oB[3]),
        "A_state": {k: getattr(sA, k) for k in ("cr", "xer", "pc")},
        "B_state": {k: getattr(sB, k) for k in ("cr", "xer", "pc")},
    }
    diffg = [i for i in range(32) if sA.gpr[i] != sB.gpr[i]]
    detail["gpr_diff"] = {i: ("%08X" % sA.gpr[i], "%08X" % sB.gpr[i]) for i in diffg}
    key = (name, tuple(p[0] if p[0] in ("A-no-page", "B-no-page") else p for p in problems))
    return key, detail


def decode_sweep(mA, mB, seed):
    """supports()/mnemonic() agreement over the whole primary + group 19/31 opcode space."""
    r = random.Random(seed ^ 0x5EED)
    n, bad = 0, []
    for op in range(64):
        for _ in range(64):
            w = (op << 26) | r.getrandbits(26)
            n += 1
            if mA.supports(w) != mB.supports(w) or mA.mnemonic(w) != mB.mnemonic(w):
                bad.append("%08X A=%s B=%s" % (w, mA.mnemonic(w), mB.mnemonic(w)))
    for op in (19, 31):
        for xo in range(1024):
            for variant in range(8):
                w = (op << 26) | (xo << 1) | (r.getrandbits(15) << 11 if variant & 1 else 0) | (variant >> 1 & 1)
                if variant & 4:
                    w = (op << 26) | (xo << 1) | (r.getrandbits(15) << 11) | r.getrandbits(1)
                n += 1
                if mA.supports(w) != mB.supports(w) or mA.mnemonic(w) != mB.mnemonic(w):
                    bad.append("%08X A=%s B=%s" % (w, mA.mnemonic(w), mB.mnemonic(w)))
    return n, bad


def worker(args):
    seed, count, modA, modB = args
    mA, mB = load_model(modA), load_model(modB)
    names = sorted(nm for nm in CAT)
    r = random.Random(seed)
    classes = collections.OrderedDict()
    counts = collections.Counter()
    outcomes = collections.Counter()
    for _ in range(count):
        res = one_vector(r, names, mA, mB, outcomes)
        if res is None:
            continue
        key, detail = res
        counts[key] += 1
        if key not in classes:
            classes[key] = detail
    return classes, counts, outcomes


EDGE_VALUES = [0, 1, 31, 32, 63, 0x7FFF, 0x8000, 0x00010000, 0x7FFFFFFF, 0x80000000, 0x80000001,
               0xFFFFFFFF, 0xFFFFFFFE, 0x12345678, 0xFFFFFFE0, 0x0000FFFF]


def edge_word(name, va, vb, flags, alias, dirty):
    """Deterministic encoding.  Operand registers are loaded by edge_state(): r0 = r3 = r4 = va,
    r5 = vb, so the 'first' operand (rA for arithmetic, rS for logical/shift/rotate) is va and the
    second (rB / immediate / SH) is vb.  alias=True puts every register field on the same GPR."""
    op, kind, xo, reserved = CAT[name]
    rc = flags & 1
    oe = (flags >> 1) & 1
    if kind is None:
        imm = vb & 0xFFFF
        if name in ("cmpi", "cmpli"):
            w = (op << 26) | (((flags * 3) & 7) << 23) | ((0 if alias else 4) << 16) | imm
        elif name in ("rlwimi", "rlwinm", "rlwnm"):
            sh = 5 if name == "rlwnm" else vb & 31
            w = (op << 26) | (3 << 21) | ((3 if alias else 4) << 16) | (sh << 11) | \
                ((vb >> 5 & 31) << 6) | ((vb >> 10 & 31) << 1) | rc
            if name == "rlwnm" and alias:
                w = _setbits(w, 16, 20, 3)
        elif name in ("ori", "oris", "xori", "xoris", "andi.", "andis."):
            w = (op << 26) | (3 << 21) | ((3 if alias else 6) << 16) | imm
        else:  # addi addis addic addic. subfic mulli: rD=6 (or rA), rA = r4 (alias: r0)
            w = (op << 26) | ((0 if alias else 6) << 21) | ((0 if alias else 4) << 16) | imm
    elif op == 19:
        if name == "mcrf":
            w = (19 << 26) | ((va & 7) << 23) | ((vb & 7) << 18)
        else:
            bd, ba = va & 31, vb & 31
            bb = ba if alias else (va >> 5) & 31
            w = (19 << 26) | (bd << 21) | (ba << 16) | (bb << 11) | (xo << 1)
    elif kind == "x10":
        if name in ("cmp", "cmpl"):
            w = (31 << 26) | (((flags * 3) & 7) << 23) | (4 << 16) | ((4 if alias else 5) << 11) | (xo << 1)
        elif name == "mcrxr":
            w = (31 << 26) | (((va ^ flags) & 7) << 23) | (xo << 1)
        elif name == "mfcr":
            w = (31 << 26) | ((3 if alias else 6) << 21) | (xo << 1)
        elif name == "mtcrf":
            w = (31 << 26) | ((3 if alias else 5) << 21) | ((va & 0xFF) << 12) | (xo << 1)
        else:
            b = vb & 31 if name == "srawi" else (0 if name in ("cntlzw", "extsb", "extsh") else (3 if alias else 5))
            w = (31 << 26) | (3 << 21) | ((3 if alias else 6) << 16) | (b << 11) | (xo << 1) | rc
    else:  # xo9
        b = 0 if name in ("addme", "addze", "subfme", "subfze", "neg") else (4 if alias else 5)
        oeb = 0 if name in ("mulhw", "mulhwu") else oe
        w = (31 << 26) | ((4 if alias else 6) << 21) | (4 << 16) | (b << 11) | (oeb << 10) | (xo << 1) | rc
    if dirty:
        a, b = reserved[flags % len(reserved)]
        w |= 1 << (31 - (a + (flags >> 1) % (b - a + 1)))
    return w & M


def edge_state(va, vb, xer, flags, alias):
    st = State()
    for i in range(32):
        st.gpr[i] = (0x5A5A5A5A ^ (i * 0x01010101)) & M
    st.gpr[0] = st.gpr[3] = st.gpr[4] = va
    st.gpr[5] = vb
    st.xer = xer
    st.cr = (0x5A5A5A5A ^ va ^ (vb << 7)) & M
    st.fpscr = M if flags & 2 else 0x12345678
    st.msr = 0x9032 if flags & 1 else 0x2032
    st.pc = 0xFFFFFFFC if (alias and flags == 3) else 0x80003100
    return st


def edge_name(args):
    modA, modB, name = args
    mA, mB = load_model(modA), load_model(modB)
    classes, counts = collections.OrderedDict(), collections.Counter()
    n = 0
    reserved = CAT[name][3]
    for va in EDGE_VALUES:
        for vb in EDGE_VALUES:
            for dirty in ((False, True) if reserved else (False,)):
                for xer in ((0, 0xE0000000, 0x20000000, 0xDFFFFFFF) if not dirty else (0x80000000,)):
                    for flags in range(4):
                        for alias in (False, True):
                            w = edge_word(name, va, vb, flags, alias, dirty)
                            st = edge_state(va, vb, xer, flags, alias)
                            n += 1
                            sA, sB = st.clone(), st.clone()
                            pre = snapshot(st)
                            oA, oB = run_model(mA, sA, w), run_model(mB, sB, w)
                            probs = []
                            if oA[0] != oB[0]:
                                probs.append(("outcome", oA[0], oB[0]))
                            elif oA[0] in ("invalid", "unsupported"):
                                if snapshot(sA) != pre or snapshot(sB) != pre:
                                    probs.append(("modified-on-invalid",))
                            else:
                                if oA[1:3] != oB[1:3]:
                                    probs.append(("undefined-set",))
                                gp, cm = oA[1] | oB[1], oA[2] | oB[2]
                                dA, dB = snapshot(sA, gp, cm), snapshot(sB, gp, cm)
                                if dA != dB:
                                    probs.append(("state", classify_fields(diff_fields(dA, dB))))
                            if probs:
                                key = (name, tuple(probs))
                                counts[key] += 1
                                if key not in classes:
                                    classes[key] = {"name": name, "word": "%08X" % w, "va": "%08X" % va,
                                                    "vb": "%08X" % vb, "xer": "%08X" % xer,
                                                    "A": (oA[0], {i: "%08X" % sA.gpr[i] for i in (0, 3, 4, 5, 6)},
                                                          "%08X" % sA.xer, "%08X" % sA.cr),
                                                    "B": (oB[0], {i: "%08X" % sB.gpr[i] for i in (0, 3, 4, 5, 6)},
                                                          "%08X" % sB.xer, "%08X" % sB.cr)}
    return n, classes, counts


def edge_vectors(modA, modB, jobs=1):
    """Deterministic edge sweep: every mnemonic x 16 boundary values for each of two operands x
    4 XER patterns x OE/Rc (or crfD choice) x {distinct, aliased} registers, plus one
    reserved-bit-dirty pass per mnemonic that has reserved fields."""
    tasks = [(modA, modB, nm) for nm in sorted(CAT)]
    if jobs > 1:
        with mp.Pool(jobs) as pool:
            res = pool.map(edge_name, tasks)
    else:
        res = [edge_name(t) for t in tasks]
    n, classes, counts = 0, collections.OrderedDict(), collections.Counter()
    for nn, cl, ct in res:
        n += nn
        for k, v in cl.items():
            classes.setdefault(k, v)
        counts.update(ct)
    return n, classes, counts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--n", type=int, default=240000)
    ap.add_argument("--seed", type=int, default=20260914)
    ap.add_argument("--jobs", type=int, default=max(1, min(16, (os.cpu_count() or 2) - 2)))
    ap.add_argument("--models", default="int_A,int_B")
    ap.add_argument("--show", type=int, default=3)
    ap.add_argument("--mutants", action="store_true",
                    help="validate detection power: every MUTANTS entry applied to the second model must disagree")
    ap.add_argument("--no-edge", action="store_true")
    a = ap.parse_args()
    modA, modB = a.models.split(",")
    if a.mutants:
        missed = edge_missed = rand_missed = 0
        for i, (desc, _, _) in enumerate(MUTANTS):
            spec = "%s@%d" % (modB, i)
            ne, ecl, ect = (0, {}, collections.Counter()) if a.no_edge else edge_vectors(modA, spec, a.jobs)
            per = a.n // a.jobs
            tasks = [(a.seed * 31 + j, per, modA, spec) for j in range(a.jobs)]
            with mp.Pool(a.jobs) as pool:
                res = pool.map(worker, tasks)
            nbad = sum(sum(ct.values()) for _, ct, _ in res) + sum(ect.values())
            rbad = nbad - sum(ect.values())
            print("mutant %2d %-40s detected by %d vectors (edge %d, random %d)" % (i, desc, nbad, sum(ect.values()), rbad))
            missed += nbad == 0
            edge_missed += (not a.no_edge) and sum(ect.values()) == 0
            rand_missed += rbad == 0
        print("MUTANTS missed: %d of %d (edge sweep alone misses %d, random sweep alone misses %d)"
              % (missed, len(MUTANTS), edge_missed, rand_missed))
        return 1 if missed else 0
    mA, mB = load_model(modA), load_model(modB)

    # the catalogue must be exactly the intersection both models support
    for nm, (op, kind, xo, _) in CAT.items():
        w = (op << 26) | ((xo << 1) if kind == "x10" else ((xo << 1) if kind == "xo9" else 0))
        for mod in (mA, mB):
            if not mod.supports(w) or (mod.mnemonic(w) or "").rstrip(".") != nm.rstrip("."):
                print("CATALOGUE MISMATCH %s %s -> %r" % (mod.__name__, nm, mod.mnemonic(w)))

    nd, bad = decode_sweep(mA, mB, a.seed)
    print("decode sweep: %d words, %d supports/mnemonic disagreements" % (nd, len(bad)))
    for b in bad[:10]:
        print("   ", b)

    ne, eclasses, ecounts = (0, {}, collections.Counter()) if a.no_edge else edge_vectors(modA, modB, a.jobs)
    print("edge sweep: %d vectors, %d disagreeing, %d classes" % (ne, sum(ecounts.values()), len(eclasses)))
    for k, det in list(eclasses.items())[:40]:
        print("  [%d] %s\n       %s" % (ecounts[k], k, det))

    per = a.n // a.jobs
    tasks = [(a.seed * 7919 + j, per + (1 if j < a.n % a.jobs else 0), modA, modB) for j in range(a.jobs)]
    classes, counts = collections.OrderedDict(), collections.Counter()
    if a.jobs == 1:
        results = [worker(tasks[0])]
    else:
        with mp.Pool(a.jobs) as pool:
            results = pool.map(worker, tasks)
    outcomes = collections.Counter()
    for cl, ct, oc in results:
        for k, v in cl.items():
            classes.setdefault(k, v)
        counts.update(ct)
        outcomes.update(oc)
    print("outcome coverage (mnemonic ok/partial-UB/invalid-UB/unsupported):")
    line = []
    for nm in sorted(CAT):
        line.append("%s %d/%d/%d/%d" % (nm, outcomes[(nm, "ok")], outcomes[(nm, "partial")],
                                         outcomes[(nm, "invalid")], outcomes[(nm, "unsupported")]))
    for i in range(0, len(line), 6):
        print("   " + "; ".join(line[i:i + 6]))
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
    grand = nd + ne + a.n
    print("TOTAL vectors %d (decode %d + edge %d + random %d); disagreements %d" %
          (grand, nd, ne, a.n, total_bad + sum(ecounts.values()) + len(bad)))
    return 0 if (total_bad + sum(ecounts.values()) + len(bad)) == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
