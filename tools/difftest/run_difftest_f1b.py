"""ctest entry for the F1b differential harness (ctest -R difftest_f1b).

  run_difftest_f1b.py --prod difftest_f1b_driver_prod.exe --ref difftest_f1b_driver_ref.exe
                      --generated <Testing/difftest-f1b> --spec <test_data/core-audit/spec>
                      [--known known_candidates_f1b.json] [--seed S --scale X] [--jobs N] [--only id,...]
                      [--work DIR] [--max-detail N]

Runs the production driver (lanes N, NL) and the reference driver (R, RL, I, IF, IR) over the vectors
gen_f1b.py produced at build time, then compare_f1b.py (which computes the spec lane in-process) and exits
with the comparator's code. Lanes NB and N1 (budget-limited run(1) dispatch) are retired with the
instruction budget (native time, see difftest_driver.cpp): the drivers refuse them.

The known-candidate ledger is applied only when the generated vectors carry the ledger's seed and scale:
its fingerprints are exact per vector set. Each entry pins one candidate group found by this harness and
not yet decided; every run prints it as KNOWN, and a group that changes in any way or stops reproducing
fails the test. Engine-vs-engine divergences and imprecise-entry candidates are ledgered only as the
exact groups listed; host-fact violations and harness errors can never be satisfied by the ledger.
"""
import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent


def run(cmd):
    start = time.monotonic()
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True)
    sys.stdout.write(r.stdout)
    sys.stderr.write(r.stderr)
    print(f"  [{time.monotonic() - start:6.1f} s] rc={r.returncode}: {Path(str(cmd[0])).name} "
          f"{' '.join(Path(str(c)).name for c in cmd[1:3])}", flush=True)
    return r.returncode


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--prod", type=Path, required=True)
    p.add_argument("--ref", type=Path, required=True)
    p.add_argument("--generated", type=Path, required=True)
    p.add_argument("--spec", type=Path, required=True)
    p.add_argument("--known", type=Path, default=None)
    p.add_argument("--seed", type=int, default=None)
    p.add_argument("--scale", type=float, default=None)
    p.add_argument("--jobs", type=int, default=8)
    p.add_argument("--only", default="")
    p.add_argument("--work", type=Path, default=None)
    p.add_argument("--max-detail", type=int, default=100000)
    a = p.parse_args()

    gen = a.generated
    work = a.work or (gen / "results")
    work.mkdir(parents=True, exist_ok=True)
    manifest = json.loads((gen / "manifest.json").read_text(encoding="utf-8"))
    if a.seed is not None and manifest["seed"] != a.seed:
        print(f"generated vectors have seed {manifest['seed']}, expected {a.seed}: rebuild", file=sys.stderr)
        return 2
    if a.scale is not None and float(manifest["scale"]) != a.scale:
        print(f"generated vectors have scale {manifest['scale']}, expected {a.scale}: rebuild", file=sys.stderr)
        return 2
    print(f"difftest F1b: {manifest['vectors']} vectors, {manifest['rows']} rows, seed {manifest['seed']}, "
          f"generator {manifest['generator']} (sha256 {manifest['generator_sha256'][:16]})")
    print(f"  generator arguments: {' '.join(manifest['generator_command'][1:])}")

    only = ["--only", a.only] if a.only else []
    rc = run([a.prod, "--vectors", gen / "vectors.txt", "--out", work / "prod.jsonl", "--lanes", "N,NL", *only])
    rc |= run([a.ref, "--vectors", gen / "vectors.txt", "--out", work / "ref.jsonl", "--lanes", "R,RL,I,IF,IR",
               *only])
    if rc != 0:
        print("a driver failed", file=sys.stderr)
        return 2
    compare = [sys.executable, "-B", HERE / "compare_f1b.py", "--vectors", gen / "vectors.jsonl",
               "--prod", work / "prod.jsonl", "--ref", work / "ref.jsonl", "--spec", a.spec, "--generated", gen,
               "--out", work / "report.json", "--jobs", str(a.jobs), "--max-detail", str(a.max_detail)]
    if a.only:
        compare += ["--only", a.only]
    if a.known and a.known.exists() and not a.only:
        ledger = json.loads(a.known.read_text(encoding="utf-8"))
        cfg = ledger.get("config", {})
        if cfg.get("seed") == manifest["seed"] and float(cfg.get("scale", -1)) == float(manifest["scale"]):
            active = work / "known_active.json"
            active.write_text(json.dumps(ledger["candidates"]), encoding="utf-8")
            compare += ["--known", active]
            print(f"  known-candidate ledger: {len(ledger['candidates'])} entries ({a.known.name})")
        else:
            print(f"  known-candidate ledger NOT applied: it records seed {cfg.get('seed')} scale "
                  f"{cfg.get('scale')}; every candidate below is reported as NEW")
    return run(compare)


if __name__ == "__main__":
    sys.exit(main())
