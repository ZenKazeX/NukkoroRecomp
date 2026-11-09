"""Creates a reconstruction project for one DOL (docs/DOLRECON.md section 8) and runs the exact round trip.

    python -B -m dolrecon.project <main.dol> <project dir> [jobs]      (run from source/tools)

The project dir receives a copy of the DOL, dtk's config.yml (with the DOL's SHA-1, which dtk checks), dtk's analysis
and split (config/symbols.txt, config/splits.txt, out/), then dolrecon.rebuild's build/ (report.json, layout.json,
main.rebuilt.dol). An existing project dir is refused: outputs are never overwritten silently.
"""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
import sys
import time

from . import dol as dolmod
from . import rebuild


def create(dol_path: str, project: str, jobs: int = 16) -> dict:
    if os.path.exists(project):
        raise FileExistsError(f"{project} exists")
    image = dolmod.load_file(dol_path)  # validates before anything is written
    os.makedirs(os.path.join(project, "config"))
    shutil.copyfile(dol_path, os.path.join(project, "main.dol"))
    sha1 = hashlib.sha1(image.raw).hexdigest()
    with open(os.path.join(project, "config.yml"), "w", newline="\n") as f:
        f.write(f"object: main.dol\nhash: {sha1}\nsymbols: config/symbols.txt\nsplits: config/splits.txt\n")
    for name in ("symbols.txt", "splits.txt"):
        open(os.path.join(project, "config", name), "w").close()
    t = time.perf_counter()
    r = subprocess.run([rebuild.TOOLS["dtk"], "--no-color", "dol", "split", "-j", str(jobs), "config.yml", "out"],
                       cwd=project, capture_output=True, text=True)
    with open(os.path.join(project, "split.log"), "w") as f:
        f.write(r.stdout + r.stderr)
    if r.returncode != 0:
        raise RuntimeError(f"dtk split failed, see {project}/split.log")
    split_s = time.perf_counter() - t
    report = rebuild.run(project, jobs)
    report.setdefault("times_s", {})["dtk_split_s"] = split_s
    with open(os.path.join(project, "build", "report.json"), "w") as f:
        json.dump(report, f, indent=1)
    return report


if __name__ == "__main__":
    rep = create(sys.argv[1], sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 16)
    print(json.dumps({k: rep.get(k) for k in ("byte_exact", "block_mismatch_count", "rebuilt_sha256", "bytes_by_owner",
                                              "assembled_fraction", "times_s")}, indent=1))
    print("branch hint fixes:", len(rep.get("renames", {}).get("branch_hint_fixes", [])))
    sys.exit(0 if rep.get("byte_exact") else 1)
