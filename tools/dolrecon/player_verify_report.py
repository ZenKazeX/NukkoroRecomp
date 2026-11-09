"""--verify-player results per census function (docs/SHADOW_PLAYER_NATIVE_RECONSTRUCTION.md, batch 00).

    python -B -m dolrecon.player_verify_report <player_census.csv> <verify.csv> [<verify.csv> ...]

Reads the runtime's per-entry results (verify.csv) and interpreted runs (verify.csv.coverage) and prints, for the player
members: entries verified / mismatched / skipped, and the instructions the interpreter replayed inside each function
(verified coverage: an instruction counts only if a scoped replay executed it and its segment matched or mismatched -
skipped segments are never replayed). Writes <first verify.csv>.functions.csv with one row per member.
A run's <verify.csv>.mode says how it was made: "strict" (every segment) or "fast ..." (sampled arrivals under a CPU
budget - its numbers are what was checked, never the route's coverage); the mode is printed first.
"""
from __future__ import annotations

import bisect
import collections
import csv
import sys


def main(argv=None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    census = [r for r in csv.DictReader(open(argv[0], encoding="utf-8")) if r["category"] != "SHARED_ENGINE_DEPENDENCY"]
    starts = sorted(int(r["address"], 16) for r in census)
    size = {int(r["address"], 16): int(r["size"]) for r in census}
    stats = collections.defaultdict(collections.Counter)
    covered = collections.Counter()
    for path in argv[1:]:
        try:
            mode = open(path + ".mode", encoding="utf-8").read().strip()
        except OSError:
            mode = "strict (no .mode file: written before fast mode existed)"
        print(f"{path}: {mode}")
        for r in csv.DictReader(open(path, encoding="utf-8")):
            a = int(r["entry"], 16)
            for k in ("verified", "mismatches", "skipped_device", "skipped_exit", "owner_verified", "owner_mismatches"):
                stats[a][k] += int(r[k])
        for line in open(path + ".coverage", encoding="utf-8"):
            s, e = (int(x, 16) for x in line.split(","))
            for pc in range(s, e, 4):
                i = bisect.bisect_right(starts, pc) - 1
                if i >= 0 and pc < starts[i] + size[starts[i]]:
                    covered[starts[i]] += 1
    rows = []
    for r in census:
        a = int(r["address"], 16)
        rows.append({"address": r["address"], "name": r["name"], "category": r["category"], "evidence": r["evidence"],
                     "instructions": int(r["instructions"]), "dynamic_calls": r.get("dynamic_calls", ""),
                     **{k: stats[a][k] for k in ("verified", "mismatches", "skipped_device", "skipped_exit",
                                                 "owner_verified", "owner_mismatches")},
                     "replayed_instructions": covered[a]})
    with open(argv[1] + ".functions.csv", "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0]), lineterminator="\n")
        w.writeheader()
        w.writerows(rows)
    total = sum(r["instructions"] for r in rows)
    entered = [r for r in rows if r["verified"] or r["mismatches"] or r["skipped_device"] or r["skipped_exit"]]
    print(f"members {len(rows)} ({total} instructions); entries attempted {len(entered)}; "
          f"segments verified {sum(r['verified'] for r in rows)}, mismatched {sum(r['mismatches'] for r in rows)}, "
          f"skipped device {sum(r['skipped_device'] for r in rows)}, skipped exit {sum(r['skipped_exit'] for r in rows)}")
    print(f"functions with a mismatch: {sum(1 for r in rows if r['mismatches'])}; instructions replayed in members: "
          f"{sum(r['replayed_instructions'] for r in rows)} of {total} "
          f"({100 * sum(r['replayed_instructions'] for r in rows) / total:.2f} %); functions with any replayed "
          f"instruction: {sum(1 for r in rows if r['replayed_instructions'])}")
    by_cat = collections.defaultdict(lambda: [0, 0])
    for r in rows:
        by_cat[r["category"]][0] += r["replayed_instructions"]
        by_cat[r["category"]][1] += r["instructions"]
    for c, (n, t) in sorted(by_cat.items()):
        print(f"  {c:20s} {n:6d} / {t:6d} instructions replayed ({100 * n / t:.1f} %)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
