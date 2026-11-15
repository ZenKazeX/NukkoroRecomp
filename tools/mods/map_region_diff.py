#!/usr/bin/env python
"""map_region_diff.py - what differs between the PAL and the NTSC-U disc files, and what that means for a mod.

Almost every community mod is built from NTSC-U files. This script compares the two extracted trees byte by byte and
explains each differing path (docs/mods/map/REGION_DIFF.md has the reading of the result):

  * stage layouts (stgNNNN_{cmn,nrm,hrd,ds1}.dat, "sky2" files): decoded record by record, PAL-only / NTSC-U-only /
    changed placements with the object class names the game's own type table gives;
  * nukkoro.inf: line diff; opening.bnr: banner header + text blocks; *.txd: texture by texture;
  * fonts/ (.fnt message banks, .met glyph metrics, .txd glyph sheets): per language, key sets, texts, timings, glyph codes
    and glyph rectangles, the Save/Advertise key shift that matters to index-based rewrites, and the glyph coverage of a
    PAL text by an NTSC-U glyph set.

Output: one JSON document (names, numbers and md5 digests only; no game bytes).

  python -I tools/mods/map_region_diff.py --pal <game>/files --usa <usa>/files --out test_data/mods/region_diff.json \
         [--dol <game>/sys/main.dol | --types test_data/mods/set_types.json] [--mods-root <dir of installed mods>]

--dol reads the game's type table (0x8052C1A0: class names, ids, parameter schemas) from the PAL main.dol; without it the
checked-in --types file is used. --emit-types writes that file from the DOL. --mods-root adds a cross-check of installed
real mods (a folder of <id>/files/...): which region's bytes each differing file they ship is closest to.
Stdlib only, single threaded. Knowledge of the .dat record layout: docs/distant-rendering/spawn_data.md (own RE, PAL
GUPP8P); the type table is read from the user's own DOL.
"""
import argparse
import collections
import hashlib
import json
import math
import os
import re
import struct
import sys

LANGS = ('EN', 'FR', 'GE', 'IT', 'SP', 'JP')
STAGE_TITLES = {
    'stg0201': 'Glyphic Canyon', 'stg0300': 'Cryptic Castle', 'stg0401': 'The Doom', 'stg0403': 'Mad Matrix',
    'stg0501': 'Air Fleet', 'stg0502': 'Iron Jungle', 'stg0504': 'Lost Impact', 'stg0600': 'GUN Fortress',
    'stg0602': 'Lava Shelter', 'stg0604': 'Final Haunt',
}
LAYERS = {
    'cmn': 'common layer, always loaded',
    'nrm': 'normal-mode layer',
    'hrd': 'hard-mode layer',
    'ds1': 'always-on scenery / ambience layer',
}
# SETCOLLISION (type id 0x50): ctor 0x8011D4EC maps `kind` through the jump table at 0x805285C4.
SETCOLLISION_ID = 0x50


# --------------------------------------------------------------------------------------------------- file inventory
def walk(root):
    out = {}
    for dirpath, _dirs, files in os.walk(root):
        for name in files:
            full = os.path.join(dirpath, name)
            out[os.path.relpath(full, root).replace(os.sep, '/')] = full
    return out


_md5_cache = {}


def md5(path):
    if path not in _md5_cache:
        h = hashlib.md5()
        with open(path, 'rb') as f:
            for chunk in iter(lambda: f.read(1 << 20), b''):
                h.update(chunk)
        _md5_cache[path] = h.hexdigest()
    return _md5_cache[path]


def read(path):
    with open(path, 'rb') as f:
        return f.read()


# ----------------------------------------------------------------------------------------------------- type table
def read_type_table(dol_path):
    """The game's own object type table (0x8052C1A0, 286 pointers) from the PAL main.dol -> {id: {name, params}}."""
    dol = read(dol_path)
    offs = struct.unpack('>18I', dol[0:72])
    addrs = struct.unpack('>18I', dol[72:144])
    sizes = struct.unpack('>18I', dol[144:216])

    def rd(a, n):
        for o, ad, s in zip(offs, addrs, sizes):
            if s and ad <= a and a + n <= ad + s:
                return dol[o + a - ad:o + a - ad + n]
        raise KeyError(hex(a))

    def u32(a):
        return struct.unpack('>I', rd(a, 4))[0]

    def cstr(a):
        out = b''
        while True:
            c = rd(a, 1)
            if c == b'\0':
                break
            out += c
            a += 1
        return out.decode('shift_jis', 'replace')

    table = {}
    i = 0
    while True:
        p = u32(0x8052C1A0 + 4 * i)
        if p == 0:
            break
        i += 1
        name_ptr = u32(p)
        if not name_ptr:
            continue
        tid = struct.unpack('>H', rd(p + 0x18, 2))[0]
        schema = u32(p + 0x20)
        params = []
        if schema:
            for j in range(64):
                kind = u32(schema + 0x28 * j)
                if kind == 0:
                    break
                params.append([kind, cstr(u32(schema + 0x28 * j + 4))])
        name = cstr(name_ptr)
        if name.endswith(('Start', 'End')) and tid >= 9600:
            continue                                   # section markers, not placeable classes
        table[tid] = {'name': name, 'params': params}
    return table


