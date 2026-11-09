import tempfile
from pathlib import Path
import unittest
import struct

from native_watch import layout_fields, watch_spec, register_values, capture_spec, until_spec


class NativeWatchTests(unittest.TestCase):
    def test_terminal_watch_requires_valid_index_link_and_positive_count(self):
        self.assertEqual(until_spec('1:0x80097a08:88'), (1, 0x80097a08, 88))
        for value in ('4:0x80097a08:1', '0:0x100000000:1', '0:3:0', '-1:3:2', '0:3'):
            with self.assertRaises(ValueError): until_spec(value)

    def test_capture_spans_are_bounded_without_forcing_watch_alignment(self):
        self.assertEqual(capture_spec('0x8060c1a9:63'), (0x8060c1a9, 63))
        for value in ('-1:4', '0xfffffffe:4', '0x80000000:4097', '0x80000000:0'):
            with self.assertRaises(ValueError): capture_spec(value)

    def test_watch_alignment_and_extent(self):
        self.assertEqual(watch_spec("0x8060be40:8"), (0x8060be40, 8))
        self.assertEqual(watch_spec("0xfffffffc:4"), (0xfffffffc, 4))
        for spec in ("0x8060be41:8", "0x8060be40:3", "-4:4", "0x100000000:1"):
            with self.assertRaises(ValueError):
                watch_spec(spec)

    def test_layout_must_identify_every_field_once(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "layout.txt"
            text = "class gekko::Cpu size(11392):\n 0 | gpr\n128 | fpr\n648 | lr\n664 | fpscr\n5024 | fm_window_\n"
            path.write_text(text)
            self.assertEqual(layout_fields(path)["fm_window_"], 5024)
            path.write_text(text + "8000 | fm_window_\n")
            with self.assertRaises(ValueError):
                layout_fields(path)
            path.write_text(text.replace("128 | fpr", "128 | unrelated"))
            with self.assertRaises(ValueError):
                layout_fields(path)
            path.write_text(text.replace("5024 | fm_window_", "11388 | fm_window_"))
            with self.assertRaises(ValueError):
                layout_fields(path)

    def test_register_values_preserve_raw_bits_and_use_layout_offsets(self):
        data = bytearray(800)
        struct.pack_into('<32I', data, 16, *range(32))
        struct.pack_into('<64Q', data, 144, *(0x7ff8000000000000 + i for i in range(64)))
        struct.pack_into('<I', data, 680, 0x12345678)
        struct.pack_into('<I', data, 700, 0xA0080004)
        fields = {'gpr':16, 'fpr':144, 'lr':680, 'fpscr':700}
        value = register_values(lambda at, size: data[at-4096:at-4096+size], 4096, fields)
        self.assertEqual(value['gpr'][31], '0000001f')
        self.assertEqual(value['fpr_lanes'][31], ['7ff800000000003e','7ff800000000003f'])
        self.assertEqual(value['lr'], '12345678')
        self.assertEqual(value['fpscr'], 'a0080004')


if __name__ == "__main__":
    unittest.main()
