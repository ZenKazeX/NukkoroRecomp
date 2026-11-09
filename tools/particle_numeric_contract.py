"""Lane-level original-word evidence; never a native retirement certificate."""
import argparse
from collections import deque
import json
from pathlib import Path
import sys

if __package__:
    from .platform_procedure_contract import build_contract
    from .graphics_lift_inventory import decode, hx
else:
    from platform_procedure_contract import build_contract
    from graphics_lift_inventory import decode, hx


def lane(reg, half):
    return reg * 2 + half


def effect_source_lanes(effect):
    """All decoded source lanes, including arithmetic result dependencies.

    ``reads`` contains explicit observers such as stores and comparisons;
    arithmetic operands live in ``defs``. A read census must include both.
    None denotes an unknown effect, not an empty set of sources.
    """
    if effect is None:
        return None
    return sorted(set(effect['reads']).union(
        source for sources in effect['defs'].values() if sources is not None
        for source in sources))


def numeric_effect(word):
    """Explicit lane reads per result, with simultaneous destination writes.

    Deliberately partial. Unsupported numeric encodings are unknown, never
    identity/no-op. This decodes effects, not floating-point mathematical values.
    Memory, condition/status and asynchronous aliases remain separate obligations.
    """
    op, d, a, b, c = word >> 26, (word >> 21) & 31, (word >> 16) & 31, (word >> 11) & 31, (word >> 6) & 31
    xo, short = (word >> 1) & 1023, (word >> 1) & 31
    defs, reads, memory, status = {}, [], None, None
    if op in (56, 57, 60, 61):
        one, store = bool(word & 0x8000), op in (60, 61)
        if store:
            reads = [lane(d, i) for i in range(1 if one else 2)]
        else:
            defs = {lane(d, 0): None, lane(d, 1): [] if one else None}
        disp = word & 4095
        memory = dict(access="write" if store else "read", base_gpr=a,
                      displacement=disp if disp < 2048 else disp - 4096,
                      format_index=(word >> 12) & 7, elements=1 if one else 2)
    elif op in (48, 49, 50, 51, 52, 53, 54, 55):
        store = op >= 52
        if store:
            reads = [lane(d, 0)]
        else:
            defs = {lane(d, 0): None}
            if op in (48, 49):
                defs[lane(d, 1)] = None
        disp = word & 65535
        memory = dict(access="write" if store else "read", base_gpr=a,
                      displacement=disp if disp < 32768 else disp - 65536,
                      scalar_width=4 if op in (48, 49, 52, 53) else 8)
    elif op == 4:
        indexed_xo = (word >> 1) & 63
        if indexed_xo in (6, 7, 38, 39):
            # W/I occupy bits that overlap the ten-bit arithmetic XO. Decode
            # these before paired arithmetic, using their actual six-bit XO.
            one, store = bool(word & 0x400), indexed_xo in (7, 39)
            if store:
                reads = [lane(d, i) for i in range(1 if one else 2)]
            else:
                defs = {lane(d, 0): None, lane(d, 1): [] if one else None}
            memory = dict(access="write" if store else "read", base_gpr=a,
                          index_gpr=b, indexed=True,
                          format_index=(word >> 7) & 7, elements=1 if one else 2,
                          update_form=indexed_xo in (38, 39),
                          updates_base=indexed_xo in (38, 39) and a!=0)
        elif short in (12, 13, 14, 15):
            half = 0 if short in (12, 14) else 1
            for i in range(2):
                defs[lane(d, i)] = [lane(a, i), lane(c, half)] + ([lane(b, i)] if short in (14, 15) else [])
        elif short in (24, 26):
            # Paired reciprocal estimates consume each B lane independently.
            # Only dependencies are recognized; approximation, FPSCR and
            # extension/availability exceptions remain unproved.
            defs = {lane(d, i): [lane(b, i)] for i in range(2)}
        elif short in (18, 20, 21, 23, 25, 28, 29, 30, 31):
            for i in range(2):
                regs = (a, b) if short in (18, 20, 21) else (a, c) if short == 25 else (a, b, c)
                defs[lane(d, i)] = [lane(r, i) for r in regs]
        elif short in (10, 11):
            defs[lane(d, 0)] = [lane(a, 0), lane(b, 1)] if short == 10 else [lane(c, 0)]
            defs[lane(d, 1)] = [lane(c, 1)] if short == 10 else [lane(a, 0), lane(b, 1)]
        elif xo in (40, 72, 136, 264):
            defs = {lane(d, i): [lane(b, i)] for i in range(2)}
        elif xo in (528, 560, 592, 624):
            defs = {lane(d, 0): [lane(a, int(xo in (592, 624)))],
                    lane(d, 1): [lane(b, int(xo in (560, 624)))]}
        elif xo in (0, 32, 64, 96):
            reads = [lane(a, int(xo >= 64)), lane(b, int(xo >= 64))]
        else:
            return None
    elif op in (59, 63):
        # Lane effects only. Rounding, exceptional results and FPSCR are not
        # evaluated; scalar-single arithmetic duplicates its result into ps1,
        # whereas group63 scalar instructions write ps0 alone.
        arithmetic = (18, 20, 21, 22, 24, 25, 28, 29, 30, 31) if op == 59 else (18, 20, 21, 22, 23, 25, 26, 28, 29, 30, 31)
        if short in arithmetic:
            regs = (a, b) if short in (18, 20, 21) else (b,) if short in (22, 24, 26) else (a, c) if short == 25 else (a, b, c)
            defs = {lane(d, i): [lane(r, 0) for r in regs] for i in range(2 if op == 59 else 1)}
        elif op == 63 and xo in (12, 14, 15, 40, 72, 136, 264):
            defs = {lane(d, 0): [lane(b, 0)]}
        elif op == 63 and xo in (0, 32):
            reads = [lane(a, 0), lane(b, 0)]
        elif op == 63 and xo == 583:
            defs = {lane(d, 0): None}
            status = dict(access="read", identity="FPSCR", definition_lane=lane(d, 0))
        elif op == 63 and xo == 711:
            reads = [lane(b, 0)]
            status = dict(access="write", identity="FPSCR")
        elif op == 63 and xo in (38, 70, 134):
            status = dict(access="write", identity="FPSCR")
        elif op == 63 and xo == 64:
            status = dict(access="read", identity="FPSCR")
        else:
            return None
    elif op == 31 and xo in (535, 567, 599, 631, 663, 695, 727, 759, 983):
        store=xo in (663,695,727,759,983)
        if store:
            reads=[lane(d,0)]
        else:
            defs={lane(d,0):None}
            if xo in (535,567):defs[lane(d,1)]=None
        memory=dict(access="write" if store else "read",base_gpr=a,index_gpr=b,
                    indexed=True,scalar_width=4 if xo in (535,567,663,695,983) else 8,
                    low_word_only=xo==983,
                    updates_base=xo in (567,631,695,759))
    return dict(defs=defs, reads=reads, memory=memory, status=status)


