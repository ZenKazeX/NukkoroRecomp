#!/usr/bin/env python3
"""Animated preview of the Chaos power prompt (Hissatu_Hero / Hissatu_Dark), original and modern.

    python power_prompt_preview.py

When Shadow's Hero or Dark gauge fills, Hissatu_Hero.gncp ("Chaos Control") or Hissatu_Dark.gncp
("Chaos Blast") shows a 32x32 button from 000_squarebuttons (cells: 1 Xbox Y, 2 PS2 triangle,
3 GC Y; cell 0 is empty). From the layout data:

    XButton / YButton  the button, at (431, 62) Hero / (213, 62) Dark on the 640x480 canvas
    Eff_X / Eff_Y      its child, same cell, centred on it
    OpenHero/OpenDark  the button fades in, alpha 0 -> 255 over 10 frames (60 fps)
    Chaos_Control/_Blast  the child grows 1x -> 2x while fading 255 -> 0 over 10 frames: the pulse
    CloseHero/CloseDark   fade out over 10 frames

Whether the game replays the pulse motion in a loop is not read from the code here; the preview
repeats it. Writes exports/button-layouts/power_prompt_<set>.gif (3x, 50 fps - GIF cannot do 60).
"""
import os

ROOT = 'H:/ShadowTheHedgehogDiskGC_Extract/exports/button-layouts'


def frames(button, scale=3, box=72):
    from PIL import Image
    out = []
    bg = (34, 30, 40, 255)

    def frame(base_a, eff_s, eff_a):
        im = Image.new('RGBA', (box, box), bg)
        c = box / 2
        if eff_a > 0:
            n = max(1, round(32 * eff_s))
            e = button.resize((n, n), Image.BILINEAR)
            e.putalpha(e.getchannel('A').point(lambda v: v * eff_a // 255))
            im.alpha_composite(e, (int(c - n / 2), int(c - n / 2)))
        b = button.copy()
        b.putalpha(b.getchannel('A').point(lambda v: v * base_a // 255))
        im.alpha_composite(b, (int(c - 16), int(c - 16)))
        return im.resize((box * scale, box * scale), Image.NEAREST)

    for f in range(11):                              # Open: fade in
        out.append(frame(255 * f // 10, 1, 0))
    for _ in range(4):                               # Chaos_Control: pulse, repeated
        for f in range(11):
            out.append(frame(255, 1 + f / 10, 255 - 255 * f // 10))
    for f in range(11):                              # Close: fade out
        out.append(frame(255 - 255 * f // 10, 1, 0))
    return out


def main():
    from PIL import Image
    sets = {'original_gamecube': Image.open(ROOT + '/original/gamecube/y__squarebuttons.png')}
    for fam in ('xbox', 'playstation', 'switch'):
        sheet = Image.open('%s/modern/%s/atlases/squarebuttons.png' % (ROOT, fam))
        sets['modern_' + fam] = sheet.crop((32, 32, 64, 64))     # GC Y cell, as repainted
    for name, button in sets.items():
        button = button.convert('RGBA')
        fr = frames(button)
        fr[0].save('%s/power_prompt_%s.gif' % (ROOT, name), save_all=True, append_images=fr[1:],
                   duration=20, loop=0, disposal=2)
    print('wrote', len(sets), 'previews to', ROOT)


if __name__ == '__main__':
    main()
