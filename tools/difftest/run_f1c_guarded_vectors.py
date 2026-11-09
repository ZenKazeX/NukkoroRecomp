"""ctest entry f1c_guarded_f1b_vectors (test-only): the F1b harness vector programs under guard pages.

Runs the unmodified F1b production driver twice over the F1b vectors (lanes N, NL; the run(1) budget lane NB
is retired with the instruction budget - native time - and the driver refuses it):
  plain    difftest_f1b_driver_prod          (the process heap)
  guarded  difftest_f1b_driver_prod_guarded  (same source and generated objects, plus f1c_guard_alloc.cpp and
                                              f1c_guard_report.cpp: every heap block - Cpu, Bus, MEM1/ARAM/EFB,
                                              host page tables, recompiled lookup tables - guard-paged or
                                              canary-bracketed; the canaries are verified on every delete and
                                              at exit)
Pass: both drivers exit 0, their result records are byte-identical (the allocator placement changes nothing
a vector can observe), the guard allocator was active (guarded allocations > 0) and found no corrupted canary,
and the guarded process did not die of a guard-page access violation.
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--plain", type=Path, required=True)
    p.add_argument("--guarded", type=Path, required=True)
    p.add_argument("--generated", type=Path, required=True, help="F1b generated directory (vectors.txt)")
    p.add_argument("--work", type=Path, required=True)
    p.add_argument("--lanes", default="N,NL")
    a = p.parse_args()
    a.work.mkdir(parents=True, exist_ok=True)
    vectors = a.generated / "vectors.txt"
    results = {}
    for name, exe in (("plain", a.plain), ("guarded", a.guarded)):
        out = a.work / f"{name}.jsonl"
        report = a.work / f"{name}.guard.json"
        for f in (out, report):
            if f.exists():
                f.unlink()
        env = dict(os.environ)
        env["F1C_GUARD_REPORT"] = str(report)
        start = time.monotonic()
        r = subprocess.run([str(exe), "--vectors", str(vectors), "--out", str(out), "--lanes", a.lanes],
                           capture_output=True, text=True, env=env)
        results[name] = {"rc": r.returncode, "seconds": round(time.monotonic() - start, 1), "out": out,
                         "report": report, "stderr": r.stderr[-1000:]}
        print(f"  {name:8} rc={r.returncode} ({results[name]['seconds']} s) {exe.name}")
    failures = []
    for name in ("plain", "guarded"):
        if results[name]["rc"] != 0:
            failures.append(f"{name} driver exited {results[name]['rc']} (0x{results[name]['rc'] & 0xFFFFFFFF:08X}): "
                            f"{results[name]['stderr']}")
    records = 0
    if not failures:
        plain = results["plain"]["out"].read_bytes().splitlines()
        guarded = results["guarded"]["out"].read_bytes().splitlines()
        records = len(plain)
        if len(plain) != len(guarded):
            failures.append(f"record count differs: plain {len(plain)}, guarded {len(guarded)}")
        for i, (x, y) in enumerate(zip(plain, guarded)):
            if x != y:
                dx, dy = json.loads(x), json.loads(y)
                keys = sorted(k for k in set(dx) | set(dy) if dx.get(k) != dy.get(k))
                failures.append(f"record {i} ({dx.get('id')} lane {dx.get('lane')}) differs in {keys}")
                break
    guard = None
    if results["guarded"]["report"].exists():
        guard = json.loads(results["guarded"]["report"].read_text(encoding="utf-8"))
        g = guard["guard"]
        print(f"  guard allocator: {g['guarded_allocations']} guard-paged and {g['small_allocations']} canary-bracketed "
              f"allocations, {g['checks']} full checks, {g['canary_bytes_checked']} canary bytes verified, "
              f"{g['violations']} violations")
        if g["guarded_allocations"] == 0:
            failures.append("the guard allocator was not active in the guarded driver")
        if g["violations"] != 0 or guard["exit_check_new_violations"] != 0:
            failures.append(f"corrupted canaries: {g['violation_list'][:8]}")
    elif results["guarded"]["rc"] == 0:
        failures.append("the guarded driver wrote no guard report (f1c_guard_report.cpp not linked?)")
    summary = {"records": records, "failures": failures, "guard": guard,
               "drivers": {k: {kk: str(vv) for kk, vv in v.items()} for k, v in results.items()}}
    (a.work / "report.json").write_text(json.dumps(summary, indent=1), encoding="utf-8")
    if failures:
        print("FAILED:\n  " + "\n  ".join(failures))
        return 1
    print(f"f1c guarded F1b vectors: {records} records byte-identical with every heap block guard-paged; no canary "
          "corrupted")
    return 0


if __name__ == "__main__":
    sys.exit(main())
