#!/usr/bin/env python3
"""The modern button layouts over the GameCube's face buttons, as pad.cfg maps them.

    python tools/button_layouts/layout_overlay.py --photo <gc face buttons photo> [--pad-cfg <file>] [--out <dir>]

The photo: "Gamecube controller face buttons.jpg" by Evan-Amos, CC0, Wikimedia Commons
(https://commons.wikimedia.org/wiki/File:Gamecube_controller_face_buttons.jpg). For each family the modern cap a
GameCube button shows in the game (the same glyph art the runtime paints, make_modern_buttons.glyph) is drawn on that
button in the photo, and beside it the modern diamond with the GameCube action each button raises.
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from make_modern_buttons import FAMILIES, glyph, read_binding  # noqa: E402

ROOT = 'H:/ShadowTheHedgehogDiskGC_Extract'
# The GameCube buttons in the photo (543 x 479): centre and the size of the button.
GC_BUTTONS = {'a': ((278, 265), 168), 'b': ((113, 345), 96), 'x': ((433, 230), 104), 'y': ((235, 108), 104)}
# The modern diamond, centre offsets in units of the button pitch.
DIAMOND = {'south': (0, 1), 'east': (1, 0), 'west': (-1, 0), 'north': (0, -1)}
NAMES = {'xbox': {'south': 'A', 'east': 'B', 'west': 'X', 'north': 'Y'},
         'playstation': {'south': 'Cross', 'east': 'Circle', 'west': 'Square', 'north': 'Triangle'},
         'switch': {'south': 'B', 'east': 'A', 'west': 'Y', 'north': 'X'}}
TITLE = {'xbox': 'Xbox', 'playstation': 'PlayStation', 'switch': 'Nintendo Switch'}


def font(size):
    from PIL import ImageFont
    for name in ('segoeuib.ttf', 'arialbd.ttf', 'DejaVuSans-Bold.ttf'):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            continue
    return ImageFont.load_default()


def render(photo, family, pos, out_path):
    from PIL import Image, ImageDraw
    pw, ph = photo.size
    W, H = pw * 2 + 380, ph * 2 + 170
    sheet = Image.new('RGBA', (W, H), (24, 26, 32, 255))
    d = ImageDraw.Draw(sheet)
    d.text((24, 16), '%s buttons on the GameCube layout' % TITLE[family], font=font(34), fill=(255, 255, 255, 255))
    d.text((24, 58), 'Each GameCube button with the modern button that does its job in the game (pad.cfg)',
           font=font(20), fill=(200, 200, 210, 255))
    # left: the photo at 2x with the modern caps on the GameCube buttons
    big = photo.resize((pw * 2, ph * 2), Image.LANCZOS).convert('RGBA')
    shade = Image.new('RGBA', big.size, (0, 0, 0, 70))
    big.alpha_composite(shade)
    for gc, ((cx, cy), size) in GC_BUTTONS.items():
        where = pos[gc]
        cap = glyph(family, where, int(min(size, 120) * 2 * 0.9))
        big.alpha_composite(cap, (cx * 2 - cap.width // 2, cy * 2 - cap.height // 2))
        tag = 'GC %s  =  %s' % (gc.upper(), NAMES[family][where])
        bd = ImageDraw.Draw(big)
        f = font(22)
        tw = bd.textlength(tag, font=f)
        tx, ty = cx * 2 - tw / 2, cy * 2 + cap.height // 2 + 4
        bd.rounded_rectangle((tx - 8, ty - 2, tx + tw + 8, ty + 28), radius=8, fill=(0, 0, 0, 190))
        bd.text((tx, ty), tag, font=f, fill=(255, 255, 255, 255))
    sheet.alpha_composite(big, (24, 100))
    # right: the modern diamond, each button with the GameCube action it raises
    ox, oy, pitch = pw * 2 + 24 + 180, 100 + ph, 120
    d.text((ox - 150, 100), 'The %s pad' % TITLE[family], font=font(26), fill=(255, 255, 255, 255))
    gc_of = {v: k for k, v in pos.items() if k in GC_BUTTONS}
    for where, (dx, dy) in DIAMOND.items():
        cap = glyph(family, where, 96)
        x, y = ox + dx * pitch, oy + dy * pitch
        sheet.alpha_composite(cap, (x - 48, y - 48))
        gc = gc_of.get(where)
        text = ('GC %s' % gc.upper()) if gc else '(nothing)'
        f = font(20)
        tw = d.textlength(text, font=f)
        d.text((x - tw / 2, y + 52), text, font=f, fill=(255, 220, 120, 255))
    d.text((24, H - 40), 'Photo: Evan-Amos, CC0, Wikimedia Commons ("Gamecube controller face buttons.jpg")',
           font=font(16), fill=(150, 150, 160, 255))
    sheet.convert('RGB').save(out_path)
    print('wrote', out_path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--photo', required=True)
    ap.add_argument('--pad-cfg', default=ROOT + '/game/pad.cfg')
    ap.add_argument('--out', default=ROOT + '/exports/button-layouts/on-gamecube')
    args = ap.parse_args()
    from PIL import Image
    photo = Image.open(args.photo).convert('RGB')
    pos = read_binding(args.pad_cfg)
    print('GC button -> position:', {k: pos[k] for k in GC_BUTTONS})
    os.makedirs(args.out, exist_ok=True)
    for family in FAMILIES:
        render(photo, family, pos, os.path.join(args.out, '%s_on_gamecube.png' % family))


if __name__ == '__main__':
    main()
