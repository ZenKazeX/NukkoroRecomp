import argparse
import csv
import hashlib
import json
import math
import os
import platform
import re
import shutil
import statistics
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
GCI = Path("GC/EUR/Card A/8P-GUPP-SHADOWTHEHEDGEHOG.gci")
CARD = GCI.stem + ".pc.raw"
SEEDS = (CARD, CARD + ".source", "SRAM.pc.raw", "disc-read-profile.txt")
BOOLS = ("stop.failed run.window run.dx12_raster run.verify_recomp run.native_xfb_enabled "
         "run.reference_execution run.frame_capture run.filmstrip run.present_hash "
         "renderer.available presentation.available audio.active capture.headless_requested capture.complete").split()
COUNTERS = ("run.budget guest.native_entries guest.native_instructions guest.interpreted_instructions "
            "guest.session_instructions guest.hardware_ticks renderer.copies_seen renderer.copies_resolved "
            "renderer.draw_batches renderer.triangles renderer.internal_scale presentation.rendered_frames "
            "capture.observed_frames capture.expected_frames").split()
MEASUREMENTS = ("host.wall_seconds host.process_cpu_seconds presentation.average_fps "
                "presentation.one_percent_low_fps renderer.guest_blocked.total_ms "
                "renderer.timing.command_record_s renderer.timing.gpu_execution_s renderer.timing.cpu_fence_wait_s "
                "renderer.timing.readback_s renderer.timing.readback_mib "
                "host_stats.geometry.decode_ms host_stats.geometry.clip_ms host_stats.geometry.commit_ms "
                "host_stats.geometry.state_ms host_stats.geometry.texture_hash_ms host_stats.flipper.execute_ms "
                "host_stats.flipper.enqueue_guest_ms host_stats.memory.private_mib host_stats.memory.peak_private_mib "
                "renderer.native_efb_mib renderer.efb_copy_texture_peak_mib renderer.caches.descriptors_peak").split()
INVARIANTS = [key for key in COUNTERS if key.startswith(("guest.", "renderer."))]
NOTES = [
    "Local baseline capture of an existing executable; the harness does not change its rendering implementation.",
    "No Dolphin/visual parity or performance improvement is established by this tool.",
    "evidence_role=native rejects reported reference-capable builds; absent legacy build-role fields remain unknown, not proof of interpreter absence.",
    "evidence_role=reference_control requires --reference-control and a positively reported external reference-capable binary, capture mode only.",
    "Reference control is external generated-code validation, NOT native-production evidence or a Dolphin reference; interpreted instructions must still be zero.",
    "No interpreter/fallback flags or runtime translation are added. Evidence roles name requested purposes; failed runs supply no accepted evidence.",
    "start_mode=checkpoint copies the supplied state; cold_boot starts the original entry without state loading, preserving the same isolated persistence seeds.",
    "Complete capture requires positive observed/expected counts through terminal drain matching the present hash and every frame; legacy missing metadata is incomplete/unverified.",
    "Capture-time source inventory is NOT an exact build manifest and may differ from the binary's sources.",
    "External sys/main.dol is fingerprinted; an embedded DOL is covered by the binary hash, not extracted or verified.",
    "Capture is intrusive: native XFB disabled, headless audio/AX off, automatic internal scale 1 regardless of display.cfg.",
    "Capture is NOT shipped timing or performance evidence; image hashes and present chains are local reproducibility only.",
    "Performance uses windowed perf-log only; native XFB and audio retain runtime defaults, automatic scale follows the display.",
    "At 1920x1080 the current windowed auto scale is 3; capture and performance modes are incomparable for timing or quality.",
    "Windowed frame counters may vary with timing. No input is injected; --no-xinput does not block keyboard/window interaction.",
    "Known persistence writes are executable-local; original GCI/raw/SRAM companions are runtime read-only inputs, never raw card outputs.",
    "Only named seeds are copied. Missing optional seeds stay absent. Full game assets, dependencies, host load and thermals are not inventoried.",
]


