import hashlib,json,unittest
from pathlib import Path
from gqr_format_dataflow import gpr_dependencies
from platform_register_transfer_leaf import (MFSPR,MFTB,MTSPR,build_transfer_caller_frontier,build_transfer_report,
    decode_transfer,recognize_register_transfer_leaf,rlwinm_mask,scan_register_transfer_leaves)
from test_graphics_lift_inventory import BASE,BLR,branch,conditional,dol

ORIGINAL=Path(__file__).resolve().parents[2]/'game/sys/main.dol'
ORIGINAL_SHA='fde4fa6f81a60313b710161c196dc51c2260be62251ee02775d5eee06f9d55af'
REFERENCES=Path(__file__).with_name('test_data')/'register_transfer_leaf_references.json'

# Independent transcription of Gekko UM v1.2 decimal SPR columns (Tables 12-10,
# 12-11, 12-13). Deliberately typed out, not derived from the module tables.
MANUAL_MFSPR={1,8,9,18,19,22,25,26,27,272,273,274,275,282,287,528,529,530,531,532,533,534,535,536,537,538,539,
    540,541,542,543,912,913,914,915,916,917,918,919,920,921,922,923,936,937,938,939,940,941,942,943,952,953,954,955,
    956,957,958,959,1008,1009,1010,1013,1017,1019,1020,1021,1022}
MANUAL_MTSPR=(MANUAL_MFSPR-{287})|{284,285}
MANUAL_MFTB={268,269}

def xfx(xo,g,n):return 31<<26|g<<21|(n&31)<<16|((n>>5)&31)<<11|xo<<1
def mfspr(g,n):return xfx(339,g,n)
def mtspr(n,g):return xfx(467,g,n)
def mftb(g,n=268):return xfx(371,g,n)
def mfmsr(g):return 31<<26|g<<21|83<<1
def mtmsr(g):return 31<<26|g<<21|146<<1
def ori(a,s,v):return 24<<26|s<<21|a<<16|(v&65535)
def oris(a,s,v):return 25<<26|s<<21|a<<16|(v&65535)
def rlwinm(a,s,sh,mb,me,rc=0):return 21<<26|s<<21|a<<16|sh<<11|mb<<6|me<<1|rc
def mr(a,s):return 31<<26|s<<21|a<<16|s<<11|444<<1
def addi(d,a,v):return 14<<26|d<<21|a<<16|(v&65535)
def lis(d,v):return 15<<26|d<<21|(v&65535)
def call(frm,to):return branch(to-frm,link=True)

def manual_identity(word):
    """Test oracle from the manual transcription; returns None when not admitted."""
    if word>>26!=31 or word&1:return None
    xo,g,a,b=(word>>1)&1023,(word>>21)&31,(word>>16)&31,(word>>11)&31;n=a|b<<5
    if xo in (83,146):return None if a or b else ('mfmsr' if xo==83 else 'mtmsr',None,g)
    table={339:('mfspr',MANUAL_MFSPR),467:('mtspr',MANUAL_MTSPR),371:('mftb',MANUAL_MFTB)}.get(xo)
    if table is None or n not in table[1] or (xo==467 and n==8):return None
    return (table[0],n,g)

def identity(report):
    d=report['differences'];return (d['operation'],d['register_number'],d['gpr'])

def render(e):
    """Fold literal arithmetic; keep leaf reads symbolic. Presentation for tests only."""
    k=e['kind']
    if k=='literal':return int(e['value'],16)
    if k in ('copy','preserved_across_leaf'):return render(e['of'])
    inner=render(e.get('of')) if isinstance(e.get('of'),dict) else None
    if k=='add_u32':return (inner+e['offset'])&0xffffffff if isinstance(inner,int) else f"({inner}{e['offset']:+d})"
    if k=='or_u32':return inner|int(e['bits'],16) if isinstance(inner,int) else f"({inner}|{e['bits']})"
    if k=='and_mask_u32':return inner&int(e['mask'],16) if isinstance(inner,int) else f"({inner}&{e['mask']})"
    if k=='leaf_read':return e['register']
    return '<'+k+'>'


