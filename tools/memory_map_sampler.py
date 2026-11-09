"""Attribute a game process's private/committed memory by region kind over time.

Launches an OWNED child (argv after --), polls its commit counters every
--interval-ms and, on each new private-bytes maximum (rate limited) and at the
end, walks the address space with VirtualQueryEx. Regions are grouped by type
(image/mapped/private), protection and allocation base, and images by module.
This is a diagnostic: polling and walks cost CPU, so never use its run as a
benchmark. No existing process can be attached and nothing in the child is
modified. Exact counters from the OS; grouping heuristics are labelled.
"""
from __future__ import annotations

import argparse
import ctypes as ct
from ctypes import wintypes as wt
from collections import defaultdict
import json
import os
from pathlib import Path
import subprocess
import time

k32 = ct.WinDLL("kernel32", use_last_error=True)
psapi = ct.WinDLL("psapi", use_last_error=True)

MEM_COMMIT, MEM_RESERVE, MEM_FREE = 0x1000, 0x2000, 0x10000
MEM_IMAGE, MEM_MAPPED, MEM_PRIVATE = 0x1000000, 0x40000, 0x20000
PROCESS_QUERY_INFORMATION, PROCESS_VM_READ = 0x400, 0x10


class MBI(ct.Structure):
    _fields_ = [("BaseAddress", ct.c_void_p), ("AllocationBase", ct.c_void_p), ("AllocationProtect", wt.DWORD),
                ("PartitionId", wt.WORD), ("RegionSize", ct.c_size_t), ("State", wt.DWORD), ("Protect", wt.DWORD),
                ("Type", wt.DWORD)]


class PMC(ct.Structure):
    _fields_ = [("cb", wt.DWORD), ("PageFaultCount", wt.DWORD), ("PeakWorkingSetSize", ct.c_size_t),
                ("WorkingSetSize", ct.c_size_t), ("QuotaPeakPagedPoolUsage", ct.c_size_t),
                ("QuotaPagedPoolUsage", ct.c_size_t), ("QuotaPeakNonPagedPoolUsage", ct.c_size_t),
                ("QuotaNonPagedPoolUsage", ct.c_size_t), ("PagefileUsage", ct.c_size_t),
                ("PeakPagefileUsage", ct.c_size_t), ("PrivateUsage", ct.c_size_t)]


k32.OpenProcess.restype = wt.HANDLE
k32.VirtualQueryEx.argtypes = [wt.HANDLE, ct.c_void_p, ct.POINTER(MBI), ct.c_size_t]
k32.VirtualQueryEx.restype = ct.c_size_t
psapi.GetProcessMemoryInfo.argtypes = [wt.HANDLE, ct.POINTER(PMC), wt.DWORD]
psapi.GetMappedFileNameW.argtypes = [wt.HANDLE, ct.c_void_p, wt.LPWSTR, wt.DWORD]

KIND = {MEM_IMAGE: "image", MEM_MAPPED: "mapped", MEM_PRIVATE: "private"}


def counters(handle):
    pmc = PMC()
    pmc.cb = ct.sizeof(PMC)
    if not psapi.GetProcessMemoryInfo(handle, ct.byref(pmc), pmc.cb):
        return None
    return {"private": pmc.PrivateUsage, "working_set": pmc.WorkingSetSize, "peak_private": pmc.PeakPagefileUsage,
            "page_faults": pmc.PageFaultCount}


