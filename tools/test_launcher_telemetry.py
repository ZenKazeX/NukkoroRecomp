import unittest

from launcher import Launcher


class LauncherTelemetryTests(unittest.TestCase):
    def setUp(self):
        # Only exercise the formatter; no Tk window or interpreter is created.
        self.launcher = Launcher.__new__(Launcher)

    def test_missing_values_are_not_zero_cost_or_legacy_rates(self):
        lines = self.launcher._read_telemetry({"renderer": "lifted", "wall_s": "1", "fps": "59.8"}, None)
        self.assertIn("59.8 render FPS", lines[0][1])
        self.assertIn("render/present n/a", lines[0][1])
        self.assertIn("GPU n/a", lines[0][1])
        self.assertNotIn("MIPS", lines[0][1])
        self.assertNotIn("tris", lines[0][1])

    def test_measured_zero_is_valid_and_counters_report_deltas(self):
        previous = {"renderer": "lifted", "dropped": "2", "skipped_draws": "0"}
        row = {**previous, "wall_s": "2", "fps": "0", "dropped": "5", "render_present_ms": "0"}
        lines = self.launcher._read_telemetry(row, previous)
        self.assertIn("render/present 0.0", lines[0][1])
        self.assertIn(("warn", "      3 unrendered frame packets dropped"), lines)

    def test_nonfinite_measurement_is_unavailable(self):
        for value in ("NaN", "Infinity", ""):
            lines = self.launcher._read_telemetry({"renderer": "lifted", "fps": value}, None)
            self.assertIn("n/a render FPS", lines[0][1])


if __name__ == "__main__":
    unittest.main()
