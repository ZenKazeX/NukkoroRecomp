"""Bounded, offline attribution of GEKKO_AUDIO_AUDIT host audio delivery.

Two streaming passes retain only aggregate counters and a bounded set of examples.
Host render/submit observations do not prove hardware playback or audio fidelity.
DMA address records have no instruction clock; they are not duration records.
"""
import argparse
import collections
import csv
import json
from pathlib import Path


FIELDS = tuple("abcdefgh")
PROGRESS = {"guest", "quantum", "mix", "mix-done", "voice-mix", "dma-tick"}
MAX_EXAMPLES = 256


def rows(path):
    with Path(path).open(newline="", encoding="utf-8-sig") as stream:
        reader = csv.DictReader(stream)
        required = {"lane", "ns", "stage", *FIELDS}
        if not required.issubset(reader.fieldnames or ()):
            raise ValueError("Not a GEKKO_AUDIO_AUDIT CSV: missing required columns")
        for number, raw in enumerate(reader, 2):
            try:
                result = {key: int(raw[key]) for key in ("lane", "ns", *FIELDS)}
                result["stage"] = raw["stage"]
                if any(value < 0 for key, value in result.items() if key != "stage"):
                    raise ValueError("negative unsigned field")
            except (TypeError, ValueError) as error:
                raise ValueError(f"Invalid audio record on line {number}: {error}") from error
            yield result


class Distribution:
    """Fixed bins; no sample retention or approximate percentile claims."""
    LIMITS_MS = (0.5, 1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048)

    def __init__(self):
        self.count = 0
        self.total = 0.0
        self.low = None
        self.high = None
        self.bins = [0] * (len(self.LIMITS_MS) + 1)

    def add(self, value):
        self.count += 1
        self.total += value
        self.low = value if self.low is None else min(self.low, value)
        self.high = value if self.high is None else max(self.high, value)
        index = next((i for i, limit in enumerate(self.LIMITS_MS) if value <= limit),
                     len(self.LIMITS_MS))
        self.bins[index] += 1

    def result(self):
        return {"count": self.count, "min_ms": self.low, "max_ms": self.high,
                "mean_ms": self.total / self.count if self.count else None,
                "histogram_upper_ms": [*self.LIMITS_MS, None], "histogram": self.bins}


