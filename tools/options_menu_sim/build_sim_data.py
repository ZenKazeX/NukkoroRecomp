#!/usr/bin/env python3
"""Build the data the Options menu simulator (index.html) draws from, straight from the disc.

    python build_sim_data.py [--game-files H:/ShadowTheHedgehogDiskGC_Extract/game/files]

Reads (never writes) the extracted disc and writes, next to this script:

    generated/tex/Options_<lang>__<slot>.png      the four layout textures per language
    generated/tex/Advertise_<lang>NN.png          front-end glyph sheets, luminance -> alpha
    generated/data.js                             window.OPTIONS_DATA = {layout, fonts, strings}

Sources and the joins used:

  * csdFiles/Options.gncp  - scenes, cells, casts, motions, via ../assets/gncp_read.py.
  * csdFiles/Options_<lang>.txd - decoded by ../assets/txd_extract.py. The project's texture
    slot s is dictionary entry (count - 1 - s); see ../assets/menu_backgrounds.py for the proof.
  * fonts/Advertise/Advertise_<lang>.txd + Advertise_<lang>NN.met - the glyph sheets. A .met is
    text: "METRICS1", sheet name, a constant 5, then one "code x0 y0 x1 y1 #" line per glyph
    (code = Unicode, box inclusive). The sheets are C4/RGB565 grey; they are written here as white
    with alpha = luminance, which is how white text over the menu reads.
  * fonts/Advertise/Advertise_<lang>.fnt - the front-end message table (u32 LE count, 20-byte
    entries whose first u32 is the offset of a UTF-16LE string). Only the ids in STRING_IDS are
    exported.
"""
import argparse
import glob
import json
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'assets'))
from gncp_read import Gncp                                        # noqa: E402
from txd_extract import parse_txd, decode                         # noqa: E402

LANGS = ['EN', 'FR', 'GE', 'IT', 'JP', 'SP']

# Front-end message ids the Options screen uses (Advertise_<lang>.fnt).
STRING_IDS = {
    'help_voice': 0, 'help_subtitle': 1, 'help_soundtest': 2, 'help_speaker': 3,
    'help_vibration': 4, 'help_load': 5,
    'bgm_stage': 332, 'bgm_event': 333, 'bgm_theme': 334, 'bgm_main_theme': 335,
    'bgm_jingle': 336, 'bgm_opening': 337, 'bgm_ending': 338,
    'lang_jp': 339, 'lang_en': 340, 'lang_fr': 341, 'lang_ge': 342, 'lang_sp': 343,
    'lang_it': 344, 'main_menu': 345, 'on': 346, 'off': 347, 'load': 348,
    'stereo': 349, 'mono': 350, 'dolby_pl2': 351,
    'initialize_ok': 395, 'leave_without_saving': 396, 'settings_changed': 397,
    'yes': 565, 'no': 566, 'options': 656,
}
# Sound test track names, I AM .. ENDING (the Dolphin reference lists them in this order).
STRING_IDS.update({'track_%d' % i: i for i in range(611, 666)})


def read_fnt(path):
    d = open(path, 'rb').read()
    n = struct.unpack_from('<I', d, 0)[0]
    out = []
    for i in range(n):
        off = struct.unpack_from('<I', d, 4 + 20 * i)[0]
        end = off
        while end + 1 < len(d) and d[end:end + 2] != b'\0\0':
            end += 2
        out.append(d[off:end].decode('utf-16le', 'replace'))
    return out


