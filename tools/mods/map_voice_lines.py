#!/usr/bin/env python3
"""Map PRS_VOICE_E.afs entries to the subtitle tables (*_EN.fnt) that reference them.

usage: map_voice_lines.py <game-files-dir> <out.json> [--lang EN]

Reads only: <game-files>/PRS_VOICE_E.afs (table + ADX headers) and <game-files>/fonts/**/<stage>_<LANG>.fnt.
Writes names, indices, stages, keys, seconds. No audio bytes and no subtitle text.
FNT: little-endian, i32 count, 20-byte records {text_off, key, type, active_time, audio_id}.
"""
import json, os, struct, sys, glob


def read_afs(path):
    with open(path, 'rb') as f:
        head = f.read(8)
        assert head[:4] == b'AFS\0', 'not an AFS'
        n = struct.unpack('<I', head[4:8])[0]
        toc = struct.unpack('<%dI' % (2 * n), f.read(8 * n))
        noff, nlen = struct.unpack('<II', f.read(8))
        f.seek(noff)
        names = f.read(nlen)
        out = []
        for i in range(n):
            nm = names[48 * i:48 * i + 32].split(b'\0')[0].decode('latin1')
            off, ln = toc[2 * i], toc[2 * i + 1]
            f.seek(off)
            h = f.read(0x14)
            sec = None
            if len(h) >= 0x14 and h[0] == 0x80 and h[1] == 0:
                rate, samples = struct.unpack('>II', h[0x08:0x10])
                if rate:
                    sec = round(samples / rate, 3)
            out.append(dict(index=i, name=nm, offset=off, size=ln, seconds=sec))
        return out


def read_fnt(path):
    d = open(path, 'rb').read()
    n = struct.unpack('<i', d[:4])[0]
    recs = []
    for i in range(n):
        off, key, typ, tm, aud = struct.unpack('<5i', d[4 + 20 * i:24 + 20 * i])
        recs.append((key, typ, tm, aud))
    return recs


def stage_fnts(fonts, lang):
    r = {}
    for p in sorted(glob.glob(os.path.join(fonts, '*', '*_%s.fnt' % lang))):
        stage = os.path.basename(p)[:-len('_%s.fnt' % lang)]
        r[stage] = p
    return r


def main():
    a = sys.argv[1:]
    if len(a) < 2:
        print(__doc__)
        return 2
    files, out = a[0], a[1]
    lang = a[a.index('--lang') + 1] if '--lang' in a else 'EN'
    afs = read_afs(os.path.join(files, 'PRS_VOICE_E.afs'))
    entries = {}
    stats = dict(afs_entries=len(afs), fnt_files=0, voiced_records=0, bad_audio_ids=0, chained=0)
    for stage, p in stage_fnts(os.path.join(files, 'fonts'), lang).items():
        stats['fnt_files'] += 1
        recs = read_fnt(p)
        for i, (key, typ, tm, aud) in enumerate(recs):
            if aud < 0:
                continue
            stats['voiced_records'] += 1
            if aud >= len(afs):
                stats['bad_audio_ids'] += 1
                continue
            # chain: following records key+1.. with audio -1 and nonzero time
            chain = 0
            j = i + 1
            while j < len(recs) and recs[j][3] < 0 and recs[j][2] > 0 and recs[j][0] == key + chain + 1:
                chain += 1
                j += 1
            if chain:
                stats['chained'] += 1
            e = entries.setdefault(aud, dict(refs=[]))
            e['refs'].append(dict(stage=stage, key=key, type=typ, frames=tm, chain=chain))
    res = {}
    for aud, e in sorted(entries.items()):
        m = afs[aud]
        res[m['name']] = dict(
            index=aud, seconds=m['seconds'],
            stages=sorted({r['stage'] for r in e['refs']}),
            keys=[r['key'] for r in e['refs']],
            refs=e['refs'])
    unref = [m['name'] for m in afs if m['name'] not in res]
    stats['referenced_entries'] = len(res)
    stats['unreferenced_entries'] = len(unref)
    json.dump(dict(lang=lang, stats=stats, entries=res, unreferenced=unref), open(out, 'w'), separators=(',', ':'))
    print(json.dumps(stats))
    return 0


if __name__ == '__main__':
    sys.exit(main())
