#!/usr/bin/env python3
"""Extract every controller-button glyph on the Shadow the Hedgehog PAL GameCube disc (GUPP8P).

    python extract_buttons.py [--game-files <dir>] [--out <dir>]

Reads game/files only. Writes <out>/atlases (the source sheets), <out>/<platform>/ (one PNG per
glyph, native size), <out>/index.txt and <out>/_contact_sheet.png.

Where the buttons live (found 2026-10-29):

  gamemenu         csdFiles/{Options,honban,SelectMode,BattleMode,Story_Library}_<lang>.txd
                   256x256. The front-end "select"/"exit" prompts. Holds PS2, GameCube AND Xbox
                   glyphs; each button cast has four patterns, one per platform, and the GC disc
                   simply starts on the GC pattern. Same art in all six languages (the per-language
                   C8 palettes differ by a few levels, nothing visible); EN is taken.
  command_icons_gc embedded in CommandWindow.gncp / 2P_CommandWindow.gncp (weapon/vehicle window)
  squarebuttons    embedded in Hissatu_Hero.gncp / Hissatu_Dark.gncp (Chaos Control / Chaos Blast
                   prompt), PS2 / Xbox / GC patterns
  pause_main       csdFiles/pause/pause<stage>_<lang>.txd (Story route button), PS2 / Xbox / GC
  VHArrow_gc       embedded in VHArrow.gncp (vehicle exit arrow prompt)
  Synopsis         csdFiles/Synopsis_<lang>.txd (cutscene "skip"), PS2 / GC / Xbox START
  windowitem       InfoWindow.txd (disc root), used by BbuttonWindow.gncp: a PS2 SQUARE left on the
                   GameCube disc, plus small PS2 cross/square badges on two item icons
  E3 gamemenu      embedded in E3RomTitle.gncp (the E3 demo title): an older full sheet that no
                   cast references

Cell rectangles below are the layout's own cells (Chao CSD .gncp, see ../assets/gncp_read.py), not
guesses; the three unreferenced glyphs (gamemenu GC X and red B, VHArrow highlight mask) and the
E3 sheet are boxed by their alpha.
"""
import argparse
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'assets'))
from txd_extract import decode, parse_txd  # noqa: E402

# sheet id -> (where, texture name). 'emb:' = the dictionary embedded after NEND in that .gncp.
SHEETS = {
    'gamemenu': ('csdFiles/Options_EN.txd', 'gamemenu'),
    'command_icons_gc': ('emb:csdFiles/CommandWindow.gncp', '001_command_icons_gc'),
    'squarebuttons': ('emb:csdFiles/Hissatu_Hero.gncp', '000_squarebuttons'),
    'pause_main': ('csdFiles/pause/pause0100_EN.txd', 'pause_main'),
    'VHArrow_gc': ('emb:csdFiles/VHArrow.gncp', '000_VHArrow_gc'),
    'Synopsis': ('csdFiles/Synopsis_EN.txd', 'Synopsis'),
    'windowitem': ('InfoWindow.txd', '000_windowitem'),
    'e3_gamemenu': ('emb:csdFiles/E3RomTitle.gncp', '003_gamemenu'),
}

