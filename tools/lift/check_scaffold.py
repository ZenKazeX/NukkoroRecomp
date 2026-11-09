"""Scaffolding inventory (docs/lift-20261006/R2_DRIVER.md 2.1; docs/NORTH_STAR.md: scaffolding exists only to delete
itself).

Every source file under runtime/scaffold/ is temporary code that lets translated callers reach the native systems
before those callers are lifted. Each must say, in its first 10 lines, what it is and which step deletes it:

    SCAFFOLDING (R2): <what it does> - deleted by <step>: <which lift removes its callers>

<step> is one of the known steps (R4, R5, R6, T1, T2, T3, R1). The check lists the scaffolding with its deleting
step and line count (the debt), and fails on a file without the marker, with an unknown step, or with the marker
naming the wrong owning step format.

  check_scaffold.py [--root runtime/scaffold]
"""
from __future__ import annotations

import argparse
import collections
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
STEPS = {'R1', 'R4', 'R5', 'R6', 'T1', 'T2', 'T3'}
MARKER = re.compile(r'SCAFFOLDING \((R\d|T\d)\): (.+?) - deleted by (R\d|T\d)(?:\s*/\s*(?:R\d|T\d))*: (.+)')


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument('--root', type=Path, default=ROOT / 'runtime/scaffold')
    a = p.parse_args()
    if not a.root.exists():
        print(f'scaffold inventory: {a.root} does not exist - no scaffolding')
        return 0
    problems = []
    debt = collections.defaultdict(lambda: [0, 0])
    for path in sorted(a.root.rglob('*')):
        if path.suffix not in ('.h', '.hpp', '.cpp', '.inl'):
            continue
        lines = path.read_text(encoding='utf-8', errors='replace').splitlines()
        marker = None
        for line in lines[:10]:
            marker = MARKER.search(line)
            if marker:
                break
        shown = path.relative_to(ROOT) if path.is_relative_to(ROOT) else path
        if not marker:
            problems.append(f'{shown}: no "SCAFFOLDING (step): what - deleted by step: why" marker in its first lines')
            continue
        deleting = marker.group(3)
        if deleting not in STEPS:
            problems.append(f'{shown}: deleted by unknown step {deleting}')
            continue
        debt[deleting][0] += 1
        debt[deleting][1] += len(lines)
    if problems:
        print(f'scaffold inventory: {len(problems)} problem(s):')
        for problem in problems:
            print('  ' + problem)
        return 1
    total_files = sum(v[0] for v in debt.values())
    total_lines = sum(v[1] for v in debt.values())
    print(f'scaffold inventory: {total_files} file(s), {total_lines} line(s) of scaffolding')
    for step in sorted(debt):
        print(f'  deleted by {step}: {debt[step][0]} file(s), {debt[step][1]} line(s)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
