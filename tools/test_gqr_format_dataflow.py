import contextlib
import hashlib
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from gqr_format_dataflow import build_format_contract, constant, main, meet, native_format, writer_slices
from test_graphics_lift_inventory import BASE, BLR, NOP, branch, conditional, dol
from test_platform_effect_inventory import spr


def li(register, value):return (14 << 26) | (register << 21) | (value & 65535)
def lis(register, value):return (15 << 26) | (register << 21) | (value & 65535)
def psq(index=0,store=False,indexed=False,one=False,update=False):
    if indexed:
        xo=(39 if store else 38) if update else (7 if store else 6)
        return (4<<26)|(8<<21)|(4<<16)|(5<<11)|(int(one)<<10)|(index<<7)|(xo<<1)
    op=(61 if store else 57) if update else (60 if store else 56)
    return (op<<26)|(8<<21)|(4<<16)|(int(one)<<15)|(index<<12)


class FormatDataflowTests(unittest.TestCase):
    def build(self,words):return build_format_contract(dol(words),BASE)
    def set_format(self,value=4,index=0):
        return [lis(3,value>>16),(24<<26)|(3<<21)|(3<<16)|(value&65535),spr(912+index,467)]

    def test_cntlzw_zero_top_bit_and_record_forms(self):
        for value in (0,1,0x80,0x80000000,0xffffffff):
            for rc in (0,1):
                count=(31<<26)|(4<<21)|(3<<16)|(26<<1)|rc
                c=self.build([lis(4,value>>16),(24<<26)|(4<<21)|(4<<16)|(value&65535),count,spr(912,467),BLR])
                self.assertEqual(c['writers'][0]['input_known_mask'],'0xffffffff')
                self.assertEqual(int(c['writers'][0]['input_known_bits'],16),32-value.bit_length())
                self.assertEqual(c['summary']['barriers'].get('unsupported',0),0)
    def test_cntlzw_preserves_unwritten_registers_and_format_producers(self):
        count=(31<<26)|(4<<21)|(8<<16)|(26<<1)
        c=self.build([*self.set_format(),li(4,1),count,psq(store=True),BLR])
        self.assertEqual(c['consumers'][0]['candidate_format']['type_code'],4)
        self.assertEqual(c['consumers'][0]['candidate_format']['element_type'],'uint8')
        self.assertEqual(c['summary']['barriers'].get('unsupported',0),0)
    def test_cntlzw_unknown_dependency_and_reserved_rb(self):
        count=(31<<26)|(4<<21)|(3<<16)|(26<<1)
        c=self.build([count,spr(912,467),BLR])
        self.assertEqual(c['writers'][0]['input_known_mask'],'0x00000000')
        self.assertIn(dict(kind='entry',site=hex(BASE),gpr=4),c['writer_source_slices'][0]['unresolved_leaves'])
        bad=count|(5<<11)
        self.assertEqual(self.build([bad,spr(912,467),BLR])['summary']['barriers']['unsupported'],1)

    def test_call_register_entry_has_all_unknown_inputs_without_abi(self):
        c=self.build([branch(8,link=True),BLR,BLR])['call_register_inputs'][0]
        self.assertEqual([r['gpr'] for r in c['registers']],list(range(32)))
        for r in c['registers']:
            self.assertEqual(r['known_mask'],'0x00000000')
            self.assertEqual(r['source_definition_roots'],[dict(kind='entry',site=hex(BASE),gpr=r['gpr'])])
        self.assertFalse(c['argument_ownership_or_dynamic_execution_proven'])

    def test_call_register_construction_with_changed_destination(self):
        addi=(14<<26)|(4<<21)|(3<<16)|0x6938
        c=self.build([lis(3,0x8037),addi,li(3,4),branch(8,link=True),BLR,BLR])['call_register_inputs'][0]
        self.assertEqual(c['registers'][3]['known_bits'],'0x00000004')
        r=c['registers'][4]
        self.assertEqual((r['known_mask'],r['known_bits']),('0xffffffff','0x80376938'))
        self.assertEqual(r['source_definition_roots'],[dict(kind='definition',site=hex(BASE+4),gpr=4)])

    def test_call_register_meet_preserves_both_producers(self):
        c=self.build([conditional(12),li(4,4),branch(8),li(4,6),branch(8,link=True),BLR,BLR])['call_register_inputs'][0]['registers'][4]
        self.assertEqual((c['known_mask'],c['known_bits']),('0xfffffffd','0x00000004'))
        self.assertEqual([r['site'] for r in c['source_definition_roots']],[hex(BASE+4),hex(BASE+12)])

    def test_call_register_prior_call_invalidates_even_nonvolatile_candidates(self):
        rows=self.build([li(31,7),branch(20,link=True),li(4,9),branch(12,link=True),BLR,NOP,BLR])['call_register_inputs']
        r=rows[1]['registers'][31]
        self.assertEqual(r['known_mask'],'0x00000000')
        self.assertEqual(r['source_definition_roots'],[dict(kind='call',site=hex(BASE+4),gpr=31)])
        self.assertEqual(rows[1]['registers'][4]['known_bits'],'0x00000009')

    def test_call_register_memory_source_is_not_a_loaded_constant(self):
        lwz=(32<<26)|(4<<21)|(3<<16)|4
        c=self.build([li(3,32),lwz,branch(8,link=True),BLR,BLR])
        r=c['call_register_inputs'][0]['registers'][4]
        self.assertEqual(r['known_mask'],'0x00000000')
        self.assertEqual(r['source_definition_roots'],[dict(kind='definition',site=hex(BASE+4),gpr=4)])
        self.assertFalse(c['gpr_definition_nodes'][1]['loaded_memory_value_proven'])

    def test_call_register_indirect_target_remains_unknown(self):
        row=self.build([li(4,7),0x4e800421,BLR])['call_register_inputs'][0]
        self.assertIsNone(row['target'])
        self.assertEqual(row['registers'][4]['known_bits'],'0x00000007')
        self.assertFalse(row['argument_ownership_or_dynamic_execution_proven'])

    def test_entry_state_not_supplied(self):
        c=self.build([psq(),BLR])['consumers'][0]
        self.assertFalse(c['candidate_format']['resolved'])
        self.assertEqual(c['reaching_definitions'],[dict(kind='entry',site=hex(BASE))])

    def test_all_signed_scales_and_integer_types(self):
        for kind in (4,5,6,7):
            for scale in range(-32,32):
                bits=kind|((scale&63)<<8)
                for store in (False,True):
                    value=bits if store else bits<<16
                    c=self.build(self.set_format(value)+[psq(store=store),BLR])['consumers'][0]
                    self.assertEqual(c['candidate_format']['signed_scale'],scale)
                    self.assertEqual(c['candidate_format']['type_code'],kind)

    def test_load_and_store_fields_are_separate(self):
        c=self.build(self.set_format(0x3f070604)+[psq(),psq(store=True),BLR])['consumers']
        self.assertEqual([x['candidate_format']['element_type'] for x in c],['int16','uint8'])
        self.assertEqual([x['candidate_format']['signed_scale'] for x in c],[-1,6])

    def test_all_indexed_immediate_update_and_one_forms(self):
        for index in range(8):
            for indexed in (False,True):
                for update in (False,True):
                    for store in (False,True):
                        for one in (False,True):
                            c=self.build(self.set_format(0x040004,index)+[psq(index,store,indexed,one,update),BLR])['consumers'][0]
                            self.assertEqual(c['spr'],912+index)
                            self.assertEqual(c['candidate_format']['request_bytes'],1 if one else 2)

    def test_reserved_types_refuse_instead_of_float_fallback(self):
        for kind in (1,2,3):
            x=native_format(constant(kind),True,2)
            self.assertFalse(x['resolved']);self.assertIn('reserved',x['reason'])

    def test_float_scale_does_not_require_an_invented_value(self):
        x=native_format((7,0),True,1)
        self.assertTrue(x['resolved']);self.assertTrue(x['scale_ignored'])
        self.assertIsNone(x['signed_scale'])

    def test_partial_integer_scale_refuses(self):
        self.assertFalse(native_format((7,4),True,1)['resolved'])

    def test_equal_branch_values_keep_both_producer_sites(self):
        words=[conditional(16),li(3,4),spr(912,467),branch(12),li(3,4),spr(912,467),psq(store=True),BLR]
        c=self.build(words)['consumers'][0]
        self.assertTrue(c['candidate_format']['resolved'])
        self.assertEqual(c['reaching_definitions'],[dict(kind='write',site=hex(BASE+8)),dict(kind='write',site=hex(BASE+20))])

    def test_conflicting_branch_values_do_not_resolve(self):
        words=[conditional(16),li(3,4),spr(912,467),branch(12),li(3,6),spr(912,467),psq(store=True),BLR]
        self.assertFalse(self.build(words)['consumers'][0]['candidate_format']['resolved'])

    def test_one_path_without_definition_keeps_entry_unknown(self):
        c=self.build([conditional(12),li(3,4),spr(912,467),psq(store=True),BLR])['consumers'][0]
        self.assertFalse(c['candidate_format']['resolved'])
        self.assertIn(dict(kind='entry',site=hex(BASE)),c['reaching_definitions'])

    def test_unused_bits_can_conflict_without_breaking_store_format(self):
        words=[conditional(16),li(3,4),spr(912,467),branch(12),li(3,0x4004),spr(912,467),psq(store=True),BLR]
        self.assertTrue(self.build(words)['consumers'][0]['candidate_format']['resolved'])

    def test_direct_call_invalidates_all_formats_without_abi_guess(self):
        words=self.set_format(4)+[branch(12,link=True),psq(store=True),BLR,BLR]
        report=self.build(words);c=report['consumers'][0]
        self.assertFalse(c['candidate_format']['resolved'])
        self.assertEqual(c['reaching_definitions'],[dict(kind='call',site=hex(BASE+12))])

    def test_conditional_call_also_invalidates(self):
        c=self.build(self.set_format(4)+[conditional(12,link=True),psq(store=True),BLR,BLR])['consumers'][0]
        self.assertFalse(c['candidate_format']['resolved'])

    def test_indirect_call_invalidates(self):
        c=self.build(self.set_format(4)+[0x4e800421,psq(store=True),BLR])['consumers'][0]
        self.assertFalse(c['candidate_format']['resolved'])

    def test_unknown_instruction_is_an_explicit_barrier(self):
        # 0xffffffff is a valid group63 fused arithmetic encoding, not unknown.
        report=self.build(self.set_format(4)+[0x00000000,psq(store=True),BLR])
        self.assertFalse(report['consumers'][0]['candidate_format']['resolved'])
        self.assertEqual(report['summary']['barriers']['unsupported'],1)

    def test_memory_load_into_producer_is_unknown(self):
        report=self.build([0x80640000,spr(912,467),psq(store=True),BLR])
        self.assertFalse(report['consumers'][0]['candidate_format']['resolved'])

    def test_updated_integer_base_not_old_constant(self):
        report=self.build([li(3,4),(37<<26)|(3<<16),spr(912,467),psq(store=True),BLR])
        self.assertFalse(report['consumers'][0]['candidate_format']['resolved'])

    def test_updated_float_base_not_old_constant(self):
        report=self.build([li(3,4),(49<<26)|(3<<16),spr(912,467),psq(store=True),BLR])
        self.assertFalse(report['consumers'][0]['candidate_format']['resolved'])

    def test_mfspr_roundtrip_tracks_actual_definition(self):
        c=self.build(self.set_format(4)+[spr(912,339,7),spr(913,467,7),psq(1,store=True),BLR])['consumers'][0]
        self.assertTrue(c['candidate_format']['resolved'])
        self.assertEqual(c['spr'],913)

    def test_entry_mfspr_roundtrip_does_not_manufacture_constant(self):
        c=self.build([spr(912,339,7),spr(913,467,7),psq(1,store=True),BLR])['consumers'][0]
        self.assertFalse(c['candidate_format']['resolved'])

    def test_mr_propagates_bit_constants(self):
        mr=(31<<26)|(3<<21)|(7<<16)|(3<<11)|(444<<1)
        c=self.build([li(3,4),mr,spr(912,467,7),psq(store=True),BLR])['consumers'][0]
        self.assertTrue(c['candidate_format']['resolved'])

    def test_loop_back_edge_conflict_is_not_first_iteration_constant(self):
        words=[li(3,4),spr(912,467),psq(store=True),li(3,6),spr(912,467),conditional(-12),BLR]
        c=self.build(words)['consumers'][0]
        self.assertFalse(c['candidate_format']['resolved'])
        self.assertEqual(len(c['reaching_definitions']),2)

    def test_self_loop_fixed_point_terminates(self):
        report=self.build(self.set_format(4)+[psq(store=True),conditional(-4),BLR])
        self.assertTrue(report['consumers'][0]['candidate_format']['resolved'])
        self.assertLess(report['summary']['dataflow_updates'],100)

    def test_zero_consumers_does_not_authorize_deletion(self):
        report=self.build([BLR]);self.assertEqual(report['summary']['quantized_consumers'],0)
        self.assertFalse(report['scope']['deletion_authorized'])
        self.assertFalse(report['scope']['shipping_consumer_zero_proven'])

    def test_rlwimi_simultaneously_reads_old_source_and_destination(self):
        insert=(20<<26)|(3<<21)|(3<<16)|(16<<11)|(0<<6)|(15<<1)
        c=self.build([li(3,0x604),insert,spr(912,467),psq(),BLR])
        self.assertEqual(c['consumers'][0]['candidate_format']['signed_scale'],6)
        node=next(n for n in c['gpr_definition_nodes'] if n['site']==hex(BASE+4))
        self.assertEqual(node['dependencies'][0]['reaching_definitions'],[dict(kind='definition',site=hex(BASE),gpr=3)])

    def test_integer_add_keeps_format_definition(self):
        add=(31<<26)|(7<<21)|(8<<16)|(9<<11)|(266<<1)
        c=self.build(self.set_format(4)+[add,psq(store=True),BLR])
        self.assertTrue(c['consumers'][0]['candidate_format']['resolved'])
        self.assertEqual(c['summary']['barriers'],{})

    def test_memory_address_dependencies_preserved_without_loaded_value(self):
        c=self.build([li(4,0x1234),0x88640000,spr(912,467),BLR])
        sl=c['writer_source_slices'][0]
        self.assertEqual([n['site'] for n in sl['nodes']],[hex(BASE),hex(BASE+4)])
        self.assertFalse(sl['memory_read_from_and_lifetime_proven'])
        self.assertEqual(c['writers'][0]['input_known_mask'],'0xffffff00')

    def test_mullw_low_word_wrap_and_all_status_forms(self):
        for left,right in ((-1,4),(0x80000000,2),(0x7fffffff,0x7fffffff),(0,7)):
            for oe in (0,1):
                for rc in (0,1):
                    setup=[]
                    for r,v in ((4,left),(5,right)):
                        setup.extend([lis(r,(v&0xffffffff)>>16),(24<<26)|(r<<21)|(r<<16)|(v&65535)])
                    mul=(31<<26)|(3<<21)|(4<<16)|(5<<11)|(235<<1)|(oe<<10)|rc
                    c=self.build(setup+[mul,spr(912,467),BLR])
                    self.assertEqual(int(c['writers'][0]['input_known_bits'],16),(left*right)&0xffffffff)
                    self.assertEqual(c['summary']['barriers'],{})
                    node=c['gpr_definition_nodes'][-1]
                    self.assertEqual([d['source_gpr'] for d in node['dependencies']],[4,5])

    def test_negation_wrap_and_all_status_forms(self):
        for value in (0,1,-1,0x80000000,0x7fffffff):
            for oe in (0,1):
                for rc in (0,1):
                    neg=(31<<26)|(3<<21)|(4<<16)|(104<<1)|(oe<<10)|rc
                    c=self.build([lis(4,(value&0xffffffff)>>16),(24<<26)|(4<<21)|(4<<16)|(value&65535),neg,spr(912,467),BLR])
                    self.assertEqual(int(c['writers'][0]['input_known_bits'],16),(-value)&0xffffffff)
                    self.assertEqual(c['summary']['barriers'],{})

    def test_mullw_negation_unknown_inputs_not_invented(self):
        for word in ((31<<26)|(3<<21)|(4<<16)|(5<<11)|(235<<1),
                     (31<<26)|(3<<21)|(4<<16)|(104<<1)):
            c=self.build([word,spr(912,467),BLR])
            self.assertEqual(c['writers'][0]['input_known_mask'],'0x00000000')
            self.assertEqual(c['summary']['barriers'],{})
            self.assertEqual(c['writer_source_slices'][0]['unresolved_leaves'][0]['kind'],'entry')

    def test_reserved_negation_register_field_remains_barrier(self):
        bad=(31<<26)|(3<<21)|(4<<16)|(5<<11)|(104<<1)
        c=self.build([bad,spr(912,467),BLR])
        self.assertEqual(c['summary']['barriers'],{'unsupported':1})

    def test_pre_call_formats_exposed_but_not_preserved_after_call(self):
        c=self.build(self.set_format(4)+[branch(12,link=True),psq(store=True),BLR,BLR])
        edge=c['call_format_inputs'][0]
        self.assertEqual(edge['target'],hex(BASE+24))
        self.assertEqual(edge['formats'][0]['known_bits'],'0x00000004')
        self.assertFalse(edge['dynamic_execution_or_callee_preservation_proven'])
        self.assertFalse(c['consumers'][0]['candidate_format']['resolved'])

    def test_indirect_call_target_never_invented(self):
        c=self.build(self.set_format(4)+[0x4e800421,BLR])
        self.assertIsNone(c['call_format_inputs'][0]['target'])

    def test_conflicting_branch_format_is_unknown_at_call(self):
        words=[conditional(16),li(3,4),spr(912,467),branch(12),li(3,6),spr(912,467),branch(8,link=True),BLR,BLR]
        edge=self.build(words)['call_format_inputs'][0]
        self.assertNotEqual(edge['formats'][0]['known_mask'],'0xffffffff')
        self.assertEqual(len(edge['formats'][0]['reaching_definitions']),2)

    def test_indexed_unsigned_loads_have_known_zero_extension_only(self):
        for xo,mask in ((23,0),(55,0),(87,0xffffff00),(119,0xffffff00),(279,0xffff0000),(311,0xffff0000),(343,0),(375,0)):
            word=(31<<26)|(3<<21)|(4<<16)|(5<<11)|(xo<<1)
            c=self.build([word,spr(912,467),BLR])
            self.assertEqual(int(c['writers'][0]['input_known_mask'],16),mask)
            self.assertEqual(c['writers'][0]['input_known_bits'],'0x00000000')
            self.assertFalse(c['writer_memory_dependencies'][0]['reads'][0]['read']['loaded_value_proven'])

    def test_indexed_update_base_not_old_constant(self):
        word=(31<<26)|(3<<21)|(4<<16)|(5<<11)|(119<<1)
        c=self.build([li(4,4),word,spr(912,467,4),BLR])
        self.assertEqual(c['writers'][0]['input_known_mask'],'0x00000000')

    def test_format_copy_keeps_original_memory_producer_and_hazard(self):
        c=self.build([0x90010028,0x8869000c,spr(912,467),spr(912,339,7),spr(913,467,7),BLR])
        sl=c['writer_source_slices'][1]
        self.assertEqual([n['site'] for n in sl['nodes']],[hex(BASE+4),hex(BASE+12)])
        self.assertEqual(sl['format_writer_dependencies'][0]['site'],hex(BASE+8))
        read=c['writer_memory_dependencies'][1]['reads'][0]
        self.assertEqual(read['read']['site'],hex(BASE+4))
        self.assertEqual(read['may_precede_stores'][0]['site'],hex(BASE))
        self.assertFalse(sl['memory_read_from_and_lifetime_proven'])

    def test_entry_format_read_has_explicit_unknown_bank_leaf(self):
        c=self.build([spr(914,339,7),BLR])
        self.assertEqual(c['format_read_source_slices'][0]['unresolved_format_leaves'],
            [dict(kind='entry',site=hex(BASE),spr=914)])
        self.assertEqual(c['format_reads'][0]['required_mask'],'0xffffffff')
        self.assertTrue(c['format_reads'][0]['raw_state_observer'])

    def test_copy_after_call_does_not_recover_stale_producer(self):
        c=self.build([li(3,4),spr(912,467),branch(16,link=True),spr(912,339,7),spr(913,467,7),BLR,BLR])
        sl=c['writer_source_slices'][1]
        self.assertEqual(sl['format_writer_dependencies'],[])
        self.assertEqual(sl['unresolved_format_leaves'],[dict(kind='call',site=hex(BASE+8),spr=912)])
        self.assertEqual([n['site'] for n in sl['nodes']],[hex(BASE+12)])

    def test_two_branch_producers_survive_same_value_copy(self):
        words=[conditional(16),li(3,4),spr(912,467),branch(12),li(3,4),spr(912,467),spr(912,339,7),spr(913,467,7),BLR]
        c=self.build(words);sl=c['writer_source_slices'][2]
        self.assertEqual([e['site'] for e in sl['format_writer_dependencies']],[hex(BASE+8),hex(BASE+20)])
        self.assertEqual([n['site'] for n in sl['nodes']],[hex(BASE+4),hex(BASE+16),hex(BASE+24)])

    def test_cross_bank_cycle_terminates_without_assuming_first_iteration(self):
        words=[spr(912,339,3),spr(913,467),spr(913,339,7),spr(912,467,7),conditional(-16),BLR]
        c=self.build(words)
        for sl in c['writer_source_slices']:
            self.assertEqual(len(sl['nodes']),2)
            self.assertEqual(len(sl['format_writer_dependencies']),2)
            self.assertTrue(sl['unresolved_format_leaves'])
            self.assertFalse(c['scope']['deletion_authorized'])

    def test_copy_of_other_bank_not_attributed_to_written_bank(self):
        c=self.build([li(3,4),spr(912,467),spr(914,339,7),spr(913,467,7),BLR])
        sl=c['writer_source_slices'][1]
        self.assertEqual(sl['format_writer_dependencies'],[])
        self.assertEqual(sl['unresolved_format_leaves'],[dict(kind='entry',site=hex(BASE),spr=914)])

    def test_raw_format_read_preserves_bits_ignored_by_float_conversion(self):
        c=self.build([li(3,0x4000),spr(912,467),spr(912,339,7),BLR])
        self.assertEqual(c['format_reads'][0]['known_bits'],'0x00004000')
        self.assertEqual(c['format_reads'][0]['required_mask'],'0xffffffff')
        self.assertFalse(c['format_reads'][0]['execution_or_context_lifetime_proven'])

    def test_missing_or_wrong_bank_source_writer_refused(self):
        node=dict(site=hex(BASE+4),gpr=7,dependencies=[],format_read=dict(spr=912,
            reaching_definitions=[dict(kind='write',site=hex(BASE))]))
        sink=dict(site=hex(BASE+8),spr=913,source_gpr=7,
            source_definition_roots=[dict(kind='definition',site=hex(BASE+4),gpr=7)])
        for writers in ([],[dict(site=hex(BASE),spr=914,source_gpr=3,source_definition_roots=[])]):
            with self.assertRaises(ValueError):writer_slices([sink],[node],format_writers=writers)

    def test_duplicate_original_definitions_or_format_writers_refused(self):
        node=dict(site=hex(BASE),gpr=7,dependencies=[])
        sink=dict(site=hex(BASE+4),spr=913,source_gpr=7,
            source_definition_roots=[dict(kind='definition',site=hex(BASE),gpr=7)])
        with self.assertRaises(ValueError):writer_slices([sink],[node,node])
        with self.assertRaises(ValueError):writer_slices([sink],[node],format_writers=[sink,sink])

    def test_branch_slice_keeps_both_distinct_definitions(self):
        c=self.build([conditional(12),li(3,4),branch(8),li(3,6),spr(912,467),BLR])
        self.assertEqual([n['site'] for n in c['writer_source_slices'][0]['nodes']],[hex(BASE+4),hex(BASE+12)])

    def test_cyclic_definition_slice_is_bounded(self):
        addi=(14<<26)|(3<<21)|(3<<16)|1
        c=self.build([li(3,4),addi,conditional(-4),spr(912,467),BLR])
        self.assertEqual(len(c['writer_source_slices'][0]['nodes']),2)
        self.assertEqual(c['writers'][0]['input_known_mask'],'0x00000000')

    def test_source_slice_after_call_exposes_barrier(self):
        c=self.build([branch(12,link=True),spr(912,467),BLR,BLR])
        self.assertEqual(c['writer_source_slices'][0]['unresolved_leaves'],[dict(kind='call',site=hex(BASE),gpr=3)])

    def test_malformed_missing_definition_slice_refused(self):
        with self.assertRaises(ValueError):writer_slices([dict(site=hex(BASE),spr=912,source_definition_roots=[dict(kind='definition',site=hex(BASE+4),gpr=3)])],[])

    def test_bad_entry_and_image_refused(self):
        for raw,entry in [(b'',BASE),(dol(),BASE+1),(dol(),BASE+0x100)]:
            with self.assertRaises(ValueError):build_format_contract(raw,entry)

    def test_original_file_changed_during_analysis_refused_before_output(self):
        with tempfile.TemporaryDirectory() as td:
            source=Path(td)/'input.dol';out=Path(td)/'output.json';raw=dol([NOP,BLR]);source.write_bytes(raw)
            def changed(data,entry):
                result=build_format_contract(data,entry);source.write_bytes(dol([BLR,BLR]));return result
            with patch('gqr_format_dataflow.build_format_contract',side_effect=changed),contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(main(['--dol',str(source),'--entry',hex(BASE),'--out',str(out)]),1)
            self.assertFalse(out.exists())

    def test_tool_provenance_changed_during_analysis_refused_before_output(self):
        with tempfile.TemporaryDirectory() as td:
            source=Path(td)/'input.dol';out=Path(td)/'output.json';source.write_bytes(dol([BLR]))
            with patch('gqr_format_dataflow.implementation_pins',side_effect=[{'source':'a'},{'source':'b'}]),contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(main(['--dol',str(source),'--entry',hex(BASE),'--out',str(out)]),1)
            self.assertFalse(out.exists())

    def test_cli_exclusive_output_and_dependency_source_binding(self):
        with tempfile.TemporaryDirectory() as td:
            source=Path(td)/'input.dol';out=Path(td)/'output.json';source.write_bytes(dol([BLR]))
            args=['--dol',str(source),'--entry',hex(BASE),'--out',str(out),'--incoming']
            with contextlib.redirect_stdout(io.StringIO()),contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(main(args),0);before=out.read_bytes();self.assertEqual(main(args),1)
                self.assertEqual(main(['--dol',str(source),'--entry',hex(BASE),'--out',str(source)]),1)
            self.assertEqual(out.read_bytes(),before)
            report=json.loads(before);self.assertEqual(len(report['source_pins']),6)
            self.assertEqual(report['contracts'][0]['inputs']['dol']['sha256'],hashlib.sha256(source.read_bytes()).hexdigest())
            self.assertFalse(report['incoming_reference_candidates'][0]['scope']['caller_closure_proven'])


if __name__=='__main__':unittest.main()
