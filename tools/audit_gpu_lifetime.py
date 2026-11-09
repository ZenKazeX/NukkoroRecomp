"""Extract emitted-order evidence without inventing cross-thread timestamps.

Usage: python tools/audit_gpu_lifetime.py CAPTURE --output timeline.json
This reads a capture only; it never launches or changes the runtime.
"""

import argparse
import hashlib
import json
import re
from pathlib import Path


def analyze(path):
    raw = path.read_bytes()
    lines = raw.decode("utf-8-sig", errors="replace").splitlines()
    events = []
    for i, line in enumerate(lines):
        if not line.startswith(("[host-stall]", "[gpu-diverge]", "[audio-diverge]",
                                "[audio-underrun]", "[present-gap]", "[frame-pacing]")):
            continue
        kind = line[1:line.index("]")]
        event = {"line": i + 1, "kind": kind, "text": line}
        if kind == "host-stall":
            fields = {}
            for detail in lines[i + 1:i + 12]:
                match = re.match(r"\s+(\w+)\s+(.*)", detail)
                if match:
                    fields[match[1]] = match[2]
            event["fields"] = fields
            duration = re.search(r" ([\d.]+) ms", line)
            wait = re.match(r"([\d.]+) ms", fields.get("gpu_wait", ""))
            if duration and wait:
                event["wall_ms"] = float(duration[1])
                event["gpu_wait_since_previous_printed_stall_ms"] = float(wait[1])
                event["gpu_delta_exceeds_window"] = float(wait[1]) > float(duration[1])
        if kind in ("audio-underrun", "audio-diverge"):
            time = re.search(r"t=([\d.]+) s", line)
            if time:
                event["audio_open_relative_s"] = float(time[1])
        events.append(event)

    acquire = next((e for e in events if "native-xfb acquire miss" in e["text"]), None)
    ring = next((e for e in events if "submission ring full" in e["text"]), None)
    audio = [e for e in events if e["kind"] == "audio-underrun"]
    preceding = [e for e in audio if ring and e["line"] < ring["line"]]
    following = [e for e in audio if ring and e["line"] > ring["line"]]
    return {
        "capture": str(path.resolve()),
        "sha256": hashlib.sha256(raw).hexdigest(),
        "lines": len(lines),
        "ordering": "emitted text order; GPU events have no wall timestamp",
        "first_acquire_miss_line": acquire["line"] if acquire else None,
        "first_audio_underrun": audio[0] if audio else None,
        "first_ring_full_line": ring["line"] if ring else None,
        "audio_events_before_first_ring_full": len(preceding),
        "ring_audio_neighbors": {
            "preceding_audio_s": preceding[-1].get("audio_open_relative_s") if preceding else None,
            "following_audio_s": following[0].get("audio_open_relative_s") if following else None,
            "meaning": "neighboring log records, not an exact GPU event timestamp",
        },
        "gpu_delta_larger_than_stall_lines": [e["line"] for e in events
                                                if e.get("gpu_delta_exceeds_window")],
        "limitations": [
            "Acquire miss has no slot, producer serial, pin owner, or precise miss reason.",
            "UI and backend FIRST labels use independent counters.",
            "GPU wait deltas span printed stalls; they are not individual fence waits.",
            "native_execution is residual wall time, not measured thread CPU time.",
            "No run ID or executable hash is embedded in the capture.",
        ],
        "events": events,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = analyze(args.capture)
    encoded = json.dumps(result, indent=2, ensure_ascii=False) + "\n"
    if args.output:
        args.output.write_text(encoded, encoding="utf-8")
    else:
        print(encoded, end="")


if __name__ == "__main__":
    main()