def predicate_transfer(word, state):
    """Small constant lattice for original channel decisions, not execution.

    Unknown instructions/calls kill facts. Loops retain both CTR arms; memory
    reads are always unknown. No imported memory or processor state is supplied.
    """
    regs, cr = list(state[:32]), list(state[32:])
    op, d, a, b, xo = word >> 26, (word >> 21) & 31, (word >> 16) & 31, (word >> 11) & 31, (word >> 1) & 1023
    imm=word&65535
    if op in (10,11):
        value=None if word&0x200000 else regs[a]
        compare=imm
        if op==11:
            compare=imm if imm<32768 else imm-65536
            if value is not None and value&0x80000000: value-=1<<32
        field=(word>>23)&7
        cr[field*4:field*4+4]=[None,None,None,None] if value is None else [value<compare,value>compare,value==compare,None]
    elif op==21:
        value=regs[d]
        shift,mb,me=(word>>11)&31,(word>>6)&31,(word>>1)&31
        bits=range(mb,me+1) if mb<=me else (*range(mb,32),*range(me+1))
        mask=sum(1<<(31-i) for i in bits)
        value=None if value is None else (((value<<shift)|(value>>(32-shift)))&mask)
        regs[a]=value
        if word&1: cr[:4]=[None,None,None,None] if value is None else [bool(value&0x80000000),value!=0 and not value&0x80000000,value==0,None]
    elif op==14:
        value=0 if a==0 else regs[a]
        regs[d]=None if value is None else (value+(imm if imm<32768 else imm-65536))&0xffffffff
    elif op in (24,25):
        regs[a]=None if regs[d] is None else regs[d]|(imm<<(16 if op==25 else 0))
    elif op in (32,33,34,35,40,41,42,43):
        regs[d]=None
        if op&1: regs[a]=None
    elif op in (36,37,38,39,44,45):
        if op&1: regs[a]=None
    elif op==31 and xo==19:
        regs[d]=None
    elif op==31 and xo==266:
        regs[d]=None if regs[a] is None or regs[b] is None else (regs[a]+regs[b])&0xffffffff
        if word&1: cr[:4]=[None]*4
    elif op==31 and xo==144:
        fm=(word>>12)&255
        for field in range(8):
            if fm&(0x80>>field):
                cr[field*4:field*4+4]=[None]*4 if regs[d] is None else [bool(regs[d]&(1<<(31-field*4-i))) for i in range(4)]
    elif op in (4,48,49,50,51,52,53,54,55,56,57,60,61):
        if op in (49,51,53,55,57,61): regs[a]=None
        if op==4 and ((word>>1)&63) in (38,39) and a!=0: regs[a]=None
        if op==4 and word&1: cr[4:8]=[None]*4
        if op==4 and xo in (0,32,64,96):
            field=(word>>23)&7
            cr[field*4:field*4+4]=[None]*4
    elif op in (16,18) or op==19 and xo in (16,528,50) or op==31 and xo in (467,278):
        pass
    else:
        regs,cr=[None]*32,[None]*32
    return tuple(regs+cr)


