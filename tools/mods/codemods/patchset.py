#!/usr/bin/env python3
"""patchset.py - turn code mods written for another region of the game into one normalized PAL (GUPP8P) patch set.

Inputs (any number, each gives one patch set):
  gecko-ini:<file>   Dolphin game ini ([Gecko], [Gecko_Enabled], [ActionReplay])
  gecko-text:<file>  plain code text ("Code: 044978f0 60000000", one 8+8 hex line per row)
  gct:<file>         binary Gecko code list
  mcm:<file|dir>     Melee Code Manager library .txt files (entries with static overwrites, branch hooks plus cave code)
  dol-diff:<file>    a replacement main.dol, diffed against the source-region base DOL (--src-dol)

Every patch is {source_region, source_address, address (PAL), old_bytes, new_bytes, kind, confidence, provenance}.
kind: static-patch  fixed address, unconditional, applied once: can be written into the DOL
      runtime       conditional, pointer/base relative or aimed at bss/heap: needs a per-frame engine
      code-injection a hook plus relocated cave code (Gecko C2, MCM "-> Branch", caves of a replaced DOL)

Address translation: extract/address-maps/<region>.map (verified function level map) for code and data, a byte
signature search for code the map leaves out, and a byte signature confirmation of the window around every code
address. Injected code is decoded and relocated (b/bl/bc targets, lis/addi|ori|load|store address pairs, r13/r2
small-data offsets) and the result is verified. Nothing is written to the game directory; outputs go to --out-dir.

  python -I tools/mods/codemods/patchset.py --src-dol <usa>/sys/main.dol --pal-dol game/sys/main.dol \\
      --address-map extract/address-maps/GUPE8P.map --pal-functions extract/functions.csv --source-region GUPE8P \\
      --out-dir <dir> --input gecko-text:code.txt --input mcm:MCM/ --mod-dol mod/main.dol --input dol-diff:mod/main.dol
"""
import argparse, bisect, collections, csv, glob, json, os, re, struct, sys

CAVE_LO, CAVE_HI = 0x800034AC, 0x800053E0   # debugger vector stubs: free space used by Reloaded; same bytes in every region
LOWMEM_END = 0x80003100                     # below this the addresses are OS low memory, identical in all regions
RAM_HI = 0x81800000
SECTION_KIND = {0: 'text', 1: 'text', 7: 'rodata', 8: 'rodata', 9: 'rodata', 10: 'rodata', 11: 'rodata',
                12: 'data', 13: 'sdata', 14: 'sdata2'}
MUTABLE = ('data', 'sdata')
HEX8 = re.compile(r'^[0-9A-Fa-f]{8}$')


def h8(v):
    return '0x%08X' % (v & 0xFFFFFFFF)


# ================================================================================================= DOL
class Dol:
    def __init__(self, path=None, data=None):
        if data is None:
            with open(path, 'rb') as f:
                data = f.read()
        self.data = data
        h = struct.unpack('>18I18I18I', data[:0xD8])
        self.secs = [(i, h[i], h[18 + i], h[36 + i]) for i in range(18) if h[36 + i]]    # index, file off, address, size
        self.bss = struct.unpack('>II', data[0xD8:0xE0])
        self.entry = struct.unpack('>I', data[0xE0:0xE4])[0]
        self.r13 = self._find_base(13)
        self.r2 = self._find_base(2)

    def _find_base(self, reg):
        for i, o, a, sz in self.secs[:1]:
            for k in range(0, min(sz, 0x400), 4):
                w = struct.unpack('>I', self.data[o + k:o + k + 4])[0]
                if (w >> 26) == 15 and ((w >> 21) & 31) == reg and ((w >> 16) & 31) == 0:
                    n = struct.unpack('>I', self.data[o + k + 4:o + k + 8])[0]
                    if (n >> 26) == 24 and ((n >> 21) & 31) == reg and ((n >> 16) & 31) == reg:
                        return ((w & 0xFFFF) << 16) | (n & 0xFFFF)
                    if (n >> 26) == 14 and ((n >> 21) & 31) == reg and ((n >> 16) & 31) == reg:
                        return ((w & 0xFFFF) << 16) + sext16(n)
        return None

    def a2o(self, a):
        for i, o, ad, sz in self.secs:
            if ad <= a < ad + sz:
                return o + a - ad
        return None

    def o2a(self, o):
        for i, of, ad, sz in self.secs:
            if of <= o < of + sz:
                return ad + o - of
        return None

    def sec(self, a):
        for i, o, ad, sz in self.secs:
            if ad <= a < ad + sz:
                return i
        return None

    def kind(self, a):
        """text | rodata | data | sdata | sdata2 (file backed) | bss | ram (other 0x80/0x81 memory) | low | None"""
        a &= 0xFFFFFFFF
        s = self.sec(a)
        if s is not None:
            return SECTION_KIND.get(s, 'data')
        if a < 0x80000000 or a >= RAM_HI:
            return None
        if a < LOWMEM_END:
            return 'low'
        if self.bss[0] <= a < self.bss[0] + self.bss[1]:
            return 'bss'
        return 'ram'

    def word(self, a):
        o = self.a2o(a)
        if o is None or self.sec(a + 3) != self.sec(a):
            return None
        return struct.unpack('>I', self.data[o:o + 4])[0]

    def bytes_at(self, a, n):
        o = self.a2o(a)
        if o is None or self.sec(a + n - 1) != self.sec(a):
            return None
        return self.data[o:o + n]

    def mapped(self, a):
        return self.kind(a) not in (None, 'ram')


# ================================================================================================= PPC
def sext16(x):
    x &= 0xFFFF
    return x - 0x10000 if x & 0x8000 else x


VALID_PRIMARY = {3, 4, 7, 8, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 23, 24, 25, 26, 27, 28, 29, 31} | \
    set(range(32, 62)) | {59, 63}
VALID_PRIMARY -= {58}
X_DEST_RA = {28, 60, 444, 412, 124, 316, 476, 284, 24, 536, 792, 824, 954, 922, 26}
X_NO_DEST = {0, 32, 151, 215, 407, 183, 247, 439, 662, 918, 663, 727, 725, 150, 467, 144, 146, 86, 54, 278, 470,
             982, 1014, 598, 4, 759, 695, 983, 631, 695, 566, 998, 438, 727}
X_UPDATE_LOAD = {55, 119, 311, 375}
LOADS = set(range(32, 48)) - {36, 37, 38, 39, 44, 45, 46, 47}      # integer loads incl. update forms (+lmw 46 handled aside)
STORES = {36, 37, 38, 39, 44, 45, 47}
FPMEM = set(range(48, 56))
DFORM_MEM = set(range(32, 56))


def dests(w):
    """GPRs written by the instruction (conservative)."""
    op = w >> 26
    rd, ra = (w >> 21) & 31, (w >> 16) & 31
    if op in (7, 8, 12, 13, 14, 15):
        return {rd}
    if op in (24, 25, 26, 27, 28, 29, 20, 21, 23):
        return {ra}
    if op in (32, 34, 40, 42):
        return {rd}
    if op in (33, 35, 41, 43):
        return {rd, ra}
    if op == 46:
        return set(range(rd, 32))
    if op in (37, 39, 45, 49, 51, 53, 55, 57, 61):
        return {ra}
    if op == 31:
        xo = (w >> 1) & 0x3FF
        if xo in X_DEST_RA:
            return {ra}
        if xo in X_NO_DEST:
            return set()
        if xo in X_UPDATE_LOAD:
            return {rd, ra}
        return {rd}
    return set()


def branch_info(w, at):
    """-> (kind, target, absolute) for b (18) / bc (16), else None. kind 'b'|'bl'|'bc'|'bcl'"""
    op = w >> 26
    if op == 18:
        li = w & 0x03FFFFFC
        if li & 0x02000000:
            li -= 0x04000000
        aa = bool(w & 2)
        return ('bl' if w & 1 else 'b'), ((li if aa else at + li) & 0xFFFFFFFF), aa
    if op == 16:
        bd = w & 0xFFFC
        if bd & 0x8000:
            bd -= 0x10000
        aa = bool(w & 2)
        return ('bcl' if w & 1 else 'bc'), ((bd if aa else at + bd) & 0xFFFFFFFF), aa
    return None


def enc_branch(w, at, target):
    """re-encode b/bc word `w` placed at `at` so that it reaches `target`; None when out of range."""
    op = w >> 26
    aa = bool(w & 2)
    d = target if aa else target - at
    if op == 18:
        if aa:
            if not (0 <= target < 0x02000000 or target >= 0xFE000000):
                return None
        elif not (-0x02000000 <= d < 0x02000000):
            return None
        return (w & 0xFC000003) | (d & 0x03FFFFFC)
    if not aa and not (-0x8000 <= d < 0x8000):
        return None
    return (w & 0xFFFF0003) | (d & 0xFFFC)


def mk_b(at, target, link=False):
    return 0x48000000 | ((target - at) & 0x03FFFFFC) | (1 if link else 0)


def shape(w):
    """Instruction word with the parts that move between region builds masked (branch displacements, D-form immediates)."""
    o = w >> 26
    if o == 18:
        return w & 0xFC000003
    if o == 16:
        return w & 0xFFFF0003
    if o in (14, 15, 24, 25) or 32 <= o <= 55 or o in (56, 57, 60, 61):
        return w & 0xFFFF0000
    return w


class Pair:
    __slots__ = ('lis', 'use', 'signed', 'value', 'reg', 'sda', 'imm_new', 'kind')

    def __init__(self, **k):
        for s in self.__slots__:
            setattr(self, s, k.get(s))


def scan_pairs(words, targets=(), sda=None):
    """Find address constants in a straight-line instruction list.
    -> list of Pair: lis/(addi|ori|load|store) pairs (value = full address, lis = index of lis, use = index of the word
       carrying the low half) and small-data accesses (sda='r13'/'r2', value = full address of the variable)."""
    pairs, regs = [], {}
    for i, w in enumerate(words):
        if i in targets:
            regs = {}
        op = w >> 26
        rd, ra, rs = (w >> 21) & 31, (w >> 16) & 31, (w >> 21) & 31
        imm = w & 0xFFFF
        if op == 15 and ra == 0:
            regs[rd] = ('hi', imm << 16, i)
            continue
        if op == 14 and ra != 0:
            st = regs.get(ra)
            if st and st[0] == 'hi':
                v = (st[1] + sext16(imm)) & 0xFFFFFFFF
                pairs.append(Pair(lis=st[2], use=i, signed=True, value=v, reg=ra, kind='addi'))
                regs[rd] = ('full', v)
                continue
            if sda and ra in sda:
                pairs.append(Pair(lis=None, use=i, signed=True, value=(sda[ra] + sext16(imm)) & 0xFFFFFFFF, reg=ra,
                                  sda=True, kind='addi'))
            regs.pop(rd, None)
            continue
        if op == 24:
            st = regs.get(rs)
            if st and st[0] == 'hi':
                v = st[1] | imm
                pairs.append(Pair(lis=st[2], use=i, signed=False, value=v, reg=rs, kind='ori'))
                regs[ra] = ('full', v)
                continue
            regs.pop(ra, None)
            continue
        if op in DFORM_MEM and ra != 0:
            st = regs.get(ra)
            if st and st[0] == 'hi':
                pairs.append(Pair(lis=st[2], use=i, signed=True, value=(st[1] + sext16(imm)) & 0xFFFFFFFF, reg=ra,
                                  kind='mem'))
            elif sda and ra in sda and ra not in regs:
                pairs.append(Pair(lis=None, use=i, signed=True, value=(sda[ra] + sext16(imm)) & 0xFFFFFFFF, reg=ra,
                                  sda=True, kind='mem'))
        for r in dests(w):
            regs.pop(r, None)
        if op in (18, 16) and (w & 1):
            for r in [0] + list(range(3, 13)):
                regs.pop(r, None)
        if (op == 18 and not (w & 1)) or w in (0x4E800020, 0x4E800420):
            regs = {}
    return pairs


