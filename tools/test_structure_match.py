"""Unit tests for tools/platform/structure_match.py: instruction tokens and templates on fixed words (no DOL needed)."""
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "platform"))
import structure_match as sm  # noqa: E402


class Image:
    def __init__(self, base, words):
        self.base, self.words = base, words

    def word(self, at):
        return self.words[(at - self.base) // 4]


class TokenTests(unittest.TestCase):
    def test_branches(self):
        self.assertEqual(sm.token(0x4E800020), "blr")         # blr
        self.assertEqual(sm.token(0x4E800420), "bctr")        # bctr
        self.assertEqual(sm.token(0x48000005), "bl")          # bl +4
        self.assertEqual(sm.token(0x48000004), "b")           # b +4
        self.assertEqual(sm.token(0x41820010), "bc")          # beq +16

    def test_registers_and_immediates_are_dropped(self):
        self.assertEqual(sm.token(0x806D5BDC), sm.token(0x80645BDC))  # lwz r3,x(r13) vs lwz r3,x(r4): same token
        self.assertEqual(sm.token(0x38600000), sm.token(0x38600001))  # li r3,0 vs li r3,1


class TemplateTests(unittest.TestCase):
    def run_words(self, words, names=None):
        base = 0x80001000
        fp, info = sm.features(Image(base, words), base, 4 * len(words), names or {})
        return fp, info, sm.template(info, names or {})

    def test_empty_body(self):
        _, info, t = self.run_words([0x4E800020])
        self.assertEqual(info["ins"], 1)
        self.assertEqual(t, "leaf-compute")

    def test_leaf_getter(self):
        _, info, t = self.run_words([0x806D5BDC, 0x4E800020])  # lwz r3,0x5BDC(r13); blr
        self.assertIn("sda-load", info["sem"])
        self.assertEqual(t, "leaf-getter")

    def test_leaf_setter(self):
        _, info, t = self.run_words([0x906D5BDC, 0x4E800020])  # stw r3,0x5BDC(r13); blr
        self.assertEqual(t, "leaf-setter")

    def test_gx_fifo_writer(self):
        # lis r4,0xCC01; addi r4,r4,-0x8000 (0xCC008000); stw r3,0(r4); blr
        _, info, t = self.run_words([0x3C80CC01, 0x38848000, 0x90640000, 0x4E800020])
        self.assertIn("gx-fifo", info["sem"])
        self.assertEqual(t, "gx-fifo-writer")

    def test_interrupt_bracket(self):
        base = 0x80001000
        disable, restore = 0x80002000, 0x80003000
        names = {disable: "OSDisableInterrupts", restore: "OSRestoreInterrupts"}
        bl = lambda frm, to: 0x48000001 | ((to - frm) & 0x03FFFFFC)  # noqa: E731
        words = [bl(base, disable), 0x906D5BDC, bl(base + 8, restore), 0x4E800020]
        _, info, t = self.run_words(words, names)
        self.assertIn("call:OSDisableInterrupts", info["sem"])
        self.assertEqual(t, "interrupt-bracketed-update")

    def test_twins_are_identical_fingerprints(self):
        a, _, _ = self.run_words([0x906D5BDC, 0x4E800020])
        b, _, _ = self.run_words([0x906D5BE0, 0x4E800020])     # another global: the same structure
        self.assertEqual(sm.jaccard(a, b), 1.0)


if __name__ == "__main__":
    unittest.main()
