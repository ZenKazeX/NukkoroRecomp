"""Tests for tools/ppcrecomp/liveness_roots.py (extra roots of the generator's pruning pass)."""
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "ppcrecomp"))
import liveness_roots as roots  # noqa: E402


class LivenessRootTests(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        (self.dir / "runtime/src").mkdir(parents=True)
        self.census = self.dir / "census.json"
        self.census.write_text(json.dumps({"schema": "platform_address_coordinate_census_v2",
                                           "augmented_roots": [{"root": "0x8003740c", "evidence": {}}]}),
                               encoding="utf-8")

    def source(self, name, text):
        (self.dir / "runtime/src" / name).write_text(text, encoding="utf-8")

    def test_collects_every_guest_literal_in_any_runtime_source(self):
        self.source("a.cpp", "install_hle(0x80051BBCu, h); x = 0X8037829c; y = 0x80000000;")
        self.source("b.h", "constexpr u32 k = 0x803A1F54u; // 0x8012345 is seven digits")
        self.source("c.txt", "0x80999999 not a source file")
        found = roots.runtime_literals(self.dir / "runtime")
        self.assertEqual(found, {0x80051BBC, 0x8037829C, 0x80000000, 0x803A1F54})

    def test_does_not_split_longer_hex_numbers(self):
        self.source("a.cpp", "u64 v = 0x8000000000000001ull; u32 w = 0x80003100u;")
        self.assertEqual(roots.runtime_literals(self.dir / "runtime"), {0x80003100})

    def test_comments_are_not_roots_but_strings_are(self):
        # A comment cannot enter guest code; a string literal is code-adjacent
        # data the runtime might parse, so it stays a (harmless) root.
        self.source("a.cpp", 'f(0x80051BBCu); // the original at 0x8037CC34\n'
                             '/* 0x80060000\n   0x80070000 */ g(0x80080000u);\n'
                             'const char* s = "hook 0x80090000 // not a comment"; char q = \'/\';\n'
                             'h(0x800A0000u); /* 0x800B0000 */ i(0x800C0000u);')
        self.assertEqual(roots.runtime_literals(self.dir / "runtime"),
                         {0x80051BBC, 0x80080000, 0x80090000, 0x800A0000, 0x800C0000})

    def test_census_augmented_roots(self):
        self.assertEqual(roots.census_roots(self.census), {0x8003740C: []})
        self.census.write_text(json.dumps({"schema": "other", "augmented_roots": []}), encoding="utf-8")
        with self.assertRaises(SystemExit):
            roots.census_roots(self.census)

    def test_census_roots_keep_their_construction_sites(self):
        self.census.write_text(json.dumps({"schema": "platform_address_coordinate_census_v2", "augmented_roots": [
            {"root": "0x80379c18", "evidence": {"layout_boundary_after_terminal": ["0x80379c14"],
                                                "adjacent_code_pointer_construction": ["0x8037a0ec"]}},
            {"root": "0x8037b470", "evidence": {"layout_boundary_after_terminal": ["0x8037b46c"]}},
            {"root": "0x80051bbc", "evidence": {"adjacent_code_pointer_construction": ["0x80051000"]}}]}),
            encoding="utf-8")
        self.source("a.cpp", "f(0x80051BBCu);")
        text = roots.render(roots.runtime_literals(self.dir / "runtime"), roots.census_roots(self.census))
        lines = [line for line in text.splitlines() if not line.startswith("#")]
        # A runtime literal is always an unconditional root, even when the census also lists it.
        self.assertEqual(lines, ["0x80051BBC", "0x80379C18 census 0x8037A0EC", "0x8037B470 census"])
        self.assertEqual(roots.parse(text), {0x80051BBC, 0x80379C18, 0x8037B470})

    def test_written_file_round_trips_and_check_refuses_a_stale_file(self):
        self.source("a.cpp", "f(0x80051BBCu);")
        out = self.dir / "roots.txt"
        self.assertEqual(roots.main(["--runtime", str(self.dir / "runtime"), "--census", str(self.census),
                                     "--out", str(out)]), 0)
        self.assertEqual(roots.parse(out.read_text(encoding="utf-8")), {0x80051BBC, 0x8003740C})
        self.assertEqual(roots.main(["--runtime", str(self.dir / "runtime"), "--census", str(self.census),
                                     "--check", str(out)]), 0)
        self.source("b.cpp", "g(0x80060000u);")  # a new native hook
        self.assertEqual(roots.main(["--runtime", str(self.dir / "runtime"), "--census", str(self.census),
                                     "--check", str(out)]), 1)

    def test_check_refuses_a_census_root_listed_without_its_sites(self):
        self.census.write_text(json.dumps({"schema": "platform_address_coordinate_census_v2", "augmented_roots": [
            {"root": "0x80379c18", "evidence": {"adjacent_code_pointer_construction": ["0x8037a0ec"]}}]}),
            encoding="utf-8")
        out = self.dir / "roots.txt"
        out.write_text("0x80379C18 census\n", encoding="utf-8")  # a construction site dropped
        self.assertEqual(roots.main(["--runtime", str(self.dir / "runtime"), "--census", str(self.census),
                                     "--check", str(out)]), 1)

    def test_reference_interpreter_tests_contribute_no_roots(self):
        # A test that runs the original words in the reference interpreter names
        # the SDK bodies a native owner replaced; they must not stay in the image.
        (self.dir / "runtime/tests").mkdir(parents=True)
        (self.dir / "runtime/tests/ref.cpp").write_text(
            "// liveness-roots: reference-interpreter-only\nm.call(0x8037A7ACu, {}); // the original\n",
            encoding="utf-8")
        (self.dir / "runtime/tests/unit.cpp").write_text("c.try_hle(0x8037AAA0u);\n", encoding="utf-8")
        self.source("a.cpp", "f(0x80051BBCu);")
        self.assertEqual(roots.runtime_literals(self.dir / "runtime"), {0x80051BBC, 0x8037AAA0})

    def test_the_interpreter_only_declaration_is_verified(self):
        # Outside tests/: refused (production code always enters what it names).
        self.source("a.cpp", "// liveness-roots: reference-interpreter-only\nf(0x80051BBCu);\n")
        with self.assertRaises(SystemExit):
            roots.runtime_literals(self.dir / "runtime")
        (self.dir / "runtime/src/a.cpp").unlink()
        # A test that can reach the generated code: refused, both ways.
        (self.dir / "runtime/tests").mkdir(parents=True)
        for body in ('#include "gekko/recomp.h"\n', "auto f = gekko::recomp::kFunctions;\n"):
            (self.dir / "runtime/tests/t.cpp").write_text(
                "// liveness-roots: reference-interpreter-only\n" + body + "g(0x80060000u);\n", encoding="utf-8")
            with self.assertRaises(SystemExit):
                roots.runtime_literals(self.dir / "runtime")
        # The marker only counts as its own line, not inside other text.
        (self.dir / "runtime/tests/t.cpp").write_text(
            '// see "liveness-roots: reference-interpreter-only" elsewhere\ng(0x80060000u);\n', encoding="utf-8")
        self.assertEqual(roots.runtime_literals(self.dir / "runtime"), {0x80060000})

    def test_out_never_overwrites(self):
        out = self.dir / "roots.txt"
        out.write_text("# existing\n", encoding="utf-8")
        with self.assertRaises(FileExistsError):
            roots.main(["--runtime", str(self.dir / "runtime"), "--census", str(self.census), "--out", str(out)])


if __name__ == "__main__":
    unittest.main()
