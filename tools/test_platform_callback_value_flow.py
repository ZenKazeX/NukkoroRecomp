import unittest
from platform_callback_value_flow import build_value_flow
from test_graphics_lift_inventory import dol,BASE,branch


def values(row):return {v['value'] for v in row['possible_values'] if v['kind']=='constant'}


class CallbackValueFlowTests(unittest.TestCase):
    SELECT=[0x2C030000,0x41820010,0x3C808038,0x3804DF10,branch(8),0x38000000,0x900D5BB0,0x4E800020]
    def report(self,words,entry=BASE):return build_value_flow(dol(words),entry)
    def test_conditional_publication_keeps_both_literal_arms(self):
        r=self.report(self.SELECT);s=r['publications'][0]
        self.assertEqual(values(s),{'0x8037df10','0x00000000'})
        self.assertFalse(s['unknown_alternative']);self.assertEqual((s['base_gpr'],s['displacement']),(13,0x5BB0))
        self.assertFalse(s['successful_store_or_base_epoch_or_alias_proven'])
    def test_direct_entry_after_high_word_remains_unknown(self):
        r=self.report(self.SELECT,BASE+12)
        self.assertTrue(r['publications'][0]['unknown_alternative'])
        self.assertNotIn('0x8037df10',values(r['publications'][0]))
    def test_low_word_wrap_and_ori_are_original_arithmetic(self):
        a=self.report([0x3C808000,0x3804FFFF,0x900D0000,0x4E800020])
        self.assertEqual(values(a['publications'][0]),{'0x7fffffff'})
        b=self.report([0x3C008037,0x600016BC,0x900D0000,0x4E800020])
        self.assertEqual(values(b['publications'][0]),{'0x803716bc'})
    def test_call_kills_apparently_nonvolatile_and_target_banks(self):
        r=self.report([0x3BC00001,branch(100,link=True),0x93CD0000,0x4E800020])
        self.assertTrue(r['publications'][0]['unknown_alternative'])
        self.assertEqual(r['summary']['call_barriers'],1)
    def test_symbolic_field_to_lr_call_consumes_pre_link_target(self):
        r=self.report([0x818D5BB0,0x280C0000,0x4182000C,0x7D8803A6,0x4E800021,0x4E800020])
        c=r['indirect_calls'][0];self.assertEqual(c['target_source'],'LR')
        self.assertEqual(c['pre_link_target_candidates'][0]['kind'],'field_load')
        self.assertEqual(c['pre_link_target_candidates'][0]['displacement'],0x5BB0)
        self.assertFalse(c['actual_target_or_call_completion_proven'])
        self.assertEqual(c['null_skip_guard_candidate']['skip_target'],hex(BASE+20))
    def test_symbolic_field_to_ctr_call(self):
        r=self.report([0x818D5BB0,0x7D8903A6,0x4E800421,0x4E800020])
        c=r['indirect_calls'][0];self.assertEqual(c['target_source'],'CTR')
        self.assertEqual(c['pre_link_target_candidates'][0]['kind'],'field_load')
    def test_copy_value_through_other_register(self):
        r=self.report([0x38000001,0x7C041B78,0x908D0000,0x4E800020]) # or r4,r0,r3 is not a copy.
        self.assertTrue(r['publications'][0]['unknown_alternative'])
        r=self.report([0x38000001,0x7C040378,0x908D0000,0x4E800020]) # or r4,r0,r0.
        self.assertEqual(values(r['publications'][0]),{'0x00000001'})
    def test_load_never_supplies_memory_value_or_alias(self):
        r=self.report([0x806D5BB0,0x906D5BB4,0x4E800020])
        v=r['publications'][0]['possible_values'][0];self.assertEqual(v['kind'],'field_load')
        self.assertFalse(v['loaded_value_or_base_epoch_or_alias_proven'])
        self.assertEqual(values(r['publications'][0]),set())
    def test_nonadjacent_unchanged_lis_definition_is_bound(self):
        r=self.report([0x3C808038,0x60000000,0x3804DF10,0x900D0000,0x4E800020])
        self.assertEqual(values(r['publications'][0]),{'0x8037df10'})
        self.assertEqual(r['publications'][0]['possible_values'][0]['original_definition_sites'],[hex(BASE),hex(BASE+8)])
    def test_nonadjacent_clobber_call_unknown_and_nonlis_seed(self):
        for first,middle in ((0x3C808038,0x808D0000),(0x3C808038,branch(100,link=True)),
                             (0x3C808038,0),(0x38800001,0x60000000)):
            r=self.report([first,middle,0x3804DF10,0x900D0000,0x4E800020])
            self.assertTrue(r['publications'][0]['unknown_alternative'])
    def test_finite_pair_cannot_seed_repeated_arithmetic(self):
        r=self.report([0x3C808038,0x3884DF10,0x38840004,0x908D0000,branch(-8)])
        self.assertTrue(r['publications'][0]['unknown_alternative'])
        self.assertLess(r['summary']['dataflow_updates'],20)
    def test_direct_call_reports_pre_call_facts_then_kills_them(self):
        r=self.report([0x3C808038,0x90010004,0x3884D810,branch(100,link=True),0x908D0000,0x4E800020])
        c=r['direct_calls'][0]
        self.assertEqual(len(c['pre_call_gpr_candidates']),32)
        self.assertEqual(values(c['pre_call_gpr_candidates'][4]),{'0x8037d810'})
        self.assertFalse(c['input_abi_or_memory_contents_or_call_execution_proven'])
        self.assertTrue(r['publications'][-1]['unknown_alternative'])
    def test_distinct_lis_paths_are_not_collapsed_into_one_pointer(self):
        r=self.report([0x41820010,0x3C808038,0x60000000,branch(8),0x3C808039,0x3884D810,0x908D0000,0x4E800020])
        self.assertEqual(values(r['publications'][0]),{'0x8037d810','0x8038d810'})
    def test_unsupported_word_clears_all_candidates(self):
        r=self.report([0x38000001,0x00000000,0x900D0000,0x4E800020])
        self.assertTrue(r['publications'][0]['unknown_alternative']);self.assertEqual(r['summary']['unsupported_barriers'],1)
    def test_arithmetic_loop_converges_without_enumerating_u32_space(self):
        r=self.report([0x38000000,0x38000001,0x900D0000,branch(-8)])
        self.assertLessEqual(r['summary']['dataflow_updates'],6)
        self.assertEqual(values(r['publications'][0]),{'0x00000001'})
        r=self.report([0x38000000,0x38000000|0x00010000,0x900D0000,branch(-8)]) # addi r0,r1,0 unknown
        self.assertTrue(r['publications'][0]['unknown_alternative'])
        r=self.report([0x38600000,0x38630001,0x906D0000,branch(-8)])
        self.assertTrue(r['publications'][0]['unknown_alternative'])
        self.assertLessEqual(r['summary']['dataflow_updates'],6)
    def test_return_and_unreachable_words_do_not_add_fake_values(self):
        r=self.report([0x4E800020,0x38000001,0x900D0000])
        self.assertEqual(r['summary']['original_words'],1);self.assertEqual(r['publications'],[])
    def test_distinct_path_unknown_does_not_disappear_at_merge(self):
        words=self.SELECT[:];words[5]=0x800D5BB4
        r=self.report(words);s=r['publications'][0]
        self.assertEqual({v['kind'] for v in s['possible_values']},{'constant','field_load'})
        self.assertFalse(r['scope']['loaded_memory_read_from_proven'])
    def test_entry_type_alignment_and_extent_refused(self):
        for v in (True,BASE+1,0x70000000):
            with self.subTest(v=v),self.assertRaises(ValueError):self.report([0x4E800020],v)
    def test_invalid_spr_record_bit_cannot_preserve_target(self):
        r=self.report([0x818D5BB0,0x7D8803A7,0x4E800021,0x4E800020])
        self.assertEqual(r['summary']['unsupported_barriers'],1)
        self.assertEqual(r['indirect_calls'][0]['pre_link_target_candidates'][0]['kind'],'unknown')
    def test_changed_null_guard_does_not_claim_skip_semantics(self):
        words=[0x818D5BB0,0x280C0000,0x4182000C,0x7D8803A6,0x4E800021,0x4E800020]
        for i,value in ((1,0x280C0001),(1,0x284C0000),(2,0x4082000C),(3,0x7C0803A6)):
            v=words[:];v[i]=value
            with self.subTest(i=i):self.assertIsNone(self.report(v)['indirect_calls'][0]['null_skip_guard_candidate'])


if __name__=='__main__':unittest.main()
