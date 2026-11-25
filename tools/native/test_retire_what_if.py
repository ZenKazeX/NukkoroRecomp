#!/usr/bin/env python3
"""Tests for the pure parts of retire_what_if.py (the generator runs themselves are not unit-tested)."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import retire_what_if as rw  # noqa: E402

OWNERS = """\
# Native owners.
# runtime/src/gekko/native_trk.cpp (trk::install): the debugger console
0x80370BF0 DBInit
0x803A05A0 TRK console write
#
# runtime/src/gekko/native_audio.cpp (NativeAudio::install)
0x80382244 AIInit
0x80382300 AIStartDMA
# runtime/src/gekko/native_trk.cpp (a second section of the same file)
0x803A065C TRK console read
"""

ROOTS = """\
# liveness roots (generated)
0x80370BF0
0x803A05A0
0x803A065C
0x80382244
0x80399999  # an address only the retired file names, not an owner row
0x80300000
"""

NAMING = {
    0x80370BF0: {"runtime/src/gekko/native_trk.cpp", "runtime/include/native_pc_boot_contract.h"},
    0x803A05A0: {"runtime/src/gekko/native_trk.cpp", "runtime/include/gekko/native_trk.h"},
    0x803A065C: {"runtime/src/gekko/native_trk.cpp", "runtime/tests/trk_tests.cpp"},
    0x80382244: {"runtime/src/gekko/native_audio.cpp"},
    0x80399999: {"runtime/include/gekko/native_trk.h"},
    0x80300000: {"runtime/src/gekko/system.cpp"},
}
TRK = {"runtime/src/gekko/native_trk.cpp", "runtime/include/gekko/native_trk.h"}


def root_addresses(text: str) -> list[int]:
    return [int(m.group(1), 16) for line in text.splitlines() if (m := rw.ROOT_LINE.match(line.split("#")[0]))]


class OwnerListTests(unittest.TestCase):
    def test_section_removes_every_section_of_the_file(self):
        text, removed = rw.split_owner_list(OWNERS, "native_trk.cpp", set())
        self.assertEqual([a for a, _, _ in removed], [0x80370BF0, 0x803A05A0, 0x803A065C])
        self.assertEqual(removed[0][1:], ("DBInit", "runtime/src/gekko/native_trk.cpp"))
        self.assertNotIn("0x80370BF0", text)
        self.assertIn("0x80382244 AIInit", text)
        self.assertIn("# runtime/src/gekko/native_trk.cpp (trk::install)", text)   # comments stay

    def test_address_mode(self):
        text, removed = rw.split_owner_list(OWNERS, None, {0x80382244})
        self.assertEqual([(a, s) for a, _, s in removed], [(0x80382244, "runtime/src/gekko/native_audio.cpp")])
        self.assertIn("0x80382300 AIStartDMA", text)

    def test_nothing_matches(self):
        text, removed = rw.split_owner_list(OWNERS, "native_none.cpp", {0x80000000})
        self.assertEqual(removed, [])
        self.assertEqual(text, OWNERS)


class RootTests(unittest.TestCase):
    retired = {0x80370BF0, 0x803A05A0, 0x803A065C}

    def test_whole_file_with_tests_rooting(self):
        text, dropped, held = rw.plan_roots(ROOTS, NAMING, self.retired, TRK, True, False)
        self.assertEqual(dropped, [0x80399999, 0x803A05A0])            # also the non-owner root only that file names
        self.assertEqual(held, {0x80370BF0: ["runtime/include/native_pc_boot_contract.h"],
                                0x803A065C: ["runtime/tests/trk_tests.cpp"]})
        self.assertEqual(root_addresses(text), [0x80370BF0, 0x803A065C, 0x80382244, 0x80300000])
        self.assertIn("# liveness roots (generated)", text)

    def test_whole_file_without_tests(self):
        _, dropped, held = rw.plan_roots(ROOTS, NAMING, self.retired, TRK, True, True)
        self.assertEqual(dropped, [0x80399999, 0x803A05A0, 0x803A065C])   # the test file does not root the game image
        self.assertEqual(list(held), [0x80370BF0])

    def test_address_mode_drops_only_retired_roots(self):
        _, dropped, held = rw.plan_roots(ROOTS, NAMING, {0x803A05A0}, TRK, False, False)
        self.assertEqual((dropped, held), ([0x803A05A0], {}))             # 0x80399999 stays: the file is not deleted

    def test_no_source_given_keeps_the_root_and_says_who_names_it(self):
        _, dropped, held = rw.plan_roots(ROOTS, NAMING, {0x80382244}, set(), False, False)
        self.assertEqual((dropped, held), ([], {0x80382244: ["runtime/src/gekko/native_audio.cpp"]}))

    def test_retired_address_that_is_no_root(self):
        _, dropped, held = rw.plan_roots(ROOTS, NAMING, {0x80000004}, set(), False, False)
        self.assertEqual((dropped, held), ([], {}))


class RenderTests(unittest.TestCase):
    def test_render_states_refusal_and_errors(self):
        results = [
            {"config": "game", "build_dir": "g", "generator": "ppcrecomp.exe", "roots_file": "liveness-roots-game.txt",
             "roots_without_tests": True, "sources_taken_as_deleted": ["runtime/src/gekko/native_trk.cpp"],
             "retired": [{"address": "0x80370BF0", "text": "DBInit", "section": "s", "after": "pruned"}],
             "roots_dropped": ["0x80370BF0"], "roots_still_named": {}, "baseline_pruned": 10, "after_pruned": 11,
             "freed": 1, "freed_by_service": {"os.debug": {"count": 1, "rows": ["0x80370BF0 DBInit"]}},
             "revived": 0, "revived_by_service": {}, "generator_refusal": "stale wait point 0x80370C00",
             "listed_sites_in_retired_rows": []},
            {"config": "tests", "build_dir": "t", "error": "generator not built in this tree"},
        ]
        text = rw.render("native_trk.cpp", results)
        self.assertIn("1 owner rows retired: 1 are pruned (nothing reaches them), 0 come back as translated bodies.", text)
        self.assertIn("GENERATOR REFUSAL: stale wait point 0x80370C00", text)
        self.assertIn("(tests do not root)", text)
        self.assertIn("NOT RUN: generator not built in this tree", text)


if __name__ == "__main__":
    unittest.main()
