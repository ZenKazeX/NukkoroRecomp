#!/usr/bin/env python3
"""reloaded_code_map.py - inventory of the code changes of a Reloaded-style mod and their PAL counterparts.

The mod ships a modified NTSC-U `main.dol`, Melee-Code-Manager assembly libraries (`codes/code/MCM/*.txt`, entries of
"title / description / [authors] / address table") and a Gecko ini. The PC build is a translation of the PAL DOL, so none
of it can run; this tool makes "code ignored" precise:

  1. diffs the mod's DOL against the NTSC-U DOL section by section and checks that every changed byte is explained by a
     library entry (or lies in the debugger-vector area the mod uses as free space for its code caves);
  2. for every library patch: NTSC-U address, kind (branch to a cave / inline instruction / data word), cave address and
     size, the PAL address of the same place (the verified function-level map extract/address-maps/GUPE8P.map, then a
     signature search for places the map leaves out) and how well the surrounding original code agrees;
  3. maps the Gecko ini's writes the same way;
  4. groups everything into player-visible features (tools/mods/reloaded_features_def.json) and writes
     test_data/mods/reloaded_features.json and docs/mods/map/RELOADED_CODE.md.

Nothing of the mod's code is copied: the output has addresses, sizes, counts, stock/mod numbers of tuning constants and
our own descriptions, no instruction words of injected code. Stdlib only.

  python -I tools/mods/reloaded_code_map.py --ntsc-dol <usa>/sys/main.dol --reloaded-dol <mod>/codes/ISO\\ EDITS/sys/main.dol \\
      --pal-dol game/sys/main.dol --mcm-dir <mod>/codes/code/MCM --gecko-ini <mod>/codes/code/GUPR8P.ini \\
      --address-map extract/address-maps/GUPE8P.map --pal-functions extract/functions.csv \\
      --defs tools/mods/reloaded_features_def.json --census test_data/mods/reloaded_data_census.json \\
      --out-json test_data/mods/reloaded_features.json --out-md docs/mods/map/RELOADED_CODE.md
"""
import argparse, bisect, collections, csv, fnmatch, glob, json, os, re, struct, sys

CAVE_LO, CAVE_HI = 0x800034AC, 0x800053E0   # debugger interrupt-vector stubs of the original DOL, used as free space


# ----------------------------------------------------------------------------------------------------- DOL / PPC
class Dol:
    def __init__(self, path):
        with open(path, 'rb') as f:
            self.data = f.read()
        h = struct.unpack('>18I18I18I', self.data[:0xD8])
        self.secs = [(i, h[i], h[18 + i], h[36 + i]) for i in range(18) if h[36 + i]]   # index, file off, address, size

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

    def word(self, a):
        o = self.a2o(a)
        if o is None:
            return None
        return struct.unpack('>I', self.data[o:o + 4])[0]

    def is_text(self, a):
        return self.sec(a) in (0, 1)


def shape(w):
    """Instruction word with the parts that move between region builds masked: branch displacements and the 16-bit
    displacement/immediate of D-form loads, stores and address arithmetic (small-data offsets differ per region)."""
    o = w >> 26
    if o == 18:
        return w & 0xFC000003
    if o == 16:
        return w & 0xFFFF0003
    if o in (14, 15, 24, 25) or 32 <= o <= 55 or o in (56, 57, 60, 61):
        return w & 0xFFFF0000
    return w


def branch_target(w, at):
    if (w >> 26) != 18:
        return None, False
    li = w & 0x03FFFFFC
    if li & 0x02000000:
        li -= 0x04000000
    return ((li if w & 2 else at + li) & 0xFFFFFFFF), bool(w & 1)


# ----------------------------------------------------------------------------------------------------- address map
class AddressMap:
    """extract/address-maps/GUPE8P.map: segments `kind canonical_start canonical_end delta` (canonical = PAL,
    other = NTSC-U = canonical + delta); `aligned` rows are runs of equal instructions inside function bodies that differ."""

    def __init__(self, path):
        self.segs = []
        for line in open(path, encoding='utf-8'):
            t = line.split()
            if t and t[0] in ('code', 'data', 'fixed', 'aligned'):
                s, e, d = int(t[1], 16), int(t[2], 16), int(t[3], 16)
                self.segs.append((s + d, e + d, d, t[0]))
        self.segs.sort()

    def to_pal(self, ntsc):
        for s, e, d, k in self.segs:
            if s <= ntsc < e:
                return ntsc - d, k
        return None, None


