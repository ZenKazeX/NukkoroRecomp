"""Conservative original-word memory dependencies, never alias/lifetime proof.

Addresses carry pre-instruction reaching definitions. No memory is read and no
two addresses are declared disjoint, even when their encoded bases differ.
Predecessor reachability includes both branch arms and loop back edges.
"""
from collections import deque

if __package__:
    from .graphics_lift_inventory import hx
    from .particle_numeric_contract import numeric_effect
else:
    from graphics_lift_inventory import hx
    from particle_numeric_contract import numeric_effect


def refs(items):
    return [dict(kind=k, site=hx(p), gpr=r) for k, p, r in sorted(items)]


def memory_effect(word, state, pc):
    """Decode explicit bounded accesses; unsupported effects stay barriers.

    Width is the encoded request, not a successful transfer/fault guarantee.
    Quantized widths are unknown unless the element type is proven on every
    structural path. Loaded values and MMU/address aliases remain unknown.
    """
    op, d, a, b = word >> 26, (word >> 21) & 31, (word >> 16) & 31, (word >> 11) & 31
    xo = (word >> 1) & 1023
    effect = None
    widths = {32:4, 33:4, 34:1, 35:1, 36:4, 37:4, 38:1, 39:1,
              40:2, 41:2, 42:2, 43:2, 44:2, 45:2}
    indexed = {23:4, 55:4, 87:1, 119:1, 151:4, 183:4, 215:1, 247:1,
               279:2, 311:2, 343:2, 375:2, 407:2, 439:2}
    if op in widths or op in (46, 47):
        disp = word & 65535
        effect = dict(access='write' if op in (36,37,38,39,44,45,47) else 'read',
                      base_gpr=a, displacement=disp if disp < 32768 else disp-65536,
                      updates_base=op in (33,35,37,39,41,43,45),
                      request_bytes=4*(32-d) if op in (46,47) else widths[op],
                      value_domain='gpr', value_registers=list(range(d,32)) if op in (46,47) else [d])
    elif op == 31 and xo in indexed:
        effect = dict(access='write' if xo in (151,183,215,247,407,439) else 'read',
                      base_gpr=a, index_gpr=b, indexed=True,
                      updates_base=xo in (55,119,183,247,311,375,439),
                      request_bytes=indexed[xo], value_domain='gpr', value_registers=[d])
    elif op in (4,48,49,50,51,52,53,54,55,56,57,60,61) or op == 31:
        numeric = numeric_effect(word)
        if numeric and numeric['memory']:
            effect = dict(numeric['memory'], value_domain='fpr', value_registers=[d])
            effect['updates_base'] = effect.get('updates_base', op in (49,51,53,55,57,61))
            effect['request_bytes'] = effect.get('scalar_width')
            if 'format_index' in effect:
                mask, value = state[0][32+effect['format_index']]
                shift = 0 if effect['access']=='write' else 16
                kind = (value >> shift) & 7
                known = mask & (7 << shift) == 7 << shift
                width = {0:4, 4:1, 5:2, 6:1, 7:2}.get(kind) if known else None
                effect['request_bytes'] = None if width is None else width*effect['elements']
                effect['format_type_known'] = known
                effect['reserved_format'] = known and kind not in (0,4,5,6,7)
    if effect is None:
        return None
    quantized='format_index' in effect
    encoded_update=effect.get('update_form',op in (57,61)) if quantized else effect['updates_base']
    if quantized:effect['updates_base']=encoded_update and a!=0
    zero_base=a==0 and (quantized or not effect['updates_base'])
    registers = ([] if zero_base else [a]) + ([effect['index_gpr']] if effect.get('indexed') else [])
    effect.update(site=hx(pc), word=hx(word),
                  address_sources=[dict(gpr=r, reaching_definitions=refs(state[2][r])) for r in sorted(set(registers))],
                  zero_base=zero_base, encoded_update_form=encoded_update,
                  zero_base_update_encoding=a==0 and encoded_update,
                  zero_update_exception_behavior_proven=False,
                  loaded_value_proven=False, successful_effect_proven=False,
                  physical_alias_or_resource_lifetime_proven=False)
    return effect


def memory_dependencies(slices, effects, successors, barriers):
    """List all stores/barriers that MAY precede each sliced memory read.

    A path is a structural possibility, not a dynamic execution. A later address
    may precede a read through a loop. No store forwarding, temporal generation,
    callback completion or disjointness is inferred. Work/output is bounded by
    the reachable original word graph and its finite store/read pairs.
    """
    predecessors = {pc:[] for pc in successors}
    for pc, targets in successors.items():
        for target in targets:
            predecessors[target].append(pc)
    writes = [e for e in effects if e['access']=='write']
    reads = {e['site']:e for e in effects if e['access']=='read'}
    cache = {}
    result = []
    for sl in slices:
        selected = []
        for site in sorted(set(n['site'] for n in sl['nodes']) & reads.keys()):
            pc = int(site,16)
            if pc not in cache:
                pending=deque(predecessors[pc]); seen=set()
                while pending:
                    p=pending.popleft()
                    if p in seen:continue
                    seen.add(p); pending.extend(predecessors[p])
                cache[pc]=seen
            previous=cache[pc]
            selected.append(dict(read=reads[site],
                may_precede_stores=[e for e in writes if int(e['site'],16) in previous],
                may_precede_barriers=[e for e in barriers if int(e['site'],16) in previous],
                alias_classification='unresolved: owner disjointness or original effect order required',
                read_from_or_snapshot_safety_proven=False))
        result.append(dict(writer_site=sl['writer_site'],spr=sl['spr'],reads=selected,
                           resource_ownership_or_retirement_proven=False))
    return result
