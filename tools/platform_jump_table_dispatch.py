"""Bounded jump-table dispatch recognized by data flow, not by one fixed layout.

Every original `bctr` is examined. A dispatch is admitted only when, in one
straight-line window with no direct branch destination after the guard:
  - a branch leaves when crN says rI > K (`bgt`/`bgtlr`: BO=12, BI=GT) or
    rI >= K (`bge`: BO=4, BI=LT, admitted maximum K-1), its exit does not
    re-enter the window, and the last word before it that may write crN is
    `cmplwi crN,rI,K` (scheduling may interleave words that cannot write crN);
  - every other word up to the load is decoded, non-branching, keeps rI until
    `rlwinm rS,rI,2,0,29` and keeps rS until the load;
  - `lwzx rT,rA,rB` reads rS plus a base the canonical solver proves exact;
  - rT is unchanged until `mtctr rT`, which immediately precedes `bctr`.
Targets are the original file-backed table words for indices 0..maximum. The
reviewed eight-word recognizer is a held-out cross-check, never an input.
Table immutability, other entries into the window, aliases and execution stay
UNKNOWN: classification is STRUCTURAL MATCH.
"""
import argparse
from bisect import bisect_right
from collections import Counter
import hashlib
import json
from pathlib import Path
import struct
import sys

if __package__:
    from .graphics_lift_inventory import Dol, decode, hx, table
    from .gqr_format_dataflow import U32, gpr_dependencies, solve_format_states
    from .platform_bounded_switch_contract import backed_bytes, recognize_switch
else:
    from graphics_lift_inventory import Dol, decode, hx, table
    from gqr_format_dataflow import U32, gpr_dependencies, solve_format_states
    from platform_bounded_switch_contract import backed_bytes, recognize_switch

BCTR = 0x4E800420
WINDOW = 24


def implementation_pins():
    names = ('platform_jump_table_dispatch.py', 'graphics_lift_inventory.py', 'gqr_format_dataflow.py',
             'platform_bounded_switch_contract.py', 'platform_procedure_contract.py', 'platform_effect_inventory.py',
             'particle_numeric_contract.py', 'format_memory_dependencies.py')
    return {n: hashlib.sha256(Path(__file__).with_name(n).read_bytes()).hexdigest() for n in names}


def may_write_cr_field(word, field):
    """Conservative: True unless the encoding provably leaves CR field `field` unchanged."""
    op, xo, rc = word >> 26, (word >> 1) & 1023, word & 1
    target = (word >> 23) & 7
    if op in (10, 11):
        return target == field
    if op in (13, 28, 29):  # addic., andi., andis. always record into CR0
        return field == 0
    if op in (20, 21, 23):
        return rc == 1 and field == 0
    if op == 31:
        if xo in (0, 32, 512):
            return target == field
        if xo == 144:
            return bool((word >> 12) & 0xFF & (0x80 >> field))
        if xo == 150:
            return field == 0
        return gpr_dependencies(word) is None or (rc == 1 and field == 0)
    if op in (4, 59, 63):
        if (op == 63 and xo in (0, 32, 64)) or (op == 4 and xo in (0, 32, 64, 96)):
            return target == field
        return gpr_dependencies(word) is None or (rc == 1 and field == 1)
    if op == 19:
        return True
    return gpr_dependencies(word) is None


def find_guard(words, start, load_site):
    """(compare site, branch site, index gpr, admitted maximum, branch flow) or None."""
    for branch_site in range(load_site - 4, start - 4, -4):
        if branch_site not in words:
            return None
        flow = decode(words[branch_site], branch_site)
        if not flow:
            continue
        if flow.get('link') or flow['mnemonic'] not in ('bc', 'bclr'):
            return None
        field, bit = flow['bi'] // 4, flow['bi'] % 4
        if (flow['bo'], bit) not in ((12, 1), (4, 0)):
            return None
        for compare_site in range(branch_site - 4, start - 4, -4):
            word = words.get(compare_site)
            if word is None or decode(word, compare_site):
                return None
            if not may_write_cr_field(word, field):
                continue
            if word >> 26 != 10 or word & (3 << 21) or (word >> 23) & 7 != field:
                return None  # cmplwi only: L=1, signed and other writers are refused
            limit = word & 65535
            if bit == 0 and limit == 0:
                return None
            return compare_site, branch_site, (word >> 16) & 31, limit if bit == 1 else limit - 1, flow
        return None
    return None


