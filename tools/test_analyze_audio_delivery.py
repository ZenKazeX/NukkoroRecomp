import csv
import tempfile
import unittest
from pathlib import Path

from analyze_audio_delivery import analyze


def event(ms, stage, lane=1, **values):
    result = dict.fromkeys("abcdefgh", 0)
    result.update(lane=lane, ns=int(ms * 1000000), stage=stage, **values)
    return result


def wake(ms, padding=240, rate=48000, buffer=480):
    return event(ms, "endpoint-wake", a=padding, b=buffer-padding,
                 c=2000, d=1000, g=buffer, h=rate)


def submit(ms, frames=240, missing=0, hr=0):
    return event(ms, "endpoint-submit", a=frames, b=160, c=missing, d=hr)


class AudioDeliveryTests(unittest.TestCase):
    def run_trace(self, records, **kwargs):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "trace.csv"
            with path.open("w", newline="") as stream:
                writer = csv.DictWriter(stream, fieldnames=["lane", "ns", "stage", *"abcdefgh"])
                writer.writeheader()
                writer.writerows(records)
            return analyze(path, **kwargs)

    def progress(self):
        return [event(0, "quantum", lane=0), event(100, "voice-mix", lane=0, a=100)]

    def test_healthy_host_delivery_is_observation_not_parity(self):
        result = self.run_trace(self.progress() + [wake(10), submit(10.1), wake(15), submit(15.1)])
        self.assertEqual(result["counts"]["accepted_device_frames"], 480)
        self.assertEqual(result["counts"]["source_shortage_submissions"], 0)
        self.assertEqual(result["wake_intervals"]["mean_ms"], 5)
        self.assertIn("not_hardware_playback", result["scope"])

    def test_source_starvation_and_failed_submission_are_independent(self):
        result = self.run_trace(self.progress() + [wake(10), submit(10.1, missing=80),
                                                 wake(15), submit(15.1, hr=0x88890004)])
        self.assertEqual(result["counts"]["missing_device_frames"], 80)
        self.assertEqual(result["counts"]["submission_errors"], 1)
        self.assertEqual(result["counts"]["failed_submission_frames"], 240)
        self.assertEqual(result["counts"]["accepted_device_frames"], 240)
        self.assertEqual({row["category"] for row in result["examples"]},
                         {"source_shortage", "host_submission_error"})

    def test_empty_padding_is_not_automatically_missing_audio(self):
        result = self.run_trace(self.progress() + [wake(10, padding=0), submit(10.1, frames=480)])
        self.assertEqual(result["counts"]["empty_padding_observations"], 1)
        self.assertEqual(result["counts"]["missing_device_frames"], 0)

    def test_initial_start_delay_separate_from_later_wake_risk(self):
        result = self.run_trace(self.progress() + [wake(1), submit(1.1), wake(30), submit(30.1),
                                                 wake(60), submit(60.1)])
        self.assertEqual(result["counts"]["initial_wake_gaps_exceed_buffer_duration"], 1)
        self.assertEqual(result["counts"]["wake_gaps_exceed_buffer_duration"], 1)

    def test_teardown_shortage_excluded_using_last_guest_progress(self):
        records = self.progress() + [wake(10), submit(10.1), wake(101), submit(101.1, missing=240)]
        result = self.run_trace(records)
        self.assertEqual(result["counts"]["missing_device_frames"], 0)
        self.assertEqual(result["counts"]["post_guest_missing_frames"], 240)
        self.assertEqual(result["counts"]["post_guest_device_records"], 2)

    def test_dma_metadata_not_guest_progress_or_duration(self):
        result = self.run_trace([event(20, "dma-start", lane=0, a=0x80010000),
                                 event(50, "dma-end", lane=0, a=0x80020000), wake(30), submit(30.1)])
        self.assertFalse(result["coverage"]["guest_progress_present"])
        self.assertEqual(result["counts"]["unattributed_device_records"], 2)
        self.assertEqual(result["dma_records"], {"dma-start": 1, "dma-end": 1})

    def test_missing_endpoint_events_and_invalid_observations_explicit(self):
        result = self.run_trace(self.progress())
        self.assertFalse(result["coverage"]["endpoint_events_present"])
        self.assertEqual(result["counts"]["submission_attempts"], 0)
        invalid = wake(10)
        invalid["a"] = 481
        result = self.run_trace(self.progress() + [invalid])
        self.assertEqual(result["counts"]["invalid_endpoint_observations"], 1)

    def test_mix_scope_payload_is_not_an_instruction_clock(self):
        result = self.run_trace([event(0, "guest", lane=0, a=100),
                                 event(2, "mix", lane=0, a=1000000000, b=200000),
                                 event(3, "voice-mix", lane=0, a=200)])
        self.assertEqual(result["guest_activity"]["instruction_regressions"], 0)

    def test_example_storage_bounded_without_losing_counts(self):
        records = self.progress()
        for index in range(10):
            records += [wake(index+1), submit(index+1.01, missing=100)]
        result = self.run_trace(records, max_examples=2)
        self.assertEqual(len(result["examples"]), 2)
        self.assertEqual(result["examples_omitted"], 8)
        self.assertEqual(result["counts"]["source_shortage_submissions"], 10)
        self.assertEqual(result["counts"]["missing_device_frames"], 1000)

    def test_unordered_device_lane_rejected_and_configuration_changes_delimit_gaps(self):
        with self.assertRaises(ValueError):
            self.run_trace(self.progress() + [wake(20), wake(10)])
        result = self.run_trace(self.progress() + [wake(10), wake(40, rate=96000, buffer=960)])
        self.assertEqual(result["counts"]["endpoint_configuration_changes"], 1)
        self.assertEqual(result["counts"]["wake_gaps_exceed_buffer_duration"], 0)


if __name__ == "__main__":
    unittest.main()
