import copy
import unittest
from test_graphics_lift_inventory import dol,BASE,branch,conditional,csv_bytes
from platform_callback_reload_contract import recognize_reload,scan_reloads
from shadow_semantic_patterns import build_pipeline


class CallbackReloadTests(unittest.TestCase):
    def fixture(self,kind=0):
        field=(0x5E34,0x5E40,0x5E44)[kind]
        prefix=[0x800D0000|field]
        if kind==1:prefix.append(0x806D5E00)
        if kind==2:prefix.append(0x986D5E48) # Store, not a harmless byte load.
        prefix.append(0x28000000)
        if kind==1:prefix.append(0xA3E3000E)
        guard=len(prefix);prefix.append(0)
        words=prefix+[0x38610010,0,0x38610010,0,0x818D0000|field]
        if kind==1:words.append(0x387F0000)
        words.extend([0x7D8803A6,0x4E800021,0x38610010,0,0x7FE3FB78,0])
        end=len(words);words[guard]=conditional(4*(end-guard),bo=12,bi=2)
        calls=[guard+2,guard+4,end-3,end-1]
        for i,target in zip(calls,[50,51,50,51]):words[i]=branch(4*(target-i),link=True)
        words.append(0x4E800020);words.extend([0x60000000]*(50-len(words)))
        words.extend([0x4E800020,0x4E800020])
        return words,end,calls
    def contract(self,kind=0):return recognize_reload(dol(self.fixture(kind)[0]),BASE)
    def test_training_and_two_held_out_preparation_shapes(self):
        for kind in range(3):
            c=self.contract(kind)
            self.assertEqual(c['field_coordinate']['displacement'],(0x5E34,0x5E40,0x5E44)[kind])
            self.assertEqual(len(c['preparation_effects']),(0,2,1)[kind])
            self.assertFalse(c['invoked_read']['nonnull_test_of_this_read'])
    def test_preparation_store_is_retained(self):
        self.assertEqual(self.contract(2)['preparation_effects'][0]['word'],'0x986d5e48')
    def test_distinct_reads_do_not_forward_a_nonnull_fact(self):
        c=self.contract();self.assertNotEqual(c['tested_read']['site'],c['invoked_read']['site'])
        self.assertTrue(c['scope']['earlier_nonnull_test_does_not_prove_reloaded_nonnull'])
        self.assertTrue(c['scope']['same_encoded_coordinate_not_same_base_epoch_or_value'])
        self.assertFalse(c['scope']['helper_effects_or_register_preservation_proven'])
    def test_every_instruction_mutation_has_explicit_result(self):
        # Kill each instruction rather than select only easy operand mutations.
        for kind in range(3):
            words,end,_=self.fixture(kind)
            for i in range(end):
                changed=words[:];changed[i]=0
                with self.subTest(kind=kind,i=i),self.assertRaises(ValueError):recognize_reload(dol(changed),BASE)
    def test_wrong_guard_direction_target_cr_and_link_rejected(self):
        words,end,_=self.fixture();guard=2
        for w in (conditional(4*(end-guard),bo=4,bi=2),conditional(4*(end-guard-1),bo=12,bi=2),conditional(4*(end-guard),bo=12,bi=6),words[guard]|1,words[guard]|2):
            changed=words[:];changed[guard]=w
            with self.subTest(w=w),self.assertRaises(ValueError):recognize_reload(dol(changed),BASE)
    def test_mismatched_helpers_and_nontext_target_rejected(self):
        words,_,calls=self.fixture()
        for i,w in ((calls[-1],branch(4*(50-calls[-1]),link=True)),(calls[0],0x48004001),(calls[0],words[calls[0]]|2),(calls[0],words[calls[0]]&~1)):
            changed=words[:];changed[i]=w
            with self.subTest(i=i,w=w),self.assertRaises(ValueError):recognize_reload(dol(changed),BASE)
    def test_same_helper_pair_is_not_reviewed_family(self):
        words,_,calls=self.fixture()
        for i in (calls[1],calls[3]):words[i]=branch(4*(50-i),link=True)
        with self.assertRaises(ValueError):recognize_reload(dol(words),BASE)
    def test_reloaded_coordinate_register_and_target_bank_rejected(self):
        words,_,_=self.fixture()
        for i,w in ((7,0x818D5E38),(7,0x819F5E34),(7,0x800D5E34),(8,0x7D8903A6),(9,0x4E800020)):
            changed=words[:];changed[i]=w
            with self.subTest(i=i,w=w),self.assertRaises(ValueError):recognize_reload(dol(changed),BASE)
    def test_unreviewed_clobber_update_or_cr_prep_rejected(self):
        words,_,_=self.fixture(2)
        for w in (0x800D5E48,0x81AD5E48,0x840D5E48,0x28000001,0x48000001,0x60000000):
            changed=words[:];changed[1]=w
            with self.subTest(w=w),self.assertRaises(ValueError):recognize_reload(dol(changed),BASE)
    def test_intermediate_entry_is_not_guarded(self):
        words,_,_=self.fixture()
        with self.assertRaises(ValueError):recognize_reload(dol(words),BASE+28)
    def test_truncation_alignment_and_bool_entry_rejected(self):
        words,end,_=self.fixture()
        for raw,entry in ((dol(words[:end-1]),BASE),(dol(words),BASE+1),(dol(words),True)):
            with self.assertRaises(ValueError):recognize_reload(raw,entry)
    def test_full_scan_preserves_external_entry_hazard(self):
        words,end,_=self.fixture();words[49]=branch(4*(7-49))
        scan=scan_reloads(dol(words));self.assertEqual(scan['summary']['source_contracts'],1)
        incoming=scan['matches'][0]['literal_incoming_edges']
        self.assertEqual(incoming[0]['destination'],hex(BASE+28))
        self.assertEqual(incoming[0]['edges'][0]['site'],hex(BASE+196))
        self.assertFalse(scan['matches'][0]['scope']['external_entry_guard_domination_proven'])
    def test_pipeline_recognizes_interior_without_declaration_identity(self):
        words,end,_=self.fixture()
        report=build_pipeline(dol(words),csv_bytes([(BASE+28,'untrusted-interior',4)]))
        self.assertEqual(report['summary']['reviewed_reload_source_matches'],1)
        self.assertEqual(report['source_reload_matches']['matches'][0]['entry'],hex(BASE))
        self.assertFalse(report['scope']['native_retirement_proven'])
    def test_field_and_context_variants_retain_literal_differences(self):
        words,_,_=self.fixture();words[0]=0x800DFFF0;words[7]=0x818DFFF0
        for i in (3,5,10):words[i]=0x38610020
        c=recognize_reload(dol(words),BASE)
        self.assertEqual(c['field_coordinate']['displacement'],-16)
        self.assertEqual(c['original_words'][3]['word'],'0x38610020')
    def test_callback_argument_cannot_overwrite_target(self):
        words,_,_=self.fixture(1)
        words[10]=0x399F0000
        with self.assertRaises(ValueError):recognize_reload(dol(words),BASE)
    def test_no_runtime_fault_audio_or_retirement_claim(self):
        c=self.contract()
        for k in ('alias_context_or_lifetime_closed','execution_fault_state_or_audio_parity_proven','native_ownership_or_retirement_proven','deletion_authorized'):
            self.assertIs(c['scope'][k],False)


if __name__=='__main__':unittest.main()