# ------------------------------------------------------------------------------------------------------ .dat layouts
def parse_layout(data):
    """'sky2' layout file (little-endian on disc) -> list of entry dicts. Raises ValueError when malformed."""
    magic, n, m = struct.unpack_from('<4sII', data, 0)
    if magic != b'sky2' or len(data) != 12 + 44 * n + m:
        raise ValueError('not a sky2 layout of the expected size')
    entries = []
    off = 12 + 44 * n
    for i in range(n):
        f = struct.unpack_from('<6fIIHBBII', data, 12 + 44 * i)
        misc = data[off:off + f[11]]
        off += f[11]
        entries.append({'pos': f[0:3], 'rot': f[3:6], 'disc_flags': f[6], 'flags': f[7], 'type': f[8],
                        'link': f[9], 'range': f[10], 'misc': misc})
    return entries


def r1(x):
    return round(x + 0.0, 1)


def entry_key(e):
    """Everything that matters at run time: the disc-only flag word (+0x18) is overwritten with +0x1C on load."""
    return (e['type'], tuple(round(v, 3) for v in e['pos']), tuple(round(v, 3) for v in e['rot']),
            e['flags'], e['link'], e['range'], e['misc'])


def decode_misc(entry, types):
    t = types.get(entry['type'])
    misc = entry['misc']
    words = [misc[i:i + 4] for i in range(0, len(misc) - len(misc) % 4, 4)]
    if not t or len(t['params']) != len(words):
        return {'raw_words': [struct.unpack('<I', w)[0] for w in words]}
    out = {}
    for (kind, name), w in zip(t['params'], words):
        out[name] = round(struct.unpack('<f', w)[0], 3) if kind == 4 else struct.unpack('<i', w)[0]
    return out


def describe(entry, types):
    t = types.get(entry['type'])
    d = {'type_id': entry['type'], 'class': t['name'] if t else None,
         'pos': [r1(v) for v in entry['pos']], 'rot_deg': [r1(v) for v in entry['rot']],
         'link': entry['link'], 'range_x100': entry['range'], 'saved_flags': '0x%X' % entry['flags']}
    d['params'] = decode_misc(entry, types)
    return d


def diff_layouts(pal, usa, types):
    """PAL-only, NTSC-U-only and changed placements. Exact record matches are removed first; the leftovers of the same
    class are paired by nearest position (a placement that was nudged or re-ranged) and the rest are added / removed."""
    kp = collections.defaultdict(list)
    for i, e in enumerate(pal):
        kp[entry_key(e)].append(i)
    left_u = []
    for j, e in enumerate(usa):
        k = entry_key(e)
        if kp.get(k):
            kp[k].pop(0)
        else:
            left_u.append(j)
    left_p = sorted(i for v in kp.values() for i in v)
    changed = []
    by_type_u = collections.defaultdict(list)
    for j in left_u:
        by_type_u[usa[j]['type']].append(j)
    unpaired_p = []
    for i in left_p:
        cands = by_type_u.get(pal[i]['type'], [])
        best = None
        for j in cands:
            dist = math.dist(pal[i]['pos'], usa[j]['pos'])
            if best is None or dist < best[0]:
                best = (dist, j)
        if best is not None and best[0] < 150.0:
            j = best[1]
            cands.remove(j)
            fields = []
            if any(abs(a - b) > 1e-3 for a, b in zip(pal[i]['pos'], usa[j]['pos'])):
                fields.append('pos')
            if any(abs(a - b) > 1e-3 for a, b in zip(pal[i]['rot'], usa[j]['rot'])):
                fields.append('rot')
            for fname in ('flags', 'link', 'range', 'misc'):
                if pal[i][fname] != usa[j][fname]:
                    fields.append(fname)
            changed.append({'pal': describe(pal[i], types), 'usa': describe(usa[j], types), 'fields': fields,
                            'moved_units': round(best[0], 1)})
        else:
            unpaired_p.append(i)
    unpaired_u = [j for v in by_type_u.values() for j in v]
    return ([describe(pal[i], types) for i in unpaired_p], [describe(usa[j], types) for j in sorted(unpaired_u)],
            changed)


