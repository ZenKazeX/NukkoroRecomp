import unittest
from test_graphics_lift_inventory import dol,BASE,BLR
from platform_bounded_switch_contract import recognize_switch


class BoundedSwitchTests(unittest.TestCase):
    SEQ=[0x28000002,0x41810020,0x3C608001,0x38638000,0x5400103A,0x7C03002E,0x7C0903A6,0x4E800420,BLR]
    TABLE=0x80008000
    def image(self,words=None,values=None,sections=None):
        return dol(address=BASE,sections=sections or [(0,BASE,words or self.SEQ),(7,self.TABLE,values or [BASE,BASE+32,BASE+1])])
    def test_bounded_data_section_target_bytes(self):
        c=recognize_switch(self.image(),BASE)
        self.assertEqual(c['admitted_unsigned_maximum'],2)
        self.assertEqual(c['original_table_address'],hex(self.TABLE))
        self.assertEqual(c['original_table_bytes'],12)
        self.assertEqual(c['original_entries'][2]['raw_target'],hex(BASE+1))
        self.assertEqual(c['original_entries'][2]['effective_target'],hex(BASE))
    def test_every_sequence_word_is_semantically_bound(self):
        mutations=[(0,0x2C000002),(1,0x41800020),(2,0x3C608002),(3,0x38637FFC),
                   (4,0x54001838),(5,0x7C03012E),(6,0x7C0803A6),(7,0x4E800421)]
        for i,w in mutations:
            words=self.SEQ[:];words[i]=w
            with self.subTest(i=i):self.assertIsNone(recognize_switch(self.image(words),BASE))
    def test_unsigned_width_and_cr_branch_binding(self):
        for i,w in ((0,0x28200002),(0,0x28400002),(1,0x41850020),(1,0x41810021),(1,0x41810018)):
            words=self.SEQ[:];words[i]=w
            self.assertIsNone(recognize_switch(self.image(words),BASE))
    def test_indirect_ctr_or_lr_condition_is_not_a_direct_guard(self):
        for word in (0x4D810420,0x4D810020):
            words=self.SEQ[:];words[1]=word
            self.assertIsNone(recognize_switch(self.image(words),BASE))
    def test_indirect_branch_cannot_crash_whole_image_discovery(self):
        words=self.SEQ[:];words[1]=0x4D810420
        from shadow_semantic_patterns import build_pipeline
        table=('addr_hex,name,size\n'+hex(BASE)+',indirect,36\n').encode()
        report=build_pipeline(self.image(words),table)
        self.assertEqual(report['source_switch_matches'],[])
    def test_table_or_scale_must_not_clobber_index_base(self):
        for i,w in ((2,0x3C008001),(2,0x3C608001),(4,0x5403103A)):
            words=self.SEQ[:];words[i]=w
            if i==2 and w==0x3C608001:words[0]=0x28030002
            self.assertIsNone(recognize_switch(self.image(words),BASE))
    def test_truncated_unbacked_and_wrap_tables_refused(self):
        self.assertIsNone(recognize_switch(self.image(values=[BASE]),BASE))
        words=self.SEQ[:];words[2]=0x3C60FFFF;words[3]=0x3863FFFC
        self.assertIsNone(recognize_switch(self.image(words),BASE))
    def test_table_content_change_changes_evidence(self):
        a=recognize_switch(self.image(),BASE)
        b=recognize_switch(self.image(values=[BASE,BASE,BASE]),BASE)
        self.assertNotEqual(a['original_table_sha256'],b['original_table_sha256'])
        self.assertNotEqual(a['inputs']['dol_sha256'],b['inputs']['dol_sha256'])
    def test_nontext_target_is_kept_not_admitted_as_callable(self):
        c=recognize_switch(self.image(values=[0,0xFFFFFFFF,BASE]),BASE)
        self.assertFalse(c['original_entries'][0]['original_text_target'])
        self.assertFalse(c['original_entries'][1]['original_text_target'])
        self.assertFalse(c['scope']['original_table_is_live_table_proven'])
        self.assertFalse(c['scope']['target_execution_or_native_retirement_proven'])
    def test_split_adjacent_storage_and_gap(self):
        c=recognize_switch(self.image(sections=[(0,BASE,self.SEQ),(7,self.TABLE,[BASE]),(8,self.TABLE+4,[BASE,BASE])]),BASE)
        self.assertIsNotNone(c)
        self.assertIsNone(recognize_switch(self.image(sections=[(0,BASE,self.SEQ),(7,self.TABLE,[BASE]),(8,self.TABLE+8,[BASE,BASE])]),BASE))


if __name__=='__main__':unittest.main()
