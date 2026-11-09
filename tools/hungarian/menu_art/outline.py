"""Outlined faces: the outline (and any soft edge or shadow) is regenerated around the set fill instead of being cut.

Calibration on the game's own lines: for every non-fill pixel near the letters, the squared distance (dx, dy offset
allowed for a shadow) to the nearest CORE fill pixel (fill strength >= core) is measured, and the picture's pixel at
that distance is collected; the most common pixel per distance becomes the face's outline table. The model is checked
by rebuilding every original line from its own cut fill and comparing it with the picture (`verify`).
"""
from collections import Counter

R = 7    # the farthest outline pixel searched


def _core(pix, strength, core):
    return {k for k, v in pix.items() if strength(v) >= core}


def distances(core_pts, region, shift=(0, 0)):
    """{pixel: squared distance to the nearest core point} for pixels in region (iterable) within R."""
    sx, sy = shift
    pts = {(x + sx, y + sy) for x, y in core_pts}
    out = {}
    offs = sorted(((dx, dy) for dx in range(-R, R + 1) for dy in range(-R, R + 1) if dx * dx + dy * dy <= R * R),
                  key=lambda o: o[0] ** 2 + o[1] ** 2)
    for (x, y) in region:
        for dx, dy in offs:
            if (x + dx, y + dy) in pts:
                out[(x, y)] = dx * dx + dy * dy
                break
    return out


def near(core_pts, r=R):
    s = set()
    for x, y in core_pts:
        for dx in range(-r, r + 1):
            for dy in range(-r, r + 1):
                if dx * dx + dy * dy <= r * r:
                    s.add((x + dx, y + dy))
    return s


def soft_distances(fill_st, region, step=4):
    """{pixel: bucket} with bucket = round(step * min over fill q of (|p - q| - s_q)): a fill pixel's strength moves
    the letter's edge toward the pixel, so partly covered edge pixels count for what they cover."""
    offs = [(dx, dy, (dx * dx + dy * dy) ** 0.5) for dx in range(-R, R + 1) for dy in range(-R, R + 1)
            if 0 < dx * dx + dy * dy <= R * R]
    import math
    out = {}
    for (x, y) in region:
        best = None
        for dx, dy, dd in offs:
            s = fill_st.get((x + dx, y + dy))
            if s:
                v = dd - s
                if best is None or v < best[0]:
                    best = (v, dx, dy)
        if best is not None:
            v, dx, dy = best
            octant = int(round(math.atan2(-dy, -dx) / (math.pi / 4))) % 8     # direction from the fill to the pixel
            out[(x, y)] = (int(round(v * step)), octant)
    return out


class SoftOutlineModel(object):
    """Outline pixel = f(soft distance to the fill): the mean colour and alpha per quarter-pixel distance, snapped to
    the nearest pixel value the face's outlines actually use."""
    def __init__(self, face, step=4):
        self.face, self.step = face, step
        self.table = {}

    def calibrate(self, cuts):
        acc = {}
        for cut in cuts:
            img, member, st = cut['img'], cut['member'], cut['strength']
            h, w = len(img), len(img[0])
            fill = {(x, y): st[y][x] for y in range(h) for x in range(w) if member[y][x]}
            region = [(x, y) for (x, y) in near(fill) if 0 <= x < w and 0 <= y < h and not member[y][x]]
            for (x, y), (d, o) in soft_distances(fill, region, self.step).items():
                acc.setdefault((d, o), []).append(tuple(img[y][x]))
                acc.setdefault((d, None), []).append(tuple(img[y][x]))
        used = Counter(v for vs in acc.values() for v in vs)
        levels = [v for v, n in used.most_common() if n >= 3]
        self.table = {}
        self.n = {d: len(vs) for d, vs in acc.items()}
        for d, vs in acc.items():
            a = sum(v[3] for v in vs) / float(len(vs))
            if a < 1:
                self.table[d] = (0, 0, 0, 0); continue
            mean = tuple(sum(v[i] * v[3] for v in vs) / (a * len(vs)) for i in range(3)) + (a,)
            self.table[d] = min(levels, key=lambda l: sum((l[i] * l[3] / 255.0 - mean[i] * mean[3] / 255.0) ** 2
                                                          for i in range(3)) + (l[3] - mean[3]) ** 2)
        return self

    def outline_for(self, fill):
        st = {k: self.face.strength(v) for k, v in fill.items()}
        st = {k: s for k, s in st.items() if s > 0}
        region = [p for p in near(st) if p not in fill]
        out = {}
        for p, (d, o) in soft_distances(st, region, self.step).items():
            v = self.table.get((d, o)) if self.n.get((d, o), 0) >= 3 else None
            if v is None:
                v = self.table.get((d, None))
            if v is None:
                ks = [k for k, oo in self.table if oo is None and k <= d]
                v = self.table.get((max(ks), None)) if ks else None
            if v is not None and v[3] > 0:
                out[p] = v
        return out


class OutlineModel(object):
    def __init__(self, face, core=0.5, shift=(0, 0)):
        self.face, self.core, self.shift = face, core, shift
        self.table = {}

    def calibrate(self, cuts):
        acc = {}
        for cut in cuts:
            img, member, st = cut['img'], cut['member'], cut['strength']
            h, w = len(img), len(img[0])
            core_pts = {(x, y) for y in range(h) for x in range(w) if st[y][x] >= self.core and member[y][x]}
            region = [(x, y) for (x, y) in near(core_pts) if 0 <= x < w and 0 <= y < h and not member[y][x]]
            for p, d in distances(core_pts, region, self.shift).items():
                x, y = p
                acc.setdefault(d, Counter())[tuple(img[y][x])] += 1
        self.table = {d: c.most_common(1)[0][0] for d, c in acc.items()}
        self.spread = {d: (c.most_common(1)[0][1], sum(c.values())) for d, c in acc.items()}
        return self

    def outline_for(self, fill):
        """fill: {(x, y): rgba} -> {(x, y): rgba} outline pixels (not under the fill)."""
        core_pts = {k for k, v in fill.items() if self.face.strength(v) >= self.core}
        region = [p for p in near(core_pts) if p not in fill]
        out = {}
        for p, d in distances(core_pts, region, self.shift).items():
            v = self.table.get(d)
            if v is not None and v[3] > 0:
                out[p] = v
        return out


def verify(model, cut):
    """Rebuild the cut's band from its own glyph fills + the model's outline; -> (exact %, mean abs error, n)."""
    img = cut['img']
    h, w = len(img), len(img[0])
    base = cut['base']
    fill = {}
    for c, pix, wd, s in cut['glyphs']:
        for (dx, dy), v in pix.items():
            fill[(s + dx, base + dy)] = v
    out = dict(model.outline_for(fill)) if model else {}
    out.update(fill)
    n = same = 0
    err = 0
    for y in range(h):
        for x in range(w):
            a = tuple(img[y][x])
            b = out.get((x, y), (0, 0, 0, 0))
            if a[3] == 0 and b[3] == 0:
                continue
            n += 1
            e = sum(abs(a[i] * a[3] - b[i] * b[3]) for i in range(3)) / (3 * 255.0) + abs(a[3] - b[3])
            err += e
            if e < 8:
                same += 1
    return (100.0 * same / max(1, n), err / max(1, n), n)