# ================================================================================================= map / translation
class AddressMap:
    """extract/address-maps/<region>.map: `kind canonical_start canonical_end delta` (canonical = PAL, other = region
    address = canonical + delta)."""

    def __init__(self, path):
        self.segs = []
        with open(path, encoding='utf-8') as fh:
            lines_ = fh.readlines()
        for line in lines_:
            t = line.split()
            if t and t[0] in ('code', 'data', 'fixed', 'aligned'):
                s, e, d = int(t[1], 16), int(t[2], 16), int(t[3], 16)
                self.segs.append((s + d, e + d, d, t[0]))
        self.segs.sort()
        self.starts = [s[0] for s in self.segs]

    def to_pal(self, a):
        i = bisect.bisect_right(self.starts, a) - 1
        while i >= 0 and i >= bisect.bisect_right(self.starts, a) - 3:
            s, e, d, k = self.segs[i]
            if s <= a < e:
                return a - d, k
            i -= 1
        return None, None

    def neighbours(self, a):
        i = bisect.bisect_right(self.starts, a)
        lo = self.segs[i - 1] if i > 0 else None
        hi = self.segs[i] if i < len(self.segs) else None
        return lo, hi


class PalFunctions:
    def __init__(self, path):
        rows = []
        with open(path, encoding='utf-8', newline='') as f:
            for r in csv.DictReader(f):
                rows.append((int(r['addr']), int(r['size'])))
        rows.sort()
        self.rows, self.starts = rows, [r[0] for r in rows]

    def find(self, a):
        """Smallest row containing a (rows can nest: the list has sub-entries inside long functions)."""
        i = bisect.bisect_right(self.starts, a) - 1
        best = None
        for j in range(i, max(-1, i - 64), -1):
            r = self.rows[j]
            if r[0] <= a < r[0] + r[1] and (best is None or r[1] < best[1]):
                best = r
        return best

    def is_start(self, a):
        i = bisect.bisect_left(self.starts, a)
        return i < len(self.starts) and self.starts[i] == a


SCRATCH_BASE, SCRATCH_SIZE = 0x817C0000, 0x30000    # PAL guest memory reserved for the mods (above OSArenaHi 0x817A0000)


class ScratchOverflow(SystemExit):
    pass


class Scratch:
    """Relocation of the constants a mod uses as scratch memory (addresses outside the DOL image: heap, stack area, arena)
    into the reserved mod region. Two phases: `collect` (every constant met is recorded, words stay as they are) then `apply`
    (clusters built, every constant translated). A cluster keeps the relative offsets of a run of constants (gap <= `gap`),
    padded by `pad` on both sides and page aligned, so alignment and struct layout survive."""

    def __init__(self, base=SCRATCH_BASE, size=SCRATCH_SIZE, pad=0x1000, gap=0x4000):
        self.base, self.size, self.pad, self.gap = base, size, pad, gap
        self.clusters, self.seen, self.mode = [], set(), 'collect'

    def load(self, doc):
        for c in doc:
            self.clusters.append({'ntsc_lo': int(c['ntsc_range'][0], 16), 'ntsc_hi': int(c['ntsc_range'][1], 16),
                                  'pal_lo': int(c['pal_range'][0], 16), 'pal_hi': int(c['pal_range'][1], 16),
                                  'constants': sorted(int(x, 16) for x in c['constants'])})

    def find(self, v):
        for c in self.clusters:
            if c['ntsc_lo'] <= v < c['ntsc_hi']:
                return c
        return None

    def translate(self, v):
        if self.mode == 'collect':
            self.seen.add(v)
            return v
        c = self.find(v)
        if c is None:
            raise RuntimeError('scratch constant %s was not collected' % h8(v))
        return c['pal_lo'] + v - c['ntsc_lo']

    def reset_collect(self):
        self.mode = 'collect'
        self.seen = set()

    def build(self):
        free = max([c['pal_hi'] for c in self.clusters] + [self.base])
        new = sorted(v for v in self.seen if self.find(v) is None)
        groups = []
        for v in new:
            if groups and v - groups[-1][-1] <= self.gap:
                groups[-1].append(v)
            else:
                groups.append([v])
        for g in groups:
            lo = (g[0] - self.pad) & ~0xFFF
            hi = (g[-1] + 4 + self.pad + 0xFFF) & ~0xFFF
            if free + (hi - lo) > self.base + self.size:
                raise ScratchOverflow('scratch region overflow: %d bytes needed for cluster %s..%s, %d left of %d' % (
                    hi - lo, h8(lo), h8(hi), self.base + self.size - free, self.size))
            self.clusters.append({'ntsc_lo': lo, 'ntsc_hi': hi, 'pal_lo': free, 'pal_hi': free + hi - lo, 'constants': g})
            free += hi - lo
        for c in self.clusters:       # constants of earlier clusters that were seen again
            for v in self.seen:
                if c['ntsc_lo'] <= v < c['ntsc_hi'] and v not in c['constants']:
                    c['constants'].append(v)
            c['constants'].sort()
        self.mode = 'apply'

    def report(self):
        return [{'ntsc_range': [h8(c['ntsc_lo']), h8(c['ntsc_hi'])], 'pal_range': [h8(c['pal_lo']), h8(c['pal_hi'])],
                 'size': c['pal_hi'] - c['pal_lo'], 'constants': [h8(v) for v in c['constants']],
                 'constants_pal': [h8(c['pal_lo'] + v - c['ntsc_lo']) for v in c['constants']]} for c in self.clusters]

    def in_region(self, a):
        return self.base <= a < self.base + self.size


