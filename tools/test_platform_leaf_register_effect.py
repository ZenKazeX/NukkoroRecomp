import unittest
from platform_leaf_register_effect import build_leaf_register_effect
from test_graphics_lift_inventory import BASE,NOP,BLR,branch,dol

class LeafRegisterEffectTests(unittest.TestCase):
    def test_explicit_write_set_not_callee_saved_abi(self):
        q=build_leaf_register_effect(dol([0x3bc00001,0x38000000,BLR]),BASE)
        self.assertEqual(q['written_gprs'],[0,30])
        self.assertNotIn(30,q['preserved_gprs'])
        self.assertTrue(q['complete_normal_flow_encoded_gpr_effects'])
        self.assertFalse(q['scope']['native_retirement_proven'])
    def test_branch_arms_union_and_loop_visited_once(self):
        q=build_leaf_register_effect(dol([0x4182000c,0x3ba00001,branch(8),0x3bc00002,BLR]),BASE)
        self.assertEqual(q['written_gprs'],[29,30])
    def test_returning_arm_with_loop_is_not_termination_proof(self):
        q=build_leaf_register_effect(dol([0x41820008,branch(-4),BLR]),BASE)
        self.assertTrue(q['complete_normal_flow_encoded_gpr_effects'])
        self.assertFalse(q['scope']['all_paths_return_or_fault_context_memory_cr_fpr_preservation_proven'])
    def test_load_update_and_multiword_write_sets(self):
        q=build_leaf_register_effect(dol([0x8c640001,0xbbc50000,BLR]),BASE)
        self.assertEqual(q['written_gprs'],[3,4,30,31])
    def test_call_rejects_preservation_even_after_return(self):
        q=build_leaf_register_effect(dol([branch(8,link=True),NOP,BLR]),BASE)
        self.assertFalse(q['complete_normal_flow_encoded_gpr_effects'])
        self.assertEqual(q['preserved_gprs'],[])
    def test_unknown_word_rejects_preservation(self):
        q=build_leaf_register_effect(dol([0,BLR]),BASE)
        self.assertEqual(q['classification'],'UNKNOWN')
        self.assertEqual(q['preserved_gprs'],[])
    def test_indirect_and_outside_or_nonreturning_paths_remain_unknown(self):
        for words in ([0x4e800420],[branch(0x1000)],[branch(0)]):
            q=build_leaf_register_effect(dol(words),BASE)
            self.assertFalse(q['complete_normal_flow_encoded_gpr_effects'])
    def test_normal_encoded_store_is_not_memory_alias_or_fault_proof(self):
        q=build_leaf_register_effect(dol([0x93c30000,BLR]),BASE)
        self.assertIn(30,q['preserved_gprs'])
        self.assertFalse(q['scope']['all_paths_return_or_fault_context_memory_cr_fpr_preservation_proven'])
    def test_bool_invalid_entry_rejected(self):
        with self.assertRaises(ValueError):build_leaf_register_effect(dol([BLR]),True)

if __name__=='__main__':unittest.main()
