"""Original two-word register-transfer leaves: exact SOURCE transfer only.

A reviewed leaf is one Gekko move to/from the MSR, a special-purpose register or
the time base, followed by the unconditional non-linking return 4E800020.
Register numbers are admitted only from the Gekko User's Manual v1.2 tables
(docs/reference/ppc/gekko_user_manual.txt): mfspr Table 12-10 pp.463-465, mftb
Table 12-11 p.468, mtspr Table 12-13 pp.477-479; mfmsr p.461, mtmsr p.475.

Hardware consequences of a write (cache/L2/decrementer/write-gather/performance
monitor/MSR delivery), privilege faults, live register values and asynchronous
interrupts between the two words are NOT modelled. Caller provenance reuses the
canonical call-coordinate DAG; it is a source data-flow view, never native
ownership, liveness, or authorization to delete a console owner.
"""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import sys

if __package__:
    from .graphics_lift_inventory import Dol, Extents, decode, hx, table
    from .gqr_format_dataflow import gpr_dependencies
    from .platform_effect_inventory import platform_effect
    from .platform_leaf_register_effect import build_leaf_register_effect
    from .platform_procedure_contract import build_contract, build_incoming_contracts
    from .platform_call_coordinate_flow import build_call_coordinate_bundle, project_coordinate
else:
    from graphics_lift_inventory import Dol, Extents, decode, hx, table
    from gqr_format_dataflow import gpr_dependencies
    from platform_effect_inventory import platform_effect
    from platform_leaf_register_effect import build_leaf_register_effect
    from platform_procedure_contract import build_contract, build_incoming_contracts
    from platform_call_coordinate_flow import build_call_coordinate_bundle, project_coordinate

BLR = 0x4E800020
REVIEWED_XO = (83, 146, 339, 371, 467)

# Gekko UM v1.2 Tables 12-10/12-13: name and access level, never inferred.
_COMMON = {1: ('XER', 'user'), 8: ('LR', 'user'), 9: ('CTR', 'user'),
           18: ('DSISR', 'supervisor'), 19: ('DAR', 'supervisor'), 22: ('DEC', 'supervisor'),
           25: ('SDR1', 'supervisor'), 26: ('SRR0', 'supervisor'), 27: ('SRR1', 'supervisor'),
           282: ('EAR', 'supervisor'), 920: ('HID2', 'supervisor'), 921: ('WPAR', 'supervisor'),
           922: ('DMA_U', 'supervisor'), 923: ('DMA_L', 'supervisor'), 1008: ('HID0', 'supervisor'),
           1009: ('HID1', 'supervisor'), 1010: ('IABR', 'supervisor'), 1013: ('DABR', 'supervisor'),
           1017: ('L2CR', 'supervisor'), 1019: ('ICTC', 'supervisor'), 1020: ('THRM1', 'supervisor'),
           1021: ('THRM2', 'supervisor'), 1022: ('THRM3', 'supervisor')}
for _i in range(4):
    _COMMON[272 + _i] = (f'SPRG{_i}', 'supervisor')
for _i in range(16):
    _COMMON[528 + _i] = (f"{'IBAT' if _i < 8 else 'DBAT'}{(_i % 8) // 2}{'U' if _i % 2 == 0 else 'L'}", 'supervisor')
for _i in range(8):
    _COMMON[912 + _i] = (f'GQR{_i}', 'supervisor')
for _i, _n in enumerate(('UMMCR0', 'UPMC1', 'UPMC2', 'USIA', 'UMMCR1', 'UPMC3', 'UPMC4', 'USDA')):
    _COMMON[936 + _i] = (_n, 'user')
for _i, _n in enumerate(('MMCR0', 'PMC1', 'PMC2', 'SIA', 'MMCR1', 'PMC3', 'PMC4', 'SDA')):
    _COMMON[952 + _i] = (_n, 'supervisor')
MFSPR = {**_COMMON, 287: ('PVR', 'supervisor')}
MTSPR = {**_COMMON, 284: ('TBL', 'supervisor'), 285: ('TBU', 'supervisor')}
MFTB = {268: ('TBL', 'user'), 269: ('TBU', 'user')}
TABLES = {'mfspr': (MFSPR, 'Table 12-10 pp.463-465'), 'mtspr': (MTSPR, 'Table 12-13 pp.477-479'),
          'mftb': (MFTB, 'Table 12-11 p.468')}
