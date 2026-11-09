"""Ask the generator what a GameCube-removal step would free, before writing any of it.

    python -B what_if.py --name NAME [--own ADDR ...] [--root ADDR ...] [--drop-root ADDR ...]
                         [--drop-roots-from FILE ...] [--release-dir DIR] [--out DIR]

Runs ppcrecomp with the release build's exact arguments (read from its generated ensure_outputs_Release.cmake), once
as the baseline and once with the step applied to scratch copies of the owner list and the liveness roots:
  --own ADDR            the function at ADDR becomes a native owner (its body is not emitted, nothing is reached
                        through it), as an implemented owner would;
  --root ADDR           native code would enter the guest at ADDR (e.g. the native boot calling main);
  --drop-root ADDR      the runtime no longer names ADDR (e.g. an observer is deleted);
  --drop-roots-from F   every liveness root that only the runtime source file F (or, ending in '/', the files
                        under the directory F) names (liveness_roots.py's own scan, so a literal also named
                        elsewhere stays). Paths are relative to the source tree, e.g. runtime/tests/.
The extract/ inputs are never written. Writes <out>/<name>/what_if.json: the rows the step would prune, by the
platform manifest's service, with names; and the rows it would make live again (a root it adds). The result is the
generator's own liveness, not a model of it: the same code decides what the real build emits.
"""
from __future__ import annotations

import argparse
import bisect
import csv
import json
import os
import re
import shutil
import subprocess
import sys

SRC = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))


def input_digests(argv):
    """SHA-256 of every file the generator reads (the baseline is reused only while they are unchanged)."""
    import hashlib
    return {a: hashlib.sha256(open(a, "rb").read()).hexdigest() for a in argv if os.path.isfile(a)}


def release_argv(release_dir):
    cm = open(os.path.join(release_dir, "generated", "ensure_outputs_Release.cmake"), encoding="utf-8").read()
    line = next(l for l in cm.splitlines() if "ppcrecomp" in l and "COMMAND" in l)
    return re.findall(r"\[====\[(.*?)\]====\]", line)


def run(argv, out):
    if os.path.exists(out):
        shutil.rmtree(out)
    os.makedirs(out)
    a = list(argv)
    a[a.index("--out") + 1] = out
    a[a.index("--report") + 1] = os.path.join(out, "recomp_report.txt")
    p = subprocess.run(a, capture_output=True, text=True, errors="replace")
    open(os.path.join(out, "stderr.log"), "w").write(p.stderr)
    pruned = os.path.join(out, "recomp_pruned.csv")
    refusal = None
    if p.returncode != 0:
        # The generator refuses a stale site list after deciding liveness (recomp_pruned.csv is complete by then);
        # any other failure is fatal here.
        last = [l for l in p.stderr.splitlines() if l.strip()][-1:] or [""]
        if not (os.path.exists(pruned) and re.match(r"stale .*site|\d+ stale ", last[0])):
            raise SystemExit(f"ppcrecomp failed ({p.returncode}) in {out}; see stderr.log")
        refusal = last[0]
    rows = {int(r["address"], 16): r["name"] for r in csv.DictReader(open(pruned))}
    # the generated sources are not needed, only the decision
    for f in os.listdir(out):
        if f.startswith("recomp_") and f.endswith(".cpp"):
            os.remove(os.path.join(out, f))
    return rows, refusal


def roots_by_file():
    """liveness_roots.py's literal scan, kept per file: {address: {files}}."""
    sys.path.insert(0, os.path.join(SRC, "tools", "ppcrecomp"))
    import liveness_roots as lr  # noqa: E402
    from pathlib import Path
    root = Path(SRC) / "runtime"
    out = {}
    for path in sorted(root.rglob("*")):
        if not (path.is_file() and path.suffix.lower() in lr.SUFFIXES):
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        if lr.interpreter_only(path, root, text):
            continue
        code = lr.strip_comments(lr.drop_value_only(path, root, text))
        rel = path.relative_to(Path(SRC)).as_posix()
        for match in lr.LITERAL.finditer(code):
            out.setdefault(int(match.group(1), 16), set()).add(rel)
    return out


DOLRECON = os.path.join(SRC, ".build", "dolrecon")


def services(dolrecon=None):
    dolrecon = dolrecon or DOLRECON
    man = os.path.join(dolrecon, "pal-v30", "platform_manifest.json")
    m = json.load(open(man))
    svc = {int(e["address"], 16): e["service"] for e in m["entries"]}
    sk = sorted((int(r["address"], 16), int(r["size"] or 0), r["layer"])
                for r in csv.DictReader(open(os.path.join(dolrecon, "skeleton", "functions.csv"),
                                             encoding="utf-8")))
    starts = [x[0] for x in sk]

    def of(a):
        i = bisect.bisect_right(starts, a) - 1
        if i < 0 or a >= sk[i][0] + max(sk[i][1], 4):
            return "outside-any-function"
        return svc.get(sk[i][0], "layer:" + sk[i][2])
    return of


SITE_LISTS = ("--platform-sites", "--gx-sites", "--lift-sites", "--wait-points", "--psq-sites", "--gx-fail-sites",
              "--empty-variadic-leaves", "--jump-tables")


