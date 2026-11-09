"""The Hungarian alphabet of a face: real letters, accented letters composed from the face's own letters and accents,
and letters built from parts of the face's own letters (per-face recipes in recipes.py).

Accents are never drawn: an accent is a separate fill component above a letter in one of the face's ORIGINAL accented
letters (é á ó ú í / É Á ...: acute; ö ü ä ë ï / Ö Ü Ä: diaeresis; è à ò ù ì: grave; ê â ô û î: circumflex). It is put
over the target letter with the source's own gap above the letter and its own horizontal offset from the letter's
centre. Acute missing in a case -> the other case's acute (noted); no acute at all -> a grave mirrored (noted).
Double acute = two copies of the face's acute, their centres as far apart as the face's diaeresis dots (or the acute's
width + 1 when the face has no diaeresis). í = the face's i without its dot + an acute.
"""
import glyphs as G

ALPHA_UP = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ'
ALPHA_LO = 'abcdefghijklmnopqrstuvwxyz'
HU_UP = 'ÁÉÍÓÖŐÚÜŰ'
HU_LO = 'áéíóöőúüű'
DIGITS = '0123456789'
PUNCT = '.,:;!?-\'"()/+%&'
HU_ACCENT = {'Á': ('A', 'acute'), 'É': ('E', 'acute'), 'Í': ('I', 'acute'), 'Ó': ('O', 'acute'), 'Ö': ('O', 'dia'),
             'Ő': ('O', 'double'), 'Ú': ('U', 'acute'), 'Ü': ('U', 'dia'), 'Ű': ('U', 'double'),
             'á': ('a', 'acute'), 'é': ('e', 'acute'), 'í': ('i', 'acute'), 'ó': ('o', 'acute'), 'ö': ('o', 'dia'),
             'ő': ('o', 'double'), 'ú': ('u', 'acute'), 'ü': ('u', 'dia'), 'ű': ('u', 'double')}
SOURCES = {'acute': 'éáóúíÉÁÓÚÍ', 'dia': 'öüäëïÖÜÄËÏ', 'grave': 'èàòùìÈÀÒÙÌ', 'circ': 'êâôûîÊÂÔÛÎ'}
PLAIN = {'é': 'e', 'á': 'a', 'ó': 'o', 'ú': 'u', 'í': 'i', 'É': 'E', 'Á': 'A', 'Ó': 'O', 'Ú': 'U', 'Í': 'I',
         'ö': 'o', 'ü': 'u', 'ä': 'a', 'ë': 'e', 'ï': 'i', 'Ö': 'O', 'Ü': 'U', 'Ä': 'A', 'Ë': 'E', 'Ï': 'I',
         'è': 'e', 'à': 'a', 'ò': 'o', 'ù': 'u', 'ì': 'i', 'È': 'E', 'À': 'A', 'Ò': 'O', 'Ù': 'U', 'Ì': 'I',
         'ê': 'e', 'â': 'a', 'ô': 'o', 'û': 'u', 'î': 'i', 'Ê': 'E', 'Â': 'A', 'Ô': 'O', 'Û': 'U', 'Î': 'I'}


def components(pix):
    left = set(pix)
    out = []
    while left:
        p = left.pop()
        comp, stack = {p}, [p]
        while stack:
            x, y = stack.pop()
            for dx in (-1, 0, 1):
                for dy in (-1, 0, 1):
                    q = (x + dx, y + dy)
                    if q in left:
                        left.remove(q); comp.add(q); stack.append(q)
        out.append(comp)
    return out


def bbox(pts):
    xs = [x for x, _ in pts]; ys = [y for _, y in pts]
    return min(xs), max(xs), min(ys), max(ys)


def split_marks(gl):
    """-> (body pixels, [mark pixel sets above the body]) of a glyph (marks: components wholly above the body)."""
    comps = sorted(components(gl.pix), key=len, reverse=True)
    body = set(comps[0])
    top = bbox(body)[2]
    marks = []
    for c in comps[1:]:
        if bbox(c)[3] < top:
            marks.append(c)
        else:
            body |= c
            top = min(top, bbox(c)[2])
    # a mark that was absorbed because it touches nothing above: fine; marks sorted left to right
    marks.sort(key=lambda c: bbox(c)[0])
    return body, marks


