"""Original-word reaching format definitions, never shipping ownership certification.

No imported register values, guessed ABI, branch pruning or captured state input.
Both direct arms and loop back edges participate; calls invalidate all facts.
This identifies native resource-format contract candidates and their blockers.
"""
import argparse
from collections import Counter, deque
import hashlib
import json
from pathlib import Path
import sys

if __package__:
    from .graphics_lift_inventory import Dol, decode, hx
    from .platform_effect_inventory import implicit_consumers, platform_effect
    from .platform_procedure_contract import build_contract, build_incoming_contract
    from .particle_numeric_contract import numeric_effect
    from .format_memory_dependencies import memory_effect, memory_dependencies
else:
    from graphics_lift_inventory import Dol, decode, hx
    from platform_effect_inventory import implicit_consumers, platform_effect
    from platform_procedure_contract import build_contract, build_incoming_contract
    from particle_numeric_contract import numeric_effect
    from format_memory_dependencies import memory_effect, memory_dependencies

U32 = 0xffffffff
UNKNOWN = (0, 0)
TYPES = {0: ('float32', 4), 4: ('uint8', 1), 5: ('uint16', 2),
         6: ('int8', 1), 7: ('int16', 2)}


def constant(value):
    return U32, value & U32


def meet(left, right):
    mask = left[0] & right[0] & ~(left[1] ^ right[1]) & U32
    return mask, left[1] & mask


def signed(value, bits):
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


def rotate(value, shift):
    return ((value << shift) | (value >> ((32 - shift) % 32))) & U32


def gpr_dependencies(word):
    """Simultaneous destination→source register effects for the supported subset.

    Memory reads retain address dependencies, not invented loaded values.
    This vocabulary covers GPR inputs only; carry/overflow/CR values are NOT
    evaluated. Carry arithmetic results stay unknown in constant propagation.
    None is a barrier; {} is only an explicitly recognized non-GPR writer.
    """
    op,d,a,b=word>>26,(word>>21)&31,(word>>16)&31,(word>>11)&31
    xo=(word>>1)&1023
    if op in (14,15):return {d:[] if a==0 else [a]}
    if op==8:return {d:[a]}  # subfic: RT <- SIMM - RA; RA=0 is a real input.
    if op==7:return {d:[a]}
    if op in (20,23):return {a:[d,a] if op==20 else [d,b]}
    if op in (21,24,25,26,27,28,29):return {a:[d]}
    if op in (32,33,34,35,40,41,42,43):
        deps=[] if a==0 and not op&1 else [a]
        return {d:deps,**({a:[a]} if op&1 else {})}
    if op in (36,37,38,39,44,45):return {a:[a]} if op&1 else {}
    if op==46:return {r:[] if a==0 else [a] for r in range(d,32)}
    if op==47 or op in (10,11,16,18):return {}
    if op in (48,49,50,51,52,53,54,55,56,57,60,61):return {a:[a]} if op in (49,51,53,55) or op in (57,61) and a!=0 else {}
    if op in (4,59,63):
        effect=numeric_effect(word)
        if effect is None:return None
        mem=effect['memory']
        return {a:[a,b]} if mem and mem.get('updates_base') else {}
    if op==19 and xo in (16,528):return {}
    if op==31:
        # XO arithmetic permits OE at bit10; unary RB is reserved. XER carry
        # is deliberately not converted into a made-up GPR/constant input.
        if (xo&511) in (8,10,136,138):return {d:[a,b]}
        if (xo&511) in (200,202,232,234) and b==0:return {d:[a]}
        if xo in (339,371,19,83):return {d:[]}
        # Legal mfsr changes RT only. The selected segment value is external
        # state and stays UNKNOWN; normal-flow register preservation proves
        # neither privilege/fault continuation nor address mapping/lifetime.
        # Reject Rc, reserved bit20 and RB rather than broadening group31.
        if xo==595 and not word&1 and a<16 and b==0:return {d:[]}
        if xo in (467,0,32,144,278,598,854,982):return {}
        if xo in (266,40,235,747):return {d:[a,b]}
        if xo in (104,616) and b==0:return {d:[a]}
        if xo==26 and b==0:return {a:[d]}  # cntlzw: RA <- count(RS); RB is reserved.
        if xo in (444,28,316,24,536,792):return {a:[d,b]}
        if xo==824:return {a:[d]}
        if xo in (23,55,87,119,279,311,343,375):
            return {d:([b] if a==0 else [a,b]),**({a:[a,b]} if xo in (55,119,311,375) else {})}
    return None


