#!/usr/bin/env python3
"""Tests for verify_native_runtime.py (stdlib unittest): counting from a synthetic /MAP and the ratchet file."""

from __future__ import annotations

import contextlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import verify_native_runtime as vn  # noqa: E402

MAP = """\
 ShadowTheHedgehog

 Start         Length     Name                   Class
 0001:00000000 00000100H .text                   CODE
 0002:00000000 00000040H .rdata                  DATA

  Address         Publics by Value              Rva+Base               Lib:Object

 0001:00000000       ?fn_80003100@recomp@gekko@@YAXAEAVCpu@2@@Z 0000000140001000 f   shadow_recomp:recomp_000.obj
 0001:00000020       ?fn_80003140@recomp@gekko@@YAXAEAVCpu@2@@Z 0000000140001020 f   shadow_recomp:recomp_000.obj
 0001:00000030       ?fn_80003140_body_at@recomp@gekko@@YAXAEAVCpu@2@I@Z 0000000140001030 f   shadow_recomp:recomp_000.obj
 0001:00000040       ?serve@Owner@gekko@@QEAA_NAEAVCpu@2@@Z 0000000140001040 f   shadow_runtime:native_owner.obj
 0001:00000080       ?present@Renderer@native@@QEAAXXZ 0000000140001080 f   shadow_runtime:renderer.obj
 0001:000000c0       ?step@Cpu@gekko@@QEAAXXZ 00000001400010c0 f   shadow_runtime:cpu.obj
 0002:00000000       ?kTable@gekko@@3QBIB       0000000140002000     shadow_runtime:renderer.obj
"""

COMPONENTS = {"classes": [
    {"name": "translated_guest_code", "guest_machine": True, "objects": ["shadow_recomp:*"]},
    {"name": "guest_cpu", "guest_machine": True, "objects": ["shadow_runtime:cpu.obj"]},
    {"name": "native_game", "objects": ["shadow_runtime:native_*.obj"]},
    {"name": "renderer", "objects": ["shadow_runtime:renderer.obj"]},
]}


class VerifyTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.map = self.root / "image.map"
        self.map.write_text(MAP, encoding="utf-8")
        self.components = self.root / "components.json"
        self.components.write_text(json.dumps(COMPONENTS), encoding="utf-8")
        self.ratchet = self.root / "ratchet.json"
        self.report = self.root / "report.json"

    def run_tool(self, *arguments: str, components: Path | None = None) -> tuple[int, str, dict]:
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = vn.main(["--map", str(self.map), "--components", str(components or self.components),
                            "--min-symbols", "1", "--json", str(self.report), *arguments])
        report = json.loads(self.report.read_text(encoding="utf-8")) if self.report.exists() and code != 2 else {}
        return code, out.getvalue(), report

    def test_counts(self):
        code, text, report = self.run_tool()
        self.assertEqual(code, 0)
        self.assertEqual(report["translated_functions"], 2)          # two guest addresses, three symbols
        self.assertEqual(report["translated_symbols"], 3)
        self.assertEqual(report["translated_bytes"], 0x40)
        self.assertEqual(report["cpu_signature_functions"], 1)       # the owner; Cpu::step takes no Cpu&
        self.assertEqual(report["cpu_signature_bytes"], 0x40)
        self.assertEqual(report["code_bytes"], 0x100)                # the data symbol is not code
        self.assertEqual(report["guest_machine_bytes"], 0x40 + 0x40)  # translated code + cpu.obj
        self.assertFalse(report["native"])
        self.assertIn("VERDICT: NOT NATIVE", text)

    def test_command_line_ratchets(self):
        self.assertEqual(self.run_tool("--max-translated", "2", "--max-cpu-signature", "1",
                                       "--max-guest-machine-bytes", "128")[0], 0)
        code, text, _ = self.run_tool("--max-translated", "1")
        self.assertEqual(code, 1)
        self.assertIn("FAIL: translated guest functions 2 > ratchet 1", text)
        self.assertEqual(self.run_tool("--require-native")[0], 1)

    def test_unclassified_object_fails_by_default(self):
        partial = self.root / "partial.json"
        partial.write_text(json.dumps({"classes": COMPONENTS["classes"][:3]}), encoding="utf-8")
        code, text, _ = self.run_tool(components=partial)
        self.assertEqual(code, 1)
        self.assertIn("1 unclassified objects", text)

    def test_ratchet_file_is_enforced_and_command_line_wins(self):
        self.ratchet.write_text(json.dumps({"max_translated": 1, "max_cpu_signature": 5, "max_guest_machine_bytes": 999}),
                                encoding="utf-8")
        code, text, _ = self.run_tool("--ratchet", str(self.ratchet))
        self.assertEqual(code, 1)
        self.assertIn("translated guest functions 2 > ratchet 1", text)
        self.assertEqual(self.run_tool("--ratchet", str(self.ratchet), "--max-translated", "2")[0], 0)

    def test_write_ratchet_creates_lowers_and_never_raises(self):
        code, text, _ = self.run_tool("--ratchet", str(self.ratchet), "--write-ratchet")
        self.assertEqual(code, 0)
        self.assertIn("ratchet lowered", text)
        written = json.loads(self.ratchet.read_text(encoding="utf-8"))
        self.assertEqual((written["max_translated"], written["max_cpu_signature"], written["max_guest_machine_bytes"]),
                         (2, 1, 128))
        self.assertIn("ratchet unchanged", self.run_tool("--ratchet", str(self.ratchet), "--write-ratchet")[1])
        # a tighter value already in the file is kept, a looser one is lowered
        self.ratchet.write_text(json.dumps({"max_translated": 2, "max_cpu_signature": 1, "max_guest_machine_bytes": 4096,
                                            "note": "kept"}), encoding="utf-8")
        self.assertEqual(self.run_tool("--ratchet", str(self.ratchet), "--write-ratchet")[0], 0)
        written = json.loads(self.ratchet.read_text(encoding="utf-8"))
        self.assertEqual((written["max_guest_machine_bytes"], written["note"]), (128, "kept"))

    def test_failing_run_does_not_touch_the_ratchet(self):
        self.ratchet.write_text(json.dumps({"max_translated": 1}), encoding="utf-8")
        before = self.ratchet.read_text(encoding="utf-8")
        self.assertEqual(self.run_tool("--ratchet", str(self.ratchet), "--write-ratchet")[0], 1)
        self.assertEqual(self.ratchet.read_text(encoding="utf-8"), before)

    def test_refusals(self):
        for missing in (str(self.root / "no-such.map"), "", "."):   # a missing map is exit 2 with a message, never a pass
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                code = vn.main(["--map", missing, "--components", str(self.root / "no-components.json")])
            self.assertEqual(code, 2)
            self.assertIn("game image map not found", out.getvalue())
        self.assertEqual(self.run_tool("--write-ratchet")[0], 2)
        self.assertEqual(self.run_tool("--ratchet", str(self.root / "absent.json"))[0], 2)   # missing is not passing
        out = io.StringIO()
        with contextlib.redirect_stdout(out):   # the default threshold refuses a map this small
            self.assertEqual(vn.main(["--map", str(self.map), "--components", str(self.components)]), 2)
        self.ratchet.write_text("{not json", encoding="utf-8")
        self.assertEqual(self.run_tool("--ratchet", str(self.ratchet))[0], 2)


if __name__ == "__main__":
    unittest.main()