class Accent(object):
    def __init__(self, kind, pix, src_glyph, dx_centre, gap, note=''):
        self.kind, self.pix, self.src, self.dx_centre, self.gap, self.note = kind, pix, src_glyph, dx_centre, gap, note

    def width(self):
        x0, x1, _, _ = bbox(self.pix)
        return x1 - x0 + 1


def find_accents(lib):
    """-> {(kind, 'up'|'lo'): Accent} from the face's own accented letters."""
    found = {}
    for kind, chars in SOURCES.items():
        for ch in chars:
            gl = lib.g.get(ch)
            if gl is None:
                continue
            body, marks = split_marks(gl)
            if not marks:
                continue
            if kind == 'dia' and len(marks) < 2:
                continue
            case = 'up' if ch.isupper() else 'lo'
            if (kind, case) in found:
                continue
            mk = set().union(*marks) if kind == 'dia' else max(marks, key=len)
            bx0, bx1, by0, by1 = bbox(body)
            mx0, mx1, my0, my1 = bbox(mk)
            pix = {(x - mx0, y - my1): gl.pix[(x, y)] for (x, y) in mk}      # left edge 0, bottom row 0
            acc = Accent(kind, pix, gl, (mx0 + mx1) / 2.0 - (bx0 + bx1) / 2.0, by0 - my1 - 1)
            acc.char = ch
            if kind == 'dia':
                cs = [bbox(m) for m in marks[:2]]
                acc.dot_dist = (cs[1][0] + cs[1][1]) / 2.0 - (cs[0][0] + cs[0][1]) / 2.0
            # is the accented letter's body smaller than the plain letter (the game's squeezed accented capitals)?
            plain = lib.g.get(PLAIN.get(ch, ''))
            if plain is not None:
                pb = bbox(split_marks(plain)[0])
                acc.body_h = (by1 - by0 + 1, pb[3] - pb[2] + 1)
            found[(kind, case)] = acc
    return found


def mirrored(acc):
    x0, x1, _, _ = bbox(acc.pix)
    a = Accent('acute', {(x1 - x, y): v for (x, y), v in acc.pix.items()}, acc.src, -acc.dx_centre, acc.gap,
               'mirrored grave from %s' % acc.char)
    a.char = acc.char
    return a


def acute_for(accs, case):
    other = 'lo' if case == 'up' else 'up'
    if ('acute', case) in accs:
        return accs[('acute', case)]
    if ('acute', other) in accs:
        a = accs[('acute', other)]
        b = Accent('acute', a.pix, a.src, a.dx_centre, a.gap, 'acute of the other case (%s)' % a.char)
        b.char = a.char
        return b
    for c in (case, other):
        if ('grave', c) in accs:
            return mirrored(accs[('grave', c)])
    return None


def dia_for(accs, case):
    other = 'lo' if case == 'up' else 'up'
    for c in (case, other):
        if ('dia', c) in accs:
            a = accs[('dia', c)]
            if c != case:
                b = Accent('dia', a.pix, a.src, a.dx_centre, a.gap, 'diaeresis of the other case (%s)' % a.char)
                b.char, b.dot_dist = a.char, a.dot_dist
                return b
            return a
    return None


def place(base_gl, acc, strength, double_dist=None, drop_dot=False, ch='?'):
    """The base letter + the accent (or two acutes) -> a new Glyph."""
    pix = dict(base_gl.pix)
    if drop_dot:
        body, marks = split_marks(base_gl)
        pix = {k: v for k, v in pix.items() if k in body}
    body = split_marks(G.Glyph(ch, pix, base_gl.width, ''))[0]
    bx0, bx1, by0, by1 = bbox(body)
    cx = (bx0 + bx1) / 2.0 + acc.dx_centre
    w = acc.width()
    bottom = by0 - acc.gap - 1
    marks = []
    if double_dist is None:
        marks.append(int(round(cx - (w - 1) / 2.0)))
    else:
        for s in (-double_dist / 2.0, double_dist / 2.0):
            marks.append(int(round(cx + s - (w - 1) / 2.0)))
    for left in marks:
        for (x, y), v in acc.pix.items():
            k = (left + x, bottom + y)
            if k not in pix or strength(v) > strength(pix[k]):
                pix[k] = v
    minx = min(x for x, _ in pix)
    shift = -minx if minx < 0 else 0
    if shift:
        pix = {(x + shift, y): v for (x, y), v in pix.items()}
    width = max(base_gl.width + shift, max(x for x, _ in pix) + 1)
    return G.Glyph(ch, pix, width, '', 'composed')


