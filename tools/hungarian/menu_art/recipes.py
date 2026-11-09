"""Letters a face's pictures do not have, built from parts of that face's own letters (never redrawn or stretched).

A recipe is fn(glyphs, lib) -> Glyph; it may raise KeyError when a part letter is missing. Part helpers cut a letter's
pixels by columns / rows (in the glyph's own coordinates: x from its left column, y relative to the baseline, negative
up) and paste them at offsets.
"""
import glyphs as G


def part(g, x0=None, x1=None, y0=None, y1=None):
    """Pixels of glyph g with x0 <= x <= x1 and y0 <= y <= y1 (None = open)."""
    return {(x, y): v for (x, y), v in g.pix.items()
            if (x0 is None or x >= x0) and (x1 is None or x <= x1) and (y0 is None or y >= y0) and (y1 is None or y <= y1)}


def paste(*layers, strength=None):
    """layers: (pix, dx, dy) -> merged pix (later layers win where stronger)."""
    out = {}
    for pix, dx, dy in layers:
        for (x, y), v in pix.items():
            k = (x + dx, y + dy)
            if k not in out or strength is None or strength(v) >= strength(out[k]):
                out[k] = v
    return out


def glyph(ch, pix, note, width=None):
    w = width if width is not None else max(x for x, _ in pix) + 1
    g = G.Glyph(ch, pix, w, '', 'drawn', note)
    return g


def mirror(g):
    w = g.width
    return {(w - 1 - x, y): v for (x, y), v in g.pix.items()}


RECIPES = {}


# ---------------------------------------------------------------------------------------------------------- title_press
# The squared techno face of PRESS START: 12-px caps (rows -12..-1), stems 3-4 px with a dithered inner shade.
def _tp_F(g, lib):
    E = g['E']
    # E's top bar, stem and middle bar; below the middle bar only the stem (E's own stem pixels, cols 0..3)
    return glyph('F', paste((part(E, y1=-4), 0, 0), (part(E, x1=3, y0=-3), 0, 0)),
                 'E without its bottom bar (E rows -12..-4 + E stem cols 0-3 below)', E.width)


def _tp_H(g, lib):
    U, A = g['U'], g['A']
    # U's two stems above the bar row of A, A's crossbar and two legs below (both 12 wide, stems at the same columns)
    return glyph('H', paste((part(U, y1=-8), 0, 0), (part(A, y0=-7), 0, 0)),
                 'U rows -12..-8 (two stems) + A rows -7..-1 (crossbar + legs)', max(U.width, A.width))


def _tp_J(g, lib):
    U = g['U']
    # U without the upper left stem: the right stem, the bottom bar and the left hook (rows -4..-1)
    return glyph('J', paste((part(U, x0=4), 0, 0), (part(U, x1=3, y0=-4), 0, 0)),
                 'U without its left stem above row -4 (right stem + bottom bar + hook)', U.width)


def _tp_G(g, lib):
    C, O, A = g['C'], g['O'], g['A']
    # C + O's right stem from the middle down + A's right half crossbar as the inner bar
    return glyph('G', paste((C.pix, 0, 0), (part(O, x0=8, y0=-6), 0, 0), (part(A, x0=6, y0=-7, y1=-6), 0, 0)),
                 "C + O's lower right stem (cols 8-11, rows -6..-1) + A's crossbar right half (cols 6-11)",
                 max(C.width, O.width))


RECIPES['title_press'] = {'F': _tp_F, 'H': _tp_H, 'J': _tp_J, 'G': _tp_G}


# ---------------------------------------------------------------------------------------------------------- prompt
# The lowercase prompt face: x-height rows -9..-1, ascenders to -13, straight 3-row descenders (q, g).
def _pr_p(g, lib):
    q = g['q']
    return glyph('p', mirror(q), 'q mirrored left-right (the face draws q and g with straight descenders)', q.width)


def _pr_y(g, lib):
    u, q = g['u'], g['q']
    return glyph('y', paste((u.pix, 0, 0), (part(q, x0=8, x1=10, y0=0), 0, 0)),
                 "u + q's descender (cols 8-10, rows 0..1) under the right stem", u.width)


def _pr_j(g, lib):
    i, q = g['i'], g['q']
    return glyph('j', paste((i.pix, 0, 0), (part(q, x0=8, x1=10, y0=0), -8, 0)),
                 "i + q's descender (rows 0..1) under the stem", i.width)


def _pr_k(g, lib):
    h, x = g['h'], g['x']
    return glyph('k', paste((part(h, x1=2), 0, 0), (part(x, x0=3), 0, 0)),
                 "h's stem (cols 0-2) + x's right part from col 3 (upper arm, joint, lower arm)", x.width)


RECIPES['prompt'] = {'p': _pr_p, 'y': _pr_y, 'j': _pr_j, 'k': _pr_k}
