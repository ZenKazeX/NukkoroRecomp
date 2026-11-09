import struct
import unittest

from windows_cpu_topology_observation import parse_cpu_sets


def cpu(ident=8, group=0, logical=2, efficiency=1):
    return struct.pack("<IIIH6B", 32, 0, ident, group, logical, 1, 0, 0, efficiency, 0) + bytes(12)


class TopologyTests(unittest.TestCase):
    def test_current_sdk_coordinates_preserve_groups(self):
        result = parse_cpu_sets(cpu() + cpu(9, 1, 2, 0))
        self.assertEqual(result["raw_bytes"], 64)
        self.assertEqual([(x["group"], x["logical_processor"], x["efficiency_class"]) for x in result["cpu_sets"]],
                         [(0, 2, 1), (1, 2, 0)])

    def test_unknown_type_retained_as_coverage_not_guessed(self):
        result = parse_cpu_sets(struct.pack("<II", 8, 23) + cpu())
        self.assertEqual(result["unknown_record_types"], 1)
        self.assertEqual(len(result["cpu_sets"]), 1)
        with self.assertRaises(ValueError):
            parse_cpu_sets(struct.pack("<II", 8, 23))

    def test_malformed_extent_and_duplicate_identity_rejected(self):
        for raw in (b"", cpu()[:-1], cpu() + b"x", struct.pack("<II", 0, 0),
                    struct.pack("<II", 8, 0), cpu()+cpu(), cpu()+cpu(9), cpu(logical=64),
                    b"x"*((1<<20)+1)):
            with self.subTest(size=len(raw)), self.assertRaises(ValueError):
                parse_cpu_sets(raw)


if __name__ == "__main__":
    unittest.main()