def transfer(word, state, pc):
    """Partial bit constants plus complete reaching SPR-write/barrier identities.

    Unsupported words kill ALL facts, rather than guessing purity. Mathematical
    single/paired operations here preserve format definitions only; FP values,
    rounding, availability, fault/context and memory behavior are not evaluated.
    """
    values, origins = list(state[0]), list(state[1])
    definitions=list(state[2])
    op, d, a, b = word >> 26, (word >> 21) & 31, (word >> 16) & 31, (word >> 11) & 31
    xo, imm = (word >> 1) & 1023, word & 65535
    flow = decode(word, pc)
    barrier = 'call' if flow and flow.get('link') else None
    dependencies=gpr_dependencies(word)
    known = dependencies is not None
    if barrier:
        pass
    elif op in (14, 15):
        source = constant(0) if a == 0 else values[a]
        offset = signed(imm, 16) << (16 if op == 15 else 0)
        values[d] = constant(source[1] + offset) if source[0] == U32 else UNKNOWN
    elif op==7:
        values[d]=constant(values[a][1]*signed(imm,16)) if values[a][0]==U32 else UNKNOWN
    elif op in (24, 25, 26, 27, 28, 29):
        mask, value = values[d]
        bits = imm << (16 if op in (25, 27, 29) else 0)
        if op in (24, 25):
            values[a] = (mask | bits, (value | bits) & (mask | bits))
        elif op in (26, 27):
            values[a] = (mask, (value ^ bits) & mask)
        else:
            newmask = mask | (U32 ^ bits)
            values[a] = (newmask, value & bits & newmask)
    elif op in (20,21):
        shift, mb, me = (word >> 11) & 31, (word >> 6) & 31, (word >> 1) & 31
        indexes = range(mb, me + 1) if mb <= me else (*range(mb, 32), *range(me + 1))
        bits = sum(1 << (31 - i) for i in indexes)
        mask, value = values[d]
        if op==20:
            oldmask,oldvalue=values[a]
            newmask=(rotate(mask,shift)&bits)|(oldmask&(U32^bits))
            values[a]=(newmask,((rotate(value,shift)&bits)|(oldvalue&(U32^bits)))&newmask)
        else:
            newmask = rotate(mask, shift) | (U32 ^ bits)
            values[a] = (newmask, rotate(value, shift) & bits & newmask)
    elif op == 31 and xo in (339, 371):
        spr = ((word >> 16) & 31) | (((word >> 11) & 31) << 5)
        values[d] = values[32 + spr - 912] if xo == 339 and 912 <= spr < 920 else UNKNOWN
    elif op == 31 and xo == 467:
        spr = ((word >> 16) & 31) | (((word >> 11) & 31) << 5)
        if 912 <= spr < 920:
            values[32 + spr - 912] = values[d]
            origins[spr - 912] = frozenset({('write', pc)})
    elif op == 31 and xo == 444:
        lm, lv = values[d]; rm, rv = values[b]
        ones = lv | rv
        zeros = lm & ~lv & rm & ~rv & U32
        values[a] = (ones | zeros, ones)
    elif op in (32, 33, 34, 35, 40, 41, 42, 43):
        values[d] = (0xffffff00,0) if op in (34,35) else (0xffff0000,0) if op in (40,41) else UNKNOWN
        if op & 1: values[a] = UNKNOWN
    elif op in (36, 37, 38, 39, 44, 45):
        if op & 1: values[a] = UNKNOWN
    elif op == 46:
        values[d:32] = [UNKNOWN] * (32 - d)
    elif op == 47 or op in (10, 11, 16, 18):
        pass
    elif op in (48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 59, 60, 61, 63):
        if op in (49,51,53,55) or op in (57,61) and a!=0: values[a] = UNKNOWN
        # Only known FP encodings are pure with respect to GPR/GQR facts.
        if op in (59, 63):
            known = numeric_effect(word) is not None
    elif op == 4:
        if implicit_consumers(word):
            effect = numeric_effect(word)
            known = effect is not None
            if effect and effect['memory'] and effect['memory'].get('updates_base'):
                values[a] = UNKNOWN
        else: known = False
    elif op == 19 and xo in (16, 528):
        pass
    elif op == 31 and xo in (0, 32, 144, 278, 598, 854, 982):
        pass
    elif op == 31 and xo in (19, 83):
        values[d] = UNKNOWN
    elif op==31 and xo in (266,40):
        left,right=values[a],values[b]
        values[d]=constant(left[1]+right[1] if xo==266 else right[1]-left[1]) if left[0]==right[0]==U32 else UNKNOWN
    elif op==31 and xo in (235,747):
        left,right=values[a],values[b]
        values[d]=constant(left[1]*right[1]) if left[0]==right[0]==U32 else UNKNOWN
    elif op==31 and xo in (104,616) and b==0:
        values[d]=constant(-values[a][1]) if values[a][0]==U32 else UNKNOWN
    elif op==31 and xo==26 and b==0:
        values[a]=constant(32-values[d][1].bit_length()) if values[d][0]==U32 else UNKNOWN
    elif op==31 and xo in (23,55,87,119,279,311,343,375):
        values[d]=(0xffffff00,0) if xo in (87,119) else (0xffff0000,0) if xo in (279,311) else UNKNOWN
        if xo in (55,119,311,375):values[a]=UNKNOWN
    elif dependencies is not None:
        # Explicitly decoded writes still kill their destinations when the
        # numeric value operation is unsupported. Format definitions survive.
        for register in dependencies:values[register]=UNKNOWN
    else:
        known = False
    if not known: barrier = 'unsupported'
    if barrier:
        values = [UNKNOWN] * 40
        origins = [frozenset({(barrier, pc)}) for _ in range(8)]
        definitions=[frozenset({(barrier,pc,r)}) for r in range(32)]
    else:
        for register in dependencies:definitions[register]=frozenset({('definition',pc,register)})
    return (tuple(values), tuple(origins),tuple(definitions)), barrier