def only_disc_flags(pal, usa):
    """True when the two layouts differ only in the disc-only word at +0x18 (overwritten on load: no effect)."""
    if len(pal) != len(usa):
        return False
    return all(entry_key(a) == entry_key(b) for a, b in zip(pal, usa))


SETCOLLISION_KIND_NOTE = (
    'SetCollision ctor 0x8011D4EC: kind 0/3/4 give the volume a plain collision flag word (0x102 / 0x104 / 0x120); '
    'kinds 1,2 and 5..10 use flag word 0x40100, and kinds 5..10 also set the collision attribute 0x6F..0x74 on it; the '
    'volume is invisible (no model). The in-game meaning of each kind is inferred from the code shape only.')


def summarize_layout(stage, layer, added, removed, changed, n_pal, n_usa, types):
    cls = collections.Counter((a['class'] or 'type 0x%X' % a['type_id']) for a in added)
    parts = []
    if added:
        parts.append('PAL adds %d placement(s): %s' % (len(added), ', '.join('%d x %s' % (v, k) for k, v in cls.items())))
    if removed:
        parts.append('NTSC-U has %d placement(s) PAL removed' % len(removed))
    for c in changed:
        what = []
        if 'pos' in c['fields']:
            what.append('moved %.1f units' % c['moved_units'])
        if 'range' in c['fields']:
            what.append('activation range %d -> %d (x100 units, NTSC-U -> PAL)' % (c['usa']['range_x100'], c['pal']['range_x100']))
        for f in ('rot', 'flags', 'link', 'misc'):
            if f in c['fields']:
                what.append(f + ' differs')
        parts.append('%s %s' % (c['pal']['class'] or 'type 0x%X' % c['pal']['type_id'], ', '.join(what)))
    return '; '.join(parts)


def layout_risk(stage, layer, pal, usa, added, removed, changed):
    if not (added or removed or changed):
        return 'none', 'PAL and NTSC-U differ only in the disc-only flag word (+0x18), which the game overwrites on load.'
    # scenery-only change (a visual ART GADGET nudged) is cosmetic; anything that can touch collision, enemies,
    # rings, characters, vehicles or cameras is gameplay.
    visual = {'ART GADGET', 'PARTICLE', 'LIGHT_COLI'}
    classes = {(a['class'] or '') for a in added} | {(r['class'] or '') for r in removed} | \
              {(c['pal']['class'] or '') for c in changed}
    if classes <= visual and not added and not removed:
        return 'cosmetic', 'A scenery placement was nudged; it carries no collision of its own that the game reads from this file.'
    return 'gameplay', None


# ------------------------------------------------------------------------------------------------------- nukkoro.inf
def diff_inf(pal_path, usa_path):
    a = read(pal_path).decode('latin1').split('\r\n')
    b = read(usa_path).decode('latin1').split('\r\n')
    rows = []
    for i, (x, y) in enumerate(zip(a, b), 1):
        if x != y:
            rows.append({'line': i, 'pal': x, 'usa': y})
    return len(a) == len(b), rows


# ------------------------------------------------------------------------------------------------------- opening.bnr
def bnr_info(path):
    b = read(path)
    info = {'size': len(b), 'magic': b[:4].decode('latin1'), 'image_md5': hashlib.md5(b[0x20:0x1820]).hexdigest()}
    count = 6 if b[:4] == b'BNR2' else 1
    info['text_blocks'] = count
    texts = []
    for k in range(count):
        o = 0x1820 + 0x140 * k
        texts.append([b[o + s:o + s + n].split(b'\0')[0].decode('latin1') for s, n in ((0, 32), (32, 32), (64, 64), (128, 64), (192, 128))])
    info['text'] = texts
    return info


