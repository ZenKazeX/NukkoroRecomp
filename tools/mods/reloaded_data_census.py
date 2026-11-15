#!/usr/bin/env python3
"""reloaded_data_census.py - what the data files of a Reloaded-style mod change against the NTSC-U disc.

Compares a mod's `files/` tree with the extracted NTSC-U disc `files/` tree and writes a small JSON of names and
numbers (no game bytes): new / changed / identical files by group, the object census of the stage layouts
(`stgNNNN_cmn|nrm|hrd|ds1.dat`, magic `sky2`, 0x2C-byte records, class id at +0x20), the member-level difference of
the changed `.one` archives, and a few single-file facts used by docs/mods/map/RELOADED_CODE.md.

Stdlib only. The `.one` reader (ONE container + PRS) is the repository's own tools/re/assets_common.py, found next to
this script (../re); pass --repo-tools to point somewhere else.

  python -I reloaded_data_census.py --mod-files <mod>/files --ntsc-files <disc>/files \
      --object-types test_data/stage_loading/object_types.txt --out test_data/mods/reloaded_data_census.json
"""
import argparse, collections, hashlib, json, os, re, struct, sys


def walk(root):
    out = {}
    for dp, dn, fn in os.walk(root):
        for f in fn:
            ap = os.path.join(dp, f)
            out[os.path.relpath(ap, root).replace(os.sep, '/')] = ap
    return out


def read(p):
    with open(p, 'rb') as f:
        return f.read()


def object_names(path):
    names = {}
    for line in open(path, encoding='utf-8', errors='replace'):
        m = re.search(r'id=0x([0-9A-Fa-f]+) .*name="([^"]*)"', line)
        if m:
            names[int(m.group(1), 16)] = m.group(2)
    return names


def parse_layout(data):
    """[(class id, link group, range, misc length)] or None when the file is not a `sky2` layout."""
    if data[:4] != b'sky2':
        return None
    n = struct.unpack('<I', data[4:8])[0]
    recs = []
    for i in range(n):
        o = 0xC + i * 0x2C
        if o + 0x2C > len(data):
            break
        cid = struct.unpack('<H', data[o + 0x20:o + 0x22])[0]
        recs.append((cid, data[o + 0x22], data[o + 0x23], struct.unpack('<I', data[o + 0x24:o + 0x28])[0]))
    return recs


