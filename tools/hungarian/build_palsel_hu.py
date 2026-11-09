"""Builds the Hungarian 50/60 Hz picture (the game's palSel/PalSel<lang>.txd, texture "screentest", 512 x 512) out of the
game's own letters.

The five Latin pictures (En, Fr, Ge, It, Sp) hold the same three faces: a thin squared white face (the lines), a bold
white face (Yes / No / Test) and a red outlined techno face (the title). Their texts are known, so every letter is cut
out of them (one column run per letter, checked against the text's letter count) and the Hungarian lines are set from
those letters, on the English picture's bands. The few letters no picture has are made from the ones they resemble:
  A = V turned over + H's crossbar;  k, g, z (bold) = strokes in the face's weight;  m (bold) = two n;  I (bold) = the T's stem;
  acutes = strokes (the title's from the Italian a-grave mirrored);  the o / u with a double acute = the letter + two acutes.

    python -I build_palsel_hu.py <game files dir> <out raw rgb> [preview.png]

Output: 512*512*3 bytes for make_palsel.py. Python 3 stdlib only.
"""
import struct, sys, zlib, statistics

W = H = 512

# ---------------------------------------------------------------- reading the game's pictures
def textures(path):
    d = open(path, 'rb').read()
    out = []
    def walk(o, end):
        while o + 12 <= end:
            t, s, v = struct.unpack_from('<III', d, o)
            if t == 0x16:
                walk(o + 12, o + 12 + s)
            elif t == 0x15:
                body = o + 24
                name = d[body + 24:body + 56].split(b'\0')[0].decode()
                w, h = struct.unpack_from('>HH', d, body + 92)
                out.append((name, w, h, d[body + 98], d[body + 99], body))
            o += 12 + s
    walk(0, len(d))
    return d, out