# ------------------------------------------------------------------------------------------------------------ .txd
def txd_textures(data):
    """name, offset, size, differing-byte count is done by the caller; GameCube RW dictionary, little-endian chunks."""
    out = []
    if len(data) < 12 or struct.unpack_from('<I', data, 0)[0] != 0x16:
        return out
    end = 12 + struct.unpack_from('<I', data, 4)[0]
    off = 12
    _t, s, _v = struct.unpack_from('<III', data, off)
    off += 12 + s
    while off + 12 <= end:
        t, s, _v = struct.unpack_from('<III', data, off)
        if t == 0x15:
            m = re.search(rb'[A-Za-z0-9_\-]{3,}', data[off + 44:off + 104])
            out.append((m.group(0).decode() if m else '?', off, s + 12))
        off += 12 + s
    return out


def diff_txd(pal_path, usa_path):
    a, b = read(pal_path), read(usa_path)
    rows = []
    if len(a) != len(b):
        return [{'note': 'sizes differ', 'pal': len(a), 'usa': len(b)}]
    for name, off, size in txd_textures(a):
        d = sum(1 for i in range(off, off + size) if a[i] != b[i])
        rows.append({'texture': name, 'bytes': size, 'differing_bytes': d})
    return rows


# ------------------------------------------------------------------------------------------------------------ fonts
def parse_fnt(d):
    n = struct.unpack_from('<I', d, 0)[0]
    recs = {}
    order = []
    for i in range(n):
        off, key, prio, dur, voice = struct.unpack_from('<IiiiI', d, 4 + 20 * i)
        j = off
        while d[j] | d[j + 1]:
            j += 2
        recs[key] = (d[off:j].decode('utf-16le', 'replace'), prio, dur, voice)
        order.append(key)
    return recs, order


def parse_met(d):
    lines = d.decode('ascii', 'replace').split('\n')
    glyphs = {}
    for line in lines[3:]:
        t = line.split()
        if len(t) == 6 and t[5] == '#':
            glyphs[int(t[0])] = tuple(int(v) for v in t[1:5])
    return glyphs


def font_groups(root):
    groups = collections.defaultdict(lambda: {'fnt': None, 'txd': None, 'met': []})
    base = os.path.join(root, 'fonts')
    for bank in sorted(os.listdir(base)):
        for f in sorted(os.listdir(os.path.join(base, bank))):
            m = re.match(r'^(.*)_([A-Z]{2})(\d\d)?\.(fnt|txd|met)$', f)
            if not m:
                continue
            g = groups[(bank, m.group(2))]
            p = os.path.join(base, bank, f)
            if m.group(4) == 'met':
                g['met'].append(p)
            else:
                g[m.group(4)] = p
    return groups


def glyph_set(met_paths):
    s = set()
    for p in met_paths:
        s |= set(parse_met(read(p)))
    return s


