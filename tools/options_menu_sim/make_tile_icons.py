#!/usr/bin/env python3
"""The PC Options menu's new category tiles, in the style of the game's own six (docs/OPTIONS_MENU_OVERHAUL.md).

    python make_tile_icons.py      # after build_sim_data.py

The six-category grid (user directive of 2026-11-05; grid: Display / Graphics, Audio / Controls, Save Data / System).
The user approved the reference image's symbols on 2026-11-05 and asked for them in the game's style: each symbol is
cut into the game's own worn plate (the Load tile's, as make_display_icon.py made the approved Display tile), lit like
the game's symbols, its face the game's measured grey (normal) and red (selected). No text labels, as on the
original tiles.

  * Display:  a modern flat monitor (the CRT tile retired, user 2026-11-05);
  * Graphics, Audio, Controls, System: the reference tiles' silhouettes (sliders, speaker, gamepad, gear);
  * Save Data: a floppy disk, the user's choice over a memory card and the game's save emblem (the slot that held Gameplay: the user moved the camera rows to Controls and chose Save
    Data for it on 2026-11-05 - the game's Load and autosave switch; a music note was rejected as misleading, music and
    movies belong in the front menu).

Writes generated/tex/icon_{display,graphics,audio,controls,savedata,system}{,_hi}.png and the same pairs under
exports/options-menu-textures/new-category-tiles/ (emit_tile_art_inc.py's input).
"""
import argparse
import os

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

import make_display_icon as base

HERE = base.HERE
TEX = base.TEX
ART = os.path.normpath(os.path.join(HERE, '..', '..', '..', 'exports', 'options-menu-textures', 'new-category-tiles'))
K = base.K


def sliders_layers(size):
    """Three horizontal slider tracks, each with a knob at a different place (the reference image's picture
    symbol): (cut, line, core) masks at K x the tile, strokes as thick as the game's symbols."""
    s = size * K
    u = s / 82.0

    def box(x0, y0, x1, y1):
        return [x0 * u, y0 * u, x1 * u, y1 * u]

    cut = Image.new('L', (s, s), 0)
    line = Image.new('L', (s, s), 0)
    core = Image.new('L', (s, s), 0)
    dc, dl, dk = ImageDraw.Draw(cut), ImageDraw.Draw(line), ImageDraw.Draw(core)
    for y, knob in ((26.0, 50.0), (41.0, 30.0), (56.0, 56.0)):
        dc.rounded_rectangle(box(14, y - 2.6, 68, y + 2.6), radius=2.6 * u, fill=255)   # the track
        dc.rounded_rectangle(box(knob - 6.5, y - 7.5, knob + 6.5, y + 7.5), radius=3.0 * u, fill=255)  # the knob
        dk.rounded_rectangle(box(knob - 3.6, y - 4.6, knob + 3.6, y + 4.6), radius=1.8 * u, fill=255)  # its face
        dl.rectangle(box(knob - 0.6, y - 3.4, knob + 0.6, y + 3.4), fill=255)          # its grip line
    return cut, line, core


def gear_layers(size):
    """A cog wheel: ring with eight teeth, a hub disc and a bore - (cut, line, core) at K x the tile."""
    s = size * K
    u = s / 82.0
    cx, cy = 41.0, 41.0
    cut = Image.new('L', (s, s), 0)
    d = ImageDraw.Draw(cut)
    teeth = 8
    outer, root, tooth_w = 27.5, 21.0, 8.4
    for i in range(teeth):
        a = 2 * np.pi * i / teeth
        ca, sa = np.cos(a), np.sin(a)
        # a tooth: a rectangle from the root circle to the outer radius, along angle a
        pts = []
        for r, t in ((root - 2, -tooth_w / 2), (outer, -tooth_w / 2 * 0.82), (outer, tooth_w / 2 * 0.82),
                     (root - 2, tooth_w / 2)):
            pts.append(((cx + r * ca - t * sa) * u, (cy + r * sa + t * ca) * u))
        d.polygon(pts, fill=255)
    d.ellipse([(cx - root) * u, (cy - root) * u, (cx + root) * u, (cy + root) * u], fill=255)
    d.ellipse([(cx - 12.5) * u, (cy - 12.5) * u, (cx + 12.5) * u, (cy + 12.5) * u], fill=0)   # the inner rim
    d.ellipse([(cx - 9.0) * u, (cy - 9.0) * u, (cx + 9.0) * u, (cy + 9.0) * u], fill=255)     # the hub
    d.ellipse([(cx - 4.0) * u, (cy - 4.0) * u, (cx + 4.0) * u, (cy + 4.0) * u], fill=0)       # the bore
    line = Image.new('L', (s, s), 0)
    dl = ImageDraw.Draw(line)
    dl.ellipse([(cx - 17.0) * u, (cy - 17.0) * u, (cx + 17.0) * u, (cy + 17.0) * u], outline=255, width=int(1.1 * u))
    core = Image.new('L', (s, s), 0)
    dk = ImageDraw.Draw(core)
    dk.ellipse([(cx - 7.0) * u, (cy - 7.0) * u, (cx + 7.0) * u, (cy + 7.0) * u], fill=255)
    dk.ellipse([(cx - 4.0) * u, (cy - 4.0) * u, (cx + 4.0) * u, (cy + 4.0) * u], fill=0)
    return cut, line, core