class Xlate:
    """Source-region address -> PAL address with a confidence."""

    def __init__(self, src, pal, amap, pf, identity=False):
        self.src, self.pal, self.amap, self.pf, self.identity = src, pal, amap, pf, identity
        self._masked = None
        self.cache = {}
        self.scratch = None

    def _pal_text_masked(self):
        if self._masked is None:
            parts = []
            for i, o, ad, sz in self.pal.secs:
                if SECTION_KIND.get(i) == 'text':
                    words = struct.unpack('>%dI' % (sz // 4), self.pal.data[o:o + sz])
                    parts.append((ad, b''.join(struct.pack('>I', shape(w)) for w in words)))
            self._masked = parts
        return self._masked

    def _signature(self, a):
        lo_d = hi_d = None
        for s, e, d, k in self.amap.segs:
            if e <= a:
                lo_d = d
            elif s > a and hi_d is None:
                hi_d = d
        ds = [x for x in (lo_d, hi_d) if x is not None] or [0]
        lo, hi = a - max(ds) - 0x400, a - min(ds) + 0x400
        for half in (8, 6, 4, 3, 2):
            ws = []
            for j in range(-half, half + 1):
                w = self.src.word(a + 4 * j)
                if w is None:
                    ws = []
                    break
                ws.append(struct.pack('>I', shape(w)))
            if not ws:
                continue
            pat = b''.join(ws)
            hits = []
            for ad, buf in self._pal_text_masked():
                a0, a1 = max(0, lo - ad), min(len(buf), hi - ad + 4 * (2 * half + 1))
                if a1 <= a0:
                    continue
                i = buf.find(pat, a0, a1)
                while i != -1:
                    if i % 4 == 0:
                        hits.append(ad + i + 4 * half)
                    i = buf.find(pat, i + 1, a1)
            if len(hits) == 1:
                return hits[0], 2 * half + 1
            if len(hits) > 1:
                return None, 0
        return None, 0

    def window(self, a, pa, half=4):
        n = same = 0
        for j in range(-half, half + 1):
            x, y = self.src.word(a + 4 * j), self.pal.word(pa + 4 * j)
            if x is None or y is None:
                continue
            n += 1
            same += shape(x) == shape(y)
        return same, n

    def map_text(self, a):
        pa, kind = self.amap.to_pal(a)
        via = 'address-map'
        sigwin = None
        if pa is None:
            pa, n = self._signature(a)
            via = 'signature'
            sigwin = n
        if pa is None:
            return {'pal': None, 'via': None, 'confidence': 'unresolved', 'note': 'no map segment, no unique signature'}
        same, n = self.window(a, pa)
        ow, pw = self.src.word(a), self.pal.word(pa)
        status = 'exact' if ow == pw else ('shape' if ow is not None and pw is not None and shape(ow) == shape(pw)
                                           else 'differs')
        if n and same >= n - 1 and status != 'differs':
            conf = 'high'
        elif n and same >= n - 3 and status != 'differs':
            conf = 'medium'
        else:
            conf = 'low'
        if via == 'signature' and conf == 'high' and (sigwin or 0) < 5:
            conf = 'medium'
        return {'pal': pa, 'via': via, 'confidence': conf, 'window': '%d/%d' % (same, n), 'word': status}

    def _data_signature(self, a, size):
        lo, hi = self.amap.neighbours(a)
        ds = [x[2] for x in (lo, hi) if x] or [0]
        sec = self.src.sec(a)
        so, sa, sz = None, None, None
        for i, o, ad, n in self.src.secs:
            if i == sec:
                so, sa, sz = o, ad, n
        for half in (32, 16, 8):
            lo_a, hi_a = a - half, a + size + half
            if lo_a < sa or hi_a > sa + sz:
                continue
            pat = self.src.bytes_at(lo_a, hi_a - lo_a)
            if pat is None or len(set(pat)) < 3:
                continue
            hits = []
            w0, w1 = a - max(ds) - 0x400, a - min(ds) + 0x400
            for i, o, ad, n in self.pal.secs:
                if SECTION_KIND.get(i) == 'text':
                    continue
                x0, x1 = max(0, w0 - ad - half), min(n, w1 - ad + size + half)
                if x1 <= x0:
                    continue
                blob = self.pal.data
                k = blob.find(pat, o + x0, o + x1)
                while k != -1:
                    hits.append(ad + (k - o) + half)
                    k = blob.find(pat, k + 1, o + x1)
            if len(hits) == 1:
                return hits[0], 2 * half + size
            if len(hits) > 1:
                return None, 0
        return None, 0

    def map_data(self, a, size=4):
        pa, kind = self.amap.to_pal(a)
        via = 'address-map'
        if pa is None:
            lo, hi = self.amap.neighbours(a)
            if lo and hi and lo[2] == hi[2]:
                pa, via = a - lo[2], 'interpolated'
            else:
                pa, n = self._data_signature(a, size)
                if pa is None:
                    return {'pal': None, 'via': None, 'confidence': 'unresolved', 'note': 'data address outside the map, no unique byte signature'}
                via = 'data-signature'
        res = {'pal': pa, 'via': via}
        sb, pb = self.src.bytes_at(a, size), self.pal.bytes_at(pa, size)
        if sb is not None and pb is not None:
            res['value'] = 'equal' if sb == pb else 'differs'
            res['confidence'] = ('high' if sb == pb else 'medium') if via == 'address-map' else 'medium'
            if via == 'data-signature':
                res['value'] = 'signature (the window equals)'
        else:
            res['value'] = 'bss/unreadable'
            res['confidence'] = 'high' if via == 'address-map' else 'medium'
        if self.pal.kind(pa) is None:
            res['confidence'], res['note'] = 'low', 'result is not in a PAL section'
        return res

    def map(self, a, size=4):
        key = (a, size)
        if key not in self.cache:
            self.cache[key] = self._map(a, size)
        return self.cache[key]

    def _map(self, a, size):
        if self.identity:
            return {'pal': a, 'via': 'identity', 'confidence': 'high'}
        k = self.src.kind(a)
        if k == 'low':
            return {'pal': a, 'via': 'low-memory', 'confidence': 'high'}
        if k == 'text':
            return self.map_text(a)
        if k in ('rodata', 'data', 'sdata', 'sdata2', 'bss'):
            return self.map_data(a, size)
        if k == 'ram' and self.scratch is not None:
            return {'pal': self.scratch.translate(a), 'via': 'scratch', 'confidence': 'medium',
                    'note': 'outside the DOL image: relocated into the mod scratch region'}
        return {'pal': None, 'via': None, 'confidence': 'unresolved', 'note': 'not an address of the DOL (heap/pointer)'}


CONF_ORDER = {'high': 3, 'medium': 2, 'low': 1, 'unresolved': 0}


def conf_min(*c):
    return min(c, key=lambda x: CONF_ORDER[x])


# ================================================================================================= relocation
class Reloc:
    def __init__(self, words, notes, unresolved, stats, conf):
        self.words, self.notes, self.unresolved, self.stats, self.conf = words, notes, unresolved, stats, conf


def relocate_block(words, n_base, p_base, xl, ctx, internal_ok=True):
    """Relocate instruction words that sat at n_base (source region; None when unknown) to p_base (PAL).
    Returns Reloc. Unresolvable items keep the source word and are listed in `unresolved`."""
    out = list(words)
    n = len(words)
    notes, unresolved, stats = [], [], collections.Counter()
    conf = 'high'

    def bad(i, why):
        nonlocal conf
        unresolved.append({'index': i, 'word': '%08X' % words[i], 'why': why})
        conf = 'unresolved'

    def lower(c):
        nonlocal conf
        conf = conf_min(conf, c)

    # internal branch targets
    targets = set()
    for i, w in enumerate(words):
        op = w >> 26
        if op not in (16, 18) or (w & 2):
            continue
        if op == 18:
            d = w & 0x03FFFFFC
            d = d - 0x04000000 if d & 0x02000000 else d
        else:
            d = w & 0xFFFC
            d = d - 0x10000 if d & 0x8000 else d
        j = i + d // 4
        if 0 <= j <= n:
            targets.add(j)
    for i, w in enumerate(words):
        op = w >> 26
        if op not in VALID_PRIMARY:
            bad(i, 'word is not a valid Gekko instruction' + (' (zero padding)' if w == 0 else ''))
    # 1. branches
    for i, w in enumerate(words):
        op = w >> 26
        if op not in (16, 18):
            continue
        a_pal = p_base + 4 * i
        if w & 2:                                                   # absolute branch
            bi = branch_info(w, 0)
            r = xl.map(bi[1])
            if r['pal'] is None:
                bad(i, 'absolute branch target %s unresolved' % h8(bi[1]))
                continue
            nw = enc_branch(w, a_pal, r['pal'])
            if nw is None:
                bad(i, 'absolute target out of range')
            else:
                out[i] = nw
                stats['branch-abs'] += 1
                lower(r['confidence'])
            continue
        if op == 18:
            d = w & 0x03FFFFFC
            d = d - 0x04000000 if d & 0x02000000 else d
        else:
            d = w & 0xFFFC
            d = d - 0x10000 if d & 0x8000 else d
        j = i + d // 4
        if 0 <= j <= n and internal_ok:
            stats['branch-internal'] += 1
            continue
        if n_base is None:
            bad(i, 'relative branch leaves the block but the source placement is unknown (displacement %+d)' % d)
            continue
        t = n_base + 4 * i + d
        r = xl.map(t)
        if r['pal'] is None:
            bad(i, 'branch target %s has no PAL address' % h8(t))
            continue
        nw = enc_branch(w, a_pal, r['pal'])
        if nw is None:
            bad(i, 'branch to %s out of range' % h8(r['pal']))
            continue
        out[i] = nw
        stats['branch-external'] += 1
        lower(r['confidence'])
    # 2. address pairs and small data
    sda = {}
    if ctx.src.r13 is not None:
        sda[13] = ctx.src.r13
    if ctx.src.r2 is not None:
        sda[2] = ctx.src.r2
    pal_sda = {13: ctx.pal.r13, 2: ctx.pal.r2}
    pairs = scan_pairs(words, targets, sda)
    groups = collections.defaultdict(list)
    for pr in pairs:
        v = pr.value
        k = ctx.src.kind(v)
        if pr.sda:
            if k == 'ram':
                bad(pr.use, 'small-data access to %s (r%d%+d) lies beyond the DOL image (the arena after the bss): it cannot be moved '
                    'into the scratch region (out of the 16-bit offset range) and the same offset would hit the game heap in PAL' % (
                        h8(v), pr.reg, sext16(words[pr.use] & 0xFFFF)))
                continue
            if k in (None, 'low'):
                continue
            r = xl.map(v)
            if r['pal'] is None:
                bad(pr.use, 'small-data variable %s has no PAL address' % h8(v))
                continue
            nd = r['pal'] - pal_sda[pr.reg]
            if not -0x8000 <= nd < 0x8000:
                bad(pr.use, 'small-data offset out of range')
                continue
            out[pr.use] = (out[pr.use] & 0xFFFF0000) | (nd & 0xFFFF)
            stats['sda'] += 1
            lower(r['confidence'])
            continue
        if k in (None, 'low'):
            continue                                                   # constants and OS low memory
        if k == 'ram':
            if xl.scratch is None:
                notes.append('word %d: constant %s is outside the DOL image (heap/scratch), left unchanged' % (pr.use, h8(v)))
                stats['ram-constant'] += 1
                lower('medium')
                continue
            stats['scratch'] += 1
        groups[pr.lis].append(pr)
    for lis, prs in groups.items():
        his, los, maps = set(), [], []
        for pr in prs:
            r = xl.map(pr.value, 4 if pr.kind != 'mem' else 4)
            if r['pal'] is None:
                bad(pr.use, 'address %s has no PAL address' % h8(pr.value))
                his = None
                break
            maps.append(r)
            p = r['pal']
            his.add(((p + 0x8000) >> 16) if pr.signed else (p >> 16))
        if his is None:
            continue
        if len(his) != 1:
            bad(lis, 'one lis feeds addresses whose PAL high halves differ (%s)' % sorted('%04X' % x for x in his))
            continue
        hi = next(iter(his))
        out[lis] = (out[lis] & 0xFFFF0000) | hi
        for pr, r in zip(prs, maps):
            p = r['pal']
            lo = (p - (hi << 16)) & 0xFFFF if pr.signed else p & 0xFFFF
            out[pr.use] = (out[pr.use] & 0xFFFF0000) | lo
            stats['pair'] += 1
            lower(r['confidence'])
    return Reloc(out, notes, unresolved, stats, conf)


def verify_block(words, p_base, ctx, xl, expect=None):
    """Checks on relocated PAL code. -> dict of counts and failure strings."""
    res = collections.Counter()
    fails = []
    n = len(words)
    for i, w in enumerate(words):
        op = w >> 26
        if op not in VALID_PRIMARY:
            fails.append('word %d %08X does not decode' % (i, w))
            continue
        res['decoded'] += 1
        bi = branch_info(w, p_base + 4 * i)
        if bi:
            t = bi[1]
            if p_base <= t <= p_base + 4 * n:
                res['branch-in-cave'] += 1
            elif ctx.pal.kind(t) == 'text':
                f = ctx.pf.find(t)
                if f is None:
                    fails.append('word %d: branch to %s is not in a PAL function' % (i, h8(t)))
                elif t == f[0]:
                    res['branch-fn-start'] += 1
                else:
                    if bi[0] in ('bl', 'bcl'):
                        fails.append('word %d: call to %s is inside a function (+0x%X)' % (i, h8(t), t - f[0]))
                    else:
                        res['branch-mid-function'] += 1
            else:
                fails.append('word %d: branch target %s is not PAL text' % (i, h8(t)))
    sda = {}
    if ctx.pal.r13 is not None:
        sda[13] = ctx.pal.r13
    if ctx.pal.r2 is not None:
        sda[2] = ctx.pal.r2
    targets = set()
    for i, w in enumerate(words):
        bi = branch_info(w, 4 * i)
        if bi and not bi[2] and 0 <= (bi[1] // 4) <= n:
            targets.add(bi[1] // 4)
    for pr in scan_pairs(words, targets, sda):
        v = pr.value
        k = ctx.pal.kind(v)
        if k in (None, 'low') and not pr.sda:
            res['constant'] += 1
        elif k is None:
            fails.append('word %d: small-data access to %s lands outside the PAL sections' % (pr.use, h8(v)))
        elif k == 'ram':
            if ctx.xl.scratch is not None:
                if ctx.xl.scratch.in_region(v):
                    res['scratch-constant'] += 1
                else:
                    fails.append('word %d: constant %s is outside the DOL image and outside the scratch region' % (pr.use, h8(v)))
            else:
                res['ram-constant'] += 1
        else:
            res['address-in-section'] += 1
            if k == 'text' and pr.kind == 'ori' and not ctx.pf.is_start(v) and ctx.pf.find(v) is not None:
                res['pointer-into-function'] += 1
    return {'counts': dict(res), 'failures': fails}


# ================================================================================================= Gecko
class GeckoCode:
    def __init__(self, name, authors='', lines=None, section='', enabled=None, notes=None):
        self.name, self.authors, self.section, self.enabled = name, authors, section, enabled
        self.lines, self.notes, self.template = lines or [], notes or [], False


def parse_gecko_ini(path):
    codes, cur, sect = [], None, ''
    mode, enabled = None, set()
    with open(path, encoding='utf-8', errors='replace') as fh:
        raw = fh.read().splitlines()
    for line in raw:
        m = re.match(r'^\[(.*)\]\s*$', line)
        if m:
            mode = m.group(1)
            cur = None
            continue
        if mode in ('Gecko_Enabled', 'ActionReplay_Enabled'):
            if line.startswith('$'):
                enabled.add(line[1:].strip())
            continue
        if mode not in ('Gecko', 'ActionReplay'):
            continue
        if line.startswith('$==') or line.startswith('$ ==='):
            sect = line.strip('$= ').strip()
            continue
        if line.startswith('$'):
            m = re.match(r'\$(.*?)(?:\s*\[(.*)\])?\s*$', line)
            cur = GeckoCode(m.group(1).strip(), m.group(2) or '', section=sect)
            cur.kindname = 'AR' if mode == 'ActionReplay' else 'Gecko'
            codes.append(cur)
        elif cur is not None and line.startswith('*'):
            cur.notes.append(line[1:].strip())
        elif cur is not None:
            m = re.fullmatch(r'\s*([0-9A-Fa-fXx]{8})\s+([0-9A-Fa-fXx]{8})\s*', line)
            if m:
                if re.search('[Xx]', line):
                    cur.template = True
                a1, b1 = (re.sub('[Xx]', '0', g) for g in m.groups())
                cur.lines.append((int(a1, 16), int(b1, 16)))
    for c in codes:
        c.enabled = (c.name in enabled) if enabled else None
    return codes


def parse_gecko_text(path):
    """Free text: every `XXXXXXXX XXXXXXXX` line is a code line; `Name:`/`Code:` prefixes are tolerated. Consecutive
    runs of code lines form one code, named by the nearest preceding `Name:` line (or the file name)."""
    codes, cur, name = [], None, os.path.basename(path)
    with open(path, encoding='utf-8', errors='replace') as fh:
        lines_ = fh.read().split(chr(10))
    for line in lines_:
        s = line.strip()
        mn = re.match(r'^(?:Name|Title)\s*:\s*(.+)$', s, re.I)
        if mn:
            name = mn.group(1).strip()
            cur = None
            continue
        s2 = re.sub(r'^(?:Code\s*:\s*)', '', s, flags=re.I)
        m = re.fullmatch(r'(?:0x)?([0-9A-Fa-f]{8})\s+(?:0x)?([0-9A-Fa-f]{8})', s2)
        if m:
            if cur is None:
                cur = GeckoCode(name)
                codes.append(cur)
            cur.lines.append((int(m.group(1), 16), int(m.group(2), 16)))
        elif s:
            cur = None
    return codes


def parse_gct(path):
    with open(path, 'rb') as fh:
        d = fh.read()
    off = 8 if d[:8] == bytes.fromhex('00D0C0DE00D0C0DE') else 0
    lines = []
    while off + 8 <= len(d):
        a, b = struct.unpack('>II', d[off:off + 8])
        off += 8
        if a == 0xF0000000:
            break
        lines.append((a, b))
    return [GeckoCode(os.path.basename(path), lines=lines)]


COND_OPS = {0x20: (32, 'eq'), 0x22: (32, 'ne'), 0x24: (32, 'gt'), 0x26: (32, 'lt'),
            0x28: (16, 'eq'), 0x2A: (16, 'ne'), 0x2C: (16, 'gt'), 0x2E: (16, 'lt')}


def gecko_ops(code):
    """Decode a code into ops. Unconditional fixed-address RAM writes (00/02/04/06/08), conditionals 20-2E (+30-3E
    base/pointer forms) with E0/E2, C0/C2/C6, and everything that touches the base address, pointer, repeat or Gecko
    registers (40-9F, A0-BF, D0-FF) which makes later writes depend on state the tool does not model."""
    ops, stack, dirty, i, ls = [], [], None, 0, code.lines
    while i < len(ls):
        a, b = ls[i]
        i += 1
        t = a >> 24
        op = t & 0xFE
        addr = 0x80000000 | (a & 0x01FFFFFF)
        cond = list(stack)
        base = {'line': i, 'cond': cond, 'depends': dirty}
        ind = bool(op < 0x40 and op & 0x10)
        bop = op & ~0x10 if op < 0x40 else op
        if bop in (0x00, 0x02, 0x04):
            size = {0x00: 1, 0x02: 2, 0x04: 4}[bop]
            if size == 1:
                cnt, val = (b >> 8) + 1, b & 0xFF
            elif size == 2:
                cnt, val = (b >> 16) + 1, b & 0xFFFF
            else:
                cnt, val = 1, b
            data = val.to_bytes(size, 'big') * cnt
            ops.append(dict(base, op='write', addr=addr, size=size, count=cnt, data=data, indirect=ind))
        elif bop == 0x06:
            nl = (b + 7) // 8
            data = b''.join(struct.pack('>II', *ls[i + k]) for k in range(nl) if i + k < len(ls))[:b]
            i += nl
            ops.append(dict(base, op='write', addr=addr, size=1, count=b, data=data, indirect=ind, string=True))
        elif bop == 0x08:
            if i < len(ls):
                a2, b2 = ls[i]
                i += 1
                typ, cnt, inc, vinc = a2 >> 28, ((a2 >> 16) & 0xFFF) + 1, a2 & 0xFFFF, b2
                size = {0: 1, 1: 2, 2: 4}.get(typ, 4)
                parts, val = [], b
                for k in range(cnt):
                    parts.append((addr + k * inc, (val & ((1 << (8 * size)) - 1)).to_bytes(size, 'big')))
                    val += vinc
                ops.append(dict(base, op='serial', addr=addr, size=size, parts=parts, indirect=ind))
        elif bop in COND_OPS:
            w, cmpn = COND_OPS[bop]
            if w == 32:
                mask, val = 0, b
            else:
                mask, val = (b >> 16) & 0xFFFF, b & 0xFFFF
            d = dict(width=w, cmp=cmpn, addr=addr, mask=mask, value=val, indirect=ind)
            stack.append(d)
            ops.append(dict(base, op='if', cond_def=d))
        elif t in (0xE0, 0xE1, 0xE2, 0xE3):
            n = max(1, a & 0xFF)
            for _ in range(min(n, len(stack))):
                stack.pop()
            if (t & 0xFE) == 0xE2:
                stack.append({'else': True})
            ops.append(dict(base, op='endif', n=n))
        elif op in (0xC0, 0xC2, 0xC4, 0xC6) or op in (0xC1,):
            if op in (0xC0, 0xC2):
                body = []
                for k in range(b):
                    if i + k < len(ls):
                        body += list(ls[i + k])
                i += b
                ops.append(dict(base, op='insert-asm' if op == 0xC2 else 'exec-asm', addr=addr, words=body))
            elif op == 0xC6:
                ops.append(dict(base, op='branch', addr=addr, target=b))
            else:
                body = []
                for k in range(b):
                    if i + k < len(ls):
                        body += list(ls[i + k])
                i += b
                ops.append(dict(base, op='unsupported', code_type='%02X' % t, words=body))
        else:
            dirty = dirty or 'code type %02X (base/pointer/repeat/register state)' % t
            ops.append(dict(base, op='state', code_type='%02X' % t, depends=dirty))
    return ops


def cond_text(c):
    if c.get('else'):
        return 'else'
    return 'if [%s]%s %s %s' % (h8(c['addr']), ('&~%04X' % c['mask']) if c['width'] == 16 and c['mask'] else '', c['cmp'],
                                '%0*X' % (c['width'] // 4, c['value']))


# ================================================================================================= MCM
def parse_mcm_files(path):
    files = sorted(glob.glob(os.path.join(path, '*.txt'))) if os.path.isdir(path) else [path]
    entries = []
    hdr = re.compile(r'^(?:NTSC 1\.06\s*|-+\s*)?-+\s+(0x[0-9A-Fa-f]+)\s+-+\s*(.*)$', re.M)
    for f in files:
        with open(f, encoding='utf-8', errors='replace') as fh:
            text = fh.read().replace('\r', '')
        for block in re.split(r'\n\s*-==-\s*\n', text):
            m = re.search(r'^Revision -+ DOL Offset.*$', block, re.M)
            if not m:
                continue
            head = [l.strip() for l in block[:m.start()].strip('\n').split('\n') if l.strip()]
            body = block[m.end():]
            hs = list(hdr.finditer(body))
            sites = []
            for k, hm in enumerate(hs):
                end = hs[k + 1].start() if k + 1 < len(hs) else len(body)
                rest = hm.group(2).strip()
                sub = body[hm.end():end]
                site = {'offset': int(hm.group(1), 16), 'offset_text': hm.group(1), 'rest': rest}
                words_lines = [re.findall(r'[0-9A-Fa-f]{8}', ln) for ln in sub.split('\n')
                               if re.fullmatch(r'\s*(?:[0-9A-Fa-f]{8}\s*)+', ln)]
                rm = re.match(r'([0-9A-Fa-f]{8})\s*->\s*(\S+)\s*$', rest)
                if rm and rm.group(2).lower() == 'branch':
                    site['form'] = 'branch'
                    site['old'] = [int(rm.group(1), 16)]
                    site['cave'] = [int(x, 16) for ln in words_lines for x in ln]
                elif rm:
                    site['form'] = 'word'
                    site['old'] = [int(rm.group(1), 16)]
                    site['new'] = [int(rm.group(2), 16)]
                else:
                    parts = re.split(r'^\s*->\s*$', sub, flags=re.M)
                    if len(parts) == 2:
                        site['form'] = 'multi'
                        site['old'] = [int(x, 16) for x in re.findall(r'(?m)^\s*((?:[0-9A-Fa-f]{8}\s*)+)$', parts[0])
                                       for x in x.split()]
                        site['new'] = [int(x, 16) for x in re.findall(r'(?m)^\s*((?:[0-9A-Fa-f]{8}\s*)+)$', parts[1])
                                       for x in x.split()]
                    else:
                        site['form'] = 'unknown'
                sites.append(site)
            entries.append({'title': head[0] if head else '?', 'authors': next((l for l in head if l.startswith('[')), ''),
                            'file': os.path.basename(f), 'sites': sites})
    return entries


# ================================================================================================= engine
class Engine:
    def __init__(self, a):
        self.src = Dol(a.src_dol)
        self.pal = Dol(a.pal_dol)
        self.region = a.source_region
        self.identity = a.source_region == 'GUPP8P'
        amap_path = a.address_map
        self.amap = AddressMap(amap_path) if amap_path and not self.identity else AddressMap(os.devnull)
        self.pf = PalFunctions(a.pal_functions)
        self.xl = Xlate(self.src, self.pal, self.amap, self.pf, self.identity)
        self.mod_dol = Dol(a.mod_dol) if a.mod_dol else None
        self.cave_lo, self.cave_hi = CAVE_LO, CAVE_HI
        self.alloc_next = CAVE_LO
        self.scratch = Scratch()
        self.xl.scratch = self.scratch
        sm = getattr(a, 'scratch_map', None)
        self.scratch_map_path = sm
        if sm and os.path.exists(sm):
            with open(sm, encoding='utf-8') as fh:
                self.scratch.load(json.load(fh)['scratch'])

    def reset_alloc(self):
        self.alloc_next = self.cave_lo

    def alloc(self, nbytes):
        a = self.alloc_next
        if a + nbytes > self.cave_hi:
            return None
        self.alloc_next = (a + nbytes + 3) & ~3
        return a

    def cave_region_report(self):
        """Is the cave region the same in the PAL DOL, and does anything reference it?"""
        ident = None
        if not self.identity:
            x, y = self.src.bytes_at(CAVE_LO, CAVE_HI - CAVE_LO), self.pal.bytes_at(CAVE_LO, CAVE_HI - CAVE_LO)
            ident = (x == y) if x is not None and y is not None else None
        refs = collections.Counter()
        for i, o, ad, sz in self.pal.secs:
            if SECTION_KIND.get(i) == 'text':
                for k in range(0, sz, 4):
                    w = struct.unpack('>I', self.pal.data[o + k:o + k + 4])[0]
                    bi = branch_info(w, ad + k)
                    if bi and CAVE_LO <= bi[1] < CAVE_HI and not (CAVE_LO <= ad + k < CAVE_HI):
                        refs['branch-from-outside'] += 1
            else:
                for k in range(0, sz - 3, 4):
                    w = struct.unpack('>I', self.pal.data[o + k:o + k + 4])[0]
                    if CAVE_LO <= w < CAVE_HI:
                        refs['pointer-word'] += 1
        return {'identical_to_source': ident, 'references': dict(refs)}


def kind_of_pal(eng, pa):
    return eng.pal.kind(pa)


def classify_write(eng, pa, cond, indirect, depends):
    """-> (kind, notes)"""
    notes = []
    k = eng.pal.kind(pa)
    if indirect:
        return 'runtime', ['address is relative to the Gecko base address/pointer register'], k
    if depends:
        return 'runtime', ['depends on Gecko state: %s' % depends], k
    if cond:
        return 'runtime', ['conditional'], k
    if k in ('text', 'rodata', 'sdata2'):
        return 'static-patch', notes, k
    if k in MUTABLE:
        notes.append('initialized mutable data: the DOL image holds only the initial value; the game may rewrite it '
                     '(apply from the engine instead when the value must persist)')
        return 'static-patch', notes, k
    return 'runtime', ['target is %s (not stored in the DOL)' % (k or 'unmapped')], k


class PatchSet:
    def __init__(self, name, eng, kindname):
        self.name, self.eng, self.kindname = name, eng, kindname
        self.patches, self.caves, self.unresolved, self.extra = [], [], [], {}
        self.n = 0
        self.feat_meta = collections.OrderedDict()      # feature id -> {id, name, ...}

    def set_feature(self, fid, name, **meta):
        self.feat_meta[fid] = dict(meta, id=fid, name=name)
        return {'feature': name, 'feature_id': fid}

    def add(self, **p):
        self.n += 1
        prov = p.get('provenance')
        if prov is not None and 'feature_id' not in prov:
            name = prov.get('code') or prov.get('entry') or prov.get('input')
            fid = next((k for k, m in self.feat_meta.items() if m['name'] == name), None)
            if fid is None:
                fid = 'f%03d' % (len(self.feat_meta) + 1)
                self.set_feature(fid, name)
            prov['feature'], prov['feature_id'] = name, fid
        p['id'] = '%s.%04d' % (self.name, self.n)
        p.setdefault('source_region', self.eng.region)
        p.setdefault('notes', [])
        self.patches.append(p)
        if p.get('confidence') == 'unresolved':
            self.unresolved.append({'id': p['id'], 'source_address': p.get('source_address'),
                                    'why': '; '.join(p['notes'])})
        return p


def build_features(ps):
    """One record per feature: id, name, patch ids, kinds, confidence = the lowest of its patches."""
    out = []
    for fid, meta in ps.feat_meta.items():
        pp = [p for p in ps.patches if p['provenance'].get('feature_id') == fid]
        rec = dict(meta)
        rec['patches'] = [p['id'] for p in pp]
        rec['kinds'] = dict(collections.Counter(p['kind'] for p in pp))
        if pp:
            rec['confidence'] = conf_min(*[p['confidence'] for p in pp])
        else:
            rec['confidence'] = 'unresolved' if meta.get('status') in ('partial', 'version-mismatch') else None
        if any(p.get('needs_user_value') for p in pp):
            rec['needs_user_value'] = True
        out.append(rec)
    return out


def pal_bytes_hex(eng, pa, n):
    b = eng.pal.bytes_at(pa, n) if pa is not None else None
    return b.hex() if b is not None else None


def src_bytes_hex(eng, a, n):
    b = eng.src.bytes_at(a, n)
    return b.hex() if b is not None else None


def text_write(eng, ps, a, data, prov, kind_override=None, cond=None, conf_ceiling=None, code=None, extra=None):
    """Translate a write of `data` at source address a into PAL; words are relocated with the context window."""
    k = eng.src.kind(a)
    notes = []
    r = eng.xl.map(a, min(len(data), 4))
    base = dict(source_address=h8(a), new_bytes_source=data.hex(), provenance=prov, section=k)
    if extra:
        base.update(extra)
    if r['pal'] is None:
        return ps.add(address=None, old_bytes=None, source_old_bytes=src_bytes_hex(eng, a, len(data)),
                      new_bytes=None, kind=kind_override or 'static-patch', confidence='unresolved',
                      notes=[r.get('note', 'no translation')], **base)
    pa = r['pal']
    conf = r['confidence']
    nb = bytearray(data)
    if k == 'text' and len(data) % 4 == 0 and a % 4 == 0:
        words = list(struct.unpack('>%dI' % (len(data) // 4), data))
        rl = relocate_window(eng, a, pa, words)
        nb = bytearray(struct.pack('>%dI' % len(rl.words), *rl.words))
        notes += rl.notes + ['unresolved: %s' % u['why'] for u in rl.unresolved]
        conf = conf_min(conf, rl.conf)
    elif k in ('data', 'rodata', 'sdata', 'sdata2', 'bss', 'ram') and len(data) == 4 and a % 4 == 0:
        v = struct.unpack('>I', data)[0]
        if eng.src.kind(v) in ('text', 'data', 'rodata', 'sdata', 'sdata2', 'bss') and v >= 0x80003100:
            rv = eng.xl.map(v)
            if rv['pal'] is not None:
                nb = bytearray(struct.pack('>I', rv['pal']))
                notes.append('value %s looks like an address and was translated to %s' % (h8(v), h8(rv['pal'])))
                conf = conf_min(conf, 'medium', rv['confidence'])
    if r.get('via') == 'scratch':
        notes.append('address %s is outside the DOL image: relocated into the mod scratch region' % h8(a))
    if r.get('word') == 'differs' and k == 'text':
        notes.append('instruction word around the target differs between regions (window %s)' % r.get('window'))
    kind, kn, pk = classify_write(eng, pa, cond, False, None)
    if kind_override:
        kind = kind_override
    if cond:
        kn = ['conditional']
    return ps.add(address=h8(pa), old_bytes=pal_bytes_hex(eng, pa, len(nb)), source_old_bytes=src_bytes_hex(eng, a, len(nb)),
                  new_bytes=bytes(nb).hex(), kind=kind, confidence=conf, notes=notes + kn, pal_section=pk,
                  map_via=r['via'], window=r.get('window'), **base)


def relocate_window(eng, a, pa, words):
    """Relocate words written over text at source address a (PAL pa) using the surrounding source/PAL code as context for
    lis/low-half pairs. Only the written words are changed."""
    n = len(words)
    nlead, ntail = 12, 4
    sw, pw = [], []
    for j in range(-nlead, n + ntail):
        x, y = eng.src.word(a + 4 * j), eng.pal.word(pa + 4 * j)
        sw.append(x if x is not None else 0)
        pw.append(y if y is not None else 0)
    for j in range(n):
        sw[nlead + j] = words[j]
    notes, unresolved, conf = [], [], 'high'
    out = list(words)
    sda = {13: eng.src.r13, 2: eng.src.r2}
    sda = {k: v for k, v in sda.items() if v is not None}
    pal_sda = {13: eng.pal.r13, 2: eng.pal.r2}
    for j in range(n):
        w = words[j]
        at_n, at_p = a + 4 * j, pa + 4 * j
        bi = branch_info(w, at_n)
        if bi:
            if bi[2]:
                t = bi[1]
            else:
                t = bi[1]
            r = eng.xl.map(t)
            if r['pal'] is None:
                unresolved.append({'why': 'branch target %s has no PAL address' % h8(t)})
                conf = 'unresolved'
                continue
            nw = enc_branch(w, at_p, r['pal'])
            if nw is None:
                unresolved.append({'why': 'branch target out of range'})
                conf = 'unresolved'
                continue
            out[j] = nw
            conf = conf_min(conf, r['confidence'])
            if r['pal'] != t:
                notes.append('branch at %s retargeted %s -> %s' % (h8(at_p), h8(t), h8(r['pal'])))
    # pairs on the window: only the ones touching the written words
    prs = scan_pairs(sw, set(), sda)
    for pr in prs:
        if not (nlead <= pr.use < nlead + n or (pr.lis is not None and nlead <= pr.lis < nlead + n)):
            continue
        k = eng.src.kind(pr.value)
        if pr.sda:
            if k == 'ram':
                unresolved.append({'why': 'small-data access to %s lies beyond the DOL image and cannot be moved into the scratch region' % h8(pr.value)})
                conf = 'unresolved'
                continue
            if k in (None, 'low'):
                continue
            r = eng.xl.map(pr.value)
            if r['pal'] is None:
                unresolved.append({'why': 'small-data variable %s has no PAL address' % h8(pr.value)})
                conf = 'unresolved'
                continue
            nd = r['pal'] - pal_sda[pr.reg]
            j = pr.use - nlead
            out[j] = (out[j] & 0xFFFF0000) | (nd & 0xFFFF)
            conf = conf_min(conf, r['confidence'])
            continue
        if k in (None, 'low') or (k == 'ram' and eng.xl.scratch is None):
            continue
        r = eng.xl.map(pr.value)
        if r['pal'] is None:
            unresolved.append({'why': 'address %s has no PAL address' % h8(pr.value)})
            conf = 'unresolved'
            continue
        p = r['pal']
        lw_p, uw_p = pw[pr.lis], pw[pr.use]
        use_in = nlead <= pr.use < nlead + n
        lis_in = nlead <= pr.lis < nlead + n
        if use_in and lis_in:
            hi = ((p + 0x8000) >> 16) if pr.signed else (p >> 16)
            out[pr.lis - nlead] = (out[pr.lis - nlead] & 0xFFFF0000) | hi
            out[pr.use - nlead] = (out[pr.use - nlead] & 0xFFFF0000) | ((p - (hi << 16)) & 0xFFFF if pr.signed else p & 0xFFFF)
        elif use_in:
            if (lw_p >> 26) != 15:
                unresolved.append({'why': 'partner lis of the pair at %s is not a lis in the PAL code' % h8(pa + 4 * (pr.use - nlead))})
                conf = 'unresolved'
                continue
            hi = lw_p & 0xFFFF
            lo = (p - (hi << 16)) & 0xFFFF if pr.signed else p & 0xFFFF
            ok = (((hi << 16) + sext16(lo)) & 0xFFFFFFFF == p) if pr.signed else ((hi << 16) | lo) == p
            if not ok:
                unresolved.append({'why': 'PAL partner lis %04X cannot reach %s: companion lis at %s must change' % (hi, h8(p), h8(pa + 4 * (pr.lis - nlead)))})
                conf = 'unresolved'
                continue
            out[pr.use - nlead] = (out[pr.use - nlead] & 0xFFFF0000) | lo
            notes.append('low half re-derived against the PAL lis (%04X)' % hi)
        else:
            lo = uw_p & 0xFFFF
            if pr.signed:
                target_hi = ((p - sext16(lo)) >> 16) & 0xFFFF
                ok = ((target_hi << 16) + sext16(lo)) & 0xFFFFFFFF == p
            else:
                target_hi = p >> 16
                ok = (p & 0xFFFF) == lo
            if not ok:
                unresolved.append({'why': 'PAL low half %04X cannot reach %s' % (lo, h8(p))})
                conf = 'unresolved'
                continue
            out[pr.lis - nlead] = (out[pr.lis - nlead] & 0xFFFF0000) | target_hi
        conf = conf_min(conf, r['confidence'])
    return Reloc(out, notes, unresolved, collections.Counter(), conf)


def place_cave(eng, ps, hook_src, hook_pa, hook_conf, words, n_base, prov, name, hook_old_src, desc, ret_word=True):
    """Relocate `words` (cave body, last word may be the 0 placeholder for the return branch), allocate it in PAL, emit the
    hook patch and the cave patch."""
    words = list(words)
    body = words[:]
    placeholder = bool(body) and body[-1] == 0
    if not placeholder:
        body.append(0)
        pnote = ['cave has no 00000000 terminator: return branch appended']
    else:
        pnote = []
    nbytes = 4 * len(body)
    if n_base is not None and CAVE_LO <= n_base and n_base + nbytes <= CAVE_HI and not eng.identity:
        p_base, how = n_base, 'same address as in the source DOL'
    elif eng.identity and n_base is not None:
        p_base, how = n_base, 'identity'
    else:
        p_base, how = eng.alloc(nbytes), 'allocated in the cave region'
    if p_base is None:
        return ps.add(address=None, new_bytes=None, old_bytes=None, kind='code-injection', role='cave',
                      confidence='unresolved', provenance=prov, source_address=h8(hook_src), notes=['no free space left in the cave region'])
    # relocate everything but the placeholder
    core = body[:-1]
    rl = relocate_block(core, n_base, p_base, eng.xl, eng)
    new_words = rl.words + [0]
    ret_target = (hook_pa + 4) if hook_pa is not None else None
    if ret_target is not None:
        new_words[-1] = mk_b(p_base + 4 * (len(new_words) - 1), ret_target)
    ver = verify_block(new_words, p_base, eng, eng.xl) if hook_pa is not None else {'counts': {}, 'failures': ['no hook address']}
    conf = conf_min(rl.conf, hook_conf)
    if ver['failures']:
        conf = conf_min(conf, 'low')
    cave = {'name': name, 'source_base': h8(n_base) if n_base is not None else None, 'address': h8(p_base), 'placement': how,
            'instructions': len(new_words), 'relocation': dict(rl.stats), 'unresolved': rl.unresolved,
            'verification': ver['counts'], 'verification_failures': ver['failures'], 'confidence': conf}
    ps.caves.append(cave)
    hk = None
    if hook_pa is not None:
        hk_word = mk_b(hook_pa, p_base)
        hk = ps.add(address=h8(hook_pa), old_bytes=pal_bytes_hex(eng, hook_pa, 4), source_old_bytes=src_bytes_hex(eng, hook_src, 4),
                    new_bytes='%08x' % hk_word, kind='code-injection', role='hook', confidence=hook_conf,
                    provenance=prov, source_address=h8(hook_src), cave_address=h8(p_base), notes=[desc])
    else:
        hk = ps.add(address=None, old_bytes=None, new_bytes=None, kind='code-injection', role='hook', confidence='unresolved',
                    provenance=prov, source_address=h8(hook_src), notes=['hook address has no PAL translation'])
    cp = ps.add(address=h8(p_base), old_bytes=pal_bytes_hex(eng, p_base, nbytes), source_old_bytes=None,
                new_bytes=struct.pack('>%dI' % len(new_words), *new_words).hex(), kind='code-injection', role='cave',
                confidence=conf, provenance=prov, source_address=h8(n_base) if n_base is not None else None,
                hook=hk['id'], notes=pnote + rl.notes + ['unresolved: %s' % u['why'] for u in rl.unresolved]
                + ['verification: %s' % f for f in ver['failures']], placement=how,
                relocation=dict(rl.stats), verification=ver['counts'])
    return cp


# ----------------------------------------------------------------------------------------------- inputs
def run_gecko(eng, name, codes, only_enabled=False, kindname='gecko'):
    ps = PatchSet(name, eng, kindname)
    eng.reset_alloc()
    for ci, code in enumerate(codes):
        if only_enabled and code.enabled is False:
            continue
        prov = {'input': name, 'code': code.name, 'authors': code.authors}
        prov.update(ps.set_feature('g%03d' % (ci + 1), code.name, authors=code.authors, source=name))
        if getattr(code, 'kindname', 'Gecko') == 'AR':
            ops = ar_ops(code)
        else:
            ops = gecko_ops(code)
        first = len(ps.patches)
        for op in ops:
            conds = [cond_text(c) for c in op['cond']]
            if op['op'] in ('write', 'serial'):
                parts = op.get('parts') or [(op['addr'], op['data'])]
                for (a, data) in parts:
                    if op['indirect'] or op['depends']:
                        ps.add(address=None, old_bytes=None, new_bytes=data.hex(), kind='runtime', confidence='unresolved',
                               provenance=prov, source_address=h8(a), condition=conds or None,
                               notes=['base/pointer relative or state dependent: ' + (op['depends'] or 'ba/po addressing')])
                        continue
                    kind_o = None
                    if conds:
                        kind_o = 'runtime'
                    pc = translate_conditions(eng, op['cond'])
                    p = text_write(eng, ps, a, data, prov, kind_override=kind_o, cond=conds or None)
                    if conds:
                        p['condition'] = pc
            elif op['op'] == 'insert-asm':
                a = op['addr']
                r = eng.xl.map(a)
                ok = r['pal'] is not None
                if conds or op['depends']:
                    ps.add(address=h8(r['pal']) if ok else None, old_bytes=None, new_bytes=None, kind='runtime', role='hook',
                           confidence='unresolved', provenance=prov, source_address=h8(a), condition=conds,
                           notes=['C2 hook under a condition/state: not representable as a static hook'])
                    continue
                place_cave(eng, ps, a, r['pal'], r['confidence'], op['words'], None, prov, code.name, src_bytes_hex(eng, a, 4),
                           'C2 insert-asm hook (instruction at the hook site is replaced; the body must re-execute it if needed)')
            elif op['op'] == 'branch':
                a, t = op['addr'], op['target']
                r, rt = eng.xl.map(a), eng.xl.map(t)
                if r['pal'] is None or rt['pal'] is None:
                    ps.add(address=None, old_bytes=None, new_bytes=None, kind='static-patch', confidence='unresolved',
                           provenance=prov, source_address=h8(a), notes=['C6 branch endpoint has no PAL address (%s -> %s)' % (h8(a), h8(t))])
                    continue
                w = mk_b(r['pal'], rt['pal'])
                ps.add(address=h8(r['pal']), old_bytes=pal_bytes_hex(eng, r['pal'], 4), source_old_bytes=src_bytes_hex(eng, a, 4),
                       new_bytes='%08x' % w, kind='runtime' if conds else 'static-patch', confidence=conf_min(r['confidence'], rt['confidence']),
                       provenance=prov, source_address=h8(a), condition=conds or None, op='branch',
                       notes=['C6 branch %s -> %s' % (h8(r['pal']), h8(rt['pal']))])
            elif op['op'] == 'exec-asm':
                ps.add(address=None, old_bytes=None, new_bytes=struct.pack('>%dI' % len(op['words']), *op['words']).hex(),
                       kind='runtime', confidence='unresolved', provenance=prov, source_address=None, role='exec-asm',
                       notes=['C0 executes code every frame inside the handler: needs the engine, not translated'])
            elif op['op'] == 'unsupported':
                ps.add(address=None, old_bytes=None, new_bytes=None, kind='runtime', confidence='unresolved', provenance=prov,
                       source_address=None, notes=['code type %s is not supported' % op['code_type']])
            elif op['op'] == 'state':
                ps.add(address=None, old_bytes=None, new_bytes=None, kind='runtime', confidence='unresolved', provenance=prov,
                       source_address=None, notes=['code type %s changes base address/pointer/repeat/registers' % op['code_type']])
        if code.template:
            for p in ps.patches[first:]:
                p['needs_user_value'] = True
                p['notes'].append('the code has user-edit placeholders (X, read as 0 here): the bytes are a template')
    return ps


def translate_conditions(eng, conds):
    out = []
    for c in conds:
        if c.get('else'):
            out.append({'else': True})
            continue
        r = eng.xl.map(c['addr'], 4)
        d = dict(c)
        d['source_addr'] = h8(c['addr'])
        d['addr'] = h8(r['pal']) if r['pal'] is not None else None
        d['confidence'] = r['confidence']
        out.append(d)
    return out


def ar_ops(code):
    """Action Replay: only plain RAM writes (same 00/02/04 layout as Gecko) are decoded; other AR types are reported."""
    ops = []
    for i, (a, b) in enumerate(code.lines):
        t = a >> 24
        if (t & 0xFE) in (0x00, 0x02, 0x04):
            ops.extend(gecko_ops(GeckoCode(code.name, lines=[(a, b)])))
        else:
            ops.append({'op': 'state', 'code_type': '%02X' % t, 'cond': [], 'depends': 'Action Replay code type', 'line': i + 1})
    return ops


def site_state(eng, mod, s, a):
    """What the mod DOL holds at an MCM site: 'new' (the library's bytes are there), 'old' (the original), 'differs' (a branch
    into the cave region whose code is not the library text), 'other'. -> (state, cave base or None, words claimed)"""
    form = s['form']
    if form in ('word', 'multi'):
        got = [mod.word(a + 4 * k) for k in range(len(s['new']))]
        claimed = [a + 4 * k for k in range(len(s['new']))]
        if got == s['new']:
            return 'new', None, claimed
        if s.get('old') and got == s['old']:
            return 'old', None, claimed
        return 'other', None, claimed
    if form == 'branch':
        w = mod.word(a)
        bi = branch_info(w, a) if w is not None else None
        if bi and bi[0] == 'b' and not bi[2] and CAVE_LO <= bi[1] < CAVE_HI:
            base, exp = bi[1], list(s['cave'])
            claimed = [a] + [base + 4 * k for k in range(len(exp))]
            if exp and exp[-1] == 0:
                body = [mod.word(base + 4 * k) for k in range(len(exp) - 1)]
                last = base + 4 * (len(exp) - 1)
                ok = body == exp[:-1] and mod.word(last) == mk_b(last, a + 4)
            else:
                ok = [mod.word(base + 4 * k) for k in range(len(exp))] == exp
            return ('new' if ok else 'differs'), base, claimed
        if s.get('old') and w == s['old'][0]:
            return 'old', None, [a]
        return 'other', None, [a]
    return 'other', None, []


def entry_status(eng, mod, e):
    st = []
    claimed = []
    caves = []
    for s in e['sites']:
        off = s['offset']
        a = off if off >= 0x80000000 else eng.src.o2a(off)
        if a is None:
            st.append('other')
            continue
        state, base, cl = site_state(eng, mod, s, a)
        st.append(state)
        claimed += cl
        if base is not None:
            caves.append(base)
    n = len(st)
    c = collections.Counter(st)
    if not n:
        status = 'not-enabled'
    elif c['new'] == n:
        status = 'enabled'
    elif c['differs']:
        status = 'version-mismatch'
    elif c['new']:
        status = 'partial'
    else:
        status = 'not-enabled'
    return status, dict(c), claimed, caves


def diff_words(src, mod):
    out = []
    for i, o, ad, sz in src.secs:
        mo = mod.a2o(ad)
        if mo is None:
            continue
        x, y = src.data[o:o + sz], mod.data[mo:mo + sz]
        if x != y:
            for k in range(0, sz - 3, 4):
                if x[k:k + 4] != y[k:k + 4]:
                    out.append(ad + k)
    return out


def run_mcm(eng, name, path, all_entries=False):
    """With a mod DOL (--mod-dol) only the entries that are ENABLED in it are emitted: every site of the entry carries the
    library's new bytes in the DOL. Entries that are not there, only partly there, or there in another version are listed with
    their status and emit nothing (all or nothing). Without a mod DOL every entry is emitted."""
    ps = PatchSet(name, eng, 'mcm')
    eng.reset_alloc()
    entries = parse_mcm_files(path)
    ps.extra['entries'] = len(entries)
    nsites = 0
    mod = eng.mod_dol
    statuses = collections.Counter()
    estat = []
    claimed_all = set()
    mismatch_caves = {}
    mismatch_claim = {}
    for ei, e in enumerate(entries):
        fid = 'm%03d' % (ei + 1)
        if mod is not None:
            status, counts, claimed, caves = entry_status(eng, mod, e)
        else:
            status, counts, claimed, caves = 'unchecked', {}, [], []
        statuses[status] += 1
        estat.append({'id': fid, 'name': e['title'], 'file': e['file'], 'status': status, 'sites': len(e['sites']), 'site_states': counts})
        ps.set_feature(fid, e['title'], authors=e['authors'], source=e['file'], status=status, sites=len(e['sites']))
        if status == 'enabled':
            claimed_all.update(claimed)
        if status == 'version-mismatch':
            for c in caves:
                mismatch_caves[c] = e['title']
            for w_ in claimed:
                mismatch_claim[w_] = e['title']
        if mod is not None and not all_entries and status != 'enabled':
            continue
        for s in e['sites']:
            nsites += 1
            off = s['offset']
            a = off if off >= 0x80000000 else eng.src.o2a(off)
            prov = {'input': name, 'entry': e['title'], 'file': e['file'], 'offset': s['offset_text'], 'feature': e['title'], 'feature_id': fid}
            if a is None:
                ps.add(address=None, old_bytes=None, new_bytes=None, kind='static-patch', confidence='unresolved',
                       provenance=prov, source_address=None, notes=['DOL offset %s is not inside any section' % s['offset_text']])
                continue
            notes = []
            old = s.get('old')
            if old:
                actual = [eng.src.word(a + 4 * k) for k in range(len(old))]
                if actual != old:
                    notes.append('library original %s differs from the source DOL words %s' % (
                        ' '.join('%08X' % x for x in old), ' '.join('%08X' % x if x is not None else '--' for x in actual)))
            if s['form'] == 'unknown':
                ps.add(address=None, old_bytes=None, new_bytes=None, kind='static-patch', confidence='unresolved', provenance=prov,
                       source_address=h8(a), notes=['unrecognised library site form: ' + s['rest']])
                continue
            if s['form'] in ('word', 'multi'):
                data = struct.pack('>%dI' % len(s['new']), *s['new'])
                p = text_write(eng, ps, a, data, prov)
                p['notes'] = notes + p['notes']
                continue
            r = eng.xl.map(a)
            n_base = None
            cave_note = ''
            if mod is not None:
                w = mod.word(a)
                bi = branch_info(w, a) if w is not None else None
                if bi and bi[0] == 'b' and CAVE_LO <= bi[1] < CAVE_HI:
                    n_base = bi[1]
                    exp = list(s['cave'])
                    if exp and exp[-1] == 0:
                        got = [mod.word(n_base + 4 * k) for k in range(len(exp) - 1)]
                        match = got == exp[:-1]
                        cave_note = 'cave body %s the library text in the mod DOL' % ('equals' if match else 'DIFFERS from')
                        if not match:
                            notes.append(cave_note)
            place_cave(eng, ps, a, r['pal'], r['confidence'], s['cave'], n_base, prov, e['title'], src_bytes_hex(eng, a, 4),
                       'library branch hook' + ('; ' + cave_note if cave_note else ''))
            if notes:
                ps.patches[-1]['notes'] += notes
    ps.extra['sites'] = nsites
    ps.extra['entry_status'] = dict(statuses)
    ps.extra['entries_detail'] = estat
    if mod is not None:
        diff = diff_words(eng.src, mod)
        res = [a for a in diff if a not in claimed_all]

        def wclass(a):
            if a in mismatch_claim:
                return 'version-mismatch', mismatch_claim[a]
            if mod.word(a) == 0 and CAVE_LO <= a < CAVE_HI:
                return 'cave-zero-fill', None
            return 'unexplained', None
        runs = []
        for a in sorted(res):
            c = wclass(a)
            if runs and a == runs[-1][-1][0] + 4 and runs[-1][-1][1] == c:
                runs[-1].append((a, c))
            else:
                runs.append([(a, c)])
        resid = []
        for r_ in runs:
            cls, owner = r_[0][1]
            if cls == 'cave-zero-fill':
                why = 'zeroed original exception stubs (not needed in PAL)'
            elif cls == 'version-mismatch':
                why = 'hook or cave of the entry "%s", which the DOL holds in another version than the library text' % owner
            else:
                why = 'no enabled entry carries these words'
            resid.append({'address': h8(r_[0][0]), 'words': len(r_), 'class': cls, 'why': why})
        ps.extra['diff_words'] = len(diff)
        ps.extra['explained_words'] = len(diff) - len(res)
        ps.extra['residue'] = resid
        ps.extra['residue_unexplained_words'] = sum(x['words'] for x in resid if x['class'] == 'unexplained')
        ps.extra['residue_version_mismatch_words'] = sum(x['words'] for x in resid if x['class'] == 'version-mismatch')
        ps.extra['residue_zero_fill_words'] = sum(x['words'] for x in resid if x['class'] == 'cave-zero-fill')
    return ps


def run_dol_diff(eng, name, mod_path):
    ps = PatchSet(name, eng, 'dol-diff')
    eng.reset_alloc()
    mod = Dol(mod_path)
    base = eng.src
    changed = []
    for i, o, ad, sz in base.secs:
        mo = mod.a2o(ad)
        x = base.data[o:o + sz]
        y = mod.data[mod.a2o(ad):mod.a2o(ad) + sz] if mo is not None else x
        if x != y:
            # word-granular diff
            for k in range(0, sz - 3, 4):
                if x[k:k + 4] != y[k:k + 4]:
                    changed.append(ad + k)
    ps.extra['changed_words'] = len(changed)
    ps.extra['sections_differ'] = [s[0] for s in base.secs if mod.sec(s[2]) != s[0]]
    cave_words = [a for a in changed if CAVE_LO <= a < CAVE_HI]
    other = [a for a in changed if not (CAVE_LO <= a < CAVE_HI)]
    # hooks: changed words that branch into the cave area
    hooks, inline = {}, []
    for a in other:
        w = mod.word(a)
        bi = branch_info(w, a)
        if base.kind(a) == 'text' and bi and bi[0] == 'b' and CAVE_LO <= bi[1] < CAVE_HI:
            hooks[a] = bi[1]
        else:
            inline.append(a)
    # cave code reachable from hooks
    caveset = set(cave_words)
    reach_code = set()
    work = list(hooks.values())
    while work:
        t = work.pop()
        while CAVE_LO <= t < CAVE_HI and t not in reach_code:
            w = mod.word(t)
            reach_code.add(t)
            bi = branch_info(w, t)
            if bi and bi[1] is not None and CAVE_LO <= bi[1] < CAVE_HI and not bi[2]:
                work.append(bi[1])
                if bi[0] == 'b':
                    break
            if w in (0x4E800020, 0x4E800420):
                break
            if bi and bi[0] == 'b' and not (CAVE_LO <= bi[1] < CAVE_HI):
                break
            t += 4
    # data refs inside the caves keep as data
    code_words = sorted(a for a in caveset if a in reach_code)
    data_words = sorted(a for a in caveset if a not in reach_code)
    # group cave code per hook: contiguous runs of reached words
    runs = []
    for a in sorted(caveset):
        if runs and a == runs[-1][-1] + 4:
            runs[-1].append(a)
        else:
            runs.append([a])
    ps.extra['cave_words'] = len(cave_words)
    ps.extra['cave_code_words'] = len(code_words)
    ps.extra['cave_data_words'] = len(data_words)
    ps.extra['hooks'] = len(hooks)
    ps.extra['inline_words'] = len(inline)
    prov = {'input': name, 'mod_dol': os.path.basename(mod_path)}
    # inline changes: runs of consecutive words grouped by kind
    irun = []
    for a in inline:
        if irun and a == irun[-1][-1] + 4 and base.kind(a) == base.kind(irun[-1][0]):
            irun[-1].append(a)
        else:
            irun.append([a])
    for r in irun:
        data = b''.join(struct.pack('>I', mod.word(a)) for a in r)
        text_write(eng, ps, r[0], data, dict(prov, kind='inline'))
    # hooks and caves
    bases_done = set()
    for hook, tgt in sorted(hooks.items()):
        r = eng.xl.map(hook)
        # extent of the cave this hook enters: its run's contiguous reached words
        run = next((x for x in runs if x[0] <= tgt <= x[-1]), None)
        if run is None:
            continue
        key = run[0]
        if key in bases_done:
            ps.add(address=h8(r['pal']) if r['pal'] is not None else None, old_bytes=pal_bytes_hex(eng, r['pal'], 4) if r['pal'] else None,
                   new_bytes='%08x' % mk_b(r['pal'], tgt) if r['pal'] is not None else None, kind='code-injection', role='hook',
                   confidence=r['confidence'], provenance=prov, source_address=h8(hook), cave_address=h8(tgt),
                   notes=['second hook into an already relocated cave'])
            continue
        bases_done.add(key)
        words = [mod.word(a) for a in run]
        body_cave_run = run
        # same address, no placeholder: cave is a closed block whose exits are explicit branches
        rl = relocate_block(words, run[0], run[0], eng.xl, eng)
        ver = verify_block(rl.words, run[0], eng, eng.xl)
        conf = conf_min(rl.conf, r['confidence'])
        if ver['failures']:
            conf = conf_min(conf, 'low')
        ps.caves.append({'name': 'cave@%s' % h8(run[0]), 'source_base': h8(run[0]), 'address': h8(run[0]), 'placement': 'same address as in the source DOL',
                         'instructions': len(words), 'relocation': dict(rl.stats), 'unresolved': rl.unresolved,
                         'verification': ver['counts'], 'verification_failures': ver['failures'], 'confidence': conf,
                         'data_words_in_run': sum(1 for a in run if a in set(data_words))})
        ps.add(address=h8(r['pal']) if r['pal'] is not None else None, old_bytes=pal_bytes_hex(eng, r['pal'], 4) if r['pal'] else None,
               source_old_bytes=src_bytes_hex(eng, hook, 4), new_bytes='%08x' % mk_b(r['pal'], tgt) if r['pal'] is not None else None,
               kind='code-injection', role='hook', confidence=r['confidence'], provenance=prov, source_address=h8(hook),
               cave_address=h8(tgt), notes=[])
        ps.add(address=h8(run[0]), old_bytes=pal_bytes_hex(eng, run[0], 4 * len(run)), new_bytes=struct.pack('>%dI' % len(rl.words), *rl.words).hex(),
               kind='code-injection', role='cave', confidence=conf, provenance=prov, source_address=h8(run[0]),
               notes=rl.notes + ['unresolved: %s' % u['why'] for u in rl.unresolved] + ['verification: %s' % f for f in ver['failures']],
               relocation=dict(rl.stats), verification=ver['counts'])
    orphan = [x for x in runs if x[0] not in bases_done]
    if orphan:
        ps.extra['cave_runs_without_hook'] = [{'address': h8(x[0]), 'words': len(x)} for x in orphan]
        for x in orphan:
            words = [mod.word(a) for a in x]
            if all(w == 0 for w in words):
                ps.add(address=h8(x[0]), old_bytes=None, new_bytes=None, kind='code-injection', role='cave-clear', confidence='high',
                       provenance=prov, source_address=h8(x[0]), notes=['zero fill of %d words of original stubs (mtspr..rfi exception stubs); not emitted: '
                                                                         'the PAL stubs are unreferenced and need no clearing' % len(x)])
                continue
            ps.add(address=h8(x[0]), old_bytes=None, new_bytes=None, kind='code-injection', role='cave-orphan', confidence='low',
                   provenance=prov, source_address=h8(x[0]), notes=['changed words in the cave region not reached from any hook (%d words): '
                                                                      'data or a hook outside the diff; not emitted' % len(x)])
    return ps


# ================================================================================================= reporting
def summarize(ps):
    by_kind = collections.Counter(p['kind'] for p in ps.patches)
    by_conf = collections.Counter(p['confidence'] for p in ps.patches)
    cav = collections.Counter()
    for c in ps.caves:
        cav['caves'] += 1
        cav['instructions'] += c['instructions']
        for k, v in c['relocation'].items():
            cav['reloc-' + k] += v
        cav['unresolved-items'] += len(c['unresolved'])
        cav['verify-failures'] += len(c['verification_failures'])
        for k, v in c['verification'].items():
            cav['verified-' + k] += v
        cav['cave-conf-' + c['confidence']] += 1
    groups = collections.OrderedDict()
    for p in ps.patches:
        g = p['provenance'].get('code') or p['provenance'].get('entry') or p['provenance'].get('input')
        e = groups.setdefault(g, {'patches': 0, 'kinds': collections.Counter(), 'worst': 'high'})
        e['patches'] += 1
        e['kinds'][p['kind']] += 1
        e['worst'] = conf_min(e['worst'], p['confidence'])
    gs = [{'name': k, 'patches': v['patches'], 'kinds': dict(v['kinds']), 'worst_confidence': v['worst']} for k, v in groups.items()]
    return {'patches': len(ps.patches), 'by_kind': dict(by_kind), 'by_confidence': dict(by_conf), 'caves': dict(cav),
            'unresolved': len(ps.unresolved), 'extra': ps.extra, 'groups': gs}


def crosscheck(mcm_ps, diff_ps, eng):
    """Every changed word of the DOL diff (source addresses) against the MCM sites/caves (source addresses)."""
    def src_set(ps):
        s = []
        for p in ps.patches:
            if p.get('source_address'):
                a = int(p['source_address'], 16)
                n = 4
                nb = p.get('new_bytes_source')
                if nb:
                    n = len(nb) // 2
                s.append((a, n))
        return s
    mcm_sites = sorted(src_set(mcm_ps))
    cave_ranges = []
    for c in mcm_ps.caves:
        if c.get('source_base'):
            cave_ranges.append((int(c['source_base'], 16), 4 * c['instructions']))
    starts = [x[0] for x in mcm_sites]
    out = collections.Counter()
    far = []
    for p in diff_ps.patches:
        if not p.get('source_address'):
            continue
        a = int(p['source_address'], 16)
        if p.get('role') == 'cave-clear':
            out['cave-clear (zero fill)'] += 1
            continue
        if any(s <= a < s + n for s, n in cave_ranges):
            out['in-mcm-cave'] += 1
            continue
        i = bisect.bisect_left(starts, a)
        d = min((abs(a - starts[j]) for j in (i - 1, i) if 0 <= j < len(starts)), default=None)
        if d is not None and d <= 64:
            out['within-64-of-mcm-site'] += 1
        else:
            out['no-mcm-site-within-64'] += 1
            far.append(p['source_address'])
    def image(ps):
        img = {}
        for p in ps.patches:
            if p.get('address') and p.get('new_bytes') and p['kind'] != 'runtime':
                b = bytes.fromhex(p['new_bytes'])
                a0 = int(p['address'], 16)
                for k in range(0, len(b) - 3, 4):
                    img[a0 + k] = b[k:k + 4]
        return img
    im, idf = image(mcm_ps), image(diff_ps)
    both = set(im) & set(idf)
    cmp = {'words_in_mcm_patchset': len(im), 'words_in_diff_patchset': len(idf), 'common': len(both),
           'identical': sum(1 for a in both if im[a] == idf[a]), 'differ': sorted(h8(a) for a in both if im[a] != idf[a])[:40],
           'only_in_mcm': sorted(h8(a) for a in set(im) - set(idf))[:40], 'only_in_mcm_n': len(set(im) - set(idf)),
           'only_in_diff_n': len(set(idf) - set(im))}
    return {'counts': dict(out), 'far': far[:40], 'pal_image_compare': cmp}


def selftest_relocate(eng, limit=0, stride=1):
    """Ground truth for the relocator: whole functions that are identical modulo relocation in both regions are relocated
    from the source DOL to PAL and compared word for word with the real PAL function."""
    res = collections.Counter()
    miss = collections.Counter()
    examples = []
    rows = eng.pf.rows
    done = 0
    for idx in range(0, len(rows), stride):
        pa, size = rows[idx]
        if size < 8 or eng.pal.kind(pa) != 'text':
            continue
        r, kind = None, None
        # source address of the function start from the map (code segments only)
        for s_, e_, d_, k_ in eng.amap.segs:
            if k_ in ('code', 'fixed') and s_ - d_ <= pa and pa + size <= e_ - d_:
                r = pa + d_
                break
        if eng.identity:
            r = pa
        if r is None or CAVE_LO <= pa < CAVE_HI:
            continue
        nw = [eng.src.word(r + 4 * k) for k in range(size // 4)]
        pw = [eng.pal.word(pa + 4 * k) for k in range(size // 4)]
        if None in nw or None in pw:
            continue
        if any(shape(x) != shape(y) for x, y in zip(nw, pw)):
            res['functions-skipped-bodies-differ'] += 1
            continue
        rl = relocate_block(nw, r, pa, eng.xl, eng)
        res['functions'] += 1
        res['words'] += len(nw)
        for k, (x, y) in enumerate(zip(rl.words, pw)):
            if x == y:
                res['words-equal'] += 1
            else:
                op = y >> 26
                key = {18: 'branch', 16: 'cond-branch', 14: 'addi', 15: 'lis', 24: 'ori'}.get(op, 'mem' if 32 <= op <= 55 else 'other')
                miss[key] += 1
                if len(examples) < 12:
                    examples.append({'pal_address': h8(pa + 4 * k), 'relocated': '%08X' % x, 'pal': '%08X' % y, 'source': '%08X' % nw[k]})
        res['functions-unresolved'] += 1 if rl.unresolved else 0
        for kk, v in rl.stats.items():
            res['reloc-' + kk] += v
        done += 1
        if limit and done >= limit:
            break
    res['words-mismatch'] = sum(miss.values())
    return {'counts': dict(res), 'mismatch_by_kind': dict(miss), 'examples': examples}


def process_inputs(eng, specs, only_enabled=False, all_entries=False):
    """Run every input once -> {name: PatchSet}."""
    sets = {}
    for spec in specs:
        kind, _, path = spec.partition(':')
        name = re.sub(r'[^A-Za-z0-9_.-]+', '_', os.path.basename(path.rstrip('/\\')) or kind)
        name = '%s-%s' % (kind, name) if name in sets else name
        if kind == 'gecko-ini':
            codes = parse_gecko_ini(path)
            ps = run_gecko(eng, name, codes, only_enabled)
            ps.extra['codes'] = len(codes)
        elif kind == 'gecko-text':
            codes = parse_gecko_text(path)
            ps = run_gecko(eng, name, codes, only_enabled)
            ps.extra['codes'] = len(codes)
        elif kind == 'gct':
            codes = parse_gct(path)
            ps = run_gecko(eng, name, codes, only_enabled)
            ps.extra['codes'] = len(codes)
        elif kind == 'mcm':
            ps = run_mcm(eng, name, path, all_entries)
        elif kind == 'dol-diff':
            ps = run_dol_diff(eng, name, path)
        else:
            raise SystemExit('unknown input kind ' + kind)
        ps.input_kind = kind
        sets[name] = ps
    return sets


def run_two_pass(eng, specs, only_enabled=False, all_entries=False):
    """Pass 1 collects the scratch constants of every input, the clusters are built, pass 2 produces the patch sets."""
    eng.scratch.reset_collect()
    eng.xl.cache.clear()
    process_inputs(eng, specs, only_enabled, all_entries)
    eng.scratch.build()
    eng.xl.cache.clear()
    return process_inputs(eng, specs, only_enabled, all_entries)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--src-dol', required=True, help='main.dol of the region the mod was written for (read only)')
    ap.add_argument('--pal-dol', required=True, help='main.dol of GUPP8P (read only)')
    ap.add_argument('--address-map', help='extract/address-maps/<region>.map (not needed for GUPP8P)')
    ap.add_argument('--pal-functions', required=True, help='extract/functions.csv')
    ap.add_argument('--source-region', default='GUPE8P', choices=['GUPE8P', 'GUPR8P', 'GUPP8P', 'GUPJ8P'])
    ap.add_argument('--mod-dol', help='mod main.dol: MCM libraries emit only the entries enabled in it, at its cave addresses')
    ap.add_argument('--all-entries', action='store_true', help='with --mod-dol: emit every MCM entry, enabled or not')
    ap.add_argument('--scratch-map', help='JSON file with the scratch clusters of earlier runs; read, extended and written back '
                                          '(keeps the placement stable between runs that share the scratch region)')
    ap.add_argument('--input', action='append', default=[], metavar='KIND:PATH')
    ap.add_argument('--selftest-relocate', type=int, nargs='?', const=0, default=None, metavar='MAX_FUNCTIONS',
                    help='relocate real source-region functions to PAL and compare with the real PAL functions (0 = all)')
    ap.add_argument('--only-enabled', action='store_true', help='skip codes a [Gecko_Enabled] list leaves out')
    ap.add_argument('--out-dir', required=True)
    a = ap.parse_args()
    os.makedirs(a.out_dir, exist_ok=True)
    eng = Engine(a)
    report = {'tool': 'patchset.py', 'source_region': a.source_region, 'cave_region': eng.cave_region_report(), 'inputs': {}}
    sets = run_two_pass(eng, a.input, a.only_enabled, a.all_entries)
    scratch = eng.scratch.report()
    report['scratch'] = scratch
    for name, ps in sets.items():
        doc = {'tool': 'patchset.py', 'source_region': a.source_region, 'target_region': 'GUPP8P', 'input': ps.input_kind,
               'summary': summarize(ps), 'features': build_features(ps), 'scratch': scratch, 'patches': ps.patches,
               'caves': ps.caves, 'unresolved': ps.unresolved}
        with open(os.path.join(a.out_dir, name + '.patchset.json'), 'w', encoding='utf-8') as f:
            json.dump(doc, f, indent=1)
        report['inputs'][name] = summarize(ps)
        report['inputs'][name]['kind'] = ps.input_kind
        report['inputs'][name]['features'] = [{k: v for k, v in f_.items() if k != 'patches'} for f_ in doc['features']]
    if a.scratch_map:
        with open(a.scratch_map, 'w', encoding='utf-8') as f:
            json.dump({'scratch': scratch}, f, indent=1)
    if a.selftest_relocate is not None:
        eng.scratch.reset_collect()          # real functions keep their heap constants as they are
        eng.xl.cache.clear()
        report['selftest_relocate'] = selftest_relocate(eng, a.selftest_relocate)
    mcms = [n for n, p in sets.items() if p.kindname == 'mcm']
    diffs = [n for n, p in sets.items() if p.kindname == 'dol-diff']
    if mcms and diffs:
        report['crosscheck_mcm_vs_dol_diff'] = crosscheck(sets[mcms[0]], sets[diffs[0]], eng)
    with open(os.path.join(a.out_dir, 'report.json'), 'w', encoding='utf-8') as f:
        json.dump(report, f, indent=1)
    print(json.dumps(report, indent=1))


if __name__ == '__main__':
    main()
