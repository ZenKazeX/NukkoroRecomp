"""Publish callback then choose a handler service from original input nullness.

This reviewed source form does not prove service identity, subscription success,
old-value return across callees, atomicity, handler ownership or lifetime.
"""
import hashlib
from pathlib import Path
if __package__:
    from .graphics_lift_inventory import Dol,decode,hx
    from .platform_procedure_contract import adjacent_address_value
else:
    from graphics_lift_inventory import Dol,decode,hx
    from platform_procedure_contract import adjacent_address_value


def pins():
    return {n:hashlib.sha256(Path(__file__).with_name(n).read_bytes()).hexdigest() for n in
        ('platform_callback_subscription_contract.py','platform_procedure_contract.py','platform_effect_inventory.py','graphics_lift_inventory.py')}


def recognize_subscription(raw,entry):
    image=Dol(raw)
    if type(entry) is not int or entry%4 or not image.contains_text(entry,84):raise ValueError('complete aligned original subscription source required')
    words=[image.word(entry+4*i) for i in range(21)]
    fixed={0:0x7C0802A6,1:0x28030000,2:0x90010004,3:0x9421FFE8,4:0x93E10014,
        7:0x41820014,11:0x48000010,15:0x7FE3FB78,16:0x8001001C,17:0x83E10014,
        18:0x38210018,19:0x7C0803A6,20:0x4E800020}
    if any(words[i]!=v for i,v in fixed.items()):raise ValueError('unreviewed publication/guard/return sequence')
    if words[5]&0xffff0000!=0x83ed0000 or words[6]&0xffff0000!=0x906d0000 or words[5]&65535!=words[6]&65535:
        raise ValueError('old read and new publication field differ')
    if words[8]&0xffff0000!=0x3c600000 or words[9]&0xffff0000!=0x38630000 or words[8:10]!=words[12:14]:
        raise ValueError('same literal handler argument required on both arms')
    address=adjacent_address_value(words[8],words[9])
    if not address or not image.contains_text(address['value'],4):raise ValueError('literal handler must be original text')
    calls=[]
    for i in (10,14):
        pc=entry+4*i;edge=decode(words[i],pc)
        if not edge or edge['mnemonic']!='b' or not edge['link'] or edge['absolute'] or not image.contains_text(edge['target'],4):
            raise ValueError('unconditional original relative service call required')
        calls.append(dict(site=hx(pc),target=hx(edge['target'])))
    if calls[0]['target']==calls[1]['target']:raise ValueError('distinct handler services required')
    field=words[5]&65535
    return dict(schema='platform_callback_subscription_source_v1',entry=hx(entry),classification='PROVEN',
        proof_scope='Exact source read/publication/branch/handler-argument ordering ONLY.',
        inputs=dict(dol_sha256=hashlib.sha256(raw).hexdigest(),source_pins=pins()),
        original_words=[dict(site=hx(entry+4*i),word=hx(w),file_offset=image.offset(entry+4*i)) for i,w in enumerate(words)],
        field_coordinate=dict(current_base_gpr=13,displacement=field-65536 if field&32768 else field,width_bytes=4),
        input_callback_gpr=3,old_read_site=hx(entry+20),publication_site=hx(entry+24),
        tested_value='Original input r3, compared before publication; not the old field.',
        handler_address=hx(address['value']),nonnull_input_service=calls[0],null_input_service=calls[1],
        source_order=['test_original_input','read_old_field_into_r31','publish_input','choose_service_from_input_nullness','supply_same_literal_handler','invoke_chosen_service','copy_current_r31_to_return_r3'],
        scope=dict(source_only=True,callee_identity_returnability_and_r31_preservation_proven=False,
            publication_precedes_service_on_successful_source_path=True,publication_fault_or_service_failure_rollback_proven=False,
            interrupt_exclusion_or_atomic_exchange_proven=False,old_value_return_across_callees_proven=False,
            alias_context_epochs_or_lifetime_proven=False,live_subscription_or_audio_parity_proven=False,
            native_ownership_or_retirement_proven=False,deletion_authorized=False))


def scan_subscriptions(raw):
    image=Dol(raw);matches=[];candidates=0
    for s in image.text:
        for pc in range(s['address'],s['address']+s['size']-4,4):
            if image.word(pc)!=0x7c0802a6 or image.word(pc+4)!=0x28030000:continue
            candidates+=1
            try:matches.append(recognize_subscription(raw,pc))
            except ValueError:continue
    return dict(schema='platform_callback_subscription_scan_v1',dol_sha256=hashlib.sha256(raw).hexdigest(),source_pins=pins(),
        summary=dict(source_text_words=sum(s['size']//4 for s in image.text),candidates=candidates,source_contracts=len(matches)),matches=matches,
        scope=dict(source_only=True,production_subscription_or_lifetime_proven=False,native_retirement_proven=False))
