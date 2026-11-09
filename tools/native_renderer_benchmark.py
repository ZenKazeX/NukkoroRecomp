"""Sequential source-matched native renderer benchmarks, with isolated inputs.

Timing, instrumented timing and screenshot verification are separate evidence
groups. No builds/tests may overlap a run. Source inventories are annotations,
never a claim about the binary's build provenance. Default route is GF 2B.
"""
import argparse
import ctypes as C
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import time

from graphics_baseline import copy_new, fingerprint, write_json


NUMBER = r"([0-9]+(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?)"
CPU_PHASE_COUNT = 21


def validate_affinity_mask(mask, available, bits=C.sizeof(C.c_size_t) * 8):
    if not isinstance(mask, int) or mask <= 0 or mask >= 1 << bits:
        raise ValueError("CPU affinity must be a nonzero native-width mask")
    if mask & ~available:
        raise ValueError("CPU affinity requests processors outside this runner's available mask")


def select_process_affinity(mask, api=None):
    """Optional controlled benchmark: child processes inherit the runner mask.

    Only this dedicated runner is changed, before any game starts. Default
    captures retain normal scheduling. This is a measurement constraint,
    never a production resource policy or a claim about all-core performance.
    Windows masks cover the current processor group; never guess another group.
    """
    if mask is None:
        return None
    if os.name != "nt":
        raise ValueError("Explicit CPU affinity currently requires Windows")
    kernel = api or C.WinDLL("kernel32", use_last_error=True)
    kernel.GetCurrentProcess.restype = C.c_void_p
    kernel.GetProcessAffinityMask.argtypes = [C.c_void_p, C.POINTER(C.c_size_t), C.POINTER(C.c_size_t)]
    kernel.GetProcessAffinityMask.restype = C.c_int
    kernel.SetProcessAffinityMask.argtypes = [C.c_void_p, C.c_size_t]
    kernel.SetProcessAffinityMask.restype = C.c_int
    process = kernel.GetCurrentProcess()
    previous, system = C.c_size_t(), C.c_size_t()
    if not kernel.GetProcessAffinityMask(process, C.byref(previous), C.byref(system)):
        raise ValueError("Cannot establish current CPU affinity")
    validate_affinity_mask(mask, previous.value)
    if not kernel.SetProcessAffinityMask(process, mask):
        raise ValueError("Windows rejected the CPU affinity constraint")
    confirmed, current_system = C.c_size_t(), C.c_size_t()
    if not kernel.GetProcessAffinityMask(process, C.byref(confirmed), C.byref(current_system)) or confirmed.value != mask:
        raise ValueError("CPU affinity constraint was not confirmed")
    return dict(requested_mask=hex(mask), previous_mask=hex(previous.value),
                system_mask=hex(system.value), confirmed_mask=hex(confirmed.value),
                child_inheritance=True, current_group_only=True,
                production_scheduling_policy=False)


