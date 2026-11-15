#!/usr/bin/env python3
"""Tests of patchset.py on synthetic DOLs, maps and code lists (no game files).

  python -I tools/mods/codemods/test_patchset.py
"""
import os, struct, sys, tempfile, unittest, argparse, json

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import patchset as P

TEXT0 = 0x80003100
NOP = 0x60000000
BLR = 0x4E800020


def lis(rd, i): return 0x3C000000 | rd << 21 | (i & 0xFFFF)
def ori(ra, rs, i): return 0x60000000 | rs << 21 | ra << 16 | (i & 0xFFFF)
def addi(rd, ra, i): return 0x38000000 | rd << 21 | ra << 16 | (i & 0xFFFF)
def lwz(rd, d, ra): return 0x80000000 | rd << 21 | ra << 16 | (d & 0xFFFF)
def stw(rs, d, ra): return 0x90000000 | rs << 21 | ra << 16 | (d & 0xFFFF)
def b(at, t, link=False): return P.mk_b(at, t, link)


def build_dol(sections, bss=(0, 0)):
    """sections: {index: (address, bytes)} -> DOL bytes"""
    hdr = [0] * 54
    off = 0x100
    body = b''
    for i, (a, data) in sorted(sections.items()):
        hdr[i], hdr[18 + i], hdr[36 + i] = off, a, len(data)
        body += data
        off += len(data)
    return struct.pack('>18I18I18I', *hdr) + struct.pack('>III', bss[0], bss[1], TEXT0) + b'\0' * (0x100 - 0xE4) + body


def words(ws):
    return struct.pack('>%dI' % len(ws), *ws)


