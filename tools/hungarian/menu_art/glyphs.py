"""Cutting a face's letters out of the game's pictures and keeping them as a glyph library.

A band is one line of known text in one language's picture: (pack, texture, language, box, text). Its pixels are split
into the face's FILL (the letter body, membership given by the face's `strength` function, 0..1) and everything else
(the outline / shadow / background). Letters are the fill's column runs, matched one to one with the text's non-space
characters (a '"' is two runs); runs that hold several touching letters are split at their weakest columns, specks
are joined to their neighbour, and a band whose count still does not match is reported, never guessed.

A glyph keeps the ORIGINAL pixels of its fill (RGBA, x from its left column, y relative to the band's baseline), its
advance width and where it came from. Outlined faces regenerate the outline around the set fill (outline.py), so the
cut never carries a neighbour's outline.
"""
import statistics

import common as C

# letters that sit on the baseline (used to find it); all capitals but J and Q, lowercase without descenders, digits
NO_DESC = set("ABCDEFGHIKLMNOPRSTUVWXYZabcdefhiklmnorstuvwxz0123456789!?:.ÀÁÂÄÈÉÊÍÎÏÒÓÔÖÙÚÛÜàáâäèéêëíìîïòóôöùúûüßñÑ")


class Band(object):
    def __init__(self, pack, tex, lang, box, text, rotate=0, splits=None, note='', cuts=None):
        self.pack, self.tex, self.lang, self.box, self.text = pack, tex, lang, box, text
        self.cuts = cuts              # explicit letter boundaries (columns in the band, between letters), checked by eye
        self.rotate = rotate          # 0, 90 (picture turned clockwise to read it) or 270
        self.splits = splits or {}    # {run index: number of letters} forced
        self.note = note

    def image(self, files=None):
        img = C.load(self.pack, self.tex, self.lang, files)
        x0, y0, x1, y1 = self.box
        sub = C.crop(img, x0, y0, x1, y1)
        if self.rotate == 90:       # clockwise
            sub = [list(r) for r in zip(*sub[::-1])]
        elif self.rotate == 270:    # counter-clockwise
            sub = [list(r) for r in zip(*sub)][::-1]
        elif self.rotate == 180:
            sub = [r[::-1] for r in sub[::-1]]
        return sub

    def src(self):
        return '%s:%s:%s' % (self.pack, self.tex or '', self.lang)


def runs_of(flags):
    out, x = [], 0
    while x < len(flags):
        if flags[x]:
            s = x
            while x < len(flags) and flags[x]:
                x += 1
            out.append([s, x - 1])
        else:
            x += 1
    return out


def n_runs(ch):
    return 2 if ch == '"' else 1


def comp_groups(member, st, want, text):
    """2-D cut for touching / overlapping letters: connected fill components, marks (accents, dots) joined to the
    letter below them, specks to the nearest letter; then broken letters merged / joined letters split until the
    count matches. -> [set of (x, y)] left to right, or None."""
    h, w = len(member), len(member[0])
    left = {(x, y) for y in range(h) for x in range(w) if member[y][x]}
    comps = []
    while left:
        p = left.pop()
        comp, stack = {p}, [p]
        while stack:
            x, y = stack.pop()
            for dx in (-1, 0, 1):
                for dy in (-1, 0, 1):
                    q = (x + dx, y + dy)
                    if q in left:
                        left.remove(q); comp.add(q); stack.append(q)
        comps.append(comp)
    if not comps:
        return None
    def bb(c):
        xs = [x for x, _ in c]; ys = [y for _, y in c]
        return min(xs), max(xs), min(ys), max(ys)
    sizes = sorted(len(c) for c in comps)
    big = sizes[-1]
    letters = [c for c in comps if len(c) >= max(6, big * 0.06)]
    small = [c for c in comps if len(c) < max(6, big * 0.06)]
    # marks: a letter-sized component lying wholly above another one it overlaps horizontally
    changed = True
    while changed:
        changed = False
        for c in list(letters):
            x0, x1, y0, y1 = bb(c)
            for d in letters:
                if d is c:
                    continue
                a0, a1, b0, b1 = bb(d)
                if y1 < b0 + (b1 - b0) * 0.25 and min(x1, a1) - max(x0, a0) >= 0 and (y1 - y0) < (b1 - b0):
                    d |= c; letters.remove(c); changed = True
                    break
            if changed:
                break
    for c in small:
        cx = sum(x for x, _ in c) / float(len(c)); cy = sum(y for _, y in c) / float(len(c))
        best = min(letters, key=lambda d: min(abs(x - cx) + abs(y - cy) * 0.5 for x, y in d))
        best |= c
    groups = sorted(letters, key=lambda c: sum(x for x, _ in c) / float(len(c)))
    while len(groups) > want:
        # merge the neighbours that overlap most (or sit closest)
        def score(i):
            a, b = bb(groups[i]), bb(groups[i + 1])
            return b[0] - a[1]
        i = min(range(len(groups) - 1), key=score)
        groups[i:i + 2] = [groups[i] | groups[i + 1]]
    while len(groups) < want:
        i = max(range(len(groups)), key=lambda k: bb(groups[k])[1] - bb(groups[k])[0])
        g = groups[i]
        x0, x1, _, _ = bb(g)
        if x1 - x0 < 6:
            return None
        colw = [0.0] * w
        for x, y in g:
            colw[x] += st[y][x]
        a, b = x0 + (x1 - x0) // 4, x1 - (x1 - x0) // 4
        c = min(range(a, b + 1), key=lambda k: colw[k])
        groups[i:i + 1] = [{p for p in g if p[0] < c}, {p for p in g if p[0] >= c}]
    return groups


