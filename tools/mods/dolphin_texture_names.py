#!/usr/bin/env python3
"""Reproduce Dolphin hires-texture names from the game's own TXD bytes and match texture packs.

    python -I tools/mods/dolphin_texture_names.py --game <disc root>/files --pack <dir> [--pack <dir> ...]
                                                  [--json out.json] [--dump-names out.txt]

What it proves (docs/mods/TEXTURE_PACKS.md): a Dolphin texture-pack file name

    tex1_{W}x{H}[_m]_{texhash:016x}[_{tluthash:016x}]_{fmt}.{png|dds}

is reproduced from the RenderWare GameCube raster as stored on the disc, with
    texhash  = XXH64(seed 0) over the BASE LEVEL texel blocks only
               (gx block-padded size of W x H in format fmt; mip levels excluded)
    tluthash = XXH64(seed 0) over palette[min .. max] (2 bytes per entry, big-endian as stored),
               where min/max are the smallest/largest palette index that occurs in the base level
               (C4: both nibbles of every byte, C8: every byte, C14X2: every big-endian u16 & 0x3FFF)
    fmt      = the GX texture format number in decimal (C4 = 8, C8 = 9, C14X2 = 10, CMPR = 14 ...)
    _m       = the sampler that draws the texture has mip filtering enabled (a draw-time property,
               not a property of the bytes: the hash is the same with or without it).

Every pack file is classified as: matched (some game texture produces exactly its key), or unmatched.
Only parses data; never executes anything from a pack. Requires the 'xxhash' Python package.
"""
import argparse
import collections
import json
import os
import re
import struct
import sys

import xxhash

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 're'))
from files_oneaudit import prs_expand  # noqa: E402

# format -> (block width, block height, bits per pixel)
GX_BLOCK = {0: (8, 8, 4), 1: (8, 4, 8), 2: (8, 4, 8), 3: (4, 4, 16), 4: (4, 4, 16), 5: (4, 4, 16),
            6: (4, 4, 32), 8: (8, 8, 4), 9: (8, 4, 8), 10: (4, 4, 16), 14: (8, 8, 4)}

NAME_RE = re.compile(r'^tex1_(\d+)x(\d+)(_m)?_([0-9a-f]{16})(?:_([0-9a-f]{16}|\$))?_(\d+)'
                     r'(?:_mip(\d+)|_arb)?\.(png|dds)$', re.I)


