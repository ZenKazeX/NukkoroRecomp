"""Straight-line call-coordinate DAGs from original words, never object identity.

Register leaves mean CURRENT register values at a source boundary, not supplied
state or an inferred ABI. Loads are symbolic reads; even repeated addresses do
not forward values. Calls, branches, direct destinations and unknown encodings
sever provenance. Unknown indirect entries, exceptions and lifetimes stay open.
"""
import hashlib
from pathlib import Path
if __package__:
    from .graphics_lift_inventory import Dol, decode, hx
    from .gqr_format_dataflow import gpr_dependencies
else:
    from graphics_lift_inventory import Dol, decode, hx
    from gqr_format_dataflow import gpr_dependencies


def implementation_pins():
    names=('platform_call_coordinate_flow.py','graphics_lift_inventory.py',
           'gqr_format_dataflow.py','platform_procedure_contract.py',
           'platform_effect_inventory.py','particle_numeric_contract.py','format_memory_dependencies.py')
    return {n:hashlib.sha256(Path(__file__).with_name(n).read_bytes()).hexdigest() for n in names}


def _build_coordinate_queries(raw, targets, registers, memory_addresses=()):
    registers=tuple(registers)
    if (not registers or any(type(r) is not int or not 0<=r<32 for r in registers)
        or len(set(registers))!=len(registers)):
        raise ValueError('unique nonempty encoded GPR selectors required')
    targets=tuple(targets)
    if (not targets and not memory_addresses) or any(type(t) is not int or not 0<=t<=0xffffffff or t%4 for t in targets):
        raise ValueError('nonempty aligned original target queries required')
    if len(targets)!=len(set(targets)):
        raise ValueError('duplicate target query')
    image=Dol(raw)
    if any(not image.contains_text(t,4) for t in targets):
        raise ValueError('target query must name original text')
    targets=set(targets);boundaries=set();words_scanned=0
    for section in image.text:
        for pc in range(section['address'],section['address']+section['size'],4):
            words_scanned+=1;flow=decode(image.word(pc),pc)
            if flow and flow.get('target') is not None:boundaries.add(flow['target'])
    rows=[];memory_rows=[]
    for section in image.text:
        nodes=[];constants=[];state=[None]*32;boundary=section['address'];reason='text_section'
        def reset(pc,why):
            nonlocal nodes,constants,state,boundary,reason
            nodes=[];constants=[];state=[None]*32;boundary=pc;reason=why
        def node(kind,**fields):
            if memory_addresses:
                result=None;children=fields.get('inputs',[])
                if kind=='literal':result=int(fields['value'],16)
                elif len(children)==1 and constants[children[0]] is not None:
                    source=constants[children[0]]
                    if kind=='copy':result=source
                    elif kind=='add_offset_u32':result=(source+fields['offset'])&0xffffffff
                    elif kind=='or_immediate_u32':result=source|int(fields['bits'],16)
                constants.append(result)
            nodes.append(dict(kind=kind,**fields));return len(nodes)-1
        def value(reg):
            if state[reg] is None:
                state[reg]=node('current_gpr_at_boundary',gpr=reg,boundary=hx(boundary),boundary_reason=reason)
            return state[reg]
        def definition(pc,kind,**fields):
            return node(kind,site=hx(pc),word=hx(image.word(pc)),file_offset=image.offset(pc),**fields)
        def snapshot(roots):
            # Emit only reachable nodes, once each, in topological order. No
            # recursively expanded trees or multiplicative copying of aliases.
            needed=set();pending=list(roots.values())
            while pending:
                n=pending.pop()
                if n in needed:continue
                needed.add(n);pending.extend(nodes[n].get('inputs',[]))
            order=sorted(needed);mapping={n:i for i,n in enumerate(order)}
            return dict(roots=[dict(gpr=r,root=mapping[n]) for r,n in roots.items()],nodes=[dict(nodes[n],id=mapping[n],
                **({'inputs':[mapping[v] for v in nodes[n]['inputs']]} if 'inputs' in nodes[n] else {})) for n in order])
        for pc in range(section['address'],section['address']+section['size'],4):
            if pc in boundaries:reset(pc,'direct_destination')
            w=image.word(pc);op,d,a,b=w>>26,(w>>21)&31,(w>>16)&31,(w>>11)&31
            # Observe an existing canonical definition before the access. Do
            # not create current-register leaves merely for observation: that
            # would reorder unrelated argument DAGs. Never evaluate a load.
            if memory_addresses and op in (32,34,40,42,36,38,44):
                base=0 if a==0 else constants[state[a]] if state[a] is not None else None
                if base is not None:
                    disp=(w&65535)-65536 if w&32768 else w&65535
                    address=(base+disp)&0xffffffff
                    width={32:4,34:1,40:2,42:2,36:4,38:1,44:2}[op]
                    hits=sorted({(address+i)&0xffffffff for i in range(width)}.intersection(memory_addresses))
                    if hits:
                        memory_rows.append(dict(site=hx(pc),word=hx(w),file_offset=image.offset(pc),
                            access='read' if op in (32,34,40,42) else 'write',request_bytes=width,
                            literal_effective_address=hx(address),queried_bytes=[hx(x) for x in hits],
                            encoded_zero_base=a==0,source_boundary=hx(boundary),boundary_reason=reason,
                            base_definition=None if a==0 else project_coordinate(snapshot({a:state[a]}),a),
                            successful_access_physical_alias_lifetime_or_execution_proven=False))
            flow=decode(w,pc)
            if flow:
                if flow.get('link') and flow.get('target') in targets:
                    rows.append(dict(site=hx(pc),word=hx(w),file_offset=image.offset(pc),target=hx(flow['target']),
                        argument_dag=snapshot({r:value(r) for r in registers}),
                        conditional_call=bool(flow.get('conditional')),execution_or_input_abi_proven=False))
                reset(pc+4,'call' if flow.get('link') else 'branch');continue
            # Reject record-bit SPR operations rather than allowing the shared
            # encoding helper's destination facts to imply a valid instruction.
            deps=None if op==31 and (w>>1)&1023 in (339,467) and w&1 else gpr_dependencies(w)
            if deps is None:reset(pc+4,'unsupported');continue
            imm=(w&65535)-65536 if w&32768 else w&65535
            if op in (14,15):
                inputs=[] if a==0 else [value(a)]
                state[d]=definition(pc,'literal' if a==0 else 'add_offset_u32',
                    **({'value':hx((imm*(65536 if op==15 else 1))&0xffffffff)} if a==0 else
                       {'offset':imm*(65536 if op==15 else 1),'inputs':inputs}))
            elif op==31 and (w>>1)&1023==444 and d==b:
                state[a]=definition(pc,'copy',inputs=[value(d)])
            elif op in (24,25):
                state[a]=definition(pc,'or_immediate_u32',inputs=[value(d)],bits=hx((w&65535)*(65536 if op==25 else 1)))
            elif op in (32,34,40,42):
                # RA=0 uses encoded zero, not the current contents of r0.
                state[d]=definition(pc,'symbolic_memory_load',inputs=[] if a==0 else [value(a)],
                    encoded_zero_base=a==0,displacement=imm,
                    width_bytes={32:4,34:1,40:2,42:2}[op],signed_load=op==42,
                    contents_or_alias_or_success_proven=False)
            else:
                # Unsupported expressions are explicit leaves, never evaluated
                # or mistaken for preservation. Apply simultaneous writes.
                for reg in deps:
                    state[reg]=definition(pc,'unresolved_definition',gpr=reg)
    result=dict(schema='platform_call_coordinate_bundle_v1',
        inputs=dict(dol_sha256=hashlib.sha256(raw).hexdigest(),source_pins=implementation_pins()),
        query=dict(targets=[hx(t) for t in sorted(targets)],gprs=list(registers)),
        original_text_words_scanned=words_scanned,calls=rows,
        scope=dict(source_only=True,loaded_values_or_entry_state_supplied=False,
            calls_assume_no_preserved_registers=True,unknown_indirect_entries_closed=False,
            current_register_leaves_are_not_native_object_ids=True,
            memory_alias_context_fault_or_lifetime_proven=False,
            node_storage='Per straight-line block; reachable DAG nodes emitted once; bounded by original words.',
            native_ownership_or_retirement_proven=False,deletion_authorized=False))
    if memory_addresses:result['literal_memory_coordinates']=memory_rows
    return result


