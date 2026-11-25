#!/usr/bin/env python3
"""Tests for watch_summary.py (stdlib unittest)."""

from __future__ import annotations

import contextlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import watch_summary as ws  # noqa: E402

CSV = """\
# guest watch (docs/GC_REMOVAL_PLAN.md gate 2, dynamic half)
# symbols yes (exact function extents)
# faults 5000, outside the ranges (same page) 1200, unattributed 0
label,kind,row,native_rva,native_symbol,count,first_guest_address
vpb,read,0x00000000,0x24EE3,``anonymous namespace'::state_report_json'::`2'::<lambda_1>::operator()+0xE23,64,0x805989A0
vpb,read,0x8048AD94,0x0,,3000,0x80599F70
vpb,write,0x8043D2F4,0x0,,600,0x8059D06A
vpb,read,0x00000000,0x1234,gekko::ax_sync::sync_pbs+0x40,150,0x805989A4
out,write,0x8048AD94,0x0,,1920,0x8058B1C0
aux,read,0x00000000,0x24EE3,``anonymous namespace'::state_report_json'::`2'::<lambda_1>::operator()+0xE23,8,0x80587EC0
"""

GRAPH = "ppcrecomp-live-graph-v1\nrow 0x8043D2F4 1528 body live gcax_voice_update\nrow 0x8048AD94 76 body live fn_8048ad94\n"


class WatchSummaryTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        (self.root / "watch.csv").write_text(CSV, encoding="utf-8")
        (self.root / "graph.txt").write_text(GRAPH, encoding="utf-8")

    def summary(self, frames=6, native_under=None):
        report = ws.read_report(self.root / "watch.csv")
        return ws.summarise(report, frames, ws.load_names(str(self.root / "graph.txt")), native_under or {})

    def test_totals_and_exit_report_rows(self):
        report = ws.read_report(self.root / "watch.csv")
        self.assertEqual(report["totals"], {"faults": 5000, "outside_ranges": 1200, "unattributed": 0})
        self.assertEqual(sum(1 for row in report["rows"] if row["exit_report"]), 2)
        summary = self.summary()
        self.assertEqual(summary["exit_report_accesses"], 72)
        self.assertEqual(summary["untouched_except_exit_report"], ["aux"])       # only the exit report read it
        self.assertEqual((summary["labels"]["vpb"]["read"], summary["labels"]["vpb"]["write"]), (3150, 600))

    def test_accessors_classes_and_rates(self):
        summary = self.summary(native_under={0x8048AD94: "audio interrupt"})
        accessors = summary["labels"]["vpb"]["accessors"]
        self.assertEqual([a["count"] for a in accessors], [3000, 600, 150])      # sorted by count
        self.assertEqual(accessors[0]["who"], "0x8048AD94 [hosts native work: audio interrupt]")
        self.assertEqual(accessors[0]["class"], "native under a row")
        self.assertEqual((accessors[1]["who"], accessors[1]["class"]), ("0x8043D2F4 gcax_voice_update", "translated row"))
        self.assertEqual((accessors[2]["who"], accessors[2]["class"]), ("native: gekko::ax_sync::sync_pbs", "native"))
        self.assertEqual(accessors[0]["per_frame"], 500.0)
        self.assertIsNone(self.summary(frames=0)["labels"]["vpb"]["accessors"][0]["per_frame"])

    def test_render_and_main(self):
        text = ws.render(self.summary(), "watch.csv")
        self.assertIn("| vpb | 3,150 | 600 | 525.0 | 100.0 |", text)
        self.assertIn("| aux | 0 | 0 | 0 | 0 |", text)
        self.assertIn("| 0x8043D2F4 gcax_voice_update | write | 600 | 100.00 | 0x8059D06A |", text)
        self.assertIn("72 accesses by the run's own exit reports are left out below", text)
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(ws.main([str(self.root / "watch.csv"), "--frames", "6", "--native-under", "8048AD94=irq",
                                      "--json", str(self.root / "out.json")]), 0)
            self.assertEqual(ws.main([str(self.root / "missing.csv")]), 2)
        self.assertEqual(json.loads((self.root / "out.json").read_text(encoding="utf-8"))["frames"], 6)

    def test_clean_symbol(self):
        self.assertEqual(ws.clean_symbol("gekko::Foo::bar+0x1F"), "gekko::Foo::bar")
        self.assertEqual(ws.clean_symbol(""), "(no symbol)")


if __name__ == "__main__":
    unittest.main()
