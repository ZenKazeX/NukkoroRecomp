import copy
import unittest

from frame_budget_report import distribution, render_size_evidence, summarize


class FrameBudgetTests(unittest.TestCase):
    def rows(self):
        return [dict(frame=str(i), tick=str(i + 10), game_ticks=str(i * 100), kind="present",
                     cpu_begin_ns=str(1_000_000_000 + i * 16_000_000),
                     cpu_submit_ns=str(1_000_500_000 + i * 16_000_000),
                     cpu_present_ns=str(1_000_600_000 + i * 16_000_000),
                     gpu_begin_ns=str(1_000_550_000 + i * 16_000_000),
                     gpu_end_ns=str(1_000_950_000 + i * 16_000_000),
                     gpu_ms="0.4000", passes="1", passes_detail="main:0.3000",
                     mark_overflow="0", upload_overflows="0") for i in range(2)]

    def test_component_budget_is_not_frame_rate(self):
        result = summarize(self.rows(), 2, 800)
        self.assertEqual(result["target_frame_ms"], 1.25)
        self.assertEqual(result["intervals"]["gpu_direct"]["p99_ms"], .4)
        self.assertEqual(result["intervals"]["present_return_interval"]["p99_ms"], 16)
        self.assertEqual(result["target_achievement"], "not_evaluated_from_instrumented_trace")

    def test_capture_window_and_game_sizes_do_not_prove_render_size(self):
        stdout = "Display: window 2560x1440\n[output] the game's output is 2560x1440\n"
        stdout += "[lifted-render] render size: 2560x1440 asked\nframe.png (2560 x 1440)\n"
        self.assertEqual(render_size_evidence(stdout)["mode"], "window_size_unverified")
        with self.assertRaises(ValueError):
            render_size_evidence(stdout, "2560x1440")

    def test_chosen_render_size_required_and_recorded(self):
        line = "[lifted-render] render size 2560x1440 (asked 2560x1440, aspect-locked)\n"
        result = render_size_evidence(line * 2, "2560x1440")
        self.assertEqual(result["announced_dimensions"], [[2560, 1440]])
        for extra in ("[lifted-render] render size 1920x1080 (asked 2560x1440, resized)\n",
                      "[lifted-render] render size 0x1440 (asked 2560x1440, invalid)\n"):
            with self.subTest(extra=extra), self.assertRaises(ValueError):
                render_size_evidence(line + extra, "2560x1440")
        for required in ("0x1440", "2560", "2560X1440", "1920x1080"):
            with self.subTest(required=required), self.assertRaises(ValueError):
                render_size_evidence(line, required)

    def test_invalid_target(self):
        for target in (0, -1, float("nan"), float("inf")):
            with self.subTest(target=target), self.assertRaises(ValueError):
                summarize(self.rows(), 2, target)

    def test_every_outlier_is_kept(self):
        result = distribution([.2] * 99 + [200], 1.25)
        self.assertEqual(result["count"], 100)
        self.assertEqual(result["max_ms"], 200)
        self.assertEqual(result["above_target_budget"], 1)
        self.assertAlmostEqual(result["mean_ms"], 2.198)

    def test_reject_incomplete_or_corrupt_trace(self):
        for key, value in (("kind", "image"), ("mark_overflow", "1"), ("upload_overflows", "1"),
                           ("gpu_ms", "nan"), ("gpu_ms", "1"), ("gpu_begin_ns", "0"),
                           ("cpu_submit_ns", "0"), ("frame", "0"), ("tick", "10"),
                           ("passes", "2"), ("passes_detail", "main:-1")):
            with self.subTest(key=key, value=value):
                rows = copy.deepcopy(self.rows())
                rows[1][key] = value
                with self.assertRaises(ValueError):
                    summarize(rows, 2, 800)
        with self.assertRaises(ValueError):
            summarize(self.rows(), 3, 800)


if __name__ == "__main__":
    unittest.main()