REFERENCE = os.path.normpath(os.path.join(HERE, '..', '..', '..', 'Options menu redesign', 'optinons overhaul.png'))
# The user's reference tiles (1448 x 1086 image) whose symbols the user approved on 2026-11-05.
REFERENCE_TILES = {'display': (160, 230, 355, 430), 'graphics': (368, 230, 562, 430), 'audio': (160, 443, 355, 643),
                   'controls': (368, 443, 562, 643), 'system': (370, 657, 565, 857)}


def components(mask):
    """Connected components (4-neighbour) of a boolean array: list of (pixel index arrays, bbox)."""
    from collections import deque
    h, w = mask.shape
    seen = np.zeros_like(mask, bool)
    out = []
    for y0 in range(h):
        for x0 in range(w):
            if not mask[y0, x0] or seen[y0, x0]:
                continue
            q = deque([(y0, x0)])
            seen[y0, x0] = True
            ys, xs = [], []
            while q:
                y, x = q.popleft()
                ys.append(y); xs.append(x)
                for ny, nx in ((y + 1, x), (y - 1, x), (y, x + 1), (y, x - 1)):
                    if 0 <= ny < h and 0 <= nx < w and mask[ny, nx] and not seen[ny, nx]:
                        seen[ny, nx] = True
                        q.append((ny, nx))
            out.append((np.array(ys), np.array(xs)))
    return out


def silhouette_from_reference(name, fill_below=60):
    """The symbol of a reference tile as a boolean mask: its dark pixels, without the plate's frame, screws, grime
    and the label under the symbol (the game's tiles carry no text)."""
    im = Image.open(REFERENCE).convert('RGB').crop(REFERENCE_TILES[name])
    a = np.asarray(im, dtype=np.float32).mean(axis=2)
    dark = a < 72
    h, w = dark.shape
    keep = np.zeros_like(dark)
    for ys, xs in components(dark):
        if len(ys) < 150:
            continue
        y0, y1, x0, x1 = ys.min(), ys.max(), xs.min(), xs.max()
        if y0 < 12 or x0 < 12 or x1 > w - 13 or y1 > 140:   # frame, label (y > 140) or touching the border
            continue
        keep[ys, xs] = True
    # Small holes (the reference's glare and grime inside a symbol, a few pixels) are filled; real openings (the gear's bore,
    # the pad's buttons) are larger and stay.
    for ys, xs in components(~keep):
        if len(ys) < fill_below and ys.min() > 0 and xs.min() > 0 and ys.max() < h - 1 and xs.max() < w - 1:
            keep[ys, xs] = True
    return keep


def monitor_layers(size):
    """A modern flat monitor (the reference's Display symbol, drawn clean): bezel ring, screen with an inset line,
    neck and foot - (cut, line, core) at K x the tile."""
    s = size * K
    u = s / 82.0
    cut = Image.new('L', (s, s), 0)
    d = ImageDraw.Draw(cut)
    b = lambda x0, y0, x1, y1: [x0 * u, y0 * u, x1 * u, y1 * u]
    d.rounded_rectangle(b(12, 15, 70, 55), radius=3.5 * u, fill=255)      # bezel
    d.rounded_rectangle(b(17.5, 20.5, 64.5, 49.5), radius=1.5 * u, fill=0)  # screen opening
    d.polygon([(37 * u, 55 * u), (45 * u, 55 * u), (47 * u, 63 * u), (35 * u, 63 * u)], fill=255)  # neck
    d.rounded_rectangle(b(26, 62, 56, 67), radius=2.0 * u, fill=255)      # foot
    line = Image.new('L', (s, s), 0)
    dl = ImageDraw.Draw(line)
    dl.rounded_rectangle(b(21, 24, 61, 46), radius=1.0 * u, outline=255, width=max(1, int(1.0 * u)))
    return cut, line, Image.new('L', (s, s), 0)