MTMSR_NOTE = ('Gekko UM p.475: execution synchronizing; MSR[EE]/MSR[RI] changes are effective when the '
              'instruction completes, so a pending external or decrementer exception may be taken before '
              'the return word. Not modelled here.')


def implementation_pins():
    names = ('platform_register_transfer_leaf.py', 'graphics_lift_inventory.py', 'gqr_format_dataflow.py',
             'platform_effect_inventory.py', 'platform_leaf_register_effect.py', 'platform_procedure_contract.py',
             'platform_call_coordinate_flow.py')
    return {n: hashlib.sha256(Path(__file__).with_name(n).read_bytes()).hexdigest() for n in names}


def decode_transfer(word):
    """Exact reviewed field decode. Undefined forms are refused, never guessed."""
    if type(word) is not int or not 0 <= word <= 0xFFFFFFFF:
        raise ValueError('32-bit original word required')
    xo, gpr, a, b = (word >> 1) & 1023, (word >> 21) & 31, (word >> 16) & 31, (word >> 11) & 31
    if word >> 26 != 31 or xo not in REVIEWED_XO:
        raise ValueError('not a reviewed register-transfer encoding')
    if word & 1:
        raise ValueError('record bit set: transfer form is not admitted')
    if xo in (83, 146):
        if a or b:
            raise ValueError('reserved MSR-transfer field bits set')
        name = 'mfmsr' if xo == 83 else 'mtmsr'
        return dict(operation=name, direction='read' if xo == 83 else 'write', register='MSR',
                    register_number=None, access_level='supervisor', gpr=gpr,
                    manual_reference=f"{name} p.{461 if xo == 83 else 475}")
    name = {339: 'mfspr', 371: 'mftb', 467: 'mtspr'}[xo]
    number = a | (b << 5)
    admitted, where = TABLES[name]
    if number not in admitted:
        raise ValueError('register number outside the reviewed Gekko encoding table')
    if name == 'mtspr' and number == 8:
        raise ValueError('LR write redirects the following return: control transfer, not a leaf')
    label, level = admitted[number]
    return dict(operation=name, direction='write' if name == 'mtspr' else 'read', register=label,
                register_number=number, access_level=level, gpr=gpr, manual_reference=f'{name} {where}')


def _canonical_effect_agrees(word, transfer):
    effect = platform_effect(word)
    if effect is None or effect['operation'] != transfer['operation'] or effect['access'] != transfer['direction']:
        return False
    if transfer['register_number'] is not None:
        return effect.get('spr') == transfer['register_number'] and effect.get('gpr') == transfer['gpr']
    return True


def _may_continue_into(image, entry):
    """Can the preceding original word continue at entry?

    The shared direct-flow decoder treats sc as terminal; architecturally a
    system call resumes at the next word after its handler, so it stays open.
    """
    if not image.contains_text(entry - 4, 4):
        return False
    word = image.word(entry - 4)
    flow = decode(word, entry - 4)
    return flow is None or flow['may_fall_through'] or flow['mnemonic'] == 'sc'


