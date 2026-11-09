"""Whole-image function catcher: what every function IS, read from its own instructions and the image's own tables.

    python -B bulk_catch.py --project <dolrecon project dir> [--out DIR]

Three grades, never mixed:

  FACT       read directly and completely from the function's instructions or a table in the image. It says what the
             code does, not what the original programmer called it:
               behaviour  empty / returns a constant / field getter or setter / global getter or setter / field or
                          global address / jump thunk / this-adjusting thunk / frame-only wrapper around one call
               slot       the function is entry N of the virtual table of class C (read from the table itself)
  FAMILY     byte-identical to other functions once call targets, lis halves and small-data offsets are masked. A fact
             about shape; it is NOT evidence that two functions have the same identity (trivial bodies repeat by chance).
  CANDIDATE  a name offered from the function's family. Only for bodies of at least --min-insns instructions with at
             least --quorum witnesses that all agree. A witness is a named member whose name says something: not a
             structural label (vfN, ctor, dtor, callback, ...) and not a name that only records its single caller. The
             same rule is tried with each witness's name hidden; both halves of the families (split by family hash, a
             held-out check on the settings) must reach --min-precision, otherwise no candidates are written.

Nothing here edits names.json or the manifest. Output: <out>/bulk_catch.csv and <out>/bulk_catch_summary.json.
"""
from __future__ import annotations

import argparse
import collections
import csv
import hashlib
import json
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))

BLR = 0x4E800020
R13 = 0x805EC500
R2 = 0x805FA780
LOADS = {32: "u32", 34: "u8", 40: "u16", 42: "s16", 48: "f32", 50: "f64"}
STORES = {36: "u32", 38: "u8", 44: "u16", 52: "f32", 54: "f64"}
# D-form memory opcodes whose displacement is a small-data offset when RA is r2 or r13
MEM_OPS = set(range(32, 56))
SYMBOL = re.compile(r"(\S+) = \.(\w+):0x([0-9A-Fa-f]+); // type:(\w+)(?: size:0x([0-9A-Fa-f]+))?")
UNNAMED = re.compile(r"fn_[0-9A-Fa-f]{8}$")
ROLE = re.compile(r"(vf\d+|ctor|dtor|static_dtor|static_init|callback|sub\d+|thunk)$")
WEAK_RULES = {"N4 helper"}


def simm(w):
    v = w & 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def fields(w):
    return w >> 26, (w >> 21) & 31, (w >> 16) & 31


def branch_target(w, at):
    li = w & 0x03FFFFFC
    if li & 0x02000000:
        li -= 0x04000000
    return (li if w & 2 else at + li) & 0xFFFFFFFF


def is_b(w):
    return w >> 26 == 18 and not w & 1


def is_bl(w):
    return w >> 26 == 18 and w & 1 == 1


def normalize(words):
    """The body with what the linker decides masked: branch-and-link / branch targets, lis halves, r2 / r13 offsets."""
    out = []
    for w in words:
        op, _, ra = fields(w)
        if op == 18:
            out.append(w & 0xFC000003)
        elif op == 15 and ra == 0:
            out.append(w & 0xFFFF0000)
        elif (op == 14 or op in MEM_OPS) and ra in (2, 13):
            out.append(w & 0xFFFF0000)
        else:
            out.append(w)
    return tuple(out)


def family_key(words):
    return hashlib.md5(struct.pack(">%dI" % len(words), *normalize(words))).hexdigest()


def _strip_frame(words):
    """Prologue / epilogue instructions removed; None if the rest is not a plain frame (saved registers count as work)."""
    body = []
    for w in words:
        op, rd, ra = fields(w)
        if w >> 16 == 0x9421 or w == 0x7C0802A6 or w == 0x7C0803A6 or w == BLR:  # stwu r1; mflr r0; mtlr r0; blr
            continue
        if op == 36 and rd == 0 and ra == 1 or op == 32 and rd == 0 and ra == 1:  # stw / lwz r0 at the LR slot
            continue
        if op == 14 and rd == 1 and ra == 1:  # addi r1,r1,N
            continue
        body.append(w)
    return body