class ManualTableTests(unittest.TestCase):
    def test_module_tables_equal_independent_manual_transcription(self):
        self.assertEqual(set(MFSPR),MANUAL_MFSPR);self.assertEqual(set(MTSPR),MANUAL_MTSPR);self.assertEqual(set(MFTB),MANUAL_MFTB)
        self.assertEqual((MFSPR[1008],MFSPR[920],MTSPR[921],MTSPR[284]),(('HID0','supervisor'),('HID2','supervisor'),('WPAR','supervisor'),('TBL','supervisor')))
        self.assertEqual({n for n,(_,level) in MFSPR.items() if level=='user'},{1,8,9,936,937,938,939,940,941,942,943})
    def test_read_only_write_only_and_split_field_numbers_refused(self):
        for word in (mtspr(287,3),mfspr(3,284),mfspr(3,285),mfspr(3,268),mftb(3,270),mfspr(3,976),mtspr(976,3),mfspr(3,0)):
            with self.assertRaises(ValueError):decode_transfer(word)
    def test_rlwinm_mask_matches_independent_formula_for_all_mb_me(self):
        for mb in range(32):
            for me in range(32):
                low=lambda k:0 if k>=32 else 0xffffffff>>k
                expected=(low(mb)&~low(me+1)&0xffffffff) if mb<=me else (~(low(me+1)&~low(mb))&0xffffffff)
                self.assertEqual(rlwinm_mask(rlwinm(3,4,0,mb,me))['mask'],expected,(mb,me))
        for word in (rlwinm(3,3,1,1,31),rlwinm(3,3,0,1,31,rc=1),ori(3,3,0)):self.assertIsNone(rlwinm_mask(word))


class FixtureLeafTests(unittest.TestCase):
    def leaf(self,first,before=None):
        words=([before] if before is not None else [])+[first,BLR]
        return recognize_register_transfer_leaf(dol(words),BASE+(4 if before is not None else 0))
    def test_read_write_msr_and_timebase_contracts(self):
        q=self.leaf(mfspr(3,1008));self.assertEqual(q['classification'],'PROVEN')
        self.assertEqual(q['shared_semantics']['gprs_written_on_normal_return'],[3]);self.assertTrue(q['differences']['privileged_execution_required'])
        q=self.leaf(mtspr(1017,3));self.assertEqual((q['shared_semantics']['transfer'],q['shared_semantics']['gprs_written_on_normal_return']),('L2CR <- r3',[]))
        q=self.leaf(mftb(5,269));self.assertEqual((q['differences']['register'],q['differences']['access_level'],q['differences']['gpr_is_r3']),('TBU','user',False))
        self.assertEqual(self.leaf(mfmsr(3))['classification'],'PROVEN')
    def test_mtmsr_is_not_promoted_without_independent_gpr_agreement(self):
        q=self.leaf(mtmsr(3))
        self.assertEqual((q['classification'],q['evidence']['canonical_leaf_gpr_effect']),('STRONGLY SUPPORTED','UNKNOWN'))
        self.assertEqual(q['shared_semantics']['other_gprs_unchanged_on_normal_return'],'recognizer field decode only')
        self.assertIn('p.475',q['architectural_notes'][0])
    def test_scope_never_claims_side_effects_values_or_retirement(self):
        s=self.leaf(mtspr(22,3))['scope']
        for key in ('register_values_or_domains_proven','hardware_side_effects_modeled','privilege_program_or_interrupt_delivery_proven',
                    'asynchronous_intervention_between_words_excluded','source_entry_identity_or_all_callers_proven','native_retirement_proven','deletion_authorized'):
            self.assertFalse(s[key],key)
    def test_record_bit_forms_refused_even_though_shared_decoder_accepts_them(self):
        for word in (mfspr(3,1008)|1,mtspr(1008,3)|1,mftb(3)|1,mfmsr(3)|1,mtmsr(3)|1):
            if word>>1&1023 in (339,371,83):self.assertIsNotNone(gpr_dependencies(word))
            with self.assertRaises(ValueError):self.leaf(word)
    def test_lr_write_and_reserved_msr_fields_refused(self):
        for word in (mtspr(8,3),mtspr(8,0),mfmsr(3)|1<<16,mfmsr(3)|1<<11,mtmsr(3)|1<<20,mtmsr(3)|1<<15):
            with self.assertRaises(ValueError):self.leaf(word)
        self.assertEqual(self.leaf(mfspr(3,8))['shared_semantics']['transfer'],'r3 <- LR')
        self.assertEqual(self.leaf(mtspr(9,3))['shared_semantics']['transfer'],'CTR <- r3')
    def test_return_word_variants_and_other_platform_words_refused(self):
        for second in (0x4e800021,0x4e800420,0x4d820020,0x60000000,0x4c000064):
            with self.assertRaises(ValueError):recognize_register_transfer_leaf(dol([mfspr(3,1008),second]),BASE)
        for first in (0x44000002,0xffa0004c,0x4c00012c,0x7c0004ac,0x7c0007ec,addi(3,3,1)):
            with self.assertRaises(ValueError):self.leaf(first)
    def test_entry_bounds_types_and_truncation_refused(self):
        raw=dol([mfspr(3,1008),BLR])
        for entry in (True,BASE+2,BASE+4,BASE-4,0):
            with self.assertRaises(ValueError):recognize_register_transfer_leaf(raw,entry)
        with self.assertRaises(ValueError):recognize_register_transfer_leaf(dol([mfspr(3,1008)]),BASE)
    def test_isolation_requires_a_terminal_predecessor_and_keeps_sc_open(self):
        for before,isolated in ((BLR,True),(branch(-64),True),(0x4c000064,True),(addi(4,4,1),False),(branch(64,link=True),False),
                                (conditional(64),False),(0x44000002,False),(0x00000000,False),(0x4e800021,False)):
            q=self.leaf(mfspr(3,1008),before)
            self.assertEqual(q['entry_context']['isolated_leaf'],isolated,hex(before))
            self.assertEqual(q['classification'],'PROVEN' if isolated else 'STRUCTURAL MATCH')
        self.assertTrue(self.leaf(mfspr(3,1008))['entry_context']['isolated_leaf'])


