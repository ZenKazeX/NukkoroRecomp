#!/usr/bin/env python3
"""Extract RenderWare GameCube texture dictionaries (.txd) to lossless RGBA PNG.

Written for the Shadow the Hedgehog PAL disc (GUPP8P). Preserves the original dimensions, the
full alpha channel and the exact colour data - the decode is integer-exact for every format the
disc uses, so a round trip loses nothing.

    python txd_extract.py --out <dir> [--filter menus|all|<glob>] [--keep-duplicates]

Writes:
    <dir>/<name>.png          one file per UNIQUE texture (deduplicated by content hash)
    <dir>/manifest.json       every (txd, index, texture name) -> png, with all header fields
    <dir>/manifest.csv        the same, flat

== The container, reverse-engineered and verified against 1,166 textures ==

    RW chunk tree:  TEXDICTIONARY(0x16) -> [ STRUCT(0x01), TEXTURENATIVE(0x15) x N, EXTENSION ]
    TEXTURENATIVE -> STRUCT(0x01) whose body is:

        +0    u32  platform id            6 = GameCube
        +4    u32  filterAddressing       e.g. 0x00001101
        +24   char name[32]
        +56   char mask[32]
        +92   u16  width,  u16 height     big-endian
        +98   u8   format                 GX_TF_*: 8 = C4, 9 = C8, 5 = RGB5A3
        +99   u8   paletteFormat          GX_TL_*: 1 = RGB565, 2 = RGB5A3, 0xFF = none
        +104  palette                     C4: 16 entries, C8: 256 entries, 2 bytes big-endian
              u32  dataLength             *** AFTER the palette, not at a fixed offset ***
              pixel data

    The offset of dataLength is what makes this file worth having: it is +136 for C4, +616 for C8
    and +104 for a direct format, and reading it at a fixed +136 gives garbage for everything but
    C4. Verified: struct size == 140 + w*h/2 (C4), 620 + w*h (C8), 108 + w*h*2 (RGB5A3), exactly,
    for every texture on the disc.

== Formats present on this disc (all 1,166 textures) ==

    C4  + RGB5A3   573      C8 + RGB5A3   389      C8 + RGB565   180
    RGB5A3 direct   12      C4 + RGB565    12

    No CMPR, no RGBA8, no I/IA. The tile orders are the standard Flipper ones:
    C4 8x8 (4bpp, high nibble first), C8 8x4 (8bpp), 16bpp formats 4x4.
"""
import argparse
import csv
import glob
import hashlib
import json
import os
import re
import struct
import sys

GX_FORMAT = {0: 'I4', 1: 'I8', 2: 'IA4', 3: 'IA8', 4: 'RGB565', 5: 'RGB5A3',
             6: 'RGBA8', 8: 'C4', 9: 'C8', 10: 'C14X2', 14: 'CMPR'}
GX_PALETTE = {0: 'IA8', 1: 'RGB565', 2: 'RGB5A3', 0xFF: 'none'}

# The menu / front-end dictionaries, as opposed to the per-stage pause and stage-title ones.
MENU_PATTERNS = [
    'honban', 'SelectMode', 'BattleMode', 'Story_Library', 'Synopsis', 'RouteDisplay',
    'Options', 'MediaMenu', 'GameOver', 'Result_', '2P_Result', '2P_Start', 'MissionFailed',
    'Logo', 'E3RomTitle', 'InfoWindow',
]


def rgb5a3(v):
    """GX RGB5A3 -> (r, g, b, a), integer-exact 5/3-bit expansion."""
    if v & 0x8000:
        r = (v >> 10) & 0x1F
        g = (v >> 5) & 0x1F
        b = v & 0x1F
        return (r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2), 255
    a = (v >> 12) & 0x7
    r = (v >> 8) & 0xF
    g = (v >> 4) & 0xF
    b = v & 0xF
    return (r << 4) | r, (g << 4) | g, (b << 4) | b, (a << 5) | (a << 2) | (a >> 1)


def rgb565(v):
    r = (v >> 11) & 0x1F
    g = (v >> 5) & 0x3F
    b = v & 0x1F
    return (r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2), 255


def ia8(v):
    i = v & 0xFF
    a = (v >> 8) & 0xFF
    return i, i, i, a


