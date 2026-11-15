#!/usr/bin/env python3
"""check_formats.py - structural checks for the disc file kinds the game reads without checking them.

Reference implementation of the rules that docs/mods/map/FORMAT_CHECKS.md ports to C++ (check_file_format in
runtime/mods/afs.cpp).  Stdlib only, never executes anything it reads, never writes next to its inputs.

Every rule belongs to one of two tiers:
  error   the game would read outside the buffer, loop on garbage, or run on a table that is certainly cut off.
          Zero false positives are required on every real file (disc trees and real mods).
  warning the file differs from every shipped file in a way the game survives (or that we cannot prove harmful).
          Reported, never used to reject a file.

Usage:
  check_formats.py scan  --root DIR [--root DIR ...] [--json out.json]
        DIR is a game "files" tree or a mod's "files" folder; kinds are found by extension, the game path is the path
        relative to DIR.  Prints a per-kind table, every error and warning, exits 1 when there is any error.
  check_formats.py mutate --root DIR --work DIR [--json out.json]
        Writes truncated (-1 byte, half) and zeroed-header copies of every sampled file below --work (in per-kind
        folders) and reports which of them the checker catches.  Exits 1 when a mutation of a kind that has a header
        is missed (kinds listed in NO_HEADER are reported, not failed).
  check_formats.py file PATH [GAME_PATH]
        One file.
"""
import argparse
import json
import math
import os
import re
import struct
import sys

KINDS = ('.flo', '.dat', '.inf', '.fnt', '.met', '.gncp', '.mlt', '.bin', '.scr', '.fog', '.bnr', '.sfd', '.tp', '.srd', '.conf')
MAX_LAYOUT_ENTRIES = 0x800      # SetManager slot arrays (guest, no bounds check)
MAX_LAYOUT_MISC = 0x10000       # SetManager misc pool (guest, no bounds check)


def be32(d, o):
    return struct.unpack_from('>I', d, o)[0]


def le32(d, o):
    return struct.unpack_from('<I', d, o)[0]


def le16(d, o):
    return struct.unpack_from('<H', d, o)[0]


def finite_f32(d, o):
    v = struct.unpack_from('<f', d, o)[0]
    return not (math.isnan(v) or math.isinf(v))


class Result:
    def __init__(self):
        self.errors = []
        self.warnings = []

    def err(self, rule, msg):
        self.errors.append('%s %s' % (rule, msg))

    def warn(self, rule, msg):
        self.warnings.append('%s %s' % (rule, msg))


# ------------------------------------------------------------------------------------------------ 1B container
def reloc_walk(d, start, length, data_len, r, what):
    """Walk the relocation stream of a "1B" container (reader loop 0x8040F890..0x8040F9A8).  Returns False on damage."""
    pos = 0
    i = 0
    end = start + length
    seen_end_zero = False
    while i < end - start:
        c = d[start + i]
        i += 1
        hi = c >> 6
        if hi == 0:
            continue                      # padding / no-op code
        if hi == 1:
            delta = c & 0x3F
        elif hi == 2:
            if start + i + 1 > end:
                r.err('C08', '%s relocation stream is cut inside a code' % what)
                return False
            delta = ((c & 0x3F) << 8) | d[start + i]
            i += 1
        else:
            if start + i + 3 > end:
                r.err('C08', '%s relocation stream is cut inside a code' % what)
                return False
            delta = ((c & 0x3F) << 24) | (d[start + i] << 16) | (d[start + i + 1] << 8) | d[start + i + 2]
            i += 3
        pos += delta
        if pos * 4 + 4 > data_len:
            r.err('C09', '%s relocation reaches word %d, past the %d data bytes (the loader would write there)' %
                  (what, pos, data_len))
            return False
    return True


def check_container(d, r, what, strict_tail=True):
    """-> (D, R) or None.  Big-endian 0x20 header, data, relocation stream (docs/re/formats.md section 2)."""
    n = len(d)
    if n < 0x20:
        r.err('C01', '%s is %d bytes, shorter than the 32-byte container header' % (what, n))
        return None
    size, D, R = be32(d, 0), be32(d, 4), be32(d, 8)
    if size > n:
        r.warn('C04', '%s header says %d bytes but the file is %d (cut by %d byte(s); the loader reads the real size, '
               'a real mod ships one byte short)' % (what, size, n, size - n))
    elif size < n:
        r.warn('C04', '%s header says %d bytes but the file is %d (a real mod ships longer files than its header says)' % (what, size, n))
    if D % 4 or R % 4:
        r.err('C06', '%s data/relocation lengths (%d, %d) are not multiples of 4' % (what, D, R))
        return None
    if 0x20 + D + R > n:
        r.err('C07', '%s data (%d) + relocation (%d) overrun the file' % (what, D, R))
        return None
    if d[0x17] != 0x42:
        r.err('C02', '%s byte order marker at +0x17 is %#x, expected 0x42 "B"' % (what, d[0x17]))
        return None
    if not reloc_walk(d, 0x20 + D, R, D, r, what):
        return None
    if strict_tail:
        if be32(d, 0xC) != 1 or be32(d, 0x10) != 0:
            r.warn('C05', '%s table counts (%d, %d) are not the shipped (1, 0)' % (what, be32(d, 0xC), be32(d, 0x10)))
    return D, R


