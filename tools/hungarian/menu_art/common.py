"""Paths, languages and small pixel helpers shared by the menu-art tools."""
import os
import sys

GAME_FILES = 'H:/ShadowTheHedgehogDiskGC_Extract/game/files'
OUT_DIR = 'H:/ShadowTheHedgehogDiskGC_Extract/wt-gc-removal/.build/hu-loc/menus/fontmaps'
LANGS = ('EN', 'FR', 'GE', 'IT', 'SP')          # the five Latin packs; EN first (preferred source)

try:
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
except Exception:  # noqa: BLE001
    pass


def txd_path(pack, lang, files=None):
    """pack 'honban' -> csdFiles/honban_EN.txd; 'pause/pause0100' -> csdFiles/pause/pause0100_EN.txd;
    'Result.gncp' -> csdFiles/Result_EN.gncp (a CSD layout with its texture dictionary embedded)."""
    if pack.endswith('.gncp'):
        return '%s/csdFiles/%s_%s.gncp' % (files or GAME_FILES, pack[:-5], lang)
    return '%s/csdFiles/%s_%s.txd' % (files or GAME_FILES, pack, lang)


_cache = {}


def texture(pack, tex, lang, files=None):
    """The txd.Texture; tex None = the dictionary's only texture (the gncp textures are renamed per language)."""
    from txd import Txd
    x = Txd(txd_path(pack, lang, files))
    return x.textures[0] if tex is None else x.get(tex)


def load(pack, tex, lang, files=None):
    """RGBA rows of texture `tex` in pack `pack` of language `lang` (cached)."""
    key = (pack, tex, lang, files)
    if key not in _cache:
        _cache[key] = texture(pack, tex, lang, files).rgba()
    return _cache[key]


def lum(p):
    return (p[0] + p[1] + p[2]) // 3


def over(p, bg=(40, 40, 40)):
    """Composite an RGBA pixel over a background colour -> RGB."""
    a = p[3]
    return tuple((p[i] * a + bg[i] * (255 - a)) // 255 for i in range(3))


def crop(img, x0, y0, x1, y1):
    return [row[x0:x1] for row in img[y0:y1]]


def scale(img, s):
    out = []
    for row in img:
        r = []
        for p in row:
            r += [p] * s
        for _ in range(s):
            out.append(list(r))
    return out


def ensure_out(sub=''):
    p = os.path.join(OUT_DIR, sub) if sub else OUT_DIR
    os.makedirs(p, exist_ok=True)
    return p
