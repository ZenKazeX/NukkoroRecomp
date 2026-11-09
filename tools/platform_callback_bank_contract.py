"""Counted callback-bank source order; never snapshot/ABI/lifetime equivalence."""
import hashlib
from pathlib import Path
if __package__:
    from .graphics_lift_inventory import Dol,hx
    from .platform_indirect_guard_contract import recognize_guard
else:
    from graphics_lift_inventory import Dol,hx
    from platform_indirect_guard_contract import recognize_guard

WORDS=(0x3AC00000,0x92D70000,0x3ABC01A0,0x92DC01E4,0x92DC01E8,0x92DC01EC,
       0x81950000,0x280C0000,0x41820014,0x7D8803A6,0x387E0000,0x389F0000,
       0x4E800021,0x3AD60001,0x2C160004,0x3AB50004,0x4180FFD8)


def pins():
    names=('platform_callback_bank_contract.py','platform_indirect_guard_contract.py',
        'platform_callback_value_flow.py','gqr_format_dataflow.py','platform_procedure_contract.py',
        'platform_effect_inventory.py','particle_numeric_contract.py','format_memory_dependencies.py','graphics_lift_inventory.py')
    return {n:hashlib.sha256(Path(__file__).with_name(n).read_bytes()).hexdigest() for n in names}


def recognize_bank(raw,entry):
    image=Dol(raw)
    if type(entry) is not int or entry%4 or not image.contains_text(entry,68):raise ValueError('complete aligned original bank loop required')
    actual=[image.word(entry+4*i) for i in range(17)]
    if tuple(actual)!=WORDS:raise ValueError('unreviewed bank reset/guard/call/iteration sequence')
    guard=recognize_guard(image,entry+24,entry+48,8)
    if guard is None:raise ValueError('current loaded callback guard not independently recognized')
    return dict(schema='platform_callback_bank_source_v1',candidate_family='reset-and-counted-null-guarded-callback-bank',classification='PROVEN',
        proof_scope='Exact source reset/read/guard/invoke/reload ordering, conditional on normal continuations only.',
        inputs=dict(dol_sha256=hashlib.sha256(raw).hexdigest(),source_pins=pins()),
        original_words=[dict(site=hx(entry+4*i),word=hx(w),file_offset=image.offset(entry+4*i)) for i,w in enumerate(actual)],
        entry=hx(entry),callback_read_site=hx(entry+24),callback_invoke_site=hx(entry+48),null_guard=guard,
        bank_coordinate=dict(current_base_gpr=28,displacement=0x1a0,slot_width_bytes=4,iteration_pointer_gpr=21),
        source_counter=dict(gpr=22,initial_value=0,increment=1,less_than_bound=4,bank_pointer_stride=4,
            four_iterations_only_if_callback_preserves_counter_and_pointer=True),
        callback_arguments=[dict(destination_gpr=3,source_gpr=30),dict(destination_gpr=4,source_gpr=31)],
        resets_before_first_slot=[dict(base_gpr=23,displacement=0),*[dict(base_gpr=28,displacement=n) for n in (0x1e4,0x1e8,0x1ec)]],
        shared_semantics='Reset source fields; read/test current slot; optionally invoke this loaded value; advance and reread next slot AFTER callback return.',
        differences='Only this exact encoding reviewed; other registers, bounds, stride, reset order or callbacks UNKNOWN.',
        callers_consumers='Current r28/r23 fields and source arguments; physical registry identity, incoming indirect entries and callback effects UNKNOWN.',
        scope=dict(source_only=True,nonnull_guard_of_this_loaded_value_on_linear_path=True,
            whole_bank_snapshot_equivalence_proven=False,callee_gpr21_22_28_30_31_preservation_proven=False,
            source_loop_count_is_not_unconditional_runtime_bound=True,
            physical_bank_reset_or_callback_alias_closed=False,actual_fault_completion_context_or_lifetime_proven=False,
            full_state_pixel_audio_or_native_retirement_proven=False,deletion_authorized=False))


def scan_banks(raw):
    image=Dol(raw);matches=[];candidates=0
    for s in image.text:
        for pc in range(s['address'],s['address']+s['size'],4):
            if image.word(pc)!=WORDS[0]:continue
            candidates+=1
            try:matches.append(recognize_bank(raw,pc))
            except ValueError:continue
    return dict(schema='platform_callback_bank_scan_v1',dol_sha256=hashlib.sha256(raw).hexdigest(),source_pins=pins(),
        summary=dict(source_text_words=sum(s['size']//4 for s in image.text),counter_zero_candidates=candidates,reviewed_source_banks=len(matches)),
        matches=matches,scope=dict(source_only=True,unconditional_iteration_bound_or_lifetime_closure=False,native_retirement_proven=False))