def walk(handle):
    """Committed bytes per kind, per image module and per private allocation base."""
    address, mbi = 0, MBI()
    kinds = defaultdict(int)
    modules = defaultdict(int)
    allocations = defaultdict(lambda: {"committed": 0, "reserved": 0, "protect": set()})
    names = {}
    buffer = ct.create_unicode_buffer(1024)
    while k32.VirtualQueryEx(handle, ct.c_void_p(address), ct.byref(mbi), ct.sizeof(mbi)):
        size, base = mbi.RegionSize, mbi.AllocationBase or 0
        if mbi.State == MEM_COMMIT:
            kind = KIND.get(mbi.Type, "other")
            kinds[kind] += size
            if mbi.Type == MEM_IMAGE:
                if base not in names:
                    n = psapi.GetMappedFileNameW(handle, ct.c_void_p(base), buffer, 1024)
                    names[base] = buffer.value.rsplit("\\", 1)[-1] if n else hex(base)
                writable = mbi.Protect & 0xCC  # RW/WC/XRW/XWC pages: per-process copies when touched
                modules[(names[base], "writable" if writable else "shared")] += size
            elif mbi.Type == MEM_PRIVATE:
                allocations[base]["committed"] += size
                allocations[base]["protect"].add(hex(mbi.Protect))
        if mbi.State == MEM_RESERVE and mbi.Type == MEM_PRIVATE:
            allocations[base]["reserved"] += size
        address += size
        if address >= 1 << 47:
            break
    largest = sorted(allocations.items(), key=lambda item: -item[1]["committed"])
    histogram = defaultdict(lambda: [0, 0])
    for _, a in allocations.items():
        if a["committed"]:
            bucket = 1 << max(12, a["committed"].bit_length() - 1)
            histogram[bucket][0] += 1
            histogram[bucket][1] += a["committed"]
    return {"committed_by_kind": dict(kinds),
            "image_modules": sorted(([n, w, s] for (n, w), s in modules.items()), key=lambda r: -r[2])[:40],
            "private_allocations": len([a for a in allocations.values() if a["committed"]]),
            "private_size_histogram": {str(k): v for k, v in sorted(histogram.items())},
            "largest_private": [{"base": hex(b), "committed": a["committed"], "reserved": a["reserved"],
                                 "protect": sorted(a["protect"])} for b, a in largest[:40]]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--cwd", type=Path, required=True)
    parser.add_argument("--interval-ms", type=int, default=20)
    parser.add_argument("--walk-gap-ms", type=int, default=500, help="minimum gap between peak walks")
    parser.add_argument("--snapshot-every-ms", type=int, default=10000)
    parser.add_argument("--env", action="append", default=[], help="NAME=VALUE observer for the child")
    parser.add_argument("argv", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    argv = args.argv[1:] if args.argv and args.argv[0] == "--" else args.argv
    env = {k: v for k, v in os.environ.items() if not k.upper().startswith(("GEKKO_", "SHADOW_"))}
    env.update(item.split("=", 1) for item in args.env)
    process = subprocess.Popen(argv, cwd=args.cwd, env=env, stdin=subprocess.DEVNULL,
                               stdout=(args.cwd / "stdout.log").open("xb"), stderr=(args.cwd / "stderr.log").open("xb"))
    handle = k32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, process.pid)
    start = time.perf_counter()
    trace, walks = [], []
    best, last_walk, next_snapshot = 0, -1e9, args.snapshot_every_ms
    try:
        while process.poll() is None:
            now = (time.perf_counter() - start) * 1000
            c = counters(handle)
            if c:
                c["ms"] = round(now, 1)
                trace.append(c)
                peak = c["private"] > best * 1.02 and now - last_walk >= args.walk_gap_ms
                if peak or now >= next_snapshot:
                    w = walk(handle)
                    w.update({"ms": round(now, 1), "reason": "peak" if peak else "periodic", "private": c["private"]})
                    walks.append(w)
                    last_walk = now
                    if not peak:
                        next_snapshot += args.snapshot_every_ms
                best = max(best, c["private"])
            time.sleep(args.interval_ms / 1000)
    finally:
        process.wait()
    args.out.write_text(json.dumps({"argv": argv, "exit": process.returncode, "trace": trace, "walks": walks}, indent=1))
    peak = max(trace, key=lambda r: r["private"]) if trace else None
    print(json.dumps({"exit": process.returncode, "samples": len(trace), "walks": len(walks), "peak": peak}))


if __name__ == "__main__":
    main()
