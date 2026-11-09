#!/usr/bin/env python3
"""Make a "display settings" icon tile in the style of the Options menu's own six tiles.

    python make_display_icon.py          # after build_sim_data.py

Reads generated/tex/Options_EN__2.png (the Options2 sheet) and generated/data.js (its cells), writes
generated/tex/icon_display.png and generated/tex/icon_display_hi.png (82 x 82, normal and highlighted) and the same
pair as exports/options-menu-textures/new-display-tile/display_tv{,_selected}.png (emit_display_tile_inc.py's input).

Second version (user, 2026-10-30: the first was "too clean and sticks out like a sore thumb"; the user's example is a
worn steel plate with a deeply cut CRT set). Everything but the symbol's shape is taken from the game's tiles:
  * the plate is a real tile's, wear and all: the Load tile (and, for the selected state, its selected twin, whose
    whole plate the game draws lighter), its own symbol filled row by row from the same tile's brushed steel
    (worn_plate);
  * the symbol's face is the game's measured normal grey and selected red (symbol_colours), carrying the plate's
    brushing and stains plus a grime field;
  * the cut is lit from a height field as the game's are, inside the symbol only: dark rim, shadowed upper-left
    and lit lower-right walls (lit towards the face's own highlight), shallower inset lines, the knobs' turned
    steel cores standing proud.
The symbol (tv_layers): the example's set - cabinet with an inset line, screen ring with an inset line, two knobs,
three speaker slots, feet, no antennas - with strokes as thick as the game's symbols, drawn at 4x and reduced.
"""
import json
import os

from PIL import Image, ImageChops, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
TEX = os.path.join(HERE, 'generated', 'tex')
ART = os.path.normpath(os.path.join(HERE, '..', '..', '..', 'exports', 'options-menu-textures', 'new-display-tile'))
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




K = 4                                                # the symbol is drawn at K x the tile and reduced


def tv_layers(size):
    """The symbol, after the user's example of 2026-10-30 (an engraved CRT set, no antennas), at K x the tile:
    (cut, line, core) masks, white = present. `cut` is the deep channel (cabinet frame, screen ring, knob rings,
    speaker slots, feet), `line` the thin inset lines cut inside the cabinet and the screen, `core` the raised steel
    discs of the knobs. Strokes are as thick as the game's own symbols (4-6 px at 82 px)."""
    s = size * K
    u = s / 82.0
    dy = -2.0                                        # the set spans y 19..68: centred on the plate's face

    def box(x0, y0, x1, y1):
        return [x0 * u, (y0 + dy) * u, x1 * u, (y1 + dy) * u]

    def ring(d, b, radius, width):
        d.rounded_rectangle(box(*b), radius=radius * u, fill=255)
        x0, y0, x1, y1 = b
        d.rounded_rectangle(box(x0 + width, y0 + width, x1 - width, y1 - width), radius=max(0.5, radius - width) * u,
                            fill=0)

    cut = Image.new('L', (s, s), 0)
    d = ImageDraw.Draw(cut)
    ring(d, (9, 19, 73, 62), 6, 4.6)                 # cabinet
    d.rectangle(box(16, 61, 24, 68), fill=255)       # feet
    d.rectangle(box(58, 61, 66, 68), fill=255)
    ring(d, (17, 27, 49, 54), 6, 4.0)                # screen
    for cy in (32.5, 42.5):                          # knob rings
        d.ellipse(box(52, cy - 4.6, 61.2, cy + 4.6), fill=255)
        d.ellipse(box(54.4, cy - 2.2, 58.8, cy + 2.2), fill=0)
    for y in (49.4, 52.2, 55.0):                     # speaker slots
        d.rounded_rectangle(box(52, y - 0.85, 62, y + 0.85), radius=0.8 * u, fill=255)

    line = Image.new('L', (s, s), 0)
    d = ImageDraw.Draw(line)
    ring(d, (14.6, 24.6, 67.4, 57.6), 3.5, 1.1)      # inset line inside the cabinet
    ring(d, (21.6, 31.6, 44.4, 49.4), 3.5, 1.0)      # inset line inside the screen
    d.rectangle(box(17, 56.2, 49, 57.2), fill=0)     # (keeps the cabinet line clear under the screen)

    core = Image.new('L', (s, s), 0)
    d = ImageDraw.Draw(core)
    for cy in (32.5, 42.5):
        d.ellipse(box(54.6, cy - 2.0, 58.6, cy + 2.0), fill=255)
    return cut, line, core


