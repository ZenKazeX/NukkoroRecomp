"""frC precision at the single-precision multiply sites (docs/SHADOW_PLAYER_COVERAGE_AUDIT.md §6).

    python -B -m dolrecon.player_frc <dolrecon root> <callgraph.json> <player_inventory.csv> <out.csv>
        [--functions <hex,...>] [--probe <verify.csv.frc>]... [--edges <call census counts.csv>]...

fmuls / fmadds / fmsubs / fnmadds / fnmsubs and ps_mul / ps_muls0/1 / ps_madd / ps_madds0/1 / ps_msub / ps_nmadd /
ps_nmsub "require all operands to be single-precision" (Gekko manual PAGE 82); a frC that is not leaves the result to
the implementation (Dolphin rounds it to a 25-bit significand; neither engine here does). For every site this walks
back to the instructions that can define frC there and classifies them:

  single     lfs / lfsu / lfsx / lfsux, psq_l*, any opcode-59 result, paired-single arithmetic, frsp, fres / ps_res
  const      lfd from the small-data area (r2 / r13) whose 8 bytes, read from main.dol, are a single-representable
             double (single) or not (wide)
  double     opcode-63 arithmetic (fadd, fmul, fmadd, fdiv, ...) - a double-precision result, wide unless proven
  memory     lfd from anywhere else - whatever was stored there
  param      the function's entry (a caller's f1..f13): resolved through every static caller's call site
  return     a call's f1 result: resolved through the callee's returns
  copies     fmr, fneg, fabs, fnabs, ps_mr, ps_neg, ps_abs, ps_nabs, ps_merge*, fsel / ps_sel: followed to their sources

Verdict per site: PROVEN_UNAFFECTED (every reaching definition is single / single const), AFFECTED (a reaching
definition is a wide constant, or a probe run observed a frC that was not a single there), UNRESOLVED (anything else:
double arithmetic, memory, an unresolved caller or callee, the depth limit).
"""
from __future__ import annotations

import bisect
import collections
import csv
import json
import pathlib
import struct
import sys

from dolrecon import dol, game_map
from dolrecon.player_audit import fp_class

DEPTH = 5  # caller / callee hops


def frc_of(w: int) -> int:
    return (w >> 6) & 31


