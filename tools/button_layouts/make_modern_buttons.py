#!/usr/bin/env python3
"""Modern controller button glyphs for the game, in the style of its own glyphs, plus drop-in copies
of every button sheet with the buttons repainted.

    python make_modern_buttons.py [--game-files <dir>] [--pad-cfg <file>] [--out <dir>]

Run extract_buttons.py first (this reads its atlases). Writes, for each family (xbox, playstation,
switch):
    <out>/<family>/glyphs/<button>.png        256x256 masters (south/east/west/north/start/back)
    <out>/<family>/atlases/<sheet>.png        the original sheet, every button cell repainted
    <out>/_compare.png                        original vs each family, cell by cell

Which modern button a cell shows follows the port's controller binding (game/pad.cfg, written by
the launcher): a cell means a GameCube button (the select prompt is GC A whichever platform pattern
draws it), the binding says which XInput button drives that GC button, and the XInput button's
position (A south, B east, X west, Y north) picks the glyph of each family. Without pad.cfg the
launcher's defaults apply (GC A/B/X/Y <- XInput A/B/X/Y).

The glyphs are drawn here from plain shapes and letters: glossy dark round caps with a light rim,
like the game's own, sized to the alpha box of the glyph they replace. Nothing is copied from any
console maker's artwork.
"""
import argparse
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
FONT = 'C:/Windows/Fonts/arialbd.ttf'
M = 256                      # master size

# What each original cell means, as a GameCube button. Badges painted onto item icons
# (windowitem cross/square badges) are left alone: they are part of the icon art.
CELL_ROLE = [
    # sheet, x0, y0, x1, y1, gc button
    ('gamemenu', 128, 203, 156, 231, 'a'),     # PS2 cross: select
    ('gamemenu', 156, 203, 184, 231, 'x'),
    ('gamemenu', 184, 203, 212, 231, 'y'),     # GC Y: exit
    ('gamemenu', 212, 203, 240, 231, 'a'),     # Xbox A: select
    ('gamemenu', 128, 230, 156, 256, 'y'),     # PS2 triangle: exit
    ('gamemenu', 156, 230, 184, 256, 'a'),     # PS2 circle: select (Japan)
    ('gamemenu', 184, 230, 212, 256, 'a'),     # GC A
    ('gamemenu', 212, 230, 240, 256, 'y'),     # Xbox Y: exit
    ('gamemenu', 229, 177, 256, 202, 'b'),     # red B, no cast
    ('command_icons_gc', 223, 103, 246, 126, 'b'),
    ('command_icons_gc', 223, 48, 246, 71, 'x'),
    ('command_icons_gc', 223, 76, 246, 99, 'a'),
    ('squarebuttons', 0, 32, 32, 64, 'y'),
    ('squarebuttons', 32, 0, 64, 32, 'y'),
    ('squarebuttons', 32, 32, 64, 64, 'y'),
    ('pause_main', 172, 99, 198, 120, 'y'),
    ('pause_main', 200, 99, 226, 120, 'y'),
    ('pause_main', 229, 99, 255, 120, 'y'),
    ('VHArrow_gc', 48, 0, 84, 36, 'x'),
    ('VHArrow_gc', 2, 0, 44, 42, 'x_mask'),    # circlebg: the X button's white silhouette, drawn behind it
    ('Synopsis', 0, 25, 35, 53, 'start'),
    ('Synopsis', 36, 25, 81, 53, 'start'),
    ('Synopsis', 82, 27, 128, 55, 'start'),
    ('windowitem', 44, 44, 84, 84, 'b'),       # BbuttonWindow: the B-button window
]
# Cells whose glyph sits on a framed panel: the panel stays and the new cap (this size, centred)
# covers the old round button (its rim spans x 4..35 of the 40 px cell).
KEEP_PANEL = {('windowitem', 44, 44): 36}
# Outline cells made from their cap. VHArrow.gncp draws cell 0 (circlebg, 2,0-44,42) 46.2 x 42 behind the button
# cell (48,0-84,36) drawn 36 x 33.4, both centred on the same screen point, at 20 % white: an outline round the
# button. On the disc it is the GC X kidney grown by 3.2 screen px on every side (measured: margins 3.55 / 3.15 /
# 3.19 / 3.19, 95th percentile of the width 3.23). A disc sized to the old kidney sat off centre round the modern cap
# (the cap sits off its cell's centre, and the two casts stretch differently), and the user wants the outline kept
# (2026-10-29). So the outline is the cap's own opaque shape carried through both casts' screen geometry and grown
# by the same 3.2 screen px: even all round, wherever the cap is.
# Cells a cast draws stretched (screen size per texel, x over y), from the CSD projects: VHArrow.gncp's button draws
# its 36 x 36 cell 36 x 33.5 (1.075), Synopsis.gncp's skip_btn its 45 x 28 GC cell 46 x 29 (0.987); every other button
# cast draws its cell 1:1. A cap painted round in such a cell shows squashed (user, 2026-10-29), so it is painted
# 1 / stretch wide for its height: round on screen.
CAST_STRETCH = {('VHArrow_gc', 48, 0): 36.0 / 33.5, ('Synopsis', 36, 25): (46.0 / 45.0) / (29.0 / 28.0)}
#   outline cell -> (the cap's cell, the outline cast's screen size, the cap cast's screen size, width in screen px)
MASK_OF = {('VHArrow_gc', 2, 0): (('VHArrow_gc', 48, 0, 84, 36, 'x'), (46.2, 42.0), (36.0, 33.4), 3.2)}

