#!/usr/bin/env python3
"""Differential test of the hot-body GPR read cache (ppcrecomp --gpr-cache-filter).

Random hot-body snippets in the exact spelling the emitter produces are run twice by a small evaluator: as emitted
(the old text) and as rewritten by the REAL generator pass (the new text). Both start at the same instruction (the
first one, or any instruction after a label - with garbage in the cache locals, as on arrival from another path) and
must end with identical guest registers, memory, CR, LR and SPR mirror, whichever exit (fall off the end, goto, return)
they take. Barrier statements (calls, unknown helpers) scramble every guest register, as a callee does.

Negative controls: the evaluator must catch three seeded defects of the rewritten text (a refill after a barrier
removed, a write-through store removed, the flush at a label removed). If a mutation is not caught the test fails.

Usage: test_gpr_cache.py --generator <ppcrecomp.exe> [--trials N] [--seed S]
"""
import argparse
import random
import re
import subprocess
import sys

M = 0xFFFFFFFF
POOL = [0, 1, 3, 4, 5, 9, 12, 31]  # small pool: registers are reused heavily


def rand_snippets(rng):
    """Returns a list of (is_label, [lines]) instructions."""
    insns = []
    n = rng.randrange(4, 16)
    label_no = 0
    for i in range(n):
        lines = []
        kind = rng.random()
        r = lambda: rng.choice(POOL)
        imm = lambda: "0x%Xu" % rng.choice([0, 4, 8, 0x10, 0x20, 0x7FFC])
        if kind < 0.20:
            lines.append("    c.gpr[%d] = c.gpr[%d] + %s;" % (r(), r(), imm()))
        elif kind < 0.30:
            lines.append("    c.gpr[%d] = c.gpr[%d] | c.gpr[%d];" % (r(), r(), r()))
        elif kind < 0.36:
            lines.append("    c.gpr[%d] = c.gpr[%d] + c.gpr[%d];" % (r(), r(), r()))
        elif kind < 0.46:
            lines.append("    ea = (c.gpr[%d] & 0xFFu) + %s;" % (r(), imm()))
            lines.append("    c.gpr[%d] = c.mem_read32(ea);" % r())
        elif kind < 0.58:
            lines.append("    ea = (c.gpr[%d] & 0xFFu) + %s;" % (r(), imm()))
            lines.append("    c.mem_write32(ea, c.gpr[%d]);" % r())
        elif kind < 0.62:
            lines.append("    c.gpr[%d] = 0x%Xu + 0x10u;" % (r(), rng.randrange(1 << 16)))
        elif kind < 0.70:
            lines.append("    ppc::cmpl(c, 0u, c.gpr[%d], 0x%Xu);" % (r(), rng.randrange(4)))
            lines.append("    cnd = !(c.gpr[%d] == 0x0u);" % r())
            label_no += 1
            lines.append("    if (cnd) {")
            lines.append("        c.pc = 0x8001D9F8u;")
            lines.append("        goto L_%d;" % label_no)
            lines.append("    }")
        elif kind < 0.75:
            lines.append("    ppc::update_cr0(c, c.gpr[%d]);" % r())
        elif kind < 0.79:
            lines.append("    ppc::write_spr(c, 9u, c.gpr[%d]);" % r())
        elif kind < 0.83:
            lines.append("    c.gpr[%d] = ppc::read_spr(c, 9u);" % r())
        elif kind < 0.88:
            lines.append("    c.lr = 0x%Xu;" % rng.randrange(1 << 16))
            lines.append("    fn_8001DB08(c);")
            lines.append("    if (c.exiting() && c.finish_transfers()) return;")
            lines.append("    if (c.pc != 0x8001DA6Cu) { recomp::resume_elsewhere(c, 0x8001DA68u); return; }")
        elif kind < 0.92:
            lines.append("    c.gpr[%d] = unknown_helper(c.gpr[%d]);" % (r(), r()))
        elif kind < 0.95:
            lines.append("    c.gpr[%d] = c.gpr[%d];" % (r(), r()))
        else:
            lines.append("    if ((c.msr() & 0x2000u) == 0x0u) { recomp::fp_unavailable(c, 0x8001DA68u); return; }")
        insns.append((i > 0 and rng.random() < 0.25, lines))
    return insns


