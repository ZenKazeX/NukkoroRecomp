#!/usr/bin/env python3
"""Tests for the parts of state_readers.py that do not need the DOL (the instruction scan is
tools/platform/retired_readers.py's and is exercised by the real run)."""

from __future__ import annotations

import io
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import state_readers as sr  # noqa: E402
from closure_report import Graph  # noqa: E402

GRAPH = """\
ppcrecomp-live-graph-v1
row 0x80001000 16 body live game_main
row 0x80002000 16 owner live AIInit
row 0x80002010 16 body pruned ai_helper
seed entry 0x80001000 0x80001000
seed root 0x80002000 0x80002000
edge 0x80001000 0x80002000 0x80002000
"""


class HelperTests(unittest.TestCase):
    def test_overlap_finder(self):
        find = sr.overlap_finder(sr.merge_spans([(0x100, 4), (0x200, 8), (0x100, 4)]))
        self.assertEqual(find(0x100, 4), (0x100, 0x104))
        self.assertEqual(find(0x102, 1), (0x100, 0x104))
        self.assertEqual(find(0xFE, 4), (0x100, 0x104))      # a wider load that covers the first written byte
        self.assertIsNone(find(0x104, 4))
        self.assertEqual(find(0x207, 1), (0x200, 0x208))
        self.assertIsNone(find(0x208, 1))
        self.assertIsNone(sr.overlap_finder([])(0x100, 4))

    def test_sda_offset_hints_need_the_base_on_the_line_and_skip_tests(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "runtime"
            (root / "src").mkdir(parents=True)
            (root / "tests").mkdir()
            (root / "src" / "audio.cpp").write_text(
                "constexpr u32 kSdaAiCallback = 0x5C64u;\n"
                "c.mem_write32(sda + 0x5D00u, value);\n"
                "const u32 unrelated = 0x5C64u;\n"
                "u32 other = c.gpr[13] + 0x1234u;  // r13 small data\n", encoding="utf-8")
            (root / "tests" / "audio_tests.cpp").write_text("c.mem_read32(sda + 0x5C64u);\n", encoding="utf-8")
            hints = sr.sda_offset_hints(root, {0x805F2164: 0x5C64, 0x805F2200: 0x5D00, 0x805F0000: 0x9999})
        self.assertEqual(hints[0x805F2164], ["runtime/src/audio.cpp:1: constexpr u32 kSdaAiCallback = 0x5C64u;"])
        self.assertEqual(hints[0x805F2200], ["runtime/src/audio.cpp:2: c.mem_write32(sda + 0x5D00u, value);"])
        self.assertNotIn(0x805F0000, hints)
        self.assertEqual(sr.sda_offset_hints(root, {}), {})


class RenderTests(unittest.TestCase):
    def test_render(self):
        graph = Graph(io.StringIO(GRAPH))
        result = {
            "members": [0x80002000, 0x80002010], "surviving_rows": 1, "sda": {"r2": 1, "r13": 2},
            "unproven_stores": {0x80002010: 3}, "surviving_unproven_loads": 7,
            "writes": [
                {"ea": 0x805F2164, "width": 4, "object": "lbl_805F2164+0x0", "kind": "guest RAM", "sda_offset": 0x5C64,
                 "writers": [0x80002000], "sites": [0x80002008],
                 "translated_readers": [{"row": 0x80001000, "name": "game_main", "pc": 0x80001004, "ea": 0x805F2164, "width": 4}],
                 "native_literal": [], "native_offset_hints": ["runtime/src/audio.cpp:1: constexpr u32 kSdaAiCallback = 0x5C64u;"]},
                {"ea": 0xCC006C00, "width": 4, "object": None, "kind": "hardware register", "sda_offset": None,
                 "writers": [0x80002010], "sites": [0x80002014], "translated_readers": [],
                 "native_literal": [{"address": 0xCC006C00, "files": ["runtime/src/gekko/native_audio.cpp"]}],
                 "native_offset_hints": []},
            ],
        }
        text = sr.render(graph, result, "AIInit", "graph.txt")
        self.assertIn("2 fixed-address locations written: 1 in guest RAM, 1 hardware registers. 2 have a proven reader", text)
        self.assertIn("Not placed: 3 stores of the set through unproven pointers, 7 loads", text)
        self.assertIn("| 0x805F2164 | 4 | lbl_805F2164+0x0 | 0x5C64 | 0x80002000 | 0x80001000 | - | audio.cpp |", text)
        self.assertIn("| 0xCC006C00 | 4 | (hardware register) |  | 0x80002010 | - | native_audio.cpp | - |", text)
        self.assertIn("- 0x80002000 AIInit (owner): 1 fixed-address locations, 0 stores through unproven pointers", text)
        self.assertIn("- 0x80002010 ai_helper (pruned): 1 fixed-address locations, 3 stores through unproven pointers", text)
        self.assertIn("translated 0x80001000 game_main loads 4 bytes at pc 0x80001004", text)


if __name__ == "__main__":
    unittest.main()