def utc():
    return datetime.now(timezone.utc).isoformat()


def fingerprint(path):
    result = {"path": str(path), "size": None, "sha256": None}
    if path.exists():
        digest, size = hashlib.sha256(), 0
        with path.open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
                size += len(block)
        result.update(size=size, sha256=digest.hexdigest())
    return result


def write_json(path, value):
    with path.open("x", encoding="utf-8") as stream:
        json.dump(value, stream, indent=2, allow_nan=False)
        stream.write("\n")


def copy_new(source, target, expected):
    with source.open("rb") as reader, target.open("xb") as writer:
        shutil.copyfileobj(reader, writer)
    target.chmod(source.stat().st_mode | 0o200)
    actual = fingerprint(target)
    if any(actual[key] != expected[key] for key in ("sha256", "size")):
        raise ValueError(f"Input changed while copying: {source}")
    return actual


def git_metadata():
    result = {}
    for name, command in (("head", ["rev-parse", "HEAD"]),
                          ("status", ["status", "--porcelain=v1", "--untracked-files=normal"])):
        try:
            process = subprocess.run(["git", "--no-optional-locks", "-C", str(ROOT), *command],
                                     capture_output=True, timeout=15, shell=False)
            result[name] = {"exit_code": process.returncode,
                            "stdout": process.stdout.decode("utf-8", "replace").strip(),
                            "stderr": process.stderr.decode("utf-8", "replace")}
        except (OSError, subprocess.TimeoutExpired) as error:
            result[name] = {"exit_code": None, "error": str(error)}
    status = result["status"]
    result["dirty"] = bool(status["stdout"]) if status["exit_code"] == 0 else None
    return result


def source_inventory():
    paths = {ROOT / "CMakeLists.txt", *(ROOT / "thirdparty/plume" / name for name in
             ("plume_d3d12.cpp", "plume_d3d12.h", "plume_render_interface.h", "plume_render_interface_types.h"))}
    for folder, patterns in ((ROOT / "runtime", ("*.cpp", "*.h")),
                             (ROOT / "tools/ppcrecomp", ("*.cpp", "*.h", "*.py", "*.cmake.in")),
                             (ROOT / "tools/shaderbake", ("*.cpp", "*.h"))):
        for pattern in patterns:
            paths.update(folder.rglob(pattern))
    return [fingerprint(path) for path in sorted(paths)
            if path.is_file() and path.resolve().is_relative_to(ROOT)]


def collect_inputs(args):
    paths = {name: getattr(args, name) for name in ("exe", "state", "display_cfg") if getattr(args, name) is not None}
    required = tuple(paths) + ("dol", "gci", "raw_card_seed")
    paths.update(dol=args.game_data / "sys/main.dol", gci=args.game_data / GCI,
                 raw_card_seed=args.game_data / GCI.parent / "Memory Card A.raw")
    seed_dir = getattr(args, "seed_dir", None)
    if seed_dir is None:
        seed_dir = args.exe.parent / "save-data"
    elif not seed_dir.is_dir():
        raise ValueError(f"--seed-dir must be an existing directory: {seed_dir}")
    paths.update({"seed:" + name: seed_dir / name for name in SEEDS})
    paths.update({"sys:" + name: args.game_data / "sys" / name
                  for name in ("boot.bin", "bi2.bin", "apploader.img", "fst.bin")})
    directories = [paths["gci"].parent, *paths["gci"].parent.parents][:6]
    paths.update({f"sram_candidate:{i}": folder / "SRAM.raw" for i, folder in enumerate(directories)})
    inputs = {name: fingerprint(path) for name, path in paths.items()}
    for name in required:
        if not inputs[name]["size"]:
            raise ValueError(f"Missing or empty required input: {paths[name]}")
    return inputs


def changed_inputs(inputs):
    return [name for name, before in inputs.items() if fingerprint(Path(before["path"])) != before]


def executable_name(args):
    return "ShadowTheHedgehogReference.exe" if args.reference_control else "ShadowTheHedgehog.exe"


