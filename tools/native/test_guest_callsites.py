#!/usr/bin/env python3
"""Tests for guest_callsites.py (stdlib unittest)."""

from __future__ import annotations

import contextlib
import io
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import guest_callsites as gc  # noqa: E402

OWNER = """\
#include "gekko/native_guest_call.h"
namespace gekko {
constexpr u32 kRwRasterLock = 0x8048AFC0u;   // RwRasterLock(raster, level, mode); guest_call(c, 0x80000000u, ...) in a comment
constexpr u32 kTable = 0x90001000u;          // a data address: not executable
constexpr u64 kBound = 4'000'000ull;
bool call_guest(Cpu& c, u32 function, u32 r3 = 0u) {
    recomp::call_indirect(c, function, 0x80001000u);
    return !c.stopped();
}
bool NativeThing::call_guest(Cpu& c, u32 function, u32 r3) { return ::gekko::call_guest(c, function, r3); }
void owner(Cpu& c) {
    u32 ignored = 0;
    const char* text = "guest_call(c, 0x80111111u, link, {}, ignored)";
    if (!guest_call(c, kRwRasterLock, link, {raster, 0u, mode}, ignored)) return;
    guest_call(c, 0x8048A168u, link, {scratch + 0x20u, pixel,
                                      static_cast<u32>(c.mem_read8(raster + 1)) << 8}, ignored);
    guest_call(c, c.mem_read32(engine + 0x108u), link, {rows, 0x30411u}, ignored);
    call_guest(c, kShared, 1u);
    call_guest(c, kTable);
    interrupts_.call_guest(c, frame, kOsWakeupThread, queue, 0u);
    guest_continue(c, 0x80200010u, return_pc, return_sp);
}
}
"""

OTHER = """\
constexpr u32 kShared = 0x80300000u;
enum : u32 { kOsWakeupThread = 0x80377000u, kAmbiguous = 0x80400000u };
void wrapper(Cpu& c) { call_guest(c, kAmbiguous); }
"""

THIRD = "constexpr u32 kAmbiguous = 0x80400004u;\n"

FUNCTIONS = """\
addr_hex,addr,name,size,ins,tags,strings,callees
0x80200000,0,"fn_80200000",64,16,"","",""
0x80300000,0,"shared_fn",32,8,"","",""
0x8048a168,0,"fn_8048a168",16,4,"","",""
"""


class ScanTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name) / "runtime"
        (self.root / "src").mkdir(parents=True)
        (self.root / "tests").mkdir()
        (self.root / "src" / "owner.cpp").write_text(OWNER, encoding="utf-8")
        (self.root / "src" / "other.cpp").write_text(OTHER, encoding="utf-8")
        (self.root / "src" / "third.h").write_text(THIRD, encoding="utf-8")
        (self.root / "tests" / "owner_tests.cpp").write_text("void t(Cpu& c) { guest_call(c, 0x80123456u, 0u, {}, r); }\n",
                                                             encoding="utf-8")
        self.result = gc.scan(self.root)
        self.sites = {(s["file"], s["line"]): s for s in self.result["sites"]}

    def site(self, line: int, name: str = "src/owner.cpp") -> dict:
        return self.sites[(name, line)]

    def test_definitions_comments_strings_and_tests_are_not_sites(self):
        self.assertEqual(sorted(self.sites), [
            ("src/other.cpp", 3), ("src/owner.cpp", 7), ("src/owner.cpp", 10), ("src/owner.cpp", 14),
            ("src/owner.cpp", 15), ("src/owner.cpp", 17), ("src/owner.cpp", 18), ("src/owner.cpp", 19),
            ("src/owner.cpp", 20), ("src/owner.cpp", 21)])

    def test_classification(self):
        self.assertEqual((self.site(14)["kind"], self.site(14)["target"], self.site(14)["name"]),
                         ("constant", 0x8048AFC0, "kRwRasterLock"))
        self.assertEqual((self.site(15)["kind"], self.site(15)["target"]), ("literal", 0x8048A168))
        self.assertEqual((self.site(17)["kind"], self.site(17)["target"]), ("loaded", None))
        self.assertEqual((self.site(7)["kind"], self.site(7)["form"]), ("variable", "call_indirect"))
        self.assertEqual(self.site(10)["kind"], "variable")  # the forwarding member helper

    def test_constant_from_another_file_only_when_unique(self):
        self.assertEqual((self.site(18)["kind"], self.site(18)["target"]), ("constant", 0x80300000))
        self.assertEqual((self.site(20)["form"], self.site(20)["target"]), ("interrupts", 0x80377000))
        ambiguous = self.site(3, "src/other.cpp")  # the same file's own definition wins over another file's
        self.assertEqual(ambiguous["target"], 0x80400000)

    def test_non_code_constant_is_not_a_target(self):
        self.assertEqual((self.site(19)["kind"], self.site(19)["target"]), ("variable", None))

    def test_guest_continue_entry(self):
        self.assertEqual((self.site(21)["form"], self.site(21)["target"]), ("guest_continue", 0x80200010))

    def test_summary_and_function_names(self):
        functions_file = Path(self.tmp.name) / "functions.csv"
        functions_file.write_text(FUNCTIONS, encoding="utf-8")
        summary = gc.summarize(self.result, gc.load_functions(functions_file))
        rows = {row["address"]: row for row in summary["targets"]}
        self.assertEqual(sorted(rows), [0x80200010, 0x80300000, 0x80377000, 0x80400000, 0x8048A168, 0x8048AFC0])
        self.assertEqual((rows[0x80200010]["function"], rows[0x80200010]["is_entry"]), ("fn_80200000", False))
        self.assertEqual((rows[0x80300000]["function"], rows[0x80300000]["is_entry"]), ("shared_fn", True))
        self.assertIsNone(rows[0x8048AFC0]["function"])
        self.assertEqual(summary["per_file"]["src/owner.cpp"], {"literal": 2, "constant": 3, "loaded": 1, "variable": 3})
        text = gc.render(self.result, summary)
        self.assertIn("| 0x80200010 | 1 |  | fn_80200000 (mid-function) | src/owner.cpp |", text)
        self.assertIn("| 0x8048AFC0 | 1 | kRwRasterLock | NOT IN functions.csv | src/owner.cpp |", text)
        self.assertIn("| src/owner.cpp:17 | guest_call | loaded | `c.mem_read32(engine + 0x108u)` |", text)

    def test_main_writes_address_list(self):
        out = Path(self.tmp.name) / "targets.txt"
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(gc.main(["--root", str(self.root), "--addresses", str(out)]), 0)
        self.assertEqual(out.read_text(encoding="ascii").split(),
                         ["80200010", "80300000", "80377000", "80400000", "8048A168", "8048AFC0"])
        with contextlib.redirect_stderr(io.StringIO()), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(gc.main(["--root", str(Path(self.tmp.name) / "nothing")]), 2)


class HelperTests(unittest.TestCase):
    def test_strip_comments_keeps_lines_and_digit_separators(self):
        text = "a /* x\ny */ b // c\nconstexpr u64 k = 4'000'000ull; char q = 'x';\n"
        stripped = gc.strip_comments(text)
        self.assertEqual(stripped.count("\n"), text.count("\n"))
        self.assertIn("4'000'000ull", stripped)
        self.assertNotIn("x", stripped.split("\n")[0])

    def test_split_arguments_nested(self):
        text = "f(a, {b, c}, g(d, e), h)"
        self.assertEqual(gc.split_arguments(text, 1)[0], ["a", "{b, c}", "g(d, e)", "h"])
        self.assertIsNone(gc.split_arguments("f(a, b", 1))


if __name__ == "__main__":
    unittest.main()
