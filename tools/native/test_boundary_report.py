#!/usr/bin/env python3
"""Tests for boundary_report.py (stdlib unittest; run: python -m unittest tools/native/test_boundary_report.py)."""

from __future__ import annotations

import contextlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import boundary_report as br  # noqa: E402

LOG = """\
Native interrupts: delivered VI 100, AI 300, ARAM 0, DSP 0, PE finish 0, CP breakpoint 99; 1200 guest calls, 0 reschedules, 0 OS mask applications
Native execution: 5000 entries, 2400 dispatcher round trips (sc/rfi/yield/trap, nothing interpreted unless the interpreter line says so)
Native transfers: 70 published to the waiting frame (jump_indirect/resume_elsewhere), 3 left to the dispatcher
Counter identity: interpreted 0; entries 5000 = dispatcher exits 2400 + normal returns 2600
TOTAL_NON_NATIVE_PPC_EXECUTIONS: 0
HLE filter: 0 of 4 hooked slots shared
HLE hooks installed: 5; hits this run:
    0x80003154  x1  native boot: __start
    0x800054F4  x900  native game memory: memcpy (memmove)
    0x800093B8  x4000  S1 shptr (native)
    0x80009408  x99  S1 shptr (native)
Dispatch blocked: interrupt due 7, HLE hook 0, no native code 2300
native SVM (CRI server manager, docs/CRI_SERVERS.md 1): server calls 40 (type 2 10, 4 10, 5 10, 6 10), callbacks 60, lock hooks called 5
native SJ (CRI stream joints, docs/CRI_SERVERS.md 4b): creates 1, destroys 1, gets 2, errors 0, guest callbacks 4
"""


def session(**guest) -> str:
    return json.dumps({"schema": 1, "guest": guest})


class ParseTests(unittest.TestCase):
    def test_hooks_and_aggregates(self):
        run = br.parse_stdout(LOG)
        self.assertEqual(run["hooks_installed"], 5)
        self.assertEqual([(h["address"], h["hits"]) for h in run["hooks"]],
                         [(0x80003154, 1), (0x800054F4, 900), (0x800093B8, 4000), (0x80009408, 99)])
        self.assertEqual(run["hooks"][1]["name"], "native game memory: memcpy (memmove)")
        self.assertEqual(run["aggregates"], {
            "dispatcher_entries": 5000, "dispatcher_round_trips": 2400, "published_transfers": 70,
            "transfers_left_to_dispatcher": 3, "normal_returns": 2600, "non_native_ppc_executions": 0,
            "dispatch_no_native_code": 2300, "dispatch_blocked_interrupt": 7, "interrupt_guest_calls": 1200,
            "vi_interrupts": 100, "svm_guest_callbacks": 60, "sj_guest_callbacks": 4})

    def test_hook_block_ends_at_first_other_line(self):
        run = br.parse_stdout(LOG + "    0x80000000  x5  not a hook row: it follows other text\n")
        self.assertEqual(len(run["hooks"]), 4)

    def test_log_without_hook_block_is_an_error(self):
        with self.assertRaises(br.InputError):
            br.parse_stdout("Native execution: 5 entries, 1 dispatcher round trips\n")

    def test_group_of(self):
        self.assertEqual(br.group_of("native boot: __start"), "native boot")
        self.assertEqual(br.group_of("S1 shptr (native)"), "S1 shptr (native)")
        self.assertEqual(br.group_of(""), "(unnamed)")


class RunTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def make(self, name: str, log: str = LOG, guest: dict | None = None) -> Path:
        directory = self.root / name
        directory.mkdir()
        (directory / "stdout.log").write_text(log, encoding="utf-8")
        if guest is not None:
            (directory / "session.json").write_text(session(**guest), encoding="utf-8")
        return directory

    def load(self, directory: Path) -> dict:
        return br.load_run(*br.resolve(str(directory), None, None))

    def test_rates_use_presented_frames(self):
        run = self.load(self.make("a", guest={"limiter_frames": 50, "owner_hits": 5000, "owner_hooks": 5,
                                              "native_entries": 5000, "owner_entry_attempts": 5100}))
        self.assertEqual((run["rate_unit"], run["rate_base"]), ("presented frame", 50))
        self.assertEqual(run["hook_hits_total"], 5000)
        self.assertEqual(br.consistency(run), [])
        text = br.render(run, top=2)
        self.assertIn("| S1 shptr (native) | 2 | 4,099 | 81.98 % | 82.0 |", text)
        self.assertIn("| 0x800093B8 | 4,000 | 80.00 % | 80.00 % | 80.0 | S1 shptr (native) |", text)
        self.assertIn("top 2 of 4", text)
        self.assertIn("(4 of 5 hooks hit) | 5,000 | 100.0 |", text)
        self.assertNotIn("INCONSISTENT", text)
        self.assertIn("Not measured by the existing counters", text)

    def test_rates_fall_back_to_vi_without_frames(self):
        run = self.load(self.make("b", guest={"limiter_frames": 0}))
        self.assertEqual((run["rate_unit"], run["rate_base"]), ("VI interrupt", 100))

    def test_no_session_file_still_reports(self):
        run = self.load(self.make("c"))
        self.assertEqual(run["guest"], {})
        self.assertEqual(run["rate_unit"], "VI interrupt")

    def test_mismatched_session_is_reported(self):
        run = self.load(self.make("d", guest={"limiter_frames": 50, "owner_hits": 4999, "owner_hooks": 6,
                                              "native_entries": 1}))
        self.assertEqual(len(br.consistency(run)), 3)
        self.assertEqual(br.render(run, top=0).count("INCONSISTENT INPUT"), 3)

    def test_compare_lists_changed_new_and_gone_addresses(self):
        base = self.load(self.make("base", guest={"limiter_frames": 50}))
        changed = LOG.replace("x4000  S1 shptr", "x3000  S1 shptr").replace(
            "    0x80009408  x99  S1 shptr (native)\n", "    0x8000A000  x7  new owner: probe\n")
        run = self.load(self.make("run", log=changed, guest={"limiter_frames": 50}))
        text = br.compare(run, base, top=0)
        self.assertIn("| guest -> native calls served | 5,000 | 3,908 | -1,092 |", text)
        self.assertIn("Addresses whose served calls changed: 3", text)
        self.assertIn("| 0x800093B8 | 4,000 | 3,000 | -1,000 | S1 shptr (native) |", text)
        self.assertIn("| 0x80009408 | 99 | 0 | -99 | S1 shptr (native) |", text)
        self.assertIn("| 0x8000A000 | 0 | 7 | +7 | new owner: probe |", text)

    def test_main_exit_codes_and_json(self):
        directory = self.make("e", guest={"limiter_frames": 50, "owner_hits": 5000})
        out = self.root / "run.json"
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(br.main([str(directory), "--json", str(out)]), 0)
        payload = json.loads(out.read_text(encoding="utf-8"))
        self.assertEqual(payload["hook_hits_total"], 5000)
        self.assertEqual(payload["groups"][0], {"group": "S1 shptr (native)", "hooks": 2, "hits": 4099})
        with contextlib.redirect_stderr(io.StringIO()), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(br.main([str(self.root / "missing")]), 2)


if __name__ == "__main__":
    unittest.main()
