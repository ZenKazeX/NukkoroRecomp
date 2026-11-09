"""Shadow DOL discovery workbench: structural candidates are NEVER semantic proof.

Original words, uncertain extraction ranges and overlaps remain intact. Only an
explicit reviewed subset permits register alpha-renaming/branch relocation.
Unknown instructions, constants, condition bits, stack/SDA registers and hardware
words stay exact. No instruction is dropped as harmless. Labels are annotations.
"""
import argparse
from collections import Counter,defaultdict
import hashlib,json,re,struct
from pathlib import Path
from xml.sax.saxutils import escape
if __package__:
    from .graphics_lift_inventory import Dol,decode,hx,table,Extents
    from .gqr_format_dataflow import gpr_dependencies
    from .format_memory_dependencies import memory_effect
    from .platform_effect_inventory import platform_effect
else:
    from graphics_lift_inventory import Dol,decode,hx,table,Extents
    from gqr_format_dataflow import gpr_dependencies
    from format_memory_dependencies import memory_effect
    from platform_effect_inventory import platform_effect

CLASSES=('PROVEN','STRONGLY SUPPORTED','STRUCTURAL MATCH','UNKNOWN')
SCHEMA='shadow_semantic_patterns_v1'
UNKNOWN_STATE=(tuple([(0,0)]*40),tuple(frozenset() for _ in range(8)),tuple(frozenset() for _ in range(32)))
def digest(data):return hashlib.sha256(data).hexdigest()
def signature(value):return digest(json.dumps(value,sort_keys=True,separators=(',',':')).encode())
def signed16(n):return n-65536 if n&32768 else n
def implementation_pins():
    names=('shadow_semantic_patterns.py','graphics_lift_inventory.py','gqr_format_dataflow.py',
           'format_memory_dependencies.py','particle_numeric_contract.py','platform_effect_inventory.py',
           'platform_callback_setter_contract.py','platform_callback_toggle_contract.py',
           'platform_callback_exchange.py','particle_lane_events.py','platform_bounded_switch_contract.py',
           'platform_callback_reload_contract.py','platform_callback_subscription_contract.py',
           'platform_procedure_contract.py','platform_callback_registry_contract.py',
           'platform_callback_bank_contract.py','platform_indirect_guard_contract.py','platform_callback_value_flow.py',
           'platform_register_transfer_leaf.py','platform_leaf_register_effect.py','platform_call_coordinate_flow.py')
    return {n:digest(Path(__file__).with_name(n).read_bytes()) for n in names}

class Registers:
    def __init__(self):self.names={}
    def __call__(self,r):
        # These are distinguished encodings, not a proof of the active ABI.
        if r in (0,1,2,13):return 'r'+str(r)
        if r not in self.names:self.names[r]='v'+str(len(self.names))
        return self.names[r]