def rgb565(v):
    return (((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31)

def load(files, lang):
    d, ts = textures('%s/palsel/PalSel%s.txd' % (files, lang))
    name, w, h, fmt, pf, body = [t for t in ts if t[0] == 'screentest'][0]
    img = [[None] * w for _ in range(h)]
    if fmt == 9:
        pal = [rgb565(struct.unpack_from('>H', d, body + 104 + 2 * i)[0]) for i in range(256)]
        i = body + 104 + 512 + 4
        for ty in range(0, h, 4):
            for tx in range(0, w, 8):
                for y in range(4):
                    for x in range(8):
                        img[ty + y][tx + x] = pal[d[i]]; i += 1
    elif fmt == 8:
        pal = [rgb565(struct.unpack_from('>H', d, body + 104 + 2 * i)[0]) for i in range(16)]
        i = body + 104 + 32 + 4
        for ty in range(0, h, 8):
            for tx in range(0, w, 8):
                for y in range(8):
                    for x in range(0, 8, 2):
                        b = d[i]; i += 1
                        img[ty + y][tx + x] = pal[b >> 4]; img[ty + y][tx + x + 1] = pal[b & 15]
    else:
        raise Exception('format %d' % fmt)
    return img

def lum(p): return (p[0] + p[1] + p[2]) // 3

def fillcov(p):  # the title's fill: (205,0,0) full, two edge blends
    return {(205, 0, 0): 3, (156, 16, 16): 2, (106, 32, 32): 1}.get(p, 0)

# ---------------------------------------------------------------- cutting letters
def col_runs(img, y0, y1, ink, x0=0, x1=W):
    cols = [any(ink(img[y][x]) for y in range(y0, y1)) for x in range(x0, x1)]
    out, x = [], 0
    while x < len(cols):
        if cols[x]:
            s = x
            while x < len(cols) and cols[x]: x += 1
            out.append((s + x0, x - 1 + x0))
        else:
            x += 1
    return out

NO_DESC = set("aceimnorsuvwz0H56TLDBMNPSFVWOJYNd")
def cut_line(img, text, y0, y1, ink, value, x0=0, x1=W):
    """-> {char: [(px dict {(dx,dy): v}, width)]}, baseline row, gaps; None when the letter count does not match."""
    runs = col_runs(img, y0, y1, ink, x0, x1)
    chars = [c for c in text if c != ' ']
    if len(runs) != len(chars):
        return None
    glyphs = []
    for c, (s, e) in zip(chars, runs):
        pix = {}
        for y in range(y0, y1):
            for x in range(s, e + 1):
                v = value(img[y][x])
                if v: pix[(x - s, y)] = v
        glyphs.append((c, pix, e - s + 1, s, e))
    bottoms = [max(y for (_, y) in pix) for c, pix, w, s, e in glyphs if c in NO_DESC and pix]
    base = int(statistics.median(bottoms))
    out = []
    for c, pix, w, s, e in glyphs:
        out.append((c, {(dx, y - base): v for (dx, y), v in pix.items()}, w))
    # gaps between neighbours: inside words and across a space
    gaps_in, gaps_sp = [], []
    k = 0
    idx = [i for i, c in enumerate(text) if c != ' ']
    for a, b in zip(range(len(runs) - 1), range(1, len(runs))):
        gap = runs[b][0] - runs[a][1] - 1
        (gaps_sp if idx[b] - idx[a] > 1 else gaps_in).append(gap)
    return out, base, gaps_in, gaps_sp

class Face:
    def __init__(self):
        self.g = {}            # char -> (pix, width)
        self.gap_in, self.gap_sp = [], []
        self.bases = []
    def add(self, cut):
        if cut is None: return
        glyphs, base, gi, gs = cut
        self.gap_in += gi; self.gap_sp += gs
        for c, pix, w in glyphs:
            self.g.setdefault(c, (pix, w))
    @property
    def gap(self): return int(statistics.median(self.gap_in)) if self.gap_in else 1
    @property
    def space(self): return int(statistics.median(self.gap_sp)) if self.gap_sp else 6

# ---------------------------------------------------------------- strokes (for the few made letters)
def stroke_canvas(width, rows_above, rows_below, polylines, thickness, ss=4):
    """Thick polylines (points (x, y) with y relative to the baseline, upward negative) -> {(dx,dy): 0..255}."""
    top = -rows_above
    hh = rows_above + rows_below
    acc = [[0] * (width * ss) for _ in range(hh * ss)]
    r = thickness / 2.0 * ss
    for pts in polylines:
        for (xa, ya), (xb, yb) in zip(pts, pts[1:]):
            steps = max(1, int(max(abs(xb - xa), abs(yb - ya)) * ss * 2))
            for i in range(steps + 1):
                t = i / steps
                cx = (xa + (xb - xa) * t) * ss
                cy = (ya + (yb - ya) * t - top) * ss
                for yy in range(int(cy - r) - 1, int(cy + r) + 2):
                    for xx in range(int(cx - r) - 1, int(cx + r) + 2):
                        if 0 <= xx < width * ss and 0 <= yy < hh * ss and (xx - cx) ** 2 + (yy - cy) ** 2 <= r * r:
                            acc[yy][xx] = 1
    pix = {}
    for y in range(hh):
        for x in range(width):
            n = sum(acc[y * ss + j][x * ss + i] for j in range(ss) for i in range(ss))
            if n: pix[(x, y + top)] = min(255, n * 255 // (ss * ss))
    return pix

def bbox(pix):
    xs = [x for x, _ in pix]; ys = [y for _, y in pix]
    return min(xs), max(xs), min(ys), max(ys)

def overlay(*layers):
    out = {}
    for pix, dx, dy in layers:
        for (x, y), v in pix.items():
            k = (x + dx, y + dy)
            out[k] = max(out.get(k, 0), v)
    return out

def flip_v(pix):
    x0, x1, y0, y1 = bbox(pix)
    return {(x, y0 + y1 - y): v for (x, y), v in pix.items()}

def mirror_h(pix, width):
    return {(width - 1 - x, y): v for (x, y), v in pix.items()}

# ---------------------------------------------------------------- the faces (the English picture's letters only)
BODY_LINES = ["Screen display has been set to 60Hz mode.", "Screen display has been set to 50Hz mode.",
              "Test 60Hz mode on your TV", "Now testing...."]
BODY_BANDS = [(123, 150), (192, 219), (261, 288), (330, 357)]
TITLE_LINES = [("Would you like to display", 0, 38), ("in 60Hz mode?", 38, 76)]
OTHER_BODY = {
    'Fr': ["L'affichage est en mode 60 Hz.", "L'affichage est en mode 50 Hz.", "Tester le mode 60 Hz sur votre TV.", "Test en cours..."],
    'Ge': ["Die Bildschirmdarstellung erfolgt im 60Hz-Modus.", "Die Bildschirmdarstellung erfolgt im 50Hz-Modus.", "Teste den 60Hz Modus auf deinem Fernseher.", "Testvorgang ..."],
    'Sp': ["La pantalla se ha configurado en modo 60 Hz.", "La pantalla se ha configurado en modo 50 Hz.", "Prueba el modo 60Hz en tu TV.", "Probando..."],
}
OTHER_TITLES = {   # the Spanish title is set a little narrower than the others, so it is the last resort
    'Fr': [("Voulez-vous afficher", 0, 38), ("l'image en mode 60 Hz ?", 38, 76)],
    'It': [("Visualizzare", 0, 38), ("in modalità 60Hz?", 38, 76)],
    'Sp': [("¿Deseas ver", 0, 38), ("el juego en modo 60 Hz ?", 38, 76)],
}
CHOICE_BAND = (88, 115)
CHOICE_PARTS = ["Y", "e", "s", "N", "o", "Te", "s", "t"]   # the runs of "Yes No Test" (two are two letters joined)

def split_run(img, run, parts, y0, y1):
    """A column run holding `parts` letters joined: cut at the weakest columns."""
    s, e = run
    ink = [sum(lum(img[y][x]) for y in range(y0, y1)) for x in range(s, e + 1)]
    cuts = []
    lo = 0
    for k in range(parts - 1):
        a = lo + 4
        b = len(ink) - 4 * (parts - 1 - k)
        c = min(range(a, b), key=lambda i: ink[i])
        cuts.append(c); lo = c
    edges = [0] + [c for c in cuts] + [len(ink)]
    return [(s + edges[i], s + edges[i + 1] - 1) for i in range(parts)]

def build(files):
    en = load(files, 'En')
    body, title, bold = Face(), Face(), Face()
    ink = lambda p: lum(p) > 30
    bases = {}
    cuts = []
    for text, (y0, y1) in zip(BODY_LINES, BODY_BANDS):
        c = cut_line(en, text, y0 - 6, y1 + 8, ink, lum)
        body.add(c); cuts.append(c[1])
    bases['body'] = cuts
    tcuts = []
    for text, y0, y1 in TITLE_LINES:
        c = cut_line(en, text, y0, y1, lambda p: fillcov(p) > 0, fillcov)
        title.add(c); tcuts.append(c[1])
    bases['title'] = tcuts
    # letters the English picture lacks that another language's picture has (same face, same heights):
    # the lines' v, f and hyphen, the title's g, j, r, v and hyphen
    for lang in ('Fr', 'Ge', 'Sp'):
        img = load(files, lang)
        for text, (y0, y1) in zip(OTHER_BODY[lang], BODY_BANDS):
            body.add(cut_line(img, text, y0 - 6, y1 + 8, ink, lum))
    for lang in ('Fr', 'It', 'Sp'):
        img = load(files, lang)
        for text, y0, y1 in OTHER_TITLES[lang]:
            title.add(cut_line(img, text, y0, y1, lambda p: fillcov(p) > 0, fillcov))
    # the choices: runs of "Yes No Test", the joined ones cut at their weakest column
    y0, y1 = CHOICE_BAND
    runs = col_runs(en, y0, y1, ink, 0, 262)
    assert len(runs) == len(CHOICE_PARTS), (len(runs), runs)
    letters = []
    for run, part in zip(runs, CHOICE_PARTS):
        if len(part) == 1: letters.append((part, run))
        else:
            for ch, r in zip(part, split_run(en, run, len(part), y0, y1)): letters.append((ch, r))
    bottoms = []
    glyph_px = []
    for ch, (s, e) in letters:
        pix = {(x - s, y): lum(en[y][x]) for y in range(y0 - 4, y1 + 8) for x in range(s, e + 1) if lum(en[y][x]) > 30}
        glyph_px.append((ch, pix, e - s + 1, s))
        if ch not in 'g': bottoms.append(max(y for _, y in pix))
    base_choice = int(statistics.median(bottoms))
    for ch, pix, w, s in glyph_px:
        bold.g.setdefault(ch, ({(dx, y - base_choice): v for (dx, y), v in pix.items()}, w))
    gaps = [glyph_px[i + 1][3] - (glyph_px[i][3] + glyph_px[i][2]) for i in range(len(glyph_px) - 1)
            if glyph_px[i + 1][3] - (glyph_px[i][3] + glyph_px[i][2]) < 8]
    bold.gap_in = gaps
    word_starts = [glyph_px[0][3], glyph_px[3][3], glyph_px[5][3]]   # Y, N, T
    make_body_letters(body); make_title_letters(title); make_bold_letters(bold)
    return body, title, bold, bases['body'], bases['title'], base_choice, word_starts

def ink_rows(pix): return sorted({y for _, y in pix})

def hyphen_rows(pix_n):
    x0, x1, y0, y1 = bbox(pix_n)
    return y0, y1

def make_body_letters(f):
    g = f.g
    n_pix, n_w = g['n']
    xh = bbox(n_pix)[2]                       # the x-height row (dy, negative)
    # A = H's stems and crossbar under a rounded top bar (the face's capitals are squared, like the 0 and the H)
    h, hw = g['H']
    top = bbox(h)[2]
    A = {k: val for k, val in h.items() if k[1] >= top + 2}
    for x in range(1, hw - 1):
        A[(x, top + 1)] = 255 if 2 <= x <= hw - 3 else 170
        A[(x, top)] = 255 if 3 <= x <= hw - 4 else (170 if 2 <= x <= hw - 3 else 0)
    A = {k: val for k, val in A.items() if val}
    g.setdefault('A', (A, hw))
    # k = the h's stem + two arms
    hp, hw2 = g['h']
    stem = {(x, y): val for (x, y), val in hp.items() if x <= 2}
    mid = xh * 0.55
    arms = stroke_canvas(hw2, 40, 6, [[(hw2 - 2.2, xh + 0.6), (3.4, mid), (hw2 - 1.8, -0.4)]], 1.6)
    g.setdefault('k', (overlay((stem, 0, 0), (arms, 0, 0)), hw2))
    # v = two diagonals at the x-height (the u's width)
    uw = g['u'][1]
    g.setdefault('v', (stroke_canvas(uw, 40, 6, [[(0.9, xh + 0.5), (uw / 2.0, -0.4), (uw - 0.9, xh + 0.5)]], 1.6), uw))
    # f = a stem with a hook and a crossbar (the t's width and crossbar row)
    asc = bbox(g['l'][0])[2]
    tw = g['t'][1] + 1
    g.setdefault('f', (stroke_canvas(tw, 40, 6, [[(2.5, -0.4), (2.5, asc + 2.5), (3.0, asc + 0.9), (tw - 0.8, asc + 0.9)],
                                        [(0.6, xh + 0.5), (tw - 1.0, xh + 0.5)]], 1.6), tw))
    # the hyphen: a bar at half the x-height, the width of the z / 0 less two
    hy_w = 7
    row = int(round(xh * 0.5))
    g.setdefault('-', ({(x, y): 255 for x in range(1, hy_w - 1) for y in (row, row + 1)}, hy_w))
    # accents: a stroke two pixels wide, four rows high, leaning right; one blank row above the x-height
    def acute_marks(offsets):
        pix = {}
        for x0 in offsets:
            for i in range(4):
                for dx in (0, 1):
                    pix[(x0 + i + dx, -i)] = 255
        return pix
    def acute_over(base, dotless=False, double=False):
        pix, w = g[base]
        if dotless:
            pix = {k: val for k, val in pix.items() if k[1] >= xh - 1}
        top = bbox(pix)[2]
        if double:
            mark = acute_marks([0, 4]); mw = 9
        else:
            mark = acute_marks([0]); mw = 5
        ox = (w - mw) // 2
        return overlay((pix, 0, 0), (mark, ox, top - 2)), w
    g['é'] = acute_over('e'); g['á'] = acute_over('a'); g['ó'] = acute_over('o')
    g['í'] = acute_over('i', dotless=True); g['ő'] = acute_over('o', double=True)

def make_title_letters(f):
    g = f.g
    n_pix, n_w = g['n']
    xh = bbox(n_pix)[2]
    # the hyphen: a bar of the stem's thickness (levels 3 inside, 2 at the ends)
    hy_w = 9
    row = int(round(xh * 0.5))
    bar = {}
    for x in range(hy_w):
        for y in range(row - 1, row + 3):
            bar[(x, y)] = 3 if 0 < x < hy_w - 1 else 2
    g.setdefault('-', (bar, hy_w))
    # the acute: a slanted bar, 3 columns wide, 6 rows high, leaning right
    def acute():
        pix = {}
        for i in range(6):
            x0 = i * 0.8
            for dx in range(3):
                pix[(int(round(x0)) + dx, -i)] = 3
        return pix
    ac = acute()
    if 'à' in g:
        a_gr, aw = g['à']
        grave = {k: v for k, v in a_gr.items() if k[1] <= xh - 3}
        real = mirror_h(grave, aw)
        bx0, bx1, by0, by1 = bbox(real)
        ac = {(x - bx0, y - by1): v for (x, y), v in real.items()}   # bottom row at 0, left edge at 0
    def with_acute(base, dotless=False):
        pix, w = g[base]
        if dotless:
            pix = {k: v for k, v in pix.items() if k[1] >= xh - 2}
        top = bbox(pix)[2]
        x0 = bbox(ac)[0]
        ox = (w - 8) // 2 + 0
        return overlay((pix, 0, 0), (ac, ox - x0 + 1, top - 3)), w
    for ch, base in (('á', 'a'), ('ó', 'o'), ('é', 'e')):
        g[ch] = with_acute(base)
    g['í'] = with_acute('i', dotless=True)
    # b = the d turned round; r = the n's left stem and arch; g = the o + a stem and curve below; j = the i + a stem below
    d, dw = g['d']
    g.setdefault('b', (mirror_h(d, dw), dw))
    n_pix2, nw2 = g['n']
    nx0, nx1, ntop, nbot = bbox(n_pix2)
    arch = int((nbot - ntop) * 0.40)
    g.setdefault('r', ({(x, y): v for (x, y), v in n_pix2.items() if y <= ntop + arch or x <= 5}, nw2 - 2))
    o_pix, ow2 = g['o']
    ox0, ox1, otop, obot = bbox(o_pix)
    rmid = {(x, 0): v for (x, y), v in o_pix.items() if y == (otop + obot) // 2 and x >= ow2 - 7}
    gg = dict(o_pix)
    for yy in range(obot + 1, obot + 9):
        for (x, _), v in rmid.items(): gg[(x, yy)] = v
    for (x, y), v in o_pix.items():
        if y >= obot - 4: gg[(x, y + 8)] = max(gg.get((x, y + 8), 0), v)
    g.setdefault('g', (gg, ow2))
    i_pix, iw2 = g['i']
    ix0, ix1, itop, ibot = bbox(i_pix)
    stem = {(x, y): v for (x, y), v in i_pix.items() if y == ibot}
    jj = dict(i_pix)
    for yy in range(ibot + 1, ibot + 9):
        for (x, _), v in stem.items(): jj[(x, yy)] = v
    for x in range(max(0, ix0 - 3), ix0 + 2):
        for yy in range(ibot + 5, ibot + 9): jj[(x, yy)] = 3
    g.setdefault('j', (jj, iw2))
    # S = the s stretched to the capital height (rows repeated)
    s_pix, sw2 = g['s']
    sx0, sx1, stop, sbot = bbox(s_pix)
    cap = bbox(g['H'][0])
    caph = cap[3] - cap[2] + 1
    sh = sbot - stop + 1
    S = {}
    for yy in range(caph):
        src = stop + min(sh - 1, int(yy * sh / caph))
        for x in range(sw2 + 1):
            if (x, src) in s_pix: S[(x, cap[2] + yy)] = s_pix[(x, src)]
    g.setdefault('S', (S, sw2))

def make_bold_letters(f):
    g = f.g
    o, ow = g['o']
    x0, x1, top, bot = bbox(o)
    mid = (top + bot) // 2
    row = {x: v for (x, y), v in o.items() if y == mid}
    sw = 1
    while (sw in row) and sw < ow: sw += 1                      # the stem's width at the o's middle
    sw = max(sw, 3)
    left = {(x, y): v for (x, y), v in o.items() if y == mid and x < sw + 1}
    right = {(x, y): v for (x, y), v in o.items() if y == mid and x > ow - sw - 2}
    arch_rows = int((bot - top) * 0.42)
    # n = the o's top (the arch) over two straight stems
    n = {(x, y): v for (x, y), v in o.items() if y <= top + arch_rows}
    for y in range(top + arch_rows + 1, bot + 1):
        for (x, _), v in left.items(): n[(x, y)] = v
        for (x, _), v in right.items(): n[(x, y)] = v
    g['n'] = (n, ow)
    # m = two n sharing a stem
    g['m'] = (overlay((n, 0, 0), (n, ow - sw - 1, 0)), 2 * ow - sw - 1)
    # g = the o + its right stem down and the o's bottom curve moved down
    gg = dict(o)
    for y in range(bot + 1, bot + 8):
        for (x, _), v in right.items(): gg[(x, y)] = v
    for (x, y), v in o.items():
        if y >= bot - 4: gg[(x, y + 7)] = max(gg.get((x, y + 7), 0), v)
    g['g'] = (gg, ow)
    # I = the T's stem
    t, tw = g['T']
    col = [x for x in range(tw) if sum(1 for (xx, y) in t if xx == x and y > top + 4) >= 6]
    sx0, sx1 = min(col), max(col)
    g['I'] = ({(x - sx0, y): v for (x, y), v in t.items() if sx0 <= x <= sx1}, sx1 - sx0 + 1)
    # z = three strokes in the stem's weight
    zw = ow
    g['z'] = (stroke_canvas(zw, 14, 2, [[(1.0, top + 1.2), (zw - 1.2, top + 1.2), (1.2, bot - 0.2), (zw - 1.0, bot - 0.2)]], sw * 0.95), zw)

# ---------------------------------------------------------------- setting text
def line_width(face, text):
    w = 0
    for i, c in enumerate(text):
        if c == ' ': w += face.space - face.gap
        else: w += face.g[c][1] + (face.gap if i < len(text) - 1 else 0)
    return w

def set_line(face, text, left):
    """-> {(x, dy): v} with x absolute, dy relative to the baseline."""
    out, x = {}, left
    for c in text:
        if c == ' ':
            x += face.space + face.gap
            continue
        pix, w = face.g[c]
        for (dx, dy), v in pix.items():
            k = (x + dx, dy)
            out[k] = max(out.get(k, 0), v)
        x += w + face.gap
    return out

def u(*codes): return ''.join(chr(c) for c in codes)

TEXTS = {
    'title': ["Szeretn" + u(0xe9) + "d 60Hz-es", "m" + u(0xf3) + "dban megjelen" + u(0xed) + "teni?"],
    'choices': ["Igen", "Nem", "Teszt"],
    'body': ["A k" + u(0xe9) + "perny" + u(0x151) + " 60Hz-es m" + u(0xf3) + "dra lett be" + u(0xe1) + "ll" + u(0xed) + "tva.",
             "A k" + u(0xe9) + "perny" + u(0x151) + " 50Hz-es m" + u(0xf3) + "dra lett be" + u(0xe1) + "ll" + u(0xed) + "tva.",
             "60Hz-es m" + u(0xf3) + "d tesztel" + u(0xe9) + "se a TV-n",
             "Tesztel" + u(0xe9) + "s folyamatban...."],
}

def render(files, out_raw, preview=None):
    body, title, bold, base_body, base_title, base_choice, choice_x = build(files)
    canvas = [[(0, 0, 0)] * W for _ in range(H)]
    # body lines, centred on x 256.5 (the English lines' centre)
    for text, base in zip(TEXTS['body'], base_body):
        wd = line_width(body, text)
        left = int(round(256.5 - wd / 2.0))
        for (x, dy), v in set_line(body, text, left).items():
            y = base + dy
            if 0 <= x < W and 0 <= y < H:
                canvas[y][x] = (v, v, v)
    for text, x0 in zip(TEXTS['choices'], choice_x):
        for (x, dy), v in set_line(bold, text, x0).items():
            y = base_choice + dy
            if 0 <= x < W and 0 <= y < H:
                canvas[y][x] = (v, v, v)
    # the title: red fill (three levels) and a two-pixel dark outline around it
    fills = {}
    for text, base in zip(TEXTS['title'], base_title):
        wd = line_width(title, text)
        left = int(round(256 - wd / 2.0))
        for (x, dy), lv in set_line(title, text, left).items():
            fills[(x, base + dy)] = lv
    colors = {3: (205, 0, 0), 2: (156, 16, 16), 1: (106, 32, 32)}
    ring = set()
    for (x, y) in fills:
        for yy in range(-2, 3):
            for xx in range(-2, 3):
                if xx * xx + yy * yy <= 5:
                    ring.add((x + xx, y + yy))
    for (x, y) in ring:
        if 0 <= x < W and 0 <= y < H and (x, y) not in fills:
            canvas[y][x] = (49, 48, 49)
    for (x, y), lv in fills.items():
        if 0 <= x < W and 0 <= y < H:
            canvas[y][x] = colors[lv]
    raw = bytearray()
    for row in canvas:
        for p in row: raw += bytes(p)
    open(out_raw, 'wb').write(raw)
    if preview:
        rows = [bytes(raw[y * W * 3:(y + 1) * W * 3]) for y in range(H)]
        def ch(t, b):
            c = struct.pack('>I', len(b)) + t + b
            return c + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
        open(preview, 'wb').write(b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0)) +
                                  ch(b'IDAT', zlib.compress(b''.join(b'\0' + r for r in rows))) + ch(b'IEND', b''))
    print('body letters', ''.join(sorted(c for c in body.g if ord(c) < 128)))
    print('gaps: body', body.gap, body.space, 'title', title.gap, title.space)

if __name__ == '__main__':
    render(sys.argv[1], sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else None)
