"""Measure a lift candidate's boundary in the game's static call graph (docs/lift-20261006).

  boundary.py --seeds 0x80382794,0x80382960 [--seed-file seeds.txt] [--stop 0x80370000-0x80380000]
              [--include-callees] [--json out.json]

A subsystem is a set of guest functions. Start from the seeds; with --include-callees, add every function the
seeds reach through direct calls (extract/functions.csv `callees`) that is called ONLY from inside the growing
set (a callee also called from outside stays outside: it is shared, and lifting it is a separate decision).
Reported:
  * the subsystem: functions, instructions;
  * inbound edges: (outside caller -> subsystem entry), grouped by entry - every entry needs a boundary
    adapter while those callers remain translated;
  * outbound edges: (subsystem function -> outside callee) - calls the rewritten code would have to make back
    into translated code (avoid);
  * address-taken entries: subsystem functions whose address appears in the census / data words (reached
    through pointers: callbacks, vtables) - inbound edges the static call graph cannot see.
Direct calls only: indirect calls (bctrl through vtables / function pointers) are invisible here; the
address-taken list is the evidence that they exist."""
from __future__ import annotations

import argparse
import csv
import json
import re
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def parse_address(text: str) -> int:
    return int(text, 16) if text.lower().startswith('0x') else int(text)


def load_graph():
    rows = list(csv.DictReader(open(ROOT / 'extract/functions.csv', encoding='utf-8', errors='replace')))
    info = {}
    callees = defaultdict(set)
    callers = defaultdict(set)
    for r in rows:
        a = parse_address(r['addr_hex'])
        info[a] = {'name': r['name'], 'size': int(r['size'] or 0), 'ins': int(r['ins'] or 0)}
        for c in r['callees'].split():
            c = parse_address(c)
            callees[a].add(c)
            callers[c].add(a)
    return info, callees, callers


def address_taken():
    """Function addresses that appear as data (the liveness roots file lists every runtime literal and census
    root; extract/analysis.json may list data-word code pointers)."""
    taken = set()
    roots = ROOT / 'extract/liveness-roots.txt'
    if roots.exists():
        for line in roots.read_text(encoding='utf-8').splitlines():
            m = re.match(r'\s*(0x[0-9A-Fa-f]{8})', line)
            if m:
                taken.add(int(m.group(1), 16))
    return taken


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--seeds', default='')
    p.add_argument('--seed-file', type=Path)
    p.add_argument('--range', action='append', default=[],
                   help='lo-hi: every function starting in [lo, hi) is a seed')
    p.add_argument('--include-callees', action='store_true')
    p.add_argument('--stop', action='append', default=[], help='address range lo-hi never pulled in')
    p.add_argument('--json', type=Path)
    a = p.parse_args()
    info, callees, callers = load_graph()
    seeds = {parse_address(s) for s in a.seeds.split(',') if s.strip()}
    if a.seed_file:
        for line in a.seed_file.read_text(encoding='utf-8').splitlines():
            line = line.split('#')[0].strip()
            if line:
                seeds.add(parse_address(line.split()[0]))
    for r in a.range:
        lo, hi = (parse_address(x) for x in r.split('-'))
        seeds |= {f for f in info if lo <= f < hi}
    unknown = sorted(s for s in seeds if s not in info)
    if unknown:
        raise SystemExit('not function starts: ' + ', '.join(f'0x{s:08X}' for s in unknown))
    stops = []
    for s in a.stop:
        lo, hi = s.split('-')
        stops.append((parse_address(lo), parse_address(hi)))
    blocked = lambda f: any(lo <= f < hi for lo, hi in stops)

    members = set(seeds)
    if a.include_callees:
        changed = True
        while changed:
            changed = False
            for f in list(members):
                for c in callees[f]:
                    if c in members or c not in info or blocked(c):
                        continue
                    if callers[c] <= members:   # called only from inside
                        members.add(c)
                        changed = True

    inbound = defaultdict(set)
    outbound = defaultdict(set)
    for f in members:
        for c in callers[f]:
            if c not in members:
                inbound[f].add(c)
        for c in callees[f]:
            if c not in members:
                outbound[f].add(c)
    taken = address_taken() & members
    out = {
        'functions': len(members),
        'instructions': sum(info[f]['ins'] for f in members),
        'entries_called_from_outside': len(inbound),
        'outside_callers': len(set().union(*inbound.values())) if inbound else 0,
        'outbound_targets': len(set().union(*outbound.values())) if outbound else 0,
        'address_taken_members': len(taken),
        'inbound': {f'0x{f:08X}': sorted(f'0x{c:08X}' for c in cs) for f, cs in sorted(inbound.items())},
        'outbound': {f'0x{f:08X}': sorted(f'0x{c:08X}' for c in cs) for f, cs in sorted(outbound.items())},
        'address_taken': sorted(f'0x{f:08X}' for f in taken),
        'members': sorted(f'0x{f:08X}' for f in members),
    }
    summary = {k: v for k, v in out.items() if not isinstance(v, (dict, list))}
    print(json.dumps(summary, indent=1))
    if a.json:
        a.json.write_text(json.dumps(out, indent=1), encoding='utf-8')


if __name__ == '__main__':
    main()
