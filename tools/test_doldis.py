"""Lossless original-word display, including encodings Capstone cannot decode."""
import contextlib
import io
import struct
import unittest
from unittest.mock import patch

if __package__:
    from . import doldis
    from .test_graphics_lift_inventory import dol, BASE, BLR, NOP
else:
    import doldis
    from test_graphics_lift_inventory import dol, BASE, BLR, NOP


class LosslessDisassemblyTests(unittest.TestCase):
    def rows(self, words, md=None):
        return list(doldis.disassembly_rows(struct.pack('>' + 'I' * len(words), *words), BASE, md))

    def test_unknown_never_discards_following_words(self):
        class UnsupportedDecoder:
            def disasm(self, data, address):
                return iter(())
        words = [0, 0xffffffff, 0xe1040000, BLR, NOP]
        rows = self.rows(words, UnsupportedDecoder())
        self.assertEqual([r[1] for r in rows], words)
        self.assertEqual([r[0] for r in rows], [BASE + i * 4 for i in range(5)])
        self.assertEqual(rows[0][2], '.long')
        self.assertEqual(rows[2][2], 'psq_l')

    def test_psq_known_original_words(self):
        rows = self.rows([0xe1040000, 0xe0258008, 0xf1638008])
        self.assertEqual([(r[2], r[3]) for r in rows], [
            ('psq_l', 'f8, 0(r4), 0, 0'), ('psq_l', 'f1, 8(r5), 1, 0'),
            ('psq_st', 'f11, 8(r3), 1, 0')])
        self.assertIn('mask=0x3f070000', rows[0][4])
        self.assertIn('mask=0x00003f07', rows[2][4])

    def test_all_displacement_lane_and_format_fields(self):
        for op, name in ((56, 'psq_l'), (57, 'psq_lu'), (60, 'psq_st'), (61, 'psq_stu')):
            for index in range(8):
                for w in range(2):
                    for displacement in (0, 2047, -2048, -1):
                        word = op << 26 | 31 << 21 | 29 << 16 | w << 15 | index << 12 | (displacement & 4095)
                        decoded = doldis.decode_word(word, BASE)
                        self.assertEqual(decoded[:2], (name, f'f31, {displacement}(r29), {w}, {index}'))
                        self.assertIn(f'GQR{index}', decoded[2])

    def test_indexed_forms_all_fields(self):
        for xo, name in ((6, 'psq_lx'), (7, 'psq_stx'), (38, 'psq_lux'), (39, 'psq_stux')):
            for index in range(8):
                for w in range(2):
                    word = 4 << 26 | 17 << 21 | 9 << 16 | 31 << 11 | w << 10 | index << 7 | xo << 1
                    decoded = doldis.decode_word(word, BASE)
                    self.assertEqual(decoded[:2], (name, f'f17, r9, r31, {w}, {index}'))
                    self.assertIn(f'GQR{index}', decoded[2])

    def test_live_original_paired_math_not_silently_hidden(self):
        words = [0x1140321c, 0x11613a1c, 0x1142521e, 0x1144525c, 0x11400198]
        expected = [('ps_madds0', 'f10, f0, f8, f6'), ('ps_madds0', 'f11, f1, f8, f7'),
                    ('ps_madds1', 'f10, f2, f8, f10'), ('ps_madds0', 'f10, f4, f9, f10'),
                    ('ps_muls0', 'f10, f0, f6')]
        self.assertEqual([doldis.decode_word(w, BASE)[:2] for w in words], expected)
        self.assertEqual(doldis.decode_word(words[0] | 1, BASE)[0], 'ps_madds0.')

    def test_paired_math_operand_families(self):
        for short, operands in ((10, 'f17, f9, f31, f11'), (25, 'f17, f9, f11'),
                                (21, 'f17, f9, f31'), (26, 'f17, f31'),
                                (29, 'f17, f9, f11, f31')):
            word = 4 << 26 | 17 << 21 | 9 << 16 | 31 << 11 | 11 << 6 | short << 1
            self.assertEqual(doldis.decode_word(word, BASE)[1], operands)

    def test_generic_decoder_cannot_mislabel_gekko_as_altivec(self):
        class WrongIsaDecoder:
            def disasm(self, data, address):
                raise AssertionError('Gekko primary-4 must not use generic AltiVec decoding')
        words = [0x118c6c20, 0x128c00c0, 0x130d0040, 4 << 26 | 22 << 1]
        rows = self.rows(words, WrongIsaDecoder())
        self.assertEqual([r[2] for r in rows], ['ps_merge00', 'ps_cmpo1', 'ps_cmpo0', '.long'])
        self.assertEqual(rows[0][3], 'f12, f12, f13')

    def test_malformed_ranges_refused(self):
        for raw, start in ((b'x', BASE), (b'1234', BASE + 1), (b'1234', -4), (b'12345678', 0xfffffffc)):
            with self.assertRaises(ValueError):
                list(doldis.disassembly_rows(raw, start))
        self.assertEqual(list(doldis.disassembly_rows(b'', BASE)), [])

    def test_original_section_boundary_not_crossed(self):
        raw = dol([NOP, BLR])
        with patch.object(doldis, '_d', raw), patch.object(doldis, '_secs', [(BASE, 0x100, 8, 0)]):
            self.assertEqual(doldis.range_bytes(BASE, 2), struct.pack('>II', NOP, BLR))
            for start, count in ((BASE, 3), (BASE - 4, 1), (BASE + 1, 1), (BASE, -1)):
                with self.assertRaises(ValueError):
                    doldis.range_bytes(start, count)
            self.assertEqual(doldis.word(BASE + 4), BLR)
            self.assertIsNone(doldis.word(BASE + 5))

    def test_failed_load_does_not_publish_invalid_image(self):
        with patch.object(doldis, '_d', None), patch.object(doldis, '_secs', []), \
             patch('builtins.open', return_value=io.BytesIO(b'broken')):
            with self.assertRaises(ValueError):
                doldis._load()
            self.assertIsNone(doldis._d)
            self.assertEqual(doldis._secs, [])

    def test_heuristic_reports_its_scope_and_retains_unknown(self):
        words = [0xffffffff, BLR, 37 << 26 | 1 << 16]
        raw = dol(words)
        out = io.StringIO()
        with patch.object(doldis, '_d', raw), patch.object(doldis, '_secs', [(BASE, 0x100, 12, 0)]), \
             patch.object(doldis, '_md', return_value=None), contextlib.redirect_stdout(out):
            doldis.func(BASE)
        text = out.getvalue()
        self.assertIn('not procedure or consumer-closure proof', text)
        self.assertIn('FFFFFFFF', text)
        self.assertIn('4E800020', text)
        self.assertNotIn(f'{BASE + 8:08X}', text)

    def test_help_never_loads_a_disc(self):
        with patch.object(doldis, '_load', side_effect=AssertionError('must not read disc')), \
             contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(doldis.main(['--help']), 0)


if __name__ == '__main__':
    unittest.main()