def cut_band(face, band, files=None):
    """-> dict with glyphs [(char, pix, width, x_left)], baseline row, gaps, or {'error': ...}."""
    img = band.image(files)
    h, w = len(img), len(img[0])
    st = [[face.strength(p) for p in row] for row in img]
    member = [[s > face.min_strength for s in row] for row in st]
    colw = [sum(st[y][x] for y in range(h) if member[y][x]) for x in range(w)]
    runs = runs_of([v > 0 for v in colw])
    if getattr(face, 'seg', 'columns') == 'components' and not band.cuts:
        chars = [c for c in band.text if c != ' ']
        groups = comp_groups(member, st, sum(n_runs(c) for c in chars), band.text)
        band.how = 'components'
        if groups is None:
            return {'error': 'component cut failed', 'band': band}
        per, bottoms, k = [], [], 0
        for c in chars:
            n = n_runs(c)
            g = set().union(*groups[k:k + n]); k += n
            x0 = min(x for x, _ in g); x1 = max(x for x, _ in g)
            pix = {(x - x0, y): tuple(img[y][x]) for (x, y) in g}
            per.append((c, pix, x1 - x0 + 1, x0))
            if c in NO_DESC:
                bottoms.append(max(y for _, y in pix))
        return _finish(per, bottoms, band, img, member, st)
    # specks: a run with almost no ink joins the nearer neighbour (or is dropped at the band's ends)
    chars = [c for c in band.text if c != ' ']
    want = sum(n_runs(c) for c in chars)
    def ink(r): return sum(colw[r[0]:r[1] + 1])
    changed = True
    while len(runs) > want and changed:
        changed = False
        small = min(range(len(runs)), key=lambda i: ink(runs[i]))
        if ink(runs[small]) < face.speck:
            i = small
            if 0 < i < len(runs) - 1:
                j = i - 1 if runs[i][0] - runs[i - 1][1] <= runs[i + 1][0] - runs[i][1] else i + 1
            elif len(runs) > 1:
                j = 1 if i == 0 else i - 1
                if abs(runs[j][0] - runs[i][1]) > face.speck_join and abs(runs[i][0] - runs[j][1]) > face.speck_join:
                    runs.pop(i); changed = True; continue
            else:
                break
            a, b = sorted((i, j))
            runs[a] = [min(runs[a][0], runs[b][0]), max(runs[a][1], runs[b][1])]
            runs.pop(b)
            changed = True
    if band.cuts:
        edges = [0] + list(band.cuts) + [w]
        runs = []
        for a, b in zip(edges, edges[1:]):
            xs = [x for x in range(a, b) if colw[x] > 0]
            if xs:
                runs.append([xs[0], xs[-1]])
    elif len(runs) != want and getattr(face, 'dp_split', False):
        runs = dp_band(colw, band.text, face.min_letter_w)
        band.how = 'dp'
    # forced splits, then automatic splits of the widest runs (touching letters)
    for i in sorted(band.splits, reverse=True):
        runs[i:i + 1] = split_run(colw, runs[i], band.splits[i])
    while runs and len(runs) < want:
        wide = max(range(len(runs)), key=lambda i: runs[i][1] - runs[i][0])
        if runs[wide][1] - runs[wide][0] < 2 * face.min_letter_w:
            break
        runs[wide:wide + 1] = split_run(colw, runs[wide], 2, face.min_letter_w)
    if len(runs) != want:
        return {'error': 'runs %d != letters %d' % (len(runs), want), 'runs': runs, 'band': band}
    # group runs per character
    glyphs = []
    k = 0
    for c in chars:
        n = n_runs(c)
        s, e = runs[k][0], runs[k + n - 1][1]
        k += n
        glyphs.append([c, s, e])
    # rows of each glyph
    bottoms = []
    per = []
    for c, s, e in glyphs:
        pix = {}
        for y in range(h):
            for x in range(s, e + 1):
                if member[y][x]:
                    pix[(x - s, y)] = tuple(img[y][x])
        per.append((c, pix, e - s + 1, s))
        if c in NO_DESC and pix:
            bottoms.append(max(y for (_, y) in pix))
    return _finish(per, bottoms, band, img, member, st)