def stack_roundtrip_candidates(words, effects, procedure):
    """Correlate original stfd/lfd paths, without inventing memory alias closure.

    This additional evidence never rewrites the main lane dependencies. Even a
    matched save/restore can be observed or overwritten through a pointer, call,
    context switch or exception. Its exact raw-primary serialization is separate
    from paired-single saves, which may convert rather than preserve bits.
    """
    successors, predecessors = {}, {pc: set() for pc in words}
    for pc, word in words.items():
        flow = decode(word, pc)
        targets=[]
        if flow and not flow.get("return_like") and not flow.get("link") and flow.get("target") is not None:
            targets.append(flow["target"])
        if flow is None or flow.get("may_fall_through"):
            targets.append(pc+4)
        successors[pc]={target for target in targets if target in words}
        for target in successors[pc]:
            predecessors[target].add(pc)

    def reachable(initial, adjacency, stop=None):
        seen, pending = set(), list(initial)
        while pending:
            pc=pending.pop()
            if pc in seen:
                continue
            seen.add(pc)
            if pc != stop:
                pending.extend(adjacency[pc]-seen)
        return seen

    calls={int(row["site"],16) for row in procedure["calls"]}
    candidates=[]
    saves=[pc for pc, word in words.items() if word>>26==54 and (word>>16)&31==1]
    restores=[pc for pc, word in words.items() if word>>26==50 and (word>>16)&31==1]
    for restore in restores:
        restore_word=words[restore]
        offset=effects[restore]["memory"]["displacement"]
        register=(restore_word>>21)&31
        ancestors=reachable([restore],predecessors)
        for save in saves:
            if save not in ancestors or (words[save]>>21)&31!=register or effects[save]["memory"]["displacement"]!=offset:
                continue
            descendants=reachable(successors[save],successors,stop=restore)
            paths=(descendants&ancestors)-{save,restore}
            overlap, aliases, stack_changes, undecoded = [], [], [], []
            for pc in sorted(paths):
                word=words[pc]; op=word>>26
                d,a,b=(word>>21)&31,(word>>16)&31,(word>>11)&31
                effect=effects[pc]
                if pc in calls:
                    aliases.append(pc)
                if effect is None:
                    undecoded.append(pc)
                if op in (14,15) and d==1 or op==37 and a==1 or op in (49,51,53,55,57,61) and a==1:
                    stack_changes.append(pc)
                elif op in (32,33,34,35,40,41,42,43) and (d==1 or op&1 and a==1):
                    stack_changes.append(pc)
                elif op in (20,21,23,24,25,26,27,28,29) and a==1 or op==46 and d<=1:
                    stack_changes.append(pc)
                elif op==31 and 1 in (d,a,b):
                    # Intentionally conservative for indexed/unknown GPR effects.
                    stack_changes.append(pc)
                mem=effect["memory"] if effect is not None else None
                if mem and mem.get("updates_base") and mem["base_gpr"]==1:
                    stack_changes.append(pc)
                if mem and mem["access"]=="write":
                    if mem["base_gpr"]!=1 or mem.get("indexed"):
                        aliases.append(pc)
                    elif "scalar_width" in mem:
                        start=mem["displacement"]
                        if start<offset+8 and offset<start+mem["scalar_width"]:
                            overlap.append(pc)
                    else:
                        # Variable format width cannot be guessed from index.
                        aliases.append(pc)
                elif op in (36,37,38,39,44,45,47):
                    width={36:4,37:4,38:1,39:1,44:2,45:2}.get(op)
                    disp=word&65535; disp=disp if disp<32768 else disp-65536
                    if a!=1 or width is None:
                        aliases.append(pc)
                    elif disp<offset+8 and offset<disp+width:
                        overlap.append(pc)
                elif op==31:
                    # No complete indexed integer/cache-write decoder here.
                    # Retain these possible aliases instead of assuming purity.
                    aliases.append(pc)
            candidates.append(dict(save_site=hx(save),restore_site=hx(restore),
                register=f"f{register}",primary_lane=f"f{register}.0",
                stack_displacement=offset,width_bytes=8,
                raw_primary_serialization="stfd/lfd; neither instruction restores the secondary lane",
                intervening_words=len(paths), known_overlapping_stack_write_sites=[hx(pc) for pc in overlap],
                possible_alias_or_call_sites=[hx(pc) for pc in sorted(set(aliases))],
                possible_stack_base_change_sites=[hx(pc) for pc in stack_changes],
                unsupported_numeric_sites=[hx(pc) for pc in undecoded],
                memory_alias_closure_proven=False,asynchronous_observer_closure_proven=False,
                preservation_proven=False))
    return candidates