def runtime_argv(args, directory):
    argv = [str(directory / executable_name(args)), "--game-data", str(args.game_data)]
    extra = list(getattr(args, "game_arg", None) or [])
    if args.state is not None:
        argv += ["--state-file", str(directory / "input.gekkostate"), "--load-state"]
    argv += ["--display-cfg", str(directory / "display.cfg"), "--budget", str(args.budget),
             "--no-xinput", "--json-report", str(directory / "session.json")]
    if getattr(args, "internal_scale", None) is not None:
        argv += ["--internal-scale", str(args.internal_scale)]
    if args.mode == "capture":
        return argv + ["--headless", "--screenshot", str(directory / "final.ppm"),
                       "--present-hash", "--filmstrip", str(directory / "frames"),
                       "--filmstrip-stride", "1"] + extra
    if getattr(args, "record_audio", False):
        argv += ["--audio-dump", str(directory / "audio.pcm")]
    argv += ["--windowed", "--perf-log", str(directory / "frames.csv")]
    return argv + extra


def launch(argv, directory, timeout):
    result = {"exit_code": None, "timed_out": False, "interrupted": False, "launch_error": None}
    process, started = None, time.perf_counter()
    try:
        with (directory / "stdout.bin").open("xb") as stdout, (directory / "stderr.bin").open("xb") as stderr:
            try:
                process = subprocess.Popen(argv, cwd=str(directory), stdin=subprocess.DEVNULL,
                                           stdout=stdout, stderr=stderr, shell=False)
                result["exit_code"] = process.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                result["timed_out"] = True
            except KeyboardInterrupt:
                result["interrupted"] = True
            finally:
                if process is not None and process.poll() is None:
                    process.kill()
                    result["exit_code"] = process.wait()
    except OSError as error:
        result["launch_error"] = str(error)
    result["harness_wall_seconds"] = time.perf_counter() - started
    return result


def reject_json(value):
    raise ValueError(f"Invalid JSON constant: {value}")


def unique_object(pairs):
    if len(dict(pairs)) != len(pairs):
        raise ValueError("Duplicate JSON keys")
    return dict(pairs)


def metrics(document):
    result = {}
    for key in BOOLS + COUNTERS + MEASUREMENTS:
        value = document
        for part in key.split("."):
            value = value.get(part) if isinstance(value, dict) else None
        valid = (type(value) is bool if key in BOOLS else
                 type(value) is int and value >= 0 if key in COUNTERS else
                 type(value) in (int, float) and math.isfinite(value) and value >= 0)
        result[key] = value if valid else None
    return result


