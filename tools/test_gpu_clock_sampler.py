import argparse
import json
from pathlib import Path
import tempfile
import unittest

import gpu_clock_sampler as sampler


class JoinTests(unittest.TestCase):
    def write(self, folder, samples, frames):
        clocks = Path(folder) / "clocks.csv"
        with clocks.open("w", newline="") as f:
            f.write(",".join(sampler.FIELDS) + "\n")
            for ns, mhz, pstate in samples:
                f.write(f"{ns},{mhz},{mhz},405,{pstate},30000,20,5,1,45\n")
        trace = Path(folder) / "gpu.csv"
        with trace.open("w") as f:
            f.write("# shadow gpu trace 1\nframe,gpu_begin_ns,gpu_end_ns,gpu_ms\n")
            for frame, begin, end in frames:
                f.write(f"{frame},{begin},{end},{(end - begin) / 1e6}\n")
        return clocks, trace

    def join(self, folder, samples, frames, budget=16.0):
        clocks, trace = self.write(folder, samples, frames)
        out = Path(folder) / "join.json"
        sampler.join(argparse.Namespace(samples=str(clocks), gpu_trace=str(trace), budget_ms=budget, out=str(out)))
        return json.loads(out.read_text())

    def test_a_frame_takes_the_lowest_clock_inside_its_gpu_interval(self):
        with tempfile.TemporaryDirectory() as folder:
            result = self.join(folder, [(0, 2700, 0), (10_000_000, 300, 8), (20_000_000, 2700, 0)],
                               [(1, 5_000_000, 25_000_000), (2, 30_000_000, 31_000_000)])
        self.assertEqual(result["frames_joined"], 2)
        slow = result["over_budget"]["frames"]
        self.assertEqual([(f["frame"], f["graphics_mhz"], f["pstate"]) for f in slow], [(1, 300, 8)])
        self.assertEqual(result["within_budget"]["count"], 1)

    def test_a_frame_between_samples_takes_the_previous_sample(self):
        with tempfile.TemporaryDirectory() as folder:
            result = self.join(folder, [(0, 600, 5), (100_000_000, 2700, 0)], [(7, 40_000_000, 41_000_000)])
        self.assertEqual(result["within_budget"]["graphics_mhz"]["p50"], 600)

    def test_frames_outside_the_samples_are_refused(self):
        with tempfile.TemporaryDirectory() as folder:
            with self.assertRaises(SystemExit):
                self.join(folder, [(100, 600, 5)], [(1, 10, 50)])

    def test_summary_counts_pstates_and_throttle_reasons(self):
        rows = [dict(zip(sampler.FIELDS, [0, 210, 210, 405, 8, 20000, 3, 1, 0x1, 40])),
                dict(zip(sampler.FIELDS, [1, 2700, 2700, 11201, 0, 90000, 60, 30, 0x0, 50]))]
        summary = sampler.summarize(rows)
        self.assertEqual(summary["pstate_samples"], {"P8": 1, "P0": 1})
        self.assertEqual(summary["throttle_reason_samples"], {"gpu_idle": 1})
        self.assertEqual(summary["graphics_mhz"]["max"], 2700)


if __name__ == "__main__":
    unittest.main()
