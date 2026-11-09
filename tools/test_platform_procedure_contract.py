import contextlib
import io
import json
from pathlib import Path
import tempfile
import unittest

if __package__:
    from . import platform_procedure_contract as contract
    from .test_graphics_lift_inventory import BASE, BLR, NOP, dol, branch, conditional, csv_bytes
    from .test_platform_effect_inventory import spr
else:
    import platform_procedure_contract as contract
    from test_graphics_lift_inventory import BASE, BLR, NOP, dol, branch, conditional, csv_bytes
    from test_platform_effect_inventory import spr


class ProcedureCandidateTests(unittest.TestCase):
    def build(self, words, functions=None):
        return contract.build_contract(dol(words), BASE, functions)

    def test_incoming_batch_keeps_source_order_and_independent_schema(self):
        raw=dol([branch(16,link=True),branch(8),BLR,BLR,BLR])
        reports=contract.build_incoming_contracts(raw,(BASE+16,BASE+12))
        self.assertEqual([x['target'] for x in reports],[hex(BASE+16),hex(BASE+12)])
        self.assertEqual(reports[0]['summary']['direct_call_encodings'],1)
        self.assertEqual(reports[1]['summary']['direct_call_encodings'],0)
        self.assertEqual(reports[1]['direct_branches'][0]['site'],hex(BASE+4))
        reports[0]['inputs']['dol']['sha256']='mutation'
        self.assertNotEqual(reports[1]['inputs']['dol']['sha256'],'mutation')
        self.assertFalse(reports[1]['scope']['deletion_authorized'])

    def test_incoming_batch_keeps_data_literals_and_cross_declaration_tails(self):
        raw=dol(sections=[(0,BASE,[branch(12),BLR,BLR,BLR]),(7,BASE+0x100,[BASE+12,BASE+4])])
        functions=csv_bytes([(BASE,'tiny',4),(BASE+12,'overlap',4)])
        reports=contract.build_incoming_contracts(raw,(BASE+12,BASE+4),functions)
        self.assertEqual(reports[0]['summary']['direct_branch_encodings'],1)
        self.assertEqual(reports[0]['literals'][0]['site'],hex(BASE+0x100))
        self.assertEqual(reports[1]['literals'][0]['site'],hex(BASE+0x104))
        self.assertFalse(reports[0]['literals'][0]['pointer_identity_proven'])
        self.assertIn('functions',reports[0]['inputs'])

    def test_incoming_batch_keeps_address_constructions_and_reserved_near_match(self):
        first=0x3c608000;second=0x60631014
        raw=dol([first,second,first|(1<<16),second,BLR,BLR])
        reports=contract.build_incoming_contracts(raw,(BASE+20,BASE+16))
        self.assertEqual(reports[0]['summary']['adjacent_address_constructions'],1)
        self.assertEqual(reports[1]['summary']['adjacent_address_constructions'],0)
        self.assertFalse(reports[0]['adjacent_address_constructions'][0]['pointer_identity_or_execution_proven'])

    def test_incoming_batch_indirect_targets_never_become_direct_hits(self):
        reports=contract.build_incoming_contracts(dol([0x4e800421,BLR,BLR]),(BASE+4,BASE+8))
        for report in reports:
            self.assertEqual(report['summary']['direct_branch_encodings'],0)
            self.assertFalse(report['scope']['indirect_closure_proven'])

    def test_incoming_batch_rejects_duplicates_empty_bool_unbacked_and_data(self):
        raw=dol(sections=[(0,BASE,[BLR]),(7,BASE+0x100,[BLR])])
        for targets in ((),(BASE,BASE),(True,),(BASE+1,),(BASE-4,),(BASE+0x100,),(float(BASE),)):
            with self.assertRaises(ValueError):contract.build_incoming_contracts(raw,targets)

    def test_calls_do_not_swallow_callee_effects(self):
        report = self.build([branch(12, link=True), spr(912), BLR, spr(913), BLR])
        self.assertEqual(report["summary"]["words"], 3)
        self.assertEqual(len(report["calls"]), 1)
        self.assertEqual(report["calls"][0]["target"], BASE + 12)
        self.assertNotIn("spr.913:read", report["summary"]["platform_effects"])
        self.assertFalse(report["scope"]["native_replacement_proven"])

    def test_indirect_call_keeps_continuation_and_unresolved_owner(self):
        report = self.build([0x4e800421, NOP, BLR])
        self.assertEqual(report["summary"]["words"], 3)
        self.assertEqual(report["summary"]["indirect_calls"], 1)
        self.assertFalse(report["scope"]["indirect_closure_proven"])

    def test_indirect_terminal_never_guesses_target(self):
        report = self.build([0x4e800420, spr(912), BLR])
        self.assertEqual(report["summary"]["words"], 1)
        self.assertEqual(report["summary"]["unresolved_terminal_sites"], 1)

    def test_conditional_paths_cycles_and_return(self):
        report = self.build([conditional(8), branch(-4), BLR])
        self.assertEqual(report["summary"]["words"], 3)
        self.assertEqual(report["summary"]["return_like_sites"], 1)

    def test_csv_boundary_does_not_truncate_real_direct_flow(self):
        rows = csv_bytes([(BASE, "untrusted", 4), (BASE, "overlap", 8)])
        report = self.build([NOP, spr(912, 467), BLR], rows)
        self.assertEqual(report["summary"]["words"], 3)
        self.assertEqual(len(report["words"][0]["declaration_annotations"]), 2)
        self.assertEqual(report["summary"]["platform_effects"]["spr.912:write"], 1)
        self.assertFalse(report["scope"]["procedure_identity_proven"])

    def test_implicit_psq_consumer_is_not_lost(self):
        report = self.build([0xe1040000, BLR])
        self.assertEqual(report["summary"]["implicit_consumers"]["psq_load:spr.912"], 1)

    def test_exception_is_not_invented_continuation(self):
        report = self.build([0x4c000064, NOP, BLR])
        self.assertEqual(report["summary"]["words"], 1)
        self.assertEqual(report["unresolved_terminals"][0]["mnemonic"], "rfi")

    def test_outside_edge_retained_and_bad_entry_refused(self):
        report = self.build([branch(100)])
        self.assertEqual(report["summary"]["outside_text_edges"], 1)
        for entry in (BASE + 1, BASE - 4, BASE + 100):
            with self.assertRaises(ValueError):
                contract.build_contract(dol([BLR]), entry)

    def test_cli_exclusive_output_preserves_inputs(self):
        with tempfile.TemporaryDirectory() as directory:
            source, output = Path(directory) / 'source.dol', Path(directory) / 'contract.json'
            raw = dol([NOP, BLR])
            source.write_bytes(raw)
            args = ['--dol', str(source), '--entry', hex(BASE), '--out', str(output)]
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(contract.main(args), 0)
                before = output.read_bytes()
                self.assertEqual(contract.main(args), 1)
                self.assertEqual(contract.main(args[:-1] + [str(source)]), 1)
            self.assertEqual(output.read_bytes(), before)
            self.assertEqual(source.read_bytes(), raw)
            self.assertFalse(json.loads(before)['scope']['deletion_authorized'])

    def continuation(self, words, site=BASE):
        return contract.build_continuation_contract(dol(words), site)

    def test_continuation_excludes_called_body_and_refuses_liveness_claim(self):
        report = self.continuation([branch(12, link=True), NOP, BLR, 0xc0230000, BLR])
        self.assertEqual(report["entry"], hex(BASE + 4))
        self.assertEqual(report["summary"]["words"], 2)
        self.assertEqual(report["known_numeric_encoding_sites"], [])
        self.assertFalse(report["scope"]["numeric_liveness_proven"])
        self.assertFalse(report["scope"]["native_context_retirement_proven"])
        self.assertFalse(report["scope"]["deletion_authorized"])

    def test_continuation_detects_scalar_indexed_paired_and_status_encodings(self):
        # lfs f1,0(r3); lfsx f1,r3,r4; psq_l; mffs f0.
        indexed = (31 << 26) | (1 << 21) | (3 << 16) | (4 << 11) | (535 << 1)
        status = (63 << 26) | (583 << 1)
        report = self.continuation([0x4e800421, 0xc0230000, indexed, 0xe1040000, status, BLR])
        self.assertEqual(report["known_numeric_encoding_sites"], [hex(BASE + i*4) for i in range(1, 5)])
        self.assertEqual(report["summary"]["known_numeric_encoding_sites"], 4)

    def test_continuation_keeps_later_call_effects_unresolved(self):
        report = self.continuation([0x4e800421, 0x4e800421, BLR])
        self.assertEqual(report["summary"]["indirect_calls"], 1)
        self.assertFalse(report["scope"]["indirect_closure_proven"])
        self.assertEqual(report["known_numeric_encoding_sites"], [])

    def test_continuation_includes_both_arms_and_cycles(self):
        report = self.continuation([0x4e800421, conditional(8), BLR, 0xc0230000, branch(-12)])
        self.assertEqual(report["summary"]["words"], 4)
        self.assertEqual(report["known_numeric_encoding_sites"], [hex(BASE + 12)])

    def test_conditional_call_does_not_invent_execution_or_returnability(self):
        call = conditional(12) | 1
        report = self.continuation([call, NOP, BLR, BLR])
        self.assertTrue(report["source_call"]["conditional"])
        self.assertFalse(report["scope"]["executed_proven"])
        self.assertEqual(report["summary"]["words"], 2)

    def test_continuation_refuses_non_calls_exceptions_and_missing_next_word(self):
        for word in (NOP, BLR, branch(4), 0x44000002, 0x4c000064, 0x4e800420):
            with self.assertRaises(ValueError):
                self.continuation([word, BLR])
        with self.assertRaises(ValueError):
            self.continuation([0x4e800421])
        for site in (BASE + 1, BASE - 4, BASE + 100):
            with self.assertRaises(ValueError):
                self.continuation([0x4e800421, BLR], site)

    def test_continuation_exception_after_return_has_no_guessed_successor(self):
        report = self.continuation([0x4e800421, 0x4c000064, 0xc0230000, BLR])
        self.assertEqual(report["summary"]["words"], 1)
        self.assertEqual(report["summary"]["unresolved_terminal_sites"], 1)
        self.assertFalse(report["scope"]["native_context_retirement_proven"])

    def test_continuation_cli_preserves_exclusive_output_and_invalid_input(self):
        with tempfile.TemporaryDirectory() as directory:
            source, output = Path(directory) / 'source.dol', Path(directory) / 'continuation.json'
            raw = dol([0x4e800421, NOP, BLR])
            source.write_bytes(raw)
            args = ['--dol', str(source), '--after-call', hex(BASE), '--out', str(output)]
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(contract.main(args), 0)
                before = output.read_bytes()
                self.assertEqual(contract.main(args), 1)
                self.assertEqual(contract.main(args[:-1] + [str(source)]), 1)
                invalid = Path(directory) / 'invalid.json'
                self.assertEqual(contract.main(['--dol', str(source), '--after-call', hex(BASE + 4),
                                                '--out', str(invalid)]), 1)
            self.assertEqual(output.read_bytes(), before)
            self.assertEqual(source.read_bytes(), raw)
            self.assertFalse(invalid.exists())
            self.assertEqual(json.loads(before)['schema'], 'platform_call_continuation_candidate_v1')

    def test_incoming_includes_direct_calls_and_non_call_tails(self):
        raw=dol([branch(12,link=True),branch(8),0x4e800421,BLR])
        report=contract.build_incoming_contract(raw,BASE+12)
        self.assertEqual(report['summary']['direct_branch_encodings'],2)
        self.assertEqual(report['summary']['direct_call_encodings'],1)
        self.assertFalse(report['scope']['caller_closure_proven'])
        self.assertFalse(report['scope']['indirect_closure_proven'])

    def test_incoming_literal_is_not_certified_pointer_or_execution(self):
        report=contract.build_incoming_contract(dol([BASE+4,BLR]),BASE+4)
        self.assertEqual(report['summary']['aligned_target_literals'],1)
        self.assertEqual(report['literals'][0]['section_kind'],'text')
        self.assertFalse(report['literals'][0]['pointer_identity_proven'])
        self.assertFalse(report['scope']['executed_proven'])

    def test_adjacent_lis_addi_recognizes_address_but_not_caller(self):
        target=BASE+8
        first=(15<<26)|(3<<21)|(target>>16)
        second=(14<<26)|(4<<21)|(3<<16)|(target&65535)
        c=contract.build_incoming_contract(dol([first,second,BLR]),target)
        self.assertEqual(c['summary']['adjacent_address_constructions'],1)
        self.assertEqual(c['adjacent_address_constructions'][0]['result_gpr'],4)
        self.assertFalse(c['adjacent_address_constructions'][0]['pointer_identity_or_execution_proven'])
        self.assertFalse(c['scope']['caller_closure_proven'])

    def test_signed_low_addition_and_32_bit_wrap(self):
        for high,low,result in ((0x8038,0xE014,0x8037E014),(0,0xffff,0xffffffff),(0xffff,0x8000,0xfffe8000)):
            value=contract.adjacent_address_value((15<<26)|(3<<21)|high,(14<<26)|(4<<21)|(3<<16)|low)
            self.assertEqual(value['value'],result)

    def test_ori_low_half_unsigned_and_zero_source_register_allowed(self):
        first=(15<<26)|0x8037
        second=(24<<26)|(4<<16)|0xe014
        value=contract.adjacent_address_value(first,second)
        self.assertEqual(value['value'],0x8037e014);self.assertEqual(value['pattern'],'lis_ori')
        self.assertEqual(value['source_gpr'],0);self.assertEqual(value['result_gpr'],4)

    def test_zero_base_addi_does_not_read_lis_r0(self):
        self.assertIsNone(contract.adjacent_address_value((15<<26)|0x8037,(14<<26)|(4<<21)|0xe014))

    def test_mismatched_registers_and_non_lis_first_refused(self):
        first=(15<<26)|(3<<21)|0x8037
        for second in ((14<<26)|(4<<21)|(5<<16)|0xe014,(24<<26)|(5<<21)|(4<<16)|0xe014):
            self.assertIsNone(contract.adjacent_address_value(first,second))
        self.assertIsNone(contract.adjacent_address_value(first|(1<<16),(14<<26)|(4<<21)|(3<<16)|0xe014))

    def test_nonadjacent_and_data_patterns_not_guessed(self):
        target=BASE+12;first=(15<<26)|(3<<21)|(target>>16)
        second=(14<<26)|(4<<21)|(3<<16)|(target&65535)
        raw=dol([first,NOP,second,BLR],sections=[(0,BASE,[first,NOP,second,BLR]),(7,BASE+4096,[first,second])])
        c=contract.build_incoming_contract(raw,target)
        self.assertEqual(c['adjacent_address_constructions'],[])

    def test_pair_cannot_cross_two_text_sections(self):
        target=BASE+4;first=(15<<26)|(3<<21)|(target>>16)
        second=(14<<26)|(4<<21)|(3<<16)|(target&65535)
        raw=dol([first],sections=[(0,BASE,[first]),(1,BASE+4,[second,BLR])])
        self.assertEqual(contract.build_incoming_contract(raw,target)['adjacent_address_constructions'],[])

    def test_incoming_refuses_bad_target_and_preserves_exclusive_cli_outputs(self):
        raw=dol([branch(4,link=True),BLR])
        for target in (BASE+1,BASE-4,BASE+100):
            with self.assertRaises(ValueError): contract.build_incoming_contract(raw,target)
        with tempfile.TemporaryDirectory() as directory:
            source,output=Path(directory)/'original.dol',Path(directory)/'incoming.json'
            source.write_bytes(raw)
            args=['--dol',str(source),'--incoming',hex(BASE+4),'--out',str(output)]
            with contextlib.redirect_stdout(io.StringIO()),contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(contract.main(args),0); before=output.read_bytes()
                self.assertEqual(contract.main(args),1)
                self.assertEqual(contract.main(args[:-1]+[str(source)]),1)
            self.assertEqual(output.read_bytes(),before); self.assertEqual(source.read_bytes(),raw)
            self.assertFalse(json.loads(before)['scope']['deletion_authorized'])


if __name__ == "__main__":
    unittest.main()
