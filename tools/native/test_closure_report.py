#!/usr/bin/env python3
"""Tests for closure_report.py (stdlib unittest)."""

from __future__ import annotations

import contextlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import closure_report as cr  # noqa: E402

# entry -> main -> {sub_a, shared}; sub_a -> {helper, owner, shared}; helper -> leaf; vtable word -> sub_b -> leaf2;
# root -> rooted; dead is pruned. Subsystem under test: sub_a (+ sub_b).
GRAPH = """\
ppcrecomp-live-graph-v1
row 0x80001000 16 body live main
row 0x80001010 32 body live sub_a
row 0x80001030 16 body live helper
row 0x80001040 8 body live leaf
row 0x80001048 8 owner live owner
row 0x80001050 24 body live shared
row 0x80001068 16 body live sub_b
row 0x80001078 8 body live leaf2
row 0x80001080 8 body live rooted
row 0x80001088 8 body pruned dead
seed entry 0x80001000 0x80001000
seed data 0x80400000 0x80001068
seed root 0x80001080 0x80001080
edge 0x80001000 0x80001010 0x80001010
edge 0x80001000 0x80001050 0x80001050
edge 0x80001010 0x80001030 0x80001030
edge 0x80001010 0x80001048 0x80001048
edge 0x80001010 0x80001054 0x80001050
edge 0x80001030 0x80001040 0x80001040
edge 0x80001068 0x80001078 0x80001078
edge 0x80001048 0x80001088 0x80001088
"""


def graph(text: str = GRAPH) -> cr.Graph:
    return cr.Graph(io.StringIO(text))


class GraphTests(unittest.TestCase):
    def test_self_check_passes_and_owner_edges_are_not_followed(self):
        g = graph()
        self.assertEqual(g.self_check(), [])
        self.assertNotIn(0x80001088, g.reachable())   # only an owner row "calls" it

    def test_self_check_reports_both_directions(self):
        wrong = GRAPH.replace("row 0x80001040 8 body live leaf", "row 0x80001040 8 body pruned leaf").replace(
            "row 0x80001088 8 body pruned dead", "row 0x80001088 8 body live dead")
        problems = graph(wrong).self_check()
        self.assertEqual(len(problems), 2)
        self.assertIn("0x80001040", problems[0])
        self.assertIn("0x80001088", problems[1])

    def test_header_and_bad_lines(self):
        with self.assertRaises(cr.InputError):
            cr.Graph(io.StringIO("something else\n"))
        with self.assertRaises(cr.InputError):
            graph(GRAPH + "bogus 1 2 3\n")

    def test_row_containing(self):
        g = graph()
        self.assertEqual(g.row_containing(0x80001054), 0x80001050)
        self.assertIsNone(g.row_containing(0x80002000))
        self.assertIsNone(g.row_containing(0x80000FFF))


class AnalyseTests(unittest.TestCase):
    def setUp(self):
        self.g = graph()

    def test_single_member(self):
        result = cr.analyse(self.g, {0x80001010})
        self.assertEqual(dict(result["callers"]), {0x80001000: [(0x80001010, 0x80001010)]})
        self.assertEqual(result["seeds"], {"root": [], "data": [], "entry": []})
        self.assertEqual(sorted(result["reaches"]), [0x80001030, 0x80001048, 0x80001050])
        # helper and leaf are reachable only through sub_a; shared is also called by main; the owner is not a body
        self.assertEqual(result["freed"], {0x80001030, 0x80001040})
        self.assertEqual((result["bytes_members"], result["bytes_freed"]), (32, 24))
        self.assertEqual(result["live_bodies_total"], 8)

    def test_data_word_pin_and_internal_edges(self):
        result = cr.analyse(self.g, {0x80001010, 0x80001068, 0x80001030})
        self.assertEqual(result["seeds"]["data"], [(0x80001068, 0x80400000)])
        self.assertNotIn(0x80001030, result["reaches"])            # a member, not an outside callee
        self.assertEqual(result["freed"], {0x80001040, 0x80001078})

    def test_root_and_pruned_and_owner_members(self):
        result = cr.analyse(self.g, {0x80001080, 0x80001088, 0x80001048})
        self.assertEqual(result["seeds"]["root"], [(0x80001080, 0x80001080)])
        self.assertEqual(result["live_bodies"], {0x80001080})
        self.assertEqual(result["freed"], set())
        self.assertEqual(dict(result["callers"]), {0x80001010: [(0x80001048, 0x80001048)]})

    def test_select(self):
        members, notes = cr.select(self.g, [0x80001054, 0x80009000], [(0x80001060, 0x80001080)], "^sub_", [])
        self.assertEqual(members, {0x80001050, 0x80001068, 0x80001078, 0x80001010})
        self.assertEqual(notes, ["0x80009000 is not inside any row of the graph"])


class MainTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name) / "live_graph.txt"
        self.path.write_text(GRAPH, encoding="utf-8")

    def run_main(self, *arguments: str) -> tuple[int, str]:
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
            code = cr.main(["--graph", str(self.path), *arguments])
        return code, out.getvalue()

    def test_report_text_and_json(self):
        json_path = Path(self.tmp.name) / "out.json"
        listing = Path(self.tmp.name) / "list.txt"
        listing.write_text("# sub_a\n80001010\n", encoding="utf-8")
        code, text = self.run_main("--list", str(listing), "--label", "sub_a", "--json", str(json_path))
        self.assertEqual(code, 0)
        self.assertIn("1 rows: 1 live translated bodies (32 guest bytes), 0 native owners, 0 already pruned.", text)
        self.assertIn("| 0x80001000 main | 0x80001010 sub_a | 0x80001010 |", text)
        self.assertIn("1 native owners (already native) and 2 translated bodies (40 guest bytes", text)
        self.assertIn("2 further translated bodies (24 guest bytes) become unreachable", text)
        self.assertIn("3 bodies, 56 guest bytes = 43.750 % of the live translated code", text)
        payload = json.loads(json_path.read_text(encoding="utf-8"))
        self.assertEqual(payload["freed"], [0x80001030, 0x80001040])

    def test_exit_2_cases(self):
        self.assertEqual(self.run_main("--name", "^nothing$")[0], 2)
        self.path.write_text(GRAPH.replace("body live leaf2", "body pruned leaf2"), encoding="utf-8")
        self.assertEqual(self.run_main("--addr", "80001010")[0], 2)
        self.assertEqual(cr.main(["--graph", str(Path(self.tmp.name) / "missing.txt"), "--addr", "80001010"]), 2)


if __name__ == "__main__":
    unittest.main()