def build_numeric_contract(raw, entry, *, argument_r4=None, argument_count=None,
                           direct_call_depth=0, _active=(), _cache=None):
    if type(direct_call_depth) is not int or not 0 <= direct_call_depth <= 3:
        raise ValueError("original direct-call depth must be 0..3")
    if _cache is None:
        _cache = {}
    for value in (argument_r4,argument_count):
        if value is not None and (type(value) is not int or not 0<=value<=0xffffffff):
            raise ValueError("parameter model must be a raw unsigned 32-bit argument")
    procedure = build_contract(raw, entry)
    words = {int(r["site"], 16): int(r["word"], 16) for r in procedure["words"]}
    effects = {pc: numeric_effect(word) for pc, word in words.items()}
    initial = tuple(frozenset({f"entry.f{r}.{i}"}) for r in range(32) for i in range(2))
    states, pending, queued = {entry: initial}, deque([entry]), {entry}
    predicates=[None]*64
    predicates[4],predicates[3]=argument_r4,argument_count
    predicate_states={entry:tuple(predicates)}
    calls = {int(e["site"], 16): e for e in procedure["calls"]}
    resolved_calls = {}
    if direct_call_depth:
        for site, call in calls.items():
            target = call.get("target")
            if target is None or target in (*_active, entry):
                continue
            key = (target, direct_call_depth - 1, (*_active, entry))
            if key not in _cache:
                try:
                    _cache[key] = build_numeric_contract(raw, target,
                        direct_call_depth=direct_call_depth - 1,
                        _active=(*_active, entry), _cache=_cache)
                except ValueError:
                    _cache[key] = None
            candidate = _cache[key]
            if candidate is None:
                continue
            body = candidate["procedure"]
            if candidate["unknown_sites"] or not candidate["return_lane_dependencies"] or body["unresolved_terminals"] or body["outside_text_edges"]:
                continue
            resolved_calls[site] = candidate
    unknown, stores, observations, return_states, return_predicates = set(), {}, {}, {}, {}
    iterations = 0
    while pending:
        pc = pending.popleft(); queued.remove(pc); iterations += 1
        state = states[pc]; effect = effects[pc]; result = list(state)
        predicates=predicate_transfer(words[pc],predicate_states[pc])
        if pc in resolved_calls:
            callee = resolved_calls[pc]
            def substitute(roots):
                translated = frozenset()
                for root in roots:
                    if root.startswith("entry.f"):
                        reg, half = root[7:].split(".")
                        translated |= state[lane(int(reg), int(half))]
                    else:
                        translated |= {root}
                return translated
            for i in range(64):
                result[i] = substitute(root for row in callee["return_lane_dependencies"]
                                       for root in row["lanes"][f"f{i//2}.{i%2}"])
                # Conditional linked branches can decline the call. Keeping
                # that arm is conservative even if an argument model knows it.
                if calls[pc].get("conditional"):
                    result[i] |= state[i]
            observed = substitute(root for row in callee["lane_observations"] for root in row["dependencies"])
            observations[pc] = observations.get(pc, frozenset()) | observed
            predicates=tuple([None]*64)
        elif effect is None or pc in calls:
            unknown.add(pc)
            result = [value | {f"unknown.{hx(pc)}"} for value in state]
            predicates=tuple([None]*64)
        else:
            for target, inputs in effect["defs"].items():
                # Memory roots distinguish both halves; W=1 defines literal 1.
                status=effect.get("status")
                result[target] = (frozenset({f"status.{status['identity']}.{hx(pc)}"})
                                  if status and status.get("definition_lane")==target else
                                  frozenset({f"memory.{hx(pc)}.{target % 2}"}) if inputs is None else
                                  frozenset().union(*(state[i] for i in inputs)))
            consumed = frozenset().union(*(state[i] for i in effect["reads"]))
            if effect["reads"]:
                observations[pc] = observations.get(pc, frozenset()) | consumed
            if effect["memory"] and effect["memory"]["access"] == "write":
                stores[pc] = stores.get(pc, frozenset()) | consumed
        flow = decode(words[pc], pc)
        successors = []
        taken=None
        if flow and flow.get("conditional"):
            bo,bi=flow["bo"],flow["bi"]
            condition=True if bo&0x10 else predicates[32+bi]
            if not bo&0x10 and condition is not None: condition=condition==bool(bo&8)
            if condition is False: taken=False
            elif condition is True and bo&4: taken=True
        if flow and flow.get("return_like"):
            return_states[pc] = tuple(result)
            return_predicates[pc] = predicates
        elif flow and not flow.get("link") and flow.get("target") is not None and taken is not False:
            successors.append(flow["target"])
        if flow is None or flow["may_fall_through"] and (flow.get("link") or taken is not True):
            successors.append((pc+4) & 0xffffffff)
        for target in successors:
            if target not in words:
                continue  # Outside/exception edges retained by procedure report.
            joined = tuple(result) if target not in states else tuple(x | y for x, y in zip(states[target], result))
            predicate_join=predicates if target not in predicate_states else tuple(x if x==y else None for x,y in zip(predicate_states[target],predicates))
            if states.get(target) != joined or predicate_states.get(target)!=predicate_join:
                states[target] = joined
                predicate_states[target]=predicate_join
                if target not in queued:
                    pending.append(target); queued.add(target)
    rows = []
    for pc in sorted(words):
        effect = effects[pc]
        if pc not in states or effect is None or not effect["memory"]:
            continue
        roots = sorted(stores.get(pc, ()))
        rows.append(dict(site=hx(pc), word=hx(words[pc]), **effect["memory"],
                         source_lanes=effect["reads"], dependencies=roots,
                         possible_incoming_fp=[r for r in roots if r.startswith("entry.")]))
    return dict(schema="particle_numeric_effect_candidate_v2", entry=hx(entry), inputs=procedure["inputs"],
        summary=dict(words=len(words), parameter_model_words=len(states), worklist_iterations=iterations, unknown_numeric_or_call_sites=len(unknown),
                     output_store_sites=sum(r["access"] == "write" and r["base_gpr"] != 1 for r in rows),
                     nonstack_incoming_fp_store_sites=sum(r["access"] == "write" and r["base_gpr"] != 1 and
                                                         bool(r["possible_incoming_fp"]) for r in rows)),
        scope=dict(known_local_lane_effects_decoded=not unknown, path_feasibility_proven=False,
                   parameter_model=dict(r4=hx(argument_r4) if argument_r4 is not None else None,
                                        count=hx(argument_count) if argument_count is not None else None),
                   branch_refinement="Known original integer argument decisions only; CTR loops keep both arms, all memory values unknown.",
                   numeric_values_evaluated=False, stack_alias_closure_proven=False,
                   external_memory_alias_closure_proven=False, asynchronous_context_closure_proven=False,
                   native_replacement_proven=False, deletion_authorized=False,
                   lattice="Finite entry/memory/unknown roots; union at joins, each original word visited until stable.",
                   status="FPSCR/CR effects not propagated; unresolved calls poison every lane; no caller-saved ABI assumption.",
                   dependency_identity="A dependency root is not a bit-preservation claim: arithmetic can transform an incoming value.",
                   direct_call_depth=direct_call_depth,
                   direct_call_summary="Only original bounded direct bodies with decoded lane effects and explicit returns; no parameter values, memory aliases, exceptions or returnability proof."),
        unknown_sites=[hx(pc) for pc in sorted(unknown)], memory_effects=rows,
        explicit_status_effects=[dict(site=hx(pc),**effect["status"],
            status_value_or_liveness_proven=False) for pc,effect in sorted(effects.items())
            if pc in states and effect is not None and effect.get("status")],
        stack_roundtrip_candidates=stack_roundtrip_candidates(words,effects,procedure),
        lane_observations=[dict(site=hx(pc), dependencies=sorted(roots)) for pc, roots in sorted(observations.items())],
        direct_callee_summaries=[dict(site=hx(pc), target=report["entry"],
            words=report["summary"]["words"], memory_effects=report["memory_effects"],
            explicit_status_effects=report["explicit_status_effects"],
            nested_direct_callee_summaries=report["direct_callee_summaries"],
            scope=report["scope"], return_lane_dependencies=report["return_lane_dependencies"])
            for pc, report in sorted(resolved_calls.items())],
        return_lane_dependencies=[dict(site=hx(pc), lanes={f"f{i//2}.{i%2}": sorted(value)
                                  for i, value in enumerate(state)}) for pc,state in sorted(return_states.items())],
        return_integer_models=[dict(site=hx(pc),known_gprs={f"r{i}":hx(value)
            for i,value in enumerate(state[:32]) if value is not None},
            scope="Only the explicit constant lattice; no memory values or supplied runtime state; not a value/ABI/liveness proof")
            for pc,state in sorted(return_predicates.items())],
        procedure=procedure)


