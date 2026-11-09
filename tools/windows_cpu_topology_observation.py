"""Read-only Windows CPU-set topology; no affinity or scheduling policy changes.

This is current topology, not historical clocks, frequency or migration causality.
The group-less audio sample may be classified only on single-group systems.
"""
import argparse
import ctypes
import hashlib
import json
import os
import struct
import time
from pathlib import Path

MAX_BYTES = 1 << 20


def parse_cpu_sets(raw):
    if not isinstance(raw, bytes) or not 0 < len(raw) <= MAX_BYTES:
        raise ValueError("CPU-set observation must be a nonempty bounded byte record")
    sets, cursor, unknown = [], 0, 0
    identifiers, coordinates = set(), set()
    while cursor < len(raw):
        if len(raw)-cursor < 8:
            raise ValueError("Truncated CPU-set record header")
        size, kind = struct.unpack_from("<II", raw, cursor)
        if size < 8 or size > len(raw)-cursor:
            raise ValueError("Invalid CPU-set record extent")
        if kind == 0:
            # Full current SDK structure includes reserved/scheduling + AllocationTag.
            if size < 32:
                raise ValueError("Truncated current CPU-set structure")
            ident, group, logical, core, cache, numa, efficiency, flags = struct.unpack_from("<IH6B", raw, cursor+8)
            if logical >= 64 or ident in identifiers or (group, logical) in coordinates:
                raise ValueError("Duplicate/invalid CPU identity")
            identifiers.add(ident)
            coordinates.add((group, logical))
            sets.append(dict(id=ident, group=group, logical_processor=logical, core=core,
                             last_level_cache=cache, numa_node=numa, efficiency_class=efficiency,
                             flags=flags))
        else:
            unknown += 1
        cursor += size
    if not sets:
        raise ValueError("No known CPU-set records; refusing invented topology")
    return dict(cpu_sets=sets, unknown_record_types=unknown, raw_bytes=len(raw))


def capture():
    if os.name != "nt":
        raise OSError("Windows CPU-set observation requires Windows")
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    fn = kernel.GetSystemCpuSetInformation
    fn.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint32), ctypes.c_void_p, ctypes.c_uint32]
    fn.restype = ctypes.c_int
    groups = kernel.GetActiveProcessorGroupCount
    groups.restype = ctypes.c_uint16
    required = ctypes.c_uint32()
    ctypes.set_last_error(0)
    first = fn(None, 0, ctypes.byref(required), None, 0)
    error = ctypes.get_last_error()
    if first or error != 122 or not 0 < required.value <= MAX_BYTES:
        raise OSError("CPU-set size query did not produce bounded insufficient-buffer demand")
    buffer = ctypes.create_string_buffer(required.value)
    if not fn(buffer, len(buffer), ctypes.byref(required), None, 0):
        raise ctypes.WinError(ctypes.get_last_error())
    if not 0 < required.value <= len(buffer):
        raise ValueError("CPU-set returned demand exceeds its owned allocation")
    raw = buffer.raw[:required.value]
    result = parse_cpu_sets(raw)
    group_count = groups()
    if group_count < 1 or any(item["group"] >= group_count for item in result["cpu_sets"]):
        raise ValueError("CPU-set groups do not match active group ownership")
    result.update(schema=1, processor_groups=group_count, host_perf_ns=time.perf_counter_ns(),
                  raw_sha256=hashlib.sha256(raw).hexdigest(),
                  current_topology_only=True, historical_frequency_or_causality_proven=False,
                  affinity_or_cpu_policy_changed=False)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    result = capture()
    result["observer_source"] = dict(path=str(Path(__file__).resolve()),
                                     sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
    with args.out.open("x", encoding="utf-8") as stream:
        json.dump(result, stream, indent=2)
        stream.write("\n")
    print(json.dumps({k: result[k] for k in ("processor_groups", "raw_bytes", "unknown_record_types")}))


if __name__ == "__main__":
    main()