def recognize_register_transfer_leaf(raw, entry):
    image = Dol(raw)
    if type(entry) is not int or entry % 4 or entry < 4 or not image.contains_text(entry, 8):
        raise ValueError('complete aligned original two-word leaf required')
    first, second = image.word(entry), image.word(entry + 4)
    if second != BLR:
        raise ValueError('unconditional non-linking blr 4E800020 required')
    transfer = decode_transfer(first)
    if not _canonical_effect_agrees(first, transfer):
        raise ValueError('canonical platform-effect decoder disagrees')
    procedure = build_contract(raw, entry)
    summary = procedure['summary']
    if ([w['site'] for w in procedure['words']] != [hx(entry), hx(entry + 4)] or summary['calls']
            or summary['unresolved_terminal_sites'] or summary['outside_text_edges'] or procedure['branches']
            or [r['site'] for r in procedure['return_like']] != [hx(entry + 4)]):
        raise ValueError('canonical direct flow is not exactly transfer then return')
    leaf = build_leaf_register_effect(raw, entry)
    written = [transfer['gpr']] if transfer['direction'] == 'read' else []
    if leaf['complete_normal_flow_encoded_gpr_effects']:
        if leaf['written_gprs'] != written:
            raise ValueError('canonical leaf GPR effect disagrees')
        gpr_signal = 'AGREES'
    else:
        gpr_signal = 'UNKNOWN'
    isolated = not _may_continue_into(image, entry)
    classification = ('STRUCTURAL MATCH' if not isolated else
                      'PROVEN' if gpr_signal == 'AGREES' else 'STRONGLY SUPPORTED')
    read, g, reg = transfer['direction'] == 'read', transfer['gpr'], transfer['register']
    previous = image.word(entry - 4) if image.contains_text(entry - 4, 4) else None
    return dict(schema='platform_register_transfer_leaf_v1', entry=hx(entry),
        inputs=dict(dol_sha256=hashlib.sha256(raw).hexdigest(), source_pins=implementation_pins()),
        classification=classification, candidate_family='register-transfer-leaf:' + transfer['operation'],
        proof_scope='Exact two-word source transfer and return on a completed normal path ONLY.',
        evidence=dict(complete_original_words=True, manual_table_admitted=True,
                      manual_reference=transfer['manual_reference'], canonical_platform_effect_agrees=True,
                      canonical_direct_flow_exact=True, canonical_leaf_gpr_effect=gpr_signal),
        shared_semantics=dict(
            transfer=f'r{g} <- {reg}' if read else f'{reg} <- r{g}',
            destination_gpr=g if read else None, source_gpr=None if read else g,
            gprs_written_on_normal_return=written,
            other_gprs_unchanged_on_normal_return=(True if gpr_signal == 'AGREES' else 'recognizer field decode only'),
            normal_return='Unconditional branch to the current LR value; LR itself is not written.',
            memory_cr_xer_or_fpr_encodings=False),
        differences=dict(operation=transfer['operation'], direction=transfer['direction'], register=reg,
                         register_number=transfer['register_number'], gpr=g, gpr_is_r3=g == 3,
                         access_level=transfer['access_level'],
                         privileged_execution_required=transfer['access_level'] == 'supervisor'),
        entry_context=dict(preceding_word=None if previous is None else hx(previous),
                           preceding_word_may_continue_into_entry=not isolated, isolated_leaf=isolated,
                           note='A non-isolated match is a transfer/return SUFFIX of possibly larger code, '
                                'not a separately entered leaf.'),
        architectural_notes=[MTMSR_NOTE] if transfer['operation'] == 'mtmsr' else [],
        original_words=[dict(site=w['site'], word=w['word'], file_offset=w['file_offset']) for w in procedure['words']],
        scope=dict(source_successful_normal_return_only=True, register_values_or_domains_proven=False,
                   hardware_side_effects_modeled=False, privilege_program_or_interrupt_delivery_proven=False,
                   asynchronous_intervention_between_words_excluded=False,
                   source_entry_identity_or_all_callers_proven=False,
                   native_retirement_proven=False, deletion_authorized=False))