class Code:
    def __init__(self, root: pathlib.Path, cg_path: str):
        self.image = dol.load_file(str(root / "pal-v30/main.dol"))
        self.fn = game_map.load_functions(root)
        self.starts = sorted(self.fn)
        n2a = {r["name"]: a for a, r in self.fn.items()}
        self.callers = collections.defaultdict(set)
        self.virtual_callers = collections.defaultdict(set)  # resolved virtual calls (callgraph kind "virtual")
        for e in json.load(open(cg_path, encoding="utf-8"))["edges"]:
            s, t = n2a.get(e["from"]), n2a.get(e["to"])
            if s is None or t is None:
                continue
            if e["kind"] in ("call", "tail"):
                self.callers[t].add(s)
            elif e["kind"] == "virtual":
                self.virtual_callers[t].add(s)
        self.memo = {}
        self.observed = collections.defaultdict(set)  # callee -> indirect call sites seen at run time

    def add_observed_edges(self, path: str) -> None:
        """The runtime call census (GEKKO_CALL_CENSUS_COUNTS): `edge,callee,return address,count` rows. Only indirect
        sites (bctrl / blrl) are kept; direct calls are already in the static graph."""
        for row in csv.reader(open(path, encoding="utf-8")):
            if row and row[0] == "edge":
                site = int(row[2], 16) - 4
                if self.image.word(site) in (0x4E800421, 0x4E800021):
                    self.observed[int(row[1], 16)].add(site)

    def owner(self, pc: int) -> int | None:
        i = bisect.bisect_right(self.starts, pc) - 1
        return self.starts[i] if i >= 0 and pc < self.starts[i] + int(self.fn[self.starts[i]]["size"]) else None

    def words(self, a: int) -> list[int]:
        return [self.image.word(a + k) for k in range(0, int(self.fn[a]["size"]), 4)]

    def preds(self, a: int) -> dict[int, list[int]]:
        """Instruction-level predecessors inside function a (fall-through and in-function branches)."""
        if ("p", a) in self.memo:
            return self.memo[("p", a)]
        ws = self.words(a)
        end = a + 4 * len(ws)
        p = collections.defaultdict(list)
        for k, w in enumerate(ws):
            pc = a + 4 * k
            op = w >> 26
            uncond_out = False
            if op == 18:  # b / bl
                tgt = (pc if not (w & 2) else 0) + (((w & 0x03FFFFFC) ^ 0x02000000) - 0x02000000)
                if not (w & 1):
                    uncond_out = True
                    if a <= tgt < end:
                        p[tgt].append(pc)
            elif op == 16:  # bc
                tgt = (pc if not (w & 2) else 0) + (((w & 0xFFFC) ^ 0x8000) - 0x8000)
                if not (w & 1) and a <= tgt < end:
                    p[tgt].append(pc)
                if ((w >> 21) & 0x14) == 0x14:  # branch always
                    uncond_out = True
            elif op == 19 and ((w >> 1) & 1023) in (16, 528) and not (w & 1) and ((w >> 21) & 0x14) == 0x14:
                uncond_out = True  # blr / bctr
            if not uncond_out and pc + 4 < end:
                p[pc + 4].append(pc)
        self.memo[("p", a)] = p
        return p

    def const_double(self, w: int) -> str:
        d = (w >> 21) & 31  # unused; the displacement and base matter
        ra = (w >> 16) & 31
        disp = ((w & 0xFFFF) ^ 0x8000) - 0x8000
        base = {2: 0x805FA780, 13: 0x805EC500}.get(ra)
        if base is None:
            return "memory"
        raw = self.image.read(base + disp, 8)
        v = struct.unpack(">d", raw)[0]
        if v != v or v in (float("inf"), float("-inf")) or v == 0.0:
            return "single-const"
        f = struct.unpack(">f", struct.pack(">f", v))[0] if abs(v) < 3.4e38 else None
        return "single-const" if f == v else f"wide-const {v!r}"

    def classify_def(self, w: int, pc: int, a: int, depth: int, seen: set) -> set[str]:
        """What the FPR written by instruction w (at pc, in function a) can hold."""
        op = w >> 26
        if op in (48, 49, 56, 57, 59):
            return {"single"}
        if op in (50, 51):
            return {self.const_double(w)}
        if op == 4:
            x10, x5 = (w >> 1) & 1023, (w >> 1) & 31
            if (w >> 1) & 63 in (6, 38):  # psq_lx / psq_lux (6-bit extended opcode)
                return {"single"}
            if x10 in (72, 40, 264, 136, 528, 560, 592, 624):  # ps_mr ps_neg ps_abs ps_nabs ps_merge*
                srcs = [(w >> 11) & 31] + ([(w >> 16) & 31] if x10 >= 528 else [])
                return set().union(*(self.reaching(r, pc, a, depth, seen) for r in srcs))
            if x5 == 23:  # ps_sel
                return self.reaching((w >> 11) & 31, pc, a, depth, seen) | self.reaching((w >> 6) & 31, pc, a, depth, seen)
            return {"single"}  # paired-single arithmetic rounds to single
        if op == 31:
            x10 = (w >> 1) & 1023
            if x10 in (535, 567):
                return {"single"}
            if x10 in (599, 631):
                return {"memory"}
            return {"unknown-def"}
        if op == 63:
            x10, x5 = (w >> 1) & 1023, (w >> 1) & 31
            if x10 == 12:
                return {"single"}  # frsp
            if x10 in (72, 40, 264, 136):
                return self.reaching((w >> 11) & 31, pc, a, depth, seen)
            if x5 == 23:  # fsel
                return self.reaching((w >> 11) & 31, pc, a, depth, seen) | self.reaching((w >> 6) & 31, pc, a, depth, seen)
            if x10 in (14, 15):
                return {"integer-bits"}
            if x10 == 583:
                return {"fpscr-bits"}
            if x5 == 24 or x5 == 26:
                return {"estimate"}
            return {"double"}
        return {"unknown-def"}

    def reaching(self, reg: int, pc: int, a: int, depth: int, seen: set, through: bool = False) -> set[str]:
        """Definitions of FPR reg that reach instruction pc (exclusive) in function a. through: a callee's return
        value is being resolved - reaching the entry means the caller's own register passes through ("passthrough")."""
        key = (reg, pc, a, through)
        if key in seen:
            return set()
        seen.add(key)
        preds = self.preds(a)
        out = set()
        stack, visited = list(preds.get(pc, [])), set()
        entry = (lambda: {"passthrough"}) if through else (lambda: self.param(reg, a, depth, seen))
        if pc == a:
            out |= entry()
        while stack:
            q = stack.pop()
            if q in visited:
                continue
            visited.add(q)
            w = self.image.word(q)
            op = w >> 26
            is_call = (op == 18 and (w & 1)) or (op == 19 and (w & 1)) or (op == 16 and (w & 1))
            writes = (op in (48, 49, 50, 51, 56, 57, 59, 63) or (op == 4 and ((w >> 1) & 1023) not in
                      (0, 32, 64, 96, 1014) and ((w >> 1) & 63) not in (7, 39)) or
                      (op == 31 and ((w >> 1) & 1023) in (535, 567, 599, 631)))
            if writes and ((w >> 21) & 31) == reg and not (op == 63 and ((w >> 1) & 1023) in (0, 32, 64, 38, 70, 134, 711)):
                out |= self.classify_def(w, q, a, depth, seen)
                continue
            if is_call and reg <= 13:
                out |= self.ret(w, q, a, depth, seen) if reg == 1 else {"clobbered-by-call"}
                continue
            if q == a:
                out |= entry()
            stack.extend(preds.get(q, []))
        return out

    def param(self, reg: int, a: int, depth: int, seen: set) -> set[str]:
        if reg > 13 or depth <= 0:
            return {"param-unresolved"}
        callers = self.callers.get(a, set())
        virtual = self.virtual_callers.get(a, set())
        observed = self.observed.get(a, set())
        if not callers and not virtual and not observed:
            return {"param-no-static-caller"}
        out = set()
        # Indirect callers seen at run time: their definitions count, tagged - a caller never observed may exist.
        for site in observed:
            c = self.owner(site)
            if c is not None:
                out |= {"observed-indirect"} | self.reaching(reg, site, c, depth - 1, seen)
        for c in callers:
            for k, w in enumerate(self.words(c)):
                if (w >> 26) == 18 and (w & 1) and not (w & 2):
                    if c + 4 * k + (((w & 0x03FFFFFC) ^ 0x02000000) - 0x02000000) == a:
                        out |= self.reaching(reg, c + 4 * k, c, depth - 1, seen)
        # A resolved virtual caller does not say which bctrl reaches a: every bctrl in it is taken (a superset).
        for c in virtual:
            for k, w in enumerate(self.words(c)):
                if w == 0x4E800421:
                    out |= self.reaching(reg, c + 4 * k, c, depth - 1, seen)
        return out or {"param-no-call-site"}

    def ret(self, w: int, site: int, a: int, depth: int, seen: set) -> set[str]:
        """f1 after the call at site (in function a): the callee's f1 at each of its returns; where the callee leaves
        f1 untouched, the f1 that reached the call."""
        if (w >> 26) != 18 or (w & 2) or depth <= 0:
            return {"return-unresolved"}
        callee = site + (((w & 0x03FFFFFC) ^ 0x02000000) - 0x02000000)
        if callee not in self.fn:
            return {"return-unresolved"}
        # The callee's own return definitions do not depend on the call site: computed once, with their own
        # visited set (a shared one made a second call into the same callee look empty).
        key = ("ret", callee)
        if key not in self.memo:
            own = set()
            for k, x in enumerate(self.words(callee)):
                if x == 0x4E800020:  # blr
                    own |= self.reaching(1, callee + 4 * k, callee, depth - 1, set(), through=True)
            self.memo[key] = own
        out = set(self.memo[key])
        if not out:
            return {"return-unresolved"}
        if "passthrough" in out:
            out.discard("passthrough")
            out |= self.reaching(1, site, a, depth - 1, seen)
        return out

