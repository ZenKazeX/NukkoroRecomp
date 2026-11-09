"""Differential cross-check of the two independent "branch" specification models.

    python crosscheck_branch.py [--n N] [--seed S] [--jobs J] [--models branch_A,branch_B] [--show K]
                                [--no-edge] [--mutants]

Draws N random execution vectors plus a deterministic edge sweep for every instruction both models
support (b/ba/bl/bla, bc family with all 32 BO encodings, bclr/bcctr, sc, rfi, tw, twi, mfmsr, mtmsr,
mfspr, mftb (XO 371), mtspr), with clean and reserved-bit-dirty encodings, every SPR number 0-1023,
aliasing registers, boundary operands (CTR 0/1/2/-1, LR/CTR/SRR0 low bits, SIMM/BD/LI extremes),
every MSR mode that matters here (PR, SE, BE, IP, ILE, LE, POW, EE, reserved bits) plus random
FPSCR/HID0/HID2/DEC/TB/pending, and runs both models from identical cloned states comparing
  - the outcome class: ok / partial (some locations undefined) / invalid (whole outcome undefined,
    state untouched) / unsupported (needs state gekko_state.State does not carry, state untouched)
  - the normalised set of undefined locations with their bit masks
  - the full State.arch_dict(), pending, mem.log and mem.bytes, undefined bits masked in both
It also sweeps the opcode space comparing supports().

UB contracts being normalised:
  A (and the reconciled branch.py): step() raises UndefinedBehaviour without touching state;
     .partial is None -> invalid; else .apply_defined(state) and .undefined maps location ->
     int mask or True.  NotImplementedError (incl. NotModelled) -> unsupported.
  B: step() raises NotModelled -> unsupported, UndefinedBehaviour -> invalid, ValueError ->
     unsupported; on success state.undefined_bits maps location -> int mask or True.
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


def _b(i):
    return 1 << (31 - i)


# MSR bits, Table 4-4 PAGE 164-165 (written here independently of both models)
POW, ILE, EE, PR, FP, ME = _b(13), _b(15), _b(16), _b(17), _b(18), _b(19)
FE0, SE, BE, FE1, IP, IR, DR, PM, RI, LE = _b(20), _b(21), _b(22), _b(23), _b(25), _b(26), _b(27), \
    _b(29), _b(30), _b(31)
MSR_RESERVED = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 14, 24, 28]

# Every SPR number appearing in Tables 12-10 / 12-13 (PAGE 463-465, 477-479) and Table 2-52 / 2-53
SPR_LISTED = ([1, 8, 9, 18, 19, 22, 25, 26, 27, 268, 269, 272, 273, 274, 275, 282, 284, 285, 287] +
              list(range(528, 544)) + list(range(912, 924)) + list(range(936, 944)) +
              list(range(952, 960)) + [1008, 1009, 1010, 1013, 1017, 1019, 1020, 1021, 1022])
SPR_NEAR = sorted({(n + d) & 0x3FF for n in SPR_LISTED for d in (-1, 1, 16, -16, 32)} - set(SPR_LISTED))

BOUND32 = [0, 1, 2, 3, 4, 0x7FFFFFFF, 0x80000000, 0x80000001, 0xFFFFFFFF, 0xFFFFFFFE, 0xFFFFFFFC,
           0x7FFF, 0x8000, 0xFFFF, 0xFFFF8000, 0xFFFF7FFF, 0x00008000, 0x12345678, 0x80003100,
           0x80003103, 0xFFF00000, 0x0000B032, 0x00002032, 0x55555555, 0xAAAAAAAA]
BOUND16 = [0, 1, 2, 0x7FFF, 0x8000, 0x8001, 0xFFFF, 0xFFFE, 0x00FF, 0xFF00]

NAMES = ["b", "bc", "bclr", "bcctr", "sc", "rfi", "tw", "twi", "mfmsr", "mtmsr", "mfspr", "mftb", "mtspr"]


def spr_split(n):
    return ((n & 0x1F) << 16) | (((n >> 5) & 0x1F) << 11)


def rand_u32(r):
    k = r.random()
    if k < 0.45:
        return r.choice(BOUND32)
    if k < 0.55:
        return r.choice(BOUND32) ^ (1 << r.randrange(32))
    return r.getrandbits(32)


def rand_reg(r):
    return r.choice((0, 1, 3, 4, 31)) if r.random() < 0.5 else r.randrange(32)


def rand_msr(r):
    k = r.random()
    if k < 0.25:
        return r.getrandbits(32)
    base = r.choice((0x2032, 0xB032, 0x3032, 0x0000, 0x1000, 0x9032, 0x00002030))
    for bit in (POW, ILE, EE, PR, SE, BE, IP, LE, PM, FE0, FE1):
        if r.random() < 0.2:
            base ^= bit
    if r.random() < 0.25:
        base |= _b(r.choice(MSR_RESERVED))
    return base & M


def gen_word(r, name):
    dirty = r.random() < 0.12
    if name == "b":
        li = r.choice((0, 1, 0x7FFFFF, 0x800000, 0xFFFFFF, 0xFFFFFE)) if r.random() < 0.4 else r.getrandbits(24)
        return (18 << 26) | (li << 2) | r.getrandbits(2)
    if name == "bc":
        bd = r.choice((0, 1, 0x1FFF, 0x2000, 0x3FFF, 0x3FFE)) if r.random() < 0.4 else r.getrandbits(14)
        return (16 << 26) | (r.randrange(32) << 21) | (r.randrange(32) << 16) | (bd << 2) | r.getrandbits(2)
    if name in ("bclr", "bcctr"):
        xo = 16 if name == "bclr" else 528
        res = r.getrandbits(5) if dirty else 0
        return (19 << 26) | (r.randrange(32) << 21) | (r.randrange(32) << 16) | (res << 11) | (xo << 1) | r.getrandbits(1)
    if name == "sc":
        if not dirty:
            return 0x44000002
        return (17 << 26) | r.getrandbits(26) if r.random() < 0.5 else 0x44000002 ^ (1 << r.randrange(26))
    if name == "rfi":
        if not dirty:
            return 0x4C000064
        return 0x4C000064 | (r.getrandbits(15) << 11) | r.getrandbits(1) if r.random() < 0.5 else \
            0x4C000064 ^ r.choice([1 << i for i in list(range(11, 26)) + [0]])
    if name == "twi":
        simm = r.choice(BOUND16) if r.random() < 0.5 else r.getrandbits(16)
        return (3 << 26) | (r.randrange(32) << 21) | (rand_reg(r) << 16) | simm
    if name == "tw":
        return (31 << 26) | (r.randrange(32) << 21) | (rand_reg(r) << 16) | (rand_reg(r) << 11) | (4 << 1) | \
            (1 if dirty else 0)
    if name in ("mfmsr", "mtmsr"):
        xo = 83 if name == "mfmsr" else 146
        res = (r.getrandbits(10) << 11 | r.getrandbits(1)) if dirty else 0
        return (31 << 26) | (rand_reg(r) << 21) | res | (xo << 1)
    if name in ("mfspr", "mftb", "mtspr"):
        xo = {"mfspr": 339, "mftb": 371, "mtspr": 467}[name]
        k = r.random()
        if k < 0.6:
            n = r.choice(SPR_LISTED)
        elif k < 0.75:
            n = r.choice(SPR_NEAR)
        else:
            n = r.randrange(1024)
        rc = 1 if r.random() < 0.1 else 0
        return (31 << 26) | (rand_reg(r) << 21) | spr_split(n) | (xo << 1) | rc
    raise KeyError(name)


def gen_state(r, name=None, word=0):
    st = State()
    for i in range(32):
        st.gpr[i] = rand_u32(r)
        st.fpr[i] = [r.getrandbits(64), r.getrandbits(64)]
    k = r.random()
    st.cr = r.getrandbits(32) if k < 0.5 else r.choice((0, M, 0x20000000, 0x80000000, 0x00000001, 0x7FFFFFFF))
    st.xer = r.getrandbits(32) if r.random() < 0.5 else r.choice((0, 0xE000007F, 0x2000FF00, 0x10000000))
    # every FPSCR mode (RN 0-3, NI, enables, sticky bits) - irrelevant to branches but must survive
    st.fpscr = r.getrandbits(32) if r.random() < 0.7 else r.choice((0, 3, 4, 0xF8, M))
    st.msr = rand_msr(r)
    st.lr = rand_u32(r) if r.random() < 0.7 else (r.getrandbits(30) << 2) | r.getrandbits(2)
    st.ctr = r.choice((0, 1, 2, M, 0x80000000, 0x80000001)) if r.random() < 0.5 else rand_u32(r)
    st.srr0 = rand_u32(r)
    st.srr1 = rand_msr(r) if r.random() < 0.6 else r.getrandbits(32)
    st.dar, st.dsisr = r.getrandbits(32), r.getrandbits(32)
    st.dec = r.choice((0, 0x7FFFFFFF, 0x80000000, M, 1)) if r.random() < 0.4 else r.getrandbits(32)
    st.tb = r.getrandbits(64)
    st.hid0 = r.getrandbits(32) if r.random() < 0.5 else r.choice((0, _b(8), _b(9), _b(10), 0x0011C004, _b(16) | _b(17)))
    st.hid2 = r.getrandbits(32) if r.random() < 0.5 else r.choice((0xE0000000, 0xA0000000, 0x0F800000, 0))
    st.gqr = [r.getrandbits(32) if r.random() < 0.5 else r.choice((0, 0x00040004, 0xFFFFFFFF)) for _ in range(8)]
    st.sprg = [r.getrandbits(32) for _ in range(4)]
    k = r.random()
    if k < 0.15:
        st.pc = r.choice((0x80003100, 0xFFFFFFFC, 0x7FFFFFFC, 0, 0xFFF00000, 4))
    elif k < 0.17:
        st.pc = r.getrandbits(32)                       # unaligned CIA: arithmetic must still agree
    else:
        st.pc = r.getrandbits(30) << 2
    st.reserve = r.random() < 0.5
    st.reserve_addr = r.getrandbits(32)
    st.pending = {"external": r.random() < 0.5, "decrementer": r.random() < 0.5}
    st.exception = r.choice((None, None, "program", "dsi", "trace"))
    if r.random() < 0.05:
        st.mem.load(r.getrandbits(32) & ~3, bytes(r.getrandbits(8) for _ in range(8)))
    # instruction-directed biasing
    if name == "mtmsr":
        rs = (word >> 21) & 0x1F
        v = st.msr
        for bit in (SE, BE, LE, POW, EE, PR, IP, ILE):
            if r.random() < 0.3:
                v ^= bit
        if r.random() < 0.3:
            v |= _b(r.choice(MSR_RESERVED))
        st.gpr[rs] = rand_msr(r) if r.random() < 0.3 else v & M
    if name == "rfi" and r.random() < 0.5:
        st.srr1 = st.msr ^ r.choice((_b(24), _b(28), _b(29), POW, SE, LE, PR, _b(1), _b(0), 0))
    if name == "mtspr":
        rs = (word >> 21) & 0x1F
        if r.random() < 0.5:
            st.gpr[rs] = r.choice((0x80000000, 0x7FFFFFFF, M, 0, 0x00110C00, 0x0000C800, 0xE0800000,
                                   0xF00FFFFF, 0x0F000000, 0x00070007, 0xC0F8C0F8, 0x1000FF7F))
    return st


# ------------------------------------------------------------------------------------------
# normalisation
# ------------------------------------------------------------------------------------------
REG32 = ("srr0", "srr1", "msr", "cr", "xer", "hid0", "hid2", "dec", "lr", "ctr", "dar", "dsisr", "pc")
ANNOTATIONS = ("trace", "power_mode", "le_effective_point", "pending.decrementer")


def norm_loc(loc):
    m = re.fullmatch(r"(gpr|gqr|sprg)\[?(\d+)\]?", loc)
    if m:
        return "%s[%d]" % (m.group(1), int(m.group(2)))
    return loc


def norm_undef(und):
    out = {}
    for loc, v in und.items():
        loc = norm_loc(loc)
        if loc in ANNOTATIONS or loc == "all" or not (loc in REG32 or re.fullmatch(r"(gpr|gqr|sprg)\[\d+\]", loc)):
            out[loc] = True if v is True or v else v
        else:
            v = M if v is True else int(v) & M
            if v:
                out[loc] = out.get(loc, 0) | v
    return out


class Unsupported(Exception):
    pass


def run_model(mod, st, w):
    """-> (outcome, normalised undefined map, message)"""
    contract_b = hasattr(mod, "undefined_bits")
    try:
        if contract_b and hasattr(st, "undefined_bits"):
            del st.undefined_bits
        mod.step(st, w)
        und = norm_undef(getattr(st, "undefined_bits", {}) or {}) if contract_b else {}
        return ("partial" if und else "ok", und, "")
    except NotImplementedError as e:
        return ("unsupported", {}, str(e))
    except ValueError as e:
        return ("unsupported", {}, str(e))
    except mod.UndefinedBehaviour as u:
        if type(u).__name__ == "NotModelled":
            return ("unsupported", {}, str(u))
        if contract_b:
            return ("invalid", {}, str(u))
        if u.partial is None:
            return ("invalid", {}, getattr(u, "what", str(u)))
        u.apply_defined(st)
        return ("partial", norm_undef(u.undefined), getattr(u, "what", str(u)))


def snapshot(st, und=None):
    d = st.arch_dict()
    d = {k: (list(map(list, v)) if k == "fpr" else (list(v) if isinstance(v, list) else v)) for k, v in d.items()}
    d["pending"] = dict(st.pending)
    d["mem.log"] = list(st.mem.log)
    d["mem.bytes"] = dict(st.mem.bytes)
    for loc, v in (und or {}).items():
        m = re.fullmatch(r"(gpr|gqr|sprg)\[(\d+)\]", loc)
        if m:
            d[m.group(1)][int(m.group(2))] &= ~v & M
        elif loc in REG32:
            d[loc] &= ~v & M
        elif loc == "pending.decrementer":
            d["pending"].pop("decrementer", None)
    return d


def diff_fields(a, b):
    out = []
    for k in a:
        if a[k] != b[k]:
            if k in ("gpr", "gqr", "sprg"):
                out.extend("%s[%d]" % (k, i) for i in range(len(a[k])) if a[k][i] != b[k][i])
            else:
                out.append(k)
    return out


def compare(mA, mB, base, w):
    pre = snapshot(base)
    sA, sB = base.clone(), base.clone()
    oA = run_model(mA, sA, w)
    oB = run_model(mB, sB, w)
    problems = []
    if oA[0] != oB[0]:
        problems.append(("outcome", oA[0], oB[0]))
    else:
        if oA[2] and "PAGE" not in oA[2] and oA[0] != "unsupported":
            problems.append(("A-no-page",))
        if oB[2] and "PAGE" not in oB[2] and oA[0] != "unsupported":
            problems.append(("B-no-page",))
        if oA[0] in ("invalid", "unsupported"):
            if snapshot(sA) != pre:
                problems.append(("A-modified-on-" + oA[0], tuple(diff_fields(pre, snapshot(sA)))))
            if snapshot(sB) != pre:
                problems.append(("B-modified-on-" + oB[0], tuple(diff_fields(pre, snapshot(sB)))))
        else:
            if oA[1] != oB[1]:
                keys = sorted(set(oA[1]) | set(oB[1]))
                problems.append(("undefined-set", tuple((k, _fmt(oA[1].get(k)), _fmt(oB[1].get(k)))
                                                        for k in keys if oA[1].get(k) != oB[1].get(k))))
            und = dict(oA[1])
            for k, v in oB[1].items():
                if isinstance(v, bool) or isinstance(und.get(k), bool):
                    und[k] = True
                else:
                    und[k] = und.get(k, 0) | v
            dA, dB = snapshot(sA, und), snapshot(sB, und)
            if dA != dB:
                problems.append(("state", tuple(diff_fields(dA, dB))))
    return oA, oB, problems, sA, sB, pre


def _fmt(v):
    if v is None or v is True:
        return v
    return "%08X" % v


def mnemonic_of(w):
    op = w >> 26
    xo = (w >> 1) & 0x3FF
    return {18: "b", 16: "bc", 17: "sc", 3: "twi"}.get(op) or \
        ({16: "bclr", 528: "bcctr", 50: "rfi"}.get(xo) if op == 19 else None) or \
        ({4: "tw", 83: "mfmsr", 146: "mtmsr", 339: "mfspr", 371: "mftb", 467: "mtspr"}.get(xo) if op == 31 else None)


def class_key(name, w, problems):
    sub = name
    if name in ("mfspr", "mftb", "mtspr"):
        # n = spr[5-9] || spr[0-4]; spr[0-4] = instruction bits 11-15 (PAGE 462, PAGE 117 note 1)
        sub = "%s[%d]" % (name, (((w >> 11) & 0x1F) << 5) | ((w >> 16) & 0x1F))
    out = []
    for p in problems:
        if p[0] == "undefined-set":
            out.append(("undefined-set", tuple((k, "A" if a is not None else "-", "B" if b is not None else "-")
                                                for k, a, b in p[1])))
        else:
            out.append(p)
    return (sub, tuple(out))


def record(classes, counts, name, w, base, res):
    oA, oB, problems, sA, sB, pre = res
    if not problems:
        return
    key = class_key(name, w, problems)
    counts[key] += 1
    if key not in classes:
        classes[key] = {
            "word": "%08X" % w, "pre": {k: ("%08X" % pre[k] if isinstance(pre[k], int) else pre[k])
                                        for k in ("msr", "pc", "lr", "ctr", "cr", "srr0", "srr1", "dec", "hid0", "hid2")},
            "problems": problems, "A": (oA[0], {k: _fmt(v) for k, v in oA[1].items()}, oA[2][:160]),
            "B": (oB[0], {k: _fmt(v) for k, v in oB[1].items()}, oB[2][:160]),
            "diff": {f: (_val(sA, f), _val(sB, f)) for f in diff_fields(snapshot(sA), snapshot(sB))},
        }


def _val(st, f):
    m = re.fullmatch(r"(gpr|gqr|sprg)\[(\d+)\]", f)
    if m:
        return "%08X" % getattr(st, m.group(1))[int(m.group(2))]
    if f == "pending":
        return dict(st.pending)
    if f.startswith("mem"):
        return "mem"
    v = getattr(st, f)
    return "%08X" % v if isinstance(v, int) and not isinstance(v, bool) else v


# ------------------------------------------------------------------------------------------
# mutation self-validation: plausible single-point defects applied to the second model's text;
# every mutant must be reported as a disagreement (--mutants)
# ------------------------------------------------------------------------------------------
MUTANTS = [
    ("bc LR only when taken", "    if lk:\n        st.lr = (cia + 4) & M\n\n\ndef _bclr",
     "    if lk and taken:\n        st.lr = (cia + 4) & M\n\n\ndef _bclr"),
    ("ctr_ok ignores BO[3]", "((st.ctr != 0) != bool(bo & 0x02))", "(st.ctr != 0)"),
    ("CTR tested before decrement", "        st.ctr = (st.ctr - 1) & M\n    ctr_ok = bool(bo & 0x04) or ((st.ctr != 0) != bool(bo & 0x02))",
     "        pass\n    ctr_ok = bool(bo & 0x04) or ((st.ctr != 0) != bool(bo & 0x02))\n    if not (bo & 0x04):\n        st.ctr = (st.ctr - 1) & M"),
    ("cond uses BO[4] as sense", "(_cr_bit(st, bi) == ((bo >> 3) & 1))\n    return", "(_cr_bit(st, bi) == (bo & 1))\n    return"),
    ("BI bit order little-endian", "return (st.cr >> (31 - bi)) & 1", "return (st.cr >> bi) & 1"),
    ("bclrl target = new LR", "    target = st.lr & 0xFFFFFFFC                 # PAGE 363",
     "    st.lr = (cia + 4) & M if lk else st.lr\n    target = st.lr & 0xFFFFFFFC                 # PAGE 363"),
    ("bctr keeps low bits", "target = st.ctr & 0xFFFFFFFC", "target = st.ctr"),
    ("b displacement 24-bit sign", "disp = _exts(li, 26)", "disp = _exts(li, 24)"),
    ("bca relative", "st.pc = ((disp if aa else cia + disp) if taken else cia + 4) & M",
     "st.pc = ((cia + disp) if taken else cia + 4) & M"),
    ("twi SIMM zero-extended", "simm = _exts(word & 0xFFFF, 16) & M", "simm = word & 0xFFFF"),
    ("tw TO bits reversed", "((to & 0x10) and sa < sb) or ((to & 0x08) and sa > sb)",
     "((to & 0x01) and sa < sb) or ((to & 0x08) and sa > sb)"),
    ("tw <U signed", "((to & 0x02) and ua < ub)", "((to & 0x02) and sa < sb)"),
    ("rfi restores POW", "new = (old & ~RFI_RESTORE & ~MSR_POW) | (st.srr1 & RFI_RESTORE)",
     "new = (old & ~RFI_RESTORE) | (st.srr1 & RFI_RESTORE)"),
    ("rfi keeps low SRR0 bits", "st.pc = st.srr0 & 0xFFFFFFFC", "st.pc = st.srr0"),
    ("exception LE not from ILE", "    if old & MSR_ILE:\n        new |= MSR_LE\n    st.msr = new",
     "    st.msr = new"),
    ("exception keeps EE", "new = old & (MSR_ILE | MSR_ME | MSR_IP)", "new = old & (MSR_ILE | MSR_ME | MSR_IP | MSR_EE)"),
    ("IP prefix ignored", "st.pc = ((0xFFF00000 if old & MSR_IP else 0) + vector) & M", "st.pc = vector & M"),
    ("sc SRR0 = CIA", "_enter_exception(st, und, cia + 4, VEC_SYSCALL, \"syscall\")",
     "_enter_exception(st, und, cia, VEC_SYSCALL, \"syscall\")"),
    ("mfspr SPR halves swapped", "    return (hi << 5) | lo", "    return (lo << 5) | hi"),
    ("user may write SPRG", "    if access == \"S\" and st.msr & MSR_PR:\n        # PAGE 477",
     "    if False:\n        # PAGE 477"),
    ("mttbl clobbers TBU", "st.tb = (st.tb & ~M) | v", "st.tb = v"),
    ("mtmsr ignores PR", "    if st.msr & MSR_PR:  # PAGE 475", "    if False:  # PAGE 475"),
    ("SE trace exempts nothing", "name not in (\"rfi\", \"sc\")", "True"),
    ("BE traces non-branches", "pre_msr & MSR_BE and name in _BRANCHES", "pre_msr & MSR_BE"),
    ("mtspr HID2 writes DMAQL", "(v & ~HID2_DMAQL) | (old & HID2_DMAQL)", "v"),
    # the reconciled disagreement classes themselves must stay visible
    ("XCHK1 trace SRR0 = CIA", "_enter_exception(work, und, work.pc, VEC_TRACE, \"trace\")",
     "_enter_exception(work, und, state.pc, VEC_TRACE, \"trace\")"),
    ("XCHK2 program SRR0 = CIA+4", "_enter_exception(st, und, cia, VEC_PROGRAM, \"program\")",
     "_enter_exception(st, und, cia + 4, VEC_PROGRAM, \"program\")"),
    ("XCHK3 trap named program", "_enter_exception(st, und, cia + 4, VEC_PROGRAM, \"trap\", srr0_mask=M)",
     "_enter_exception(st, und, cia + 4, VEC_PROGRAM, \"program\", srr0_mask=M)"),
    ("XCHK5 mtspr PVR illegal", "_spr(287, \"PVR\", \"S\", \"S\")", "_spr(287, \"PVR\", \"S\", None)"),
    ("XCHK7 DEC write sets pending", "            und[\"pending.decrementer\"] = True\n        elif st.pending",
     "            st.pending[\"decrementer\"] = True\n        elif st.pending"),
    ("XCHK8 mfxer read fully defined", "_und_or(und, key, val & XER_UNCERTAIN)", "pass"),
    ("XCHK13 mtmsr SE toggle traced", "    if (old ^ v) & MSR_SE:\n        # XCHK", "    if False:\n        # XCHK"),
]


def load_model(spec):
    if "@" not in spec:
        return importlib.import_module(spec)
    base, idx = spec.split("@")
    key = "%s_mut%s" % (base, idx)
    if key in sys.modules:
        return sys.modules[key]
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
# sweeps
# ------------------------------------------------------------------------------------------
def worker(args):
    seed, count, modA, modB = args
    mA, mB = load_model(modA), load_model(modB)
    r = random.Random(seed)
    classes, counts, outcomes = collections.OrderedDict(), collections.Counter(), collections.Counter()
    for _ in range(count):
        name = r.choice(NAMES)
        w = gen_word(r, name) & M
        base = gen_state(r, name, w)
        res = compare(mA, mB, base, w)
        outcomes[(name, res[0][0])] += 1
        record(classes, counts, name, w, base, res)
    return classes, counts, outcomes


def edge_vectors(modA, modB):
    mA, mB = load_model(modA), load_model(modB)
    classes, counts, outcomes = collections.OrderedDict(), collections.Counter(), collections.Counter()
    n = 0

    def go(name, w, st):
        nonlocal n
        n += 1
        res = compare(mA, mB, st, w & M)
        outcomes[(name, res[0][0])] += 1
        record(classes, counts, name, w & M, st, res)

    msr_modes = (0x2032, 0x2032 | BE, 0x2032 | SE, 0x2032 | PR, 0x2032 | IP | ILE, 0x00012032 | BE | SE | LE)
    # bc / bclr / bcctr: every BO x BI 0/2/31 x CR bit value x CTR boundary x AA/LK x trace/prefix modes
    for bo in range(32):
        for bi in (0, 2, 31):
            for crv in (0, 1):
                for ctr in (0, 1, 2, M, 0x80000003):
                    for aalk in range(4):
                        for msr in msr_modes:
                            st = State()
                            st.msr, st.ctr, st.lr = msr, ctr, 0x80001237
                            st.cr = (1 << (31 - bi)) if crv else (~(1 << (31 - bi)) & M)
                            st.pc = 0x80003100 if aalk & 1 == 0 else 0xFFFFFFFC
                            for bd in (0x0010, 0x8000 >> 2 << 2, 0x7FFC):
                                go("bc", (16 << 26) | (bo << 21) | (bi << 16) | bd | aalk, st.clone())
                            go("bclr", (19 << 26) | (bo << 21) | (bi << 16) | (16 << 1) | (aalk & 1), st.clone())
                            go("bcctr", (19 << 26) | (bo << 21) | (bi << 16) | (528 << 1) | (aalk & 1), st.clone())
    # b: LI extremes x AA/LK x pc extremes x trace modes
    for li in (0, 1, 0x7FFFFF, 0x800000, 0xFFFFFF, 0x000100):
        for aalk in range(4):
            for pc in (0, 0x80003100, 0xFFFFFFFC):
                for msr in msr_modes:
                    st = State()
                    st.msr, st.pc = msr, pc
                    go("b", (18 << 26) | (li << 2) | aalk, st)
    # tw / twi: every TO x boundary operand pairs x aliasing
    vals = (0, 1, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0xFFFF8000, 0x00007FFF, 0x00008000)
    for to in range(32):
        for va in vals:
            for vb in vals:
                for msr in (0x2032, 0x2032 | SE, 0x2032 | PR | IP):
                    st = State()
                    st.msr, st.gpr[3], st.gpr[4] = msr, va, vb
                    go("tw", (31 << 26) | (to << 21) | (3 << 16) | (4 << 11) | (4 << 1), st.clone())
                    st2 = st.clone()
                    st2.gpr[3] = va
                    go("tw", (31 << 26) | (to << 21) | (3 << 16) | (3 << 11) | (4 << 1), st2)
                    go("twi", (3 << 26) | (to << 21) | (3 << 16) | (vb & 0xFFFF), st.clone())
    # every SPR number x mfspr/mftb/mtspr x PR x Rc x SE, value with reserved bits
    for spr in range(1024):
        for xo, name in ((339, "mfspr"), (371, "mftb"), (467, "mtspr")):
            for msr in (0x2032, 0x2032 | PR, 0x2032 | SE):
                for rc in (0, 1):
                    for val in (0xFFFFFFFF, 0x80000000, 0x00000000):
                        st = State()
                        st.msr, st.gpr[5] = msr, val
                        st.dec, st.xer, st.hid0, st.hid2 = 0x10, 0xE000FF7F | 0x10000000, 0x0011C004, 0xE0800000
                        st.gqr = [0xFFFFFFFF] * 8
                        go(name, (31 << 26) | (5 << 21) | spr_split(spr) | (xo << 1) | rc, st)
    # mtmsr / mfmsr: single-bit toggles of every MSR bit from three base modes
    for base in (0x2032, 0x2032 | SE, 0x2032 | PR):
        for bit in range(32):
            for hid0 in (0, _b(8)):
                st = State()
                st.msr, st.hid0 = base, hid0
                st.gpr[6] = base ^ _b(bit)
                go("mtmsr", 0x7CC00124, st.clone())
                st.msr = base ^ _b(bit)
                go("mfmsr", 0x7CA000A6, st)
    # rfi: every SRR1 bit toggled against three MSR modes; sc in the same modes
    for base in (0x1000, 0x2032 | SE, 0x2032 | PR, 0x2032 | IP | ILE):
        for bit in range(32):
            st = State()
            st.msr, st.srr1, st.srr0 = base, base ^ _b(bit), 0x80001237
            go("rfi", 0x4C000064, st.clone())
            st.msr = base ^ _b(bit)
            go("sc", 0x44000002, st.clone())
            go("twi", 0x0FE00000, st.clone())
            go("mfspr", 0x7C0802A6 | 0, st.clone())
    return n, classes, counts, outcomes


def decode_sweep(mA, mB, seed):
    r = random.Random(seed ^ 0x5EED)
    n, bad = 0, []
    for op in range(64):
        for _ in range(96):
            w = (op << 26) | r.getrandbits(26)
            n += 1
            if mA.supports(w) != mB.supports(w):
                bad.append("%08X A=%s B=%s" % (w, mA.supports(w), mB.supports(w)))
    for op in (19, 31):
        for xo in range(1024):
            for variant in range(6):
                w = (op << 26) | (xo << 1) | ((r.getrandbits(15) << 11) if variant else 0) | (variant & 1)
                n += 1
                if mA.supports(w) != mB.supports(w):
                    bad.append("%08X A=%s B=%s" % (w, mA.supports(w), mB.supports(w)))
    return n, bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--n", type=int, default=240000)
    ap.add_argument("--seed", type=int, default=20260914)
    ap.add_argument("--jobs", type=int, default=max(1, min(16, (os.cpu_count() or 2) - 2)))
    ap.add_argument("--models", default="branch_A,branch_B")
    ap.add_argument("--show", type=int, default=2)
    ap.add_argument("--no-edge", action="store_true")
    ap.add_argument("--mutants", action="store_true")
    a = ap.parse_args()
    modA, modB = a.models.split(",")

    if a.mutants:
        missed = 0
        for i, (desc, _, _) in enumerate(MUTANTS):
            spec = "%s@%d" % (modB, i)
            per = max(1, a.n // a.jobs)
            with mp.Pool(a.jobs) as pool:
                res = pool.map(worker, [(a.seed * 31 + j, per, modA, spec) for j in range(a.jobs)])
            nbad = sum(sum(ct.values()) for _, ct, _ in res)
            ne = 0
            if nbad == 0 and not a.no_edge:
                _, _, ect, _ = edge_vectors(modA, spec)
                ne = sum(ect.values())
            print("mutant %2d %-32s detected by %d random vectors (+%d edge)" % (i, desc, nbad, ne))
            missed += (nbad + ne) == 0
        print("MUTANTS missed: %d of %d" % (missed, len(MUTANTS)))
        return 1 if missed else 0

    mA, mB = load_model(modA), load_model(modB)
    nd, bad = decode_sweep(mA, mB, a.seed)
    print("decode sweep: %d words, %d supports() disagreements" % (nd, len(bad)))
    for x in bad[:10]:
        print("   ", x)

    outcomes = collections.Counter()
    ne, ecl, ect = 0, {}, collections.Counter()
    if not a.no_edge:
        ne, ecl, ect, eoc = edge_vectors(modA, modB)
        outcomes.update(eoc)
        print("edge sweep: %d vectors, %d disagreeing, %d classes" % (ne, sum(ect.values()), len(ecl)))
        _print_classes(ecl, ect, a.show)

    per = a.n // a.jobs
    tasks = [(a.seed * 7919 + j, per + (1 if j < a.n % a.jobs else 0), modA, modB) for j in range(a.jobs)]
    if a.jobs == 1:
        results = [worker(tasks[0])]
    else:
        with mp.Pool(a.jobs) as pool:
            results = pool.map(worker, tasks)
    classes, counts = collections.OrderedDict(), collections.Counter()
    for cl, ct, oc in results:
        for k, v in cl.items():
            classes.setdefault(k, v)
        counts.update(ct)
        outcomes.update(oc)
    total_bad = sum(counts.values())
    print("random sweep: %d vectors, %d disagreeing, %d classes" % (a.n, total_bad, len(classes)))
    _print_classes(classes, counts, a.show)
    print("outcome coverage per mnemonic (ok/partial/invalid/unsupported):")
    for nm in NAMES:
        print("   %-6s %7d / %7d / %6d / %6d" % (nm, outcomes[(nm, "ok")], outcomes[(nm, "partial")],
                                               outcomes[(nm, "invalid")], outcomes[(nm, "unsupported")]))
    dis = total_bad + sum(ect.values()) + len(bad)
    print("TOTAL vectors %d (decode %d + edge %d + random %d); disagreements %d" % (nd + ne + a.n, nd, ne, a.n, dis))
    return 0 if dis == 0 else 1


def _print_classes(classes, counts, show):
    by = collections.defaultdict(list)
    for k in classes:
        # group mfspr[n] classes by problem signature to keep the listing short
        sig = (re.sub(r"\[\d+\]", "", k[0]), k[1])
        by[sig].append(k)
    for sig in sorted(by, key=lambda s: (s[0], str(s[1]))):
        ks = by[sig]
        tot = sum(counts[k] for k in ks)
        sprs = sorted({int(re.search(r"\[(\d+)\]", k[0]).group(1)) for k in ks if "[" in k[0]})
        print("  [%d] %s %s%s" % (tot, sig[0], sig[1], (" SPRs=%s" % _ranges(sprs)) if sprs else ""))
        for k in ks[:show]:
            print("       e.g. %s" % classes[k])


def _ranges(ns):
    out, i = [], 0
    while i < len(ns):
        j = i
        while j + 1 < len(ns) and ns[j + 1] == ns[j] + 1:
            j += 1
        out.append(str(ns[i]) if i == j else "%d-%d" % (ns[i], ns[j]))
        i = j + 1
    return ",".join(out)


if __name__ == "__main__":
    sys.exit(main())
