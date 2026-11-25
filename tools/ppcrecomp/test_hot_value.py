#!/usr/bin/env python3
"""Differential test of the hot-body value pass (ppcrecomp --hot-value-filter).

Random hot bodies in the exact spelling the emitter produces are run twice by a small evaluator: as emitted and as
rewritten by the REAL generator pass. Both start at the same instruction (the first one, or any label - with garbage in
the pass's fs<n> locals, as on arrival from the dispatcher) and must produce

  * the same guest memory writes in the same order,
  * the same complete guest state (GPRs, both halves of every FPR, CR, FPSCR compare bits, LR) at every observer the
    path reaches: a call, an MSR[FP] guard, a paired-single store, mfcr, a polled back edge, and
  * the same complete guest state where the body is left (return, the end of the body, a goto out of it).

Between observers the rewritten body may hold stale CR fields and FPR halves; that is the pass.

Negative controls (the test fails when one is NOT caught): generator mutant 1 (a single store forwards across a
label), generator mutant 2 (calls are not observers), and a text mutant (one more compare removed from the rewritten
body).

Helper list check: every ppc_ops.h helper that touches CR or the FPRs, or calls into the Cpu, must be an observer for
the pass (or one of the helpers it models); probed through the real pass, so a helper added later cannot silently be
treated as transparent.

Usage: test_hot_value.py --generator <ppcrecomp.exe> --ppc-ops <ppc_ops.h> [--trials N] [--seed S]
"""
import argparse
import os
import random
import re
import struct
import subprocess
import sys

M = 0xFFFFFFFF
M64 = 0xFFFFFFFFFFFFFFFF
GPOOL = [3, 4, 5, 31]
FPOOL = [0, 1, 2, 31]
BASE = 0x80001000
SPECIAL = [0x00000000, 0x80000000, 0x3F800000, 0xBF800000, 0x7F800000, 0xFF800000, 0x7FC00000, 0x7F800001,
           0xFFC12345, 0x00000001, 0x007FFFFF, 0x80000123, 0x00800000, 0x7F7FFFFF, 0x41200000, 0xC2C80000]


def s2d(word):
    """types.h single_to_double_bits: D.6."""
    value = struct.unpack("<f", struct.pack("<I", word))[0]
    if value != value:
        return ((word & 0x80000000) << 32) | 0x7FF0000000000000 | ((word & 0x007FFFFF) << 29)
    return struct.unpack("<Q", struct.pack("<d", value))[0]


def d2s(bits):
    """types.h double_to_single_bits: D.7 (the literal conversion; the fast path is the same function)."""
    exponent = (bits >> 52) & 0x7FF
    if exponent > 896 or (bits << 1) & M64 == 0:
        return ((bits >> 32) & 0xC0000000) | ((bits >> 29) & 0x3FFFFFFF)
    sign = (bits >> 32) & 0x80000000
    if exponent < 874:
        return sign
    mantissa = 0x80000000 | ((bits >> 21) & 0x7FFFFFFF)
    return sign | (mantissa >> (905 - exponent))


def fps_add(a, b):
    """Stands for any value helper: a deterministic function of its operands with single-valued results."""
    x = struct.unpack("<d", struct.pack("<Q", a))[0]
    y = struct.unpack("<d", struct.pack("<Q", b))[0]
    try:
        r = x + y
        r = struct.unpack("<f", struct.pack("<f", r))[0]
    except OverflowError:
        r = float("inf")
    if r != r:
        return 0x7FF8000000000000
    return struct.unpack("<Q", struct.pack("<d", r))[0]


def bswap(word):
    return int.from_bytes((word & M).to_bytes(4, "big"), "little")


def label_name(address):
    return "L_%08X" % address