class State:
    def __init__(self, rng):
        self.g = [rng.getrandbits(32) for _ in range(32)]
        self.mem = {}
        self.cr = rng.getrandbits(32)
        self.lr = 0
        self.spr9 = 0
        self.ea = 0
        self.cnd = False
        self.pc = 0
        self.locals = {"gp%d" % i: rng.getrandbits(32) for i in range(32)}  # stale values from "another path"

    def snapshot(self):
        return (tuple(self.g), tuple(sorted(self.mem.items())), self.cr, self.lr, self.spr9, self.pc)


def mem_rd(st, ea):
    return st.mem.setdefault(ea & M, (ea * 2654435761 + 12345) & M)


def run(lines, st):
    """Interprets the lines; returns when they end or on goto/return."""
    env = {"MR": lambda ea: mem_rd(st, ea), "U": lambda x: x & M}

    def ev(expr):
        e = expr
        e = re.sub(r"c\.gpr\[(\d+)\]", r"st.g[\1]", e)
        e = re.sub(r"\bgp(\d+)\b", r"st.locals['gp\1']", e)
        e = re.sub(r"0x([0-9A-Fa-f]+)u", r"0x\1", e)
        e = re.sub(r"\b(\d+)u\b", r"\1", e)
        e = e.replace("c.mem_read32(", "MR(").replace("c.lr", "st.lr").replace("c.msr()", "0x2000")
        e = re.sub(r"\bea\b", "st.ea", e)
        e = e.replace("!(", "not (")
        e = re.sub(r"\bppc::read_spr\(c, 9\)", "st.spr9", e)
        e = re.sub(r"\bunknown_helper\(", "UH(", e)
        return eval(e, {"st": st, "M": M, **env, "UH": lambda x: (x * 3 + 5) & M})

    i = 0
    while i < len(lines):
        t = lines[i].strip()
        i += 1
        if t.startswith("gp") and "= c.gpr[" in t and t.endswith("];") and t.count("=") == 1:
            name, rhs = t[:-1].split(" = ")
            st.locals[name] = st.g[int(re.search(r"\[(\d+)\]", rhs).group(1))]
            continue
        m = re.match(r"(gp\d+) = (.*); c\.gpr\[(\d+)\] = (gp\d+);$", t)
        if m:
            v = ev(m.group(2)) & M
            st.locals[m.group(1)] = v
            st.g[int(m.group(3))] = st.locals[m.group(4)]
            continue
        m = re.match(r"c\.gpr\[(\d+)\] = (.*);$", t)
        if m:
            st.g[int(m.group(1))] = ev(m.group(2).replace("ppc::read_spr(c, 9u)", "st.spr9")) & M
            continue
        m = re.match(r"ea = (.*);$", t)
        if m:
            st.ea = ev(m.group(1)) & M
            continue
        m = re.match(r"c\.mem_write32\(ea, (.*)\);$", t)
        if m:
            st.mem[st.ea] = ev(m.group(1)) & M
            continue
        m = re.match(r"ppc::cmpl\(c, 0u, (.*), (.*)\);$", t)
        if m:
            a, b = ev(m.group(1)) & M, ev(m.group(2)) & M
            st.cr = (st.cr & ~(0xF << 28)) | ((8 if a < b else 4 if a > b else 2) << 28)
            continue
        m = re.match(r"ppc::update_cr0\(c, (.*)\);$", t)
        if m:
            v = ev(m.group(1)) & M
            s = v - (1 << 32) if v & 0x80000000 else v
            st.cr = (st.cr & ~(0xF << 28)) | ((8 if s < 0 else 4 if s > 0 else 2) << 28)
            continue
        m = re.match(r"ppc::write_spr\(c, 9u, (.*)\);$", t)
        if m:
            st.spr9 = ev(m.group(1)) & M
            continue
        m = re.match(r"cnd = (.*);$", t)
        if m:
            st.cnd = bool(ev(m.group(1)))
            continue
        m = re.match(r"c\.lr = (.*);$", t)
        if m:
            st.lr = ev(m.group(1)) & M
            continue
        if t == "if (cnd) {":
            depth = 1
            body_start = i
            while depth:
                depth += lines[i].count("{") - lines[i].count("}")
                i += 1
            if st.cnd:
                st.pc = 1
                return "goto"
            continue
        if t.startswith("fn_"):
            st.g = [(x * 31 + 7 + k) & M for k, x in enumerate(st.g)]
            st.lr = (st.lr ^ 0x5A5A) & M
            continue
        if t.startswith("if (c.exiting()") or t.startswith("if (c.pc !="):
            continue
        if t.startswith("if ((c.msr()"):
            continue
        raise RuntimeError("evaluator does not know: " + t)
    return "end"


