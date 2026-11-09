"""The typefaces baked into the game's language pictures: where each line of text sits, what it says in each language,
and how the face's fill is told from its outline. Everything the library builder, the generator and the composer need
is here as data; composing a picture later = (face, picture, band, Hungarian string).

Boxes are (x0, y0, x1, y1) in texture pixels, end exclusive; one box holds one line. Texts are exactly what the
picture shows (checked by letter count when cutting).
"""
from glyphs import Band


def grey_on_dark(full, sat=40, floor=0):
    """Fill strength of a light grey/white fill over a black outline: luminance / the fill's full level."""
    def f(p):
        if p[3] < 128:
            return 0.0
        r, g, b = p[:3]
        if max(r, g, b) - min(r, g, b) > sat:
            return 0.0
        v = (r + g + b) / 3.0 - floor
        return max(0.0, min(1.0, v / float(full - floor)))
    return f


def alpha_fill(p):
    return p[3] / 255.0


def colour_fill(ref, outline=(0, 0, 0)):
    """Fill strength of a coloured fill over an outline colour: the pixel's position between the two."""
    span = sum((a - b) ** 2 for a, b in zip(ref, outline)) ** 0.5
    def f(p):
        if p[3] < 128:
            return 0.0
        d_out = sum((a - b) ** 2 for a, b in zip(p[:3], outline)) ** 0.5
        d_ref = sum((a - b) ** 2 for a, b in zip(p[:3], ref)) ** 0.5
        if d_ref > span * 1.05 and d_out > span * 0.5:
            return 0.0          # a colour that is neither (another element of the picture)
        return max(0.0, min(1.0, d_out / span))
    return f


class Face(object):
    def __init__(self, name, title, strength, bands, outline=None, caps_only=False, min_strength=0.1, speck=1.5,
                 speck_join=3, min_letter_w=3, exclude_langs=(), priority=9, notes='', needs=None, dp_split=False,
                 samples=(), seg='columns'):
        self.name, self.title, self.strength, self.bands = name, title, strength, bands
        self.outline = outline                  # None (fill on transparency) or {'rgb': ...} : regenerate
        self.caps_only = caps_only
        self.min_strength = min_strength
        self.speck, self.speck_join, self.min_letter_w = speck, speck_join, min_letter_w
        self.metric_lang = 'EN'
        self.exclude_langs = set(exclude_langs)
        self.priority = priority
        self.notes = notes
        self.needs = needs
        self.dp_split = dp_split        # touching letters: split the band by dynamic programming (grunge faces)
        self.samples = samples          # Hungarian sample lines for the review sheet
        self.seg = seg                  # 'columns' (column runs) or 'components' (2-D, for overlapping grunge)


def bands(pack, tex, table):
    """table: {LANG: [(box, text), ...]} -> [Band]"""
    out = []
    for lang, rows in table.items():
        for row in rows:
            box, text = row[0], row[1]
            extra = row[2] if len(row) > 2 else {}
            out.append(Band(pack, tex, lang, box, text, **extra))
    return out


FACES = {}


def face(*a, **k):
    f = Face(*a, **k)
    FACES[f.name] = f
    return f


# ---------------------------------------------------------------------------------------------------------- title
face('title_press', 'Title screen: PRESS START (honban_XX.txd TitleParts, RGB5A3)',
     grey_on_dark(231, sat=30), outline={'rgb': (0, 0, 0)}, priority=1,
     bands=bands('honban', 'TitleParts', {
         'EN': [((195, 234, 512, 256), 'PRESS START BUTTON'), ((195, 258, 512, 281), 'PRESS START')],
         'FR': [((195, 234, 512, 256), 'Appuyer sur la touche START'), ((195, 258, 512, 281), 'APPUYER SUR START')],
         'GE': [((195, 234, 512, 256), 'START-TASTE DRÜCKEN'), ((195, 258, 512, 281), 'START DRÜCKEN')],
         'IT': [((195, 234, 512, 256), 'PREMI IL TASTO START'), ((195, 258, 512, 281), 'PREMI START')],
         'SP': [((195, 234, 512, 256), 'PULSA EL BOTÓN START'), ((195, 258, 512, 281), 'PULSA START')],
     }),
     notes='The SEGA/NINTENDO legal lines above stay English (not translated).')


# ---------------------------------------------------------------------------------------------------------- main menu
MENU_ROWS = [(0, 21), (21, 43), (43, 62), (62, 85), (85, 106), (106, 128), (128, 150), (150, 171), (171, 200)]
MENU_TEXT = {
    'EN': ['1P GAME', '2P GAME', 'OPTIONS', 'STORY MODE', 'SELECT MODE', 'EXPERT MODE', 'NEW', 'RESUME', 'CONTINUE'],
    'FR': ['JEU 1J', 'JEU 2J', 'OPTIONS', 'MODE HISTOIRE', 'MODE SÉLECTION', 'MODE EXPERT', 'NOUVEAU', 'REPRENDRE'],
    'GE': ['1 SP SPIEL', '2 SP SPIEL', 'OPTIONEN', 'STORY-MODUS', 'MODUS WÄHLEN', 'EXPERTEN-MODUS', 'NEU', 'FORTSETZEN'],
    'IT': ['PARTITA 1G', 'PARTITA 2G', 'OPZIONI', 'MODALITÀ STORIA', 'MODALITÀ SELEZ.', 'MODALITÀ ESPERTO', 'NUOVA',
           'RIPRENDI'],
    'SP': ['PARTIDA 1J', 'PARTIDA 2J', 'OPCIONES', 'MODO HISTORIA', 'MODO SELECCIONAR', 'MODO EXPERTO', 'NUEVO',
           'REANUDAR'],
}
face('menu_caps', 'Main menu items: 1P GAME / OPTIONS ... (honban_XX.txd gamemenu 256x256 C8; the same picture is in '
     'Options, SelectMode, Story_Library and BattleMode)', grey_on_dark(255, sat=30), outline={'rgb': (0, 0, 0)},
     caps_only=True, priority=2,
     bands=bands('honban', 'gamemenu', {l: [((0, y0, 232, y1), t) for (y0, y1), t in zip(MENU_ROWS, ts)]
                                       for l, ts in MENU_TEXT.items()}))

PROMPTS = {'EN': ('select', 'exit'), 'FR': ('confirmer', 'quitter'), 'GE': ('wählen', 'beenden'),
           'IT': ('seleziona', 'esci'), 'SP': ('elegir', 'salir')}
face('prompt', 'Button prompts: select / exit (gamemenu picture; also in Options, SelectMode, Story_Library)',
     grey_on_dark(255, sat=30), outline={'rgb': (0, 0, 0)}, priority=3,
     bands=bands('honban', 'gamemenu', {l: [((0, 200, 125, 232), a), ((0, 232, 125, 256), b)]
                                       for l, (a, b) in PROMPTS.items()}))


def ink_on_black(full, alpha_min=128):
    """Fill strength of any fill colour over a black outline: brightest channel / the fill's full level."""
    def f(p):
        if p[3] < alpha_min:
            return 0.0
        return max(0.0, min(1.0, max(p[:3]) / float(full)))
    return f


def alpha_white(p):
    """White (or grey) fill on transparency, no outline: alpha x brightness."""
    return (p[3] / 255.0) * (max(p[:3]) / 255.0)


import faces_more  # noqa: E402,F401  (the later screens)