def native_format(value, store, elements):
    shift = 0 if store else 16
    mask, bits = value
    type_mask = 7 << shift
    if mask & type_mask != type_mask:
        return dict(resolved=False, reason='element type is not constant on all structural paths')
    kind = (bits >> shift) & 7
    if kind not in TYPES:
        return dict(resolved=False, reason='reserved element type; no fallback inferred', type_code=kind)
    name, width = TYPES[kind]
    scale_mask = 63 << (shift + 8)
    if kind and mask & scale_mask != scale_mask:
        return dict(resolved=False, reason='integer scale is not constant on all structural paths', type_code=kind)
    scale = None if kind == 0 else signed((bits >> (shift + 8)) & 63, 6)
    return dict(resolved=True, type_code=kind, element_type=name, elements=elements,
                element_bytes=width, request_bytes=width * elements, byte_order='big-endian',
                signed_scale=scale, scale_ignored=kind == 0,
                numeric_conversion_proven=False, native_resource_owner_proven=False)


def writer_slices(writers, nodes, format_writers=None):
    """Cross-domain producer slices; entry/call/unknown leaves stay unknown.

    A GQR read depends on its reaching writes, not just the instruction's empty
    GPR operand set. Follow those writes back into GPR sources. Register copies
    and loops never certify memory, callback, lifetime or context stability.
    """
    index={(n['site'],n['gpr']):n for n in nodes}
    if len(index)!=len(nodes):raise ValueError('duplicate original GPR definition')
    original_writers=writers if format_writers is None else format_writers
    bank_index={w['site']:w for w in original_writers}
    if len(bank_index)!=len(original_writers):raise ValueError('duplicate original format writer')
    result=[]
    for writer in writers:
        pending=[('gpr',r) for r in writer['source_definition_roots']]
        seen=set();selected=[];leaves=[];format_leaves=[];format_edges=[]
        while pending:
            domain,item=pending.pop()
            key=(domain,item['kind'],item['site'],item['gpr'] if domain=='gpr' else item['spr'])
            if key in seen:continue
            seen.add(key)
            if domain=='format':
                if item['kind']!='write':format_leaves.append(item);continue
                source=bank_index.get(item['site'])
                if source is None:raise ValueError('format provenance refers to a missing original writer')
                if source['spr']!=item['spr']:raise ValueError('format provenance refers to a different source bank')
                format_edges.append(dict(site=source['site'],spr=source['spr'],source_gpr=source['source_gpr'],
                    source_definition_roots=source['source_definition_roots']))
                pending.extend(('gpr',r) for r in source['source_definition_roots'])
                continue
            if item['kind']!='definition':leaves.append(item);continue
            node=index.get((item['site'],item['gpr']))
            if node is None:raise ValueError('producer graph refers to a missing original definition')
            selected.append(node)
            for dep in node['dependencies']:pending.extend(('gpr',r) for r in dep['reaching_definitions'])
            bank=node.get('format_read')
            if bank:
                pending.extend(('format',dict(r,spr=bank['spr'])) for r in bank['reaching_definitions'])
        result.append(dict(writer_site=writer['site'],spr=writer['spr'],
            nodes=sorted(selected,key=lambda n:(n['site'],n['gpr'])),
            unresolved_leaves=sorted(leaves,key=lambda n:(n['kind'],n['site'],n['gpr'])),
            format_writer_dependencies=sorted(format_edges,key=lambda n:(n['site'],n['spr'])),
            unresolved_format_leaves=sorted(format_leaves,key=lambda n:(n['kind'],n['site'],n['spr'])),
            memory_read_from_and_lifetime_proven=False,called_owner_closure_proven=False))
    return result


