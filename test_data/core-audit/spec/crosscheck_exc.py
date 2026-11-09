"""Differential cross-check of the two independent "exc" specification models.

    python crosscheck_exc.py [--n N] [--seed S] [--jobs J] [--models exc_A,exc_B] [--show K]

Draws N vectors over every public operation both models expose, runs both models from
identical cloned states and compares the full State.arch_dict(), mem.log/mem.bytes, the
extra attributes the exception area owns (checkstop, iabr) and state.pending (normalised), with
the locations each model declares undefined masked out of BOTH states.

Families (weights in FAMILIES below):
  step      rfi / sc / tw / twi / mtmsr / mfmsr: clean and reserved-bit-dirty encodings, aliasing
            rA == rB, boundary operands, every MSR bit class (SE/BE/PR/PM/POW/ILE/LE/IP/EE/ME,
            reserved full/partial/unclassified), SRR1 vs MSR PM mismatch, IABR matching the
            instruction or the next one, checkstopped state
  pending   take_pending over every combination of hard/soft reset, machine check causes (with
            HID0 EMCP/EBA/EBD), external, performance monitor, decrementer, thermal, smi, EE, ME,
            checkstop
  helper    every exception-entry helper (external .. hard reset) with random MSR / pc / EA / cause
  timer     TB/DEC advance over DEC and tick boundaries (0, 1, old, old+1, 2**32 ...)
  bus       bus-clock prescaler
  wdec      DEC writes (MSB 0->1, latch pending)
  trace     post-instruction trace decision over every branch / rfi / isync / sc / other kind
  supports  (separate sweep) supports() agreement over the whole opcode space

UB contracts being normalised:
  A contract (exc_A, exc): the operation raises UndefinedBehaviour WITHOUT touching the state;
      u.undefined == {'all': ..} for an invalid form, else u.defined holds every defined part and
      u.undefined = {location: mask}.  A mask on 'pc' or 'exception' means control flow is
      undefined -> class 'control'.
  B contract (exc_B): strict (partial=False) raises before touching the state; partial=True
      completes with state.spec_undefined = {location: mask} for register contents, or still
      raises for control-flow ambiguity -> class 'control'.
Outcome classes compared: ok / partial(masks) / control.  In addition every raise is checked
for atomicity (state untouched) and B strict vs B partial must agree.
"""
from __future__ import annotations

import argparse
import collections
import importlib
import multiprocessing as mp
import os
import random
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from gekko_state import MASK32, MASK64, State  # noqa: E402

M = MASK32


def B(n):
    return 1 << (31 - n)


# MSR bits (Table 4-4 PAGE 164-165), written here independently of both models
POW, ILE, EE, PR, FP, ME, FE0, SE, BE, FE1 = B(13), B(15), B(16), B(17), B(18), B(19), B(20), B(21), B(22), B(23)
IP, IR, DR, PM, RI, LE = B(25), B(26), B(27), B(29), B(30), B(31)
RESERVED_BITS = (0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 14, 24, 28)
NAMED = (POW, ILE, EE, PR, FP, ME, FE0, SE, BE, FE1, IP, IR, DR, PM, RI, LE)
MSR_BASES = (0x2032, 0xA032, 0xB032, 0x9032, 0x0000, 0x0040, 0x1032, 0x3032, 0xE032, 0xFF73,
             0xFF33, 0x00013032, 0x0001B072, 0x8000B032, 0x0000A036, 0x0000A432, 0x0000A232)
BOUND32 = (0, 1, 2, 3, 4, 0x7FFF, 0x8000, 0xFFFF, 0x10000, 0x7FFFFFFF, 0x80000000, 0x80000001,
           0xFFFFFFFF, 0xFFFFFFFE, 0xFFFF8000, 0xFFFF7FFF, 0x12345678, 0xDEADBEEF, 0x55555555,
           0xAAAAAAAA, 0x80003100, 0xFFFFFFFC, 0x7FFFFFFC, 0x00000500)
