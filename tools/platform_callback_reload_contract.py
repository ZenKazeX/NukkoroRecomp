"""Recognize tested-field / helper-pair / reloaded-callback source contracts.

The first read controls entry to a block. Arbitrary callees separate that read
from the invoked read: no nonnull, base identity, context, ABI or lifetime fact
crosses those calls. This is a source pattern, never a native retirement proof.
"""
import argparse,hashlib,json
from pathlib import Path
if __package__:
    from .graphics_lift_inventory import Dol,decode,hx
else:
    from graphics_lift_inventory import Dol,decode,hx


def pins():
    return {n:hashlib.sha256(Path(__file__).with_name(n).read_bytes()).hexdigest()
            for n in ('platform_callback_reload_contract.py','graphics_lift_inventory.py')}


def recognize_reload(raw,entry):
    image=Dol(raw)
    if type(entry) is not int or entry%4 or not image.contains_text(entry,4):
        raise ValueError('aligned original text required')
    used=[]
    def word():
        pc=entry+4*len(used)
        if len(used)>=24 or not image.contains_text(pc,4):
            raise ValueError('complete bounded original sequence required')
        w=image.word(pc);used.append((pc,w));return pc,w
    def exact(expected):
        pc,w=word()
        if w!=expected:raise ValueError('source sequence differs')
        return pc
    def preparation(w):
        op,d=w>>26,(w>>21)&31
        # Only the observed non-update integer loads / byte store are admitted.
        # Their literal reads/writes remain in the evidence; never call them NOPs.
        return op in (32,40) and d not in (0,13) or op==38
    def call():
        pc,w=word();edge=decode(w,pc)
        if not edge or edge['mnemonic']!='b' or edge['conditional'] or not edge['link'] or edge['absolute'] or edge['target'] is None:
            raise ValueError('original relative direct call required')
        if not image.contains_text(edge['target'],4):raise ValueError('helper target not original text')
        return dict(site=hx(pc),target=hx(edge['target']))
    tested_site,w=word()
    if w&0xffff0000!=0x800d0000:raise ValueError('tested scalar current-r13 field load required')
    field=w&65535;prep=[]
    # Keep both observed alternatives: memory operation before or after cmp.
    for _ in range(3):
        pc,w=word()
        if w==0x28000000:compare_site=pc;break
        if not preparation(w):raise ValueError('precompare effects not reviewed')
        prep.append(dict(site=hx(pc),word=hx(w)))
    else:raise ValueError('bounded zero comparison required')
    for _ in range(3):
        pc,w=word();edge=decode(w,pc)
        if edge:break
        if not preparation(w):raise ValueError('postcompare effects not reviewed')
        prep.append(dict(site=hx(pc),word=hx(w)))
    else:raise ValueError('bounded original guard required')
    if w&0xffff0003!=0x41820000:raise ValueError('unlinked relative CR0-equal guard required')
    guard_site=pc;guard_target=edge['target']
    pc,context=word()
    if context&0xffff0000!=0x38610000:raise ValueError('literal stack-relative argument required')
    before=[call()];exact(context);before.append(call())
    if before[0]['target']==before[1]['target']:raise ValueError('distinct helper pair required')
    reload_site=exact(0x818d0000|field)
    pc,w=word();argument=None
    if w!=0x7d8803a6:
        if w&0xffe0ffff!=0x38600000 or (w>>16)&31 not in (30,31):
            raise ValueError('reviewed saved-register callback argument required')
        argument=dict(site=hx(pc),word=hx(w),value_or_preservation_proven=False)
        pc=exact(0x7d8803a6)
    transfer_site=pc;invoke_site=exact(0x4e800021)
    exact(context);after=[call()]
    resume_site,w=word();saved=(w>>21)&31
    if saved not in (30,31) or w!=(0x7c000378|(saved<<21)|(3<<16)|(saved<<11)):
        raise ValueError('reviewed saved-register resume argument required')
    after.append(call())
    if [x['target'] for x in before]!=[x['target'] for x in after]:
        raise ValueError('cleanup/resume helper pair differs')
    end=entry+4*len(used)
    if guard_target!=end:raise ValueError('guard must skip complete callback and cleanup block')
    words=[dict(site=hx(pc),word=hx(w),file_offset=image.offset(pc)) for pc,w in used]
    return dict(schema='platform_callback_reload_source_v1',entry=hx(entry),end=hx(end),
        classification='PROVEN',proof_scope='Literal source order and distinct reads ONLY.',
        inputs=dict(dol_sha256=hashlib.sha256(raw).hexdigest(),source_pins=pins()),
        original_words=words,preparation_effects=prep,
        field_coordinate=dict(current_base_gpr=13,displacement=field-65536 if field&32768 else field,width_bytes=4),
        tested_read=dict(site=hx(tested_site),destination_gpr=0,compare_site=hx(compare_site),guard_site=hx(guard_site),skip_target=hx(end)),
        invoked_read=dict(site=hx(reload_site),destination_gpr=12,transfer_site=hx(transfer_site),call_site=hx(invoke_site),bank='LR',nonnull_test_of_this_read=False),
        helper_calls_before_reload=before,helper_calls_after_callback=after,
        callback_argument=argument,resume_argument=dict(site=hx(resume_site),saved_gpr=saved,value_or_preservation_proven=False),
        source_order=['read_tested_field','test_zero','skip_if_zero','helper_A','helper_B','read_field_again','invoke_reloaded_value','helper_A','helper_B'],
        scope=dict(source_only=True,normal_call_returns_are_premises=True,
            earlier_nonnull_test_does_not_prove_reloaded_nonnull=True,
            same_encoded_coordinate_not_same_base_epoch_or_value=True,
            helper_effects_or_register_preservation_proven=False,
            external_entry_guard_domination_proven=False,alias_context_or_lifetime_closed=False,
            execution_fault_state_or_audio_parity_proven=False,native_ownership_or_retirement_proven=False,deletion_authorized=False))