def rewrite(generator, insns):
    text = ""
    for is_label, lines in insns:
        text += ("@label\n" if is_label else "@insn\n") + "\n".join(lines) + "\n"
    text += "@end\n"
    out = subprocess.run([generator, "--gpr-cache-filter"], input=text, capture_output=True, text=True, check=True).stdout
    result, cur = [], None
    for line in out.splitlines():
        if line in ("@insn", "@label"):
            cur = (line == "@label", [])
            result.append(cur)
        elif line.startswith("@used"):
            pass
        else:
            cur[1].append(line)
    assert len(result) == len(insns), (len(result), len(insns))
    return result


def execute(insns, start, seed):
    rng = random.Random(seed)
    st = State(rng)
    lines = [l for _, ls in insns[start:] for l in ls]
    run(lines, st)
    return st.snapshot()


def mutate(insns_new, kind):
    """Seeded defects. Returns a mutated copy or None when the sample has no site for it."""
    out = [(lab, list(ls)) for lab, ls in insns_new]
    flat = [(i, j) for i, (_, ls) in enumerate(out) for j in range(len(ls))]
    if kind == "drop-refill":
        # remove the first fill that follows a barrier call
        seen_call = False
        for i, j in flat:
            t = out[i][1][j].strip()
            if t.startswith("fn_") or "unknown_helper" in t:
                seen_call = True
            elif seen_call and re.match(r"gp\d+ = c\.gpr\[\d+\];$", t):
                del out[i][1][j]
                return out
    elif kind == "drop-writethrough":
        for i, j in flat:
            t = out[i][1][j].strip()
            m = re.match(r"(gp\d+ = .*); c\.gpr\[\d+\] = gp\d+;$", t)
            if m:
                out[i][1][j] = "    " + m.group(1) + ";"
                return out
    elif kind == "no-label-flush":
        # a label that arrives with registers cached: pretend the run continued (drop the refill that follows it)
        for i in range(len(out)):
            if out[i][0] and i > 0:
                for j, l in enumerate(out[i][1]):
                    if re.match(r"\s*gp\d+ = c\.gpr\[\d+\];$", l):
                        del out[i][1][j]
                        return out
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--generator", required=True)
    ap.add_argument("--trials", type=int, default=1500)
    ap.add_argument("--seed", type=int, default=20261112)
    args = ap.parse_args()
    rng = random.Random(args.seed)
    bad = 0
    caught = {"drop-refill": 0, "drop-writethrough": 0, "no-label-flush": 0}
    tried = dict.fromkeys(caught, 0)
    cached_reads = 0
    for trial in range(args.trials):
        insns = rand_snippets(rng)
        new = rewrite(args.generator, insns)
        starts = [0] + [i for i, (lab, _) in enumerate(insns) if lab]
        for start in starts:
            seed = rng.getrandbits(32)
            a = execute(insns, start, seed)
            b = execute(new, start, seed)
            if a != b:
                bad += 1
                if bad <= 5:
                    print("MISMATCH trial %d start %d" % (trial, start))
                    print("old:\n" + "\n".join("\n".join(l) for _, l in insns[start:]))
                    print("new:\n" + "\n".join("\n".join(l) for _, l in new[start:]))
        cached_reads += sum(1 for _, ls in new for l in ls if re.search(r"\bgp\d+\b", l))
        for kind in caught:
            mutant = mutate(new, kind)
            if mutant is None:
                continue
            tried[kind] += 1
            for start in starts:
                seed = rng.getrandbits(32)
                try:
                    if execute(insns, start, seed) != execute(mutant, start, seed):
                        caught[kind] += 1
                        break
                except Exception:
                    caught[kind] += 1
                    break
    print("trials %d, mismatches %d, lines using the cache %d" % (args.trials, bad, cached_reads))
    for kind in caught:
        print("negative control %-18s mutants %5d, caught %5d" % (kind, tried[kind], caught[kind]))
    if bad:
        return 1
    if cached_reads == 0:
        print("the pass cached nothing: the test proves nothing")
        return 1
    for kind in caught:
        if tried[kind] == 0 or caught[kind] == 0:
            print("negative control %s was not caught" % kind)
            return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
