"""Label-free SDK anchors: intrinsic evidence read from each function's own code, with no use of any manifest label.

    python -B sdk_anchors.py --manifest <platform_manifest.json> [--out DIR]

For every function in extract/functions.csv this resolves, by straight-line constant propagation inside each basic
block (lis/addi/ori/addis, r13 = 0x805EC500 and r2 = 0x805FA780 as fixed small-data bases):

- mmio      loads/stores to the hardware register window 0xCC000000-0xCC00FFFF, named by device (hardware fact,
            not a label: CP, PE, VI, PI, MI, DSP, DI, SI, EXI, AI, the GX write-gather pipe);
- spr       mfspr/mtspr numbers (HID0/2/4, GQRn, DEC, TB, SRR0/1, WPAR, DMAU/L, L2CR, ...), plus mfmsr/mtmsr, rfi and
            cache-block instructions;
- lowmem    accesses to the OS low-memory globals 0x80000000-0x800030FF;
- strings   C strings (>= 4 printable bytes) at resolved addresses in initialized data, with two derived hints:
              name_hint   an identifier that the string starts with and follows with "(" or ":" (an SDK report or
                          panic message names its function: "OSAllocFromHeap(): heap is not initialized");
              module_hint a source file name "xxx.c" or a "<< Dolphin SDK - XX" version banner.

Labels are used ONLY to score the anchors afterwards (validation.json):
- find:  how many functions with a hardware anchor (mmio or a platform-only SPR) are already platform rows, and the
         ones that are not (candidates for SDK code the manifest misses), each with its evidence;
- names: precision of name_hint against manifest names whose evidence is a symbol name;
- devices: the device -> manifest-service confusion table (the device map is checked, not trusted).
Nothing here assigns an identity; every output row carries its raw evidence for a human or a contract test.
"""
from __future__ import annotations

import argparse
import collections
import csv
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(SRC, "tools"))
from dolrecon import dol as dolmod  # noqa: E402

R13, R2 = 0x805EC500, 0x805FA780

# Hardware register blocks (GameCube memory map; public hardware documentation).
DEVICES = [(0xCC000000, 0xCC001000, "CP"), (0xCC001000, 0xCC002000, "PE"), (0xCC002000, 0xCC003000, "VI"),
           (0xCC003000, 0xCC004000, "PI"), (0xCC004000, 0xCC005000, "MI"), (0xCC005000, 0xCC006000, "DSP"),
           (0xCC006000, 0xCC006400, "DI"), (0xCC006400, 0xCC006800, "SI"), (0xCC006800, 0xCC006C00, "EXI"),
           (0xCC006C00, 0xCC007000, "AI"), (0xCC008000, 0xCC009000, "WGP")]
SPRS = {1: "XER", 8: "LR", 9: "CTR", 18: "DSISR", 19: "DAR", 22: "DEC", 25: "SDR1", 26: "SRR0", 27: "SRR1",
        272: "SPRG0", 273: "SPRG1", 274: "SPRG2", 275: "SPRG3", 282: "EAR", 268: "TBL", 269: "TBU", 284: "TBL_W",
        285: "TBU_W", 287: "PVR", 528: "IBAT0U", 529: "IBAT0L", 530: "IBAT1U", 531: "IBAT1L", 532: "IBAT2U",
        533: "IBAT2L", 534: "IBAT3U", 535: "IBAT3L", 536: "DBAT0U", 537: "DBAT0L", 538: "DBAT1U", 539: "DBAT1L",
        540: "DBAT2U", 541: "DBAT2L", 542: "DBAT3U", 543: "DBAT3L", 912: "GQR0", 913: "GQR1", 914: "GQR2",
        915: "GQR3", 916: "GQR4", 917: "GQR5", 918: "GQR6", 919: "GQR7", 920: "HID2", 921: "WPAR", 922: "DMAU",
        923: "DMAL", 936: "UMMCR0", 940: "UMMCR1", 952: "MMCR0", 953: "PMC1", 954: "PMC2", 956: "MMCR1",
        957: "PMC3", 958: "PMC4", 1008: "HID0", 1009: "HID1", 1010: "IABR", 1011: "HID4", 1013: "DABR",
        1017: "L2CR", 1019: "ICTC", 1020: "THRM1", 1021: "THRM2", 1022: "THRM3"}
