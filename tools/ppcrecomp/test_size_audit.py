"""Independent PE fixtures for accounting, bad evidence and folded-code handling."""
import struct
import unittest

from size_audit import AuditError, attribute, read_map, read_pe


def fixture():
    image = bytearray(0x620)
    image[:2] = b"MZ"
    struct.pack_into("<I", image, 0x3c, 0x80)
    image[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", image, 0x84, 0x8664, 2, 0x12345678, 0, 0, 0xf0, 0x22)
    opt = 0x98
    struct.pack_into("<H", image, opt, 0x20b)
    struct.pack_into("<Q", image, opt + 24, 0x140000000)
    struct.pack_into("<II", image, opt + 56, 0x3000, 0x200)
    struct.pack_into("<I", image, opt + 108, 16)
    struct.pack_into("<II", image, opt + 136, 0x2000, 24)
    struct.pack_into("<8sIIIIIIHHI", image, opt + 0xf0, b".text", 0x180, 0x1000,
                     0x200, 0x200, 0, 0, 0, 0, 0x60000020)
    struct.pack_into("<8sIIIIIIHHI", image, opt + 0xf0 + 40, b".pdata", 24, 0x2000,
                     0x200, 0x400, 0, 0, 0, 0, 0x40000040)
    struct.pack_into("<IIIIII", image, 0x400, 0x1000, 0x1020, 0x2100, 0x1040, 0x1050, 0x2108)
    image[0x500] = image[0x508] = 1  # v1 leaf unwind info, no fixed frame
    return image


def map_lines(rows):
    return [" Preferred load address is 0000000140000000\n",
            " Timestamp is 12345678 (fixture)\n"] + rows


def symbol(offset, name, origin="shadow_recomp:recomp_000.obj"):
    return f" 0001:{offset:08x} {name} {0x140001000 + offset:016x} f {origin}\n"


class SizeAuditTests(unittest.TestCase):
    def test_fixed_stack_small_allocation_and_saved_registers(self):
        image = fixture()
        # Three pushes plus 56 bytes: fixed prologue = 80, not 88. The
        # caller's return address is deliberately outside this measurement.
        struct.pack_into("<I", image, 0x414, 0x2140)
        image[0x500:0x50c] = bytes([1, 16, 4, 0, 16, 0x62, 7, 0x70, 6, 0x60, 5, 0x30])
        image[0x540] = 1
        pe = read_pe(image)
        self.assertEqual(pe["stack_frames"][0x1000]["fixed_stack_bytes"], 80)
        result = attribute(pe, read_map(map_lines([symbol(0, "function")]), pe))
        self.assertEqual(result["largest_fixed_stack_frames"][0]["fixed_stack_bytes"], 80)
        self.assertEqual(result["unknown_stack_extent_count"], 0)

    def test_large_stack_encodings(self):
        for info, count, payload, expected in [(0, 2, struct.pack("<H", 13789), 110312),
                                                (1, 3, struct.pack("<I", 600000), 600000)]:
            image = fixture()
            struct.pack_into("<I", image, 0x414, 0x2140)
            image[0x540] = 1
            data = bytes([1, 16, count, 0, 16, info * 16 + 1]) + payload
            image[0x500:0x500 + len(data)] = data
            self.assertEqual(read_pe(image)["stack_frames"][0x1000]["fixed_stack_bytes"], expected)

    def test_chained_unwind_and_nonallocating_saves(self):
        image = fixture()
        struct.pack_into("<I", image, 0x414, 0x2140)
        # Child: register save at an existing stack offset; parent: 32-byte
        # allocation and frame pointer. Saved slots must not count twice.
        image[0x500:0x508] = bytes([0x21, 8, 2, 0, 8, 0x34, 2, 0])
        struct.pack_into("<III", image, 0x508, 0x1040, 0x1050, 0x2140)
        image[0x540:0x548] = bytes([1, 8, 2, 5, 8, 3, 4, 0x32])
        frame = read_pe(image)["stack_frames"][0x1000]
        self.assertEqual(frame["fixed_stack_bytes"], 32)
        self.assertTrue(frame["uses_frame_pointer"])

    def test_unknown_unwind_is_not_a_zero_stack_claim(self):
        for info in (bytes([3, 0, 0, 0]), bytes([1, 1, 1, 0, 1, 7])):
            image = fixture()
            image[0x500:0x500 + len(info)] = info
            self.assertIsNone(read_pe(image)["stack_frames"][0x1000]["fixed_stack_bytes"])

    def test_rejects_bad_unwind_data(self):
        for info in (bytes([1, 1, 1, 0, 1, 1]), bytes([0x39, 0, 0, 0]),
                     bytes([0x21, 0, 0, 0]) + struct.pack("<III", 0x1000, 0x1020, 0x2100)):
            image = fixture()
            image[0x500:0x500 + len(info)] = info
            with self.assertRaises(AuditError):
                read_pe(image)

    def test_explicit_owner_chain_and_odd_code_padding(self):
        image = fixture()
        struct.pack_into("<I", image, 0x414, 0x2180)
        image[0x500:0x508] = bytes([0x21, 1, 1, 0, 1, 0x30, 0, 0])
        struct.pack_into("<III", image, 0x508, 0x1020, 0x1030, 0x2140)
        image[0x540:0x544] = bytes([0x21, 0, 0, 0])
        struct.pack_into("<III", image, 0x544, 0x1040, 0x1050, 0x2180)
        image[0x580:0x584] = bytes([1, 0, 0, 0])
        pe = read_pe(image, include_unwind_owners=True)
        self.assertEqual(pe["unwind_owners"][0x1000], {
            "primary_start_rva": 0x1040, "status": "explicit PE unwind chain",
            "chain": [[0x1000, 0x1020, 0x2100], [0x1020, 0x1030, 0x2140],
                      [0x1040, 0x1050, 0x2180]]})
        self.assertNotIn(0x1040, pe["unwind_owners"])
        self.assertNotIn("unwind_owners", read_pe(image))
        # Unsupported parent encoding is unknown, not a guessed owner.
        image[0x580] = 3
        self.assertIsNone(read_pe(image, include_unwind_owners=True)[
            "unwind_owners"][0x1000]["primary_start_rva"])

    def test_owner_chain_rejects_bad_parent_ranges_pointers_and_cycles(self):
        for parent in ((0x2000, 0x2010, 0x2140), (0x1050, 0x1040, 0x2140),
                       (0x1040, 0x1050, 0x3000), (0x1000, 0x1020, 0x2100)):
            image = fixture()
            struct.pack_into("<I", image, 0x414, 0x2180)
            image[0x580] = image[0x540] = 1
            image[0x500:0x504] = bytes([0x21, 0, 0, 0])
            struct.pack_into("<III", image, 0x504, *parent)
            with self.subTest(parent=parent), self.assertRaises(AuditError):
                read_pe(image, include_unwind_owners=True)
        image = fixture()
        struct.pack_into("<I", image, 0x408, 0x21f8)
        image[0x5f8:0x5fc] = bytes([0x21, 0, 0, 0])
        with self.assertRaises(AuditError):
            read_pe(image, include_unwind_owners=True)

    def test_file_accounting_preserves_padding_and_overlay(self):
        pe = read_pe(fixture())
        self.assertEqual(pe["headers_file_bytes"], 512)
        self.assertEqual(sum(s["file_bytes"] for s in pe["sections"]), 1024)
        self.assertEqual(pe["unassigned_file_bytes"], 32)
        self.assertEqual(pe["code_virtual_bytes"], 384)

    def test_unwind_sizes_do_not_absorb_alignment_or_leaf_code(self):
        pe = read_pe(fixture())
        symbols = read_map(map_lines([symbol(0, "?fn_80001000@recomp@gekko@@Z"),
                                    symbol(0x30, "leaf"), symbol(0x40, "helper", "runtime:helper.obj")]), pe)
        result = attribute(pe, symbols)
        self.assertEqual(result["unwind_covered_code_bytes"], 48)
        self.assertEqual(result["code_bytes_without_unwind_extent"], 336)
        self.assertEqual(result["groups"], {"generated_game_code": 32, "runtime": 16})
        self.assertEqual(result["largest_generated_functions"][0]["exclusive_unwind_bytes"], 32)

    def test_folded_aliases_are_not_double_counted(self):
        pe = read_pe(fixture())
        symbols = read_map(map_lines([symbol(0, "?fn_80001000@recomp@gekko@@Z"),
                                    symbol(0, "?fn_80002000@recomp@gekko@@Z")]), pe)
        result = attribute(pe, symbols)
        self.assertEqual(result["groups"]["generated_game_code"], 32)
        self.assertEqual(result["folded_or_mixed_guest_unwind_bytes"], 32)
        self.assertEqual(result["largest_generated_functions"], [])
        self.assertEqual(result["groups"]["no_exact_symbol"], 16)

    def test_cross_owner_aliases_stay_shared(self):
        pe = read_pe(fixture())
        symbols = read_map(map_lines([symbol(0, "game"), symbol(0, "native", "runtime:native.obj")]), pe)
        self.assertEqual(attribute(pe, symbols)["groups"]["shared_across_owners"], 32)

    def test_no_nearest_symbol_guess(self):
        pe = read_pe(fixture())
        symbols = read_map(map_lines([symbol(0x10, "interior_label")]), pe)
        self.assertEqual(attribute(pe, symbols)["exact_named_unwind_bytes"], 0)

    def test_rejects_wrong_map_identity(self):
        pe = read_pe(fixture())
        lines = map_lines([symbol(0, "function")])
        for index, value in [(0, " Preferred load address is 0000000150000000"),
                             (1, " Timestamp is 00000001")]:
            broken = lines.copy()
            broken[index] = value
            with self.assertRaises(AuditError):
                read_map(broken, pe)

    def test_rejects_inconsistent_symbol_address(self):
        pe = read_pe(fixture())
        with self.assertRaises(AuditError):
            read_map(map_lines([symbol(0, "function").replace("0000000140001000", "0000000140001001")]), pe)

    def test_rejects_truncation_wrong_machine_and_bad_directories(self):
        for offset, fmt, value in [(0x84, "<H", 0x14c), (0x98 + 140, "<I", 25),
                                   (0x400 + 4, "<I", 0x1300), (0x400 + 12, "<I", 0x1010)]:
            broken = fixture()
            struct.pack_into(fmt, broken, offset, value)
            with self.assertRaises(AuditError):
                read_pe(broken)
        with self.assertRaises(AuditError):
            read_pe(fixture()[:100])


if __name__ == "__main__":
    unittest.main()