def rand_body(rng):
    """Returns a list of pieces: [address, label, falls, [lines]]."""
    n = rng.randrange(5, 22)
    addresses = [BASE + 4 * i for i in range(n)]
    labels = set(a for a in addresses[1:] if rng.random() < 0.22)
    pieces = []
    for i, address in enumerate(addresses):
        lines = []
        falls = True
        g = lambda: rng.choice(GPOOL)
        f = lambda: rng.choice(FPOOL)
        ea = lambda: "    ea = (c.gpr[%d] & 0x3Cu) + 0x%Xu;" % (g(), rng.choice([0, 4, 8, 0x40]))
        forward = [a for a in sorted(labels) if a > address]
        kind = rng.random()
        if kind < 0.20:
            r = f()
            lines += [ea(), "    tmp64 = single_to_double_bits(c.mem_read32(ea));",
                      "    c.fpr[%d].bits[0] = tmp64;" % r, "    c.fpr[%d].bits[1] = tmp64;" % r]
        elif kind < 0.38:
            lines += [ea(), "    c.mem_write32(ea, double_to_single_bits(c.fpr[%d].bits[0]));" % f()]
        elif kind < 0.43:
            lines += ["    c.fpr[%d].bits[0] = c.fpr[%d].bits[0];" % (f(), f())]
        elif kind < 0.50:
            r = f()
            lines += ["    tmp64 = ppc::fps_add(c, c.fpr[%d].bits[0], c.fpr[%d].bits[0]);" % (f(), f()),
                      "    c.fpr[%d].bits[0] = tmp64;" % r, "    c.fpr[%d].bits[1] = tmp64;" % r]
        elif kind < 0.53:
            lines += [ea(), "    c.fpr[%d].bits[0] = c.mem_read64(ea);" % f()]
        elif kind < 0.63:
            a, k, field = g(), rng.randrange(3), rng.choice([0, 0, 0, 1, 7])
            lines += ["    ppc::cmpl(c, %du, c.gpr[%d], 0x%Xu);" % (field, a, k)]
        elif kind < 0.69:
            lines += ["    ppc::update_cr0(c, c.gpr[%d]);" % g()]
        elif kind < 0.72:
            lines += ["    ppc::cmp(c, %du, static_cast<s32>(c.gpr[%d]), static_cast<s32>(c.gpr[%d]));"
                      % (rng.choice([0, 6]), g(), g())]
        elif kind < 0.75:
            lines += ["    ppc::fp_compare(c, %du, c.fpr[%d].bits[0], c.fpr[%d].bits[0], false);"
                      % (rng.choice([0, 1]), f(), f())]
        elif kind < 0.84 and forward:
            target = rng.choice(forward)
            if rng.random() < 0.5:
                lines += ["    cnd = (c.gpr[%d] == 0x%Xu);" % (g(), rng.randrange(3))]
            else:
                lines += ["    cnd = %sppc::cr_bit(c, %du);" % (rng.choice(["", "!"]), rng.choice([0, 1, 2, 4, 30]))]
            lines += ["    if (cnd) {", "        goto %s;" % label_name(target), "    }"]
        elif kind < 0.86 and forward:
            lines += ["    goto %s;" % label_name(rng.choice(forward))]
            falls = False
        elif kind < 0.89:
            lines += ["    c.gpr[%d] = c.gpr[%d] + 0x%Xu;" % (g(), g(), rng.randrange(1, 9))]
        elif kind < 0.92:
            lines += ["    c.lr = 0x%Xu;" % (address + 4), "    fn_8001DB08(c);",
                      "    if (c.exiting() && c.finish_transfers()) return;",
                      "    if (c.pc != 0x%Xu) { recomp::resume_elsewhere(c, 0x%Xu); return; }" % (address + 4, address)]
        elif kind < 0.94:
            lines += ["    if ((c.msr() & 0x2000u) == 0u) { recomp::fp_unavailable(c, 0x%Xu); return; }" % address]
        elif kind < 0.96:
            lines += [ea(), "    ppc::psq_store(c, %du, ea, false, 0u);" % f()]
        elif kind < 0.97:
            lines += ["    c.gpr[%d] = c.cr;" % g()]
        elif kind < 0.985 and i > 2:
            back = [a for a in sorted(labels) if a <= address]
            if back:
                target = rng.choice(back)
                lines += ["    cnd = (c.gpr[%d] == 0x%Xu);" % (g(), rng.randrange(3)), "    if (cnd) {",
                          "        c.pc = 0x%Xu;" % target,
                          "        if (c.needs_dispatch()) { c.leave_to_dispatcher(0x%Xu, FallbackReason::Yield, 0x%Xu); return; }"
                          % (target, address),
                          "        goto %s;" % label_name(target), "    }"]
            else:
                lines += ["    ppc::update_cr0(c, c.gpr[%d]);" % g()]
        else:
            lines += ["    tmp = c.lr & ~3u;", "    c.pc = tmp;", "    return;"]
            falls = False
        pieces.append([address, address in labels, falls, lines])
    return pieces


