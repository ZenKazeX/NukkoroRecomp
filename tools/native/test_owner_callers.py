#!/usr/bin/env python3
"""Tests for owner_callers.py (stdlib unittest)."""

from __future__ import annotations

import contextlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import owner_callers as oc  # noqa: E402
from closure_report import Graph  # noqa: E402

GRAPH = """\
ppcrecomp-live-graph-v1
row 0x80001000 16 body live game_main
row 0x80001010 16 body live game_other
row 0x80001020 16 body live rw_draw
row 0x80002000 16 owner live VISetBlack
row 0x80002010 32 owner live VIWaitForRetrace
row 0x80002030 16 owner live OSReport
row 0x80002040 16 owner live TRKWrite
row 0x80002050 16 body pruned dead_caller
seed entry 0x80001000 0x80001000
seed root 0x80002000 0x80002000
seed root 0x80002010 0x80002010
seed root 0x80002030 0x80002030
seed root 0x80002040 0x80002040
seed data 0x80500000 0x80002040
edge 0x80001000 0x80001010 0x80001010
edge 0x80001000 0x80001020 0x80001020
edge 0x80001000 0x80002000 0x80002000
edge 0x80001010 0x80002000 0x80002000
edge 0x80001010 0x80002018 0x80002010
edge 0x80001020 0x80002030 0x80002030
edge 0x80002050 0x80002040 0x80002040
edge 0x80002000 0x80002010 0x80002010
"""

OWNERS = """\
# Native owners. A comment that only mentions runtime/src/gekko/other.cpp mid-line opens nothing.
0x80002030 OSReport
#
# runtime/src/gekko/native_video.cpp (NativeVideo::install): the VI library's entries
# that the game calls.
0x80002000 VISetBlack
0x80002018 entry inside VIWaitForRetrace
0x80002000 VISetBlack (listed twice)
# runtime/src/gekko/native_trk.cpp (trk::install)
0x80002040 TRKWrite
0x80009000 not in the graph
# runtime/src/gekko/native_misc.cpp (a hook table)
0x80001020 a body row
"""

MANIFEST = {"entries": [{"address": "0x80001020", "service": "graphics.rw"}]}
SKELETON = "address,size,layer\n0x80001000,16,game\n0x80001010,16,game\n0x80001020,16,rw\n"


class OwnerCallersTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        (self.root / "graph.txt").write_text(GRAPH, encoding="utf-8")
        (self.root / "owners.txt").write_text(OWNERS, encoding="utf-8")
        (self.root / "dolrecon" / "pal-v30").mkdir(parents=True)
        (self.root / "dolrecon" / "skeleton").mkdir()
        (self.root / "dolrecon" / "pal-v30" / "platform_manifest.json").write_text(json.dumps(MANIFEST), encoding="utf-8")
        (self.root / "dolrecon" / "skeleton" / "functions.csv").write_text(SKELETON, encoding="utf-8")
        self.graph = Graph(io.StringIO(GRAPH))

    def test_sections(self):
        owned = oc.read_owner_lists([self.root / "owners.txt"])
        self.assertEqual(list(owned), [oc.UNNAMED, "runtime/src/gekko/native_video.cpp", "runtime/src/gekko/native_trk.cpp",
                                       "runtime/src/gekko/native_misc.cpp"])
        self.assertEqual([a for a, _ in owned["runtime/src/gekko/native_video.cpp"]], [0x80002000, 0x80002018])
        self.assertEqual(owned[oc.UNNAMED], [(0x80002030, "OSReport")])

    def test_callers_per_file(self):
        owned = oc.read_owner_lists([self.root / "owners.txt"])
        results = {item["file"]: item for item in oc.analyse(self.graph, owned, oc.Layers(self.root / "dolrecon"))}
        video = results["runtime/src/gekko/native_video.cpp"]
        self.assertEqual(video["rows"], [0x80002000, 0x80002010])            # the mid-row entry maps to its row
        self.assertEqual(video["callers"], {0x80001000: [0x80002000], 0x80001010: [0x80002000, 0x80002010]})
        self.assertEqual(video["by_layer"], {"layer:game": 2})                # the owner-to-owner edge is not a caller
        self.assertEqual((video["entries_called"], video["entries_uncalled"]), (2, []))
        trk = results["runtime/src/gekko/native_trk.cpp"]
        self.assertEqual(trk["callers"], {})                                  # its only caller is pruned
        self.assertEqual((trk["data_words"], trk["roots"]), (1, 1))
        self.assertEqual((trk["missing"], trk["not_owner"]), ([0x80009000], []))
        misc = results["runtime/src/gekko/native_misc.cpp"]
        self.assertEqual((misc["not_owner"], misc["callers"]), ([0x80001020], {0x80001000: [0x80001020]}))
        self.assertEqual(results[oc.UNNAMED]["by_layer"], {"graphics.rw": 1})  # manifest service wins over the layer

    def test_order_and_layers_optional(self):
        owned = oc.read_owner_lists([self.root / "owners.txt"])
        results = oc.analyse(self.graph, owned, oc.Layers(None))
        self.assertEqual([item["file"] for item in results][0], "runtime/src/gekko/native_trk.cpp")   # fewest callers
        self.assertEqual(results[-1]["by_layer"], {"translated": 2})

    def test_main(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = oc.main(["--graph", str(self.root / "graph.txt"), "--owners", str(self.root / "owners.txt"),
                            "--dolrecon-dir", str(self.root / "dolrecon"), "--files", "native_v*.cpp", "native_trk.cpp",
                            "--json", str(self.root / "out.json")])
        text = out.getvalue()
        self.assertEqual(code, 0)
        self.assertIn("| runtime/src/gekko/native_video.cpp | 2 | 2 | 2 | 0 | 2 | layer:game 2 |", text)
        self.assertIn("No live translated function calls any entry of this file (1 data words and 1 roots still name them).", text)
        self.assertNotIn(oc.UNNAMED, text)
        self.assertIn("| 0x80001010 game_other | layer:game | 0x80002000 VISetBlack, 0x80002010 VIWaitForRetrace |", text)
        self.assertEqual(len(json.loads((self.root / "out.json").read_text(encoding="utf-8"))), 2)
        with contextlib.redirect_stderr(io.StringIO()), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(oc.main(["--graph", str(self.root / "graph.txt"), "--owners", str(self.root / "owners.txt"),
                                      "--files", "nothing.cpp"]), 2)


if __name__ == "__main__":
    unittest.main()