PALETTE_DECODE = {0: ia8, 1: rgb565, 2: rgb5a3}


def read_palette(data, off, count, fmt):
    dec = PALETTE_DECODE.get(fmt)
    if dec is None:
        raise ValueError('unsupported palette format %r' % fmt)
    return [dec(struct.unpack_from('>H', data, off + 2 * i)[0]) for i in range(count)]


def untile(width, height, tile_w, tile_h, emit):
    """Walk Flipper tile order and call emit(tile_index, x, y, pixel_index_in_tile)."""
    tiles_x = (width + tile_w - 1) // tile_w
    tiles_y = (height + tile_h - 1) // tile_h
    t = 0
    for ty in range(tiles_y):
        for tx in range(tiles_x):
            for py in range(tile_h):
                for px in range(tile_w):
                    x = tx * tile_w + px
                    y = ty * tile_h + py
                    emit(t, x, y, py * tile_w + px, x < width and y < height)
            t += 1


def decode(fmt, pal_fmt, palette, pixels, width, height):
    """Return a flat RGBA bytearray of width*height*4."""
    out = bytearray(width * height * 4)

    def put(x, y, rgba):
        o = (y * width + x) * 4
        out[o:o + 4] = bytes(rgba)

    if fmt == 8:                                    # C4, 8x8 tiles, 4bpp
        tile_bytes = 32
        def emit(t, x, y, i, inside):
            if not inside:
                return
            byte = pixels[t * tile_bytes + (i >> 1)]
            idx = (byte >> 4) if (i & 1) == 0 else (byte & 0xF)
            put(x, y, palette[idx])
        untile(width, height, 8, 8, emit)
    elif fmt == 9:                                  # C8, 8x4 tiles, 8bpp
        tile_bytes = 32
        def emit(t, x, y, i, inside):
            if not inside:
                return
            put(x, y, palette[pixels[t * tile_bytes + i]])
        untile(width, height, 8, 4, emit)
    elif fmt in (4, 5):                             # RGB565 / RGB5A3, 4x4 tiles, 16bpp
        dec = rgb565 if fmt == 4 else rgb5a3
        tile_bytes = 32
        def emit(t, x, y, i, inside):
            if not inside:
                return
            v = struct.unpack_from('>H', pixels, t * tile_bytes + 2 * i)[0]
            put(x, y, dec(v))
        untile(width, height, 4, 4, emit)
    else:
        raise ValueError('format %s (%d) is not present on this disc and is not implemented'
                         % (GX_FORMAT.get(fmt, '?'), fmt))
    return out


def parse_txd(path):
    data = open(path, 'rb').read()
    cid, size, _ = struct.unpack_from('<III', data, 0)
    if cid != 0x16:
        return []
    end = 12 + size
    p = 12
    out = []
    index = 0
    while p + 12 <= end:
        c, csize, _ = struct.unpack_from('<III', data, p)
        if c == 0x15:
            b = p + 12
            _, ssize, _ = struct.unpack_from('<III', data, b)
            body = b + 12
            plat = struct.unpack_from('>I', data, body)[0]
            filt = struct.unpack_from('>I', data, body + 4)[0]
            name = data[body + 24:body + 56].split(b'\0')[0].decode('ascii', 'replace')
            mask = data[body + 56:body + 88].split(b'\0')[0].decode('ascii', 'replace')
            w, h = struct.unpack_from('>HH', data, body + 92)
            _u96, mips, fmt, pal_fmt = struct.unpack_from('>BBBB', data, body + 96)
            pal_off = body + 104
            if fmt == 8:
                pal_count = 16
            elif fmt == 9:
                pal_count = 256
            else:
                pal_count = 0
            palette = read_palette(data, pal_off, pal_count, pal_fmt) if pal_count else None
            len_off = pal_off + pal_count * 2
            data_len = struct.unpack_from('>I', data, len_off)[0]
            pix_off = len_off + 4
            expect = ssize - (pix_off - body)
            out.append(dict(index=index, name=name, mask=mask, platform=plat,
                            filter_addressing=filt, width=w, height=h, mipmaps=mips,
                            format=fmt, format_name=GX_FORMAT.get(fmt, '?'),
                            palette_format=pal_fmt,
                            palette_format_name=GX_PALETTE.get(pal_fmt, '?'),
                            data_length=data_len, data_length_consistent=(data_len == expect),
                            _palette=palette, _pixels=data[pix_off:pix_off + expect]))
            index += 1
        p += 12 + csize
    return out


