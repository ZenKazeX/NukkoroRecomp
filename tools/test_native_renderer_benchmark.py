"""Offline benchmark gates: real saved stdout/report and adversarial pairs."""
import copy
import json
import tempfile
from pathlib import Path
import unittest
from unittest.mock import Mock, patch

from native_renderer_benchmark import extract, errors_for, summarize, idle_msbuild_proof, screenshot_frames, screenshot_manifest, validate_affinity_mask, select_process_affinity


ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "tools/test_data/renderer-benchmark/final-fastgeo"


def row(arm="control", repetition=0, group="timing"):
    metrics = dict(guest_session_instructions=2000000001, frames=247, draws=95410,
                   immediate_draws=37921, mesh_draws=57489, clears=1976, copies=2240,
                   interpreted_instructions=0, record_ms=90.0, screenshots=0)
    report = dict(stop=dict(reason=0, failed=False), run=dict(reference_execution=False),
                  audio=dict(active=False))
    return dict(arm=arm, repetition=repetition, group=group, metrics=metrics, report=report,
                exit_code=0, errors=[], stdout="SILENT DROPS: none\nRESOURCE REJECTIONS: none",
                fixture=dict(state="same", card="same", settings=dict(group=group)),
                snapshots=dict(executable=dict(sha256=arm)))


