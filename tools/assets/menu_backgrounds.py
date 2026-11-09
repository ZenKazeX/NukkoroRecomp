#!/usr/bin/env python3
"""Work out which extracted menu textures are BACKGROUND plates, and write the mapping.

Joins three things:

  * the .gncp project  - which cast covers the canvas, and which cell (and therefore which of the
    project's texture slots) it draws from;
  * the .txd dictionary - the actual texture names and dimensions;
  * the PNGs written by txd_extract.py.

The .gncp does NOT carry texture names - only cast, scene and motion names. The join from a
project's texture slot to a dictionary entry is nevertheless EXACT, because the project's texture
list is the dictionary in REVERSE order:

    dictionary_index = (texture_count - 1) - gncp_texture_slot

Verified on all 60 project/dictionary pairs on the disc: the slot count equals the entry count
every time, and the size sequences match under reversal with zero exceptions. 36 of those pairs
are directional (the sizes differ enough that reverse is the ONLY fit); the other 24 have
palindromic size sequences, which are consistent with the rule but do not independently prove it.
The rule also agrees with the older dimension-matching join on all six unambiguous backgrounds.

The size comparison is retained as an assertion: a slot whose reversed entry has the wrong
dimensions is reported as a mismatch rather than silently accepted.

    python menu_backgrounds.py --textures <dir written by txd_extract.py> [--csd <dir>]

Writes <dir>/background_map.json and <dir>/README.md.
"""
import argparse
import collections
import glob
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gncp_read import Gncp                                        # noqa: E402

CANVAS_W, CANVAS_H = 640.0, 480.0


def coverage(cast):
    w = min(cast['x1'], CANVAS_W) - max(cast['x0'], 0.0)
    h = min(cast['y1'], CANVAS_H) - max(cast['y0'], 0.0)
    if w <= 0 or h <= 0:
        return 0.0
    return (w * h) / (CANVAS_W * CANVAS_H)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--textures', required=True)
    ap.add_argument('--csd', default='H:/ShadowTheHedgehogDiskGC_Extract/files/csdFiles')
    ap.add_argument('--min-coverage', type=float, default=0.90)
    args = ap.parse_args()

    manifest = json.load(open(os.path.join(args.textures, 'manifest.json'), encoding='utf-8'))
    by_txd = collections.defaultdict(list)
    by_index = collections.defaultdict(dict)
    for t in manifest['textures']:
        by_txd[t['source_txd']].append(t)
        by_index[t['source_txd']][t['index']] = t

    # Which dictionaries belong to which project: same stem, ignoring the language suffix.
    def stem_of(p):
        s = os.path.splitext(os.path.basename(p))[0]
        for suf in ('_EN', '_FR', '_GE', '_IT', '_JP', '_SP'):
            if s.endswith(suf):
                return s[:-3]
        return s

    dicts_for = collections.defaultdict(list)
    for txd in by_txd:
        dicts_for[stem_of(txd)].append(txd)

    projects = sorted(glob.glob(os.path.join(args.csd, '*.gncp'))) + \
               sorted(glob.glob(os.path.join(args.csd, '*', '*.gncp')))

    out = []
    for path in projects:
        stem = stem_of(path)
        if stem not in dicts_for:
            continue
        try:
            g = Gncp(path)
            scenes = g.scenes()
        except Exception as exc:                                   # noqa: BLE001
            out.append(dict(project=os.path.basename(path), error=str(exc)))
            continue
        for sc in scenes:
            for cast in sc['casts']:
                cov = coverage(cast)
                if cov < args.min_coverage:
                    continue
                slot = cast['texture']
                dims = sc['textures'][slot] if 0 <= slot < len(sc['textures']) else None
                candidates = []
                mismatch = False
                if dims:
                    for txd in sorted(dicts_for[stem]):
                        entries = by_index[txd]
                        t = entries.get(len(entries) - 1 - slot)
                        if t is None:
                            mismatch = True
                            continue
                        if abs(t['width'] - dims[0]) > 1.5 or abs(t['height'] - dims[1]) > 1.5:
                            mismatch = True
                            continue
                        candidates.append(dict(txd=txd, texture=t['name'],
                                               index=t['index'], png=t['png'],
                                               width=t['width'], height=t['height'],
                                               format=t['format_name'],
                                               has_alpha=t['has_alpha']))
                out.append(dict(
                    project=os.path.relpath(path, args.csd).replace('\\', '/'),
                    scene=sc['name'], cast=cast['name'], group=cast['group'],
                    index=cast['index'],
                    canvas_box=[round(cast['x0'], 1), round(cast['y0'], 1),
                                round(cast['x1'], 1), round(cast['y1'], 1)],
                    coverage=round(cov, 4),
                    textured=(slot >= 0),
                    texture_slot=slot,
                    texture_size=[round(dims[0]), round(dims[1])] if dims else None,
                    candidates=candidates,
                    size_mismatch=mismatch,
                    ambiguous=(mismatch or (dims is not None and not candidates))))

    json.dump({'min_coverage': args.min_coverage, 'backgrounds': out},
              open(os.path.join(args.textures, 'background_map.json'), 'w', encoding='utf-8'),
              indent=2, ensure_ascii=False)

    textured = [b for b in out if b.get('textured')]
    flat = [b for b in out if b.get('textured') is False]
    exact = [b for b in textured if b.get('candidates') and not b.get('ambiguous')]
    print('%d canvas-covering casts across %d projects' % (len(out), len(set(
        b.get('project') for b in out))))
    print('   %d textured (%d resolve to exactly one texture, %d ambiguous by size)'
          % (len(textured), len(exact), len(textured) - len(exact)))
    print('   %d untextured flat-colour plates (no texture to extract)' % len(flat))
    for b in sorted(out, key=lambda r: r.get('project', '')):
        if not b.get('textured'):
            print('   %-30s %-14s %-14s  UNTEXTURED flat plate %s'
                  % (b['project'], b['scene'][:14], b['cast'][:14],
                     'x'.join(str(int(v)) for v in b['texture_size']) if b['texture_size'] else ''))
            continue
        langs = sorted({c['txd'].split('_')[-1].replace('.txd', '') for c in b['candidates']})
        first = b['candidates'][0]
        print('   %-30s %-14s %-14s  %4dx%-4d %-7s%s  %s'
              % (b['project'], b['scene'][:14], b['cast'][:14],
                 first['width'], first['height'], first['format'],
                 ' alpha' if first['has_alpha'] else '      ',
                 'UNRESOLVED' if b['ambiguous'] else first['png']))
        if not b['ambiguous'] and len(langs) > 1:
            print('        (same texture in %d language dictionaries: %s)'
                  % (len(langs), ', '.join(langs)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
