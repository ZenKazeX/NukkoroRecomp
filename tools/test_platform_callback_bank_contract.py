import unittest
from test_graphics_lift_inventory import dol,BASE
from platform_callback_bank_contract import recognize_bank,scan_banks


class CallbackBankTests(unittest.TestCase):
    EXAMPLE=[0x3AC00000,0x92D70000,0x3ABC01A0,0x92DC01E4,0x92DC01E8,0x92DC01EC,
        0x81950000,0x280C0000,0x41820014,0x7D8803A6,0x387E0000,0x389F0000,
        0x4E800021,0x3AD60001,0x2C160004,0x3AB50004,0x4180FFD8]
    def report(self):return recognize_bank(dol(self.EXAMPLE),BASE)
    def test_current_slot_guard_reuses_existing_semantic_owner(self):
        c=self.report();self.assertTrue(c['null_guard']['normal_linear_sequence_preserves_loaded_target'])
        self.assertEqual(c['null_guard']['skip_target'],hex(BASE+52))
    def test_read_after_callback_not_upfront_snapshot(self):
        c=self.report();self.assertIn('AFTER callback return',c['shared_semantics'])
        self.assertFalse(c['scope']['whole_bank_snapshot_equivalence_proven'])
    def test_each_instruction_and_bit_mutation_refused(self):
        for i in range(17):
            for bit in range(32):
                changed=self.EXAMPLE[:];changed[i]^=1<<bit
                with self.subTest(i=i,bit=bit),self.assertRaises(ValueError):recognize_bank(dol(changed),BASE)
    def test_counter_bound_is_conditional_on_real_callee_preservation(self):
        c=self.report();self.assertEqual(c['source_counter']['less_than_bound'],4)
        self.assertTrue(c['source_counter']['four_iterations_only_if_callback_preserves_counter_and_pointer'])
        self.assertFalse(c['scope']['callee_gpr21_22_28_30_31_preservation_proven'])
    def test_reset_order_and_argument_sources_retained(self):
        c=self.report();self.assertEqual(c['resets_before_first_slot'],[dict(base_gpr=23,displacement=0),dict(base_gpr=28,displacement=0x1e4),dict(base_gpr=28,displacement=0x1e8),dict(base_gpr=28,displacement=0x1ec)])
        self.assertEqual(c['callback_arguments'],[dict(destination_gpr=3,source_gpr=30),dict(destination_gpr=4,source_gpr=31)])
    def test_not_real_bank_or_external_entry_domination(self):
        c=self.report();self.assertFalse(c['scope']['physical_bank_reset_or_callback_alias_closed'])
        self.assertFalse(c['null_guard']['external_entry_or_context_guard_domination_proven'])
    def test_truncation_bool_alignment_refused(self):
        for raw,entry in ((dol(self.EXAMPLE[:-1]),BASE),(dol(self.EXAMPLE),BASE+1),(dol(self.EXAMPLE),True)):
            with self.assertRaises(ValueError):recognize_bank(raw,entry)
    def test_scan_counter_candidate_is_not_a_loop_proof(self):
        c=scan_banks(dol([self.EXAMPLE[0],0x4e800020]));self.assertEqual(c['summary']['reviewed_source_banks'],0)
        self.assertFalse(c['scope']['unconditional_iteration_bound_or_lifetime_closure'])


if __name__=='__main__':unittest.main()