SIMM16 = (0, 1, 2, 0x7FFF, 0x8000, 0x8001, 0xFFFF, 0xFFFE, 0x00FF, 0xFF00)
PCS = (0x80003100, 0x00000000, 0xFFFFFFFC, 0x7FFFFFFC, 0x80000000, 0xFFF00100, 0x00000500,
       0x0000FFFC, 0x80001000)
MC_NAMES_A = ("dma", "l2dp", "mcp", "tea", "dp", "ap")
MC_A_TO_B = {"dma": "dma", "l2dp": "l2", "mcp": "mcp", "tea": "tea", "dp": "data_parity",
             "ap": "address_parity"}
MC_B_TO_A = {v: k for k, v in MC_A_TO_B.items()}
HID0_EMCP, HID0_EBA, HID0_EBD = B(0), B(2), B(3)
DSI_A_TO_B = {"page_fault": "page_fault", "protection": "protection",
              "direct_store": "direct_store", "eciwx": "ear"}
ISI_CAUSES = ("page_fault", "protection", "no_execute", "direct_store", "guarded")
PROGRAM_KINDS = ("illegal", "privileged", "trap", "fp_enabled")
PENDING_KEYS = ("hard_reset", "soft_reset", "external", "performance_monitor", "decrementer",
                "thermal", "smi")


# ------------------------------------------------------------------------------------------
# random draws
# ------------------------------------------------------------------------------------------
def rand32(r):
    k = r.random()
    if k < 0.45:
        return r.choice(BOUND32)
    if k < 0.55:
        return r.choice(BOUND32) ^ (1 << r.randrange(32))
    return r.getrandbits(32)


def gen_msr(r, ee_bias=0.0):
    k = r.random()
    if k < 0.25:
        m = r.choice(MSR_BASES)
    elif k < 0.45:
        m = r.getrandbits(16) & ~(B(24) | B(28))
    elif k < 0.52:
        m = r.getrandbits(32)
    else:
        m = r.choice(MSR_BASES)
        for bit in NAMED:
            if r.random() < 0.12:
                m ^= bit
    if r.random() < 0.08:
        m |= B(r.choice(RESERVED_BITS))
    if ee_bias and r.random() < ee_bias:
        m |= EE
    return m & M


def gen_srr1(r, msr):
    k = r.random()
    if k < 0.3:
        return msr
    if k < 0.45:
        return msr ^ r.choice(NAMED)
    if k < 0.55:
        return msr ^ PM
    if k < 0.7:
        return gen_msr(r)
    if k < 0.8:
        return r.getrandbits(32)
    return r.choice(MSR_BASES)


def gen_pc(r):
    k = r.random()
    if k < 0.4:
        return r.choice(PCS)
    if k < 0.97:
        return r.getrandbits(32) & ~3
    return r.getrandbits(32)                       # unaligned pc (rare)


def gen_iabr(r, st):
    k = r.random()
    if k < 0.45:
        return None
    if k < 0.55:
        return 0
    if k < 0.62:
        return r.getrandbits(32)
    target = st.pc if k < 0.82 else (st.pc + 4) & M
    v = (target & ~3) | B(30)
    if st.msr & IR:
        v |= B(31)
    q = r.random()
    if q < 0.1:
        v ^= B(31)                                  # TE mismatch
    elif q < 0.2:
        v &= ~B(30)                                 # BE = 0
    elif q < 0.3:
        v ^= B(r.randrange(30))                     # address mismatch
    return v & M


