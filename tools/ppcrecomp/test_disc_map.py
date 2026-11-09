"""disc_map.py on two synthetic releases of one tiny program (no game bytes).

The canonical DOL holds: an entry that sets r2/r13 and calls f0; f0, which forms the addresses of a function
table and a data word; f1, a leaf; f2, a body the other release changed (and lengthened by two words); f3, which
calls f1 and f0 and reads a small-data word. The other release moves f3 by 8 bytes and every data object by
0x20 bytes (so r13 moves too).
"""
from __future__ import annotations

import hashlib
import json
import struct
import tempfile
import unittest
from pathlib import Path

import disc_map as dm

TEXT = 0x80004000
DATA = 0x80008000
BLR, NOP, MFLR = 0x4E800020, 0x60000000, 0x7C0802A6


def ha(a): return ((a >> 16) + (1 if a & 0x8000 else 0)) & 0xFFFF
def lo(a): return a & 0xFFFF
def lis(rd, imm): return (15 << 26) | (rd << 21) | imm
def ori(ra, rs, imm): return (24 << 26) | (rs << 21) | (ra << 16) | imm
def addi(rd, ra, imm): return (14 << 26) | (rd << 21) | (ra << 16) | (imm & 0xFFFF)
def lwz(rd, d, ra): return (32 << 26) | (rd << 21) | (ra << 16) | (d & 0xFFFF)
def li(rd, v): return addi(rd, 0, v)
def bl(at, target): return 0x48000001 | ((target - at) & 0x03FFFFFC)


def program(other: bool, corrupt: bool = False):
    shift = 0x20 if other else 0
    table, d_word, e_word = DATA + shift, DATA + shift + 0x10, DATA + shift + 0x20
    r13 = DATA + shift + 0x30
    f2_len = 8 if other else 6
    starts = {"start": TEXT, "f0": TEXT + 24, "f1": TEXT + 24 + 28, "f2": TEXT + 24 + 28 + 24}
    starts["f3"] = starts["f2"] + 4 * f2_len
    s = starts
    code = {
        "start": [lis(13, r13 >> 16), ori(13, 13, lo(r13)), lis(2, r13 >> 16), ori(2, 2, lo(r13)), None, BLR],
        "f0": [MFLR, lis(3, ha(table)), addi(3, 3, lo(table)), lis(4, ha(d_word)), lwz(4, lo(d_word), 4), None,
               BLR],
        "f1": [li(3, 1), NOP, NOP, NOP, NOP, BLR],
        "f2": ([li(3, 9), li(4, 8), li(5, 7), li(6, 6), li(7, 5), li(8, 4), li(9, 3), BLR] if other else
               [li(3, 2), li(4, 3), li(5, 4), li(6, 5), li(7, 6), BLR]),
        "f3": [MFLR, None, lwz(5, e_word - r13, 13), NOP, None, BLR],
    }
    code["start"][4] = bl(s["start"] + 16, s["f0"])
    code["f0"][5] = bl(s["f0"] + 20, s["f1"])
    code["f3"][1] = bl(s["f3"] + 4, s["f2"] if corrupt else s["f1"])
    code["f3"][4] = bl(s["f3"] + 16, s["f0"])
    words = [w for name in ("start", "f0", "f1", "f2", "f3") for w in code[name]]
    text = struct.pack(f">{len(words)}I", *words)
    data = b"\0" * shift + struct.pack(">4I", s["f0"], s["f3"], 0, 0) + struct.pack(">4I", 0x12345678, 0, 0, 0) \
        + struct.pack(">4I", 0xCAFEF00D, 0, 0, 0) + b"\0" * 16
    header = bytearray(0x100)
    struct.pack_into(">I", header, 0, 0x100)
    struct.pack_into(">I", header, 7 * 4, 0x100 + len(text))
    struct.pack_into(">I", header, 72, TEXT)
    struct.pack_into(">I", header, 72 + 7 * 4, DATA)
    struct.pack_into(">I", header, 144, len(text))
    struct.pack_into(">I", header, 144 + 7 * 4, len(data))
    struct.pack_into(">3I", header, 216, DATA + 0x1000, 0x100, TEXT)
    sizes = {"start": 24, "f0": 28, "f1": 24, "f2": 4 * (8 if other else 6), "f3": 24}
    return bytes(header) + text + data, starts, sizes, dict(table=table, d=d_word, e=e_word)


class DiscMapTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.canon_raw, self.cs, self.csizes, self.cdata = program(False)
        self.other_raw, self.os, _, self.odata = program(True)
        self.canon = self.write("canon.dol", self.canon_raw)
        self.other = self.write("other.dol", self.other_raw)
        self.catalog = {"canonical": "TEST01", "discs": [
            self.disc("TEST01", self.canon_raw), self.disc("TEST02", self.other_raw)]}
        self.functions = self.dir / "functions.csv"
        rows = ["addr_hex,addr,name,size,ins,tags,strings,callees"]
        for name, a in self.cs.items():
            rows.append(f'0x{a:08x},{a},"fn_{a:08x}",{self.csizes[name]},{self.csizes[name] // 4},"","",'
                        f'"0x{self.cs["f1"]:08x}"')
        self.functions.write_text("\n".join(rows) + "\n", encoding="utf-8")

    def tearDown(self):
        self.tmp.cleanup()

    def write(self, name, raw):
        p = self.dir / name
        p.write_bytes(raw)
        return p

    @staticmethod
    def disc(game_id, raw):
        return {"game_id": game_id, "region": "test", "revision": 0,
                "main_dol": {"size": len(raw), "sha1": hashlib.sha1(raw).hexdigest()}}

    def build(self, other=None):
        return dm.build(self.catalog, dm.Dol(self.canon), dm.Dol(other or self.other), self.functions)

    def test_identify(self):
        self.assertEqual(dm.identify(self.catalog, dm.Dol(self.other))["game_id"], "TEST02")
        unknown = self.write("unknown.dol", self.other_raw + b"\0\0\0\0")
        with self.assertRaises(LookupError):
            dm.identify(self.catalog, dm.Dol(unknown))

    def test_map(self):
        m = dm.AddressMap(self.build())
        for name in ("start", "f0", "f1"):
            self.assertEqual(m.map(self.cs[name]), (self.os[name], "code"))
        self.assertEqual(m.map(self.cs["f3"] + 8), (self.os["f3"] + 8, "code"))
        target, why = m.map(self.cs["f2"])
        self.assertIsNone(target)
        self.assertIn("changed", why)
        for k in ("table", "d", "e"):  # lis/addi, lis/lwz and r13 forms
            self.assertEqual(m.map(self.cdata[k]), (self.odata[k], "data"))

    def test_branch_verification_refuses_a_wrong_pair(self):
        bad = self.write("bad.dol", program(True, corrupt=True)[0])
        self.catalog["discs"][1] = self.disc("TEST02", bad.read_bytes())
        # f3's body still normalizes identically (displacements are masked), but its bl now reaches f2: the
        # branch check must refuse the whole map rather than publish the pair
        with self.assertRaisesRegex(ValueError, "branches of identical bodies disagree"):
            self.build(bad)

    def test_constants_are_compared(self):
        # li r3,2 against li r3,9: an immediate that forms no address is part of the body
        self.assertNotEqual(dm.normalize_words([0x38600002]), dm.normalize_words([0x38600009]))
        # the low half applied to a lis register or to r13 is masked; a stack offset is not
        self.assertEqual(dm.normalize_words([lis(3, 1), lwz(4, 8, 3)]), dm.normalize_words([lis(3, 2), lwz(4, 12, 3)]))
        self.assertEqual(dm.normalize_words([lwz(4, 8, 13)]), dm.normalize_words([lwz(4, -8, 13)]))
        self.assertNotEqual(dm.normalize_words([lwz(4, 8, 1)]), dm.normalize_words([lwz(4, 12, 1)]))

    def test_rebase(self):
        m = dm.AddressMap(self.build())
        rb = dm.Rebaser(m, dm.Dol(self.canon), dm.Dol(self.other))
        owners = f"# owners\n0x{self.cs['f3']:08X} the owner\n0x{self.cs['f2']:08X} changed\n"
        self.assertEqual(rb.rebase_lines("native-owners", owners),
                         f"# owners\n0x{self.os['f3']:08X} the owner\n")
        site = self.cs["f3"] + 4  # bl f1: f3 moved, f1 did not, so the pinned word must be re-read
        word = dm.Dol(self.canon).word_at(site)
        out = rb.rebase_lines("lift-sites", f"site 0x{site:08X} 0x{word:08X} call x 2    # note 0x{site:08X}\n")
        new_word = dm.Dol(self.other).word_at(self.os["f3"] + 4)
        self.assertNotEqual(word, new_word)
        self.assertEqual(out, f"site 0x{self.os['f3'] + 4:08X} 0x{new_word:08X} call x 2    # note 0x{site:08X}\n")
        stale = rb.rebase_lines("platform-sites", f"0x{site:08X} 0x{word ^ 4:08X} kind\n")
        self.assertEqual(stale, "\n")
        self.assertIn("stale", rb.dropped[-1][2])
        functions = rb.rebase_functions(self.functions.read_text(encoding="utf-8"))
        self.assertIn(f'0x{self.os["f3"]:08x},{self.os["f3"]},"fn_{self.os["f3"]:08x}",24,6', functions)
        self.assertNotIn(f"0x{self.cs['f2']:08x}", functions)

    def test_check_refuses_a_stale_map(self):
        catalog = self.write("discs.json", json.dumps(self.catalog).encode())
        good = self.write("good.map", self.build().encode())
        args = ["check", "--catalog", str(catalog), "--canonical", str(self.canon), "--dol", str(self.other),
                "--functions", str(self.functions), "--check"]
        self.assertEqual(dm.main(args + [str(good)]), 0)
        stale = self.write("stale.map", self.build().replace("code", "code ", 1).encode())
        self.assertEqual(dm.main(args + [str(stale)]), 1)


if __name__ == "__main__":
    unittest.main()