def scan_reloads(raw):
    image=Dol(raw);matches=[];candidates=0
    for s in image.text:
        for pc in range(s['address'],s['address']+s['size'],4):
            if image.word(pc)&0xffff0000!=0x800d0000:continue
            candidates+=1
            try:matches.append(recognize_reload(raw,pc))
            except ValueError:continue
    # Literal incoming edges reveal alternate entries; indirect destinations
    # remain unresolved. Do not turn a locally observed guard into domination.
    sites={int(w['site'],16):[] for m in matches for w in m['original_words']}
    unresolved=0
    for s in image.text:
        for pc in range(s['address'],s['address']+s['size'],4):
            w=image.word(pc);edge=decode(w,pc)
            if not edge:continue
            if edge['target'] is None:unresolved+=1
            elif edge['target'] in sites:
                sites[edge['target']].append(dict(site=hx(pc),word=hx(w),link=edge['link'],conditional=edge['conditional']))
    for m in matches:
        m['literal_incoming_edges']=[dict(destination=w['site'],edges=sites[int(w['site'],16)]) for w in m['original_words'] if sites[int(w['site'],16)]]
    return dict(schema='platform_callback_reload_scan_v1',dol_sha256=hashlib.sha256(raw).hexdigest(),source_pins=pins(),
        summary=dict(source_text_words=sum(s['size']//4 for s in image.text),tested_field_load_candidates=candidates,source_contracts=len(matches),unresolved_indirect_edges=unresolved),
        matches=matches,scope=dict(original_text_only=True,indirect_or_alias_closure_proven=False,native_retirement_proven=False))


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--dol',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
    a=p.parse_args()
    if a.out.exists():raise ValueError('preserve previous evidence; output exists')
    before=pins();raw=a.dol.read_bytes();report=scan_reloads(raw)
    if a.dol.read_bytes()!=raw or pins()!=before:raise ValueError('source or tool changed during scan')
    report['input_file']=str(a.dol.resolve())
    with a.out.open('x',encoding='utf-8') as f:json.dump(report,f,indent=2,allow_nan=False);f.write('\n')
    print(json.dumps(report['summary']))
if __name__=='__main__':main()