def gen_state(r, ee_bias=0.0):
    st = State()
    for i in range(32):
        st.gpr[i] = rand32(r)
        st.fpr[i] = [r.getrandbits(64), r.getrandbits(64)]
    st.cr = r.getrandbits(32)
    st.xer = r.getrandbits(32)
    st.lr = rand32(r)
    st.ctr = rand32(r)
    st.fpscr = r.choice((0, 0xFFFFFFFF, 0x000000F8, 0x00000003, r.getrandbits(32)))
    st.msr = gen_msr(r, ee_bias)
    st.srr0 = rand32(r)
    st.srr1 = gen_srr1(r, st.msr)
    st.dar = rand32(r)
    st.dsisr = rand32(r)
    st.dec = rand32(r)
    st.tb = r.choice((0, MASK64, (1 << 32) - 1, r.getrandbits(64)))
    st.hid0 = r.choice((0, HID0_EMCP, HID0_EMCP | HID0_EBA | HID0_EBD, r.getrandbits(4) << 28,
                        r.getrandbits(32)))
    st.hid2 = r.getrandbits(32)
    st.gqr = [r.getrandbits(32) for _ in range(8)]
    st.sprg = [r.getrandbits(32) for _ in range(4)]
    st.pc = gen_pc(r)
    st.reserve = r.random() < 0.5
    st.reserve_addr = r.getrandbits(32)
    st.pending = {"external": r.random() < 0.3, "decrementer": r.random() < 0.3}
    st.exception = r.choice((None, None, "external", "program", "trace"))
    iabr = gen_iabr(r, st)
    if iabr is not None:
        st.iabr = iabr
    k = r.random()
    if k < 0.03:
        st.checkstop = True
    elif k < 0.08:
        st.checkstop = False
    if r.random() < 0.05:
        st.mem.load(st.pc & ~3, bytes((r.getrandbits(8) for _ in range(8))))
    return st


def enc_x31(a, b, c, xo, rc=0):
    return ((31 << 26) | (a << 21) | (b << 16) | (c << 11) | (xo << 1) | rc) & M


def gen_step(r):
    st = gen_state(r, ee_bias=0.2)
    name = r.choice(("rfi", "sc", "tw", "twi", "mtmsr", "mfmsr"))
    dirty = r.random() < 0.1
    if name == "rfi":
        w = 0x4C000064
        if dirty:
            w |= (r.getrandbits(15) << 11) | r.getrandbits(1)
    elif name == "sc":
        w = 0x44000002
        if dirty:
            w |= (r.getrandbits(24) << 2) | r.getrandbits(1)
            if r.random() < 0.3:
                w &= ~2
    elif name in ("tw", "twi"):
        to = r.choice((0, 31, 0x10, 0x08, 0x04, 0x02, 0x01, 4, 5, 24, 6, 12, r.getrandbits(5)))
        ra = r.choice((0, 3, 4, 5)) if r.random() < 0.5 else r.randrange(32)
        if name == "tw":
            rb = ra if r.random() < 0.3 else r.randrange(32)
            w = enc_x31(to, ra, rb, 4, 1 if dirty else 0)
            rel = r.random()
            if rb != ra:
                if rel < 0.2:
                    st.gpr[rb] = st.gpr[ra]
                elif rel < 0.35:
                    st.gpr[rb] = (st.gpr[ra] + r.choice((1, -1))) & M
                elif rel < 0.45:
                    st.gpr[ra], st.gpr[rb] = r.choice(((0x7FFFFFFF, 0x80000000),
                                                       (0x80000000, 0x7FFFFFFF),
                                                       (0xFFFFFFFF, 0), (0, 0xFFFFFFFF)))
        else:
            simm = r.choice(SIMM16) if r.random() < 0.6 else r.getrandbits(16)
            w = ((3 << 26) | (to << 21) | (ra << 16) | simm) & M
            ext = (simm - 0x10000 if simm & 0x8000 else simm) & M
            rel = r.random()
            if rel < 0.25:
                st.gpr[ra] = ext
            elif rel < 0.4:
                st.gpr[ra] = (ext + r.choice((1, -1))) & M
    else:
        rs = r.randrange(32)
        xo = 146 if name == "mtmsr" else 83
        w = enc_x31(rs, 0, 0, xo)
        if dirty:
            w |= (r.getrandbits(10) << 11) | r.getrandbits(1)
        if name == "mtmsr":
            k = r.random()
            if k < 0.15:
                v = st.msr
            elif k < 0.5:
                v = st.msr ^ r.choice((EE, PR, FP, ME, FE0, FE1, IR, DR, RI, IP, ILE, PM))
            elif k < 0.65:
                v = st.msr ^ r.choice((SE, BE, POW, LE))
            elif k < 0.75:
                v = st.msr | B(r.choice(RESERVED_BITS))
            elif k < 0.85:
                v = gen_msr(r)
            else:
                v = r.getrandbits(32)
            st.gpr[rs] = v & M
    return ("step", name, st, (w,))