def extract(stdout, document):
    values = {}
    def read(pattern, names):
        match = re.search(pattern, stdout)
        if match:
            for name, value in zip(names, match.groups()):
                values[name] = float(value) if any(c in value for c in ".eE") else int(value)
    read(r"\[native-gpu\] frames (\d+) draws (\d+) \(immediate (\d+), mesh (\d+), skipped (\d+).*? clears (\d+) copies (\d+)",
         ("frames", "draws", "immediate_draws", "mesh_draws", "skipped_draws", "clears", "copies"))
    read(r"\[native-gpu\] record " + NUMBER + r" ms total \(max " + NUMBER + r" ms/segment\); GPU " + NUMBER + r" ms/frame mean over (\d+).*? VRAM usage " + NUMBER + r" MiB of " + NUMBER + r" MiB budget; screenshots (\d+)",
         ("record_ms", "record_max_ms", "gpu_mean_ms", "gpu_timed_frames", "vram_mib", "vram_budget_mib", "screenshots"))
    read(r"constants uploaded (\d+) reused (\d+) \(identity (\d+), rebuilt-identical (\d+), materialized (\d+)\) upload " + NUMBER + r" MiB ring retained " + NUMBER + r" MiB peak " + NUMBER + r" MiB",
         ("constant_uploads", "constant_reuses", "constant_identity_reuses", "constant_rebuild_reuses", "constant_materializations", "upload_mib", "ring_retained_mib", "ring_peak_mib"))
    read(r"restored constant sources reused=(\d+) byte-checks=(\d+) mismatches=(\d+)",
         ("constant_restored_reuses", "constant_restore_checks", "constant_restore_mismatches"))
    read(r"ring splits (\d+) ring bound failures (\d+) deferred releases (\d+) fence waits (\d+) \(" + NUMBER + r" ms, already complete (\d+)\)",
         ("ring_splits", "ring_bound_failures", "deferred_releases", "fence_waits", "fence_wait_ms", "fence_already_complete"))
    read(r"largest-request " + NUMBER + r" MiB growths (\d+) allocation-failures (\d+) current " + NUMBER + r" MiB resets (\d+) chunk-switches (\d+) ring splits",
         ("ring_largest_request_mib", "ring_growths", "ring_allocation_failures", "ring_current_mib", "ring_resets", "ring_chunk_switches"))
    read(r"fence waits (\d+) \(slot already complete (\d+), FAILED (\d+)\) target allocation failures (\d+)",
         ("frame_fence_waits", "frame_fence_already_complete", "frame_fence_failures", "target_allocation_failures"))
    read(r"copy queue submits (\d+) uploads (\d+) \(" + NUMBER + r" MiB\) graphics waits on copy (\d+) barriers (\d+) \(requested (\d+)\) pso hits (\d+) misses (\d+)",
         ("copy_submits", "copy_uploads", "copy_upload_mib", "graphics_copy_waits", "barriers", "barriers_requested", "pso_hits", "pso_misses"))
    read(r"transfer queue: submits (\d+) \(early (\d+)\) uploads (\d+) \(" + NUMBER + r" MiB, staging " + NUMBER + r" MiB\) retained " + NUMBER + r" MiB peak-batch " + NUMBER + r" MiB.*? growths (\d+) allocation-failures (\d+).*? batch reuse waits (\d+) \(" + NUMBER + r" ms\)",
         ("transfer_submits", "transfer_early", "transfer_uploads", "transfer_mib", "transfer_staging_mib", "transfer_retained_mib", "transfer_peak_mib", "transfer_growths", "transfer_failures", "transfer_waits", "transfer_wait_ms"))
    read(r"\[native-capture\] state calls (\d+) deduplicated (\d+)", ("state_calls", "state_deduplicated"))
    read(r"\[native-capture\] segments (\d+) presents (\d+) commands (\d+) payload (\d+) B",
         ("capture_segments", "capture_presents", "capture_commands", "capture_payload_bytes"))
    for line in stdout.splitlines():
        queue = re.fullmatch(r"\[native-queue-cpu\] ([\w-]+) calls=(\d+) total-ms=" + NUMBER + " max-us=" + NUMBER + r" failures=(\d+) last-hresult=(-?\d+)", line)
        if queue:
            name, count, total, maximum, failures, hresult = queue.groups()
            for key, value in (("calls", int(count)), ("total_ms", float(total)), ("max_us", float(maximum)), ("failures", int(failures)), ("last_hresult", int(hresult))):
                values[f"queue_cpu.{name}.{key}"] = value
        phase = re.fullmatch(r"\[native-cpu-profile\] ([\w-]+) calls=(\d+) total-ms=" + NUMBER + " max-us=" + NUMBER, line)
        if phase:
            name, count, total, maximum = phase.groups()
            values[f"cpu_phase.{name}.calls"] = int(count)
            values[f"cpu_phase.{name}.total_ms"] = float(total)
            values[f"cpu_phase.{name}.max_us"] = float(maximum)
        capture = re.fullmatch(r"\[native-capture-cpu\] ([\w-]+) address=(\d+) calls=(\d+) ticks=(\d+)", line)
        if capture:
            name, address, count, ticks = capture.groups()
            values[f"capture_cpu.{name}.address"] = int(address)
            values[f"capture_cpu.{name}.calls"] = int(count)
            values[f"capture_cpu.{name}.tsc_ticks"] = int(ticks)
        if "graphics upload owners:" in line:
            for owner, count, mib in re.findall(r"([\w-]+)=(\d+)/" + NUMBER + "MiB", line):
                values[f"upload_owner.{owner}.allocations"] = int(count)
                values[f"upload_owner.{owner}.mib"] = float(mib)
        if "transfer owners:" in line:
            for owner, count, mib, staging in re.findall(r"([\w-]+)=(\d+)/" + NUMBER + "/" + NUMBER + "MiB", line):
                values[f"transfer_owner.{owner}.allocations"] = int(count)
                values[f"transfer_owner.{owner}.mib"] = float(mib)
                values[f"transfer_owner.{owner}.staging_mib"] = float(staging)
        if line.startswith("[native-gpu]") and ("generation changes:" in line or "changed state ops:" in line or "exclusive generation owner:" in line):
            group = line.split("] ", 1)[1].split(":", 1)[0]
            for owner, count in re.findall(r"([\w-]+)=(\d+)", line):
                values[f"state_owner.{group}.{owner}"] = int(count)
    values["guest_session_instructions"] = document.get("guest", {}).get("session_instructions")
    values["interpreted_instructions"] = document.get("guest", {}).get("interpreted_instructions")
    values["native_memory"] = document.get("native_memory", {})
    def flatten(prefix, item):
        for key, value in item.items():
            name = f"{prefix}.{key}"
            if isinstance(value, dict): flatten(name, value)
            elif isinstance(value, (int, float)) and not isinstance(value, bool): values[name] = value
    flatten("native_memory", values["native_memory"])
    flatten("host", document.get("host", {}))
    flatten("host_memory", document.get("host_stats", {}).get("memory", {}))
    read(r"clock-pair (\d+) ns/(\d+) pairs", ("profile_clock_pair_ns", "profile_clock_pair_calls"))
    read(r"\[native-cpu-profile\] inclusive[^\n]*phase-count=(\d+)", ("profile_phase_count",))
    read(r"\[native-cpu-profile\] constants-sampling stride=(\d+) materializations=(\d+) samples=(\d+)",
         ("constants_sample_stride", "constants_sample_materializations", "constants_sample_count"))
    read(r"\[native-cpu-profile\] binding-census vertex-attempts=(\d+) vertex-reuses=(\d+) index-attempts=(\d+) index-reuses=(\d+) pipeline-emits=(\d+) viewport-emits=(\d+) scissor-emits=(\d+)",
         ("binding_census.vertex_attempts", "binding_census.vertex_reuses", "binding_census.index_attempts", "binding_census.index_reuses",
          "binding_census.pipeline_emits", "binding_census.viewport_emits", "binding_census.scissor_emits"))
    read(r"perspective raw-key reuse hits=(\d+) misses=(\d+) non-neutral-policy=(\d+)",
         ("perspective_raw_key_hits", "perspective_raw_key_misses", "perspective_non_neutral_policy"))
    return values


WORKLOAD = ("guest_session_instructions", "frames", "draws", "immediate_draws", "mesh_draws", "clears", "copies")


def screenshot_frames(first, stride, count, total):
    if total <= 0 or first < 1 or first > total or count < 1 or count > total or stride < 0:
        raise ValueError("screenshot first/count must be positive and bounded by expected frames")
    if stride == 0 and count != 1:
        raise ValueError("multiple screenshots require a positive stride")
    frames = [first + i * stride for i in range(count)]
    if frames[-1] > total:
        raise ValueError("screenshot range exceeds expected frames")
    return frames