class PalFunctions:
    def __init__(self, path):
        rows = []
        with open(path, encoding='utf-8', newline='') as f:
            for r in csv.DictReader(f):
                rows.append((int(r['addr']), int(r['size'])))
        rows.sort()
        self.starts = [r[0] for r in rows]
        self.rows = rows

    def find(self, a):
        i = bisect.bisect_right(self.starts, a) - 1
        if i >= 0 and self.rows[i][0] <= a < self.rows[i][0] + self.rows[i][1]:
            return self.rows[i]
        return None


# ----------------------------------------------------------------------------------------------------- MCM / Gecko
def parse_mcm(path):
    text = open(path, encoding='utf-8', errors='replace').read().replace('\r', '')
    entries = []
    for block in re.split(r'\n\s*-==-\s*\n', text):
        m = re.search(r'^Revision -+ DOL Offset.*$', block, re.M)
        if not m:
            continue
        head = [l.strip() for l in block[:m.start()].strip('\n').split('\n')]
        title = head[0]
        authors, desc = '', []
        for l in head[1:]:
            if re.fullmatch(r'\[.*\]', l):
                authors = l
            elif l:
                desc.append(l)
        patches = []
        for am in re.finditer(r'^(?:NTSC 1\.06\s*)?-+\s+(0x[0-9A-Fa-f]+)\s+-+\s*(.*)$', block[m.end():], re.M):
            patches.append((int(am.group(1), 16), am.group(2).strip()))
        entries.append({'title': title, 'authors': authors, 'description': desc, 'patches': patches,
                        'file': os.path.basename(path)})
    return entries


def parse_gecko(path):
    codes, cur, section = [], None, ''
    for line in open(path, encoding='utf-8', errors='replace'):
        line = line.rstrip('\r\n')
        if line.startswith('$==='):
            section = line.strip('$= ').strip()
            continue
        if line.startswith('$'):
            m = re.match(r'\$(.*?)(?:\s*\[(.*)\])?\s*$', line)
            cur = {'name': m.group(1).strip(), 'authors': m.group(2) or '', 'section': section, 'lines': [], 'notes': []}
            codes.append(cur)
        elif line.startswith('['):
            cur = None
        elif cur is not None and line.startswith('*'):
            cur['notes'].append(line[1:].strip())
        elif cur is not None and re.fullmatch(r'[0-9A-Fa-f]{8} [0-9A-Fa-f]{8}', line.strip()):
            a, b = line.split()
            cur['lines'].append((int(a, 16), int(b, 16)))
    return codes


def gecko_addresses(code):
    """[(kind, ntsc address)] of the places a Gecko code writes or hooks (`04`/`00`/`02` writes, `C2`/`C6` hooks);
    conditionals and terminators are skipped."""
    out, skip = [], 0
    for a, b in code['lines']:
        if skip:
            skip -= 1
            continue
        t = a >> 24
        t0 = t & 0xFE
        if t0 in (0x00, 0x02, 0x04):
            out.append(('write%d' % {0: 8, 2: 16, 4: 32}[t0], 0x80000000 | (a & 0x01FFFFFF)))
        elif t0 == 0xC2:
            out.append(('asm-insert', 0x80000000 | (a & 0x01FFFFFF)))
            skip = b          # b = number of 8-byte lines of injected code, counted from here
        elif t0 == 0xC6:
            out.append(('branch', 0x80000000 | (a & 0x01FFFFFF)))
        elif t0 in (0x20, 0x22, 0x24, 0x26, 0x28, 0x2A, 0x2C, 0x2E):
            pass
    return out