def gen_pending(r):
    st = gen_state(r, ee_bias=0.5)
    p = {}
    for k, prob in (("hard_reset", 0.04), ("soft_reset", 0.12), ("external", 0.35),
                    ("performance_monitor", 0.2), ("decrementer", 0.35), ("thermal", 0.2),
                    ("smi", 0.03)):
        v = r.random() < prob
        if v or r.random() < 0.5:
            p[k] = v
    if r.random() < 0.18:
        n = r.randrange(1, 4)
        p["machine_check"] = tuple(r.sample(MC_NAMES_A, n))
    elif r.random() < 0.3:
        p["machine_check"] = ()
    st.pending = p
    if r.random() < 0.3:
        st.msr |= ME
    return ("pending", "take_pending", st, ())


HELPERS = ("external", "decrementer", "performance_monitor", "thermal", "system_reset", "iabr",
           "fp_unavailable", "syscall", "alignment", "dsi", "isi", "program", "trace",
           "machine_check", "hard_reset")


def gen_helper(r):
    st = gen_state(r)
    h = r.choice(HELPERS)
    args = ()
    if h in ("alignment",):
        args = (rand32(r),)
    elif h == "dsi":
        args = (rand32(r), tuple(r.sample(tuple(DSI_A_TO_B), r.randrange(1, 3))))
    elif h == "isi":
        args = (r.choice(ISI_CAUSES),)
    elif h == "program":
        args = (r.choice(PROGRAM_KINDS),)
    elif h == "trace":
        args = (r.choice(((st.pc + 4) & M, rand32(r) & ~3, rand32(r))),)
    elif h == "machine_check":
        st.msr |= ME
        st.hid0 |= HID0_EMCP | HID0_EBA | HID0_EBD
        args = (tuple(r.sample(MC_NAMES_A, r.randrange(1, 4))),)
    return ("helper", h, st, args)


DEC_B = (0, 1, 2, 0x7FFFFFFF, 0x80000000, 0x80000001, 0xFFFFFFFF, 0xFFFFFFFE, 5, 100)


def gen_timer(r):
    st = gen_state(r)
    st.dec = r.choice(DEC_B) if r.random() < 0.6 else r.getrandbits(32)
    st.tb = r.choice((0, MASK64, MASK64 - 1, (1 << 32) - 1, r.getrandbits(64)))
    st.pending = {"decrementer": r.random() < 0.2, "external": r.random() < 0.2}
    d = st.dec
    ticks = r.choice((0, 1, 2, d, d + 1, max(d - 1, 0), 1 << 32, (1 << 32) + d, (1 << 32) + d + 1,
                      (1 << 33) + 5, r.randrange(1 << 20), r.randrange(1 << 36)))
    return ("timer", "advance", st, (ticks,))


def gen_bus(r):
    st = gen_state(r)
    st.dec = r.choice(DEC_B) if r.random() < 0.6 else r.getrandbits(32)
    st.pending = {"decrementer": r.random() < 0.2}
    n = r.choice((0, 1, 2, 3, 4, 5, 6, 7, 8, 4 * r.randrange(1 << 10), r.randrange(1 << 12),
                  4 * (st.dec + 1)))
    return ("bus", "bus_clocks", st, (n,))


def gen_wdec(r):
    st = gen_state(r)
    st.dec = r.choice(DEC_B) if r.random() < 0.6 else r.getrandbits(32)
    st.pending = {"decrementer": r.random() < 0.2}
    v = r.choice(DEC_B) if r.random() < 0.6 else r.getrandbits(32)
    return ("wdec", "write_dec", st, (v,))