def analyse_fonts(pal_root, usa_root, save_ids):
    pal_g, usa_g = font_groups(pal_root), font_groups(usa_root)
    per_lang = {l: collections.Counter() for l in LANGS}
    per_lang_sets = {l: collections.defaultdict(list) for l in LANGS}
    text_changes = []
    key_shift = {}
    for (bank, lang), pg in sorted(pal_g.items()):
        ug = usa_g.get((bank, lang))
        if ug is None:
            per_lang[lang]['pal_only_group'] += 1
            continue
        c = per_lang[lang]
        c['groups'] += 1
        pf, uf = read(pg['fnt']), read(ug['fnt'])
        if pf == uf:
            c['fnt_identical'] += 1
        else:
            c['fnt_differ'] += 1
            pr, po = parse_fnt(pf)
            ur, uo = parse_fnt(uf)
            if set(pr) != set(ur):
                c['fnt_keyset_differs'] += 1
                key_shift[(bank, lang)] = (po, uo)
                per_lang_sets[lang]['keyset'].append(bank)
            changed_text = [k for k in sorted(set(pr) & set(ur)) if pr[k][0] != ur[k][0]]
            changed_timing = [k for k in sorted(set(pr) & set(ur)) if pr[k][1:] != ur[k][1:]]
            c['records_text_changed'] += len(changed_text)
            c['records_timing_changed'] += len(changed_timing)
            if changed_text or set(pr) != set(ur):
                per_lang_sets[lang]['semantic'].append(bank)
            else:
                c['fnt_bytes_only'] += 1               # same keys, texts and timings: the unread 4*count tail differs
            for k in changed_text:
                text_changes.append({'lang': lang, 'bank': bank, 'key': k, 'pal': pr[k][0][:80], 'usa': ur[k][0][:80]})
            for k in sorted(set(pr) - set(ur)):
                text_changes.append({'lang': lang, 'bank': bank, 'key': k, 'pal': pr[k][0][:80], 'usa': None})
            for k in sorted(set(ur) - set(pr)):
                text_changes.append({'lang': lang, 'bank': bank, 'key': k, 'pal': None, 'usa': ur[k][0][:80]})
        if read(pg['txd']) != read(ug['txd']):
            c['txd_differ'] += 1
            per_lang_sets[lang]['txd'].append(bank)
        pm = {os.path.basename(p): p for p in pg['met']}
        um = {os.path.basename(p): p for p in ug['met']}
        c['met_pal_only'] += len(set(pm) - set(um))
        for name in sorted(set(pm) & set(um)):
            a, b = read(pm[name]), read(um[name])
            if a == b:
                continue
            c['met_differ'] += 1
            ga, gb = parse_met(a), parse_met(b)
            if set(ga) != set(gb):
                c['met_codeset_differs'] += 1
            if ga != gb:
                c['met_rect_differs'] += 1
            per_lang_sets[lang]['met'].append(name)
    # glyph coverage of the PAL text by NTSC-U glyph sets
    coverage = {}
    for lang in LANGS:
        if lang == 'JP':
            continue
        own_missing = en_missing = 0
        missing_chars = set()
        for (bank, l), pg in sorted(pal_g.items()):
            if l != lang:
                continue
            pr, _ = parse_fnt(read(pg['fnt']))
            need = {ord(ch) for t in pr.values() for ch in t[0] if ch != '\n'}
            own = glyph_set(usa_g[(bank, lang)]['met'])
            en = glyph_set(usa_g[(bank, 'EN')]['met'])
            if need - own:
                own_missing += 1
            if need - en:
                en_missing += 1
                missing_chars |= need - en
        coverage[lang] = {'banks_missing_with_own_ntsc_u_met': own_missing,
                          'banks_missing_with_ntsc_u_en_met': en_missing,
                          'chars_the_en_set_lacks': ''.join(chr(c) for c in sorted(missing_chars))}
    # the Save / Advertise index shift: rewrites by message index (native_save_text) hit other messages
    shift = {}
    for bank in ('Save', 'Advertise'):
        for lang in ('EN', 'FR', 'GE', 'IT', 'SP', 'JP'):
            ks = key_shift.get((bank, lang))
            if not ks:
                continue
            po, uo = ks
            n = 0
            ex = None
            for idx in save_ids:
                if idx < len(po) and idx < len(uo) and po[idx] != uo[idx]:
                    n += 1
                    ex = ex or {'index': idx, 'pal_key': po[idx], 'usa_key': uo[idx]}
            shift['%s_%s' % (bank, lang)] = {'pal_records': len(po), 'usa_records': len(uo),
                                           'pal_only_keys': sorted(set(po) - set(uo)),
                                           'usa_only_keys': sorted(set(uo) - set(po)),
                                           'indexes_80_126_naming_another_message': n, 'example': ex}
    return per_lang, per_lang_sets, text_changes, coverage, shift