class MutationTests(unittest.TestCase):
    REPRESENTATIVE=(mfspr(3,1008),mtspr(1017,3),mfmsr(3),mtmsr(3),mftb(3,268),mtspr(22,3),mtspr(921,3),mfspr(3,920))
    def test_every_transfer_word_bit_is_refused_or_changes_manual_identity(self):
        changed=refused=0
        for word in self.REPRESENTATIVE:
            original=manual_identity(word);self.assertIsNotNone(original)
            for bit in range(32):
                mutant=word^(1<<bit);oracle=manual_identity(mutant)
                try:q=recognize_register_transfer_leaf(dol([mutant,BLR]),BASE)
                except ValueError:
                    self.assertIsNone(oracle,(hex(word),bit));refused+=1;continue
                self.assertEqual(identity(q),oracle,(hex(word),bit));self.assertNotEqual(identity(q),original,(hex(word),bit))
                changed+=1
        self.assertEqual(changed+refused,256);self.assertGreater(refused,changed)
    def test_every_return_word_bit_is_refused(self):
        for word in self.REPRESENTATIVE:
            for bit in range(32):
                with self.assertRaises(ValueError):recognize_register_transfer_leaf(dol([word,BLR^(1<<bit)]),BASE)


class FrontierFixtureTests(unittest.TestCase):
    # Leaves at BASE: HID0 read/write, WPAR write, HID2 read/write, MSR write, L2CR read/write.
    LEAVES=[mfspr(3,1008),BLR,mtspr(1008,3),BLR,mtspr(921,3),BLR,mfspr(3,920),BLR,mtspr(920,3),BLR,mtmsr(3),BLR,mfspr(3,1017),BLR,mtspr(1017,3),BLR]
    HID0R,HID0W,WPARW,HID2R,HID2W,MSRW,L2R,L2W=(BASE+8*i for i in range(8))
    def frontier(self,body):
        words=list(self.LEAVES);at=BASE+4*len(words);encoded=[]
        for i,item in enumerate(body):
            pc=at+4*i
            encoded.append(call(pc,item[1]) if isinstance(item,tuple) and item[0]=='bl' else
                           conditional(item[1]-pc,bo=4,link=True) if isinstance(item,tuple) else item)
        return build_transfer_caller_frontier(dol(words+encoded+[BLR]))
    def writes(self,f):return {c['register']:c for c in f['calls'] if c['direction']=='write'}
    def test_read_modify_write_or_through_leaves(self):
        f=self.frontier([('bl',self.HID0R),ori(3,3,0x200),('bl',self.HID0W)]);w=self.writes(f)['HID0']
        self.assertEqual(render(w['argument']),'(HID0|0x00000200)');self.assertTrue(w['read_modify_write_same_register'])
        self.assertEqual((w['argument_resolution'],w['argument_classification']),('COMPLETE','STRONGLY SUPPORTED'))
        reads=[c for c in f['calls'] if c['direction']=='read'];self.assertEqual(reads[0]['consumed_by_reviewed_write_calls'],[w['site']])
    def test_conditional_read_call_is_ambiguous(self):
        f=self.frontier([('bc',self.HID0R),ori(3,3,0x200),('bl',self.HID0W)]);w=self.writes(f)['HID0']
        self.assertEqual(w['argument_resolution'],'PARTIAL');self.assertFalse(w['read_modify_write_same_register'])
    def test_direct_destination_severs_the_chain(self):
        body=[('bl',self.HID0R),ori(3,3,0x200),('bl',self.HID0W),BLR,branch(-12)]
        f=self.frontier(body);w=self.writes(f)['HID0']
        self.assertEqual(render(w['argument']),'(<unresolved_boundary>|0x00000200)');self.assertEqual(w['argument_resolution'],'PARTIAL')
    def test_preserving_leaf_chain_and_literal_write_gather_address(self):
        f=self.frontier([('bl',self.HID2R),mr(28,3),lis(3,0x0c01),addi(3,3,-0x8000),('bl',self.WPARW),oris(3,28,0x4000),('bl',self.HID2W)])
        w=self.writes(f);self.assertEqual(render(w['WPAR']['argument']),0x0c008000)
        self.assertEqual(render(w['HID2']['argument']),'(HID2|0x40000000)');self.assertTrue(w['HID2']['read_modify_write_same_register'])
        self.assertEqual(w['HID2']['argument']['of']['kind'],'preserved_across_leaf')
    def test_strongly_supported_mtmsr_leaf_does_not_carry_preservation(self):
        f=self.frontier([('bl',self.HID2R),mr(28,3),('bl',self.MSRW),oris(3,28,0x4000),('bl',self.HID2W)])
        w=self.writes(f)['HID2'];self.assertEqual(w['argument_resolution'],'PARTIAL')
        self.assertIn('not independently agreed',json.dumps(w['argument']))
    def test_reviewed_mask_completion_rules(self):
        f=self.frontier([('bl',self.L2R),rlwinm(3,3,0,1,31),('bl',self.L2W)])
        self.assertEqual(render(self.writes(f)['L2CR']['argument']),'(L2CR&0x7fffffff)')
        f=self.frontier([('bl',self.L2R),oris(0,3,0x8000),rlwinm(3,0,0,11,9),('bl',self.L2W)])
        w=self.writes(f)['L2CR'];self.assertEqual(render(w['argument']),'((L2CR|0x80000000)&0xffdfffff)');self.assertEqual(w['reviewed_mask_completions'],1)
    def test_mask_completion_refuses_ambiguous_or_unreviewed_forms(self):
        for body in ([('bl',self.L2R),rlwinm(3,3,1,1,31),('bl',self.L2W)],
                     [('bl',self.L2R),rlwinm(3,3,0,1,31,rc=1),('bl',self.L2W)],
                     [('bl',self.L2R),oris(0,3,0x8000),rlwinm(3,0,0,11,9),addi(0,0,1),('bl',self.L2W)],
                     [('bl',self.L2R),addi(4,4,1),rlwinm(3,3,0,1,31),('bl',self.L2W)]):
            w=self.writes(self.frontier(body))['L2CR']
            self.assertEqual(w['argument_resolution'],'PARTIAL');self.assertEqual(w['reviewed_mask_completions'],0)
        f=self.frontier([('bl',self.L2R),rlwinm(3,3,0,1,31),('bl',self.L2W),BLR,branch(-12)])
        self.assertEqual(self.writes(f)['L2CR']['reviewed_mask_completions'],0)
    def test_non_leaf_call_severs_and_scope_stays_source_only(self):
        f=self.frontier([('bl',self.HID0R),mr(28,3),('bl',BASE+0x1000),oris(3,28,0),('bl',self.HID0W)])
        w=self.writes(f)['HID0'];self.assertIn('not a reviewed isolated transfer leaf',json.dumps(w['argument']))
        self.assertFalse(f['scope']['native_ownership_or_retirement_proven']);self.assertFalse(f['scope']['callers_complete_or_live_proven'])


class OriginalImageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.raw=ORIGINAL.read_bytes()
        cls.report=build_transfer_report(cls.raw,(Path(__file__).resolve().parents[1]/'extract/functions.csv').read_bytes())
    def test_original_hash_and_source_bound_training_held_out_references(self):
        self.assertEqual(hashlib.sha256(self.raw).hexdigest(),ORIGINAL_SHA)
        refs=json.loads(REFERENCES.read_text(encoding='utf-8'));self.assertEqual(refs['dol_sha256'],ORIGINAL_SHA)
        self.assertEqual({r['role'] for r in refs['references']},{'training','held-out','held-out-suffix'})
        for ref in refs['references']:
            entry=int(ref['entry'],16);words=[self.raw[ref['file_offset']+4*i:ref['file_offset']+4*i+4].hex() for i in (0,1)]
            self.assertEqual(['0x'+w for w in words],ref['original_words'],ref['entry'])
            q=recognize_register_transfer_leaf(self.raw,entry);e=ref['expected'];d=q['differences']
            self.assertEqual((d['operation'],d['direction'],d['register'],d['register_number'],d['gpr'],q['classification']),
                             (e['operation'],e['direction'],e['register'],e['register_number'],e['gpr'],e['classification']),ref['entry'])
    def test_whole_original_scan_census_and_retained_refusals(self):
        s=self.report['scan']['summary']
        self.assertEqual((s['original_text_words'],s['matches'],s['isolated_leaves'],s['refused']),(1216776,29,19,472))
        self.assertEqual(s['classifications'],{'PROVEN':17,'STRONGLY SUPPORTED':2,'STRUCTURAL MATCH':10})
        self.assertEqual(list(s['refusal_reasons']),['LR write redirects the following return: control transfer, not a leaf'])
        other={x['site']:x['effect']['operation'] for x in self.report['scan']['unreviewed_platform_return_pairs']}
        self.assertEqual(sorted(other.values()),['isync']*4+['mtfsb1','mtfsf']+['sc']*3+['sync'])
        msr_reads=[c['entries'] for c in self.report['scan']['clusters'] if c['operation']=='mfmsr']
        self.assertEqual(msr_reads,[['0x80370adc','0x8039d9b8']])
    def test_original_caller_frontier_recovers_leaf_mediated_data_flow(self):
        f=self.report['caller_frontier'];s=f['summary']
        self.assertEqual((s['direct_call_sites'],s['write_calls'],s['complete_write_arguments'],s['read_modify_write_same_register'],s['reviewed_mask_completions']),(72,39,25,12,5))
        w={c['site']:render(c['argument']) for c in f['calls'] if c['direction']=='write'}
        expected={'0x80370bd4':'(HID0|0x00000200)','0x80371034':'(HID2&0xbfffffff)','0x80371728':'(HID2|0xa0000000)',
            '0x8037265c':'(L2CR&0x7fffffff)','0x8037266c':'(L2CR|0x00200000)','0x80372690':'(L2CR&0xffdfffff)',
            '0x803728e8':'((L2CR|0x80000000)&0xffdfffff)','0x80393b50':0x0c008000,'0x80393b58':'(HID2|0x40000000)',
            '0x8039eb74':'(MSR|0x00002000)','0x803728b4':0x30,'0x80370ec0':0}
        for site,value in expected.items():self.assertEqual(w[site],value,site)
        self.assertEqual(sorted(c['site'] for c in f['calls'] if c['register'] in ('MMCR0','MMCR1','PMC1','PMC2','PMC3','PMC4')),
                         ['0x80370ec0','0x80370ec8','0x80370ed0','0x80370ed8','0x80370ee0','0x80370ee8'])
        tb=[c for c in f['calls'] if c['register']=='TBL'];self.assertEqual(len(tb),12)
        self.assertTrue(all(not c['consumed_by_reviewed_write_calls'] for c in tb))
    def test_scan_is_repeatable_and_bound_to_the_original(self):
        again=scan_register_transfer_leaves(self.raw)
        self.assertEqual(json.dumps(again,sort_keys=True),json.dumps(self.report['scan'],sort_keys=True))
        self.assertEqual(again['inputs']['dol_sha256'],ORIGINAL_SHA)


if __name__=='__main__':unittest.main()
