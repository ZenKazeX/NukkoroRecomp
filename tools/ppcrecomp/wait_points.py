"""Wait points for the generator (ppcrecomp --wait-points): guest loops that
wait for an event.

Under the native time model (docs/native-time-20261005/DESIGN.md) guest code
runs in zero game time and the game clock moves only at a wait point. A loop
whose iteration has no side effect and carries no register state from one
iteration into the next is a pure function of what it reads - memory, the
time base, loop-invariant registers. Nothing inside it can change those, so
once its back edge is taken it is taken forever, unless something outside it
changes them: an interrupt handler or a woken thread (memory), a device
(memory-mapped registers), the clock itself (the time base). All of those
happen only at an event. Such a back edge is therefore a wait point: the
generated code advances the game clock to its next event there
(Cpu::idle_wait -> System::idle_wait) - exactly what the hardware's spin
waited for, without the spin.

A loop qualifies when, over the region [head, last back edge]:
  * every instruction decodes (an unknown one rejects the loop);
  * nothing has a side effect: no store, call, sc, rfi, tw, mtmsr, mtspr
    (other than LR/CTR/XER), cache maintenance, lwarx/stwcx.;
  * no branch goes backward to anything but the head (no inner loop);
  * a back edge is reachable from the head within one iteration;
  * no register is loop-carried: every register an instruction reads is
    either not written in the region or must-defined on every path from the
    head to that instruction within the iteration.

Each back edge is listed with its head and a class. The first four are
proven waits; the generator emits Cpu::idle_wait on the back edge:
  poll   reads memory (an interrupt handler, a thread or a device changes it)
  clock  reads the time base or the decrementer
  spin   reads registers only (entered with the condition true, it never ends)
  halt   has no exit at all

The fifth is a loop the same rules admit except that its iteration calls
functions or stores - a status query that re-stores an unchanged copy, a
retry around an SDK call, the main loop. Whether one iteration changed
anything is not decidable here, so the generator emits Cpu::poll_point on the
back edge and the runtime decides: an iteration that left every register as it
found it and made no net change to memory is a fixed point - nothing but an
event can end the loop - and only then does the clock advance:
  progress  calls or stores; the runtime confirms a fixed point before waiting

For this class a call is modelled as the ABI's volatile registers (r0, r3-r12,
f0-f13, cr0, cr1, cr5-cr7, CTR, XER, LR) written and nothing read, and a store
as reading its source and base registers (an update form writes its base).

  wait_points.py --dol <main.dol> --functions <functions.csv> --out <file>
  wait_points.py --dol <main.dol> --functions <functions.csv> --check <file>

--check fails (exit 1) unless the file is exactly what these inputs produce now
(CTest native_wait_points). The generator refuses a listed site that is not a
backward branch to its head.
"""
from __future__ import annotations

import argparse
import csv
import io
import struct
import sys
from pathlib import Path


class Image:
    def __init__(self, raw: bytes):
        self.raw = raw
        self.sections = []
        for index in range(7):
            offset, address, size = (struct.unpack_from(">I", raw, base + index * 4)[0]
                                     for base in (0, 0x48, 0x90))
            if size:
                self.sections.append((address, size, offset))

    def text(self, address: int) -> bool:
        return any(start <= address < start + size for start, size, _ in self.sections)

    def read32(self, address: int) -> int:
        for start, size, offset in self.sections:
            if start <= address < start + size:
                return struct.unpack_from(">I", self.raw, offset + address - start)[0]
        raise KeyError(hex(address))


class Decoded:
    __slots__ = ("kind", "reads", "writes", "target", "conditional", "memory", "clock")

    def __init__(self, kind, reads=(), writes=()):
        self.kind = kind  # alu load nop branch call exit impure unknown
        self.reads = set(reads)
        self.writes = set(writes)
        self.target = None
        self.conditional = False
        self.memory = False
        self.clock = False


def _r(n):
    return "r%d" % n


def _f(n):
    return "f%d" % n


def _cr(n):
    return "cr%d" % n


def _ra0(n):
    return [] if n == 0 else [_r(n)]


def _rc(d, ins, field=0):
    if ins & 1:
        d.writes.add(_cr(field))
        if field == 0:
            d.reads.add("so")
    return d


