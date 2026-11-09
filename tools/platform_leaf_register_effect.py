"""Conservative encoded GPR write sets for complete direct-flow source leaves.

No ABI assumptions. Calls/unsupported words/outside/indirect terminals refuse
preservation. Normal returning flow only; memory/fault/context ownership unknown.
"""
import hashlib
from pathlib import Path
if __package__:
    from .platform_procedure_contract import build_contract
    from .gqr_format_dataflow import gpr_dependencies,implementation_pins
else:
    from platform_procedure_contract import build_contract
    from gqr_format_dataflow import gpr_dependencies,implementation_pins

def build_leaf_register_effect(raw,entry):
    if type(entry) is not int:raise ValueError('integer original source entry required')
    procedure=build_contract(raw,entry)
    barriers=[];effects=[];written=set()
    for p in procedure['words']:
        dependencies=gpr_dependencies(int(p['word'],16))
        if dependencies is None:barriers.append(p['site']);continue
        written.update(dependencies)
        effects.append(dict(site=p['site'],word=p['word'],written_gprs=sorted(dependencies)))
    closed=not (barriers or procedure['calls'] or procedure['unresolved_terminals'] or procedure['outside_text_edges']) and bool(procedure['return_like'])
    return dict(schema='platform_leaf_register_effect_v1',entry=procedure['entry'],
        inputs=dict(dol=procedure['inputs']['dol'],source_pins=dict(implementation_pins(),
            leaf_register_effect=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())),
        classification='STRONGLY SUPPORTED' if closed else 'UNKNOWN',
        complete_normal_flow_encoded_gpr_effects=closed,
        written_gprs=sorted(written),preserved_gprs=sorted(set(range(32))-written) if closed else [],
        unknown_word_sites=barriers,original_words=procedure['words'],effects=effects,
        unresolved=dict(calls=procedure['calls'],indirect_terminals=procedure['unresolved_terminals'],outside_edges=procedure['outside_text_edges']),
        scope=dict(canonical_source_gpr_effects_only=True,abi_not_assumed=True,
            all_paths_return_or_fault_context_memory_cr_fpr_preservation_proven=False,
            native_retirement_proven=False,deletion_authorized=False))
