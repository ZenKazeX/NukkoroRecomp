"""Frame functions with calls and no branches, decompiled by trial and error; kept ONLY when they compile byte-identical.

    python -B -m dolrecon.autodecomp_calls <named project> <spine dir> [--max 60] [--jobs 6] [--passes 12]   (from tools/)

Extends dolrecon.autodecomp (straight-line leaves) to the next shapes, in order of yield:
  frame wrapper   stwu / mflr / stw r0 ... one `bl` ... lwz r0 / mtlr / addi r1 / blr
  frame, N calls  the same with several calls in sequence (callee-saved registers hold values across calls)
Not C-expressible, reported and not tried: tail calls and thunks (`b target`, `addi r3,r3,K ; b target`). The game's CodeWarrior
(-O4,s, 1.3.2-2.7) never emits a sibling call from C: `int w(int a) { return g(a); }` compiles to a frame plus `bl g`
(shown by tools/dolrecon/test_autodecomp_calls.py), so those bodies come from C++ virtual thunks, which need the class
layout, not a guess.

The search. The symbolic executor is autodecomp.Leaf plus calls. What the instructions cannot show is how many argument
registers a call passes (a register the body never touches is invisible) - so the argument count of each call is a
search variable: `max(registers defined since the last call, K)` for K in 0..4. Further variables: loads inlined at their
use or kept in temporaries, float constants literal or extern, the compiler flag set. Every distinct candidate source is
compiled at most once per function (a variant that produces the same text as an earlier one is not an attempt) and judged
by dolrecon.match M1-M4; a wrong guess costs CPU and can never produce a false match. Every attempt is counted per function
and per shape in report.json (matched / tried / attempts), failures included in the denominator.
"""
from __future__ import annotations

import argparse
import concurrent.futures as cf
import csv
import json
import os
import re
import sys

from . import autodecomp as ad
from . import match as m

Unsupported = ad.Unsupported
MEM_RE = ad.MEM_RE

# Passes: (flags, inline loads, extern constants, minimum arguments of each call). Tried in this order; the order is the
# prior - the first entries are what won most often on the 4,636 straight-line matches.
PASSES = [(ad.GAME_FLAGS, True, False, 0, False), (ad.GAME_FLAGS, False, False, 0, False),
          (ad.GAME_FLAGS, True, False, 1, False), (ad.GAME_FLAGS, False, False, 1, False),
          (ad.GAME_FLAGS, True, False, 2, False), (ad.GAME_FLAGS, False, False, 2, False),
          (ad.GAME_FLAGS, True, False, 3, False), (ad.GAME_FLAGS, False, False, 3, False),
          (ad.GAME_FLAGS, True, False, 4, False), (ad.GAME_FLAGS, False, False, 4, False),
          (ad.GAME_FLAGS, True, False, 0, True), (ad.GAME_FLAGS, False, False, 0, True),
          (ad.GAME_FLAGS, True, False, 1, True), (ad.GAME_FLAGS, False, False, 1, True),
          (ad.GAME_FLAGS, True, False, 2, True), (ad.GAME_FLAGS, False, False, 2, True),
          (ad.ALT_FLAGS, True, False, 0, False), (ad.ALT_FLAGS, False, False, 0, False),
          (ad.ALT_FLAGS, True, False, 1, False), (ad.ALT_FLAGS, False, False, 1, False),
          (ad.ALT_FLAGS, True, True, 0, False), (ad.ALT_FLAGS, False, True, 0, False),
          (ad.GAME_FLAGS, True, True, 0, False), (ad.GAME_FLAGS, False, True, 0, False)]
# the same prefix with every register the arms of a branch disagree on carried in a C variable
PASSES += [p + ("hoist",) for p in PASSES[:10]]
# nesting single-use call results into the next use, and passing float arguments, alone and combined (tried on the winners' prefix)
PASSES += [p + (o,) for o in (("nest",), ("fargs",), ("nest", "fargs"), ("hoist", "nest"), ("hoist", "fargs"), ("hoist", "nest", "fargs"))
           for p in PASSES[:8]]