def validate_run(args, directory):
    errors, document, artifacts = [], {}, {}
    try:
        document = json.loads((directory / "session.json").read_text(encoding="utf-8"),
                              parse_constant=reject_json, object_pairs_hook=unique_object)
        if not isinstance(document, dict):
            raise ValueError("Expected a JSON object")
    except (OSError, ValueError) as error:
        errors.append(f"session.json: {error}")
        document = {}
    values = metrics(document)
    expected = {"stop.failed": False, "run.window": args.mode == "performance",
                "run.dx12_raster": True, "run.verify_recomp": False, "renderer.available": True,
                "run.budget": args.budget, "guest.interpreted_instructions": 0}
    if getattr(args, "internal_scale", None) is not None:
        expected["renderer.internal_scale"] = args.internal_scale
    if args.reference_control:
        expected["run.reference_execution"] = True
    for key, value in expected.items():
        if values[key] is None or values[key] != value:
            errors.append(f"{key}: expected {value}, got {values[key]}")
    for key in ("guest.native_entries", "guest.native_instructions", "renderer.copies_seen", "renderer.triangles"):
        if values[key] is None or values[key] <= 0:
            errors.append(f"{key}: missing/invalid or no native rendering data")
    if values["guest.session_instructions"] is None or values["guest.session_instructions"] < args.budget:
        errors.append("Instruction budget not completed; early termination is not a baseline")
    reported_run = document.get("run", {})
    mode_flags = {"native_xfb_enabled": args.mode == "performance", "frame_capture": False,
                  "filmstrip": args.mode == "capture", "present_hash": args.mode == "capture",
                  "reference_execution": args.reference_control}
    for key, value in mode_flags.items():
        if isinstance(reported_run, dict) and key in reported_run and values["run." + key] != value:
            errors.append(f"run.{key}: expected {value}, got {values['run.' + key]}")
    if args.mode == "capture":
        errors.extend(f"Missing capture invariant: {key}" for key in INVARIANTS if values[key] is None)
    else:
        if values["presentation.available"] is not True:
            errors.append("Native presentation is unavailable or not reported")
        for key in ("presentation.rendered_frames", "host.wall_seconds", "host.process_cpu_seconds"):
            if values[key] is None or values[key] <= 0:
                errors.append(f"Missing or empty performance measurement: {key}")
    chain, published, frames, coverage = None, None, [], None
    required = [directory / "session.json"]
    if args.mode == "capture":
        with (directory / "stdout.bin").open("rb") as stream:
            matches = re.findall(rb"(?m)^Present hash: ([0-9a-fA-F]{16}) over (\d+) published frames\r?$", stream.read())
        if len(matches) == 1:
            chain, published = matches[0][0].decode("ascii").lower(), int(matches[0][1])
        frames = sorted((directory / "frames").glob("frame_*.ppm"))
        observed, expected_frames = values["capture.observed_frames"], values["capture.expected_frames"]
        coverage = (values["capture.headless_requested"] is True and values["capture.complete"] is True
                    and observed is not None and expected_frames is not None
                    and 0 < observed == expected_frames == published == len(frames))
        if not coverage:
            errors.append("Capture incomplete/unverified: require headless_requested and complete true, positive "
                          "observed_frames == expected_frames == present hash count == actual frame count")
        if not frames or published != len(frames):
            errors.append("Missing capture frames or present chain/frame count mismatch")
        if [path.name for path in frames] != [f"frame_{i:06d}.ppm" for i in range(len(frames))]:
            errors.append("Capture frames are not a contiguous sequence starting at zero")
        required += [directory / "final.ppm", *frames]
    else:
        required.append(directory / "frames.csv")
        try:
            with (directory / "frames.csv").open(newline="", encoding="utf-8") as stream:
                rows = csv.reader(stream, strict=True)
                if not next(rows, None) or not any(row for row in rows):
                    errors.append("frames.csv has no presentation samples")
        except (OSError, ValueError, csv.Error) as error:
            errors.append(f"frames.csv: {error}")
    if getattr(args, "record_audio", False):
        required.append(directory / "audio.pcm")
    for path in required + [directory / "stdout.bin", directory / "stderr.bin"]:
        info = fingerprint(path)
        artifacts[str(path.relative_to(directory))] = info
        if path in required and not info["size"]:
            errors.append(f"Missing or empty required artifact: {path.name}")
    if "audio.pcm" in artifacts:
        size = artifacts["audio.pcm"]["size"]
        if size is not None and size % 4:
            errors.append("audio.pcm: size must be divisible by 4 for S16 stereo samples")
        audio = document.get("audio", {})
        if not isinstance(audio, dict):
            errors.append("audio: expected an object")
        elif "produced_samples" in audio:
            samples = audio["produced_samples"]
            if type(samples) is not int or samples < 0:
                errors.append(f"audio.produced_samples: expected a nonnegative integer, got {samples!r}")
            elif size != samples * 4:
                errors.append(f"audio.pcm: expected {samples * 4} bytes from audio.produced_samples, got {size}")
    return {"metrics": values, "artifacts": artifacts, "present_chain64": chain,
            "capture_verification": "not_requested" if coverage is None else "complete" if coverage else "incomplete_unverified",
            "capture_frame_count": len(frames) if args.mode == "capture" else None,
            "effective_mode": {"window": values["run.window"], "dx12_raster": values["run.dx12_raster"],
                               "internal_scale": values["renderer.internal_scale"], "audio_active": values["audio.active"],
                               "native_xfb_default_expected": args.mode == "performance",
                               "native_xfb_reported": values["run.native_xfb_enabled"],
                               "reference_execution_reported": values["run.reference_execution"]},
            "graphics_observations": document.get("graphics_observations"),
            "published_frames": published, "frame_sha256": [artifacts[str(path.relative_to(directory))]["sha256"] for path in frames],
            "errors": errors}


