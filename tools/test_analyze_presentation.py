import copy
import unittest

from analyze_presentation import analyze


def fixture():
    rows = [{"schema": "1", "wall_seconds": str(t), "present_index": str(i + 1),
             "packet_tick": str(10 + i), "game_ticks": str(1000 + 100 * i), "success": "1",
             "render_return_interval_ms": "" if i == 0 else "25", "packet_wait_ms": "10",
             "capture_ms": "0", "render_present_ms": "2"} for i, t in enumerate((0.01, 0.035, 0.06))]
    presentation = {"available": True, "timing_enabled": True, "gpu_frame_ms": None,
                    "render_rate_applied": False, "rendered_frames": 3, "render_failures": 0,
                    "intervals": 2, "average_fps": 40.0,
                    "interval_ms": {"sum": 50.0, "max": 25.0,
                                    **{f"p{p}": {"lower_inclusive": 25.0, "upper_exclusive": 25.1}
                                       for p in (50, 95, 99)}},
                    "cpu_wall_ms": {"packet_wait": 31.0, "capture": 0.0, "render_present": 6.0},
                    "queue": {"pushed": 5, "popped": 3, "dropped": 1, "shutdown_discards": 1, "pending": 0}}
    interval = {"schema": "1", "renderer": "lifted", "interval_s": "1", "frames": "3", "attempts": "3",
                "fps": "3", "game_ms": "50", "game_speed": "0.05", "packets": "5", "dropped": "1",
                "skipped_draws": "0", "resource_failures": "0", "render_present_ms": "2",
                "produced_per_game_s": "100", "packet_wait_ms": "31", "capture_ms": "0", "gpu_frame_ms": ""}
    return rows, {"renderer": {"backend": "lifted"}, "presentation": presentation}, [interval]


class PresentationLogTests(unittest.TestCase):
    def test_complete_accounting_and_empty_tail_wait(self):
        result = analyze(*fixture())
        self.assertTrue(result["complete_frame_accounting"])
        self.assertEqual(result["durations"]["render_return_interval_ms"]["sum_ms"], 50)
        self.assertEqual(result["queue"]["shutdown_discards"], 1)

    def test_missing_last_or_middle_frame_refused(self):
        for index in (1, 2):
            rows, report, intervals = fixture()
            del rows[index]
            with self.assertRaises(ValueError):
                analyze(rows, report, intervals)

    def test_reordered_duplicate_and_forged_interval_refused(self):
        for key, value in (("present_index", "1"), ("wall_seconds", "0"),
                           ("render_return_interval_ms", "16.666"), ("success", "2")):
            rows, report, intervals = fixture()
            rows[1][key] = value
            with self.assertRaises(ValueError):
                analyze(rows, report, intervals)

    def test_first_interval_is_missing_not_zero(self):
        rows, report, intervals = fixture()
        rows[0]["render_return_interval_ms"] = "0"
        with self.assertRaises(ValueError):
            analyze(rows, report, intervals)

    def test_unmeasured_gpu_duration_cannot_be_zero(self):
        for target in ("json", "csv"):
            rows, report, intervals = fixture()
            if target == "json":
                report["presentation"]["gpu_frame_ms"] = 0
            else:
                intervals[0]["gpu_frame_ms"] = "0"
            with self.assertRaises(ValueError):
                analyze(rows, report, intervals)

    def test_nan_infinity_and_negative_duration_refused(self):
        for value in ("NaN", "Infinity", "-1"):
            rows, report, intervals = fixture()
            rows[0]["render_present_ms"] = value
            with self.assertRaises(ValueError):
                analyze(rows, report, intervals)

    def test_all_slow_frames_and_overflow_bounds_retained(self):
        rows, report, _ = fixture()
        rows[2]["wall_seconds"] = "0.235"
        rows[2]["render_return_interval_ms"] = "200"
        p = report["presentation"]
        p["interval_ms"].update(sum=225, max=200)
        p["average_fps"] = 2000 / 225
        for q in (95, 99):
            p["interval_ms"][f"p{q}"] = {"lower_inclusive": 100, "upper_exclusive": None}
        result = analyze(rows, report)
        self.assertEqual(result["durations"]["render_return_interval_ms"]["max_ms"], 200)
        self.assertEqual(result["durations"]["render_return_interval_ms"]["count"], 2)

    def test_failed_attempt_is_accounted_and_not_ready(self):
        rows, report, _ = fixture()
        rows[-1]["success"] = "0"
        p = report["presentation"]
        p.update(rendered_frames=2, render_failures=1, intervals=1)
        p["interval_ms"]["sum"] = 25
        result = analyze(rows, report)
        self.assertEqual(result["failed_attempts"], 1)
        self.assertNotEqual(result["readiness"], "passed")

    def test_restore_rewind_is_reported_not_hidden(self):
        rows, report, intervals = fixture()
        rows[-1]["game_ticks"] = "1"
        rows[-1]["packet_tick"] = "0"
        self.assertEqual(analyze(rows, report, intervals)["game_time_rewinds"], 1)

    def test_forged_interval_rates_or_lost_queue_work_refused(self):
        for key, value in (("fps", "800"), ("game_speed", "2"), ("produced_per_game_s", "800")):
            rows, report, intervals = fixture()
            intervals[0][key] = value
            with self.assertRaises(ValueError):
                analyze(rows, report, intervals)
        rows, report, intervals = fixture()
        report["presentation"]["queue"]["dropped"] = 0
        with self.assertRaises(ValueError):
            analyze(rows, report, intervals)

    def test_no_attempt_is_not_a_zero_time_render(self):
        rows, report, intervals = fixture()
        extra = copy.deepcopy(intervals[0])
        extra.update(frames="0", attempts="0", fps="0", game_ms="0", game_speed="0",
                     produced_per_game_s="", render_present_ms="")
        self.assertEqual(analyze(rows, report, intervals + [extra])["interval_rows"], 2)
        extra["render_present_ms"] = "0"
        with self.assertRaises(ValueError):
            analyze(rows, report, intervals + [extra])


if __name__ == "__main__":
    unittest.main()
