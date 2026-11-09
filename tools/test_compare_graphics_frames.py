import contextlib
import io
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

import numpy as np
from PIL import Image

if __package__:
    from . import compare_graphics_frames as compare
else:
    import compare_graphics_frames as compare


class ArrayTests(unittest.TestCase):
    def test_identical(self):
        pixels = np.random.default_rng(4).integers(0, 256, (9, 11, 3), dtype=np.uint8)
        result = compare.compare_arrays(pixels, pixels.copy())
        self.assertTrue(result["exact_match"])
        self.assertEqual(result["mismatch_pixels"], 0)
        self.assertIsNone(result["bbox_xyxy_exclusive"])
        self.assertEqual(result["rgb"]["R"], {"mae": 0.0, "rmse": 0.0, "max": 0.0})
        self.assertEqual(result["alpha"]["status"], "absent")
        self.assertIsNone(result["alpha"]["metrics"])
        self.assertAlmostEqual(result["ssim"]["mean"], 1.0)
        self.assertEqual(result["ssim"]["window"], [7, 7])
        for axis in ("x", "y"):
            self.assertEqual(result["edge_gradient_error"][axis]["max"], 0.0)

    def test_single_channel_and_edge_known_values(self):
        a = np.zeros((1, 2, 3), dtype=np.uint8)
        b = a.copy()
        b[0, 1, 0] = 6
        result = compare.compare_arrays(a, b)
        self.assertFalse(result["exact_match"])
        self.assertEqual(result["mismatch_pixels"], 1)
        self.assertEqual(result["bbox_xyxy_exclusive"], [1, 0, 2, 1])
        self.assertEqual(result["rgb"]["R"]["mae"], 3)
        self.assertAlmostEqual(result["rgb"]["R"]["rmse"], np.sqrt(18))
        self.assertEqual(result["rgb"]["R"]["max"], 6)
        self.assertEqual(result["rgb"]["G"]["max"], 0)
        edge = result["edge_gradient_error"]
        self.assertEqual(edge["x"]["mae"], 2)
        self.assertAlmostEqual(edge["x"]["rmse"], np.sqrt(12))
        self.assertEqual(edge["x"]["max"], 6)
        self.assertIsNone(edge["y"])

    def test_alpha_only_mismatch(self):
        a = np.zeros((2, 2, 4), dtype=np.uint8)
        b = a.copy()
        b[1, 0, 3] = 128
        result = compare.compare_arrays(a, b)
        self.assertFalse(result["exact_match"])
        self.assertEqual(result["mismatch_pixels"], 1)
        self.assertEqual(result["bbox_xyxy_exclusive"], [0, 1, 1, 2])
        self.assertEqual(result["alpha"]["metrics"], {"mae": 32, "rmse": 64, "max": 128})
        self.assertTrue(result["alpha"]["reference_present"])
        self.assertEqual(result["rgb"]["R"]["max"], 0)
        self.assertEqual(result["ssim"]["mean"], 1)

    def test_invalid_arrays_and_missing_alpha(self):
        rgb = np.zeros((2, 3, 3), dtype=np.uint8)
        for other in (np.zeros((3, 2, 3), dtype=np.uint8),
                      np.zeros((2, 3, 4), dtype=np.uint8), rgb.astype(np.float32)):
            with self.subTest(shape=other.shape, dtype=other.dtype), self.assertRaises(ValueError):
                compare.compare_arrays(rgb, other)
        for pixels in (np.empty((0, 2, 3), dtype=np.uint8), np.zeros((2, 3), dtype=np.uint8)):
            with self.assertRaises(ValueError):
                compare.compare_arrays(pixels, pixels)

    def test_small_constant_and_black_white_ssim(self):
        expected = (0.01 * 255) ** 2 / (255 ** 2 + (0.01 * 255) ** 2)
        for shape in ((1, 1, 3), (2, 3, 3), (8, 9, 3)):
            with self.subTest(shape=shape):
                black = np.zeros(shape, dtype=np.uint8)
                white = np.full(shape, 255, dtype=np.uint8)
                result = compare.local_ssim(black, white)
                self.assertAlmostEqual(result["mean"], expected, places=14)
                self.assertEqual(result["window"], [min(7, *shape[:2])] * 2)
                self.assertEqual(result["range"], [0, 255])
                self.assertEqual((result["K1"], result["K2"], result["L"]), (0.01, 0.03, 255))
                self.assertIn("population", result["method"])
                self.assertEqual(compare.local_ssim(white, white)["mean"], 1)
        edge = compare.compare_arrays(black[:1, :1], black[:1, :1])["edge_gradient_error"]
        self.assertIsNone(edge["x"])
        self.assertIsNone(edge["y"])

    def test_population_ssim_and_roundoff(self):
        a = np.repeat(np.array([[0, 0], [255, 255]], dtype=np.uint8)[..., None], 3, axis=2)
        b = a.transpose(1, 0, 2)
        c2 = (0.03 * 255) ** 2
        self.assertAlmostEqual(compare.local_ssim(a, b)["mean"], c2 / (2 * 127.5 ** 2 + c2))
        self.assertLess(compare.local_ssim(a, 255 - a)["mean"], 0)
        flat = np.full((10, 12, 3), 254, dtype=np.uint8)
        near = flat.copy()
        near[5, 6, 1] = 255
        result = compare.local_ssim(flat, near)
        self.assertTrue(np.isfinite(result["mean"]))
        self.assertLessEqual(result["mean"], 1)
        self.assertGreaterEqual(result["mean"], -1)
        self.assertAlmostEqual(result["mean"], compare.local_ssim(near, flat)["mean"])

    def test_temporal_corruption_and_signed_range(self):
        black = np.zeros((2, 3, 3), dtype=np.uint8)
        ten, twelve = black + 10, black + 12
        good = compare.temporal_error(black, ten, black, ten)
        bad = compare.temporal_error(black, ten, black, twelve)
        for channel in "RGB":
            self.assertEqual(good["rgb"][channel]["max"], 0)
            self.assertEqual(bad["rgb"][channel], {"mae": 2, "rmse": 2, "max": 2})
        extreme = compare.temporal_error(black + 255, black, black, black + 255)
        self.assertEqual(extreme["rgb"]["R"]["max"], 510)
        self.assertEqual(bad["alpha"]["status"], "absent")
        rgba = np.zeros((1, 1, 4), dtype=np.uint8)
        changed = rgba.copy()
        changed[..., 3] = 255
        self.assertEqual(compare.temporal_error(rgba, rgba, rgba, changed)["alpha"]["metrics"]["max"], 255)


class CaptureTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.left, self.right = self.root / "reference", self.root / "candidate"
        self.left.mkdir()
        self.right.mkdir()
        self.output = self.root / "report.json"

    def image(self, path, pixels=None):
        Image.fromarray(np.zeros((2, 3, 3), dtype=np.uint8) if pixels is None else pixels).save(path)
        return path

    def invoke(self, reference=None, candidate=None, *extra):
        arguments = [str(reference or self.left), str(candidate or self.right),
                     "--output", str(self.output), *map(str, extra)]
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            return compare.main(arguments)

    def report(self):
        return json.loads(self.output.read_text(encoding="utf-8"))

    def test_decoded_identity_across_file_formats(self):
        a = self.image(self.left / "one.png")
        b = self.image(self.right / "two.ppm")
        self.assertNotEqual(a.read_bytes(), b.read_bytes())
        self.assertEqual(self.invoke(a, b), 0)
        result = self.report()
        self.assertEqual(result["dolphin_parity"], "not_assessed")
        self.assertEqual(result["dolphin_reference_status"], "not_verified_by_tool")
        self.assertEqual(result["temporal"]["status"], "unavailable_single_frame")

    def test_directory_numeric_order_and_temporal_corruption(self):
        for identity in (10, 1, 2):
            pixels = np.full((2, 3, 3), identity, dtype=np.uint8)
            self.image(self.left / f"frame{identity}.png", pixels)
            self.image(self.right / f"frame{identity}.png", pixels + (3 if identity == 2 else 0))
        self.assertEqual(self.invoke(), 1)
        result = self.report()
        self.assertEqual([frame["frame_id"] for frame in result["frames"]], [1, 2, 10])
        self.assertIn("ascending trailing numeric ID", result["pairing"])
        transitions = result["temporal"]["transitions"]
        self.assertEqual([row["frame_id_delta"] for row in transitions], [1, 8])
        self.assertEqual([row["rgb"]["R"]["mae"] for row in transitions], [3, 3])

    def test_empty_capture_invalid_report(self):
        self.assertEqual(self.invoke(), 2)
        self.assertEqual(self.report()["status"], "invalid_capture")
        self.assertIsNone(self.report()["exact_match"])
        self.assertIn("empty capture", self.report()["error"])

    def test_missing_unmatched_and_mixed_inputs(self):
        self.image(self.left / "frame1.png")
        self.image(self.right / "frame2.png")
        self.assertEqual(self.invoke(), 2)
        self.assertIn("unmatched frame names", self.report()["error"])
        for a, b in ((self.left, self.root / "missing"),
                     (self.left / "frame1.png", self.right)):
            with self.assertRaises(ValueError):
                compare.matched_paths(a, b)

    def test_duplicate_numeric_identities(self):
        self.image(self.left / "frame01.png")
        for name in ("frame01.ppm", "other1.ppm"):
            with self.subTest(name=name):
                duplicate = self.image(self.left / name)
                with self.assertRaisesRegex(ValueError, "duplicate numeric"):
                    compare.capture_files(self.left)
                duplicate.unlink()

    def test_unsupported_and_ambiguous_directory_entries(self):
        self.image(self.left / "frame1.png")
        for name in ("notes.txt", "frame2.jpg", "unnumbered.png"):
            entry = self.left / name
            entry.write_bytes(b"not an image")
            with self.subTest(name=name), self.assertRaises(ValueError):
                compare.capture_files(self.left)
            entry.unlink()
        (self.left / "nested").mkdir()
        with self.assertRaises(ValueError):
            compare.capture_files(self.left)

    def test_unreadable_unsupported_truncated_and_disguised_images(self):
        for name, data in (("1.png", b"garbage"), ("1.bmp", b"garbage"),
                           ("1.ppm", b"P6\n1 1\n255\n\x00")):
            path = self.left / name
            path.write_bytes(data)
            with self.subTest(name=name), self.assertRaises(compare.INVALID):
                compare.load_image(path)
        path = self.image(self.left / "truncated.png")
        path.write_bytes(path.read_bytes()[:-15])
        with self.assertRaises(compare.INVALID):
            compare.load_image(path)
        Image.new("RGB", (2, 2)).save(self.left / "disguised.png", format="JPEG")
        with self.assertRaises(ValueError):
            compare.load_image(self.left / "disguised.png")

    def test_high_bit_depth_refused_before_lossy_decode(self):
        def chunk(kind, data):
            return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
        header = struct.pack(">IIBBBBB", 1, 1, 16, 2, 0, 0, 0)
        png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header)
        png += chunk(b"IDAT", zlib.compress(b"\x00\x00\x01\x00\x02\x00\x03")) + chunk(b"IEND", b"")
        for name, data in (("16.png", png), ("16.ppm", b"P6\n1 1\n65535\n" + b"\x00" * 6)):
            path = self.left / name
            path.write_bytes(data)
            with self.subTest(name=name), self.assertRaises(ValueError):
                compare.load_image(path)

    def test_palette_transparency_is_real_alpha(self):
        path = self.left / "palette.png"
        image = Image.new("P", (1, 1))
        image.putpalette([10, 20, 30] + [0] * 765)
        image.save(path, transparency=0)
        self.assertEqual(compare.load_image(path).tolist(), [[[10, 20, 30, 0]]])
        image.save(self.left / "opaque.png")
        self.assertEqual(compare.load_image(self.left / "opaque.png").shape, (1, 1, 3))

    def test_animation_is_not_silently_reduced(self):
        path = self.left / "animated.png"
        Image.new("RGB", (2, 2), "black").save(
            path, save_all=True, append_images=[Image.new("RGB", (2, 2), "white")], duration=100)
        with self.assertRaisesRegex(ValueError, "multiple frames"):
            compare.load_image(path)

    def test_sequence_shape_and_alpha_changes_fail_closed(self):
        for channels, height in ((3, 4), (4, 2)):
            with self.subTest(channels=channels):
                for directory in (self.left, self.right):
                    self.image(directory / "1.png")
                    self.image(directory / "2.png", np.zeros((height, 3, channels), dtype=np.uint8))
                with self.assertRaisesRegex(ValueError, "dimension/channel mismatch"):
                    compare.compare_captures(self.left, self.right)

    def test_existing_output_is_untouched(self):
        self.output.write_text("sentinel", encoding="utf-8")
        self.assertEqual(self.invoke(), 2)
        self.assertEqual(self.output.read_text(encoding="utf-8"), "sentinel")

    def test_output_inside_capture_and_missing_parent_refused(self):
        for output in (self.left / "result.json", self.root / "absent" / "result.json"):
            self.output = output
            self.assertEqual(self.invoke(), 2)
            self.assertFalse(output.exists())

    def test_diagnostic_pixels_and_existing_directory_refused(self):
        a = np.zeros((2, 3, 4), dtype=np.uint8)
        b = a.copy()
        b[1, 2] = [9, 0, 0, 11]
        self.image(self.left / "1.png", a)
        self.image(self.right / "1.png", b)
        directory = self.root / "diffs"
        self.assertEqual(self.invoke(None, None, "--diff-dir", directory), 1)
        paths = self.report()["frames"][0]["diagnostic_images"]
        for kind, expected in (("rgb_absolute", [9, 0, 0]), ("alpha_absolute", 11), ("mismatch_mask", 255)):
            with Image.open(paths[kind]) as image:
                self.assertEqual(np.asarray(image)[1, 2].tolist(), expected)
        original = {path: path.read_bytes() for path in directory.iterdir()}
        self.output = self.root / "another.json"
        self.assertEqual(self.invoke(None, None, "--diff-dir", directory), 2)
        self.assertEqual(original, {path: path.read_bytes() for path in directory.iterdir()})

    def test_cli_exit_statuses(self):
        reference = self.image(self.left / "a.png")
        candidate = self.right / "b.png"
        for code, pixels, status in ((0, np.zeros((2, 3, 3), dtype=np.uint8), "match"),
                                     (1, np.ones((2, 3, 3), dtype=np.uint8), "mismatch"),
                                     (2, np.zeros((3, 3, 3), dtype=np.uint8), "invalid_capture")):
            with self.subTest(code=code):
                self.image(candidate, pixels)
                self.output = self.root / f"exit{code}.json"
                process = subprocess.run(
                    [sys.executable, str(Path(compare.__file__)), str(reference), str(candidate),
                     "--output", str(self.output)], capture_output=True, text=True, timeout=30,
                    env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"})
                self.assertEqual(process.returncode, code, process.stderr)
                self.assertEqual(self.report()["status"], status)
                self.assertEqual(self.report()["dolphin_parity"], "not_assessed")

    def test_cli_requires_output_and_rejects_relaxed_threshold(self):
        for extra in ([], ["--output", str(self.output), "--threshold", "1"]):
            with self.subTest(extra=extra), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as caught:
                    compare.main([str(self.left), str(self.right), *extra])
                self.assertEqual(caught.exception.code, 2)
        self.assertFalse(self.output.exists())


if __name__ == "__main__":
    unittest.main()
