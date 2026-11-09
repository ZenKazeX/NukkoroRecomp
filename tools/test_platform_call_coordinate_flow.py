import unittest
from test_graphics_lift_inventory import dol,BASE,BLR,branch,conditional
from platform_call_coordinate_flow import build_call_coordinates


class CallCoordinateTests(unittest.TestCase):
    def report(self,words,register=3):
        target=BASE+len(words)*4
        return build_call_coordinates(dol([*words,BLR]),[target],register)
    def call(self,words,register=3):return self.report(words,register)['calls'][0]['coordinate']
    def test_member_coordinate_and_copy_bind_original_definitions(self):
        c=self.call([0x7C7D1B78,0x387D0008,branch(4,link=True)])
        self.assertEqual([n['kind'] for n in c['nodes']],['current_gpr_at_boundary','copy','add_offset_u32'])
        self.assertEqual(c['nodes'][0]['gpr'],3);self.assertEqual(c['nodes'][-1]['offset'],8)
        self.assertEqual(c['nodes'][-1]['site'],hex(BASE+4))
    def test_calls_and_branches_sever_earlier_member_identity(self):
        for barrier in (branch(32,link=True),conditional(4),branch(4)):
            c=self.call([0x7C7D1B78,barrier,0x387D0008,branch(4,link=True)])
            self.assertEqual(c['nodes'][0]['gpr'],29)
            self.assertEqual(c['nodes'][0]['boundary'],hex(BASE+8))
            self.assertNotIn('copy',[n['kind'] for n in c['nodes']])
    def test_direct_entry_interior_resets_even_on_linear_fallthrough(self):
        c=self.call([conditional(12),0x7C7D1B78,0x60000000,0x387D0008,branch(4,link=True)])
        self.assertEqual(c['nodes'][0]['boundary_reason'],'direct_destination')
        self.assertEqual(c['nodes'][0]['gpr'],29)
    def test_unknown_and_invalid_spr_are_full_barriers(self):
        for unknown in (0,0x7C0803A7):
            c=self.call([0x7C7D1B78,unknown,0x387D0008,branch(4,link=True)])
            self.assertEqual(c['nodes'][0]['boundary_reason'],'unsupported')
            self.assertEqual(c['nodes'][0]['gpr'],29)
    def test_signed_offset_zero_base_and_aliasing_are_explicit(self):
        c=self.call([0x8060FFFC,branch(4,link=True)])
        self.assertEqual(len(c['nodes']),1);n=c['nodes'][0]
        self.assertEqual((n['displacement'],n['encoded_zero_base']),(-4,True))
        self.assertFalse(n['contents_or_alias_or_success_proven'])
        c=self.call([0x3864FFFF,branch(4,link=True)])
        self.assertEqual(c['nodes'][-1]['offset'],-1)
    def test_nested_loads_are_reads_not_invented_memory(self):
        c=self.call([0x806D4588,0x80630008,branch(4,link=True)])
        self.assertEqual([n.get('displacement') for n in c['nodes'][1:]],[0x4588,8])
        self.assertFalse(any(n.get('contents_or_alias_or_success_proven') for n in c['nodes']))
    def test_dag_copies_never_recursively_expand_or_claim_execution(self):
        r=self.report([0x7C641B78,0x7C832378,0x7C641B78,0x7C832378,branch(4,link=True)])
        c=r['calls'][0]['coordinate'];self.assertEqual(len(c['nodes']),5)
        self.assertEqual(c['root'],4);self.assertFalse(r['scope']['native_ownership_or_retirement_proven'])
    def test_clobbered_destinations_remain_unresolved_not_preserved(self):
        c=self.call([0x7C7D1B78,0x7FA42850,0x387D0008,branch(4,link=True)])
        self.assertEqual(c['nodes'][0]['kind'],'unresolved_definition')
        self.assertEqual(c['nodes'][0]['gpr'],29)
    def test_literal_ori_r0_and_nonwriting_store_semantics(self):
        c=self.call([0x3C008058,0x60004964,0x7C030378,0x90610008,branch(4,link=True)])
        self.assertEqual(c['nodes'][0]['value'],'0x80580000')
        self.assertEqual(c['nodes'][1]['bits'],'0x00004964')
        self.assertEqual(c['nodes'][-1]['kind'],'copy')
    def test_no_matches_is_not_deadness_and_queries_are_strict(self):
        raw=dol([BLR]);r=build_call_coordinates(raw,[BASE]);self.assertEqual(r['calls'],[])
        self.assertFalse(r['scope']['deletion_authorized'])
        for targets,reg in (([],3),([BASE,BASE],3),([BASE+1],3),([BASE+4],3),([True],3),([BASE],True),([BASE],32)):
            with self.assertRaises(ValueError):build_call_coordinates(raw,targets,reg)


if __name__=='__main__':unittest.main()
