import hashlib,json,struct,unittest
from pathlib import Path
from test_graphics_lift_inventory import dol
from platform_callback_registry_contract import recognize_registry,scan_registries


class RegistryContractTests(unittest.TestCase):
    REG,REM,ACT=0x803B617C,0x803B6248,0x803B60E4
    def segments(self):
        x=json.loads((Path(__file__).with_name('test_data')/'callback_registry_source_examples.json').read_text(encoding='utf-8'))
        out=[]
        for s in x['segments']:
            words=[int(w,16) for w in s['words']]
            self.assertEqual(hashlib.sha256(b''.join(w.to_bytes(4,'big') for w in words)).hexdigest(),s['original_word_bytes_sha256'])
            out.append((len(out),int(s['address'],16),words))
        return out
    def fixture(self,segments=None):return dol(address=self.REG,sections=self.segments() if segments is None else segments)
    def report(self):return recognize_registry(self.fixture(),self.REG,self.REM)
    def test_source_capacity_and_stride_are_recovered(self):
        c=self.report();self.assertEqual(c['table']['slots'],4);self.assertEqual(c['table']['stride_bytes'],4)
        self.assertEqual(c['table']['original_address'],'0x805a68b0')
        self.assertTrue(c['scope']['source_capacity_is_observable_not_arbitrary_host_pool'])
    def test_bss_or_absent_table_not_invented_zero(self):
        c=self.report();self.assertIsNone(c['table']['file_backed_initial_bytes'])
        self.assertFalse(c['table']['boot_initial_contents_proven'])
    def test_duplicate_search_precedes_first_empty_and_zero_is_not_filtered(self):
        c=self.report()
        self.assertIn('Search all four',c['register_cases']['duplicate'])
        self.assertIn('first empty',c['register_cases']['absent_with_space'])
        self.assertIn('existing duplicate',c['register_cases']['zero_input'])
    def test_repeated_activation_is_not_collapsed_to_empty_transition(self):
        c=self.report();a=c['activation']
        self.assertFalse(a['repeated_enable_is_noop'])
        self.assertEqual(a['unconditional_on_nonzero_input_zero_store_addresses'],['0x805a68f0','0x805a68f4','0x805a68f8','0x805a68fc'])
        self.assertIn('other slots already nonzero',c['register_cases']['absent_with_space'])
    def test_remove_only_first_and_disable_only_when_all_empty(self):
        c=self.report();self.assertIn('only first',c['remove_cases']['found'])
        self.assertIn('all are zero',c['remove_cases']['found'])
        self.assertIn('Later duplicates remain',c['remove_cases']['duplicate_slots'])
    def test_failure_and_duplicate_have_no_activation(self):
        c=self.report();self.assertIn('no table store or activation',c['register_cases']['duplicate'])
        self.assertIn('no table store or activation',c['register_cases']['absent_and_full'])
        self.assertIn('no table store or activation',c['remove_cases']['absent'])
    def test_each_original_word_mutation_rejected(self):
        segments=self.segments()
        for s,(_,_,words) in enumerate(segments):
            for i in range(len(words)):
                changed=[(index,a,w[:]) for index,a,w in segments];changed[s][2][i]=0
                with self.subTest(segment=s,word=i),self.assertRaises(ValueError):recognize_registry(self.fixture(changed),self.REG,self.REM)
    def test_every_capacity_branch_stride_and_reset_bit_mutation_rejected(self):
        segments=self.segments()
        positions=((0,8),(0,14),(0,15),(0,21),(0,22),(0,29),(0,32),(0,33),(0,41),
                   (1,8),(1,16),(1,18),(1,22),(1,26),(1,40),(1,41),(1,42),(1,43),(1,51),
                   (2,12),(2,18),(2,20),(2,21),(2,22),(2,23),(2,24),(2,27))
        for s,i in positions:
            for bit in range(32):
                changed=[(index,a,w[:]) for index,a,w in segments];changed[s][2][i]^=1<<bit
                with self.subTest(segment=s,word=i,bit=bit),self.assertRaises(ValueError):recognize_registry(self.fixture(changed),self.REG,self.REM)
    def test_independent_table_addresses_cannot_be_merged(self):
        segments=self.segments();segments[1][2][9]^=4
        with self.assertRaises(ValueError):recognize_registry(self.fixture(segments),self.REG,self.REM)
    def test_leaf_preservation_or_activation_calls_cannot_be_assumed(self):
        segments=self.segments()
        for s,i in ((0,6),(0,17),(0,34),(0,36),(0,43),(1,6),(1,44),(1,46),(1,53),(2,9),(2,29)):
            changed=[(index,a,w[:]) for index,a,w in segments];changed[s][2][i]^=4
            with self.subTest(segment=s,word=i),self.assertRaises(ValueError):recognize_registry(self.fixture(changed),self.REG,self.REM)
    def test_later_faults_aliases_and_callback_lifetime_stay_open(self):
        c=self.report()
        for k in ('stack_table_backing_or_device_alias_closed','actual_publication_or_failure_rollback_proven','interrupted_table_epochs_callback_delivery_or_lifetime_proven','full_cpu_fault_retirement_state_pixel_or_audio_parity_proven','native_ownership_or_retirement_proven','deletion_authorized'):
            self.assertFalse(c['scope'][k])
    def test_full_scan_and_pair_work_bound(self):
        r=scan_registries(self.fixture());self.assertEqual(r['summary']['reviewed_registry_pairs'],1)
        self.assertEqual(r['summary']['pair_attempts'],1)
        for budget in (0,-1,True):
            with self.assertRaises(ValueError):scan_registries(self.fixture(),max_pair_attempts=budget)
    def test_work_overflow_refuses_instead_of_emitting_partial_pairs(self):
        s=self.segments()
        extra=s[0][2][:10]
        s.append((5,0x803C0000,extra))
        with self.assertRaisesRegex(ValueError,'no partial proof emitted'):
            scan_registries(self.fixture(s),max_pair_attempts=1)
    def test_control_masks_match_independent_ppc_mask_bit_positions(self):
        c=self.report();words=self.segments()[2][2]
        def mask(word):
            self.assertEqual((word>>11)&31,0)
            mb,me=(word>>6)&31,(word>>1)&31
            positions=list(range(mb,me+1)) if mb<=me else list(range(mb,32))+list(range(me+1))
            return sum(1<<(31-i) for i in positions)
        self.assertEqual(mask(words[12]),c['activation']['nonzero_input_sets_control_bit'])
        self.assertEqual(mask(words[26]),0xffffffff^c['activation']['zero_input_clears_control_bit'])
        self.assertEqual(mask(words[27]),c['activation']['final_control_mask'])
    def test_missing_registry_arm_cannot_be_declared_retired(self):
        s=self.segments();s[1][2][8]=0x38000005
        r=scan_registries(self.fixture(s));self.assertEqual(r['summary']['reviewed_registry_pairs'],0)
        self.assertFalse(r['scope']['native_retirement_proven'])
    def test_alignment_bool_and_wrong_entries_refused(self):
        raw=self.fixture()
        for a,b in ((self.REG+1,self.REM),(True,self.REM),(self.REM,self.REG)):
            with self.assertRaises(ValueError):recognize_registry(raw,a,b)


if __name__=='__main__':unittest.main()