# SPRs an application never needs (supervisor/system configuration); LR/CTR/XER and GQRs are excluded because compiled
# game code (paired-single setup, longjmp-like code) can touch them.
SYSTEM_SPRS = {s for s in SPRS.values() if s not in ("LR", "CTR", "XER") and not s.startswith("GQR")}
NAME_RE = re.compile(r"^\s*([A-Za-z_][A-Za-z0-9_]{2,})\s*(?:\(|:)")
FILE_RE = re.compile(r"([A-Za-z0-9_]+\.c)\b")
# Write-gather pipe stores by width: the order and widths of a function's FIFO writes are its command signature.
STORE_WIDTH = {38: "b", 39: "b", 44: "h", 45: "h", 36: "w", 37: "w", 52: "f", 53: "f", 54: "d", 55: "d"}
BANNER_RE = re.compile(r"<<\s*Dolphin SDK\s*-\s*([A-Za-z0-9_]+)")


def device(ea):
    for lo, hi, n in DEVICES:
        if lo <= ea < hi:
            return n
    return None


def read_string(image, ea):
    sec = image.section_at_address(ea)
    if sec is None or sec.kind != "data":
        return None
    out = bytearray()
    while len(out) < 200:
        try:
            b = image.read(ea + len(out), 1)[0]
        except Exception:
            return None
        if b == 0:
            break
        if not (32 <= b < 127 or b in (9, 10, 13)):
            return None
        out.append(b)
    else:
        return None
    s = out.decode("ascii")
    return s if len(s.strip()) >= 4 else None


def gx_registers(values):
    """Register ids a function writes through the pipe, where the code makes them constant: command 0x61 + a BP word
    (id = top byte), 0x10 + an XF header word (address = low 16 bits), 0x08 + a CP register byte."""
    regs, i = [], 0
    while i < len(values):
        width, _, v = values[i].partition(":")
        nxt = values[i + 1] if i + 1 < len(values) else None
        if width == "b" and v in ("0x61", "0x10", "0x8") and nxt:
            w2, _, v2 = nxt.partition(":")
            if v == "0x61" and w2 == "w":
                regs.append("BP:" + (f"{int(v2, 16) >> 24:#04x}" if v2 != "?" else "?"))
            elif v == "0x10" and w2 == "w":
                regs.append("XF:" + (f"{int(v2, 16) & 0xFFFF:#06x}" if v2 != "?" else "?"))
            elif v == "0x8" and w2 == "b":
                regs.append("CP:" + (v2 if v2 != "?" else "?"))
            i += 2
            continue
        i += 1
    return regs


def leaders_of(words, start):
    lead = {start}
    for at, w in words:
        op = w >> 26
        if op == 16:
            bd = w & 0xFFFC
            bd = bd - 0x10000 if bd & 0x8000 else bd
            lead.update(((at + bd) & 0xFFFFFFFF, at + 4))
        elif op == 18 and not (w & 1):
            li = w & 0x03FFFFFC
            li = li - 0x04000000 if li & 0x02000000 else li
            lead.update(((at + li) & 0xFFFFFFFF, at + 4))
        elif op == 19 or (op == 18 and (w & 1)):
            lead.add(at + 4)
    return lead


