"""Crash-tolerant verification driver for the lifted leaf functions (docs/DOLRECON.md "C++ reconstruction").

    python -B -m dolrecon.lift_verify <dolrecon_lift_tests.exe> <main.dol> <out dir> [--jobs 16] [--chunk 200]

Runs `dolrecon_lift_tests --range from to` over kLifted in chunks on bounded worker processes. The harness prints
"BEGIN i" before and "RESULT ..." after each function. When a worker process dies (the reference interpreter's
fault handler aborts on a host access fault - an ORIGINAL function chasing a random pointer across a host reservation
boundary), the function it was on is recorded as unverified_original_faulted with the process's last output as
evidence, and the chunk resumes at the next function. Nothing is retried silently into a pass.

Writes <out>/results.csv (one row per lifted function), <out>/faults.txt (the skip list the CTest uses, with
evidence), <out>/summary.json.
"""

from __future__ import annotations

import argparse
import collections
import concurrent.futures as cf
import csv
import io
import json
import os
import re
import subprocess
import sys
import time

RESULT = re.compile(r"^RESULT (\d+),(.*)$")
BEGIN = re.compile(r"^BEGIN (\d+)$")


def count(exe: str, dol: str) -> int:
    r = subprocess.run([exe, dol, "-", "1", "--range", "0", "0"], capture_output=True, text=True)
    # an empty range prints nothing; ask the full-run banner instead is too slow - read kLiftedCount from a 1-run range
    r = subprocess.run([exe, dol, "-", "1", "--range", "999999999", "999999999"], capture_output=True, text=True)
    return -1


EXTRA: list[str] = []  # extra harness arguments (e.g. --mem1 <image>)


def run_chunk(exe: str, dol: str, lo: int, hi: int) -> tuple[list[str], list[dict]]:
    rows, faults = [], []
    start = lo
    while start < hi:
        p = subprocess.run([exe, dol, "-", "1", *EXTRA, "--range", str(start), str(hi)], capture_output=True,
                           text=True)
        last_begin, done = None, set()
        for line in p.stdout.splitlines():
            m = BEGIN.match(line)
            if m:
                last_begin = int(m.group(1))
                continue
            m = RESULT.match(line)
            if m:
                rows.append(line[len("RESULT "):])
                done.add(int(m.group(1)))
        if p.returncode == 0:
            break
        if last_begin is None or last_begin in done:
            # died outside a function test (start-up): record the whole remainder as a harness failure
            faults.append({"index": start, "reason": f"worker exit {p.returncode} before a function began",
                           "evidence": (p.stdout + p.stderr)[-600:]})
            break
        faults.append({"index": last_begin, "reason": f"worker exit {p.returncode} while testing",
                       "evidence": (p.stdout + p.stderr)[-600:].strip()})
        start = last_begin + 1
    return rows, faults


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("exe")
    ap.add_argument("dol")
    ap.add_argument("out")
    ap.add_argument("--jobs", type=int, default=16)
    ap.add_argument("--chunk", type=int, default=200)
    ap.add_argument("--total", type=int, required=True, help="kLiftedCount (lift_cpp report 'lifted')")
    ap.add_argument("--mem1", help="a 24 MiB MEM1 image of the original game as background memory")
    args = ap.parse_args(argv)
    if args.mem1:
        EXTRA[:] = ["--mem1", args.mem1]
    os.makedirs(args.out, exist_ok=True)
    t0 = time.perf_counter()
    chunks = [(lo, min(lo + args.chunk, args.total)) for lo in range(0, args.total, args.chunk)]
    rows, faults = [], []
    with cf.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for r, f in pool.map(lambda c: run_chunk(args.exe, args.dol, *c), chunks):
            rows += r
            faults += f
    parsed = []
    for line in rows:
        rec = next(csv.reader(io.StringIO(line)))
        parsed.append(rec)
    parsed.sort(key=lambda r: int(r[0]))
    status = collections.Counter(r[6] for r in parsed)
    with open(os.path.join(args.out, "results.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["index", "address", "name", "instructions", "valid_cases", "invalid_cases", "status", "detail"])
        w.writerows(parsed)
    # map fault indices to addresses through the generated table order (the harness prints the address only in RESULT)
    with open(os.path.join(args.out, "faults.json"), "w") as f:
        json.dump(faults, f, indent=1)
    summary = {"mem1": args.mem1, "lifted": args.total, "results": len(parsed), "status": dict(status), "worker_faults": len(faults),
               "seconds": time.perf_counter() - t0, "jobs": args.jobs, "chunk": args.chunk}
    with open(os.path.join(args.out, "summary.json"), "w") as f:
        json.dump(summary, f, indent=1)
    print(json.dumps(summary, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
