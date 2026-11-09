#!/usr/bin/env python3
"""Simulated renders of every in-game button prompt, original and modern.

    BUTTON_GLYPHS_DUMP=<dir> .build/tests/Release/button_glyphs_tests.exe ../game/sys/main.dol
    python tools/button_layouts/simulate_prompts.py --sheets <dir> [--out <dir>]

The test binary writes each sharp sheet exactly as the runtime builds it (ButtonGlyphs::sharp_sheet: every family,
two bindings - by label, and GC B on west / GC X on east as the user's pad.cfg has it). This draws each prompt's casts
from its own CSD project: the cast boxes, cells (UVs), colours and the motion keys that animate the prompt, sampled
from those sheets (bicubic, as a GPU's filtered draw would look) at 4 guest pixels per pixel. The "original" row
draws the disc's own sheets. Not drawn: the blend mode of a cast (everything is alpha-blended) and the game's
own placement of a prompt on screen (each prompt is drawn on its own around its casts).
"""
import argparse
import glob
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'assets'))
from gncp_read import Gncp  # noqa: E402
from txd_extract import decode, parse_txd  # noqa: E402

ROOT = 'H:/ShadowTheHedgehogDiskGC_Extract'
FILES = ROOT + '/game/files/'
Z = 4                       # canvas pixels per guest pixel
BG = (34, 38, 48, 255)
FAMILIES = ('xbox', 'playstation', 'switch')
BINDINGS = (('label', 'by label'), ('swapped', 'your pad.cfg (GC B on X, GC X on B)'))


def embedded_txd_textures(path):
    data = open(path, 'rb').read()
    start = data.find(b'\x16\x00\x00\x00', data.find(b'NEND'))
    size = struct.unpack_from('<I', data, start + 4)[0]
    tmp = os.path.join(os.environ.get('TEMP', '.'), '_simulate_prompts.txd')
    open(tmp, 'wb').write(data[start:start + 12 + size])
    return txd_images(tmp)


def txd_images(path):
    from PIL import Image
    out = {}
    for t in parse_txd(path):
        rgba = decode(t['format'], t['palette_format'], t['_palette'], t['_pixels'], t['width'], t['height'])
        out[t['name']] = Image.frombytes('RGBA', (t['width'], t['height']), bytes(rgba))
    return out


def load_sharp(sheets_dir, sheet, family, binding):
    from PIL import Image
    found = glob.glob(os.path.join(sheets_dir, 'sheet%d__%s_%s_*.rgba' % (sheet, family, binding)))
    if not found:
        raise SystemExit('no dumped sheet %d %s %s in %s' % (sheet, family, binding, sheets_dir))
    w, h = map(int, re.search(r'_(\d+)x(\d+)\.rgba$', found[0]).groups())
    return Image.frombytes('RGBA', (w, h), open(found[0], 'rb').read())


def colour_of(word):
    v = int(word, 16) if isinstance(word, str) else word
    return ((v >> 24) & 255, (v >> 16) & 255, (v >> 8) & 255, v & 255)