def simm(w):
    v = w & 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def anchors(image, start, size):
    words = []
    for at in range(start, start + size, 4):
        try:
            words.append((at, image.word(at)))
        except Exception:
            break
    lead = leaders_of(words, start)
    reg = {}
    top = {}                                                           # registers whose top byte alone is known
    out = {"mmio": collections.Counter(), "spr": collections.Counter(), "lowmem": collections.Counter(),
           "strings": [], "wgp": [], "wgp_values": []}
    seen = set()

    def note_address(ea, op=None):
        if 0xCC000000 <= ea < 0xCC010000:
            out["mmio"][device(ea) or f"{ea:#x}"] += 1
            if device(ea) == "WGP" and op in STORE_WIDTH:
                out["wgp"].append(STORE_WIDTH[op])
        elif 0x80000000 <= ea < 0x80003100:
            out["lowmem"][f"{ea:#010x}"] += 1
        elif ea not in seen:
            seen.add(ea)
            s = read_string(image, ea)
            if s is not None:
                out["strings"].append(s)

    for at, w in words:
        if at in lead:
            reg = {}
            top = {}
        reg[13], reg[2] = R13, R2
        op, d, a = w >> 26, (w >> 21) & 31, (w >> 16) & 31
        xo = (w >> 1) & 0x3FF
        if op == 15:                                                   # addis / lis
            base = 0 if a == 0 else reg.get(a)
            if base is None:
                reg.pop(d, None)
            else:
                reg[d] = (base + (simm(w) << 16)) & 0xFFFFFFFF
        elif op == 14:                                                 # addi / li
            base = 0 if a == 0 else reg.get(a)
            if base is None:
                reg.pop(d, None)
            else:
                reg[d] = (base + simm(w)) & 0xFFFFFFFF
                if a != 0:
                    note_address(reg[d])
        elif op == 24:                                                 # ori rA,rS,uimm
            if d in reg:
                reg[a] = reg[d] | (w & 0xFFFF)
                note_address(reg[a])
            else:
                reg.pop(a, None)
        elif 32 <= op <= 55:                                           # D-form loads/stores
            if a in reg:
                ea = (reg[a] + simm(w)) & 0xFFFFFFFF
                note_address(ea, op)
                if device(ea) == "WGP" and op in STORE_WIDTH and op not in (52, 53, 54, 55):
                    v = reg.get(d)                                     # the stored constant, when known
                    if v is None and d in top:                         # only the register id byte is known
                        v = top[d] << 24
                    out["wgp_values"].append(f"{STORE_WIDTH[op]}:{v:#x}" if v is not None else f"{STORE_WIDTH[op]}:?")
            if op in (32, 33, 34, 35, 40, 41, 42, 43, 46):             # integer loads (and update forms) write rD
                reg.pop(d, None)
            if op in (33, 35, 37, 39, 41, 43, 45, 47, 49, 51, 53, 55):  # update forms write rA
                reg.pop(a, None)
        elif op == 31:
            spr = ((w >> 16) & 31) | (((w >> 11) & 31) << 5)
            if xo in (339, 467):                                       # mfspr / mtspr
                if spr not in (1, 8, 9):                               # XER/LR/CTR: every compiled function
                    out["spr"][SPRS.get(spr, f"spr{spr}")] += 1
            elif xo == 371:
                out["spr"]["TB"] += 1
            elif xo == 83:
                out["spr"]["MSR(mf)"] += 1
            elif xo == 146:
                out["spr"]["MSR(mt)"] += 1
            elif xo in (86, 54, 470, 982, 1014, 246, 278):             # dcbf dcbst dcbi icbi dcbz dcbt dcbtst
                out["spr"]["cache-op"] += 1
            # stores (stwx...) and cache ops write nothing; logical ops write rA; the rest write rD
            if xo in (28, 60, 124, 284, 316, 412, 444, 476, 24, 536, 792, 824, 26, 922, 954):
                reg.pop(a, None)
            else:
                reg.pop(d, None)
        elif op == 19 and xo == 50:
            out["spr"]["rfi"] += 1
        elif op == 4 and xo == 1014:
            out["spr"]["cache-op"] += 1                                # dcbz_l
        elif op == 20:                                                 # rlwimi rA,rS,SH,MB,ME
            sh, mb, me = (w >> 11) & 31, (w >> 6) & 31, (w >> 1) & 31
            if mb == 0 and me >= 7 and d in reg:                       # inserts the whole top byte from a constant
                rot = ((reg[d] << sh) | (reg[d] >> (32 - sh))) & 0xFFFFFFFF if sh else reg[d]
                top[a] = rot >> 24
            elif mb > 7 and a in top or mb > me:
                pass                                                   # leaves the top byte as it was
            else:
                top.pop(a, None)
            reg.pop(a, None)
        elif op in (21, 23, 25, 26, 27, 28, 29):                       # rlwinm, rlwnm, oris, xori, xoris, andi(s).: rA
            reg.pop(a, None)
        elif op not in (10, 11, 16, 17, 18, 19, 3):                    # compares, branches, sc, tw write no GPR
            reg.pop(d, None)
        # the register byte: constants set it, rlwimi of a constant into bits 0-7 sets it (above), ori/rlwimi below
        # bit 8 keep it, every other GPR write forgets it
        if op == 31:
            target = a if xo in (28, 60, 124, 284, 316, 412, 444, 476, 24, 536, 792, 824, 26, 922, 954) else d
            if xo in (151, 215, 407, 662, 918, 150, 183, 247, 86, 54, 470, 982, 1014, 246, 278, 467, 0, 32):
                target = None                                          # stores, cache ops, mtspr, cmp/cmpl
        elif op in (20, 21, 23, 24, 25, 26, 27, 28, 29):
            target = a
        elif op in (10, 11, 16, 17, 18, 19, 3) or op in (36, 37, 38, 39, 44, 45, 47, 52, 53, 54, 55):
            target = None
        else:
            target = d
        if target is not None and not (op == 20 and target in top):
            if target in reg:
                top[target] = reg[target] >> 24
            elif op == 24 and d in top:                                # ori rA,rS: the low half only
                top[target] = top[d]
            else:
                top.pop(target, None)
        if op == 18 and (w & 1):                                       # bl: volatiles die
            for r in (0, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12):
                reg.pop(r, None)
                top.pop(r, None)
    hints = {"wgp_signature": "".join(out["wgp"]), "gx_registers": gx_registers(out["wgp_values"]),
             "name_hint": sorted({m.group(1) for s in out["strings"] for m in [NAME_RE.match(s)] if m}),
             "module_hint": sorted({m for s in out["strings"] for m in FILE_RE.findall(s)}
                                   | {f"banner:{m}" for s in out["strings"] for m in BANNER_RE.findall(s)})}
    return out, hints


