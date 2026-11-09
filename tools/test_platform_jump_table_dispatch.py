import hashlib,unittest
from pathlib import Path
from platform_jump_table_dispatch import BCTR,Context,find_guard,may_write_cr_field,recognize_dispatch,scan_dispatches
from test_graphics_lift_inventory import BASE,BLR,branch,conditional,dol

ORIGINAL=Path(__file__).resolve().parents[2]/'game/sys/main.dol'
FUNCTIONS=Path(__file__).resolve().parents[1]/'extract/functions.csv'
TABLE=0x80100000
def lis(d,v):return 15<<26|d<<21|(v&65535)
def addi(d,a,v):return 14<<26|d<<21|a<<16|(v&65535)
def cmplwi(r,v,field=0):return 10<<26|field<<23|r<<16|(v&65535)
def cmpwi(r,v):return 11<<26|r<<16|(v&65535)
def slwi(a,s,n):return 21<<26|s<<21|a<<16|n<<11|0<<6|(31-n)<<1
def lwzx(d,a,b):return 31<<26|d<<21|a<<16|b<<11|23<<1
def lwz(d,disp,a):return 32<<26|d<<21|a<<16|(disp&65535)
MTCTR0=0x7C0903A6
BGTLR=0x4D810020
def body(order='reviewed',guard_words=None,between=()):
    g=guard_words or [cmplwi(3,2),conditional(0x40,bo=12,bi=1)]
    hi,lo,sc=lis(4,TABLE>>16),addi(4,4,TABLE&0xFFFF),slwi(0,3,2)
    middle={'reviewed':[hi,lo,sc],'reordered':[hi,sc,lo]}[order]
    return g+list(between)+middle+[lwzx(0,4,0),MTCTR0,BCTR]
