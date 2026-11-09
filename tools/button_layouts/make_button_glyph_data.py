#!/usr/bin/env python3
"""The modern button glyphs as the game patches them in: runtime/src/gekko/button_glyph_data.inc.

    python make_button_glyph_data.py [--game-files <dir>] [--out <file>] [--report <dir>]

Every button sheet on the disc is paletted (C8 or C4, RGB5A3 palette) and its palette is full, so a
glyph cannot simply be painted in. For each unique copy of a sheet (by its bytes; 27 on the PAL disc,
in 187 files) and each family (xbox, playstation, switch) this fits a palette that keeps every pixel
outside the button cells and holds the new glyphs:

  * entries used only inside the button cells are free;
  * if fewer than the glyphs want are free, the closest pairs of the remaining entries are merged
    (their pixels outside the cells move to the survivor - an index remap the runtime applies);
  * the free entries are fitted to the new glyphs (k-means, every variant of every cell), and each
    glyph pixel takes the nearest entry of the whole palette.

A face cell (the GC A / B / X / Y it means) is stored in all four positions (south, east, west,
north), so the runtime picks the one the player's binding puts that GC button on; a start cell and
the VHArrow highlight mask have one variant. The runtime finds a sheet by the FNV-1a 64 hash of its
bytes from the palette to the end of the texels, so another region's files are left alone unless
they hold the same sheet.

Each variant is also painted in full colour on the sheet scaled by SHARP (kSharpVariants): the runtime
composes the sharp sheet from them and serves it to the native renderer in place of the paletted one,
which stays the fallback (a 16-colour sheet cannot hold a glyph's colours, and a cell holds few pixels).

--report writes, per family, every sheet as the game will decode it (_check_<family>_<sheet>.png)
and prints the largest change outside the cells (must be small) and the fit error inside them.
"""
import argparse
import glob
import os
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..', 'assets'))
from make_modern_buttons import CELL_ROLE, FAMILIES, paint_cell, scaled_rect  # noqa: E402
from txd_extract import rgb5a3  # noqa: E402

ROOT = 'H:/ShadowTheHedgehogDiskGC_Extract'
# texture name on the disc -> sheet id of CELL_ROLE (the extraction's names)
SHEET_OF = {'gamemenu': 'gamemenu', '001_command_icons_gc': 'command_icons_gc',
            '000_squarebuttons': 'squarebuttons', 'pause_main': 'pause_main',
            '000_VHArrow_gc': 'VHArrow_gc', 'Synopsis': 'Synopsis', '000_windowitem': 'windowitem'}
POSITIONS = ('south', 'east', 'west', 'north')
FAMILY_ORDER = ('xbox', 'playstation', 'switch')
SHARP = (5, 4)  # the sharp sheets' scale: 64 / 128 / 256 px sheets stay whole pixels
C4_FREE = 5   # entries a 16-colour sheet gives its buttons (merging the closest others)
KIND = {'a': 0, 'b': 1, 'x': 2, 'y': 3, 'start': 4, 'x_mask': 5}   # runtime enum CellKind
FNV_OFFSET, FNV_PRIME = 0xcbf29ce484222325, 0x100000001b3