def scan_register_transfer_leaves(raw):
    """Every aligned original text pair ending in blr; refusals are retained."""
    image = Dol(raw)
    matches, refused, other, words = [], [], [], 0
    for section in image.text:
        start, stop = section['address'], section['address'] + section['size']
        words += section['size'] // 4
        for pc in range(start, stop - 4, 4):
            if image.word(pc + 4) != BLR:
                continue
            word = image.word(pc)
            if word >> 26 == 31 and (word >> 1) & 1023 in REVIEWED_XO:
                try:
                    matches.append(recognize_register_transfer_leaf(raw, pc))
                except ValueError as error:
                    refused.append(dict(site=hx(pc), word=hx(word), reason=str(error)))
                continue
            effect = platform_effect(word)
            if effect:
                other.append(dict(site=hx(pc), word=hx(word), effect=effect, classification='UNKNOWN',
                                  reason='platform word outside the reviewed transfer forms'))
    if matches:
        incoming = build_incoming_contracts(raw, [int(m['entry'], 16) for m in matches])
        for match, report in zip(matches, incoming):
            match['incoming_summary'] = report['summary']
    clusters = defaultdict(list)
    for m in matches:
        d = m['differences']
        clusters[(d['operation'], d['register'], d['gpr'])].append(m['entry'])
    return dict(schema='platform_register_transfer_leaf_scan_v1',
        inputs=dict(dol_sha256=hashlib.sha256(raw).hexdigest(), source_pins=implementation_pins()),
        summary=dict(original_text_words=words, matches=len(matches),
                     classifications=dict(sorted(Counter(m['classification'] for m in matches).items())),
                     isolated_leaves=sum(m['entry_context']['isolated_leaf'] for m in matches),
                     refused=len(refused), refusal_reasons=dict(sorted(Counter(r['reason'] for r in refused).items())),
                     unreviewed_platform_return_pairs=len(other), identity_clusters=len(clusters)),
        matches=matches, refused=refused, unreviewed_platform_return_pairs=other,
        clusters=[dict(operation=k[0], register=k[1], gpr=k[2], entries=v) for k, v in sorted(clusters.items())],
        scope=dict(source_only=True, whole_text_pairs_scanned=True, refusals_are_not_deadness=True,
                   duplicate_byte_identical_leaves_retained=True, functions_semantically_solved=False,
                   native_ownership_or_retirement_proven=False, deletion_authorized=False))


def rlwinm_mask(word):
    """rlwinm rA,rS,0,MB,ME with Rc=0 is rS AND MASK(MB,ME); anything else is None."""
    if word >> 26 != 21 or word & 1 or (word >> 11) & 31:
        return None
    mb, me = (word >> 6) & 31, (word >> 1) & 31
    mask = 0
    for bit in range(32):
        if (mb <= bit <= me) if mb <= me else (bit >= mb or bit <= me):
            mask |= 1 << (31 - bit)
    return dict(rs=(word >> 21) & 31, ra=(word >> 16) & 31, mb=mb, me=me, mask=mask)


def _unchanged_until(ctx, start, stop, register):
    for pc in range(start, stop, 4):
        word = ctx['image'].word(pc)
        if pc in ctx['destinations'] or decode(word, pc):
            return False
        op, xo = word >> 26, (word >> 1) & 1023
        deps = None if op == 31 and xo in (339, 467) and word & 1 else gpr_dependencies(word)
        if deps is None or register in deps:
            return False
    return True


def _through_call(ctx, call_site, register, trail):
    row = ctx['rows'].get(call_site)
    boundary = hx(call_site + 4)
    if row is None:
        return dict(kind='unresolved_boundary', gpr=register, boundary=boundary,
                    reason='call target is not a reviewed isolated transfer leaf')
    leaf = ctx['leaves'][int(row['target'], 16)]
    if leaf['shared_semantics']['destination_gpr'] == register:
        if row['conditional_call']:
            return dict(kind='unresolved_boundary', gpr=register, boundary=boundary,
                        reason='conditional call: transferred or earlier value')
        return dict(kind='leaf_read', register=leaf['differences']['register'], call_site=hx(call_site),
                    leaf=leaf['entry'], leaf_classification=leaf['classification'])
    if leaf['classification'] != 'PROVEN':
        return dict(kind='unresolved_boundary', gpr=register, boundary=boundary,
                    reason='leaf GPR preservation is not independently agreed')
    return dict(kind='preserved_across_leaf', gpr=register, call_site=hx(call_site), leaf=leaf['entry'],
                of=_expression(ctx, call_site, register, trail))


def _mask_completion(ctx, call_site, node, trail):
    site = int(node['site'], 16)
    word = ctx['image'].word(site)
    unresolved = dict(kind='unresolved_definition', site=node['site'], word=node['word'], gpr=node['gpr'])
    if hx(word) != node['word']:
        raise ValueError('canonical node word differs from the original image')
    form = rlwinm_mask(word)
    if form is None or form['ra'] != node['gpr'] or gpr_dependencies(word) != {form['ra']: [form['rs']]}:
        return unresolved
    if form['rs'] != form['ra'] and _unchanged_until(ctx, site + 4, call_site, form['rs']):
        operand = _expression(ctx, call_site, form['rs'], trail)
        rule = 'operand register unchanged from the mask word to the call'
    elif site - 4 in ctx['rows'] and site not in ctx['destinations']:
        operand = _through_call(ctx, site - 4, form['rs'], trail)
        rule = 'mask word directly follows a reviewed leaf call and is not a direct destination'
    else:
        return unresolved
    return dict(kind='and_mask_u32', mask=hx(form['mask']), site=node['site'], word=node['word'],
                completion_rule=rule, canonical_dag_unresolved=True, of=operand)


