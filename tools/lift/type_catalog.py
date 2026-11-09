"""Recover immutable class descriptions for native type-query generation.

Explicit descriptor roots are evidence supplied by the caller. Virtual-table
discovery is a structural candidate census, not proof of object ownership or
metadata immutability. Missing or malformed explicit roots fail closed.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from graphics_lift_inventory import Dol, decode, table as function_table

MAX_NAME = 4096
MAX_BASES = 4096
MAX_TYPES = 16384
NAME = re.compile(rb"[A-Za-z_@][A-Za-z0-9_:@<>,* &().\[\]-]*\Z")


class Reader:
    def __init__(self, raw):
        self.image = Dol(raw)
        self.reads = set()
        self.cache = {}

    def data(self, address, size):
        if address < 0 or size < 0 or address + size > 0x100000000:
            raise ValueError("metadata span wraps")
        for section in self.image.sections:
            start, end = section['address'], section['address'] + section['size']
            if section['kind'] == 'data' and start <= address and address + size <= end:
                offset = section['file_offset'] + address - start
                self.reads.add((address, size))
                return self.image.raw[offset:offset + size]
        raise ValueError(f"metadata is not file-backed data: {address:#x}+{size}")

    def word(self, address):
        if address & 3:
            raise ValueError("unaligned metadata word")
        return struct.unpack('>I', self.data(address, 4))[0]

    def name(self, descriptor):
        at = self.word(descriptor)
        value = bytearray()
        for i in range(MAX_NAME):
            byte = self.data(at + i, 1)[0]
            if byte == 0:
                return bytes(value)
            value.append(byte)
        raise ValueError("unterminated type name")

    def descriptor(self, address):
        if address in self.cache:
            return self.cache[address]
        name = self.name(address)
        table = self.word(address + 4)
        bases = []
        while table:
            target = self.word(table)
            if not target:
                break
            if len(bases) == MAX_BASES:
                raise ValueError("unterminated base list")
            self.name(target)  # validate the referenced identity as well
            encoded = self.word(table + 4)
            restricted = bool(encoded & 0x80000000)
            offset = encoded & 0x7fffffff
            table += 8
            sources = []
            if restricted:
                count = self.word(table)
                if count > MAX_BASES:
                    raise ValueError("unsupported negative or oversized source count")
                table += 4
                for _ in range(count):
                    source, location = self.word(table), self.word(table + 4)
                    self.name(source)
                    sources.append({'descriptor': source, 'offset': location if location < 0x80000000 else location - 0x100000000})
                    table += 8
            bases.append({'descriptor': target, 'offset': offset,
                          'source_restricted': restricted, 'sources': sources})
        result = {'address': address, 'name_bytes': name.hex(), 'bases': bases}
        self.cache[address] = result
        return result

    def closure(self, roots):
        found, pending = {}, list(roots)
        while pending:
            address = pending.pop()
            if address in found:
                continue
            if len(found) == MAX_TYPES:
                raise ValueError("type closure exceeds explicit limit")
            descriptor = self.descriptor(address)
            found[address] = descriptor
            for base in descriptor['bases']:
                pending.append(base['descriptor'])
                pending.extend(source['descriptor'] for source in base['sources'])
        return found

    def vtable_candidates(self):
        candidates, rejected = [], 0
        considered = {}
        for section in self.image.sections:
            if section['kind'] != 'data':
                continue
            first = (section['address'] + 3) & ~3
            end = section['address'] + section['size']
            for at in range(first, end - 11, 4):
                offset = section['file_offset'] + at - section['address']
                descriptor, adjustment, method = struct.unpack_from('>III', self.image.raw, offset)
                if not self.image.contains_text(method, 4):
                    continue
                if descriptor not in considered:
                    try:
                        record = self.descriptor(descriptor)
                        considered[descriptor] = bool(NAME.fullmatch(bytes.fromhex(record['name_bytes'])))
                    except ValueError:
                        considered[descriptor] = False
                if not considered[descriptor]:
                    rejected += 1
                    continue
                candidates.append({'address': at, 'descriptor': descriptor,
                                   'adjustment': adjustment if adjustment < 0x80000000 else adjustment - 0x100000000,
                                   'first_method': method})
        return candidates, rejected


def build_catalog(raw, explicit_roots, discover=False, explicit_vtables=()):
    explicit_roots = tuple(explicit_roots)
    explicit_vtables = tuple(explicit_vtables)
    if any(type(root) is not int or not 0 < root <= 0xffffffff for root in explicit_roots):
        raise ValueError('descriptor roots must be nonzero 32-bit addresses')
    if any(type(root) is not int or not 0 < root <= 0xffffffff for root in explicit_vtables):
        raise ValueError('vtable roots must be nonzero 32-bit addresses')
    reader = Reader(raw)
    rooted_tables = []
    for address in sorted(set(explicit_vtables)):
        descriptor, adjustment, method = (reader.word(address + 4*i) for i in range(3))
        rooted_tables.append({'address': address, 'descriptor': descriptor,
                              'adjustment': adjustment if adjustment < 0x80000000 else adjustment - 0x100000000,
                              'first_method': method})
    explicit = reader.closure(explicit_roots + tuple(v['descriptor'] for v in rooted_tables))
    candidates, rejected = reader.vtable_candidates() if discover else ([], 0)
    candidates = sorted({v['address']: v for v in candidates + rooted_tables}.values(), key=lambda v: v['address'])
    accepted_candidates, invalid_candidates = [], []
    descriptors = dict(explicit)
    for candidate in candidates:
        try:
            descendants = reader.closure([candidate['descriptor']])
        except ValueError as error:
            invalid_candidates.append({'vtable': candidate['address'], 'error': str(error)})
            continue
        accepted_candidates.append(candidate)
        descriptors.update(descendants)
        if len(descriptors) > MAX_TYPES:
            raise ValueError('combined type catalog exceeds explicit limit')
    names = sorted({d['name_bytes'] for d in descriptors.values()})
    ids = {name: i + 1 for i, name in enumerate(names)}
    ordered = []
    for address, source in sorted(descriptors.items()):
        record = dict(source, identity=ids[source['name_bytes']])
        record['bases'] = []
        for base in source['bases']:
            item = dict(base, identity=ids[descriptors[base['descriptor']]['name_bytes']])
            item['sources'] = [dict(s, identity=ids[descriptors[s['descriptor']]['name_bytes']]) for s in base['sources']]
            record['bases'].append(item)
        ordered.append(record)
    # Equal names establish type identity, but sharing an entire class view also
    # requires equal ordered lookup behavior. Never merge different base lists
    # merely because their class names match.
    def shape(record):
        return (record['identity'], tuple(
            (b['identity'], b['offset'], b['source_restricted'],
             tuple((s['identity'], s['offset']) for s in b['sources']))
            for b in record['bases']))
    shapes = sorted({shape(d) for d in ordered})
    class_indices = {value: i for i, value in enumerate(shapes)}
    for record in ordered:
        record['class_index'] = class_indices[shape(record)]
    # Only accepted metadata contributes to these spans. Structural probes may
    # read unrelated data before they are rejected.
    accepted = Reader(raw)
    accepted.closure(descriptors)
    spans = []
    section_end = 0
    for address, size in sorted(accepted.reads):
        if spans and address <= spans[-1][1] and address + size <= section_end:
            spans[-1][1] = max(spans[-1][1], address + size)
        else:
            spans.append([address, address + size])
            section_end = next(s['address'] + s['size'] for s in accepted.image.sections
                               if s['kind'] == 'data' and s['address'] <= address < s['address'] + s['size'])
    metadata = [{'address': begin, 'size': end - begin,
                 'sha256': hashlib.sha256(accepted.data(begin, end - begin)).hexdigest()}
                for begin, end in spans]
    base_count = sum(len(d['bases']) for d in ordered)
    source_count = sum(len(b['sources']) for d in ordered for b in d['bases'])
    shared_bases = sum(len(s[1]) for s in shapes)
    shared_sources = sum(len(b[3]) for s in shapes for b in s[1])
    return {'schema': 1, 'dol_sha256': hashlib.sha256(raw).hexdigest(),
            'explicit_roots': sorted(set(explicit_roots)), 'explicit_vtable_roots': sorted(set(explicit_vtables)),
            'explicit_closure_count': len(explicit),
            'descriptors': ordered, 'identities': len(ids),
            'candidate_vtables': accepted_candidates, 'invalid_candidate_graphs': invalid_candidates,
            'nonmatching_structural_windows': rejected,
            'counts': {'descriptors': len(ordered), 'base_entries': base_count, 'source_entries': source_count},
            'uncompressed_native_array_bytes': len(ordered) * 24 + base_count * 40 + source_count * 16,
            'shared_counts': {'classes': len(shapes), 'base_entries': shared_bases, 'source_entries': shared_sources},
            'shared_native_array_bytes': len(shapes) * 24 + shared_bases * 40 + shared_sources * 16,
            'array_size_scope': 'x64 ClassInfo/BaseSubobject/SourceSubobject arrays only (24/40/16 bytes). Excludes binding tables, alignment and all other executable content; not an EXE size result.',
            'metadata_spans': metadata,
            'limits': {'name_bytes': MAX_NAME, 'bases_per_descriptor': MAX_BASES, 'types': MAX_TYPES},
            'scope': 'Deterministic metadata decoding and collision-free name identities. Explicit roots are caller-supplied; virtual tables are structural candidates. Not an immutability, complete reachability, mutation, caller ABI or executable retirement proof.'}


def recover_call_arguments(raw, functions_raw, jump_tables, cast_entry):
    """Use the existing conservative CFG solver, including overlapping labels.

    These are static argument candidates under the solver's EABI/SDA assumptions,
    not proof that a declared entry is the only possible route into a procedure.
    An unknown context remains unknown even if another context has constants.
    """
    import gx_site_inventory as flow
    image = Dol(raw)
    declarations = function_table(functions_raw, 'functions', image)
    sites = []
    for section in image.text:
        for pc in range(section['address'], section['address'] + section['size'], 4):
            word = image.word(pc)
            if word >> 26 not in (16, 18):
                continue
            edge = decode(word, pc)
            if edge['link'] and edge['target'] == cast_entry:
                sites.append(pc)
    cache, roots, calls = {}, set(), []
    entries = {d['address'] for d in declarations}
    for site in sites:
        contexts = []
        for decl in declarations:
            begin, end = decl['address'], decl['address'] + decl['size']
            if not begin <= site < end:
                continue
            key = begin, end
            if key not in cache:
                # An overlapping declaration inside this extent may be a local
                # branch label. Analyze it as part of this context, then analyze
                # its independently declared entry as a separate context too.
                external_entries = frozenset(e for e in entries if not begin <= e < end)
                words, states, _, _, _ = flow.analyse_procedure(
                    image.word, lambda pc: begin <= pc < end and image.contains_text(pc, 4),
                    begin, jump_tables, entries=external_entries)
                unresolved = [pc for pc, word in words.items()
                              if (edge := decode(word, pc)) and edge.get('target_source') == 'CTR'
                              and not edge['link'] and pc not in jump_tables]
                cache[key] = states, sorted(unresolved)
            states, unresolved = cache[key]
            values = states.get(site)
            registers = {f'r{r}': values[0][r][1] if values and flow.known(values[0][r]) else None
                         for r in (4, 5, 6, 7)}
            contexts.append({'entry': begin, 'end_exclusive': end, 'reachable': values is not None,
                             'registers': registers, 'unresolved_control_sites': unresolved})
            roots.update(registers[r] for r in ('r5', 'r6') if registers[r])
        agreed = {}
        for r in ('r4', 'r5', 'r6', 'r7'):
            values = {c['registers'][r] for c in contexts}
            agreed[r] = next(iter(values)) if len(values) == 1 else None
        calls.append({'site': site, 'word': image.word(site), 'contexts': contexts, 'arguments': agreed})
    return {'entry': cast_entry, 'calls': calls, 'candidate_roots': sorted(roots),
            'scope': 'Declared-entry CFG, known-bits merges and EABI call clobbers. r2/r13 use the shared analyzer SDA assumptions. Unknown entries and indirect branches remain reported. Not a complete executable-entry or metadata-ownership proof.'}


def compile_queries(catalog, target_identities):
    """Specialize finite, source-independent queries; refuse lossy compression.

    This compiles metadata semantics, not object addresses. A production owner
    must separately prove its complete target/dynamic-type domain and lifetime.
    """
    classes, representatives = {}, {}
    for record in catalog['descriptors']:
        classes.setdefault(record['class_index'], record)
        representatives.setdefault(record['identity'], record['address'])
    identities = [record['identity'] for record in classes.values()]
    if len(identities) != len(set(identities)):
        raise ValueError('query compilation requires one class view per identity')
    if any(b['source_restricted'] for d in classes.values() for b in d['bases']):
        raise ValueError('source-restricted classes require a source-sensitive query representation')
    result = []
    for target in sorted(set(target_identities)):
        if target == 0 or target not in representatives:
            raise ValueError('compiled query target must name a catalog class')
        matches = []
        for record in sorted(classes.values(), key=lambda d: d['identity']):
            offset = 0 if record['identity'] == target else next(
                (b['offset'] for b in record['bases'] if b['identity'] == target), None)
            if offset is None:
                continue
            if not 0 < record['identity'] <= 0xffff or not -0x8000 <= offset <= 0x7fff:
                raise ValueError('compiled query exceeds compact identity/offset bounds')
            matches.append({'dynamic_identity': record['identity'], 'offset': offset})
        result.append({'target_identity': target, 'target_descriptor': representatives[target], 'matches': matches})
    return {'plans': result, 'matches': sum(len(plan['matches']) for plan in result),
            'match_bytes': sum(len(plan['matches']) for plan in result) * 4,
            'scope': 'Finite source-independent type queries; exact identity and signed-offset bounds checked. Excludes native-owner bindings and plan headers; no executable size or domain-completeness claim.'}


def native_query_header(catalog):
    """Own finite class queries as immutable native values, without image aliases.

    Provenance stays in the catalog/report. Neither original addresses nor
    descriptor pointers occur in this generated runtime-facing data owner.
    """
    compiled = catalog.get('compiled_queries')
    if compiled is not None:
        targets = [plan['target_identity'] for plan in compiled['plans']]
        if compiled != compile_queries(catalog, targets):
            raise ValueError('compiled queries disagree with catalog semantics')
    else:
        compiled = {'plans': []}
    matches, ranges, names = [], [], []
    for ordinal, plan in enumerate(compiled['plans']):
        begin, count = len(matches), len(plan['matches'])
        if ordinal > 0xffff or begin > 0xffff or count > 0xffff:
            raise ValueError('native query owner exceeds compact range bounds')
        names.append(f"    type_{plan['target_identity']} = {ordinal},")
        matches.extend(f"{{{m['dynamic_identity']}, {m['offset']}}}" for m in plan['matches'])
        ranges.append(f'{{{begin}, {count}}}')
    def array(type_name, name, items):
        return f'inline constexpr std::array<{type_name}, {len(items)}> {name}{{{{\n' + ',\n'.join(items) + '\n}};\n'
    output = '// Generated native class-query data; image provenance is recorded separately.\n#pragma once\n'
    output += '#include "object/type_cast.h"\n#include <array>\n\nnamespace shadow::object::game_type_queries {\n'
    output += 'enum class Query : std::uint16_t {\n' + '\n'.join(names) + '\n};\n'
    output += 'namespace detail {\nstruct QueryRange { std::uint16_t begin; std::uint16_t count; };\n'
    output += 'static_assert(sizeof(QueryRange) == 4 && sizeof(CompiledCastMatch) == 4);\n'
    output += array('CompiledCastMatch', 'matches', matches)
    output += array('QueryRange', 'ranges', ranges)
    output += '}  // namespace detail\n'
    output += 'inline constexpr std::size_t query_count = detail::ranges.size();\n'
    output += 'inline constexpr std::size_t data_bytes = sizeof(detail::matches) + sizeof(detail::ranges);\n'
    output += '''inline CastResult find_subobject(TypeId dynamic_type, Query query) noexcept {
    const auto index = static_cast<std::size_t>(query);
    if (index >= detail::ranges.size()) return {};
    const auto range = detail::ranges[index];
    return find_compiled_subobject(dynamic_type, detail::matches.data() + range.begin, range.count);
}
}  // namespace shadow::object::game_type_queries
'''
    return output


def query_binding_layout(catalog):
    """Recognize the exact descriptor domain with only query-relevant identities.

    A known class absent from every compiled match has the same failure outcome
    for every compiled query. Its concrete identity need not occupy a binding
    record, but its domain membership must remain distinct from an unknown input.
    This is disposable image binding data, not native object ownership.
    """
    native_query_header(catalog)  # Revalidate all compiled query semantics/bounds.
    queried = {m['dynamic_identity'] for plan in catalog.get('compiled_queries', {}).get('plans', [])
               for m in plan['matches']}
    groups, addresses = {}, set()
    for descriptor in sorted(catalog['descriptors'], key=lambda d: d['address']):
        address, identity = descriptor['address'], descriptor['identity']
        if not 0 < address <= 0xffffffff or address & 3 or address in addresses:
            raise ValueError('temporary type binding needs unique aligned 32-bit addresses')
        if not 0 < identity <= 0xffff:
            raise ValueError('temporary type binding exceeds identity bounds')
        addresses.add(address)
        groups.setdefault(address >> 16, []).append((address & 0xffff, identity))
    bindings, domain, buckets = [], [], []
    for prefix, records in sorted(groups.items()):
        first, last = records[0][0], records[-1][0]
        slots = (last - first) // 4 + 1
        bits = bytearray((slots + 7) // 8)
        bucket = {'prefix': prefix, 'first': first, 'slots': slots,
                  'domain_begin': len(domain), 'begin': len(bindings)}
        for offset, identity in records:
            slot = (offset - first) // 4
            bits[slot // 8] |= 1 << (slot % 8)
            if identity in queried:
                bindings.append({'offset': offset, 'identity': identity})
        bucket['end'] = len(bindings)
        buckets.append(bucket)
        domain.extend(bits)
    return {'bindings': bindings, 'domain': domain, 'buckets': buckets,
            'descriptor_count': len(addresses), 'queried_class_count': len(queried),
            'data_payload_bytes': len(bindings) * 4 + len(domain)}


def call_boundary_header(catalog):
    """Disposable caller ABI for the pinned game; native query data stays separate."""
    if catalog['dol_sha256'] != 'fde4fa6f81a60313b710161c196dc51c2260be62251ee02775d5eee06f9d55af':
        raise ValueError('type-call boundary requires the verified original game image')
    layout = query_binding_layout(catalog)
    bindings = [f"{{{b['offset']}, {b['identity']}}}" for b in layout['bindings']]
    output = """// SCAFFOLDING (R2): translated-object type bindings and exception ABI,
