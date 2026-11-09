"""Building a face's glyph library from all its bands (all five Latin languages), with the cut report."""
import common as C
import glyphs as G

_libs = {}


def build(face, files=None):
    if face.name in _libs:
        return _libs[face.name]
    lib = G.Library(face)
    lib.cuts = []
    order = sorted(face.bands, key=lambda b: C.LANGS.index(b.lang))
    for band in order:
        cut = G.cut_band(face, band, files)
        lib.add_cut(cut)
        if 'error' not in cut:
            lib.cuts.append(cut)
            lib.bases.append((band, cut['base']))
    lib.fill_from_excluded()
    _libs[face.name] = lib
    return lib