XINPUT = {0x1000: 'south', 0x2000: 'east', 0x4000: 'west', 0x8000: 'north', 0x0010: 'start',
          0x0020: 'back'}
PAD_DEFAULTS = {'a': 0x1000, 'b': 0x2000, 'x': 0x4000, 'y': 0x8000, 'start': 0x0010}

# Per family: how each position looks. kind 'letter' draws text, 'symbol' a shape.
FAMILIES = {
    'xbox': {
        'body': ((70, 74, 80), (16, 17, 19)),
        'south': ('letter', 'A', (108, 196, 74)),
        'east': ('letter', 'B', (228, 66, 58)),
        'west': ('letter', 'X', (62, 140, 232)),
        'north': ('letter', 'Y', (246, 196, 40)),
        'start': ('menu', 'MENU', (235, 235, 235)),
        'back': ('view', 'VIEW', (235, 235, 235)),        # two overlapping windows
    },
    'playstation': {
        'body': ((64, 66, 72), (12, 12, 14)),
        'south': ('symbol', 'cross', (124, 170, 236)),
        'east': ('symbol', 'circle', (236, 92, 104)),
        'west': ('symbol', 'square', (230, 140, 200)),
        'north': ('symbol', 'triangle', (70, 210, 170)),
        'start': ('menu', 'OPTIONS', (235, 235, 235)),
        'back': ('create', 'CREATE', (235, 235, 235)),    # three lines fanning upward
    },
    'switch': {
        'body': ((86, 88, 94), (26, 27, 30)),
        'south': ('letter', 'B', (245, 245, 245)),
        'east': ('letter', 'A', (245, 245, 245)),
        'west': ('letter', 'Y', (245, 245, 245)),
        'north': ('letter', 'X', (245, 245, 245)),
        'start': ('plus', '', (245, 245, 245)),
        'back': ('minus', '', (245, 245, 245)),
    },
}


def read_binding(path):
    bind = dict(PAD_DEFAULTS)
    if path and os.path.exists(path):
        for line in open(path, encoding='utf-8', errors='replace'):
            m = re.match(r'\s*(a|b|x|y|start)\s*=\s*(0x[0-9a-fA-F]+|\d+)', line)
            if m:
                bind[m.group(1)] = int(m.group(2), 0)
    return {gc: XINPUT.get(mask, gc) for gc, mask in bind.items()}