# (platform, button, sheet, x0, y0, x1, y1, used by). Rects clip to the sheet.
FRONT = 'Options, SelectMode, Story_Library: [PS2, GC, Xbox, PS2-JP]; honban, BattleMode: [PS2, Xbox, GC, PS2-JP]'
GLYPHS = [
    ('ps2', 'cross', 'gamemenu', 128, 203, 156, 231, 'select pattern 0; exit pattern 3 (PS2 Japan cancel). ' + FRONT),
    ('gamecube', 'x', 'gamemenu', 156, 203, 184, 231, 'no cast'),
    ('gamecube', 'y', 'gamemenu', 184, 203, 212, 231, 'exit, GC pattern (default on this disc)'),
    ('xbox', 'a', 'gamemenu', 212, 203, 240, 231, 'select, Xbox pattern'),
    ('ps2', 'triangle', 'gamemenu', 128, 230, 156, 256, 'exit pattern 0'),
    ('ps2', 'circle', 'gamemenu', 156, 230, 184, 256, 'select pattern 3 (PS2 Japan confirm)'),
    ('gamecube', 'a', 'gamemenu', 184, 230, 212, 256, 'select, GC pattern (default on this disc)'),
    ('xbox', 'y', 'gamemenu', 212, 230, 240, 256, 'exit, Xbox pattern'),
    ('unknown', 'b_red', 'gamemenu', 229, 177, 256, 202, 'no cast (red B: GC B or Xbox B)'),
    ('gamecube', 'b', 'command_icons_gc', 223, 103, 246, 126, 'CommandWindow button1/button2 pattern 0'),
    ('gamecube', 'x', 'command_icons_gc', 223, 48, 246, 71, 'CommandWindow button1/button2 pattern 1'),
    ('gamecube', 'a', 'command_icons_gc', 223, 76, 246, 99, 'CommandWindow button1/button2 pattern 2'),
    ('ps2', 'triangle', 'squarebuttons', 0, 32, 32, 64, 'Hissatu XButton/YButton pattern 0'),
    ('xbox', 'y', 'squarebuttons', 32, 0, 64, 32, 'Hissatu XButton/YButton pattern 1'),
    ('gamecube', 'y', 'squarebuttons', 32, 32, 64, 64, 'Hissatu XButton/YButton pattern 2 (default)'),
    ('ps2', 'triangle', 'pause_main', 172, 99, 198, 120, 'pause SRText/StoryRoute button pattern 0'),
    ('xbox', 'y', 'pause_main', 200, 99, 226, 120, 'pause button pattern 1'),
    ('gamecube', 'y', 'pause_main', 229, 99, 255, 120, 'pause button pattern 2 (default)'),
    ('gamecube', 'x', 'VHArrow_gc', 48, 0, 84, 36, 'VHArrow button / button_light'),
    ('gamecube', 'x_glow_mask', 'VHArrow_gc', 2, 0, 44, 42, 'VHArrow circlebg: its white silhouette, drawn behind at 20 %'),
    ('ps2', 'start', 'Synopsis', 0, 25, 35, 53, 'Synopsis skip_btn pattern 0 (cell starts at x -13)'),
    ('gamecube', 'start_pause', 'Synopsis', 36, 25, 81, 53, 'Synopsis skip_btn pattern 1 (default)'),
    ('xbox', 'start', 'Synopsis', 82, 27, 128, 55, 'Synopsis skip_btn pattern 2'),
    ('ps2', 'square', 'windowitem', 44, 44, 84, 84, 'BbuttonWindow BButton (PS2 leftover on the GC disc)'),
    ('ps2', 'cross_badge_on_vehicle', 'windowitem', 86, 44, 126, 84, 'InfoWindow item icon with a PS2 cross badge'),
    ('ps2', 'square_badge_on_gun', 'windowitem', 86, 86, 126, 126, 'InfoWindow item icon with a PS2 square badge'),
    # The E3 demo sheet: no cast uses these; boxed by alpha, named by look.
    ('e3', 'ps2_cross', 'e3_gamemenu', 132, 208, 155, 231, 'E3RomTitle, no cast'),
    ('e3', 'x_grey', 'e3_gamemenu', 156, 209, 179, 230, 'E3RomTitle, no cast'),
    ('e3', 'a_green', 'e3_gamemenu', 180, 209, 203, 230, 'E3RomTitle, no cast'),
    ('e3', 'a_light_green', 'e3_gamemenu', 204, 208, 228, 231, 'E3RomTitle, no cast'),
    ('e3', 'ps2_triangle', 'e3_gamemenu', 132, 232, 155, 255, 'E3RomTitle, no cast'),
    ('e3', 'ps2_circle', 'e3_gamemenu', 156, 232, 179, 255, 'E3RomTitle, no cast'),
    ('e3', 'y_black', 'e3_gamemenu', 180, 233, 203, 254, 'E3RomTitle, no cast'),
    ('e3', 'y_yellow', 'e3_gamemenu', 204, 233, 228, 255, 'E3RomTitle, no cast'),
    ('e3', 'b_red', 'e3_gamemenu', 229, 233, 252, 254, 'E3RomTitle, no cast'),
]