def symbol_layers_from_mask(mask, size, box=56.0, line=None):
    """(cut, line, core) at K x the tile from a boolean symbol mask (and an optional inner-line mask in the same
    frame): fitted into a `box` px square on the 82 px plate, centred, smoothed when enlarged."""
    ys, xs = np.nonzero(mask)
    y0, y1, x0, x1 = ys.min(), ys.max() + 1, xs.min(), xs.max() + 1
    s = size * K
    scale = box * K / max(x1 - x0, y1 - y0)
    tw, th = max(1, round((x1 - x0) * scale)), max(1, round((y1 - y0) * scale))
    ox, oy = (s - tw) // 2, (s - th) // 2

    def place(m):
        crop = Image.fromarray((m[y0:y1, x0:x1] * 255).astype('uint8'), 'L').resize((tw, th), Image.BICUBIC)
        crop = crop.filter(ImageFilter.GaussianBlur(K * 0.35)).point(lambda v: 255 if v >= 128 else 0)
        out = Image.new('L', (s, s), 0)
        out.paste(crop, (ox, oy))
        return out

    cut = place(mask)
    lines = place(line) if line is not None else Image.new('L', (s, s), 0)
    return cut, lines, Image.new('L', (s, s), 0)


def floppy_layers(size):
    """The Save Data tile's symbol (the user's choice of 2026-11-05): a floppy disk - a square body with a clipped
    top-right corner, the metal shutter across its top with its slot standing proud, and the label below - (cut, line,
    core) at K x the tile, strokes as thick as the game's."""
    s = size * K
    u = s / 82.0
    cut = Image.new('L', (s, s), 0)
    d = ImageDraw.Draw(cut)
    x0, y0, x1, y1, clip, w = 15.0, 14.0, 67.0, 66.0, 8.0, 4.4
    d.polygon([(x0 * u, y0 * u), ((x1 - clip) * u, y0 * u), (x1 * u, (y0 + clip) * u), (x1 * u, y1 * u),
               (x0 * u, y1 * u)], fill=255)
    d.polygon([((x0 + w) * u, (y0 + w) * u), ((x1 - clip - w * 0.4) * u, (y0 + w) * u),
               ((x1 - w) * u, (y0 + clip + w * 0.4) * u), ((x1 - w) * u, (y1 - w) * u), ((x0 + w) * u, (y1 - w) * u)],
              fill=0)
    d.rectangle([27.0 * u, (y0 + w) * u, 52.0 * u, 33.0 * u], fill=255)          # the shutter
    d.rectangle([24.0 * u, 41.0 * u, 58.0 * u, (y1 - w) * u], fill=255)          # the label's recess
    core = Image.new('L', (s, s), 0)
    ImageDraw.Draw(core).rectangle([42.0 * u, (y0 + w + 2.6) * u, 48.0 * u, 30.4 * u], fill=255)  # the shutter slot
    line = Image.new('L', (s, s), 0)
    dl = ImageDraw.Draw(line)
    for y in (48.0, 53.5):                                                         # ruling on the label
        dl.line([(28.0 * u, y * u), (54.0 * u, y * u)], fill=255, width=max(1, int(1.0 * u)))
    return cut, line, core


def main():
    argparse.ArgumentParser(description=__doc__.split('\n\n')[0]).parse_args()
    os.makedirs(ART, exist_ok=True)
    sheet = Image.open(os.path.join(TEX, 'Options_EN__2.png'))
    cells = base.load_cells()
    normals = [base.crop(sheet, cells[c]) for c in base.NORMAL_CELLS]
    highlights = [base.crop(sheet, cells[c + base.HIGHLIGHT_OFFSET]) for c in base.NORMAL_CELLS]
    grey, red = base.symbol_colours(normals, highlights)
    plate = base.worn_plate(normals, highlights, base_index=4)
    plate_selected = base.worn_plate(highlights, highlights, base_index=4)
    size = normals[0].size[0]
    layers = {}
    layers['display'] = monitor_layers(size)
    for name in ('graphics', 'audio', 'controls', 'system'):
        # The pad's face buttons are 33..47 px openings in the reference: only smaller specks are glare there.
        layers[name] = symbol_layers_from_mask(silhouette_from_reference(name, 20 if name == 'controls' else 60), size)
    layers['savedata'] = floppy_layers(size)
    for name, lay in layers.items():
        normal = base.engrave(plate, lay, grey)
        selected = base.engrave(plate_selected, lay, red)
        normal.save(os.path.join(TEX, 'icon_%s.png' % name))
        selected.save(os.path.join(TEX, 'icon_%s_hi.png' % name))
        normal.save(os.path.join(ART, '%s.png' % name))
        selected.save(os.path.join(ART, '%s_selected.png' % name))
    print('symbol grey %s, red %s -> %s' % (grey, red, ART))


if __name__ == '__main__':
    main()
