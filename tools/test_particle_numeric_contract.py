import contextlib
import io
import json
from pathlib import Path
import tempfile
import unittest

if __package__:
    from . import particle_numeric_contract as nc
    from .test_graphics_lift_inventory import BASE, BLR, NOP, branch, conditional, dol
else:
    import particle_numeric_contract as nc
    from test_graphics_lift_inventory import BASE, BLR, NOP, branch, conditional, dol


def psq(reg, base=3, *, store=False, one=False, fmt=0):
    return ((60 if store else 56)<<26)|(reg<<21)|(base<<16)|(int(one)<<15)|(fmt<<12)


def paired(d,a,b,c,xo):
    return (4<<26)|(d<<21)|(a<<16)|(b<<11)|(c<<6)|(xo<<1)


class LaneContractTests(unittest.TestCase):
    def build(self, words):
        return nc.build_numeric_contract(dol(words), BASE)

    def stores(self, report):
        return [r for r in report['memory_effects'] if r['access']=='write']

    def test_uninitialized_normal_detected_at_output(self):
        report=self.build([psq(19,27,store=True,fmt=7),BLR])
        self.assertEqual(self.stores(report)[0]['possible_incoming_fp'],['entry.f19.0','entry.f19.1'])
        self.assertEqual(report['summary']['nonstack_incoming_fp_store_sites'],1)

    def test_local_load_removes_entry_roots(self):
        report=self.build([psq(19),psq(19,27,store=True,fmt=7),BLR])
        self.assertEqual(self.stores(report)[0]['possible_incoming_fp'],[])
        self.assertEqual(self.stores(report)[0]['dependencies'],[f'memory.{hex(BASE)}.0',f'memory.{hex(BASE)}.1'])

    def test_one_element_load_defines_secondary_lane_constant(self):
        report=self.build([psq(19,one=True),psq(19,27,store=True),BLR])
        self.assertEqual(self.stores(report)[0]['dependencies'],[f'memory.{hex(BASE)}.0'])

    def test_one_element_store_does_not_consume_secondary_lane(self):
        report=self.build([psq(19,27,store=True,one=True),BLR])
        self.assertEqual(self.stores(report)[0]['possible_incoming_fp'],['entry.f19.0'])

    def test_double_restore_defines_only_primary_lane(self):
        lfd=(50<<26)|(19<<21)|(1<<16)|0x80
        report=self.build([lfd,psq(19,27,store=True),BLR])
        self.assertEqual(self.stores(report)[0]['possible_incoming_fp'],['entry.f19.1'])

    def test_single_load_defines_both_lanes(self):
        lfs=(48<<26)|(19<<21)|(3<<16)
        report=self.build([lfs,psq(19,27,store=True),BLR])
        self.assertEqual(self.stores(report)[0]['possible_incoming_fp'],[])

    def test_merge_aliasing_reads_before_simultaneous_write(self):
        report=self.build([paired(19,20,19,0,560),psq(19,27,store=True),BLR])
        self.assertEqual(self.stores(report)[0]['possible_incoming_fp'],['entry.f19.1','entry.f20.0'])

    def test_scalar_multiplier_uses_selected_half_only(self):
        report=self.build([paired(19,20,0,21,13),psq(19,27,store=True),BLR])
        self.assertEqual(self.stores(report)[0]['possible_incoming_fp'],['entry.f20.0','entry.f20.1','entry.f21.1'])

    def test_paired_estimates_use_only_matching_source_lanes(self):
        for xo in (24,26):
            for destination in (12,19):
                for source in (12,19,20):
                    word=paired(destination,0,source,0,xo)
                    effect=nc.numeric_effect(word)
                    self.assertEqual(effect['defs'],{nc.lane(destination,i):[nc.lane(source,i)] for i in range(2)})
                    report=self.build([word,psq(destination,27,store=True),BLR])
                    self.assertEqual(self.stores(report)[0]['possible_incoming_fp'],[f'entry.f{source}.0',f'entry.f{source}.1'])
                    self.assertFalse(report['scope']['numeric_values_evaluated'])
        self.assertEqual(nc.numeric_effect(0x11806034)['defs'],{24:[24],25:[25]})

    def test_cache_zero_memory_and_exception_effects_remain_unknown(self):
        for word in (0x10041FEC,0x101D27EC):
            self.assertIsNone(nc.numeric_effect(word))
            report=self.build([word,psq(19,27,store=True),BLR])
            self.assertEqual(report['unknown_sites'],[hex(BASE)])
            self.assertFalse(report['scope']['known_local_lane_effects_decoded'])

    def test_source_census_includes_arithmetic_and_explicit_observers(self):
        for word,expected in ((paired(19,18,19,0,21),[36,37,38,39]),
                              (paired(19,20,19,0,560),[39,40]),
                              (psq(19,27,store=True),[38,39]),
                              (psq(19,one=True),[])):
            self.assertEqual(nc.effect_source_lanes(nc.numeric_effect(word)),expected)
        self.assertIsNone(nc.effect_source_lanes(None))

    def test_conditional_skipped_load_keeps_incoming_dependency(self):
        report=self.build([conditional(8),psq(19),psq(19,27,store=True),BLR])
        self.assertEqual(self.stores(report)[0]['possible_incoming_fp'],['entry.f19.0','entry.f19.1'])
        self.assertFalse(report['scope']['path_feasibility_proven'])

    def test_loop_reaches_stable_union_without_unrolling(self):
        report=self.build([paired(19,19,20,0,21),conditional(-4),psq(19,27,store=True),BLR])
        self.assertLess(report['summary']['worklist_iterations'],20)
        self.assertEqual(self.stores(report)[0]['possible_incoming_fp'],['entry.f19.0','entry.f19.1','entry.f20.0','entry.f20.1'])

    def test_call_poisons_all_lanes_no_abi_guesses(self):
        report=self.build([branch(12,link=True),psq(19,27,store=True),BLR,psq(19),BLR])
        self.assertEqual(report['unknown_sites'],[hex(BASE)])
        self.assertIn('unknown.'+hex(BASE),self.stores(report)[0]['dependencies'])
        self.assertFalse(report['scope']['known_local_lane_effects_decoded'])

    def test_unsupported_numeric_is_unknown_never_noop(self):
        for word in ((63<<26)|(1<<1),paired(19,3,4,0,1),(59<<26)):
            report=self.build([word,psq(19,27,store=True),BLR])
            self.assertEqual(report['unknown_sites'],[hex(BASE)])
            self.assertFalse(report['scope']['known_local_lane_effects_decoded'])

    def test_scalar_single_arithmetic_duplicates_primary_dependencies(self):
        # fmuls uses fA.ps0/fC.ps0, and writes both destination halves.
        word=(59<<26)|(19<<21)|(20<<16)|(21<<6)|(25<<1)
        report=self.build([word,psq(19,27,store=True),BLR])
        self.assertEqual(self.stores(report)[0]['possible_incoming_fp'],['entry.f20.0','entry.f21.0'])
        self.assertEqual(report['unknown_sites'],[])

    def test_scalar_double_arithmetic_leaves_secondary_untouched(self):
        word=(63<<26)|(19<<21)|(20<<16)|(21<<6)|(25<<1)
        report=self.build([word,psq(19,27,store=True),BLR])
        self.assertEqual(self.stores(report)[0]['possible_incoming_fp'],['entry.f19.1','entry.f20.0','entry.f21.0'])

    def test_scalar_unary_and_compare_effects_keep_status_unproven(self):
        word=(63<<26)|(19<<21)|(20<<11)|(12<<1)
        report=self.build([word,(63<<26)|(19<<16)|(21<<11),psq(19,27,store=True),BLR])
        self.assertEqual(self.stores(report)[0]['possible_incoming_fp'],['entry.f19.1','entry.f20.0'])
        self.assertEqual(report['lane_observations'][0]['dependencies'],['entry.f20.0','entry.f21.0'])
        self.assertIn('not propagated',report['scope']['status'])

    def test_direct_original_callee_summary_is_explicitly_opt_in(self):
        words=[branch(12,link=True),psq(19,27,store=True),BLR,psq(19),BLR]
        report=nc.build_numeric_contract(dol(words),BASE,direct_call_depth=1)
        self.assertEqual(report['unknown_sites'],[])
        self.assertEqual(self.stores(report)[0]['dependencies'],[f'memory.{hex(BASE+12)}.0',f'memory.{hex(BASE+12)}.1'])
        self.assertEqual(len(report['direct_callee_summaries']),1)
        self.assertFalse(report['scope']['external_memory_alias_closure_proven'])

    def test_direct_callee_arguments_are_symbolically_substituted(self):
        words=[psq(20),branch(12,link=True),psq(19,27,store=True),BLR,paired(19,20,21,0,21),BLR]
        report=nc.build_numeric_contract(dol(words),BASE,direct_call_depth=1)
        self.assertEqual(self.stores(report)[0]['possible_incoming_fp'],['entry.f21.0','entry.f21.1'])
        self.assertIn(f'memory.{hex(BASE)}.0',self.stores(report)[0]['dependencies'])

    def test_direct_callee_store_observations_are_not_lost(self):
        words=[branch(12,link=True),NOP,BLR,psq(19,27,store=True),BLR]
        report=nc.build_numeric_contract(dol(words),BASE,direct_call_depth=1)
        self.assertEqual(report['lane_observations'][0]['dependencies'],['entry.f19.0','entry.f19.1'])
        self.assertEqual(report['direct_callee_summaries'][0]['memory_effects'][0]['access'],'write')

    def test_unknown_indirect_recursive_and_unsupported_callees_stay_unknown(self):
        for words in ([0x4e800421,psq(19,27,store=True),BLR],
                      [branch(0,link=True),psq(19,27,store=True),BLR],
                      [branch(12,link=True),psq(19,27,store=True),BLR,(63<<26)|(1<<1),BLR],
                      [branch(12,link=True),psq(19,27,store=True),BLR,0x4e800420]):
            report=nc.build_numeric_contract(dol(words),BASE,direct_call_depth=2)
            self.assertIn(hex(BASE),report['unknown_sites'])
            self.assertEqual(report['direct_callee_summaries'],[])

    def test_status_read_defines_only_primary_status_value(self):
        mffs=(63<<26)|(19<<21)|(583<<1)
        report=self.build([mffs,psq(19,27,store=True),BLR])
        self.assertEqual(self.stores(report)[0]['dependencies'],['entry.f19.1',f'status.FPSCR.{hex(BASE)}'])
        self.assertEqual(report['explicit_status_effects'][0]['access'],'read')
        self.assertFalse(report['explicit_status_effects'][0]['status_value_or_liveness_proven'])

    def test_status_write_observes_primary_source_without_poisoning_unrelated_lanes(self):
        mtfsf=(63<<26)|(19<<11)|(711<<1)
        report=self.build([mtfsf,psq(20,27,store=True),BLR])
        self.assertEqual(report['lane_observations'][0]['dependencies'],['entry.f19.0'])
        self.assertEqual(self.stores(report)[0]['dependencies'],['entry.f20.0','entry.f20.1'])
        self.assertEqual(report['explicit_status_effects'][0]['access'],'write')

    def test_nested_callee_status_and_memory_effects_remain_explicit(self):
        mtfsf=(63<<26)|(19<<11)|(711<<1)
        words=[branch(12,link=True),NOP,BLR,branch(8,link=True),BLR,mtfsf,psq(19,27,store=True),BLR]
        report=nc.build_numeric_contract(dol(words),BASE,direct_call_depth=2)
        nested=report['direct_callee_summaries'][0]['nested_direct_callee_summaries'][0]
        self.assertEqual(nested['explicit_status_effects'][0]['access'],'write')
        self.assertEqual(nested['memory_effects'][0]['access'],'write')
        self.assertEqual(report['lane_observations'][0]['dependencies'],['entry.f19.0','entry.f19.1'])

    def test_indexed_scalar_loads_have_explicit_address_and_lane_width(self):
        for xo in (535,567,599,631):
            word=(31<<26)|(19<<21)|(3<<16)|(4<<11)|(xo<<1)
            report=self.build([word,psq(19,27,store=True),BLR])
            row=report['memory_effects'][0]
            self.assertEqual((row['base_gpr'],row['index_gpr'],row['indexed']),(3,4,True))
            self.assertEqual(row['scalar_width'],4 if xo in (535,567) else 8)
            self.assertEqual(self.stores(report)[0]['possible_incoming_fp'],[] if xo in (535,567) else ['entry.f19.1'])

    def test_indexed_scalar_stores_keep_primary_word_and_alias_uncertainty(self):
        stfd=(54<<26)|(19<<21)|(1<<16)|0x58
        lfd=(50<<26)|(19<<21)|(1<<16)|0x58
        for xo in (663,695,727,759,983):
            word=(31<<26)|(19<<21)|(1<<16)|(4<<11)|(xo<<1)
            report=self.build([stfd,word,lfd,BLR])
            row=self.stores(report)[1]
            self.assertEqual(row['possible_incoming_fp'],['entry.f19.0'])
            self.assertEqual(row['low_word_only'],xo==983)
            self.assertIn(hex(BASE+4),report['stack_roundtrip_candidates'][0]['possible_alias_or_call_sites'])

    def test_indexed_quantized_load_store_fields_and_lane_effects(self):
        for xo in (6,7,38,39):
            for one in (False,True):
                for fmt in range(8):
                    word=(4<<26)|(19<<21)|(3<<16)|(4<<11)|(int(one)<<10)|(fmt<<7)|(xo<<1)
                    report=self.build([word,psq(19,27,store=True),BLR])
                    row=report['memory_effects'][0]
                    self.assertEqual((row['base_gpr'],row['index_gpr'],row['indexed']),(3,4,True))
                    self.assertEqual((row['format_index'],row['elements'],row['updates_base']),(fmt,1 if one else 2,xo in (38,39)))
                    self.assertNotIn('scalar_width',row)
                    self.assertNotIn('displacement',row)
                    self.assertEqual(report['unknown_sites'],[])
                    if xo in (7,39):
                        self.assertEqual(row['possible_incoming_fp'],['entry.f19.0'] if one else ['entry.f19.0','entry.f19.1'])
                    else:
                        self.assertEqual(self.stores(report)[0]['possible_incoming_fp'],[])
                        self.assertEqual(len(self.stores(report)[0]['dependencies']),1 if one else 2)

    def test_indexed_quantized_update_kills_base_fact_and_reports_stack_alias(self):
        stfd=(54<<26)|(19<<21)|(1<<16)|0x58
        lfd=(50<<26)|(19<<21)|(1<<16)|0x58
        for xo in (6,7,38,39):
            word=(4<<26)|(19<<21)|(1<<16)|(4<<11)|(xo<<1)
            report=self.build([stfd,word,lfd,BLR])
            candidate=report['stack_roundtrip_candidates'][0]
            self.assertFalse(candidate['preservation_proven'])
            if xo in (7,39):
                self.assertIn(hex(BASE+4),candidate['possible_alias_or_call_sites'])
            self.assertEqual(hex(BASE+4) in candidate['possible_stack_base_change_sites'],xo in (38,39))
            facts=tuple([None,0x1000,None,0x2000]+[None]*60)
            self.assertEqual(nc.predicate_transfer(word,facts)[1],None if xo in (38,39) else 0x1000)

    def test_indexed_quantized_zero_base_and_original_helper_encodings(self):
        for xo in (38,39):
            word=(4<<26)|(19<<21)|(4<<11)|(xo<<1)
            effect=nc.numeric_effect(word)
            self.assertTrue(effect['memory']['update_form'])
            self.assertFalse(effect['memory']['updates_base'])
            facts=tuple([0x1234]+[None]*63)
            self.assertEqual(nc.predicate_transfer(word,facts)[0],0x1234)
        # Original indexed ABI helper words: psq_lx/stx f19,r0,r7/r3,0,0.
        load=nc.numeric_effect(0x1260380C)
        store=nc.numeric_effect(0x1260180E)
        self.assertEqual(load['defs'],{38:None,39:None})
        self.assertEqual(store['reads'],[38,39])
        self.assertEqual((load['memory']['base_gpr'],load['memory']['index_gpr'],load['memory']['format_index']),(0,7,0))
        self.assertEqual((store['memory']['base_gpr'],store['memory']['index_gpr'],store['memory']['format_index']),(0,3,0))

    def test_conditional_call_keeps_no_call_lane_roots(self):
        words=[conditional(12,link=True),psq(19,27,store=True),BLR,psq(19),BLR]
        report=nc.build_numeric_contract(dol(words),BASE,direct_call_depth=1)
        self.assertEqual(self.stores(report)[0]['possible_incoming_fp'],['entry.f19.0','entry.f19.1'])

    def test_direct_call_depth_and_nested_calls_are_bounded(self):
        words=[branch(12,link=True),psq(19,27,store=True),BLR,branch(8,link=True),BLR,psq(19),BLR]
        self.assertNotEqual(nc.build_numeric_contract(dol(words),BASE,direct_call_depth=1)['unknown_sites'],[])
        self.assertEqual(nc.build_numeric_contract(dol(words),BASE,direct_call_depth=2)['unknown_sites'],[])
        for value in (-1,4,True,1.5):
            with self.assertRaises(ValueError): nc.build_numeric_contract(dol([BLR]),BASE,direct_call_depth=value)

    def test_stack_observer_not_silently_deleted(self):
        stfd=(54<<26)|(19<<21)|(1<<16)|0x58
        report=self.build([stfd,BLR])
        self.assertEqual(self.stores(report)[0]['possible_incoming_fp'],['entry.f19.0'])
        self.assertEqual(report['summary']['nonstack_incoming_fp_store_sites'],0)
        self.assertFalse(report['scope']['stack_alias_closure_proven'])

    def test_raw_primary_stack_roundtrip_is_candidate_not_rewritten_dependency(self):
        stfd=(54<<26)|(19<<21)|(1<<16)|0x58
        lfd=(50<<26)|(19<<21)|(1<<16)|0x58
        report=self.build([stfd,psq(19),lfd,BLR])
        row=report['stack_roundtrip_candidates'][0]
        self.assertEqual((row['save_site'],row['restore_site'],row['primary_lane']),(hex(BASE),hex(BASE+8),'f19.0'))
        self.assertFalse(row['preservation_proven'])
        self.assertEqual(report['return_lane_dependencies'][0]['lanes']['f19.0'],[f'memory.{hex(BASE+8)}.0'])

    def test_roundtrip_reports_overlapping_integer_and_alias_writes_calls(self):
        stfd=(54<<26)|(19<<21)|(1<<16)|0x58
        lfd=(50<<26)|(19<<21)|(1<<16)|0x58
        overlap=(36<<26)|(3<<21)|(1<<16)|0x5c
        alias=(36<<26)|(3<<21)|(4<<16)
        report=self.build([stfd,overlap,alias,branch(12,link=True),lfd,BLR,BLR])
        row=report['stack_roundtrip_candidates'][0]
        self.assertEqual(row['known_overlapping_stack_write_sites'],[hex(BASE+4)])
        self.assertEqual(row['possible_alias_or_call_sites'],[hex(BASE+8),hex(BASE+12)])

    def test_roundtrip_respects_flow_and_records_frame_changes(self):
        stfd=(54<<26)|(19<<21)|(1<<16)|0x58
        lfd=(50<<26)|(19<<21)|(1<<16)|0x58
        report=self.build([stfd,(14<<26)|(1<<21)|(1<<16)|16,lfd,BLR])
        self.assertEqual(report['stack_roundtrip_candidates'][0]['possible_stack_base_change_sites'],[hex(BASE+4)])
        # The save has no flow to this load (it is on the opposite branch).
        report=self.build([conditional(12),stfd,BLR,lfd,BLR])
        self.assertEqual(report['stack_roundtrip_candidates'],[])

    def test_paired_stack_roundtrip_is_not_treated_as_raw_double_preservation(self):
        report=self.build([psq(19,1,store=True),psq(19,1),BLR])
        self.assertEqual(report['stack_roundtrip_candidates'],[])

    def test_empty_effects_do_not_certify_retirement(self):
        report=self.build([NOP,BLR])
        for key in ('asynchronous_context_closure_proven','external_memory_alias_closure_proven',
                    'native_replacement_proven','deletion_authorized'):
            self.assertFalse(report['scope'][key])

    def test_signed_displacement_and_format_kept(self):
        report=self.build([psq(19,27,store=True,fmt=7)|0xFFF,BLR])
        store=self.stores(report)[0]
        self.assertEqual((store['displacement'],store['format_index']),(-1,7))

    def test_original_argument_branch_refinement_excludes_only_known_arm(self):
        # cmpwi r4,0; beq skip; load normal; output normal.
        words=[(11<<26)|(4<<16),conditional(8,bi=2),psq(19),psq(19,27,store=True),BLR]
        nonzero=nc.build_numeric_contract(dol(words),BASE,argument_r4=0x01100000)
        zero=nc.build_numeric_contract(dol(words),BASE,argument_r4=0)
        self.assertEqual(self.stores(nonzero)[0]['possible_incoming_fp'],[])
        self.assertEqual(self.stores(zero)[0]['possible_incoming_fp'],['entry.f19.0','entry.f19.1'])
        self.assertFalse(nonzero['scope']['path_feasibility_proven'])

    def test_unknown_memory_write_to_argument_register_kills_constant_fact(self):
        words=[(32<<26)|(4<<21)|(3<<16),(11<<26)|(4<<16),conditional(8,bi=2),psq(19),psq(19,27,store=True),BLR]
        report=nc.build_numeric_contract(dol(words),BASE,argument_r4=7)
        self.assertEqual(self.stores(report)[0]['possible_incoming_fp'],['entry.f19.0','entry.f19.1'])

    def test_recording_float_compare_and_unknown_cr_operation_kill_predicates(self):
        for word in (paired(0,1,2,0,0), (19<<26)|(257<<1)):
            words=[(11<<26)|(4<<16),word,conditional(8,bi=2),psq(19),psq(19,27,store=True),BLR]
            report=nc.build_numeric_contract(dol(words),BASE,argument_r4=7)
            self.assertEqual(self.stores(report)[0]['possible_incoming_fp'],['entry.f19.0','entry.f19.1'])

    def test_ctr_loop_keeps_both_arms_without_guessing_trip_count(self):
        words=[psq(19),0x4200FFFC,psq(19,27,store=True),BLR]
        report=nc.build_numeric_contract(dol(words),BASE,argument_count=3)
        self.assertEqual(report['summary']['parameter_model_words'],4)
        self.assertFalse(report['scope']['numeric_values_evaluated'])

    def test_bad_parameter_models_refused(self):
        for value in (-1,0x100000000,True,1.5):
            with self.assertRaises(ValueError): nc.build_numeric_contract(dol([BLR]),BASE,argument_r4=value)

    def test_return_integer_model_exposes_only_known_original_transformations(self):
        words=[(25<<26)|(3<<21)|(3<<16)|0x2000,BLR]
        report=nc.build_numeric_contract(dol(words),BASE,argument_count=0x91)
        self.assertEqual(report['return_integer_models'][0]['known_gprs'],{'r3':'0x20000091'})
        words=[(32<<26)|(3<<21)|(4<<16),BLR]
        report=nc.build_numeric_contract(dol(words),BASE,argument_count=0x91)
        self.assertNotIn('r3',report['return_integer_models'][0]['known_gprs'])

    def test_cli_exclusive_output_and_original_input(self):
        with tempfile.TemporaryDirectory() as tmp:
            source,out=Path(tmp)/'original.dol',Path(tmp)/'report.json'
            raw=dol([psq(19,27,store=True),BLR]); source.write_bytes(raw)
            argv=['--dol',str(source),'--entry',hex(BASE),'--out',str(out)]
            with contextlib.redirect_stdout(io.StringIO()),contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(nc.main(argv),0); before=out.read_bytes()
                self.assertEqual(nc.main(argv),1)
                self.assertEqual(nc.main(argv[:-1]+[str(source)]),1)
            self.assertEqual(out.read_bytes(),before); self.assertEqual(source.read_bytes(),raw)
            self.assertFalse(json.loads(before)['scope']['deletion_authorized'])


if __name__=='__main__': unittest.main()