def image(words,table=None,data_at=TABLE):
    targets=table if table is not None else [BASE+0x60,BASE+0x64,BASE+0x68]
    padded=list(words)+[BLR]*(0x70//4-len(words))
    return dol(sections=[(0,BASE,padded),(7,data_at,targets)])
def recognize(words,**kw):
    raw=image(words,**kw);return recognize_dispatch(Context(raw,{BASE}),BASE+4*(len(words)-1))


def guard_of(words):
    table={BASE+4*i:w for i,w in enumerate(words)}
    return find_guard(table,BASE,BASE+4*len(words))

class GuardTests(unittest.TestCase):
    def test_unsigned_greater_and_greater_equal_forms(self):
        self.assertEqual(guard_of([cmplwi(3,5),conditional(16,bo=12,bi=1)])[2:4],(3,5))
        self.assertEqual(guard_of([cmplwi(3,5,field=2),conditional(16,bo=4,bi=8)])[2:4],(3,4))
        self.assertEqual(guard_of([cmplwi(3,5),BGTLR])[4]['mnemonic'],'bclr')
        for words in ([cmpwi(3,5),conditional(16,bo=12,bi=1)],[cmplwi(3,5)|1<<21,conditional(16,bo=12,bi=1)],
                      [cmplwi(3,5),conditional(16,bo=12,bi=0)],[cmplwi(3,5),conditional(16,bo=12,bi=1,link=True)],
                      [cmplwi(3,0),conditional(16,bo=4,bi=0)]):
            self.assertIsNone(guard_of(words))
    def test_scheduled_guard_skips_only_words_that_cannot_write_the_field(self):
        stw,andi_dot,rlwinm_dot=36<<26|0<<21|1<<16|0x14,28<<26|5<<21|6<<16|1,slwi(6,5,1)|1
        self.assertEqual(guard_of([cmplwi(3,5),stw,addi(4,4,1),conditional(16,bo=12,bi=1)])[2:4],(3,5))
        self.assertIsNone(guard_of([cmplwi(3,5),andi_dot,conditional(16,bo=12,bi=1)]))
        self.assertIsNone(guard_of([cmplwi(3,5),rlwinm_dot,conditional(16,bo=12,bi=1)]))
        self.assertEqual(guard_of([cmplwi(3,5,field=1),andi_dot,conditional(16,bo=12,bi=5)])[2:4],(3,5))
        self.assertIsNone(guard_of([cmplwi(3,5),cmpwi(4,1),conditional(16,bo=12,bi=1)]))
        self.assertTrue(may_write_cr_field(0x7C0FF120,3));self.assertFalse(may_write_cr_field(addi(3,3,1),0))


class DispatchTests(unittest.TestCase):
    def test_reviewed_and_reordered_layouts_give_the_same_contract(self):
        a,b=recognize(body('reviewed')),recognize(body('reordered'))
        for c in (a,b):
            self.assertEqual((c['admitted_unsigned_maximum'],c['original_table_address'],c['targets']),
                             (2,f'0x{TABLE:08x}',[f'0x{BASE+0x60:08x}',f'0x{BASE+0x64:08x}',f'0x{BASE+0x68:08x}']))
            self.assertTrue(c['targets_in_text']);self.assertEqual(c['classification'],'STRUCTURAL MATCH')
    def test_conditional_return_and_greater_equal_guards(self):
        c=recognize(body(guard_words=[cmplwi(3,2),BGTLR]))
        self.assertEqual((c['guard_exit'],c['admitted_unsigned_maximum']),('conditional_return',2))
        c=recognize(body(guard_words=[cmplwi(3,3),conditional(0x40,bo=4,bi=0)]))
        self.assertEqual(c['admitted_unsigned_maximum'],2)
    def test_unrelated_decoded_words_inside_the_window_are_allowed(self):
        self.assertIsInstance(recognize(body(between=[addi(7,7,1),lwz(8,0,9)])),dict)
        interleaved=[cmplwi(3,2),addi(7,7,1),lwz(8,0,9),conditional(0x40,bo=12,bi=1)]
        self.assertEqual(recognize(body(guard_words=interleaved))['admitted_unsigned_maximum'],2)
        redefined=[cmplwi(3,2),addi(3,3,1),conditional(0x40,bo=12,bi=1)]
        self.assertEqual(recognize(body(guard_words=redefined)),'index redefined between guard and scaling')
    def test_refusals(self):
        cases={'no unsigned bounded guard in the window':body(guard_words=[cmpwi(3,2),conditional(0x40,bo=12,bi=1)]),
               'index redefined between guard and scaling':body(between=[addi(3,3,1)]),
               'lwzx does not read the scaled guarded index':body()[:4]+[slwi(0,3,3)]+body()[5:],
               'mtctr does not immediately precede bctr':body()[:-2]+[MTCTR0,addi(7,7,1),BCTR],
               'guard exit re-enters the dispatch window':body(guard_words=[cmplwi(3,2),conditional(12,bo=12,bi=1)])}
        for reason,words in cases.items():
            self.assertEqual(recognize(words),reason)
        words=body();raw=image(words+[branch(-4*len(words)+12)])
        self.assertEqual(recognize_dispatch(Context(raw,{BASE}),BASE+4*(len(words)-1)),'direct branch destination inside the guarded window')
        loaded=body()[:2]+[lwz(4,0,9),addi(4,4,0)]+body()[4:]
        self.assertEqual(recognize(loaded),'table base is not an exact canonical constant')
        self.assertEqual(recognize(body(),table=[BASE+0x60,BASE+0x64]),'table is not entirely original file-backed storage')
    def test_not_a_bctr_and_misaligned_sites(self):
        raw=image(body());context=Context(raw,{BASE})
        self.assertEqual(recognize_dispatch(context,BASE),'not an unconditional bctr')
        with self.assertRaises(ValueError):recognize_dispatch(context,BASE+2)


class OriginalImageTests(unittest.TestCase):
    def test_whole_original_scan_agrees_with_every_reviewed_switch(self):
        raw=ORIGINAL.read_bytes()
        self.assertEqual(hashlib.sha256(raw).hexdigest(),'fde4fa6f81a60313b710161c196dc51c2260be62251ee02775d5eee06f9d55af')
        report=scan_dispatches(raw,FUNCTIONS.read_bytes());s=report['summary']
        self.assertEqual((s['bctr_sites'],s['reviewed_recognizer_sites'],s['reviewed_sites_agreeing'],s['reviewed_sites_missed']),(206,10,10,0))
        self.assertTrue(s['all_targets_in_text']);self.assertGreater(s['recognized'],10)
        self.assertEqual(s['recognized']+sum(s['refusal_reasons'].values()),206)


if __name__=='__main__':unittest.main()
