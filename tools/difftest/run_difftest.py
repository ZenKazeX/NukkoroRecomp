"""ctest entry for the F1a differential harness (ctest -R difftest_f1a).

  run_difftest.py --prod difftest_driver_prod.exe --ref difftest_driver_ref.exe
                  --generated <Testing/difftest-f1a> --spec <test_data/core-audit/spec>
                  [--known known_candidates_f1a.json] [--seed S --scale X] [--only id,...]

Runs the production-build driver (lanes N, NL), the reference-build driver (R, RL, I, IF), the
spec runner and the comparator over the vectors gen_vectors.py produced at build time, then exits
with the comparator's code.

The known-candidate ledger is applied ONLY when the generated vectors were produced with the
ledger's seed and scale (its fingerprints are exact per vector set). Each entry pins the exact
fingerprint of one candidate-divergence group found by this harness and not yet decided; every run
prints it as KNOWN, and a group that changes in any way (new vector, different values) or stops
reproducing fails the test. Engine-vs-engine divergences, host-fact violations and harness errors
are never in the ledger.
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
    p.add_argument("--only", default="")
    p.add_argument("--work", type=Path, default=None)
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
    print(f"difftest F1a: {manifest['vectors']} vectors, {manifest['rows']} rows, seed {manifest['seed']}, "
          f"generator {manifest['generator']} (sha256 {manifest['generator_sha256'][:16]})")
    print(f"  generator arguments: {' '.join(Path(x).name if i in (0,) else x for i, x in enumerate(manifest['generator_command']))}")

    only = ["--only", a.only] if a.only else []
    rc = run([a.prod, "--vectors", gen / "vectors.txt", "--out", work / "prod.jsonl", *only])
    rc |= run([a.ref, "--vectors", gen / "vectors.txt", "--out", work / "ref.jsonl", *only])
    rc |= run([sys.executable, "-B", HERE / "spec_runner.py", "--vectors", gen / "vectors.jsonl",
               "--spec", a.spec, "--out", work / "spec.jsonl", *only])
    if rc != 0:
        print("a harness stage failed", file=sys.stderr)
        return 2

    compare = [sys.executable, "-B", HERE / "compare.py", "--vectors", gen / "vectors.jsonl",
               "--lanes", work / "prod.jsonl", work / "ref.jsonl", "--spec-results", work / "spec.jsonl",
               "--generated", gen, "--out", work / "report.json"]
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