def _writes(word):
    deps = gpr_dependencies(word)
    return None if deps is None else set(deps)


class Context:
    """Per-image caches: direct destinations and canonical states of nearby roots."""

    def __init__(self, raw, roots):
        self.raw, self.image = raw, Dol(raw)
        self.roots = sorted(roots)
        self.states = {}
        self.destinations = set()
        for section in self.image.text:
            for pc in range(section['address'], section['address'] + section['size'], 4):
                flow = decode(self.image.word(pc), pc)
                if flow and flow.get('target') is not None:
                    self.destinations.add(flow['target'])

    def state_at(self, site):
        """Canonical pre-instruction state from the nearest preceding root reaching site."""
        position = bisect_right(self.roots, site)
        for root in reversed(self.roots[max(0, position - 8):position]):
            if root not in self.states:
                try:
                    self.states[root] = solve_format_states(self.raw, root)[3]
                except ValueError:
                    self.states[root] = {}
            if site in self.states[root]:
                return self.states[root][site], root
        return None, None


def recognize_dispatch(context, bctr_site):
    """Contract dict, or a refusal reason string."""
    image = context.image
    if type(bctr_site) is not int or bctr_site % 4 or not image.contains_text(bctr_site, 4):
        raise ValueError('aligned original text site required')
    if image.word(bctr_site) != BCTR:
        return 'not an unconditional bctr'
    start = bctr_site - 4 * WINDOW
    words = {pc: image.word(pc) for pc in range(start, bctr_site + 4, 4) if image.contains_text(pc, 4)}
    move = words.get(bctr_site - 4)
    if move is None or move & 0xFC1FFFFF != 0x7C0903A6:
        return 'mtctr does not immediately precede bctr'
    target = (move >> 21) & 31
    load_site = None
    for pc in range(bctr_site - 8, start - 4, -4):
        written = _writes(words[pc]) if pc in words else None
        if written is None or decode(words[pc], pc):
            return 'table target source not reached through decoded straight-line words'
        if target in written:
            load_site = pc
            break
    if load_site is None:
        return 'table target source not found in the window'
    load = words[load_site]
    if load >> 26 != 31 or (load >> 1) & 1023 != 23 or load & 1 or (load >> 21) & 31 != target:
        return 'table target source is not lwzx'
    first, second = (load >> 16) & 31, (load >> 11) & 31
    found = find_guard(words, start, load_site)
    if found is None:
        return 'no unsigned bounded guard in the window'
    guard_site, branch_site, index, maximum, flow = found
    if flow['mnemonic'] == 'bc' and guard_site <= flow['target'] <= bctr_site:
        return 'guard exit re-enters the dispatch window'
    if any(pc in context.destinations for pc in range(guard_site + 4, bctr_site + 4, 4)):
        return 'direct branch destination inside the guarded window'
    scale_site = scaled = None
    for pc in range(guard_site + 4, load_site, 4):
        if pc == branch_site:
            continue
        word, written = words[pc], _writes(words[pc])
        if written is None or decode(word, pc):
            return 'unsupported or control word inside the guarded window'
        if (scale_site is None and word >> 26 == 21 and (word >> 21) & 31 == index and (word >> 11) & 31 == 2
                and (word >> 6) & 31 == 0 and (word >> 1) & 31 == 29 and not word & 1):
            scale_site, scaled = pc, (word >> 16) & 31
            continue
        if scale_site is None and index in written:
            return 'index redefined between guard and scaling'
        if scaled is not None and scaled in written:
            return 'scaled index redefined before the load'
    if scale_site is None or scaled not in (first, second) or first == second:
        return 'lwzx does not read the scaled guarded index'
    base = second if first == scaled else first
    if base == 0:
        return 'zero-register table base is not a table'
    state, root = context.state_at(load_site)
    if state is None:
        return 'no canonical root reaches the table load'
    mask, address = state[0][base]
    if mask != U32:
        return 'table base is not an exact canonical constant'
    try:
        payload = backed_bytes(image, address, (maximum + 1) * 4)
    except ValueError:
        return 'table is not entirely original file-backed storage'
    entries = struct.unpack('>' + 'I' * (maximum + 1), payload)
    return dict(dispatch_site=hx(bctr_site), guard_site=hx(guard_site), guard_branch_site=hx(branch_site),
                guard_exit='conditional_return' if flow['mnemonic'] == 'bclr' else hx(flow['target']),
                index_gpr=index, admitted_unsigned_maximum=maximum, scale_site=hx(scale_site), load_site=hx(load_site),
                base_gpr=base, base_root=hx(root), original_table_address=hx(address),
                original_table_sha256=hashlib.sha256(payload).hexdigest(), targets=[hx(v & ~3) for v in entries],
                raw_entries_aligned=all(v % 4 == 0 for v in entries),
                targets_in_text=all(image.contains_text(v & ~3, 4) for v in entries),
                classification='STRUCTURAL MATCH')