def screenshot_manifest(directory, frames, stride, dimensions):
    """Read only the native P6 writer's bounded format; hash pixels in chunks."""
    width, height = dimensions
    maximum_file_bytes = width * height * 3 + 128
    result = dict(expected_frames=frames, files=[], errors=[], total_bytes=0,
                  maximum_bytes=len(frames) * maximum_file_bytes)
    seen = set()
    for ordinal, path in enumerate(directory.glob("final*.ppm")):
        if ordinal >= len(frames):
            result["errors"].append("screenshot file count exceeds requested bound")
            break
        match = re.fullmatch(r"final_(\d+)\.ppm", path.name) if stride else None
        frame = int(match.group(1)) if match else frames[0] if not stride and path.name == "final.ppm" else None
        if frame in seen: result["errors"].append(f"duplicate screenshot frame {frame}")
        seen.add(frame)
        expected_name = f"final_{frame:06d}.ppm" if frame is not None and stride else "final.ppm"
        if frame not in frames or path.name != expected_name:
            result["errors"].append(f"unexpected screenshot file {path.name}")
            continue
        if len(result["files"]) >= len(frames) or path.stat().st_size > maximum_file_bytes:
            result["errors"].append(f"screenshot output exceeds bounded demand: {path.name}")
            continue
        try:
            with path.open("rb") as stream:
                header = b"".join(stream.readline(64) for _ in range(3))
                expected_header = f"P6\n{width} {height}\n255\n".encode("ascii")
                if header != expected_header: raise ValueError("invalid native P6 header/dimensions")
                digest, pixel_bytes = hashlib.sha256(), 0
                for block in iter(lambda: stream.read(1024 * 1024), b""):
                    pixel_bytes += len(block)
                    if pixel_bytes > width * height * 3: raise ValueError("extra native pixel bytes")
                    digest.update(block)
                if pixel_bytes != width * height * 3: raise ValueError("truncated/extra native pixel bytes")
            entry = dict(frame=frame, width=width, height=height, pixel_bytes=pixel_bytes,
                         pixel_sha256=digest.hexdigest(), **fingerprint(path))
            result["files"].append(entry)
            result["total_bytes"] += entry["size"]
        except (OSError, ValueError) as error:
            result["errors"].append(f"invalid screenshot {path.name}: {error}")
    coverage = [entry["frame"] for entry in result["files"]]
    if len(coverage) != len(frames) or set(coverage) != set(frames): result["errors"].append("incomplete screenshot frame coverage")
    result["files"].sort(key=lambda entry: entry["frame"])
    return result


def manifest_signature(manifest):
    return sorted((entry["frame"], entry["width"], entry["height"], entry["pixel_bytes"], entry["pixel_sha256"], entry["sha256"])
                  for entry in manifest.get("files", []))


