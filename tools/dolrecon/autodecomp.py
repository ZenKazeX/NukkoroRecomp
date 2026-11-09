"""Straight-line leaf decompiler: C for small functions, kept ONLY when it compiles byte-identical.

    python -B -m dolrecon.autodecomp <named project> <spine dir> [--max 24] [--jobs 8]      (run from source/tools)

Scope: leaf functions without branches, calls or stack frame (the body is straight-line code ending in blr). The
instructions are executed symbolically into C expressions (registers -> expressions, loads -> typed reads at the
time they happen, stores -> statements in their original order). Candidates are written per split unit, compiled
with the game's CodeWarrior (dolrecon.match) and judged M1-M4; only matched functions are written to
source/decomp/auto/<layer>/<unit>.c, each with a note on what is inferred. dolrecon.decomp_verify re-proves them.

Inferred, and said so in each file:
  parameters  the registers the body reads before writing (r3.. as int, f1.. as float/double); a parameter the body
              never reads leaves no trace in the code and cannot be recovered;
  result      r3 written -> int, f1 written -> float/double; neither -> void.
Not attempted: tail calls (the arguments passed through are invisible), branches, calls, stack frames.
"""
from __future__ import annotations

import argparse
import concurrent.futures as cf
import csv
import json
import os
import re
import struct
import sys

from . import elfobj
from . import match as m

GAME_FLAGS = "-O4,s -use_lmw_stmw on -inline auto -str readonly"
ALT_FLAGS = "-O4,p -inline auto -str readonly"
CC = "2.6"

FN_RE = re.compile(r"^\.fn (\S+), (\w+)")
INS_RE = re.compile(r"^/\* ([0-9A-F]{8}) [0-9A-F]{8}  (?:[0-9A-F]{2} ){4}\*/\t(\S+)\s*(.*)$")
MEM_RE = re.compile(r"^(-?(?:0x[0-9a-fA-F]+|\d+)|[A-Za-z_]\w*@sda21)\((r\d+)\)$")
LOADS = {"lwz": ("int", 4), "lhz": ("unsigned short", 2), "lha": ("short", 2), "lbz": ("unsigned char", 1),
         "lfs": ("float", 4), "lfd": ("double", 8)}
STORES = {"stw": ("int", 4), "sth": ("short", 2), "stb": ("char", 1), "stfs": ("float", 4), "stfd": ("double", 8)}


LOAD_MARK = re.compile("\x01([0-9]+)\x01")
VARIANTS = [(True, False), (False, False), (True, True), (False, True)]   # (inline loads, constants as globals)


class Unsupported(Exception):
    pass


def num(s: str) -> int:
    return int(s, 0)