def implementation_pins():
    names=['gqr_format_dataflow.py','graphics_lift_inventory.py','platform_procedure_contract.py','platform_effect_inventory.py','particle_numeric_contract.py','format_memory_dependencies.py']
    return {n:hashlib.sha256(Path(__file__).with_name(n).read_bytes()).hexdigest() for n in names}


def read_original(path):
    with path.open('rb') as f:raw=f.read(64*1024*1024+1)
    if len(raw)>64*1024*1024:raise ValueError('original image exceeds supported DOL bound')
    return raw


def solve_format_states(raw, entry):
    """Canonical pre-instruction facts for both direct arms and loop joins.

    Consumers may project these facts; none may supply entry state, assume a
    call ABI, forward memory contents or turn virtual coordinates into owners.
    """
    procedure = build_contract(raw, entry)
    words = {int(row['site'], 16): int(row['word'], 16) for row in procedure['words']}
    successors = {}
    for pc, word in words.items():
        flow = decode(word, pc); targets = []
        if flow and not flow.get('return_like') and not flow.get('link') and flow.get('target') is not None:
            targets.append(flow['target'])
        if flow is None or flow.get('may_fall_through'): targets.append(pc + 4)
        successors[pc] = [target for target in targets if target in words]
    initial = (tuple([UNKNOWN] * 40), tuple(frozenset({('entry', entry)}) for _ in range(8)),
               tuple(frozenset({('entry',entry,r)}) for r in range(32)))
    states = {entry: initial}; pending = deque([entry]); queued = {entry}; updates = 0
    while pending:
        pc = pending.popleft(); queued.remove(pc)
        result, _ = transfer(words[pc], states[pc], pc)
        for target in successors[pc]:
            old = states.get(target)
            merged = result if old is None else (
                tuple(meet(x, y) for x, y in zip(old[0], result[0])),
                tuple(x | y for x, y in zip(old[1], result[1])),
                tuple(x | y for x, y in zip(old[2], result[2])))
            if old != merged:
                updates += 1; states[target] = merged
                if target not in queued: pending.append(target); queued.add(target)
    return procedure, words, successors, states, updates


