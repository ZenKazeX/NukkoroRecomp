#!/usr/bin/env python3
"""Tests for state_diff.py (stdlib unittest)."""

from __future__ import annotations

import contextlib
import copy
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import state_diff as sd  # noqa: E402

BASE = {
    "format": "shadow-state-report 1", "game_ticks": 100, "vi_frames": 7, "render_frames": 50,
    "memory": {"block_bytes": 65536, "regions": [
        {"name": "image", "base": 0x80000000, "committed": 262144, "blocks": ["a0", "a1", "a2", "a3"]},
        {"name": "heap", "base": 0x80610000, "committed": 131072, "blocks": ["b0", "b1"]},
    ]},
}


def changed(**edits):
    report = copy.deepcopy(BASE)
    for key, value in edits.items():
        report[key] = value
    return report


class DiffTests(unittest.TestCase):
    def test_identical(self):
        result = sd.diff(BASE, copy.deepcopy(BASE))
        self.assertTrue(result["identical"] and result["guest_equal"])
        self.assertEqual(result["blocks_compared"], 6)
        self.assertIn("VERDICT: identical", sd.render(result, "a", "b", 40))

    def test_host_side_field_only(self):
        result = sd.diff(BASE, changed(render_frames=20))
        self.assertTrue(result["guest_equal"])
        self.assertFalse(result["identical"])
        text = sd.render(result, "a", "b", 40)
        self.assertIn("render_frames: A 50  B 20  (host-side: not guest state)", text)
        self.assertIn("VERDICT: guest state equal; only host-side header fields differ (render_frames)", text)

    def test_guest_header_field(self):
        result = sd.diff(BASE, changed(vi_frames=8))
        self.assertFalse(result["guest_equal"])
        self.assertIn("VERDICT: guest state differs", sd.render(result, "a", "b", 40))

    def test_blocks_become_address_ranges(self):
        other = copy.deepcopy(BASE)
        other["memory"]["regions"][0]["blocks"] = ["a0", "XX", "YY", "a3"]
        other["memory"]["regions"][1]["blocks"] = ["b0", "ZZ"]
        result = sd.diff(BASE, other)
        regions = {entry["region"]: entry for entry in result["regions"]}
        self.assertEqual(regions["image"]["ranges"], [[0x80010000, 0x80030000]])
        self.assertEqual((regions["heap"]["blocks"], regions["heap"]["ranges"]), (1, [[0x80620000, 0x80630000]]))
        text = sd.render(result, "a", "b", 40)
        self.assertIn("image: 2 blocks in 1 ranges", text)
        self.assertIn("0x80010000..0x80030000", text)
        self.assertFalse(result["guest_equal"])

    def test_region_set_and_layout(self):
        other = copy.deepcopy(BASE)
        other["memory"]["regions"][1]["committed"] = 196608
        other["memory"]["regions"][1]["blocks"].append("b2")
        other["memory"]["regions"].append({"name": "extra", "base": 0x90000000, "committed": 65536, "blocks": ["c0"]})
        result = sd.diff(BASE, other)
        regions = {entry["region"]: entry for entry in result["regions"]}
        self.assertEqual(regions["extra"], {"region": "extra", "only_in": "b"})
        self.assertEqual(regions["heap"]["layout"], {"a": [0x80610000, 131072], "b": [0x80610000, 196608]})
        self.assertEqual(regions["heap"]["ranges"], [[0x80630000, 0x80640000]])

    def test_merge_ranges(self):
        self.assertEqual(sd.merge_ranges([5, 1, 2, 3, 9, 10]), [(1, 3), (5, 5), (9, 10)])


class MainTests(unittest.TestCase):
    def test_exit_codes_and_directory_argument(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "a").mkdir()
            (root / "a" / "state.json").write_text(json.dumps(BASE), encoding="utf-8")
            (root / "b.json").write_text(json.dumps(changed(render_frames=1)), encoding="utf-8")
            (root / "c.json").write_text(json.dumps(changed(game_ticks=101)), encoding="utf-8")
            (root / "bad.json").write_text("{}", encoding="utf-8")
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(sd.main([str(root / "a"), str(root / "b.json"), "--json", str(root / "out.json")]), 0)
                self.assertEqual(sd.main([str(root / "a"), str(root / "c.json")]), 1)
                self.assertEqual(sd.main([str(root / "a"), str(root / "bad.json")]), 2)
                self.assertEqual(sd.main([str(root / "a"), str(root / "missing.json")]), 2)
            self.assertTrue(json.loads((root / "out.json").read_text(encoding="utf-8"))["guest_equal"])


if __name__ == "__main__":
    unittest.main()