def stale_sites(argv, freed, owned):
    """Every listed site inside a row the step prunes or owns: each is a work item (the list entry goes, or the
    native code that replaced the site takes over the whole function)."""
    ext = {}
    for r in csv.DictReader(open(os.path.join(SRC, "extract", "functions.csv"), encoding="utf-8")):
        ext[int(r["addr_hex"], 16)] = int(r["size"] or 0)
    spans = sorted((a, a + max(ext.get(a, 4), 4)) for a in set(freed) | owned)
    starts = [x[0] for x in spans]
    out = []
    for i, a in enumerate(argv):
        if a not in SITE_LISTS:
            continue
        path = argv[i + 1]
        for n, line in enumerate(open(path, encoding="utf-8", errors="replace"), 1):
            t = line.split("#")[0].split()
            if not t:
                continue
            tok = t[1] if t[0] == "site" and len(t) > 1 else t[0]
            if not re.match(r"0x[0-9A-Fa-f]{8}$", tok):
                continue
            v = int(tok, 16)
            j = bisect.bisect_right(starts, v) - 1
            if j >= 0 and v < spans[j][1]:
                out.append({"list": os.path.relpath(path, SRC).replace(os.sep, "/"), "line": n, "site": tok,
                            "row": f"0x{spans[j][0]:08X}", "text": line.strip()[:160]})
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--name", required=True)
    ap.add_argument("--own", action="append", default=[])
    ap.add_argument("--root", action="append", default=[])
    ap.add_argument("--drop-root", action="append", default=[])
    ap.add_argument("--drop-roots-from", action="append", default=[])
    ap.add_argument("--release-dir", default=os.path.join(SRC, ".build", "release"))
    ap.add_argument("--out", default=os.path.join(SRC, ".build", "platform", "what_if"))
    ap.add_argument("--dolrecon-dir", default=DOLRECON, help="tools/dolrecon outputs (pal-v30 manifest, skeleton)")
    ap.add_argument("--dry-run", action="store_true", help="print the step and the generator command, run nothing")
    args = ap.parse_args(argv)

    base_argv = release_argv(args.release_dir)
    work = os.path.join(args.out, args.name)
    os.makedirs(work, exist_ok=True)
    hexes = lambda xs: sorted({int(x, 16) for v in xs for x in v.split(",") if x})
    own, add, drop = hexes(args.own), hexes(args.root), set(hexes(args.drop_root))

    roots_path = base_argv[base_argv.index("--prune-roots") + 1]
    roots = [l for l in open(roots_path, encoding="utf-8").read().splitlines()]
    if args.drop_roots_from:
        by = roots_by_file()
        if by is None:
            raise SystemExit("liveness_roots.py has no literals_by_file(); cannot attribute roots to files")
        # a file, or a directory ending in '/': a root is dropped only when every file naming it is covered
        covered = lambda f: any(f.startswith(d) if d.endswith("/") else f == d for d in args.drop_roots_from)
        for a, files in by.items():
            if files and all(covered(f) for f in files):
                drop.add(a)
    kept = []
    for l in roots:
        t = l.split("#")[0].strip()
        if t and re.match(r"0x[0-9A-Fa-f]{8}$", t) and int(t, 16) in drop:
            continue
        kept.append(l)
    kept += [f"0x{a:08X}" for a in add]
    step = {"own": [f"0x{a:08X}" for a in own], "add_roots": [f"0x{a:08X}" for a in add],
            "drop_roots": [f"0x{a:08X}" for a in sorted(drop)]}
    if args.dry_run:
        print(json.dumps(step, indent=1))
        print(" ".join(base_argv[:3]), "...")
        return 0

    trial = list(base_argv)
    rp = os.path.join(work, "liveness-roots.txt")
    open(rp, "w", encoding="utf-8", newline="\n").write("\n".join(kept) + "\n")
    trial[trial.index("--prune-roots") + 1] = rp
    if own:
        op = os.path.join(work, "owners-what-if.txt")
        open(op, "w", encoding="utf-8", newline="\n").write("".join(f"0x{a:08X} what-if\n" for a in own))
        trial += ["--native-owners", op]

    base_dir = os.path.join(args.out, "_baseline")
    stamp = os.path.join(base_dir, "argv.json")
    key = {"argv": base_argv, "inputs": input_digests(base_argv)}
    if os.path.exists(stamp) and json.load(open(stamp)) == key and \
            os.path.exists(os.path.join(base_dir, "recomp_pruned.csv")):
        base = {int(r["address"], 16): r["name"] for r in csv.DictReader(open(os.path.join(base_dir, "recomp_pruned.csv")))}
    else:
        base, refusal = run(base_argv, base_dir)
        if refusal:
            raise SystemExit("the baseline itself is refused: " + refusal)
        json.dump(key, open(stamp, "w"))
    after, refusal = run(trial, os.path.join(work, "generated"))
    of = services(args.dolrecon_dir)
    freed = sorted(set(after) - set(base))
    revived = sorted(set(base) - set(after))
    by = {}
    for a in freed:
        by.setdefault(of(a), []).append(f"0x{a:08X} {after[a]}")
    result = {"step": step, "baseline_pruned": len(base), "after_pruned": len(after),
              "freed": len(freed), "revived": len(revived),
              "freed_by_service": {k: {"count": len(v), "rows": v} for k, v in sorted(by.items(), key=lambda kv: -len(kv[1]))},
              "revived_rows": [f"0x{a:08X} {base[a]}" for a in revived],
              "generator_refusal": refusal,
              "stale_sites": stale_sites(trial, freed, set(own))}
    json.dump(result, open(os.path.join(work, "what_if.json"), "w"), indent=1)
    print(f"{args.name}: frees {len(freed)} rows, revives {len(revived)}, {len(result['stale_sites'])} listed sites "
          f"become work items" + (f" (generator: {refusal})" if refusal else ""))
    for k, v in result["freed_by_service"].items():
        print(f"  {v['count']:5}  {k}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
