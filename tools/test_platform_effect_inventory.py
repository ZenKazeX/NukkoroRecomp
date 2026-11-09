import contextlib
import io
import json
from pathlib import Path
import struct
import tempfile
import unittest

if __package__:
    from . import platform_effect_inventory as inventory
    from .test_graphics_lift_inventory import BASE, BLR, NOP, branch, conditional, csv_bytes, dol
else:
    import platform_effect_inventory as inventory
    from test_graphics_lift_inventory import BASE, BLR, NOP, branch, conditional, csv_bytes, dol


def spr(number, xo=339, gpr=3):
    return (31 << 26) | (gpr << 21) | ((number & 31) << 16) | ((number >> 5) << 11) | (xo << 1)


class PlatformEffectTests(unittest.TestCase):
    def build(self, words, **kwargs):
        return inventory.build_inventory(dol(words), **kwargs)

    def test_original_known_words(self):
        for word, number, access in ((0x7c70faa6, 1008, "read"), (0x7c70fba6, 1008, "write"),
                                      (0x7c78e2a6, 920, "read"), (0x7c12e3a6, 914, "write"),
                                      (0x7cac42e6, 268, "read"), (0x7f1c43a6, 284, "write")):
            effect = inventory.platform_effect(word)
            self.assertEqual((effect["spr"], effect["access"]), (number, access))

    def test_all_spr_field_permutations(self):
        for number in range(1024):
            for xo, operation in ((339, "mfspr"), (467, "mtspr"), (371, "mftb")):
                effect = inventory.platform_effect(spr(number, xo, 17))
                self.assertEqual((effect["spr"], effect["operation"], effect["gpr"]), (number, operation, 17))

    def test_timebase_read_write_aliases_prevent_write_only_claim(self):
        report = self.build([spr(268, 371), spr(284, 467), spr(269, 371), spr(285, 467), BLR])
        groups = {g["identity"]: g for g in report["groups"]}
        self.assertEqual(groups["timebase.low"]["register_encodings"], [268, 284])
        self.assertEqual(groups["timebase.high"]["register_encodings"], [269, 285])
        for group in groups.values():
            self.assertFalse(group["encoded_reads_absent"])
            self.assertFalse(group["deletion_authorized"])

    def test_write_only_is_not_deletion_proof(self):
        group = self.build([spr(1017, 467), BLR])["groups"][0]
        self.assertTrue(group["encoded_reads_absent"])
        self.assertFalse(group["consumer_closure_proven"])
        self.assertFalse(group["deletion_authorized"])

    def test_dcbz_is_memory_write(self):
        effect = inventory.platform_effect(0x7c0007ec)
        self.assertEqual((effect["operation"], effect["domain"], effect["access"]), ("dcbz", "memory_zero", "write"))
        effect = inventory.platform_effect((4 << 26) | (1014 << 1))
        self.assertEqual((effect["operation"], effect["domain"], effect["access"]), ("dcbz_l", "memory_zero", "write"))

    def test_fpscr_has_one_numeric_owner(self):
        report = self.build([(63 << 26) | (xo << 1) for xo in (583, 711, 38, 70, 134, 64)] + [BLR])
        group = next(g for g in report["groups"] if g["identity"] == "fpscr")
        self.assertEqual(len(group["reads"]), 1)
        self.assertEqual(len(group["other"]), 5)
        self.assertFalse(group["deletion_authorized"])

    def test_ordering_and_translation_encodings(self):
        for xo, (name, domain, access) in inventory.OPERATIONS.items():
            effect = inventory.platform_effect((31 << 26) | (xo << 1))
            self.assertEqual((effect["operation"], effect["domain"], effect["access"]), (name, domain, access))
        self.assertEqual(inventory.platform_effect(0x4c00012c)["operation"], "isync")
        self.assertEqual(inventory.platform_effect(0x4c000064)["operation"], "rfi")
        self.assertEqual(inventory.platform_effect(0x44000002)["operation"], "sc")
        self.assertIsNone(inventory.platform_effect(NOP))

    def test_quantization_is_numeric_semantics(self):
        for number in range(912, 920):
            self.assertEqual(inventory.platform_effect(spr(number))["domain"], "quantized_numeric_semantics")

    def test_immediate_psq_consumes_correct_gqr_half(self):
        for primary in (56, 57, 60, 61):
            for index in range(8):
                word = (primary << 26) | (index << 12)
                consumer = inventory.implicit_consumers(word)[0]
                self.assertEqual(consumer["spr"], 912 + index)
                self.assertEqual(consumer["required_mask"], "0x00003f07" if primary >= 60 else "0x3f070000")

    def test_indexed_psq_consumes_correct_gqr_half(self):
        for xo6 in (6, 7, 38, 39):
            for index in range(8):
                word = (4 << 26) | (index << 7) | (xo6 << 1)
                consumer = inventory.implicit_consumers(word)[0]
                self.assertEqual(consumer["spr"], 912 + index)
                self.assertEqual(consumer["consumer"], "psq_store" if xo6 in (7, 39) else "psq_load")

    def test_psq_read_is_present_without_mfspr(self):
        group = next(g for g in self.build([spr(915, 467), (56 << 26) | (3 << 12), BLR])["groups"]
                     if g["identity"] == "spr.915")
        self.assertEqual(group["reads"], [])
        self.assertEqual(group["implicit_reads"], 1)
        self.assertFalse(group["encoded_reads_absent"])
        self.assertFalse(group["deletion_authorized"])

    def test_hid2_and_msr_implicit_guards(self):
        words = [(4 << 26) | (1014 << 1), (56 << 26), (4 << 26) | (6 << 1), (48 << 26), NOP]
        expected = [(0x10000000, False), (0xa0000000, True), (0x20000000, True), (0, True), (0, False)]
        for word, (mask, fp) in zip(words, expected):
            consumers = inventory.implicit_consumers(word)
            hid2 = [c for c in consumers if c.get("spr") == 920]
            self.assertEqual(hid2[0]["required_mask"] if hid2 else "0x00000000", f"0x{mask:08x}")
            self.assertEqual(any(c["identity"] == "msr" for c in consumers), fp)

    def test_rfi_reads_srr_without_mfspr(self):
        consumers = inventory.implicit_consumers(0x4c000064)
        self.assertEqual([c["spr"] for c in consumers], [26, 27])

    def test_msr_read_write_are_one_owner(self):
        group = self.build([(31 << 26) | (83 << 1), (31 << 26) | (146 << 1), BLR])["groups"][0]
        self.assertEqual(group["identity"], "msr")
        self.assertEqual(len(group["reads"]), 1)
        self.assertEqual(len(group["writes"]), 1)

    def test_implicit_example_bound_does_not_drop_counts(self):
        group = next(g for g in self.build([(56 << 26)] * 100 + [BLR])["groups"] if g["identity"] == "spr.912")
        self.assertEqual(group["implicit_reads"], 100)
        self.assertEqual(len(group["implicit_examples"]), 32)
        self.assertFalse(group["implicit_examples_complete"])
        self.assertFalse(group["consumer_closure_proven"])

    def test_scan_includes_unreached_words_without_dead_claim(self):
        report = self.build([BLR, spr(1008), BLR])
        site = report["sites"][0]
        self.assertFalse(site["direct_structural_path_from_roots"])
        self.assertFalse(site["executed_proven"])
        self.assertFalse(report["scope"]["deletion_authorized"])
        self.assertEqual(report["summary"]["text_words_scanned"], 3)

    def test_direct_call_conservatively_follows_return_and_target(self):
        report = self.build([branch(12, link=True), spr(1008), BLR, spr(1017), BLR])
        self.assertTrue(all(s["direct_structural_path_from_roots"] for s in report["sites"]))
        self.assertEqual(len(report["unresolved_control_sites"]), 2)

    def test_conditional_branches_follow_both_arms(self):
        report = self.build([conditional(12), spr(1008), BLR, spr(1017), BLR])
        self.assertTrue(all(s["direct_structural_path_from_roots"] for s in report["sites"]))

    def test_unconditional_branch_does_not_fallthrough(self):
        report = self.build([branch(12), spr(1008), BLR, spr(1017), BLR])
        self.assertEqual([s["direct_structural_path_from_roots"] for s in report["sites"]], [False, True])

    def test_indirect_call_follows_continuation_but_does_not_guess_target(self):
        report = self.build([0x4e800421, spr(1008), BLR, spr(1017), BLR])
        self.assertEqual([s["direct_structural_path_from_roots"] for s in report["sites"]], [True, False])
        self.assertFalse(report["scope"]["indirect_and_exception_closure_proven"])
        self.assertEqual(report["unresolved_control_sites"][0]["target_source"], "CTR")

    def test_exception_does_not_guess_resume(self):
        for word in (0x4c000064, 0x44000002):
            report = self.build([word, spr(1008), BLR])
            self.assertFalse(report["sites"][1]["direct_structural_path_from_roots"])
            self.assertEqual(len(report["unresolved_control_sites"]), 1)

    def test_cycle_terminates(self):
        report = self.build([spr(1008), branch(-4)])
        self.assertEqual(report["summary"]["direct_structural_words"], 2)

    def test_outside_text_is_retained(self):
        report = self.build([branch(0x1000)])
        self.assertEqual(report["outside_text_edges"], [{"site": hex(BASE), "target": hex(BASE + 0x1000)}])

    def test_explicit_roots_are_hypotheses_not_live_proof(self):
        report = self.build([BLR, spr(1008), BLR], roots=[BASE + 4])
        self.assertTrue(report["sites"][0]["direct_structural_path_from_roots"])
        self.assertFalse(report["scope"]["runtime_liveness_proven"])

    def test_invalid_roots_and_dol_refused(self):
        for root in (BASE + 1, BASE + 0x1000, -4, 1 << 32):
            with self.assertRaises(ValueError):
                self.build([BLR], roots=[root])
        with self.assertRaises(ValueError):
            inventory.build_inventory(b"invalid")

    def test_overlap_annotations_preserved(self):
        rows = csv_bytes([(BASE, "first", 12), (BASE + 4, "second", 8)])
        report = self.build([NOP, spr(1008), BLR], functions_raw=rows)
        self.assertEqual(len(report["sites"][0]["declaration_annotations"]), 2)
        self.assertEqual(len(report["declaration_extent_overlaps"]), 1)
        self.assertFalse(report["sites"][0]["executed_proven"])

    def test_lr_ctr_xer_summary_does_not_swamp_platform_sites(self):
        report = self.build([spr(8), spr(9, 467), spr(1), spr(1008), BLR])
        self.assertEqual(len(report["sites"]), 1)
        self.assertEqual(report["summary"]["execution_register_encodings_omitted_from_site_list"],
                         {"CTR.write": 1, "LR.read": 1, "XER.read": 1})

    def test_data_section_is_not_instruction_census(self):
        raw = dol([BLR], sections=[(0, BASE, [BLR]), (7, BASE + 0x1000, [spr(1008)])])
        report = inventory.build_inventory(raw)
        self.assertEqual(report["sites"], [])
        self.assertEqual(report["summary"]["text_words_scanned"], 1)

    def test_input_hashes_and_json_serialization(self):
        raw = dol([spr(1008), BLR])
        report = inventory.build_inventory(raw)
        self.assertEqual(report["inputs"]["dol"]["size_bytes"], len(raw))
        self.assertEqual(json.loads(json.dumps(report)), report)

    def test_cli_exclusive_output_and_input_preservation(self):
        with tempfile.TemporaryDirectory() as directory:
            source, output = Path(directory) / "input.dol", Path(directory) / "output.json"
            raw = dol([spr(1008), BLR])
            source.write_bytes(raw)
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(inventory.main(["--dol", str(source), "--out", str(output)]), 0)
                report = output.read_bytes()
                self.assertEqual(inventory.main(["--dol", str(source), "--out", str(output)]), 1)
                self.assertEqual(inventory.main(["--dol", str(source), "--out", str(source)]), 1)
            self.assertEqual(source.read_bytes(), raw)
            self.assertEqual(output.read_bytes(), report)


if __name__ == "__main__":
    unittest.main()