def run_one(args, directory, inputs):
    directory.mkdir()
    (directory / "save-data").mkdir()
    row = {"mode": args.mode, "evidence_role": args.evidence_role, "start_mode": args.start_mode,
           "argv": runtime_argv(args, directory), "cwd": str(directory), "utc": utc(),
           "metrics": metrics({}), "errors": [], "snapshots": {}, "interrupted": False}
    write_json(directory / "invocation.json", row)
    try:
        changed = changed_inputs(inputs)
        if changed:
            raise ValueError(f"Original inputs changed before run: {changed}")
        targets = {"exe": executable_name(args), "state": "input.gekkostate", "display_cfg": "display.cfg"}
        targets.update({"seed:" + name: "save-data/" + name for name in SEEDS})
        for name, target in targets.items():
            if name in inputs and inputs[name]["size"] is not None:
                row["snapshots"][name] = copy_new(Path(inputs[name]["path"]), directory / target, inputs[name])
        row["launch_utc"] = utc()
        row.update(launch(row["argv"], directory, args.timeout))
        row.update(validate_run(args, directory))
        if row["exit_code"] != 0 or row["timed_out"] or row["interrupted"] or row["launch_error"]:
            row["errors"].append("Process failed, timed out, was interrupted, or could not launch")
    except (OSError, ValueError) as error:
        row["errors"].append(str(error))
    try:
        row["changed_original_inputs"] = changed_inputs(inputs)
        if row["changed_original_inputs"]:
            row["errors"].append("Original input fingerprints changed; not restored or overwritten")
    except OSError as error:
        row["errors"].append(f"Cannot verify original inputs: {error}")
    row["outcome"] = "failed" if row["errors"] else "ok"
    row["finished_utc"] = utc()
    write_json(directory / "result.json", row)
    return row


def summarize(mode, runs):
    eligible = mode == "capture" and len(runs) > 1 and all(row["outcome"] == "ok" for row in runs)
    def equal(values):
        return all(value == values[0] for value in values) if eligible and None not in values else None
    successful = [row for row in runs if row["outcome"] == "ok"]
    measurements = {}
    for key in MEASUREMENTS:
        samples = [row["metrics"].get(key) for row in successful if row["metrics"].get(key) is not None]
        measurements[key] = {"samples": samples, "missing": len(successful) - len(samples),
                             "min": min(samples) if samples else None,
                             "median": statistics.median(samples) if samples else None,
                             "max": max(samples) if samples else None}
    return {"scope": "Local baseline evidence only; never compare modes or infer parity/improvement",
            "measurement_policy": "All successful runs, no warmup or outlier removal; capture mode is intrusive, not performance evidence",
            "failed_runs": len(runs) - len(successful), "measurements": measurements,
            "comparison_performed": eligible,
            "repeat_invariants_equal": {key: equal([row["metrics"][key] for row in runs]) for key in INVARIANTS},
            "frame_sequence_equal": equal([row.get("frame_sha256") for row in runs]),
            "present_chain_equal": equal([row.get("present_chain64") for row in runs]),
            "screenshot_equal": equal([row.get("artifacts", {}).get("final.ppm", {}).get("sha256") for row in runs])}


def positive_int(text):
    value = int(text)
    if value <= 0:
        raise argparse.ArgumentTypeError("Must be positive")
    return value


