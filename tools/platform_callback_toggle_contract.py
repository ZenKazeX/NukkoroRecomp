"""Exact optional-callback toggle with original leaf register-effect proofs.

No SDK names, guessed ABI, runtime inputs, field/stack alias claims or native
retirement. Source effects are conditional on successful normal call returns.
"""
import hashlib
from pathlib import Path
if __package__:
    from .graphics_lift_inventory import Dol,decode,hx
else:
    from graphics_lift_inventory import Dol,decode,hx

PREFIX=[0x7C0802A6,0x90010004,0x9421FFE8,0x93E10014,0x93C10010,0x7C7E1B78,
        None,None,0x28000000,0x4182000C,0x3BE00001,0x48000008,0x3BE00000,
        0x2C1E0000,0x41820010,None,None,0x48000008,0x38000000,None,None,
        0x7FE3FB78,0x8001001C,0x83E10014,0x83C10010,0x38210018,0x7C0803A6,0x4E800020]
# Actual slot indexes are checked below; these are complete reviewed leaves,
# including their canonical returns and every conditional branch instruction.
DISABLE=[0x7C6000A6,0x5464045E,0x7C800124,0x54638FFE,0x4E800020]
RESTORE=[0x2C030000,0x7C8000A6,0x4182000C,0x60858000,0x48000008,
         0x5485045E,0x7CA00124,0x54838FFE,0x4E800020]


def recognize_exclusion_leaf(image,entry,kind):
    """Canonical complete-source register proof, shared by caller recognizers."""
    if kind not in ('disable','restore'):raise ValueError('reviewed exclusion kind required')
    expected,writes=(DISABLE,[3,4]) if kind=='disable' else (RESTORE,[3,4,5])
    if type(entry) is not int or entry%4 or not image.contains_text(entry,4*len(expected)):
        raise ValueError('leaf must be completely original-file backed')
    actual=[image.word(entry+4*i) for i in range(len(expected))]
    if actual!=expected:raise ValueError('callee register-preservation proof changed')
    return dict(entry=hx(entry),gpr_writes=writes,all_other_gprs_preserved_on_successful_normal_return=True,
        original_words=[dict(site=hx(entry+4*i),word=hx(w),file_offset=image.offset(entry+4*i)) for i,w in enumerate(actual)],
        hardware_msr_semantics_still_present=True,event_context_or_native_lifetime_proven=False)


def recognize_toggle(raw,entry):
    if type(entry) is not int:raise ValueError('original integer entry required')
    image=Dol(raw)
    if entry%4 or not image.contains_text(entry,112):raise ValueError('complete original toggle required')
    words=[image.word(entry+4*i) for i in range(28)]
    # The optional callback construction has lis/addi at indexes15/16.
    for i,expected in enumerate(PREFIX):
        if expected is not None and words[i]!=expected:raise ValueError('unrecognized toggle effect at '+hx(entry+4*i))
    disable=decode(words[6],entry+24);restore=decode(words[20],entry+80)
    for flow in (disable,restore):
        if not flow or flow.get('mnemonic')!='b' or flow.get('target') is None or not flow.get('link') or flow.get('conditional'):
            raise ValueError('toggle requires original unconditional direct leaf calls')
    load,store=words[7],words[19]
    if load&0xFFFF0000!=0x800D0000 or store&0xFFFF0000!=0x900D0000 or load&65535!=store&65535:
        raise ValueError('old read and new write must use the same encoded field')
    high,low=words[15],words[16]
    if high&0xFFFF0000!=0x3C800000 or low&0xFFFF0000!=0x38040000:
        raise ValueError('original callback constant construction required')
    value=((high&65535)<<16)+((low&65535)-65536 if low&32768 else low&65535)
    leaves=[]
    for flow,kind in ((disable,'disable'),(restore,'restore')):
        leaves.append(recognize_exclusion_leaf(image,flow['target'],kind))
    return dict(schema='platform_callback_toggle_contract_v1',entry=hx(entry),
        inputs=dict(dol_sha256=hashlib.sha256(raw).hexdigest(),recognizer_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest()),
        original_words=[dict(site=hx(entry+4*i),word=hx(w),file_offset=image.offset(entry+4*i)) for i,w in enumerate(words)],
        field_base_gpr=13,field_displacement=(load&65535)-65536 if load&32768 else load&65535,
        enable_input_gpr=3,callback_literal=hx(value&0xffffffff),
        old_value_read_site=hx(entry+28),publication_site=hx(entry+76),
        selected_publication='Input r3 is zero: publish zero; otherwise publish the original callback literal.',
        return_value='Boolean nonzero test of the value actually read at the original old-value load, before publication.',
        input_copy_preserved_through_first_leaf_gpr=30,old_boolean_preserved_through_second_leaf_gpr=31,
        leaf_proofs=leaves,
        scope=dict(source_normal_effects_only=True,callee_register_preservation_source_proven=True,
            arbitrary_callee_abi_assumed=False,old_value_is_function_entry_value_proven=False,
            field_stack_or_external_alias_closure_proven=False,successful_execution_or_fault_parity_proven=False,
            callback_identity_or_lifetime_proven=False,native_retirement_proven=False,deletion_authorized=False))
