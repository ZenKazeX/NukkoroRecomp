"""Finite original-word callback publication and indirect-target candidates.

Both direct branch arms participate. Calls kill all facts; loads are symbolic
register-relative field reads, NEVER invented contents or alias identities.
Only literal definitions, definition-bound lis pairs and copies create value atoms.
This finite vocabulary prevents arithmetic loops from manufacturing an unbounded
set of computed constants. Fault/context/lifetime and callable identity stay open.
"""
import argparse
from collections import deque
import hashlib
import json
from pathlib import Path
import sys
if __package__:
    from .graphics_lift_inventory import Dol,decode,hx
    from .platform_procedure_contract import build_contract
    from .gqr_format_dataflow import gpr_dependencies
else:
    from graphics_lift_inventory import Dol,decode,hx
    from platform_procedure_contract import build_contract
    from gqr_format_dataflow import gpr_dependencies

UNKNOWN=('unknown',0,())
EMPTY_STATE=tuple(frozenset({UNKNOWN}) for _ in range(34))


def pins():
    names=('platform_callback_value_flow.py','graphics_lift_inventory.py','platform_procedure_contract.py',
           'gqr_format_dataflow.py','platform_effect_inventory.py','particle_numeric_contract.py','format_memory_dependencies.py')
    return {n:hashlib.sha256(Path(__file__).with_name(n).read_bytes()).hexdigest() for n in names}


def signed16(v):return v-65536 if v&32768 else v


def transfer(image,pc,state):
    w=image.word(pc);op,d,a,b=w>>26,(w>>21)&31,(w>>16)&31,(w>>11)&31
    flow=decode(w,pc);s=list(state);barrier=None
    def unknown(reg):s[reg]=frozenset({UNKNOWN})
    def constant(reg,v,origins):s[reg]=frozenset({('constant',v&0xffffffff,origins)})
    if flow and flow.get('link'):
        return EMPTY_STATE,'call'
    if op==31 and (w>>1)&1023 in (339,467) and w&1:
        return EMPTY_STATE,'unsupported'
    if op in (14,15) and a==0:
        constant(d,signed16(w&65535)*(65536 if op==15 else 1),(pc,))
    elif op in (14,24):
        source=a if op==14 else d;dest=d if op==14 else a;result=set()
        for atom in state[source]:
            valid=False
            if atom[0]=='constant' and len(atom[2])==1 and image.contains_text(atom[2][0],4):
                prior=image.word(atom[2][0])
                valid=prior>>26==15 and (prior>>16)&31==0 and (prior>>21)&31==source
            if valid:
                value=atom[1]+signed16(w&65535) if op==14 else atom[1]|(w&65535)
                result.add(('constant',value&0xffffffff,(atom[2][0],pc)))
            else:result.add(UNKNOWN)
        s[dest]=frozenset(result)
    elif op==31 and (w>>1)&1023==444 and d==b:
        s[a]=state[d]
    elif op==32:
        # Encoded field identity only. Even repeated r13 bases are NOT declared
        # the same register epoch, SDA instance, allocation or memory alias.
        s[d]=frozenset({('field_load',(a,signed16(w&65535)),(pc,))})
    elif op==31 and (w>>1)&1023 in (339,467) and not w&1:
        spr=((w>>16)&31)|(((w>>11)&31)<<5);bank=32 if spr==8 else 33 if spr==9 else None
        if (w>>1)&1023==339:s[d]=state[bank] if bank is not None else frozenset({UNKNOWN})
        elif bank is not None:s[bank]=state[d]
    else:
        deps=gpr_dependencies(w)
        if deps is None:return EMPTY_STATE,'unsupported'
        for reg in deps:unknown(reg)
    return tuple(s),barrier


def atoms(values):
    result=[]
    for kind,value,origins in sorted(values,key=repr):
        row=dict(kind=kind,original_definition_sites=[hx(p) for p in origins])
        if kind=='constant':row['value']=hx(value)
        elif kind=='field_load':row.update(base_gpr=value[0],displacement=value[1],zero_base=value[0]==0,
            loaded_value_or_base_epoch_or_alias_proven=False)
        result.append(row)
    return result


def null_skip_guard(image,pc):
    if not image.contains_text(pc-16,20):return None
    load,cmp,branch,move,call=[image.word(pc-16+4*i) for i in range(5)]
    reg=(load>>21)&31;bf=(cmp>>23)&7;edge=decode(branch,pc-8);invoke=decode(call,pc)
    spr=((move>>16)&31)|(((move>>11)&31)<<5)
    if (load>>26!=32 or cmp>>26!=10 or cmp&65535 or (cmp>>16)&31!=reg or
        cmp&(3<<21) or not edge or edge.get('link') or edge.get('bo')!=12 or
        edge.get('bi')!=4*bf+2 or edge.get('target')!=pc+4 or
        move>>26!=31 or (move>>1)&1023!=467 or move&1 or (move>>21)&31!=reg or
        spr not in (8,9) or not invoke or not invoke.get('link') or invoke.get('conditional') or
        invoke.get('target_source')!=('LR' if spr==8 else 'CTR')):return None
    return dict(load_site=hx(pc-16),compare_site=hx(pc-12),branch_site=hx(pc-8),
        skip_target=hx(pc+4),loaded_gpr=reg,base_gpr=(load>>16)&31,displacement=signed16(load&65535),
        condition='Skip indirect call when the original loaded u32 equals zero.',
        executed_or_pointer_identity_proven=False)