// deleted by native game-object ownership and native exception callers.
// This header is NOT native object ownership or a metadata-immutability proof.
#pragma once
#include "gekko/cpu.h"
#include "native_type_queries.h"
#include <array>

namespace gekko::type_query_boundary {
enum class Outcome { Returned, Throw, Fault };
namespace detail {
struct Binding { std::uint16_t offset; std::uint16_t identity; };
static_assert(sizeof(Binding) == 4);
// All compiled identities fit 16 bits. This value means a known class that
// cannot match any compiled query; zero continues to mean an unknown descriptor.
inline constexpr shadow::object::TypeId kNoCompiledMatch{0x10000u};
"""
    output += f'inline constexpr std::array<Binding, {len(bindings)}> bindings{{{{\n'
    output += ',\n'.join(bindings) + '\n}};\n'
    output += f"inline constexpr std::array<std::uint8_t, {len(layout['domain'])}> domain{{{{\n"
    output += ',\n'.join(', '.join(str(b) for b in layout['domain'][i:i+32])
                        for i in range(0, len(layout['domain']), 32)) + '\n}};\n'
    output += 'inline shadow::object::TypeId identity(u32 descriptor) noexcept {\n'
    output += '    if (descriptor & 3u) return 0;\n'
    output += '    std::size_t begin = 0, end = 0, domain_begin = 0;\n'
    output += '    u32 first = 0, slots = 0;\n    switch (descriptor >> 16) {\n'
    for b in layout['buckets']:
        output += (f"    case 0x{b['prefix']:04X}u: begin = {b['begin']}; end = {b['end']}; "
                   f"first = {b['first']}; slots = {b['slots']}; domain_begin = {b['domain_begin']}; break;\n")
    output += """    default: return 0;
    }
    const auto offset = descriptor & 0xffffu;
    if (offset < first) return 0;
    const auto slot = (offset - first) >> 2;
    if (slot >= slots || !(domain[domain_begin + slot / 8u] & (1u << (slot & 7u)))) return 0;
    const auto limit = end;
    while (begin != end) {
        const auto middle = begin + (end - begin) / 2;
        if (bindings[middle].offset < offset) begin = middle + 1;
        else end = middle;
    }
    return begin < limit && bindings[begin].offset == offset ? bindings[begin].identity : kNoCompiledMatch;
}
}  // namespace detail
inline constexpr std::size_t binding_bytes = sizeof(detail::bindings) + sizeof(detail::domain);

