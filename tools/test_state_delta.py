import unittest
import struct
from types import SimpleNamespace

from state_delta import cpu_blocks, describe, spans


class StateDeltaTests(unittest.TestCase):
    def test_supported_versions_preserve_known_cpu_layout(self):
        blocks = [bytes(4872), b"interrupt", b"timeline"]
        data = bytes(20) + b"".join(struct.pack("<Q", len(b)) + b for b in blocks)
        for version in (19, 20, 21, 22, 23):
            self.assertEqual(cpu_blocks(SimpleNamespace(version=version, data=data)), blocks)
        with self.assertRaises(ValueError):
            cpu_blocks(SimpleNamespace(version=24, data=data))

    def test_trailing_length_difference_is_counted(self):
        from state_delta import describe_trailing
        result = describe_trailing(b"abcd", b"abXdEXTRA")
        self.assertEqual(result["changed_bytes"], 1)
        self.assertEqual((result["control_bytes"], result["candidate_bytes"], result["unmatched_bytes"]), (4, 9, 5))

    def test_equal_and_empty(self):
        self.assertEqual(list(spans(b"", b"")), [])
        self.assertEqual(describe(b"same", b"same")["changed_bytes"], 0)

    def test_exact_ranges_cross_blocks_and_final_byte(self):
        before = bytes(8200)
        after = bytearray(before)
        after[4094:4099] = b"abcde"
        after[8199] = 1
        self.assertEqual(list(spans(before, after)), [(4094, 4099), (8199, 8200)])
        result = describe(before, after, 100)
        self.assertEqual(result["changed_bytes"], 6)
        self.assertEqual(result["ranges"][0]["start"], 4194)
        self.assertEqual(result["ranges"][0]["after_hex"], "6162636465")

    def test_counts_remain_complete_when_details_truncate(self):
        result = describe(bytes(2000), b"\0\1" * 1000)
        self.assertEqual(result["changed_bytes"], 1000)
        self.assertEqual(result["changed_ranges"], 1000)
        self.assertTrue(result["range_details_truncated"])

    def test_length_mismatch_is_not_silently_zipped(self):
        with self.assertRaises(ValueError):
            describe(b"a", b"ab")


if __name__ == "__main__":
    unittest.main()
