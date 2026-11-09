import unittest

from present_trace_report import (DROP_ON_PUSH, POP, PRESENT_BEGIN, PRESENT_END, PUSH, RENDER_END, STATS,
                                  WAIT_BEGIN, WAIT_END, analyze)

PERIOD = 16_000_000  # a synthetic 62.5 Hz display, so the ppm and beat arithmetic is not trivially zero
ORIGIN = 1_000_000_000


def vblank(refresh):
    return ORIGIN + refresh * PERIOD


def frame(rows, tick, present_id, push, present_begin, shown_previous, shown_refresh, game_ticks=None):
    """One packet: pushed, popped 1 ms later, presented; the statistics read after it report the previous present."""
    game_ticks = tick * 100 if game_ticks is None else game_ticks
    rows.append((push, PUSH, tick, game_ticks, 1, 0))
    pop = max(push + 1_000_000, present_begin - 3_000_000)
    rows.append((pop, POP, tick, game_ticks, 0, 0))
    rows.append((present_begin - 500_000, WAIT_BEGIN, 0, 0, 0, 0))
    rows.append((present_begin - 100_000, WAIT_END, 1, 0, 0, 0))
    rows.append((present_begin, PRESENT_BEGIN, 1, 0, 0, 0))
    rows.append((present_begin + 50_000, PRESENT_END, 0, present_id, 0, 0))
    previous_id, previous_refresh = shown_previous
    rows.append((present_begin + 60_000, STATS, previous_id, previous_refresh, previous_refresh,
                 vblank(previous_refresh)))
    rows.append((present_begin + 70_000, RENDER_END, tick, 1, 0, 0))
    return present_id, shown_refresh


def fixture():
    rows = []
    shown = (0, 0)
    # Presents 1..3 each made their vblank (refresh 1, 2, 3).
    shown = frame(rows, 1, 1, vblank(0) + 2_000_000, vblank(0) + 6_000_000, shown, 1)
    shown = frame(rows, 2, 2, vblank(1) + 2_000_000, vblank(1) + 6_000_000, shown, 2)
    shown = frame(rows, 3, 3, vblank(2) + 2_000_000, vblank(2) + 6_000_000, shown, 3)
    # A present shown at refresh R had to be queued before vblank(R).
    # Present 4 needed refresh 4 but its packet came 1 ms after vblank(4): shown at 5 (packet_late).
    shown = frame(rows, 4, 4, vblank(4) + 1_000_000, vblank(4) + 5_000_000, shown, 5)
    # Present 5 needed 6; the packet was early but Present was called after vblank(6): shown at 7 (render_late).
    shown = frame(rows, 5, 5, vblank(5) + 10_000_000, vblank(6) + 200_000, shown, 7)
    # Present 6 needed 8; Present was called before vblank(8) and it still missed it: shown at 9 (gpu_late).
    shown = frame(rows, 6, 6, vblank(7) + 2_000_000, vblank(8) - 1_000_000, shown, 9)
    rows.append((vblank(8) + 3_000_000, DROP_ON_PUSH, 7, 0, 0, 0))
    # A last present only to carry the statistics of present 6.
    frame(rows, 8, 7, vblank(9) + 2_000_000, vblank(9) + 6_000_000, shown, 10)
    return rows


class PresentTraceReportTests(unittest.TestCase):
    def test_refresh_period_and_beat(self):
        report = analyze(fixture())
        self.assertAlmostEqual(report["display"]["refresh_period_ms"], 16.0, places=6)
        self.assertAlmostEqual(report["display"]["pacing_minus_refresh_ppm"], (1e9 / 60 - PERIOD) / PERIOD * 1e6,
                               places=3)
        self.assertAlmostEqual(report["display"]["phase_beat_seconds"],
                               PERIOD * (1e9 / 60) / abs(PERIOD - 1e9 / 60) / 1e9, places=6)

    def test_each_repeat_is_attributed_to_its_late_side(self):
        report = analyze(fixture())
        self.assertEqual(report["presents"]["repeat_causes"], {"packet_late": 1, "render_late": 1, "gpu_late": 1})
        self.assertEqual(report["presents"]["consecutive_pairs_fresh"], 2)
        by_present = {r["present"]: r for r in report["repeats"]}
        self.assertEqual(by_present[4]["cause"], "packet_late")
        self.assertAlmostEqual(by_present[4]["push_minus_deadline_ms"], 1.0)
        self.assertAlmostEqual(by_present[4]["push_phase_ms"], 1.0)
        self.assertEqual(by_present[5]["cause"], "render_late")
        self.assertEqual(by_present[6]["cause"], "gpu_late")

    def test_drops_and_push_intervals(self):
        report = analyze(fixture())
        self.assertEqual(report["packets"]["dropped_on_push"], 1)
        self.assertEqual(report["packets"]["pushed"], 7)
        self.assertEqual(sum(report["packets"]["push_phase_ms"]["counts"]), 7)

    def test_a_two_refresh_game_step_is_not_a_repeat(self):
        rows, shown = [], (0, 0)
        step = 40_500_000 * PERIOD * 2 // 1_000_000_000  # two refreshes of game time
        for n in range(4):
            shown = frame(rows, n + 1, n + 1, vblank(2 * n) + 2_000_000, vblank(2 * n) + 6_000_000, shown,
                          2 * n + 1, game_ticks=n * step)
        frame(rows, 5, 5, vblank(8) + 2_000_000, vblank(8) + 6_000_000, shown, 10, game_ticks=4 * step)
        report = analyze(rows)
        self.assertEqual(report["presents"]["consecutive_pairs_fresh"], 3)
        self.assertEqual(report["presents"]["consecutive_pairs_repeat"], 0)

    def test_single_vblank_sample_is_refused(self):
        rows = [r for r in fixture() if r[1] != STATS][:]
        rows.append((ORIGIN, STATS, 1, 1, 1, vblank(1)))
        with self.assertRaises(ValueError):
            analyze(rows)


if __name__ == "__main__":
    unittest.main()
