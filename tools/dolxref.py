#!/usr/bin/env python
"""Cross-reference the PAL main.dol.

  xrefs   <addr>          every bl / lis+addi|ori that materialises or calls <addr>
  callers <addr>          only bl call sites
  owner   <addr>          the recovered function containing <addr>
  strfun  <substr>        for each string matching <substr>, the functions that reference it

Addresses are guest virtual addresses (0x80xxxxxx).
"""
import bisect
import csv
import struct
import sys
from pathlib import Path

SOURCE = Path(__file__).resolve().parents[1]
ROOT = SOURCE.parent / "game"
DOL = ROOT / "sys/main.dol"
FUNCS = SOURCE / "extract/functions.csv"
STRINGS = SOURCE / "extract/strings.csv"


class Image:
    def __init__(self, path):
        raw = open(path, "rb").read()
        self.raw = raw
        self.text = []   # (addr, size, off) code sections only
        self.all = []
        for i in range(18):
            off = struct.unpack_from(">I", raw, i * 4)[0]
            addr = struct.unpack_from(">I", raw, 0x48 + i * 4)[0]
            size = struct.unpack_from(">I", raw, 0x90 + i * 4)[0]
            if size:
                self.all.append((addr, size, off))
                if i < 7:
                    self.text.append((addr, size, off))

    def read32(self, addr):
        for a, s, o in self.all:
            if a <= addr < a + s:
                return struct.unpack_from(">I", self.raw, o + addr - a)[0]
        return None


def load_functions():
    rows = []
    with open(FUNCS, newline="", encoding="utf-8") as fh:
        for r in csv.DictReader(fh):
            try:
                rows.append((int(r["addr_hex"], 16), int(r["size"]), r["name"]))
            except (ValueError, KeyError):
                pass
    rows.sort()
    return rows


def owner(funcs, starts, addr):
    i = bisect.bisect_right(starts, addr) - 1
    if i < 0:
        return None
    a, size, name = funcs[i]
    return (a, size, name) if addr < a + size else (a, size, name + "?")