def check_bin(d, name, r):
    base = name.rsplit('/', 1)[-1].lower()
    n = len(d)
    if base == 'setid.bin':
        if n < 8:
            r.err('B01', 'is %d bytes, shorter than its 8-byte header' % n)
            return
        count = le32(d, 4)
        want = 8 + 12 * count
        if want > n:
            r.err('B02', 'header says %d records (%d bytes) but the file is %d (truncated or damaged)' % (count, want, n))
        elif want != n:
            r.err('B03', 'header says %d records (%d bytes) but the file is %d bytes' % (count, want, n))
        if le32(d, 0) != 0:
            r.warn('B04', 'first header word is %d, shipped files have 0' % le32(d, 0))
        return
    if base.endswith('_light.bin'):
        if n < 0x340:
            r.err('B05', 'is %d bytes, the loader reads 0x340 (832)' % n)
            return
        if n != 0x340:
            r.warn('B06', 'is %d bytes, shipped files are exactly 832' % n)
        words = struct.unpack_from('<208I', d, 0)
        bad = [i for i, w in enumerate(words) if struct.unpack('<f', struct.pack('<I', w))[0] != struct.unpack('<f', struct.pack('<I', w))[0]]
        if bad:
            r.err('B07', 'word %d is a NaN (light table is floats)' % bad[0])
        if not any(d[:16]):
            r.warn('B08', 'first 16 bytes are all zero (no shipped light table starts that way)')
        return
    if base.startswith('scoredat') or base.startswith('audiodata') or base.endswith('_hint.bin'):
        got = check_container(d, r, 'container')
        if got is None:
            return
        D, R = got
        if base.endswith('_hint.bin'):
            if R != 0 or D % 12 or D < 12:
                r.err('H01', 'hint table data is %d bytes with %d relocation bytes (expected n*12+12, none)' % (D, R))
                return
            last = D - 12
            if be32(d, 0x20 + last) != 0xFFFFFFFF or be32(d, 0x20 + last + 4) != 0xFFFFFFFF:
                r.err('H02', 'hint table has no {-1, -1, 0} terminator record (the reader would run on)')
            for off in range(0, last, 12):
                if be32(d, 0x20 + off) == 0xFFFFFFFF:
                    r.warn('H03', 'terminator before the last record')
                    break
        elif base.startswith('audiodata'):
            if D < 8:
                r.err('A01', 'audio table data is %d bytes, shorter than its row header' % D)
                return
            rows = be32(d, 0x24)
            rows_end = 8 + 0x18 * rows
            if rows_end > D:
                r.err('A02', 'audio table says %d rows (%d bytes) but its data is %d bytes' % (rows, rows_end, D))
                return
            for i in range(rows):
                noff = be32(d, 0x28 + 0x18 * i)
                if noff < rows_end or noff >= D:
                    r.err('A03', 'audio row %d name offset %#x is outside the string area [%#x, %#x)' %
                          (i, noff, rows_end, D))
                    break
                if d.find(b'\0', 0x20 + noff, 0x20 + D) < 0:
                    r.err('A04', 'audio row %d name has no terminator' % i)
                    break
        return
    # unknown .bin (for example a mod's own table): no rule


# ------------------------------------------------------------------------------------------------ .dat
def check_dat(d, name, r):
    base = name.rsplit('/', 1)[-1].lower()
    n = len(d)
    if base.endswith('_cam.dat'):
        if n < 0x18:
            r.err('K01', 'is %d bytes, shorter than the 24-byte camera header' % n)
            return
        if le32(d, 0) != 3 or le32(d, 4) != 0x0131CD93:
            r.err('K02', 'camera header {%#x, %#x} is not {3, 0x0131CD93}' % (le32(d, 0), le32(d, 4)))
            return
        count = le32(d, 0x14)
        want = 0x18 + 220 * count
        if want > n:
            r.err('K03', 'header says %d camera records (%d bytes) but the file is %d (truncated or damaged)' % (count, want, n))
        elif want != n:
            r.err('K04', 'header says %d camera records (%d bytes) but the file is %d bytes' % (count, want, n))
        m = re.search(r'stg(\d{4})_cam\.dat$', base)
        if m and le32(d, 8) != int(m.group(1)):
            r.warn('K05', 'stage number %d in the header, the file name says %d' % (le32(d, 8), int(m.group(1))))
        return
    # layout files stgNNNN_cmn|nrm|hrd|ds1.dat ("sky2")
    if n < 12:
        r.err('L01', 'is %d bytes, shorter than the 12-byte layout header' % n)
        return
    if d[:4] != b'sky2':
        r.err('L02', 'is not a layout file (no "sky2" signature)')
        return
    count, misc = le32(d, 4), le32(d, 8)
    want = 12 + 0x2C * count + misc
    if want != n:
        r.err('L03', 'header says %d entries + %d misc bytes = %d bytes but the file is %d (truncated or damaged)' %
              (count, misc, want, n))
        return
    if misc % 4:
        r.err('L04', 'misc length %d is not a multiple of 4' % misc)
    if count > MAX_LAYOUT_ENTRIES:
        r.err('L05', '%d entries, the loader has room for %d and does not check' % (count, MAX_LAYOUT_ENTRIES))
    if misc > MAX_LAYOUT_MISC:
        r.err('L06', '%d misc bytes, the loader has room for %d and does not check' % (misc, MAX_LAYOUT_MISC))
    total = 0
    for i in range(count):
        ln = le32(d, 12 + 0x2C * i + 0x24)
        if ln % 4:
            r.err('L07', 'entry %d misc length %d is not a multiple of 4' % (i, ln))
            break
        total += ln
    else:
        if total != misc:
            r.err('L08', 'entry misc lengths add up to %d, the header says %d' % (total, misc))
    if r.errors:
        return
    for i in range(count):
        if not (le32(d, 12 + 0x2C * i + 0x1C) & 1):
            r.warn('L09', 'entry %d is not enabled in its saved flags (every shipped entry is)' % i)
            break


