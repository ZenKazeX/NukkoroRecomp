import copy
from pathlib import Path
import tempfile
import unittest

from stage_benchmark import clean_environment, inspect_run, presentation_intervals, summarize, staged_executable
from graphics_baseline import fingerprint


REPORT = {
    "stop": {"reason": 0, "failed": False},
    "run": {"budget_ms": 22000, "window": False, "reference_execution": False,
            "native_renderer": True, "native_renderer_failed": False,
            "native_presenter_failed": False, "verify_recomp": False},
    "guest": {"game_ticks": 891598050, "initial_game_ticks": 0, "interpreted_instructions": 0},
    "audio": {"active": False}, "capture": {"native_screenshot_requested": False},
    "assets": {"failed": 0, "unmapped": 0, "out_of_range": 0, "map_failures": 0, "reads": 50, "bytes": 4000},
    "host": {"wall_seconds": 3.0, "process_cpu_seconds": 4.0},
    "host_stats": {"memory": {"peak_working_set_mib": 250, "peak_private_mib": 300}},
}
STDOUT = """[lifted-render] frames 1200 presented 0 headless 1200 dropped 0 captures 0 render failures 0
[gameplay-scheduler] traversals 1300 levels 100 visits 200 executes 180 faults 0
[lifted-render] sectors 20 drawn meshes 90 built 8 skipped 0 import failures 0
[lifted-render] models: draws 50 built 3 rebuilt 0 failed 0
[lifted-render] particles: batches 90 particles 2000 unsupported 0 skipped 0
[lifted-render] renderer: frames 0 last draws 0 (skipped 0, all frames 0)
"""


def rows():
    errors, workload, metrics = inspect_run(REPORT, STDOUT, 22000, False)
    return [{"sequence": i, "arm": arm, "repetition": rep, "binary_sha256": "a" * 64,
             "errors": errors[:], "workload": workload.copy(), "metrics": metrics.copy()}
            for i, (arm, rep) in enumerate((("control", 0), ("candidate", 0), ("candidate", 1), ("control", 1)))]


