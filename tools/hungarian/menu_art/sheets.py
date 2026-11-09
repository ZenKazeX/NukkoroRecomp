"""Review sheets: per face the full Hungarian alphabet (original letters as they are, generated ones tinted, missing
ones as red boxes), sample lines, and an 8x close-up of every accented letter next to its base letter and the original
letter its accent was taken from."""
import compose as K
import generate as GEN
import png

BG = (92, 96, 116)


def tint(v):
    return (v[0], int(v[1] * 0.82), int(v[2] * 0.5), v[3])


def outlined(model, g):
    """The glyph with its regenerated outline; a generated glyph's fill tinted AFTER the outline is made."""
    out = K.with_outline(model, g.pix)
    if g.kind != 'real':
        for k, v in g.pix.items():
            out[k] = tint(v)
    return out


def _rows_for(face, glyphs):
    lo = getattr(face, 'lower_only', False)
    rows = [] if lo else [GEN.ALPHA_UP]
    if not face.caps_only:
        rows.append(GEN.ALPHA_LO)
    if not lo:
        rows.append(GEN.HU_UP)
    if not face.caps_only:
        rows.append(GEN.HU_LO)
    rows.append(GEN.DIGITS + GEN.PUNCT)
    extra = ''.join(sorted(c for c in glyphs if all(c not in r for r in rows) and c != ' '))
    if extra:
        rows.append(extra)
    return rows


def _extent(glyphs):
    ys = [y for g in glyphs.values() for _, y in g.pix]
    return min(ys), max(ys)


def alphabet_sheet(face, lib, glyphs, model, path, scale=4, samples=()):
    top, bot = _extent(glyphs)
    margin = 6
    line_h = (bot - top + 1) + 2 * margin + 4
    rows = _rows_for(face, glyphs)
    cap_h = max(4, -top)
    cells = []
    for r in rows:
        line = []
        for c in r:
            g = glyphs.get(c)
            if g is None:
                line.append((c, None, 8))
            else:
                line.append((c, outlined(model, g), g.width))
        cells.append(line)
    sample_pix = []
    for s in samples:
        pix, missing = K.set_text(face, lib, glyphs, model, s, tint)
        sample_pix.append(pix)
    W = max([sum(w + 2 * margin for _, _, w in line) for line in cells] +
            [(max(x for x, _ in p) + 2 * margin + 2) if p else 0 for p in sample_pix]) + 8
    H = line_h * (len(cells) + len(sample_pix)) + 8
    canvas = [[BG] * (W * scale) for _ in range(H * scale)]
    for ri, line in enumerate(cells):
        x = 4
        base = 4 + ri * line_h + margin - top
        for c, pix, w in line:
            if pix is None:
                for yy in range(-cap_h, 0):
                    for xx in range(w):
                        if yy in (-cap_h, -1) or xx in (0, w - 1):
                            K.blit(canvas, {(xx, yy): (230, 30, 30, 255)}, x + margin, base, BG, scale)
            else:
                K.blit(canvas, pix, x + margin, base, BG, scale)
            x += w + 2 * margin
    for si, pix in enumerate(sample_pix):
        base = 4 + (len(cells) + si) * line_h + margin - top
        K.blit(canvas, pix, 4 + margin, base, BG, scale)
    png.write(path, canvas)
    return rows


def accent_sheet(face, lib, glyphs, model, path, scale=8):
    items = []
    for ch, (base, kind) in GEN.HU_ACCENT.items():
        if face.caps_only and ch.islower():
            continue
        if getattr(face, 'lower_only', False) and ch.isupper():
            continue
        g = glyphs.get(ch)
        b = glyphs.get(base)
        src = None
        if g is not None and g.kind != 'real' and 'from ' in (g.note or ''):
            sc = g.note.split('from ')[1][0]
            src = lib.g.get(sc)
        items.append((ch, b, g, src))
    if not items:
        return
    top, bot = _extent(glyphs)
    margin = 5
    line_h = (bot - top + 1) + 2 * margin + 2
    cols = 3
    def cellw(it):
        return sum((x.width if x is not None else 8) + 2 * margin for x in it[1:])
    per_row = 3
    rows = [items[i:i + per_row] for i in range(0, len(items), per_row)]
    W = max(sum(cellw(it) + 12 for it in r) for r in rows) + 8
    H = line_h * len(rows) + 8
    canvas = [[BG] * (W * scale) for _ in range(H * scale)]
    for ri, r in enumerate(rows):
        x = 4
        base = 4 + ri * line_h + margin - top
        for it in r:
            for k, g in enumerate(it[1:]):
                if g is None:
                    x += 8 + 2 * margin
                    continue
                K.blit(canvas, outlined(model, g), x + margin, base, BG, scale)
                x += g.width + 2 * margin
            # separator
            for yy in range(top - margin, bot + margin):
                K.blit(canvas, {(0, yy): (40, 40, 48, 255)}, x + 4, base, BG, scale)
            x += 12
    png.write(path, canvas)


def cut_sheet(face, lib, path, scale=3):
    """Every band of the face as the picture has it, each cut letter's columns underlined in alternating colours
    (red / green), so the letter split can be checked by eye."""
    import common as C
    strips = []
    for cut in lib.cuts:
        img = cut['img']
        rows = [[C.over(p, BG) for p in r] for r in img]
        w = len(rows[0])
        mark = [BG] * w
        for i, (c, pix, wd, s) in enumerate(cut['glyphs']):
            for x in range(s, s + wd):
                if 0 <= x < w:
                    mark[x] = (230, 40, 40) if i % 2 == 0 else (40, 220, 60)
        rows += [mark, mark, [BG] * w]
        strips.append(rows)
    W = max(len(r[0]) for r in strips)
    canvas = []
    for rows in strips:
        for r in rows:
            canvas.append(r + [BG] * (W - len(r)))
    png.write(path, [[p for p in r for _ in range(scale)] for r in canvas for _ in range(scale)])
