"""Minimal PNG read/write (stdlib only). Images are lists of rows; a pixel is an (r, g, b, a) or (r, g, b) tuple."""
import struct
import zlib


def _chunk(tag, data):
    c = struct.pack('>I', len(data)) + tag + data
    return c + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff)


def write(path, img, alpha=False):
    """img: rows of (r, g, b[, a]) tuples. alpha=False drops the alpha channel."""
    h = len(img)
    w = len(img[0])
    n = 4 if alpha else 3
    raw = bytearray()
    for row in img:
        raw.append(0)
        for p in row:
            raw += bytes(p[:n]) if len(p) >= n else bytes(tuple(p) + (255,))
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + _chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6 if alpha else 2, 0, 0, 0)) +
                _chunk(b'IDAT', zlib.compress(bytes(raw), 6)) + _chunk(b'IEND', b''))


def read(path):
    """-> rows of (r, g, b, a). 8-bit RGB / RGBA / grey / grey+alpha, non-interlaced."""
    d = open(path, 'rb').read()
    o = 8
    idat = b''
    w = h = ct = 0
    while o < len(d):
        n = struct.unpack('>I', d[o:o + 4])[0]
        t = d[o + 4:o + 8]
        b = d[o + 8:o + 8 + n]
        o += 12 + n
        if t == b'IHDR':
            w, h, bd, ct = struct.unpack('>IIBB', b[:10])
            assert bd == 8, 'only 8-bit PNG'
        elif t == b'IDAT':
            idat += b
    raw = zlib.decompress(idat)
    bpp = {0: 1, 2: 3, 4: 2, 6: 4}[ct]
    stride = w * bpp
    rows = []
    prev = bytearray(stride)
    p = 0
    for _ in range(h):
        f = raw[p]
        line = bytearray(raw[p + 1:p + 1 + stride])
        p += 1 + stride
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b_ = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b_) & 255
            elif f == 3:
                line[i] = (line[i] + (a + b_) // 2) & 255
            elif f == 4:
                pa = abs(b_ - c); pb = abs(a - c); pc = abs(a + b_ - 2 * c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else (b_ if pb <= pc else c))) & 255
        prev = line
        row = []
        for x in range(w):
            q = line[x * bpp:(x + 1) * bpp]
            if ct == 0:
                row.append((q[0], q[0], q[0], 255))
            elif ct == 4:
                row.append((q[0], q[0], q[0], q[1]))
            elif ct == 2:
                row.append((q[0], q[1], q[2], 255))
            else:
                row.append((q[0], q[1], q[2], q[3]))
        rows.append(row)
    return rows
