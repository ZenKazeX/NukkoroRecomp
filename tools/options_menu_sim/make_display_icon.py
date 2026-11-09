#!/usr/bin/env python3
"""Make a "display settings" icon tile in the style of the Options menu's own six tiles.

    python make_display_icon.py          # after build_sim_data.py

Reads generated/tex/Options_EN__2.png (the Options2 sheet) and generated/data.js (its cells), writes
generated/tex/icon_display.png and generated/tex/icon_display_hi.png (82 x 82, normal and highlighted).

Everything but the symbol's shape is taken from the game's tiles:
  * the blank steel tile is, per pixel, the median of the six normal tiles whose symbol does not cover that
    pixel (a symbol is where its tile and the highlighted twin differ): the steel and rivets they share;
  * the highlighted symbol's red is measured where a normal tile and its highlighted twin differ (the
    symbol); the normal symbol is the steel itself, recessed, as on the game's tiles;
  * the engraving is the tiles' look: a dark outline, the symbol face, a light lower-right inner edge and a
    dark upper-left one.
The plate is then cleaned (clean_plate): frame and rivets kept, the interior's brushed steel rebuilt without the
tiles' grime. The symbol is an old CRT television (screen, knobs, speaker slots, feet; no antennas), matching the game's
era, drawn at 4x and reduced for smooth edges. Also writes generated/tex/icon_blank.png, the clean plate.
"""
import json
import os

from PIL import Image, ImageChops, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
TEX = os.path.join(HERE, 'generated', 'tex')
NORMAL_CELLS = range(41, 47)       # icon_sound .. icn_audio, pattern 0
HIGHLIGHT_OFFSET = 6               # pattern 1 = cell + 6


def load_cells():
    text = open(os.path.join(HERE, 'generated', 'data.js'), encoding='utf-8').read()
    data = json.loads(text[text.index('=') + 1:].rstrip().rstrip(';'))
    menu = next(s for s in data['scenes'] if s['name'] == 'menu')
    return menu['cells']


def crop(sheet, cell):
    w, h = sheet.size
    return sheet.crop((round(cell['u0'] * w), round(cell['v0'] * h),
                       round(cell['u1'] * w), round(cell['v1'] * h))).convert('RGBA')


def symbol_mask(normal, highlight):
    """Where a tile's symbol is: the pixels its highlighted twin changes, grown by 5 px to take the outline and its shadow."""
    m = Image.new('L', normal.size, 0)
    pn, ph, pm = normal.load(), highlight.load(), m.load()
    w, h = normal.size
    for y in range(h):
        for x in range(w):
            if sum(abs(pn[x, y][c] - ph[x, y][c]) for c in range(3)) > 40:
                pm[x, y] = 255
    return m.filter(ImageFilter.MaxFilter(11))