def _load(reads, writes):
    d = Decoded("load", reads, writes)
    d.memory = True
    return d


def _store(reads, writes=()):
    return Decoded("store", reads, writes)


# The ABI's volatile registers: what a call may leave changed.
_CALL_WRITES = ["r0"] + ["r%d" % n for n in range(3, 13)] + \
    ["f%d" % n for n in range(14)] + ["cr0", "cr1", "cr5", "cr6", "cr7", "ctr", "ca", "so", "lr",
                                       "fpflags"]


def _call(reads=()):
    return Decoded("call", reads, _CALL_WRITES)


def _fp(ins, reads, writes):
    return _rc(Decoded("alu", list(reads) + ["fpmode"], list(writes) + ["fpflags"]), ins, 1)


def decode(ins: int, address: int) -> Decoded:
    op = ins >> 26
    rd, ra, rb = (ins >> 21) & 31, (ins >> 16) & 31, (ins >> 11) & 31
    if op == 7:
        return Decoded("alu", [_r(ra)], [_r(rd)])
    if op == 8:
        return Decoded("alu", [_r(ra)], [_r(rd), "ca"])
    if op in (10, 11):
        return Decoded("alu", [_r(ra), "so"], [_cr(rd >> 2)])
    if op == 12:
        return Decoded("alu", [_r(ra)], [_r(rd), "ca"])
    if op == 13:
        return Decoded("alu", [_r(ra), "so"], [_r(rd), "ca", "cr0"])
    if op in (14, 15):
        return Decoded("alu", _ra0(ra), [_r(rd)])
    if op == 16:
        bo, bi = rd, ra
        bd = ins & 0xFFFC
        if bd & 0x8000:
            bd -= 0x10000
        d = _call() if ins & 1 else Decoded("branch")
        if not bo & 0x10:
            d.reads.add(_cr(bi >> 2))
        if not bo & 0x04:
            d.reads.add("ctr")
            d.writes.add("ctr")
        d.target = (bd if ins & 2 else address + bd) & 0xFFFFFFFF
        d.conditional = (bo & 0x14) != 0x14
        return d
    if op == 17:
        return Decoded("impure")
    if op == 18:
        li = ins & 0x03FFFFFC
        if li & 0x02000000:
            li -= 0x04000000
        d = _call() if ins & 1 else Decoded("branch")
        d.target = (li if ins & 2 else address + li) & 0xFFFFFFFF
        return d
    if op == 19:
        xo = (ins >> 1) & 0x3FF
        if xo == 0:
            return Decoded("alu", [_cr(ra >> 2)], [_cr(rd >> 2)])
        if xo in (16, 528):
            bo, bi = rd, ra
            d = _call() if ins & 1 else Decoded("exit")
            if not bo & 0x10:
                d.reads.add(_cr(bi >> 2))
            d.reads.add("lr" if xo == 16 else "ctr")
            if xo == 16 and not bo & 0x04:
                d.reads.add("ctr")
                d.writes.add("ctr")
            d.conditional = (bo & 0x14) != 0x14
            return d
        if xo in (33, 129, 193, 225, 257, 289, 417, 449):
            return Decoded("alu", [_cr(rd >> 2), _cr(ra >> 2), _cr(rb >> 2)], [_cr(rd >> 2)])
        if xo == 150:
            return Decoded("nop")
        if xo == 50:
            return Decoded("impure")
        return Decoded("unknown")
    if op in (20, 21, 23):
        reads = [_r(rd)] + ([_r(ra)] if op == 20 else []) + ([_r(rb)] if op == 23 else [])
        return _rc(Decoded("alu", reads, [_r(ra)]), ins)
    if op in (24, 25, 26, 27):
        if op == 24 and ins == 0x60000000:
            return Decoded("nop")
        return Decoded("alu", [_r(rd)], [_r(ra)])
    if op in (28, 29):
        return Decoded("alu", [_r(rd), "so"], [_r(ra), "cr0"])
    if op == 31:
        return _decode31(ins, rd, ra, rb)
    if op in (32, 34, 40, 42):
        return _load(_ra0(ra), [_r(rd)])
    if op in (33, 35, 41, 43):
        return _load([_r(ra)], [_r(rd), _r(ra)])
    if op in (48, 50):
        return _load(_ra0(ra), [_f(rd)])
    if op in (49, 51):
        return _load([_r(ra)], [_f(rd), _r(ra)])
    if op == 46:
        return _load(_ra0(ra), [_r(n) for n in range(rd, 32)])
    if op in (36, 38, 44):
        return _store([_r(rd)] + _ra0(ra))
    if op in (37, 39, 45):
        return _store([_r(rd), _r(ra)], [_r(ra)])
    if op == 47:
        return _store([_r(n) for n in range(rd, 32)] + _ra0(ra))
    if op in (52, 54):
        return _store([_f(rd)] + _ra0(ra))
    if op in (53, 55):
        return _store([_f(rd), _r(ra)], [_r(ra)])
    if op == 60:
        return _store([_f(rd), "gqr"] + _ra0(ra))
    if op == 61:
        return _store([_f(rd), "gqr", _r(ra)], [_r(ra)])
    if op == 56:
        return _load(_ra0(ra) + ["gqr"], [_f(rd)])
    if op == 57:
        return _load([_r(ra), "gqr"], [_f(rd), _r(ra)])
    if op == 4:
        xo6 = (ins >> 1) & 0x3F
        xo10 = (ins >> 1) & 0x3FF
        if xo6 == 6:
            return _load(_ra0(ra) + [_r(rb), "gqr"], [_f(rd)])
        if xo6 == 38:
            return _load([_r(ra), _r(rb), "gqr"], [_f(rd), _r(ra)])
        if xo6 == 7:
            return _store([_f(rd), _r(rb), "gqr"] + _ra0(ra))
        if xo6 == 39:
            return _store([_f(rd), _r(rb), "gqr", _r(ra)], [_r(ra)])
        if xo10 == 1014:
            return Decoded("impure")
        if xo10 in (0, 32, 64, 96):
            return Decoded("alu", [_f(ra), _f(rb), "fpmode"], [_cr(rd >> 2), "fpflags"])
        if xo10 in (40, 72, 136, 264):
            return _rc(Decoded("alu", [_f(rb)], [_f(rd)]), ins, 1)
        if xo10 in (528, 560, 592, 624):
            return _rc(Decoded("alu", [_f(ra), _f(rb)], [_f(rd)]), ins, 1)
        return _fp(ins, [_f(ra), _f(rb), _f((ins >> 6) & 31)], [_f(rd)])
    if op == 59:
        return _fp(ins, [_f(ra), _f(rb), _f((ins >> 6) & 31)], [_f(rd)])
    if op == 63:
        xo5 = (ins >> 1) & 0x1F
        xo10 = (ins >> 1) & 0x3FF
        if xo5 in (18, 20, 21, 22, 23, 25, 26, 28, 29, 30, 31):
            return _fp(ins, [_f(ra), _f(rb), _f((ins >> 6) & 31)], [_f(rd)])
        if xo10 in (0, 32):
            return Decoded("alu", [_f(ra), _f(rb), "fpmode"], [_cr(rd >> 2), "fpflags"])
        if xo10 in (12, 14, 15):
            return _fp(ins, [_f(rb)], [_f(rd)])
        if xo10 in (40, 72, 136, 264):
            return _rc(Decoded("alu", [_f(rb)], [_f(rd)]), ins, 1)
        if xo10 == 583:
            return _rc(Decoded("alu", ["fpflags", "fpmode"], [_f(rd)]), ins, 1)
        if xo10 in (38, 70, 134, 711):
            return _rc(Decoded("alu", [_f(rb)] if xo10 == 711 else [], ["fpmode", "fpflags"]), ins, 1)
        if xo10 == 64:
            return Decoded("alu", ["fpflags"], [_cr(rd >> 2), "fpflags"])
        return Decoded("unknown")
    return Decoded("unknown")