def mem(operand):
    mm = MEM_RE.match(operand)
    return (ad.num(mm[1]), mm[2]) if mm and not mm[1].endswith("@sda21") else None


def shape_of(body):
    """The shape tag of a body, for the report. 'frame-calls-N' bodies are the ones this module tries."""
    ops = [op for op, _ in body]
    labels = "label" in ops
    calls = sum(op == "bl" for op in ops)
    frame = bool(ops) and ops[0] == "stwu"
    if frame and not labels:
        return "frame-calls-1" if calls == 1 else "frame-calls-2" if calls == 2 else "frame-calls-3+" if calls > 2 else "frame-no-call"
    if frame:
        return "frame-branches"
    if labels:
        return "leaf-branches" if calls == 0 else "frameless-branches-calls"
    if ops and ops[-1] == "b":
        return "tail-call" if len(ops) == 1 else "thunk-or-tail"
    if ops and ops[-1] == "bctr":
        return "tail-indirect"
    if calls:
        return "frameless-call"
    return "leaf-straight"


class CallLeaf(ad.Leaf):
    def __init__(self, name, body, symbols, read_dol, inline=False, extern_consts=False, min_args=0, locals_desc=False, opts=()):
        super().__init__(name, body, symbols, read_dol, inline=inline, extern_consts=extern_consts)
        self.min_args = min_args
        self.locals_desc = locals_desc
        self.opts = frozenset(opts)
        self.hoist_all = "hoist" in self.opts
        self.objs = []          # stack objects (frame offset, size)
        self.frame = None
        self.dead = set()
        self.vararg = False     # a `crclr 6` was seen: the next call is a variadic call
        self.ctr = None         # expression moved to CTR by mtctr

    def set(self, r, e, t="int"):
        self.dead.discard(r)
        super().set(r, e, t)

    def get(self, r, want="int"):
        if r in self.dead:
            raise Unsupported(f"reads {r} after a call")
        return super().get(r, want)

    def frame_op(self, k, op, a):
        """True when the instruction is the frame's own bookkeeping (prologue / epilogue)."""
        n = self.frame
        if op == "mflr":
            return a == ["r0"]
        if op == "mtlr":
            return a == ["r0"]
        if op == "addi" and a[:2] == ["r1", "r1"]:
            return ad.num(a[2]) == n
        if op in ("stw", "lwz") and len(a) == 2:
            mm = mem(a[1])
            if mm and mm[1] == "r1":
                reg = int(a[0][1:])
                if a[0] == "r0":
                    return mm[0] == n + 4
                return a[0][0] == "r" and 14 <= reg <= 31 and 8 <= mm[0] < n + 4
        if op in ("stmw", "lmw") and len(a) == 2:
            mm = mem(a[1])
            return bool(mm) and mm[1] == "r1" and 14 <= int(a[0][1:]) <= 31
        return False

    def scan_locals(self):
        """Stack objects: frame offsets whose address is taken (addi rD, r1, off) or that are loaded / stored directly."""
        n = self.frame
        taken, acc, low = set(), set(), n
        for op, args in self.body[1:-1]:
            a = [x.strip() for x in args.split(",")] if args else []
            if op == "addi" and len(a) == 3 and a[1] == "r1" and a[0] != "r1":
                taken.add(ad.num(a[2]))
                continue
            if op in ("stw", "lwz", "stmw", "lmw", "stb", "lbz", "sth", "lhz", "lha", "stfs", "lfs", "stfd", "lfd") and len(a) == 2:
                mm = mem(a[1])
                if mm and mm[1] == "r1":
                    if self.frame_op(0, op, a):
                        if op in ("stw", "stmw") and a[0] != "r0":
                            low = min(low, mm[0])
                    else:
                        acc.add(mm[0])
        starts = sorted(taken | acc)
        end = low & ~3
        self.objs = []
        for i, s in enumerate(starts):
            e = starts[i + 1] if i + 1 < len(starts) else end
            if e <= s or s < 8:
                raise Unsupported("stack object layout")
            self.objs.append((s, e - s))

    def local_ref(self, off):
        for s, size in self.objs:
            if s <= off < s + size:
                return s, off - s
        raise Unsupported("stack offset outside objects")

    def local_decls(self):
        out = []
        for s, size in (reversed(self.objs) if self.locals_desc else self.objs):
            out.append(f"int l{s}[{size // 4}];" if size % 4 == 0 and s % 4 == 0 else f"char l{s}[{size}];")
        return out

    def address(self, operand, size, ctype):
        mm = MEM_RE.match(operand)
        if mm and mm[2] == "r1":
            s, k = self.local_ref(ad.num(mm[1]))
            return f"*({ctype}*)l{s}" if k == 0 else f"*({ctype}*)((char*)l{s} + {k})"
        return super().address(operand, size, ctype)

    def step(self, op, a):
        if op == "addi" and len(a) == 3 and a[1] == "r1" and a[0] != "r1":
            s, k = self.local_ref(ad.num(a[2]))
            self.set(a[0], f"((int)l{s})" if k == 0 else f"((int)l{s} + {k})")
            return
        if op == "extrwi" and len(a) == 4:
            n, b = ad.num(a[2]), ad.num(a[3])
            self.set(a[0], f"(int)(((unsigned int){self.get(a[1])} >> {32 - b - n}) & 0x{(1 << n) - 1:X})")
            return
        if op == "clrrwi" and len(a) == 3:
            n = ad.num(a[2])
            self.set(a[0], f"({self.get(a[1])} & 0x{(~((1 << n) - 1)) & 0xFFFFFFFF:X})")
            return
        if op in self.INDEXED and len(a) == 3:
            # indexed access: the address rA + rB (rA = r0 reads as zero) held in a pseudo register, then the plain form
            base = "0" if a[1] == "r0" else self.get(a[1], "int")
            self.set("r99", f"({base} + {self.get(a[2], 'int')})")
            self.written.discard("r99")
            try:
                super().step(self.INDEXED[op], [a[0], "0(r99)"])
            finally:
                self.reg.pop("r99", None)
            return
        super().step(op, a)

    def count_args(self):
        auto = 0
        for i in range(3, 11):
            r = f"r{i}"
            if r in self.reg and r not in self.dead:
                auto = i - 2
            else:
                break
        return max(auto, self.min_args)

    def finish_call(self, callee, sym=None, kind="fn"):
        args = [self.get(f"r{i}", "int") for i in range(3, 3 + self.count_args())]
        types = ["int"] * len(args)
        if "fargs" in self.opts:
            # floating-point arguments are invisible to the integer count: f1.. defined since the last call are passed too
            for i in range(1, 9):
                r = f"f{i}"
                if r not in self.reg or r in self.dead:
                    break
                t = self.reg[r][1]
                args.append(self.get(r, t))
                types.append(t)
        if sym is not None:
            if len(types) > types.count("int") and kind == "fn":
                kind = "fn:" + ",".join(types)
            prev = self.externs.get(sym)
            if prev and prev != kind:
                raise Unsupported(f"{sym} used as {prev} and a function")
            self.externs[sym] = kind
        else:
            callee = callee.replace("(int(*)())", "(int(*)(" + (",".join(types) if types else "") + "))")
        v = self.fresh("int", f"{callee}({', '.join(args)})")
        self.stores += 1                      # a call may write memory: no inlined load may cross it
        for r in [f"r{i}" for i in range(4, 13)] + ["r0"] + [f"f{i}" for i in range(1, 14)]:
            self.reg.pop(r, None)
            self.dead.add(r)
        self.dead.discard("r3")
        self.set("r3", v)

    INDEXED = {"lwzx": "lwz", "lbzx": "lbz", "lhzx": "lhz", "lhax": "lha", "stwx": "stw", "stbx": "stb", "sthx": "sth",
               "lfsx": "lfs", "lfdx": "lfd", "stfsx": "stfs", "stfdx": "stfd"}

    def call(self, sym):
        if sym.startswith(".L") or sym not in self.symbols:
            raise Unsupported("call target")
        kind = "fnv" if self.vararg else "fn"
        self.vararg = False
        if kind == "fnv" and self.count_args() < 1:
            self.min_args = max(self.min_args, 1)
        self.finish_call(sym, sym, kind)

    def call_indirect(self):
        if self.ctr is None:
            raise Unsupported("bctrl without mtctr")
        target, self.ctr = self.ctr, None
        self.finish_call(f"((int(*)())({target}))")

    def run(self):
        body = self.body
        if not body or body[0][0] != "stwu":
            raise Unsupported("no frame")
        a0 = [x.strip() for x in body[0][1].split(",")]
        mm = mem(a0[1]) if len(a0) == 2 and a0[0] == "r1" else None
        if not mm or mm[1] != "r1" or mm[0] >= 0:
            raise Unsupported("frame form")
        self.frame = -mm[0]
        self.scan_locals()
        if body[-1][0] != "blr":
            raise Unsupported("does not end in blr")
        self.labels = {args.rstrip(":"): k for k, (op, args) in enumerate(body) if op == "label"}
        self.hoisted = []
        self.cmp = None
        self.range(1, len(body) - 1)

    NEGATE = {"beq": "!=", "bne": "==", "blt": ">=", "bge": "<", "bgt": "<=", "ble": ">"}

    def condition(self, op):
        """C condition under which the fall-through block of branch `op` runs."""
        if self.cmp is None:
            raise Unsupported("branch without a compare")
        lhs, rhs, signed = self.cmp
        if not signed:
            lhs, rhs = f"((unsigned){lhs})", f"((unsigned){rhs})"
        return f"{lhs} {self.NEGATE[op]} {rhs}"

    def snapshot(self):
        return dict(self.reg), set(self.dead), set(self.written), self.cmp

    def restore(self, snap):
        self.reg, self.dead, self.written, self.cmp = dict(snap[0]), set(snap[1]), set(snap[2]), snap[3]

    def arm(self, lo, hi, snap):
        """Run body[lo:hi] from the snapshot state; returns (statements, end state)."""
        self.restore(snap)
        outer, self.stmts = self.stmts, []
        self.range(lo, hi)
        stmts, self.stmts = self.stmts, outer
        return stmts, self.snapshot()

    def merge(self, a, b, base):
        """State after a join of two paths: registers on which they agree stay; the others are unreadable afterwards."""
        reg, dead = {}, set(a[1]) | set(b[1])
        for r in set(a[0]) | set(b[0]):
            ea, eb = a[0].get(r), b[0].get(r)
            if ea == eb:
                reg[r] = ea
            elif (ea is None or eb is None) and (ea or eb)[0] == f"a{r[1:]}":
                reg[r] = ea or eb                  # a parameter first read inside one arm
            else:
                dead.add(r)
        self.reg, self.dead = reg, dead
        self.written = {r for r in set(a[2]) | set(b[2]) if r in reg or r not in dead}
        self.written -= {r for r in dead if r not in reg}
        self.cmp = None

    def range(self, lo, hi):
        body = self.body
        k = lo
        while k < hi:
            op, args = body[k]
            a = [x.strip() for x in args.split(",")] if args else []
            k += 1
            if op == "label":
                raise Unsupported("label that is not a forward join")
            if self.frame_op(k - 1, op, a):
                continue
            if op in ("cmpwi", "cmplwi", "cmpw", "cmplw") and len(a) == 2:
                lhs = self.get(a[0], "int")
                rhs = str(ad.num(a[1])) if op.endswith("i") else self.get(a[1], "int")
                self.cmp = (lhs, rhs, op in ("cmpwi", "cmpw"))
                continue
            if op.endswith(".") and op[:-1] in ("mr", "clrlwi", "extrwi", "extsb", "extsh", "rlwinm", "addic", "subic", "or", "and", "andi", "neg"):
                self.step(op[:-1], a)          # record form: also compares the result with zero (cr0)
                self.cmp = (self.get(a[0], "int"), "0", True)
                continue
            if op in self.NEGATE:
                target = self.labels.get(a[0])
                if target is None or target <= k - 1 or target > hi:
                    raise Unsupported("branch target")
                cond = self.condition(op)
                base = self.snapshot()
                last = body[target - 1]
                if last[0] == "b" and last[1] in self.labels and self.labels[last[1]] > target:
                    # if / else: the then-arm ends in `b join`, the else-arm runs from the label to the join
                    join = self.labels[last[1]]
                    if join > hi:
                        raise Unsupported("else join")
                    then, end_a = self.arm(k, target - 1, base)
                    other, end_b = self.arm(target + 1, join, base)
                    self.emit_if(cond, then, other, end_a, end_b, base)
                    k = join + 1
                    continue
                then, end_a = self.arm(k, target, base)
                self.emit_if(cond, then, [], end_a, base, base)
                k = target + 1
                continue
            if op == "bl":
                self.call(a[0])
            elif op == "crclr" and a == ["cr1eq"]:
                self.vararg = True
            elif op == "mtctr":
                self.ctr = self.get(a[0], "int")
            elif op == "bctrl":
                self.call_indirect()
            elif op in ("b", "bctr", "blrl"):
                raise Unsupported("branch or indirect call")
            else:
                self.step(op, a)

    def emit_if(self, cond, then, other, end_a, end_b, base):
        a, b = dict(end_a[0]), dict(end_b[0])
        # an r3 the arms set differently is carried in a hoisted variable assigned at the end of each arm
        r0 = base[0].get("r3")
        if "r3" in a and "r3" in b and a["r3"] != b["r3"] and a["r3"][1] == b["r3"][1] and a["r3"] != r0 and b["r3"] != r0:
            v = f"v{len(self.hoisted)}"
            self.hoisted.append(f"int {v};")
            then = then + [f"{v} = {a['r3'][0]};"]
            other = other + [f"{v} = {b['r3'][0]};"]
            end_a = (dict(a, r3=(v, "int")), end_a[1], end_a[2], None)
            end_b = (dict(b, r3=(v, "int")), end_b[1], end_b[2], None)
        pre = []
        if self.hoist_all:
            for r in sorted(set(end_a[0]) & set(end_b[0]) - {"r3"}):
                x, y = end_a[0][r], end_b[0][r]
                if x != y and x[1] == y[1] == "int" and r not in end_a[1] and r not in end_b[1]:
                    v = f"v{len(self.hoisted)}"
                    self.hoisted.append(f"int {v};")
                    if end_b is base:
                        pre.append(f"{v} = {y[0]};")
                    else:
                        other = other + [f"{v} = {y[0]};"]
                    then = then + [f"{v} = {x[0]};"]
                    end_a = (dict(end_a[0], **{r: (v, "int")}), end_a[1], end_a[2], None)
                    end_b = (dict(end_b[0], **{r: (v, "int")}), end_b[1], end_b[2], None)
        self.stmts.extend(pre)
        text = [f"if ({cond}) {{"] + [f"    {x}" for x in then]
        if other:
            text += ["} else {"] + [f"    {x}" for x in other]
        text.append("}")
        self.stmts.extend(text)
        self.merge(end_a, end_b, base)

    def c_source(self):
        src = super().c_source()
        decls = self.local_decls() + list(self.hoisted)
        if decls:
            head, _, rest = src.partition("{\n")
            src = head + "{\n" + "".join(f"    {d}\n" for d in decls) + rest
        # a call whose result is never used is a statement, not a temporary
        for mt in list(re.finditer(r"^( *)int (t\d+) = ([A-Za-z_]\w*\(.*\));$", src, re.M)):
            if len(re.findall(rf"\b{mt[2]}\b", src)) == 1:
                src = src.replace(mt[0], f"{mt[1]}{mt[3]};")
        if "nest" in self.opts:
            src = nest_single_use(src)
        return src