class State:
    def __init__(self, rng):
        self.g = [rng.choice([0, 1, 2, rng.getrandbits(32)]) for _ in range(32)]
        self.f = [[rng.choice([s2d(rng.choice(SPECIAL)), rng.getrandbits(64)]) for _ in range(2)] for _ in range(32)]
        self.cr = rng.getrandbits(32)
        self.fpcc = rng.getrandbits(4)
        self.lr = rng.getrandbits(32) & ~3
        self.pc = 0
        self.mem = {}
        self.writes = []
        self.observed = []
        self.ea = rng.getrandbits(8)
        self.tmp = rng.getrandbits(32)
        self.tmp64 = rng.getrandbits(64)
        self.cnd = False
        self.fs = {n: rng.getrandbits(32) for n in range(32)}  # garbage: a local is only valid inside its run
        self.salt = rng.getrandbits(32)

    def rd32(self, ea):
        ea &= M
        if ea not in self.mem:
            h = (ea * 2654435761 + self.salt) & M
            self.mem[ea] = SPECIAL[h % len(SPECIAL)] if (h >> 8) & 1 else (h * 40503 + 977) & M
        return self.mem[ea]

    def wr32(self, ea, value):
        self.mem[ea & M] = value & M
        self.writes.append((ea & M, value & M))

    def guest(self):
        return (tuple(self.g), tuple(tuple(x) for x in self.f), self.cr, self.fpcc, self.lr)

    def observe(self, what):
        self.observed.append((what, self.guest()))


def set_field(st, field, flags):
    shift = 28 - 4 * field
    st.cr = (st.cr & ~(0xF << shift) & M) | (flags << shift)


def cr_bit(st, bit):
    return (st.cr >> (31 - bit)) & 1


