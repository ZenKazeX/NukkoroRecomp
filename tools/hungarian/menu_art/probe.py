"""Finding text lines in a picture: row runs of a face's fill inside a box, and each line's column runs.

    python -I probe.py <pack> <texture|-> <LANG> <x0> <y0> <x1> <y1> <face>
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))   # python -I drops the script's folder
import common as C
import faces as F
import glyphs as G


def rows(face, pack, tex, lang, box, rotate=0):
    b = G.Band(pack, tex, lang, box, '', rotate)
    img = b.image()
    st = [[face.strength(p) for p in r] for r in img]
    rr = G.runs_of([any(s > face.min_strength for s in r) for r in st])
    out = []
    for y0, y1 in rr:
        colw = [sum(st[y][x] for y in range(y0, y1 + 1) if st[y][x] > face.min_strength) for x in range(len(img[0]))]
        cr = G.runs_of([v > 0 for v in colw])
        out.append((y0 + (box[1] if not rotate else 0), y1 + (box[1] if not rotate else 0), len(cr),
                    (cr[0][0] + box[0], cr[-1][1] + box[0]) if cr else None, [(a, b - a + 1) for a, b in cr]))
    return out


if __name__ == '__main__':
    pack, tex, lang = sys.argv[1], (None if sys.argv[2] == '-' else sys.argv[2]), sys.argv[3]
    box = tuple(int(v) for v in sys.argv[4:8])
    face = F.FACES[sys.argv[8]]
    rot = int(sys.argv[9]) if len(sys.argv) > 9 else 0
    for y0, y1, n, ext, cr in rows(face, pack, tex, lang, box, rot):
        print('rows %3d-%3d  runs %2d  x %s  %s' % (y0, y1, n, ext, cr if len(cr) < 40 else ''))