def verdict(defs: set[str], observed_wide: int) -> str:
    if observed_wide or any(d.startswith("wide-const") for d in defs):
        return "AFFECTED"
    if defs and defs <= {"single", "single-const"}:
        return "PROVEN_UNAFFECTED"
    return "UNRESOLVED"


def basis(defs: set[str]) -> str:
    """For an UNRESOLVED site: whether every definition is single once the observed indirect callers are counted."""
    rest = defs - {"observed-indirect"}
    if "observed-indirect" in defs and rest and rest <= {"single", "single-const"}:
        return "single on every observed caller"
    return ""


def main(argv=None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    root, cg, inv_path, out = pathlib.Path(argv[0]), argv[1], argv[2], argv[3]
    opts = collections.defaultdict(list)
    for i in range(4, len(argv), 2):
        opts[argv[i]].append(argv[i + 1])
    code = Code(root, cg)
    for p in opts["--edges"]:
        code.add_observed_edges(p)
    inv = {int(r["address"], 16): r for r in csv.DictReader(open(inv_path, encoding="utf-8"))}
    targets = set(inv)
    for spec in opts["--functions"]:
        targets |= {int(t, 16) for t in spec.split(",") if t}
    probe = collections.Counter()
    for p in opts["--probe"]:
        for r in csv.DictReader(open(p, encoding="utf-8")):
            probe[int(r["pc"], 16)] += int(r["wide"])
    rows = []
    for a in sorted(targets):
        if a not in code.fn:
            continue
        for k, w in enumerate(code.words(a)):
            if fp_class(w) != "single_mul":
                continue
            pc = a + 4 * k
            defs = code.reaching(frc_of(w), pc, a, DEPTH, set())
            rows.append({"pc": f"0x{pc:08X}", "function": f"0x{a:08X}", "name": code.fn[a]["name"],
                         "member": int(a in inv), "frC": f"f{frc_of(w)}", "word": f"{w:08X}",
                         "definitions": ";".join(sorted(defs)), "observed_wide": probe[pc],
                         "verdict": verdict(defs, probe[pc]), "basis": basis(defs)})
    with open(out, "w", newline="", encoding="utf-8") as f:
        wr = csv.DictWriter(f, fieldnames=list(rows[0]), lineterminator="\n")
        wr.writeheader()
        wr.writerows(rows)
    c = collections.Counter((r["member"], r["verdict"]) for r in rows)
    print(f"sites {len(rows)}; " + ", ".join(f"{'member' if m else 'outside'} {v}: {n}" for (m, v), n in sorted(c.items())))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
