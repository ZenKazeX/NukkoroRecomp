import math
import struct
import unittest
import uuid

from wasapi_loopback_capture import FLOAT_GUID, PCM_GUID, normalized_samples, parse_format, silence_bytes, wave_header


def format_blob(tag=1, channels=2, rate=48000, bits=16, subtype=None, valid_bits=None):
    extra = b""
    if subtype:
        tag = 0xFFFE
        extra = struct.pack("<HI", valid_bits or bits, 3) + uuid.UUID(subtype).bytes_le
    align = channels * bits // 8
    return struct.pack("<HHIIHHH", tag, channels, rate, rate * align, align, bits, len(extra)) + extra


class FormatTests(unittest.TestCase):
    def test_pcm_float_and_extensible(self):
        for blob, kind in [(format_blob(), "pcm"), (format_blob(tag=3, bits=32), "float"),
                           (format_blob(bits=32, subtype=PCM_GUID, valid_bits=24), "pcm"),
                           (format_blob(bits=32, subtype=FLOAT_GUID), "float")]:
            fmt = parse_format(blob)
            self.assertEqual(fmt["kind"], kind)
            self.assertTrue(fmt["standard_wave"])
            header = wave_header(blob, 37)
            self.assertEqual(header[:4], b"RIFF")
            self.assertEqual(struct.unpack_from("<I", header, 4)[0], len(header) - 8 + 37 * fmt["block_align"])
            self.assertEqual(header[-8:-4], b"data")
            self.assertEqual(struct.unpack_from("<I", header, len(header)-4)[0], 37 * fmt["block_align"])

    def test_signed_pcm_and_float_conversion(self):
        for bits in (16, 24, 32):
            limit = 1 << (bits-1)
            raw = b"".join(value.to_bytes(bits // 8, "little", signed=True) for value in (-limit, 0, limit-1))
            fmt = parse_format(format_blob(channels=1, bits=bits))
            self.assertEqual(list(normalized_samples(raw, fmt)), [-1, 0, (limit-1)/limit])
        fmt = parse_format(format_blob(channels=1, bits=8))
        self.assertEqual(list(normalized_samples(bytes([0, 128, 255]), fmt)), [-1, 0, 127/128])
        fmt = parse_format(format_blob(tag=3, channels=1, bits=32))
        values = list(normalized_samples(struct.pack("<fff", -.5, 1.25, math.nan), fmt))
        self.assertEqual(values[:2], [-.5, 1.25])
        self.assertTrue(math.isnan(values[2]))

    def test_invalid_stride_length_and_unknown_subtype(self):
        with self.assertRaises(ValueError): parse_format(b"\0" * 17)
        with self.assertRaises(ValueError): parse_format(format_blob() + b"extra")
        bad = bytearray(format_blob())
        struct.pack_into("<H", bad, 12, 3)
        with self.assertRaises(ValueError): parse_format(bytes(bad))
        blob = format_blob(bits=32, subtype="00000055-0000-0010-8000-00aa00389b71")
        self.assertFalse(parse_format(blob)["standard_wave"])
        with self.assertRaises(ValueError): wave_header(blob, 10)
        with self.assertRaises(ValueError): list(normalized_samples(b"\0", parse_format(format_blob())))

    def test_odd_pcm_data_riff_padding_and_riff_bounds(self):
        blob = format_blob(channels=1, bits=8)
        header = wave_header(blob, 3)
        self.assertEqual(struct.unpack_from("<I", header, 4)[0], len(header) - 8 + 4)
        with self.assertRaises(ValueError): wave_header(format_blob(), 2**32)

    def test_silent_pcm_packets_keep_mathematical_zero(self):
        for bits in (8, 16, 24, 32):
            fmt = parse_format(format_blob(bits=bits))
            self.assertEqual(list(normalized_samples(silence_bytes(3, fmt), fmt)), [0.0] * 6)


if __name__ == "__main__":
    unittest.main()