def gx_size(fmt, w, h):
    bw, bh, bpp = GX_BLOCK[fmt]
    return ((w + bw - 1) // bw * bw) * ((h + bh - 1) // bh * bh) * bpp // 8


def palette_range(fmt, base):
    lo, hi = 0xFFFF, 0
    if fmt == 8:
        for b in base:
            a, c = b >> 4, b & 0xF
            lo = min(lo, a, c)
            hi = max(hi, a, c)
    elif fmt == 9:
        lo, hi = min(base), max(base)
    elif fmt == 10:
        for i in range(0, len(base), 2):
            v = ((base[i] << 8) | base[i + 1]) & 0x3FFF
            lo = min(lo, v)
            hi = max(hi, v)
    return lo, hi


def txd_textures(data):
    """Yield dicts for every platform-6 TEXTURENATIVE in a TXD stream."""
    if len(data) < 12:
        return
    cid, size, _ = struct.unpack_from('<III', data, 0)
    if cid != 0x16:
        return
    end = min(len(data), 12 + size)
    p = 12
    while p + 12 <= end:
        c, csize, _ = struct.unpack_from('<III', data, p)
        if c == 0x15:
            b = p + 12
            _, ssize, _ = struct.unpack_from('<III', data, b)
            body = b + 12
            plat = struct.unpack_from('>I', data, body)[0]
            if plat == 6:
                name = data[body + 24:body + 56].split(b'\0')[0].decode('ascii', 'replace')
                w, h = struct.unpack_from('>HH', data, body + 92)
                _u, mips, fmt, pal_fmt = struct.unpack_from('>BBBB', data, body + 96)
                pal_count = {8: 16, 9: 256, 10: 16384}.get(fmt, 0)
                pal_off = body + 104
                len_off = pal_off + pal_count * 2
                data_len = struct.unpack_from('>I', data, len_off)[0]
                pix = data[len_off + 4:len_off + 4 + data_len]
                yield dict(name=name, w=w, h=h, mips=mips, fmt=fmt, pal_fmt=pal_fmt,
                           palette=data[pal_off:len_off], pixels=pix)
        p += 12 + csize


def one_members(blob):
    """Yield (member name, bytes) of a 'One Ver 0.xx' archive."""
    if len(blob) < 0x60 or not blob[12:22].startswith(b'One Ver 0.'):
        return
    d = 12
    legacy = float(blob[d + 8:d + 12].split(b'\0')[0] or b'0.60') <= 0.59
    n = struct.unpack_from('<I', blob, d + 0x10)[0]
    for i in range(2, n + 2):
        o = d + 0x34 + i * 0x38
        raw = blob[o:o + 0x38]
        nm = raw[:0x20 if legacy else 0x2C]
        name = nm.split(b'\0')[0].decode('latin1')
        size, off, fl = struct.unpack_from('<III', raw, 0x20 if legacy else 0x2C)
        a = d + off
        try:
            if fl & 1:
                out, _used, _term = prs_expand(blob, a, len(blob))
            else:
                out = blob[a:a + size]
        except ValueError:
            continue
        yield name, out


def dolphin_key(t):
    fmt, w, h = t['fmt'], t['w'], t['h']
    if fmt not in GX_BLOCK:
        return None
    size = gx_size(fmt, w, h)
    base = t['pixels'][:size]
    if len(base) < size:
        return None
    tex = xxhash.xxh64(base, seed=0).hexdigest()
    tlut = ''
    if fmt in (8, 9, 10):
        lo, hi = palette_range(fmt, base)
        tlut = xxhash.xxh64(t['palette'][2 * lo:2 * (hi + 1)], seed=0).hexdigest()
    return (w, h, tex, tlut, fmt)


def key_name(key, mip):
    w, h, tex, tlut, fmt = key
    return 'tex1_%dx%d%s_%s%s_%d' % (w, h, '_m' if mip else '', tex, ('_' + tlut) if tlut else '', fmt)


def scan_game(root):
    keys = {}            # key -> list of (source, texture name, mips)
    alt_full = {}        # hypothesis check: texhash over ALL stored levels
    alt_fullpal = {}     # hypothesis check: tluthash over the whole palette
    alt_tex = set()      # texel hash alone (palette differs)
    for dirpath, _dirs, files in os.walk(root):
        for f in files:
            path = os.path.join(dirpath, f)
            low = f.lower()
            if not (low.endswith('.one') or low.endswith('.txd')):
                continue
            blob = open(path, 'rb').read()
            rel = os.path.relpath(path, root).replace('\\', '/')
            streams = list(one_members(blob)) if low.endswith('.one') else [(f, blob)]
            for member, data in streams:
                for t in txd_textures(data):
                    k = dolphin_key(t)
                    if k is None:
                        continue
                    keys.setdefault(k, []).append((rel + ':' + member, t['name'], t['mips']))
                    full = xxhash.xxh64(t['pixels'], seed=0).hexdigest()
                    alt_full[(k[0], k[1], full, k[3], k[4])] = k
                    alt_tex.add((k[0], k[1], k[2], k[4]))
                    if t['fmt'] in (8, 9):
                        alt_fullpal[(k[0], k[1], k[2], xxhash.xxh64(t['palette'], seed=0).hexdigest(), k[4])] = k
    return keys, alt_full, alt_fullpal, alt_tex


def scan_pack(root):
    out = []
    for dirpath, _dirs, files in os.walk(root):
        for f in files:
            m = NAME_RE.match(f)
            if m:
                w, h, mflag, tex, tlut, fmt, mipn, ext = m.groups()
                out.append(dict(file=os.path.relpath(os.path.join(dirpath, f), root).replace('\\', '/'),
                                key=(int(w), int(h), tex.lower(), (tlut or '').lower(), int(fmt)),
                                m=bool(mflag), mip=mipn, ext=ext.lower(), arb='_arb' in f.lower()))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', required=True, help='disc files/ directory')
    ap.add_argument('--pack', action='append', default=[])
    ap.add_argument('--json')
    ap.add_argument('--dump-names')
    args = ap.parse_args()

    keys, alt_full, alt_fullpal, alt_tex = scan_game(args.game)
    print('game: %d distinct texture keys (%d raster records) under %s'
          % (len(keys), sum(len(v) for v in keys.values()), args.game))
    if args.dump_names:
        with open(args.dump_names, 'w', encoding='utf-8', newline='\n') as fh:
            for k, srcs in sorted(keys.items()):
                fh.write('%s\t%s\t%s\n' % (key_name(k, False), srcs[0][0], srcs[0][1]))
    report = {}
    for pack in args.pack:
        files = scan_pack(pack)
        tally = collections.Counter()
        rows = []
        for e in files:
            k = e['key']
            if e['mip'] is not None:
                tally['custom_mip_level_files'] += 1
            if k in keys:
                tally['matched'] += 1
                src = keys[k][0]
                rows.append((e['file'], 'MATCH', src[0], src[1]))
            elif e['key'][3] == '$' and any(kk[:3] == k[:3] and kk[4] == k[4] for kk in keys):
                tally['matched_wildcard'] += 1
                rows.append((e['file'], 'MATCH-WILDCARD', '', ''))
            elif k in alt_full:
                tally['matched_only_if_all_levels_hashed'] += 1
                rows.append((e['file'], 'ALT-FULL', '', ''))
            elif k in alt_fullpal:
                tally['matched_only_if_whole_palette_hashed'] += 1
                rows.append((e['file'], 'ALT-PAL', '', ''))
            elif (k[0], k[1], k[2], k[4]) in alt_tex:
                tally['texels_match_palette_differs'] += 1
                rows.append((e['file'], 'TEX-ONLY', '', ''))
            else:
                tally['unmatched'] += 1
                rows.append((e['file'], 'NO-MATCH', '', ''))
        print('\npack %s: %d tex1_ files' % (pack, len(files)))
        for name, n in sorted(tally.items()):
            print('  %-40s %d' % (name, n))
        for r in rows:
            print('  %-10s %s%s' % (r[1], r[0], ('   <- %s [%s]' % (r[2], r[3])) if r[2] else ''))
        report[pack] = dict(files=len(files), tally=dict(tally), rows=rows)
    if args.json:
        with open(args.json, 'w', encoding='utf-8', newline='\n') as fh:
            json.dump(report, fh, indent=1)
    return 0


if __name__ == '__main__':
    sys.exit(main())