def cap(family, symbol_kind, value, colour, size=M):
    """One round button: drop shadow, dark rim, radial body, gloss, then the mark."""
    from PIL import Image, ImageDraw, ImageFilter, ImageFont
    s = size * 4                                   # supersample
    im = Image.new('RGBA', (s, s), (0, 0, 0, 0))
    c = s / 2
    r_out, r_body = s * 0.47, s * 0.41
    sh = Image.new('L', (s, s), 0)
    ImageDraw.Draw(sh).ellipse((c - r_out, c - r_out + s * 0.02, c + r_out, c + r_out + s * 0.02), fill=150)
    sh = sh.filter(ImageFilter.GaussianBlur(s * 0.015))
    im.paste((0, 0, 0, 255), (0, 0), sh)
    d = ImageDraw.Draw(im)
    d.ellipse((c - r_out, c - r_out, c + r_out, c + r_out), fill=(8, 8, 9, 255))
    # light bevel ring, as on the game's PS2/Xbox caps
    ring = s * 0.025
    d.ellipse((c - r_out + ring, c - r_out + ring, c + r_out - ring, c + r_out - ring), fill=(150, 154, 160, 255))
    hi, lo = FAMILIES[family]['body']
    steps = 48
    for k in range(steps):                           # radial gradient, light from upper left
        t = k / (steps - 1)
        rr = r_body * (1 - t * 0.92)
        col = tuple(int(lo[j] + (hi[j] - lo[j]) * t) for j in range(3))
        ox, oy = -s * 0.06 * t, -s * 0.07 * t
        d.ellipse((c + ox - rr, c + oy - rr, c + ox + rr, c + oy + rr), fill=col + (255,))
    mark(im, symbol_kind, value, colour)
    gloss = Image.new('L', (s, s), 0)
    ImageDraw.Draw(gloss).ellipse((c - r_body * 0.78, c - r_body * 0.92, c + r_body * 0.78, c + r_body * 0.05), fill=70)
    gloss = gloss.filter(ImageFilter.GaussianBlur(s * 0.03))
    im.alpha_composite(Image.merge('RGBA', (Image.new('L', (s, s), 255),) * 3 + (gloss,)))
    return im.resize((size, size), Image.LANCZOS)


