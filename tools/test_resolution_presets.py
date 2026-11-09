import os
import re
import unittest

import resolution_presets as rp

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


class PresetTableTests(unittest.TestCase):
    def test_python_table_equals_the_engines(self):
        with open(os.path.join(ROOT, "runtime", "lift", "render", "display_modes.cpp"), encoding="utf-8") as f:
            source = f.read()
        body = source[source.index("kPresets = {"):source.index("};", source.index("kPresets = {"))]
        engine = tuple((int(w), int(h), f) for w, h, f in re.findall(r"\{(\d+), (\d+), \"([0-9:]+)\"\}", body))
        self.assertEqual(engine, rp.PRESETS)

    def test_every_preset_is_classified_as_its_family(self):
        for w, h, family in rp.PRESETS:
            self.assertEqual(rp.classify(w, h)[0], family, "%dx%d" % (w, h))

    def test_brief_examples(self):
        self.assertEqual(rp.classify(3440, 1440), ("21:9", False))  # commercially 21:9, really 2.389:1
        self.assertEqual(rp.classify(1366, 768), ("16:9", False))   # near-16:9, not exact
        self.assertEqual(rp.classify(1920, 1080), ("16:9", True))
        self.assertEqual(rp.classify(5120, 1440), ("32:9", True))
        self.assertEqual(rp.classify(3840, 1200), ("32:10", True))
        self.assertEqual(rp.classify(1280, 1024), ("5:4", True))
        self.assertEqual(rp.classify(2844, 1600)[0], "16:9")         # what the old rule produced: near-16:9, not a preset
        self.assertNotIn((2844, 1600), [(w, h) for w, h, _ in rp.PRESETS])
        self.assertEqual(rp.classify(1000, 777), ("other", False))


class ChoiceTests(unittest.TestCase):
    def test_family_filters_and_native_and_custom(self):
        items = rp.choices("21:9", (1920, 1080, 60), [{"width": 2560, "height": 1080}, {"width": 1920, "height": 1080}])
        labels = [label for label, _ in items]
        self.assertEqual(labels[0], "Native (1920x1080)")
        self.assertEqual([size for _, size in items[1:-1]], [(2560, 1080), (3440, 1440), (3840, 1600), (5120, 2160)])
        self.assertEqual(labels[-1], "Custom")

    def test_monitor_modes_outside_the_presets_are_offered_once(self):
        items = rp.choices("16:9", None, [{"width": 1360, "height": 768}, {"width": 1920, "height": 1080}])
        sizes = [size for _, size in items if size]
        self.assertIn((1360, 768), sizes)
        self.assertEqual(sizes.count((1920, 1080)), 1)

    def test_fullscreen_offers_only_the_screens_shape(self):
        # Borderless fullscreen on a 1920x1080 screen: only exact 16:9 sizes (no bar, not even a pixel).
        items = rp.choices("Auto", (1920, 1080, 60), [{"width": 1366, "height": 768}, {"width": 1280, "height": 720},
                                                      {"width": 1280, "height": 1024}], fill_screen=True)
        sizes = [size for _, size in items if size]
        self.assertEqual(sizes, [(1280, 720), (1600, 900), (1920, 1080), (2560, 1440), (3200, 1800), (3840, 2160),
                                 (5120, 2880), (7680, 4320)])
        self.assertTrue(rp.same_shape(3840, 2160, (1920, 1080)))
        self.assertFalse(rp.same_shape(1366, 768, (1920, 1080)))
        self.assertFalse(rp.same_shape(3440, 1440, (1920, 1080)))

    def test_validate_and_flags(self):
        self.assertIsNone(rp.validate(3840, 2160))
        self.assertIsNotNone(rp.validate(16385, 1080))
        self.assertIsNotNone(rp.validate(100, 100))
        # No display flag ever: display.cfg (the Options menu's store) decides.
        self.assertEqual(rp.DISPLAY_MODES, (rp.GAME_SETTINGS,))
        self.assertEqual(rp.command_flags(rp.GAME_SETTINGS, (1920, 1080)), [])


if __name__ == "__main__":
    unittest.main()