def gen_trace(r):
    st = gen_state(r)
    if hasattr(st, "iabr"):
        del st.iabr
    st.checkstop = False
    kind = r.choice(("b", "bc", "bclr", "bcctr", "rfi", "isync", "sc", "addi", "x31", "op19"))
    if kind == "b":
        w = (18 << 26) | r.getrandbits(26)
    elif kind == "bc":
        w = (16 << 26) | r.getrandbits(26)
    elif kind == "bclr":
        w = (19 << 26) | (r.getrandbits(15) << 11) | (16 << 1) | r.getrandbits(1)
    elif kind == "bcctr":
        w = (19 << 26) | (r.getrandbits(15) << 11) | (528 << 1) | r.getrandbits(1)
    elif kind == "rfi":
        w = 0x4C000064
    elif kind == "isync":
        w = 0x4C00012C
    elif kind == "sc":
        w = 0x44000002
    elif kind == "addi":
        w = (14 << 26) | r.getrandbits(26)
    elif kind == "x31":
        w = (31 << 26) | r.getrandbits(26)
    else:
        w = (19 << 26) | r.getrandbits(26)
    m = st.msr
    for bit in (SE, BE):
        if r.random() < 0.5:
            m ^= bit
    st.msr = m & M
    return ("trace", kind, st, (w & M, st.pc))


def harness_trace_kind(word):
    # Table 2-46 PAGE 113: b, bc, bclr, bcctr are the branch instructions; rfi/isync/sc named on
    # PAGE 164 / PAGE 176.
    op = word >> 26
    xo = (word >> 1) & 0x3FF
    if op in (16, 18) or (op == 19 and xo in (16, 528)):
        return "branch"
    if op == 19 and xo == 50:
        return "rfi"
    if op == 19 and xo == 150:
        return "isync"
    if op == 17:
        return "sc"
    return "other"


FAMILIES = ((gen_step, 45), (gen_pending, 25), (gen_helper, 15), (gen_timer, 6), (gen_bus, 3),
            (gen_wdec, 3), (gen_trace, 3))
_CUM = []
_t = 0
for _g, _w in FAMILIES:
    _t += _w
    _CUM.append((_t, _g))


def gen_vector(seed, idx):
    r = random.Random(seed * 1000003 + idx)
    x = r.randrange(_t)
    for lim, g in _CUM:
        if x < lim:
            return g(r)
    raise AssertionError


# ------------------------------------------------------------------------------------------
# normalisation
# ------------------------------------------------------------------------------------------
def norm_pending(p, b_names=False):
    out = {}
    for k, v in (p or {}).items():
        if k == "machine_check":
            if v is True:
                out[k] = "True"
            elif v:
                names = set(v)
                if b_names:
                    names = {MC_B_TO_A.get(n, n) for n in names}
                out[k] = tuple(sorted(names))
        elif v:
            out[k] = True
    return out


def snapshot(st, b_names=False, with_pending=True):
    d = {k: (list(map(list, v)) if k == "fpr" else (list(v) if isinstance(v, list) else v))
         for k, v in st.arch_dict().items()}
    d["checkstop"] = bool(getattr(st, "checkstop", False))
    d["iabr"] = getattr(st, "iabr", 0) & M
    d["mem.log"] = list(st.mem.log) if st.mem is not None else None
    d["mem.bytes"] = dict(st.mem.bytes) if st.mem is not None else None
    if with_pending:
        d["pending"] = norm_pending(st.pending, b_names)
    return d


def raw_snapshot(st):
    d = snapshot(st, with_pending=False)
    d["pending.raw"] = repr(sorted((k, repr(v)) for k, v in (st.pending or {}).items()))
    d["spec_undefined"] = repr(getattr(st, "spec_undefined", None))
    return d


def apply_masks(snap, masks):
    s = dict(snap)
    for loc, mask in masks.items():
        if loc == "gpr":
            s["gpr"] = [v & ~mask & M for v in s["gpr"]]
        elif loc == "fpr":
            s["fpr"] = [[a & ~mask & MASK64, b & ~mask & MASK64] for a, b in s["fpr"]]
        elif loc.startswith("pending."):
            key = loc.split(".", 1)[1]
            p = dict(s["pending"])
            p.pop(key, None)
            s["pending"] = p
        elif loc in s and isinstance(s[loc], int) and not isinstance(s[loc], bool):
            s[loc] = s[loc] & ~mask
        else:
            s[loc] = "<undefined>"
    return s


