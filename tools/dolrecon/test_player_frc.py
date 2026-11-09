"""Controls for dolrecon.player_frc: synthetic functions whose frC is a double result, a wide or a single constant, a
copy of a double, a single load or frsp must be classified as such (the classifier found no double-defined frC in the
whole game; this proves it can).

    python -B -m dolrecon.test_player_frc
"""
from __future__ import annotations

import collections
import struct

from dolrecon.player_frc import Code, frc_of, verdict

BASE = 0x90000000
R2 = 0x805FA780
BLR = 0x4E800020


def a_form(op, d, a, b, c, xo):
    return (op << 26) | (d << 21) | (a << 16) | (b << 11) | (c << 6) | (xo << 1)


def fmul(d, a, c):
    return a_form(63, d, a, 0, c, 25)


def fadd(d, a, b):
    return a_form(63, d, a, b, 0, 21)


def fmuls(d, a, c):
    return a_form(59, d, a, 0, c, 25)


def fmr(d, b):
    return (63 << 26) | (d << 21) | (b << 11) | (72 << 1)


def frsp(d, b):
    return (63 << 26) | (d << 21) | (b << 11) | (12 << 1)


def lfs(d, ra, disp):
    return (48 << 26) | (d << 21) | (ra << 16) | (disp & 0xFFFF)


def lfd(d, ra, disp):
    return (50 << 26) | (d << 21) | (ra << 16) | (disp & 0xFFFF)


class FakeImage:
    def __init__(self, words, data):
        self.words, self.data = words, data

    def word(self, a):
        return self.words[a]

    def read(self, a, n):
        return self.data[a][:n]


def classify(program, data=None) -> set:
    code = Code.__new__(Code)
    words = {BASE + 4 * k: w for k, w in enumerate(program)}
    code.image = FakeImage(words, data or {})
    code.fn = {BASE: {"size": str(4 * len(program)), "name": "synthetic"}}
    code.starts = [BASE]
    code.callers = collections.defaultdict(set)
    code.virtual_callers = collections.defaultdict(set)
    code.memo = {}
    site = next(k for k, w in enumerate(program) if (w >> 26) == 59)
    return code.reaching(frc_of(program[site]), BASE + 4 * site, BASE, 3, set())


def main() -> int:
    wide = struct.pack(">d", 0.1)          # 0.1 is not a single
    narrow = struct.pack(">d", 0.5)
    cases = [
        ("double result", [fmul(3, 1, 2), fmuls(4, 5, 3), BLR], None, "UNRESOLVED", {"double"}),
        ("copy of a double", [fadd(3, 1, 2), fmr(6, 3), fmuls(4, 5, 6), BLR], None, "UNRESOLVED", {"double"}),
        ("wide constant", [lfd(3, 2, -0x10), fmuls(4, 5, 3), BLR], {R2 - 0x10: wide}, "AFFECTED", None),
        ("single constant", [lfd(3, 2, -0x10), fmuls(4, 5, 3), BLR], {R2 - 0x10: narrow}, "PROVEN_UNAFFECTED", None),
        ("single load", [lfs(3, 2, -0x10), fmuls(4, 5, 3), BLR], None, "PROVEN_UNAFFECTED", {"single"}),
        ("frsp of a double", [fmul(3, 1, 2), frsp(3, 3), fmuls(4, 5, 3), BLR], None, "PROVEN_UNAFFECTED", {"single"}),
        ("non-volatile parameter", [fmuls(4, 5, 20), BLR], None, "UNRESOLVED", {"param-unresolved"}),
    ]
    failed = 0
    for name, program, data, want, want_defs in cases:
        defs = classify(program, data)
        got = verdict(defs, 0)
        ok = got == want and (want_defs is None or defs == want_defs)
        failed += not ok
        print(f"{'PASS' if ok else 'FAIL'} {name}: {got} {sorted(defs)}")
    print("FAIL" if failed else "PASS", "player_frc controls")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
