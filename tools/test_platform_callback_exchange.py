import copy
import unittest
from platform_callback_exchange import recognize_exchange,classify_entries,census_exchanges
from test_graphics_lift_inventory import BASE,dol,branch
from test_particle_lane_events import fixture

WORDS=[0x5460063e,0x806d5a44,0x5400103a,0x7ca30214,0x80650000,0x90850000,0x4e800020]


class CallbackExchangeTests(unittest.TestCase):
    def observed(self):
        raw=dol(WORDS+[branch(-28,link=True)]);contract=recognize_exchange(raw,BASE)
        doc=fixture();e=doc['ordered_numeric_events']['events'][0]
        e['site']=hex(BASE);e['lr']=hex(BASE+32)
        e['roots'][1]='0x00000108';e['roots'][2]='0x80371e14';e['roots'][6]='0x805ec500'
        doc['config']=dict(numeric_entries=[hex(BASE)])
        doc['probes']=[dict(address=hex(BASE),hits=1,installed=True,still_installed=True,
            numeric_entry_evidence=dict(hits=1,states=[dict(hits=1)],unclassified=0))]
        return raw,contract,doc

    def test_exact_source_contract(self):
        c=recognize_exchange(dol(WORDS),BASE)
        self.assertEqual((c['index_input_gpr'],c['new_callback_input_gpr'],c['old_callback_output_gpr']),(3,4,3))
        self.assertEqual(c['current_pointer_displacement'],0x5a44)
        self.assertEqual(c['publication_site'],hex(BASE+20))
        self.assertFalse(c['scope']['successful_publication_proven'])

    def test_every_word_is_semantically_bound(self):
        for i in range(7):
            words=WORDS[:];words[i]=0x60000000
            with self.subTest(i=i),self.assertRaises(ValueError):recognize_exchange(dol(words),BASE)

    def test_mutated_index_stride_displacement_and_record_bit(self):
        for i,word in ((0,WORDS[0]^0x40),(2,WORDS[2]^0x800),(3,WORDS[3]|1),(4,WORDS[4]|4),(5,WORDS[5]|4)):
            words=WORDS[:];words[i]=word
            with self.subTest(i=i),self.assertRaises(ValueError):recognize_exchange(dol(words),BASE)

    def test_new_callback_clobber_alias_rejected(self):
        words=WORDS[:];words[5]=0x90650000
        with self.assertRaises(ValueError):recognize_exchange(dol(words),BASE)

    def test_source_bounds_and_argument_types(self):
        for entry in (True,BASE+1,BASE+4,0xffffffff):
            with self.subTest(entry=entry),self.assertRaises(ValueError):recognize_exchange(dol(WORDS),entry)

    def test_negative_pointer_displacement_is_preserved(self):
        words=WORDS[:];words[1]=0x806dfff0
        self.assertEqual(recognize_exchange(dol(words),BASE)['current_pointer_displacement'],-16)

    def test_foreign_source_contract_rejected_before_events(self):
        raw=dol(WORDS);c=recognize_exchange(raw,BASE);c['index_mask']=15
        with self.assertRaises(ValueError):classify_entries(raw,c,{})

    def test_actual_entry_low_byte_and_call_source_without_publication_claim(self):
        raw,c,d=self.observed();q=classify_entries(raw,c,d);row=q['rows'][0]
        self.assertEqual(row['slot'],8);self.assertEqual(row['raw_index'],'0x00000108')
        self.assertEqual(row['callback'],'0x80371e14')
        self.assertTrue(row['encoded_direct_call_matches'])
        self.assertEqual(row['current_pointer_slot_address_candidate'],'0x805f1f44')
        self.assertFalse(row['old_callback_or_publication_success_observed'])
        self.assertFalse(q['scope']['uninterrupted_table_history_proven'])

    def test_unknown_lr_is_not_invented_caller_alias(self):
        raw,c,d=self.observed();d['ordered_numeric_events']['events'][0]['lr']='0x00000100'
        self.assertFalse(classify_entries(raw,c,d)['rows'][0]['encoded_direct_call_matches'])

    def test_uninstalled_replaced_or_duplicate_requested_entries_rejected(self):
        for mutate in (lambda d:d['probes'][0].update(installed=False),
                       lambda d:d['probes'][0].update(still_installed=False),
                       lambda d:d['config']['numeric_entries'].append(hex(BASE))):
            raw,c,d=self.observed();mutate(d)
            with self.assertRaises(ValueError):classify_entries(raw,c,d)

    def test_incomplete_or_wrong_population_rejected(self):
        for mutate in (lambda d:d['probes'][0].update(hits=True),
                       lambda d:d['ordered_numeric_events'].update(hits=2,dropped=1,complete_for_observed_entries=False),
                       lambda d:d['ordered_numeric_events']['events'][0].update(site=hex(BASE+4))):
            raw,c,d=self.observed();mutate(d)
            with self.assertRaises(ValueError):classify_entries(raw,c,d)

    def test_noncanonical_operands_rejected(self):
        raw,c,d=self.observed();d['ordered_numeric_events']['events'][0]['roots'][1]='0x8'
        with self.assertRaises(ValueError):classify_entries(raw,c,d)

    def test_census_after_return_and_mutated_noncontract(self):
        raw=dol([0x4e800020,*WORDS,0x5460063e,*[0x60000000]*6])
        q=census_exchanges(raw)
        self.assertEqual(q['low_byte_transform_candidates'],2)
        self.assertEqual(q['recognized_structural_exchanges'],1)
        self.assertEqual(q['contracts'][0]['entry'],hex(BASE+4))
        self.assertFalse(q['scope']['callable_identity_proven'])


if __name__=='__main__':unittest.main()