def _finish(per, bottoms, band, img, member, st):
    if not bottoms:
        return {'error': 'no baseline letters', 'band': band}
    base = int(statistics.median(bottoms)) + 1     # the baseline is the row below the letters' last row
    out = [(c, {(x, y - base): v for (x, y), v in pix.items()}, wd, s) for c, pix, wd, s in per]
    gaps_in, gaps_sp = [], []
    idx = [i for i, c in enumerate(band.text) if c != ' ']
    for a in range(len(per) - 1):
        gap = per[a + 1][3] - (per[a][3] + per[a][2])
        (gaps_sp if idx[a + 1] - idx[a] > 1 else gaps_in).append(gap)
    return {'glyphs': out, 'base': base, 'gaps_in': gaps_in, 'gaps_sp': gaps_sp, 'band': band, 'img': img,
            'member': member, 'strength': st}


REL_W = {}
for _c in "il.,:;!'|¡íìÍI":
    REL_W[_c] = 0.42
for _c in 'fjtr-':
    REL_W[_c] = 0.7
for _c in 'mwMW':
    REL_W[_c] = 1.5


def dp_band(colw, text, minw=3):
    """Words first (the widest blank gaps), then each word's letters by dp_split with per-letter width hints."""
    words = text.split()
    blanks = [r for r in runs_of([v == 0 for v in colw])]
    xs = [x for x, v in enumerate(colw) if v > 0]
    if not xs:
        return []
    inner = [r for r in blanks if r[0] > xs[0] and r[1] < xs[-1]]
    seps = sorted(sorted(inner, key=lambda r: -(r[1] - r[0]))[:len(words) - 1])
    if len(seps) != len(words) - 1:
        return []
    edges = [xs[0]] + [x for r in seps for x in (r[0], r[1] + 1)] + [xs[-1] + 1]
    out = []
    for k, word in enumerate(words):
        a, b = edges[2 * k], edges[2 * k + 1]
        sub = [0] * a + colw[a:b] + [0] * (len(colw) - b)
        letters = [c for c in word]
        rel = [REL_W.get(c, 1.12 if c.isupper() else 1.0) for c in letters]
        part = dp_split(sub, len(letters), minw, rel=rel)
        if len(part) != len(letters):
            return []
        out += [[s_, e_] for s_, e_ in part for _ in range(1)]
    # a '"' counts two runs in the caller: duplicate its segment split in half
    res = []
    for c, r in zip([c for c in text if c != ' '], out):
        if c == '"':
            m = (r[0] + r[1]) // 2
            res += [[r[0], m], [m + 1, r[1]]]
        else:
            res.append(r)
    return res


