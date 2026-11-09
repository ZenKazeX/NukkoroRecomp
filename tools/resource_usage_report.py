"""Summarize measured CPU/RAM and sampled D3D12 memory without adding overlapping scopes.

Use a session.json plus GEKKO_GPU_MEMORY CSV from the same isolated run.
GPU maxima are sampled (normally once per second), not continuous peaks.
Instrumentation runs are diagnostics, not gameplay speed measurements.
"""
import argparse
import csv
import json
import math
from pathlib import Path

from graphics_baseline import fingerprint

HEAPS = ("default", "upload", "readback", "custom", "gpu_upload")


def summarize(session, rows):
    if session.get("stop") != {"reason": 0, "failed": False}:
        raise ValueError("run did not complete normally")
    if not rows:
        raise ValueError("GPU memory trace has no samples")
    numbers = []
    fields = ["ns", "frame", "local_usage", "non_local_usage", "mapped_bytes",
              "upload_ring_waits", "upload_dedicated"]
    for heap in HEAPS:
        fields += [f"{heap}_block_bytes", f"{heap}_allocation_bytes"]
    for row in rows:
        value = {key: int(row[key]) for key in fields}
        if any(n < 0 for n in value.values()):
            raise ValueError("negative resource measurement")
        if numbers and (value["ns"] < numbers[-1]["ns"] or value["frame"] < numbers[-1]["frame"]):
            raise ValueError("trace clocks/frames moved backwards")
        for heap in HEAPS:
            if value[f"{heap}_allocation_bytes"] > value[f"{heap}_block_bytes"]:
                raise ValueError("allocations exceed their heap blocks")
        numbers.append(value)
    host = session["host"]
    memory = session["host_stats"]["memory"]
    cpu = float(host["process_cpu_seconds"])
    wall = float(host["wall_seconds"])
    ram = {key: float(memory[key]) for key in
           ("working_set_mib", "peak_working_set_mib", "private_mib", "peak_private_mib")}
    if not math.isfinite(cpu) or cpu < 0 or not math.isfinite(wall) or wall <= 0 or any(
            not math.isfinite(n) or n < 0 for n in ram.values()):
        raise ValueError("invalid process measurement")
    peak = lambda key: max(row[key] for row in numbers)
    heaps = {}
    for heap in HEAPS:
        block, allocation = f"{heap}_block_bytes", f"{heap}_allocation_bytes"
        heaps[heap] = {"sampled_max_block_bytes": peak(block),
                       "sampled_max_allocation_bytes": peak(allocation),
                       # Subtract within each sample, never unrelated maxima.
                       "sampled_max_unused_block_bytes": max(row[block] - row[allocation] for row in numbers),
                       "last_block_bytes": numbers[-1][block],
                       "last_allocation_bytes": numbers[-1][allocation]}
    return {"schema": 1, "cpu": {"process_seconds": cpu, "wall_seconds": wall,
            "mean_logical_cores": cpu / wall}, "ram_mib": ram,
            "gpu": {"samples": len(numbers), "first_frame": numbers[0]["frame"],
                    "last_frame": numbers[-1]["frame"],
                    "sampled_max_local_usage_bytes": peak("local_usage"),
                    "sampled_max_non_local_usage_bytes": peak("non_local_usage"),
                    "sampled_max_cpu_mapped_bytes": peak("mapped_bytes"), "heaps": heaps,
                    "last_upload_ring_waits": numbers[-1]["upload_ring_waits"],
                    "last_oversized_uploads": numbers[-1]["upload_dedicated"]},
            "limitations": ["GPU maxima are sampled, not continuous peaks.",
                "DEFAULT is a D3D12 heap type, not a guarantee of physical VRAM residency.",
                "Driver local/non-local usage includes resources outside D3D12MA.",
                "CPU mappings, process RAM and GPU usage overlap; do not add them together.",
                "Allocated bytes, reserved heap slack and driver usage are distinct.",
                "This diagnostic summary alone does not establish a speed improvement."]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--session", type=Path, required=True)
    parser.add_argument("--gpu-csv", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    args.session, args.gpu_csv = args.session.resolve(), args.gpu_csv.resolve()
    try:
        pins = {"session": fingerprint(args.session), "gpu_csv": fingerprint(args.gpu_csv)}
        session = json.loads(args.session.read_text(encoding="utf-8-sig"))
        with args.gpu_csv.open(encoding="utf-8-sig", newline="") as stream:
            rows = list(csv.DictReader(line for line in stream if not line.startswith("#")))
        result = summarize(session, rows)
        if pins != {"session": fingerprint(args.session), "gpu_csv": fingerprint(args.gpu_csv)}:
            raise ValueError("input changed while summarizing")
        result["inputs"] = pins
        args.out.parent.mkdir(parents=True, exist_ok=True)
        with args.out.open("x", encoding="utf-8") as stream:
            json.dump(result, stream, indent=2)
            stream.write("\n")
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(1, f"resource report failed: {error}\n")
    print(args.out)


if __name__ == "__main__":
    main()
