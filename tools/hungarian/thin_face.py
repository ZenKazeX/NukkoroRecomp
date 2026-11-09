"""The thin squared face of the Hz screen's lines, as a monoline skeleton generator.

The game's letters in that face are monoline: strokes about 2.1 px wide with square ends and corners rounded by a
couple of pixels (the H, 0, a, e ... of palSel/PalSel<lang>.txd). Letters the pictures do not have are drawn here as
skeletons (polylines with rounded corners) and rasterized at 8x8 sub-pixels, so they carry the same anti-aliased edges
as the real ones. Coordinates: x across from the glyph's left edge, y in rows relative to the baseline (negative up);
a pixel (x, y) covers [x, x+1) x [y, y+1). Caps: bars centred on y = -19 and y = 0, stems from -20 to +1; x-height
bars centred on y = -11 and y = 0.
"""
import math

T = 2.1        # stroke width
SS = 8         # sub-pixel samples per axis

def fillet(points, r):
    """Polyline -> denser polyline with every interior corner rounded by radius r (r = 0: sharp)."""
    if r <= 0 or len(points) < 3:
        return list(points)
    out = [points[0]]
    for i in range(1, len(points) - 1):
        p0, p1, p2 = points[i - 1], points[i], points[i + 1]
        d0 = (p0[0] - p1[0], p0[1] - p1[1]); d2 = (p2[0] - p1[0], p2[1] - p1[1])
        l0 = math.hypot(*d0); l2 = math.hypot(*d2)
        rr = min(r, l0 / 2.0, l2 / 2.0)
        a = (p1[0] + d0[0] / l0 * rr, p1[1] + d0[1] / l0 * rr)
        b = (p1[0] + d2[0] / l2 * rr, p1[1] + d2[1] / l2 * rr)
        steps = 6
        for k in range(steps + 1):
            t = k / steps
            # quadratic Bezier a - p1 - b
            x = (1 - t) ** 2 * a[0] + 2 * (1 - t) * t * p1[0] + t * t * b[0]
            y = (1 - t) ** 2 * a[1] + 2 * (1 - t) * t * p1[1] + t * t * b[1]
            out.append((x, y))
    out.append(points[-1])
    return out

def thickness_of(xa, ya, xb, yb):
    """The face's strokes: horizontals 2.0 px, verticals 2.25 px, diagonals 2.1 px."""
    dx, dy = abs(xb - xa), abs(yb - ya)
    if dy < 0.01: return 2.0
    if dx < 0.01: return 2.25
    return 2.1

def raster(strokes, width):
    """strokes: list of (points, corner radius). -> {(x, y): 0..255}. Sharp (radius 0) interior corners are squared."""
    cover = {}
    for pts, r in strokes:
        pl = fillet(pts, r)
        segs = [[a, b] for a, b in zip(pl, pl[1:])]
        if r <= 0 and len(pl) > 2:
            for i in range(len(segs)):
                a, b = segs[i]
                ln = math.hypot(b[0] - a[0], b[1] - a[1])
                if ln == 0: continue
                ux, uy = (b[0] - a[0]) / ln, (b[1] - a[1]) / ln
                th = thickness_of(a[0], a[1], b[0], b[1])
                if i > 0: segs[i][0] = (a[0] - ux * th / 2.0 * 0.0 - ux * 0.0, a[1])  # placeholder, replaced below
                segs[i][0] = a; segs[i][1] = b
            # extend the ends that meet another segment by half the *other* segment's thickness
            for i in range(len(segs)):
                a, b = segs[i]
                ln = math.hypot(b[0] - a[0], b[1] - a[1])
                if ln == 0: continue
                ux, uy = (b[0] - a[0]) / ln, (b[1] - a[1]) / ln
                if i > 0:
                    pa, pb = segs[i - 1]
                    th = thickness_of(pa[0], pa[1], pb[0], pb[1])
                    segs[i][0] = (a[0] - ux * th / 2.0, a[1] - uy * th / 2.0)
                if i < len(segs) - 1:
                    na, nb = segs[i + 1]
                    th = thickness_of(na[0], na[1], nb[0], nb[1])
                    segs[i][1] = (b[0] + ux * th / 2.0, b[1] + uy * th / 2.0)
        for (xa, ya), (xb, yb) in segs:
            dx, dy = xb - xa, yb - ya
            ln = math.hypot(dx, dy)
            if ln == 0: continue
            ux, uy = dx / ln, dy / ln
            nx, ny = -uy, ux
            half = thickness_of(xa, ya, xb, yb) / 2.0
            corners = [(xa + nx * half, ya + ny * half), (xa - nx * half, ya - ny * half),
                       (xb - nx * half, yb - ny * half), (xb + nx * half, yb + ny * half)]
            xs = [c[0] for c in corners]; ys = [c[1] for c in corners]
            for py in range(int(math.floor(min(ys))), int(math.ceil(max(ys))) + 1):
                for px in range(int(math.floor(min(xs))), int(math.ceil(max(xs))) + 1):
                    for j in range(SS):
                        sy = py + (j + 0.5) / SS
                        for i in range(SS):
                            sx = px + (i + 0.5) / SS
                            tx, ty = sx - xa, sy - ya
                            along = tx * ux + ty * uy
                            across = tx * nx + ty * ny
                            if -0.0001 <= along <= ln + 0.0001 and abs(across) <= half:
                                cover.setdefault((px, py), set()).add((i, j))
    pix = {}
    for (px, py), s in cover.items():
        v = int(round(255.0 * len(s) / (SS * SS)))
        if v >= 12:
            pix[(px, py)] = min(255, v)
    return pix

