import unittest
from platform_callback_toggle_contract import recognize_toggle,PREFIX,DISABLE,RESTORE
from test_graphics_lift_inventory import dol,BASE,branch


def fixture():
    words=PREFIX[:];words[6]=branch(112-24,link=True);words[20]=branch(132-80,link=True)
    words[7]=0x800D5BB0;words[19]=0x900D5BB0;words[15]=0x3C808038;words[16]=0x3804DF10
    return words+DISABLE+RESTORE


class ToggleContractTests(unittest.TestCase):
    def test_complete_toggle_and_leaf_register_proofs(self):
        r=recognize_toggle(dol(fixture()),BASE)
        self.assertEqual(r['callback_literal'],'0x8037df10');self.assertEqual(r['field_displacement'],0x5BB0)
        self.assertEqual([p['gpr_writes'] for p in r['leaf_proofs']],[[3,4],[3,4,5]])
        self.assertTrue(r['scope']['callee_register_preservation_source_proven'])
        self.assertFalse(r['scope']['old_value_is_function_entry_value_proven'])
    def test_every_parent_word_is_bound(self):
        for i in range(28):
            w=fixture();w[i]=0x60000000
            with self.subTest(i=i),self.assertRaises(ValueError):recognize_toggle(dol(w),BASE)
    def test_every_callee_word_is_bound(self):
        for i in range(28,42):
            w=fixture();w[i]=0x60000000
            with self.subTest(i=i),self.assertRaises(ValueError):recognize_toggle(dol(w),BASE)
    def test_field_mismatch_and_wrong_base_rejected(self):
        for at,v in ((7,0x800C5BB0),(19,0x900D5BB4),(19,0x906D5BB0)):
            w=fixture();w[at]=v
            with self.subTest(at=at),self.assertRaises(ValueError):recognize_toggle(dol(w),BASE)
    def test_signed_field_displacement(self):
        w=fixture();w[7]=0x800DFFF0;w[19]=0x900DFFF0
        self.assertEqual(recognize_toggle(dol(w),BASE)['field_displacement'],-16)
    def test_high_low_construction_wrap(self):
        w=fixture();w[15]=0x3C808000;w[16]=0x3804FFFF
        self.assertEqual(recognize_toggle(dol(w),BASE)['callback_literal'],'0x7fffffff')
    def test_noncall_or_unbacked_leaf_rejected(self):
        for v in (branch(112-24),branch(400,link=True)):
            w=fixture();w[6]=v
            with self.subTest(v=v),self.assertRaises(ValueError):recognize_toggle(dol(w),BASE)
    def test_entry_alignment_and_type(self):
        for entry in (True,BASE+1,BASE+4):
            with self.subTest(entry=entry),self.assertRaises(ValueError):recognize_toggle(dol(fixture()),entry)


if __name__=='__main__':unittest.main()