class Res:
    __slots__ = ("cls", "snap", "masks", "ret", "notes")

    def __init__(self, cls, snap=None, masks=None, ret=None, notes=()):
        self.cls, self.snap, self.masks, self.ret, self.notes = cls, snap, masks or {}, ret, list(notes)


# ------------------------------------------------------------------------------------------
# model adapters
# ------------------------------------------------------------------------------------------
def a_call(mod, family, name, s, args):
    if family == "step":
        return mod.step(s, *args)
    if family == "pending":
        return mod.take_pending(s)
    if family == "helper":
        return getattr(mod, "take_" + name)(s, *args)
    if family == "timer":
        return bool(mod.advance_timebase(s, *args))
    if family == "bus":
        return bool(mod.advance_timebase(s, mod.bus_clocks_to_ticks(args[0])))
    if family == "wdec":
        return mod.write_dec(s, *args)
    if family == "trace":
        word, next_pc = args
        if mod.trace_condition(s.msr, harness_trace_kind(word)):
            mod.take_trace(s, next_pc)
            return True
        return False
    raise AssertionError(family)


def run_a(mod, family, name, st, args):
    if hasattr(mod, "STRICT"):
        mod.STRICT = True
    s = st.clone()
    before = raw_snapshot(s)
    try:
        ret = a_call(mod, family, name, s, args)
        return Res("ok", snapshot(s), {}, ret)
    except mod.UndefinedBehaviour as u:
        notes = []
        if raw_snapshot(s) != before:
            notes.append("A:raise-not-atomic")
        und = {k: v for k, v in u.undefined.items() if v}
        # timer / bus / DEC-write helpers have no partial contract in B: any raise is compared as
        # "the operation is undefined" (A's write_dec raises with a pending.decrementer mark).
        if (family in ("timer", "bus", "wdec") or "all" in und or "pc" in und
                or "exception" in und or u.defined is None):
            return Res("control", notes=notes)
        return Res("partial", snapshot(u.defined), und, None, notes)


def b_call(mod, family, name, s, args, partial):
    if family == "step":
        return mod.step(s, *args, partial=partial)
    if family == "pending":
        mc = s.pending.get("machine_check")
        if mc:
            s.pending["machine_check"] = {MC_A_TO_B[c] for c in mc}
        return mod.take_pending(s, partial=partial)
    if family == "helper":
        if name == "hard_reset":
            return mod.hard_reset(s, partial=partial)
        fn = getattr(mod, "enter_" + name)
        if name == "dsi":
            return fn(s, args[0], tuple(DSI_A_TO_B[c] for c in args[1]), partial=partial)
        if name == "isi":
            return fn(s, s.pc, args[0], partial=partial)
        if name == "machine_check":
            return fn(s, {MC_A_TO_B[c] for c in args[0]}, partial=partial)
        return fn(s, *args, partial=partial)
    if family == "timer":
        return bool(mod.timer_advance(s, *args))
    if family == "bus":
        return bool(mod.advance_bus_clocks(s, *args))
    if family == "wdec":
        return mod.write_dec(s, *args)
    if family == "trace":
        return bool(mod.trace_after(s, args[0], args[1], partial=partial))
    raise AssertionError(family)