def run(pieces, start, st, budget=400):
    index = {p[0]: i for i, p in enumerate(pieces)}

    def value(expr):
        e = expr
        e = re.sub(r"c\.fpr\[(\d+)\]\.bits\[(\d)\]", r"st.f[\1][\2]", e)
        e = re.sub(r"c\.gpr\[(\d+)\]", r"st.g[\1]", e)
        e = re.sub(r"\bfs(\d+)\b", r"st.fs[\1]", e)
        e = re.sub(r"0x([0-9A-Fa-f]+)u", r"0x\1", e)
        e = re.sub(r"\b(\d+)u\b", r"\1", e)
        e = re.sub(r"static_cast<s32>\(([^()]*)\)", r"S32(\1)", e)
        e = e.replace("c.mem_read32_raw(ea)", "BS(RD32())").replace("bswap32(", "BS(")
        e = e.replace("c.mem_read32(ea)", "RD32()").replace("c.mem_read64(ea)", "RD64()")
        e = e.replace("ppc::fps_add(c, ", "fps_add(").replace("ppc::cr_bit(c, ", "CRB(")
        e = e.replace("single_to_double_bits(", "s2d(").replace("double_to_single_bits(", "d2s(")
        e = e.replace("c.lr", "st.lr").replace("c.cr", "st.cr").replace("~3", "0xFFFFFFFC")
        e = re.sub(r"\btmp64\b", "st.tmp64", e)
        e = re.sub(r"\btmp\b", "st.tmp", e)
        e = re.sub(r"\bea\b", "st.ea", e)
        e = re.sub(r"!\s*CRB", "not CRB", e)
        return eval(e, {"st": st, "s2d": s2d, "d2s": d2s, "fps_add": fps_add, "CRB": lambda b: cr_bit(st, b),
                        "S32": lambda x: x - (1 << 32) if x & 0x80000000 else x,
                        "RD32": lambda: st.rd32(st.ea), "BS": bswap,
                        "RD64": lambda: (st.rd32(st.ea) << 32) | st.rd32(st.ea + 4)})

    p = start
    while True:
        if p >= len(pieces):
            return "end"
        budget -= 1
        if budget < 0:
            return "budget"
        lines = pieces[p][3]
        jumped = None
        i = 0
        while i < len(lines):
            t = lines[i].strip()
            i += 1
            m = re.match(r"ea = (.*);$", t)
            if m:
                st.ea = value(m.group(1)) & M
                continue
            m = re.match(r"fs(\d+) = (.*);$", t)
            if m:
                st.fs[int(m.group(1))] = value(m.group(2)) & M
                continue
            m = re.match(r"tmp64 = (.*);$", t)
            if m:
                st.tmp64 = value(m.group(1)) & M64
                continue
            m = re.match(r"tmp = (.*);$", t)
            if m:
                st.tmp = value(m.group(1)) & M
                continue
            m = re.match(r"c\.fpr\[(\d+)\]\.bits\[(\d)\] = (.*);$", t)
            if m:
                st.f[int(m.group(1))][int(m.group(2))] = value(m.group(3)) & M64
                continue
            m = re.match(r"c\.gpr\[(\d+)\] = (.*);$", t)
            if m:
                if "c.cr" in m.group(2):
                    st.observe("mfcr")
                st.g[int(m.group(1))] = value(m.group(2)) & M
                continue
            m = re.match(r"c\.mem_write32\(ea, (.*)\);$", t)
            if m:
                st.wr32(st.ea, value(m.group(1)))
                continue
            m = re.match(r"c\.mem_write32_raw\(ea, (.*)\);$", t)
            if m:
                st.wr32(st.ea, bswap(value(m.group(1))))
                continue
            m = re.match(r"ppc::cmpl\(c, (\d)u, (.*), (.*)\);$", t)
            if m:
                a, b = value(m.group(2)) & M, value(m.group(3)) & M
                set_field(st, int(m.group(1)), 8 if a < b else 4 if a > b else 2)
                continue
            m = re.match(r"ppc::cmp\(c, (\d)u, (.*), (static_cast.*)\);$", t)
            if m:
                a, b = value(m.group(2)), value(m.group(3))
                set_field(st, int(m.group(1)), 8 if a < b else 4 if a > b else 2)
                continue
            m = re.match(r"ppc::update_cr0(?:_inl)?\(c, (.*)\);$", t)
            if m:
                v = value(m.group(1)) & M
                s = v - (1 << 32) if v & 0x80000000 else v
                set_field(st, 0, 8 if s < 0 else 4 if s > 0 else 2)
                continue
            m = re.match(r"ppc::fp_compare(?:_inl)?\(c, (\d)u, (.*), (.*), false\);$", t)
            if m:
                a = struct.unpack("<d", struct.pack("<Q", value(m.group(2))))[0]
                b = struct.unpack("<d", struct.pack("<Q", value(m.group(3))))[0]
                flags = 8 if a < b else 4 if a > b else 2 if a == b else 1
                set_field(st, int(m.group(1)), flags)
                st.fpcc = flags
                continue
            m = re.match(r"ppc::psq_store(?:_inl)?\(c, (\d+)u, ea, false, 0u\);$", t)
            if m:
                st.observe("psq")
                r = int(m.group(1))
                st.wr32(st.ea, d2s(st.f[r][0]))
                st.wr32(st.ea + 4, d2s(st.f[r][1]))
                continue
            m = re.match(r"cnd = (.*);$", t)
            if m:
                st.cnd = bool(value(m.group(1)))
                continue
            m = re.match(r"c\.lr = (.*);$", t)
            if m:
                st.lr = value(m.group(1)) & M
                continue
            m = re.match(r"c\.pc = (.*);$", t)
            if m:
                st.pc = value(m.group(1)) & M
                continue
            if t == "if (cnd) {":
                block = []
                while lines[i].strip() != "}":
                    block.append(lines[i].strip())
                    i += 1
                i += 1
                if st.cnd:
                    for b in block:
                        m = re.match(r"goto (L_[0-9A-F]{8});$", b)
                        if m:
                            jumped = int(m.group(1)[2:], 16)
                        elif b.startswith("if (c.needs_dispatch())"):
                            st.observe("poll")
                        elif b.startswith("c.pc = "):
                            st.pc = int(b[7:-2], 16)
                        else:
                            raise RuntimeError("evaluator does not know (block): " + b)
                    break
                continue
            m = re.match(r"goto (L_[0-9A-F]{8});$", t)
            if m:
                jumped = int(m.group(1)[2:], 16)
                break
            if t.startswith("fn_"):
                st.observe("call")
                st.g = [(x * 31 + 7 + k) & M for k, x in enumerate(st.g)]
                st.f = [[(x * 6364136223846793005 + 1442695040888963407 + k) & M64 for x in pair]
                        for k, pair in enumerate(st.f)]
                st.cr = (st.cr * 1103515245 + 12345) & M
                st.lr = (st.lr ^ 0x5A5A0) & M
                continue
            if t.startswith("if (c.exiting()") or t.startswith("if (c.pc !="):
                continue
            if t.startswith("if ((c.msr()"):
                st.observe("guard")
                continue
            if t == "return;":
                return "return"
            raise RuntimeError("evaluator does not know: " + t)
        if jumped is not None:
            if jumped not in index:
                return "goto-out"
            p = index[jumped]
            continue
        if not pieces[p][2]:
            return "end"
        p += 1


