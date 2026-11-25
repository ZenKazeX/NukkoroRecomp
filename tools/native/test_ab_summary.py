#!/usr/bin/env python3
"""Tests for ab_summary.py (stdlib unittest)."""

from __future__ import annotations

import contextlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ab_summary as ab  # noqa: E402


def session(cpu, cycles, mix_ns, native_ns, frames=1000, hits=5, callback_ns=4000000):
    return {"host": {"game_thread_cpu_s": cpu, "game_thread_cycles": cycles},
            "guest": {"owner_hits": hits, "native_entries": 9, "game_ticks": 100},
            "host_stats": {"audio_engine": {"frames": frames, "frame_steps": 3000, "frame_native_ns": native_ns, "mix_ns": mix_ns,
                                            "effect_ns": 3000000, "frame_callback_ns": callback_ns, "frame_callbacks": frames,
                                            "mixed_voices": 7000}}}


class AbSummaryTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def write(self, number, arm, data, state="same"):
        directory = self.root / f"r{number}-{arm}"
        directory.mkdir()
        (directory / "session.json").write_text(json.dumps(data), encoding="utf-8")
        (directory / "state.json").write_text(state, encoding="utf-8")

    def rows(self):
        rounds = ab.load(self.root)
        return {row["metric"]: row for row in (ab.metric_row(rounds, *spec) for spec in ab.DEFAULT_METRICS + ab.AUDIO_METRICS) if row}

    def test_disjoint_and_unanimous_is_outside_the_spread(self):
        for n, (a_mix, b_mix) in enumerate([(6300000, 5800000), (6200000, 5790000), (6250000, 5810000)], 1):
            self.write(n, "A", session([20.4, 20.3, 20.5][n - 1], 61e9, a_mix, a_mix + 900000))
            self.write(n, "B", session([20.45, 20.2, 20.6][n - 1], 61e9, b_mix, b_mix + 900000))
        rows = self.rows()
        mixer = rows["  mixer (us/frame)"]
        self.assertTrue(mixer["outside_spread"])
        self.assertEqual((mixer["b_lower_rounds"], mixer["rounds"]), (3, 3))
        self.assertAlmostEqual(mixer["median_a"], 6.25)
        self.assertAlmostEqual(mixer["delta_percent"], 100 * (5.8 / 6.25 - 1))
        self.assertFalse(rows["game thread CPU (s)"]["outside_spread"])          # overlapping ranges
        self.assertFalse(rows["  native outside the mixer (us/frame)"]["outside_spread"])   # identical values
        self.assertFalse(rows["game thread cycles (G)"]["outside_spread"])

    def test_disjoint_medians_but_overlapping_ranges_is_inside(self):
        values = [(6.0, 5.5), (6.1, 5.6), (5.55, 5.4)]        # A's minimum is below B's maximum
        for n, (a, b) in enumerate(values, 1):
            self.write(n, "A", session(a, 1e9, 1, 2))
            self.write(n, "B", session(b, 1e9, 1, 2))
        row = self.rows()["game thread CPU (s)"]
        self.assertEqual(row["b_lower_rounds"], 3)
        self.assertFalse(row["ranges_disjoint"])
        self.assertFalse(row["outside_spread"])

    def test_counter_differences_and_incomplete_rounds(self):
        self.write(1, "A", session(1, 1, 1, 2))
        self.write(1, "B", session(1, 1, 1, 2, hits=6), state="other")
        self.write(2, "A", session(1, 1, 1, 2))                 # no r2-B: the round is ignored
        rounds = ab.load(self.root)
        self.assertEqual(sorted(rounds), [1])
        problems = ab.counter_differences(rounds)
        self.assertTrue(any("guest.owner_hits differs between the arms: A 5 B 6" in p for p in problems))
        self.assertTrue(any("_state_md5 differs" in p for p in problems))

    def test_missing_audio_block_and_main(self):
        plain = {"host": {"game_thread_cpu_s": 2.0, "game_thread_cycles": 5e9}, "guest": {"owner_hits": 1, "native_entries": 1, "game_ticks": 1}}
        self.write(1, "A", plain)
        self.write(1, "B", plain)
        self.assertEqual(sorted(self.rows()), ["game thread CPU (s)", "game thread cycles (G)"])
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(ab.main([str(self.root), "--metric", "host.game_thread_cycles/host.game_thread_cpu_s",
                                      "--json", str(self.root / "s.json")]), 0)
            self.assertEqual(ab.main([str(self.root / "nothing-here")]), 2)
        self.assertIn("inside the run-to-run spread", out.getvalue())
        self.assertIn("are equal in every run of both arms", out.getvalue())
        self.assertEqual(len(json.loads((self.root / "s.json").read_text(encoding="utf-8"))["rows"]), 3)


if __name__ == "__main__":
    unittest.main()
