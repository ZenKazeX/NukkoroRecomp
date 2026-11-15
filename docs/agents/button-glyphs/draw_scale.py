#!/usr/bin/env python3
"""How the game draws every button cell: for each CSD cast (any scene of any .gncp under csdFiles) whose cell is
one of make_modern_buttons.CELL_ROLE, the cast's quad (canvas px, 640 x 480) against the cell's texels, i.e.
the screen px per texel in x and y, and the animated scale tracks that change it.
    python draw_scale.py [--json out.json]
Pairs with the runtime: the quad is drawn 1:1 (no extra x/y factor) into the canvas, which the native renderer maps
to the output uniformly (see the report); a sheet's txd-only copies (Options_*.txd, pause_*.txd) are drawn by the
same casts of the matching .gncp.
"""
import glob
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', '..', '..', 'tools', 'assets'))
sys.path.insert(0, os.path.join(HERE, '..', '..', '..', 'tools', 'button_layouts'))
from gncp_read import Gncp  # noqa: E402
from make_modern_buttons import CELL_ROLE  # noqa: E402

FILES = 'H:/ShadowTheHedgehogDiskGC_Extract/game/files/'


def match(rect):
    out = []
    for (sid, x0, y0, x1, y1, role) in CELL_ROLE:
        if all(abs(a - b) <= 1.5 for a, b in zip(rect, (x0, y0, x1, y1))):
            out.append((sid, x0, y0, role))
    return out


def main():
    rows = {}
    for path in sorted(glob.glob(FILES + 'csdFiles/**/*.gncp', recursive=True)):
        try:
            g = Gncp(path)
            scenes = g.scenes()
        except Exception as e:  # noqa: BLE001
            print('skip', path, e)
            continue
        for sc in scenes:
            for ci, cast in enumerate(sc['casts']):
                cells = {cast['cell']} | {p for p in cast['patterns'] if p >= 0}
                for cell_i in cells:
                    if not (0 <= cell_i < len(sc['cells'])):
                        continue
                    c = sc['cells'][cell_i]
                    if not (0 <= c['tex'] < len(sc['textures'])):
                        continue
                    tw, th = sc['textures'][c['tex']]
                    rect = (c['u0'] * tw, c['v0'] * th, c['u1'] * tw, c['v1'] * th)
                    for (sid, x0, y0, role) in match(rect):
                        cw, ch = rect[2] - rect[0], rect[3] - rect[1]
                        qw, qh = cast['w'], cast['h']
                        # a cast's own quad is axis aligned unless rotated
                        key = (sid, x0, y0, role, os.path.relpath(path, FILES).replace(os.sep, '/'),
                               sc['name'], cast['name'])
                        rows[key] = dict(sheet=sid, cell=(x0, y0), role=role, file=key[4], scene=sc['name'],
                                         cast=cast['name'], texel=(round(cw, 2), round(ch, 2)),
                                         quad=(round(qw, 2), round(qh, 2)), rot=round(cast['rotation'], 2),
                                         sx=round(qw / cw, 4) if cw else 0, sy=round(qh / ch, 4) if ch else 0)
    out = sorted(rows.values(), key=lambda r: (r['sheet'], r['cell'], r['file']))
    for r in out:
        print('%-17s %-8s %-4s %-34s %-18s %-10s texel %-12s quad %-14s px/texel x %.3f y %.3f ratio %.3f' % (
            r['sheet'], r['cell'], r['role'], r['file'][-34:], r['scene'][:18], r['cast'][:10], r['texel'],
            r['quad'], r['sx'], r['sy'], r['sx'] / r['sy'] if r['sy'] else 0))
    if len(sys.argv) > 2 and sys.argv[1] == '--json':
        json.dump(out, open(sys.argv[2], 'w'), indent=1)


if __name__ == '__main__':
    main()