def build_value_flow(raw,entry):
    if type(entry) is not int:raise ValueError('original integer entry required')
    image=Dol(raw);procedure=build_contract(raw,entry)
    words={int(row['site'],16):int(row['word'],16) for row in procedure['words']};edges={}
    for pc,w in words.items():
        flow=decode(w,pc);dest=[]
        if flow and not flow.get('link') and flow.get('target') is not None:dest.append(flow['target'])
        if flow is None or flow.get('may_fall_through'):dest.append((pc+4)&0xffffffff)
        edges[pc]=[v for v in dest if v in words]
    states={entry:EMPTY_STATE};queue=deque([entry]);updates=0;barriers={}
    while queue:
        pc=queue.popleft();after,barrier=transfer(image,pc,states[pc]);updates+=1
        if barrier:barriers[pc]=barrier
        for dest in edges[pc]:
            prior=states.get(dest);merged=after if prior is None else tuple(x|y for x,y in zip(prior,after))
            if prior!=merged:states[dest]=merged;queue.append(dest)
    publications=[];calls=[];direct=[]
    for pc,w in sorted(words.items()):
        before=states[pc];op,d,a=w>>26,(w>>21)&31,(w>>16)&31;flow=decode(w,pc)
        if op==36:
            publications.append(dict(site=hx(pc),word=hx(w),file_offset=image.offset(pc),
                source_gpr=d,base_gpr=a,displacement=signed16(w&65535),zero_base=a==0,
                possible_values=atoms(before[d]),unknown_alternative=UNKNOWN in before[d],
                successful_store_or_base_epoch_or_alias_proven=False))
        if flow and flow.get('link') and flow.get('target') is None:
            bank=32 if flow.get('target_source')=='LR' else 33 if flow.get('target_source')=='CTR' else None
            calls.append(dict(site=hx(pc),word=hx(w),target_source=flow.get('target_source'),
                pre_link_target_candidates=atoms(before[bank]) if bank is not None else atoms({UNKNOWN}),
                target_low_bits_mask='0xfffffffc',null_skip_guard_candidate=null_skip_guard(image,pc),
                post_call_facts_invalidated=True,actual_target_or_call_completion_proven=False))
        if flow and flow.get('link') and flow.get('target') is not None:
            direct.append(dict(site=hx(pc),word=hx(w),target=hx(flow['target']),
                pre_call_gpr_candidates=[dict(gpr=i,possible_values=atoms(before[i])) for i in range(32)],
                input_abi_or_memory_contents_or_call_execution_proven=False,
                post_call_facts_invalidated=True))
    return dict(schema='platform_callback_value_flow_v2',entry=hx(entry),
        inputs=dict(dol_sha256=hashlib.sha256(raw).hexdigest(),source_pins=pins()),
        summary=dict(original_words=len(words),dataflow_updates=updates,publications=len(publications),
            indirect_call_candidates=len(calls),direct_call_candidates=len(direct),unsupported_barriers=sum(v=='unsupported' for v in barriers.values()),
            call_barriers=sum(v=='call' for v in barriers.values()),
            # Each atom is unknown, one original literal/load, or one finite
            # lis/low pair. A pair can never become a new arithmetic seed.
            finite_atom_vocabulary_bound=1+2*len(words)+len(words)*len(words)),
        publications=publications,indirect_calls=calls,direct_calls=direct,
        barriers=[dict(site=hx(pc),kind=kind) for pc,kind in sorted(barriers.items())],
        scope=dict(source_only=True,both_direct_arms=True,calls_assume_no_preserved_registers=True,
            memory_contents_imported=False,arithmetic_cycles_do_not_expand_atom_vocabulary=True,
            loaded_memory_read_from_proven=False,branch_conditions_or_execution_proven=False,
            callback_alias_or_lifetime_proven=False,context_or_fault_parity_proven=False,
            native_retirement_proven=False,deletion_authorized=False))


def main(argv=None):
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--dol',type=Path,required=True)
    p.add_argument('--entry',type=lambda v:int(v,0),action='append',required=True);p.add_argument('--out',type=Path,required=True)
    a=p.parse_args(argv)
    if a.out.resolve()==a.dol.resolve():raise ValueError('output cannot replace original input')
    before=pins();raw=a.dol.read_bytes();reports=[build_value_flow(raw,v) for v in a.entry]
    if before!=pins() or raw!=a.dol.read_bytes():raise ValueError('source changed during analysis')
    with a.out.open('x',encoding='utf-8') as f:json.dump(dict(schema='platform_callback_value_flow_batch_v1',contracts=reports),f,indent=2);f.write('\n')
    print('Recognized',len(reports),'source-only value-flow candidates; no retirement.')
if __name__=='__main__':main()