def positive_seconds(text):
    value = float(text)
    if not math.isfinite(value) or value <= 0:
        raise argparse.ArgumentTypeError("Must be finite and positive")
    return value


def parse_args(argv):
    parser = argparse.ArgumentParser(description=NOTES[0] + " " + NOTES[1], allow_abbrev=False)
    for name in ("exe", "game-data", "display-cfg", "out"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--seed-dir", type=Path, default=None)
    start = parser.add_mutually_exclusive_group(required=True)
    start.add_argument("--state", type=Path, help="Copy and load this exact checkpoint in each isolated run")
    start.add_argument("--cold-boot", action="store_true", help="Start the original entry without loading a checkpoint")
    parser.add_argument("--mode", required=True, choices=("capture", "performance"))
    parser.add_argument("--reference-control", action="store_true", help="Capture-only external validation binary, not native production")
    parser.add_argument("--internal-scale", type=int, choices=range(1, 7), default=None)
    parser.add_argument("--record-audio", action="store_true")
    parser.add_argument("--runs", type=positive_int, default=3)
    parser.add_argument("--budget", type=positive_int, default=2000000000)
    parser.add_argument("--timeout", type=positive_seconds, default=180)
    # Append-only passthrough to the game binary, for A/B arms of a host
    # switch. Absent = the exact command line every earlier capture used.
    parser.add_argument("--game-arg", action="append", default=None,
                        help="Extra argument passed verbatim to the game (repeatable)")
    args = parser.parse_args(argv)
    if args.reference_control and args.mode != "capture":
        parser.error("--reference-control is only allowed in capture mode")
    if args.record_audio and args.mode != "performance":
        parser.error("--record-audio is only allowed in performance mode")
    args.evidence_role = "reference_control" if args.reference_control else "native"
    args.start_mode = "cold_boot" if args.cold_boot else "checkpoint"
    return args


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    args = parse_args(argv)
    try:
        if args.out.exists() or args.out.is_symlink():
            raise FileExistsError(f"Refusing existing output: {args.out}")
        for name in ("exe", "game_data", "state", "display_cfg", "seed_dir", "out"):
            if getattr(args, name) is not None:
                setattr(args, name, getattr(args, name).resolve())
        inputs = collect_inputs(args)
        inventory = source_inventory()
        metadata = {"schema": 3, "utc": utc(), "original_cli_args": argv, "harness_cwd": str(Path.cwd()),
                    "harness": fingerprint(Path(__file__).resolve()), "evidence_role": args.evidence_role, "start_mode": args.start_mode,
                    "mode": args.mode, "requested_runs": args.runs, "budget": args.budget, "timeout_seconds": args.timeout,
                    "game_data": str(args.game_data), "inputs": inputs, "notes": NOTES, "git": git_metadata(),
                    "capture_time_source_inventory_not_build_manifest": inventory,
                    "machine": {"platform": platform.platform(), "machine": platform.machine(),
                                "processor": platform.processor(), "logical_processors": os.cpu_count(), "python": sys.version}}
        args.out.mkdir()
        write_json(args.out / "provenance.json", metadata)
        runs = []
        for number in range(args.runs):
            runs.append(run_one(args, args.out / f"run-{number + 1:03d}", inputs))
            if runs[-1]["interrupted"]:
                break
        summary = summarize(args.mode, runs)
        repeat_checks = [*summary["repeat_invariants_equal"].values(), summary["frame_sequence_equal"],
                         summary["present_chain_equal"], summary["screenshot_equal"]]
        summary["reproducible"] = all(value is True for value in repeat_checks) if summary["comparison_performed"] else None
        index = {**metadata, "runs": runs, "summary": summary,
                 "ok": len(runs) == args.runs and all(row["outcome"] == "ok" for row in runs)
                       and summary["reproducible"] is not False}
        write_json(args.out / "index.json", index)
        print(f"Local baseline index: {args.out / 'index.json'}")
        return 0 if index["ok"] else 1
    except (OSError, ValueError) as error:
        print(f"Baseline refused/failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