def top_group(rel):
    if '/' not in rel:
        ext = rel.rsplit('.', 1)[-1].lower() if '.' in rel else ''
        return 'root.' + ext
    top = rel.split('/')[0]
    return 'stage' if re.fullmatch(r'stg\d{4}', top) else top


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--mod-files', required=True)
    ap.add_argument('--ntsc-files', required=True)
    ap.add_argument('--object-types', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--repo-tools', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 're'))
    args = ap.parse_args()

    sys.path.insert(0, args.repo_tools)
    from assets_common import one_members  # noqa: E402

    names = object_names(args.object_types)
    mod = walk(args.mod_files)
    stock = walk(args.ntsc_files)
    stock_lc = {k.lower(): k for k in stock}

    status = {}
    for rel, p in mod.items():
        s = stock_lc.get(rel.lower())
        if s is None:
            status[rel] = 'new'
        elif read(p) == read(stock[s]):
            status[rel] = 'identical'
        else:
            status[rel] = 'changed'

    groups = collections.defaultdict(lambda: collections.Counter())
    for rel, st in status.items():
        groups[top_group(rel)][st] += 1

    census = {
        'schema': 1,
        'files_total': len(mod),
        'files_new': sorted(r for r, s in status.items() if s == 'new'),
        'files_identical': sorted(r for r, s in status.items() if s == 'identical'),
        'files_changed': sorted(r for r, s in status.items() if s == 'changed'),
        'files_changed_count': sum(1 for s in status.values() if s == 'changed'),
        'by_group': {g: dict(c) for g, c in sorted(groups.items())},
    }

    # stage layouts: object census, stock vs mod
    tot_stock = collections.Counter()
    tot_mod = collections.Counter()
    unknown_misc = []
    stock_misc = collections.defaultdict(set)
    mod_misc = collections.defaultdict(set)
    layouts = {}
    for rel in sorted(mod):
        if not re.fullmatch(r'stg\d{4}/stg\d{4}_(cmn|nrm|hrd|ds1)\.dat', rel):
            continue
        a = parse_layout(read(mod[rel]))
        s = stock_lc.get(rel.lower())
        b = parse_layout(read(stock[s])) if s else None
        if a is None:
            continue
        for c, g, r, m in a:
            tot_mod[c] += 1
            mod_misc[c].add(m)
        if b:
            for c, g, r, m in b:
                tot_stock[c] += 1
                stock_misc[c].add(m)
        diff = collections.Counter()
        for c, g, r, m in a:
            diff[c] += 1
        if b:
            for c, g, r, m in b:
                diff[c] -= 1
        layouts[rel] = {'status': status[rel], 'objects_mod': len(a), 'objects_stock': len(b) if b else 0,
                        'delta_by_class': {names.get(c, hex(c)): v for c, v in sorted(diff.items()) if v}}
    # layouts of the stock disc that the mod does not carry still count for "classes only in stock"
    for rel, p in stock.items():
        if re.fullmatch(r'stg\d{4}/stg\d{4}_(cmn|nrm|hrd|ds1)\.dat', rel) and rel not in mod:
            b = parse_layout(read(p))
            if b:
                for c, g, r, m in b:
                    tot_stock[c] += 1
                    stock_misc[c].add(m)
                    tot_mod[c] += 1
                    mod_misc[c].add(m)
    for c in mod_misc:
        extra = sorted(mod_misc[c] - stock_misc.get(c, set()))
        if extra:
            unknown_misc.append({'class': names.get(c, hex(c)), 'misc_lengths_new': extra})
    census['layouts'] = {
        'files': layouts,
        'classes_used_by_mod_never_by_stock': sorted(names.get(c, hex(c)) for c in tot_mod if c not in tot_stock),
        'classes_dropped_by_mod': {names.get(c, hex(c)): tot_stock[c] for c in tot_stock if c not in tot_mod},
        'class_totals_stock_vs_mod': {names.get(c, hex(c)): [tot_stock[c], tot_mod[c]]
                                       for c in sorted(set(tot_stock) | set(tot_mod))
                                       if names.get(c) in ('SECRETKEY', 'SECRETDOOR', 'SECRET CONTAINER', 'RING', 'HINTRING',
                                                           'HINT COLLISION', 'SETCOLLISION', 'SET GENERATOR', 'ELEVATOR',
                                                           'DEFENSE_SHIELD', 'WARP_HOLE', 'DOOR')},
        'new_misc_length_combinations': unknown_misc,
        'layouts_only_in_mod': sorted(r for r in layouts if layouts[r]['status'] == 'new'),
    }

    # .one archives: member-level difference
    one = {}
    for rel in sorted(mod):
        if not rel.lower().endswith('.one') or status[rel] != 'changed':
            continue
        try:
            a = {n: hashlib.md5(d).hexdigest() for n, f, d in one_members(stock[stock_lc[rel.lower()]])}
            b = {n: hashlib.md5(d).hexdigest() for n, f, d in one_members(mod[rel])}
        except Exception as e:  # keep the census going; record the failure
            one[rel] = {'error': str(e)[:80]}
            continue
        one[rel] = {'members_stock': len(a), 'members_mod': len(b),
                    'added': sorted(set(b) - set(a)), 'removed_count': len(set(a) - set(b)),
                    'changed': sorted(n for n in a if n in b and a[n] != b[n])}
    census['one_archives'] = one
    ext = collections.Counter()
    for rel, v in one.items():
        for k in ('added', 'changed'):
            for n in v.get(k, []):
                ext[(k, n.rsplit('.', 1)[-1].lower())] += 1
    census['one_member_kinds'] = {'%s:%s' % k: v for k, v in sorted(ext.items())}
    census['one_trimmed_stage_archives'] = sorted(r for r, v in one.items() if v.get('removed_count', 0) >= 10)

    # single-file facts
    def size_of(rel, where):
        p = where.get(rel)
        return os.path.getsize(p) if p else None
    facts = {}
    for rel in ('common/ScoreData.bin', 'common/ScoreDatb.bin', 'setid.bin', 'nukkoro.inf', 'nukkoro2.inf', 'stg_cmn_gdt.one',
                'stg_cmn_gd2.one', 'sound/B81_FINALDOOM.mlt', 'stafroll/StafRoll.srd', 'sng_vox06.adx', 'sng_vox07.adx',
                'sng_stg0613.adx', 'sng_EV_music4.adx', 'Op.flo', 'E4101.flo', 'E4102.flo', 'E4201.flo', 'E8003.flo',
                'E8201.flo', 'FadeLogo.txd', 'opening.bnr', 'mc_icon01.tp', 'mc_bunner_e.tp'):
        facts[rel] = {'status': status.get(rel), 'size_mod': size_of(rel, mod),
                      'size_stock': size_of(stock_lc.get(rel.lower(), rel), stock)}
    census['single_files'] = facts
    st_mtp = one.get('character/shadow.one', {})
    census['shadow_motion_pack'] = {'changed_members': st_mtp.get('changed', [])}
    sd, sd0 = mod.get('common/ScoreData.bin'), stock.get('common/ScoreData.bin')
    if sd and sd0:
        x, y = read(sd0), read(sd)
        n = min(len(x), len(y))
        runs = 0
        i = 0
        while i < n:
            if x[i] != y[i]:
                runs += 1
                while i < n and x[i] != y[i]:
                    i += 1
            else:
                i += 1
        census['score_data'] = {'size_stock': len(x), 'size_mod': len(y), 'differing_runs_in_common_prefix': runs}

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, 'w', encoding='utf-8', newline='\n') as f:
        json.dump(census, f, indent=1, sort_keys=True)
        f.write('\n')
    print('files %d: new %d, changed %d, identical %d; layouts %d; .one archives changed %d -> %s' % (
        len(mod), len(census['files_new']), census['files_changed_count'], len(census['files_identical']),
        len(layouts), len(one), args.out))


if __name__ == '__main__':
    main()
