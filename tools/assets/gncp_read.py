#!/usr/bin/env python3
"""Read a Chao::CSD .gncp project: scenes, cells, casts, and which texture each cast uses.

Format (see GekkoForge/docs/2D_LAYOUT_ARCHITECTURE.md section 1.2). Pointers and the NOF0
relocation offsets are relative to the "nCPJ" chunk header; the payload is big-endian.

    project header at nCPJ+0x20 - TWO tables, see the comment on scenes():
        +0x20 u32 sceneCount
        +0x24 u32 sceneDescPtrArray   (array of u32 pointers, one per scene)
        +0x28 u32 sceneNamePtrArray   (8 bytes per entry: namePtr, index - ALPHABETICAL, so the
                                       index field, not the position, selects the descriptor)
        +0x2C u32 groupCount          (Story_Library / RouteDisplay use these three instead)
        +0x30 u32 groupArrayPtr       (24 bytes each: the same count/desc/name triple, padded)
        +0x34 u32 groupNamePtrArray
    All pointers are relative to the nCPJ CHUNK START (the "nCPJ" magic itself), not to the
    file and not to the payload after the 8-byte chunk header.
    scene descriptor:
        +0x14/+0x18  texture sizes  count, ptr   (8 bytes: normalised w, h)
        +0x1C/+0x20  cells          count, ptr   (20 bytes: texIdx, u0, v0, u1, v1)
        +0x24/+0x28  groups         count, ptr   (16 bytes: nCast, castPtrArray, 0, pairArray)
        +0x2C/+0x30  cast names     count, ptr   (12 bytes: namePtr, groupIdx, indexInGroup)
        +0x40        authored aspect (1.3333334)

    cast record, 80 bytes:
        +0x0C..+0x28  four quad corners (x, y) normalised, about the cast's own origin
        +0x30         POINTER to the transform block - it is NOT inline:
                          +0x00 pad/flags   +0x04 x   +0x08 y   +0x0C rotation in DEGREES
                          +0x10 scaleX      +0x14 scaleY        +0x1C RGBA
        +0x40         POINTER to the pattern (cell index) array; first entry -1 = UNTEXTURED

    The corners are local: the on-canvas box is corner * scale, rotated by the angle, then
    translated by (x, y). Reading +0x30 as an inline block (as an earlier draft did) leaves every
    translation at zero and every box centred on the origin.

THE AUTHORING UNIT IS x * 640, y * 480 - not 1/480 on both axes. Confirmed by checking the texture
size table against the real dimensions in the paired .txd.
"""
import math
import struct
import sys

try:                       # cast names are Shift-JIS; do not die on a cp1250 console
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
except Exception:          # noqa: BLE001
    pass


