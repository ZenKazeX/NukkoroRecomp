import contextlib
import io
import json
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import Mock, patch

import graphics_baseline as baseline


DEFAULT = object()
STDOUT = b"\xffraw stdout\r\nPresent hash: 00000000000000ab over 2 published frames\r\n"
STDERR = b"\x80raw stderr\r\n"
PCM = b"\x00\x01\x00\x02" * 2


class BaselineTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.game = self.root / "game data"
        self.exe = self.root / "original bin/ShadowTheHedgehog.exe"
        self.state = self.game / "save-data/route.gekkostate"
        self.config = self.game / "display.cfg"
        self.originals = {self.exe: b"unchanged executable", self.state: b"exact route state",
                          self.config: b"window_width=1920\r\nwindow_height=1080\r\n",
                          self.game / "sys/main.dol": b"external dol",
                          self.game / baseline.GCI: b"original gci",
                          self.game / baseline.GCI.parent / "Memory Card A.raw": b"original raw seed",
                          self.game / "GC/SRAM.raw": b"s" * 68}
        for name in baseline.SEEDS:
            self.originals[self.exe.parent / "save-data" / name] = name.encode("ascii")
        self.originals[self.exe.parent / "save-data/unrelated.gekkostate"] = b"do not copy"
        for path, data in self.originals.items():
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        self.processes, self.argv_seen = [], []

    def tearDown(self):
        for path, data in self.originals.items():
            self.assertEqual(path.read_bytes(), data)

    def cli(self, out, mode="capture", runs=1, reference_control=False, cold_boot=False):
        start = ["--cold-boot"] if cold_boot else ["--state", str(self.state)]
        return ["--exe", str(self.exe), "--game-data", str(self.game), *start,
                "--display-cfg", str(self.config), "--out", str(out), "--mode", mode, "--runs", str(runs),
                *(["--reference-control"] if reference_control else [])]

    def invoke(self, mode="capture", code=0, timeout=False, report_text=DEFAULT, change=None, omit=None, runs=1,
               reference_control=False, cold_boot=False, stdout_data=STDOUT, extra_args=(), audio_data=PCM):
        out = self.root / f"baseline-{len(list(self.root.glob('baseline-*')))}"
        seed_dir = (Path(extra_args[extra_args.index("--seed-dir") + 1]) if "--seed-dir" in extra_args else
                    self.exe.parent / "save-data")
        seeds = {name: (seed_dir / name).read_bytes() for name in baseline.SEEDS if (seed_dir / name).is_file()}
        def spawn(argv, cwd, stdin, stdout, stderr, shell):
            directory = Path(cwd)
            self.argv_seen.append(argv)
            self.assertFalse(shell)
            self.assertEqual(stdin, subprocess.DEVNULL)
            self.assertEqual(Path(argv[0]).parent, directory)
            self.assertNotEqual(Path(argv[0]), self.exe)
            self.assertEqual(Path(argv[0]).name, "ShadowTheHedgehogReference.exe" if reference_control else "ShadowTheHedgehog.exe")
            for flag in ("--reference-control", "--cold-boot", "--no-recomp", "--verify-recomp", "--seed-dir"):
                self.assertNotIn(flag, argv)
            for flag in ("--state-file", "--load-state"):
                self.assertEqual(flag in argv, not cold_boot)
            self.assertEqual((directory / "input.gekkostate").exists(), not cold_boot)
            for flag, original in (("--state-file", self.state), ("--display-cfg", self.config)):
                if cold_boot and flag == "--state-file":
                    continue
                target = Path(argv[argv.index(flag) + 1])
                self.assertEqual(target.parent, directory)
                self.assertEqual(target.read_bytes(), self.originals[original])
            self.assertEqual({path.name for path in (directory / "save-data").iterdir()}, set(seeds))
            for name, data in seeds.items():
                target = directory / "save-data" / name
                self.assertEqual(target.read_bytes(), data)
                target.write_bytes(b"sandbox runtime persistence")
            stdout.write(stdout_data)
            stderr.write(STDERR)
            report = {"stop": {"failed": False},
                      "run": {"budget": 2000000000, "window": mode == "performance", "dx12_raster": True,
                              "verify_recomp": False, "reference_execution": reference_control},
                      "capture": {"headless_requested": mode == "capture", "complete": mode == "capture",
                                  "observed_frames": 2 if mode == "capture" else 0, "expected_frames": 2 if mode == "capture" else 0},
                      "guest": {"native_entries": 20, "native_instructions": 2000000000, "interpreted_instructions": 0,
                                "session_instructions": 2000000000, "hardware_ticks": 100},
                      "renderer": {"available": True, "copies_seen": 2 if mode == "capture" else len(self.argv_seen),
                                   "copies_resolved": 2, "draw_batches": 2, "triangles": 4,
                                   "internal_scale": 1 if mode == "capture" else 3},
                      "presentation": {"available": mode == "performance", "rendered_frames": 2 if mode == "performance" else 0,
                                       "average_fps": 60.0, "one_percent_low_fps": 50.0},
                      "host": {"wall_seconds": 1.0, "process_cpu_seconds": 0.75}}
            if "--record-audio" in extra_args:
                report["audio"] = {"active": True, "produced_samples": 2, "underruns": 0}
            if change:
                change(report)
            text = json.dumps(report) if report_text is DEFAULT else report_text
            if text is not None:
                (directory / "session.json").write_text(text, encoding="utf-8")
            outputs = ("final.ppm", "frames/frame_000000.ppm", "frames/frame_000001.ppm") if mode == "capture" else ("frames.csv",)
            for name in outputs:
                if name != omit:
                    target = directory / name
                    target.parent.mkdir(exist_ok=True)
                    target.write_bytes(b"frame,seconds\n1,0.016\n" if mode == "performance" else b"P6\n1 1\n255\n\x00\x01\x02")
            if "--audio-dump" in argv:
                target = Path(argv[argv.index("--audio-dump") + 1])
                self.assertEqual(target, directory / "audio.pcm")
                self.assertFalse(target.exists())
                if omit != "audio.pcm":
                    target.write_bytes(audio_data)
            process = Mock()
            process.poll.return_value = None if timeout else code
            if timeout:
                process.wait.side_effect = [subprocess.TimeoutExpired(argv, 180), -9]
            else:
                process.wait.return_value = code
            self.processes.append(process)
            return process
        with patch.object(baseline, "git_metadata", return_value={}), \
             patch.object(baseline, "source_inventory", return_value=[]), \
             patch.object(baseline.platform, "machine", return_value="AMD64"), \
             patch.object(baseline.platform, "platform", return_value="mock platform"), \
             patch.object(baseline.platform, "processor", return_value="mock processor"), \
             patch.object(baseline.subprocess, "Popen", side_effect=spawn), contextlib.redirect_stdout(io.StringIO()):
            status = baseline.main(self.cli(out, mode, runs, reference_control, cold_boot) + list(extra_args))
        return status, json.loads((out / "index.json").read_text(encoding="utf-8")), out

    def test_modes_argv_and_independent_immutable_seeds(self):
        for mode in ("capture", "performance"):
            status, index, out = self.invoke(mode, runs=2)
            self.assertEqual(status, 0)
            self.assertEqual(index["original_cli_args"], self.cli(out, mode, 2))
            self.assertEqual((index["schema"], index["evidence_role"], index["start_mode"]), (3, "native", "checkpoint"))
            self.assertIn("capture_time_source_inventory_not_build_manifest", index)
            for name in baseline.SEEDS:
                self.assertEqual(index["inputs"]["seed:" + name],
                                 baseline.fingerprint((self.exe.parent / "save-data" / name).resolve()))
            for row in index["runs"]:
                self.assertEqual((row["evidence_role"], row["start_mode"]), ("native", "checkpoint"))
                self.assertEqual(row["capture_verification"], "complete" if mode == "capture" else "not_requested")
                self.assertEqual(row["capture_frame_count"], 2 if mode == "capture" else None)
                argv = row["argv"]
                self.assertIn("--no-xinput", argv)
                self.assertNotIn("--memory-card", argv)
                for flag in ("--save-state", "--tune", "--internal-scale", "--verify-recomp", "--frame-capture", "--audio-dump", "--record-audio"):
                    self.assertNotIn(flag, argv)
                self.assertNotIn("audio.pcm", row["artifacts"])
                self.assertFalse((Path(row["cwd"]) / "audio.pcm").exists())
                for flag in ("--headless", "--screenshot", "--present-hash", "--filmstrip", "--filmstrip-stride"):
                    self.assertEqual(flag in argv, mode == "capture")
                for flag in ("--windowed", "--perf-log"):
                    self.assertEqual(flag in argv, mode == "performance")
                self.assertEqual(row["metrics"]["host.wall_seconds"], 1.0)
                self.assertEqual(row["changed_original_inputs"], [])
            self.assertEqual(index["summary"]["frame_sequence_equal"], True if mode == "capture" else None)
            self.assertEqual(index["summary"]["repeat_invariants_equal"]["renderer.copies_seen"], True if mode == "capture" else None)
        for path, data in self.originals.items():
            self.assertEqual(path.read_bytes(), data)

    def test_seed_dir_cli_and_optional_defaults(self):
        argv = self.cli(self.root / "unused")
        args = baseline.parse_args(argv)
        inputs = baseline.collect_inputs(args)
        vars(args).pop("seed_dir", None)
        self.assertEqual(baseline.collect_inputs(args), inputs)
        self.assertEqual(baseline.parse_args(argv + ["--seed-dir", "relative seeds"]).seed_dir, Path("relative seeds"))
        with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
            baseline.parse_args(argv + ["--seed-dir"])
        original_exe = self.exe
        self.exe = self.root / "fresh bin" / original_exe.name
        self.exe.parent.mkdir()
        self.originals[self.exe] = self.originals[original_exe]
        self.exe.write_bytes(self.originals[self.exe])
        status, index, _ = self.invoke()
        self.assertEqual(status, 0)
        for name in baseline.SEEDS:
            self.assertEqual(index["inputs"]["seed:" + name],
                             baseline.fingerprint((self.exe.parent / "save-data" / name).resolve()))
            self.assertNotIn("seed:" + name, index["runs"][0]["snapshots"])

    def test_seed_dir_override_copies_only_named_seeds_and_records_hashes(self):
        seed_dir = self.root / "old build" / "save-data"
        seed_dir.mkdir(parents=True)
        for name in (*baseline.SEEDS, "unrelated.gekkostate", "display.cfg", "ShadowTheHedgehog.exe"):
            path = seed_dir / name
            self.originals[path] = b"override:" + name.encode("ascii")
            path.write_bytes(self.originals[path])
        for mode, cold_boot in (("capture", False), ("performance", True)):
            with self.subTest(mode=mode):
                _, defaults, _ = self.invoke(mode=mode, cold_boot=cold_boot)
                extra = ["--seed-dir", str(seed_dir / ".." / seed_dir.name)]
                if mode == "performance":
                    extra += ["--record-audio", "--internal-scale", "3"]
                status, index, out = self.invoke(mode=mode, cold_boot=cold_boot, runs=2, extra_args=extra)
                self.assertEqual(status, 0)
                self.assertEqual({key: value for key, value in index["inputs"].items() if not key.startswith("seed:")},
                                 {key: value for key, value in defaults["inputs"].items() if not key.startswith("seed:")})
                expected_cli = self.cli(out, mode=mode, runs=2, cold_boot=cold_boot) + extra
                self.assertEqual(index["original_cli_args"], expected_cli)
                provenance = json.loads((out / "provenance.json").read_text(encoding="utf-8"))
                self.assertEqual(provenance["original_cli_args"], expected_cli)
                for name in baseline.SEEDS:
                    expected = baseline.fingerprint((seed_dir / name).resolve())
                    self.assertEqual(index["inputs"]["seed:" + name], expected)
                    self.assertEqual(provenance["inputs"]["seed:" + name], expected)
                    for row in index["runs"]:
                        target = Path(row["cwd"]) / "save-data" / name
                        self.assertEqual(row["snapshots"]["seed:" + name], {**expected, "path": str(target)})
                for row in index["runs"]:
                    self.assertEqual(row["changed_original_inputs"], [])
                    self.assertNotIn("--seed-dir", row["argv"])
                    self.assertEqual({path.name for path in (Path(row["cwd"]) / "save-data").iterdir()}, set(baseline.SEEDS))

    def test_seed_dir_missing_named_seeds_do_not_fall_back(self):
        for present in ((), (baseline.CARD,)):
            with self.subTest(present=present):
                seed_dir = self.root / f"optional-seeds-{len(present)}"
                seed_dir.mkdir()
                for name in (*present, "unrelated.gekkostate"):
                    path = seed_dir / name
                    self.originals[path] = b"override:" + name.encode("ascii")
                    path.write_bytes(self.originals[path])
                status, index, _ = self.invoke(mode="performance", extra_args=["--seed-dir", str(seed_dir)])
                self.assertEqual(status, 0)
                for name in baseline.SEEDS:
                    self.assertEqual(index["inputs"]["seed:" + name], baseline.fingerprint((seed_dir / name).resolve()))
                    self.assertEqual("seed:" + name in index["runs"][0]["snapshots"], name in present)

    def test_seed_dir_invalid_explicit_directory_is_refused_before_outputs(self):
        for number, seed_dir in enumerate((self.root / "missing seeds", self.config)):
            with self.subTest(seed_dir=seed_dir):
                out = self.root / f"invalid-seed-{number}"
                stderr = io.StringIO()
                with patch.object(baseline.subprocess, "Popen") as spawn, \
                     patch.object(baseline, "source_inventory", return_value=[]) as inventory, \
                     patch.object(baseline, "git_metadata", return_value={}), contextlib.redirect_stderr(stderr):
                    self.assertEqual(baseline.main(self.cli(out) + ["--seed-dir", str(seed_dir)]), 2)
                    spawn.assert_not_called()
                    inventory.assert_not_called()
                self.assertIn("--seed-dir", stderr.getvalue())
                self.assertFalse(out.exists())

    def test_diagnostic_defaults_preserve_legacy_callers(self):
        for mode in ("capture", "performance"):
            def change(doc):
                doc["audio"] = {"produced_samples": "invalid"}
                if mode == "performance":
                    doc["renderer"].pop("internal_scale")
            status, index, out = self.invoke(mode=mode, change=change)
            self.assertEqual(status, 0)
            args = baseline.parse_args(self.cli(out, mode))
            args.game_data = args.game_data.resolve()
            vars(args).pop("internal_scale", None)
            vars(args).pop("record_audio", None)
            row = index["runs"][0]
            directory = Path(row["cwd"])
            expected = [str(directory / "ShadowTheHedgehog.exe"), "--game-data", str(args.game_data),
                        "--state-file", str(directory / "input.gekkostate"), "--load-state",
                        "--display-cfg", str(directory / "display.cfg"), "--budget", "2000000000",
                        "--no-xinput", "--json-report", str(directory / "session.json")]
            expected += (["--headless", "--screenshot", str(directory / "final.ppm"), "--present-hash",
                          "--filmstrip", str(directory / "frames"), "--filmstrip-stride", "1"] if mode == "capture" else
                         ["--windowed", "--perf-log", str(directory / "frames.csv")])
            self.assertEqual(row["argv"], expected)
            self.assertEqual(baseline.runtime_argv(args, directory), expected)
            self.assertEqual(baseline.validate_run(args, directory)["errors"], [])
            self.assertNotIn("audio.pcm", row["artifacts"])

    def test_internal_scale_cli_choices(self):
        argv = self.cli(self.root / "unused")
        for scale in range(1, 7):
            self.assertEqual(baseline.parse_args(argv + ["--internal-scale", str(scale)]).internal_scale, scale)
        for value in ("0", "7", "-1", "auto", "1.5", "true"):
            with self.subTest(value=value), self.assertRaises(SystemExit) as stopped, contextlib.redirect_stderr(io.StringIO()):
                baseline.parse_args(argv + ["--internal-scale", value])
            self.assertEqual(stopped.exception.code, 2)
        with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
            baseline.parse_args(argv + ["--internal-scale"])

    def test_internal_scale_is_forwarded_and_recorded(self):
        for mode in ("capture", "performance"):
            for scale in (1, 6):
                with self.subTest(mode=mode, scale=scale):
                    extra = ["--internal-scale", str(scale)]
                    if mode == "performance" and scale == 6:
                        extra += ["--record-audio"]
                    status, index, out = self.invoke(mode=mode, extra_args=extra,
                                                     change=lambda doc: doc["renderer"].update(internal_scale=scale))
                    self.assertEqual(status, 0)
                    row = index["runs"][0]
                    self.assertEqual(row["argv"].count("--internal-scale"), 1)
                    self.assertEqual(row["argv"][row["argv"].index("--internal-scale") + 1], str(scale))
                    self.assertEqual(row["effective_mode"]["internal_scale"], scale)
                    expected_cli = self.cli(out, mode) + extra
                    self.assertEqual(index["original_cli_args"], expected_cli)
                    provenance = json.loads((out / "provenance.json").read_text(encoding="utf-8"))
                    self.assertEqual(provenance["original_cli_args"], expected_cli)

    def test_requested_internal_scale_must_match_report(self):
        for mode in ("capture", "performance"):
            for value in (DEFAULT, 1, 3, None, True, "2", 2.0, -1):
                with self.subTest(mode=mode, value=value):
                    def change(doc):
                        if value is DEFAULT:
                            doc["renderer"].pop("internal_scale")
                        else:
                            doc["renderer"]["internal_scale"] = value
                    status, index, _ = self.invoke(mode=mode, extra_args=["--internal-scale", "2"], change=change)
                    self.assertEqual(status, 1)
                    self.assertTrue(any("renderer.internal_scale" in error for error in index["runs"][0]["errors"]))

    def test_record_audio_is_performance_only(self):
        out = self.root / "invalid-audio-capture"
        args = baseline.parse_args(self.cli(out, mode="performance") + ["--record-audio"])
        self.assertTrue(args.record_audio)
        for reference in (False, True):
            with patch.object(baseline.subprocess, "Popen") as spawn, contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as stopped:
                    baseline.main(self.cli(out, reference_control=reference) + ["--record-audio"])
                self.assertEqual(stopped.exception.code, 2)
                spawn.assert_not_called()
            self.assertFalse(out.exists())
        with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
            baseline.parse_args(self.cli(out, mode="performance") + ["--record-audio", "false"])

    def test_record_audio_isolated_artifacts_and_defaults(self):
        for directory in (self.exe.parent, self.game):
            original = directory / "audio.pcm"
            self.originals[original] = b"original audio must not be overwritten or copied"
            original.write_bytes(self.originals[original])
        for cold_boot in (False, True):
            status, index, out = self.invoke(mode="performance", extra_args=["--record-audio"], runs=2, cold_boot=cold_boot)
            self.assertEqual(status, 0)
            cli = self.cli(out, mode="performance", runs=2, cold_boot=cold_boot)
            self.assertEqual(index["original_cli_args"], cli + ["--record-audio"])
            provenance = json.loads((out / "provenance.json").read_text(encoding="utf-8"))
            self.assertEqual(provenance["original_cli_args"], cli + ["--record-audio"])
            args = baseline.parse_args(cli)
            args.game_data = args.game_data.resolve()
            paths = []
            for row in index["runs"]:
                directory, argv = Path(row["cwd"]), row["argv"]
                position = argv.index("--audio-dump")
                self.assertEqual(argv.count("--audio-dump"), 1)
                self.assertEqual(argv[:position] + argv[position + 2:], baseline.runtime_argv(args, directory))
                path = directory / "audio.pcm"
                self.assertEqual(Path(argv[position + 1]), path)
                self.assertEqual(path.read_bytes(), PCM)
                self.assertEqual(row["artifacts"]["audio.pcm"], baseline.fingerprint(path))
                self.assertEqual(row["changed_original_inputs"], [])
                paths.append(path)
            self.assertEqual(len(set(paths)), 2)
            self.assertFalse((out / "audio.pcm").exists())

    def test_record_audio_rejects_missing_empty_incomplete_or_mismatched_pcm(self):
        for data, error_text in ((None, "Missing or empty required artifact: audio.pcm"),
                                 (b"", "Missing or empty required artifact: audio.pcm"),
                                 (PCM[:-1], "divisible by 4"),
                                 (PCM[:4], "audio.produced_samples"), (PCM + PCM[:4], "audio.produced_samples")):
            with self.subTest(data=data):
                status, index, _ = self.invoke(mode="performance", extra_args=["--record-audio"],
                                               omit="audio.pcm" if data is None else None, audio_data=data)
                self.assertEqual(status, 1)
                row = index["runs"][0]
                self.assertEqual(row["outcome"], "failed")
                self.assertEqual(row["artifacts"]["audio.pcm"]["size"], None if data is None else len(data))
                self.assertTrue(any(error_text in error for error in row["errors"]))

    def test_record_audio_rejects_malformed_audio_counts(self):
        for value in (None, True, False, "2", 2.0, -1, [], {}, 0, 1, 3):
            with self.subTest(produced_samples=value):
                status, index, _ = self.invoke(mode="performance", extra_args=["--record-audio"],
                                               change=lambda doc: doc["audio"].update(produced_samples=value))
                self.assertEqual(status, 1)
                self.assertTrue(any("audio.produced_samples" in error for error in index["runs"][0]["errors"]))
        for value in (None, [], "invalid", True):
            with self.subTest(audio=value):
                status, index, _ = self.invoke(mode="performance", extra_args=["--record-audio"],
                                               change=lambda doc: doc.update(audio=value))
                self.assertEqual(status, 1)
                self.assertTrue(any("audio" in error for error in index["runs"][0]["errors"]))

    def test_record_audio_accepts_unreported_legacy_sample_counts(self):
        for change in (lambda doc: doc.pop("audio"), lambda doc: doc["audio"].pop("produced_samples")):
            status, index, _ = self.invoke(mode="performance", extra_args=["--record-audio"], change=change)
            self.assertEqual(status, 0)
            self.assertEqual(index["runs"][0]["artifacts"]["audio.pcm"]["size"], len(PCM))

    def test_timeout_retains_raw_output_and_kills_only_owned_process(self):
        status, index, out = self.invoke(timeout=True)
        self.assertEqual(status, 1)
        self.assertTrue(index["runs"][0]["timed_out"])
        self.processes[0].kill.assert_called_once_with()
        self.assertEqual((out / "run-001/stdout.bin").read_bytes(), STDOUT)
        self.assertEqual((out / "run-001/stderr.bin").read_bytes(), STDERR)

    def test_failing_exit_rejects_good_report(self):
        status, index, _ = self.invoke(code=5)
        self.assertEqual(status, 1)
        self.assertFalse(index["ok"])
        self.processes[0].kill.assert_not_called()

    def test_missing_malformed_reports_and_null_metrics(self):
        for text in (None, "{bad", "[]", '{"stop":0,"stop":1}', '{"bad":NaN}'):
            with self.subTest(text=text):
                status, index, _ = self.invoke(report_text=text)
                self.assertEqual(status, 1)
                self.assertIsNone(index["runs"][0]["metrics"]["guest.native_entries"])
        for value in (None, "20", True, -1):
            status, index, _ = self.invoke(change=lambda doc: doc["guest"].update(native_entries=value))
            self.assertEqual(status, 1)
            self.assertIsNone(index["runs"][0]["metrics"]["guest.native_entries"])

    def test_missing_required_metrics(self):
        for section, key in (("guest", "native_entries"), ("guest", "interpreted_instructions"),
                             ("renderer", "copies_seen"), ("run", "dx12_raster"), ("stop", "failed")):
            status, index, _ = self.invoke(change=lambda doc: doc[section].pop(key))
            self.assertEqual(status, 1)
            self.assertIsNone(index["runs"][0]["metrics"][section + "." + key])

    def test_no_native_rendering_or_failed_stop(self):
        for section, key, value in (("guest", "native_entries", 0), ("guest", "interpreted_instructions", 1),
                                    ("renderer", "copies_seen", 0), ("run", "dx12_raster", False), ("stop", "failed", True)):
            with self.subTest(key=key):
                status, _, _ = self.invoke(change=lambda doc: doc[section].update({key: value}))
                self.assertEqual(status, 1)

    def test_missing_required_artifacts(self):
        for mode, name in (("capture", "final.ppm"), ("capture", "frames/frame_000001.ppm"), ("performance", "frames.csv")):
            status, _, _ = self.invoke(mode=mode, omit=name)
            self.assertEqual(status, 1)

    def assert_incomplete_capture(self, **kwargs):
        status, index, _ = self.invoke(**kwargs)
        self.assertEqual(status, 1)
        self.assertEqual(index["runs"][0]["capture_verification"], "incomplete_unverified")
        self.assertTrue(any("Capture incomplete/unverified" in error for error in index["runs"][0]["errors"]))

    def test_capture_metadata_required_and_strict(self):
        self.assert_incomplete_capture(change=lambda doc: doc.pop("capture"))
        for value in (None, {}, [], True, "legacy"):
            self.assert_incomplete_capture(change=lambda doc: doc.update(capture=value))
        for key in ("headless_requested", "complete", "observed_frames", "expected_frames"):
            self.assert_incomplete_capture(change=lambda doc: doc["capture"].pop(key))
            invalid = (False, None, 1, "true") if key in ("headless_requested", "complete") else (0, -1, True, "2", 2.0, None)
            for value in invalid:
                with self.subTest(key=key, value=value):
                    self.assert_incomplete_capture(change=lambda doc: doc["capture"].update({key: value}))

    def test_capture_counts_must_match_without_drop_tolerance(self):
        for key in ("observed_frames", "expected_frames"):
            self.assert_incomplete_capture(change=lambda doc: doc["capture"].update({key: 3}))
        change = lambda doc: doc["capture"].update(observed_frames=3, expected_frames=3)
        three_frames = STDOUT.replace(b"over 2 ", b"over 3 ")
        self.assert_incomplete_capture(change=change)
        self.assert_incomplete_capture(stdout_data=three_frames)
        self.assert_incomplete_capture(change=change, stdout_data=three_frames)
        self.assert_incomplete_capture(omit="frames/frame_000001.ppm")
        self.assert_incomplete_capture(stdout_data=STDOUT.replace(b"Present hash:", b"No present hash:"))

    def test_reference_control_is_explicit_external_capture(self):
        status, index, out = self.invoke(reference_control=True, runs=2)
        self.assertEqual(status, 0)
        self.assertEqual(index["evidence_role"], "reference_control")
        for row in index["runs"]:
            self.assertEqual(row["evidence_role"], "reference_control")
            self.assertTrue(row["effective_mode"]["reference_execution_reported"])
            invocation = json.loads((Path(row["cwd"]) / "invocation.json").read_text(encoding="utf-8"))
            self.assertEqual(invocation["evidence_role"], "reference_control")
        self.assertEqual(json.loads((out / "provenance.json").read_text(encoding="utf-8"))["evidence_role"], "reference_control")
        self.assert_incomplete_capture(reference_control=True, change=lambda doc: doc.pop("capture"))
        for value in (False, None, 1, "true"):
            status, _, _ = self.invoke(reference_control=True, change=lambda doc: doc["run"].update(reference_execution=value))
            self.assertEqual(status, 1)
        status, _, _ = self.invoke(reference_control=True, change=lambda doc: doc["run"].pop("reference_execution"))
        self.assertEqual(status, 1)
        for key, value in (("native_entries", 0), ("native_instructions", 0), ("interpreted_instructions", 1)):
            status, _, _ = self.invoke(reference_control=True, change=lambda doc: doc["guest"].update({key: value}))
            self.assertEqual(status, 1)

    def test_reference_control_cannot_be_performance(self):
        out = self.root / "invalid-reference-performance"
        with patch.object(baseline.subprocess, "Popen") as spawn, contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit) as stopped:
                baseline.main(self.cli(out, mode="performance", reference_control=True))
            self.assertEqual(stopped.exception.code, 2)
            spawn.assert_not_called()
        self.assertFalse(out.exists())

    def test_cold_boot_omits_state_inputs_hashes_and_copies(self):
        for mode, reference in (("capture", False), ("performance", False), ("capture", True)):
            with patch.object(baseline, "fingerprint", wraps=baseline.fingerprint) as probe:
                status, index, _ = self.invoke(mode=mode, reference_control=reference, cold_boot=True, runs=2)
            self.assertEqual(status, 0)
            self.assertEqual(index["start_mode"], "cold_boot")
            self.assertNotIn("state", index["inputs"])
            self.assertNotIn(self.state, [call.args[0] for call in probe.call_args_list])
            for row in index["runs"]:
                self.assertEqual(row["start_mode"], "cold_boot")
                self.assertNotIn("state", row["snapshots"])
                self.assertEqual(row["changed_original_inputs"], [])
                invocation = json.loads((Path(row["cwd"]) / "invocation.json").read_text(encoding="utf-8"))
                self.assertEqual(invocation["start_mode"], "cold_boot")

    def test_cold_boot_still_requires_budget_and_frames(self):
        for mode in ("capture", "performance"):
            status, _, _ = self.invoke(mode=mode, cold_boot=True,
                                       change=lambda doc: doc["guest"].update(session_instructions=1999999999))
            self.assertEqual(status, 1)
        self.assert_incomplete_capture(cold_boot=True, omit="frames/frame_000001.ppm")
        status, _, _ = self.invoke(mode="performance", cold_boot=True,
                                   change=lambda doc: doc["presentation"].update(rendered_frames=0))
        self.assertEqual(status, 1)

    def test_state_and_cold_boot_are_required_and_exclusive(self):
        missing = self.cli(self.root / "missing-start", cold_boot=True)
        missing.remove("--cold-boot")
        conflict = self.cli(self.root / "conflicting-start") + ["--cold-boot"]
        for argv in (missing, conflict):
            with patch.object(baseline.subprocess, "Popen") as spawn, contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as stopped:
                    baseline.main(argv)
                self.assertEqual(stopped.exception.code, 2)
                spawn.assert_not_called()

    def test_existing_output_and_duplicate_files_refused(self):
        out = self.root / "existing"
        out.mkdir()
        with patch.object(baseline.subprocess, "Popen") as spawn, contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(baseline.main(self.cli(out)), 2)
            self.assertEqual(baseline.main(self.cli(out, mode="performance") + ["--record-audio", "--internal-scale", "2"]), 2)
            spawn.assert_not_called()
        self.assertEqual(list(out.iterdir()), [])
        target = out / "duplicate.json"
        baseline.write_json(target, {"original": True})
        before = target.read_bytes()
        with self.assertRaises(FileExistsError):
            baseline.write_json(target, {})
        with self.assertRaises(FileExistsError):
            baseline.copy_new(self.exe, target, baseline.fingerprint(self.exe))
        self.assertEqual(target.read_bytes(), before)

    def test_incomplete_route_is_not_a_baseline(self):
        for session in (None, 0, 1999999999):
            with self.subTest(session=session):
                status, index, _ = self.invoke(change=lambda doc: doc["guest"].update(session_instructions=session))
                self.assertEqual(status, 1)
                self.assertFalse(index["ok"])

    def test_performance_requires_actual_presentation_and_timing(self):
        for section, key, value in (("presentation", "available", False),
                                    ("presentation", "rendered_frames", 0),
                                    ("host", "wall_seconds", None),
                                    ("host", "process_cpu_seconds", None)):
            with self.subTest(key=key):
                status, _, _ = self.invoke(mode="performance", change=lambda doc: doc[section].update({key: value}))
                self.assertEqual(status, 1)

    def test_capture_repeat_divergence_fails(self):
        status, index, _ = self.invoke(runs=2, change=lambda doc: doc["renderer"].update(triangles=len(self.argv_seen)))
        self.assertFalse(index["summary"]["repeat_invariants_equal"]["renderer.triangles"])
        self.assertEqual(status, 1)
        self.assertFalse(index["ok"])

    def test_reported_mode_flags_are_checked_when_available(self):
        status, _, _ = self.invoke(mode="performance", change=lambda doc: doc["run"].update(native_xfb_enabled=False))
        self.assertEqual(status, 1)
        status, index, _ = self.invoke(mode="performance", change=lambda doc: doc["run"].update(native_xfb_enabled=True))
        self.assertEqual(status, 0)
        self.assertTrue(index["runs"][0]["effective_mode"]["native_xfb_reported"])

    def test_reference_execution_cannot_be_a_native_baseline(self):
        for mode in ("capture", "performance"):
            for value in (True, None, 1, "false"):
                status, index, _ = self.invoke(mode=mode, change=lambda doc: doc["run"].update(reference_execution=value))
                self.assertEqual(status, 1)
                self.assertFalse(index["ok"])
        status, index, _ = self.invoke(change=lambda doc: doc["run"].update(reference_execution=False))
        self.assertEqual(status, 0)
        self.assertFalse(index["runs"][0]["effective_mode"]["reference_execution_reported"])
        status, index, _ = self.invoke(change=lambda doc: doc["run"].pop("reference_execution"))
        self.assertEqual(status, 0)
        self.assertIsNone(index["runs"][0]["effective_mode"]["reference_execution_reported"])

    def test_positive_cli_and_defaults(self):
        argv = self.cli(self.root / "unused")[:-2]
        args = baseline.parse_args(argv)
        self.assertEqual((args.runs, args.budget, args.timeout), (3, 2000000000, 180))
        self.assertIsNone(args.internal_scale)
        self.assertFalse(args.record_audio)
        self.assertIsNone(args.seed_dir)
        for flag, value in (("--runs", "0"), ("--budget", "-1"), ("--timeout", "0"), ("--timeout", "nan"), ("--timeout", "inf")):
            with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
                baseline.parse_args([*argv, flag, value])


if __name__ == "__main__":
    unittest.main()
