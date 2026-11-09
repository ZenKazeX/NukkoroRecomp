import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

import guest_classes
import stack_report


def tiny_dol(path):
    """A DOL with one text section and one data section holding an RTTI record {name, vtable}."""
    text_addr, data_addr = 0x80003100, guest_classes.RTTI_BEGIN
    text = b"\x60\x00\x00\x00" * 64  # 0x80003100..0x80003200
    name_addr, vtable_addr = data_addr + 0x40, data_addr + 0x80
    data = bytearray(0x100)
    struct.pack_into(">II", data, 0, name_addr, vtable_addr)
    data[0x40:0x40 + 12] = b"Enemy::Task\0"
    struct.pack_into(">IIII", data, 0x80, name_addr, 0, 0x80003110, 0x80003120)  # header, then two methods
    header = bytearray(0x100)
    struct.pack_into(">I", header, 0, 0x100)            # text 0 offset
    struct.pack_into(">I", header, 7 * 4, 0x100 + len(text))  # data 0 offset
    struct.pack_into(">I", header, 0x48, text_addr)
    struct.pack_into(">I", header, 0x48 + 7 * 4, data_addr)
    struct.pack_into(">I", header, 0x90, len(text))
    struct.pack_into(">I", header, 0x90 + 7 * 4, len(data))
    Path(path).write_bytes(bytes(header) + text + bytes(data))


class ClassMapTests(unittest.TestCase):
    def test_methods_follow_the_vtable_header(self):
        with tempfile.TemporaryDirectory() as folder:
            tiny_dol(Path(folder) / "main.dol")
            dol = guest_classes.Dol((Path(folder) / "main.dol").read_bytes())
        owners = guest_classes.method_owners(guest_classes.classes(dol))
        self.assertEqual(guest_classes.label(owners, 0x80003110), "Enemy::Task::vf0")
        self.assertEqual(guest_classes.label(owners, 0x80003120), "Enemy::Task::vf1")
        self.assertIsNone(guest_classes.label(owners, 0x80003130))


class StackReportTests(unittest.TestCase):
    def test_samples_are_charged_to_task_and_owner(self):
        with tempfile.TemporaryDirectory() as folder:
            tiny_dol(Path(folder) / "main.dol")
            folded = Path(folder) / "stacks.folded"
            folded.write_text("thread_1;main;fn_80003110;fn_80003180;fn_80003120;fn_800031C0 3\n"
                              "thread_1;main;fn_80003190;ntdll.dll 1\n")
            out = Path(folder) / "report.json"
            subprocess.run([sys.executable, "-B", str(Path(stack_report.__file__)), "--folded", str(folded),
                            "--dol", str(Path(folder) / "main.dol"), "--out", str(out)],
                           check=True, capture_output=True)
            result = json.loads(out.read_text())
        self.assertEqual(result["samples"], 4)
        self.assertEqual(result["by_task"][0], {"name": "Enemy::Task::vf0", "samples": 3, "share": 0.75})
        self.assertEqual(result["by_owner"][0]["name"], "Enemy::Task::vf1")
        self.assertEqual(result["inclusive_by_class"][0], {"name": "Enemy::Task", "samples": 3, "share": 0.75})
        under = {row["function"]: row["under"] for row in result["inclusive_guest_functions"]}
        self.assertEqual(under["fn_800031C0"], "Enemy::Task::vf1")
        self.assertEqual(under["fn_80003190"], "(no class)")
        self.assertEqual(result["innermost_outside_generated_code"][0]["name"], "ntdll.dll")


if __name__ == "__main__":
    unittest.main()
