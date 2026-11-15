"""Unit tests for build_variant.py's helpers (synthetic code only; no game or mod bytes).

    python -I tools/mods/codemods/test_build_variant.py
"""
import importlib.util
import pathlib
import unittest

spec = importlib.util.spec_from_file_location("build_variant", pathlib.Path(__file__).with_name("build_variant.py"))
bv = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bv)


def lis(rd, hi):
    return (15 << 26) | (rd << 21) | hi


def ori(ra, rs, lo):
    return (24 << 26) | (rs << 21) | (ra << 16) | lo


def lwz(rd, ra, d):
    return (32 << 26) | (rd << 21) | (ra << 16) | (d & 0xFFFF)


def stw(rs, ra, d):
    return (36 << 26) | (rs << 21) | (ra << 16) | (d & 0xFFFF)


class ConstantAccesses(unittest.TestCase):
    def test_load_and_store_through_built_constant(self):
        words = [lis(15, 0x817C), ori(15, 15, 0x61A0), lwz(16, 15, 0), stw(16, 15, 8)]
        self.assertEqual(bv.constant_accesses(words), [("load", 0x817C61A0), ("store", 0x817C61A8)])

    def test_overwritten_register_is_forgotten(self):
        words = [lis(15, 0x817C), lwz(15, 15, 0), lwz(16, 15, 4)]
        self.assertEqual(bv.constant_accesses(words), [("load", 0x817C0000)])

    def test_negative_displacement(self):
        words = [lis(3, 0x817D), stw(0, 3, -4)]
        self.assertEqual(bv.constant_accesses(words), [("store", 0x817CFFFC)])


class RemapGecko(unittest.TestCase):
    def test_write_and_condition_remapped(self):
        src = [(0x2856ED4E, 0x00002020), (0x045766C8, 0x00007724), (0xE2000001, 0x80008000)]
        mapping = {0x8056ED4E: 0x8056FE0E, 0x805766C8: 0x80577788}
        out, why = bv.remap_gecko(src, mapping, set())
        self.assertIsNone(why)
        self.assertEqual(out, [(0x2856FE0E, 0x2020), (0x04577788, 0x7724), (0xE2000001, 0x80008000)])

    def test_endif_first_bit_kept(self):
        out, why = bv.remap_gecko([(0x2856ED4F, 0xFFDF0020)], {0x8056ED4E: 0x8056FE0E}, set())
        self.assertIsNone(why)
        self.assertEqual(out, [(0x2856FE0F, 0xFFDF0020)])

    def test_high_address_bit(self):
        out, why = bv.remap_gecko([(0x04000010, 1)], {0x80000010: 0x81000020}, set())
        self.assertIsNone(why)
        self.assertEqual(out, [(0x05000020, 1)])

    def test_unmapped_address_refused(self):
        out, why = bv.remap_gecko([(0x04000010, 1)], {}, set())
        self.assertIsNone(out)
        self.assertIn("no PAL address", why)

    def test_static_write_dropped_at_top_level_refused_inside_condition(self):
        mapping = {0x80001000: 0x80002000, 0x80003000: 0x80004000}
        out, why = bv.remap_gecko([(0x04001000, 5)], mapping, {0x80002000})
        self.assertEqual((out, why), ([], None))
        out, why = bv.remap_gecko([(0x20003000, 1), (0x04001000, 5), (0xE2000001, 0)], mapping, {0x80002000})
        self.assertIsNone(out)
        self.assertIn("conditional write", why)

    def test_string_write_data_lines_untouched(self):
        src = [(0x06001000, 0x00000009), (0x11223344, 0x55667788), (0x99000000, 0)]
        out, why = bv.remap_gecko(src, {0x80001000: 0x80002000}, set())
        self.assertIsNone(why)
        self.assertEqual(out, [(0x06002000, 9), (0x11223344, 0x55667788), (0x99000000, 0)])


class WriteLines(unittest.TestCase):
    def test_sizes_and_alignment(self):
        self.assertEqual(bv.write_lines(0x8057E8A8, bytes([0])), [(0x0057E8A8, 0)])
        self.assertEqual(bv.write_lines(0x80590A5C, bytes(4)), [(0x04590A5C, 0)])
        self.assertEqual(bv.write_lines(0x81000001, bytes([1, 2, 3])), [(0x01000001, 1), (0x03000002, 0x0203)])


if __name__ == "__main__":
    unittest.main()