def build_format_contract(raw, entry):
    procedure, words, successors, states, updates = solve_format_states(raw, entry)
    consumers, writers, barriers, nodes, memory, call_inputs, format_reads, call_register_inputs = [], [], [], [], [], [], [], []
    def refs(items):return [dict(kind=k,site=hx(p),gpr=r) for k,p,r in sorted(items)]
    for pc in sorted(states):
        word = words[pc]; state = states[pc]
        access=memory_effect(word,state,pc)
        if access:memory.append(access)
        flow=decode(word,pc)
        if flow and flow.get('link'):
            call_register_inputs.append(dict(site=hx(pc),word=hx(word),
                target=None if flow.get('target') is None else hx(flow['target']),
                registers=[dict(gpr=i,known_mask=hx(state[0][i][0]),known_bits=hx(state[0][i][1]),
                    source_definition_roots=refs(state[2][i])) for i in range(32)],
                scope='All encoded GPR inputs before possible call; no argument ABI, pointer identity, memory value or callee preservation inferred.',
                argument_ownership_or_dynamic_execution_proven=False))
            call_inputs.append(dict(site=hx(pc),word=hx(word),
                target=None if flow.get('target') is None else hx(flow['target']),
                formats=[dict(spr=912+i,known_mask=hx(state[0][32+i][0]),known_bits=hx(state[0][32+i][1]),
                    reaching_definitions=[dict(kind=k,site=hx(p)) for k,p in sorted(state[1][i])]) for i in range(8)],
                scope='Pre-call structural facts only; callee, faults and asynchronous observers not closed.',
                dynamic_execution_or_callee_preservation_proven=False))
        _, barrier = transfer(word, state, pc)
        if barrier: barriers.append(dict(site=hx(pc), kind=barrier, word=hx(word)))
        effect = platform_effect(word)
        deps=gpr_dependencies(word)
        if not barrier:
            for register,sources in deps.items():
                node=dict(kind='definition',site=hx(pc),word=hx(word),gpr=register,
                    dependencies=[dict(source_gpr=s,reaching_definitions=refs(state[2][s])) for s in sorted(set(sources))],
                    loaded_memory_value_proven=False)
                # Carry arithmetic has a non-GPR input. Never let an all-GPR
                # producer slice look like a complete semantic input proof.
                if word>>26==31 and (((word>>1)&1023)&511) in (136,138,200,202,232,234):
                    node['implicit_state_inputs']=['xer.ca']
                    node['implicit_state_values_proven']=False
                if effect and effect.get('operation')=='mfspr' and 912<=effect.get('spr',0)<920:
                    node['format_read']=dict(spr=effect['spr'],
                        reaching_definitions=[dict(kind=k,site=hx(p)) for k,p in sorted(state[1][effect['spr']-912])])
                nodes.append(node)
        if effect and effect.get('operation')=='mfspr' and 912<=effect.get('spr',0)<920:
            slot=effect['spr']-912
            format_reads.append(dict(site=hx(pc),word=hx(word),spr=effect['spr'],gpr=effect['gpr'],
                known_mask=hx(state[0][32+slot][0]),known_bits=hx(state[0][32+slot][1]),
                required_mask='0xffffffff',
                source_definition_roots=[dict(kind='definition',site=hx(pc),gpr=effect['gpr'])],
                raw_state_observer=True,execution_or_context_lifetime_proven=False))
        if effect and effect.get('operation') == 'mtspr' and 912 <= effect.get('spr', 0) < 920:
            mask, value = state[0][effect['gpr']]
            writers.append(dict(site=hx(pc), word=hx(word), spr=effect['spr'], source_gpr=effect['gpr'],
                                input_known_mask=hx(mask), input_known_bits=hx(value),
                                source_definition_roots=refs(state[2][effect['gpr']])))
        for consumer in implicit_consumers(word):
            if consumer['domain'] != 'quantized_numeric_semantics': continue
            slot = consumer['spr'] - 912; store = consumer['consumer'] == 'psq_store'
            one = bool(word & (0x400 if word >> 26 == 4 else 0x8000))
            mask, value = state[0][32 + slot]
            origins = [dict(kind=kind, site=hx(site)) for kind, site in sorted(state[1][slot])]
            consumers.append(dict(site=hx(pc), word=hx(word), spr=consumer['spr'], operation=consumer['consumer'],
                required_mask=consumer['required_mask'], input_known_mask=hx(mask), input_known_bits=hx(value),
                reaching_definitions=origins, candidate_format=native_format((mask, value), store, 1 if one else 2),
                execution_proven=False, asynchronous_context_closure_proven=False))
    slices=writer_slices(writers,nodes)
    read_slices=writer_slices(format_reads,nodes,format_writers=writers)
    for sl in read_slices:sl['read_site']=sl.pop('writer_site')
    dependencies=memory_dependencies(slices,memory,successors,barriers)
    return dict(schema='gqr_format_dataflow_candidate_v6', entry=hx(entry), inputs=procedure['inputs'],
        scope=dict(values='Original-word abstract bit constants only; entry state unknown; both branches and loops.',
            calls='All GPR/GQR facts invalidated; no inferred ABI or callee preservation.',
            bound='One state per file-backed reachable word; monotone finite bit/reaching-definition lattice.',
            context_fault_availability_memory_alias_closure_proven=False,
            runtime_liveness_proven=False, shipping_consumer_zero_proven=False,
            native_retirement_proven=False, deletion_authorized=False),
        summary=dict(structural_words=len(words), dataflow_updates=updates, format_writers=len(writers),
            quantized_consumers=len(consumers), structurally_resolved_formats=sum(x['candidate_format']['resolved'] for x in consumers),
            barriers=dict(Counter(x['kind'] for x in barriers))),
        writers=writers, consumers=consumers, barriers=barriers, gpr_definition_nodes=nodes,
        format_reads=format_reads,format_read_source_slices=read_slices,
        writer_source_slices=slices, memory_effects=memory, writer_memory_dependencies=dependencies,
        call_format_inputs=call_inputs,call_register_inputs=call_register_inputs,calls=procedure['calls'],
        unresolved_terminals=procedure['unresolved_terminals'], outside_text_edges=procedure['outside_text_edges'])