def layout_numbers(d):
    """(entries, misc) of a valid layout file, else None."""
    if len(d) >= 12 and d[:4] == b'sky2':
        return le32(d, 4), le32(d, 8)
    return None


# ------------------------------------------------------------------------------------------------ .fnt / .met
def check_fnt(d, name, r):
    n = len(d)
    if n < 4:
        r.err('F01', 'is %d bytes, shorter than its count word' % n)
        return
    count = le32(d, 0)
    table_end = 4 + 20 * count
    if count == 0 or table_end > n:
        r.err('F02', 'says %d records (%d bytes of table) but the file is %d bytes (truncated or damaged)' % (count, table_end, n))
        return
    prev_key = None
    prev_off = None
    for i in range(count):
        off, key = struct.unpack_from('<Ii', d, 4 + 20 * i)
        if off & 1 or off < table_end or off + 2 > n:
            r.err('F03', 'record %d text offset %d is odd or outside the string area [%d, %d)' % (i, off, table_end, n))
            return
        j = off
        while j + 1 < n and (d[j] | d[j + 1]):
            j += 2
        if j + 1 >= n:
            r.err('F04', 'record %d string has no zero terminator before the end of the file (truncated)' % i)
            return
        if prev_key is not None and key <= prev_key:
            r.warn('F05', 'record %d key %d does not ascend (the game binary-searches the table)' % (i, key))
        prev_key = key
        if prev_off is not None and off <= prev_off:
            r.warn('F06', 'record %d text offset does not ascend' % i)
        prev_off = off
    if prev_key != 9999000:
        r.warn('F07', 'last key is %s, shipped banks end with the sentinel 9999000' % prev_key)


def check_met(d, name, r):
    stem = name.rsplit('/', 1)[-1]
    stem = stem[:stem.rfind('.')] if '.' in stem else stem
    if not d:
        r.err('M01', 'is empty')
        return
    if b'\0' in d:
        r.err('M02', 'contains a NUL byte (a text file)')
        return
    text = d.decode('latin-1')
    if '\r' in text:
        r.warn('M13', 'has carriage returns (shipped files use bare LF; the Reloaded fonts ship CRLF)')
        text = text.replace('\r', '')
    lines = text.split('\n')
    if lines[-1] != '':
        r.err('M03', 'does not end with a newline (the last glyph line would be cut)')
        return
    lines.pop()
    if len(lines) < 4:
        r.err('M04', 'has %d lines, a metrics file needs the header, texture name, parameter and one glyph' % len(lines))
        return
    if lines[0] != 'METRICS1':
        r.err('M05', 'first line is %r, expected METRICS1 (the only variant shipped)' % lines[0][:20])
        return
    if lines[1] != stem:
        r.warn('M06', 'texture name %r is not the file name %r (shipped files name their own sheet; Reloaded shares Movie_EN00 '
               'and ships a matching .txd; the dictionary is another file, so this cannot be an error here)' % (lines[1][:40], stem))
    try:
        int(lines[2].strip())
    except ValueError:
        r.err('M07', 'third line %r is not an integer' % lines[2][:20])
        return
    glyphs = 0
    seen = set()
    for k, line in enumerate(lines[3:], 4):
        if not line.strip():
            continue
        t = line.split('#')[0].split()
        try:
            if len(t) != 5:
                raise ValueError
            code, x0, y0, x1, y1 = (int(v, 0) for v in t)
        except ValueError:
            r.err('M08', 'line %d %r is not "code x0 y0 x1 y1"' % (k, line[:40]))
            return
        if code <= 0 or code > 0xFFFF:
            r.err('M09', 'line %d: code %d is outside 1..65535' % (k, code))
            return
        if x0 < 0 or y0 < 0 or x1 < x0 or y1 < y0:
            r.err('M10', 'line %d: rectangle %d,%d - %d,%d is inverted or negative' % (k, x0, y0, x1, y1))
            return
        if code in seen:
            r.warn('M11', 'line %d: code %d is defined twice' % (k, code))
        seen.add(code)
        glyphs += 1
    if not glyphs:
        r.err('M12', 'has no glyph lines')
    if '\r' in text:
        r.warn('M13', 'has carriage returns (shipped files use bare LF)')