def nest_single_use(src):
    """`int t = f(x);` followed on the next line by the only use of t: the call moves into that use."""
    NL = chr(10)
    lines = src.split(NL)
    i = 0
    while i < len(lines) - 1:
        mt = re.match(r"^( *)int (t\d+) = (.*);$", lines[i])
        if mt and "(" in mt[3]:
            use = re.compile(r"\b" + mt[2] + r"\b")
            if len(use.findall(src)) == 2 and use.search(lines[i + 1]):
                lines[i + 1] = use.sub(lambda _m: mt[3], lines[i + 1], count=1)
                del lines[i]
                src = NL.join(lines)
                continue
        i += 1
    return NL.join(lines)


def compile_plan(unit, funcs, project, symbols, work, flags, inline, consts, min_args, ldesc, tried, stats, opts=()):
    """Candidate sources for one pass over a unit; returns {name: (source, externs)}; skips an already tried text."""
    out = {}
    for name, body in funcs:
        try:
            leaf = CallLeaf(name, body, symbols, project.read, inline=inline, extern_consts=consts, min_args=min_args, locals_desc=ldesc, opts=opts)
            src = leaf.c_source()
        except Unsupported as e:
            key = re.sub(r"(r|f)\d+|0x[0-9A-Fa-f]+|\d+|\w+@\w+", "#", str(e))[:40]
            stats["unsupported"][key] = stats["unsupported"].get(key, 0) + 1
            continue
        except (KeyError, ValueError, IndexError):
            stats["unsupported"]["parse"] = stats["unsupported"].get("parse", 0) + 1
            continue
        h = hash((flags, src))
        if h in tried.setdefault(name, set()):
            continue
        tried[name].add(h)
        out[name] = (src, leaf.externs)
    return out


