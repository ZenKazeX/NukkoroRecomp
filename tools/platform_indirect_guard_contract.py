"""Source-bound null guards and consume-before-callback ordering candidates.

Pure argument setup may separate the guard and LR/CTR invocation. No external
entries, caller ABI, memory alias, execution, lifetime or native retirement are
inferred. The current value-flow report is recomputed, never supplied as truth.
"""
import hashlib
from pathlib import Path
if __package__:
    from .graphics_lift_inventory import Dol,decode,hx
    from .gqr_format_dataflow import gpr_dependencies
    from .platform_callback_value_flow import build_value_flow,signed16
else:
    from graphics_lift_inventory import Dol,decode,hx
    from gqr_format_dataflow import gpr_dependencies
    from platform_callback_value_flow import build_value_flow,signed16


def zero_compare(w,reg):
    return w>>26 in (10,11) and not w&(3<<21) and not w&65535 and (w>>16)&31==reg


def skip_branch(image,pc,bf,target):
    edge=decode(image.word(pc),pc)
    return bool(edge and not edge.get('link') and edge.get('bo')==12 and edge.get('bi')==4*bf+2 and edge.get('target')==target)


def recognize_guard(image,load_site,call_site,target_bank):
    if load_site>=call_site or not image.contains_text(load_site,call_site-load_site+4):return None
    load=image.word(load_site);reg=(load>>21)&31
    if load>>26!=32:return None
    comparisons=[];moves=[]
    for pc in range(load_site+4,call_site,4):
        w=image.word(pc);edge=decode(w,pc)
        if zero_compare(w,reg) and pc+4<call_site and skip_branch(image,pc+4,(w>>23)&7,call_site+4):
            comparisons.append(pc)
        if edge:
            if edge.get('link') or not comparisons or pc!=comparisons[-1]+4:return None
            continue
        deps=gpr_dependencies(w)
        if deps is None or reg in deps:return None
        if w>>26==31 and (w>>1)&1023==467:
            spr=((w>>16)&31)|(((w>>11)&31)<<5)
            if spr in (8,9):
                if w&1 or spr!=target_bank or (w>>21)&31!=reg:return None
                moves.append(pc)
    if len(comparisons)!=1 or len(moves)!=1:return None
    cmp_site=comparisons[0]
    if moves[0]<cmp_site:return None
    return dict(load_site=hx(load_site),compare_site=hx(cmp_site),guard_site=hx(cmp_site+4),
        target_register=reg,target_move_site=hx(moves[0]),skip_target=hx(call_site+4),
        null_skip_condition='The consumed u32 is zero.',
        normal_linear_sequence_preserves_loaded_target=True,
        external_entry_or_context_guard_domination_proven=False)


def recognize_consume(image,load_site,call_site):
    begin=load_site-12
    if not image.contains_text(begin,24):return None
    f,cmp,branch,callback,zero,store=[image.word(begin+4*i) for i in range(6)]
    flag=(f>>21)&31;cb=(callback>>21)&31;base=(f>>16)&31;disp=signed16(f&65535)
    if (f>>26!=32 or callback>>26!=32 or flag==cb or (base!=0 and base in (flag,cb)) or not zero_compare(cmp,flag) or
        not skip_branch(image,begin+8,(cmp>>23)&7,call_site+4) or
        zero!=(0x38000000|(flag<<21)) or store>>26!=36 or (store>>21)&31!=flag or
        (store>>16)&31!=base or signed16(store&65535)!=disp):return None
    return dict(flag_load_site=hx(begin),flag_compare_site=hx(begin+4),flag_guard_site=hx(begin+8),
        callback_load_site=hx(load_site),flag_clear_site=hx(load_site+8),
        flag_base_gpr=base,flag_displacement=disp,flag_width_bytes=4,
        clear_precedes_callback_null_test_and_invocation=True,
        null_callback_still_consumes_pending_flag=True,
        actual_flag_epoch_resource_or_lifetime_proven=False)


def build_guard_contract(raw,entry):
    flow=build_value_flow(raw,entry);image=Dol(raw);rows=[]
    for call in flow['indirect_calls']:
        pc=int(call['site'],16);bank=8 if call['target_source']=='LR' else 9 if call['target_source']=='CTR' else None
        guard=None;consume=None
        if bank is not None and len(call['pre_link_target_candidates'])==1:
            atom=call['pre_link_target_candidates'][0]
            if atom['kind']=='field_load' and len(atom['original_definition_sites'])==1:
                load=int(atom['original_definition_sites'][0],16)
                guard=recognize_guard(image,load,pc,bank)
                if guard is not None:consume=recognize_consume(image,load,pc)
        rows.append(dict(source_call=call,null_guard=guard,consume_before_callback=consume))
    return dict(schema='platform_indirect_guard_contract_v1',entry=flow['entry'],
        inputs=dict(dol_sha256=hashlib.sha256(raw).hexdigest(),recognizer_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            value_flow_inputs=flow['inputs']),calls=rows,
        summary=dict(indirect_calls=len(rows),recognized_null_guards=sum(x['null_guard'] is not None for x in rows),
            recognized_consume_before_callback=sum(x['consume_before_callback'] is not None for x in rows)),
        scope=dict(source_normal_sequence_only=True,arbitrary_callee_abi_assumed=False,
            flags_or_callback_contents_imported=False,external_alias_epoch_or_lifetime_proven=False,
            executed_or_fault_state_parity_proven=False,native_retirement_proven=False,deletion_authorized=False))
