"""Original direct-flow procedure candidate. No ABI/alias/indirect closure claim."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import struct
import sys

if __package__:
    from .graphics_lift_inventory import Dol, Extents, decode, hx, table
    from .platform_effect_inventory import platform_effect, implicit_consumers
else:
    from graphics_lift_inventory import Dol, Extents, decode, hx, table
    from platform_effect_inventory import platform_effect, implicit_consumers


def build_contract(raw, entry, functions_raw=None):
    image = Dol(raw)
    if entry % 4 or not image.contains_text(entry, 4):
        raise ValueError("entry must be an aligned original text word")
    declarations = table(functions_raw, "functions", image) if functions_raw is not None else []
    extents = Extents(declarations)
    pending, seen = [entry], set()
    calls, branches, returns, unresolved, outside, words = [], [], [], [], [], []
    consumers, effects = Counter(), Counter()
    while pending:
        pc = pending.pop()
        if pc in seen:
            continue
        seen.add(pc)
        word = image.word(pc)
        flow = decode(word, pc)
        implicit = implicit_consumers(word)
        effect = platform_effect(word)
        for consumer in implicit:
            consumers[consumer["consumer"] + ":" + consumer["identity"]] += 1
        if effect:
            effects[effect["identity"] + ":" + effect["access"]] += 1
        words.append(dict(site=hx(pc), word=hx(word), file_offset=image.offset(pc),
                          declaration_annotations=list(extents.at(pc)),
                          platform_effect=effect, implicit_consumers=implicit))
        successors = []
        edge = dict(site=hx(pc), word=hx(word), **flow) if flow else None
        if flow and flow.get("return_like"):
            returns.append(edge)
        elif flow and flow.get("link"):
            # Callee effects remain unknown. Never inline/cross a call because
            # a CSV row ended there, and never infer a callee-saved format ABI.
            calls.append(edge)
        elif flow:
            if flow["target"] is None:
                unresolved.append(edge)
            else:
                branches.append(edge)
                successors.append(flow["target"])
        if flow is None or flow["may_fall_through"]:
            successors.append((pc + 4) & 0xffffffff)
        for target in successors:
            if image.contains_text(target, 4):
                if target not in seen:
                    pending.append(target)
            else:
                outside.append(dict(site=hx(pc), target=hx(target)))
    return dict(schema="platform_procedure_candidate_v1", entry=hx(entry),
        inputs={name: dict(sha256=hashlib.sha256(data).hexdigest(), size_bytes=len(data))
                for name, data in (("dol", raw), ("functions", functions_raw)) if data is not None},
        scope=dict(conditional_paths="Both direct arms; no runtime conditions evaluated.",
                   calls="Call target effects excluded, possible return fallthrough included; ABI/returnability unproved.",
                   branches="Direct branches followed even across CSV extents; tails/shared ownership unproved.",
                   returns="Only canonical blr marked return-like; not return ownership proof.",
                   bound="Visited words bounded by original file-backed text; cycles visited once.",
                   declarations="Containment annotations only, including overlaps.",
                   indirect_closure_proven=False, memory_alias_closure_proven=False,
                   procedure_identity_proven=False, executed_proven=False,
                   native_replacement_proven=False, deletion_authorized=False),
        summary=dict(words=len(words), calls=len(calls), indirect_calls=sum(e["target"] is None for e in calls),
                     return_like_sites=len(returns), unresolved_terminal_sites=len(unresolved),
                     outside_text_edges=len(outside), platform_effects=dict(sorted(effects.items())),
                     implicit_consumers=dict(sorted(consumers.items()))),
        words=sorted(words, key=lambda r: r["site"]), calls=sorted(calls, key=lambda r: r["site"]),
        branches=sorted(branches, key=lambda r: r["site"]), return_like=returns,
        unresolved_terminals=unresolved, outside_text_edges=outside,
        declaration_annotations=declarations, declaration_extent_overlaps=extents.overlaps())


def build_continuation_contract(raw, call_site, functions_raw=None):
    """Direct continuation after a possible call return, never ABI liveness.

    Analyze one selected call, not every call suffix in the entire image. The
    existing file-backed-word traversal bound applies and each word is visited
    once. Callees, enclosing callers, aliases and asynchronous observers remain
    explicit gaps even when this suffix has no known floating-point encoding.
    """
    image = Dol(raw)
    if call_site % 4 or not image.contains_text(call_site, 4):
        raise ValueError("call site must be an aligned original text word")
    word = image.word(call_site)
    flow = decode(word, call_site)
    if not flow or not flow.get("link") or not flow.get("may_fall_through"):
        raise ValueError("selected site is not a branch with link and possible return")
    report = build_contract(raw, call_site + 4, functions_raw)
    report["schema"] = "platform_call_continuation_candidate_v1"
    report["source_call"] = dict(site=hx(call_site), word=hx(word), **flow)
    numeric = [row["site"] for row in report["words"]
               if any(item["consumer"] == "fp_available_guard" for item in row["implicit_consumers"])
               or (row["platform_effect"] or {}).get("domain") == "numeric_context"]
    report["known_numeric_encoding_sites"] = numeric
    report["summary"]["known_numeric_encoding_sites"] = len(numeric)
    report["scope"].update(
        entry="Selected call's next word; not a recovered function entry.",
        returnability="Possible return only; conditional call execution and callee returnability unproved.",
        numeric_encodings="Known direct FP/FPSCR encodings only; no lane use/def or numeric liveness proof.",
        omitted_observers="Callee effects, enclosing callers, memory aliases, contexts, exceptions and events.",
        numeric_liveness_proven=False, native_context_retirement_proven=False)
    return report


def adjacent_address_value(first,second):
    """Value of consecutive lis→addi/ori encodings; no execution/pointer proof."""
    if first>>26!=15 or (first>>16)&31:return None
    seed=(first>>21)&31
    high=(first&65535)<<16
    op=second>>26;d=(second>>21)&31;a=(second>>16)&31;low=second&65535
    if op==14 and a==seed and a!=0:
        return dict(pattern='lis_addi',source_gpr=seed,result_gpr=d,
                    value=(high+(low if low<32768 else low-65536))&0xffffffff)
    if op==24 and d==seed:
        return dict(pattern='lis_ori',source_gpr=seed,result_gpr=a,value=high|low)
    return None


def build_incoming_contract(raw, target, functions_raw=None):
    """All original direct branch encodings and aligned target literals.

    This is a reference index, not caller closure: code may be unreachable,
    literals may be ordinary data. Adjacent address construction is a value
    candidate only, not an actual pointer/caller. Other constructions, aliases,
    relocated code and indirect calls remain unresolved. No CSV filters apply.
    """
    return build_incoming_contracts(raw,(target,),functions_raw)[0]


def build_incoming_contracts(raw, targets, functions_raw=None):
    """One canonical original-image pass for unique source target coordinates.

    Each result has exactly the single-target schema and scope. Storage is
    bounded by unique file-backed text targets and their actual source hits;
    nothing is inferred from a declaration, literal or zero-match result.
    """
    image=Dol(raw);targets=tuple(targets)
    if (not targets or any(type(t) is not int or t%4 or not image.contains_text(t,4) for t in targets)
        or len(set(targets))!=len(targets)):
        raise ValueError('unique nonempty aligned original text targets required')
    hits={target:([],[],[]) for target in targets}
    for section in image.sections:
        for offset in range(0,section["size"]-3,4):
            pc=section["address"]+offset
            word=struct.unpack_from(">I",raw,section["file_offset"]+offset)[0]
            if word in hits:
                hits[word][1].append(dict(site=hx(pc),file_offset=section["file_offset"]+offset,
                                     section_index=section["index"],section_kind=section["kind"],
                                     pointer_identity_proven=False))
            if section["kind"]=="text":
                flow=decode(word,pc)
                if flow and flow.get("target") in hits:
                    hits[flow['target']][0].append(dict(site=hx(pc),word=hx(word),file_offset=section["file_offset"]+offset,**flow))
                if offset+8<=section['size'] and word>>26==15:
                    second=struct.unpack_from('>I',raw,section['file_offset']+offset+4)[0]
                    candidate=adjacent_address_value(word,second)
                    if candidate and candidate['value'] in hits:
                        hits[candidate['value']][2].append(dict(site=hx(pc),word=hx(word),second_site=hx(pc+4),second_word=hx(second),
                            file_offset=section['file_offset']+offset,section_index=section['index'],**candidate,
                            execution_requirement='Both original words execute consecutively, with no intervening entry/context.',
                            pointer_identity_or_execution_proven=False))
    inputs={name:dict(sha256=hashlib.sha256(data).hexdigest(),size_bytes=len(data))
            for name,data in (("dol",raw),("functions",functions_raw)) if data is not None}
    reports=[]
    for target in targets:
        branches,literals,constructions=hits[target]
        reports.append(dict(schema="platform_incoming_reference_candidate_v2",target=hx(target),
        inputs={name:dict(values) for name,values in inputs.items()},
        scope=dict(executed_proven=False,caller_closure_proven=False,memory_alias_closure_proven=False,
                   indirect_closure_proven=False,native_replacement_proven=False,deletion_authorized=False,
                   literals="Aligned equality is not pointer identity; relocation writes omitted.",
                   constructions="Consecutive lis/addi or lis/ori encodings only; nonadjacent/data/relocated/context cases unresolved.",
                   branches="Original text encodings, including non-call tails; not a reachability proof."),
        summary=dict(direct_branch_encodings=len(branches),direct_call_encodings=sum(bool(e["link"]) for e in branches),
                     aligned_target_literals=len(literals),adjacent_address_constructions=len(constructions)),
        direct_branches=branches,literals=literals,adjacent_address_constructions=constructions))
    return reports


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dol", type=Path, required=True)
    selection = parser.add_mutually_exclusive_group(required=True)
    selection.add_argument("--entry", type=lambda v: int(v, 0))
    selection.add_argument("--after-call", type=lambda v: int(v, 0),
                           help="Analyze one possible return continuation, never certify register/alias liveness")
    selection.add_argument("--incoming",type=lambda v:int(v,0),
                           help="Index original direct branches/literals; never certify caller or alias closure")
    parser.add_argument("--functions", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        if args.out.resolve() in {p.resolve() for p in (args.dol, args.functions) if p}:
            raise ValueError("output may not overwrite input")
        if args.dol.stat().st_size > 64 * 1024 * 1024:
            raise ValueError("DOL exceeds supported original-image bound")
        builder = build_contract if args.entry is not None else build_continuation_contract if args.after_call is not None else build_incoming_contract
        selected=args.entry if args.entry is not None else args.after_call if args.after_call is not None else args.incoming
        contract = builder(args.dol.read_bytes(), selected,
                           args.functions.read_bytes() if args.functions else None)
        with args.out.open("x", encoding="utf-8") as stream:
            json.dump(contract, stream, indent=2)
            stream.write("\n")
    except (OSError, ValueError) as error:
        print(f"procedure candidate refused: {error}", file=sys.stderr)
        return 1
    print(json.dumps(contract["summary"], sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
