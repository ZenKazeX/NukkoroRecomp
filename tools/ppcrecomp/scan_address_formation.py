#!/usr/bin/env python3
"""RC-gen-13 scanner: code addresses formed by register arithmetic that the generator's H5/H6 liveness
pairing (hi half through a register into a low half) does not see.

Reads extract/functions.jsonl (every function's disassembly) and a recomp_pruned.csv (the rows the generator
pruned as unreachable). Walks each function straight-line with exact constant propagation (reset at every
branch target, after every unconditional branch and every call) and reports every value that is a function
start, split by the SHAPE of its formation:

  direct   lis/addis/oris rX,..,hi followed by addi/addic/ori rY,rX,lo           (H5/H6 see it)
  copy_hi  a register copy (mr / addi rD,rA,0) of a high half BEFORE its low half (H5 does not follow copies)
  li-addis li rX,lo ; addis rY,rX,hi (the high half reads the low half's register) (H5 knows only li-then-oris)
  chained  more than one low half applied to one high half                         (H6 keeps only the last)

A formed value that equals a PRUNED row's start is a liveness bug candidate: the generator did not keep a row
whose address the code forms. Usage:
    scan_address_formation.py --functions extract/functions.jsonl --pruned <generated>/recomp_pruned.csv \n        --owners extract/native-owners.txt [--owners extract/native-owners-r2.txt] [--loose] [--dol sys/main.dol]
"""
import argparse
import csv
import json
import re
import sys

M32 = 0xFFFFFFFF


def s16(v):
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def parse_num(t):
    t = t.strip()
    return int(t, 16) if t.lower().startswith(("0x", "-0x")) else int(t)


BRANCH_RE = re.compile(r"^(b|bl|bc|bcl|bdnz|bdz|beq|bne|blt|bgt|ble|bge)\b")


def targets_of(text):
    m = re.search(r"0x([0-9a-fA-F]{8})\s*$", text)
    return int(m.group(1), 16) if m else None