def run_b(mod, family, name, st, args):
    notes = []
    s1 = st.clone()
    before = raw_snapshot(s1)
    strict_raised = False
    try:
        ret1 = b_call(mod, family, name, s1, args, False)
    except mod.UndefinedBehaviour:
        strict_raised = True
        after = raw_snapshot(s1)
        # B converts machine-check cause names in place before the call (harness adapter);
        # compare everything but pending for atomicity of the model itself.
        after.pop("pending.raw"), before.pop("pending.raw")
        if after != before:
            notes.append("B:strict-raise-not-atomic")
    if family in ("timer", "bus", "wdec"):
        if strict_raised:
            return Res("control", notes=notes)
        return Res("ok", snapshot(s1, b_names=True), {}, ret1, notes)
    s2 = st.clone()
    try:
        ret = b_call(mod, family, name, s2, args, True)
    except mod.UndefinedBehaviour:
        if not strict_raised:
            notes.append("B:partial-raises-but-strict-ok")
        return Res("control", notes=notes)
    masks = {k: v for k, v in (getattr(s2, "spec_undefined", None) or {}).items() if v}
    cls = "partial" if masks else "ok"
    if strict_raised != (cls != "ok"):
        notes.append("B:strict-raise=%s but partial marks=%r" % (strict_raised, masks))
    if not strict_raised and snapshot(s1, b_names=True) != snapshot(s2, b_names=True):
        notes.append("B:strict-vs-partial-state-differs")
    return Res(cls, snapshot(s2, b_names=True), masks, ret, notes)


# ------------------------------------------------------------------------------------------
# comparison
# ------------------------------------------------------------------------------------------
def compare(family, name, ra, rb):
    """Return list of (class_key, detail)."""
    out = []
    tag = "%s:%s" % (family, name)
    for n in ra.notes + rb.notes:
        out.append((tag + ":" + n.split(" ")[0], n))
    if ra.cls != rb.cls:
        out.append((tag + ":class A=%s B=%s" % (ra.cls, rb.cls),
                    "A masks=%r B masks=%r" % ({k: hex(v) for k, v in ra.masks.items()},
                                               {k: hex(v) for k, v in rb.masks.items()})))
        return out
    if ra.cls == "control":
        return out
    ma = {k: v for k, v in ra.masks.items()}
    mb = {k: v for k, v in rb.masks.items()}
    if ma != mb:
        locs = sorted(set(ma) | set(mb))
        diff = [l for l in locs if ma.get(l, 0) != mb.get(l, 0)]
        out.append((tag + ":masks[" + ",".join(diff) + "]",
                    " ".join("%s A=%s B=%s" % (l, hex(ma.get(l, 0)), hex(mb.get(l, 0))) for l in diff)))
    union = dict(ma)
    for k, v in mb.items():
        union[k] = union.get(k, 0) | v
    sa = apply_masks(ra.snap, union)
    sb = apply_masks(rb.snap, union)
    if family == "helper":
        sa.pop("pending", None)
        sb.pop("pending", None)
    for k in sorted(set(sa) | set(sb)):
        if sa.get(k) != sb.get(k):
            va, vb = sa.get(k), sb.get(k)
            if isinstance(va, int) and isinstance(vb, int) and not isinstance(va, bool):
                det = "%s A=0x%X B=0x%X xor=0x%X" % (k, va, vb, va ^ vb)
            else:
                det = "%s A=%r B=%r" % (k, va, vb)
            out.append((tag + ":field " + k, det[:400]))
    if ra.cls == "ok" and family in ("pending", "timer", "bus", "trace") and ra.ret != rb.ret:
        out.append((tag + ":return", "A=%r B=%r" % (ra.ret, rb.ret)))
    return out


def worker(job):
    models, seed, start, count, show = job
    ma = importlib.import_module(models[0])
    mb = importlib.import_module(models[1])
    counts = collections.Counter()
    examples = {}
    outcome = collections.Counter()
    done = 0
    for idx in range(start, start + count):
        family, name, st, args = gen_vector(seed, idx)
        if family == "step":
            w = args[0]
            sa, sb = ma.supports(w), mb.supports(w)
            if sa != sb:
                key = "supports:%s A=%s B=%s" % (name, sa, sb)
                counts[key] += 1
                examples.setdefault(key, []).append("seed=%d idx=%d word=0x%08X" % (seed, idx, w)) \
                    if len(examples.get(key, [])) < show else None
                continue
            if not sa:
                continue
        try:
            ra = run_a(ma, family, name, st, args)
        except Exception as e:                       # model crash is a finding too
            ra = Res("crash", notes=["A:crash %s: %s" % (type(e).__name__, e)])
        try:
            rb = run_b(mb, family, name, st, args)
        except Exception as e:
            rb = Res("crash", notes=["B:crash %s: %s" % (type(e).__name__, e)])
        done += 1
        outcome["%s A=%s B=%s" % (family, ra.cls, rb.cls)] += 1
        for key, det in compare(family, name, ra, rb):
            counts[key] += 1
            lst = examples.setdefault(key, [])
            if len(lst) < show:
                lst.append("seed=%d idx=%d %s" % (seed, idx, det))
    return done, counts, examples, outcome