def decl(sym, t):
    return f"extern int {sym}({t[3:].replace(',', ', ')});" if t.startswith("fn:") else f"extern int {sym}(int, ...);" if t == "fnv" else f"extern int {sym}();" if t == "fn" else (f"extern {t} {sym};" if t != "char" else f"extern char {sym}[];")


def judge_batch(project, unit, work, tag, flags, names, cands, externs, stats, attempts, matched, bad):
    src_path = os.path.join(work, f"{unit}_{tag}.c")
    lines = ["/* generated candidate */"] + [decl(s, t) for s, t in sorted(externs.items())] + [cands[n][0] for n in names]
    with open(src_path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n\n".join(lines) + "\n")
    results, obj, pairs = m.run_one(project, src_path, ad.CC, flags, [], work, [], {})
    os.remove(src_path)
    if results is None:
        if len(names) == 1:
            stats["compile_dropped"] += 1
            attempts[names[0]] = attempts.get(names[0], 0) + 1
            return
        half = len(names) // 2
        judge_batch(project, unit, work, tag + "a", flags, names[:half], cands, externs, stats, attempts, matched, bad)
        judge_batch(project, unit, work, tag + "b", flags, names[half:], cands, externs, stats, attempts, matched, bad)
        return
    for r, (bs, ts) in zip(results, pairs):
        attempts[bs.name] = attempts.get(bs.name, 0) + 1
        if r.matched:
            matched[bs.name] = (cands[bs.name][0], flags, cands[bs.name][1])
        else:
            first = (r.problems or ["?"])[0]
            ms = re.match(r"M1 size 0x([0-9A-Fa-f]+) vs original 0x([0-9A-Fa-f]+)", first)
            key = (f"M1 size delta {(int(ms[2], 16) - int(ms[1], 16)) // 4:+d} insns" if ms else
                   re.sub(r"0x[0-9A-Fa-f]+|[0-9a-f]{8}|\+\S+", "#", first)[:50])
            stats["mismatch"][key] = stats["mismatch"].get(key, 0) + 1
            bad.setdefault(bs.name, key)
    if os.path.exists(obj):
        os.remove(obj)


def process_unit(args):
    unit, funcs, project_root, symbols, work, npasses = args
    project = m.Project(project_root)
    stats = {"unsupported": {}, "mismatch": {}, "compile_dropped": 0}
    tried, attempts, matched, bad, externs_all = {}, {}, {}, {}, {}
    for flags, inline, consts, min_args, ldesc, *rest in PASSES[:npasses]:
        todo = [(n, b) for n, b in funcs if n not in matched]
        if not todo:
            break
        cands = compile_plan(unit, todo, project, symbols, work, flags, inline, consts, min_args, ldesc, tried, stats, tuple(rest[0]) if rest and not isinstance(rest[0], bool) else ("hoist",) if rest and rest[0] else ())
        # waves: a function that calls another candidate of the same file (or is called by one) goes in a later file
        remaining = list(cands)
        wave = 0
        while remaining:
            batch, deferred, ex = [], [], {}
            for n in remaining:
                e = cands[n][1]
                callees = {s for s, t in e.items() if t.startswith("fn")}
                if (any(ex.get(s, t) != t for s, t in e.items()) or (callees & set(batch))
                        or any(n in cands[b][1] for b in batch)):
                    deferred.append(n)
                    continue
                batch.append(n)
                ex.update(e)
            judge_batch(project, unit, work, f"p{wave}", flags, batch, cands, ex, stats, attempts, matched, bad)
            remaining = deferred
            wave += 1
    return unit, matched, attempts, bad, stats, externs_all


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("project")
    ap.add_argument("spine")
    ap.add_argument("--decomp", default=os.path.join(os.path.dirname(__file__), "..", "..", "decomp"))
    ap.add_argument("--max", type=int, default=60, help="max instructions")
    ap.add_argument("--jobs", type=int, default=6)
    ap.add_argument("--passes", type=int, default=len(PASSES))
    ap.add_argument("--tag", default="", help="file-name tag of this run's output (_calls<tag><depth>.c); only files with the same tag are replaced")
    ap.add_argument("--report", default=None, help="report.json path (default: <project>/build/autodecomp/report_calls.json)")
    args = ap.parse_args(argv)
    symbols = {}
    pat = re.compile(r"^(\S+) = (\.\w+):0x([0-9A-Fa-f]+); // type:(\w+)(?: size:0x([0-9A-Fa-f]+))?")
    with open(os.path.join(args.project, "config", "symbols.txt"), encoding="utf-8") as f:
        for line in f:
            mm = pat.match(line)
            if mm:
                symbols[mm[1]] = (int(mm[3], 16), int(mm[5] or "0", 16), mm[2], mm[4])
    with open(os.path.join(args.spine, "functions.csv"), encoding="utf-8") as f:
        layer = {r["name"]: r["layer"] for r in csv.DictReader(f)}
    done = set()
    mpath = os.path.join(args.decomp, "matched.json")
    if os.path.exists(mpath):
        with open(mpath, encoding="utf-8") as f:
            for e in json.load(f).get("files", []):
                for fn in e.get("functions", []):
                    if fn.get("matched"):
                        done.add(int(fn["address"], 16))
    asm_dir = os.path.join(args.project, "build", "asm")
    work = os.path.join(args.project, "build", "autodecomp_calls")
    os.makedirs(work, exist_ok=True)
    shapes = {}
    jobs = []
    unit_of = {}
    for n in sorted(os.listdir(asm_dir)):
        if not n.endswith(".s"):
            continue
        unit = n[:-2]
        funcs = []
        for name, body, _ in ad.parse_asm(os.path.join(asm_dir, n)):
            if not body or len(body) > args.max:
                continue
            tag = shape_of(body)
            row = shapes.setdefault(tag, {"functions": 0, "already_matched": 0, "tried": 0, "supported": 0, "matched": 0, "attempts": 0})
            row["functions"] += 1
            if symbols.get(name, (0,))[0] in done:
                row["already_matched"] += 1
                continue
            if tag.startswith("frame-calls") or tag in ("frame-no-call", "frame-branches"):
                funcs.append((name, body))
                unit_of[name] = (unit, tag)
        if funcs:
            jobs.append((unit, funcs, args.project, symbols, work, args.passes))
    for u, fs, *_ in jobs:
        for nm, _b in fs:
            shapes[unit_of[nm][1]]["tried"] += 1
    matched_all, bad_all, stats_all = {}, {}, {"unsupported": {}, "mismatch": {}, "compile_dropped": 0}
    attempts_all = {}
    with cf.ProcessPoolExecutor(max_workers=args.jobs) as pool:
        for unit, matched, attempts, bad, stats, ex in pool.map(process_unit, jobs):
            for nme, sf in matched.items():
                matched_all[nme] = (unit, sf)
            for nme, c in attempts.items():
                attempts_all[nme] = c
            bad_all.update({k: v for k, v in bad.items() if k not in matched})
            for k in ("unsupported", "mismatch"):
                for kk, v in stats[k].items():
                    stats_all[k][kk] = stats_all[k].get(kk, 0) + v
            stats_all["compile_dropped"] += stats["compile_dropped"]
    for nme in attempts_all:
        shapes[unit_of[nme][1]]["attempts"] += attempts_all[nme]
        shapes[unit_of[nme][1]]["supported"] += 1
    for nme in matched_all:
        shapes[unit_of[nme][1]]["matched"] += 1
    report = {"matched": len(matched_all), "attempts": sum(attempts_all.values()), "shapes": dict(sorted(shapes.items())),
              "unsupported": dict(sorted(stats_all["unsupported"].items(), key=lambda kv: -kv[1])[:25]),
              "mismatch": dict(sorted(stats_all["mismatch"].items(), key=lambda kv: -kv[1])[:25]),
              "compile_dropped": stats_all["compile_dropped"],
              "last_failure_by_function": {k: bad_all[k] for k in sorted(bad_all)[:40]}}
    out = args.report or os.path.join(work, "report_calls.json")
    with open(out, "w", encoding="utf-8") as f:
        json.dump(report, f, indent=1)
    # write the matched functions: one file per (layer, unit, flags, call depth). A function that calls another matched
    # function of the same unit goes in a deeper file, so a call is never a local call inside the compiled file.
    by_unit = {}
    for nme, (unit, (src, flags, ex)) in matched_all.items():
        by_unit.setdefault(unit, {})[nme] = (src, flags, ex)
    out_root = os.path.join(args.decomp, "auto")
    if os.path.isdir(out_root):
        for root, _, files in os.walk(out_root):
            for fn in files:
                if re.search(rf"_calls{args.tag}\d*(_p)?\.c$", fn) and (args.tag or not re.search(r"_calls[a-z]", fn)):
                    os.remove(os.path.join(root, fn))
    for unit, fns in sorted(by_unit.items()):
        depth = {}

        def dep(n, seen=()):
            if n in depth:
                return depth[n]
            cs = [c for c, k in fns[n][2].items() if c in fns and c != n and c not in seen]
            depth[n] = 0 if not cs else 1 + max(dep(c, seen + (n,)) for c in cs)
            return depth[n]
        groups = {}
        for nme in fns:
            groups.setdefault((layer.get(nme, "unresolved"), fns[nme][1], dep(nme)), []).append((symbols[nme][0], nme, fns[nme][0]))
        for (lay, flags, d_), items in sorted(groups.items()):
            items.sort()
            d = os.path.join(out_root, lay)
            os.makedirs(d, exist_ok=True)
            ex_all = {}
            for _, nme, _s in items:
                ex_all.update(fns[nme][2])
            ex_all = {s: k for s, k in ex_all.items() if s not in {n for _, n, _ in items}}
            head = [f"/* {unit}: frame functions with calls, decompiled by trial by tools/dolrecon/autodecomp_calls.py.",
                    " * Each compiles byte-identical (dolrecon.match M1-M4). Parameters and the argument count of each call are inferred",
                    " * (a register the body never touches is invisible); callees are declared `int f();`.",
                    f" *   @match cc={ad.CC} flags={flags}",
                    " */", ""]
            body = ["".join(f"// @sym {n} = 0x{a:08X}\n" for a, n, _ in items)]
            body += [decl(s, k) for s, k in sorted(ex_all.items())]
            for a, nme, s in items:
                body.append(f"/* 0x{a:08X} */\n{s}")
            suffix = f"_calls{args.tag}{d_ if d_ else ''}" + ("" if flags == ad.GAME_FLAGS else "_p")
            with open(os.path.join(d, f"{unit}{suffix}.c"), "w", encoding="utf-8", newline="\n") as f:
                f.write("\n".join(head) + "\n\n".join(body) + "\n")
    print(json.dumps(report, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