def fnv1a64(data):
    h = FNV_OFFSET
    for b in data:
        h = ((h ^ b) * FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    return h


def textures(data, base):
    """(name, fmt, w, h, palette_offset, texel_offset, texel_bytes) of each texture in the RW texture
    dictionary at `base`."""
    cid, size, _ = struct.unpack_from('<III', data, base)
    if cid != 0x16:
        return
    p, end = base + 12, base + 12 + size
    while p + 12 <= end:
        c, csize, _ = struct.unpack_from('<III', data, p)
        if c == 0x15:
            body = p + 24
            name = data[body + 24:body + 56].split(b'\0')[0].decode('ascii', 'replace')
            w, h = struct.unpack_from('>HH', data, body + 92)
            fmt = data[body + 98]
            entries = {8: 16, 9: 256}.get(fmt, 0)
            pal = body + 104
            texels = pal + entries * 2 + 4
            nbytes = w * h // 2 if fmt == 8 else w * h
            yield name, fmt, w, h, pal, texels, nbytes
        p += 12 + csize


def disc_sheets(files):
    """{hash: sheet} and [(path, [(hash, span_offset, span_length)])]."""
    sheets, hits = {}, []
    paths = [p for p in glob.glob(files + '/**/*.txd', recursive=True)
             if '/fonts/' not in p.replace(os.sep, '/')]
    paths += glob.glob(files + '/csdFiles/**/*.gncp', recursive=True)
    for path in sorted(paths):
        data = open(path, 'rb').read()
        if path.endswith('.gncp'):
            nend = data.find(b'NEND')
            base = data.find(b'\x16\x00\x00\x00', nend) if nend >= 0 else -1
            if base < 0:
                continue
        else:
            base = 0
        found = []
        for name, fmt, w, h, pal, texels, nbytes in textures(data, base):
            sid = SHEET_OF.get(name)
            if sid is None or fmt not in (8, 9):
                continue
            cells = [c for c in CELL_ROLE if c[0] == sid]
            if sid == 'windowitem' and (w, h) != (128, 128):
                continue                    # InfoWindow.gncp's own windowitem has no buttons
            span = data[pal:texels + nbytes]
            hsh = fnv1a64(span)
            if hsh not in sheets:
                sheets[hsh] = dict(sid=sid, name=name, fmt=fmt, w=w, h=h, entries=16 if fmt == 8 else 256,
                                   span=span, texel_at=texels - pal, cells=cells,
                                   first=os.path.relpath(path, files).replace(os.sep, '/'))
            found.append((hsh, pal, len(span)))
        if found:
            hits.append((os.path.relpath(path, files).replace(os.sep, '/'), found))
    return sheets, hits


def index_at(fmt, w, x, y):
    """(byte offset in the texels, nibble shift or None) of pixel (x, y)."""
    if fmt == 9:   # C8: 8x4 tiles, 32 bytes
        return ((y // 4) * (w // 8) + x // 8) * 32 + (y % 4) * 8 + x % 8, None
    off = ((y // 8) * (w // 8) + x // 8) * 32 + (y % 8) * 4 + (x % 8) // 2
    return off, (4 if x % 2 == 0 else 0)


def read_indices(sheet):
    fmt, w, h = sheet['fmt'], sheet['w'], sheet['h']
    tex = sheet['span'][sheet['texel_at']:]
    out = np.zeros((h, w), np.int32)
    for y in range(h):
        for x in range(w):
            off, sh = index_at(fmt, w, x, y)
            out[y, x] = tex[off] if sh is None else (tex[off] >> sh) & 15
    return out


def read_palette(sheet):
    raw = sheet['span'][:sheet['entries'] * 2]
    return np.array([rgb5a3((raw[2 * i] << 8) | raw[2 * i + 1]) for i in range(sheet['entries'])], np.float64)


def encode_rgb5a3(c):
    r, g, b, a = [int(round(v)) for v in c]
    r, g, b, a = [max(0, min(255, v)) for v in (r, g, b, a)]
    if a >= 0xF8:
        return 0x8000 | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)
    return ((a >> 5) << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4)


def snap(c):
    """A colour as an RGB5A3 entry decodes."""
    return np.array(rgb5a3(encode_rgb5a3(c)), np.float64)


def feature(c):
    """Distance space: colour premultiplied by alpha, plus alpha (fully clear pixels all match)."""
    c = np.asarray(c, np.float64)
    a = c[..., 3:4] / 255.0
    return np.concatenate([c[..., :3] * a, c[..., 3:4]], axis=-1)


def fit(sheet, variants):
    """variants: {(cell_index, variant): RGBA uint8 array}. Returns (palette RGBA float [n,4],
    remap [n], {(cell_index, variant): index array})."""
    n = sheet['entries']
    pal = read_palette(sheet)
    idx = read_indices(sheet)
    inside = np.zeros(idx.shape, bool)
    for (x0, y0, x1, y1) in [(c[1], c[2], min(c[3], sheet['w']), min(c[4], sheet['h'])) for c in sheet['cells']]:
        inside[y0:y1, x0:x1] = True
    use_out = np.bincount(idx[~inside].ravel(), minlength=n)
    fixed = [i for i in range(n) if use_out[i] > 0]
    remap = np.arange(n)
    pixels = np.concatenate([feature(v.reshape(-1, 4).astype(np.float64)) for v in variants.values()])
    want = 24 if n == 256 else C4_FREE
    free = [i for i in range(n) if use_out[i] == 0]
    # Merge the closest pairs of entries still used outside the cells until enough are free.
    while len(free) < want and len(fixed) > 2:
        f = feature(pal[fixed])
        d = ((f[:, None, :] - f[None, :, :]) ** 2).sum(-1)
        np.fill_diagonal(d, np.inf)
        i, j = np.unravel_index(np.argmin(d), d.shape)
        keep, drop = (fixed[i], fixed[j]) if use_out[fixed[i]] >= use_out[fixed[j]] else (fixed[j], fixed[i])
        remap[remap == drop] = keep
        use_out[keep] += use_out[drop]
        use_out[drop] = 0
        fixed.remove(drop)
        free.append(drop)
    # k-means of the free entries over the glyph pixels, the fixed ones held.
    rng = np.random.default_rng(1)
    centres = feature(pal.copy())
    seeds = pixels[rng.choice(len(pixels), size=len(free), replace=len(pixels) < len(free))]
    centres[free] = seeds
    order = fixed + free
    for _ in range(25):
        c = centres[order]
        d = ((pixels[:, None, :] - c[None, :, :]) ** 2).sum(-1)
        nearest = np.argmin(d, axis=1)
        for k, entry in enumerate(order):
            if entry in fixed:
                continue
            m = pixels[nearest == k]
            if len(m):
                centres[entry] = m.mean(0)
    # Back from feature space to RGBA, snapped to what RGB5A3 can hold.
    out_pal = pal.copy()
    for entry in free:
        a = centres[entry][3]
        rgb = centres[entry][:3] / (a / 255.0) if a > 0.5 else np.zeros(3)
        out_pal[entry] = snap(np.concatenate([rgb, [a]]))
    feats = feature(out_pal[order])
    out_idx = {}
    for key, v in variants.items():
        f = feature(v.reshape(-1, 4).astype(np.float64))
        d = ((f[:, None, :] - feats[None, :, :]) ** 2).sum(-1)
        out_idx[key] = np.array(order)[np.argmin(d, axis=1)].reshape(v.shape[:2])
    return out_pal, remap, out_idx


def decode_sheet(sheet, pal, remap, cell_indices):
    idx = remap[read_indices(sheet)]
    for (ci, x0, y0, x1, y1), block in cell_indices.items():
        idx[y0:y1, x0:x1] = block
    return pal[idx].astype(np.uint8)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game-files', default=ROOT + '/game/files')
    ap.add_argument('--out', default=ROOT + '/source/runtime/src/gekko/button_glyph_data.inc')
    ap.add_argument('--report', default=None)
    ap.add_argument('--only', default=None, help='sheet id: report on that sheet only, write no data')
    args = ap.parse_args()
    from PIL import Image

    sheets, hits = disc_sheets(args.game_files)
    order = sorted(sheets, key=lambda k: (sheets[k]['sid'], sheets[k]['first']))
    if args.only:
        order = [k for k in order if sheets[k]['sid'] == args.only]
    lines = ['// Generated by source/tools/button_layouts/make_button_glyph_data.py - do not edit.',
             '// The modern button glyphs fitted into each button sheet of the PAL disc; see button_glyphs.h.', '']
    blob = bytearray()

    def put(data):
        at = len(blob)
        blob.extend(data)
        return at

    sheet_rows, family_rows, cell_rows, variant_rows, sharp_rows = [], [], [], [], []
    sharp_at = {}   # identical sharp blocks (a sheet's language copies) are stored once

    def put_sharp(im):
        data = im.tobytes()
        if data not in sharp_at:
            sharp_at[data] = put(data)
        return sharp_at[data]
    for si, hsh in enumerate(order):
        sh = sheets[hsh]
        rgba = decode_sheet(sh, read_palette(sh), np.arange(sh['entries']), {})
        img = Image.fromarray(rgba, 'RGBA')
        cells = sh['cells']
        first_cell = len(cell_rows)
        for (_sid, x0, y0, x1, y1, role) in cells:
            cell_rows.append((min(x0, sh['w']), y0, min(x1, sh['w']), min(y1, sh['h']), KIND[role]))
        first_family = len(family_rows)
        for fam in FAMILY_ORDER:
            variants = {}
            for ci, cell in enumerate(cells):
                role = cell[5]
                positions = ('start',) if role == 'start' else POSITIONS
                for vi, p in enumerate(positions):
                    variants[(ci, vi)] = np.array(paint_cell(img, cell, fam, p), np.uint8)
            pal, remap, idx = fit(sh, variants)
            pal_at = put(b''.join(struct.pack('>H', encode_rgb5a3(c)) for c in pal))
            remap_at = put(bytes(int(v) for v in remap))
            first_variant = len(variant_rows)
            for ci, cell in enumerate(cells):
                nvar = 1 if cell[5] == 'start' else 4
                positions = ('start',) if cell[5] == 'start' else POSITIONS
                for vi in range(nvar):
                    variant_rows.append(put(bytes(int(v) for v in idx[(ci, vi)].ravel())))
                    sharp = paint_cell(img, cell, fam, positions[vi], SHARP)
                    _s, x0, y0, x1, y1, _r = cell
                    sx0, sy0, sx1, sy1 = scaled_rect(x0, y0, min(x1, sh['w']), min(y1, sh['h']), *SHARP)
                    assert sharp.size == (sx1 - sx0, sy1 - sy0), (cell, sharp.size)
                    sharp_rows.append(put_sharp(sharp.convert('RGBA')))
            family_rows.append((pal_at, remap_at, first_variant))
            if args.report:
                os.makedirs(args.report, exist_ok=True)
                # As the game decodes it with the launcher's default binding (positions by label).
                blocks = {}
                for ci, cell in enumerate(cells):
                    _s, x0, y0, x1, y1, role = cell
                    vi = 0 if role == 'start' else {'a': 0, 'b': 1, 'x': 2, 'y': 3, 'x_mask': 2}[role]
                    blocks[(ci, x0, y0, min(x1, sh['w']), min(y1, sh['h']))] = idx[(ci, vi)]
                out = decode_sheet(sh, pal, remap, blocks)
                inside = np.zeros(out.shape[:2], bool)
                for (ci, x0, y0, x1, y1) in blocks:
                    inside[y0:y1, x0:x1] = True
                diff = np.abs(out.astype(int) - rgba.astype(int))[~inside]
                ideal = {k: variants[(k[0], 0 if cells[k[0]][5] == 'start' else
                                      {'a': 0, 'b': 1, 'x': 2, 'y': 3, 'x_mask': 2}[cells[k[0]][5]])]
                         for k in blocks}
                err = [np.abs(out[y0:y1, x0:x1].astype(int) - ideal[(ci, x0, y0, x1, y1)].astype(int)).mean()
                       for (ci, x0, y0, x1, y1) in blocks]
                print('%-16s %-28s %-11s outside max %3d (%5d px changed)  cells mean err %.1f' % (
                    sh['sid'], sh['first'][-28:], fam, diff.max() if diff.size else 0,
                    int((diff.max(-1) > 0).sum()) if diff.size else 0, float(np.mean(err))))
                Image.fromarray(out, 'RGBA').save(os.path.join(
                    args.report, '_check_%s_%s_%d.png' % (fam, sh['sid'], si)))
        sheet_rows.append((hsh, sh['fmt'], sh['w'], sh['h'], sh['entries'], len(sh['span']), sh['texel_at'],
                           first_cell, len(cells), first_family))

    if args.only:
        return
    lines.append('constexpr SheetRow kSheets[] = {')
    for r in sheet_rows:
        lines.append('    {0x%016XULL, %d, %d, %d, %d, %d, %d, %d, %d, %d},' % r)
    lines.append('};')
    lines.append('constexpr CellRow kCells[] = {')
    for r in cell_rows:
        lines.append('    {%d, %d, %d, %d, CellKind(%d)},' % r)
    lines.append('};')
    lines.append('// Per sheet, one row per family in the order xbox, playstation, switch.')
    lines.append('constexpr FamilyRow kFamilies[] = {')
    for r in family_rows:
        lines.append('    {%d, %d, %d},' % r)
    lines.append('};')
    lines.append('// Offset in kBlob of each cell variant (face cells: south, east, west, north; others: one).')
    lines.append('constexpr u32 kVariants[] = {')
    for i in range(0, len(variant_rows), 12):
        lines.append('    ' + ' '.join('%d,' % v for v in variant_rows[i:i + 12]))
    lines.append('};')
    lines.append('// The sharp sheets: each sheet scaled by kSharpNum / kSharpDen, its cells painted in full colour.')
    lines.append('constexpr u32 kSharpNum = %d, kSharpDen = %d;' % SHARP)
    lines.append('// Offset in kBlob of the sharp cell of each variant, RGBA8 rows of the scaled cell (order of kVariants).')
    lines.append('constexpr u32 kSharpVariants[] = {')
    for i in range(0, len(sharp_rows), 12):
        lines.append('    ' + ' '.join('%d,' % v for v in sharp_rows[i:i + 12]))
    lines.append('};')
    lines.append('// Files that hold a sheet: path, sheet (by hash), span offset in the file.')
    lines.append('constexpr FileRow kFiles[] = {')
    for path, found in hits:
        for hsh, at, n in found:
            lines.append('    {"%s", %d, %d},' % (path, order.index(hsh), at))
    lines.append('};')
    lines.append('constexpr u32 kBlobSize = %d;' % len(blob))
    lines.append('alignas(4) constexpr unsigned char kBlob[] = {')
    for i in range(0, len(blob), 32):
        lines.append('    ' + ','.join('%d' % b for b in blob[i:i + 32]) + ',')
    lines.append('};')
    open(args.out, 'w', encoding='utf-8', newline='\n').write('\n'.join(lines) + '\n')
    print('%d sheets, %d files, %d bytes of glyph data -> %s' % (
        len(sheet_rows), sum(len(f) for _, f in hits), len(blob), args.out))


if __name__ == '__main__':
    main()
