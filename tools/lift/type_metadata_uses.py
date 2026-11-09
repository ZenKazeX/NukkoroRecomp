"""Audit static stores, reads and call values relevant to type-metadata ownership.

This is evidence for a migration, never an immutability certificate. It keeps
unknown store destinations, overlapping entry contexts and unresolved control
flow visible. A stored two-word header is only a virtual-table candidate until
its destination's object ownership is recovered.
"""
import argparse
from bisect import bisect_right
from collections import Counter
import hashlib
import json
from pathlib import Path
import struct
import sys

import type_catalog

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import gx_site_inventory as flow
from graphics_lift_inventory import Dol, decode, table
from format_memory_dependencies import memory_effect

EMPTY_READ_DEFINITIONS = tuple(frozenset() for _ in range(32))


class SpanIndex:
    def __init__(self, spans):
        merged = []
        for begin, end in sorted(spans):
            if begin < 0 or end <= begin or end > 0x100000000:
                raise ValueError('invalid ownership span')
            if merged and begin <= merged[-1][1]:
                merged[-1][1] = max(end, merged[-1][1])
            else:
                merged.append([begin, end])
        self.spans = merged
        self.ends = [end for _, end in merged]

    def overlaps(self, address, size):
        # Effective addresses wrap at 32 bits, including a bulk store's end.
        if address + size > 0x100000000:
            return self.overlaps(address, 0x100000000 - address) + self.overlaps(0, address + size - 0x100000000)
        result = []
        index = bisect_right(self.ends, address)
        while index < len(self.spans) and self.spans[index][0] < address + size:
            begin, end = self.spans[index]
            result.append([max(begin, address), min(end, address + size)])
            index += 1
        return result


def store_extent(form, word, values):
    name, width, _, _, _, _, _, _ = form
    address = flow.effective_address(form, values)
    exact = width is not None
    if name in ('dcbz', 'dcbz_l') and address is not None:
        address &= ~31
    if width is None:
        if name.startswith('psq_st'):
            # One/two values, at most four bytes each; GQR may shorten them.
            wbit = (word >> (10 if word >> 26 == 4 else 15)) & 1
            width = 4 if wbit else 8
        elif name == 'stswx':
            width = 127  # XER's seven-bit byte count, which this solver does not track.
        else:
            raise ValueError(f'unbounded store form: {name}')
    return address, width, exact