def draw(canvas, origin, tex, cell, box, colour=(255, 255, 255, 255), scale=1.0, dy=0.0):
    """One cast: the cell's rectangle of `tex` stretched over the guest box (x0, y0, x1, y1), scaled about its
    centre, multiplied by `colour`."""
    from PIL import Image
    x0, y0, x1, y1 = box
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2 + dy
    w, h = (x1 - x0) * scale, (y1 - y0) * scale
    if w <= 0 or h <= 0:
        return
    tw, th = tex.size
    crop = tex.crop((round(cell['u0'] * tw), round(cell['v0'] * th), round(cell['u1'] * tw), round(cell['v1'] * th)))
    img = crop.resize((max(1, round(w * Z)), max(1, round(h * Z))), Image.BICUBIC)
    r, g, b, a = colour
    if (r, g, b, a) != (255, 255, 255, 255):
        bands = img.split()
        bands = [bands[i].point(lambda v, m=m: v * m // 255) for i, m in enumerate((r, g, b, a))]
        img = Image.merge('RGBA', bands)
    canvas.alpha_composite(img, (round(origin[0] + (cx - w / 2) * Z), round(origin[1] + (cy - h / 2) * Z)))


def lerp_key(keys, frame):
    """A motion track's value at `frame` (linear between keys, held outside them)."""
    if frame <= keys[0][0]:
        return keys[0][1]
    for (f0, v0), (f1, v1) in zip(keys, keys[1:]):
        if f0 <= frame <= f1:
            t = (frame - f0) / (f1 - f0) if f1 != f0 else 0.0
            if isinstance(v0, tuple):
                return tuple(round(a + (b - a) * t) for a, b in zip(v0, v1))
            return v0 + (v1 - v0) * t
    return keys[-1][1]


def motion_tracks(scene, name):
    """{(group, index): {track: [(frame, value)]}} of motion `name`; colours as RGBA tuples."""
    out = {}
    for m in scene['motions']:
        if m['name'] != name:
            continue
        for gi, group in enumerate(m['groups']):
            for ci, cm in enumerate(group):
                tracks = {}
                for k, keys in cm['tracks'].items():
                    tracks[k] = [(kf['frame'], colour_of(kf['i']) if k == 'colour' else kf['f']) for kf in keys]
                if tracks:
                    out[(gi, ci)] = tracks
    return out


def scene_of(gncp, name):
    for s in Gncp(gncp).scenes():
        if s['name'] == name:
            return s
    raise SystemExit('no scene %s in %s' % (name, gncp))


def cast_box(c):
    return (c['x0'], c['y0'], c['x1'], c['y1'])


def label_tile(img, text, sub=None):
    from PIL import Image, ImageDraw
    tile = Image.new('RGBA', (img.width, img.height + 34), BG)
    tile.alpha_composite(img, (0, 34))
    d = ImageDraw.Draw(tile)
    d.text((6, 4), text, fill=(255, 230, 120, 255))
    if sub:
        d.text((6, 18), sub, fill=(190, 190, 200, 255))
    return tile


def grid(rows, title, out_path):
    """rows: [(row label, [tile images])] -> one sheet."""
    from PIL import Image, ImageDraw
    label_w = 230
    cols = max(len(t) for _, t in rows)
    cw = max(t.width for _, ts in rows for t in ts)
    rh = [max(t.height for t in ts) for _, ts in rows]
    sheet = Image.new('RGBA', (label_w + cols * (cw + 8) + 8, 40 + sum(h + 8 for h in rh)), (22, 24, 30, 255))
    d = ImageDraw.Draw(sheet)
    d.text((10, 12), title, fill=(255, 255, 255, 255))
    y = 40
    for (label, tiles), h in zip(rows, rh):
        d.text((10, y + h // 2 - 6), label, fill=(220, 220, 220, 255))
        for i, t in enumerate(tiles):
            sheet.alpha_composite(t, (label_w + i * (cw + 8), y))
        y += h + 8
    sheet.save(out_path)
    print('wrote', out_path)


def rows_for(sheets_dir, sheet, original_tex, render):
    """The original row, then a row per family and binding; render(tex) -> [tiles]."""
    rows = [('original (GameCube disc)', render(original_tex))]
    for fam in FAMILIES:
        for key, text in BINDINGS:
            rows.append(('%s, %s' % (fam, text), render(load_sharp(sheets_dir, sheet, fam, key))))
    return rows


def canvas(w, h):
    from PIL import Image
    return Image.new('RGBA', (int(w * Z), int(h * Z)), BG)


def hissatu(sheets_dir, out_dir, which):
    gncp = FILES + 'csdFiles/Hissatu_%s.gncp' % which
    scene = scene_of(gncp, 'Hissatu' + which)
    button, eff = scene['casts'][0], scene['casts'][1]
    cell = scene['cells'][button['cell']]
    original = list(embedded_txd_textures(gncp).values())[0]
    power = 'Chaos_Control' if which == 'Hero' else 'Chaos_Blast'
    motion = motion_tracks(scene, power)
    if not motion:   # the dark scene names its motion differently: take the one that scales the effect
        for m in scene['motions']:
            tracks = motion_tracks(scene, m['name'])
            if any('scale_x' in t for t in tracks.values()):
                power, motion = m['name'], tracks
                break
    eff_tracks = motion.get((0, 1), {})
    bx = cast_box(button)
    centre = ((bx[0] + bx[2]) / 2, (bx[1] + bx[3]) / 2)

    def render(tex):
        tiles = []
        for frame in (0, 3, 6, 10):
            c = canvas(80, 80)
            o = ((40 - centre[0]) * Z, (40 - centre[1]) * Z)
            draw(c, o, tex, cell, bx)
            s = lerp_key(eff_tracks['scale_x'], frame) if 'scale_x' in eff_tracks else 1.0
            col = lerp_key(eff_tracks['colour'], frame) if 'colour' in eff_tracks else (255, 255, 255, 255)
            draw(c, o, tex, cell, bx, col, s)
            tiles.append(label_tile(c, '%s frame %d' % (power, frame), 'pulse x%.2f alpha %d' % (s, col[3])))
        return tiles

    grid(rows_for(sheets_dir, 25, original, render),
         'Chaos %s prompt (Hissatu_%s.gncp, %s button): the awakening pulse' %
         ('Control' if which == 'Hero' else 'Blast', which, button['name']),
         os.path.join(out_dir, '1_%s_awakening_%s.png' % ('chaos_control' if which == 'Hero' else 'chaos_blast',
                                                          which.lower())))


def vharrow(sheets_dir, out_dir):
    gncp = FILES + 'csdFiles/VHArrow.gncp'
    scene = scene_of(gncp, 'VHArrow')
    original = list(embedded_txd_textures(gncp).values())[0]
    motion = motion_tracks(scene, 'DefaultAnim')

    def render(tex):
        tiles = []
        for frame in (0, 10, 18):
            c = canvas(70, 90)
            o = (35 * Z, 92 * Z)
            for gi, cast in enumerate(scene['casts']):
                tracks = motion.get((gi, 0), {})
                col = lerp_key(tracks['colour'], frame) if 'colour' in tracks else colour_of(cast['colour'])
                dy = (lerp_key(tracks['y'], frame) * 480.0 - cast['ty']) if 'y' in tracks else 0.0
                draw(c, o, tex, scene['cells'][cast['cell']], cast_box(cast), col, 1.0, dy)
            tiles.append(label_tile(c, 'frame %d' % frame, 'circlebg 20%%, light %d' % lerp_key(
                motion.get((2, 0), {}).get('colour', [(0, (0, 0, 0, 0))]), frame)[3]))
        return tiles

    grid(rows_for(sheets_dir, 6, original, render),
         'Checkpoint / vehicle prompt (VHArrow.gncp): circlebg, button, button_light, arrow (DefaultAnim)',
         os.path.join(out_dir, '2_checkpoint_vehicle.png'))


def command_window(sheets_dir, out_dir):
    gncp = FILES + 'csdFiles/CommandWindow.gncp'
    texs = embedded_txd_textures(gncp)
    hud, icons = texs['000_hud_meter'], texs['001_command_icons_gc']
    window = scene_of(gncp, 'Window')
    weapon = scene_of(gncp, 'Weapon')
    car = scene_of(gncp, 'Car_BCom')
    button_cells = [p for p in weapon['casts'][2]['patterns'] if p >= 0]
    names = {33: 'GC B', 27: 'GC X', 34: 'GC A'}

    def render(icon_tex):
        tiles = []
        for pattern in button_cells:
            c = canvas(160, 85)
            o = (155 * Z, 80 * Z)
            for cast in window['casts']:
                if cast['cell'] >= 0:
                    draw(c, o, hud, window['cells'][cast['cell']], cast_box(cast), colour_of(cast['colour']))
            for sc in (weapon, car):
                for cast in sc['casts']:
                    if cast['cell'] < 0:
                        continue
                    cell = sc['cells'][pattern if cast['name'].startswith('button') else cast['cell']]
                    tex = icons if cell['tex'] == 1 else hud
                    tex = icon_tex if cell['tex'] == 1 else hud
                    draw(c, o, tex, cell, cast_box(cast), colour_of(cast['colour']))
            tiles.append(label_tile(c, 'button pattern %d (%s)' % (pattern, names.get(pattern, '?'))))
        return tiles

    grid(rows_for(sheets_dir, 7, icons, render),
         'Weapon / vehicle window (CommandWindow.gncp): the button cast in its three patterns',
         os.path.join(out_dir, '3_weapon_vehicle_window.png'))


def pause(sheets_dir, out_dir):
    gncp = FILES + 'csdFiles/pause/pause.gncp'
    txd = txd_images(FILES + 'csdFiles/pause/pause0100_EN.txd')
    by_index = {0: txd['hud_meter'], 1: txd['micon'], 2: txd['pause_main'], 3: txd['storyroute']}

    def render(main_tex):
        tiles = []
        for scene_name in ('Pause/SRText', 'StoryRoute/StoryRoute'):
            scene = scene_of(gncp, scene_name)
            button = [c for c in scene['casts'] if c['name'] == 'button'][0]
            near = [c for c in scene['casts'] if c['cell'] >= 0 and abs(c['y0'] - button['y0']) < 30 and
                    abs(c['x0'] - button['x0']) < 160]
            xs = [c['x0'] for c in near] + [c['x1'] for c in near]
            ys = [c['y0'] for c in near] + [c['y1'] for c in near]
            c = canvas(max(xs) - min(xs) + 16, max(ys) - min(ys) + 16)
            o = ((8 - min(xs)) * Z, (8 - min(ys)) * Z)
            for cast in near:
                cell = scene['cells'][cast['cell']]
                tex = main_tex if cell['tex'] == 2 else by_index[cell['tex']]
                draw(c, o, tex, cell, cast_box(cast), colour_of(cast['colour']))
            tiles.append(label_tile(c, scene_name))
        return tiles

    grid(rows_for(sheets_dir, 14, txd['pause_main'], render),
         'Pause screen (pause.gncp): the story-route button with the casts beside it',
         os.path.join(out_dir, '4_pause_story_route.png'))


def synopsis(sheets_dir, out_dir):
    gncp = FILES + 'csdFiles/Synopsis.gncp'
    txd = txd_images(FILES + 'csdFiles/Synopsis_EN.txd')
    scene = scene_of(gncp, 'Button')
    patterns = [p for p in scene['casts'][0]['patterns'] if p >= 0]
    names = {1: 'PS2', 3: 'GC (drawn)', 2: 'Xbox'}

    def render(tex):
        tiles = []
        btn, txt = scene['casts'][0], scene['casts'][1]
        bx = cast_box(btn)
        tbox = (bx[0] + txt['x0'] + (bx[2] - bx[0]), bx[1] + txt['y0'] + (bx[3] - bx[1]) / 2,
                bx[0] + txt['x1'] + (bx[2] - bx[0]), bx[1] + txt['y1'] + (bx[3] - bx[1]) / 2)
        c = canvas(190, 40)
        o = ((6 - bx[0]) * Z, (6 - bx[1]) * Z)
        draw(c, o, tex, scene['cells'][btn['cell']], bx)
        draw(c, o, tex, scene['cells'][txt['cell']], tbox)
        tiles.append(label_tile(c, 'skip (pattern %d, %s)' % (btn['cell'], names.get(btn['cell'], ''))))
        return tiles

    grid(rows_for(sheets_dir, 0, txd['Synopsis'], render),
         'Cutscene skip (Synopsis.gncp): START', os.path.join(out_dir, '5_cutscene_skip.png'))


def menus(sheets_dir, out_dir):
    gncp = FILES + 'csdFiles/Options.gncp'
    txd = txd_images(FILES + 'csdFiles/Options_EN.txd')
    scene = scene_of(gncp, 'menu')
    casts = [c for c in scene['casts'] if c['name'] in ('select_btn', 'exit_btn')]

    def render(tex):
        tiles = []
        for cast in casts:
            c = canvas(40, 40)
            bx = cast_box(cast)
            o = ((20 - (bx[0] + bx[2]) / 2) * Z, (20 - (bx[1] + bx[3]) / 2) * Z)
            draw(c, o, tex, scene['cells'][cast['cell']], bx)
            tiles.append(label_tile(c, cast['name'], 'GC %s' % ('A' if 'select' in cast['name'] else 'Y')))
        return tiles

    grid(rows_for(sheets_dir, 8, txd['gamemenu'], render),
         'Menus (Options / honban / SelectMode / Story_Library / BattleMode share the gamemenu sheet): select, exit',
         os.path.join(out_dir, '6_menus_select_exit.png'))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--sheets', required=True, help='BUTTON_GLYPHS_DUMP directory of the test binary')
    ap.add_argument('--out', default=ROOT + '/exports/button-layouts/simulated')
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    hissatu(args.sheets, args.out, 'Hero')
    hissatu(args.sheets, args.out, 'Dark')
    vharrow(args.sheets, args.out)
    command_window(args.sheets, args.out)
    pause(args.sheets, args.out)
    synopsis(args.sheets, args.out)
    menus(args.sheets, args.out)


if __name__ == '__main__':
    main()