def scan(img, funcs, starts, target, sda2=0x805FA780, sda=0x805EC500):
    """Return (call_sites, materialisations).

    Constant-propagates integer register values through lis/addi/addis/ori/add
    and the r2/r13 small-data bases, so a literal-pool base materialised once
    and then used as `base + k` is still attributed to `base + k`.

    This matters: a plain "lis immediately consumed by an addi whose immediate
    is the low half" test misses 809 of the image's 13,397 strings. It is how
    "palSel/PalSel%s.txd" at 0x8050C9C8 - reached as 0x8050C888 + 0x140 inside
    fn_80306b48 - was once wrongly declared unreferenced.
    """
    calls, mats = [], []
    for base, size, off in img.text:
        words = struct.unpack_from(">%dI" % (size // 4), img.raw, off)
        known = {2: sda2, 13: sda}   # register -> constant, when known
        for k, w in enumerate(words):
            addr = base + k * 4
            op = w >> 26

            if op == 18:  # b / bl
                if w & 1:
                    d = w & 0x03FFFFFC
                    if d & 0x02000000:
                        d -= 0x04000000
                    dest = d if (w & 2) else (addr + d) & 0xFFFFFFFF
                    if dest == target:
                        calls.append(addr)
                # A call clobbers r0 and r3..r12. r14..r31 are callee-saved and
                # survive it, which is the whole point: fn_80306b48 keeps its
                # literal-pool base in r31 across two RenderWare calls before
                # using it.
                for volatile in [0] + list(range(3, 13)):
                    known.pop(volatile, None)
                known[2] = sda2
                known[13] = sda
                continue

            rt = (w >> 21) & 31
            ra = (w >> 16) & 31
            imm = w & 0xFFFF
            simm = imm - 0x10000 if (imm & 0x8000) else imm

            if op == 15:  # addis / lis
                base_value = 0 if ra == 0 else known.get(ra)
                if base_value is None:
                    known.pop(rt, None)
                else:
                    known[rt] = (base_value + (simm << 16)) & 0xFFFFFFFF
                continue

            if op == 14:  # addi / li
                base_value = 0 if ra == 0 else known.get(ra)
                if base_value is None:
                    known.pop(rt, None)
                    continue
                value = (base_value + simm) & 0xFFFFFFFF
                known[rt] = value
                if value == target:
                    mats.append((addr, "addi"))
                continue

            if op == 24 or op == 25:  # ori / oris
                base_value = known.get(ra)
                if base_value is None:
                    known.pop(rt, None)   # note: ori's dest field is rA-form rS
                    continue
                value = base_value | (imm << (16 if op == 25 else 0))
                # ori rA,rS,imm - destination is the rA field
                known[ra] = value
                if value == target:
                    mats.append((addr, "ori"))
                continue

            if 32 <= op <= 55:  # D-form loads/stores: rA + simm
                base_value = known.get(ra)
                if base_value is not None and ((base_value + simm) & 0xFFFFFFFF) == target:
                    mats.append((addr, "mem"))
                # a load writes rT; stores do not
                if op in (32, 33, 34, 35, 40, 41, 42, 43, 46, 48, 49, 50, 51):
                    known.pop(rt, None)
                continue

            if op == 31:
                xo = (w >> 1) & 0x3FF
                rb = (w >> 11) & 31
                if xo == 266 or xo == 10:  # add / addc
                    a_value = known.get(ra)
                    b_value = known.get(rb)
                    if a_value is None or b_value is None:
                        known.pop(rt, None)
                    else:
                        value = (a_value + b_value) & 0xFFFFFFFF
                        known[rt] = value
                        if value == target:
                            mats.append((addr, "add"))
                    continue
                if xo == 444:  # or / mr
                    rs = rt
                    value = known.get(rs) if rs == rb else None
                    if value is None:
                        known.pop(ra, None)
                    else:
                        known[ra] = value
                    continue
                # X-form indexed loads/stores: rA + rB
                a_value = known.get(ra if ra != 0 else -1)
                b_value = known.get(rb)
                if a_value is not None and b_value is not None and                         ((a_value + b_value) & 0xFFFFFFFF) == target:
                    mats.append((addr, "memx"))
                known.pop(rt, None)
                continue

            known.pop(rt, None)
    return calls, mats


def describe(funcs, starts, sites):
    seen = {}
    for site in sites:
        a = site[0] if isinstance(site, tuple) else site
        o = owner(funcs, starts, a)
        key = o[0] if o else None
        seen.setdefault(key, (o, []))[1].append(site)
    out = []
    for key, (o, group) in sorted(seen.items(), key=lambda kv: (kv[0] is None, kv[0])):
        label = "0x%08x %s size=%d" % (o[0], o[2], o[1]) if o else "(no function)"
        out.append("  %s   <- %s" % (label, ", ".join(
            ("0x%08x(%s)" % s) if isinstance(s, tuple) else ("0x%08x" % s) for s in group)))
    return out


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    cmd, arg = sys.argv[1], sys.argv[2]
    img = Image(DOL)
    funcs = load_functions()
    starts = [f[0] for f in funcs]

    if cmd == "owner":
        print(owner(funcs, starts, int(arg, 16)))
        return 0

    if cmd == "strfun":
        targets = []
        with open(STRINGS, newline="", encoding="utf-8") as fh:
            for r in csv.reader(fh):
                if len(r) >= 2 and arg.lower() in r[1].lower():
                    targets.append((int(r[0], 16), r[1]))
        for a, text in targets:
            calls, mats = scan(img, funcs, starts, a)
            print('0x%08x "%s"' % (a, text[:70]))
            for line in describe(funcs, starts, mats):
                print(line)
        return 0

    target = int(arg, 16)
    calls, mats = scan(img, funcs, starts, target)
    if cmd in ("xrefs", "callers"):
        print("call sites (bl 0x%08x): %d" % (target, len(calls)))
        for line in describe(funcs, starts, calls):
            print(line)
    if cmd == "xrefs":
        print("materialisations of 0x%08x: %d" % (target, len(mats)))
        for line in describe(funcs, starts, mats):
            print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