def build_call_coordinate_bundle(raw,targets,registers=(3,4,5,6,7,8)):
    return _build_coordinate_queries(raw,targets,registers)


def build_literal_memory_coordinates(raw,addresses):
    """Exact original virtual-byte query; other aliases/encodings stay unknown."""
    addresses=tuple(addresses)
    if (not addresses or any(type(a) is not int or not 0<=a<=0xffffffff for a in addresses)
        or len(set(addresses))!=len(addresses)):
        raise ValueError('unique nonempty original virtual byte coordinates required')
    result=_build_coordinate_queries(raw,(),(3,),frozenset(addresses))
    return dict(schema='platform_literal_memory_coordinates_v1',inputs=result['inputs'],
        query=dict(virtual_bytes=[hx(a) for a in sorted(addresses)]),
        original_text_words_scanned=result['original_text_words_scanned'],
        accesses=result['literal_memory_coordinates'],
        scope=dict(source_only=True,canonical_coordinate_transfer_reused=True,
            coverage='Non-update scalar GPR D-form accesses with encoded-zero or wholly literal current base only.',
            loaded_values_or_entry_state_supplied=False,indirect_indexed_update_float_alias_or_context_coverage_complete=False,
            branch_conditions_execution_or_physical_alias_proven=False,zero_matches_prove_deadness=False,
            memory_lifetime_clock_epoch_or_native_ownership_proven=False,deletion_authorized=False))