def is_terminator(w):
    """b (no link), unconditional blr / bctr (no link), rfi: control never falls through to the next word."""
    op = w >> 26
    if op == 18:
        return not (w & 1) or bool(w & 2)                              # b, or an absolute branch (bla 0x60: vectors)
    if op == 19:
        xo, bo, lk = (w >> 1) & 0x3FF, (w >> 21) & 31, w & 1
        return xo == 50 or (xo in (16, 528) and (bo & 0x14) == 0x14 and not lk)
    return False


def rejoin(image, funcs, callers):
    """Function pieces the function list split off a real function, from code alone. Returns {piece: (root, rule)}.

    fallthrough  the previous adjacent entry does not end in a terminator, so execution runs into this one;
    jumptable    the entry is a target of a lis/addi + lwzx + mtctr + bctr table inside another entry;
    inside       the entry starts inside the address range of an earlier entry;
    branch-only  nothing calls the entry (no bl); every reference is a plain branch from one other entry.
    Pieces chain to the root of the entry they continue."""
    order = sorted(a for a, (_, size) in funcs.items() if size > 0)
    end = {a: a + funcs[a][1] for a in order}
    parent = {}

    def last_word(a):
        try:
            return image.word(end[a] - 4)
        except Exception:
            return None

    def padding_only(a):
        try:
            return all(image.word(at) in (0, 0x60000000) for at in range(a, end[a], 4))
        except Exception:
            return True

    def has_prologue(a):
        try:
            first = [image.word(a + 4 * i) for i in range(2)]
        except Exception:
            return False
        return any(w == 0x7C0802A6 or (w & 0xFFFF0000) == 0x94210000 for w in first)   # mflr r0 / stwu r1,-n(r1)

    outer = None                                                       # inside: the entry starts within another
    for a in order:
        if outer is not None and outer < a < end[outer]:
            parent[a] = (outer, "inside")
            continue
        outer = a
    for prev, a in zip(order, order[1:]):
        if a in parent:
            continue
        if end[prev] == a and not padding_only(prev) and not padding_only(a):
            w = last_word(prev)
            if w is not None and not is_terminator(w) and w != 0:
                parent[a] = (prev, "fallthrough")
    starts = set(order)
    branch_refs = collections.defaultdict(set)
    for a in order:
        words = []
        for at in range(a, end[a], 4):
            try:
                words.append((at, image.word(at)))
            except Exception:
                break
        reg = {}
        for k, (at, w) in enumerate(words):
            op = w >> 26
            if op == 18 and not (w & 1):
                li = w & 0x03FFFFFC
                li = li - 0x04000000 if li & 0x02000000 else li
                t = (at + li) & 0xFFFFFFFF
                if t in starts and t != a:
                    branch_refs[t].add(a)
            elif op == 16 and not (w & 1):
                t = (at + simm(w & 0xFFFC)) & 0xFFFFFFFF
                if t in starts and t != a:
                    branch_refs[t].add(a)
            elif op == 15 and ((w >> 16) & 31) == 0:
                reg[(w >> 21) & 31] = (w & 0xFFFF) << 16
            elif op == 14 and ((w >> 16) & 31) in reg:
                reg[(w >> 21) & 31] = (reg[(w >> 16) & 31] + simm(w)) & 0xFFFFFFFF
            elif op == 31 and ((w >> 1) & 0x3FF) == 23:                # lwzx rD, rA, rB: a table load
                base = reg.get((w >> 16) & 31) or reg.get((w >> 11) & 31)
                tail = [x for _, x in words[k + 1:k + 4]]
                if base is not None and 0x7C0903A6 | (((w >> 21) & 31) << 21) in tail and 0x4E800420 in tail:
                    sec = image.section_at_address(base)
                    if sec is None or sec.kind != "data":
                        continue
                    for i in range(256):
                        try:
                            t = image.word(base + 4 * i)
                        except Exception:
                            break
                        if not (a - 0x10000 <= t < a + 0x10000) or t & 3:
                            break
                        if t in starts and t != a and not callers.get(t):
                            parent.setdefault(t, (a, "jumptable"))
    for t, refs in branch_refs.items():
        # a tail call (plain b) also reaches a real function: only a target without its own frame set-up is a piece
        if t not in parent and not callers.get(t) and len(refs) == 1 and not has_prologue(t)                 and not padding_only(t):
            parent[t] = (next(iter(refs)), "branch-only")

    out = {}
    for a, (p, rule) in parent.items():
        seen, root = {a}, p
        while root in parent and root not in seen:
            seen.add(root)
            root = parent[root][0]
        out[a] = (root, rule)
    return out