def read_met(path):
    lines = open(path, encoding='ascii').read().splitlines()
    glyphs = []
    for ln in lines[3:]:
        m = re.match(r'\s*(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)', ln)
        if m:
            glyphs.append(tuple(int(v) for v in m.groups()))
    return lines[1].strip(), glyphs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game-files', default='H:/ShadowTheHedgehogDiskGC_Extract/game/files')
    args = ap.parse_args()
    from PIL import Image

    files = args.game_files
    out_tex = os.path.join(HERE, 'generated', 'tex')
    os.makedirs(out_tex, exist_ok=True)

    # Layout.
    g = Gncp(os.path.join(files, 'csdFiles', 'Options.gncp'))
    scenes = []
    for sc in g.scenes():
        casts = []
        for c in sc['casts']:
            casts.append(dict(name=c['name'], group=c['group'], index=c['index'], kind=c['kind'],
                              pair=list(c['pair']), local_corners=c['local_corners'],
                              x=c['tx'] / 640.0, y=c['ty'] / 480.0, rotation=c['rotation'],
                              scale_x=c['scale_x'], scale_y=c['scale_y'],
                              pattern=c['pattern_index'], patterns=c['patterns'],
                              colour=int(c['colour'], 16), hide=c['hide']))
        motions = []
        for m in sc['motions']:
            motions.append(dict(name=m['name'], start=m['start'], end=m['end'], groups=[
                [{k: [[key['frame'], key['i'] if k == 'colour' else key['f'], key['interp']]
                       for key in keys] for k, keys in cast['tracks'].items()}
                 for cast in grp] for grp in m['groups']]))
        scenes.append(dict(name=sc['name'], fps=sc['fps'], textures=sc['textures'],
                           cells=sc['cells'], casts=casts, motions=motions))
    slot_count = len(scenes[0]['textures'])

    # Layout textures per language: slot s = dictionary entry count-1-s.
    for lang in LANGS:
        texs = parse_txd(os.path.join(files, 'csdFiles', 'Options_%s.txd' % lang))
        assert len(texs) == slot_count, 'slot count mismatch for %s' % lang
        for t in texs:
            slot = len(texs) - 1 - t['index']
            rgba = decode(t['format'], t['palette_format'], t['_palette'], t['_pixels'],
                          t['width'], t['height'])
            Image.frombytes('RGBA', (t['width'], t['height']), bytes(rgba)).save(
                os.path.join(out_tex, 'Options_%s__%d.png' % (lang, slot)), optimize=True)

    # Fonts and strings.
    fonts, strings = {}, {}
    adv = os.path.join(files, 'fonts', 'Advertise')
    for lang in LANGS:
        sheets = {}
        for t in parse_txd(os.path.join(adv, 'Advertise_%s.txd' % lang)):
            rgba = bytearray(decode(t['format'], t['palette_format'], t['_palette'], t['_pixels'],
                                    t['width'], t['height']))
            for i in range(0, len(rgba), 4):
                lum = (rgba[i] * 299 + rgba[i + 1] * 587 + rgba[i + 2] * 114) // 1000
                rgba[i:i + 4] = bytes((255, 255, 255, lum))
            sheets[t['name']] = (t['width'], t['height'], rgba)
            Image.frombytes('RGBA', (t['width'], t['height']), bytes(rgba)).save(
                os.path.join(out_tex, '%s.png' % t['name']), optimize=True)
        glyphs = {}
        for met in sorted(glob.glob(os.path.join(adv, 'Advertise_%s[0-9][0-9].met' % lang))):
            sheet, entries = read_met(met)
            w, _h, rgba = sheets[sheet]
            for code, x0, y0, x1, y1 in entries:
                # Ink columns, for proportional spacing of the Latin sheets.
                ink = [x for x in range(x0, x1 + 1)
                       if any(rgba[((y * w) + x) * 4 + 3] > 40 for y in range(y0, y1 + 1))]
                glyphs[chr(code)] = [sheet, x0, y0, x1 - x0 + 1, y1 - y0 + 1,
                                     (ink[0] - x0) if ink else 0,
                                     (ink[-1] - x0 + 1) if ink else 0]
        fonts[lang] = glyphs
        table = read_fnt(os.path.join(adv, 'Advertise_%s.fnt' % lang))
        strings[lang] = {k: table[i] for k, i in STRING_IDS.items() if i < len(table)}

    data = dict(source='game/files/csdFiles/Options.gncp + Options_<lang>.txd, '
                       'fonts/Advertise/Advertise_<lang>.{txd,met,fnt}',
                langs=LANGS, scenes=scenes, fonts=fonts, strings=strings)
    with open(os.path.join(HERE, 'generated', 'data.js'), 'w', encoding='utf-8', newline='\n') as fh:
        fh.write('// Generated by build_sim_data.py from the disc - do not edit.\n')
        fh.write('window.OPTIONS_DATA = ')
        json.dump(data, fh, ensure_ascii=False, separators=(',', ':'))
        fh.write(';\n')
    print('scenes: %s' % ', '.join(s['name'] for s in scenes))
    print('glyphs: %s' % ', '.join('%s=%d' % (k, len(v)) for k, v in fonts.items()))
    print('EN strings: %s' % json.dumps(strings['EN'], ensure_ascii=False)[:400])
    return 0


if __name__ == '__main__':
    sys.exit(main())