def rewrite(generator, pieces, mask, mutant=0):
    text = ""
    for address, label, falls, lines in pieces:
        text += "@piece %X %d %d\n" % (address, 1 if label else 0, 1 if falls else 0)
        text += "".join(line + "\n" for line in lines)
    text += "@end\n"
    out = subprocess.run([generator, "--hot-value-filter", str(mask), str(mutant)], input=text, capture_output=True,
                         text=True, check=True).stdout
    result = []
    for line in out.splitlines():
        if line.startswith("@piece "):
            _, address, label, falls = line.split()
            result.append([int(address, 16), label == "1", falls == "1", []])
        elif line.startswith("@fs"):
            pass
        else:
            result[-1][3].append(line)
    assert len(result) == len(pieces), (len(result), len(pieces))
    return result


def execute(pieces, start, seed):
    st = State(random.Random(seed))
    how = run(pieces, start, st)
    final = st.guest() if how != "budget" else None
    return (how, tuple(st.writes), tuple(st.observed), final)


def differs(old, new, starts, rng):
    for start in starts:
        for _ in range(3):
            seed = rng.getrandbits(32)
            try:
                if execute(old, start, seed) != execute(new, start, seed):
                    return (start, seed)
            except Exception as error:  # a mutant may produce text the evaluator cannot run
                return (start, str(error))
    return None


def scan_helpers(path):
    """ppc_ops.h functions tagged by what their bodies (transitively) touch."""
    src = open(path, encoding="utf-8", errors="replace").read()
    funcs = {}
    pattern = r"\n(?:template[^\n]*\n)?(?:GEKKO_\w+ |inline |constexpr |static )+[\w:<>]+[ &*]+(\w+)\(([^)]*)\)[^{;]*\{"
    for m in re.finditer(pattern, src):
        depth, i = 1, m.end()
        while depth and i < len(src):
            depth += {"{": 1, "}": -1}.get(src[i], 0)
            i += 1
        funcs[m.group(1)] = funcs.get(m.group(1), "") + m.group(2) + src[m.end():i]

    def direct(body):
        tags = set()
        if re.search(r"c\.cr\b|set_cr_field|set_cr_bit|cr_field|cr_bit", body):
            tags.add("cr")
        if re.search(r"c\.fpr\b", body):
            tags.add("fpr")
        calls = set(re.findall(r"\bc\.(\w+)\(", body)) - {"mem_read8", "mem_read16", "mem_read32", "mem_read64",
                                                          "mem_write8", "mem_write16", "mem_write32", "mem_write64",
                                                          "time_base", "msr"}
        if calls:
            tags.add("cpu-call")
        return tags

    tags = {name: direct(body) for name, body in funcs.items()}
    changed = True
    while changed:
        changed = False
        for name, body in funcs.items():
            for other in funcs:
                if other != name and re.search(r"\b" + other + r"\(", body) and not tags[other] <= tags[name]:
                    tags[name] |= tags[other]
                    changed = True
    return tags