def project_coordinate(dag,register):
    """One selected reachable DAG; projection adds no source/alias proof."""
    if type(register) is not int or not 0<=register<32:raise ValueError('integer GPR selector required')
    if any(type(r.get('gpr')) is not int or not 0<=r['gpr']<32 for r in dag['roots']):
        raise ValueError('encoded GPR roots required')
    roots={r['gpr']:r['root'] for r in dag['roots']}
    if len(roots)!=len(dag['roots']) or register not in roots:raise ValueError('unique selected root required')
    nodes={n['id']:n for n in dag['nodes']}
    if (len(nodes)!=len(dag['nodes']) or any(type(i) is not int or i<0 for i in nodes)
        or sorted(nodes)!=list(range(len(nodes)))):raise ValueError('canonical unique DAG ids required')
    if any(type(i) is not int or i not in nodes for i in roots.values()):
        raise ValueError('all DAG roots must name original nodes')
    for i,n in nodes.items():
        children=n.get('inputs',[])
        if not isinstance(children,list) or any(type(c) is not int or c<0 or c>=i for c in children):
            raise ValueError('all DAG edges must retain strict source definition order')
    needed=set();pending=[roots[register]]
    while pending:
        i=pending.pop()
        if type(i) is not int or i not in nodes:raise ValueError('missing original DAG node')
        if i in needed:continue
        needed.add(i)
        children=nodes[i].get('inputs',[])
        if any(type(c) is not int or c<0 or c>=i for c in children):raise ValueError('DAG must retain strict source definition order')
        pending.extend(children)
    order=sorted(needed);mapping={i:j for j,i in enumerate(order)}
    return dict(root=mapping[roots[register]],nodes=[dict(nodes[i],id=mapping[i],
        **({'inputs':[mapping[c] for c in nodes[i]['inputs']]} if 'inputs' in nodes[i] else {})) for i in order])


def build_call_coordinates(raw,targets,register=3):
    # One canonical translation; the historical report is a source-only view.
    bundle=build_call_coordinate_bundle(raw,targets,(register,))
    return dict(schema='platform_call_coordinate_candidates_v1',inputs=bundle['inputs'],
        query=dict(targets=bundle['query']['targets'],gpr=register),
        original_text_words_scanned=bundle['original_text_words_scanned'],
        calls=[dict(**{k:v for k,v in row.items() if k!='argument_dag'},queried_gpr=register,
            coordinate=project_coordinate(row['argument_dag'],register)) for row in bundle['calls']],
        scope=bundle['scope'])