def median_tile(tiles, masks):
    """Per pixel, the upper median over the tiles whose symbol does not cover that pixel."""
    w, h = tiles[0].size
    out = Image.new('RGBA', (w, h))
    px = [t.load() for t in tiles]
    pm = [m.load() for m in masks]
    o = out.load()
    for y in range(h):
        for x in range(w):
            vals = [p[x, y] for p, m in zip(px, pm) if m[x, y] == 0] or [p[x, y] for p in px]
            # The upper median (75th percentile by brightness): the tiles carry grime stains in different places,
            # and the brighter half of the candidates is the clean steel.
            vals.sort(key=lambda v: v[0] + v[1] + v[2])
            o[x, y] = vals[min(len(vals) - 1, (3 * len(vals)) // 4)]
    return out


def symbol_colours(normals, highlights):
    grey, red, n = [0, 0, 0], [0, 0, 0], 0
    for a, b in zip(normals, highlights):
        pa, pb = a.load(), b.load()
        w, h = a.size
        for y in range(h):
            for x in range(w):
                ra, ga, ba, _ = pa[x, y]
                rb, gb, bb, _ = pb[x, y]
                if rb - gb > 60:                     # clearly red in the highlighted twin: symbol face
                    grey = [grey[0] + ra, grey[1] + ga, grey[2] + ba]
                    red = [red[0] + rb, red[1] + gb, red[2] + bb]
                    n += 1
    return tuple(v // n for v in grey), tuple(v // n for v in red)


def tv_mask(size, k=4):
    """The symbol, an old CRT television (no antennas, user 2026-10-28), drawn at k times the tile size; white = symbol."""
    s = size * k
    m = Image.new('L', (s, s), 0)
    d = ImageDraw.Draw(m)
    u = s / 82.0                                     # one tile pixel

    dy = -6                                          # the set (y 24..70) centred on the tile

    def box(x0, y0, x1, y1):
        return [x0 * u, (y0 + dy) * u, x1 * u, (y1 + dy) * u]

    # Cabinet: a thick rounded ring.
    d.rounded_rectangle(box(11, 24, 71, 66), radius=5 * u, fill=255)
    d.rounded_rectangle(box(15, 28, 67, 62), radius=3 * u, fill=0)
    # CRT screen: a bulging rounded rectangle, as a ring.
    d.rounded_rectangle(box(18, 31, 52, 59), radius=7 * u, fill=255)
    d.rounded_rectangle(box(21, 34, 49, 56), radius=5 * u, fill=0)
    # Control panel: two knobs and speaker slots.
    d.ellipse(box(55, 32, 64, 41), fill=255)
    d.ellipse(box(55, 43, 64, 52), fill=255)
    for y in (54.5, 57.5):
        d.line(box(55, y, 64, y), fill=255, width=round(1.5 * u))
    # Feet.
    d.rectangle(box(17, 66, 24, 70), fill=255)
    d.rectangle(box(58, 66, 65, 70), fill=255)
    return m.resize((size, size), Image.LANCZOS)


def clean_plate(tile, seed=7):
    """A clean steel plate: the tile's frame bevel and rivets kept, the inside rebuilt without grime.

    The game's steel is brushed: it varies along the rows (horizontal grain), with a soft light falloff to
    the right and down. Each interior row is rebuilt from that row's clean steel at the left and right
    edges (inside the frame, clear of the rivets), interpolated across, plus faint horizontally streaked
    noise for the grain.
    """
    import random
    w, h = tile.size
    src = tile.convert('RGBA').load()
    out = tile.convert('RGBA').copy()
    o = out.load()
    rnd = random.Random(seed)
    inner0, inner1 = 5, w - 5                        # inside the frame bevel
    rivets = [(7, 7), (w - 8, 7), (7, h - 8), (w - 8, h - 8)]

    def strip_median(xs, y):
        vals = sorted((src[x, y] for x in xs), key=lambda v: v[0] + v[1] + v[2])
        return vals[len(vals) // 2]

    for y in range(inner0, h - 5):
        near_rivet_row = y < 13 or y > h - 14
        left = range(13, 19) if near_rivet_row else range(6, 12)
        right = range(w - 19, w - 13) if near_rivet_row else range(w - 12, w - 6)
        lv, rv = strip_median(left, y), strip_median(right, y)
        # Grain: one smooth random walk per row, so the noise streaks horizontally.
        walk, g = [], 0.0
        for _ in range(w):
            g = 0.85 * g + rnd.uniform(-1.6, 1.6)
            walk.append(g)
        for x in range(inner0, inner1):
            if any((x - rx) ** 2 + (y - ry) ** 2 <= 16 for rx, ry in rivets):
                continue
            t = (x - inner0) / float(inner1 - 1 - inner0)
            o[x, y] = tuple(int(max(0, min(255, lv[c] + (rv[c] - lv[c]) * t + walk[x]))) for c in range(3)) + (255,)
    return out


def engrave(blank, mask, face=None):
    """Composite the symbol onto the tile the way the game's icons are cut into the steel."""
    w, h = blank.size
    ring = ImageChops.subtract(mask.filter(ImageFilter.MaxFilter(3)), mask)          # outline
    shifted_dn = ImageChops.offset(mask, 1, 1)
    shifted_up = ImageChops.offset(mask, -1, -1)
    lit = ImageChops.subtract(mask, shifted_up)          # inner lower-right edge
    shade = ImageChops.subtract(mask, shifted_dn)        # inner upper-left edge
    out = blank.copy()
    po, pm, pr, pl, ps = out.load(), mask.load(), ring.load(), lit.load(), shade.load()
    pb = blank.load()
    for y in range(h):
        for x in range(w):
            r, g, b, a = pb[x, y]
            m = pm[x, y] / 255.0
            # Face: recessed steel (face None) or the measured red, keeping the steel's grain.
            grain = (r + g + b) / 3.0 / 160.0
            if face is None:
                fr, fg, fb = r * 0.82, g * 0.82, b * 0.82
            else:
                fr, fg, fb = (min(255, c * (0.65 + 0.5 * grain)) for c in face)
            r, g, b = (r * (1 - m) + fr * m, g * (1 - m) + fg * m, b * (1 - m) + fb * m)
            o = pr[x, y] / 255.0 * 0.8
            r, g, b = r * (1 - o), g * (1 - o), b * (1 - o)
            li = pl[x, y] / 255.0 * 0.6
            r, g, b = r + (255 - r) * li, g + (255 - g) * li, b + (255 - b) * li
            sh = ps[x, y] / 255.0 * 0.55
            r, g, b = r * (1 - sh), g * (1 - sh), b * (1 - sh)
            po[x, y] = (int(r), int(g), int(b), a)
    return out


def main():
    sheet = Image.open(os.path.join(TEX, 'Options_EN__2.png'))
    cells = load_cells()
    normals = [crop(sheet, cells[c]) for c in NORMAL_CELLS]
    highlights = [crop(sheet, cells[c + HIGHLIGHT_OFFSET]) for c in NORMAL_CELLS]
    blank = clean_plate(median_tile(normals, [symbol_mask(n, h) for n, h in zip(normals, highlights)]))
    grey, red = symbol_colours(normals, highlights)
    mask = tv_mask(blank.size[0])
    engrave(blank, mask).save(os.path.join(TEX, 'icon_display.png'))
    engrave(blank, mask, red).save(os.path.join(TEX, 'icon_display_hi.png'))
    blank.save(os.path.join(TEX, 'icon_blank.png'))
    print('symbol grey %s, red %s -> icon_display.png, icon_display_hi.png' % (grey, red))


if __name__ == '__main__':
    main()
