"""Font maps of the game's picture faces.

    python -I cli.py list                          the faces and their bands
    python -I cli.py cut <face>                    cut report + per-letter consistency across languages
    python -I cli.py sheets [face ...]             review sheets + glyph maps into .build/hu-loc/menus/fontmaps/
    python -I cli.py set <face> "<text>" <out.png> a Hungarian line set in the face (preview, 4x)
    python -I cli.py selftest <txd|gncp>           decode -> encode round trip of every texture (byte-exact)
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))   # python -I drops the script's folder
import common as C
import compose as K
import faces as F
import generate as GEN
import library as L
import outline as O
import recipes as RC
import sheets as S
import txd as T
import png


def model_for(face, lib):
    if not face.outline:
        return None
    # calibrated on the preferred (English) picture's lines: the other languages' outlines differ a little in weight
    own = [c for c in lib.cuts if c['band'].lang == face.metric_lang] or lib.cuts
    return O.SoftOutlineModel(face, 8).calibrate(own)


def build_face(face):
    lib = L.build(face)
    model = model_for(face, lib)
    glyphs, info = GEN.alphabet(lib, RC.RECIPES.get(face.name))
    return lib, model, glyphs, info


def cmd_cut(name):
    face = F.FACES[name]
    lib = L.build(face)
    for r in lib.report:
        print('%-36s %-40s %s' % r)
    print('gap', lib.gap, 'space', lib.space)
    for c, (same, n, ws, langs) in sorted(lib.consistency().items()):
        print('%r  %d/%d identical  widths %s  %s  <- %s' % (c, same, n, ws, ','.join(langs), lib.g[c].src))


def glyph_json(g):
    return {'kind': g.kind, 'width': g.width, 'src': g.src, 'note': g.note,
            'pix': [[x, y] + list(v) for (x, y), v in sorted(g.pix.items())]}


def cmd_sheets(names):
    out = C.ensure_out()
    maps = C.ensure_out('maps')
    summary = {}
    for name in names or sorted(F.FACES, key=lambda n: (F.FACES[n].priority, n)):
        face = F.FACES[name]
        lib, model, glyphs, info = build_face(face)
        errs = [r for r in lib.report if not r[2].startswith('ok')]
        verify = [O.verify(model, c) for c in lib.cuts] if model else []
        S.alphabet_sheet(face, lib, glyphs, model, os.path.join(out, name + '.png'), 4, getattr(face, 'samples', ()))
        S.accent_sheet(face, lib, glyphs, model, os.path.join(out, name + '_accents.png'), 8)
        S.cut_sheet(face, lib, os.path.join(C.ensure_out('cuts'), name + '_cuts.png'), 2)
        target = GEN.target_set(face)
        real = [c for c in target if c in glyphs and glyphs[c].kind == 'real']
        comp = [c for c in target if c in glyphs and glyphs[c].kind == 'composed']
        drawn = [c for c in target if c in glyphs and glyphs[c].kind not in ('real', 'composed')]
        miss = [c for c in target if c not in glyphs]
        summary[name] = {'title': face.title, 'real': ''.join(real), 'composed': ''.join(comp), 'drawn': ''.join(drawn),
                         'missing': ''.join(miss), 'extra_real': ''.join(sorted(c for c in glyphs if c not in target)),
                         'gap': lib.gap, 'space': lib.space, 'cut_errors': errs,
                         'outline_rebuild': [(round(a, 1), round(b, 1)) for a, b, n in verify],
                         'info': {c: info.get(c, '') for c in sorted(glyphs) + miss},
                         'accents': {'%s/%s' % k: (a.char, a.note, getattr(a, 'body_h', None))
                                     for k, a in getattr(lib, 'accents', {}).items()},
                         'notes': face.notes}
        with open(os.path.join(maps, name + '.json'), 'w', encoding='utf-8', newline='\n') as f:
            json.dump({'face': name, 'title': face.title, 'gap': lib.gap, 'space': lib.space,
                       'outline_table': {'%s/%s' % k: v for k, v in model.table.items()} if model else None,
                       'glyphs': {c: glyph_json(g) for c, g in sorted(glyphs.items())}}, f, ensure_ascii=False)
        print('%-18s real %3d  composed %3d  drawn %3d  missing %3d  %s' % (name, len(real), len(comp), len(drawn),
                                                                           len(miss), ''.join(miss)))
        for e in errs:
            print('   CUT ERROR', e)
    old = {}
    sp = os.path.join(out, 'summary.json')
    if names and os.path.exists(sp):
        old = json.load(open(sp, encoding='utf-8'))
    old.update(summary)
    with open(sp, 'w', encoding='utf-8', newline='\n') as f:
        json.dump(old, f, ensure_ascii=False, indent=1)


def cmd_set(name, text, path):
    face = F.FACES[name]
    lib, model, glyphs, info = build_face(face)
    pix, missing = K.set_text(face, lib, glyphs, model, text)
    xs = [x for x, _ in pix]; ys = [y for _, y in pix]
    W, H = max(xs) + 8, max(ys) - min(ys) + 9
    canvas = [[S.BG] * (W * 4) for _ in range(H * 4)]
    K.blit(canvas, pix, 4, 4 - min(ys), S.BG, 4)
    png.write(path, canvas)
    if missing:
        print('missing letters:', ''.join(missing))


if __name__ == '__main__':
    a = sys.argv[1:]
    if not a or a[0] == 'list':
        for n, f in sorted(F.FACES.items(), key=lambda kv: (kv[1].priority, kv[0])):
            print('%-18s p%d %s  (%d bands)' % (n, f.priority, f.title, len(f.bands)))
    elif a[0] == 'cut':
        cmd_cut(a[1])
    elif a[0] == 'sheets':
        cmd_sheets(a[1:])
    elif a[0] == 'set':
        cmd_set(a[1], a[2], a[3])
    elif a[0] == 'selftest':
        for r in T.selftest(a[1]):
            print(*r)