def _expression(ctx, call_site, register, trail=frozenset()):
    key = (call_site, register)
    if key in trail:
        raise ValueError('cyclic coordinate chain')
    trail = trail | {key}
    dag = project_coordinate(ctx['rows'][call_site]['argument_dag'], register)
    nodes = dag['nodes']

    def build(index):
        n = nodes[index]
        kind = n['kind']
        if kind == 'literal':
            return dict(kind='literal', value=n['value'], site=n['site'])
        if kind == 'copy':
            return dict(kind='copy', site=n['site'], of=build(n['inputs'][0]))
        if kind == 'add_offset_u32':
            return dict(kind='add_u32', offset=n['offset'], site=n['site'], of=build(n['inputs'][0]))
        if kind == 'or_immediate_u32':
            return dict(kind='or_u32', bits=n['bits'], site=n['site'], of=build(n['inputs'][0]))
        if kind == 'symbolic_memory_load':
            return dict(kind='symbolic_load', site=n['site'], width_bytes=n['width_bytes'],
                        displacement=n['displacement'], encoded_zero_base=n['encoded_zero_base'],
                        base=build(n['inputs'][0]) if n['inputs'] else None, value_proven=False)
        if kind == 'current_gpr_at_boundary':
            if n['boundary_reason'] == 'call':
                return _through_call(ctx, int(n['boundary'], 16) - 4, n['gpr'], trail)
            return dict(kind='unresolved_boundary', gpr=n['gpr'], boundary=n['boundary'], reason=n['boundary_reason'])
        if kind == 'unresolved_definition':
            return _mask_completion(ctx, call_site, n, trail)
        return dict(kind='unresolved_node', node_kind=kind)
    return build(dag['root'])


def _walk(expr):
    yield expr
    for key in ('of', 'base'):
        if isinstance(expr.get(key), dict):
            yield from _walk(expr[key])


def build_transfer_caller_frontier(raw, functions_raw=None):
    """Direct callers of isolated leaves with canonical argument provenance."""
    return build_transfer_report(raw, functions_raw)['caller_frontier']


def build_transfer_report(raw, functions_raw=None):
    """One fresh whole-image scan and its caller frontier; nothing is supplied."""
    scan = scan_register_transfer_leaves(raw)
    return dict(scan=scan, caller_frontier=_frontier(raw, scan, functions_raw))