_ARITH = {8, 10, 11, 40, 75, 104, 136, 138, 200, 202, 232, 234, 235, 266, 459, 491}
_CARRY_OUT = {8, 10, 136, 138, 200, 202, 232, 234}
_CARRY_IN = {136, 138, 200, 202, 232, 234}
_ONE_SOURCE = {104, 200, 202, 232, 234}
_LOADS = {23: _r, 87: _r, 279: _r, 343: _r, 534: _r, 790: _r, 535: _f, 599: _f}
_UPDATE_LOADS = {55: _r, 119: _r, 311: _r, 375: _r, 567: _f, 631: _f}
# Indexed stores: (source kind, update form).
_STORES = {151: (_r, False), 183: (_r, True), 215: (_r, False), 247: (_r, True), 407: (_r, False),
           439: (_r, True), 662: (_r, False), 918: (_r, False), 663: (_f, False), 695: (_f, True),
           727: (_f, False), 759: (_f, True), 983: (_f, False)}
_LOGICAL = {24, 536, 792, 26, 28, 60, 124, 284, 316, 412, 444, 476, 824, 922, 954}


def _decode31(ins, rd, ra, rb):
    xo = (ins >> 1) & 0x3FF
    xo9 = (ins >> 1) & 0x1FF
    if xo9 in _ARITH:
        reads = [_r(ra)] + ([] if xo9 in _ONE_SOURCE else [_r(rb)])
        writes = [_r(rd)]
        if xo9 in _CARRY_OUT:
            writes.append("ca")
        if xo9 in _CARRY_IN:
            reads.append("ca")
        if (ins >> 10) & 1:
            writes.append("so")
            reads.append("so")
        return _rc(Decoded("alu", reads, writes), ins)
    if xo in (0, 32):
        return Decoded("alu", [_r(ra), _r(rb), "so"], [_cr(rd >> 2)])
    if xo in (4, 20, 150, 146, 54, 86, 470, 1014, 982, 661, 725):
        return Decoded("impure")
    if xo in _STORES:
        source, update = _STORES[xo]
        if update:
            return _store([source(rd), _r(ra), _r(rb)], [_r(ra)])
        return _store([source(rd), _r(rb)] + _ra0(ra))
    if xo == 19:
        return Decoded("alu", [_cr(n) for n in range(8)], [_r(rd)])
    if xo in _LOADS:
        return _load(_ra0(ra) + [_r(rb)], [_LOADS[xo](rd)])
    if xo in _UPDATE_LOADS:
        return _load([_r(ra), _r(rb)], [_UPDATE_LOADS[xo](rd), _r(ra)])
    if xo in _LOGICAL:
        reads = [_r(rd)] + ([] if xo in (26, 824, 922, 954) else [_r(rb)])
        writes = [_r(ra)] + (["ca"] if xo in (792, 824) else [])
        return _rc(Decoded("alu", reads, writes), ins)
    if xo in (246, 278, 598, 854):
        return Decoded("nop")
    if xo == 83:
        return Decoded("alu", ["msr"], [_r(rd)])
    if xo == 144:
        fxm = (ins >> 12) & 0xFF
        return Decoded("alu", [_r(rd)], [_cr(n) for n in range(8) if fxm & (0x80 >> n)])
    if xo in (339, 371):
        spr = ((ins >> 16) & 0x1F) | (((ins >> 11) & 0x1F) << 5)
        d = Decoded("alu", [], [_r(rd)])
        if xo == 371 or spr in (22, 268, 269):
            d.clock = True
        elif spr == 1:
            d.reads |= {"ca", "so"}
        elif spr == 8:
            d.reads.add("lr")
        elif spr == 9:
            d.reads.add("ctr")
        else:
            d.reads.add("spr%d" % spr)
        return d
    if xo == 467:
        spr = ((ins >> 16) & 0x1F) | (((ins >> 11) & 0x1F) << 5)
        if spr == 8:
            return Decoded("alu", [_r(rd)], ["lr"])
        if spr == 9:
            return Decoded("alu", [_r(rd)], ["ctr"])
        if spr == 1:
            return Decoded("alu", [_r(rd)], ["ca", "so"])
        return Decoded("impure")
    if xo == 512:
        return Decoded("alu", ["ca", "so"], [_cr(rd >> 2), "ca", "so"])
    return Decoded("unknown")


