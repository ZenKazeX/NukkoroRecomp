"""RenderWare 3.x GameCube texture dictionaries (.txd): read every native texture, decode it to RGBA, and write a modified
picture back into a copy of the dictionary in the texture's own format (same size, palette format and byte layout).

Layout (all chunk headers little-endian {type, size, version}; the texture struct body is big-endian):
    0x16 texture dictionary > 0x01 struct (count) , 0x15 native texture > 0x01 struct (body) , 0x03 extension
    body +0   u32 platform (6)       +4 u32 filter/addressing
         +24  char[32] name          +56 char[32] mask name
         +88  u32 raster format      +92 u16 width   +94 u16 height
         +96  u8 depth   +97 u8 mip levels   +98 u8 GX texture format   +99 u8 palette format (1 RGB565, 2 RGB5A3, 0xff none)
         +100 u32 (1)                +104 palette: 16 (C4) or 256 (C8) big-endian u16 entries, none for direct formats
         then per mip level: u32 byte count, pixels in the GX tile order
GX formats handled: 0 I4, 1 I8, 2 IA4, 3 IA8, 4 RGB565, 5 RGB5A3, 6 RGBA8, 8 C4, 9 C8, 14 CMPR (decode only).
Colour expansion = bit replication (the hardware's), so encode(decode(x)) == x for the direct formats.
"""
import struct

TILE = {0: (8, 8), 1: (8, 4), 2: (8, 4), 3: (4, 4), 4: (4, 4), 5: (4, 4), 6: (4, 4), 8: (8, 8), 9: (8, 4), 14: (8, 8)}
BPP = {0: 4, 1: 8, 2: 8, 3: 16, 4: 16, 5: 16, 6: 32, 8: 4, 9: 8, 14: 4}


def x5(v): return (v << 3) | (v >> 2)
def x6(v): return (v << 2) | (v >> 4)
def x4(v): return v * 17
def x3(v): return (v << 5) | (v << 2) | (v >> 1)


def rgb565(v):
    return (x5((v >> 11) & 31), x6((v >> 5) & 63), x5(v & 31), 255)


def rgb5a3(v):
    if v & 0x8000:
        return (x5((v >> 10) & 31), x5((v >> 5) & 31), x5(v & 31), 255)
    return (x4((v >> 8) & 15), x4((v >> 4) & 15), x4(v & 15), x3((v >> 12) & 7))


def to565(p):
    r, g, b = p[:3]
    return (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3)


def to5a3(p):
    r, g, b, a = p
    if a >= 0xff:
        return 0x8000 | (r >> 3) << 10 | (g >> 3) << 5 | (b >> 3)
    return (a >> 5) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | (b >> 4)


def snap(p, palfmt):
    """The colour the hardware would show for p in this palette / direct format."""
    return rgb565(to565(p)) if palfmt == 1 else rgb5a3(to5a3(p))