class StageBenchmarkTests(unittest.TestCase):
    def test_staged_arms_share_path_and_preserve_originals(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            paths = []
            for name, data in (("control.exe", b"original"), ("candidate.exe", b"new native code")):
                source = root / name
                source.write_bytes(data)
                with staged_executable(source, root / "execution", fingerprint(source)) as staged:
                    paths.append(staged["path"])
                    self.assertEqual(Path(staged["path"]).read_bytes(), data)
                    self.assertEqual(staged["sha256"], fingerprint(source)["sha256"])
                self.assertFalse(Path(paths[-1]).exists())
                self.assertEqual(source.read_bytes(), data)
            self.assertEqual(paths[0], paths[1])

    def test_staging_rejects_changed_input_and_cleans_owned_copy_on_error(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / "source.exe"
            source.write_bytes(b"before")
            pin = fingerprint(source)
            source.write_bytes(b"changed")
            with self.assertRaisesRegex(ValueError, "Input changed"):
                with staged_executable(source, root / "execution", pin):
                    self.fail("stale input accepted")
            target = root / "execution" / "ShadowTheHedgehog.exe"
            self.assertFalse(target.exists())
            with self.assertRaisesRegex(RuntimeError, "child failure"):
                with staged_executable(source, target.parent, fingerprint(source)):
                    raise RuntimeError("child failure")
            self.assertFalse(target.exists())
            self.assertEqual(source.read_bytes(), b"changed")

    def test_staging_cannot_replace_existing_file_or_source(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / "input.exe"
            source.write_bytes(b"input")
            target = root / "ShadowTheHedgehog.exe"
            target.write_bytes(b"keep")
            with self.assertRaises(FileExistsError):
                with staged_executable(source, root, fingerprint(source)):
                    self.fail("existing target replaced")
            with self.assertRaisesRegex(ValueError, "source executable"):
                with staged_executable(target, root, fingerprint(target)):
                    self.fail("source replaced")
            self.assertEqual(target.read_bytes(), b"keep")
            self.assertEqual(source.read_bytes(), b"input")

    def test_presentation_interval_bounds_and_actual_counts(self):
        line = ("[lifted-render] present intervals: 1269, p50 16.7 p95 18.2 p99 19.1 max 615.8 ms; "
                "longer than 20 ms 10, 34 ms 4, 50 ms 3")
        value, errors = presentation_intervals(line, 1270)
        self.assertFalse(errors)
        self.assertEqual(value["quantile_bounds"]["p99"],
                         {"lower_inclusive_ms": 19.0, "upper_exclusive_ms": 19.1, "overflow": False})
        self.assertEqual(value["at_least_ms"], {"20": 10, "34": 4, "50": 3})
        self.assertEqual(value["max_rounded_ms"], 615.8)
        for bad in (line + "\n" + line, line.replace("p50 16.7", "p50 nan"),
                    line.replace("1269", "1268"), line.replace("p95 18.2", "p95 15.0"),
                    line.replace("20 ms 10", "20 ms 1270"), line.replace("50 ms 3", "50 ms 5"),
                    line.replace("max 615.8", "max 17.0")):
            with self.subTest(output=bad):
                self.assertTrue(presentation_intervals(bad, 1270)[1])

    def test_presentation_overflow_is_censored_and_missing_is_unavailable(self):
        line = ("[lifted-render] present intervals: 99, p50 16.7 p95 100.1 p99 100.1 max 600.0 ms; "
                "longer than 20 ms 10, 34 ms 10, 50 ms 10")
        value, errors = presentation_intervals(line, 100)
        self.assertFalse(errors)
        self.assertEqual(value["quantile_bounds"]["p99"],
                         {"lower_inclusive_ms": 100.0, "upper_exclusive_ms": None, "overflow": True})
        self.assertEqual(presentation_intervals("", 100), ({"available": False}, []))
        zero = ("[lifted-render] present intervals: 0, p50 0.0 p95 0.0 p99 0.0 max 0.0 ms; "
                "longer than 20 ms 0, 34 ms 0, 50 ms 0")
        self.assertFalse(presentation_intervals(zero, 1)[1])
        self.assertIsNone(presentation_intervals(zero, 1)[0]["quantile_bounds"]["p50"])
        self.assertTrue(presentation_intervals(zero.replace("p99 0.0", "p99 0.1"), 1)[1])

    def test_rendered_mode_requires_audio_and_complete_gpu_work(self):
        report = copy.deepcopy(REPORT)
        report["run"]["window"] = True
        report["audio"] = {"active": True, "blocks": 4000, "produced_samples": 64000,
                           "consumed_frames": 64000, **{key: 0 for key in (
            "underrun_events", "underrun_frames", "dropped_frames", "dropped_events",
            "open_failures", "render_errors", "device_invalidations")}}
        output = STDOUT.replace("presented 0 headless 1200", "presented 1200 headless 0").replace(
            "renderer: frames 0", "renderer: frames 1200")
        self.assertFalse(inspect_run(report, output, 22000, False, True)[0])
        intervals = ("[lifted-render] present intervals: 1199, p50 16.7 p95 16.8 p99 16.9 max 19.0 ms; "
                     "longer than 20 ms 0, 34 ms 0, 50 ms 0")
        errors, _, metrics = inspect_run(report, output + intervals, 22000, False, True)
        self.assertFalse(errors)
        self.assertEqual(metrics["presentation_intervals"]["count"], 1199)
        self.assertTrue(inspect_run(report, output + intervals.replace("1199", "1200"), 22000, False, True)[0])
        self.assertTrue(inspect_run(report, output, 22000, False)[0])
        capture = copy.deepcopy(report)
        capture["capture"]["native_screenshot_requested"] = True
        captured_output = output.replace("captures 0", "captures 1").replace(
            "renderer: frames 1200", "renderer: frames 1201")
        self.assertFalse(inspect_run(capture, captured_output, 22000, True, True)[0])
        # The extra GPU capture cannot excuse an unpresented queue packet.
        incomplete = captured_output.replace("presented 1200", "presented 1199").replace(
            "renderer: frames 1201", "renderer: frames 1200")
        self.assertIn("incomplete rendered packet accounting",
                      inspect_run(capture, incomplete, 22000, True, True)[0])
        for before, after in (("presented 1200", "presented 1199"),
                              ("renderer: frames 1200", "renderer: frames 0"),
                              ("renderer: frames 1200", "renderer: frames 1199")):
            self.assertTrue(inspect_run(report, output.replace(before, after), 22000, False, True)[0])
        for key in ("underrun_events", "dropped_frames", "open_failures", "render_errors"):
            bad = copy.deepcopy(report)
            bad["audio"][key] = 1
            self.assertTrue(inspect_run(bad, output, 22000, False, True)[0])

    def test_capture_sequence_requires_all_images_and_gpu_work(self):
        report = copy.deepcopy(REPORT)
        report["run"]["window"] = True
        report["capture"]["native_screenshot_requested"] = True
        report["audio"] = {"active": True, "blocks": 4000, "produced_samples": 64000,
                           "consumed_frames": 64000, **{key: 0 for key in (
            "underrun_events", "underrun_frames", "dropped_frames", "dropped_events",
            "open_failures", "render_errors", "device_invalidations")}}
        output = STDOUT.replace("presented 0 headless 1200", "presented 1200 headless 0").replace(
            "captures 0", "captures 6").replace("renderer: frames 0", "renderer: frames 1206")
        self.assertFalse(inspect_run(report, output, 22000, True, True, expected_captures=6)[0])
        self.assertIn("wrong screenshot count", inspect_run(report, output, 22000, True, True)[0])
        for before, after in (("captures 6", "captures 5"), ("frames 1206", "frames 1205"),
                              ("presented 1200", "presented 1199")):
            self.assertTrue(inspect_run(report, output.replace(before, after), 22000,
                                        True, True, expected_captures=6)[0])
        for verification, count in ((False, 6), (True, 0), (True, -1), (True, 1.0), (True, True)):
            with self.assertRaises(ValueError):
                inspect_run(report, output, 22000, verification, True, expected_captures=count)

    def test_shutdown_discards_require_actual_queue_ownership_counters(self):
        report = copy.deepcopy(REPORT)
        report["run"]["window"] = True
        report["audio"] = {"active": True, "blocks": 4000, "produced_samples": 64000,
                           "consumed_frames": 64000, **{key: 0 for key in (
            "underrun_events", "underrun_frames", "dropped_frames", "dropped_events",
            "open_failures", "render_errors", "device_invalidations")}}
        output = STDOUT.replace("presented 0 headless 1200", "presented 1199 headless 0").replace(
            "renderer: frames 0", "renderer: frames 1199")
        self.assertIn("incomplete rendered packet accounting", inspect_run(report, output, 22000, False, True)[0])
        ownership = "[lifted-render] queue: pushed 1200 popped 1199 dropped 0 shutdown discards 1 pending 0\n"
        errors, workload, _ = inspect_run(report, output + ownership, 22000, False, True)
        self.assertFalse(errors)
        self.assertEqual(workload["presented"], 1199)
        self.assertEqual(workload["shutdown_discarded_packets"], 1)
        for before, after in (("pending 0", "pending 1"), ("popped 1199", "popped 1200"),
                              ("pushed 1200", "pushed 1201"), ("shutdown discards 1", "shutdown discards 2")):
            self.assertTrue(inspect_run(report, output + ownership.replace(before, after), 22000, False, True)[0])
        data = rows()
        for row in data: row["workload"]["shutdown_discarded_packets"] = 0
        data[1]["workload"]["shutdown_discarded_packets"] = 1
        self.assertFalse(summarize(data, 2, False, True)["accepted"])

    def test_rendered_summary_keeps_workload_mismatch_and_pacing_scope(self):
        data = rows()
        result = summarize(data, 2, False, True)
        self.assertIn("pacing-limited", result["scope"])
        self.assertFalse(result["performance_gain_proven"])
        for row in data:
            row["workload"]["presented"] = 1200
        data[1]["workload"]["presented"] = 1199
        self.assertFalse(summarize(data, 2, False, True)["accepted"])

    def test_real_counters_required(self):
        self.assertFalse(inspect_run(REPORT, STDOUT, 22000, False)[0])
        self.assertTrue(inspect_run({}, "", 22000, False)[0])
        bad = copy.deepcopy(REPORT)
        bad["guest"]["interpreted_instructions"] = 1
        self.assertIn("interpreter execution or absent count", inspect_run(bad, STDOUT, 22000, False)[0])

    def test_drops_and_failures_rejected(self):
        for before, after in (("headless 1200", "headless 1199"), ("presented 0", "presented 1"),
                              ("render failures 0", "render failures 1"), ("faults 0", "faults 1"),
                              ("all frames 0", "all frames 1"), ("import failures 0", "import failures 1")):
            self.assertTrue(inspect_run(REPORT, STDOUT.replace(before, after), 22000, False)[0])

    def test_capture_and_timing_separated(self):
        capture = copy.deepcopy(REPORT)
        capture["capture"]["native_screenshot_requested"] = True
        output = STDOUT.replace("captures 0", "captures 1").replace("renderer: frames 0", "renderer: frames 39")
        self.assertTrue(inspect_run(capture, output, 22000, False)[0])
        self.assertFalse(inspect_run(capture, output, 22000, True)[0])

    def test_cold_start_duration_and_audio_pinned(self):
        for group, key, value in (("guest", "initial_game_ticks", 1), ("guest", "game_ticks", 1),
                                  ("run", "budget_ms", 21000), ("audio", "active", True)):
            bad = copy.deepcopy(REPORT)
            bad[group][key] = value
            self.assertTrue(inspect_run(bad, STDOUT, 22000, False)[0])

    def test_calibration_never_claims_gain(self):
        data = rows()
        data[1]["metrics"]["wall_seconds"] = 2.0
        result = summarize(data, 2, False)
        self.assertTrue(result["accepted"])
        self.assertTrue(result["identical_binary_calibration"])
        self.assertFalse(result["performance_gain_proven"])
        self.assertEqual(result["metrics"]["wall_seconds"]["paired_deltas"], [-1, 0])

    def test_no_missing_duplicate_or_changed_work(self):
        for data in (rows()[:-1], rows() + rows()[:1]):
            self.assertFalse(summarize(data, 2, False)["accepted"])
        data = rows()
        data[1]["workload"]["frames"] -= 1
        self.assertFalse(summarize(data, 2, False)["accepted"])
        data = rows()
        data[1]["binary_sha256"] = "b" * 64
        self.assertFalse(summarize(data, 2, False)["accepted"])

    def test_all_state_and_image_evidence_compared(self):
        data = rows()
        for row in data:
            row["image"] = "d" * 64
            row["state"] = {"memory": ["0011", "0022"], "game_ticks": 891598050}
        self.assertTrue(summarize(data, 2, True)["accepted"])
        data[-1]["state"]["memory"][1] = "0033"
        self.assertFalse(summarize(data, 2, True)["accepted"])

    def test_environment_cannot_inherit_observers(self):
        result = clean_environment({"PATH": "tools", "GEKKO_GPU_TRACE": "a", "SHADOW_R2_DEBUG_LAYER": "1",
                                    "shadow_test": "1", "geKKo_test": "1"})
        self.assertEqual(result, {"PATH": "tools"})


if __name__ == "__main__":
    unittest.main()