class BenchmarkTests(unittest.TestCase):
    def test_reference_identity_is_explicit_and_verification_only(self):
        value = row(group="verification")
        value["metrics"]["screenshots"] = 1
        value["screenshot"] = {"sha256": "a" * 64}
        value["report"]["run"]["reference_execution"] = True
        self.assertIn("not positively identified as production executable", errors_for(value, "verification"))
        self.assertEqual(errors_for(value, "verification", evidence_role="reference_control"), [])
        self.assertIn("reference control is verification-only, never shipping timing evidence",
                      errors_for(value, "timing", evidence_role="reference_control"))
        for identity in (False, None, 1, "true"):
            value["report"]["run"]["reference_execution"] = identity
            self.assertIn("not positively identified as reference executable",
                          errors_for(value, "verification", evidence_role="reference_control"))
        self.assertIn("unknown runtime evidence role", errors_for(value, "verification", evidence_role="anything"))

    def test_reference_role_preserves_workload_and_failure_gates(self):
        value = row(group="verification")
        value["metrics"]["screenshots"] = 1
        value["screenshot"] = {"sha256": "a" * 64}
        value["report"]["run"].update(reference_execution=True, native_renderer_failed=True)
        value["metrics"].update(interpreted_instructions=1, draws=0)
        errors = errors_for(value, "verification", evidence_role="reference_control")
        self.assertIn("native renderer terminal failure", errors)
        self.assertIn("non-native execution or absent counter", errors)
        self.assertIn("no live renderer work", errors)

    def test_identical_binary_calibration_never_proves_source_gain(self):
        before, after = row(), row('candidate')
        before['snapshots']['executable']['sha256'] = 'A' * 64
        after['snapshots']['executable']['sha256'] = 'a' * 64
        after['metrics']['record_ms'] = 80
        proof = summarize([before, after])
        self.assertTrue(proof['accepted'])
        self.assertEqual(proof['comparison']['kind'], 'identical-binary-calibration')
        self.assertFalse(proof['comparison']['performance_gain_proven'])
        self.assertFalse(proof['comparison']['calibration_bias_correction_applied'])
        self.assertEqual(proof['metrics']['record_ms']['paired_median_delta'], -10)
        after['snapshots']['executable']['sha256'] = 'b' * 64
        self.assertEqual(summarize([before, after])['comparison']['kind'], 'different-binary-observation')
        after['snapshots']['executable']['sha256'] = 'missing'
        self.assertEqual(summarize([before, after])['comparison']['kind'], 'single-arm-or-unproven-binary-identity')

    def test_actual_capture_order_strata_do_not_assume_file_or_arm_order(self):
        rows = []
        for repetition, (control_sequence, candidate_sequence, delta) in enumerate(((0,1,4),(3,2,-6),(None,None,2))):
            before, after = row(repetition=repetition), row('candidate', repetition)
            before['capture_sequence'], after['capture_sequence'] = control_sequence, candidate_sequence
            after['metrics']['record_ms'] += delta
            rows.extend((after, before)) # Input ordering is not capture chronology.
        proof = summarize(rows)['metrics']['record_ms']
        self.assertEqual(proof['paired_sign_counts'], dict(negative=1, zero=0, positive=2))
        self.assertEqual(proof['paired_capture_order']['control-first']['deltas'], [4])
        self.assertEqual(proof['paired_capture_order']['candidate-first']['deltas'], [-6])
        self.assertEqual(proof['paired_capture_order']['unrecorded']['deltas'], [2])

    def test_affinity_bounds_and_available_subset(self):
        validate_affinity_mask(0xffff, 0xffffffff, 64)
        for mask in (0, -1, 1 << 64, 0x10000):
            with self.assertRaises(ValueError): validate_affinity_mask(mask, 0xffff, 64)

    def test_affinity_default_never_calls_windows_api(self):
        api = Mock()
        self.assertIsNone(select_process_affinity(None, api))
        self.assertFalse(api.mock_calls)

    def test_affinity_confirmation_and_failure(self):
        api = Mock()
        api.GetCurrentProcess.return_value = 42
        current = [0xffffffff]
        def get(handle, process, system):
            process._obj.value = current[0]; system._obj.value = 0xffffffff
            return 1
        def set_mask(handle, mask):
            current[0] = mask
            return 1
        api.GetProcessAffinityMask.side_effect = get
        api.SetProcessAffinityMask.side_effect = set_mask
        with patch('native_renderer_benchmark.os.name', 'nt'):
            proof = select_process_affinity(0xffff, api)
            self.assertEqual(proof['previous_mask'], '0xffffffff')
            self.assertEqual(proof['confirmed_mask'], '0xffff')
            self.assertFalse(proof['production_scheduling_policy'])
            api.SetProcessAffinityMask.return_value = 0
            api.SetProcessAffinityMask.side_effect = None
            with self.assertRaises(ValueError): select_process_affinity(0xffff, api)
            api.GetProcessAffinityMask.return_value = 0
            api.GetProcessAffinityMask.side_effect = None
            with self.assertRaises(ValueError): select_process_affinity(0xffff, api)

    def test_pinned_and_normal_scheduling_cannot_be_pooled(self):
        before, after = row(), row('candidate')
        after['fixture']['cpu_affinity'] = {'requested_mask': '0xffff'}
        result = summarize([before, after])
        self.assertFalse(result['accepted'])

    def test_complete_six_frame_pixel_manifest(self):
        frames = screenshot_frames(1, 1, 6, 6)
        with tempfile.TemporaryDirectory(dir=ROOT / "tools", prefix="benchmark-pixels-") as temporary:
            directory = Path(temporary)
            self.assertEqual(directory.resolve().parent, (ROOT / "tools").resolve())
            for frame in frames:
                (directory / f"final_{frame:06d}.ppm").write_bytes(b"P6\n2 1\n255\n" + bytes([frame] * 6))
            manifest = screenshot_manifest(directory, frames, 1, (2, 1))
            self.assertFalse(manifest["errors"])
            self.assertEqual(len(manifest["files"]), 6)
            self.assertLessEqual(manifest["total_bytes"], manifest["maximum_bytes"])
            rows = [row(group="verification"), row("candidate", group="verification")]
            for value in rows:
                value["screenshots"] = copy.deepcopy(manifest)
                value["screenshot"] = manifest["files"][0]
                value["metrics"]["screenshots"] = 6
                self.assertFalse(errors_for(value, "verification"))
            self.assertTrue(summarize(rows)["accepted"])
            rows[1]["screenshots"]["files"][3]["pixel_sha256"] = "0" * 64
            self.assertIn("Per-frame native pixel bytes/hashes differ", summarize(rows)["errors"])
            rows[1]["screenshots"]["files"].pop()
            self.assertIn("Screenshot manifest frame coverage differs or is incomplete", summarize(rows)["errors"])
            (directory / "final_000006.ppm").unlink()
            self.assertIn("incomplete screenshot frame coverage", screenshot_manifest(directory, frames, 1, (2, 1))["errors"])
            (directory / "final_1.ppm").write_bytes(b"P6\n2 1\n255\n" + b"\1" * 6)
            self.assertIn("duplicate screenshot frame 1", screenshot_manifest(directory, frames, 1, (2, 1))["errors"])

    def test_screenshot_demand_bounds_and_corrupt_bytes(self):
        for arguments in ((0, 1, 6, 6), (1, 0, 6, 6), (1, 1, 7, 6), (2, 1, 6, 6), (1, -1, 1, 6)):
            with self.assertRaises(ValueError): screenshot_frames(*arguments)
        self.assertEqual(screenshot_frames(1, 2, 3, 6), [1, 3, 5])
        with tempfile.TemporaryDirectory(dir=ROOT / "tools", prefix="benchmark-pixels-") as temporary:
            directory = Path(temporary)
            self.assertEqual(directory.resolve().parent, (ROOT / "tools").resolve())
            path = directory / "final.ppm"
            path.write_bytes(b"P6\n2 1\n255\n" + b"\0" * 5)
            self.assertTrue(screenshot_manifest(directory, [3], 0, (2, 1))["errors"])
            path.write_bytes(b"P6\n2 1\n255\n" + b"\0" * 7)
            self.assertTrue(screenshot_manifest(directory, [3], 0, (2, 1))["errors"])
            path.write_bytes(b"P6\n2 1\n255\n" + b"\0" * 6)
            self.assertFalse(screenshot_manifest(directory, [3], 0, (2, 1))["errors"])

    def test_live_saved_report_parser(self):
        stdout = FIXTURE.with_suffix(".stdout.txt").read_text(encoding="utf-8")
        report = json.loads(FIXTURE.with_suffix(".json").read_text(encoding="utf-8"))
        metrics = extract(stdout, report)
        self.assertEqual((metrics["frames"], metrics["draws"]), (247, 95410))
        self.assertEqual(metrics["constant_materializations"], 93561)
        self.assertEqual(metrics["upload_owner.draw-constants.allocations"], 73469)
        self.assertEqual(metrics["transfer_owner.texture.allocations"], 81)
        self.assertEqual(metrics["native_memory.total.allocations"], report["native_memory"]["total"]["allocations"])
        self.assertEqual(metrics["screenshots"], 1)
        saved = row()
        saved.update(metrics=metrics, stdout=stdout, report=report)
        self.assertIn("screenshot contaminated timing", errors_for(saved, "timing"))

    def test_host_request_exit_zero_rejected(self):
        value = row()
        value["report"]["stop"]["reason"] = 5
        self.assertIn("run did not reach instruction budget", errors_for(value, "timing"))

    def test_wrong_route_and_missing_renderer_rejected(self):
        value = row()
        value["expected"] = dict(frames=247, draws=95410)
        value["metrics"]["frames"] = 246
        self.assertIn("unexpected frames", errors_for(value, "timing"))
        value["metrics"].pop("draws")
        self.assertIn("missing workload counters", errors_for(value, "timing"))

    def test_pair_exact_work_and_fixture(self):
        control, candidate = row(), row("candidate")
        candidate["metrics"]["draws"] -= 1
        self.assertFalse(summarize([control, candidate])["accepted"])
        candidate = row("candidate")
        candidate["fixture"]["card"] = "changed"
        self.assertFalse(summarize([control, candidate])["accepted"])

    def test_pair_robust_statistics_and_ranking(self):
        rows = []
        for repetition, (before, after) in enumerate(((100, 90), (110, 100), (300, 305))):
            control, candidate = row(repetition=repetition), row("candidate", repetition)
            control["metrics"].update(record_ms=before, **{"upload_owner.vertex.allocations": 50})
            candidate["metrics"].update(record_ms=after, **{"upload_owner.vertex.allocations": 40})
            rows.extend((control, candidate))
        report = summarize(rows)
        self.assertTrue(report["accepted"])
        self.assertEqual(report["metrics"]["record_ms"]["paired_deltas"], [-10, -10, 5])
        self.assertEqual(report["metrics"]["record_ms"]["paired_median_delta"], -10)
        self.assertEqual(report["allocation_owner_ranking"][0]["owner"], "upload_owner.vertex.allocations")

    def test_instrumentation_and_screenshots_separate(self):
        self.assertFalse(summarize([row(), row("candidate", group="instrumented")])["accepted"])
        before, after = row(group="verification"), row("candidate", group="verification")
        before["screenshot"] = dict(sha256="a")
        after["screenshot"] = dict(sha256="b")
        self.assertFalse(summarize([before, after])["accepted"])
        after["screenshot"] = copy.deepcopy(before["screenshot"])
        self.assertTrue(summarize([before, after])["accepted"])

    def test_production_and_resource_failures(self):
        value = row()
        value["report"]["run"]["reference_execution"] = True
        value["metrics"]["ring_allocation_failures"] = 1
        errors = errors_for(value, "timing")
        self.assertIn("not positively identified as production executable", errors)
        self.assertIn("renderer refused work/resources", errors)

    def test_inclusive_phase_and_raw_tsc_parser(self):
        lines = ["[native-cpu-profile] inclusive render-thread timings; nested phases overlap, do not sum or subtract clock overhead; clock-pair 60000 ns/4096 pairs"]
        lines.extend(f"[native-cpu-profile] phase-{i} calls=10 total-ms={i + .5} max-us=2.5" for i in range(21))
        lines += ["[native-cpu-profile] perspective raw-key reuse hits=50 misses=4 non-neutral-policy=0 (census only; no cache enabled)",
                  "[native-capture-cpu] projection address=2147483904 calls=40 ticks=5000"]
        value = row(group="instrumented")
        value["stdout"] += "\n" + "\n".join(lines)
        value["metrics"].update(extract(value["stdout"], {"guest": {"session_instructions": 2000000001, "interpreted_instructions": 0}}))
        self.assertFalse(errors_for(value, "instrumented"))
        self.assertIn("CPU instrumentation contaminated uninstrumented timing", errors_for(value, "timing"))
        self.assertEqual(value["metrics"]["capture_cpu.projection.tsc_ticks"], 5000)
        self.assertEqual(value["metrics"]["perspective_raw_key_hits"], 50)
        report = summarize([value])
        self.assertEqual(report["inclusive_cpu_phase_ranking"][0]["owner"], "cpu_phase.phase-20.total_ms")
        self.assertIn("do not sum", report["profile_interpretation"])

    def test_missing_profile_and_unpaired_arm_fail(self):
        value = row(group="instrumented")
        self.assertIn("missing complete CPU phase instrumentation", errors_for(value, "instrumented"))
        self.assertFalse(summarize([row(), row("candidate", repetition=1)])["accepted"])

    def test_only_proven_idle_msbuild_nodes_eligible(self):
        evidence = dict(active_root_build=False, sample_interval_seconds=1,
                        processes=[dict(pid=123, command_line="MSBuild.exe /nodemode:1 /nodeReuse:true",
                                        cpu_seconds_before=4.0, cpu_seconds_after=4.0, parent_build_absent=True)])
        self.assertTrue(idle_msbuild_proof(evidence, [123]))
        evidence["active_root_build"] = True
        self.assertFalse(idle_msbuild_proof(evidence, [123]))
        evidence["active_root_build"] = False
        evidence["processes"][0]["cpu_seconds_after"] = 4.1
        self.assertFalse(idle_msbuild_proof(evidence, [123]))
        evidence["processes"][0]["cpu_seconds_after"] = 4.0
        evidence["processes"][0]["command_line"] = "MSBuild.exe project.vcxproj"
        self.assertFalse(idle_msbuild_proof(evidence, [123]))
        self.assertFalse(idle_msbuild_proof(evidence, [999]))

    def test_constant_validation_requires_live_byte_checks(self):
        value = row(group="verification")
        value["screenshot"] = dict(sha256="same")
        value["environment"] = dict(GEKKO_NATIVE_CONSTANT_VALIDATE="1")
        value["metrics"].update(extract("[native-gpu] restored constant sources reused=0 byte-checks=4 mismatches=0", {"guest": {"session_instructions": 2000000001, "interpreted_instructions": 0}}))
        self.assertFalse(errors_for(value, "verification"))
        value["metrics"]["constant_restore_mismatches"] = 1
        self.assertIn("restored constants failed byte equivalence", errors_for(value, "verification"))
        value["metrics"]["constant_restore_checks"] = 0
        self.assertIn("no restored constant byte checks executed", errors_for(value, "verification"))

    def test_declared_phase_count_and_queue_stage_failures(self):
        value = row(group="instrumented")
        lines = ["[native-cpu-profile] inclusive render-thread timings; clock-pair 80 ns/4096 pairs; phase-count=24"]
        lines += [f"[native-cpu-profile] phase-{i} calls=2 total-ms=1.0 max-us=500.0" for i in range(24)]
        lines += ["[native-queue-cpu] total calls=3 total-ms=2.1 max-us=900.0 failures=0 last-hresult=0"]
        value["stdout"] += "\n" + "\n".join(lines)
        value["metrics"].update(extract(value["stdout"], {"guest": {"session_instructions": 2000000001, "interpreted_instructions": 0}}))
        self.assertFalse(errors_for(value, "instrumented"))
        self.assertEqual(value["metrics"]["profile_phase_count"], 24)
        self.assertEqual(summarize([value])["inclusive_queue_stage_ranking"][0]["owner"], "queue_cpu.total.total_ms")
        value["metrics"].pop("cpu_phase.phase-23.calls")
        self.assertIn("missing complete CPU phase instrumentation", errors_for(value, "instrumented"))
        value["metrics"]["queue_cpu.total.failures"] = 1
        self.assertIn("native queue submission failure", errors_for(value, "instrumented"))
        queue_only = row()
        queue_only["metrics"]["queue_cpu.total.calls"] = 1
        self.assertIn("CPU instrumentation contaminated uninstrumented timing", errors_for(queue_only, "timing"))

    def test_sampled_fragments_rank_separately_without_extrapolation(self):
        value = row(group="instrumented")
        stdout = "[native-cpu-profile] constants-sampling stride=64 materializations=640 samples=10; no extrapolation\n"
        stdout += "[native-cpu-profile] constants calls=640 total-ms=12 max-us=80\n"
        stdout += "[native-cpu-profile] sample-constants-texgen calls=10 total-ms=0.1 max-us=12"
        value["metrics"].update(extract(stdout, {"guest": {"session_instructions": 2000000001, "interpreted_instructions": 0}}))
        summary = summarize([value])
        self.assertEqual(value["metrics"]["constants_sample_stride"], 64)
        self.assertEqual(value["metrics"]["constants_sample_count"], 10)
        self.assertEqual(len(summary["inclusive_cpu_phase_ranking"]), 1)
        self.assertEqual(summary["sampled_constants_fragment_ranking"][0]["owner"], "cpu_phase.sample-constants-texgen.total_ms")
        self.assertIn("must not be extrapolated", summary["profile_interpretation"])

    def test_binding_census_matches_actual_emitted_phases(self):
        value = row(group="instrumented")
        lines = ["[native-cpu-profile] inclusive timings; phase-count=35"]
        lines += [f"[native-cpu-profile] phase-{i} calls=1 total-ms=0.1 max-us=1" for i in range(32)]
        lines += ["[native-cpu-profile] vertex-bind-emit calls=70 total-ms=1 max-us=2",
                  "[native-cpu-profile] index-bind-emit calls=20 total-ms=1 max-us=2",
                  "[native-cpu-profile] draw-emit calls=95410 total-ms=1 max-us=2",
                  "[native-cpu-profile] binding-census vertex-attempts=100 vertex-reuses=30 index-attempts=80 index-reuses=60 pipeline-emits=12 viewport-emits=8 scissor-emits=9; draw preparation only"]
        value["stdout"] += "\n" + "\n".join(lines)
        value["metrics"].update(extract(value["stdout"], {"guest": {"session_instructions": 2000000001, "interpreted_instructions": 0}}))
        self.assertEqual(value["metrics"]["binding_census.pipeline_emits"], 12)
        self.assertFalse(errors_for(value, "instrumented"))
        self.assertIn("CPU instrumentation contaminated uninstrumented timing", errors_for(value, "timing"))
        for owner in ("vertex", "index"):
            corrupt = copy.deepcopy(value)
            corrupt["metrics"][f"binding_census.{owner}_reuses"] = 101
            self.assertIn(f"{owner} binding reuses exceed attempts", errors_for(corrupt, "instrumented"))
            corrupt = copy.deepcopy(value)
            corrupt["metrics"][f"cpu_phase.{owner}-bind-emit.calls"] += 1
            self.assertIn(f"{owner} binding emitted calls differ from attempts minus reuses", errors_for(corrupt, "instrumented"))
        value["metrics"]["cpu_phase.draw-emit.calls"] -= 1
        self.assertIn("draw emitted calls differ from accepted draws", errors_for(value, "instrumented"))
        value["metrics"].pop("binding_census.scissor_emits")
        self.assertIn("missing draw-state emission census", errors_for(value, "instrumented"))

    def test_binding_census_required_for_new_phases_not_older_data(self):
        value = row(group="instrumented")
        value["metrics"]["cpu_phase.vertex-bind-emit.calls"] = 1
        self.assertIn("missing vertex binding census/emitted phase", errors_for(value, "instrumented"))
        old = row(group="instrumented")
        old["metrics"].update({f"cpu_phase.phase-{i}.calls": 1 for i in range(21)})
        self.assertFalse(errors_for(old, "instrumented"))
        timing = row()
        timing["metrics"]["binding_census.vertex_attempts"] = 1
        self.assertIn("CPU instrumentation contaminated uninstrumented timing", errors_for(timing, "timing"))