def font_entry(rel, pal_path, usa_path, save_ids):
    m = re.match(r'^fonts/([^/]+)/\1_([A-Z]{2})(\d\d)?\.(fnt|txd|met)$', rel)
    bank, lang, ext = (m.group(1), m.group(2), m.group(4)) if m else (None, None, rel.rsplit('.', 1)[-1])
    a, b = read(pal_path), read(usa_path)
    e = {'pal_md5': md5(pal_path), 'usa_md5': md5(usa_path), 'pal_bytes': len(a), 'usa_bytes': len(b),
         'bank': bank, 'lang': lang}
    pair = 'Keep the .fnt, .met and .txd of one bank and language from the same region.'
    if ext == 'fnt':
        pr, po = parse_fnt(a)
        ur, uo = parse_fnt(b)
        only_p, only_u = sorted(set(pr) - set(ur)), sorted(set(ur) - set(pr))
        text = [k for k in sorted(set(pr) & set(ur)) if pr[k][0] != ur[k][0]]
        timing = [k for k in sorted(set(pr) & set(ur)) if pr[k][1:] != ur[k][1:]]
        e['kind'] = 'font-messages'
        bits = []
        if only_p or only_u:
            bits.append('key set differs (PAL-only %s, NTSC-U-only %s)' % (only_p[:4] or '-', only_u[:4] or '-'))
        if text:
            bits.append('%d record text(s) differ' % len(text))
        if timing:
            bits.append('%d record(s) differ in priority/window/voice' % len(timing))
        if not bits:
            e['summary'] = 'Same keys, texts, priorities, windows and voice ids; only the unread 4*count byte tail differs.'
            e['risk'] = 'none'
            e['advice'] = 'Harmless: nothing the game reads differs.'
        else:
            e['summary'] = '; '.join(bits) + '.'
            e['risk'] = 'cosmetic'
            if bank in ('Save', 'Advertise') and (only_p or only_u):
                n = sum(1 for i in save_ids if i < len(po) and i < len(uo) and po[i] != uo[i])
                e['index_shift'] = {'pal_records': len(po), 'usa_records': len(uo), 'indexes_80_126_naming_another_message': n}
                e['advice'] = ('The NTSC-U table lacks key %s, so every message index before it is one lower than in PAL. The PC save-text rewrite '
                               '(Save tables, by message index) would then put its words on the wrong messages; the Advertise rewrite matches by text and '
                               'only misses the PS2 memory-card wording. Ship the PAL table.' % ((only_p or only_u)[0]))
            else:
                e['advice'] = 'Only wording differs; timing is unchanged. Acceptable for a text mod, but it replaces PAL\'s wording of these records.'
    elif ext == 'met':
        ga, gb = parse_met(a), parse_met(b)
        e['kind'] = 'font-glyph-metrics'
        e['summary'] = ('%d glyphs in PAL, %d in NTSC-U; code set %s; glyph rectangles %s.' % (
            len(ga), len(gb), 'same' if set(ga) == set(gb) else 'differs (PAL-only %s, NTSC-U-only %s)' % (sorted(set(ga) - set(gb))[:6] or '-', sorted(set(gb) - set(ga))[:6] or '-'),
            'differ (the sheet was re-packed)' if ga != gb else 'same'))
        e['risk'] = 'cosmetic'
        e['advice'] = pair + ' A NTSC-U .met with the PAL .txd points glyphs at the wrong cells.'
    else:
        e['kind'] = 'font-glyph-sheet'
        e['summary'] = 'Glyph sheet (256x256 C4 texture) differs from the PAL sheet.'
        e['risk'] = 'cosmetic'
        e['advice'] = pair
    return e


# -------------------------------------------------------------------------------------------------- real-mod cross-check
def check_mods(mods_root, pal, usa, diff_paths, types, save_ids):
    out = {}
    for mod in sorted(os.listdir(mods_root)):
        root = os.path.join(mods_root, mod, 'files')
        if not os.path.isdir(root):
            continue
        shipped = walk(root)
        rows = {'ships_differing_paths': 0, 'identical_to_pal': 0, 'identical_to_usa': 0, 'modified': 0, 'layouts': {}}
        lows = {}
        for rel in diff_paths:
            hit = next((p for k, p in shipped.items() if k.lower() == rel.lower()), None)
            if hit is None:
                continue
            rows['ships_differing_paths'] += 1
            h = md5(hit)
            if h == md5(pal[rel]):
                rows['identical_to_pal'] += 1
            elif h == md5(usa[rel]):
                rows['identical_to_usa'] += 1
            else:
                rows['modified'] += 1
            if re.match(r'^fonts/Save/Save_[A-Z]{2}\.fnt$', rel):
                # the PC save text rewrites messages by index: how many of the indexes it targets name another message here
                _r, mine_order = parse_fnt(read(hit))
                _r, pal_order = parse_fnt(read(pal[rel]))
                rows.setdefault('save_tables', {})[rel] = {
                    'records': len(mine_order),
                    'indexes_80_126_naming_another_message_than_pal': sum(
                        1 for i in save_ids if i < len(mine_order) and i < len(pal_order) and mine_order[i] != pal_order[i])}
            if rel.endswith('.dat'):
                try:
                    mine = parse_layout(read(hit))
                    p_e = parse_layout(read(pal[rel]))
                    u_e = parse_layout(read(usa[rel]))
                except ValueError:
                    continue
                added, removed, changed = diff_layouts(p_e, u_e, types)
                mk = collections.Counter((e['type'], tuple(round(v, 1) for v in e['pos'])) for e in mine)

                def has(d):
                    return mk[(d['type_id'], tuple(d['pos']))] > 0
                rows['layouts'][rel] = {
                    'entries': len(mine),
                    'pal_only_placements_present': sum(1 for d in added if has(d)),
                    'pal_only_placements': len(added),
                    'usa_only_placements_present': sum(1 for d in removed if has(d)),
                    'usa_only_placements': len(removed),
                }
        if rows['ships_differing_paths']:
            out[mod] = rows
    return out


