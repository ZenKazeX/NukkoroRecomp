"""Recognize an original callback exchange and classify actual entry operands.

Entry observations do not prove the load/store completed, table identity,
callback lifetime, a successful publication epoch, or native retirement.
"""
from collections import Counter
import hashlib
from pathlib import Path
import re

if __package__:
    from .graphics_lift_inventory import Dol,decode,hx
    from .particle_lane_events import analyze,u64
else:
    from graphics_lift_inventory import Dol,decode,hx
    from particle_lane_events import analyze,u64


def hex32(value):
    if not isinstance(value,str) or re.fullmatch(r'0x[0-9a-fA-F]{8}',value) is None:
        raise ValueError('exact raw hexadecimal u32 required')
    return int(value,16)


def recognize_exchange(raw,entry):
    image=Dol(raw)
    if type(entry) is not int or entry%4 or not image.contains_text(entry,28):
        raise ValueError('exchange must have seven original text words')
    words=[image.word(entry+i*4) for i in range(7)]
    def fields(w):return w>>26,(w>>21)&31,(w>>16)&31,(w>>11)&31
    mask,load,shift,add,old,new,ret=words
    op,index,temp,_=fields(mask)
    if op!=21 or (mask&0xffff)!=0x063e or mask&1:
        raise ValueError('unsupported low-byte index transform')
    op,table,sda,_=fields(load);disp=load&0xffff
    if op!=32 or sda==0:raise ValueError('current pointer must be read from a register-relative slot')
    op,source,target,_=fields(shift)
    if op!=21 or source!=temp or target!=temp or (shift&0xffff)!=0x103a or shift&1:
        raise ValueError('unsupported four-byte index stride')
    op,address,left,right=fields(add)
    if op!=31 or (add>>1)&1023!=266 or add&1 or left!=table or right!=temp or (add>>10)&1:
        raise ValueError('unsupported table/index address assembly')
    op,result,base,_=fields(old)
    if op!=32 or base!=address or old&0xffff:raise ValueError('old slot read must precede publication')
    op,callback,base,_=fields(new)
    if op!=36 or base!=address or new&0xffff or ret!=0x4e800020:
        raise ValueError('unsupported exchange store or return')
    if (temp==table or address in (sda,callback) or
        callback in (temp,table,result) or result==address or sda in (temp,table)):
        raise ValueError('register alias invalidates entry-operand contract')
    return dict(schema='platform_callback_exchange_contract_v1',entry=hx(entry),
        original_words=[dict(site=hx(entry+i*4),word=hx(w),file_offset=image.offset(entry+i*4)) for i,w in enumerate(words)],
        inputs=dict(dol_sha256=hashlib.sha256(raw).hexdigest(),recognizer_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest()),
        index_input_gpr=index,index_mask=255,index_stride_bytes=4,new_callback_input_gpr=callback,
        current_pointer_base_gpr=sda,current_pointer_displacement=disp if disp<32768 else disp-65536,
        old_callback_output_gpr=result,old_slot_read_site=hx(entry+16),publication_site=hx(entry+20),
        ordering='Read current pointer, then old slot, then write new callback, then return old slot.',
        scope=dict(source_only=True,table_size_proven=False,successful_publication_proven=False,
            callback_alias_lifetime_or_native_retirement_proven=False,deletion_authorized=False))


def census_exchanges(raw):
    """Whole original text, no annotation boundaries or callable-identity claim."""
    image=Dol(raw);matches=[];possible=0
    for section in image.text:
        for pc in range(section['address'],section['address']+section['size'],4):
            if image.word(pc)&0xfc00ffff!=0x5400063e:continue
            possible+=1
            try:contract=recognize_exchange(raw,pc)
            except ValueError:continue
            matches.append(contract)
    return dict(schema='platform_indexed_exchange_census_v1',
        inputs=dict(dol_sha256=hashlib.sha256(raw).hexdigest(),recognizer_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest()),
        low_byte_transform_candidates=possible,recognized_structural_exchanges=len(matches),contracts=matches,
        scope=dict(all_original_text_words_scanned=True,callable_identity_proven=False,
            stored_values_are_callbacks_proven=False,interior_or_indirect_alias_closure_proven=False,
            shipping_producer_consumer_zero_proven=False,deletion_authorized=False))


