"""Rebuild the platform manifest with the identified SDK names, in THIS worktree's .build/dolrecon.

    python -B tools/platform/regen_manifest.py [--source ../source/.build/dolrecon] [--out .build/dolrecon]

The removal planner and `what_if` classify every live body by service from `<dolrecon>/pal-v30/platform_manifest.json`,
which tools/dolrecon/platform_deps.py derives from `<dolrecon>/pal-v30/config/symbols.txt`. The shared copy under
source/.build is read-only for a worktree, so this script builds a private copy of exactly what platform_deps reads
(config/, main.dol, names.json, callgraph.json, receivers.json, out/asm, and the sibling skeleton/), applies the proposed
renames (test_data/platform/unidentified_names.symbols.patch, 162 renames of PROVEN / INFERRED SDK names, evidence in
docs/agents/sdk-identify.md) to the copied symbols.txt, and runs platform_deps with the repository's native-owners lists.
The exact-name service rules those names need are already in platform_deps.py LIBRARY.

Nothing under --source is written. CMake's SHADOW_DOLRECON_DIR defaults to <worktree>/.build/dolrecon, so a configured
tree picks the result up on its next run of the planner (platform_gc_removal_plan, tools/platform/removal_plan.py,
what_if.py --dolrecon-dir).

--no-names builds the control: the same copy without the renames, to separate what regenerating does by itself (the
shared manifest predates later changes to platform_deps.py) from what the names change.
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
PROJECT_FILES = ("config.yml", "main.dol", "names.json", "callgraph.json", "receivers.json")


def apply_anchor_names(symbols_path, names_csv):
    """Rename the symbols of the reviewed table. A row whose current name is not the recorded old name is left alone and
    reported (the symbol table moved under it); the new name is applied as the same line with only the name replaced."""
    import csv
    import re
    rows = list(csv.DictReader(open(names_csv, encoding="utf-8")))
    lines = open(symbols_path, encoding="utf-8", newline="").read().split(chr(10))
    index = {}
    for number, line in enumerate(lines):
        m = re.match(r"^(\S+) = \.\w+:0x([0-9A-Fa-f]{8});", line)
        if m:
            index[int(m.group(2), 16)] = (number, m.group(1))
    done, skipped = 0, []
    for row in rows:
        address = int(row["address"], 16)
        if address not in index:
            skipped.append((row["address"], "no symbol"))
            continue
        number, current = index[address]
        if current != row["old_name"]:
            skipped.append((row["address"], f"has {current}, expected {row['old_name']}"))
            continue
        lines[number] = row["new_name"] + lines[number][len(current):]
        done += 1
    open(symbols_path, "w", encoding="utf-8", newline="").write(chr(10).join(lines))
    print(f"applied {done} of {len(rows)} reviewed renames from {names_csv}")
    for address, why in skipped:
        print("  not applied:", address, why)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--source", default=os.path.join(REPO, "..", "source", ".build", "dolrecon"))
    ap.add_argument("--out", default=os.path.join(REPO, ".build", "dolrecon"))
    ap.add_argument("--patch", default=os.path.join(REPO, "test_data", "platform", "unidentified_names.symbols.patch"))
    ap.add_argument("--anchor-names", default=os.path.join(REPO, "test_data", "platform", "sdk_anchor_names.csv"),
                    help="reviewed renames (address, old name, new name, grade, evidence)")
    ap.add_argument("--no-names", action="store_true", help="control: do not apply the renames")
    args = ap.parse_args(argv)

    source, out = os.path.abspath(args.source), os.path.abspath(args.out)
    if os.path.normcase(source) == os.path.normcase(out):
        raise SystemExit("--out must not be --source (the shared copy is read-only)")
    src_project, project = os.path.join(source, "pal-v30"), os.path.join(out, "pal-v30")
    for needed in [os.path.join(src_project, "config", "symbols.txt"), os.path.join(src_project, "out", "asm"),
                   os.path.join(source, "skeleton", "functions.csv")]:
        if not os.path.exists(needed):
            raise SystemExit(f"missing {needed}")

    for part in (project, os.path.join(out, "skeleton")):
        if os.path.exists(part):
            shutil.rmtree(part)
    os.makedirs(os.path.join(project, "out"))
    shutil.copytree(os.path.join(src_project, "config"), os.path.join(project, "config"))
    for name in PROJECT_FILES:
        shutil.copy2(os.path.join(src_project, name), os.path.join(project, name))
    shutil.copytree(os.path.join(src_project, "out", "asm"), os.path.join(project, "out", "asm"))
    shutil.copytree(os.path.join(source, "skeleton"), os.path.join(out, "skeleton"))

    if not args.no_names:
        # the patch is relative to the project directory (a/config/symbols.txt)
        subprocess.run(["git", "apply", "--unidiff-zero", "-p1", os.path.abspath(args.patch)], cwd=project, check=True)
        print("applied", os.path.relpath(args.patch, REPO))
        apply_anchor_names(os.path.join(project, "config", "symbols.txt"), args.anchor_names)

    owners = [os.path.join(REPO, "extract", name) for name in ("native-owners.txt", "native-owners-r2.txt")
              if os.path.exists(os.path.join(REPO, "extract", name))]
    env = dict(os.environ, PYTHONPATH=os.path.join(REPO, "tools"))
    subprocess.run([sys.executable, "-B", "-m", "dolrecon.platform_deps", project, "--owners", *owners],
                   cwd=os.path.join(REPO, "tools"), env=env, check=True)
    print("wrote", os.path.join(project, "platform_manifest.json"))


if __name__ == "__main__":
    main()
