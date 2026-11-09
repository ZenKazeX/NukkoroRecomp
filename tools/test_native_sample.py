import os
import unittest

from native_sample import CodeIndex, StackWalker, frame_name


class FrameNameTests(unittest.TestCase):
    def setUp(self):
        self.index = CodeIndex({"unwind_extents": [(100, 120), (120, 160)]},
                               {100: {("?fn_801D6434@recomp@gekko@@YAXAEAVCpu@2@@Z", "recomp_017.obj")},
                                120: {("?run@Mixer@audio@shadow@@QEAAXXZ", "mixer.obj")}})
        self.image = (0x140000000, 0x1000)

    def test_guest_functions_are_named_by_guest_address(self):
        self.assertEqual(frame_name(self.index, self.image, 0x140000000 + 105, True), "fn_801D6434")

    def test_a_return_address_names_the_calling_instruction(self):
        # A return address one past the end of fn_801D6434 belongs to its call, not to the next function.
        self.assertEqual(frame_name(self.index, self.image, 0x140000000 + 120, False), "fn_801D6434")
        self.assertEqual(frame_name(self.index, self.image, 0x140000000 + 120, True),
                         "?run@Mixer@audio@shadow@@QEAAXXZ")

    def test_outside_the_image_is_left_to_the_module_list(self):
        self.assertIsNone(frame_name(self.index, self.image, 0x100, True))
        self.assertEqual(frame_name(self.index, self.image, 0x140000000 + 170, True), "image:unattributed")

    @unittest.skipUnless(os.name == "nt", "dbghelp is Windows-only")
    def test_stackframe64_layout(self):
        import ctypes
        walker = StackWalker(ctypes.windll.kernel32.GetCurrentProcess())
        self.assertEqual(ctypes.sizeof(walker.Frame), 264)
        walker.close()


class CodeIndexTests(unittest.TestCase):
    def setUp(self):
        self.index = CodeIndex({"unwind_extents": [(100, 120), (130, 160), (200, 240)]},
                               {100: {("first", "one.obj"), ("folded", "two.obj")},
                                125: {("leaf_without_extent", "leaf.obj")},
                                200: {("last", "last.obj")}})

    def test_boundaries_are_half_open(self):
        for rva in (100, 119):
            self.assertEqual(self.index.lookup(rva)["start_rva"], 100)
        for rva in (-1, 0, 99, 120, 129, 160, 199, 240):
            self.assertIsNone(self.index.lookup(rva))

    def test_folded_names_are_all_retained(self):
        self.assertEqual(self.index.lookup(105)["aliases"], [["first", "one.obj"], ["folded", "two.obj"]])

    def test_chained_owner_is_additive_and_retains_all_primary_aliases(self):
        self.index.unwind_owners[130] = {"primary_start_rva": 100,
            "chain": [[130, 160, 300], [100, 120, 320]], "status": "explicit PE unwind chain"}
        self.index.symbols[130] = {("fragment_label", "child.obj")}
        result = self.index.lookup(150)
        self.assertEqual(result["aliases"], [["fragment_label", "child.obj"]])
        self.assertEqual(result["unwind_owner"]["aliases"], [["first", "one.obj"], ["folded", "two.obj"]])
        self.assertEqual(result["unwind_owner"]["primary_start_rva"], 100)
        self.assertIsNone(self.index.lookup(160))
        self.assertNotIn("unwind_owner", self.index.lookup(110))
        for primary in (101, None):
            self.index.unwind_owners[130]["primary_start_rva"] = primary
            self.assertEqual(self.index.lookup(150)["unwind_owner"]["aliases"], [])

    def test_leaf_and_unnamed_ranges_are_not_guessed(self):
        self.assertIsNone(self.index.lookup(125))
        self.assertEqual(self.index.lookup(150)["aliases"], [])
        self.assertEqual(self.index.lookup(239)["aliases"], [["last", "last.obj"]])


if __name__ == "__main__":
    unittest.main()
