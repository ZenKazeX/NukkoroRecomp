import unittest
from test_graphics_lift_inventory import dol,BASE
from platform_indirect_guard_contract import build_guard_contract


class IndirectGuardTests(unittest.TestCase):
    ARGUMENT=[0x819F0028,0x280C0000,0x41820014,0x7D8803A6,0x389F0000,0x3860FFFF,0x4E800021,0x4E800020]
    CONSUME=[0x800D5B80,0x28000000,0x41820028,0x818D5B84,0x38000000,0x900D5B80,
             0x280C0000,0x41820014,0x7D8803A6,0x389F0000,0x38600000,0x4E800021,0x4E800020]
    def report(self,words):return build_guard_contract(dol(words),BASE)
    def test_argument_setup_between_guard_and_call(self):
        r=self.report(self.ARGUMENT);self.assertEqual(r['summary']['recognized_null_guards'],1)
        self.assertEqual(r['calls'][0]['null_guard']['target_register'],12)
        self.assertIsNone(r['calls'][0]['consume_before_callback'])
    def test_pending_cleared_before_null_check_and_callback(self):
        r=self.report(self.CONSUME);c=r['calls'][0]['consume_before_callback']
        self.assertEqual(c['flag_displacement'],0x5B80)
        self.assertEqual(c['flag_clear_site'],hex(BASE+20))
        self.assertTrue(c['null_callback_still_consumes_pending_flag'])
        self.assertFalse(r['scope']['external_alias_epoch_or_lifetime_proven'])
    def test_target_clobber_or_unproven_intervening_call_rejected(self):
        for w in (0x39800000,0x48000041):
            words=self.ARGUMENT[:];words[4]=w
            with self.subTest(w=w):self.assertIsNone(self.report(words)['calls'][-1]['null_guard'])
    def test_wrong_null_branch_and_compare_rejected(self):
        for i,w in ((1,0x280C0001),(1,0x284C0000),(1,0x282C0000),(2,0x40820014),(2,0x41820010)):
            words=self.ARGUMENT[:];words[i]=w
            with self.subTest(i=i):self.assertIsNone(self.report(words)['calls'][0]['null_guard'])
    def test_clear_wrong_field_value_or_register_rejected(self):
        for i,w in ((4,0x38000001),(5,0x900D5B84),(5,0x906D5B80)):
            words=self.CONSUME[:];words[i]=w
            with self.subTest(i=i):self.assertIsNone(self.report(words)['calls'][0]['consume_before_callback'])
    def test_target_bank_changes_rejected(self):
        words=self.ARGUMENT[:];words[3]=0x7D8903A6
        self.assertIsNone(self.report(words)['calls'][0]['null_guard'])
    def test_clear_must_preserve_original_flag_base(self):
        for base in (12,13):
            words=self.CONSUME[:]
            words[0]=0x80000000|(base<<21)|(base<<16)|0x5B80
            words[1]=0x28000000|(base<<16)
            words[4]=0x38000000|(base<<21)
            words[5]=0x90000000|(base<<21)|(base<<16)|0x5B80
            self.assertIsNone(self.report(words)['calls'][0]['consume_before_callback'])
        words=self.CONSUME[:];words[0]=0x800C5B80;words[5]=0x900C5B80
        self.assertIsNone(self.report(words)['calls'][0]['consume_before_callback'])
    def test_guard_semantics_not_extended_to_external_entries(self):
        r=build_guard_contract(dol(self.ARGUMENT),BASE+12)
        self.assertEqual(r['summary']['recognized_null_guards'],0)
    def test_null_guard_stays_distinct_from_execution_proof(self):
        r=self.report(self.ARGUMENT)
        self.assertFalse(r['scope']['executed_or_fault_state_parity_proven'])
        self.assertFalse(r['calls'][0]['null_guard']['external_entry_or_context_guard_domination_proven'])


if __name__=='__main__':unittest.main()
