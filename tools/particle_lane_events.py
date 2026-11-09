"""Validate bounded ordered reference observations; never infer native ownership."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path

FIRST_FIVE={0x8044A25C,0x8044A620,0x8044A9E4,0x8044ADAC,0x8044B1B8}

def u64(value,label):
    if type(value) is not int or not 0<=value<=0xffffffffffffffff:
        raise ValueError(f'{label} must be a nonnegative u64, not bool')
    return value

def address(value):
    if not isinstance(value,str):raise ValueError('address must preserve raw hex')
    result=int(value,16)
    if not 0<=result<=0xffffffff:raise ValueError('address outside raw u32')
    return result

def analyze(document):
    probe=document.get('r5probes',document)
    trace=probe['ordered_numeric_events']
    if type(trace['schema']) is not int or trace['schema']!=1 or trace['entry_observations_only'] is not True:
        raise ValueError('unsupported observation scope')
    capacity,stored,hits,dropped=(trace[k] for k in ('capacity','stored','hits','dropped'))
    for label,value in zip(('capacity','stored','hits','dropped'),(capacity,stored,hits,dropped)):u64(value,label)
    if not stored<=capacity<=65536 or stored!=len(trace['events']):raise ValueError('invalid capacity/stored count')
    if type(trace['retained_bytes']) is not int or trace['retained_bytes']!=capacity*176:
        raise ValueError('retained bytes differ from frozen Windows x64 event schema')
    if type(trace['counter_overflow']) is not bool or type(trace['complete_for_observed_entries']) is not bool:
        raise ValueError('invalid completeness flags')
    if capacity==0 and (hits or stored or dropped or trace['counter_overflow']):raise ValueError('disabled log has activity')
    if not trace['counter_overflow'] and hits!=stored+dropped:raise ValueError('counter totals disagree')
    complete=hits!=0 and dropped==0 and not trace['counter_overflow']
    if trace['complete_for_observed_entries']!=complete:raise ValueError('false completeness')
    root_registers=trace['root_registers']
    if root_registers!=[1,3,4,5,6,7,13,27,28,29,30,31]:raise ValueError('unknown root register schema')
    if trace['lane_order']!=['f18.0','f18.1','f19.0','f19.1']:raise ValueError('unknown lane schema')
    sites=Counter();first_five=[];transitions=[];previous=None;coordinate_regressions=[]
    lane_populations={};context_populations={}
    for i,event in enumerate(trace['events'],1):
        if type(event['sequence']) is not int or event['sequence']!=i:raise ValueError('prefix sequence missing or reordered')
        site=address(event['site']);address(event['lr']);sites[site]+=1
        mask=event['context_fields_observed']
        if type(mask) is not int or not 0<=mask<=7:raise ValueError('invalid context observation mask')
        context=tuple(address(event[k]) if mask&(1<<n) else None
            for n,k in enumerate(('current_context','fp_owner','thread')))
        raw=event['f18_f19_raw_u64']
        if len(raw)!=4 or any(not isinstance(v,str) or len(v)!=18 or not v.startswith('0x') for v in raw):
            raise ValueError('lanes must be exact hex64 strings')
        lanes=tuple(int(v,16) for v in raw)
        if any(not 0<=v<=0xffffffffffffffff for v in lanes):raise ValueError('lane overflow')
        if len(event['roots'])!=len(root_registers) or len(event['gqr'])!=8:raise ValueError('missing raw roots/formats')
        roots={f'r{reg}':value for reg,value in zip(root_registers,event['roots'])}
        for value in (*event['roots'],*event['gqr'],event['msr'],event['fpscr']):address(value)
        if type(event['instructions']) is not int or event['instructions']<0:raise ValueError('invalid cumulative coordinate')
        u64(event['instructions'],'instructions');u64(event['vi'],'vi');u64(event['guest_frame'],'guest_frame')
        if event['vi']>0xffffffff:raise ValueError('vi exceeds frozen u32 frame counter')
        lane_populations.setdefault(event['site'],Counter())[tuple(raw)]+=1
        context_populations.setdefault(event['site'],Counter())[context]+=1
        if site in FIRST_FIVE:
            first_five.append(dict(sequence=i,site=event['site'],context=context,raw_lanes=raw,roots=roots,
                value_origin_proven=False,source_tags_dereferenced=False))
        if previous is not None:
            old,old_context,old_lanes=previous
            if event['instructions']<old['instructions']:coordinate_regressions.append(i)
            changed=[trace['lane_order'][n] for n in range(4) if lanes[n]!=old_lanes[n]]
            if context!=old_context or changed:
                transitions.append(dict(previous_sequence=old['sequence'],sequence=i,
                    previous_site=old['site'],site=event['site'],context_changed=context!=old_context,
                    changed_lanes=changed,unobserved_intervening_work=True,causal_owner_proven=False))
        previous=(event,context,lanes)
    installed=[dict(address=p['address'],installed=p['installed'],hits=p['hits']) for p in probe.get('probes',[])
        if p.get('numeric_entry_evidence') is not None]
    configured=probe.get('config',{}).get('numeric_entries')
    installation_verified=False
    if configured is not None:
        configured_addresses=[address(p) for p in configured]
        installed_addresses=[address(p['address']) for p in installed]
        if len(set(configured_addresses))!=len(configured_addresses) or len(set(installed_addresses))!=len(installed_addresses):
            raise ValueError('duplicate configured/probe numeric sites')
        if set(installed_addresses)!=set(configured_addresses):
            raise ValueError('configured numeric entries differ from entry evidence')
        for p in installed:
            u64(p['hits'],'probe hits')
            if type(p['installed']) is not bool:raise ValueError('installed must be boolean')
        installation_verified=all(p['installed'] is True for p in installed)
        if capacity and sum(p['hits'] for p in installed)!=hits:raise ValueError('all-hit population differs from ordered hit count')
        for p in probe.get('probes',[]):
            evidence=p.get('numeric_entry_evidence')
            if evidence is None:continue
            u64(evidence['hits'],'numeric evidence hits');u64(evidence['unclassified'],'numeric unclassified')
            for row in evidence['states']:u64(row['hits'],'numeric state hits')
            if evidence['hits']!=p['hits'] or sum(row['hits'] for row in evidence['states'])+evidence['unclassified']!=p['hits']:
                raise ValueError('numeric histogram totals disagree')
        populations={address(p['address']):p['hits'] for p in installed}
        if any(site not in populations for site in sites):raise ValueError('stored event site not configured')
        if any(count>populations[site] for site,count in sites.items()):
            raise ValueError('stored site exceeds full observed population')
        if not installation_verified and (trace.get('complete_for_configured_entries') is True or
                trace.get('whole_route_coverage_proven') is True):
            raise ValueError('uninstalled entries contradict declared full completeness')
    return dict(schema='ordered_particle_entry_evidence_summary_v1',stored=stored,hits=hits,dropped=dropped,
        complete_for_observed_entries=complete,retained_bytes=trace['retained_bytes'],
        site_counts={f'0x{k:08x}':v for k,v in sorted(sites.items())},
        configured_entry_status=installed,first_five_observed_entries=first_five,
        configured_numeric_installation_verified=installation_verified,
        lane_populations={site:[dict(raw_lanes=list(key),count=count) for key,count in population.items()]
            for site,population in lane_populations.items()},
        context_populations={site:[dict(context=list(key),count=count) for key,count in population.items()]
            for site,population in context_populations.items()},
        adjacent_observation_transitions=transitions,instruction_coordinate_regressions=coordinate_regressions,
        scope='Exact captured prefix ordering and raw values only; instruction coordinates can be batched. Unobserved work remains between entries',
        producer_output_observed=False,source_span_or_origin_proven=False,whole_route_coverage_proven=False,
        asynchronous_observer_closure_proven=False,native_retirement_proven=False)

def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--measure',type=Path,required=True);parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args(argv)
    try:
        if args.measure.resolve()==args.out.resolve():raise ValueError('output may not overwrite input')
        if args.measure.stat().st_size>128*1024*1024:raise ValueError('input exceeds diagnostic bound')
        raw=args.measure.read_bytes();result=analyze(json.loads(raw))
        result['input']=dict(path=str(args.measure),sha256=hashlib.sha256(raw).hexdigest())
        with args.out.open('x',encoding='utf-8') as f:json.dump(result,f,indent=2);f.write('\n')
    except (OSError,ValueError,KeyError,TypeError) as error:
        print(f'ordered observation refused: {error}');return 1
    print(json.dumps({k:result[k] for k in ('stored','hits','dropped','complete_for_observed_entries','site_counts')}))
    return 0

if __name__=='__main__':raise SystemExit(main())
