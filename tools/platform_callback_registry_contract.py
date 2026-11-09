"""Original four-slot registry effects; no host registry or device emulation.

Exact source sequences establish duplicate-before-empty search, first removal,
activation on EVERY insertion, and last-removal deactivation. Tables, callees,
faults, callbacks, asynchronous exclusion and native lifetime remain separate.
"""
import argparse,hashlib,json
from pathlib import Path
if __package__:
    from .graphics_lift_inventory import Dol,decode,hx
    from .platform_callback_toggle_contract import DISABLE,RESTORE
else:
    from graphics_lift_inventory import Dol,decode,hx
    from platform_callback_toggle_contract import DISABLE,RESTORE

REGISTER=(0x7C0802A6,0x90010004,0x9421FFE8,0x93E10014,0x93C10010,0x7C7E1B78,None,None,0x38000004,None,0x7C0903A6,0x3BE30000,0x38640000,0x80030000,0x7C00F040,0x40820014,0x7FE3FB78,None,0x38600001,0x48000068,0x38630004,0x4200FFE0,0x38000004,0x7C0903A6,0x38A00000,0x80040000,0x28000000,0x40820030,None,0x54A4103A,None,0x7C602214,0x93C30000,0x38600001,None,0x7FE3FB78,None,0x38600001,0x4800001C,0x38840004,0x38A50001,0x4200FFC0,0x7FE3FB78,None,0x38600000,0x8001001C,0x83E10014,0x83C10010,0x38210018,0x7C0803A6,0x4E800020)
REMOVE=(0x7C0802A6,0x90010004,0x9421FFE8,0x93E10014,0x93C10010,0x7C7E1B78,None,None,0x38000004,None,0x7C0903A6,0x3BE30000,0x38650000,0x38800000,0x80030000,0x7C00F040,0x40820084,None,0x5484103A,None,0x7C602214,0x38000000,0x90030000,0x38800000,0x80050000,0x28000000,0x4082003C,0x38650004,0x80050004,0x38800001,0x28000000,0x40820028,0x84030004,0x38800002,0x28000000,0x40820018,0x80030004,0x38800003,0x28000000,0x40820008,0x38800004,0x2C040004,0x4082000C,0x38600000,None,0x7FE3FB78,None,0x38600001,0x4800001C,0x38630004,0x38840001,0x4200FF6C,0x7FE3FB78,None,0x38600000,0x8001001C,0x83E10014,0x83C10010,0x38210018,0x7C0803A6,0x4E800020)
ACTIVATE=(0x7C0802A6,None,0x90010004,0x9421FFD0,0x93E1002C,None,0x93C10028,0x93A10024,0x3BA30000,None,0x3C80CC00,0x84A46434,0x54A00109,0x4182000C,0x3BC00001,0x48000008,0x3BC00000,0x2C1D0000,0x41820020,0x38000000,0x901F01E0,0x64A50800,0x901F01E4,0x901F01E8,0x901F01EC,0x48000008,0x54A50146,0x54A5007C,0x90A40000,None,0x7FC3F378,0x80010034,0x83E1002C,0x83C10028,0x83A10024,0x38210030,0x7C0803A6,0x4E800020)


def pins():
    names=('platform_callback_registry_contract.py','platform_callback_toggle_contract.py','graphics_lift_inventory.py')
    return {n:hashlib.sha256(Path(__file__).with_name(n).read_bytes()).hexdigest() for n in names}