def classify_entries(raw,contract,probe):
    expected=recognize_exchange(raw,hex32(contract['entry']))
    if contract!=expected:raise ValueError('exchange contract is not current original-source recognition')
    checked=analyze(probe)
    trace=probe['ordered_numeric_events']
    if not trace['complete_for_observed_entries'] or trace['dropped'] or trace['counter_overflow']:
        raise ValueError('complete nonempty entry observations required')
    configured=probe.get('config',{}).get('numeric_entries')
    if not isinstance(configured,list) or not configured:raise ValueError('explicit configured entries required')
    requested={hex32(v) for v in configured}
    if any(hex32(v['site']) not in requested for v in trace['events']):
        raise ValueError('unconfigured observed site')
    records={hex32(v['address']):v for v in probe['probes'] if v.get('numeric_entry_evidence') is not None}
    if set(records)!=requested or len(records)!=len(configured):raise ValueError('duplicate/missing installed entries')
    if any(v['installed'] is not True or v.get('still_installed') is not True for v in records.values()):
        raise ValueError('some requested entries were not installed or were replaced')
    for p,v in records.items():
        count=sum(hex32(e['site'])==p for e in trace['events'])
        if type(v['hits']) is not int or v['hits']!=count:raise ValueError('entry population mismatch')
    image=Dol(raw);rows=[];slots=Counter();callbacks=Counter()
    roots=trace['root_registers']
    for e in trace['events']:
        if hex32(e['site'])!=hex32(contract['entry']):continue
        registers=dict(zip(roots,[hex32(v) for v in e['roots']]))
        if not {contract['index_input_gpr'],contract['new_callback_input_gpr'],contract['current_pointer_base_gpr']}<=registers.keys():
            raise ValueError('entry operands absent from frozen root schema')
        rawindex=registers[contract['index_input_gpr']];slot=rawindex&contract['index_mask']
        callback=registers[contract['new_callback_input_gpr']];sda=registers[contract['current_pointer_base_gpr']]
        pointer_address=(sda+contract['current_pointer_displacement'])&0xffffffff
        lr=hex32(e['lr']);caller=lr-4 if lr>=4 else None
        direct=False;caller_word=None
        if caller is not None and image.contains_text(caller,4):
            caller_word=image.word(caller);flow=decode(caller_word,caller)
            direct=bool(flow and flow.get('link') and flow.get('target')==hex32(contract['entry']))
        slots[slot]+=1;callbacks[(slot,callback)]+=1
        rows.append(dict(sequence=e['sequence'],raw_index=hx(rawindex),slot=slot,
            callback=hx(callback),lr=e['lr'],source_call_candidate=hx(caller) if caller is not None else None,
            original_caller_word=hx(caller_word) if caller_word is not None else None,
            encoded_direct_call_matches=direct,current_pointer_slot_address_candidate=hx(pointer_address),
            current_pointer_value_observed=False,old_callback_or_publication_success_observed=False,
            cumulative_coordinate=e['instructions'],exact_instruction_timestamp_proven=False))
    if not rows:raise ValueError('no exchange entry observations')
    return dict(schema='platform_callback_exchange_entries_v1',contract=contract,
        observed_exchange_entries=len(rows),rows=rows,
        slot_population={str(k):v for k,v in sorted(slots.items())},
        callback_population=[dict(slot=k[0],callback=hx(k[1]),entries=v) for k,v in sorted(callbacks.items())],
        entry_probe_validation=checked,
        scope=dict(actual_entry_operands_observed=True,successful_publication_proven=False,
            uninterrupted_table_history_proven=False,table_allocation_bounds_proven=False,
            context_or_callback_lifetime_proven=False,native_retirement_proven=False,deletion_authorized=False))
