#!/usr/bin/env python
"""map_region_ingame.py - compare the placements two headless runs of one stage loaded (stock vs a mod's layout file).

Both runs are `ShadowTheHedgehog.exe --boot-stage <stage> --headless --no-audio --stop-at-tick <T> --state-report <r.json>
--report-peek "0x805EF77C>0:4,0x805EF77C>0x41C>0:<44*count>"` (one game process at a time). 0x805EF77C is the layout
manager's holder (r13 + 0x327C); the manager's +0 is the number of loaded placements and +0x41C the array of 0x2C-byte
entries (big-endian in guest memory). The script decodes the two peeks and prints / writes the placements that only one
run has. Names and numbers only.

  python -I tools/mods/map_region_ingame.py --stock stock/report.json --mod mod/report.json --stage stg0201 [--out f.json]
"""
import argparse
import collections
import json
import struct


def placements(report_path):
    with open(report_path, encoding='utf-8') as f:
        peeks = json.load(f)['peeks']
    count = struct.unpack('>I', bytes.fromhex(peeks['0x805EF77C>0:4']))[0]
    blob = bytes.fromhex(next(v for k, v in peeks.items() if '0x41C' in k))
    if len(blob) < 44 * count:
        raise SystemExit('the entries peek is shorter than 44 x %d bytes' % count)
    out = []
    for i in range(count):
        f = struct.unpack('>6fIIHBBII', blob[44 * i:44 * i + 44])
        out.append((f[8], tuple(round(x, 1) for x in f[0:3]), f[9], f[10]))   # type, pos, link, range
    return count, out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[1])
    ap.add_argument('--stock', required=True)
    ap.add_argument('--mod', required=True)
    ap.add_argument('--stage', default='')
    ap.add_argument('--out')
    args = ap.parse_args()
    n_s, a = placements(args.stock)
    n_m, b = placements(args.mod)
    only_s = collections.Counter(a) - collections.Counter(b)
    only_m = collections.Counter(b) - collections.Counter(a)
    result = {'stage': args.stage, 'loaded_placements_stock': n_s, 'loaded_placements_mod': n_m,
              'only_in_stock': [{'type_id': t, 'pos': list(p), 'link': l, 'range_x100': r, 'count': c} for (t, p, l, r), c in sorted(only_s.items())],
              'only_in_mod': [{'type_id': t, 'pos': list(p), 'link': l, 'range_x100': r, 'count': c} for (t, p, l, r), c in sorted(only_m.items())],
              'setcollision_stock': sum(1 for e in a if e[0] == 0x50), 'setcollision_mod': sum(1 for e in b if e[0] == 0x50)}
    text = json.dumps(result, indent=1)
    if args.out:
        with open(args.out, 'w', encoding='utf-8', newline='\n') as f:
            f.write(text + '\n')
    print(text)


if __name__ == '__main__':
    main()