def recognize_registry(raw,register_entry,remove_entry):
    image=Dol(raw)
    def sequence(entry,template):
        if type(entry) is not int or entry%4 or not image.contains_text(entry,4*len(template)):
            raise ValueError('complete aligned original registry body required')
        words=[image.word(entry+4*i) for i in range(len(template))]
        if any(v is not None and words[i]!=v for i,v in enumerate(template)):raise ValueError('unreviewed registry sequence')
        return words
    def address(words,hi,lo,hi_mask,lo_mask):
        a,b=words[hi],words[lo]
        if a&0xffff0000!=hi_mask or b&0xffff0000!=lo_mask:raise ValueError('reviewed literal address construction required')
        n=b&65535;n=n-65536 if n&32768 else n
        return (((a&65535)<<16)+n)&0xffffffff
    def call(entry,words,i):
        edge=decode(words[i],entry+4*i)
        if not edge or edge['mnemonic']!='b' or not edge['link'] or edge['absolute'] or not image.contains_text(edge['target'],4):
            raise ValueError('original relative direct call required')
        return edge['target']
    def original(entry,words):
        return [dict(site=hx(entry+4*i),word=hx(w),file_offset=image.offset(entry+4*i)) for i,w in enumerate(words)]
    reg=sequence(register_entry,REGISTER);rem=sequence(remove_entry,REMOVE)
    table=address(reg,7,9,0x3c800000,0x38840000)
    tables=[table,address(reg,28,30,0x3c600000,0x38030000),address(rem,7,9,0x3c800000,0x38a40000),address(rem,17,19,0x3c600000,0x38030000)]
    if len(set(tables))!=1 or table%4 or table+16>0x100000000:raise ValueError('same aligned nonwrapping four-slot table required')
    disable=call(register_entry,reg,6);restore=call(register_entry,reg,17);activate=call(register_entry,reg,34)
    if call(remove_entry,rem,6)!=disable or call(remove_entry,rem,44)!=activate:raise ValueError('registry owner calls differ')
    for e,words,indices in ((register_entry,reg,(36,43)),(remove_entry,rem,(46,53))):
        if any(call(e,words,i)!=restore for i in indices):raise ValueError('normal restore leaf differs')
    act=sequence(activate,ACTIVATE)
    backing=address(act,1,5,0x3c800000,0x3be40000)
    if backing%4 or backing+0x1f0>0x100000000:raise ValueError('aligned nonwrapping activation backing required')
    if call(activate,act,9)!=disable or call(activate,act,29)!=restore:raise ValueError('activation exclusion leaves differ')
    leaves=[]
    for entry,template,writes in ((disable,DISABLE,[3,4]),(restore,RESTORE,[3,4,5])):
        words=sequence(entry,template)
        leaves.append(dict(entry=hx(entry),normal_path_gpr_writes=writes,original_words=original(entry,words),hardware_and_async_effects_native=False))
    # The original table may be BSS/unmapped in the DOL. Never fabricate its
    # initial contents from a zero sentinel or an observed checkpoint.
    table_bytes=None
    for s in image.sections:
        if s['address']<=table and table+16<=s['address']+s['size']:
            off=s['file_offset']+table-s['address'];table_bytes=raw[off:off+16].hex();break
    return dict(schema='platform_callback_registry_source_v1',classification='PROVEN',
        proof_scope='Exact bounded source control/memory/call order only; normal access/return premises.',
        inputs=dict(dol_sha256=hashlib.sha256(raw).hexdigest(),source_pins=pins()),
        bodies=[dict(kind='register',entry=hx(register_entry),original_words=original(register_entry,reg)),dict(kind='remove',entry=hx(remove_entry),original_words=original(remove_entry,rem)),dict(kind='activation',entry=hx(activate),original_words=original(activate,act))],
        exclusion_leaves=leaves,table=dict(original_address=hx(table),slots=4,stride_bytes=4,zero_is_empty=True,file_backed_initial_bytes=table_bytes,boot_initial_contents_proven=False),
        register_cases=dict(duplicate='Search all four for input first; return1, no table store or activation.',
            absent_with_space='Store input in first empty slot; call activation(1) even if other slots already nonzero; restore; return1.',
            absent_and_full='Restore; return0; no table store or activation.',
            zero_input='An empty slot counts as an existing duplicate; return1 without insertion. All nonzero slots: return0.'),
        remove_cases=dict(found='Clear only first matching slot, then inspect all four. Call activation(0) only if all are zero; restore; return1.',
            absent='Restore; return0; no table store or activation.',
            duplicate_slots='Later duplicates remain; there is no remove-all operation.',
            zero_input='First empty slot is a match; still writes zero and checks whole-table emptiness.'),
        activation=dict(entry=hx(activate),control_address='0xcc006434',sampling_backing=hx(backing),
            unconditional_on_nonzero_input_zero_store_addresses=[hx(backing+o) for o in (0x1e0,0x1e4,0x1e8,0x1ec)],
            repeated_enable_is_noop=False,previous_enable_bit_returned=True,
            zero_input_does_not_clear_sampling_backing=True,nonzero_input_sets_control_bit=0x08000000,
            zero_input_clears_control_bit=0x08000000,final_control_mask=0x7ffffffe),
        scope=dict(source_only=True,source_capacity_is_observable_not_arbitrary_host_pool=True,
            stack_table_backing_or_device_alias_closed=False,actual_publication_or_failure_rollback_proven=False,
            interrupted_table_epochs_callback_delivery_or_lifetime_proven=False,
            full_cpu_fault_retirement_state_pixel_or_audio_parity_proven=False,
            native_ownership_or_retirement_proven=False,deletion_authorized=False))


def scan_registries(raw,*,max_pair_attempts=None):
    image=Dol(raw);regs=[];removes=[];candidates=0
    for s in image.text:
        for pc in range(s['address'],s['address']+s['size']-36,4):
            if image.word(pc)!=0x7c0802a6 or image.word(pc+32)!=0x38000004:continue
            if [image.word(pc+4*i) for i in range(6)]!=list(REGISTER[:6]):continue
            candidates+=1
            # Discriminating original address-preparation opcodes; exact bodies
            # and both complete callees are checked by the final recognizer.
            op=image.word(pc+36)&0xffff0000
            if op==0x38840000:regs.append(pc)
            if op==0x38a40000:removes.append(pc)
    text_words=sum(s['size']//4 for s in image.text)
    budget=text_words if max_pair_attempts is None else max_pair_attempts
    if type(budget) is not int or budget<=0:raise ValueError('positive diagnostic work budget required')
    if len(regs)*len(removes)>budget:raise ValueError('pair work exceeds source-derived diagnostic budget; no partial proof emitted')
    pairs=[]
    for a in regs:
        for b in removes:
            try:pairs.append(recognize_registry(raw,a,b))
            except ValueError:continue
    return dict(schema='platform_callback_registry_scan_v1',dol_sha256=hashlib.sha256(raw).hexdigest(),source_pins=pins(),
        summary=dict(source_text_words=text_words,candidates=candidates,register_candidates=len(regs),remove_candidates=len(removes),pair_attempts=len(regs)*len(removes),pair_work_budget=budget,reviewed_registry_pairs=len(pairs)),
        pairs=pairs,scope=dict(source_only=True,live_or_alias_or_lifetime_closure_proven=False,native_retirement_proven=False))


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--dol',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
    a=p.parse_args()
    if a.out.exists():raise ValueError('preserve prior evidence')
    before=pins();raw=a.dol.read_bytes();r=scan_registries(raw)
    if a.dol.read_bytes()!=raw or before!=pins():raise ValueError('input or tool changed')
    with a.out.open('x',encoding='utf-8') as f:json.dump(r,f,indent=2,allow_nan=False);f.write('\n')
    print(json.dumps(r['summary']))
if __name__=='__main__':main()