def supports_sweep(models, seed):
    ma = importlib.import_module(models[0])
    mb = importlib.import_module(models[1])
    r = random.Random(seed)
    counts = collections.Counter()
    ex = {}
    n = 0
    for op in range(64):
        for _ in range(64):
            w = (op << 26) | r.getrandbits(26)
            if op in (19, 31):
                w = (w & ~(0x3FF << 1)) | (r.randrange(1024) << 1)
            n += 1
            if ma.supports(w) != mb.supports(w):
                key = "supports op=%d xo=%d A=%s B=%s" % (op, (w >> 1) & 0x3FF, ma.supports(w), mb.supports(w))
                counts[key] += 1
                ex.setdefault(key, "0x%08X" % w)
    for op in (19, 31):
        for xo in range(1024):
            for noise in (0, r.getrandbits(21) << 11, 1, 0x03FFF801):
                w = ((op << 26) | (xo << 1) | noise) & M
                n += 1
                if ma.supports(w) != mb.supports(w):
                    key = "supports op=%d xo=%d A=%s B=%s" % (op, xo, ma.supports(w), mb.supports(w))
                    counts[key] += 1
                    ex.setdefault(key, "0x%08X" % w)
    for w in (0x44000000, 0x44000001, 0x44000002, 0x44000003, 0x47FFFFFE):
        n += 1
        if ma.supports(w) != mb.supports(w):
            key = "supports sc-form 0x%08X A=%s B=%s" % (w, ma.supports(w), mb.supports(w))
            counts[key] += 1
            ex.setdefault(key, "0x%08X" % w)
    return n, counts, ex


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--n", type=int, default=220000)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 4))
    ap.add_argument("--models", default="exc_A,exc_B")
    ap.add_argument("--show", type=int, default=3)
    a = ap.parse_args()
    models = tuple(a.models.split(","))
    sn, scounts, sex = supports_sweep(models, a.seed)
    chunk = max(1, a.n // (a.jobs * 8))
    jobs = [(models, a.seed, s, min(chunk, a.n - s), a.show) for s in range(0, a.n, chunk)]
    total = 0
    counts = collections.Counter()
    examples = {}
    outcome = collections.Counter()
    if a.jobs > 1:
        with mp.Pool(a.jobs) as pool:
            results = pool.map(worker, jobs)
    else:
        results = [worker(j) for j in jobs]
    for done, c, ex, oc in results:
        total += done
        counts.update(c)
        outcome.update(oc)
        for k, v in ex.items():
            lst = examples.setdefault(k, [])
            lst.extend(v[: max(0, a.show - len(lst))])
    print("models: %s vs %s   seed %d" % (models[0], models[1], a.seed))
    print("supports sweep: %d words, %d disagreement classes" % (sn, len(scounts)))
    for k, v in sorted(scounts.items()):
        print("  %6d  %s   e.g. %s" % (v, k, sex[k]))
    print("vectors executed on both models: %d (drawn %d)" % (total, a.n))
    print("outcome mix:")
    for k, v in sorted(outcome.items()):
        print("  %7d  %s" % (v, k))
    ndis = sum(counts.values())
    print("disagreements: %d in %d classes" % (ndis, len(counts)))
    for k, v in sorted(counts.items(), key=lambda kv: -kv[1]):
        print("  %7d  %s" % (v, k))
        for e in examples.get(k, []):
            print("            %s" % e)
    return 0 if (ndis == 0 and not scounts) else 1


if __name__ == "__main__":
    sys.exit(main())