def blur(im, r):
    return im.filter(ImageFilter.GaussianBlur(r))


def lighting(height, strength):
    """Lambert-style relief from a height field (L image, 0 = deep): +1 lit, -1 shadowed; light from the upper left,
    as on the game's tiles (their cut edges are dark at the upper left and light at the lower right)."""
    import numpy as np
    h = np.asarray(height, dtype=np.float32) / 255.0
    gx = np.zeros_like(h)
    gy = np.zeros_like(h)
    gx[:, 1:-1] = (h[:, 2:] - h[:, :-2]) * 0.5
    gy[1:-1, :] = (h[2:, :] - h[:-2, :]) * 0.5
    lx, ly = -0.7071, -0.7071                        # towards the light
    return np.clip(-(gx * lx + gy * ly) * strength, -1.0, 1.0)


def worn_plate(normals, highlights, base_index, seed=5):
    """A real tile's steel, worn as the game's are: tile `base_index` (its grime, scratches and light) with its own
    symbol filled from the same tile's steel. The steel is brushed along its rows, so each masked run of a row is
    interpolated between the unmasked steel at its two ends, with a row-wise random walk for the grain; the edge
    is feathered."""
    import numpy as np
    rnd = np.random.default_rng(seed)
    base = np.asarray(normals[base_index], dtype=np.float32)
    # The symbol is where the selected twin is red (its whole plate is also lighter, so a plain difference marks
    # everything), grown 4 px to take the cut's walls and rim.
    hl = np.asarray(highlights[base_index], dtype=np.float32)
    red = Image.fromarray(((hl[..., 0] - hl[..., 1] > 60) * 255).astype('uint8'), 'L')
    mask = np.asarray(red.filter(ImageFilter.MaxFilter(9)), dtype=np.float32) > 0
    filled = base.copy()
    h, w = mask.shape
    for y in range(h):
        x = 0
        while x < w:
            if not mask[y, x]:
                x += 1
                continue
            x0 = x
            while x < w and mask[y, x]:
                x += 1
            left = base[y, max(0, x0 - 3):x0].mean(axis=0) if x0 > 0 else base[y, min(w - 1, x):min(w, x + 3)].mean(0)
            right = base[y, x:min(w, x + 3)].mean(axis=0) if x < w else left
            walk, g = [], 0.0
            for _ in range(x - x0):
                g = 0.85 * g + rnd.uniform(-1.6, 1.6)
                walk.append(g)
            for i, xx in enumerate(range(x0, x)):
                t = (i + 0.5) / (x - x0)
                filled[y, xx, :3] = left[:3] * (1 - t) + right[:3] * t + walk[i]
                filled[y, xx, 3] = base[y, xx, 3]
    feather = np.asarray(blur(Image.fromarray((mask * 255).astype('uint8'), 'L'), 1.2),
                         dtype=np.float32)[..., None] / 255.0
    return base * (1 - feather) + filled * feather