def main(argv=None):
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--dol',type=Path,required=True);p.add_argument('--entry',action='append',type=lambda x:int(x,0),required=True)
    p.add_argument('--out',type=Path,required=True);p.add_argument('--incoming',action='store_true')
    args=p.parse_args(argv)
    try:
        if args.out.resolve()==args.dol.resolve():raise ValueError('output may not overwrite input')
        if args.dol.stat().st_size>64*1024*1024:raise ValueError('original image exceeds supported DOL bound')
        if len(set(args.entry))!=len(args.entry):raise ValueError('duplicate entry')
        pins=implementation_pins()
        raw=read_original(args.dol);Dol(raw)
        result=dict(schema='gqr_format_dataflow_bundle_v6',contracts=[build_format_contract(raw,e) for e in sorted(args.entry)])
        if args.incoming:result['incoming_reference_candidates']=[build_incoming_contract(raw,e) for e in sorted(args.entry)]
        if pins!=implementation_pins():raise ValueError('analysis implementation changed during extraction')
        if read_original(args.dol)!=raw:raise ValueError('original image changed during extraction')
        result['source_pins']=pins
        with args.out.open('x',encoding='utf-8',newline='\n') as f:json.dump(result,f,indent=2);f.write('\n')
        print(json.dumps([dict(entry=c['entry'],**c['summary']) for c in result['contracts']]))
    except (OSError,ValueError,TypeError) as error:
        print('format dataflow refused: '+str(error),file=sys.stderr);return 1
    return 0


if __name__=='__main__':raise SystemExit(main())