class Gncp(object):
    def __init__(self, path):
        self.path = path
        self.data = open(path, 'rb').read()
        self.base = self.data.find(b'nCPJ')
        if self.base < 0:
            raise ValueError('no nCPJ chunk in %s' % path)

    def u32(self, off):
        return struct.unpack_from('>I', self.data, self.base + off)[0]

    def i32(self, off):
        return struct.unpack_from('>i', self.data, self.base + off)[0]

    def f32(self, off):
        return struct.unpack_from('>f', self.data, self.base + off)[0]

    def cstr(self, off):
        if off <= 0 or self.base + off >= len(self.data):
            return ''
        end = self.data.index(b'\0', self.base + off)
        return self.data[self.base + off:end].decode('shift_jis', 'replace')

    # The project header carries TWO scene tables:
    #   +0x20 {count, descPtrArray, namePtrArray}   - a FLAT list of scenes
    #   +0x2C {count, groupArrayPtr, namePtrArray}  - a list of scene GROUPS, each group being a
    #         24-byte {count, descPtrArray, namePtrArray, 0, 0, 0}: a flat list of its own
    # Most projects fill the flat table and zero the group table; Story_Library (6 groups holding
    # 9 scenes) and RouteDisplay do the opposite. Reading only +0x20 reports those two as having
    # no scenes at all - which is how their background plates went missing from the first pass.
    # Group scenes validate the same way as flat ones: aspect 1.3333334 at desc+0x40.
    #
    # In BOTH tables the name array is 8 bytes per entry {namePtr, sceneIndex} and is sorted
    # ALPHABETICALLY, so the i-th name is NOT the i-th descriptor - the index field must be used.
    # It is the identity for every flat-table project, which is why pairing by position appeared
    # to work, but Story_Library's groups are Background=1, Buttons=5, Library=4, Loop=0,
    # StoryMode=2, StoryRoute=3.

    def _names(self, ptr, count):
        if not ptr:
            return {}
        return {self.u32(ptr + 8 * i + 4): self.cstr(self.u32(ptr + 8 * i)) for i in range(count)}

    def _flat(self, count, desc_ptr, name_ptr, prefix=''):
        if not count or not desc_ptr:
            return []
        names = self._names(name_ptr, count)
        return [self.scene(prefix + names.get(i, 'scene%d' % i), self.u32(desc_ptr + 4 * i))
                for i in range(count)]

    # The block at cast+0x30 is not a bespoke struct: it is the authored copy of the runtime
    # node's 15-slot property array, written back verbatim by fn_8035E8F0. Which slots are
    # present is a bitmask at cast+0x2C (0x7FFF - all fifteen - for every one of the 1300 casts
    # in this game), so word index == slot index here.
    #
    #   0 hide flag (non-zero = never drawn)      1 x        2 y        3 rotation, degrees
    #   4 scaleX    5 scaleY                      6 pattern index, as a FLOAT
    #   7 colour, 0xRRGGBBAA - ALPHA IS THE LOW BYTE
    #   8..11 per-corner vertex colours, same packing        12..14 unused (0 everywhere)
    #
    # The packing is what makes 0xFFFFFF00 (opaque-looking white) actually a fully transparent
    # cast and 0x0000000A a black shadow at alpha 10. Reading it as ARGB inverts both.
    PROP_DEFAULTS = (0, 0.0, 0.0, 0.0, 1.0, 1.0, 0.0,
                     0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0, 0, 0)

    def props(self, cp):
        mask = self.u32(cp + 0x2C)
        block = self.u32(cp + 0x30)

        def slot(k, as_float):
            if not block or not (mask >> k) & 1:
                return self.PROP_DEFAULTS[k]
            off = block + 4 * bin(mask & ((1 << k) - 1)).count('1')
            return self.f32(off) if as_float else self.u32(off)

        return dict(hide=slot(0, False), x=slot(1, True), y=slot(2, True),
                    rotation=slot(3, True), scale_x=slot(4, True), scale_y=slot(5, True),
                    pattern=slot(6, True), colour=slot(7, False),
                    corner_colours=[slot(8 + k, False) for k in range(4)])

    def resolve_cell(self, cp, props=None):
        """fn_8035DC14. The drawn cell is chosen by property slot 6, NOT by pattern[0]:
        72 casts author a non-zero index, and four of them would come out blank or wrong under
        an 'entry 0' rule. Returns -1 for untextured (still drawn, as a flat quad)."""
        if props is None:
            props = self.props(cp)
        n = int(math.trunc(props['pattern']))
        a = int(math.fmod(n, 10000))
        count, ptr = self.u32(cp + 0x3C), self.u32(cp + 0x40)
        if not ptr or not (0 <= a < count):
            return -1
        cell = self.i32(ptr + 4 * a)
        return cell if cell >= 0 else -1

    def scenes(self):
        # Parsing a project is not cheap and an animation timeline asks for it every frame, so the
        # result is cached. The file is read once in __init__ and never changes underneath us.
        cached = getattr(self, '_scene_cache', None)
        if cached is not None:
            return cached
        self._scene_cache = self._scenes_uncached()
        return self._scene_cache

    def _scenes_uncached(self):
        out = self._flat(self.u32(0x20), self.u32(0x24), self.u32(0x28))
        gcount, garr = self.u32(0x2C), self.u32(0x30)
        groups = self._names(self.u32(0x34), gcount)
        for g in range(gcount if garr else 0):
            o = garr + 24 * g
            out.extend(self._flat(self.u32(o), self.u32(o + 4), self.u32(o + 8),
                                  prefix=groups.get(g, 'group%d' % g) + '/'))
        return out

    def scene(self, name, desc):
        tex_n, tex_p = self.u32(desc + 0x14), self.u32(desc + 0x18)
        cell_n, cell_p = self.u32(desc + 0x1C), self.u32(desc + 0x20)
        grp_n, grp_p = self.u32(desc + 0x24), self.u32(desc + 0x28)
        nam_n, nam_p = self.u32(desc + 0x2C), self.u32(desc + 0x30)
        textures = [(self.f32(tex_p + 8 * i) * 640.0, self.f32(tex_p + 8 * i + 4) * 480.0)
                    for i in range(tex_n)] if tex_p else []
        cells = []
        for i in range(cell_n):
            o = cell_p + 20 * i
            cells.append(dict(tex=self.i32(o), u0=self.f32(o + 4), v0=self.f32(o + 8),
                              u1=self.f32(o + 12), v1=self.f32(o + 16)))
        names = {}
        for i in range(nam_n):
            o = nam_p + 12 * i
            names[(self.u32(o + 4), self.u32(o + 8))] = self.cstr(self.u32(o))
        casts = []
        for g in range(grp_n):
            o = grp_p + 16 * g
            ncast, arr, root, pair_p = self.u32(o), self.u32(o + 4), self.i32(o + 8), self.u32(o + 12)
            for c in range(ncast):
                cp = self.u32(arr + 4 * c)
                if not cp:
                    continue
                # Local quad corners, in AUTHORED INDEX ORDER - never min/maxed. The order is
                # fixed: 0 = (minX,minY) TL, 1 = (minX,maxY) BL, 2 = (maxX,minY) TR,
                # 3 = (maxX,maxY) BR, and they bind to the cell rect by index:
                # uv[0]=(u0,v0) uv[1]=(u0,v1) uv[2]=(u1,v0) uv[3]=(u1,v1).
                # Collapsing to a bounding box would silently un-mirror the 19 casts authored
                # with a negative scaleX and the 9 with a negative scaleY.
                corners = [(self.f32(cp + 0x0C + 8 * k), self.f32(cp + 0x10 + 8 * k))
                           for k in range(4)]
                props = self.props(cp)
                sx, sy = props['scale_x'], props['scale_y']
                rot = props['rotation']
                tx, ty = props['x'] * 640.0, props['y'] * 480.0
                rad = math.radians(rot)
                cs, sn = math.cos(rad), math.sin(rad)
                xs, ys = [], []
                for lx_, ly_ in corners:
                    px, py = lx_ * sx * 640.0, ly_ * sy * 480.0
                    # guest sense: X = px*cos + py*sin, Y = -px*sin + py*cos (fn_80360848)
                    xs.append(px * cs + py * sn + tx)
                    ys.append(-px * sn + py * cs + ty)
                cell = self.resolve_cell(cp, props)
                # The whole pattern array, so a motion's cell track (which animates property
                # slot 6, a PATTERN index) can be resolved to a cell index at any frame.
                pat_n, pat_p = self.u32(cp + 0x3C), self.u32(cp + 0x40)
                patterns = ([self.i32(pat_p + 4 * k) for k in range(min(pat_n, 64))]
                            if pat_p else [])
                casts.append(dict(
                    group=g, index=c, name=names.get((g, c), ''),
                    x0=min(xs), x1=max(xs), y0=min(ys), y1=max(ys),
                    corners=list(zip(xs, ys)), local_corners=corners,
                    local=(min(p[0] for p in corners), min(p[1] for p in corners),
                           max(p[0] for p in corners), max(p[1] for p in corners)),
                    kind=self.u32(cp + 0x04), enabled=self.u32(cp + 0x08),
                    flags=self.u32(cp + 0x34), group_root=root,
                    pair=((self.i32(pair_p + 8 * c), self.i32(pair_p + 8 * c + 4))
                          if pair_p else (-1, -1)),
                    hide=props['hide'], colour='0x%08X' % props['colour'],
                    corner_colours=['0x%08X' % v for v in props['corner_colours']],
                    pattern_index=props['pattern'], patterns=patterns,
                    tx=tx, ty=ty, scale_x=sx, scale_y=sy, rotation=rot, cell=cell,
                    w=max(xs) - min(xs), h=max(ys) - min(ys),
                    texture=(cells[cell]['tex'] if 0 <= cell < len(cells) else -1)))
        return dict(name=name, textures=textures, cells=cells, casts=casts,
                    fps=self.f32(desc + 0x08),
                    default_range=(self.f32(desc + 0x0C), self.f32(desc + 0x10)),
                    motions=self.motions(desc))

    # Motions. The chain, all big-endian, all pointers relative to the nCPJ chunk:
    #
    #   desc+0x34/+0x38  motion count, motion array   (8 bytes: groupCount, groupArrayPtr)
    #   desc+0x3C        motion name table            (8 bytes: namePtr, index - alphabetical)
    #   groupArray[g]    8 bytes: castCount, castArrayPtr
    #   castArray[c]     8 bytes: trackMask, trackArrayPtr
    #   trackArray[t]    12 bytes: flags, keyCount, keyPtr   - popcount(trackMask) of them
    #   key[i]           24 bytes: frame u32, value, interp u32, 0, 0, 0
    #
    # trackMask is a bitfield indexed by track number, bit 0 unused:
    #   1 = X, 2 = Y, 3 = rotation (degrees), 4 = scaleX, 5 = scaleY, 6 = cell index, 7 = RGBA.
    # Tracks appear in ascending bit order. X and Y are in the same normalised units as the cast
    # transform (x * 640, y * 480); cell and RGBA are integers, the rest are floats.
    #
    # The group/cast indexing is what makes this usable: a motion has one slot PER GROUP, and
    # within it one entry per cast in that group. Options' "menu" scene has 9 groups and 11 casts,
    # and its motions have 9 slots of which two carry 2 entries - exactly the two 2-cast groups.

    TRACK_NAMES = {1: 'x', 2: 'y', 3: 'rotation', 4: 'scale_x', 5: 'scale_y',
                   6: 'cell', 7: 'colour'}

    def motions(self, desc):
        count, arr, names_p = self.u32(desc + 0x34), self.u32(desc + 0x38), self.u32(desc + 0x3C)
        if not count or not arr:
            return []
        names = self._names(names_p, count)
        # desc+0x44 is a table of {startFrame, endFrame} float pairs, one per motion. Scene::
        # SetMotion (0x803680BC) copies the pair into the scene's clamp range, so this - NOT the
        # largest key frame - is how long a motion actually runs. honban's Menu/MenuOpen is
        # authored [0,20] while its keys reach 100; the frames past 20 are never reached.
        rng = self.u32(desc + 0x44)
        out = []
        for m in range(count):
            gcount, gptr = self.u32(arr + 8 * m), self.u32(arr + 8 * m + 4)
            groups = []
            for g in range(gcount if gptr else 0):
                ccount, cptr = self.u32(gptr + 8 * g), self.u32(gptr + 8 * g + 4)
                casts = []
                for c in range(ccount if cptr else 0):
                    mask, tptr = self.u32(cptr + 8 * c), self.u32(cptr + 8 * c + 4)
                    tracks = {}
                    slot = 0
                    for bit in range(1, 8):
                        if not (mask >> bit) & 1:
                            continue
                        o = tptr + 12 * slot
                        slot += 1
                        kn, kp = self.u32(o + 4), self.u32(o + 8)
                        keys = []
                        for k in range(kn if kp else 0):
                            ko = kp + 24 * k
                            keys.append(dict(frame=self.u32(ko),
                                             f=self.f32(ko + 4), i=self.u32(ko + 4),
                                             interp=self.u32(ko + 8)))
                        tracks[self.TRACK_NAMES[bit]] = keys
                    casts.append(dict(mask=mask, tracks=tracks))
                groups.append(casts)
            frames = [k['frame'] for gg in groups for cc in gg
                      for ks in cc['tracks'].values() for k in ks]
            start = self.f32(rng + 8 * m) if rng else 0.0
            end = self.f32(rng + 8 * m + 4) if rng else 0.0
            out.append(dict(name=names.get(m, 'motion%d' % m), index=m, groups=groups,
                            last_key=max(frames) if frames else 0,
                            start=start, end=end,
                            last_frame=int(round(end)) if rng else (max(frames) if frames else 0)))
        return out


def main():
    for path in sys.argv[1:]:
        g = Gncp(path)
        print('=== %s' % path)
        for sc in g.scenes():
            print('  scene %-18s  %d textures, %d cells, %d casts'
                  % (sc['name'], len(sc['textures']), len(sc['cells']), len(sc['casts'])))
            for i, (w, h) in enumerate(sc['textures']):
                print('      texture[%d] %.0f x %.0f' % (i, w, h))
            for c in sc['casts']:
                print('      cast %-22s g%-2d i%-2d  %8.1f..%-8.1f x %7.1f..%-7.1f '
                      '(%6.1f x %-6.1f) rot %7.2f cell %-3d tex %d'
                      % (c['name'][:22], c['group'], c['index'], c['x0'], c['x1'],
                         c['y0'], c['y1'], c['w'], c['h'], c['rotation'], c['cell'],
                         c['texture']))


if __name__ == '__main__':
    main()
