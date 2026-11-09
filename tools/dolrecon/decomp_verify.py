"""Verify every matched source file in source/decomp and write the proof manifest.

    python -B -m dolrecon.decomp_verify <project> [--decomp ../decomp] [--objdiff]        (run from source/tools)

Each .c / .cpp file declares how it was built:  // @match cc=<version> flags=<compiler flags>
and its symbol placement with // @sym lines (dolrecon/match.py). Every function in the file that pairs with an
original function is judged by match.py (M1-M4, objdiff X1 with --objdiff). The manifest
<decomp>/matched.json lists, per file, each function's verdict and, for the matched ones, the candidate's real
(mangled) name and the original address. names.py takes rule M0 names ONLY from matched entries; a file that stops
matching loses its names on the next run. Nothing here edits a source file.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys

from . import elfobj
from . import match as m


def build_spec(path: str) -> tuple[str, str] | None:
    pat = re.compile(r"@match\s+cc=(\S+)\s+flags=(.+?)\s*$")
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            hit = pat.search(line)
            if hit:
                return hit[1], hit[2]
    return None


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("project")
    ap.add_argument("--decomp", default=os.path.join(os.path.dirname(__file__), "..", "..", "decomp"))
    ap.add_argument("--objdiff", action="store_true")
    args = ap.parse_args(argv)
    project = m.Project(args.project)
    work = os.path.join(args.project, "build", "match")
    os.makedirs(work, exist_ok=True)
    files, total, matched, errors = [], 0, 0, 0
    for root, _, names in os.walk(args.decomp):
        for n in sorted(names):
            if not n.endswith((".c", ".cpp")):
                continue
            path = os.path.join(root, n)
            rel = os.path.relpath(path, args.decomp).replace("\\", "/")
            spec = build_spec(path)
            entry = {"file": rel}
            if spec is None:
                entry["error"] = "no // @match cc=... flags=... line"
                files.append(entry)
                errors += 1
                continue
            cc, flags = spec
            symmap = m.read_symbol_map(path)
            results, obj, pairs = m.run_one(project, path, cc, flags, [], work, [], symmap)
            entry.update(cc=cc, flags=flags)
            if results is None:
                entry["error"] = obj.splitlines()[-1] if obj else "compile failed"
                files.append(entry)
                errors += 1
                continue
            base = elfobj.read(obj)
            rows = []
            paired = {bs.name for bs, _ in pairs}
            unpaired = [f.name for f in base.functions() if f.name not in paired and f.bind != elfobj.STB_WEAK]
            if unpaired:
                entry["unpaired"] = unpaired
                errors += 1
            for r, (bs, ts) in zip(results, pairs):
                if args.objdiff and r.matched:
                    pct, bad = m.objdiff_check(obj, base, bs, project, ts, symmap)
                    judge = m.Judge(project, base, symmap)
                    lits = {(x.offset - bs.value) // 4 for x in base.relocs_in(bs.shndx, bs.value, bs.value + bs.size)
                            if judge.is_local_data(x.symbol)}
                    if pct is None:
                        # objdiff failed or did not find the symbol: no cross-check is no pass (an empty difference
                        # list used to be accepted here; found by Astra, 2026-11-01)
                        r.matched = False
                        r.problems.append("X1 objdiff produced no comparison for this function")
                    elif [i for i in bad if i not in lits]:
                        r.matched = False
                        r.problems.append(f"X1 objdiff marks instructions {bad} different")
                rows.append({"name": bs.name, "address": f"0x{ts.address:08X}", "original": ts.name,
                             "matched": r.matched, "fuzzy": round(r.fuzzy, 2), "problems": r.problems[:8]})
                total += 1
                matched += r.matched
            entry["functions"] = rows
            files.append(entry)
    out = {"schema": "dolrecon.decomp-matched/1", "project": os.path.abspath(args.project),
           "functions": total, "matched": matched, "file_errors": errors, "files": files}
    with open(os.path.join(args.decomp, "matched.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(out, f, indent=1)
        f.write("\n")
    for e in files:
        if "error" in e:
            print(f"ERROR {e['file']}: {e['error']}")
            continue
        ok = sum(r["matched"] for r in e["functions"])
        print(f"{ok}/{len(e['functions'])} matched  {e['file']}  (cc {e['cc']})")
        if e.get("unpaired"):
            print(f"    ERROR unpaired functions (no original): {', '.join(e['unpaired'][:5])}")
        for r in e["functions"]:
            if not r["matched"]:
                print(f"    diff {r['fuzzy']:6.2f}% {r['address']} {r['name']}: {r['problems'][:1]}")
    print(f"total {matched}/{total} functions matched, {errors} file errors")
    return 0 if matched == total and errors == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