def errors_for(row, group, *, evidence_role="production"):
    values, stdout, report = row["metrics"], row["stdout"], row["report"]
    errors = []
    if row["exit_code"] != 0: errors.append("process failure")
    if report.get("stop", {}).get("failed") or report.get("stop", {}).get("reason") != 0: errors.append("run did not reach instruction budget")
    run = report.get("run", {})
    if "native_renderer" in run and run["native_renderer"] is not True:
        errors.append("explicit native renderer selection is absent")
    if run.get("native_renderer_failed", False) is not False:
        errors.append("native renderer terminal failure")
    if run.get("native_presenter_failed", False) is not False:
        errors.append("native presenter terminal failure")
    guest = report.get("guest", {})
    if "instruction_counter_scope" in guest:
        counters = [guest.get(name) for name in
                    ("initial_total_retired_instructions", "session_instructions", "total_retired_instructions")]
        if guest["instruction_counter_scope"] != "session_delta":
            errors.append("unexpected instruction counter scope")
        if any(type(value) is not int or not 0 <= value < (1 << 64) for value in counters):
            errors.append("missing/invalid cumulative instruction counters")
        elif counters[0] + counters[1] != counters[2] or guest.get("retired_instructions") != counters[1]:
            errors.append("session and cumulative instruction counters do not reconcile")
    if any(values.get(name) is None for name in WORKLOAD): errors.append("missing workload counters")
    if values.get("interpreted_instructions") != 0: errors.append("non-native execution or absent counter")
    if values.get("draws", 0) <= 0 or values.get("record_ms", 0) <= 0: errors.append("no live renderer work")
    if any(values.get(name, 0) for name in ("skipped_draws", "ring_bound_failures", "transfer_failures", "ring_allocation_failures", "frame_fence_failures", "target_allocation_failures")): errors.append("renderer refused work/resources")
    if values.get("native_memory.total.failures", 0) or values.get("native_memory.invalid_releases", 0) or values.get("native_memory.rejected_mappings", 0): errors.append("native memory ownership/failure gate")
    if values.get("constant_restore_mismatches", 0): errors.append("restored constants failed byte equivalence")
    if any(value for name, value in values.items() if name.startswith("queue_cpu.") and name.endswith(".failures")): errors.append("native queue submission failure")
    if row.get("environment", {}).get("GEKKO_NATIVE_CONSTANT_VALIDATE") == "1" and values.get("constant_restore_checks", 0) <= 0: errors.append("no restored constant byte checks executed")
    if "SILENT DROPS: none" not in stdout or "RESOURCE REJECTIONS: none" not in stdout: errors.append("missing/failed renderer correctness gate")
    if re.search(r"\[gameplay-scheduler\].*?faults [1-9]", stdout): errors.append("scheduler fault")
    if "[native-gpu] last error:" in stdout: errors.append("native renderer error")
    if group != "verification" and values.get("screenshots") != 0: errors.append("screenshot contaminated timing")
    phases = sum(name.startswith("cpu_phase.") and name.endswith(".calls") for name in values)
    if group == "instrumented" and phases != values.get("profile_phase_count", CPU_PHASE_COUNT): errors.append("missing complete CPU phase instrumentation")
    binding_present = any(name.startswith("binding_census.") for name in values)
    emit_phases = any(name in values for name in ("cpu_phase.vertex-bind-emit.calls", "cpu_phase.index-bind-emit.calls", "cpu_phase.draw-emit.calls"))
    if group == "instrumented" and (binding_present or emit_phases or "[native-cpu-profile] binding-census" in stdout):
        for owner in ("vertex", "index"):
            attempts, reuses = values.get(f"binding_census.{owner}_attempts"), values.get(f"binding_census.{owner}_reuses")
            emitted = values.get(f"cpu_phase.{owner}-bind-emit.calls")
            if attempts is None or reuses is None or emitted is None: errors.append(f"missing {owner} binding census/emitted phase")
            elif attempts < reuses: errors.append(f"{owner} binding reuses exceed attempts")
            elif emitted != attempts - reuses: errors.append(f"{owner} binding emitted calls differ from attempts minus reuses")
        if any(values.get(f"binding_census.{owner}_emits") is None for owner in ("pipeline", "viewport", "scissor")): errors.append("missing draw-state emission census")
        if values.get("cpu_phase.draw-emit.calls") != values.get("draws"): errors.append("draw emitted calls differ from accepted draws")
    if group == "timing" and (phases or binding_present or "[native-cpu-profile]" in stdout or any(name.startswith("queue_cpu.") for name in values)): errors.append("CPU instrumentation contaminated uninstrumented timing")
    if group == "verification" and not row.get("screenshot", {}).get("sha256"): errors.append("missing screenshot")
    if group == "verification" and "screenshots" in row:
        manifest = row["screenshots"]
        errors.extend(manifest.get("errors", []))
        if values.get("screenshots") != len(manifest["expected_frames"]): errors.append("reported screenshot count differs from requested coverage")
    if evidence_role == "production":
        if report.get("run", {}).get("reference_execution") is not False:
            errors.append("not positively identified as production executable")
    elif evidence_role == "reference_control":
        if group != "verification":
            errors.append("reference control is verification-only, never shipping timing evidence")
        if report.get("run", {}).get("reference_execution") is not True:
            errors.append("not positively identified as reference executable")
    else:
        errors.append("unknown runtime evidence role")
    if report.get("audio", {}).get("active") is not False: errors.append("audio enabled or unknown")
    if report.get("run", {}).get("frame_capture") or report.get("run", {}).get("filmstrip") or report.get("run", {}).get("present_hash"): errors.append("intrusive capture path enabled")
    expected = row.get("expected", {})
    for name in ("frames", "draws"):
        if expected.get(name) is not None and values.get(name) != expected[name]: errors.append(f"unexpected {name}")
    if report.get("run", {}).get("budget") != expected.get("budget", report.get("run", {}).get("budget")): errors.append("wrong requested budget")
    if report.get("renderer", {}).get("internal_scale") != expected.get("internal_scale", report.get("renderer", {}).get("internal_scale")): errors.append("wrong internal scale")
    return errors


def process_times(pid):
    """Creation identity and cumulative CPU ticks; unknown means not ignorable."""
    kernel = C.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.argtypes = [C.c_uint32, C.c_int, C.c_uint32]
    kernel.OpenProcess.restype = C.c_void_p
    kernel.GetProcessTimes.argtypes = [C.c_void_p] + [C.POINTER(C.c_uint64)] * 4
    kernel.CloseHandle.argtypes = [C.c_void_p]
    handle = kernel.OpenProcess(0x1000, 0, pid)
    if not handle: return None
    try:
        created, ended, system, user = (C.c_uint64() for _ in range(4))
        if not kernel.GetProcessTimes(handle, C.byref(created), C.byref(ended), C.byref(system), C.byref(user)): return None
        return created.value, system.value + user.value
    finally: kernel.CloseHandle(handle)


def idle_msbuild_proof(document, pids):
    """Explicit caller authorization still requires command/parent/stable-CPU proof."""
    if document.get("active_root_build") is not False or document.get("sample_interval_seconds", 0) < 1: return False
    entries = {entry.get("pid"): entry for entry in document.get("processes", [])}
    for pid in pids:
        entry = entries.get(pid, {})
        command = entry.get("command_line", "").lower()
        before, after = entry.get("cpu_seconds_before"), entry.get("cpu_seconds_after")
        if "/nodemode:1" not in command or "/nodereuse:true" not in command: return False
        if entry.get("parent_build_absent") is not True or before is None or before != after: return False
    return bool(pids)