# ------------------------------------------------------------------------------------------------ .gncp
def check_gncp(d, name, r):
    n = len(d)
    if n < 12:
        r.err('G01', 'is %d bytes, too short for a CSD project' % n)
        return
    blob2 = b''
    if d[:4] in (b'CPAF', b'FAPC'):
        rd = be32 if d[:4] == b'CPAF' else le32
        l1 = rd(d, 4)
        if 8 + l1 + 4 > n:
            r.err('G02', 'first blob (%d bytes) runs past the file (%d bytes)' % (l1, n))
            return
        l2 = rd(d, 8 + l1)
        if 12 + l1 + l2 > n:
            r.err('G03', 'second blob (%d bytes) runs past the file (truncated or damaged)' % l2)
            return
        if 12 + l1 + l2 != n:
            r.warn('G04', '%d bytes follow the second blob (shipped files have none)' % (n - 12 - l1 - l2))
        blob1 = d[8:8 + l1]
        blob2 = d[12 + l1:12 + l1 + l2]
    else:
        blob1 = d
    m = len(blob1)
    if m < 0x20 or blob1[:4] != b'NGIF':
        r.err('G05', 'project image has no "NGIF" header')
        return
    data_off, total, reloc_off = be32(blob1, 0xC), be32(blob1, 0x10), be32(blob1, 0x14)
    if data_off < 0x18 or data_off + total > m or total < 0x20:
        r.err('G06', 'data area [%#x, +%#x) does not fit the %d-byte image' % (data_off, total, m))
        return
    if reloc_off < data_off or reloc_off + 0x10 > m:
        r.err('G07', 'offset table at %#x is outside the image' % reloc_off)
        return
    count = be32(blob1, reloc_off + 8)
    if reloc_off + 0x10 + 4 * count > m:
        r.err('G08', 'offset table of %d entries overruns the image' % count)
        return
    data = blob1[data_off:data_off + total]
    for i in range(count):
        o = be32(blob1, reloc_off + 0x10 + 4 * i) & ~3
        if o + 4 > total:
            r.err('G09', 'relocation %d at data offset %#x is outside the %d data bytes (the loader would write there)' % (i, o, total))
            return
        if be32(data, o) > total:
            r.err('G10', 'relocated pointer %d (%#x) points outside the data area' % (i, be32(data, o)))
            return
    if data[:4] != b'nCPJ' or le32(data, 4) != total - 8:
        r.err('G11', 'data area does not hold one "nCPJ" chunk of %d bytes' % (total - 8))
        return
    if be32(data, 8) + 0x10 > total:
        r.err('G12', 'project root %#x is outside the data area' % be32(data, 8))
        return
    if blob2:
        if len(blob2) < 12 or le32(blob2, 0) != 0x16 or le32(blob2, 4) + 12 > len(blob2):
            r.err('G13', 'embedded texture dictionary is not a whole RenderWare 0x16 chunk')


# ------------------------------------------------------------------------------------------------ .mlt
def check_mlt(d, name, r):
    n = len(d)
    if n < 0x70 or d[:8] != b'gcaxMLT ':
        r.err('S01', 'is not a gcax sound bank (no "gcaxMLT " signature)')
        return
    if be32(d, 8) != 0 or be32(d, 12) != n:
        r.err('S02', 'header says %d bytes but the file is %d (truncated or damaged)' % (be32(d, 12), n))
        return
    if d[0x10:0x18] != b'gcaxMLTM':
        r.err('S03', 'no "gcaxMLTM" entry table at 0x10')
        return
    if d[0x40:0x48] != b'gcaxMPB ' or d[0x50:0x58] != b'gcaxMPBW':
        r.err('S04', 'no "gcaxMPB" / "gcaxMPBW" chunk at 0x40 / 0x50')
        return
    w = be32(d, 0x5C)
    ip = 0x60 + w
    if ip + 16 > n or d[ip:ip + 8] != b'gcaxMPBP':
        r.err('S05', 'wave data (%d bytes) does not end at a "gcaxMPBP" chunk inside the file' % w)
        return
    p = be32(d, ip + 12)
    end = ip + 16 + p
    if end > n:
        r.err('S06', 'program data (%d bytes) runs past the file end (%d > %d)' % (p, end, n))
        return
    if 0x50 + be32(d, 0x4C) != end:
        r.warn('S07', 'MPB size field does not end at the program data')
    if n - end >= 32:
        r.warn('S08', '%d bytes follow the program data (shipped banks pad to 32)' % (n - end))


# ------------------------------------------------------------------------------------------------ .scr .fog .sfd .tp .bnr .srd
def check_scr(d, name, r):
    n = len(d)
    if n < 0x188:
        r.err('E01', 'is %d bytes, shorter than the 0x188-byte event script header' % n)
        return
    count = le32(d, 4)
    want = 0x188 + 0x80 * count
    if want > n:
        r.err('E02', 'header says %d records (%d bytes) but the file is %d (truncated or damaged)' % (count, want, n))
    elif want != n:
        r.err('E03', 'header says %d records (%d bytes) but the file is %d bytes' % (count, want, n))
    m = re.search(r'event(\d{4})\.scr$', name.lower())
    if m and le32(d, 0) != int(m.group(1)):
        r.warn('E04', 'event id %d in the header, the file name says %d' % (le32(d, 0), int(m.group(1))))


def check_fog(d, name, r):
    n = len(d)
    if n < 960:
        r.err('O01', 'is %d bytes, the loader reads 24 entries of 40 bytes (960)' % n)
        return
    if n != 960:
        r.warn('O02', 'is %d bytes, shipped files are exactly 960' % n)
    for i in range(24):
        for off in (16, 20, 28, 32):
            if not finite_f32(d, 40 * i + off):
                r.err('O03', 'entry %d has a NaN/infinite float at +%d' % (i, off))
                return
    if not any(d[:40]):
        r.err('O04', 'first fog entry is empty (every shipped file has its first entry filled)')


