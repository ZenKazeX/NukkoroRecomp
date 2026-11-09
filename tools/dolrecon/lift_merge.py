"""Merges dolrecon.lift_verify runs (e.g. DOL image + real MEM1 snapshots) into one status per lifted function.

    python -B -m dolrecon.lift_merge <out.csv> <run dir> [<run dir> ...]

FAILED if any run failed it; else verified if any run verified it; else the most specific unverified status.
Valid cases are summed. Nothing is upgraded: a function verified in one run and failed in another is FAILED.
"""

import collections
import csv
import json
import os
import re
import sys

COV = re.compile(r"^cov (\d+)/(\d+)")


def cov_of(detail):
    m = COV.match(detail or "")
    return (int(m.group(1)), int(m.group(2))) if m else (0, 0)

RANK = {"FAILED": 0, "verified": 1, "unverified_unlifted_callee": 2, "unverified_hardware": 3, "unverified": 4,
        "unverified_original_faulted": 5}


def main(argv):
    out, runs = argv[0], argv[1:]
    merged = {}
    for run in runs:
        with open(os.path.join(run, "results.csv")) as f:
            for r in csv.DictReader(f):
                key = r["address"]
                cur = merged.get(key)
                c, n = cov_of(r["detail"])
                if cur is None:
                    merged[key] = dict(r, runs=run, covered=c, instructions_total=n)
                    continue
                # best coverage of any run: a lower bound of the union (which instructions differ is not merged)
                cur["covered"] = max(cur["covered"], c)
                cur["valid_cases"] = str(int(cur["valid_cases"]) + int(r["valid_cases"]))
                if RANK[r["status"]] < RANK[cur["status"]]:
                    cur["status"], cur["detail"] = r["status"], r["detail"]
                cur["runs"] += ";" + run
    rows = sorted(merged.values(), key=lambda r: int(r["address"], 16))
    with open(out, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)
    status = collections.Counter(r["status"] for r in rows)
    verified = [r for r in rows if r["status"] == "verified"]
    full = sum(1 for r in verified if r["instructions_total"] and r["covered"] >= r["instructions_total"])
    buckets = collections.Counter()
    for r in verified:
        n = r["instructions_total"] or 1
        f = r["covered"] / n
        buckets["100%" if f >= 1 else ">=90%" if f >= 0.9 else ">=50%" if f >= 0.5 else "<50%"] += 1
    print(json.dumps({"functions": len(rows), "status": dict(status), "verified_fully_covered": full,
                      "verified_coverage": dict(buckets)}, indent=1))


if __name__ == "__main__":
    main(sys.argv[1:])
