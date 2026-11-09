from collections import Counter
import contextlib
import csv
import hashlib
import io
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

if __package__:
    from . import graphics_lift_inventory as inventory
else:
    import graphics_lift_inventory as inventory


BASE, NOP, BLR = 0x80001000, 0x60000000, 0x4e800020


def branch(offset, absolute=False, link=False):
    return 0x48000000 | (offset & 0x03fffffc) | (int(absolute) << 1) | int(link)


def conditional(offset, bo=12, bi=0, absolute=False, link=False):
    return 0x40000000 | (bo << 21) | (bi << 16) | (offset & 0xfffc) | (int(absolute) << 1) | int(link)


def dol(words=(BLR,), address=BASE, sections=None, bss=(0, 0)):
    raw = bytearray(0x100)
    for index, start, data in sections if sections is not None else [(0, address, words)]:
        data = struct.pack(f">{len(data)}I", *data)
        for base, value in ((0, len(raw)), (0x48, start), (0x90, len(data))):
            struct.pack_into(">I", raw, base + index * 4, value)
        raw.extend(data)
    struct.pack_into(">III", raw, 0xd8, *bss, address)
    return bytes(raw)


def csv_bytes(rows):
    stream = io.StringIO(newline="")
    writer = csv.writer(stream)
    writer.writerow(("addr_hex", "name", "size"))
    writer.writerows((inventory.hx(address), name, size) for address, name, size in rows)
    return stream.getvalue().encode("utf-8")


def header(raw, changes):
    raw = bytearray(raw)
    for offset, value in changes.items():
        struct.pack_into(">I", raw, offset, value)
    return bytes(raw)