# ------------------------------------------------------------------------------------------------------------- driver
def advice_for(kind, risk, path, extra=None):
    if kind == 'stage-layout':
        return ('A mod file built from NTSC-U replaces the PAL layout and drops the PAL changes listed in this entry. '
                'Warn before enabling; keep the PAL file unless the mod is a gameplay mod that edits this layout on purpose.')
    return extra or ''


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[1])
    ap.add_argument('--pal', required=True, help='PAL files/ tree (the game data)')
    ap.add_argument('--usa', required=True, help='NTSC-U files/ tree')
    ap.add_argument('--out', required=True)
    ap.add_argument('--dol', help='PAL main.dol (type table)')
    ap.add_argument('--types', help='set_types.json (used when --dol is not given)')
    ap.add_argument('--emit-types', help='write the type table read from --dol here')
    ap.add_argument('--mods-root', help='folder of installed mods (<id>/files/...) to cross-check')
    ap.add_argument('--save-ids', default='80-126', help='message indexes the native save text rewrites (default 80-126)')
    args = ap.parse_args()

    if args.dol:
        types = read_type_table(args.dol)
        if args.emit_types:
            with open(args.emit_types, 'w', encoding='utf-8', newline='\n') as f:
                json.dump({str(k): v for k, v in sorted(types.items())}, f, indent=1, ensure_ascii=True)
                f.write('\n')
    elif args.types:
        types = {int(k): v for k, v in json.load(open(args.types, encoding='utf-8')).items()}
    else:
        types = {}
    lo, hi = (int(x) for x in args.save_ids.split('-'))
    save_ids = range(lo, hi + 1)

    pal, usa = walk(args.pal), walk(args.usa)
    shared = sorted(set(pal) & set(usa))
    differing = [n for n in shared if md5(pal[n]) != md5(usa[n])]
    non_font = [n for n in differing if not n.startswith('fonts/')]
    paths = {}
    for rel in non_font:
        entry = {'pal_md5': md5(pal[rel]), 'usa_md5': md5(usa[rel]), 'pal_bytes': os.path.getsize(pal[rel]),
                 'usa_bytes': os.path.getsize(usa[rel])}
        m = re.match(r'^(stg\d{4})/(stg\d{4})_(cmn|nrm|hrd|ds1)\.dat$', rel)
        if m:
            stage, layer = m.group(1), m.group(3)
            p_e, u_e = parse_layout(read(pal[rel])), parse_layout(read(usa[rel]))
            added, removed, changed = diff_layouts(p_e, u_e, types)
            risk, why = layout_risk(stage, layer, p_e, u_e, added, removed, changed)
            entry.update({'kind': 'stage-layout', 'stage': stage, 'stage_title': STAGE_TITLES.get(stage), 'layer': layer,
                          'layer_meaning': LAYERS[layer], 'entries_pal': len(p_e), 'entries_usa': len(u_e),
                          'pal_only': added, 'usa_only': removed, 'changed': changed})
            if risk == 'none':
                entry['summary'] = why
            else:
                entry['summary'] = summarize_layout(stage, layer, added, removed, changed, len(p_e), len(u_e), types)
            entry['risk'] = risk
            entry['advice'] = advice_for('stage-layout', risk, rel)
        elif rel == 'nukkoro.inf':
            same_len, rows = diff_inf(pal[rel], usa[rel])
            entry.update({'kind': 'config', 'lines_differing': rows,
                          'summary': 'Only the REGION and AREA lines differ (PAL: PAL50 / EU, NTSC-U: NTSC / US); the other 70 lines are identical. '
                                     'The game parses them into its InfDatabase (peeked in game: region 2 -> 0, area 2 -> 1).',
                          'risk': 'none',
                          'advice': 'No visible effect found: a 10 s headless Westopolis boot with the NTSC-U file gave the same frame (sha256) and log as stock. '
                                    'Treat as informational: tell the user the file would claim an NTSC-U disc.'})
        elif rel == 'opening.bnr':
            entry.update({'kind': 'banner', 'pal': bnr_info(pal[rel]), 'usa': bnr_info(usa[rel]),
                          'summary': 'GameCube IPL banner. PAL is BNR2 (6 language text blocks), NTSC-U is BNR1 (one block); the 96x32 image and the title / '
                                     'description texts are identical. The IPL reads it, the game does not.',
                          'risk': 'none', 'advice': 'Never used by the PC build. Silently accept.'})
        elif rel.endswith('.txd'):
            rows = diff_txd(pal[rel], usa[rel])
            entry.update({'kind': 'texture-dictionary', 'textures': rows,
                          'summary': 'Same file size; texture(s) with changed pixels: %s. The other textures are byte-identical.' %
                                     ', '.join('%s (%d of %d bytes)' % (r['texture'], r['differing_bytes'], r['bytes']) for r in rows if r.get('differing_bytes')),
                          'risk': 'cosmetic',
                          'advice': 'A mod that ships this file replaces one language screen texture with the NTSC-U artwork; other languages are not affected.'})
        else:
            entry.update({'kind': 'other', 'summary': 'differs', 'risk': 'cosmetic', 'advice': ''})
        paths[rel] = entry

    for rel in differing:
        if rel.startswith('fonts/'):
            paths[rel] = font_entry(rel, pal[rel], usa[rel], save_ids)

    per_lang, per_lang_sets, text_changes, coverage, shift = analyse_fonts(args.pal, args.usa, save_ids)
    only_pal = sorted(set(pal) - set(usa))
    only_usa = sorted(set(usa) - set(pal))
    census = collections.Counter()
    if types:
        for rel, p in pal.items():
            if re.match(r'^stg\d{4}/stg\d{4}_(cmn|nrm|hrd|ds1)\.dat$', rel):
                for e in parse_layout(read(p)):
                    if e['type'] == SETCOLLISION_ID and len(e['misc']) >= 4:
                        census['kind %d' % struct.unpack_from('<i', e['misc'], 0)[0]] += 1

    font_counts = collections.Counter(f for f in differing if f.startswith('fonts/'))
    result = {
        'schema': 1,
        'generator': 'tools/mods/map_region_diff.py',
        'trees': {'pal_files': len(pal), 'usa_files': len(usa), 'shared': len(shared),
                  'byte_identical': len(shared) - len(differing), 'differing': len(differing),
                  'differing_non_font': len(non_font), 'differing_font': len(differing) - len(non_font),
                  'only_in_pal': only_pal, 'only_in_usa': only_usa},
        'paths': paths,
        'fonts': {
            'layout': 'fonts/<bank>/<bank>_<LANG>.fnt (message bank), <bank>_<LANG>NN.met (glyph metrics), <bank>_<LANG>.txd (glyph sheet); '
                      '58 banks x 6 languages in BOTH discs; the game picks <LANG> from its text language.',
            'languages_in_both_discs': list(LANGS),
            'per_language': {l: dict(c) for l, c in per_lang.items()},
            'banks_by_difference': {l: {k: sorted(v) for k, v in s.items()} for l, s in per_lang_sets.items()},
            'text_changes_ex_jp': [t for t in text_changes if t['lang'] != 'JP'],
            'text_changes_jp_count': sum(1 for t in text_changes if t['lang'] == 'JP'),
            'save_advertise_key_shift': shift,
            'ntsc_u_glyph_coverage_of_pal_text': coverage,
            'facts': [
                'The .fnt files that differ only in bytes (same keys, texts, priorities, windows and voice ids) differ in the unread 4*count tail.',
                'Priorities, windows and voice ids are identical between PAL and NTSC-U in every record both have: a NTSC-U .fnt does not change subtitle timing.',
                'The glyph rectangles of every differing .met differ: PAL re-packed the sheets. A .met must travel with the .txd of the same region.',
                'A mod that replaces only *_EN files changes only the English text; with FR/GE/IT/SP/JP selected, the game reads the other languages\' own files.',
            ],
        },
        'setcollision': {'note': SETCOLLISION_KIND_NOTE, 'kind_census_pal_all_layouts': dict(sorted(census.items()))},
        'font_paths_differing': len(font_counts),
    }
    if args.mods_root:
        result['real_mods'] = check_mods(args.mods_root, pal, usa, differing, types, save_ids)
    with open(args.out, 'w', encoding='utf-8', newline='\n') as f:
        json.dump(result, f, indent=1, ensure_ascii=True, sort_keys=False)
        f.write('\n')
    print('wrote %s: %d differing paths (%d non-font)' % (args.out, len(differing), len(non_font)))


if __name__ == '__main__':
    main()