def behaviour(words, at):
    """(kind, detail) when the whole body is one of the recognised shapes, else None. Every case reads every instruction."""
    n = len(words)
    if n == 1:
        if words[0] == BLR:
            return "empty", ""
        if is_b(words[0]):
            return "thunk", "%08X" % branch_target(words[0], at)
        return None
    if n == 2 and words[1] == BLR:
        w = words[0]
        op, rd, ra = fields(w)
        if op == 14 and rd == 3 and ra == 0:
            return "returns-constant", str(simm(w))
        if op == 14 and rd == 3 and ra == 3:
            return "field-address", "%+#x" % simm(w)
        if op in LOADS and rd == (1 if op >= 48 else 3):
            if ra == 3:
                return "getter", "%s %+#x" % (LOADS[op], simm(w))
            if ra in (2, 13):
                return "global-getter", "%s %08X" % (LOADS[op], ((R2 if ra == 2 else R13) + simm(w)) & 0xFFFFFFFF)
        if op in STORES and rd == (1 if op >= 48 else 4) and ra == 3:
            return "setter", "%s %+#x" % (STORES[op], simm(w))
        if op in STORES and rd == (1 if op >= 48 else 3) and ra in (2, 13):
            return "global-setter", "%s %08X" % (STORES[op], ((R2 if ra == 2 else R13) + simm(w)) & 0xFFFFFFFF)
        return None
    if n == 2 and is_b(words[1]):
        op, rd, ra = fields(words[0])
        if op == 14 and rd == 3 and ra == 3:
            return "adjustor-thunk", "%+#x -> %08X" % (simm(words[0]), branch_target(words[1], at + 4))
        return None
    if n == 3 and words[2] == BLR:
        op0, rd0, ra0 = fields(words[0])
        op1, rd1, ra1 = fields(words[1])
        if op0 == 15 and ra0 == 0 and rd0 == 3 and rd1 == 3 and ra1 == 3:
            hi = (words[0] & 0xFFFF) << 16
            if op1 == 14:
                return "global-address", "%08X" % ((hi + simm(words[1])) & 0xFFFFFFFF)
            if op1 == 24:  # ori: the operand order is rS, rA
                pass
        return None
    rest = _strip_frame(words)
    if len(rest) == 1 and is_bl(rest[0]) and n >= 5:
        at_bl = at + 4 * words.index(rest[0])
        return "wrapper", "%08X" % branch_target(rest[0], at_bl)
    return None


def vtable_slots(classes, read_word, starts):
    """{function: [(class, slot)]} from each class's own table: entries from +8 while the word is a function start."""
    slots = collections.defaultdict(list)
    for name, info in classes.items():
        table = info.get("vtable")
        if not table:
            continue
        at = int(table, 16) + 8
        slot = 0
        while slot < 2048:
            try:
                word = read_word(at)
            except Exception:
                break
            if word not in starts:
                break
            slots[word].append((name, slot))
            at += 4
            slot += 1
    return slots


def base_name(name):
    """The method part of a name, without class scope, mangling suffix or the address added to tell duplicates apart."""
    base = name.split("__")[0] if name.startswith(("vf", "__")) is False and "__Q" in name else name.rsplit("__", 1)[-1]
    return re.sub(r"_[0-9A-Fa-f]{8}$", "", base)


def witnesses(named, rules):
    """The named functions whose names can vouch for another function (see CANDIDATE above)."""
    return {a: n for a, n in named.items() if rules.get(a) not in WEAK_RULES and not ROLE.search(base_name(n))}


def agreed(members, names, quorum, exclude=None):
    """The one base name at least `quorum` witnesses in `members` agree on, else None."""
    said = [base_name(names[m]) for m in members if m in names and m != exclude]
    return said[0] if len(said) >= quorum and len(set(said)) == 1 else None


def validate(families, names, min_insns, quorum=2):
    """Hide every witness in turn; predict it from the others; returns (tried, right)."""
    tried = right = 0
    for members, insns in families:
        if insns < min_insns or len(members) < 2:
            continue
        for hidden in (m for m in members if m in names):
            guess = agreed(members, names, quorum, exclude=hidden)
            if guess is None:
                continue
            tried += 1
            right += guess == base_name(names[hidden])
    return tried, right


def load_symbols(project):
    out = {}
    with open(os.path.join(project, "config", "symbols.txt"), encoding="utf-8") as f:
        for line in f:
            m = SYMBOL.match(line)
            if m and m.group(4) == "function":
                out[int(m.group(3), 16)] = (m.group(1), int(m.group(5) or "0", 16))
    return out


