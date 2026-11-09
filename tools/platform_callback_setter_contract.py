"""Original read-before-exclusion callback setters; not native atomic exchange.

Reuse the reviewed leaf vocabulary from callback toggles, verify every original
word, and prove normal-path GPR writes only. Context/events, MSR consequences,
actual loads/stores/returns, aliases and native ownership remain unresolved.
"""
import hashlib
from pathlib import Path
if __package__:
    from .graphics_lift_inventory import Dol,decode,hx
    from .platform_callback_toggle_contract import DISABLE,RESTORE
else:
    from graphics_lift_inventory import Dol,decode,hx
    from platform_callback_toggle_contract import DISABLE,RESTORE


def recognize_setter(raw,entry):
    image=Dol(raw)
    if type(entry) is not int or entry%4 or not image.contains_text(entry,68):
        raise ValueError('complete aligned original setter required')
    words=[image.word(entry+4*i) for i in range(17)]
    fixed={0:0x7C0802A6,1:0x90010004,2:0x9421FFE8,3:0x93E10014,4:0x93C10010,
        5:0x7C7E1B78,10:0x7FE3FB78,11:0x8001001C,12:0x83E10014,13:0x83C10010,
        14:0x38210018,15:0x7C0803A6,16:0x4E800020}
    if any(words[i]!=w for i,w in fixed.items()):raise ValueError('setter original sequence changed')
    if (words[6]&0xffff0000!=0x83ED0000 or words[8]&0xffff0000!=0x93CD0000 or
        words[6]&65535!=words[8]&65535):raise ValueError('old read and publication coordinates differ')
    leaves=[]
    for index,expected,writes in ((7,DISABLE,[3,4]),(9,RESTORE,[3,4,5])):
        pc=entry+4*index;edge=decode(words[index],pc)
        if not edge or not edge.get('link') or edge.get('conditional') or edge.get('target') is None:
            raise ValueError('unconditional original leaf call required')
        target=edge['target']
        if not image.contains_text(target,4*len(expected)) or [image.word(target+4*i) for i in range(len(expected))]!=expected:
            raise ValueError('reviewed normal-path leaf preservation changed')
        leaves.append(dict(call_site=hx(pc),entry=hx(target),normal_path_gpr_writes=writes,
            all_other_gprs_preserved_on_successful_normal_return=True,
            original_words=[dict(site=hx(target+4*i),word=hx(w),file_offset=image.offset(target+4*i)) for i,w in enumerate(expected)],
            hardware_msr_semantics_or_events_translated=False))
    disp=(words[6]&65535)-65536 if words[6]&32768 else words[6]&65535
    names=('platform_callback_setter_contract.py','platform_callback_toggle_contract.py','graphics_lift_inventory.py')
    return dict(schema='platform_callback_setter_candidate_v1',entry=hx(entry),
        inputs=dict(dol_sha256=hashlib.sha256(raw).hexdigest(),source_pins={n:hashlib.sha256(Path(__file__).with_name(n).read_bytes()).hexdigest() for n in names}),
        original_words=[dict(site=hx(entry+4*i),word=hx(w),file_offset=image.offset(entry+4*i)) for i,w in enumerate(words)],
        field_coordinate=dict(current_base_gpr=13,displacement=disp,width_bytes=4),input_gpr=3,
        old_value_read_site=hx(entry+24),publication_site=hx(entry+32),return_value='Previously read CURRENT field value, not a Boolean.',
        source_order=['copy_input','read_old_field','exclude_via_original_leaf','publish_input','restore_via_original_leaf','return_previously_read_value'],
        old_read_precedes_exclusion=True,atomic_exchange_equivalence_proven=False,
        source_leaf_preservation=leaves,
        scope=dict(source_only=True,normal_call_returns_are_premises=True,
            field_stack_or_context_alias_closure_proven=False,actual_publication_return_or_lifetime_proven=False,
            native_ownership_or_retirement_proven=False,deletion_authorized=False))
