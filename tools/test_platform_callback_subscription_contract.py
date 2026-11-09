import unittest
from test_graphics_lift_inventory import dol,BASE,branch,csv_bytes
from platform_callback_subscription_contract import recognize_subscription,scan_subscriptions
from shadow_semantic_patterns import build_pipeline


class SubscriptionTests(unittest.TestCase):
    def fixture(self):
        words=[0x7c0802a6,0x28030000,0x90010004,0x9421ffe8,0x93e10014,
            0x83ed5c50,0x906d5c50,0x41820014,0x3c608000,0x38631080,branch(4*(30-10),link=True),0x48000010,
            0x3c608000,0x38631080,branch(4*(31-14),link=True),0x7fe3fb78,0x8001001c,0x83e10014,0x38210018,0x7c0803a6,0x4e800020]
        return words+[0x60000000]*9+[0x4e800020]*3
    def test_input_test_old_read_and_publication_order(self):
        c=recognize_subscription(dol(self.fixture()),BASE)
        self.assertEqual(c['old_read_site'],hex(BASE+20));self.assertEqual(c['publication_site'],hex(BASE+24))
        self.assertIn('Original input',c['tested_value']);self.assertEqual(c['handler_address'],hex(BASE+128))
    def test_every_instruction_and_bit_mutation_refused(self):
        original=self.fixture()
        # Literal field/handler/service variations are reviewed parameter slots;
        # mutate every fixed bit and separately test the cross-slot invariants.
        for i in (0,1,2,3,4,7,11,15,16,17,18,19,20):
            for bit in range(32):
                words=original[:];words[i]^=1<<bit
                with self.subTest(i=i,bit=bit),self.assertRaises(ValueError):recognize_subscription(dol(words),BASE)
        for i in range(21):
            words=original[:];words[i]=0
            with self.subTest(i=i),self.assertRaises(ValueError):recognize_subscription(dol(words),BASE)
    def test_different_field_register_width_and_handler_rejected(self):
        for i,w in ((5,0x83ed5c54),(6,0x907f5c50),(6,0x986d5c50),(12,0x3c608001),(13,0x38631084)):
            words=self.fixture();words[i]=w
            with self.subTest(i=i),self.assertRaises(ValueError):recognize_subscription(dol(words),BASE)
    def test_services_must_be_distinct_original_relative_calls(self):
        words=self.fixture()
        for w in (branch(4*(30-14),link=True),branch(4*(31-14)),words[14]|2,0x48004001):
            changed=words[:];changed[14]=w
            with self.subTest(w=w),self.assertRaises(ValueError):recognize_subscription(dol(changed),BASE)
    def test_halfword_signed_handler_construction(self):
        words=self.fixture()
        for a,b in ((8,9),(12,13)):words[a]=0x3c608001;words[b]=0x38639080
        # 0x80010000 - 0x6f80 = 0x80009080, deliberately not file-backed.
        with self.assertRaises(ValueError):recognize_subscription(dol(words),BASE)
    def test_no_old_return_atomicity_native_or_audio_claim(self):
        c=recognize_subscription(dol(self.fixture()),BASE)
        for k in ('old_value_return_across_callees_proven','interrupt_exclusion_or_atomic_exchange_proven','live_subscription_or_audio_parity_proven','native_ownership_or_retirement_proven'):
            self.assertFalse(c['scope'][k])
    def test_scan_and_pipeline_ignore_declaration_filter(self):
        raw=dol(self.fixture());s=scan_subscriptions(raw)
        self.assertEqual(s['summary']['source_contracts'],1)
        p=build_pipeline(raw,csv_bytes([(BASE+80,'interior',4)]))
        self.assertEqual(p['summary']['reviewed_subscription_source_matches'],1)
    def test_alignment_truncation_and_wrong_entry_rejected(self):
        words=self.fixture()
        for raw,entry in ((dol(words[:20]),BASE),(dol(words),BASE+1),(dol(words),True),(dol(words),BASE+20)):
            with self.assertRaises(ValueError):recognize_subscription(raw,entry)


if __name__=='__main__':unittest.main()