def grime_field(w, h, seed):
    """Low-frequency stains and fine streaks (multiplicative, about 0.82 .. 1.06), for the cut's face."""
    import numpy as np
    rnd = np.random.default_rng(seed)
    coarse = Image.fromarray((rnd.random((h // 6 + 2, w // 6 + 2)) * 255).astype('uint8'), 'L')
    coarse = np.asarray(coarse.resize((w, h), Image.BICUBIC), dtype=np.float32) / 255.0
    streak = np.asarray(blur(Image.fromarray((rnd.random((h, w)) * 255).astype('uint8'), 'L'), 0.6),
                        dtype=np.float32) / 255.0
    return 0.82 + 0.18 * coarse + 0.06 * (streak - 0.5)


def engrave(plate, layers, face, seed=11):
    """The symbol cut into the plate the way the game's are (the example's look): a dark rim, a recessed face in
    the face colour with grime and the plate's own brushing showing through, a shadowed upper-left and lit
    lower-right inner wall, thin inset lines cut shallower, and the knobs' cores standing proud as turned steel."""
    import numpy as np
    w, h = plate.shape[1], plate.shape[0]
    cut, line, core = layers
    small = lambda im: np.asarray(im.resize((w, h), Image.LANCZOS), dtype=np.float32) / 255.0
    m_cut, m_line, m_core = small(cut), small(line), small(core)
    # Height at K x: plate 1.0, cut channel 0.25, inset lines 0.6, knob cores 0.9 (domed); then reduced.
    hcut = np.asarray(cut, dtype=np.float32) / 255.0
    hline = np.asarray(line, dtype=np.float32) / 255.0
    hcore = np.asarray(core, dtype=np.float32) / 255.0
    height = 1.0 - 0.75 * hcut - 0.4 * hline * (1 - hcut)
    height = height * (1 - hcore) + 0.9 * hcore
    hk = Image.fromarray((np.clip(height, 0, 1) * 255).astype('uint8'), 'L')
    relief = lighting(blur(hk, 0.8 * K / 4).resize((w, h), Image.LANCZOS), 9.0)
    # Turned-steel cores: a radial highlight, upper left.
    core_shade = lighting(blur(Image.fromarray((hcore * 255).astype('uint8'), 'L'), 2.2 * K)
                          .resize((w, h), Image.LANCZOS), 5.0)

    lum = plate[..., :3].mean(axis=2, keepdims=True)
    grain = lum / max(1.0, float(lum.mean()))       # the plate's brushing and stains, relative
    stains = grime_field(w, h, seed)[..., None]
    out = plate[..., :3].copy()
    face_rgb = np.array(face, dtype=np.float32)[None, None, :] if face is not None else plate[..., :3] * 0.5
    recess = face_rgb * grain * stains
    mc = m_cut[..., None]
    out = out * (1 - mc) + recess * mc
    # thin inset lines: a shallow, darker cut in whatever surface they cross
    ml = (m_line * (1 - m_cut))[..., None]
    out = out * (1 - 0.45 * ml)
    # dark rim just outside the cut (the game's outline)
    rim = np.clip(np.asarray(blur(cut, 1.0 * K).resize((w, h), Image.LANCZOS), dtype=np.float32) / 255.0
                  - m_cut, 0, 1)[..., None]
    out = out * (1 - 0.35 * rim)
    # Relief light only within the cuts (the game's tiles shade inside their symbols; the plate round them stays
    # flat): lit walls towards the face's own highlight (a light red on red, near white on steel), shadowed ones
    # darker.
    cuts = Image.fromarray(np.clip((hcut + hline + hcore) * 255, 0, 255).astype('uint8'), 'L')
    inside = np.asarray(cuts.filter(ImageFilter.MaxFilter(3)).resize((w, h), Image.LANCZOS),
                        dtype=np.float32)[..., None] / 255.0
    r = relief[..., None] * inside
    hi = mc * np.clip(face_rgb * 1.5 + 90, 0, 255) + (1 - mc) * 245.0
    out = np.where(r > 0, out + (hi - out) * r * 0.5, out * (1 + r * 0.6))
    # knob cores: turned steel standing proud, lit from the upper left
    cs = core_shade[..., None]
    steel = plate[..., :3] * 1.15
    steel = np.where(cs > 0, steel + (255 - steel) * cs * 0.8, steel * (1 + cs * 0.6))
    mk = m_core[..., None]
    out = out * (1 - mk) + steel * mk
    result = np.concatenate([np.clip(out, 0, 255), plate[..., 3:4]], axis=2)
    return Image.fromarray(result.astype('uint8'), 'RGBA')


def main():
    import numpy as np
    sheet = Image.open(os.path.join(TEX, 'Options_EN__2.png'))
    cells = load_cells()
    normals = [crop(sheet, cells[c]) for c in NORMAL_CELLS]
    highlights = [crop(sheet, cells[c + HIGHLIGHT_OFFSET]) for c in NORMAL_CELLS]
    grey, red = symbol_colours(normals, highlights)
    # The Load tile (arrow and bar, under the TV); the selected state on its selected twin, whose whole plate the
    # game draws lighter.
    plate = worn_plate(normals, highlights, base_index=4)
    plate_selected = worn_plate(highlights, highlights, base_index=4)
    layers = tv_layers(normals[0].size[0])
    # The cut's face: the game's measured normal grey and selected red (symbol_colours).
    normal = engrave(plate, layers, grey)
    selected = engrave(plate_selected, layers, red)
    normal.save(os.path.join(TEX, 'icon_display.png'))
    selected.save(os.path.join(TEX, 'icon_display_hi.png'))
    Image.fromarray(np.clip(plate, 0, 255).astype('uint8'), 'RGBA').save(os.path.join(TEX, 'icon_blank.png'))
    for name, im in (('display_tv.png', normal), ('display_tv_selected.png', selected)):
        im.save(os.path.join(ART, name))
    print('symbol grey %s, red %s -> %s' % (grey, red, ART))


if __name__ == '__main__':
    main()