def classify(image: Image, head: int, back_edges: list[int]):
    """The loop's class, or None when it is not a wait (see the module doc)."""
    end = max(back_edges)
    region = list(range(head, end + 4, 4))
    decoded = {address: decode(image.read32(address), address) for address in region}
    effects = False  # a call or a store: only the runtime can confirm a fixed point
    for address in region:
        d = decoded[address]
        if d.kind in ("impure", "unknown"):
            return None
        if d.kind in ("call", "store"):
            effects = True
        if d.kind == "branch" and head < d.target <= address:
            return None  # an inner loop
    preds = {address: [] for address in region}
    exits = 0
    for address in region:
        d = decoded[address]
        falls = not (d.kind in ("branch", "exit") and not d.conditional)
        if d.kind == "branch":
            if head < d.target <= end:
                preds[d.target].append(address)
            elif d.target != head:
                exits += 1
        elif d.kind == "exit":
            exits += 1
        if falls:
            if address + 4 <= end:
                preds[address + 4].append(address)
            else:
                exits += 1
    must = {head: frozenset()}
    for address in region[1:]:
        incoming = [p for p in preds[address] if p in must]
        if not incoming:
            continue
        result = None
        for p in incoming:
            defined = must[p] | decoded[p].writes
            result = defined if result is None else result & defined
        must[address] = frozenset(result)
    if not any(edge in must for edge in back_edges):
        return None  # no back edge is reachable within an iteration
    written = set()
    for address in must:
        written |= decoded[address].writes
    for address in must:
        if any(reg in written and reg not in must[address] for reg in decoded[address].reads):
            return None  # a loop-carried register
    if effects:
        return "progress"
    if exits == 0:
        return "halt"
    if any(decoded[address].clock for address in must):
        return "clock"
    if any(decoded[address].memory for address in must):
        return "poll"
    return "spin"