def data_word_gap(args, funcs, extents, container):
    r"""RC-gen-13, H6 `block_starts` omits code addresses held in data words. A data word holding an INTERIOR code
    address can be reached by an indirect jump; H6 walks a low half back to its high half through the straight line
    above it and does not stop at such an address. Instance: a data word whose value A is the address of (or, within
    two instructions, precedes) a low half addi/ori rD,rX,lo whose rX was written by a lis/addis/oris earlier in the
    same straight line ABOVE A."""
    import struct
    with open(args.dol, "rb") as dol_file:
        raw = dol_file.read()
    offs = struct.unpack(">18I", raw[0x00:0x48])
    addrs = struct.unpack(">18I", raw[0x48:0x90])
    sizes = struct.unpack(">18I", raw[0x90:0xD8])
    by_pc = {}
    for fn in funcs:
        for i in fn["ins"]:
            by_pc[i["pc"]] = (i["text"], fn["addr"])
    hits = 0
    words = 0
    for k in range(7, 18):  # the data sections
        if sizes[k] == 0:
            continue
        base = addrs[k]
        for o in range(0, sizes[k] - 3, 4):
            v = struct.unpack(">I", raw[offs[k] + o:offs[k] + o + 4])[0]
            if v % 4 or not (0x80003100 <= v < 0x80450000):
                continue
            c = container(v)
            if c is None or v == c[0]:
                continue  # not an interior code address
            words += 1
            # the low half within the next 3 instructions, reading a register a high half wrote in the 6 above
            for d in range(0, 3):
                t = by_pc.get(v + 4 * d)
                if not t or not re.match(r"(addi|ori|addic)", t[0]):
                    continue
                ops = [x.strip() for x in t[0].partition(" ")[2].split(",")]
                if len(ops) != 3 or ops[1] in ("r0",) and t[0].startswith("addi"):
                    continue
                src = ops[1]
                for u in range(1, 7):
                    p = by_pc.get(v - 4 * u)
                    if not p or p[1] != t[1]:
                        break
                    m = re.match(r"(lis|addis|oris)\s+(r\d+)", p[0])
                    if m and (m.group(2) == src):
                        hits += 1
                        print("  data word at 0x%08X holds 0x%08X (%s): low half '%s' at 0x%08X reads %s set by '%s' at 0x%08X"
                              % (base + o, v, c[2], t[0], v + 4 * d, src, p[0], v - 4 * u))
                        break
    print("data words holding an interior code address: %d; of them landing between a high half and its low half: %d" % (words, hits))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--functions", required=True)
    ap.add_argument("--pruned", required=True)
    ap.add_argument("--owners", action="append", default=[], help="native-owners list(s): their rows emit only a thunk, so they form nothing")
    ap.add_argument("--loose", action="store_true", help="keep register values across labels, branches and calls (an over-approximation, like H5's any-pair rule)")
    ap.add_argument("--dol", help="also scan the DOL's data words: code addresses held in data that land between a high half and its low half")
    ap.add_argument("--all-starts", action="store_true", help="report formations of any function start, not only pruned rows")
    args = ap.parse_args()

    starts = {}
    extents = []  # (start, end, name)
    funcs = []
    with open(args.functions, encoding="utf8") as f:
        for line in f:
            r = json.loads(line)
            funcs.append(r)
            starts[r["addr"]] = r["name"]
            extents.append((r["addr"], r["addr"] + r["size"], r["name"]))
    extents.sort()
    import bisect
    ext_starts = [e[0] for e in extents]

    def container(v):
        k = bisect.bisect_right(ext_starts, v) - 1
        return extents[k] if k >= 0 and v < extents[k][1] else None
    pruned = set()
    with open(args.pruned, newline="") as f:
        for row in csv.DictReader(f):
            pruned.add(int(row["address"], 16))

    owners = set()
    for path in args.owners:
        with open(path, encoding="utf8") as f:
            for line in f:
                m = re.match(r"\s*0x([0-9a-fA-F]+)", line)
                if m:
                    owners.add(int(m.group(1), 16))
    found = []  # (shape, site, value, func)
    for fn in funcs:
        ins = fn["ins"]
        if fn["addr"] in owners and not args.all_starts:
            continue
        if fn["addr"] in pruned and not args.all_starts:
            continue  # a pruned row forms nothing that matters: only LIVE rows root other rows
        labels = set()
        for i in ins:
            t = i["text"]
            if BRANCH_RE.match(t.split()[0]):
                tg = targets_of(t)
                if tg is not None:
                    labels.add(tg)
        # value state: reg -> (value, shape tags frozenset, hi_applied_lows count)
        state = {}

        def put(reg, val, tags):
            state[reg] = (val & M32, tags)

        for i in ins:
            pc, t = i["pc"], i["text"]
            if pc in labels and not args.loose:
                state.clear()
            op, _, rest = t.partition(" ")
            a = [x.strip() for x in rest.split(",")] if rest else []

            def reg(x):
                return int(x[1:]) if x.startswith("r") and x[1:].isdigit() else None

            def emit(rd, val, tags):
                put(rd, val, tags)
                if val % 4 == 0 and 0x80003100 <= val < 0x80450000:
                    c = container(val)
                    if c is not None and (args.all_starts or c[0] in pruned):
                        missed = sorted(t for t in tags if t in ("copy_hi", "chained", "li-addis"))
                        shape = "+".join(missed) if missed else "direct"
                        if val != c[0]:
                            shape += "(interior)"
                        found.append((shape, pc, val, fn["name"], c[0] in pruned))

            if op == "li" and len(a) == 2:
                put(reg(a[0]), s16(parse_num(a[1])), frozenset({"li"}))
            elif op == "lis" and len(a) == 2:
                put(reg(a[0]), (s16(parse_num(a[1])) << 16) & M32, frozenset({"hi"}))
            elif op in ("addi", "addic", "addic.") and len(a) == 3:
                rd, ra, imm = reg(a[0]), reg(a[1]), s16(parse_num(a[2]))
                if ra in state and imm == 0 and op == "addi":
                    v, tags = state[ra]  # addi rD,rA,0 is a register copy
                    state[rd] = (v, frozenset(set(tags) | ({"copy_hi"} if "lo" not in tags else set())))
                elif ra in state:
                    v, tags = state[ra]
                    nt = set(tags)
                    if "lo_nz" in nt:
                        nt.add("chained")  # a second non-zero low half: H6 keeps only the last
                    nt.discard("li")
                    nt.update({"lo", "lo_nz"})
                    emit(rd, v + imm, frozenset(nt))
                else:
                    state.pop(rd, None)
            elif op == "addis" and len(a) == 3:
                rd, ra, imm = reg(a[0]), reg(a[1]), s16(parse_num(a[2]))
                if ra in state:
                    v, tags = state[ra]
                    nt = set(tags)
                    shape = "li-addis" if "li" in nt else None
                    nt.discard("li")
                    nt.add("hi")
                    if shape:
                        nt.add(shape)
                    emit(rd, v + (imm << 16), frozenset(nt))
                else:
                    state.pop(rd, None)
            elif op in ("ori", "oris") and len(a) == 3:
                ra, rs, imm = reg(a[0]), reg(a[1]), parse_num(a[2]) & 0xFFFF
                if rs in state:
                    v, tags = state[rs]
                    nt = set(tags)
                    if op == "ori":
                        if "lo_nz" in nt:
                            nt.add("chained")
                        nt.update({"lo", "lo_nz"})
                        emit(ra, v | imm, frozenset(nt))
                    else:
                        emit(ra, v | (imm << 16), frozenset(nt | {"hi"}))
                else:
                    state.pop(ra, None)
            elif op == "mr" and len(a) == 2:
                rd, rs = reg(a[0]), reg(a[1])
                if rs in state:
                    v, tags = state[rs]
                    state[rd] = (v, frozenset(set(tags) | ({"copy_hi"} if "lo" not in tags else set())))
                else:
                    state.pop(rd, None)
            elif op == "add" and len(a) == 3:
                rd, ra, rb = reg(a[0]), reg(a[1]), reg(a[2])
                if ra in state and rb in state:
                    emit(rd, state[ra][0] + state[rb][0], frozenset({"add"}))
                else:
                    state.pop(rd, None)
            elif op in ("bl", "bctrl", "blrl", "bcl", "bcctrl") or op in ("b", "blr", "bctr", "bclr", "bcctr"):
                if not args.loose:
                    state.clear()
            else:
                # any other instruction that writes a GPR forgets it (conservative: drop the first operand register)
                if a:
                    r0 = reg(a[0])
                    if r0 is not None and not op.startswith(("st", "cmp", "mt", "b", "tw", "dcb", "icb", "sync", "isync")):
                        state.pop(r0, None)

    if args.dol:
        data_word_gap(args, funcs, extents, container)
    shapes = {}
    for shape, site, val, fname, was_pruned in found:
        shapes.setdefault(shape, []).append((site, val, fname, was_pruned))
    print("shapes of formations of %s:" % ("function starts" if args.all_starts else "pruned function starts"))
    for shape in sorted(shapes):
        rows = shapes[shape]
        print("  %-24s %d (%d of them pruned rows)" % (shape, len(rows), sum(1 for r in rows if r[3])))
        for site, val, fname, was_pruned in rows[:12]:
            print("      site 0x%08X forms 0x%08X (%s) in %s" % (site, val, container(val)[2], fname))
    return 0


if __name__ == "__main__":
    sys.exit(main())