class Texture(object):
    def __init__(self, data, body, size):
        self.body = body                    # offset of the struct body in the file
        self.size = size                    # struct body size
        d = data
        self.name = d[body + 24:body + 56].split(b'\0')[0].decode('latin1')
        self.w, self.h = struct.unpack_from('>HH', d, body + 92)
        self.depth, self.levels, self.fmt, self.palfmt = d[body + 96], d[body + 97], d[body + 98], d[body + 99]
        o = body + 104
        self.npal = {8: 16, 9: 256}.get(self.fmt, 0)
        self.pal_off = o
        self.palette = [struct.unpack_from('>H', d, o + 2 * i)[0] for i in range(self.npal)]
        o += 2 * self.npal
        self.pix_len = struct.unpack_from('>I', d, o)[0]
        self.pix_off = o + 4
        self.raw = d[self.pix_off:self.pix_off + self.pix_len]

    # ------------------------------------------------------------ decoding
    def tile_order(self):
        """Yields (x, y) in the stored order of the first level's texels."""
        tw, th = TILE[self.fmt]
        for ty in range(0, self.h, th):
            for tx in range(0, self.w, tw):
                for y in range(th):
                    for x in range(tw):
                        yield tx + x, ty + y

    def indices(self):
        """Palette indices (C4 / C8) as rows."""
        assert self.fmt in (8, 9)
        out = [[0] * self.w for _ in range(self.h)]
        if self.fmt == 9:
            vals = self.raw
        else:
            vals = []
            for b in self.raw:
                vals.append(b >> 4); vals.append(b & 15)
        for (x, y), v in zip(self.tile_order(), vals):
            if x < self.w and y < self.h:
                out[y][x] = v
        return out

    def pal_rgba(self):
        conv = rgb565 if self.palfmt == 1 else rgb5a3
        return [conv(v) for v in self.palette]

    def rgba(self):
        f, d, w, h = self.fmt, self.raw, self.w, self.h
        img = [[(0, 0, 0, 0)] * w for _ in range(h)]
        if f in (8, 9):
            pal = self.pal_rgba()
            for y, row in enumerate(self.indices()):
                img[y] = [pal[i] for i in row]
            return img
        if f == 14:
            return self._cmpr()
        vals = []
        if f == 0:
            for b in d:
                vals.append((x4(b >> 4),) * 3 + (255,)); vals.append((x4(b & 15),) * 3 + (255,))
        elif f == 1:
            vals = [(b, b, b, 255) for b in d]
        elif f == 2:
            vals = [(x4(b & 15),) * 3 + (x4(b >> 4),) for b in d]
        elif f == 3:
            vals = [(d[i + 1],) * 3 + (d[i],) for i in range(0, len(d), 2)]
        elif f == 4:
            vals = [rgb565(struct.unpack_from('>H', d, i)[0]) for i in range(0, w * h * 2, 2)]
        elif f == 5:
            vals = [rgb5a3(struct.unpack_from('>H', d, i)[0]) for i in range(0, w * h * 2, 2)]
        elif f == 6:   # 4x4 tiles: 32 bytes AR, then 32 bytes GB
            out = []
            for t in range(0, w * h * 4, 64):
                for k in range(16):
                    out.append((d[t + 2 * k + 1], d[t + 32 + 2 * k], d[t + 32 + 2 * k + 1], d[t + 2 * k]))
            vals = out
        else:
            raise ValueError('GX format %d' % f)
        for (x, y), v in zip(self.tile_order(), vals):
            if x < w and y < h:
                img[y][x] = v
        return img

    def _cmpr(self):
        w, h, d = self.w, self.h, self.raw
        img = [[(0, 0, 0, 0)] * w for _ in range(h)]
        o = 0
        for ty in range(0, h, 8):
            for tx in range(0, w, 8):
                for sy in (0, 4):
                    for sx in (0, 4):
                        c0, c1 = struct.unpack_from('>HH', d, o)
                        bits = struct.unpack_from('>I', d, o + 4)[0]
                        o += 8
                        a, b = rgb565(c0), rgb565(c1)
                        if c0 > c1:
                            cs = [a, b, tuple((2 * a[i] + b[i]) // 3 for i in range(3)) + (255,),
                                  tuple((a[i] + 2 * b[i]) // 3 for i in range(3)) + (255,)]
                        else:
                            cs = [a, b, tuple((a[i] + b[i]) // 2 for i in range(3)) + (255,), (0, 0, 0, 0)]
                        for k in range(16):
                            v = (bits >> (30 - 2 * k)) & 3
                            x, y = tx + sx + (k & 3), ty + sy + (k >> 2)
                            if x < w and y < h:
                                img[y][x] = cs[v]
        return img

    # ------------------------------------------------------------ encoding
    def encode(self, img, keep_index=None):
        """-> (palette list or None, pixel bytes) in this texture's format for the RGBA rows img.

        Paletted: pixels that keep their original colour keep their original index (so an unchanged picture writes
        back byte-exactly); new colours take an existing exact entry, else an unused entry, else the palette is
        re-quantized (median cut, <= 16 / 256 colours) over the whole picture.
        """
        f, w, h = self.fmt, self.w, self.h
        order = list(self.tile_order())
        if f in (8, 9):
            pal_rgba = self.pal_rgba()
            orig = self.indices()
            pal = list(self.palette)
            conv = to565 if self.palfmt == 1 else to5a3
            snapped = [[conv(p) for p in row] for row in img]
            idx = [[None] * w for _ in range(h)]
            need = {}
            for y in range(h):
                for x in range(w):
                    v = snapped[y][x]
                    if pal[orig[y][x]] == v:
                        idx[y][x] = orig[y][x]
                    else:
                        need.setdefault(v, []).append((x, y))
            if need:
                used = {idx[y][x] for y in range(h) for x in range(w) if idx[y][x] is not None}
                free = [i for i in range(self.npal) if i not in used]
                first = {}
                for i, v in enumerate(pal):
                    first.setdefault(v, i)
                missing = [v for v in need if v not in first]
                if len(missing) <= len(free):
                    for v in missing:
                        i = free.pop(0); pal[i] = v; first[v] = i
                    for v, pts in need.items():
                        for x, y in pts:
                            idx[y][x] = first[v]
                else:
                    pal, idx = quantize(snapped, self.npal, self.palfmt)
            if f == 9:
                data = bytes(idx[y][x] for x, y in order)
            else:
                vals = [idx[y][x] for x, y in order]
                data = bytes((vals[i] << 4) | vals[i + 1] for i in range(0, len(vals), 2))
            return pal, data
        if f in (4, 5):
            # a texel whose colour is unchanged keeps its original 16 bits (RGB5A3 has two codes for opaque colours)
            conv, back = (rgb5a3, to5a3) if f == 5 else (rgb565, to565)
            out = bytearray()
            for k, (x, y) in enumerate(order):
                ov = struct.unpack_from('>H', self.raw, 2 * k)[0]
                p = img[y][x]
                out += struct.pack('>H', ov if conv(ov) == tuple(p) else back(p))
            return None, bytes(out)
        raise ValueError('writing GX format %d is not implemented' % f)


def from_index565(v):
    return rgb565(v)


def quantize(snapped, n, palfmt):
    """Median cut over the snapped 16-bit colours -> (palette, index rows). Exact when <= n colours."""
    counts = {}
    for row in snapped:
        for v in row:
            counts[v] = counts.get(v, 0) + 1
    conv = rgb565 if palfmt == 1 else rgb5a3
    back = to565 if palfmt == 1 else to5a3
    if len(counts) <= n:
        pal = sorted(counts)
    else:
        boxes = [list(counts.items())]
        while len(boxes) < n:
            boxes.sort(key=lambda b: -sum(c for _, c in b) * (len(b) > 1))
            box = boxes.pop(0)
            if len(box) < 2:
                boxes.append(box); break
            ch = max(range(4), key=lambda k: max(conv(c)[k] for c, _ in box) - min(conv(c)[k] for c, _ in box))
            box.sort(key=lambda it: conv(it[0])[ch])
            half = sum(c for _, c in box) / 2.0
            acc, cut = 0, 1
            for i, (_, c) in enumerate(box):
                acc += c
                if acc >= half:
                    cut = max(1, min(len(box) - 1, i + 1)); break
            boxes += [box[:cut], box[cut:]]
        pal = []
        for box in boxes:
            tw = float(sum(c for _, c in box))
            avg = tuple(int(sum(conv(v)[k] * c for v, c in box) / tw + .5) for k in range(4))
            pal.append(back(avg))
    pal = (pal + [pal[0]] * n)[:n]
    cache = {}
    def pick(v):
        if v not in cache:
            c = conv(v)
            cache[v] = min(range(n), key=lambda k: sum((a - b) ** 2 for a, b in zip(conv(pal[k]), c)))
        return cache[v]
    return pal, [[pick(v) for v in row] for row in snapped]


class Txd(object):
    def __init__(self, path):
        self.path = path
        self.data = open(path, 'rb').read()
        self.textures = []
        d = self.data
        if struct.unpack_from('<I', d, 0)[0] == 0x16:
            self._walk(0, len(d))
        else:
            # a .gncp (CSD layout) embeds its texture dictionary: find every 0x16 chunk of RW version 0x1c020037
            o = 0
            while True:
                o = d.find(b'\x16\x00\x00\x00', o)
                if o < 0:
                    break
                if o + 12 <= len(d) and struct.unpack_from('<I', d, o + 8)[0] == 0x1c020037:
                    s = struct.unpack_from('<I', d, o + 4)[0]
                    self._walk(o, min(len(d), o + 12 + s))
                    o += 12 + s
                else:
                    o += 1

    def _walk(self, o, end):
        d = self.data
        while o + 12 <= end:
            t, s, v = struct.unpack_from('<III', d, o)
            if t == 0x16:
                self._walk(o + 12, o + 12 + s)
            elif t == 0x15:
                st = o + 12
                ct, cs, cv = struct.unpack_from('<III', d, st)
                self.textures.append(Texture(d, st + 12, cs))
            o += 12 + s

    def get(self, name):
        for t in self.textures:
            if t.name == name:
                return t
        raise KeyError(name)

    def replaced(self, name, img):
        """-> bytes of the whole dictionary with texture `name` re-encoded from img (same length)."""
        t = self.get(name)
        pal, data = t.encode(img)
        assert len(data) == t.pix_len, (len(data), t.pix_len)
        out = bytearray(self.data)
        if pal is not None:
            for i, v in enumerate(pal):
                struct.pack_into('>H', out, t.pal_off + 2 * i, v)
        out[t.pix_off:t.pix_off + t.pix_len] = data
        return bytes(out)


def selftest(path):
    """Round-trip every writable texture of a dictionary: decode -> encode must give the same bytes."""
    x = Txd(path)
    res = []
    for t in x.textures:
        if t.fmt not in (4, 5, 8, 9):
            res.append((t.name, 'skip fmt %d' % t.fmt)); continue
        same = x.replaced(t.name, t.rgba()) == x.data
        res.append((t.name, 'ok' if same else 'DIFF'))
    return res