def check_sfd(d, name, r, size=None, tail=None):
    """An MPEG program stream, walked packet by packet (the C++ check_movie does the same). The disc's movies start
    with a pack header and end with the program end code padded with 0xFF; movies muxed by CRI's later sfdmux (the
    community's re-edited intros) start with a private stream 2 packet ("SofdecStream2") and end at the last packet's
    last byte, with no end code and no padding. Whole = every packet inside the file and the walk ends at its end
    (or at an end code followed by 0xFF / 0x00 padding only)."""
    n = len(d)
    if n < 16:
        r.err('V01', 'is too small to be a movie (%d bytes)' % n)
        return
    # V05 (check_file_compatible in C++): a picture larger than the disc's 512x480 plays scrambled (Xbox movies are 640x480)
    k = d.find(bytes([0, 0, 1, 0xB3]), 0, 65536)
    if k >= 0 and k + 7 <= n:
        w, h = d[k + 4] << 4 | d[k + 5] >> 4, (d[k + 5] & 15) << 8 | d[k + 6]
        if w > 512 or h > 480:
            r.err('V05', 'is a %dx%d movie; this game\'s movies are 512x480 and a larger one plays scrambled' % (w, h))
    at = packets = 0
    packed = False  # starts with a pack header: the disc's layout, which always ends with the end code
    while at < n:
        if n - at < 4 or d[at:at + 3] != b'\x00\x00\x01' or d[at + 3] < 0xB9:
            if packets == 0:
                r.err('V01', 'does not start with an MPEG packet (not a movie, or damaged)')
            else:
                r.err('V04', 'has no MPEG packet at byte %d (damaged movie)' % at)
            return
        sid = d[at + 3]
        if sid == 0xB9:
            if d[at + 4:].strip(b'\xff\x00'):
                r.err('V02', 'has data after its MPEG end code (damaged movie)')
            return
        if sid == 0xBA:
            if n - at < 14:
                r.err('V02', 'ends inside a pack header (truncated movie)')
                return
            length = 14 + (d[at + 13] & 7) if (d[at + 4] & 0xC0) == 0x40 else 12
        else:
            if n - at < 6:
                r.err('V02', 'ends inside a packet header (truncated movie)')
                return
            length = 6 + (d[at + 4] << 8 | d[at + 5])
        if at + length > n:
            r.err('V02', 'ends inside a packet (%d of %d bytes at byte %d; truncated movie)' % (n - at, length, at))
            return
        if packets == 0:
            packed = sid == 0xBA
        at += length
        packets += 1
    if packed:
        r.err('V02', 'does not end with the MPEG end code (truncated movie)')


TP_SIZES = {'mc_bunner_e.tp': 6144, 'mc_bunner_j.tp': 6144, 'mc_icon01.tp': 2048, 'mc_icon02.tp': 2048, 'mc_icon03.tp': 2048}


def check_tp(d, name, r):
    base = name.rsplit('/', 1)[-1].lower()
    want = TP_SIZES.get(base)
    if want is None:
        return
    if len(d) < want:
        r.err('P01', 'is %d bytes, the memory card %s is %d' % (len(d), 'banner' if 'bunner' in base else 'icon', want))
    elif len(d) != want:
        r.warn('P02', 'is %d bytes, expected %d' % (len(d), want))


def check_bnr(d, name, r):
    # GameCube disc banner: BNR1 = 0x1960 bytes (one language), BNR2 = 0x1FA0 (six); read by the IPL, not by the game
    want = {b'BNR1': 0x1960, b'BNR2': 0x1FA0}.get(d[:4])
    if want is None:
        r.err('N02', 'has no "BNR1"/"BNR2" signature')
    elif len(d) < want:
        r.err('N01', 'is %d bytes, a %s banner is %d' % (len(d), d[:4].decode(), want))
    elif len(d) != want:
        r.warn('N03', 'is %d bytes, a %s banner is %d' % (len(d), d[:4].decode(), want))


def check_srd(d, name, r):
    n = len(d)
    if n < 0x20 or d[:4] != b'SSRD':
        r.err('R01', 'is not a staff-roll table (no "SSRD" signature)')
        return
    ents, tab, strips, soff = le32(d, 0x10), le32(d, 0x14), le32(d, 0x18), le32(d, 0x1C)
    if tab + 12 * ents > n:
        r.err('R02', 'entry table (%d x 12 at %#x) overruns the file' % (ents, tab))
        return
    if soff + 8 * strips > n:
        r.err('R03', 'strip table (%d x 8 at %#x) overruns the file (truncated or damaged)' % (strips, soff))
        return
    if n != soff + 8 * strips + 16:
        r.warn('R04', 'size %d, shipped table ends 16 bytes after the strips (%d)' % (n, soff + 8 * strips + 16))
    for i in range(ents):
        t = le32(d, tab + 12 * i + 8)
        if t + 1 > n:
            r.err('R05', 'entry %d text offset %d is outside the file' % (i, t))
            return


# ------------------------------------------------------------------------------------------------ dispatch
def check_bytes(d, game_path, size=None, tail=None):
    """Check a file's bytes.  game_path is the path relative to the files folder (forward slashes)."""
    r = Result()
    ext = os.path.splitext(game_path)[1].lower()
    fn = {'.dat': check_dat, '.fnt': check_fnt, '.met': check_met, '.gncp': check_gncp, '.mlt': check_mlt,
          '.bin': check_bin, '.scr': check_scr, '.fog': check_fog, '.tp': check_tp, '.bnr': check_bnr,
          '.srd': check_srd}.get(ext)
    if ext in ('.sfd', '.flo'):                  # .flo: the movie files Reloaded ships (same MPEG program stream)
        check_sfd(d, game_path, r, size, tail)
    elif ext == '.inf':
        check_inf(d, game_path, r)
    elif ext == '.conf':
        pass
    elif fn is not None:
        fn(d, game_path, r)
    return r


INF_SECTION = re.compile(r'^\[([A-Za-z0-9_]+)\]\s*$')


def inf_sections(d):
    text = d.decode('latin-1').replace('\r', '')
    return [m.group(1) for m in (INF_SECTION.match(l) for l in text.split('\n')) if m]