def gaps(funcs, labels, callers):
    """Functions with no label that sit strictly inside a run of ONE labeled service (nearest labeled neighbour on each
    side has the same service) and that no function outside the platform calls. Address-derived: TENTATIVE only."""
    order = sorted(funcs)
    labeled = [a for a in order if a in labels]
    out = {}
    idx = {a: k for k, a in enumerate(order)}
    for i in range(len(labeled) - 1):
        lo, hi = labeled[i], labeled[i + 1]
        if labels[lo] != labels[hi]:
            continue
        for a in order[idx[lo] + 1:idx[hi]]:
            if a in labels:
                continue
            outside = [c for c in callers.get(a, ()) if c not in labels]
            if not outside:
                out[a] = labels[lo]
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--manifest", required=True)
    ap.add_argument("--dol", default=os.path.join(SRC, "..", "game", "sys", "main.dol"))
    ap.add_argument("--functions", default=os.path.join(SRC, "extract", "functions.csv"))
    ap.add_argument("--graph", help="live_graph.txt: lists anchored functions that are compiled but not platform rows")
    ap.add_argument("--out", default=os.path.join(SRC, ".build", "puzzle"))
    args = ap.parse_args(argv)
    image = dolmod.load_file(args.dol)
    funcs = {int(r["addr"]): (r["name"], int(r["size"])) for r in csv.DictReader(open(args.functions, encoding="utf-8"))}
    man = {int(e["address"], 16): e for e in json.load(open(args.manifest, encoding="utf-8"))["entries"]}

    rows = {}
    for a, (name, size) in sorted(funcs.items()):
        if size <= 0:
            continue
        ev, hints = anchors(image, a, size)
        if any(ev[k] for k in ("mmio", "spr", "lowmem", "strings")):
            rows[a] = {"address": f"{a:#010x}", "csv_name": name,
                       "manifest": ({"service": man[a]["service"], "name": man[a]["name"],
                                     "evidence": man[a]["evidence"]} if a in man else None),
                       "mmio": dict(ev["mmio"]), "spr": dict(ev["spr"]), "lowmem": dict(ev["lowmem"]),
                       "strings": ev["strings"], **hints}
    live = {}
    if args.graph:
        for line in open(args.graph, encoding="utf-8"):
            p = line.split()
            if len(p) >= 5 and p[0] == "row":
                live[int(p[1], 16)] = f"{p[3]} {p[4]}"

    # ---- validation (labels used only here, to score) ----
    def hardware(r):
        return bool(r["mmio"]) or any(s in SYSTEM_SPRS or s in ("rfi", "MSR(mt)") for s in r["spr"])
    hw = [r for r in rows.values() if hardware(r)]
    hw_in = [r for r in hw if r["manifest"]]
    find_candidates = [r for r in hw if not r["manifest"]]
    truth = {a: e["name"] for a, e in man.items() if e["evidence"] == "name"}
    hinted = [(a, r) for a, r in rows.items() if r["name_hint"] and a in truth]
    exact = [(a, r) for a, r in hinted if truth[a] in r["name_hint"]]
    dev_conf = collections.defaultdict(collections.Counter)
    for a, r in rows.items():
        if r["manifest"] and r["manifest"]["evidence"] == "name":
            for dv in r["mmio"]:
                dev_conf[dv][r["manifest"]["service"]] += 1
    # ---- manifest gaps, validated by hiding labels (leak-free): hide every 10th labeled row, re-find it ----
    callers = collections.defaultdict(set)
    for r in csv.DictReader(open(args.functions, encoding="utf-8")):
        for c in r["callees"].split():
            callers[int(c, 16)].add(int(r["addr"]))
    labels = {a: e["service"] for a, e in man.items() if e["service"] != "sdk:unidentified" and a in funcs}
    platform_layers = {a for a, e in man.items() if e["layer"] in ("sdk",)}
    hidden = {a for k, a in enumerate(sorted(a for a in labels if a in platform_layers)) if k % 10 == 0}
    seen_lab = {a: l for a, l in labels.items() if a not in hidden}
    refound = gaps({a: 1 for a in funcs}, seen_lab, callers)
    rf = [a for a in hidden if a in refound]
    gap_validation = {"hidden_sdk_rows": len(hidden), "refound": len(rf),
                      "recall": round(len(rf) / len(hidden), 4) if hidden else None,
                      "service_correct": sum(refound[a] == labels[a] for a in rf),
                      "unlabeled_finds_not_scorable": sum(1 for a in refound if a not in hidden)}
    # ---- split functions rejoined (code only); scored against symbol names (a symbol starts a real function) ----
    joined = rejoin(image, funcs, callers)
    named_starts = {a for a, e in man.items() if e["evidence"] == "name" and "__sub" not in e["name"]}
    false_joins = sorted(a for a in joined if a in named_starts)
    join_rows = []
    for a, (root, rule) in sorted(joined.items()):
        rm, pm = man.get(root), man.get(a)
        join_rows.append({"piece": f"{a:#010x}", "piece_csv_name": funcs[a][0], "root": f"{root:#010x}",
                          "root_csv_name": funcs[root][0], "rule": rule,
                          "root_manifest": rm and {"name": rm["name"], "service": rm["service"]},
                          "piece_manifest": pm and {"name": pm["name"], "service": pm["service"]},
                          "generator_row": None})
    manifest_gaps = gaps({a: 1 for a in funcs}, labels, callers)
    gap_rows = [{"address": f"{a:#010x}", "csv_name": funcs[a][0], "size": funcs[a][1], "run_service": svc,
                 "generator_row": None, "anchors": rows.get(a, {}).get("mmio") or None,
                 "grade": "TENTATIVE (address run)"} for a, svc in sorted(manifest_gaps.items())]
    for r in find_candidates:
        r["generator_row"] = live.get(int(r["address"], 16))
    compiled_missing = [r for r in find_candidates if r["generator_row"] == "body live"]
    for r in gap_rows:
        r["generator_row"] = live.get(int(r["address"], 16))
    for r in join_rows:
        r["generator_row"] = live.get(int(r["piece"], 16))
    platform_pieces = [r for r in join_rows if r["root_manifest"] or r["piece_manifest"]]
    by_sig = collections.defaultdict(list)
    for r in rows.values():
        if r["wgp_signature"] and r["mmio"].keys() == {"WGP"}:
            by_sig[r["wgp_signature"]].append(r["address"])
    mod_conf = collections.defaultdict(collections.Counter)
    for a, r in rows.items():
        if r["manifest"] and r["manifest"]["evidence"] == "name":
            for m in r["module_hint"]:
                mod_conf[m][r["manifest"]["service"]] += 1
    new_names = [r for a, r in rows.items()
                 if r["name_hint"] and (a not in man or man[a]["evidence"] != "name")]
    validation = {
        "functions_scanned": len(funcs), "functions_with_any_anchor": len(rows),
        "find": {"hardware_anchored": len(hw), "already_platform_rows": len(hw_in),
                 "precision_of_hardware_anchor_as_platform": round(len(hw_in) / len(hw), 4) if hw else None,
                 "not_in_manifest": len(find_candidates),
                 "compiled_but_not_platform_rows": len(compiled_missing) if live else None},
        "names": {"truth_functions_with_a_name_hint": len(hinted), "hint_equals_symbol_name": len(exact),
                  "precision": round(len(exact) / len(hinted), 4) if hinted else None,
                  "mismatches": [{"address": r["address"], "truth": truth[a], "hints": r["name_hint"]}
                                 for a, r in hinted if truth[a] not in r["name_hint"]]},
        "rejoin": {"pieces": len(join_rows), "by_rule": dict(collections.Counter(r["rule"] for r in join_rows)),
                   "negative_control_symbol_named_starts_joined": len(false_joins),
                   "symbol_named_starts_checked": len(named_starts),
                   "false_joins": [f"{a:#010x} {man[a]['name']}" for a in false_joins],
                   "platform_pieces": len(platform_pieces),
                   "platform_pieces_missing_from_manifest": sum(1 for r in platform_pieces if not r["piece_manifest"]),
                   "platform_pieces_missing_and_compiled": sum(1 for r in platform_pieces if not r["piece_manifest"]
                                                               and r["generator_row"] == "body live"),
                   "pieces_with_their_own_manifest_name": sum(1 for r in platform_pieces if r["piece_manifest"]
                                                              and r["root_manifest"]
                                                              and r["piece_manifest"]["name"] != r["root_manifest"]["name"])},
        "manifest_gaps": {"found": len(gap_rows),
                          "compiled": sum(1 for r in gap_rows if r["generator_row"] == "body live") if live else None,
                          "by_service": dict(collections.Counter(r["run_service"] for r in gap_rows)),
                          "validation_hide_every_10th_sdk_row": gap_validation},
        "module_hint_vs_manifest_service": {k: dict(v) for k, v in sorted(mod_conf.items())},
        "wgp_signature_groups": {k: len(v) for k, v in sorted(by_sig.items())},
        "device_vs_manifest_service": {k: dict(v) for k, v in sorted(dev_conf.items())},
    }
    os.makedirs(args.out, exist_ok=True)
    dump = lambda n, o: json.dump(o, open(os.path.join(args.out, n), "w", encoding="utf-8", newline="\n"),
                                  indent=1, sort_keys=True)
    dump("anchors.json", list(rows.values()))
    dump("anchor_find_candidates.json", find_candidates)
    dump("anchor_name_candidates.json", new_names)
    dump("anchor_compiled_missing.json", compiled_missing)
    dump("anchor_manifest_gaps.json", gap_rows)
    dump("anchor_rejoined_pieces.json", platform_pieces)
    dump("anchor_wgp_groups.json", {k: v for k, v in sorted(by_sig.items())})
    dump("anchor_validation.json", validation)
    print(json.dumps({k: v for k, v in validation.items() if k not in ("names", "device_vs_manifest_service")},
                     indent=1))
    print("names:", {k: v for k, v in validation["names"].items() if k != "mismatches"})
    return 0


if __name__ == "__main__":
    sys.exit(main())