def alphabet(lib, recipes=None):
    """-> (glyphs {char: Glyph}, info {char: text}) for the face's full target set."""
    face = lib.face
    out = {}
    info = {}
    for c, gl in lib.g.items():
        out[c] = gl
        gl.kind = 'real'
        info[c] = 'real: ' + gl.src + ((' (' + gl.note + ')') if gl.note else '')
    # letters built from parts (before the accents, which may need them)
    if recipes:
        for c, fn in recipes.items():
            if c in out:
                continue
            try:
                g = fn(out, lib)
            except KeyError as e:
                info[c] = 'MISSING: recipe needs %s' % e
                continue
            if g is not None:
                g.kind = 'drawn' if getattr(g, 'kind', '') in ('', 'real', 'drawn') else g.kind
                out[c] = g
                info[c] = '%s: %s' % (g.kind, g.note)
    accs = find_accents(lib)
    for case, ch in (('lo', 'i'), ('lo', 'j')):
        if ('dia', 'lo') in accs or ch not in lib.g:
            continue
        body, marks = split_marks(lib.g[ch])
        if not marks:
            continue
        gl = lib.g[ch]
        dot = max(marks, key=len)
        bx0, bx1, by0, by1 = bbox(body)
        mx0, mx1, my0, my1 = bbox(dot)
        dw = mx1 - mx0 + 1
        o = out.get('o') or out.get('u')
        bw = (bbox(split_marks(o)[0])[1] - bbox(split_marks(o)[0])[0] + 1) if o is not None else 3 * dw
        dist = max(dw + 2, int(round(bw * 0.45)))
        pix = {}
        for k, sx in enumerate((0, dist)):
            for (x, y) in dot:
                pix[(x - mx0 + sx, y - my1)] = gl.pix[(x, y)]
        a = Accent('dia', pix, gl, 0.0, by0 - my1 - 1, "two dots of the face's own %s (no diaeresis in the face)" % ch)
        a.char, a.dot_dist = ch, float(dist)
        accs[('dia', 'lo')] = a
    lib.accents = accs
    for ch, (base, kind) in HU_ACCENT.items():
        if ch in out:
            continue
        if face.caps_only and ch.islower():
            continue
        if getattr(face, 'lower_only', False) and ch.isupper():
            continue
        case = 'up' if ch.isupper() else 'lo'
        if base not in out:
            info[ch] = 'MISSING: no %s' % base
            continue
        if kind in ('acute', 'double'):
            acc = acute_for(accs, case)
        else:
            acc = dia_for(accs, case)
        if acc is None:
            info[ch] = 'MISSING: the face has no %s to take' % ('acute or grave' if kind != 'dia' else 'diaeresis')
            continue
        dd = None
        note = ''
        if kind == 'double':
            dia = dia_for(accs, case)
            if dia is not None:
                dd = max(dia.dot_dist, acc.width() - 1)
                note = 'two acutes %.1f px apart (the %s dots, at least the acute width - 1)' % (dd, dia.char)
            else:
                dd = acc.width() + 1
                note = 'two acutes %d px apart (no diaeresis in the face)' % dd
        g = place(out[base], acc, face.strength, dd, drop_dot=(base == 'i'), ch=ch)
        src = '%s from %s' % ({'acute': 'acute', 'double': 'acute', 'dia': 'diaeresis'}[kind], acc.char)
        if acc.note:
            src += ' [' + acc.note + ']'
        g.note = '%s + %s%s' % (base, src, ('; ' + note) if note else '')
        if base == 'i':
            g.note += '; dot removed'
        out[ch] = g
        info[ch] = 'composed: ' + g.note
    return out, info


def target_set(face):
    if getattr(face, 'lower_only', False):
        return ALPHA_LO + HU_LO + DIGITS + PUNCT
    s = ALPHA_UP + HU_UP + DIGITS + PUNCT
    if not face.caps_only:
        s += ALPHA_LO + HU_LO
    return s