def check_inf(d, name, r):
    """nukkoro.inf / nukkoro2.inf: Shift-JIS text read section by section by InfDatabase (0x801E0C68).  Bytes only, so
    the C++ port needs no code page: a NUL, no [section], the last shipped section / key gone."""
    base = name.rsplit('/', 1)[-1].lower()
    if not d:
        r.err('I01', 'is empty')
        return
    if b'\0' in d:
        r.err('I02', 'contains a NUL byte (a Shift-JIS text file)')
        return
    if not d.endswith(b'\n'):
        r.warn('I04', 'does not end with a line break (a real mod ships that way)')
    secs = inf_sections(d)
    if not secs:
        r.err('I05', 'has no [section] headers')
        return
    need = INF_REQUIRED.get(base)
    if need:
        missing = [s for s in need if s not in secs]
        if missing:
            r.warn('I06', 'misses %d section(s) every shipped file has, first %s' % (len(missing), missing[0]))
    if base == 'nukkoro2.inf' and 'EVENT0910' not in secs:
        r.err('I07', 'last shipped section [EVENT0910] is absent (cut off: the later stages lose their tuning)')
    if base == 'nukkoro.inf' and not re.search(rb'(?m)^AREA[ \t]*:', d):
        r.err('I08', 'last shipped key "AREA" is absent (cut off: the global settings fall back to defaults)')


INF_REQUIRED = {}
INF_NUKKORO2_TAIL = []


def load_rules(path):
    """Read test_data/mods/format_rules.json: the required inf sections of the shipped files."""
    with open(path, 'r', encoding='utf-8') as f:
        j = json.load(f)
    INF_REQUIRED.update(j.get('inf_required_sections', {}))
    INF_NUKKORO2_TAIL[:] = j.get('inf_last_sections', {}).get('nukkoro2.inf', [])


# ------------------------------------------------------------------------------------------------ scan / mutate
def walk(root):
    for dp, dn, fns in os.walk(root):
        dn[:] = [x for x in dn if not x.startswith('.')]
        for fn in fns:
            if os.path.splitext(fn)[1].lower() in KINDS:
                p = os.path.join(dp, fn)
                yield p, os.path.relpath(p, root).replace('\\', '/')


def check_path(p, gp):
    size = os.path.getsize(p)
    with open(p, 'rb') as f:
        return check_bytes(f.read(), gp)


def cmd_scan(a):
    if a.rules:
        load_rules(a.rules)
    total = {}
    report = []
    for root in a.root:
        for p, gp in sorted(walk(root)):
            r = check_path(p, gp)
            ext = os.path.splitext(gp)[1].lower()
            t = total.setdefault(ext, [0, 0, 0])
            t[0] += 1
            if r.errors:
                t[1] += 1
            if r.warnings:
                t[2] += 1
            if r.errors or r.warnings:
                report.append({'root': root, 'file': gp, 'errors': r.errors, 'warnings': r.warnings})
    print('%-7s %7s %7s %9s' % ('kind', 'files', 'errors', 'warnings'))
    for ext in sorted(total):
        t = total[ext]
        print('%-7s %7d %7d %9d' % (ext, t[0], t[1], t[2]))
    for e in report:
        for m in e['errors']:
            print('ERROR   %s :: %s :: %s' % (e['root'], e['file'], m))
        if a.warnings:
            for m in e['warnings']:
                print('warning %s :: %s :: %s' % (e['root'], e['file'], m))
    if a.json:
        with open(a.json, 'w', encoding='utf-8') as f:
            json.dump({'totals': total, 'findings': report}, f, indent=1)
    return 1 if any(e['errors'] for e in report) else 0


# kinds whose shipped files have no header the checker could see zeroed
NO_HEADER = {'.tp', '.conf'}
# (kind, mutation) pairs that a structural rule cannot or need not catch, with the reason (docs/mods/map/FORMAT_CHECKS.md):
HARMLESS = {
    ('.fnt', 'trunc1'): 'the last byte is part of the 4*count tail the game never reads',
    ('.met', 'trunc1'): 'the last byte is the empty final line the game never reads',
    ('.met', 'trunch'): 'a cut on a glyph-line boundary is a valid, smaller glyph table (no count to compare)',
    ('.gncp', 'trunc1'): 'a bare project ends in a trailer chunk after its pointer table, never read',
    ('.gncp', 'cutlast'): 'same trailer',
}