def wait_points(dol: bytes, functions: bytes):
    image = Image(dol)
    records = []
    for row in csv.DictReader(io.StringIO(functions.decode("utf-8"))):
        records.append((int(row["addr_hex"], 16), int(row["size"], 0)))
    edges = {}
    for start, size in records:
        for address in range(start, start + size, 4):
            if not image.text(address):
                break
            ins = image.read32(address)
            op = ins >> 26
            if ins & 1 or op not in (16, 18):
                continue
            if op == 18:
                offset = ins & 0x03FFFFFC
                if offset & 0x02000000:
                    offset -= 0x04000000
            else:
                offset = ins & 0xFFFC
                if offset & 0x8000:
                    offset -= 0x10000
            target = (offset if ins & 2 else address + offset) & 0xFFFFFFFF
            if start <= target <= address:
                edges.setdefault(target, set()).add(address)
    rows = []
    for head, sites in sorted(edges.items()):
        sites = sorted(sites)
        kind = classify(image, head, sites)
        if kind is not None:
            rows.extend((site, head, kind) for site in sites)
    return sorted(rows)


def render(dol: Path, functions: Path) -> str:
    lines = ["# Wait points for ppcrecomp --wait-points: back_edge_site loop_head class.",
             "# Generated by tools/ppcrecomp/wait_points.py: every polling loop of the DOL's function",
             "# records - proven waits (poll clock spin halt: Cpu::idle_wait) and loops whose iteration",
             "# the runtime checks for a fixed point (progress: Cpu::poll_point). Do not edit;",
             "# regenerate (the native_wait_points test refuses a stale file)."]
    for site, head, kind in wait_points(dol.read_bytes(), functions.read_bytes()):
        lines.append("0x%08X 0x%08X %s" % (site, head, kind))
    return "\n".join(lines) + "\n"


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dol", type=Path, required=True)
    parser.add_argument("--functions", type=Path, required=True)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--out", type=Path)
    group.add_argument("--check", type=Path)
    args = parser.parse_args(argv)
    text = render(args.dol, args.functions)
    count = sum(1 for line in text.splitlines() if not line.startswith("#"))
    if args.out is not None:
        with args.out.open("x", encoding="utf-8", newline="\n") as stream:
            stream.write(text)
        print(f"{count} wait points written to {args.out}")
        return 0
    if args.check.read_text(encoding="utf-8") != text:
        print("wait points file is stale; regenerate it with --out")
        return 1
    print(f"wait points current: {count} back edges")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