def _frontier(raw, scan, functions_raw):
    image = Dol(raw)
    leaves = {int(m['entry'], 16): m for m in scan['matches'] if m['entry_context']['isolated_leaf']}
    rows_csv = table(functions_raw, 'functions', image) if functions_raw is not None else []
    extents, names = Extents(rows_csv), {r['id']: r['name'] for r in rows_csv}
    calls, owners = [], defaultdict(lambda: dict(reads=Counter(), writes=Counter(), call_sites=[]))
    if leaves:
        bundle = build_call_coordinate_bundle(raw, sorted(leaves), tuple(range(32)))
        destinations = set()
        for section in image.text:
            for pc in range(section['address'], section['address'] + section['size'], 4):
                flow = decode(image.word(pc), pc)
                if flow and flow.get('target') is not None:
                    destinations.add(flow['target'])
        ctx = dict(image=image, leaves=leaves, destinations=destinations,
                   rows={int(r['site'], 16): r for r in bundle['calls']})
        consumed = defaultdict(list)
        for site in sorted(ctx['rows']):
            row = ctx['rows'][site]
            leaf = leaves[int(row['target'], 16)]
            d = leaf['differences']
            owner_ids = list(extents.at(site) or ())
            record = dict(site=hx(site), word=row['word'], leaf=leaf['entry'], leaf_classification=leaf['classification'],
                          register=d['register'], direction=d['direction'], conditional_call=row['conditional_call'],
                          owner_annotations=[dict(id=i, name=names.get(i)) for i in owner_ids])
            if d['direction'] == 'write':
                expr = _expression(ctx, site, d['gpr'])
                kinds = [e['kind'] for e in _walk(expr)]
                reads = [e for e in _walk(expr) if e['kind'] == 'leaf_read']
                for r in reads:
                    consumed[r['call_site']].append(hx(site))
                complete = not any(k.startswith('unresolved') or k == 'symbolic_load' for k in kinds)
                record.update(argument_gpr=d['gpr'], argument=expr,
                              argument_resolution='COMPLETE' if complete else 'PARTIAL',
                              argument_classification='STRONGLY SUPPORTED' if complete else 'UNKNOWN',
                              reads_feeding_argument=[dict(register=r['register'], call_site=r['call_site']) for r in reads],
                              read_modify_write_same_register=any(r['register'] == d['register'] for r in reads),
                              reviewed_mask_completions=kinds.count('and_mask_u32'))
            calls.append(record)
            for i in owner_ids or ['<undeclared>']:
                o = owners[i]
                o['reads' if d['direction'] == 'read' else 'writes'][d['register']] += 1
                o['call_sites'].append(hx(site))
        for record in calls:
            if record['direction'] == 'read':
                record['consumed_by_reviewed_write_calls'] = sorted(set(consumed.get(record['site'], [])))
                record['other_consumers'] = 'UNKNOWN: only reviewed write-leaf arguments are traced'
    owner_rows = [dict(owner=i, name=names.get(i), reads=dict(sorted(o['reads'].items())),
                       writes=dict(sorted(o['writes'].items())), call_sites=o['call_sites'])
                  for i, o in sorted(owners.items())]
    writes = [c for c in calls if c['direction'] == 'write']
    return dict(schema='platform_register_transfer_caller_frontier_v1',
        inputs=dict(dol_sha256=hashlib.sha256(raw).hexdigest(),
                    functions_sha256=hashlib.sha256(functions_raw).hexdigest() if functions_raw is not None else None,
                    source_pins=implementation_pins()),
        summary=dict(isolated_leaves=len(leaves), direct_call_sites=len(calls), write_calls=len(writes),
                     complete_write_arguments=sum(c['argument_resolution'] == 'COMPLETE' for c in writes),
                     read_modify_write_same_register=sum(c['read_modify_write_same_register'] for c in writes),
                     reviewed_mask_completions=sum(c['reviewed_mask_completions'] for c in writes),
                     owner_annotations=len(owner_rows)),
        calls=calls, owners=owner_rows,
        premises=['Both calls execute and return normally (leaf contracts are normal-path only).',
                  'No asynchronous exception, interrupt or other thread changes the register or GPRs between words.',
                  'No indirect entry lands inside a traced straight-line span (direct destinations are excluded).',
                  'Masks are completed only for rlwinm SH=0 Rc=0 words whose operand provenance is unambiguous.'],
        scope=dict(source_only=True, canonical_call_coordinate_dag_reused=True, loaded_values_supplied=False,
                   calls_to_non_leaf_targets_sever_provenance=True, read_consumers_beyond_write_leaves='UNKNOWN',
                   hardware_side_effects_or_live_values_proven=False, callers_complete_or_live_proven=False,
                   native_ownership_or_retirement_proven=False, deletion_authorized=False))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dol', type=Path, required=True)
    parser.add_argument('--functions', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        if args.out.exists():
            raise ValueError('preserve previous runs; output already exists')
        raw = args.dol.read_bytes()
        functions_raw = args.functions.read_bytes() if args.functions else None
        report = build_transfer_report(raw, functions_raw)
        if args.dol.read_bytes() != raw or (args.functions and args.functions.read_bytes() != functions_raw):
            raise ValueError('input changed during scan')
        with args.out.open('x', encoding='utf-8', newline='\n') as stream:
            json.dump(report, stream, indent=2)
            stream.write('\n')
    except (OSError, ValueError) as error:
        print(f'register-transfer leaf scan refused: {error}', file=sys.stderr)
        return 1
    print(json.dumps(dict(scan=report['scan']['summary'], caller_frontier=report['caller_frontier']['summary']),
                     sort_keys=True))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