def blockers(idle_workers=None):
    if os.name != "nt": return []
    class Entry(C.Structure):
        _fields_ = [("size", C.c_uint32), ("usage", C.c_uint32), ("pid", C.c_uint32),
                    ("heap", C.c_size_t), ("module", C.c_uint32), ("threads", C.c_uint32),
                    ("parent", C.c_uint32), ("priority", C.c_int32), ("flags", C.c_uint32),
                    ("name", C.c_wchar * 260)]
    kernel = C.WinDLL("kernel32", use_last_error=True)
    kernel.CreateToolhelp32Snapshot.argtypes = [C.c_uint32, C.c_uint32]
    kernel.CreateToolhelp32Snapshot.restype = C.c_void_p
    kernel.Process32FirstW.argtypes = kernel.Process32NextW.argtypes = [C.c_void_p, C.POINTER(Entry)]
    kernel.CloseHandle.argtypes = [C.c_void_p]
    handle = kernel.CreateToolhelp32Snapshot(2, 0)
    if handle == C.c_void_p(-1).value: raise C.WinError(C.get_last_error())
    found = []
    try:
        entry = Entry()
        entry.size = C.sizeof(entry)
        present = kernel.Process32FirstW(handle, C.byref(entry))
        while present:
            name = entry.name.lower()
            if name in ("cmake.exe", "msbuild.exe", "ctest.exe", "cl.exe", "link.exe", "shadowthehedgehog.exe", "shadowthehedgehogreference.exe") or (name.startswith("shadowthehedgehog") and name.endswith(".exe")) or name.endswith("_tests.exe"):
                proven_idle = name == "msbuild.exe" and idle_workers and entry.pid in idle_workers and process_times(entry.pid) == idle_workers[entry.pid]
                if not proven_idle: found.append((entry.pid, name))
            present = kernel.Process32NextW(handle, C.byref(entry))
    finally:
        kernel.CloseHandle(handle)
    return found


def summarize(rows):
    errors, workload = [], None
    for row in rows:
        errors.extend(f"{row['arm']}/{row['repetition']}: {e}" for e in row["errors"])
        current = {name: row["metrics"].get(name) for name in WORKLOAD}
        if workload is None: workload = current
        elif current != workload: errors.append(f"{row['arm']}/{row['repetition']}: exact workload mismatch")
    groups = {row["group"] for row in rows}
    if len(groups) != 1: errors.append("instrumented/timing/verification groups cannot be combined")
    fixtures = {json.dumps(row.get("fixture", {}), sort_keys=True) for row in rows}
    if len(fixtures) != 1: errors.append("checkpoint/config/persistence/settings mismatch")
    for arm in {row["arm"] for row in rows}:
        if len({row["snapshots"]["executable"]["sha256"] for row in rows if row["arm"] == arm}) != 1: errors.append(f"{arm}: binary changed")
    arms = sorted({row["arm"] for row in rows})
    repetitions = {arm: {row["repetition"] for row in rows if row["arm"] == arm} for arm in arms}
    if len(arms) > 1 and len({tuple(sorted(items)) for items in repetitions.values()}) != 1: errors.append("incomplete control/candidate repetition pairs")
    if len({(row["arm"], row["repetition"]) for row in rows}) != len(rows): errors.append("duplicate arm/repetition")
    result = dict(accepted=not errors, errors=errors, workload=workload, group=next(iter(groups), None), metrics={})
    binary_hashes = {arm: sorted({row["snapshots"]["executable"]["sha256"] for row in rows
                                 if row["arm"] == arm}) for arm in arms}
    valid_hashes = all(len(values) == 1 and re.fullmatch(r"[0-9a-fA-F]{64}", str(values[0]))
                       for values in binary_hashes.values())
    if valid_hashes and set(arms) == {"control", "candidate"}:
        kind = ("identical-binary-calibration" if binary_hashes["control"][0].lower() ==
                binary_hashes["candidate"][0].lower() else "different-binary-observation")
    else:
        kind = "single-arm-or-unproven-binary-identity"
    result["comparison"] = dict(kind=kind, binary_hashes=binary_hashes,
        performance_gain_proven=False, calibration_bias_correction_applied=False,
        interpretation="Acceptance certifies the captured workload/fixture gates, not a performance gain. "
                       "Identical-binary deltas measure instability; never attribute them to a source change or "
                       "subtract them to manufacture gains. Different binaries still need stable calibration, "
                       "source/build provenance and independent correctness gates.")
    numeric = sorted({key for row in rows for key, value in row["metrics"].items()
                      if isinstance(value, (int, float)) and key not in WORKLOAD})
    for key in numeric:
        entries = {}
        for arm in arms:
            data = [row["metrics"][key] for row in rows if row["arm"] == arm and key in row["metrics"]]
            if data:
                median = statistics.median(data)
                entries[arm] = dict(n=len(data), median=median, minimum=min(data), maximum=max(data),
                                    median_absolute_deviation=statistics.median(abs(value-median) for value in data))
        if len(entries) == 2 and "control" in entries and "candidate" in entries:
            delta = entries["candidate"]["median"] - entries["control"]["median"]
            entries["candidate_minus_control"] = delta
            before = entries["control"]["median"]
            entries["percent"] = 100*delta/before if before else None
        result["metrics"][key] = entries
        if "control" in entries and "candidate" in entries:
            paired = []
            for repetition in sorted({row["repetition"] for row in rows}):
                pair = {row["arm"]: row["metrics"].get(key) for row in rows if row["repetition"] == repetition}
                if pair.get("control") is not None and pair.get("candidate") is not None:
                    paired.append(pair["candidate"] - pair["control"])
            entries["paired_deltas"] = paired
            entries["paired_median_delta"] = statistics.median(paired) if paired else None
            entries["paired_sign_counts"] = dict(negative=sum(value < 0 for value in paired),
                                                   zero=sum(value == 0 for value in paired),
                                                   positive=sum(value > 0 for value in paired))
            orders = {"control-first": [], "candidate-first": [], "unrecorded": []}
            for repetition in sorted({row["repetition"] for row in rows}):
                pair = {row["arm"]: row for row in rows if row["repetition"] == repetition}
                if not all(arm in pair and key in pair[arm]["metrics"] for arm in ("control", "candidate")):
                    continue
                first, second = pair["control"].get("capture_sequence"), pair["candidate"].get("capture_sequence")
                order = ("control-first" if first < second else "candidate-first") if (
                    type(first) is int and type(second) is int and first != second) else "unrecorded"
                orders[order].append(pair["candidate"]["metrics"][key] - pair["control"]["metrics"][key])
            entries["paired_capture_order"] = {order: dict(n=len(values), deltas=values,
                median_delta=statistics.median(values) if values else None) for order, values in orders.items()}
    def ranking(prefix, suffix):
        return sorted([dict(owner=key, arms=data) for key, data in result["metrics"].items()
                       if key.startswith(prefix) and key.endswith(suffix)],
                      key=lambda entry: max((item.get("median", 0) for item in entry["arms"].values() if isinstance(item, dict)), default=0), reverse=True)
    result["allocation_owner_ranking"] = ranking("upload_owner.", ".allocations")
    result["native_allocation_ranking"] = ranking("native_memory.domains.", ".allocations")
    result["upload_byte_ranking"] = ranking("upload_owner.", ".mib")
    result["inclusive_cpu_phase_ranking"] = [entry for entry in ranking("cpu_phase.", ".total_ms") if not entry["owner"].startswith("cpu_phase.sample-")]
    result["sampled_constants_fragment_ranking"] = ranking("cpu_phase.sample-constants-", ".total_ms")
    result["inclusive_queue_stage_ranking"] = ranking("queue_cpu.", ".total_ms")
    result["raw_capture_tsc_ranking"] = ranking("capture_cpu.", ".tsc_ticks")
    result["profile_interpretation"] = "CPU phases are inclusive and nested; do not sum them, infer exclusive costs, or subtract clock-pair calibration. Sampled constants fragments are ranked separately: totals cover only sampled calls, must not be extrapolated, and short fragments can be dominated by timer overhead. Capture owner ticks are raw TSC, not ns or GPU time. Profile comparisons stay separate from uninstrumented timing."
    if groups == {"verification"}:
        if any("screenshots" in row for row in rows):
            manifests = [row.get("screenshots") for row in rows]
            if any(not item or item.get("errors") or not item.get("files") for item in manifests):
                result["errors"].append("Screenshot manifests incomplete or missing")
            else:
                expected = manifests[0]["expected_frames"]
                if any(item["expected_frames"] != expected or [entry[0] for entry in manifest_signature(item)] != sorted(expected) for item in manifests):
                    result["errors"].append("Screenshot manifest frame coverage differs or is incomplete")
                if any(not re.fullmatch(r"[0-9a-f]{64}", str(entry.get(key))) for item in manifests for entry in item["files"] for key in ("sha256", "pixel_sha256")):
                    result["errors"].append("Screenshot fingerprint missing or malformed")
                if any(manifest_signature(item) != manifest_signature(manifests[0]) for item in manifests[1:]):
                    result["errors"].append("Per-frame native pixel bytes/hashes differ")
                result["verified_screenshot_frames"] = expected
                result["native_pixel_manifest"] = manifests[0]["files"]
        else:
            images = {row.get("screenshot", {}).get("sha256") for row in rows}
            if len(images) != 1 or None in images: result["errors"].append("Screenshot hashes differ or are missing")
    result["accepted"] = not result["errors"]
    return result


