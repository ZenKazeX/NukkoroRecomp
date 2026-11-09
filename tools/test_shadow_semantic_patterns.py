import copy,json,struct,unittest
from unittest.mock import patch
from gqr_format_dataflow import gpr_dependencies
from shadow_semantic_patterns import build_pipeline,describe,digest,match_evidence,normalize,Registers,strict_json,resolve_references,compare_scans,graph_svg
from graphics_lift_inventory import Dol
from test_graphics_lift_inventory import dol,BASE,BLR,branch
import test_platform_callback_setter_contract as setter_fixtures

def mem(op,r=3,base=13,offset=8):return (op<<26)|(r<<21)|(base<<16)|(offset&65535)
def csv(records):return ('addr_hex,name,size\n'+''.join(f'0x{p:08x},{name},{size}\n' for p,name,size in records)).encode()
def row(at,size,name='unknown'):return dict(address=at,size=size,name=name,id=name+':'+str(at))
class PatternTests(unittest.TestCase):
    def descriptor(self,words,at=BASE):return describe(Dol(dol(words)),row(at,len(words)*4))
    def fixture(self):
        a=[mem(32,3),0x38830001,mem(36,4),BLR]
        b=[mem(32,5),0x38c50001,mem(36,6),BLR]
        return a,b
    def test_alpha_rename_keeps_register_relationships(self):
        a,b=self.fixture();e=match_evidence(self.descriptor(a),self.descriptor(b))
        self.assertEqual(e['classification'],'STRUCTURAL MATCH');self.assertFalse(e['semantic_equivalence_proven'])
    def test_dependency_vocabulary_cannot_renumber_other_feature_owners(self):
        words=[0x22B60003,mem(32,3,8),0x38830001,mem(36,4,8),BLR]
        current=self.descriptor(words)
        with patch('shadow_semantic_patterns.gpr_dependencies',side_effect=lambda w:None if w>>26==8 else gpr_dependencies(w)):
            prior=self.descriptor(words)
        self.assertNotEqual(current['signals']['register_dataflow'],prior['signals']['register_dataflow'])
        for k in current['signals']:
            if k!='register_dataflow':self.assertEqual(current['signals'][k],prior['signals'][k],k)
        for k in current:
            if k!='signals':self.assertEqual(current[k],prior[k],k)
    def test_memory_feature_labels_do_not_renumber_instruction_tokens(self):
        words=[mem(32,3,8),0x38830001,mem(36,4,8),BLR]
        current=self.descriptor(words)
        with patch('shadow_semantic_patterns.memory_effect',return_value=None):
            changed=self.descriptor(words)
        self.assertEqual(current['signals']['normalized_sequence'],changed['signals']['normalized_sequence'])
        self.assertEqual(current['normalized_ngrams'],changed['normalized_ngrams'])
        self.assertEqual(current['signals']['register_dataflow'],changed['signals']['register_dataflow'])
    def test_changed_constant_is_not_erased(self):
        a,b=self.fixture();b[1]+=1
        self.assertEqual(match_evidence(self.descriptor(a),self.descriptor(b))['classification'],'UNKNOWN')
    def test_width_signedness_update_and_displacement_mutations(self):
        a,b=self.fixture()
        for replacement in (mem(34,5),mem(33,5),mem(32,5,offset=12),mem(42,5)):
            c=list(b);c[0]=replacement
            self.assertEqual(match_evidence(self.descriptor(a),self.descriptor(c))['classification'],'UNKNOWN')
    def test_read_write_polarity_not_erased(self):
        a,b=self.fixture();b[0]=mem(36,5)
        self.assertEqual(match_evidence(self.descriptor(a),self.descriptor(b))['classification'],'UNKNOWN')
    def test_stack_sda_and_zero_bases_remain_distinct(self):
        for base in (0,1,2,13):
            self.assertNotEqual(normalize(mem(32,base=base),BASE,BASE,BASE+4,Registers()),normalize(mem(32,base=12),BASE,BASE,BASE+4,Registers()))
    def test_arbitrary_hardware_and_unknown_words_remain_exact(self):
        for word in (0x7c6000a6,0x7c6802a6,0xe0030000,0xf0030000,0xffffffff,0x60000000):
            self.assertEqual(normalize(word,BASE,BASE,BASE+4,Registers()),('opaque',f'0x{word:08x}') if word!=0x60000000 else ('logical-immediate',24,'r0','r0',0))
    def test_no_nop_removal(self):
        a,_=self.fixture();b=a[:1]+[0x60000000]+a[1:]
        self.assertNotEqual(self.descriptor(a)['signals']['normalized_sequence'],self.descriptor(b)['signals']['normalized_sequence'])
    def test_relative_branch_relocation_keeps_topology(self):
        r=Registers();a=normalize(branch(8),BASE,BASE,BASE+16,r)
        b=normalize(branch(8),BASE+32,BASE+32,BASE+48,Registers())
        self.assertEqual(a,b)
    def test_branch_condition_link_absolute_and_destination_mutations(self):
        word=0x41820008;base=normalize(word,BASE,BASE,BASE+32,Registers())
        for mutated in (word^1,word^2,word^0x10000,word^0x200000,word+4):
            self.assertNotEqual(base,normalize(mutated,BASE,BASE,BASE+32,Registers()))
    def test_external_call_targets_are_difference_evidence_not_proof(self):
        a=[mem(32),branch(100,link=True),mem(36),BLR]
        b=[mem(32),branch(200,link=True),mem(36),BLR]
        x,y=self.descriptor(a),self.descriptor(b)
        self.assertNotEqual(x['calls'],y['calls']);self.assertFalse(match_evidence(x,y)['semantic_equivalence_proven'])
    def test_raw_ngrams_and_normalized_ngrams_both_preserved(self):
        a,b=self.fixture();x,y=self.descriptor(a),self.descriptor(b)
        self.assertEqual(x['normalized_ngrams'],y['normalized_ngrams']);self.assertNotEqual(x['raw_ngram_hashes'],y['raw_ngram_hashes'])
    def test_tiny_identical_leaf_is_not_promoted_by_similarity(self):
        x=self.descriptor([BLR]);self.assertEqual(match_evidence(x,x)['classification'],'UNKNOWN')
    def test_source_bound_cluster_callers_and_ranking(self):
        a,b=self.fixture();words=a+b+[branch(-32,link=True),BLR]
        p=build_pipeline(dol(words),csv([(BASE,'first',16),(BASE+16,'second',16),(BASE+32,'caller',8)]))
        self.assertEqual(len(p['families']),1);self.assertEqual(p['descriptors'][0]['callers'][0]['site'],f'0x{BASE+32:08x}')
        self.assertGreater(p['unknown_ranking'][0]['score'],0);self.assertFalse(p['scope']['native_retirement_proven'])
    def test_overlapping_callsite_annotations_do_not_inflate_callers(self):
        words=[mem(32),0x38830001,mem(36),BLR,branch(-16,link=True),BLR]
        p=build_pipeline(dol(words),csv([(BASE,'target',16),(BASE+16,'caller-a',8),(BASE+16,'caller-b',8)]))
        self.assertEqual(len(p['descriptors'][0]['callers']),1)
        self.assertEqual(len(p['source_call_index']),1)
        self.assertEqual(len(p['source_call_index'][0]['declaration_owner_annotations']),2)
    def test_undeclared_original_calls_are_still_retained(self):
        words=[mem(32),0x38830001,mem(36),BLR,branch(-16,link=True),BLR]
        p=build_pipeline(dol(words),csv([(BASE,'target',16)]))
        self.assertEqual(len(p['descriptors'][0]['callers']),1)
        self.assertFalse(p['source_call_index'][0]['declaration_owner_annotations'])
    def test_overlapping_declarations_preserved(self):
        a,b=self.fixture();p=build_pipeline(dol(a+b),csv([(BASE,'one',32),(BASE+8,'overlap',16)]))
        self.assertEqual(len(p['descriptors']),2);self.assertTrue(p['declaration_overlaps'])
    def test_bounds_and_omissions_are_explicit(self):
        a,b=self.fixture();p=build_pipeline(dol(a+b),csv([(BASE,'big',32),(BASE,'small',16)]),max_words=4,max_work_words=4)
        self.assertEqual(p['summary']['omitted_ranges'],1);self.assertEqual(p['omitted_ranges'][0]['reason'],'extent-exceeds-diagnostic-limit')
        for n in (True,0,65537):
            with self.assertRaises(ValueError):build_pipeline(dol(a),csv([(BASE,'a',16)]),max_words=n)
    def test_total_budget_does_not_silently_truncate_a_function(self):
        a,b=self.fixture();p=build_pipeline(dol(a+b),csv([(BASE,'a',16),(BASE+16,'b',16)]),max_work_words=4)
        self.assertEqual(p['summary']['described_ranges'],1);self.assertEqual(p['omitted_ranges'][0]['word_count'],4)
    def refs(self,raw,entry=BASE):
        image=Dol(raw);blob=b''.join(image.word(entry+4*i).to_bytes(4,'big') for i in range(17))
        return [dict(id='setter-training',entry=f'0x{entry:08x}',recognizer='callback-setter-read-before-exclusion',dol_sha256=digest(raw),original_bytes_sha256=digest(blob),word_count=17,role='training')]
    def test_reviewed_reference_proves_only_limited_source_contract(self):
        raw=dol(setter_fixtures.CallbackSetterTests().words());x=resolve_references(raw,self.refs(raw))[0]
        self.assertEqual(x['classification'],'PROVEN');self.assertFalse(x['full_semantic_equivalence_proven']);self.assertFalse(x['native_retirement_proven'])
    def test_mutated_source_hash_or_recognizer_cannot_promote(self):
        raw=dol(setter_fixtures.CallbackSetterTests().words());ref=self.refs(raw)
        for key,value in (('dol_sha256','0'*64),('original_bytes_sha256','0'*64),('recognizer','fake'),('role','test'),('word_count',16)):
            changed=copy.deepcopy(ref);changed[0][key]=value
            with self.assertRaises(ValueError):resolve_references(raw,changed)
    def test_held_out_mutation_each_setter_word_rejected_when_semantics_change(self):
        words=setter_fixtures.CallbackSetterTests().words()
        for i in range(17):
            changed=list(words);changed[i]^=4;raw=dol(changed);ref=self.refs(raw);ref[0]['role']='held-out'
            with self.assertRaises(ValueError):resolve_references(raw,ref)
    def test_duplicate_unknown_reference_and_input_types_rejected(self):
        raw=dol(setter_fixtures.CallbackSetterTests().words());ref=self.refs(raw)
        for changed in (ref+ref,{},['bad'],[dict(ref[0],supplied_state={})]):
            with self.assertRaises(ValueError):resolve_references(raw,changed)
    def test_recognizer_automatically_discovers_family_without_training_transfer(self):
        words=setter_fixtures.CallbackSetterTests().words();p=build_pipeline(dol(words),csv([(BASE,'unknown',68)]))
        self.assertEqual(len(p['source_recognizer_matches']),1);self.assertFalse(p['source_recognizer_matches'][0]['native_retirement_proven'])
    def test_rescan_requires_same_inputs_and_reports_changes(self):
        a,_=self.fixture();p=build_pipeline(dol(a),csv([(BASE,'a',16)]));q=copy.deepcopy(p)
        q['descriptors'][0]['signals']['cfg_shape']='changed';d=compare_scans(p,q);self.assertEqual(len(d['signal_changes']),1)
        q['inputs']['dol_sha256']='other'
        with self.assertRaises(ValueError):compare_scans(p,q)
    def test_strict_json_duplicate_and_nonfinite_rejected(self):
        for data in ('{"a":1,"a":2}','{"a":NaN}','{"a":Infinity}'):
            with self.assertRaises(ValueError):strict_json(data)
    def test_register_transfer_leaves_are_whole_text_source_matches_not_cluster_votes(self):
        hid0_read,lr_write=0x7c70faa6,0x7c0803a6
        p=build_pipeline(dol([hid0_read,BLR,lr_write,BLR]),csv([(BASE,'unknown',16)]))
        self.assertEqual((p['summary']['reviewed_register_transfer_leaf_matches'],p['summary']['reviewed_register_transfer_isolated_leaves']),(1,1))
        self.assertEqual(p['source_register_transfer_matches']['summary']['refused'],1)
        self.assertEqual(p['source_register_transfer_matches']['matches'][0]['classification'],'PROVEN')
        self.assertEqual(p['descriptors'][0]['classification'],'UNKNOWN');self.assertFalse(p['unknown_ranking'][0]['known_source_contract'])
    def test_graph_is_bounded_and_labels_unknown_ownership(self):
        a,_=self.fixture();p=build_pipeline(dol(a),csv([(BASE,'a',16)]))
        graph=graph_svg(p,1);self.assertIn('UNKNOWN ownership',graph);self.assertIn('0x800',graph)
if __name__=='__main__':unittest.main()
