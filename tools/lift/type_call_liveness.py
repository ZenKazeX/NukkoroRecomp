"""Trace possible uses of the cast's scratch state after original call sites.

This is a conservative encoded-flow audit, not a complete ABI or memory proof.
Direct callees are followed without assuming ABI argument lists or clobbers.
Unknown instructions, indirect calls, recursion and bounds remain explicit.
An LR return with a surviving value reports escape, never dead state.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from graphics_lift_inventory import Dol
from gx_site_inventory import Image as FlowImage, read_jump_tables
from ppcrecomp import wait_points

# Original normal-return cast body, pinned below: r1/r31/LR are restored;
# r3 is the result. r6/r7, FPRs and XER are not written on normal-return paths.
# The reference-failure/throw route is a separate contract.
SCRATCH = ('r0', 'r4', 'r5', 'r8', 'r9', 'r10', 'r11', 'r12', 'cr0', 'ctr')
CAST = 0x803A1AFC
CAST_END = 0x803A1D60
CAST_SHA256 = 'd1883bb7ab9577c5c68ae53b3e2be3c4c89e7adb64ed5d042ef017fa61ed6569'


class Tracer:
    def __init__(self, read, contains, jump_tables=None, max_sites=100000, max_depth=64):
        self.read = read
        self.contains = contains
        self.jump_tables = jump_tables or {}
        self.max_sites = max_sites
        self.max_depth = max_depth
        self.cache = {}
        self.active = set()

    def trace(self, entry, register, bounds=None):
        key = (entry, register, bounds)
        if key in self.active:
            return frozenset(), frozenset({('recursive_call', entry)}), False
        if len(self.active) >= self.max_depth:
            return frozenset(), frozenset({('call_depth_bound', entry)}), False
        if key in self.cache:
            return self.cache[key]
        self.active.add(key)
        reads, barriers, seen, pending = set(), set(), set(), [entry]
        escapes = False
        try:
            while pending:
                pc = pending.pop()
                if pc in seen:
                    continue
                if len(seen) >= self.max_sites:
                    barriers.add(('site_bound', pc))
                    continue
                seen.add(pc)
                if bounds is not None and not bounds[0] <= pc < bounds[1]:
                    barriers.add(('outside_caller_context', pc))
                    continue
                if pc & 3 or not self.contains(pc):
                    barriers.add(('outside_text', pc))
                    continue
                ins = self.read(pc)
                decoded = wait_points.decode(ins, pc)
                if decoded.kind in ('unknown', 'impure'):
                    barriers.add((decoded.kind + '_instruction', pc))
                    continue
                # Reads precede writes, including read/modify/write operations.
                if register in decoded.reads:
                    reads.add(pc)
                    continue
                if decoded.kind == 'call':
                    # The decoder's ABI call write set is deliberately NOT used.
                    if decoded.target is None:
                        barriers.add(('indirect_call', pc))
                    else:
                        child_reads, child_barriers, returns = self.trace(decoded.target, register)
                        reads.update(child_reads)
                        barriers.update(child_barriers)
                        if returns:
                            pending.append(pc + 4)
                    if decoded.conditional:
                        pending.append(pc + 4)
                    continue
                if register in decoded.writes:
                    continue
                if decoded.kind == 'exit':
                    xo = (ins >> 1) & 0x3ff
                    if xo == 16:  # encoded LR-return convention, not a lifetime proof
                        escapes = True
                    elif xo == 528 and pc in self.jump_tables and self.jump_tables[pc]:
                        pending.extend(self.jump_tables[pc])
                    else:
                        barriers.add(('indirect_branch', pc))
                    if decoded.conditional:
                        pending.append(pc + 4)
                elif decoded.kind == 'branch':
                    pending.append(decoded.target)
                    if decoded.conditional:
                        pending.append(pc + 4)
                else:
                    pending.append(pc + 4)
            result = frozenset(reads), frozenset(barriers), escapes
            self.cache[key] = result
            return result
        finally:
            self.active.remove(key)


def negative_byte_guard(read, entry, end, branch, source_register, observed_read):
    """Check an inductive first-edge witness; do not suppress the raw census.

    Before the first traversal of a signed-byte >= 0 edge, all paths to its
    predicate must establish a negative byte and have no other writes/calls.
    Removing that edge must disconnect the reported read from this entry.
    Scope is normal encoded direct flow, not arbitrary interior entry/transfers.
    """
    def refused(reason):
        return {'proven_for_direct_entry': False, 'reason': reason}

    if not entry <= branch-4 < branch < end or not entry <= observed_read < end:
        return refused('guard/read outside the explicit function extent')
    ins, compare = read(branch), read(branch-4)
    if (ins & 0xffff0003 != 0x40800000 or compare >> 26 != 31 or
            (compare >> 1) & 1023 != 954 or not compare & 1 or compare & 0xf800):
        return refused('requires CR0 bge immediately after recording extsb')
    if (compare >> 21) & 31 != source_register:
        return refused('different byte source')
    guard_target = wait_points.decode(ins, branch).target
    graph, decoded, outside = {}, {}, []
    for pc in range(entry, end, 4):
        effect = decoded[pc] = wait_points.decode(read(pc), pc)
        targets = []
        if effect.kind == 'branch':
            targets.append(effect.target)
            if effect.conditional:
                targets.append(pc+4)
        elif effect.kind == 'exit':
            if effect.conditional:
                targets.append(pc+4)
        else:
            targets.append(pc+4)
        outside.extend((pc, target) for target in targets if not entry <= target < end)
        graph[pc] = {target for target in targets if entry <= target < end}
    if outside:
        return refused('explicit flow leaves the function extent')
    predecessors = {pc: set() for pc in graph}
    for pc, successors in graph.items():
        for target in successors:
            predecessors[target].add(pc)
    if predecessors[branch] != {branch-4}:
        return refused('another predecessor can bypass the predicate')
    graph[branch].discard(guard_target)

    def reachable(edges, start, blocked=frozenset()):
        seen, pending = set(), [start]
        while pending:
            pc = pending.pop()
            if pc in seen or pc in blocked:
                continue
            seen.add(pc)
            pending.extend(edges[pc])
        return seen

    forward = reachable(graph, entry)
    if branch not in forward or observed_read in forward:
        return refused('cut does not isolate the reported read behind a reachable guard')
    reverse = {pc: set() for pc in graph}
    for pc, successors in graph.items():
        for target in successors:
            reverse[target].add(pc)
    ancestors = forward & reachable(reverse, branch)
    if any(decoded[pc].kind in ('call', 'unknown', 'impure') for pc in ancestors):
        return refused('call or unknown effects can change the guard input')
    writes = {pc for pc in ancestors if f'r{source_register}' in decoded[pc].writes}
    if not writes or branch in reachable(graph, entry, writes):
        return refused('input initialization does not dominate the guard')
    for pc in writes:
        word = read(pc)
        if word >> 26 != 14 or (word >> 16) & 31 or not word & 0x80:
            return refused('a guard-input write is not a literal negative signed byte')
    return {'proven_for_direct_entry': True, 'entry': entry, 'end_exclusive': end,
            'branch': branch, 'rejected_target': guard_target, 'observed_read': observed_read,
            'source_register': source_register, 'initializers': sorted(writes),
            'scope': 'Inductive first-edge proof under direct encoded control flow and ordinary LR-return conventions. '
                     'Does not certify arbitrary computed entries, exception transfers or the complete caller contract.'}


def audit(raw, catalog, jump_tables=None):
    if hashlib.sha256(raw).hexdigest() != catalog['dol_sha256']:
        raise ValueError('catalog belongs to a different DOL')
    image = Dol(raw)
    body = b''.join(image.word(pc).to_bytes(4, 'big') for pc in range(CAST, CAST_END, 4))
    if hashlib.sha256(body).hexdigest() != CAST_SHA256:
        raise ValueError('original cast scratch-state contract changed')
    tracer = Tracer(image.word, lambda pc: image.contains_text(pc, 4), jump_tables)
    calls, counts, read_sites = [], Counter(), {}
    for call in catalog['call_arguments']['calls']:
        site = call['site']
        decoded = wait_points.decode(image.word(site), site)
        if image.word(site) != call['word'] or decoded.kind != 'call' or decoded.target != CAST:
            raise ValueError('stale or non-cast call site')
        contexts = []
        for context in call['contexts']:
            bounds = (context['entry'], context['end_exclusive'])
            states = {}
            for register in SCRATCH:
                reads, barriers, escapes = tracer.trace(site + 4, register, bounds)
                for at in reads:
                    read_sites.setdefault(at, set()).add(register)
                row = {'read_sites': sorted(reads),
                       'barriers': [{'kind': kind, 'site': at} for kind, at in sorted(barriers)],
                       'survives_encoded_return': escapes}
                row['no_read_before_overwrite_or_return'] = not reads and not barriers
                row['no_read_or_return_escape_in_known_flow'] = not reads and not barriers and not escapes
                states[register] = row
                counts['register_contexts'] += 1
                counts['with_possible_read'] += bool(reads)
                counts['with_barrier'] += bool(barriers)
                counts['survives_return'] += escapes
                counts['no_read_or_return_escape'] += row['no_read_or_return_escape_in_known_flow']
            contexts.append({'entry': bounds[0], 'end_exclusive': bounds[1],
                             'catalog_reachable': context['reachable'], 'registers': states})
        calls.append({'site': site, 'word': call['word'], 'contexts': contexts})
    observations = []
    for at, registers in sorted(read_sites.items()):
        word = image.word(at)
        decoded = wait_points.decode(word, at)
        observations.append({'site': at, 'word': word, 'registers': sorted(registers),
                             'kind': decoded.kind,
                             'r1_relative_store': word >> 26 in (36, 38, 44, 47, 52, 54)
                             and (word >> 16) & 31 == 1})
    return {'schema': 1, 'dol_sha256': catalog['dol_sha256'], 'cast_body_sha256': CAST_SHA256,
            'normal_return_scratch_state': list(SCRATCH), 'calls': calls, 'counts': dict(counts),
            'read_sites': observations,
            'abi_clobbers_assumed': False, 'complete_caller_contract_proven': False,
            'scope': 'Conservative possible read-before-overwrite census under encoded direct flow and '
                     'LR-return conventions. Conditional paths are not value-pruned. Call/recursion/unknown '
                     'barriers and surviving return values are unresolved. Stack/memory, exception paths, '
                     'arbitrary computed entries, termination and parent callers are not certified.'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('dol', 'catalog', 'jump-tables', 'out'):
        parser.add_argument('--' + name, required=True, type=Path)
    args = parser.parse_args()
    raw = args.dol.read_bytes()
    catalog = json.loads(args.catalog.read_text())
    tables = read_jump_tables(args.jump_tables, FlowImage(raw))
    result = audit(raw, catalog, tables)
    paths = {'dol': args.dol, 'catalog': args.catalog, 'jump_tables': args.jump_tables,
             'tool': Path(__file__), 'decoder': Path(wait_points.__file__),
             'image_tool': Path(sys.modules[Dol.__module__].__file__),
             'jump_table_tool': Path(sys.modules[FlowImage.__module__].__file__)}
    result['inputs'] = {k: {'path': str(p.resolve()), 'sha256': hashlib.sha256(p.read_bytes()).hexdigest()}
                        for k, p in paths.items()}
    args.out.parent.mkdir(parents=True, exist_ok=True)
    temporary = args.out.with_suffix(args.out.suffix + '.tmp')
    temporary.write_text(json.dumps(result, indent=2) + '\n')
    temporary.replace(args.out)
    print(json.dumps(result['counts'], sort_keys=True))


if __name__ == '__main__':
    main()