def run_one(args, arm, repetition, source, directory, environment):
    directory.mkdir()
    snapshots = {}
    for name, path, target in (("executable", source, "ShadowTheHedgehog.exe"),
                              ("checkpoint", args.state, "input.gekkostate"),
                              ("display", args.display_cfg, "display.cfg"),
                              ("card", args.card, "working-card.raw")):
        snapshots[name] = copy_new(path, directory / target, fingerprint(path))
    (directory / "save-data").mkdir()
    for name in ("SRAM.pc.raw", "disc-read-profile.txt"):
        path = args.seed_dir / name
        if path.is_file(): snapshots[name] = copy_new(path, directory / "save-data" / name, fingerprint(path))
        else: snapshots[name] = {"absent": True}
    argv = [str(directory / "ShadowTheHedgehog.exe"), "--game-data", str(args.game_data),
            "--headless" if args.presentation == "headless" else "--windowed",
            "--native-renderer", "--state-file", str(directory / "input.gekkostate"), "--load-state",
            "--budget", str(args.budget), "--no-audio",
            "--internal-scale", str(args.internal_scale), "--native-output", args.output,
            "--aspect", args.aspect, "--width", str(args.width), "--height", str(args.height),
            "--display-cfg", str(directory / "display.cfg"), "--memory-card", str(directory / "working-card.raw"),
            "--json-report", str(directory / "session.json")]
    if args.group == "verification":
        argv += ["--native-screenshot", str(directory / "final.ppm"), "--native-screenshot-frame", str(args.screenshot_frame),
                 "--native-screenshot-stride", str(args.screenshot_stride), "--native-screenshot-count", str(args.screenshot_count)]
    row = dict(arm=arm, repetition=repetition, group=args.group, capture_sequence=args.capture_sequence,
               argv=argv, snapshots=snapshots,
               fixture=args.fixture, expected=dict(frames=args.expect_frames, draws=args.expect_draws, budget=args.budget, internal_scale=args.internal_scale),
               environment={key: value for key, value in environment.items() if key.startswith("GEKKO_")})
    write_json(directory / "invocation.json", row)
    active = blockers(args.idle_workers)
    if active: raise ValueError(f"Other build/test/game processes active: {active}")
    with (directory / "stdout.log").open("xb") as out, (directory / "stderr.log").open("xb") as err:
        process = subprocess.Popen(argv, cwd=directory, env=environment, stdin=subprocess.DEVNULL, stdout=out, stderr=err)
        deadline = time.monotonic() + args.timeout
        aborted = None
        while process.poll() is None:
            collision = [item for item in blockers(args.idle_workers) if item[0] != process.pid]
            if collision or time.monotonic() >= deadline:
                process.kill()
                process.wait()
                aborted = f"Run aborted: {'overlapping workload ' + str(collision) if collision else 'timeout'}"
                break
            try: process.wait(timeout=.2)
            except subprocess.TimeoutExpired: pass
    row["exit_code"] = process.returncode
    row["stdout"] = (directory / "stdout.log").read_text(encoding="utf-8", errors="replace")
    try: row["report"] = json.loads((directory / "session.json").read_text(encoding="utf-8"))
    except (OSError, ValueError): row["report"] = {}
    row["metrics"] = extract(row["stdout"], row["report"])
    if args.group == "verification":
        row["screenshots"] = screenshot_manifest(directory, args.screenshot_frames, args.screenshot_stride, tuple(map(int, args.screenshot_dimensions.split("x"))))
        row["screenshot"] = row["screenshots"]["files"][0] if row["screenshots"]["files"] else dict(sha256=None)
    row["errors"] = errors_for(row, args.group)
    if aborted: row["errors"].append(aborted)
    if args.golden and row.get("screenshot", {}).get("sha256") != fingerprint(args.golden)["sha256"]: row["errors"].append("Screenshot differs from pinned golden")
    row["outputs"] = {name: fingerprint(directory / name) for name in ("stdout.log", "stderr.log", "session.json", "working-card.raw", "save-data/SRAM.pc.raw", "save-data/disc-read-profile.txt")}
    write_json(directory / "result.json", row)
    return row


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--control", type=Path)
    parser.add_argument("--candidate", type=Path)
    parser.add_argument("--state", type=Path)
    parser.add_argument("--game-data", type=Path)
    parser.add_argument("--display-cfg", type=Path)
    parser.add_argument("--card", type=Path)
    parser.add_argument("--seed-dir", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--group", choices=("timing", "instrumented", "verification"), default="timing")
    parser.add_argument("--presentation", choices=("headless", "windowed"), default="headless")
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--budget", type=int, default=2000000000)
    parser.add_argument("--timeout", type=float, default=180)
    parser.add_argument("--internal-scale", type=int, choices=range(1, 7), default=1)
    parser.add_argument("--output", default="640x480")
    parser.add_argument("--aspect", default="auto", help="Accepted GF fixture uses auto; explicit 4:3 selects a different camera policy")
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--height", type=int, default=720)
    parser.add_argument("--expect-frames", type=int, default=247, help="Accepted GF workload; explicitly override for another route")
    parser.add_argument("--expect-draws", type=int, default=95410)
    parser.add_argument("--screenshot-frame", type=int, default=200, help="Verification only; historical GF 7D49 golden is frame200")
    parser.add_argument("--screenshot-stride", type=int, default=0, help="Verification only; positive stride produces final_NNNNNN.ppm")
    parser.add_argument("--screenshot-count", type=int, default=1, help="Verification only; bounded by expected frame coverage")
    parser.add_argument("--screenshot-dimensions", help="Native capture route contract, independent of --output; defaults to GF guest 640x480 multiplied by internal scale. Explicit WIDTHxHEIGHT for other routes")
    parser.add_argument("--golden", type=Path, help="Optional exact native screenshot fixture, verification only")
    parser.add_argument("--ignore-idle-msbuild-pids", type=int, nargs="+", default=[], help="Explicit verified orphan worker PIDs only; activity/identity changes still block")
    parser.add_argument("--idle-msbuild-evidence", type=Path, help="Required proof JSON for explicitly authorized idle workers; see idle_msbuild_proof schema")
    parser.add_argument("--env", action="append", default=[])
    parser.add_argument("--source-inventory", type=Path, help="Optional annotation, not a build manifest")
    parser.add_argument("--cpu-affinity", type=lambda text: int(text, 0),
                        help="Optional Windows runner/child mask for controlled paired captures; recorded separately from normal scheduling")
    parser.add_argument("--blind-run-names", action="store_true",
                        help="Use equal-length ordinal run paths that do not expose the arm to the game/host; roles remain in reports")
    parser.add_argument("--summarize", type=Path, nargs="+", help="Offline result.json paths; launches no game")
    args = parser.parse_args()
    if args.summarize:
        rows = [json.loads(path.read_text(encoding="utf-8")) for path in args.summarize]
        report = summarize(rows)
        write_json(args.out, report)
        if not report["accepted"]: raise SystemExit(1)
        return
    if not 1 <= args.runs <= 10 or not 0 < args.timeout <= 600 or args.budget <= 0: parser.error("runs 1..10, timeout <=600, budget >0 required")
    if args.expect_frames <= 0 or args.expect_draws <= 0: parser.error("positive expected workload required")
    try: args.screenshot_frames = screenshot_frames(args.screenshot_frame, args.screenshot_stride, args.screenshot_count, args.expect_frames) if args.group == "verification" else []
    except ValueError as error: parser.error(str(error))
    if not re.fullmatch(r"[1-9][0-9]*x[1-9][0-9]*", args.output) or max(map(int, args.output.split("x"))) > 16384: parser.error("output dimensions 1..16384 required")
    args.screenshot_dimensions = args.screenshot_dimensions or f"{640 * args.internal_scale}x{480 * args.internal_scale}"
    if not re.fullmatch(r"[1-9][0-9]*x[1-9][0-9]*", args.screenshot_dimensions) or max(map(int, args.screenshot_dimensions.split("x"))) > 16384: parser.error("screenshot route dimensions 1..16384 required")
    if args.golden and args.group != "verification": parser.error("--golden requires verification group")
    if args.golden and args.screenshot_count != 1: parser.error("--golden is a single-frame fixture; complete routes compare paired manifests")
    if args.group != "verification" and (args.screenshot_stride != 0 or args.screenshot_count != 1): parser.error("screenshot sequence options require verification group")
    args.idle_workers = {}
    idle_evidence = None
    if args.ignore_idle_msbuild_pids:
        if os.name != "nt" or not args.idle_msbuild_evidence: parser.error("Idle worker authorization requires Windows and --idle-msbuild-evidence")
        args.idle_msbuild_evidence = args.idle_msbuild_evidence.resolve()
        proof = json.loads(args.idle_msbuild_evidence.read_text(encoding="utf-8-sig"))
        if not idle_msbuild_proof(proof, args.ignore_idle_msbuild_pids): parser.error("Idle-worker proof is incomplete/unstable/active")
        args.idle_workers = {pid: process_times(pid) for pid in args.ignore_idle_msbuild_pids}
        if any(value is None for value in args.idle_workers.values()): parser.error("Cannot verify current idle-worker identity/CPU time")
        idle_evidence = dict(proof=fingerprint(args.idle_msbuild_evidence), authorized_pids=args.ignore_idle_msbuild_pids,
                             process_creation_and_cpu_ticks=args.idle_workers)
    for name in ("control", "state", "game_data", "display_cfg", "card", "seed_dir"):
        if getattr(args, name) is None: parser.error(f"--{name.replace('_', '-')} must be explicitly pinned")
        setattr(args, name, getattr(args, name).resolve())
    if args.candidate: args.candidate = args.candidate.resolve()
    if args.golden: args.golden = args.golden.resolve()
    args.out = args.out.resolve()
    args.out.mkdir(exist_ok=False, parents=True)
    if args.group == "verification":
        width, height = map(int, args.screenshot_dimensions.split("x"))
        required_bytes = args.screenshot_count * (width * height * 3 + 128) * args.runs * (2 if args.candidate else 1)
        if required_bytes > shutil.disk_usage(args.out).free: parser.error("native screenshot demand exceeds available disk capacity")
    environment = {key: value for key, value in os.environ.items() if not key.startswith("GEKKO_")}
    for item in args.env:
        name, sep, value = item.partition("=")
        valid = (name == "GEKKO_NATIVE_CPU_PROFILE" and args.group == "instrumented") or (name == "GEKKO_NATIVE_CONSTANT_VALIDATE" and args.group == "verification")
        if not sep or not valid or value != "1": parser.error("Only CPU_PROFILE=1 in instrumented or CONSTANT_VALIDATE=1 in verification is accepted")
        environment[name] = value
    if args.group == "instrumented" and not args.env: environment["GEKKO_NATIVE_CPU_PROFILE"] = "1"
    pinned = {name: fingerprint(getattr(args, name)) for name in ("control", "state", "display_cfg", "card")}
    if args.candidate: pinned["candidate"] = fingerprint(args.candidate)
    if args.golden: pinned["golden"] = fingerprint(args.golden)
    for name in ("SRAM.pc.raw", "disc-read-profile.txt"):
        pinned[name] = fingerprint(args.seed_dir / name)
    pinned["external_dol"] = fingerprint(args.game_data / "sys" / "main.dol")
    required = ("control", "state", "display_cfg", "card", "external_dol") + (("candidate",) if args.candidate else ()) + (("golden",) if args.golden else ())
    if any(not pinned[name]["sha256"] for name in required): parser.error("Pinned executable/fixture/DOL file is missing")
    args.fixture = {name: dict(size=entry["size"], sha256=entry["sha256"]) for name, entry in pinned.items() if name not in ("control", "candidate")}
    args.fixture["settings"] = dict(game_data=str(args.game_data), group=args.group, presentation=args.presentation,
                                   budget=args.budget, internal_scale=args.internal_scale, output=args.output, aspect=args.aspect,
                                   width=args.width, height=args.height, screenshot_frame=args.screenshot_frame if args.group == "verification" else None,
                                   screenshot_stride=args.screenshot_stride if args.group == "verification" else None,
                                   screenshot_count=args.screenshot_count if args.group == "verification" else None,
                                   screenshot_dimensions=args.screenshot_dimensions if args.group == "verification" else None)
    args.fixture["observers"] = {key: value for key, value in environment.items() if key.startswith("GEKKO_")}
    try:
        affinity = select_process_affinity(args.cpu_affinity)
    except ValueError as error:
        parser.error(str(error))
    if affinity is not None:
        args.fixture["cpu_affinity"] = affinity
    if args.blind_run_names:
        args.fixture["run_layout"] = "equal-length-role-blind-ordinals"
    write_json(args.out / "provenance.json", dict(inputs=pinned, group=args.group,
               source_inventory=fingerprint(args.source_inventory) if args.source_inventory else None,
               fixture=args.fixture, source_inventory_is_build_manifest=False,
               idle_msbuild_authorization=idle_evidence,
               note="No comparison against older screenshot-contaminated timing datasets. Asset tree is read-only but not fully inventoried. Process guard scans every 200 ms in both arms."))
    def verify_pins():
        for name, expected in pinned.items():
            if fingerprint(Path(expected["path"]))["sha256"] != expected["sha256"]: raise ValueError(f"Pinned input changed: {name}")
    rows = []
    for repetition in range(args.runs):
        arms = [("control", args.control)] + ([("candidate", args.candidate)] if args.candidate else [])
        if repetition & 1: arms.reverse()
        for arm, source in arms:
            verify_pins()
            args.capture_sequence = len(rows)
            run_name = f"run-{len(rows):06d}" if args.blind_run_names else f"{repetition:02d}-{arm}"
            row = run_one(args, arm, repetition, source, args.out / run_name, environment)
            verify_pins()
            rows.append(row)
            print(f"{arm}/{repetition}: record={row['metrics'].get('record_ms')}ms errors={row['errors']}", flush=True)
            if row["errors"]: break
        if rows[-1]["errors"]: break
    result = summarize(rows)
    write_json(args.out / "summary.json", result)
    if not result["accepted"]: raise SystemExit(1)


if __name__ == "__main__": main()