def main():
    from dolrecon import dol
    ap = argparse.ArgumentParser()
    ap.add_argument("--project", required=True)
    ap.add_argument("--out", default=".build/puzzle")
    ap.add_argument("--min-insns", type=int, default=12)
    ap.add_argument("--quorum", type=int, default=2)
    ap.add_argument("--min-precision", type=float, default=0.98)
    a = ap.parse_args()
    image = dol.load_file(os.path.join(a.project, "main.dol"))
    symbols = load_symbols(a.project)
    named = {k: n for k, (n, _) in symbols.items() if not UNNAMED.match(n)}
    with open(os.path.join(a.project, "names.json"), encoding="utf-8") as f:
        data = json.load(f)
    classes = data["classes"]
    rules = {int(k, 16): v["rule"] for k, v in data["names"].items()}
    witness = witnesses(named, rules)
    bodies = {}
    for addr, (_, size) in symbols.items():
        if size >= 4 and size % 4 == 0:
            try:
                bodies[addr] = struct.unpack(">%dI" % (size // 4), image.read(addr, size))
            except Exception:
                continue
    groups = collections.defaultdict(list)
    for addr, words in bodies.items():
        groups[family_key(words)].append(addr)
    families = {addr: k for k, v in groups.items() if len(v) > 1 for addr in v}
    slots = vtable_slots(classes, lambda at: struct.unpack(">I", image.read(at, 4))[0], set(bodies))
    # candidate names, only if the hidden-name validation clears the bar
    halves = ([], [])
    for key, v in groups.items():
        if len(v) > 1:
            halves[int(key[-1], 16) & 1].append((v, len(bodies[v[0]])))
    results = [validate(h, witness, a.min_insns, a.quorum) for h in halves]
    tried = results[0][0] + results[1][0]
    right = results[0][1] + results[1][1]
    precision = right / tried if tried else 0.0
    half_precision = [r / t if t else 0.0 for t, r in results]
    allow = all(t > 0 for t, _ in results) and min(half_precision) >= a.min_precision
    # the unit that counts as independent evidence is the family: one template family can supply hundreds of trials
    per_family = [validate([f], witness, a.min_insns, a.quorum) for h in halves for f in h]
    per_family = [(t, r) for t, r in per_family if t]
    families_right = sum(1 for t, r in per_family if t == r)
    rows = []
    counts = collections.Counter()
    unnamed = [x for x in bodies if x not in named]
    for addr in sorted(bodies):
        words = bodies[addr]
        kind = behaviour(words, addr)
        fam = families.get(addr)
        slot = slots.get(addr, [])
        candidate = ""
        if allow and addr not in named and fam and len(words) >= a.min_insns:
            if agreed(groups[fam], witness, a.quorum) is not None:
                candidate = witness[next(m for m in groups[fam] if m in witness)]
        rows.append({
            "address": "%08X" % addr, "name": symbols[addr][0], "named": int(addr in named), "insns": len(words),
            "behaviour": kind[0] if kind else "", "detail": kind[1] if kind else "",
            "family": fam[:10] if fam else "", "family_size": len(groups[fam]) if fam else 1,
            "vtable_slots": ";".join("%s[%d]" % s for s in slot[:6]), "vtable_count": len(slot), "candidate": candidate,
        })
        if addr not in named:
            counts["unnamed"] += 1
            if kind:
                counts["fact:behaviour"] += 1
                counts["behaviour:" + kind[0]] += 1
            if slot:
                counts["fact:vtable-slot"] += 1
            if kind or slot:
                counts["fact:any"] += 1
            if fam:
                counts["family"] += 1
            if candidate:
                counts["candidate"] += 1
            if kind or slot or fam or candidate:
                counts["caught-any"] += 1
    os.makedirs(a.out, exist_ok=True)
    with open(os.path.join(a.out, "bulk_catch.csv"), "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0]))
        w.writeheader()
        w.writerows(rows)
    summary = {
        "functions": len(bodies), "unnamed": len(unnamed), "counts": dict(counts),
        "family_count": sum(1 for v in groups.values() if len(v) > 1),
        "validation": {"min_insns": a.min_insns, "quorum": a.quorum, "witnesses": len(witness),
                       "halves": [list(r) for r in results], "families_validated": len(per_family), "families_all_right": families_right,
                       "tried": tried, "right": right, "precision": round(precision, 4),
                       "bar": a.min_precision, "candidates_written": allow},
    }
    with open(os.path.join(a.out, "bulk_catch_summary.json"), "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=1)
    print(json.dumps(summary, indent=1))


if __name__ == "__main__":
    main()