def dp_split(colw, n, minw=3, lam=0.15, rel=None):
    """Touching letters (grunge faces): n segments over the band's ink, cuts at weak columns (cost = the column's
    ink; blank columns are free) with a mild preference for even widths. -> [[start, end]] trimmed to ink."""
    xs = [x for x, v in enumerate(colw) if v > 0]
    if not xs:
        return []
    lo, hi = xs[0], xs[-1] + 1
    rel = rel or [1.0] * n
    unit = (hi - lo) / float(sum(rel))
    INF = float('inf')
    # best[k][c] = cost of k segments ending at cut c (segment = [prev, c))
    cand = list(range(lo, hi + 1))
    best = [{lo: 0.0}] + [dict() for _ in range(n)]
    back = [dict() for _ in range(n + 1)]
    for k in range(1, n + 1):
        for c in cand:
            if k == n and c != hi:
                continue
            bc, bp = INF, None
            for p, v in best[k - 1].items():
                wd = c - p
                if wd < minw:
                    continue
                avg = unit * rel[k - 1]
                cost = v + (colw[c] if c < hi else 0) + lam * (wd - avg) ** 2 / max(1.0, avg)
                if cost < bc:
                    bc, bp = cost, p
            if bp is not None:
                best[k][c] = bc; back[k][c] = bp
    if hi not in best[n]:
        return []
    cuts, c = [hi], hi
    for k in range(n, 0, -1):
        c = back[k][c]; cuts.append(c)
    cuts = cuts[::-1]
    out = []
    for a, b in zip(cuts, cuts[1:]):
        ink = [x for x in range(a, b) if colw[x] > 0]
        out.append([ink[0], ink[-1]] if ink else [a, b - 1])
    return out


def split_run(colw, run, parts, minw=3):
    s, e = run
    ink = colw[s:e + 1]
    cuts, lo = [], 0
    for k in range(parts - 1):
        a = lo + minw
        b = len(ink) - minw * (parts - 1 - k)
        if b <= a:
            c = (lo + len(ink)) // 2
        else:
            c = min(range(a, b), key=lambda i: (ink[i], abs(i - (lo + len(ink)) / 2.0)))
        cuts.append(c)
        lo = c
    edges = [0] + cuts + [len(ink)]
    out = []
    for i in range(parts):
        a, b = s + edges[i], s + edges[i + 1] - 1
        # a weakest column with ink belongs to neither side when it is empty; keep it on the left
        out.append([a, b])
    return out


class Glyph(object):
    def __init__(self, ch, pix, width, src, kind='real', note=''):
        self.ch, self.pix, self.width, self.src, self.kind, self.note = ch, pix, width, src, kind, note

    def bbox(self):
        xs = [x for x, _ in self.pix]; ys = [y for _, y in self.pix]
        return min(xs), max(xs), min(ys), max(ys)


class Library(object):
    """A face's letters: the preferred real cut per character plus every variant seen, and the face's spacing."""
    def __init__(self, face):
        self.face = face
        self.g = {}             # char -> Glyph (preferred)
        self.variants = {}      # char -> [Glyph]
        self.gaps_in, self.gaps_sp = [], []
        self.bases = []
        self.report = []        # (band src, text, 'ok' / error)

    def add_cut(self, cut):
        band = cut['band']
        if 'error' in cut:
            self.report.append((band.src(), band.text, cut['error']))
            return
        how = getattr(band, 'how', 'column runs')
        self.report.append((band.src(), band.text, 'ok %d letters (%s)' % (len(cut['glyphs']), how)))
        if band.lang == self.face.metric_lang or not self.gaps_in:
            self.gaps_in += cut['gaps_in']; self.gaps_sp += cut['gaps_sp']
        for c, pix, wd, s in cut['glyphs']:
            if not pix:
                self.report.append((band.src(), band.text, 'empty cut for %r' % c))
                continue
            gl = Glyph(c, pix, wd, '%s @x%d' % (band.src(), band.box[0] + s))
            gl.lang = band.lang
            self.variants.setdefault(c, []).append(gl)
            if c not in self.g and band.lang not in self.face.exclude_langs:
                self.g[c] = gl

    def fill_from_excluded(self):
        """Letters only a squeezed language has are used as a last resort (and flagged)."""
        for c, vs in self.variants.items():
            if c not in self.g:
                self.g[c] = vs[0]
                self.g[c].note = 'only in %s (narrower set)' % vs[0].lang

    @property
    def gap(self):
        return int(round(statistics.median(self.gaps_in))) if self.gaps_in else 1

    @property
    def space(self):
        return int(round(statistics.median(self.gaps_sp))) if self.gaps_sp else 6

    def consistency(self):
        """Per character: how many variants are pixel-identical to the preferred one (same face metrics check)."""
        out = {}
        for c, vs in self.variants.items():
            ref = self.g.get(c)
            if ref is None:
                continue
            same = sum(1 for v in vs if v.pix == ref.pix)
            widths = sorted({v.width for v in vs})
            out[c] = (same, len(vs), widths, sorted({v.lang for v in vs}))
        return out