class InventoryTests(unittest.TestCase):
    def build(self, words, rows=None, hints=None, address=BASE):
        report = inventory.build_inventory(dol(words, address), csv_bytes(
            rows if rows is not None else [(address, "fn_test", len(words) * 4)]),
            csv_bytes(hints) if hints is not None else None)
        addresses = [pc for node in report["cfg_nodes"]
                     for pc in range(int(node["address"], 16), int(node["end_exclusive"], 16), 4)]
        self.assertEqual(len(addresses), len(set(addresses)))
        self.assertEqual(len(addresses), report["summary"]["instruction_count"])
        self.assertEqual(len(addresses), sum(n["instruction_count"] for n in report["cfg_nodes"]))
        sites = [e["site"] for e in report["direct_edges"] + report["unresolved_sites"]]
        self.assertEqual(len(sites), len(set(sites)))
        starts = {node["address"] for node in report["cfg_nodes"]}
        self.assertTrue(all(set(node["successors"]) <= starts for node in report["cfg_nodes"]))
        self.assertTrue({root["address"] for root in report["roots"]} <= starts)
        return report

    def test_signed_absolute_and_wrapping_direct_branches(self):
        for encode, offsets in ((branch, (-0x02000000, -4, 0, 4, 0x01fffffc)),
                                (conditional, (-0x8000, -4, 0, 4, 0x7ffc))):
            for address in (BASE, 0, 0xfffffffc):
                for absolute in (False, True):
                    for offset in offsets:
                        with self.subTest(encode=encode.__name__, address=address, absolute=absolute, offset=offset):
                            word = encode(offset, absolute=absolute)
                            result = self.build([word], address=address)
                            edge = result["direct_edges"][0]
                            target = (offset + (0 if absolute else address)) & 0xffffffff
                            self.assertEqual(edge["target"], inventory.hx(target))
                            self.assertEqual(edge["absolute"], absolute)
                            self.assertEqual(edge["word"], inventory.hx(word))
                            self.assertEqual(edge["site"], inventory.hx(address))
                            self.assertEqual(edge["file_offset"], 0x100)
                            self.assertEqual(result["summary"]["instruction_count"], 1)

    def test_all_bo_values_use_only_ignore_test_mask(self):
        for bo in range(32):
            for link in (False, True):
                with self.subTest(bo=bo, link=link):
                    result = self.build([conditional(8, bo, 7, link=link), branch(0x100, link=True), BLR])
                    tested = (bo & 0x14) != 0x14
                    follows = tested or link
                    self.assertEqual(result["summary"]["instruction_count"], 3 if follows else 2)
                    self.assertEqual(len(result["direct_edges"]), 2 if follows else 1)
                    self.assertEqual(result["direct_edges"][0]["conditional"], tested)
                    self.assertEqual(result["direct_edges"][0]["bi"], 7)
                    self.assertEqual(result["direct_edges"][0]["kind"], "call" if link else "branch")
                    self.assertEqual(result["cfg_nodes"][0]["successors"],
                                     [inventory.hx(BASE + 4), inventory.hx(BASE + 8)] if follows else [inventory.hx(BASE + 8)])

    def test_linked_targets_and_may_return_are_both_visited(self):
        for encode in (branch, conditional):
            result = self.build([encode(8, link=True), BLR, branch(0x100, link=True), BLR])
            self.assertEqual(result["summary"]["instruction_count"], 4)
            self.assertEqual([node["instruction_count"] for node in result["cfg_nodes"]], [1, 1, 1, 1])
            self.assertEqual(result["cfg_nodes"][0]["successors"], [inventory.hx(BASE + 4), inventory.hx(BASE + 8)])
            self.assertEqual([edge["kind"] for edge in result["direct_edges"]], ["call", "call"])
            self.assertEqual(result["direct_edges"][0]["target_declarations"], ("functions:2",))
            self.assertEqual(result["direct_edges"][1]["file_offset"], 0x108)

    def test_unvisited_branch_words_are_not_counted_as_calls(self):
        result = self.build([branch(12), branch(0x100, link=True), branch(0x200, link=True), BLR, branch(0x300, link=True)])
        self.assertEqual(result["summary"]["direct_calls"], 0)
        self.assertEqual(result["summary"]["instruction_count"], 2)
        self.assertEqual(len(result["direct_edges"]), 1)
        self.assertEqual([gap["word_count"] for gap in result["coverage_gaps"]["unvisited_text"]], [2, 1])
        self.assertEqual(result["unresolved_sites"][0]["site"], inventory.hx(BASE + 12))
        loop = self.build([NOP, branch(-4), branch(0x100, link=True)])
        self.assertEqual(loop["summary"]["instruction_count"], 2)
        self.assertEqual(loop["cfg_nodes"][0]["successors"], [inventory.hx(BASE)])
        self.assertEqual(loop["cfg_nodes"][0]["instruction_count"], 2)

    def test_overlapping_declarations_aliases_shared_tails_and_reads_are_unique(self):
        words = [branch(16), branch(12), branch(0x100, link=True), NOP, branch(8, link=True), BLR, BLR]
        rows = [(BASE, "outer", 28), (BASE + 4, "GXInner", 20), (BASE + 4, "RwAlias", 24)]
        hints = [(BASE + 16, "GXShared", 12), (BASE + 16, "GXShared", 4),
                 (BASE + 4, "rwAlternate", 4), (BASE + 4, "other", 8)]
        reads, original = Counter(), inventory.Dol.word
        def read_word(image, address):
            reads[address] += 1
            return original(image, address)
        with patch.object(inventory.Dol, "word", read_word):
            report = self.build(words, rows, hints)
        self.assertEqual(reads, Counter({BASE + offset: 1 for offset in (0, 4, 16, 20, 24)}))
        self.assertEqual((report["summary"]["instruction_count"], report["summary"]["direct_edges"], report["summary"]["direct_calls"]), (5, 3, 1))
        self.assertEqual([row["size_bytes"] for row in report["declarations"]], [28, 20, 24])
        self.assertEqual(len(report["name_hints"]), 4)
        self.assertEqual(report["summary"]["roots"], 2)
        self.assertEqual(report["roots"][1]["declarations"], ["functions:3", "functions:4"])
        self.assertEqual(report["roots"][1]["name_hints"], ["names:4", "names:5"])
        self.assertEqual(report["cfg_nodes"][1]["root_labels"], report["roots"][1])
        self.assertEqual(report["direct_edges"][1]["site_declarations"], ("functions:2", "functions:3", "functions:4"))
        self.assertEqual(report["direct_edges"][2]["target_declarations"], ("functions:2", "functions:4"))
        self.assertEqual(report["summary"]["duplicate_function_entries"], 1)
        self.assertEqual(report["summary"]["duplicate_name_hint_entries"], 2)
        self.assertEqual(len(report["ambiguities"]["declaration_extent_overlaps"]), 2)
        shared = next(c for c in report["graphics_candidates"] if c["entry"] == inventory.hx(BASE + 16))
        self.assertFalse(shared["is_root"])
        self.assertTrue(shared["visited"])
        self.assertEqual(len(shared["labels"]), 2)
        self.assertEqual(shared["entry_word"], inventory.hx(words[4]))
        self.assertEqual(shared["inbound_direct_edges"], report["direct_edges"][:2])
        self.assertEqual([e["file_offset"] for e in shared["inbound_direct_edges"]], [0x100, 0x104])
        self.assertEqual(len(inventory.candidates_view(report)["referenced_declarations"]), 3)

    def test_extents_do_not_clamp_or_bound_global_fallthrough(self):
        words = [NOP] * 27 + [BLR]
        large = self.build(words, [(BASE, "outer", 108), (BASE + 52, "inner", 36), (BASE + 108, "next", 4)])
        small = self.build(words, [(BASE, "outer", 4), (BASE + 52, "inner", 4), (BASE + 108, "next", 4)])
        self.assertEqual(large["summary"]["instruction_count"], 28)
        self.assertEqual(large["summary"]["unvisited_text_words"], 0)
        for left, right in zip(large["cfg_nodes"], small["cfg_nodes"]):
            for key in ("address", "end_exclusive", "instruction_count", "successors"):
                self.assertEqual(left[key], right[key])
        self.assertEqual(len(large["cfg_nodes"]), len(small["cfg_nodes"]))
        self.assertEqual(large["declarations"][0]["end_exclusive"], inventory.hx(BASE + 108))
        self.assertEqual(large["ambiguities"]["declaration_extent_overlaps"],
                         [{"address": inventory.hx(BASE + 52), "end_exclusive": inventory.hx(BASE + 88),
                           "label_ids": ("functions:2", "functions:3")}])

    def test_cross_declaration_interior_targets_and_unowned_text_are_followed(self):
        report = self.build([branch(16), NOP, BLR, NOP, branch(0x100, link=True), BLR],
                            [(BASE, "one", 4), (BASE + 8, "two", 12)])
        self.assertEqual(report["summary"]["direct_calls"], 1)
        self.assertEqual(report["summary"]["instruction_count"], 4)
        self.assertEqual(report["summary"]["unvisited_text_words"], 2)
        self.assertEqual(report["direct_edges"][0]["target_declarations"], ("functions:3",))
        self.assertEqual(report["unresolved_sites"][-1]["site_declarations"], ())
        plain = self.build([NOP, NOP, BLR], [(BASE, "short", 4)])
        self.assertEqual(plain["summary"]["instruction_count"], 3)
        self.assertEqual(plain["unresolved_sites"][0]["site_declarations"], ())
        call = self.build([branch(8, link=True), NOP, BLR], [(BASE, "short", 4)])
        self.assertEqual(call["direct_edges"][0]["target_declarations"], ())
        self.assertEqual(call["summary"]["instruction_count"], 3)
        self.assertTrue(call["direct_edges"][0]["target_in_text"])

    def test_dol_entry_and_every_declared_entry_seed_the_same_graph(self):
        report = self.build([BLR, branch(0x100, link=True), BLR], [(BASE + 4, "hypothetical", 8)])
        self.assertEqual([root["address"] for root in report["roots"]], [inventory.hx(BASE), inventory.hx(BASE + 4)])
        self.assertEqual(report["roots"][0]["declarations"], [])
        self.assertTrue(report["roots"][0]["dol_entry"])
        self.assertEqual(report["summary"]["instruction_count"], 3)
        self.assertEqual(report["summary"]["direct_calls"], 1)

    def test_unmapped_targets_and_continuations_are_explicit_gaps(self):
        for link in (False, True):
            result = self.build([conditional(8, link=link)])
            self.assertEqual(result["summary"]["direct_edges_outside_text"], 1)
            self.assertEqual(result["direct_edges"][0]["target_declarations"], ())
            self.assertEqual(result["unmapped_fallthroughs"][0]["basis"],
                             "conditional_or_call_may_return" if link else "conditional_fallthrough")
            self.assertEqual(result["coverage_gaps"]["unfollowed_direct_sites"], [inventory.hx(BASE)])

    def test_all_dynamic_bo_and_link_forms_remain_unresolved(self):
        for xo in (16, 528):
            for bo in range(32):
                for link in (False, True):
                    with self.subTest(xo=xo, bo=bo, link=link):
                        word = (19 << 26) | (bo << 21) | (xo << 1) | int(link)
                        result = self.build([word, branch(0x100, link=True), BLR])
                        follows = link or (bo & 0x14) != 0x14
                        self.assertEqual(result["summary"]["instruction_count"], 3 if follows else 1)
                        edge = result["unresolved_sites"][0]
                        self.assertIsNone(edge["target"])
                        self.assertIsNone(edge["target_declarations"])
                        self.assertEqual(edge["target_source"], "LR" if xo == 16 else "CTR")
                        self.assertEqual(edge["may_fall_through"], follows)
                        self.assertEqual(edge["return_like"], word == BLR)

    def test_returns_ctr_sc_rfi_stop_paths_without_abi_inference(self):
        for word, mnemonic in ((BLR, "bclr"), (0x4e800420, "bcctr"), (0x44000002, "sc"), (0x4c000064, "rfi")):
            result = self.build([word, branch(0x100, link=True), BLR])
            self.assertEqual(result["summary"]["instruction_count"], 1)
            self.assertEqual(result["direct_edges"], [])
            self.assertEqual(result["unresolved_sites"][0]["mnemonic"], mnemonic)
            self.assertIsNone(result["unresolved_sites"][0]["target"])
        result = self.build([0x3d808000, 0x618c0100, 0x7d8903a6, 0x4e800421, BLR])
        self.assertEqual(result["summary"]["instruction_count"], 5)
        self.assertEqual(result["direct_edges"], [])
        self.assertTrue(all(edge["target"] is None for edge in result["unresolved_sites"]))

    def test_nonbranches_and_partial_scope_make_no_validity_or_lifting_claims(self):
        report = self.build([0, 0xffffffff, 0x90030000, BLR])
        self.assertEqual(report["summary"]["instruction_count"], 4)
        for key in ("native_lifting_proven", "whole_program_reachability_proven", "state_effect_liveness_proven",
                    "interprocedural_constant_propagation", "runtime_generated_code_covered",
                    "instruction_validity_proven", "exception_closure_proven", "indirect_targets_resolved"):
            self.assertIs(report["scope"][key], False)
        self.assertIn("hypothetical", report["scope"]["roots"])
        self.assertIn("instruction validity", report["scope"]["nonbranches"])
        self.assertIn("0/1/many", report["scope"]["ownership"])
        self.assertEqual(report["schema"], "graphics_lift_inventory_v2")

    def test_tiny_unverified_hints_do_not_create_roots_and_keep_all_aliases(self):
        for name in ("GXInitTexObjUserData", "RwTest", "rwTest", "RpTest", "rpTest", "RenderTest", "DrawTest", "PostGlareTest"):
            report = self.build([0x90030000, BLR, NOP, BLR], [(BASE, "unknown", 8)],
                                [(BASE, name, 4), (BASE, "another_alias", 8), (BASE + 8, "__GXOther", 8)])
            first, second = report["graphics_candidates"]
            self.assertEqual(len(first["labels"]), 3)
            self.assertEqual(first["entry_word"], "0x90030000")
            self.assertEqual(first["entry_block_instruction_count"], 2)
            self.assertTrue(all(item["verification"] == "unverified" for item in first["labels"]))
            self.assertFalse(second["is_root"])
            self.assertFalse(second["visited"])
            self.assertIsNone(second["entry_word"])
            self.assertIsNone(second["entry_block_instruction_count"])
            self.assertEqual(report["summary"]["instruction_count"], 2)
            self.assertEqual(report["summary"]["roots"], 1)

    def test_input_hashes_sizes_determinism_and_ignored_cached_claims(self):
        raw = dol([branch(8, link=True), BLR, BLR])
        functions = f'addr_hex,addr,name,size,ins,callees\n{BASE:x},garbage,fn_test,4,999999,"0x81234560"\n'.encode()
        report = inventory.build_inventory(raw, functions)
        self.assertEqual(report, inventory.build_inventory(raw, functions))
        for key, data in (("dol", raw), ("functions", functions)):
            self.assertEqual(report["inputs"][key], {"sha256": hashlib.sha256(data).hexdigest(), "size_bytes": len(data)})
        self.assertIsNone(report["inputs"]["names"])
        self.assertEqual(report["summary"]["direct_calls"], 1)
        self.assertEqual(report["summary"]["instruction_count"], 3)
        self.assertEqual(report["direct_edges"][0]["target"], inventory.hx(BASE + 8))

    def test_invalid_headers_bounds_alignment_and_truncation_reject(self):
        raw = dol([NOP, NOP, NOP, BLR]) + b"\0" * 8
        changes = ({0: 0xfc}, {0: 0xfffffff0}, {0x90: 0x1000}, {0: 0x101}, {0x48: BASE + 1},
                   {0x90: 6}, {0x48: 0xfffffffc}, {0xe0: BASE + 1}, {0xe0: BASE + 0x1000},
                   {0x90: 0}, {0xd8: 0xffffffff, 0xdc: 2})
        for bad in [b"", b"\0" * 255, dol()[:-1]] + [header(raw, change) for change in changes]:
            with self.subTest(raw=bad[:16]), self.assertRaises(ValueError):
                inventory.Dol(bad)

    def test_loaded_section_overlaps_reject_and_adjacent_text_is_traversed(self):
        for index in (1, 6, 7, 17):
            bad = dol(sections=[(0, BASE, [NOP, BLR]), (index, BASE + 4, [BLR])])
            with self.assertRaisesRegex(ValueError, "overlapping"):
                inventory.Dol(bad)
            bad = dol(sections=[(0, BASE, [NOP, BLR]), (index, BASE + 16, [BLR])])
            with self.assertRaisesRegex(ValueError, "overlapping"):
                inventory.Dol(header(bad, {index * 4: 0x104}))
        raw = dol(sections=[(0, BASE, [NOP]), (1, BASE + 4, [BLR])], bss=(BASE + 4, 8))
        report = inventory.build_inventory(raw, csv_bytes([(BASE, "short", 4)]))
        self.assertEqual(report["summary"]["instruction_count"], 2)
        self.assertEqual(report["cfg_nodes"][0]["successors"], [inventory.hx(BASE + 4)])
        self.assertEqual(report["dol"]["bss"]["overlapping_loaded_sections"], [1])

    def test_genuinely_malformed_csv_headers_rows_and_ranges_still_reject(self):
        valid = csv_bytes([(BASE, "fn", 8)])
        bad_rows = [("-1", "fn", "4"), ("100000000", "fn", "4"), (f"{BASE + 1:x}", "fn", "4"),
                    (f"{BASE:x}", "fn", "0"), (f"{BASE:x}", "fn", "2"), (f"{BASE:x}", "fn", "-4"),
                    (f"{BASE:x}", "fn", "bad"), (f"{BASE:x}", "", "4"), (f"{BASE:x}", "fn", "12"),
                    (f"{BASE + 8:x}", "fn", "4"), ("fffffffc", "fn", "8")]
        bad = [b"", b"addr_hex,size\n", b"addr_hex,name,size,name\n", b"addr_hex,name,size,\n",
               b"addr_hex,name,size\n\n", valid + b"short,row\n", valid + b"too,many,extra,columns\n",
               valid + b'"unterminated', valid + b"\xff\n"]
        bad.extend(("addr_hex,name,size\n" + ",".join(row) + "\n").encode() for row in bad_rows)
        for data in bad:
            for source in ("functions", "names"):
                with self.subTest(data=data, source=source), self.assertRaisesRegex(ValueError, source + " CSV"):
                    inventory.build_inventory(dol([NOP, BLR]), data if source == "functions" else valid,
                                              data if source == "names" else None)
        with self.assertRaises(ValueError):
            inventory.build_inventory(dol(), b"addr_hex,name,size\n")
        gap = dol(sections=[(0, BASE, [NOP]), (1, BASE + 8, [BLR]), (7, BASE + 16, [NOP])])
        for address, size in ((BASE, 12), (BASE + 4, 4), (BASE + 16, 4)):
            with self.assertRaises(ValueError):
                inventory.build_inventory(gap, csv_bytes([(address, "bad", size)]))


class CliTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.dol, self.functions, self.names = [self.root / name for name in ("input.dol", "functions.csv", "names.csv")]
        self.dol.write_bytes(dol([NOP, BLR]))
        self.functions.write_bytes(csv_bytes([(BASE, "fn_test", 8)]))
        self.names.write_bytes(csv_bytes([(BASE, "GXTest", 8)]))

    def args(self, output, candidates=None, names=True):
        args = ["--dol", str(self.dol), "--functions", str(self.functions), "--output", str(output)]
        return args + (["--names", str(self.names)] if names else []) + (["--candidates-output", str(candidates)] if candidates is not None else [])

    def invoke(self, output, candidates=None, names=True):
        stdout, stderr = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            code = inventory.main(self.args(output, candidates, names))
        return code, stdout.getvalue(), stderr.getvalue()

    def test_deterministic_dual_json_bounded_stdout_and_unchanged_inputs(self):
        originals = {path: path.read_bytes() for path in (self.dol, self.functions, self.names)}
        first, second = self.root / "first.json", self.root / "second.json"
        compact, compact2 = self.root / "candidates.json", self.root / "candidates2.json"
        code, stdout, stderr = self.invoke(first, compact)
        self.assertEqual((code, stderr), (0, ""))
        self.assertLess(len(stdout), 2000)
        self.assertEqual(json.loads(stdout)["status"], "ok")
        run = subprocess.run([sys.executable, "-B", str(Path(inventory.__file__)), *self.args(second, compact2)],
                             capture_output=True, text=True, check=False)
        self.assertEqual((run.returncode, run.stderr, run.stdout), (0, "", stdout))
        self.assertEqual(first.read_bytes(), second.read_bytes())
        self.assertEqual(compact.read_bytes(), compact2.read_bytes())
        full, small = json.loads(first.read_bytes()), json.loads(compact.read_bytes())
        self.assertEqual(small["graphics_candidates"], full["graphics_candidates"])
        self.assertEqual(small["inputs"], full["inputs"])
        self.assertEqual(small["view"], "graphics_candidates")
        self.assertEqual(small["referenced_declarations"], full["declarations"])
        self.assertEqual(full["inputs"]["names"]["sha256"], hashlib.sha256(originals[self.names]).hexdigest())
        self.assertTrue(all(path.read_bytes() == data for path, data in originals.items()))

    def test_optional_names_and_companion(self):
        output = self.root / "no-names.json"
        self.assertEqual(self.invoke(output, names=False)[0], 0)
        report = json.loads(output.read_bytes())
        self.assertIsNone(report["inputs"]["names"])
        self.assertEqual(report["name_hints"], [])

    def test_existing_outputs_input_aliases_and_identical_outputs_refuse_all_writes(self):
        output, companion = self.root / "out.json", self.root / "existing.json"
        companion.write_bytes(b"keep exact bytes")
        for path in (companion, self.dol, self.functions, self.names, self.root):
            self.assertEqual(self.invoke(path)[0], 2)
            self.assertEqual(self.invoke(output, path)[0], 2)
            self.assertFalse(output.exists())
        self.assertEqual(companion.read_bytes(), b"keep exact bytes")
        for alias in (output, self.root / "sub" / ".." / "out.json"):
            self.assertEqual(self.invoke(output, alias)[0], 2)
            self.assertFalse(output.exists())
        self.assertEqual(self.dol.read_bytes(), dol([NOP, BLR]))

    def test_existing_hard_link_input_alias_is_not_overwritten(self):
        alias = self.root / "hardlink.json"
        try:
            os.link(self.dol, alias)
        except OSError as error:
            self.skipTest(str(error))
        self.assertEqual(self.invoke(alias)[0], 2)
        self.assertEqual(alias.read_bytes(), self.dol.read_bytes())

    def test_outputs_created_during_analysis_are_refused_before_other_writes(self):
        report = inventory.build_inventory(self.dol.read_bytes(), self.functions.read_bytes())
        for name in ("full", "compact"):
            output, compact = self.root / (name + ".json"), self.root / (name + "-compact.json")
            raced = output if name == "full" else compact
            def race(*args):
                raced.write_bytes(b"created by someone else")
                return report
            with patch.object(inventory, "build_inventory", side_effect=race):
                self.assertEqual(self.invoke(output, compact)[0], 2)
            self.assertEqual(raced.read_bytes(), b"created by someone else")
            self.assertFalse((compact if raced == output else output).exists())

    def test_exclusive_create_protects_against_final_open_race(self):
        output, original = self.root / "race.json", Path.open
        def race(path, mode="r", *args, **kwargs):
            if path == output and mode == "x":
                with original(output, "wb") as stream:
                    stream.write(b"racing writer")
            return original(path, mode, *args, **kwargs)
        with patch.object(Path, "open", race):
            self.assertEqual(self.invoke(output)[0], 2)
        self.assertEqual(output.read_bytes(), b"racing writer")

    def test_invalid_inputs_leave_no_outputs_and_report_errors(self):
        output, compact = self.root / "invalid.json", self.root / "invalid-compact.json"
        for path, data in ((self.names, b"bad header\n"), (self.functions, b"bad header\n"), (self.dol, b"truncated")):
            original = path.read_bytes()
            path.write_bytes(data)
            code, stdout, stderr = self.invoke(output, compact)
            self.assertEqual((code, stdout), (2, ""))
            self.assertTrue(stderr)
            self.assertFalse(output.exists())
            self.assertFalse(compact.exists())
            path.write_bytes(original)


if __name__ == "__main__":
    unittest.main()