# ---------------------------------------------------------------- skeletons
def caps(letter, w):
    """Skeleton strokes of a capital in a box w wide (caps are 10 wide, 11 for H, 15 for W)."""
    L, R = 1.35, w - 1.35
    top, bot = -19.0, 0.0
    m = (L + R) / 2.0
    if letter == 'A':
        return [([(L, 1), (L, -16.5), (L + 1.4, top), (R - 1.4, top), (R, -16.5), (R, 1)], 0.9), ([(L, -10.5), (R, -10.5)], 0)]
    if letter == 'C':
        return [([(R, -14.5), (R, top), (L, top), (L, bot), (R, bot), (R, -4.5)], 0.9)]
    if letter == 'E':
        return [([(R, top), (L, top), (L, bot), (R, bot)], 0), ([(L, -10.5), (R - 0.8, -10.5)], 0)]
    if letter == 'G':
        return [([(R, -14.5), (R, top), (L, top), (L, bot), (R, bot), (R, -9.5), (m + 0.3, -9.5)], 0.9)]
    if letter == 'J':
        return [([(R, -20), (R, bot), (L, bot), (L, -4.5)], 0.9)]
    if letter == 'K':
        return [([(L, -20), (L, 1)], 0), ([(R + 0.2, -20), (L + 1.5, -9.6), (R + 0.4, 1)], 0)]
    if letter == 'O':
        return [([(L, -16.5), (L, top), (R, top), (R, bot), (L, bot), (L, -16.5)], 0.9)]
    if letter == 'Q':
        return [([(L, top), (R, top), (R, bot), (L, bot), (L, top)], 0.9), ([(R - 3.0, -4.5), (R + 1.0, 2.0)], 0)]
    if letter == 'U':
        return [([(L, -20), (L, bot), (R, bot), (R, -20)], 0.9)]
    if letter == 'W':
        return [([(L - 0.3, -20), (L + 3.2, 1), (m, -11), (R - 3.2, 1), (R + 0.3, -20)], 0)]
    if letter == 'X':
        return [([(L, -20), (R, 1)], 0), ([(R, -20), (L, 1)], 0)]
    if letter == 'Y':
        return [([(L - 0.2, -20), (m, -9.5)], 0), ([(R + 0.2, -20), (m, -9.5)], 0), ([(m, -9.5), (m, 1)], 0)]
    if letter == 'Z':
        return [([(L - 0.4, top), (R + 0.4, top), (L - 0.2, bot), (R + 0.4, bot)], 0)]
    raise KeyError(letter)

def lower(letter, w=9):
    L, R = 1.35, w - 1.35
    top, bot = -11.0, 0.0
    if letter == 'x':
        return [([(L - 0.2, -12), (R + 0.2, 1)], 0), ([(R + 0.2, -12), (L - 0.2, 1)], 0)]
    if letter == 'k':
        return [([(L, -20), (L, 1)], 0), ([(R + 0.1, -11.8), (L + 1.8, -4.6)], 0), ([(L + 2.4, -5.8), (R + 0.3, 1)], 0)]
    raise KeyError(letter)