def normalize(word,pc,entry,end,register):
    op,d,a,b=word>>26,(word>>21)&31,(word>>16)&31,(word>>11)&31
    flow=decode(word,pc)
    if flow:
        result={k:v for k,v in flow.items() if k not in ('target','absolute')}
        if flow['target'] is not None:
            target=flow['target'];result['destination']=('internal',(target-entry)//4) if entry<=target<end else ('external',)
            # AA is preserved. Raw external targets are separately retained.
            result['absolute']=flow['absolute']
        return ('flow',result)
    if op in (14,15):return ('immediate',op,register(d),'ZERO' if a==0 else register(a),signed16(word&65535))
    if op in (24,25,26,27,28,29):return ('logical-immediate',op,register(d),register(a),word&65535)
    if op in (32,33,34,35,36,37,38,39,40,41,42,43,44,45):
        zero=a==0 and op not in (33,35,37,39,41,43,45)
        return ('integer-memory',op,register(d),'ZERO' if zero else register(a),signed16(word&65535))
    if op==7:return ('multiply-immediate',op,register(d),register(a),signed16(word&65535))
    if op in (20,21):return ('rotate',op,register(d),register(a),word&65535)
    if op==31 and ((word>>1)&1023)==444:
        return ('or',register(d),register(a),register(b),word&1)
    # SPR/MSR/FPR/quantized/multiple-register/reserved/unknown encodings exact.
    return ('opaque',hx(word))

def data_objects(image,address,limit=128):
    """Raw string/table candidates. Pointer-looking bytes are NOT vtables/RTTI."""
    for s in image.sections:
        if s['address']<=address<s['address']+s['size']:
            off=s['file_offset']+address-s['address'];available=s['address']+s['size']-address
            blob=image.raw[off:off+min(available,limit)];out=dict(address=hx(address),file_offset=off,raw_prefix_sha256=digest(blob),available_bytes=available)
            stop=blob.find(b'\0')
            if stop>=4 and all(32<=x<127 for x in blob[:stop]):out['ascii_string_candidate']=blob[:stop].decode('ascii')
            targets=[]
            if address%4==0:
                for i in range(0,min(len(blob),32)-3,4):
                    value=int.from_bytes(blob[i:i+4],'big')
                    if not image.contains_text(value,4):break
                    targets.append(hx(value))
            if len(targets)>=2:out['code_pointer_table_candidate']=targets
            out['rtti_vtable_or_object_identity_proven']=False
            return out
    return dict(address=hx(address),file_backed=False,rtti_vtable_or_object_identity_proven=False)

def describe(image,row):
    entry,end=row['address'],row['address']+row['size']
    # Each feature owns its register namespace. Learning a new dependency must
    # not renumber unrelated instruction or memory-footprint signatures.
    regs=Registers();memory_regs=Registers();dataflow_regs=Registers()
    words=[image.word(pc) for pc in range(entry,end,4)]
    norm=[];edges=[];calls=[];memory=[];dataflow=[];constants=[];platform=[];definitions={};opaque=0
    stack=[];addresses=[]
    for i,word in enumerate(words):
        pc=entry+4*i;token=normalize(word,pc,entry,end,regs);norm.append(token);opaque+=token[0]=='opaque'
        flow=decode(word,pc)
        if flow:
            target=flow['target'];edge=dict(offset=i,mnemonic=flow['mnemonic'],conditional=flow['conditional'],link=flow['link'],target=hx(target) if target is not None else None,source=flow.get('target_source'))
            edges.append(edge)
            if flow['link']:calls.append(dict(site=hx(pc),target=edge['target'],source=edge['source']))
        effect=memory_effect(word,UNKNOWN_STATE,pc)
        if effect:
            footprint={k:v for k,v in effect.items() if k not in ('address_sources','site','word','value_registers')}
            for key in ('base_gpr','index_gpr'):
                if key in footprint:footprint[key]=memory_regs(footprint[key])
            memory.append(footprint)
        dependencies=gpr_dependencies(word)
        if dependencies is None:dataflow.append(('unknown',hx(word)));definitions.clear()
        else:
            updates=[]
            for dest,sources in sorted(dependencies.items()):
                updates.append((dataflow_regs(dest),tuple((dataflow_regs(s),definitions.get(s,'entry-or-barrier')) for s in sources)))
            dataflow.append(updates)
            for dest in dependencies:definitions[dest]=i
        if flow:definitions.clear()  # no guessed reaching definitions across joins/calls
        pe=platform_effect(word)
        if pe:platform.append(dict(site=hx(pc),word=hx(word),effect=pe))
        op,a,d=word>>26,(word>>16)&31,(word>>21)&31
        if op in (7,14,15,24,25,26,27,28,29):constants.append(dict(offset=i,op=op,encoding=word&65535))
        if op==37 and a==1 and d==1:stack.append(dict(offset=i,allocation=signed16(word&65535)))
        if op==15 and a==0 and i+1<len(words):
            nxt=words[i+1];nd,na=(nxt>>21)&31,(nxt>>16)&31
            if nxt>>26==14 and na==d:
                # Original adjacent arithmetic only; no loaded value or relocation claim.
                value=((signed16(word&65535)<<16)+signed16(nxt&65535))&0xffffffff
                addresses.append(dict(site=hx(pc),result_gpr=nd,value=hx(value),object=data_objects(image,value),is_address_or_relocation_proven=False))
    normalized=signature(norm);grams=sorted({signature(norm[i:i+3]) for i in range(max(0,len(norm)-2))})
    raw_grams=sorted({digest(struct.pack('>III',*words[i:i+3])) for i in range(max(0,len(words)-2))})
    cfg=[]
    for e in edges:
        t=int(e['target'],16) if e['target'] else None
        cfg.append(dict(offset=e['offset'],mnemonic=e['mnemonic'],conditional=e['conditional'],link=e['link'],destination=((t-entry)//4 if t is not None and entry<=t<end else 'external-or-indirect')))
    footprint_counts=Counter(json.dumps(x,sort_keys=True) for x in memory)
    abi=dict(stack_allocations=stack,mflr_offsets=[i for i,x in enumerate(words) if x==0x7c0802a6],mtlr_offsets=[i for i,x in enumerate(words) if x==0x7c0803a6],canonical_returns=[i for i,x in enumerate(words) if x==0x4e800020])
    return dict(id=row['id'],entry=hx(entry),size_bytes=row['size'],annotation_name=row['name'],
        provenance=dict(file_offset=image.offset(entry),original_bytes_sha256=digest(struct.pack('>'+str(len(words))+'I',*words))),
        signals=dict(normalized_sequence=normalized,cfg_shape=signature(cfg),register_dataflow=signature(dataflow),memory_footprint=signature(memory),abi_stack=signature(abi),constant_usage=signature(constants)),
        raw_ngram_hashes=raw_grams[:8],normalized_ngrams=grams[:8],
        raw_ngram_set_sha256=signature(raw_grams),raw_ngram_count=len(raw_grams),normalized_ngram_set_sha256=signature(grams),normalized_ngram_count=len(grams),
        ngram_examples_limit=8,ngram_examples_are_not_complete=(len(raw_grams)>8 or len(grams)>8),
        cfg_edges=edges,callers=[],calls=calls,abi_signature=abi,memory_footprint_counts=dict(footprint_counts),constant_usage=constants,
        address_usage_candidates=addresses,platform_sites=platform,opaque_words=opaque,word_count=len(words),
        cfg_scope='Declared original extent only; external tails/indirect closure/true function identity UNKNOWN.',
        semantic_roles=dict(constructor='UNKNOWN',destructor='UNKNOWN',state_machine='UNKNOWN',renderware_gx='UNKNOWN',gameplay_enemy_player_task='UNKNOWN',producer_consumer='Encoded memory/call candidates only; aliases/lifetime UNKNOWN'),
        classification='UNKNOWN',semantic_proof=False,native_retirement_proven=False)

def match_evidence(left,right):
    signals=[name for name in ('normalized_sequence','cfg_shape','register_dataflow','memory_footprint','abi_stack') if left['signals'][name]==right['signals'][name]]
    grams_equal=left['normalized_ngram_count']==right['normalized_ngram_count'] and left['normalized_ngram_set_sha256']==right['normalized_ngram_set_sha256']
    jaccard=1 if grams_equal and left['normalized_ngram_count'] else None
    # Different representations are corroboration, not independent semantic proofs.
    informative=left['word_count']>=4 and right['word_count']>=4 and bool(left['calls'] or left['memory_footprint_counts'] or left['platform_sites'])
    accepted=informative and all(k in signals for k in ('normalized_sequence','cfg_shape','register_dataflow','memory_footprint','abi_stack')) and jaccard==1
    return dict(classification='STRUCTURAL MATCH' if accepted else 'UNKNOWN',evidence_channels=signals,normalized_ngram_jaccard=jaccard,
        raw_bytes_equal=left['provenance']['original_bytes_sha256']==right['provenance']['original_bytes_sha256'],
        shared_semantics='UNKNOWN: candidate encoded structure only',differences=dict(entries=[left['entry'],right['entry']],constants_equal=left['signals']['constant_usage']==right['signals']['constant_usage'],raw_bytes_equal=left['provenance']['original_bytes_sha256']==right['provenance']['original_bytes_sha256']),semantic_equivalence_proven=False)

def resolve_references(raw,references):
    """Only reviewed recognizers can prove their named, limited SOURCE contract."""
    if __package__:
        from .platform_callback_setter_contract import recognize_setter
    else:
        from platform_callback_setter_contract import recognize_setter
    if not isinstance(references,(list,tuple)):raise ValueError('reference list required')
    image=Dol(raw);out=[];ids=set()
    for ref in references:
        if not isinstance(ref,dict):raise ValueError('reference record required')
        if set(ref)!={'id','entry','recognizer','dol_sha256','original_bytes_sha256','word_count','role'}:raise ValueError('reference schema unknown')
        if not isinstance(ref['id'],str) or not ref['id'] or ref['id'] in ids:raise ValueError('duplicate/invalid reference id')
        ids.add(ref['id'])
        if ref['role'] not in ('training','held-out'):raise ValueError('reference split required')
        if ref['dol_sha256']!=digest(raw) or ref['recognizer']!='callback-setter-read-before-exclusion' or ref['word_count']!=17:raise ValueError('unsupported/stale reference proof')
        if not isinstance(ref['entry'],str) or not re.fullmatch(r'0x[0-9a-fA-F]{8}',ref['entry']):raise ValueError('reference entry')
        entry=int(ref['entry'],16);blob=b''.join(image.word(entry+4*i).to_bytes(4,'big') for i in range(17))
        if digest(blob)!=ref['original_bytes_sha256']:raise ValueError('reference words changed')
        contract=recognize_setter(raw,entry)
        out.append(dict(reference=ref,classification='PROVEN',scope='Reviewed original setter sequence and normal-return leaf GPR preservation ONLY.',source_contract=contract,full_semantic_equivalence_proven=False,native_retirement_proven=False))
    return out

def build_pipeline(raw,functions_raw,*,max_words=4096,max_work_words=None,references=()):
    if type(max_words) is not int or not 4<=max_words<=65536:raise ValueError('bounded extent limit required')
    if len(functions_raw)>32*1024*1024:raise ValueError('CSV exceeds diagnostic input bound')
    image=Dol(raw);rows=table(functions_raw,'functions',image);extents=Extents(rows)
    text_words=sum(s['size']//4 for s in image.text)
    work_limit=text_words*4 if max_work_words is None else max_work_words
    if type(work_limit) is not int or work_limit<=0:raise ValueError('positive total work budget required')
    described=[];omitted=[];work=0
    for row in rows:
        n=row['size']//4
        reason='extent-exceeds-diagnostic-limit' if n>max_words else 'total-work-limit' if work+n>work_limit else None
        if reason:omitted.append(dict(id=row['id'],entry=hx(row['address']),word_count=n,reason=reason));continue
        described.append(describe(image,row));work+=n
    incoming_sites=defaultdict(dict);by_entry=defaultdict(list);source_calls=[]
    for d in described:by_entry[d['entry']].append(d['id'])
    # Direct caller accounting comes from every ORIGINAL text word, not from
    # repeated/overlapping annotations or only the selected diagnostic extents.
    for section in image.text:
        for pc in range(section['address'],section['address']+section['size'],4):
            word=image.word(pc);flow=decode(word,pc)
            if not flow or not flow.get('link'):continue
            site=hx(pc);target=hx(flow['target']) if flow['target'] is not None else None
            owners=list(extents.at(pc) or ())
            source_calls.append(dict(site=site,word=hx(word),target=target,target_source=flow.get('target_source'),declaration_owner_annotations=owners))
            if target:incoming_sites[target][site]=owners
    incoming={target:[dict(site=site,declaration_owner_annotations=ids) for site,ids in sorted(sites.items())] for target,sites in incoming_sites.items()}
    def callers(target):return incoming.get(target,[])
    groups=defaultdict(list)
    for d in described:
        d['callers']=[dict(site=x['site']) for x in callers(d['entry'])]
        d['call_neighborhood_signature']=signature([(x['target'],len(callers(x['target']))) for x in d['calls']])
        groups[tuple(d['signals'][k] for k in ('normalized_sequence','cfg_shape','register_dataflow','memory_footprint','abi_stack'))].append(d)
    families=[]
    for members in groups.values():
        if len(members)<2:continue
        ev=match_evidence(members[0],members[1])
        if ev['classification']!='STRUCTURAL MATCH':continue
        family='candidate-'+members[0]['signals']['normalized_sequence'][:16]
        same_entries=len({m['entry'] for m in members})<len(members)
        families.append(dict(candidate_family=family,classification='STRUCTURAL MATCH',evidence=ev,shared_semantics=ev['shared_semantics'],differences=[dict(id=m['id'],entry=m['entry'],original_bytes_sha256=m['provenance']['original_bytes_sha256'],constants=m['constant_usage']) for m in members],
            members=[m['id'] for m in members],callers_consumers='Resolve member descriptor IDs: full raw call targets/callers/memory footprints retained once.',
            duplicate_entry_annotations=same_entries,normalization_is_not_semantic_proof=True,held_out_semantic_validation_passed=False))
    refs=resolve_references(raw,references)
    # An existing solved family is automatically tested at candidate starts.
    # Acceptance is its exact reviewed source recognizer, never a cluster vote.
    if __package__:
        from .platform_callback_setter_contract import recognize_setter
        from .platform_callback_toggle_contract import recognize_toggle
        from .platform_callback_exchange import recognize_exchange
        from .platform_bounded_switch_contract import recognize_switch
        from .platform_callback_reload_contract import scan_reloads
        from .platform_callback_subscription_contract import scan_subscriptions
        from .platform_callback_registry_contract import scan_registries
        from .platform_callback_bank_contract import scan_banks
        from .platform_register_transfer_leaf import scan_register_transfer_leaves
    else:
        from platform_callback_setter_contract import recognize_setter
        from platform_callback_toggle_contract import recognize_toggle
        from platform_callback_exchange import recognize_exchange
        from platform_bounded_switch_contract import recognize_switch
        from platform_callback_reload_contract import scan_reloads
        from platform_callback_subscription_contract import scan_subscriptions
        from platform_callback_registry_contract import scan_registries
        from platform_callback_bank_contract import scan_banks
        from platform_register_transfer_leaf import scan_register_transfer_leaves
    recognized=[]
    for d in described:
        entry=int(d['entry'],16);first=image.word(entry)
        attempts=[]
        if d['word_count']>=17 and first==0x7c0802a6:attempts.append(('callback-setter-read-before-exclusion',recognize_setter))
        if d['word_count']>=28 and first==0x7c0802a6:attempts.append(('optional-callback-toggle',recognize_toggle))
        if d['word_count']>=7 and first>>26==21:attempts.append(('indexed-callback-exchange',recognize_exchange))
        for family,recognizer in attempts:
            try:contract=recognizer(raw,entry)
            except ValueError:continue
            recognized.append(dict(candidate_family=family,entry=d['entry'],id=d['id'],classification='PROVEN',
                shared_semantics='Reviewed recognizer SOURCE contract only: see literal effects/order and remaining unknowns.',
                differences=dict(original_entry=d['entry'],original_words=contract['original_words']),evidence=contract,
                proof_scope='Exact reviewed original sequence ONLY; full function/runtime/fault/alias/lifetime semantics UNKNOWN.',
                callers_consumers='Resolve source descriptor ID for complete encoded call and memory candidates.',native_retirement_proven=False))
    switches=[]
    for section in image.text:
        for pc in range(section['address'],section['address']+section['size'],4):
            if image.word(pc)>>26!=10:continue
            contract=recognize_switch(raw,pc)
            if contract is not None:switches.append(dict(candidate_family='bounded-unsigned-index-CTR-dispatch',classification='PROVEN',evidence=contract,
                shared_semantics='Source unsigned guard, scaled original table lookup and CTR branch; external-entry domination/live table/lifetime UNKNOWN.',
                differences=dict(compare_site=hx(pc),table=contract['original_table_address'],maximum=contract['admitted_unsigned_maximum']),native_retirement_proven=False))
    # Correlate training/held-out source matches, but never transfer their proof.
    reloads=scan_reloads(raw)
    subscriptions=scan_subscriptions(raw)
    registries=scan_registries(raw)
    banks=scan_banks(raw)
    # Whole-text source pairs; matches never relabel descriptors or rankings.
    transfers=scan_register_transfer_leaves(raw)
    reference_index={x['reference']['entry']:x for x in refs}
    for d in described:
        if d['entry'] in reference_index:d['reviewed_source_reference']=reference_index[d['entry']]['reference']['id']
    membership={i:len(f['members']) for f in families for i in f['members']}
    ranked=[]
    for d in described:
        consumers=len(callers(d['entry']));fanout=len({c['target'] for c in d['calls'] if c['target']})
        family_size=membership.get(d['id'],1)
        ranked.append(dict(id=d['id'],entry=d['entry'],candidate_family_size=family_size,source_direct_callers=consumers,source_direct_callees=fanout,platform_site_count=len(d['platform_sites']),
            score=(family_size-1)*4+consumers+min(fanout,32)+len(d['platform_sites'])*2,
            objective='Potential source recognition leverage; NOT measured runtime cost or deletion priority.',known_source_contract=d['entry'] in reference_index,semantic_role='UNKNOWN'))
    ranked.sort(key=lambda x:(-x['score'],x['entry'],x['id']))
    return dict(schema=SCHEMA,inputs=dict(dol_sha256=digest(raw),functions_sha256=digest(functions_raw)),source_pins=implementation_pins(),
        scope=dict(semantic_similarity_never_source_proof=True,native_retirement_proven=False,true_function_ownership_proven=False,full_dol_execution_coverage=False,normalization='Only explicit integer subset and branch targets; constants/condition bits/SPR/FPR/GQR/stack/SDA/unknowns preserved; no NOP or instruction removal.',rtti='Address/string/code-pointer candidates only; use reviewed RTTI reader for actual type graphs.',declaration_overlap_not_clamped=True),
        summary=dict(declared_ranges=len(rows),described_ranges=len(described),omitted_ranges=len(omitted),source_text_words=text_words,processed_declared_words=work,total_work_limit=work_limit,max_words_per_extent=max_words,structural_families=len(families),candidate_members=sum(len(x['members']) for x in families),reviewed_source_references=len(refs),reviewed_source_recognizer_matches=len(recognized),reviewed_switch_source_matches=len(switches),reviewed_reload_source_matches=reloads['summary']['source_contracts'],reviewed_subscription_source_matches=subscriptions['summary']['source_contracts'],reviewed_registry_source_pairs=registries['summary']['reviewed_registry_pairs'],reviewed_bank_source_matches=banks['summary']['reviewed_source_banks'],reviewed_register_transfer_leaf_matches=transfers['summary']['matches'],reviewed_register_transfer_isolated_leaves=transfers['summary']['isolated_leaves'],original_call_sites=len(source_calls),declared_union_words=sum((b-a)//4 for a,b,_ in extents.spans)),
        omitted_ranges=omitted,declaration_overlaps=extents.overlaps(),descriptors=described,families=families,references=refs,source_recognizer_matches=recognized,source_switch_matches=switches,source_reload_matches=reloads,source_subscription_matches=subscriptions,source_registry_matches=registries,source_bank_matches=banks,source_register_transfer_matches=transfers,source_call_index=source_calls,
        undeclared_direct_call_targets=sorted({c['target'] for c in source_calls if c['target'] and image.contains_text(int(c['target'],16),4) and c['target'] not in by_entry}),
        unknown_ranking=ranked)

def compare_scans(before,after):
    if before.get('schema')!=SCHEMA or after.get('schema')!=SCHEMA:raise ValueError('scan schema unknown')
    if before['inputs']!=after['inputs']:raise ValueError('DOL/declaration provenance differs: not a tooling-only rescan')
    old={d['id']:d for d in before['descriptors']};new={d['id']:d for d in after['descriptors']}
    return dict(inputs=after['inputs'],source_tool_versions_changed=before['source_pins']!=after['source_pins'],
        added=sorted(new.keys()-old.keys()),removed=sorted(old.keys()-new.keys()),
        signal_changes=[dict(id=i,before=old[i]['signals'],after=new[i]['signals']) for i in sorted(old.keys()&new.keys()) if old[i]['signals']!=new[i]['signals']],
        before_summary=before['summary'],after_summary=after['summary'],earlier_semantic_proofs_revalidated=False,
        required_action='Reopen affected source recognizers/contracts and rerun their semantic/parity gates; clustering changes are not proof.')

def graph_svg(report,limit=64):
    """Bounded readable graph. Whole graph stays in the source-bound JSON."""
    nodes=report['unknown_ranking'][:limit];selected={x['id']:i for i,x in enumerate(nodes)}
    width=1200;height=max(180,80+45*((len(nodes)+3)//4))
    parts=[f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
           '<rect width="100%" height="100%" fill="#101722"/>',f'<text x="20" y="25" fill="#fff" font-family="sans-serif">Source call candidates: top {len(nodes)} of {len(report["descriptors"])} ranges; UNKNOWN ownership</text>']
    for d in report['descriptors']:
        if d['id'] not in selected:continue
        i=selected[d['id']];x=25+295*(i%4);y=45+45*(i//4)
        for c in d['calls']:
            targets=[n for n in nodes if n['entry']==c['target']]
            for t in targets:
                j=selected[t['id']];tx=25+295*(j%4);ty=45+45*(j//4)
                parts.append(f'<path d="M{x+100},{y+15} L{tx+100},{ty+15}" stroke="#567591" opacity=".25"/>')
    for i,n in enumerate(nodes):
        x=25+295*(i%4);y=45+45*(i//4)
        parts.extend([f'<rect x="{x}" y="{y}" width="270" height="32" rx="5" fill="#24394f"/>',f'<text x="{x+8}" y="{y+21}" fill="#dcefff" font-size="13" font-family="monospace">{escape(n["entry"])} score {n["score"]} family {n["candidate_family_size"]}</text>'])
    parts.append('</svg>');return '\n'.join(parts)

def strict_json(data):
    def pairs(items):
        out={}
        for k,v in items:
            if k in out:raise ValueError('duplicate JSON key')
            out[k]=v
        return out
    return json.loads(data,object_pairs_hook=pairs,parse_constant=lambda x:(_ for _ in ()).throw(ValueError('nonfinite JSON')))
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--dol',type=Path,required=True);p.add_argument('--functions',type=Path,required=True)
    p.add_argument('--out',type=Path,required=True);p.add_argument('--references',type=Path);p.add_argument('--prior-scan',type=Path);p.add_argument('--max-words',type=int,default=4096);p.add_argument('--max-work-words',type=int)
    args=p.parse_args();paths=[args.dol,args.functions]+([args.references] if args.references else [])+([args.prior_scan] if args.prior_scan else [])
    pins={str(x.resolve()):digest(x.read_bytes()) for x in paths}
    if args.out.exists():raise ValueError('preserve previous runs; output already exists')
    refs=strict_json(args.references.read_text(encoding='utf-8')) if args.references else []
    report=build_pipeline(args.dol.read_bytes(),args.functions.read_bytes(),max_words=args.max_words,max_work_words=args.max_work_words,references=refs)
    for x in paths:
        if digest(x.read_bytes())!=pins[str(x.resolve())]:raise ValueError('input changed during scan')
    args.out.mkdir();report['input_file_pins']=pins
    outputs={'discovery.json':json.dumps(report,indent=2,allow_nan=False)+'\n','next-targets.json':json.dumps(report['unknown_ranking'],indent=2)+'\n','source-graph.svg':graph_svg(report)}
    if args.prior_scan:outputs['rescan-delta.json']=json.dumps(compare_scans(strict_json(args.prior_scan.read_text(encoding='utf-8')),report),indent=2)+'\n'
    for name,text in outputs.items():
        with (args.out/name).open('x',encoding='utf-8') as f:f.write(text)
    with (args.out/'completed-evidence.json').open('x',encoding='utf-8') as f:json.dump(dict(completed=True,summary=report['summary'],input_file_pins=pins,source_pins=report['source_pins'],artifacts={n:digest((args.out/n).read_bytes()) for n in outputs},semantic_retirement_or_runtime_gain_proven=False),f,indent=2)
    print(json.dumps(report['summary']))
if __name__=='__main__':main()