def embedded_txd(path):
    data = open(path, 'rb').read()
    start = data.find(b'\x16\x00\x00\x00', data.find(b'NEND'))
    size = struct.unpack_from('<I', data, start + 4)[0]
    return data[start:start + 12 + size]


def load_sheet(files, where, name):
    from PIL import Image
    if where.startswith('emb:'):
        import tempfile
        with tempfile.NamedTemporaryFile(suffix='.txd', delete=False) as f:
            f.write(embedded_txd(os.path.join(files, where[4:])))
        texs = parse_txd(f.name)
        os.unlink(f.name)
    else:
        texs = parse_txd(os.path.join(files, where))
    for t in texs:
        if t['name'] == name:
            rgba = decode(t['format'], t['palette_format'], t['_palette'], t['_pixels'],
                          t['width'], t['height'])
            return Image.frombytes('RGBA', (t['width'], t['height']), bytes(rgba))
    raise KeyError('%s not in %s' % (name, where))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game-files', default='H:/ShadowTheHedgehogDiskGC_Extract/game/files')
    ap.add_argument('--out', default='H:/ShadowTheHedgehogDiskGC_Extract/exports/button-layouts/original')
    args = ap.parse_args()
    from PIL import Image, ImageDraw

    os.makedirs(os.path.join(args.out, 'atlases'), exist_ok=True)
    sheets = {}
    for sid, (where, name) in SHEETS.items():
        sheets[sid] = load_sheet(args.game_files, where, name)
        sheets[sid].save(os.path.join(args.out, 'atlases', sid + '.png'))

    lines = ['Controller button glyphs on the PAL GameCube disc (GUPP8P). Extracted by',
             'source/tools/button_layouts/extract_buttons.py. Rect = x0 y0 x1 y1 in the sheet.', '']
    lines += ['Sheets:'] + ['  %-17s %s : %s (%dx%d)' % (sid, w, n, *sheets[sid].size)
                            for sid, (w, n) in SHEETS.items()] + ['']
    crops = []
    seen = {}
    for plat, button, sid, x0, y0, x1, y1, used in GLYPHS:
        im = sheets[sid].crop((x0, y0, min(x1, sheets[sid].width), min(y1, sheets[sid].height)))
        fname = '%s__%s.png' % (button, sid)
        os.makedirs(os.path.join(args.out, plat), exist_ok=True)
        im.save(os.path.join(args.out, plat, fname))
        crops.append((plat, button, sid, im))
        lines.append('%-9s %-24s %-17s %3d %3d %3d %3d  %s' % (plat, button, sid, x0, y0, x1, y1, used))
        seen.setdefault(plat, []).append(button)
    open(os.path.join(args.out, 'index.txt'), 'w', encoding='utf-8', newline='\n').write(
        '\n'.join(lines) + '\n')

    # Contact sheet: one row per platform, glyphs at 3x on the menu's dark blue.
    order = ['gamecube', 'ps2', 'xbox', 'unknown', 'e3']
    scale, cell, label_w = 3, 140, 110
    rows = [[c for c in crops if c[0] == p] for p in order]
    width = label_w + cell * max(len(r) for r in rows)
    sheet = Image.new('RGBA', (width, cell * len(rows)), (40, 44, 70, 255))
    d = ImageDraw.Draw(sheet)
    for r, row in enumerate(rows):
        d.text((8, r * cell + cell // 2 - 6), order[r], fill=(255, 255, 255, 255))
        for i, (_p, button, sid, im) in enumerate(row):
            big = im.resize((im.width * scale, im.height * scale), Image.NEAREST)
            x = label_w + i * cell + (cell - big.width) // 2
            sheet.alpha_composite(big, (x, r * cell + 6))
            d.text((label_w + i * cell + 4, r * cell + cell - 26), button[:20], fill=(255, 255, 160, 255))
            d.text((label_w + i * cell + 4, r * cell + cell - 14), sid[:20], fill=(170, 180, 220, 255))
    sheet.save(os.path.join(args.out, '_contact_sheet.png'))
    print('%d glyphs from %d sheets -> %s' % (len(crops), len(sheets), args.out))


if __name__ == '__main__':
    main()