def mutations(d, ext, game_path):
    n = len(d)
    out = {'trunc1': d[:-1], 'trunch': d[:n // 2]}
    # "cutlast": remove the last byte that carries information (the shipped .fnt / .met / containers end in bytes the
    # game never reads, so trunc1 is harmless for them and only the information-bearing cut is a real truncation)
    cut = n - 1
    if ext == '.fnt' and n >= 4:
        count = le32(d, 0)
        if count and 4 + 20 * count <= n:
            off = struct.unpack_from('<I', d, 4 + 20 * (count - 1))[0]
            j = off
            while j + 1 < n and (d[j] | d[j + 1]):
                j += 2
            cut = j + 1                         # inside the last string's zero terminator
    elif ext == '.met':
        cut = d.rfind(b'#') + 1 if b'#' in d else n - 1
    elif ext == '.bin' and n >= 0x20 and d[0x17:0x18] == b'B':
        cut = 0x20 + be32(d, 4) + be32(d, 8) - 1 if 0x20 + be32(d, 4) + be32(d, 8) <= n else n - 1
    if 0 < cut < n:
        out['cutlast'] = d[:cut]
    z = bytearray(d)
    hdr = {'.met': 8, '.inf': 0, '.fog': 0x28, '.bin': 0x20, '.dat': 0x30, '.gncp': 0x20, '.mlt': 0x40, '.fnt': 4, '.scr': 8,
           '.bnr': 4, '.srd': 0x20, '.sfd': 4, '.tp': 0x10}.get(ext, 0x20)
    if ext == '.inf':
        k = min(n, 24)
    else:
        k = min(n, hdr)
    z[:k] = bytes(k)
    out['zero'] = bytes(z)
    return out


def cmd_mutate(a):
    if a.rules:
        load_rules(a.rules)
    os.makedirs(a.work, exist_ok=True)
    seen = {}
    rows = []
    per = a.per_kind
    cand = {}
    for root in a.root:
        for p, gp in sorted(walk(root)):
            ext = os.path.splitext(gp)[1].lower()
            if ext not in ('.sfd', '.flo'):       # big files: handled below with the real tail logic
                cand.setdefault(ext, []).append((p, gp))
    picks = []
    for ext, lst in cand.items():
        step = max(1, len(lst) // per)
        picks += lst[::step][:per]                # spread over the sorted list (different banks, stages, languages)
    for p, gp in picks:
        if True:
            ext = os.path.splitext(gp)[1].lower()
            with open(p, 'rb') as f:
                d = f.read()
            if check_bytes(d, gp).errors:
                continue
            seen[ext] = seen.get(ext, 0) + 1
            for tag, md in mutations(d, ext, gp).items():
                r = check_bytes(md, gp)
                caught = 'error' if r.errors else ('warning' if r.warnings else 'MISSED')
                if a.write:
                    op = os.path.join(a.work, tag, *gp.split('/'))
                    os.makedirs(os.path.dirname(op), exist_ok=True)
                    with open(op, 'wb') as f:
                        f.write(md)
                rows.append({'kind': ext, 'file': gp, 'mutation': tag, 'result': caught,
                             'why': (r.errors or r.warnings or [''])[0]})
    # movies: mutated in memory (the walk reads the whole stream)
    for root in a.root:
        for p, gp in sorted(walk(root)):
            if not gp.lower().endswith(('.sfd', '.flo')) or seen.get('.sfd', 0) >= 4:
                continue
            seen['.sfd'] = seen.get('.sfd', 0) + 1
            with open(p, 'rb') as f:
                d = f.read()
            for tag, md in {'trunc1': d[:-1], 'trunch': d[:len(d) // 2], 'zero': bytes(2048) + d[2048:]}.items():
                r = check_bytes(md, gp)
                caught = 'error' if r.errors else ('warning' if r.warnings else 'MISSED')
                rows.append({'kind': os.path.splitext(gp)[1].lower(), 'file': gp, 'mutation': tag, 'result': caught,
                             'why': (r.errors or r.warnings or [''])[0]})
    print('%-7s %-9s %-7s %s' % ('kind', 'mutation', 'result', 'file'))
    missed = 0
    for row in rows:
        print('%-7s %-9s %-7s %s' % (row['kind'], row['mutation'], row['result'], row['file']))
        if row['result'] != 'error':
            missed += 1
    byk = {}
    for row in rows:
        c = byk.setdefault(row['kind'] + ' ' + row['mutation'], {'error': 0, 'warning': 0, 'MISSED': 0})
        c[row['result']] += 1
    for k in sorted(byk):
        c = byk[k]
        print('%-16s error %2d  warning %2d  missed %2d' % (k, c['error'], c['warning'], c['MISSED']))
    if a.json:
        with open(a.json, 'w', encoding='utf-8') as f:
            json.dump(rows, f, indent=1)
    for k, why in sorted(HARMLESS.items()):
        n = sum(1 for r in rows if (r['kind'], r['mutation']) == k and r['result'] == 'MISSED')
        if n:
            print('not detectable by design: %s %s x%d (%s)' % (k[0], k[1], n, why))
    return 1 if any(r['result'] == 'MISSED' and r['kind'] not in NO_HEADER and (r['kind'], r['mutation']) not in HARMLESS
                    for r in rows) else 0


def cmd_rules(a):
    """Derive test_data/mods/format_rules.json from shipped trees: the [section] names every region's inf files share."""
    secs = {}
    for root in a.root:
        for f in ('nukkoro.inf', 'nukkoro2.inf'):
            p = os.path.join(root, f)
            if os.path.isfile(p):
                with open(p, 'rb') as fh:
                    secs.setdefault(f, []).append(inf_sections(fh.read()))
    req = {}
    last = {}
    for f, lists in secs.items():
        common = [x for x in lists[0] if all(x in l for l in lists)]
        req[f] = common
        last[f] = common[-1:]
    out = {'comment': 'generated by tools/mods/check_formats.py rules; sections every shipped nukkoro*.inf of the given trees has',
           'trees': len(a.root), 'inf_required_sections': req, 'inf_last_sections': last,
           'layout_limits': {'entries': MAX_LAYOUT_ENTRIES, 'misc_bytes': MAX_LAYOUT_MISC},
           'tp_sizes': TP_SIZES, 'bnr_sizes': {'BNR1': 0x1960, 'BNR2': 0x1FA0}}
    with open(a.out, 'w', encoding='utf-8', newline=chr(10)) as f:
        json.dump(out, f, indent=1)
        f.write(chr(10))
    print('wrote', a.out, {k: len(v) for k, v in req.items()})
    return 0


def cmd_budget(a):
    """Layout budget: entries and misc bytes the SetManager holds at once (cmn + nrm|hrd + ds1) against its fixed room."""
    rows = []
    groups = [a.root] if a.layered else [[r] for r in a.root]
    for roots in groups:
        root = '+'.join(roots)
        stages = {}
        files = {}
        for rt in roots:                    # later roots replace same-named files of earlier ones (a mod over the base)
            for p, gp in walk(rt):
                files[gp] = p
        for gp, p in sorted(files.items()):
            m = re.search(r'(?:^|/)(stg\d{4})_(cmn|nrm|hrd|ds1)\.dat$', gp)
            if m:
                with open(p, 'rb') as f:
                    nm = layout_numbers(f.read())
                if nm:
                    stages.setdefault(m.group(1), {})[m.group(2)] = nm
        for st, v in sorted(stages.items()):
            for diff in ('nrm', 'hrd'):
                parts = [v.get('cmn', (0, 0)), v.get(diff, (0, 0)), v.get('ds1', (0, 0))]
                rows.append((root, st, diff, sum(x[0] for x in parts), sum(x[1] for x in parts)))
    for root in sorted(set(r[0] for r in rows)):
        mine = [r for r in rows if r[0] == root]
        if mine:
            me = max(mine, key=lambda r: r[3])
            mm = max(mine, key=lambda r: r[4])
            print('%s: %d stage sets; max entries %d (%s %s) of %d; max misc %d (%s %s) of %d' %
                  (root, len(mine), me[3], me[1], me[2], MAX_LAYOUT_ENTRIES, mm[4], mm[1], mm[2], MAX_LAYOUT_MISC))
    over = [r for r in rows if r[3] > MAX_LAYOUT_ENTRIES or r[4] > MAX_LAYOUT_MISC]
    for r in over:
        print('OVER BUDGET', r)
    return 1 if over else 0


def scan_corpus(root, rule_ids=True):
    totals = {}
    errors = []
    wcount = {}
    for p, gp in sorted(walk(root)):
        r = check_path(p, gp)
        ext = os.path.splitext(gp)[1].lower()
        t = totals.setdefault(ext, {'files': 0, 'with_errors': 0, 'with_warnings': 0})
        t['files'] += 1
        if r.errors:
            t['with_errors'] += 1
            errors += [gp + ' :: ' + e for e in r.errors]
        if r.warnings:
            t['with_warnings'] += 1
            for w in r.warnings:
                k = ext + ' ' + w.split(' ', 1)[0]
                wcount[k] = wcount.get(k, 0) + 1
    return {'kinds': totals, 'errors': errors, 'warning_rules': wcount}


def cmd_list(a):
    """One line per file: "<game path><TAB>OK|ERR" (errors only).  The C++ port prints the same, so the two can be diffed."""
    for root in a.root:
        for p, gp in sorted(walk(root)):
            r = check_path(p, gp)
            print('%s\t%s' % (gp, 'ERR' if r.errors else 'OK'))
    return 0


def cmd_report(a):
    """Run the scan over named corpora and the mutation test over the first one; write one small JSON (no game bytes)."""
    if a.rules:
        load_rules(a.rules)
    out = {'corpora': {}}
    names = []
    for spec in a.corpus:
        name, _, root = spec.partition('=')
        names.append((name, root))
        out['corpora'][name] = scan_corpus(root)
        c = out['corpora'][name]
        print('%-12s files %5d  errors %d' % (name, sum(t['files'] for t in c['kinds'].values()), len(c['errors'])))
    with open(a.out, 'w', encoding='utf-8', newline=chr(10)) as f:
        json.dump(out, f, indent=1, sort_keys=True)
        f.write(chr(10))
    return 1 if any(c['errors'] for c in out['corpora'].values()) else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('scan')
    s.add_argument('--root', action='append', required=True)
    s.add_argument('--json')
    s.add_argument('--rules')
    s.add_argument('--warnings', action='store_true')
    m = sub.add_parser('mutate')
    m.add_argument('--root', action='append', required=True)
    m.add_argument('--work', required=True)
    m.add_argument('--json')
    m.add_argument('--rules')
    m.add_argument('--per-kind', type=int, default=6)
    m.add_argument('--write', action='store_true')
    li = sub.add_parser('list')
    li.add_argument('--root', action='append', required=True)
    rp = sub.add_parser('report')
    rp.add_argument('--corpus', action='append', required=True, help='NAME=DIR')
    rp.add_argument('--out', required=True)
    rp.add_argument('--rules')
    ru = sub.add_parser('rules')
    ru.add_argument('--root', action='append', required=True)
    ru.add_argument('--out', required=True)
    bu = sub.add_parser('budget')
    bu.add_argument('--root', action='append', required=True)
    bu.add_argument('--layered', action='store_true', help='roots are layers: later ones replace same-named files')
    f = sub.add_parser('file')
    f.add_argument('path')
    f.add_argument('game_path', nargs='?')
    f.add_argument('--rules')
    a = ap.parse_args()
    if a.cmd == 'scan':
        return cmd_scan(a)
    if a.cmd == 'mutate':
        return cmd_mutate(a)
    if a.cmd == 'rules':
        return cmd_rules(a)
    if a.cmd == 'list':
        return cmd_list(a)
    if a.cmd == 'report':
        return cmd_report(a)
    if a.cmd == 'budget':
        return cmd_budget(a)
    if a.rules:
        load_rules(a.rules)
    r = check_path(a.path, a.game_path or os.path.basename(a.path))
    for e in r.errors:
        print('ERROR', e)
    for w in r.warnings:
        print('warning', w)
    return 1 if r.errors else 0


if __name__ == '__main__':
    sys.exit(main())
