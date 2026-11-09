import unittest

from format_memory_dependencies import memory_effect
from gqr_format_dataflow import UNKNOWN, build_format_contract, constant
from test_graphics_lift_inventory import BASE, BLR, branch, conditional, dol
from test_platform_effect_inventory import spr
from test_gqr_format_dataflow import li, psq


def dword(op,reg=3,base=9,disp=0):
    return (op<<26)|(reg<<21)|(base<<16)|(disp&65535)


def initial():
    return (tuple([UNKNOWN]*40),tuple(frozenset({('entry',BASE)}) for _ in range(8)),
            tuple(frozenset({('entry',BASE,r)}) for r in range(32)))


class MemoryDependencyTests(unittest.TestCase):
    def report(self,words):return build_format_contract(dol(words),BASE)
    def dependencies(self,words):return self.report(words)['writer_memory_dependencies'][0]['reads']

    def test_original_snapshot_hazard_has_exact_store_and_read(self):
        # The actual source witness is a separate original-CPU executable.
        rows=self.dependencies([dword(36,0,1,0x28),dword(34,3,9,12),spr(912,467),BLR])
        self.assertEqual(len(rows),1)
        self.assertEqual(rows[0]['read']['site'],hex(BASE+4))
        self.assertEqual(rows[0]['may_precede_stores'][0]['site'],hex(BASE))
        self.assertFalse(rows[0]['read_from_or_snapshot_safety_proven'])

    def test_different_base_registers_never_prove_disjointness(self):
        row=self.dependencies([dword(36,0,1,0x28),dword(34,3,9,12),spr(912,467),BLR])[0]
        self.assertIn('unresolved',row['alias_classification'])
        self.assertFalse(row['read']['physical_alias_or_resource_lifetime_proven'])

    def test_mutually_exclusive_store_not_predecessor_of_read(self):
        rows=self.dependencies([conditional(12),dword(36,0,1,0x28),branch(8),dword(34,3,9,12),spr(912,467),BLR])
        self.assertEqual(rows[0]['may_precede_stores'],[])

    def test_later_store_can_precede_read_through_loop(self):
        rows=self.dependencies([dword(34,3,9,12),spr(912,467),dword(36,0,1,0x28),conditional(-12),BLR])
        self.assertEqual([e['site'] for e in rows[0]['may_precede_stores']],[hex(BASE+8)])

    def test_call_is_explicit_predecessor_barrier(self):
        rows=self.dependencies([branch(16,link=True),dword(34,3,9,12),spr(912,467),BLR,BLR])
        self.assertEqual(rows[0]['may_precede_barriers'][0]['kind'],'call')
        self.assertEqual(rows[0]['read']['address_sources'][0]['reaching_definitions'][0]['kind'],'call')

    def test_unknown_effect_is_not_pure_memory(self):
        rows=self.dependencies([0,dword(34,3,9,12),spr(912,467),BLR])
        self.assertEqual(rows[0]['may_precede_barriers'][0]['kind'],'unsupported')

    def test_no_store_forwarding_even_same_encoded_address(self):
        c=self.report([li(3,4),dword(36,3,9),dword(32,3,9),spr(912,467),BLR])
        self.assertEqual(c['writers'][0]['input_known_mask'],'0x00000000')
        self.assertFalse(c['writer_memory_dependencies'][0]['reads'][0]['read_from_or_snapshot_safety_proven'])

    def test_pre_instruction_address_for_load_that_overwrites_base(self):
        c=self.report([li(9,16),dword(32,9,9),spr(912,467,9),BLR])
        e=c['memory_effects'][0]
        self.assertEqual(e['address_sources'][0]['reaching_definitions'],[dict(kind='definition',site=hex(BASE),gpr=9)])

    def test_all_d_integer_widths_and_sign_extended_displacements(self):
        for op,width in {32:4,33:4,34:1,35:1,36:4,37:4,38:1,39:1,40:2,41:2,42:2,43:2,44:2,45:2}.items():
            e=memory_effect(dword(op,disp=-4),initial(),BASE)
            self.assertEqual(e['request_bytes'],width);self.assertEqual(e['displacement'],-4)
            self.assertFalse(e['successful_effect_proven'])

    def test_multiple_register_widths_are_encoded_demand_only(self):
        for op in (46,47):
            for reg in (0,1,31):
                e=memory_effect(dword(op,reg),initial(),BASE)
                self.assertEqual(e['request_bytes'],4*(32-reg))
                self.assertEqual(e['value_registers'],list(range(reg,32)))

    def test_indexed_integer_widths_update_and_two_address_operands(self):
        widths={23:4,55:4,87:1,119:1,151:4,183:4,215:1,247:1,279:2,311:2,343:2,375:2,407:2,439:2}
        for xo,width in widths.items():
            e=memory_effect((31<<26)|(3<<21)|(9<<16)|(5<<11)|(xo<<1),initial(),BASE)
            self.assertEqual(e['request_bytes'],width)
            self.assertEqual([d['gpr'] for d in e['address_sources']],[5,9])
            self.assertEqual(e['updates_base'],xo in (55,119,183,247,311,375,439))

    def test_zero_base_is_constant_zero_only_in_encoded_address(self):
        e=memory_effect(dword(34,base=0),initial(),BASE)
        self.assertTrue(e['zero_base']);self.assertEqual(e['address_sources'],[])
        e=memory_effect(dword(35,base=0),initial(),BASE)
        self.assertTrue(e['zero_base_update_encoding'])
        self.assertFalse(e['zero_base']);self.assertEqual(e['address_sources'][0]['gpr'],0)
        self.assertFalse(e['successful_effect_proven'])

    def test_quantized_zero_update_encoding_does_not_invent_exception_or_gpr_update(self):
        for op in (57,61):
            e=memory_effect(op<<26,initial(),BASE)
            self.assertTrue(e['zero_base']);self.assertTrue(e['encoded_update_form'])
            self.assertTrue(e['zero_base_update_encoding']);self.assertFalse(e['updates_base'])
            self.assertEqual(e['address_sources'],[])
            self.assertFalse(e['zero_update_exception_behavior_proven'])

    def test_float_widths(self):
        for op in range(48,56):
            e=memory_effect(dword(op),initial(),BASE)
            self.assertEqual(e['request_bytes'],4 if op in (48,49,52,53) else 8)
            self.assertEqual(e['value_domain'],'fpr')

    def test_quantized_unknown_width_not_assumed_float(self):
        self.assertIsNone(memory_effect(psq(),initial(),BASE)['request_bytes'])

    def test_quantized_known_type_width_all_forms(self):
        for kind,width in ((0,4),(4,1),(5,2),(6,1),(7,2)):
            state=initial();values=list(state[0]);values[32]=constant(kind|(kind<<16));state=(tuple(values),*state[1:])
            for store in (False,True):
                for indexed in (False,True):
                    for update in (False,True):
                        for one in (False,True):
                            e=memory_effect(psq(store=store,indexed=indexed,update=update,one=one),state,BASE)
                            self.assertEqual(e['request_bytes'],width*(1 if one else 2))
                            self.assertEqual(e['updates_base'],update)
                            self.assertFalse(e['reserved_format'])

    def test_reserved_format_width_refused(self):
        state=initial();values=list(state[0]);values[32]=constant(3<<16);state=(tuple(values),*state[1:])
        e=memory_effect(psq(),state,BASE)
        self.assertTrue(e['reserved_format']);self.assertIsNone(e['request_bytes'])

    def test_unsupported_effect_does_not_fabricate_memory_access(self):
        self.assertIsNone(memory_effect(0,initial(),BASE))
        self.assertIsNone(memory_effect(BLR,initial(),BASE))


if __name__=='__main__':unittest.main()
