import unittest
from test_graphics_lift_inventory import dol,BASE,BLR,branch
from platform_callback_setter_contract import recognize_setter
from platform_callback_toggle_contract import DISABLE,RESTORE


class CallbackSetterTests(unittest.TestCase):
    def words(self):return [0x7C0802A6,0x90010004,0x9421FFE8,0x93E10014,0x93C10010,
        0x7C7E1B78,0x83ED5BDC,branch(40,link=True),0x93CD5BDC,branch(52,link=True),
        0x7FE3FB78,0x8001001C,0x83E10014,0x83C10010,0x38210018,0x7C0803A6,BLR,*DISABLE,*RESTORE]
    def test_old_read_precedes_exclusion_and_original_value_returned(self):
        r=recognize_setter(dol(self.words()),BASE)
        self.assertTrue(r['old_read_precedes_exclusion'])
        self.assertFalse(r['atomic_exchange_equivalence_proven'])
        self.assertEqual(r['field_coordinate']['displacement'],0x5BDC)
        self.assertEqual([l['normal_path_gpr_writes'] for l in r['source_leaf_preservation']],[[3,4],[3,4,5]])
        self.assertFalse(r['scope']['native_ownership_or_retirement_proven'])
    def test_signed_field_and_other_slot_preserve_encoded_coordinates(self):
        for offset in (0x5BE0,0xFFF0):
            words=self.words();words[6]=0x83ED0000|offset;words[8]=0x93CD0000|offset
            self.assertEqual(recognize_setter(dol(words),BASE)['field_coordinate']['displacement'],offset if offset<32768 else offset-65536)
    def test_each_fixed_word_is_bound(self):
        for index in (0,1,2,3,4,5,10,11,12,13,14,15,16):
            words=self.words();words[index]^=4
            with self.assertRaises(ValueError):recognize_setter(dol(words),BASE)
    def test_mismatched_base_source_width_or_field_refused(self):
        for index,word in ((6,0x83EC5BDC),(6,0x83CD5BDC),(6,0xA3ED5BDC),(8,0x93ED5BDC),(8,0x93CD5BE0)):
            words=self.words();words[index]=word
            with self.assertRaises(ValueError):recognize_setter(dol(words),BASE)
    def test_call_link_target_and_leaf_words_are_bound(self):
        for index,word in ((7,branch(40)),(7,branch(44,link=True)),(9,branch(48,link=True)),(17,0x7C6000A7),(22,0x2C030001)):
            words=self.words();words[index]=word
            with self.assertRaises(ValueError):recognize_setter(dol(words),BASE)
    def test_no_guessed_callee_saved_abi(self):
        words=self.words();words[18]=0x7C7F1B78
        with self.assertRaises(ValueError):recognize_setter(dol(words),BASE)
    def test_truncation_bool_and_unaligned_entries_refused(self):
        for raw,entry in ((dol(self.words()[:16]),BASE),(dol(self.words()),BASE+1),(dol(self.words()),True),(dol(self.words()[:-1]),BASE)):
            with self.assertRaises(ValueError):recognize_setter(raw,entry)
    def test_source_match_after_return_is_not_execution(self):
        raw=dol([BLR,*self.words()]);r=recognize_setter(raw,BASE+4)
        self.assertFalse(r['scope']['actual_publication_return_or_lifetime_proven'])


if __name__=='__main__':unittest.main()