def scan_dispatches(raw, functions_raw):
    image = Dol(raw)
    context = Context(raw, {r['address'] for r in table(functions_raw, 'functions', image)})
    reviewed = {}
    for section in image.text:
        for pc in range(section['address'], section['address'] + section['size'], 4):
            if image.word(pc) >> 26 == 10:
                contract = recognize_switch(raw, pc)
                if contract:
                    reviewed[contract['dispatch_site']] = contract
    matches, refused, reasons = [], [], Counter()
    for section in image.text:
        for pc in range(section['address'], section['address'] + section['size'], 4):
            if image.word(pc) != BCTR:
                continue
            result = recognize_dispatch(context, pc)
            if isinstance(result, str):
                reasons[result] += 1
                refused.append(dict(site=hx(pc), reason=result))
                continue
            prior = reviewed.get(result['dispatch_site'])
            if prior is not None:
                if (prior['original_table_address'] != result['original_table_address']
                        or prior['admitted_unsigned_maximum'] != result['admitted_unsigned_maximum']
                        or [e['effective_target'] for e in prior['original_entries']] != result['targets']):
                    raise ValueError('generalized dispatch disagrees with the reviewed recognizer at ' + hx(pc))
                result['reviewed_recognizer_agrees'] = True
            matches.append(result)
    missed = sorted(set(reviewed) - {m['dispatch_site'] for m in matches})
    return dict(schema='platform_jump_table_dispatch_scan_v1',
        inputs=dict(dol_sha256=hashlib.sha256(raw).hexdigest(), functions_sha256=hashlib.sha256(functions_raw).hexdigest(),
                    source_pins=implementation_pins()),
        summary=dict(bctr_sites=len(matches) + len(refused), recognized=len(matches),
                     reviewed_recognizer_sites=len(reviewed),
                     reviewed_sites_agreeing=sum(bool(m.get('reviewed_recognizer_agrees')) for m in matches),
                     reviewed_sites_missed=len(missed), table_targets=sum(len(m['targets']) for m in matches),
                     all_targets_in_text=all(m['targets_in_text'] for m in matches),
                     refusal_reasons=dict(sorted(reasons.items()))),
        dispatches=matches, refused=refused, reviewed_sites_missed=missed,
        scope=dict(classification_ceiling='STRUCTURAL MATCH', table_immutability_proven=False,
                   window_entry_closure_proven=False, execution_or_target_liveness_proven=False,
                   native_retirement_proven=False, deletion_authorized=False))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dol', type=Path, required=True)
    parser.add_argument('--functions', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        if args.out.exists():
            raise ValueError('preserve previous runs; output already exists')
        raw, functions_raw = args.dol.read_bytes(), args.functions.read_bytes()
        report = scan_dispatches(raw, functions_raw)
        if args.dol.read_bytes() != raw or args.functions.read_bytes() != functions_raw:
            raise ValueError('inputs changed during the scan')
        with args.out.open('x', encoding='utf-8', newline='\n') as stream:
            json.dump(report, stream, indent=1)
            stream.write('\n')
    except (OSError, ValueError) as error:
        print(f'jump table scan refused: {error}', file=sys.stderr)
        return 1
    print(json.dumps(report['summary'], sort_keys=True))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