class CounterScopeTests(unittest.TestCase):
    def current(self):
        value = row()
        value['report']['run'].update(native_renderer=True, native_renderer_failed=False,
                                      dx12_raster=False, native_xfb_enabled=False)
        value['report']['guest'] = dict(instruction_counter_scope='session_delta',
            initial_total_retired_instructions=6000000019, session_instructions=2000000001,
            retired_instructions=2000000001, total_retired_instructions=8000000020)
        return value

    def test_restored_total_and_session_delta_are_distinct_valid_scopes(self):
        self.assertEqual(errors_for(self.current(), 'timing'), [])
        self.assertEqual(errors_for(row(), 'timing'), []) # Old evidence has no invented counter scope.

    def test_explicit_native_selection_and_terminal_status_override_legacy_flags(self):
        value = self.current()
        value['report']['run']['native_renderer'] = False
        self.assertIn('explicit native renderer selection is absent', errors_for(value, 'timing'))
        value['report']['run']['native_renderer'] = True
        value['report']['run']['native_renderer_failed'] = True
        self.assertIn('native renderer terminal failure', errors_for(value, 'timing'))

    def test_wrong_scope_or_unreconciled_total_is_not_accepted(self):
        value = self.current()
        value['report']['guest']['instruction_counter_scope'] = 'cumulative'
        self.assertIn('unexpected instruction counter scope', errors_for(value, 'timing'))
        value = self.current()
        value['report']['guest']['total_retired_instructions'] += 1
        self.assertIn('session and cumulative instruction counters do not reconcile', errors_for(value, 'timing'))

    def test_presenter_owner_failure_is_refused_independently_of_error_text(self):
        value = self.current()
        value['report']['run']['native_presenter_failed'] = True
        self.assertIn('native presenter terminal failure', errors_for(value, 'timing'))
        value['report']['run']['native_presenter_failed'] = False
        self.assertEqual(errors_for(value, 'timing'), [])

    def test_partial_negative_bool_or_overflow_counters_are_refused(self):
        for bad in (None, -1, True, 1 << 64, '8000000020'):
            value = self.current()
            value['report']['guest']['total_retired_instructions'] = bad
            self.assertIn('missing/invalid cumulative instruction counters', errors_for(value, 'timing'))


if __name__ == "__main__": unittest.main()