class Fixture:
    """SRC (source region) and PAL: text sec 0 at 0x80003100 (0x2500 bytes), .data sec 12, .sdata sec 13, .sdata2 sec 14.
    Function F1/F2 sit 0x20 lower in SRC than in PAL, the .data variable 0x40 lower, the .sdata variable 0x10 lower."""

    def __init__(self, tmp):
        self.tmp = tmp
        # r13 = 0x80030080 and r2 = 0x80040080 in both
        def text(shift):
            ws = [lis(13, 0x8003), ori(13, 13, 0x0080), lis(2, 0x8004), ori(2, 2, 0x0080)]
            ws += [NOP] * (8 + shift // 4 - len(ws) + 0)
            f1 = TEXT0 + 4 * len(ws)
            ws += [lis(3, 0x8002), addi(3, 3, 0x10 + (0x40 if shift else 0)), lwz(4, -0x60 + (0x10 if shift else 0), 13),
                   0, BLR]                                          # bl F2 patched below
            f2 = TEXT0 + 4 * len(ws)
            ws += [addi(3, 0, 1), BLR]
            ws[ws.index(0)] = b(f1 + 12, f2, True)
            ws += [NOP] * (0x2500 // 4 - len(ws))
            return ws, f1, f2
        sw, self.s_f1, self.s_f2 = text(0)
        pw, self.p_f1, self.p_f2 = text(0x20)
        data = lambda shift: bytes(0x200)
        self.src = build_dol({0: (TEXT0, words(sw)), 12: (0x80020000, bytes(0x100)), 13: (0x80030000, bytes(0x200)),
                              14: (0x80040000, bytes(0x100))}, bss=(0x80050000, 0x1000))
        self.pal = build_dol({0: (TEXT0, words(pw)), 12: (0x80020000, bytes(0x100)), 13: (0x80030000, bytes(0x200)),
                              14: (0x80040000, bytes(0x100))}, bss=(0x80050000, 0x1000))
        self.sw, self.pw = sw, pw
        self.paths = {}
        for n, d in (('src.dol', self.src), ('pal.dol', self.pal)):
            self.paths[n] = os.path.join(tmp, n)
            open(self.paths[n], 'wb').write(d)
        with open(os.path.join(tmp, 'a.map'), 'w') as f:
            f.write('fixed 0x80000000 0x80003100 +0x0\n')
            f.write('code 0x80003100 0x80003120 +0x0\n')
            f.write('code 0x80003140 0x80003200 -0x20\n')
            f.write('code 0x80003200 0x80005600 +0x0\n')
            f.write('data 0x80020050 0x80020054 -0x40 1\n')          # PAL var 0x80020050 == SRC 0x80020010
            f.write('data 0x80030030 0x80030034 -0x10 1\n')          # sdata var
            f.write('data 0x80050000 0x80051000 +0x0 5\n')           # bss identical
        with open(os.path.join(tmp, 'f.csv'), 'w', newline='') as f:
            f.write('addr_hex,addr,name,size\n')
            for a, n in ((self.p_f1, 20), (self.p_f2, 8)):
                f.write('0x%X,%d,fn,%d\n' % (a, a, n))
        self.paths['map'] = os.path.join(tmp, 'a.map')
        self.paths['fn'] = os.path.join(tmp, 'f.csv')

    def engine(self, mod_dol=None, identity=False):
        a = argparse.Namespace(src_dol=self.paths['src.dol'], pal_dol=self.paths['pal.dol'], address_map=self.paths['map'],
                               pal_functions=self.paths['fn'], source_region='GUPE8P', mod_dol=mod_dol)
        return P.Engine(a)

    def write(self, name, text):
        p = os.path.join(self.tmp, name)
        with open(p, 'w') as f:
            f.write(text)
        return p


class T(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.td = tempfile.TemporaryDirectory()
        cls.fx = Fixture(cls.td.name)

    @classmethod
    def tearDownClass(cls):
        cls.td.cleanup()

    def run_gecko(self, text, name='t.txt'):
        eng = self.fx.engine()
        return P.run_gecko(eng, 'x', P.parse_gecko_text(self.fx.write(name, text)))

    # ---- translation
    def test_text_write_translates_and_confirms(self):
        ps = self.run_gecko('Name: n\n04%06X 60000000\n' % (self.fx.s_f1 & 0xFFFFFF))
        p = ps.patches[0]
        self.assertEqual(p['address'], P.h8(self.fx.p_f1))
        self.assertEqual((p['kind'], p['confidence']), ('static-patch', 'high'))
        self.assertEqual(p['new_bytes'], '60000000')
        self.assertEqual(p['old_bytes'], '%08x' % self.fx.pw[(self.fx.p_f1 - TEXT0) // 4])

    def test_branch_word_is_relocated(self):
        # bl F2 written over the first word of F1 (as in SRC) must target PAL's F2
        w = b(self.fx.s_f1, self.fx.s_f2, True)
        ps = self.run_gecko('04%06X %08X\n' % (self.fx.s_f1 & 0xFFFFFF, w))
        nb = int(ps.patches[0]['new_bytes'], 16)
        self.assertEqual(P.branch_info(nb, self.fx.p_f1)[1], self.fx.p_f2)

    def test_data_write_and_conditional_runtime(self):
        ps = self.run_gecko('Name: n\n04020010 3F800000\n')
        p = ps.patches[0]
        self.assertEqual(p['address'], '0x80020050')
        self.assertEqual((p['kind'], p['confidence']), ('static-patch', 'high'))
        self.assertTrue(any('mutable' in n for n in p['notes']))
        ps = self.run_gecko('Name: n\n28020010 00000000\n04030020 00000001\nE0000001 80008000\n04020010 00000002\n')
        self.assertEqual([p['kind'] for p in ps.patches], ['runtime', 'static-patch'])
        c = ps.patches[0]['condition'][0]
        self.assertEqual(c['addr'], '0x80020050')

    def test_bss_write_is_runtime(self):
        ps = self.run_gecko('04050100 00000001\n')
        self.assertEqual(ps.patches[0]['kind'], 'runtime')
        self.assertEqual(ps.patches[0]['address'], '0x80050100')

    def test_pointer_write_unresolved(self):
        ps = self.run_gecko('14000010 00000001\n')                    # ba-relative
        self.assertEqual((ps.patches[0]['kind'], ps.patches[0]['confidence']), ('runtime', 'unresolved'))

    def test_string_and_serial_and_fill(self):
        ps = self.run_gecko('06020010 00000009\n41424344 45464748\n49000000 00000000\n02020010 00020005\n'
                            '08020010 00000001\n00010001 00000002\n')
        self.assertEqual(ps.patches[0]['new_bytes'], b'ABCDEFGHI'.hex())
        self.assertEqual(ps.patches[1]['new_bytes'], '0005' * 3)
        self.assertEqual(len(ps.patches), 4)                              # string, fill, two serial writes
        self.assertEqual(ps.patches[2]['new_bytes'], '01')
        self.assertEqual(ps.patches[3]['new_bytes'], '03')
        self.assertEqual(ps.patches[3]['source_address'], '0x80020011')

    def test_c6_branch(self):
        ps = self.run_gecko('C6%06X %08X\n' % (self.fx.s_f1 & 0xFFFFFF, self.fx.s_f2))
        p = ps.patches[0]
        self.assertEqual(P.branch_info(int(p['new_bytes'], 16), int(p['address'], 16))[1], self.fx.p_f2)

    def test_c2_cave_relocates_pairs_sda_and_returns(self):
        body = [lis(5, 0x8002), ori(5, 5, 0x0010), lwz(6, -0x60, 13), stw(6, 0, 5), 0]
        lines = ''.join('%08X %08X\n' % (body[i], body[i + 1]) for i in range(0, 4, 2)) + '%08X 00000000\n' % body[4] if False else None
        txt = 'Name: c2\nC2%06X 00000003\n%08X %08X\n%08X %08X\n%08X 00000000\n' % (
            self.fx.s_f1 & 0xFFFFFF, body[0], body[1], body[2], body[3], NOP)
        ps = self.run_gecko(txt)
        hook, cave = ps.patches
        self.assertEqual((hook['role'], cave['role']), ('hook', 'cave'))
        ca = int(cave['address'], 16)
        self.assertEqual(ca, P.CAVE_LO)
        nw = struct.unpack('>6I', bytes.fromhex(cave['new_bytes']))
        self.assertEqual(nw[0], lis(5, 0x8002))
        self.assertEqual(nw[1], ori(5, 5, 0x0050))                       # 0x80020010 -> 0x80020050
        self.assertEqual(nw[2], lwz(6, -0x50, 13))                       # sdata var moved by +0x10
        self.assertEqual(P.branch_info(nw[5], ca + 20)[1], self.fx.p_f1 + 4)   # return to hook+4
        self.assertEqual(P.branch_info(int(hook['new_bytes'], 16), self.fx.p_f1)[1], ca)
        self.assertEqual(ps.caves[0]['verification_failures'], [])
        self.assertEqual(cave['confidence'], 'high')

    def test_cave_external_branch_needs_base(self):
        eng = self.fx.engine()
        # relative bl out of the block with unknown placement is unresolved ...
        rl = P.relocate_block([b(0, 0x1000, True), NOP], None, P.CAVE_LO, eng.xl, eng)
        self.assertEqual(rl.conf, 'unresolved')
        # ... and relocated when the source placement is known
        base = 0x80003400
        rl = P.relocate_block([b(base, self.fx.s_f2, True), BLR], base, base, eng.xl, eng)
        self.assertEqual(P.branch_info(rl.words[0], base)[1], self.fx.p_f2)

    def test_split_lis_and_unmapped_flagged(self):
        eng = self.fx.engine()
        rl = P.relocate_block([lis(3, 0x8002), addi(3, 3, 0x0080), lwz(4, 0x0020, 3)], None, P.CAVE_LO, eng.xl, eng)
        self.assertEqual(rl.conf, 'unresolved')                          # nothing in the map at those data addresses

    def two_pass(self, specs, mod_dol=None, **kw):
        eng = self.fx.engine(mod_dol)
        return eng, P.run_two_pass(eng, specs, **kw)

    def test_heap_constant_goes_to_scratch(self):
        eng = self.fx.engine()
        code = [lis(3, 0x807D), ori(3, 3, 0x71A0), lis(4, 0x807D), ori(4, 4, 0x5700), lis(5, 0x8060), ori(5, 5, 0xB33C), BLR]
        eng.scratch.reset_collect()
        P.relocate_block(code, None, P.CAVE_LO, eng.xl, eng)          # collect
        eng.scratch.build()
        eng.xl.cache.clear()
        rl = P.relocate_block(code, None, P.CAVE_LO, eng.xl, eng)
        self.assertEqual(rl.conf, 'medium')
        self.assertEqual(rl.unresolved, [])
        val = lambda i: ((rl.words[i] & 0xFFFF) << 16) | (rl.words[i + 1] & 0xFFFF)
        a_, b_, c_ = val(0), val(2), val(4)
        for v in (a_, b_, c_):
            self.assertTrue(P.SCRATCH_BASE <= v < P.SCRATCH_BASE + P.SCRATCH_SIZE, hex(v))
        self.assertEqual(a_ - b_, 0x71A0 - 0x5700)                    # one cluster keeps the offsets
        self.assertEqual(a_ & 0xFFF, 0x1A0)                           # and the alignment
        self.assertEqual(len(eng.scratch.clusters), 2)                # 0x8060B33C is a cluster of its own
        self.assertLessEqual(eng.scratch.clusters[0]['pal_hi'], eng.scratch.clusters[1]['pal_lo'])
        self.assertEqual(P.verify_block(rl.words, P.CAVE_LO, eng, eng.xl)['failures'], [])
        rep = eng.scratch.report()
        self.assertEqual(rep[1]['constants'], ['0x807D5700', '0x807D71A0'])

    def test_scratch_overflow_fails_loudly(self):
        sc = P.Scratch(size=0x2000)
        sc.translate(0x807D5700)
        sc.translate(0x80600000)
        with self.assertRaises(P.ScratchOverflow):
            sc.build()

    def test_c2_cave_and_runtime_write_use_the_same_scratch(self):
        txt = ('Name: s\nC2%06X 00000003\n%08X %08X\n%08X %08X\n%08X 00000000\n'
               % (self.fx.s_f1 & 0xFFFFFF, lis(5, 0x807D), ori(5, 5, 0x71A0), stw(6, 0, 5), NOP, NOP)
               + 'Name: w\n047D71A4 00000001\n')
        p = self.fx.write('sc.txt', txt)
        eng, sets = self.two_pass(['gecko-text:' + p])
        ps = next(iter(sets.values()))
        cave = next(x for x in ps.patches if x.get('role') == 'cave')
        w = struct.unpack('>3I', bytes.fromhex(cave['new_bytes'])[:12])
        base = ((w[0] & 0xFFFF) << 16) | (w[1] & 0xFFFF)
        run = next(x for x in ps.patches if x['kind'] == 'runtime')
        self.assertEqual(int(run['address'], 16), base + 4)           # same cluster, same relative offset
        self.assertEqual(run['source_address'], '0x807D71A4')
        self.assertTrue(P.SCRATCH_BASE <= base < P.SCRATCH_BASE + P.SCRATCH_SIZE)

    def test_small_data_beyond_the_image_is_unresolved(self):
        eng = self.fx.engine()
        eng.scratch.reset_collect()
        eng.scratch.build()
        rl = P.relocate_block([lwz(4, 0x2000, 2), BLR], None, P.CAVE_LO, eng.xl, eng)
        self.assertEqual(rl.conf, 'unresolved')

    def test_zero_word_does_not_decode(self):
        eng = self.fx.engine()
        rl = P.relocate_block([NOP, 0, BLR], None, P.CAVE_LO, eng.xl, eng)
        self.assertEqual(rl.conf, 'unresolved')

    def test_selftest_reproduces_pal_functions(self):
        eng = self.fx.engine()
        r = P.selftest_relocate(eng)
        self.assertGreaterEqual(r['counts']['functions'], 2)
        self.assertEqual(r['counts'].get('words-mismatch', 0), 0, r['examples'])

    def test_template_placeholder_flagged(self):
        p = self.fx.write('t.ini', '[Gecko]\n$T\nC2%06X 00000001\n3880000X 00000000\n' % (self.fx.s_f1 & 0xFFFFFF))
        ps = P.run_gecko(self.fx.engine(), 't', P.parse_gecko_ini(p))
        self.assertTrue(ps.patches and all(x.get('needs_user_value') for x in ps.patches))

    # ---- parsers
    def test_ini_sections_and_enabled(self):
        p = self.fx.write('g.ini', '[Gecko]\n$=== H ===\n$A [me]\n04%06X 60000000\n*note\n$B\n04%06X 60000000\n[Gecko_Enabled]\n$A\n[Core]\nx=1\n'
                          % (self.fx.s_f1 & 0xFFFFFF, self.fx.s_f1 & 0xFFFFFF))
        codes = P.parse_gecko_ini(p)
        self.assertEqual([(c.name, c.enabled, len(c.lines)) for c in codes], [('A', True, 1), ('B', False, 1)])
        ps = P.run_gecko(self.fx.engine(), 'g', codes, only_enabled=True)
        self.assertEqual(len(ps.patches), 1)

    def test_gct(self):
        raw = bytes.fromhex('00D0C0DE00D0C0DE') + struct.pack('>II', 0x04000100 | 0x03000000 & 0, NOP) + struct.pack('>II', 0xF0000000, 0)
        p = os.path.join(self.fx.tmp, 'c.gct')
        open(p, 'wb').write(raw)
        codes = P.parse_gct(p)
        self.assertEqual(codes[0].lines, [(0x04000100, NOP)])

    def test_action_replay_write(self):
        p = self.fx.write('ar.ini', '[ActionReplay]\n$AR1\n04%06X 60000000\n' % (self.fx.s_f1 & 0xFFFFFF))
        ps = P.run_gecko(self.fx.engine(), 'ar', P.parse_gecko_ini(p))
        self.assertEqual(ps.patches[0]['address'], P.h8(self.fx.p_f1))

    # ---- MCM
    MCM = """Entry One
desc
[me]
Revision ---- DOL Offset ---- Hex to Replace ---------- ASM Code -
NTSC 1.06 ---- 0x{off:X} ------ {w0:08X} -> Branch

{w0:08X} 38600005
00000000

-------------- 0x{a2:X} ------ 60000000 -> 38600001

	-==-

Entry Two
desc
[me]
Revision ---- DOL Offset ---- Hex to Replace ---------- ASM Code -
NTSC 1.06 ---- 0x{a3:X} ------

{o0:08X} {o1:08X}

 -> 

{n0:08X} {n1:08X}

"""

    def test_mcm_forms(self):
        fx = self.fx
        w0 = fx.sw[(fx.s_f1 - TEXT0) // 4]
        o0, o1 = fx.sw[(fx.s_f1 - TEXT0) // 4 + 1], fx.sw[(fx.s_f1 - TEXT0) // 4 + 2]
        off = fx.s_f1 - TEXT0 + 0x100                                  # DOL file offset form
        a2 = fx.s_f1 + 0x40                                            # address form (a nop inside F1's neighbourhood)
        txt = self.MCM.format(off=off, w0=w0, a2=a2, a3=fx.s_f1 + 4, o0=o0, o1=o1, n0=lis(3, 0x8002), n1=addi(3, 3, 0x10))
        p = fx.write('m.txt', txt)
        # fix the second site's address: use an address the map covers
        eng = fx.engine()
        ps = P.run_mcm(eng, 'm', p)
        roles = [(x['kind'], x.get('role')) for x in ps.patches]
        self.assertIn(('code-injection', 'hook'), roles)
        self.assertIn(('code-injection', 'cave'), roles)
        self.assertEqual(ps.extra['entries'], 2)
        self.assertEqual(ps.extra['sites'], 3)
        hook = next(x for x in ps.patches if x.get('role') == 'hook')
        self.assertEqual(hook['address'], P.h8(fx.p_f1))
        self.assertEqual(hook['source_old_bytes'], '%08x' % w0)
        multi = ps.patches[-1]
        self.assertEqual(multi['new_bytes'], words([lis(3, 0x8002), addi(3, 3, 0x50)]).hex())      # pair re-derived in PAL

    # ---- features
    def test_gecko_features_and_confidence(self):
        p = self.fx.write('f.ini', '[Gecko]\n$One [a]\n04%06X 60000000\n04%06X 60000000\n$Two\n04020010 3F800000\n'
                          % (self.fx.s_f1 & 0xFFFFFF, (self.fx.s_f1 + 4) & 0xFFFFFF))
        eng, sets = self.two_pass(['gecko-ini:' + p])
        ps = next(iter(sets.values()))
        feats = P.build_features(ps)
        self.assertEqual([(f['id'], f['name'], len(f['patches'])) for f in feats], [('g001', 'One', 2), ('g002', 'Two', 1)])
        self.assertTrue(all(x['provenance']['feature'] and x['provenance']['feature_id'] for x in ps.patches))
        self.assertEqual(feats[0]['confidence'], min((x['confidence'] for x in ps.patches[:2]), key=lambda c: P.CONF_ORDER[c]))

    LIB = "{title}\nd\n[me]\nRevision ---- DOL Offset ---- Hex to Replace ---------- ASM Code -\n{body}\n\t-==-\n"

    def test_mcm_enabled_entries_and_residue(self):
        fx = self.fx
        i = lambda a: (a - TEXT0) // 4
        s1, s2 = fx.s_f1, fx.s_f2
        sw = list(fx.sw)
        old1, old2 = sw[i(s1 + 4)], sw[i(s1 + 8)]
        # A: word site, enabled; B: word site, still original; C: branch + cave, enabled; D: cave differs from the library text
        cave_c, cave_d = P.CAVE_LO, P.CAVE_LO + 0x40
        hook_c, hook_d = s2, s1 + 12
        wc, wd = sw[i(hook_c)], sw[i(hook_d)]
        lib = [self.LIB.format(title='A', body='NTSC 1.06 ---- 0x%X ---- %08X -> %08X\n' % (s1 + 4, old1, NOP)),
               self.LIB.format(title='B', body='NTSC 1.06 ---- 0x%X ---- %08X -> %08X\n' % (s1 + 8, old2, 0x38600001)),
               self.LIB.format(title='C', body='NTSC 1.06 ---- 0x%X ---- %08X -> Branch\n\n%08X 38800002\n00000000\n' % (hook_c, wc, wc)),
               self.LIB.format(title='D', body='NTSC 1.06 ---- 0x%X ---- %08X -> Branch\n\n%08X 38800003\n00000000\n' % (hook_d, wd, wd))]
        text = fx.write('lib.txt', ''.join(lib))
        sw[i(s1 + 4)] = NOP
        sw[i(hook_c)] = b(hook_c, cave_c)
        sw[i(cave_c)], sw[i(cave_c) + 1] = wc, 0x38800002
        sw[i(cave_c) + 2] = b(cave_c + 8, hook_c + 4)
        sw[i(hook_d)] = b(hook_d, cave_d)
        sw[i(cave_d)], sw[i(cave_d) + 1] = wd, 0x38800009             # another version of D
        sw[i(cave_d) + 2] = b(cave_d + 8, hook_d + 4)
        sw[i(P.CAVE_LO + 0x100)] = 0                                  # zero fill of a stub
        sw[i(s2 + 4)] = NOP                                           # a word no library entry explains
        mod = os.path.join(fx.tmp, 'mod2.dol')
        with open(mod, 'wb') as f:
            f.write(build_dol({0: (TEXT0, words(sw)), 12: (0x80020000, bytes(0x100)), 13: (0x80030000, bytes(0x200)),
                               14: (0x80040000, bytes(0x100))}, bss=(0x80050000, 0x1000)))
        eng, sets = self.two_pass(['mcm:' + text], mod_dol=mod)
        ps = next(iter(sets.values()))
        self.assertEqual({e['name']: e['status'] for e in ps.extra['entries_detail']},
                         {'A': 'enabled', 'B': 'not-enabled', 'C': 'enabled', 'D': 'version-mismatch'})
        self.assertEqual({x['provenance']['feature'] for x in ps.patches}, {'A', 'C'})
        feats = {f['name']: f for f in P.build_features(ps)}
        self.assertEqual(feats['A']['confidence'], 'high')
        self.assertEqual(feats['C']['confidence'], 'high')
        self.assertEqual(feats['D']['confidence'], 'unresolved')       # listed, nothing emitted
        self.assertEqual(feats['D']['patches'], [])
        self.assertIsNone(feats['B']['confidence'])
        res = {r['class']: r for r in ps.extra['residue']}
        self.assertEqual(res['cave-zero-fill']['words'], 1)
        self.assertEqual(res['unexplained']['address'], P.h8(s2 + 4))
        self.assertEqual(sum(r['words'] for r in ps.extra['residue'] if r['class'] == 'version-mismatch'), 4)   # D's hook + 3 cave words
        self.assertEqual(ps.extra['diff_words'], ps.extra['explained_words'] + 1 + 1 + 4)

    # ---- DOL diff
    def test_dol_diff(self):
        fx = self.fx
        sw = list(fx.sw)
        cave = P.CAVE_LO
        i = lambda a: (a - TEXT0) // 4
        hook_a = fx.s_f1
        sw[i(hook_a)] = b(hook_a, cave)
        c = [lis(5, 0x8002), ori(5, 5, 0x0010), lwz(6, -0x60, 13), stw(6, 0, 5)]
        for k, w in enumerate(c):
            sw[i(cave) + k] = w
        sw[i(cave) + 4] = b(cave + 16, hook_a + 4)
        sw[i(fx.s_f2)] = addi(3, 0, 7)                                 # inline change
        mod = os.path.join(fx.tmp, 'mod.dol')
        open(mod, 'wb').write(build_dol({0: (TEXT0, words(sw)), 12: (0x80020000, bytes(0x100)), 13: (0x80030000, bytes(0x200)),
                                         14: (0x80040000, bytes(0x100))}, bss=(0x80050000, 0x1000)))
        ps = P.run_dol_diff(fx.engine(), 'd', mod)
        kinds = sorted((p['kind'], p.get('role')) for p in ps.patches)
        self.assertEqual(kinds, [('code-injection', 'cave'), ('code-injection', 'hook'), ('static-patch', None)])
        cave_p = next(p for p in ps.patches if p.get('role') == 'cave')
        nw = struct.unpack('>5I', bytes.fromhex(cave_p['new_bytes']))
        self.assertEqual(nw[1], ori(5, 5, 0x50))
        self.assertEqual(nw[2], lwz(6, -0x50, 13))
        self.assertEqual(P.branch_info(nw[4], cave + 16)[1], fx.p_f1 + 4)
        self.assertEqual(cave_p['confidence'], 'high')
        self.assertEqual(ps.extra['hooks'], 1)


if __name__ == '__main__':
    unittest.main(verbosity=2)