def safe(text):
    text = re.sub(r'[^A-Za-z0-9._-]+', '_', text).strip('_')
    return text or 'unnamed'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='H:/ShadowTheHedgehogDiskGC_Extract/files/csdFiles')
    ap.add_argument('--out', required=True)
    ap.add_argument('--filter', default='menus',
                    help='"menus" (front-end dictionaries), "all", or a glob over file names')
    ap.add_argument('--keep-duplicates', action='store_true',
                    help='write a PNG per source texture instead of deduplicating by content')
    args = ap.parse_args()

    try:
        from PIL import Image
    except ImportError:
        sys.exit('Pillow is required: pip install pillow')

    # Recursive: dictionaries sit at several depths - files/*.txd, files/csdFiles/*.txd,
    # files/csdFiles/pause/*.txd, files/fonts/<stage>/*.txd. A two-level glob misses the deepest.
    files = sorted(set(glob.glob(os.path.join(args.root, '**', '*.txd'), recursive=True)
                       + glob.glob(os.path.join(args.root, '*.txd'))))
    if args.filter == 'menus':
        files = [f for f in files
                 if any(pat.lower() in os.path.basename(f).lower() for pat in MENU_PATTERNS)]
    elif args.filter != 'all':
        files = [f for f in files if glob.fnmatch.fnmatch(os.path.basename(f), args.filter)]

    os.makedirs(args.out, exist_ok=True)
    entries, by_hash, failures = [], {}, []
    for path in files:
        try:
            textures = parse_txd(path)
        except Exception as exc:                                   # noqa: BLE001
            failures.append((path, 'parse: %s' % exc))
            continue
        stem = os.path.splitext(os.path.basename(path))[0]
        for t in textures:
            try:
                rgba = decode(t['format'], t['palette_format'], t['_palette'],
                              t['_pixels'], t['width'], t['height'])
            except Exception as exc:                               # noqa: BLE001
                failures.append((path + '#' + t['name'], 'decode: %s' % exc))
                continue
            digest = hashlib.sha256(bytes(rgba)).hexdigest()
            if not args.keep_duplicates and digest in by_hash:
                png = by_hash[digest]
            else:
                base = '%s__%02d_%s' % (safe(stem), t['index'], safe(t['name']))
                png = base + '.png'
                n = 1
                while os.path.exists(os.path.join(args.out, png)) and png not in by_hash.values():
                    png = '%s_%d.png' % (base, n)
                    n += 1
                Image.frombytes('RGBA', (t['width'], t['height']), bytes(rgba)).save(
                    os.path.join(args.out, png), 'PNG', optimize=True)
                by_hash.setdefault(digest, png)
            rec = {k: v for k, v in t.items() if not k.startswith('_')}
            rec.update(source_txd=os.path.relpath(path, args.root).replace('\\', '/'),
                       png=png, sha256=digest,
                       has_alpha=any(rgba[i] != 255 for i in range(3, len(rgba), 4)))
            entries.append(rec)

    with open(os.path.join(args.out, 'manifest.json'), 'w', encoding='utf-8') as fh:
        json.dump({'root': args.root, 'filter': args.filter,
                   'textures': entries, 'failures': failures}, fh, indent=2)
    if entries:
        cols = [k for k in entries[0] if k != 'failures']
        with open(os.path.join(args.out, 'manifest.csv'), 'w', encoding='utf-8', newline='') as fh:
            wtr = csv.DictWriter(fh, fieldnames=cols)
            wtr.writeheader()
            for e in entries:
                wtr.writerow({k: e.get(k) for k in cols})

    unique = len(set(e['png'] for e in entries))
    print('%d dictionaries, %d textures, %d unique images written to %s'
          % (len(files), len(entries), unique, args.out))
    bad = [e for e in entries if not e['data_length_consistent']]
    if bad:
        print('WARNING: %d textures whose dataLength disagreed with the chunk size' % len(bad))
    for f in failures:
        print('  FAILED %s -- %s' % f)
    return 0


if __name__ == '__main__':
    sys.exit(main())