def mark(im, kind, value, colour):
    from PIL import Image, ImageDraw, ImageFont
    s = im.width
    c = s / 2
    layer = Image.new('RGBA', (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    w = int(s * 0.07)
    if kind == 'letter':
        f = ImageFont.truetype(FONT, int(s * 0.50))
        d.text((c, c + s * 0.01), value, font=f, fill=colour + (255,), anchor='mm',
               stroke_width=int(s * 0.018), stroke_fill=(0, 0, 0, 255))
    elif kind == 'symbol':
        q = s * 0.19
        if value == 'cross':
            for a, b in (((c - q, c - q), (c + q, c + q)), ((c - q, c + q), (c + q, c - q))):
                d.line((a, b), fill=colour + (255,), width=w)
        elif value == 'circle':
            d.ellipse((c - q * 1.05, c - q * 1.05, c + q * 1.05, c + q * 1.05), outline=colour + (255,), width=w)
        elif value == 'square':
            d.rectangle((c - q * 0.95, c - q * 0.95, c + q * 0.95, c + q * 0.95), outline=colour + (255,), width=w)
        elif value == 'triangle':
            t = q * 1.15
            pts = [(c, c - t), (c + t * 0.98, c + t * 0.72), (c - t * 0.98, c + t * 0.72)]
            d.line(pts + [pts[0], pts[1]], fill=colour + (255,), width=w, joint='curve')
    elif kind == 'menu':                            # three bars (menu / options)
        q = s * 0.17
        for k in (-1, 0, 1):
            y = c + k * q * 0.75
            d.rounded_rectangle((c - q, y - w * 0.45, c + q, y + w * 0.45), radius=w * 0.45, fill=colour + (255,))
    elif kind == 'view':                            # back: two overlapping windows
        q = s * 0.19
        lw = max(2, int(w * 0.6))
        d.rounded_rectangle((c - q * 0.55, c - q * 1.0, c + q * 1.15, c + q * 0.45), radius=q * 0.2,
                            outline=colour + (255,), width=lw)
        d.rounded_rectangle((c - q * 1.15, c - q * 0.45, c + q * 0.55, c + q * 1.0), radius=q * 0.2,
                            fill=colour + (255,))
    elif kind == 'create':                          # back: three short strokes fanning upward
        import math
        q = s * 0.26
        sw = int(w * 0.8)
        for ang in (-50, 0, 50):
            a = math.radians(ang)
            x0, y0 = c + math.sin(a) * q * 0.62, c + q * 0.62 - math.cos(a) * q * 0.62
            x1, y1 = c + math.sin(a) * q * 1.32, c + q * 0.62 - math.cos(a) * q * 1.32
            d.line(((x0, y0), (x1, y1)), fill=colour + (255,), width=sw)
            for x, y in ((x0, y0), (x1, y1)):
                d.ellipse((x - sw / 2, y - sw / 2, x + sw / 2, y + sw / 2), fill=colour + (255,))
    elif kind == 'minus':
        q = s * 0.19
        d.rounded_rectangle((c - q, c - w * 0.6, c + q, c + w * 0.6), radius=w * 0.3, fill=colour + (255,))
    elif kind == 'plus':
        q = s * 0.19
        d.rounded_rectangle((c - q, c - w * 0.6, c + q, c + w * 0.6), radius=w * 0.3, fill=colour + (255,))
        d.rounded_rectangle((c - w * 0.6, c - q, c + w * 0.6, c + q), radius=w * 0.3, fill=colour + (255,))
    im.alpha_composite(layer)


def glyph(family, position, size):
    kind, value, colour = FAMILIES[family][position]
    return cap(family, kind, value, colour, size)


def start_cell(family, w, h, position='start'):
    """Start prompt: a small cap with the family's label beside or under it, as the game's START."""
    from PIL import Image, ImageDraw, ImageFont
    kind, label, colour = FAMILIES[family][position]
    s = 8
    big = Image.new('RGBA', (w * s, h * s), (0, 0, 0, 0))
    capsz = int(min(h, w) * 0.62) * s
    big.alpha_composite(cap(family, kind, label, colour, capsz), ((w * s - capsz) // 2, 0))
    if label:
        d = ImageDraw.Draw(big)
        size = int(h * s * 0.30)
        f = ImageFont.truetype(FONT, size)
        while d.textlength(label, font=f) > w * s * 0.96 and size > 8:
            size -= 4
            f = ImageFont.truetype(FONT, size)
        d.text((w * s / 2, h * s - 2), label, font=f, fill=(250, 250, 250, 255), anchor='md',
               stroke_width=int(s * 0.9), stroke_fill=(0, 0, 0, 255))
    return big.resize((w, h), Image.LANCZOS)


def alpha_box(im):
    box = im.getchannel('A').point(lambda v: 255 if v > 24 else 0).getbbox()
    return box or (0, 0, im.width, im.height)


def scaled_rect(x0, y0, x1, y1, num, den):
    """A sheet rectangle on the sheet scaled by num/den: start rounded down, end rounded up (the
    runtime's button_glyphs.cpp sharp_rect is the same rule)."""
    return x0 * num // den, y0 * num // den, -(-x1 * num // den), -(-y1 * num // den)


_scaled_sheets = {}


def silhouette_cell(sheet, cell, family, position, scale):
    """A MASK_OF cell: white, its alpha the cap's shape (painted at the same scale) carried through the two casts'
    screen geometry and grown by the outline width on screen. A pixel at local o (scaled px) of this cell is sheet
    texel t = (rect start + o) / s, screen (t - cell centre) * (outline screen / cell size), and so cap texel
    (cap centre + screen / (cap screen / cap cell size)): an affine map. Worked at 4 x 4 samples per pixel and
    averaged down, so the outline's edge is anti-aliased."""
    import numpy as np
    from PIL import Image
    sid, x0, y0, x1, y1, role = cell
    cap_cell, (msw, msh), (csw, csh), width = MASK_OF[(sid, x0, y0)]
    num, den = scale
    s = num / den
    ss = 4
    cap = paint_cell(sheet, cap_cell, family, position, scale)
    _c, cx0, cy0, cx1, cy1, _r = cap_cell
    sx0, sy0, sx1, sy1 = scaled_rect(x0, y0, x1, y1, num, den)
    cs0x, cs0y, _e, _f = scaled_rect(cx0, cy0, cx1, cy1, num, den)
    kx = (msw / (x1 - x0)) / (csw / (cx1 - cx0))     # cap texels per outline texel, x
    ky = (msh / (y1 - y0)) / (csh / (cy1 - cy0))
    mcx, mcy = (x0 + x1) / 2, (y0 + y1) / 2
    ccx, ccy = (cx0 + cx1) / 2, (cy0 + cy1) / 2
    # cap local = o * k + s * cc + (rect start - s * mc) * k - cap rect start; o in sub-samples is o_ss / ss
    bx = s * ccx + (sx0 - s * mcx) * kx - cs0x
    by = s * ccy + (sy0 - s * mcy) * ky - cs0y
    w, h = sx1 - sx0, sy1 - sy0
    shape = cap.getchannel('A').point(lambda v: 255 if v >= 128 else 0)
    inside = np.array(shape.transform((w * ss, h * ss), Image.AFFINE, (kx / ss, 0, bx, 0, ky / ss, by),
                                      resample=Image.NEAREST)) > 0
    # Grow by `width` screen px: an ellipse in sub-samples (one sub-sample is (1 / s / ss) texel of this cell).
    rx = width / ((1.0 / s / ss) * msw / (x1 - x0))
    ry = width / ((1.0 / s / ss) * msh / (y1 - y0))
    grown = inside.copy()
    for dy in range(-int(ry), int(ry) + 1):
        for dx in range(-int(rx), int(rx) + 1):
            if (dx / rx) ** 2 + (dy / ry) ** 2 > 1.0 or (dx == 0 and dy == 0):
                continue
            shifted = np.zeros_like(inside)
            ys = slice(max(dy, 0), inside.shape[0] + min(dy, 0))
            yd = slice(max(-dy, 0), inside.shape[0] + min(-dy, 0))
            xs = slice(max(dx, 0), inside.shape[1] + min(dx, 0))
            xd = slice(max(-dx, 0), inside.shape[1] + min(-dx, 0))
            shifted[yd, xd] = inside[ys, xs]
            grown |= shifted
    alpha = grown.reshape(h, ss, w, ss).mean(axis=(1, 3))
    alpha = Image.fromarray(np.round(alpha * 255).astype(np.uint8), 'L')
    return Image.merge('RGBA', (Image.new('L', alpha.size, 255),) * 3 + (alpha,))


def paint_cell(sheet, cell, family, position, scale=(1, 1)):
    """The cell `cell` (a CELL_ROLE row) of `sheet` with its button replaced by `family`'s button at
    `position` (south / east / west / north / start), as an RGBA image the size of the cell. The old
    glyph is cleared, except on a KEEP_PANEL cell, where the new cap covers it on its panel.
    scale = (num, den) paints the cell of the sheet scaled by num/den (scaled_rect), the glyph drawn
    at that resolution."""
    from PIL import Image
    sid, x0, y0, x1, y1, role = cell
    x1, y1 = min(x1, sheet.width), min(y1, sheet.height)
    num, den = scale
    if (sid, x0, y0) in MASK_OF:
        return silhouette_cell(sheet, cell, family, position, scale)
    if (num, den) != (1, 1):
        key = (id(sheet), num, den)
        if key not in _scaled_sheets:
            _scaled_sheets[key] = (sheet, sheet.resize((sheet.width * num // den, sheet.height * num // den),
                                                       Image.BICUBIC))
        big = _scaled_sheets[key][1]
        sx0, sy0, sx1, sy1 = scaled_rect(x0, y0, x1, y1, num, den)
        w, h = sx1 - sx0, sy1 - sy0
        orig = big.crop((sx0, sy0, sx1, sy1))
        box = alpha_box(sheet.crop((x0, y0, x1, y1)))
        # the old glyph's box, in the scaled cell's pixels
        box = tuple(v * num / den for v in box)
    else:
        w, h = x1 - x0, y1 - y0
        orig = sheet.crop((x0, y0, x1, y1))
        box = alpha_box(orig)
    keep = KEEP_PANEL.get((sid, x0, y0))
    if keep:
        keep = int(round(keep * num / den))
    out = orig.copy() if keep else Image.new('RGBA', (w, h), (0, 0, 0, 0))
    stretch = CAST_STRETCH.get((sid, x0, y0), 1.0)
    if role == 'start':
        if stretch != 1.0:     # laid out round on a stretch x w canvas, then fitted to the cell: x by 1 / stretch
            out.alpha_composite(start_cell(family, int(round(w * stretch)), h).resize((w, h), Image.LANCZOS))
        else:
            out.alpha_composite(start_cell(family, w, h))
        return out
    bx0, by0, bx1, by1 = box
    bw, bh = bx1 - bx0, by1 - by0
    # never larger than the cell, so nothing is cut off at its edge
    size = keep or min(int(round(max(bw, bh))), w, h)
    if keep:
        bx0, by0, bw, bh = 0, 0, w, h
    g = glyph(family, position, size)
    if role.endswith('_mask'):    # white silhouette, same footprint
        a = g.getchannel('A')
        g = Image.merge('RGBA', (Image.new('L', g.size, 255),) * 3 + (a,))
    if stretch != 1.0:            # 1 / stretch wide for its height, within the same size
        gw, gh = (int(round(size / stretch)), size) if stretch > 1.0 else (size, int(round(size * stretch)))
        g = glyph(family, position, 4 * size).resize((gw, gh), Image.LANCZOS)
    gw, gh = g.size
    gx = int(round(bx0 + bw / 2 - gw / 2))
    gy = int(round(by0 + bh / 2 - gh / 2))
    gx = max(0, min(gx, w - gw))
    gy = max(0, min(gy, h - gh))
    out.alpha_composite(g.crop((0, 0, min(gw, w - gx), min(gh, h - gy))), (gx, gy))
    return out


def main():
    ap = argparse.ArgumentParser()
    root = 'H:/ShadowTheHedgehogDiskGC_Extract'
    ap.add_argument('--pad-cfg', default=root + '/game/pad.cfg')
    ap.add_argument('--original', default=root + '/exports/button-layouts/original')
    ap.add_argument('--out', default=root + '/exports/button-layouts/modern')
    args = ap.parse_args()
    from PIL import Image, ImageDraw

    pos = read_binding(args.pad_cfg)
    pos['x_mask'] = pos['x']
    print('GC button -> controller position:', pos)
    sheets = {}
    for f in os.listdir(os.path.join(args.original, 'atlases')):
        sheets[f[:-4]] = Image.open(os.path.join(args.original, 'atlases', f)).convert('RGBA')

    compare_rows = []
    for family in FAMILIES:
        gdir = os.path.join(args.out, family, 'glyphs')
        adir = os.path.join(args.out, family, 'atlases')
        os.makedirs(gdir, exist_ok=True)
        os.makedirs(adir, exist_ok=True)
        for p in ('south', 'east', 'west', 'north', 'start', 'back'):
            glyph(family, p, M).save(os.path.join(gdir, p + '.png'))
        for p in ('start', 'back'):                  # cap + label, the shape of the game's START cell
            start_cell(family, M, M * 5 // 8, p).save(os.path.join(gdir, p + '_labelled.png'))
        row = []
        for sid, sheet in sheets.items():
            cells = [c for c in CELL_ROLE if c[0] == sid]
            if not cells:
                continue
            out = sheet.copy()
            for cell in cells:
                _sid, x0, y0, x1, y1, role = cell
                x1, y1 = min(x1, out.width), min(y1, out.height)
                orig = sheet.crop((x0, y0, x1, y1))
                position = 'start' if role == 'start' else pos[role.replace('_mask', '')]
                painted = paint_cell(sheet, cell, family, position)
                out.paste(painted, (x0, y0))
                if role != 'x_mask':
                    row.append((orig, painted))
            out.save(os.path.join(adir, sid + '.png'))
        compare_rows.append((family, row))

    # Original vs modern, cell by cell, at 3x on the menu's dark blue.
    scale, ch, lw, gap = 3, 140, 110, 8
    widths = [o.width * scale + gap for o, _ in compare_rows[0][1]]
    img = Image.new('RGBA', (lw + sum(widths), ch * (1 + len(compare_rows))), (40, 44, 70, 255))
    d = ImageDraw.Draw(img)
    rows = [('original', [o for o, _ in compare_rows[0][1]])] + [(f, [m for _, m in r]) for f, r in compare_rows]
    for r, (name, cells) in enumerate(rows):
        d.text((8, r * ch + ch // 2 - 6), name, fill=(255, 255, 255, 255))
        x = lw
        for im, w in zip(cells, widths):
            big = im.resize((im.width * scale, im.height * scale), Image.NEAREST)
            img.alpha_composite(big, (x, r * ch + (ch - big.height) // 2))
            x += w
    img.save(os.path.join(args.out, '_compare.png'))

    # The masters side by side.
    from PIL import ImageFont
    sheet = Image.new('RGBA', (6 * 140 + 120, len(FAMILIES) * 150), (40, 44, 70, 255))
    d = ImageDraw.Draw(sheet)
    f = ImageFont.truetype(FONT, 14)
    for r, family in enumerate(FAMILIES):
        d.text((10, r * 150 + 65), family, font=f, fill=(255, 255, 255, 255))
        for i, p in enumerate(('south', 'east', 'west', 'north', 'start', 'back')):
            g = Image.open(os.path.join(args.out, family, 'glyphs', p + '.png')).resize((112, 112), Image.LANCZOS)
            sheet.alpha_composite(g, (120 + i * 140, r * 150 + 8))
            d.text((120 + i * 140 + 56, r * 150 + 132), p, font=f, fill=(255, 255, 160, 255), anchor='mm')
    sheet.save(os.path.join(args.out, '_modern_sets.png'))
    print('modern sets ->', args.out)


if __name__ == '__main__':
    main()