class Leaf:
    """Symbolic execution of one straight-line leaf."""

    def __init__(self, name: str, body: list[tuple[str, str]], symbols: dict, read_dol, inline: bool = False,
                 extern_consts: bool = False):
        self.name, self.body, self.symbols, self.read = name, body, symbols, read_dol
        self.inline, self.extern_consts = inline, extern_consts
        self.stores = 0                                # stores emitted so far (inline loads may not cross one)
        self.reg: dict[str, tuple[str, str]] = {}      # register -> (C expression, type)
        self.params: dict[str, str] = {}               # register -> type, for registers read before written
        self.stmts: list[str] = []
        self.externs: dict[str, str] = {}              # symbol -> C type
        self.tmp = 0
        self.written: set[str] = set()

    def get(self, r: str, want: str = "int") -> str:
        if r == "r0" and r not in self.reg:
            raise Unsupported("r0 read before write")
        if r in self.reg:
            e, t = self.reg[r]
            if self.inline and any(int(g) < self.stores for g in LOAD_MARK.findall(e)):
                raise Unsupported("inline load would cross a store")
            return e if t == want or want == "any" else f"(({want})({e}))"
        kind = r[0]
        if not re.fullmatch(r"r([3-9]|10)|f[1-8]", r):
            raise Unsupported(f"reads {r}")
        t = "int" if kind == "r" else ("double" if want == "double" else "float")
        self.params[r] = t
        name = f"a{r[1:]}" if kind == "r" else f"f{r[1:]}"
        self.reg[r] = (name, t)
        return name if want in (t, "any") else f"(({want})({name}))"

    def set(self, r: str, e: str, t: str = "int"):
        self.reg[r] = (e, t)
        self.written.add(r)

    def fresh(self, t: str, e: str) -> str:
        self.tmp += 1
        v = f"t{self.tmp}"
        self.stmts.append(f"{t} {v} = {e};")
        return v

    def address(self, operand: str, size: int, ctype: str) -> str:
        mm = MEM_RE.match(operand)
        if not mm:
            raise Unsupported(f"operand {operand}")
        disp, base = mm[1], mm[2]
        if disp.endswith("@sda21"):
            sym = disp[:-6]
            return self.global_ref(sym, ctype, size)
        base_e = self.get(base, "int")
        d = num(disp)
        addr = base_e if d == 0 else f"({base_e} + {d})"
        return f"*({ctype}*){addr}"

    def global_ref(self, sym: str, ctype: str, size: int) -> str:
        info = self.symbols.get(sym)
        if info is None:
            raise Unsupported(f"unknown symbol {sym}")
        addr, sec = info[0], info[2]
        if sec == ".sdata2" and ctype in ("float", "double") and self.extern_consts:
            ctype = "const " + ctype
        elif sec == ".sdata2" and ctype in ("float", "double"):
            raw = self.read(addr, size)
            if raw is None:
                raise Unsupported("literal unreadable")
            v = struct.unpack(">f" if size == 4 else ">d", raw)[0]
            if v != v or v in (float("inf"), float("-inf")):
                raise Unsupported("non-finite literal")
            lit = repr(v) if size == 8 else f"{v!r}f"
            if size == 4 and struct.unpack(">f", struct.pack(">f", float(lit[:-1])))[0] != v:
                raise Unsupported("literal does not round-trip")
            return f"({lit})" if size == 8 else lit
        prev = self.externs.get(sym)
        if prev and prev != ctype:
            raise Unsupported(f"{sym} used as {prev} and {ctype}")
        self.externs[sym] = ctype
        return sym

    def run(self):
        for k, (op, args) in enumerate(self.body):
            a = [x.strip() for x in args.split(",")] if args else []
            last = k == len(self.body) - 1
            if op == "blr":
                if not last:
                    raise Unsupported("blr inside")
                continue
            if last:
                raise Unsupported("does not end in blr")
            self.step(op, a)

    def step(self, op, a):
        if op == "li":
            if a[1].endswith("@sda21"):
                raise Unsupported("li of sda address")
            self.set(a[0], str(num(a[1])))
        elif op == "lis":
            if "@" in a[1]:
                sym = a[1].split("@")[0]
                if not a[1].endswith("@ha") or sym not in self.symbols:
                    raise Unsupported("lis form")
                self.set(a[0], f"@HA:{sym}", "ha")
            else:
                self.set(a[0], str((num(a[1]) << 16) - (1 << 32) if num(a[1]) & 0x8000 else num(a[1]) << 16))
        elif op in ("addi", "subi"):
            if a[2].endswith("@l"):
                sym = a[2][:-2]
                if self.reg.get(a[1], ("", ""))[0] != f"@HA:{sym}":
                    raise Unsupported("addi @l without its lis")
                self.externs.setdefault(sym, "char")
                self.set(a[0], f"((int)&{sym})")
            else:
                v = num(a[2]) * (-1 if op == "subi" else 1)
                self.set(a[0], f"({self.get(a[1])} + {v})")
        elif op == "mr":
            e, t = (self.reg[a[1]] if a[1] in self.reg else (self.get(a[1], "any"), self.reg[a[1]][1]))
            self.set(a[0], e, t)
        elif op in LOADS:
            ctype, size = LOADS[op]
            if "@" in a[1] and not a[1].split("(")[0].endswith("@sda21"):
                ha = a[1].split("@")[0]
                mm = re.match(r"(\w+)@l\((r\d+)\)", a[1])
                if not mm or self.reg.get(mm[2], ("", ""))[0] != f"@HA:{mm[1]}":
                    raise Unsupported("load @l form")
                e = self.global_ref(mm[1], ctype, size)
            else:
                e = self.address(a[1], size, ctype)
            if e.startswith("*(") or e in self.externs:
                rt = "float" if ctype == "float" else "double" if ctype == "double" else "int"
                if self.inline:
                    # read where it is used; get() refuses a use after a later store (the read would move)
                    self.set(a[0], f"\x01{self.stores}\x01({e})", rt)
                else:
                    v = self.fresh(rt, e if rt != "int" or ctype == "int" else f"({e})")
                    self.set(a[0], v, rt)
            else:
                self.set(a[0], e, "float" if ctype == "float" else "double")
        elif op in STORES:
            ctype, size = STORES[op]
            src = self.get(a[0], "float" if ctype == "float" else "double" if ctype == "double" else "int")
            if "@" in a[1] and not a[1].split("(")[0].endswith("@sda21"):
                mm = re.match(r"(\w+)@l\((r\d+)\)", a[1])
                if not mm or self.reg.get(mm[2], ("", ""))[0] != f"@HA:{mm[1]}":
                    raise Unsupported("store @l form")
                dst = self.global_ref(mm[1], ctype, size)
            else:
                dst = self.address(a[1], size, ctype)
            self.stmts.append(f"{dst} = {src};")
            self.stores += 1
        elif op in ("add", "subf", "mullw", "and", "or", "xor", "divw", "divwu"):
            x, y = self.get(a[1]), self.get(a[2])
            e = {"add": f"({x} + {y})", "subf": f"({y} - {x})", "mullw": f"({x} * {y})", "and": f"({x} & {y})",
                 "or": f"({x} | {y})", "xor": f"({x} ^ {y})", "divw": f"({x} / {y})",
                 "divwu": f"(int)((unsigned int){x} / (unsigned int){y})"}[op]
            self.set(a[0], e)
        elif op in ("mulli", "andi.", "ori", "xori"):
            x, v = self.get(a[1]), num(a[2])
            e = {"mulli": f"({x} * {v})", "andi.": f"({x} & {v})", "ori": f"({x} | {v})", "xori": f"({x} ^ {v})"}[op]
            self.set(a[0], e)
        elif op == "neg":
            self.set(a[0], f"(-{self.get(a[1])})")
        elif op in ("extsb", "extsh"):
            self.set(a[0], f"((int)({'signed char' if op == 'extsb' else 'short'})({self.get(a[1])}))")
        elif op == "slwi":
            self.set(a[0], f"({self.get(a[1])} << {num(a[2])})")
        elif op == "srwi":
            self.set(a[0], f"(int)((unsigned int){self.get(a[1])} >> {num(a[2])})")
        elif op == "srawi":
            self.set(a[0], f"({self.get(a[1])} >> {num(a[2])})")
        elif op == "clrlwi":
            n = num(a[2])
            self.set(a[0], f"({self.get(a[1])} & 0x{(1 << (32 - n)) - 1:X})")
        elif op == "cntlzw":
            self.set(a[0], f"__cntlzw({self.get(a[1])})")
        elif op in ("fmr", "fneg", "fabs", "frsp"):
            e, t = self.reg[a[1]] if a[1] in self.reg else (self.get(a[1], "any"), self.reg[a[1]][1])
            e2 = {"fmr": e, "fneg": f"(-{e})", "fabs": f"__fabs({e})", "frsp": f"((float){e})"}[op]
            self.set(a[0], e2, "float" if op == "frsp" else t)
        elif op in ("fadds", "fsubs", "fmuls", "fdivs", "fadd", "fsub", "fmul", "fdiv"):
            t = "float" if op.endswith("s") else "double"
            x, y = self.get(a[1], t), self.get(a[2], t)
            sym = {"fadd": "+", "fsub": "-", "fmul": "*", "fdiv": "/"}[op.rstrip("s") if op != "fsubs" else "fsub"]
            self.set(a[0], f"({x} {sym} {y})", t)
        else:
            raise Unsupported(f"instruction {op}")

    def c_source(self) -> str:
        self.run()
        ints = sorted((int(r[1:]) for r in self.params if r[0] == "r"))
        flts = sorted((int(r[1:]) for r in self.params if r[0] == "f"))
        plist = [f"int a{i}" for i in range(3, (max(ints) + 1) if ints else 3)]
        plist += [f"{self.params.get(f'f{i}', 'float')} f{i}" for i in range(1, (max(flts) + 1) if flts else 1)]
        ret = "void"
        if "r3" in self.written:
            ret = "int"
            rexpr = self.get("r3", "int")
        elif "f1" in self.written:
            ret = self.reg["f1"][1]
            rexpr = self.reg["f1"][0]
        lines = [f"{ret} {self.name}({', '.join(plist) or 'void'}) {{"]
        lines += [f"    {s}" for s in self.stmts]
        if ret != "void":
            if "@HA:" in rexpr:
                raise Unsupported("returns a half address")
            lines.append(f"    return {rexpr};")
        lines.append("}")
        src = LOAD_MARK.sub("", "\n".join(lines))
        if "@HA:" in src:
            raise Unsupported("unpaired lis")
        return src