def main(argv=None):
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--dol",type=Path,required=True)
    p.add_argument("--entry",type=lambda v:int(v,0),required=True)
    p.add_argument("--out",type=Path,required=True)
    p.add_argument("--r4",type=lambda v:int(v,0))
    p.add_argument("--count",type=lambda v:int(v,0))
    p.add_argument("--direct-call-depth",type=int,default=0,
                   help="Optional original direct-callee lane summaries, depth0..3; indirect/unsupported/cyclic effects remain unknown")
    args=p.parse_args(argv)
    try:
        if args.dol.resolve()==args.out.resolve(): raise ValueError("output may not overwrite DOL")
        if args.dol.stat().st_size>64*1024*1024: raise ValueError("DOL exceeds supported bound")
        report=build_numeric_contract(args.dol.read_bytes(),args.entry,argument_r4=args.r4,argument_count=args.count,
                                      direct_call_depth=args.direct_call_depth)
        with args.out.open("x",encoding="utf-8") as stream:
            json.dump(report,stream,indent=2); stream.write("\n")
    except (ValueError,OSError) as error:
        print(f"numeric contract refused: {error}",file=sys.stderr); return 1
    print(json.dumps(report["summary"],sort_keys=True)); return 0

if __name__=="__main__":
    raise SystemExit(main())