def transparent_for(generator, name, what):
    """True when the pass removes a dead write across `ppc::<name>(c, ...)`."""
    probe = "    c.gpr[3] = ppc::%s(c, c.gpr[4]);" % name
    if what == "cr":
        lines = [["    ppc::cmpl(c, 0u, c.gpr[3], 0x0u);"], [probe], ["    ppc::update_cr0(c, c.gpr[5]);"]]
        gone = "ppc::cmpl"
    else:
        lines = [["    c.fpr[1].bits[0] = tmp64;"], [probe], ["    c.fpr[1].bits[0] = c.fpr[2].bits[0];"]]
        gone = "c.fpr[1].bits[0] = tmp64;"
    pieces = [[BASE + 4 * i, False, True, ls] for i, ls in enumerate(lines)]
    pieces.append([BASE + 12, False, False, ["    tmp = c.lr & ~3u;", "    c.pc = tmp;", "    return;"]])
    new = rewrite(generator, pieces, 7)
    return not any(gone in line for _, _, _, ls in new for line in ls)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--generator", required=True)
    ap.add_argument("--ppc-ops", required=True)
    ap.add_argument("--trials", type=int, default=1500)
    ap.add_argument("--seed", type=int, default=20261115)
    args = ap.parse_args()
    args.generator = os.path.abspath(args.generator)
    rng = random.Random(args.seed)

    # 1. the helper list
    tags = scan_helpers(args.ppc_ops)
    modelled = {"cmp", "cmpl", "update_cr0", "fp_compare", "cr_bit", "update_cr0_inl", "fp_compare_inl"}
    reviewed_cpu_calls = {"read_spr", "write_spr"}  # literal-gated in the pass: XER, LR, CTR only
    bad_helpers = []
    probed = 0
    for name in sorted(tags):
        if name in modelled:
            continue
        risky = tags[name] - ({"cpu-call"} if name in reviewed_cpu_calls else set())
        if not risky:
            continue
        probed += 1
        if transparent_for(args.generator, name, "cr") or transparent_for(args.generator, name, "fpr"):
            bad_helpers.append((name, sorted(tags[name])))
    sanity = transparent_for(args.generator, "mullw", "cr") and transparent_for(args.generator, "mullw", "fpr")
    print("helpers in ppc_ops.h: %d, touching CR / FPR / the Cpu: %d probed, wrongly transparent: %s; probe sanity %s"
          % (len(tags), probed, bad_helpers, sanity))
    if bad_helpers or not sanity or probed < 8:
        print("FAIL: helper list")
        return 1

    # 2. differential + negative controls
    bad = 0
    stats = {"forwarded": 0, "cr": 0, "fpr": 0}
    caught = {"mutant-forward-across-label": 0, "mutant-calls-transparent": 0, "text-drop-compare": 0}
    changed_by = dict.fromkeys(caught, 0)
    for trial in range(args.trials):
        pieces = rand_body(rng)
        new = rewrite(args.generator, pieces, 7)
        starts = [0] + [i for i, p in enumerate(pieces) if p[1]]
        old_text = [l for p in pieces for l in p[3]]
        new_text = [l for p in new for l in p[3]]
        stats["forwarded"] += sum(1 for l in new_text if re.search(r"c\.mem_write32_raw\(ea, fs\d+\);", l))
        stats["cr"] += (sum(1 for l in old_text if re.match(r"\s*ppc::(cmpl?|update_cr0)\(", l)) -
                        sum(1 for l in new_text if re.match(r"\s*ppc::(cmpl?|update_cr0)\(", l)))
        stats["fpr"] += (sum(1 for l in old_text if re.match(r"\s*c\.fpr\[\d+\]\.bits\[\d\] = ", l)) -
                         sum(1 for l in new_text if re.match(r"\s*c\.fpr\[\d+\]\.bits\[\d\] = ", l)))
        where = differs(pieces, new, starts, rng)
        if where is not None:
            bad += 1
            if bad <= 3:
                print("MISMATCH trial %d start/seed %s" % (trial, where))
                print("old:\n" + "\n".join("%X%s\n%s" % (p[0], " L" if p[1] else "", "\n".join(p[3])) for p in pieces))
                print("new:\n" + "\n".join("%X%s\n%s" % (p[0], " L" if p[1] else "", "\n".join(p[3])) for p in new))
        for kind, mutant in (("mutant-forward-across-label", 1), ("mutant-calls-transparent", 2)):
            mutated = rewrite(args.generator, pieces, 7, mutant)
            if [p[3] for p in mutated] == [p[3] for p in new]:
                continue
            changed_by[kind] += 1
            if differs(pieces, mutated, starts, rng) is not None:
                caught[kind] += 1
        sites = [(i, j) for i, p in enumerate(new) for j, l in enumerate(p[3])
                 if re.match(r"\s*ppc::(cmpl?|update_cr0)\(", l)]
        if sites:
            i, j = rng.choice(sites)
            mutated = [[p[0], p[1], p[2], list(p[3])] for p in new]
            del mutated[i][3][j]
            changed_by["text-drop-compare"] += 1
            if differs(pieces, mutated, starts, rng) is not None:
                caught["text-drop-compare"] += 1
    print("trials %d, mismatches %d; stores forwarded %d, CR writes removed %d, FPR half writes removed %d"
          % (args.trials, bad, stats["forwarded"], stats["cr"], stats["fpr"]))
    for kind in caught:
        print("negative control %-28s changed bodies %5d, caught %5d" % (kind, changed_by[kind], caught[kind]))
    if bad:
        return 1
    if min(stats.values()) == 0:
        print("a rewrite never happened: the test proves nothing")
        return 1
    for kind in caught:
        if caught[kind] == 0:
            print("negative control %s was not caught" % kind)
            return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