def parse_asm(path: str) -> list[tuple[str, list[tuple[str, str]], int]]:
    out, cur, body = [], None, []
    with open(path, encoding="utf-8") as f:
        for line in f:
            mm = FN_RE.match(line)
            if mm:
                cur, body = mm[1], []
                continue
            if line.startswith(".endfn"):
                if cur:
                    out.append((cur, body, 0))
                cur = None
                continue
            if cur and line.startswith(".L_"):
                body.append(("label", line.strip()))
                continue
            im = INS_RE.match(line)
            if im and cur:
                body.append((im[2], im[3].strip()))
    return out


def process_unit(args):
    unit, funcs, project_root, symbols, work, flags, variant = args
    project = m.Project(project_root)
    stats = {"leaves": 0, "unsupported": {}, "candidates": 0, "compile_dropped": 0, "mismatch": {}}
    cands = {}
    for name, body in funcs:
        if any(op == "label" for op, _ in body):
            continue
        stats["leaves"] += 1
        try:
            leaf = Leaf(name, body, symbols, project.read, inline=variant[0], extern_consts=variant[1])
            src = leaf.c_source()
        except Unsupported as e:
            key = re.sub(r"(r|f)\d+|0x[0-9A-Fa-f]+|\d+|\w+@\w+", "#", str(e))[:40]
            stats["unsupported"][key] = stats["unsupported"].get(key, 0) + 1
            continue
        except (KeyError, ValueError, IndexError) as e:
            stats["unsupported"]["parse"] = stats["unsupported"].get("parse", 0) + 1
            continue
        cands[name] = (src, leaf.externs)
    externs, keep = {}, {}
    for name, (src, ex) in cands.items():
        if any(externs.get(s, t) != t for s, t in ex.items()) or any(s in cands for s in ex):
            continue
        externs.update(ex)
        keep[name] = src
    stats["candidates"] = len(keep)
    if not keep:
        return unit, {}, {"stats": stats}

    def compile_judge(names, tag):
        src_path = os.path.join(work, f"{unit}_{tag}.c")
        lines = ["/* generated candidate */"]
        for sym, t in sorted(externs.items()):
            lines.append(f"extern {t} {sym};" if t != "char" else f"extern char {sym}[];")
        lines += [keep[n] for n in names]
        with open(src_path, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n\n".join(lines) + "\n")
        out = m.run_one(project, src_path, CC, flags, [], work, [], {})
        os.remove(src_path)
        return out

    matched = []

    def attempt(names, tag):
        results, obj, pairs = compile_judge(names, tag)
        if results is None:
            if len(names) == 1:
                stats["compile_dropped"] += 1
                return
            half = len(names) // 2
            attempt(names[:half], tag + "a")
            attempt(names[half:], tag + "b")
            return
        for r, (bs, ts) in zip(results, pairs):
            if r.matched:
                matched.append(bs.name)
            else:
                key = re.sub(r"0x[0-9A-Fa-f]+|[0-9a-f]{8}|\+\S+", "#", (r.problems or ["?"])[0])[:50]
                stats["mismatch"][key] = stats["mismatch"].get(key, 0) + 1
                ex = stats.setdefault("examples", {}).setdefault(key, [])
                if len(ex) < 2:
                    ex.append({"name": bs.name, "c": keep.get(bs.name, "")[:400], "diff": r.diff[:3]})
        if os.path.exists(obj):
            os.remove(obj)

    attempt(list(keep), "c")
    return unit, {n: keep[n] for n in matched}, {"externs": externs, "stats": stats}


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("project")
    ap.add_argument("spine")
    ap.add_argument("--decomp", default=os.path.join(os.path.dirname(__file__), "..", "..", "decomp"))
    ap.add_argument("--max", type=int, default=24, help="max instructions")
    ap.add_argument("--jobs", type=int, default=8)
    args = ap.parse_args(argv)
    project = m.Project(args.project)
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
                if e["file"].startswith("auto/"):
                    continue
                for fn in e.get("functions", []):
                    if fn.get("matched"):
                        done.add(int(fn["address"], 16))
    asm_dir = os.path.join(args.project, "build", "asm")
    work = os.path.join(args.project, "build", "autodecomp")
    os.makedirs(work, exist_ok=True)
    jobs = []
    for n in sorted(os.listdir(asm_dir)):
        if not n.endswith(".s"):
            continue
        unit = n[:-2]
        funcs = [(name, body) for name, body, _ in parse_asm(os.path.join(asm_dir, n))
                 if 1 <= len(body) <= args.max and symbols.get(name, (0,))[0] not in done]
        if funcs:
            jobs.append((unit, funcs))
    matched_all = {}
    totals = {"leaves": 0, "candidates": 0, "compile_dropped": 0, "unsupported": {}, "mismatch": {}}
    for flags, variant in [(f, v) for f in (GAME_FLAGS, ALT_FLAGS) for v in VARIANTS]:
        todo = [(u, [(nm, b) for nm, b in fs if nm not in matched_all]) for u, fs in jobs]
        todo = [(u, fs) for u, fs in todo if fs]
        with cf.ProcessPoolExecutor(max_workers=args.jobs) as pool:
            for unit, got, info in pool.map(process_unit, [(u, fs, args.project, symbols, work, flags, variant)
                                                          for u, fs in todo]):
                for n, src in got.items():
                    matched_all[n] = (unit, src, flags, info.get("externs", {}))
                st = info.get("stats", {})
                if flags == GAME_FLAGS and variant == VARIANTS[0]:
                    for k in ("leaves", "candidates", "compile_dropped"):
                        totals[k] += st.get(k, 0)
                    for k in ("unsupported", "mismatch"):
                        for kk, v in st.get(k, {}).items():
                            totals[k][kk] = totals[k].get(kk, 0) + v
                    for kk, v in st.get("examples", {}).items():
                        lst = totals.setdefault("examples", {}).setdefault(kk, [])
                        lst.extend(v[:2 - len(lst)])
        print(f"{flags} variant {variant}: {len(matched_all)} matched so far", flush=True)

    # write the matched functions, one file per (layer, unit, flags)
    groups = {}
    for n, (unit, src, flags, ex) in matched_all.items():
        lay = layer.get(n, "unresolved")
        groups.setdefault((lay, unit, flags), []).append((symbols[n][0], n, src, ex))
    out_root = os.path.join(args.decomp, "auto")
    if os.path.isdir(out_root):
        for root, _, files in os.walk(out_root):
            for fn in files:
                if fn.endswith(".c"):
                    os.remove(os.path.join(root, fn))
    for (lay, unit, flags), items in sorted(groups.items()):
        items.sort()
        suffix = "" if flags == GAME_FLAGS else "_p"
        d = os.path.join(out_root, lay)
        os.makedirs(d, exist_ok=True)
        ex_all = {}
        for _, _, _, ex in items:
            ex_all.update({s: t for s, t in ex.items() if any(re.search(rf"\b{re.escape(s)}\b", it[2]) for it in items)})
        head = [f"/* {unit}: straight-line leaf functions decompiled by tools/dolrecon/autodecomp.py.",
                f" * Each compiles byte-identical (dolrecon.match M1-M4).",
                f" *   @match cc={CC} flags={flags}",
                " * Inferred: parameters = registers the body reads (an unread parameter leaves no trace); result =",
                " * r3 -> int, f1 -> float/double, else void. Types of memory fields come from the load/store width. */",
                ""]
        # every function placed by ADDRESS: the file stays valid when the project's names improve
        body = ["".join(f"// @sym {n} = 0x{addr:08X}\n" for addr, n, _, _ in items)]
        for s, t in sorted(ex_all.items()):
            body.append(f"extern {t} {s};" if t != "char" else f"extern char {s}[];")
        for addr, n, src, _ in items:
            body.append(f"/* 0x{addr:08X} */\n{src}")
        with open(os.path.join(d, f"{unit}{suffix}.c"), "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(head) + "\n\n".join(body) + "\n")
    by_layer = {}
    for (lay, _, _), items in groups.items():
        by_layer[lay] = by_layer.get(lay, 0) + len(items)
    totals["unsupported"] = dict(sorted(totals["unsupported"].items(), key=lambda kv: -kv[1])[:25])
    totals["mismatch"] = dict(sorted(totals["mismatch"].items(), key=lambda kv: -kv[1])[:25])
    totals["examples"] = {k: v for k, v in totals.get("examples", {}).items() if k in totals["mismatch"]}
    report = {"matched": len(matched_all), "by_layer": by_layer, **totals}
    with open(os.path.join(work, "report.json"), "w", encoding="utf-8") as f:
        json.dump(report, f, indent=1)
    print(json.dumps(report, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