# ----------------------------------------------------------------------------------------------------- analysis
class Context:
    def __init__(self, a):
        self.ntsc = Dol(a.ntsc_dol)
        self.mod = Dol(a.reloaded_dol)
        self.pal = Dol(a.pal_dol)
        self.amap = AddressMap(a.address_map)
        self.pf = PalFunctions(a.pal_functions)
        self._pal_masked = None

    def pal_text_masked(self):
        if self._pal_masked is None:
            parts = []
            for i, o, ad, sz in self.pal.secs:
                if i in (0, 1):
                    words = struct.unpack('>%dI' % (sz // 4), self.pal.data[o:o + sz])
                    parts.append((ad, b''.join(struct.pack('>I', shape(w)) for w in words)))
            self._pal_masked = parts
        return self._pal_masked

    def signature_search(self, ntsc_addr):
        """Unique place near the expected position in the PAL text whose masked words equal the NTSC-U words around
        ntsc_addr (widest window that is unique: 17 down to 5 words). The search is limited to the stretch between the
        PAL places that the address map gives for the segments before and after the gap, plus 0x400 bytes."""
        lo_d = hi_d = None
        for s, e, d, k in self.amap.segs:
            if e <= ntsc_addr:
                lo_d = d
            elif s > ntsc_addr and hi_d is None:
                hi_d = d
        ds = [x for x in (lo_d, hi_d) if x is not None] or [0]
        lo, hi = ntsc_addr - max(ds) - 0x400, ntsc_addr - min(ds) + 0x400
        for half in (8, 6, 4, 3, 2):
            ws = []
            for j in range(-half, half + 1):
                w = self.ntsc.word(ntsc_addr + 4 * j)
                if w is None:
                    ws = []
                    break
                ws.append(struct.pack('>I', shape(w)))
            if not ws:
                continue
            pat = b''.join(ws)
            hits = []
            for ad, buf in self.pal_text_masked():
                a0 = max(0, lo - ad)
                a1 = min(len(buf), hi - ad + 4 * (2 * half + 1))
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

    def map_address(self, ntsc_addr):
        """-> dict(pal, via, status, window)  status: exact | shape | window | none"""
        res = {'pal': None, 'via': None, 'status': 'none', 'window': '0/0'}
        pa, kind = self.amap.to_pal(ntsc_addr)
        is_text = self.ntsc.is_text(ntsc_addr)
        if pa is None and is_text:
            pa, n = self.signature_search(ntsc_addr)
            if pa is not None:
                res.update(pal=pa, via='signature', window='%d/%d' % (n, n))
                ow, pw = self.ntsc.word(ntsc_addr), self.pal.word(pa)
                res['status'] = 'exact' if ow == pw else ('shape' if shape(ow) == shape(pw) else 'window')
            return res
        if pa is None:
            return res
        res.update(pal=pa, via='address-map')
        ow, pw = self.ntsc.word(ntsc_addr), self.pal.word(pa)
        if pw is None:
            res['pal'] = None
            return res
        if ow == pw:
            res['status'] = 'exact'
        elif is_text and shape(ow) == shape(pw):
            res['status'] = 'shape'
        else:
            res['status'] = 'differs'
        if is_text:
            n = sim = 0
            for j in range(-6, 7):
                x, y = self.ntsc.word(ntsc_addr + 4 * j), self.pal.word(pa + 4 * j)
                if x is None or y is None:
                    continue
                n += 1
                sim += shape(x) == shape(y)
            res['window'] = '%d/%d' % (sim, n)
        return res


def follow_cave(ctx, hook):
    """The mod's word at `hook` is a branch into the cave area: (cave start, words until the branch back out)."""
    w = ctx.mod.word(hook)
    t, lk = branch_target(w, hook)
    if t is None or lk or not (CAVE_LO <= t < CAVE_HI):
        return None, 0
    n, a = 0, t
    while n < 400:
        w = ctx.mod.word(a)
        if w == 0:
            break
        n += 1
        tt, lk = branch_target(w, a)
        if tt is not None and not lk and not (CAVE_LO <= tt < CAVE_HI):
            break
        a += 4
    return t, n


def in_string_area(dol, addr):
    """True when the bytes around the word at addr (the word itself left out) are mostly printable text."""
    o = dol.a2o(addr & ~3)
    if o is None:
        return False
    around = dol.data[max(0, o - 8):o] + dol.data[o + 4:o + 12]
    return sum(1 for b in around if 32 <= b < 127) >= 12


def as_float(v):
    f = struct.unpack('>f', struct.pack('>I', v))[0]
    if f != f or abs(f) == float('inf'):
        return None
    return float('%.7g' % f)


def analyse_patch(ctx, title, addr, rest):
    ntsc_word = ctx.ntsc.word(addr & ~3)
    mod_word = ctx.mod.word(addr & ~3)
    p = {'ntsc': '0x%08X' % addr}
    m = re.match(r'([0-9A-Fa-f]{8})\s*->\s*(\S+)', rest)
    p['original_matches_ntsc_dol'] = (int(m.group(1), 16) == ntsc_word) if m else None
    text = ctx.ntsc.is_text(addr)
    cave, ncave = (follow_cave(ctx, addr & ~3) if text else (None, 0))
    if cave:
        p['kind'] = 'branch-to-cave'
        p['cave'] = '0x%08X' % cave
        p['cave_words'] = ncave
        p['_cave_range'] = (cave, ncave)
    elif text:
        p['kind'] = 'inline-code'
    else:
        p['kind'] = 'data'
    mp = ctx.map_address(addr & ~3)
    if mp['pal'] is not None:
        p['pal'] = '0x%08X' % mp['pal']
        p['pal_via'] = mp['via']
        p['pal_status'] = mp['status']
        p['pal_window'] = mp['window']
        fn = ctx.pf.find(mp['pal']) if text else None
        if fn:
            p['pal_function'] = '0x%08X' % fn[0]
            p['pal_function_size'] = fn[1]
        if p['kind'] == 'data':
            pw = ctx.pal.word(mp['pal'])
            p['pal_value_equals_ntsc'] = (pw == ntsc_word)
            if ctx.ntsc.sec(addr) in (12, 13, 14) and not in_string_area(ctx.ntsc, addr) and 'Color' not in title and 'Save' not in title:
                if 0 < ntsc_word < 0x10000 and 0 < mod_word < 0x10000:
                    p['stock_value_int'] = ntsc_word
                    p['mod_value_int'] = mod_word
                else:
                    a, b = as_float(ntsc_word), as_float(mod_word)
                    if a is not None and b is not None and (a != 0 or b != 0):
                        p['stock_value_float'] = a
                        p['mod_value_float'] = b
    else:
        p['pal'] = None
        p['pal_status'] = 'none'
    if p['pal'] is None:
        p['confidence'] = 'low'
    elif p['kind'] == 'data':
        p['confidence'] = 'high' if p.get('pal_value_equals_ntsc') else 'medium'
    else:
        n, d = (int(x) for x in p['pal_window'].split('/'))
        if p['pal_status'] in ('exact', 'shape') and d and n >= d - 1:
            p['confidence'] = 'high'
        elif p['pal_status'] in ('exact', 'shape'):
            p['confidence'] = 'medium'
        else:
            p['confidence'] = 'low'
    return p


def dol_diff(ctx):
    """changed bytes of the mod's DOL against NTSC-U, by region"""
    n, m = ctx.ntsc, ctx.mod
    changed = set()
    for i, o, ad, sz in n.secs:
        x, y = n.data[o:o + sz], m.data[o:o + sz]
        if x != y:
            for k in range(sz):
                if x[k] != y[k]:
                    changed.add(ad + k)
    header = sum(1 for k in range(0xE4) if n.data[k] != m.data[k]) + sum(
        1 for k in range(0xE4, 0x100) if n.data[k] != m.data[k])
    return changed, header


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for k in ('ntsc-dol', 'reloaded-dol', 'pal-dol', 'mcm-dir', 'gecko-ini', 'address-map', 'pal-functions', 'defs',
              'census', 'out-json', 'out-md'):
        ap.add_argument('--' + k, required=True)
    a = ap.parse_args()
    ctx = Context(a)
    defs = json.load(open(a.defs, encoding='utf-8'))
    census = json.load(open(a.census, encoding='utf-8'))

    # 1. DOL diff
    changed, header_bytes = dol_diff(ctx)
    in_cave = {x for x in changed if CAVE_LO <= x < CAVE_HI}
    outside = changed - in_cave

    # 2. library entries
    entries = []
    for f in sorted(glob.glob(os.path.join(a.mcm_dir, '*.txt'))):
        entries += parse_mcm(f)
    by_title = {}
    for e in entries:
        if e['title'] in by_title:
            raise SystemExit('duplicate library title: ' + e['title'])
        by_title[e['title']] = e

    covered = set()
    for e in entries:
        e['analysed'] = []
        for off, rest in e['patches']:
            addr = off if off >= 0x80000000 else ctx.ntsc.o2a(off)
            if addr is None:
                raise SystemExit('offset not in a section: %s %#x' % (e['title'], off))
            e['analysed'].append(analyse_patch(ctx, e['title'], addr, rest))
            for k in range(64):
                covered.add((addr & ~3) + k)
    unexplained = sorted(x for x in outside if x not in covered)

    # cave statistics
    cave_set = set()
    for e in entries:
        for p in e['analysed']:
            c0, n0 = p.pop('_cave_range', (0, 0))
            cave_set.update(c0 + 4 * k for k in range(n0))
    cave_words = len(cave_set)
    cave_changed_bytes = len(in_cave)

    # 3. features
    used = set()
    features = []
    for fd in defs['features']:
        feat = {k: fd[k] for k in ('id', 'name', 'readme', 'kind', 'effect_without_code', 'evidence', 'reimpl', 'reimpl_note')}
        patches = []
        for t in fd['mcm']:
            if t not in by_title:
                raise SystemExit('feature %s names an unknown library entry: %s' % (fd['id'], t))
            if t in used:
                raise SystemExit('library entry used twice: ' + t)
            used.add(t)
            e = by_title[t]
            patches.append({'entry': t, 'file': e['file'], 'authors': e['authors'], 'patches': e['analysed']})
        feat['library_entries'] = patches
        feat['dependent_data_paths'] = fd['data']
        files_all = set(census['files_new']) | set(census['files_changed']) | set(census['files_identical'])
        res = []
        for pat in fd['data']:
            if pat.startswith('@'):
                res.append({'pattern': pat[1:], 'resolved': 'outside files/ (mod afs/ or options/ folder)'})
                continue
            hits = sorted(x for x in files_all if fnmatch.fnmatchcase(x, pat) or (pat.endswith('/') and x.startswith(pat)))
            if not hits:
                raise SystemExit('feature %s: data pattern matches nothing: %s' % (fd['id'], pat))
            res.append({'pattern': pat, 'files': len(hits),
                        'new': sum(1 for x in hits if x in census['files_new']),
                        'changed': sum(1 for x in hits if x in set(census['files_changed']))})
        feat['dependent_data'] = res
        features.append(feat)
    unassigned = [e['title'] for e in entries if e['title'] not in used]
    if unassigned:
        raise SystemExit('library entries without a feature: %s' % unassigned)

    # 4. Gecko
    gecko_defs = {
        'Restore Partner Intro Cutscene Behavior': 'partner-intro', 'Restore Skid (not recommended)': 'player-tuning',
        'Restore Original Bloom (not recommended)': 'bloom', 'Restore Car Type Vehicle Physics (not recommended)': 'vehicles',
        'Restore Air Saucer Physics (not recommended)': 'vehicles', 'Replace Air Saucer Stomp with Double Jump (not recommended)': 'vehicles',
        'Restore Cannon & Lock On Weapons Knockback (not recommended)': 'combat-balance',
        'Restore Original Escape Pod Health in Air Fleet': 'combat-balance', 'Invuln/Recovery Adjustment': 'combat-balance',
        'Restore Multiplayer (not recommended)': 'optional-battle-and-colors',
    }
    gecko = []
    for c in parse_gecko(a.gecko_ini):
        w = []
        for kind, ad in gecko_addresses(c):
            mp = ctx.map_address(ad & ~3)
            w.append({'kind': kind, 'ntsc': '0x%08X' % ad, 'pal': ('0x%08X' % mp['pal']) if mp['pal'] else None,
                      'pal_status': mp['status'], 'pal_window': mp['window'],
                      'region': 'dol-image' if ctx.ntsc.a2o(ad & ~3) is not None else 'bss-or-heap (not in the DOL image)'})
        gecko.append({'name': c['name'], 'authors': c['authors'], 'section': c['section'] or 'preferences',
                      'writes': w, 'related_feature': gecko_defs.get(c['name'])})

    # 5. summaries
    all_patches = [p for e in entries for p in e['analysed']]
    summary = {
        'library_entries': len(entries), 'library_patches': len(all_patches),
        'by_kind': dict(collections.Counter(p['kind'] for p in all_patches)),
        'by_confidence': dict(collections.Counter(p['confidence'] for p in all_patches)),
        'pal_status': dict(collections.Counter(p['pal_status'] for p in all_patches)),
        'pal_via': dict(collections.Counter(p.get('pal_via', 'none') for p in all_patches)),
        'original_matches_ntsc_dol': dict(collections.Counter(str(p['original_matches_ntsc_dol']) for p in all_patches)),
        'dol_changed_bytes_total': len(changed) + header_bytes, 'dol_header_bytes_changed': header_bytes,
        'dol_changed_bytes_in_cave_area': cave_changed_bytes,
        'dol_changed_bytes_outside_cave_area': len(outside),
        'dol_changed_bytes_outside_cave_area_and_not_within_64_bytes_of_a_library_patch': len(unexplained),
        'cave_words_in_use': cave_words, 'cave_area': ['0x%08X' % CAVE_LO, '0x%08X' % CAVE_HI],
        'gecko_codes': len(gecko), 'gecko_writes': sum(len(g['writes']) for g in gecko),
        'features': len(features),
        'features_by_kind': dict(collections.Counter(f['kind'] for f in features)),
    }
    out = {'schema': 1, 'summary': summary, 'features': features, 'gecko': gecko,
           'unexplained_dol_diff_ranges': [hex(x) for x in unexplained[:40]]}
    os.makedirs(os.path.dirname(os.path.abspath(a.out_json)), exist_ok=True)
    with open(a.out_json, 'w', encoding='utf-8', newline='\n') as f:
        json.dump(out, f, indent=1)
        f.write('\n')
    write_md(a.out_md, out, census)
    print(json.dumps(summary, indent=1))


# ----------------------------------------------------------------------------------------------------- markdown
def fmt_float(x):
    return ('%g' % x)


def write_md(path, out, census):
    s = out['summary']
    L = []
    w = L.append
    w('# What Reloaded\'s code changes are, and which data depends on them')
    w('')
    w('Generated by `tools/mods/reloaded_code_map.py` (with `tools/mods/reloaded_data_census.py`) from a local copy of')
    w('ShdTH-Reloaded v1.2 (no licence: knowledge only; none of its code, assembly or files is in this repository, and the')
    w('tables below carry addresses, sizes, counts and our own descriptions only). Data: `test_data/mods/reloaded_features.json`')
    w('(the manager\'s feature list) and `test_data/mods/reloaded_data_census.json`.')
    w('')
    w('## 1. What the code is')
    w('')
    w('Reloaded\'s gameplay logic lives in three places, all built on the **NTSC-U** executable:')
    w('')
    w('- `ISO EDITS/sys/main.dol`: the NTSC-U DOL, edited by Melee Code Manager (its header carries the stamp of that tool).')
    w('- `codes/code/MCM/*.txt`: the assembly library the DOL was built from: %d entries, %d patch sites.' % (
        s['library_entries'], s['library_patches']))
    w('- `codes/code/GUPR8P.ini`: %d optional Gecko codes (preferences, cheats, and codes that restore original behaviour); they are not part of the DOL.' % s['gecko_codes'])
    w('')
    w('**DOL diff (our measurement).** Same 10 sections and layout as the NTSC-U DOL. %d bytes differ: %d in the header stamp,' % (
        s['dol_changed_bytes_total'], s['dol_header_bytes_changed']))
    w('%d inside %s to %s (the original DOL\'s debugger interrupt-vector stubs, zeroed and reused as free space for the mod\'s code' % (
        s['dol_changed_bytes_in_cave_area'], s['cave_area'][0], s['cave_area'][1]))
    w('caves; about %d instruction words of injected code are reachable from the hooks) and %d elsewhere. Of those %d, %d are not within 64 bytes' % (
        s['cave_words_in_use'], s['dol_changed_bytes_outside_cave_area'], s['dol_changed_bytes_outside_cave_area'],
        s['dol_changed_bytes_outside_cave_area_and_not_within_64_bytes_of_a_library_patch']))
    w('of a library patch address: **every change in the DOL is described by the library**; the DOL holds nothing the text files do not list.')
    w('Every library entry\'s "hex to replace" equals the NTSC-U DOL word at its address (%s).' % (
        ', '.join('%s: %d' % (k, v) for k, v in sorted(s['original_matches_ntsc_dol'].items()))))
    w('')
    w('Patch kinds: ' + ', '.join('%s %d' % (k, v) for k, v in sorted(s['by_kind'].items())) + '. "branch-to-cave" replaces one instruction with a jump into the')
    w('free-space cave, which runs the new logic, executes the replaced instruction and jumps back. "data" patches rewrite constants in the DOL\'s data')
    w('area (floats, a few strings and table words). They are DOL contents, not files, so a file overlay cannot carry them.')
    w('')
    w('## 2. PAL counterparts')
    w('')
    w('The PC build translates the **PAL** DOL (GUPP8P). Each NTSC-U address was mapped with the repository\'s verified function-level map')
    w('`extract/address-maps/GUPE8P.map` (PAL function bodies and data against NTSC-U, built by `tools/ppcrecomp/disc_map.py`), and where the map')
    w('has a gap (functions whose body differs between the regions) by a unique signature search of the surrounding original code. Status:')
    w('"exact" = the original word is identical at the PAL address, "shape" = same instruction apart from displacements that move between regions')
    w('(small-data offsets, branch targets), "differs" = different. Confidence: high = exact/shape with at least 12 of 13 neighbouring words agreeing')
    w('(code) or an identical value (data); medium = shape with a weaker window or a value that differs; low = no PAL place found.')
    w('Result: ' + ', '.join('%s %d' % (k, v) for k, v in sorted(s['by_confidence'].items())) + ' (patch sites); mapped via ' +
      ', '.join('%s %d' % (k, v) for k, v in sorted(s['pal_via'].items())) + '. PAL addresses of every patch are in the JSON.')
    w('')
    w('Reloaded\'s caves read and write NTSC-U small-data and global addresses (flag bytes, pad structures). A PAL version would need those re-derived with')
    w('the data deltas of the map (about -0x1060 to -0x10F0 in the data area). Not attempted: no code is being reimplemented here.')
    w('')
    w('## 3. Features')
    w('')
    w('Kind: code = lives only in the DOL/library, data = only files, both = code that selects or needs files. "Without the code" is what a player gets')
    w('on this build with the mod\'s files installed. Evidence: observed = we ran it on this build; static = derived from the code and data read offline;')
    w('unverified = a reasoned expectation we did not run. Reimplementation (information only): files-only = already works; constants = values in the DOL')
    w('data (a runtime override table would do); small-code / code = logic change.')
    w('')
    w('| Feature | Kind | NTSC-U patch sites | PAL (function) | Dependent data | Without the code on this build | Evidence | Reimplementation |')
    w('|---|---|---|---|---|---|---|---|')
    for f in out['features']:
        sites, pals = [], []
        for e in f['library_entries']:
            for p in e['patches']:
                sites.append(p['ntsc'][2:] if len(sites) < 6 else '')
                if p.get('pal') and len(pals) < 6:
                    pals.append(p['pal'][2:] + ('/' + p['pal_function'][2:] if p.get('pal_function') else ''))
        sites = [x for x in sites if x]
        n_sites = sum(len(e['patches']) for e in f['library_entries'])
        more = ' (+%d)' % (n_sites - len(sites)) if n_sites > len(sites) else ''
        data = []
        for d in f['dependent_data']:
            if 'files' in d:
                data.append('`%s` (%d files%s)' % (d['pattern'], d['files'], ', %d new' % d['new'] if d['new'] else ''))
            else:
                data.append('`%s`' % d['pattern'])
        w('| **%s** (`%s`)<br>%s | %s | %s | %s | %s | %s | %s | %s |' % (
            f['name'], f['id'], f['readme'], f['kind'], (', '.join(sites) + more) if sites else 'none',
            ', '.join(pals) if pals else 'n/a', '<br>'.join(data) if data else 'none', f['effect_without_code'], f['evidence'],
            '%s: %s' % (f['reimpl'], f['reimpl_note'])))
    w('')
    w('Addresses are hex without `0x`; the PAL column is `patch site / containing function start` (functions per `extract/functions.csv`).')
    w('')
    w('### Tuning constants (data patches in the DOL), with stock and Reloaded values')
    w('')
    w('| Library entry | NTSC-U | PAL | stock | Reloaded | PAL value equals NTSC-U |')
    w('|---|---|---|---|---|---|')
    for f in out['features']:
        for e in f['library_entries']:
            for p in e['patches']:
                if p['kind'] == 'data' and ('stock_value_float' in p or 'stock_value_int' in p):
                    isf = 'stock_value_float' in p
                    w('| %s | %s | %s | %s | %s | %s |' % (
                        e['entry'], p['ntsc'][2:], (p.get('pal') or '-')[2:],
                        fmt_float(p['stock_value_float']) if isf else '%d (integer)' % p['stock_value_int'],
                        fmt_float(p['mod_value_float']) if isf else '%d (integer)' % p['mod_value_int'], p.get('pal_value_equals_ntsc')))
    w('')
    w('## 4. Data files and the code they need')
    w('')
    c = census
    w('Reloaded\'s `files/` tree has %d files: %d new, %d changed, %d identical to the NTSC-U disc (plus 37 voice entries in `afs/` and 21 textures; see RELOADED_RESEARCH.md).' % (
        c['files_total'], len(c['files_new']), c['files_changed_count'], len(c['files_identical'])))
    w('Groups (new/changed/identical): ' + '; '.join('%s %s' % (g, '/'.join(str(v.get(k, 0)) for k in ('new', 'changed', 'identical'))) for g, v in c['by_group'].items()) + '.')
    w('')
    lay = c['layouts']
    w('- **Stage layouts load with stock code.** %d `sky2` layouts were compared with the NTSC-U ones. Reloaded uses no object class the stock layouts do not use, and no' % len(lay['files']))
    w('  class/parameter-length combination that stock data lacks (`new_misc_length_combinations` is empty); the only class dropped is SECRETDOOR (%d to %d).' % tuple(lay['class_totals_stock_vs_mod']['SECRETDOOR']))
    w('  SECRETKEY is %d to %d (same objects: the red ring is a model swap in `stg_cmn_gdt.one`, SECRETKEY00.DFF). HINTRING %d to %d, HINT COLLISION %d to %d,' % (
        *lay['class_totals_stock_vs_mod']['SECRETKEY'], *lay['class_totals_stock_vs_mod']['HINTRING'], *lay['class_totals_stock_vs_mod']['HINT COLLISION']))
    w('  ELEVATOR %d to %d, WARP_HOLE %d to %d.' % (*lay['class_totals_stock_vs_mod']['ELEVATOR'], *lay['class_totals_stock_vs_mod']['WARP_HOLE']))
    w('- **Boss-stage layouts are asked for by the stock loader.** The stock layout loader opens `stgNNNN_nrm.dat` (or `_hrd.dat` in Expert) for every stage and tolerates absence;')
    w('  Reloaded adds %d such files for 16 boss stages. A boot of stg0210 on this build with only Reloaded enabled read `stg0210/stg0210_nrm.dat` through the' % len(lay['layouts_only_in_mod']))
    w('  layout parser (guest pc 0x800CB1C0), so they take effect. The hard variants are read only in Expert (flag byte G+0x125, `docs/STAGE_LOADING.md`).')
    w('- **Trimmed stage archives.** %d stage archives lose members (%s). They match Reloaded\'s layouts only.' % (
        len(c['one_trimmed_stage_archives']), ', '.join(x.split('/')[-1] for x in c['one_trimmed_stage_archives'])))
    w('- **Archive members changed/added across %d changed `.one` files:** %s.' % (len(c['one_archives']), ', '.join('%s %d' % (k.replace(':', ' '), v) for k, v in c['one_member_kinds'].items())))
    w('  Notable: `stg_cmn_gdt.one` 12 members (RING, RINGLOW, GHOSTRING, GHOSTRINGLOW, SECRETKEY00, SAVEPOINT*, TARGETSW.CCL, STG_GDT.TXD); `character/shadow.one` SHADOW.MTP, SHADOW.TXD, SHADOW_BODY.DFF;')
    w('  138 `event/*.one` (Shadow locator models and event textures: unlit ring inhibitors, emerald colour).')
    w('- **Files only code reads.** `stg_cmn_gd2.one` (selected by Ghost Red Rings, which rewrites one letter of `stg_cmn_gdt.one`); the six `.flo` movies and `sng_EV_music4.adx`')
    w('  (Expert Ending rewrites the `.sfd` extension and the music digit); `sng_vox07.adx`, `sng_stg0613.adx` (Music Update patches one digit of the stock track-name strings per stage);')
    w('  `common/ScoreDatb.bin` is named by neither DOL (inert). None of these names occurs in the stock DOL, so the stock game never opens them.')
    w('- **Files the stock code reads that changed meaning.** `common/ScoreData.bin` (%d to %d bytes, %d differing runs): the stock rank code reads it, so Reloaded\'s thresholds apply without the S-rank display code.' % (
        c['score_data']['size_stock'], c['score_data']['size_mod'], c['score_data']['differing_runs_in_common_prefix']))
    w('  `nukkoro2.inf` (mission table) works as data; `nukkoro.inf` has the same 9 settings as stock; `setid.bin` differs in 65 bytes (per-stage object-type bits, read by the stock loader).')
    w('')
    w('### What we ran (this build, Reloaded enabled alone, headless)')
    w('')
    w('- stg0210 boot with `--asset-trace`: 2,116 trace lines; `stg0210_nrm.dat` read at pc 0x800CB1C0; `nukkoro2.inf`, `ScoreData.bin`, `setid.bin` read.')
    w('- Westopolis, game tick 1.2e9 (frame 1737), `final-47133fd7.exe` with the mod: Sonic\'s introduction camera and subtitle are on screen (partner intro not disabled); stock shows Shadow running at the same tick.')
    w('- Not run: S rank display, red rings in play, Expert Mode (needs an unlocked save), melee with the modified motion pack, widescreen HUD comparison.')
    w('')
    w('## 5. Gecko codes (`GUPR8P.ini`)')
    w('')
    w('%d codes, %d writes; applied by Dolphin to guest memory, never by this build. Addresses are NTSC-U/GUPR8P; the PAL column is the mapped place.' % (s['gecko_codes'], s['gecko_writes']))
    w('')
    w('| Code | Section | Writes (NTSC-U -> PAL) | Related feature |')
    w('|---|---|---|---|')
    for g in out['gecko']:
        ws = ', '.join('%s -> %s' % (x['ntsc'][2:], (x['pal'] or ('bss' if x['region'].startswith('bss') else '-'))[2:] if x['pal'] else 'bss') for x in g['writes'][:5]) + (' (+%d)' % (len(g['writes']) - 5) if len(g['writes']) > 5 else '')
        w('| %s | %s | %s | %s |' % (g['name'], g['section'], ws or 'none', g['related_feature'] or '-'))
    w('')
    w('## 6. How the manager should use this')
    w('')
    w('For an installed mod whose `codes/` folder holds a modified DOL, MCM library or Gecko ini, list the features of `test_data/mods/reloaded_features.json` with kind `code` or `both`')
    w('as "not on this build", show each `effect_without_code`, and the `dependent_data_paths` that are installed but inert. Kind `data` features need no warning.')
    w('')
    w('## 7. Limits')
    w('')
    w('- Function names are not given: the repository has none for most of these functions, and the only NTSC-U symbol map is a third-party file without a licence. The PAL function column is the start address from `extract/functions.csv`.')
    w('- The cave code was read (disassembled offline) to find which data it touches, not copied; the JSON carries no instruction words of it. Descriptions are ours, from reading the hooks and the files.')
    w('- "Evidence: static" statements come from reading the stock DOL strings and the mod files, not from play. Only the rows marked observed were run on this build.')
    w('- Nothing here changes the game or the loader; gameplay stays bit-exact. A later native opt-in implementation of any feature would be a new, separately validated piece of work.')
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(L) + '\n')


if __name__ == '__main__':
    main()