def stored_words(form, values):
    """Complete known words only; partial string stores cannot establish a header."""
    name, width, _, domain, source, _, _, _ = form
    if domain != 'gpr':
        return []
    if name == 'stmw':
        registers = [(i - source, i) for i in range(source, 32)]
    elif name == 'stswi':
        registers = [(i, (source + i) % 32) for i in range(width // 4)]
    elif name in ('stw', 'stwu', 'stwx', 'stwux', 'stwcx.', 'ecowx', 'stwbrx'):
        registers = [(0, source)]
    else:
        return []
    result = []
    for offset, register in registers:
        if flow.known(values[register]):
            value = values[register][1]
            if name == 'stwbrx':
                value = int.from_bytes(value.to_bytes(4, 'big'), 'little')
            result.append((4 * offset, value))
    return result


def site_summary(sites):
    ordered = sorted(sites)
    encoded = b''.join(struct.pack('>I', site) for site in ordered)
    return {'count': len(ordered), 'sha256_of_sorted_be32_sites': hashlib.sha256(encoded).hexdigest(),
            'first_sites': ordered[:32]}


def metadata_data_references(raw, catalog):
    """Classify aligned data-word references; unclassified references stay visible."""
    reader = type_catalog.Reader(raw)
    spans = SpanIndex((s['address'], s['address'] + s['size']) for s in catalog['metadata_spans'])
    owned = {}

    def field(address, kind):
        owned.setdefault(address, set()).add(kind)

    for record in catalog['descriptors']:
        descriptor = record['address']
        field(descriptor, 'descriptor_name')
        field(descriptor+4, 'descriptor_bases')
        cursor = reader.word(descriptor+4)
        for base in record['bases']:
            field(cursor, 'base_type')
            cursor += 8
            if base['source_restricted']:
                cursor += 4
                for _ in base['sources']:
                    field(cursor, 'restricted_source_type')
                    cursor += 8
    for header in catalog['candidate_vtables']:
        field(header['address'], 'vtable_type')
    counts, external = Counter(), []
    for section in reader.image.sections:
        if section['kind'] != 'data':
            continue
        begin, end = section['address'], section['address'] + section['size']
        for at in range((begin+3) & ~3, end-3, 4):
            offset = section['file_offset'] + at - begin
            value = struct.unpack_from('>I', raw, offset)[0]
            if not spans.overlaps(value, 1):
                continue
            roles = sorted(owned.get(at, ()))
            if roles:
                for role in roles:
                    counts[role] += 1
            else:
                external.append({'address': at, 'value': value})
    return {'owned_reference_roles': dict(counts), 'external_references': external,
            'scope': 'Aligned file-backed data words pointing into catalog inputs. Roles identify source fields, '
                     'not all possible code consumers; unaligned values, computed pointers and host code are not certified.'}


def read_extent(word, values, pc):
    effect = memory_effect(word, (values, (), EMPTY_READ_DEFINITIONS), pc)
    if effect is None or effect['access'] != 'read':
        return None
    base = flow.constant(0) if effect['zero_base'] else values[effect['base_gpr']]
    index = values[effect['index_gpr']] if effect.get('indexed') else flow.constant(effect.get('displacement', 0))
    address = (base[1] + index[1]) & flow.U32 if flow.known(base) and flow.known(index) else None
    width = effect['request_bytes']
    exact = width is not None
    if width is None:
        width = 4 * effect.get('elements', 2)
    return address, width, exact


def audit(raw, functions_raw, jump_tables, catalog, progress=None, include_consumers=True):
    if catalog['dol_sha256'] != hashlib.sha256(raw).hexdigest():
        raise ValueError('catalog belongs to a different DOL')
    image = Dol(raw)
    declarations = table(functions_raw, 'functions', image)
    metadata = SpanIndex((s['address'], s['address'] + s['size']) for s in catalog['metadata_spans'])
    headers = SpanIndex((v['address'], v['address'] + 8) for v in catalog['candidate_vtables'])
    reader = type_catalog.Reader(raw)
    known_descriptors = {d['address'] for d in catalog['descriptors']}
    known_vtables = {v['address'] for v in catalog['candidate_vtables']}
    candidates = {}

    def candidate(address):
        if address in candidates:
            return candidates[address]
        try:
            descriptor, adjustment = reader.word(address), reader.word(address + 4)
            record = reader.descriptor(descriptor)
            if not type_catalog.NAME.fullmatch(bytes.fromhex(record['name_bytes'])):
                raise ValueError('not a structural class name')
            descendants = reader.closure([descriptor])
            result = {'address': address, 'descriptor': descriptor, 'name_bytes': record['name_bytes'],
                      'adjustment': adjustment if adjustment < 0x80000000 else adjustment - 0x100000000,
                      'in_prior_vtable_census': address in known_vtables,
                      'new_descriptors': sorted(set(descendants) - known_descriptors)}
        except ValueError:
            result = None
        candidates[address] = result
        return result

    stores, unknown_ea, reached, raw_stores = set(), set(), set(), set()
    contexts, literal_hits, header_stores, control, mismatches = Counter(), [], [], [], []
    sda_writes, input_reads, call_values = [], [], []
    read_sites, unknown_read_sites = set(), set()
    for section in image.text:
        for pc in range(section['address'], section['address'] + section['size'], 4):
            if flow.store_form(image.word(pc)):
                raw_stores.add(pc)
    unique_declarations = sorted({(d['address'], d['address'] + d['size']) for d in declarations})
    for number, (begin, end) in enumerate(unique_declarations, 1):
        # All independently declared overlapping extents remain separate. Within
        # this extent, a declaration is also a legal local continuation label.
        words, states, _, _, _ = flow.analyse_procedure(
            image.word, lambda pc: begin <= pc < end and image.contains_text(pc, 4), begin, jump_tables,
            pin_sda=False)
        unseeded_states = None

        def unseeded_values(pc):
            nonlocal unseeded_states
            if unseeded_states is None:
                _, unseeded_states, _, _, _ = flow.analyse_procedure(
                    image.word, lambda at: begin <= at < end and image.contains_text(at, 4),
                    begin, jump_tables, pin_sda=False, seed_sda=False)
            return unseeded_states[pc][0]

        reached.update(words)
        for pc, word in sorted(words.items()):
            values = states[pc][0]
            destinations = flow.destinations(word)
            if destinations is not None and (changed := sorted(set(destinations) & set(flow.SDA_BASES))):
                sda_writes.append({'entry': begin, 'site': pc, 'registers': changed})
            edge = decode(word, pc)
            if include_consumers and edge and edge['link']:
                for register in range(3, 11):
                    value = values[register]
                    if flow.known(value) and metadata.overlaps(value[1], 1):
                        independent = unseeded_values(pc)[register]
                        call_values.append({'entry': begin, 'site': pc, 'callee': edge.get('target'),
                                            'register': register, 'value': value[1],
                                            'requires_entry_sda': not flow.known(independent) or independent[1] != value[1]})
            read = read_extent(word, values, pc) if include_consumers else None
            if read is not None:
                address, width, exact = read
                read_sites.add(pc)
                if address is None:
                    unknown_read_sites.add(pc)
                elif (overlap := metadata.overlaps(address, width)):
                    independent = read_extent(word, unseeded_values(pc), pc)
                    input_reads.append({'entry': begin, 'site': pc, 'word': word, 'effective_address': address,
                                        'width_bound': width, 'width_exact': exact, 'metadata_overlap': overlap,
                                        'requires_entry_sda': independent[0] != address})
            if edge and not edge['link'] and edge.get('target_source') and not edge.get('return_like'):
                if edge['target_source'] != 'CTR' or pc not in jump_tables:
                    control.append({'entry': begin, 'site': pc, 'source': edge['target_source']})
            form = flow.store_form(word)
            if not form:
                continue
            stores.add(pc)
            contexts['stores'] += 1
            address, width, exact = store_extent(form, word, values)
            base = {'entry': begin, 'site': pc, 'word': word, 'mnemonic': form[0],
                    'effective_address': address, 'width_bound': width, 'width_exact': exact}
            if address is None:
                unknown_ea.add(pc)
                contexts['unknown_effective_address'] += 1
            else:
                overlap = metadata.overlaps(address, width)
                vtable_overlap = headers.overlaps(address, width)
                if overlap or vtable_overlap:
                    independent_address, _, _ = store_extent(form, word, unseeded_values(pc))
                    literal_hits.append(dict(base, metadata_overlap=overlap, vtable_header_overlap=vtable_overlap,
                                             requires_entry_sda=independent_address != address))
            for offset, value in stored_words(form, values):
                if (found := candidate(value)) is not None:
                    independent = (offset, value) in stored_words(form, unseeded_values(pc))
                    header_stores.append(dict(base, word_offset=offset, stored_header=value,
                                              requires_entry_sda=not independent))
            # A known store to immutable inputs is evidence to inspect, not
            # automatically a mutation: the written value can equal the image.
            if address is not None:
                for offset, value in stored_words(form, values):
                    at = (address + offset) & 0xffffffff
                    if metadata.overlaps(at, 4) or headers.overlaps(at, 4):
                        try:
                            before = reader.word(at)
                        except ValueError:
                            continue
                        if before != value:
                            mismatches.append(dict(base, address=at, initial_word=before, stored_word=value))
        if progress and (number % 1000 == 0 or number == len(unique_declarations)):
            progress(number, len(unique_declarations))
    used_headers = sorted({s['stored_header'] for s in header_stores})
    return {'schema': 1, 'dol_sha256': hashlib.sha256(raw).hexdigest(),
            'declarations': len(unique_declarations), 'store_contexts': dict(contexts),
            'raw_store_words': site_summary(raw_stores), 'analyzed_store_sites': site_summary(stores),
            'unknown_destination_sites': site_summary(unknown_ea),
            'raw_store_words_not_reached': site_summary(raw_stores - reached),
            'literal_input_stores': literal_hits, 'different_known_word_stores': mismatches,
            'stored_header_candidates': [candidates[address] for address in used_headers],
            'header_stores': header_stores, 'unresolved_control': control,
            'explicit_sda_register_writes': sda_writes,
            'consumer_census_enabled': include_consumers,
            'consumer_catalog_counts': catalog['counts'],
            'data_references': metadata_data_references(raw, catalog) if include_consumers else None,
            'literal_input_reads': input_reads, 'known_metadata_call_values': call_values,
            'analyzed_read_sites': site_summary(read_sites), 'unknown_read_destination_sites': site_summary(unknown_read_sites),
            'new_descriptor_roots': sorted({candidates[a]['descriptor'] for a in used_headers
                                            if candidates[a]['new_descriptors']}),
            'immutability_proven': False, 'complete_dynamic_domain_proven': False,
            'scope': 'Declared-entry known-bits CFG with EABI call clobbers. SDA bases are seeded at entries, '
                     'but explicit writes are honored instead of pinning their values. Relevant stores are '
                     'also analyzed with unknown entry SDA registers; assumption-dependent rows are marked. '
                     'Call-register values are candidates, not proof of argument arity or use. Read forms use the '
                     'shared explicit-memory decoder; unsupported read forms remain outside this census. '
                     'No interprocedural argument propagation or memory-load inference. Unknown destinations '
                     'can alias metadata. Stored headers require object-field ownership recovery; their first '
                     'method need not be nonnull. Bulk/native/host writes and snapshot restore are outside '
                     'this instruction census. No production binding is authorized by this report alone.'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('dol', 'functions', 'jump-tables', 'catalog', 'out'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    try:
        raw = args.dol.read_bytes()
        catalog = json.loads(args.catalog.read_text(encoding='utf-8-sig'))
        result = audit(raw, args.functions.read_bytes(), flow.read_jump_tables(args.jump_tables, flow.Image(raw)),
                       catalog, lambda n, total: print(f'ownership contexts {n}/{total}', flush=True))
        paths = {'dol': args.dol, 'functions': args.functions, 'jump_tables': args.jump_tables,
                 'catalog': args.catalog, 'tool': Path(__file__), 'catalog_tool': Path(type_catalog.__file__),
                 'flow_tool': Path(flow.__file__), 'transfer_tool': Path(flow.transfer.__code__.co_filename),
                 'memory_decoder': Path(memory_effect.__code__.co_filename),
                 'numeric_decoder': Path(__file__).resolve().parents[1] / 'particle_numeric_contract.py'}
        result['inputs'] = {name: {'path': str(path.resolve()), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}
                            for name, path in paths.items()}
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    except (OSError, ValueError, KeyError, struct.error) as error:
        parser.exit(1, f'type ownership audit refused: {error}\n')
    print(json.dumps({'contexts': result['declarations'], 'input_store_contexts': len(result['literal_input_stores']),
                      'stored_header_candidates': len(result['stored_header_candidates']),
                      'new_descriptor_roots': len(result['new_descriptor_roots'])}))


if __name__ == '__main__':
    main()