def analyze(path, max_examples=32):
    if not 0 <= max_examples <= MAX_EXAMPLES:
        raise ValueError(f"max_examples must be between 0 and {MAX_EXAMPLES}")
    start = end = None
    stages = collections.Counter()
    dma_counts = collections.Counter()
    last_instruction = None
    instruction_regressions = 0
    for row in rows(path):
        stage = row["stage"]
        # Only known names are retained; arbitrary CSV stage names cannot grow a map.
        if stage in PROGRESS or stage in {"endpoint-wake", "endpoint-submit"}:
            stages[stage] += 1
        if row["lane"] == 0 and stage in {"dma-start", "dma-end", "dma-program"}:
            dma_counts[stage] += 1
        if row["lane"] == 0 and stage in PROGRESS:
            start = row["ns"] if start is None else min(start, row["ns"])
            end = row["ns"] if end is None else max(end, row["ns"])
            # "mix" is also Scope's duration record (a=wall start, b=duration),
            # so its overloaded payload cannot be treated as an instruction clock.
            if stage in {"guest", "mix-done", "voice-mix"}:
                if last_instruction is not None and row["a"] < last_instruction:
                    instruction_regressions += 1
                last_instruction = row["a"]

    counts = collections.Counter()
    examples = []
    omitted = 0
    intervals = Distribution()
    padding_ms = Distribution()
    source_ms = Distribution()
    previous_wake = current_wake = None
    previous_device_ns = None

    def example(row, category, **values):
        nonlocal omitted
        if len(examples) < max_examples:
            examples.append({"ns": row["ns"], "category": category, **values})
        else:
            omitted += 1

    for row in rows(path):
        if row["lane"] != 1 or row["stage"] not in {"endpoint-wake", "endpoint-submit"}:
            continue
        if previous_device_ns is not None and row["ns"] < previous_device_ns:
            raise ValueError("Device lane is not chronological; refusing invalid attribution")
        previous_device_ns = row["ns"]
        if start is None:
            counts["unattributed_device_records"] += 1
            continue
        if row["ns"] < start:
            counts["pre_guest_device_records"] += 1
            continue
        if row["ns"] > end:
            counts["post_guest_device_records"] += 1
            if row["stage"] == "endpoint-submit":
                counts["post_guest_missing_frames"] += row["c"]
                counts["post_guest_submission_errors"] += bool(row["d"] & 0x80000000)
            continue
        if row["stage"] == "endpoint-wake":
            counts["wakes"] += 1
            current_wake = row
            if row["h"] == 0 or row["g"] == 0 or row["a"] > row["g"] or row["b"] != row["g"] - row["a"]:
                counts["invalid_endpoint_observations"] += 1
                example(row, "invalid_endpoint_observation")
                previous_wake = None
                continue
            padding_ms.add(1000 * row["a"] / row["h"])
            if row["c"] >= row["d"]:
                # DMA source frequency is 486 MHz / 121496 * 8, not exactly 32 kHz.
                source_ms.add((row["c"] - row["d"]) * 1000 * 121496 / (486000000 * 8))
            else:
                counts["invalid_source_observations"] += 1
            if row["a"] == 0:
                counts["empty_padding_observations"] += 1
            if previous_wake is not None:
                delta_ns = row["ns"] - previous_wake["ns"]
                intervals.add(delta_ns / 1000000)
                # Device changes delimit comparisons; no borrowed period assumption.
                if (row["g"], row["h"]) != (previous_wake["g"], previous_wake["h"]):
                    counts["endpoint_configuration_changes"] += 1
                elif delta_ns * row["h"] > row["g"] * 1000000000:
                    # First wake can be prefill before a slow IAudioClient::Start.
                    category = ("initial_wake_gap_exceeds_buffer_duration" if counts["wakes"] == 2
                                else "wake_gap_exceeds_buffer_duration")
                    count_key = ("initial_wake_gaps_exceed_buffer_duration" if counts["wakes"] == 2
                                 else "wake_gaps_exceed_buffer_duration")
                    counts[count_key] += 1
                    example(row, category, gap_ms=delta_ns / 1000000,
                            buffer_ms=1000 * row["g"] / row["h"])
            previous_wake = row
        else:
            counts["submission_attempts"] += 1
            counts["rendered_device_frames"] += row["a"]
            counts["consumed_source_frames"] += row["b"]
            counts["missing_device_frames"] += row["c"]
            if row["d"] & 0x80000000:
                counts["submission_errors"] += 1
                counts["failed_submission_frames"] += row["a"]
                example(row, "host_submission_error", hresult=f"0x{row['d'] & 0xffffffff:08X}")
            else:
                counts["accepted_device_frames"] += row["a"]
            if row["c"]:
                counts["source_shortage_submissions"] += 1
                example(row, "source_shortage", missing_device_frames=row["c"],
                        consumed_source_frames=row["b"],
                        padding_frames=current_wake["a"] if current_wake else None)
            if current_wake is None:
                counts["submissions_without_wake"] += 1
            else:
                if row["a"] != current_wake["b"]:
                    counts["wake_submission_size_mismatches"] += 1
                current_wake = None

    keys = ("wakes", "submission_attempts", "submission_errors", "source_shortage_submissions",
            "missing_device_frames", "accepted_device_frames", "rendered_device_frames",
            "consumed_source_frames", "failed_submission_frames", "empty_padding_observations",
            "wake_gaps_exceed_buffer_duration", "initial_wake_gaps_exceed_buffer_duration", "endpoint_configuration_changes",
            "invalid_endpoint_observations", "invalid_source_observations", "submissions_without_wake",
            "wake_submission_size_mismatches", "pre_guest_device_records", "post_guest_device_records",
            "post_guest_missing_frames", "post_guest_submission_errors", "unattributed_device_records")
    return {"schema": 1, "trace": str(path), "scope": "host_delivery_observations_not_hardware_playback_parity",
            "guest_activity": {"first_ns": start, "last_ns": end, "progress_stages": dict(stages),
                               "instruction_regressions": instruction_regressions},
            "coverage": {"guest_progress_present": start is not None,
                         "endpoint_events_present": bool(stages["endpoint-wake"] or stages["endpoint-submit"]),
                         "trace_overflow_known": False,
                         "note": "Lost-record totals exist in runtime stderr; CSV alone cannot establish completeness."},
            "counts": {key: counts[key] for key in keys}, "dma_records": dict(dma_counts),
            "wake_intervals": intervals.result(), "device_padding": padding_ms.result(),
            "queued_source": source_ms.result(), "examples": examples, "examples_omitted": omitted,
            "interpretation": ["Missing frames mean source starvation at render, not device underrun.",
                               "Empty padding and long wake gaps are risks, not proof of audible hardware loss.",
                               "First wake interval may include prefill and device Start blocking before playback.",
                               "Records after final guest progress are excluded as teardown; inspect coverage before claiming no failures.",
                               "DMA addresses and voice metadata are not duration or hardware playback measurements."]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--max-examples", type=int, default=32)
    args = parser.parse_args()
    result = analyze(args.trace, args.max_examples)
    text = json.dumps(result, indent=2) + "\n"
    if args.out:
        args.out.write_text(text, encoding="utf-8")
    else:
        print(text, end="")


if __name__ == "__main__":
    main()
