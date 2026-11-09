"""Setting text in a face: glyph fills side by side at the face's measured letter gap and word space, then the outline
regenerated around the whole line (outlined faces). Result: {(x, y): rgba} with y relative to the baseline."""


def set_fill(face, lib, glyphs, text, tint_generated=None):
    out, x = {}, 0
    tinted = set()
    missing = []
    for i, c in enumerate(text):
        if c == ' ':
            x += lib.space - lib.gap
            continue
        g = glyphs.get(c)
        if g is None:
            missing.append(c)
            x += 6 + lib.gap
            continue
        for (dx, dy), v in g.pix.items():
            k = (x + dx, dy)
            if k not in out or face.strength(v) >= face.strength(out[k]):
                out[k] = v
                if tint_generated and g.kind != 'real':
                    tinted.add(k)
                else:
                    tinted.discard(k)
        x += g.width + lib.gap
    out_t = {k: (tint_generated(v) if k in tinted else v) for k, v in out.items()} if tint_generated else out
    return out, missing, out_t


def with_outline(model, fill):
    if model is None:
        return dict(fill)
    out = model.outline_for(fill)
    out.update(fill)
    return out


def set_text(face, lib, glyphs, model, text, tint_generated=None):
    fill, missing, shown = set_fill(face, lib, glyphs, text, tint_generated)
    out = with_outline(model, fill)
    out.update(shown)
    return out, missing


def blit(canvas, pix, ox, oy, bg, scale=1):
    """Composite rgba pixels over the canvas rows (rgb tuples) at (ox, oy) baseline origin."""
    H, W = len(canvas), len(canvas[0])
    for (x, y), v in pix.items():
        a = v[3]
        for sy in range(scale):
            for sx in range(scale):
                X, Y = (ox + x) * scale + sx, (oy + y) * scale + sy
                if 0 <= X < W and 0 <= Y < H:
                    b = canvas[Y][X]
                    canvas[Y][X] = tuple((v[i] * a + b[i] * (255 - a)) // 255 for i in range(3))


def paste_into(img, pix, left, baseline):
    """Composite a set line (set_text) into texture rows img (RGBA lists) with its left edge at x = left and its
    baseline at row `baseline`; returns img. The caller clears the old words first (e.g. to the picture's
    background) and writes the result back with txd.Txd.replaced()."""
    H, W = len(img), len(img[0])
    for (x, y), v in pix.items():
        X, Y = left + x, baseline + y
        if 0 <= X < W and 0 <= Y < H:
            b = img[Y][X]
            a = v[3] / 255.0
            oa = a + b[3] / 255.0 * (1 - a)
            if oa <= 0:
                continue
            img[Y][X] = tuple(int(round((v[i] * a + b[i] * b[3] / 255.0 * (1 - a)) / oa)) for i in range(3)) +                 (int(round(oa * 255)),)
    return img