inline Outcome enter_reference_failure(Cpu& c, u32 flag) {
    const u32 stack = c.gpr[1], link = c.lr;
    c.mem_write32(stack - 32u, stack);
    c.mem_write32(stack + 4u, link);
    c.mem_write32(stack - 4u, c.gpr[31]);
    c.mem_write32(stack - 24u, 0x80568DD4u);
    if (c.memory_fault().latched) return Outcome::Fault;
    c.gpr[1] = stack - 32u;
    c.gpr[0] = 0x80568DD4u;
    c.gpr[3] = 0x80514CC8u;
    c.gpr[4] = stack - 24u;
    c.gpr[5] = 0x803A1D60u;
    const auto signed_flag = static_cast<s16>(flag);
    const u32 condition = (signed_flag < 0 ? 8u : signed_flag > 0 ? 4u : 2u) |
                          ((c.xer & 0x80000000u) ? 1u : 0u);
    c.cr = (c.cr & 0x0FFFFFFFu) | (condition << 28);
    c.lr = 0x803A1D48u;
    c.pc = 0x803A2660u;
    return Outcome::Throw;
}

// No per-object cache: the current virtual table is read on every call, so
// constructor/destructor phase changes remain visible. The catalog's static
// metadata domain/lifetime must be established before production activation.
inline Outcome prepare(Cpu& c, shadow::object::game_type_queries::Query query, u32 expected_target) {
    const u32 object = c.gpr[3], flag = c.gpr[7];
    const auto returned = [&](u32 value) {
        c.gpr[3] = value;
        c.pc = c.lr;
        return Outcome::Returned;
    };
    if (!object) return returned(0);
    if (c.gpr[5] != expected_target) {
        c.request_stop(StopReason::ExecutionFault, "native type-query target contract changed");
        return Outcome::Fault;
    }
    const u32 table = c.mem_read32(object + c.gpr[4]);
    if (c.memory_fault().latched) return Outcome::Fault;
    const u32 descriptor = c.mem_read32(table);
    if (c.memory_fault().latched) return Outcome::Fault;
    if (descriptor) {
        const u32 adjustment = c.mem_read32(table + 4u);
        if (c.memory_fault().latched) return Outcome::Fault;
        const u32 complete_object = object + adjustment;
        if (!expected_target) return returned(complete_object);
        const auto identity = detail::identity(descriptor);
        if (!identity || static_cast<std::size_t>(query) >= shadow::object::game_type_queries::query_count) {
            c.request_stop(StopReason::ExecutionFault, "native type-query binding is not in the validated domain");
            return Outcome::Fault;
        }
        if (identity != detail::kNoCompiledMatch) {
            const auto result = shadow::object::game_type_queries::find_subobject(identity, query);
            if (result.found) return returned(complete_object + static_cast<u32>(result.offset));
        }
    }
    return (flag & 0xffffu) ? enter_reference_failure(c, flag) : returned(0);
}
}  // namespace gekko::type_query_boundary
"""
    return output


def oracle_header(catalog):
    """Emit immutable host-pointer arrays for the independent original-code test.

    Address aliases are test inputs only; this is not a production binding layer.
    """
    classes = {}
    for descriptor in catalog['descriptors']:
        classes.setdefault(descriptor['class_index'], descriptor)
    sources, bases, infos = [], [], []
    for _, descriptor in sorted(classes.items()):
        base_begin = len(bases)
        for base in descriptor['bases']:
            source_begin = len(sources)
            sources.extend(f"{{{s['identity']}u, {s['offset']}}}" for s in base['sources'])
            pointer = f'sources.data() + {source_begin}' if base['sources'] else 'nullptr'
            bases.append(f"{{{base['identity']}u, {base['offset']}, {pointer}, "
                         f"{len(base['sources'])}, {'true' if base['source_restricted'] else 'false'}}}")
        pointer = f'bases.data() + {base_begin}' if descriptor['bases'] else 'nullptr'
        infos.append(f"{{{descriptor['identity']}u, {pointer}, {len(descriptor['bases'])}}}")
    def array(type_name, name, items):
        return f'inline constexpr std::array<{type_name}, {len(items)}> {name}{{{{\n' + ',\n'.join(items) + '\n}};\n'
    output = '// Generated by tools/lift/type_catalog.py; original-code test fixture only.\n'
    output += '// DOL SHA-256: ' + catalog['dol_sha256'] + '\n#include <array>\n#include <cstdint>\n'
    output += '#include "native_type_queries.h"\n'
    output += 'namespace type_cast_catalog_fixture {\n'
    output += array('shadow::object::SourceSubobject', 'sources', sources)
    output += array('shadow::object::BaseSubobject', 'bases', bases)
    output += array('shadow::object::ClassInfo', 'classes', infos)
    output += 'struct Descriptor { std::uint32_t address; std::size_t class_index; };\n'
    output += array('Descriptor', 'descriptors',
                    [f"{{0x{d['address']:08X}u, {d['class_index']}}}" for d in catalog['descriptors']])
    output += 'struct Vtable { std::uint32_t address; std::uint32_t descriptor; std::int32_t adjustment; };\n'
    output += array('Vtable', 'vtables',
                    [f"{{0x{v['address']:08X}u, 0x{v['descriptor']:08X}u, {v['adjustment']}}}"
                     for v in catalog['candidate_vtables']])
    output += 'struct MetadataSpan { std::uint32_t address; std::size_t size; };\n'
    output += array('MetadataSpan', 'metadata_spans',
                    [f"{{0x{s['address']:08X}u, {s['size']}}}" for s in catalog['metadata_spans']])
    queries = [f"{{0x{plan['target_descriptor']:08X}u, shadow::object::game_type_queries::Query::type_{plan['target_identity']}}}"
               for plan in catalog.get('compiled_queries', {}).get('plans', [])]
    output += 'struct Query { std::uint32_t target_descriptor; shadow::object::game_type_queries::Query native_query; };\n'
    output += array('Query', 'queries', queries)
    return output + '}\n'


def audit_memory(catalog, read):
    """Check all catalog inputs at one instant; never infer lifetime immutability."""
    changed, missing = [], []
    for span in catalog['metadata_spans']:
        value = read(span['address'], span['size'])
        if value is None or len(value) != span['size']:
            missing.append({'address': span['address'], 'size': span['size']})
        elif (digest := hashlib.sha256(value).hexdigest()) != span['sha256']:
            changed.append(dict(span, actual_sha256=digest))
    changed_vtables, missing_vtables = [], []
    for table in catalog['candidate_vtables']:
        value = read(table['address'], 12)
        if value is None or len(value) != 12:
            missing_vtables.append(table['address'])
        elif value != struct.pack('>III', table['descriptor'], table['adjustment'] & 0xffffffff, table['first_method']):
            changed_vtables.append({'address': table['address'], 'actual_words': list(struct.unpack('>III', value))})
    return {'unchanged': not (changed or missing or changed_vtables or missing_vtables),
            'metadata_spans_checked': len(catalog['metadata_spans']),
            'metadata_bytes_checked': sum(s['size'] for s in catalog['metadata_spans']),
            'vtables_checked': len(catalog['candidate_vtables']), 'changed_metadata_spans': changed,
            'missing_metadata_spans': missing, 'changed_vtables': changed_vtables, 'missing_vtables': missing_vtables,
            'scope': 'All catalog input spans and candidate vtable headers at this instant. Not proof against transient writes, other game paths, object-vptr changes or dynamically generated metadata.'}


def audit_snapshot(catalog, path):
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'comparator'))
    from comparator.statefile import GuestImage
    snapshot = GuestImage(path)
    result = audit_memory(catalog, snapshot.read)
    result['snapshot'] = {'path': str(path.resolve()), 'bytes': len(snapshot.data),
                          'sha256': hashlib.sha256(snapshot.data).hexdigest(), 'version': snapshot.version,
                          'game_ticks': snapshot.game_ticks, 'renderer_kind': snapshot.renderer_kind}
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dol', type=Path, required=True)
    parser.add_argument('--descriptor', type=lambda value: int(value, 0), action='append', default=[])
    parser.add_argument('--roots-json', type=Path, help='JSON array of explicit descriptor addresses')
    parser.add_argument('--discover-vtables', action='store_true')
    parser.add_argument('--vtable', type=lambda value: int(value, 0), action='append', default=[],
                        help='Explicit metadata-header candidate; does not require a nonnull first method')
    parser.add_argument('--discover-stores', action='store_true',
                        help='Expand the fixture catalog using constant stored headers, with ownership limitations retained')
    parser.add_argument('--metadata-uses-out', type=Path, help='Store the full ownership-use evidence for --discover-stores')
    parser.add_argument('--functions', type=Path)
    parser.add_argument('--jump-tables', type=Path)
    parser.add_argument('--cast-entry', type=lambda value: int(value, 0))
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--call-boundary-header', type=Path, help='Disposable pinned-game caller boundary; not proof of production ownership')
    parser.add_argument('--native-query-header', type=Path, help='Immutable native queries without image addresses; defaults beside an oracle header')
    parser.add_argument('--oracle-header', type=Path, help='Generated C++ fixture for the original-code oracle; not production data')
    parser.add_argument('--snapshot', type=Path, action='append', default=[], help='Observe catalog bytes in a saved state; not lifetime immutability proof')
    args = parser.parse_args()
    if any((args.functions, args.jump_tables, args.cast_entry)) and not all((args.functions, args.jump_tables, args.cast_entry)):
        parser.error('--functions, --jump-tables and --cast-entry must be supplied together')
    if args.discover_stores and not args.functions:
        parser.error('--discover-stores requires --functions, --jump-tables and --cast-entry')
    if args.metadata_uses_out and not args.discover_stores:
        parser.error('--metadata-uses-out requires --discover-stores')
    try:
        raw = args.dol.read_bytes()
        roots = args.descriptor
        if args.roots_json:
            supplied = json.loads(args.roots_json.read_text())
            if not isinstance(supplied, list):
                raise ValueError('roots JSON must be an array')
            roots += [int(x, 0) if isinstance(x, str) else x for x in supplied]
        call_arguments = None
        if args.functions:
            import gx_site_inventory as flow
            call_arguments = recover_call_arguments(raw, args.functions.read_bytes(),
                                                    flow.read_jump_tables(args.jump_tables, flow.Image(raw)), args.cast_entry)
        if not roots and not args.vtable and call_arguments is None:
            raise ValueError('at least one explicit descriptor root or a caller analysis is required')
        all_roots = roots + (call_arguments['candidate_roots'] if call_arguments else [])
        catalog = build_catalog(raw, all_roots, args.discover_vtables, args.vtable)
        metadata_uses = None
        if args.discover_stores:
            import type_metadata_uses
            metadata_uses = type_metadata_uses.audit(
                raw, args.functions.read_bytes(), flow.read_jump_tables(args.jump_tables, flow.Image(raw)), catalog,
                lambda n, total: print(f'ownership contexts {n}/{total}', flush=True), include_consumers=False)
            stored_headers = [v['address'] for v in metadata_uses['stored_header_candidates']]
            # Overapproximate metadata semantics for the oracle. A literal store
            # is not by itself an object ownership or lifetime proof.
            catalog = build_catalog(raw, all_roots, args.discover_vtables, args.vtable + stored_headers)
            catalog['stored_header_expansion'] = {
                'header_candidates': len(stored_headers), 'new_descriptor_roots': metadata_uses['new_descriptor_roots'],
                'immutability_proven': False, 'complete_dynamic_domain_proven': False}
        catalog['caller_supplied_roots'] = sorted(set(roots))
        catalog['call_arguments'] = call_arguments
        if call_arguments:
            descriptors = {d['address']: d['identity'] for d in catalog['descriptors']}
            targets = {descriptors[ctx['registers']['r5']] for call in call_arguments['calls']
                       for ctx in call['contexts'] if ctx['registers']['r5']}
            catalog['compiled_queries'] = compile_queries(catalog, targets)
        native_header_path = args.native_query_header or (args.oracle_header.with_name('native_type_queries.h') if args.oracle_header else None)
        native_header_text = native_query_header(catalog) if native_header_path else None
        boundary_header_text = call_boundary_header(catalog) if args.call_boundary_header else None
        catalog['inputs'] = {key: {'path': str(path.resolve()), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}
                             for key, path in (('dol', args.dol), ('functions', args.functions), ('jump_tables', args.jump_tables)) if path}
        if metadata_uses is not None:
            metadata_uses['inputs'] = dict(catalog['inputs'])
            for key, path in (('tool', Path(type_metadata_uses.__file__)), ('catalog_tool', Path(__file__)),
                              ('flow_tool', Path(flow.__file__)), ('transfer_tool', Path(flow.transfer.__code__.co_filename))):
                metadata_uses['inputs'][key] = {'path': str(path.resolve()), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}
            if args.metadata_uses_out:
                args.metadata_uses_out.parent.mkdir(parents=True, exist_ok=True)
                args.metadata_uses_out.write_text(json.dumps(metadata_uses, indent=2) + '\n', encoding='utf-8')
        catalog['snapshot_observations'] = [audit_snapshot(catalog, path) for path in args.snapshot]
        if args.call_boundary_header:
            args.call_boundary_header.parent.mkdir(parents=True, exist_ok=True)
            args.call_boundary_header.write_text(boundary_header_text, encoding='utf-8')
        if native_header_path:
            native_header_path.parent.mkdir(parents=True, exist_ok=True)
            native_header_path.write_text(native_header_text, encoding='utf-8')
        if args.oracle_header:
            args.oracle_header.parent.mkdir(parents=True, exist_ok=True)
            args.oracle_header.write_text(oracle_header(catalog), encoding='utf-8')
    except (OSError, ValueError, struct.error) as error:
        parser.exit(1, f'type catalog refused: {error}\n')
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(catalog, indent=2) + '\n')
    print(json.dumps({'counts': catalog['counts'], 'identities': catalog['identities'],
                      'candidate_vtables': len(catalog['candidate_vtables']),
                      'uncompressed_native_array_bytes': catalog['uncompressed_native_array_bytes'],
                      'shared_counts': catalog['shared_counts'],
                      'shared_native_array_bytes': catalog['shared_native_array_bytes']}))
    if any(not observation['unchanged'] for observation in catalog['snapshot_observations']):
        parser.exit(1, 'type catalog: changed or missing snapshot metadata; details retained in report\n')


if __name__ == '__main__':
    main()
