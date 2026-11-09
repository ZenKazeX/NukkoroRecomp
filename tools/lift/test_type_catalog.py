"""Metadata ownership-input regressions, including hostile and ambiguous data."""
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import type_catalog as catalog

TEXT, DATA, CAST = 0x80001000, 0x80002000, 0x80001800


class Image:
    def __init__(self):
        self.raw = bytearray(0x100 + 0x1000 + 0x4000)
        self.cursor = DATA
        for index, offset, address, size in ((0, 0x100, TEXT, 0x1000), (7, 0x1100, DATA, 0x4000)):
            for field, value in ((0, offset), (0x48, address), (0x90, size)):
                struct.pack_into('>I', self.raw, field + index * 4, value)
        struct.pack_into('>I', self.raw, 0xe0, TEXT)
        self.words(TEXT, 0x4e800020)

    def offset(self, address):
        return 0x100 + address - TEXT if address < DATA else 0x1100 + address - DATA

    def reserve(self, size):
        address = self.cursor
        self.cursor += (size + 3) & ~3
        return address

    def words(self, address, *values):
        struct.pack_into(f'>{len(values)}I', self.raw, self.offset(address), *values)

    def descriptor(self, name=b'Type', bases=()):
        address, text = self.reserve(8), self.reserve(len(name) + 1)
        self.raw[self.offset(text):self.offset(text) + len(name) + 1] = name + b'\0'
        encoded = []
        for target, offset, sources in bases:
            encoded.extend((target, offset | (0x80000000 if sources is not None else 0)))
            if sources is not None:
                encoded.append(len(sources))
                for source, location in sources:
                    encoded.extend((source, location & 0xffffffff))
        base_list = self.reserve(4 * (len(encoded) + 1)) if bases else 0
        if bases:
            self.words(base_list, *encoded, 0)
        self.words(address, text, base_list)
        return address

    def vtable(self, descriptor, adjustment=0):
        address = self.reserve(12)
        self.words(address, descriptor, adjustment & 0xffffffff, TEXT)
        return address

    def build(self, roots, discover=False):
        return catalog.build_catalog(bytes(self.raw), roots, discover)


class CatalogTests(unittest.TestCase):
    def test_distinct_equal_names_share_identity_and_equal_classes(self):
        image = Image()
        first, second = image.descriptor(b'Base'), image.descriptor(b'Base')
        left = image.descriptor(b'Derived', [(first, 24, None)])
        right = image.descriptor(b'Derived', [(second, 24, None)])
        result = image.build([left, right])
        records = {r['address']: r for r in result['descriptors']}
        self.assertEqual(result['identities'], 2)
        self.assertEqual(result['shared_counts']['classes'], 2)
        self.assertEqual(records[left]['class_index'], records[right]['class_index'])
        self.assertEqual(records[first]['identity'], records[second]['identity'])

    def test_same_name_different_base_order_never_shares_class(self):
        image = Image()
        a, b = image.descriptor(b'A'), image.descriptor(b'B')
        left = image.descriptor(b'Derived', [(a, 12, None), (b, 24, None)])
        right = image.descriptor(b'Derived', [(b, 24, None), (a, 12, None)])
        result = image.build([left, right])
        records = {r['address']: r for r in result['descriptors']}
        self.assertEqual(records[left]['identity'], records[right]['identity'])
        self.assertNotEqual(records[left]['class_index'], records[right]['class_index'])

    def test_groups_keep_empty_restrictions_and_signed_source_offsets(self):
        image = Image()
        base = image.descriptor(b'Base')
        derived = image.descriptor(b'Derived', [(base, 32, []), (base, 64, [(base, -12)])])
        result = image.build([derived])
        record = next(d for d in result['descriptors'] if d['address'] == derived)
        self.assertEqual([b['source_restricted'] for b in record['bases']], [True, True])
        self.assertEqual(record['bases'][0]['sources'], [])
        self.assertEqual(record['bases'][1]['sources'][0]['offset'], -12)

    def test_non_ascii_and_embedded_prefixes_keep_exact_byte_identity(self):
        image = Image()
        roots = [image.descriptor(n) for n in (b'', b'A', b'AB', b'\xe9', b'\xc3\xa9')]
        result = image.build(roots)
        self.assertEqual(result['identities'], 5)
        self.assertEqual({d['name_bytes'] for d in result['descriptors']}, {'', '41', '4142', 'e9', 'c3a9'})

    def test_cycles_terminate(self):
        image = Image()
        first = image.descriptor(b'Cycle')
        second = image.descriptor(b'Cycle2', [(first, 0, None)])
        table = image.reserve(12)
        image.words(table, second, 0, 0)
        image.words(first + 4, table)
        self.assertEqual(image.build([first])['counts']['descriptors'], 2)

    def test_candidate_vtable_adjustment_is_signed(self):
        image = Image()
        descriptor = image.descriptor(b'Derived')
        vtable = image.vtable(descriptor, -1088)
        result = image.build([descriptor], True)
        self.assertEqual(result['candidate_vtables'], [dict(address=vtable, descriptor=descriptor,
                                                         adjustment=-1088, first_method=TEXT)])

    def test_explicit_header_with_null_first_method_is_not_dropped(self):
        image = Image()
        descriptor = image.descriptor(b'Abstract')
        vtable = image.vtable(descriptor, -24)
        image.words(vtable+8, 0)
        result = catalog.build_catalog(bytes(image.raw), [], True, [vtable, vtable])
        self.assertEqual(result['explicit_vtable_roots'], [vtable])
        self.assertEqual(result['counts']['descriptors'], 1)
        self.assertEqual(result['candidate_vtables'][0]['first_method'], 0)
        observed = catalog.audit_memory(result, lambda at, size: catalog.Reader(bytes(image.raw)).data(at, size))
        self.assertTrue(observed['unchanged'])

    def test_invalid_explicit_header_and_descendant_fail_closed(self):
        image = Image()
        bad = image.descriptor(b'Bad')
        header = image.vtable(bad)
        image.words(bad+4, DATA+0x4000)
        for address in (header, header+1, 0, True, 0x100000000):
            with self.subTest(address=address), self.assertRaises(ValueError):
                catalog.build_catalog(bytes(image.raw), [], True, [address])

    def test_invalid_candidate_does_not_hide_bad_explicit_root(self):
        image = Image()
        good, bad = image.descriptor(b'Good'), image.descriptor(b'Bad')
        image.words(bad + 4, DATA + 0x4000)
        image.vtable(bad)
        self.assertEqual(image.build([good], True)['candidate_vtables'], [])
        with self.assertRaisesRegex(ValueError, 'file-backed'):
            image.build([bad], True)

    def test_candidate_with_bad_descendant_is_reported(self):
        image = Image()
        good, bad = image.descriptor(b'Good'), image.descriptor(b'Bad')
        image.words(bad + 4, DATA + 0x4000)
        parent = image.descriptor(b'Parent', [(bad, 0, None)])
        vtable = image.vtable(parent)
        result = image.build([good], True)
        self.assertEqual(result['candidate_vtables'], [])
        self.assertEqual(result['invalid_candidate_graphs'][0]['vtable'], vtable)

    def test_unterminated_name_and_negative_count_fail_closed(self):
        image = Image()
        descriptor = image.descriptor(b'LongName')
        with patch.object(catalog, 'MAX_NAME', 3), self.assertRaisesRegex(ValueError, 'unterminated'):
            image.build([descriptor])
        derived = image.descriptor(b'Derived', [(descriptor, 0, [])])
        table = catalog.Reader(bytes(image.raw)).word(derived + 4)
        image.words(table + 8, 0xffffffff)
        with self.assertRaisesRegex(ValueError, 'negative'):
            image.build([derived])

    def test_unaligned_text_bss_and_wrapping_metadata_are_rejected(self):
        image = Image()
        for root in (DATA + 1, TEXT, 0x81000000, 0xfffffffc):
            with self.subTest(root=root), self.assertRaises(ValueError):
                image.build([root])
        with self.assertRaisesRegex(ValueError, 'wraps'):
            catalog.Reader(bytes(image.raw)).data(0xfffffffe, 4)

    def test_invalid_root_types_are_rejected(self):
        image = Image()
        for root in (0, -4, True, 1.5, [], 0x100000000):
            with self.subTest(root=root), self.assertRaisesRegex(ValueError, '32-bit'):
                image.build([root])

    def test_combined_limit_applies_across_independent_candidates(self):
        image = Image()
        first, second = image.descriptor(b'First'), image.descriptor(b'Second')
        image.vtable(second)
        with patch.object(catalog, 'MAX_TYPES', 1), self.assertRaisesRegex(ValueError, 'combined'):
            image.build([first], True)

    def test_input_order_cannot_change_ids_or_header(self):
        image = Image()
        roots = [image.descriptor(n) for n in (b'Z', b'A', b'B')]
        a, b = image.build(roots), image.build(reversed(roots))
        self.assertEqual(a, b)
        self.assertEqual(catalog.oracle_header(a), catalog.oracle_header(b))

    def test_spans_include_accepted_dependencies_without_probe_noise(self):
        image = Image()
        good, bad = image.descriptor(b'Good'), image.descriptor(b'Bad')
        image.words(bad + 4, DATA + 0x4000)
        image.vtable(bad)
        plain, discovery = image.build([good]), image.build([good], True)
        self.assertEqual(plain['metadata_spans'], discovery['metadata_spans'])
        self.assertEqual(sum(s['size'] for s in plain['metadata_spans']), 8 + len(b'Good\0'))

    def test_adjacent_sections_keep_separate_fingerprint_spans(self):
        image = Image()
        struct.pack_into('>I', image.raw, 0x90 + 7*4, 0x2000)
        for field, value in ((0, 0x3100), (0x48, DATA+0x2000), (0x90, 0x2000)):
            struct.pack_into('>I', image.raw, field+8*4, value)
        image.words(DATA+0x1ff8, DATA+0x2000, 0)
        image.raw[image.offset(DATA+0x2000)] = ord('A')
        spans = image.build([DATA+0x1ff8])['metadata_spans']
        self.assertEqual([(s['address'], s['size']) for s in spans], [(DATA+0x1ff8, 8), (DATA+0x2000, 2)])

    def test_cli_invalid_json_preserves_previous_report(self):
        image = Image()
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            (tmp/'image.dol').write_bytes(image.raw)
            (tmp/'roots.json').write_text('{}')
            output = tmp/'report.json'
            output.write_text('previous evidence')
            run = subprocess.run([sys.executable, '-B', str(Path(catalog.__file__)), '--dol', str(tmp/'image.dol'),
                                  '--roots-json', str(tmp/'roots.json'), '--out', str(output)],
                                 capture_output=True, text=True)
            self.assertNotEqual(run.returncode, 0)
            self.assertIn('array', run.stderr)
            self.assertEqual(output.read_text(), 'previous evidence')

    def test_snapshot_changes_and_missing_bytes_are_never_excused(self):
        image = Image()
        descriptor = image.descriptor(b'Observed')
        vtable = image.vtable(descriptor, -32)
        result = image.build([descriptor], True)
        read = lambda address, size: bytes(image.raw[image.offset(address):image.offset(address)+size])
        self.assertTrue(catalog.audit_memory(result, read)['unchanged'])
        image.words(vtable+4, 0)
        image.words(descriptor+4, DATA+0x100)
        changed = catalog.audit_memory(result, read)
        self.assertFalse(changed['unchanged'])
        self.assertEqual(len(changed['changed_metadata_spans']), 1)
        self.assertEqual(len(changed['changed_vtables']), 1)
        missing = catalog.audit_memory(result, lambda address, size: None)
        self.assertFalse(missing['unchanged'])
        self.assertEqual(len(missing['missing_metadata_spans']), len(result['metadata_spans']))
        self.assertEqual(missing['missing_vtables'], [vtable])

    def test_compiled_query_preserves_first_matching_base_and_exact_aliases(self):
        image = Image()
        base = image.descriptor(b'Base')
        first = image.descriptor(b'Derived', [(base, 12, None), (base, 24, None)])
        alias = image.descriptor(b'Derived', [(base, 12, None), (base, 24, None)])
        result = image.build([first, alias])
        ids = {d['address']: d['identity'] for d in result['descriptors']}
        compiled = catalog.compile_queries(result, [ids[base], ids[base]])
        self.assertEqual(len(compiled['plans']), 1)
        self.assertEqual(compiled['matches'], 2)
        self.assertEqual(compiled['match_bytes'], 8)
        self.assertEqual(compiled['plans'][0]['matches'], [
            {'dynamic_identity': ids[base], 'offset': 0}, {'dynamic_identity': ids[first], 'offset': 12}])

    def test_compiled_query_refuses_source_restrictions_and_ambiguous_views(self):
        image = Image()
        base = image.descriptor(b'Base')
        restricted = image.descriptor(b'Derived', [(base, 12, [])])
        result = image.build([restricted])
        with self.assertRaisesRegex(ValueError, 'source-sensitive'):
            catalog.compile_queries(result, [result['descriptors'][0]['identity']])
        a = image.descriptor(b'Variant', [(base, 12, None)])
        b = image.descriptor(b'Variant', [(base, 24, None)])
        result = image.build([a, b])
        with self.assertRaisesRegex(ValueError, 'one class view'):
            catalog.compile_queries(result, [result['descriptors'][0]['identity']])

    def test_compiled_query_refuses_overflow_and_missing_targets(self):
        image = Image()
        base = image.descriptor(b'Base')
        wide = image.descriptor(b'Derived', [(base, 32768, None)])
        result = image.build([wide])
        target = next(d['identity'] for d in result['descriptors'] if d['address'] == base)
        with self.assertRaisesRegex(ValueError, 'bounds'):
            catalog.compile_queries(result, [target])
        for target in (0, 70000):
            with self.subTest(target=target), self.assertRaisesRegex(ValueError, 'target'):
                catalog.compile_queries(result, [target])
        result = image.build([base])
        result['descriptors'][0]['identity'] = 65537
        with self.assertRaisesRegex(ValueError, 'bounds'):
            catalog.compile_queries(result, [65537])

    def test_compiled_query_order_and_header_are_deterministic(self):
        image = Image()
        roots = [image.descriptor(name) for name in (b'Z', b'A', b'B')]
        result = image.build(roots)
        a = catalog.compile_queries(result, [3, 1, 2])
        b = catalog.compile_queries(result, [2, 3, 1, 1])
        self.assertEqual(a, b)
        result['compiled_queries'] = a
        header = catalog.oracle_header(result)
        self.assertIn('std::array<Query, 3>', header)
        self.assertNotIn('compiled_matches', header)
        native = catalog.native_query_header(result)
        self.assertIn('std::array<CompiledCastMatch, 3>', native)
        self.assertIn('std::array<QueryRange, 3>', native)
        self.assertNotIn('target_descriptor', native)
        self.assertEqual(native, catalog.native_query_header(result))

    def test_call_boundary_refuses_unverified_exception_image(self):
        image = Image()
        root = image.descriptor(b'Type')
        with self.assertRaisesRegex(ValueError, 'verified original game image'):
            catalog.call_boundary_header(image.build([root]))

    def test_binding_layout_keeps_query_aliases_and_the_complete_domain(self):
        image = Image()
        base = image.descriptor(b'Base')
        derived = image.descriptor(b'Derived', [(base, 24, None)])
        alias = image.descriptor(b'Derived', [(base, 24, None)])
        unused = image.descriptor(b'Unused')
        result = image.build([derived, alias, unused])
        target = next(d['identity'] for d in result['descriptors'] if d['address'] == base)
        result['compiled_queries'] = catalog.compile_queries(result, [target])
        layout = catalog.query_binding_layout(result)
        mapped, recognized = set(), set()
        for b in layout['buckets']:
            mapped.update((b['prefix'] << 16) | r['offset']
                          for r in layout['bindings'][b['begin']:b['end']])
            for slot in range(b['slots']):
                if layout['domain'][b['domain_begin'] + slot//8] & (1 << (slot % 8)):
                    recognized.add((b['prefix'] << 16) | (b['first'] + slot*4))
        self.assertEqual(mapped, {base, derived, alias})
        self.assertEqual(recognized, {base, derived, alias, unused})
        self.assertEqual(layout['descriptor_count'], 4)
        self.assertEqual(layout['queried_class_count'], 2)
        self.assertEqual(layout['data_payload_bytes'], 12 + len(layout['domain']))

    def test_binding_layout_handles_prefix_edges_and_empty_queries(self):
        image = Image()
        result = image.build([image.descriptor(b'A'), image.descriptor(b'B')])
        result['descriptors'][0]['address'] = 0x9000fffc
        result['descriptors'][1]['address'] = 0x90010000
        result['compiled_queries'] = catalog.compile_queries(result, [])
        layout = catalog.query_binding_layout(result)
        self.assertEqual(layout['bindings'], [])
        self.assertEqual(layout['domain'], [1, 1])
        self.assertEqual([(b['prefix'], b['first'], b['slots'], b['domain_begin'])
                          for b in layout['buckets']], [(0x9000, 0xfffc, 1, 0), (0x9001, 0, 1, 1)])
        result['descriptors'].reverse()
        self.assertEqual(layout, catalog.query_binding_layout(result))

    def test_binding_layout_refuses_duplicate_unaligned_and_wide_addresses(self):
        image = Image()
        descriptor = image.descriptor(b'Type')
        for address in (0, descriptor + 1, 0x100000000):
            result = image.build([descriptor])
            result['descriptors'][0]['address'] = address
            with self.subTest(address=address), self.assertRaisesRegex(ValueError, 'unique aligned'):
                catalog.query_binding_layout(result)
        result = image.build([descriptor])
        result['descriptors'].append(dict(result['descriptors'][0]))
        with self.assertRaisesRegex(ValueError, 'unique aligned'):
            catalog.query_binding_layout(result)

    def test_binding_layout_keeps_maximum_identity_and_rejects_overflow(self):
        image = Image()
        root = image.descriptor(b'Type')
        result = image.build([root])
        result['descriptors'][0]['identity'] = 65535
        result['compiled_queries'] = catalog.compile_queries(result, [65535])
        layout = catalog.query_binding_layout(result)
        self.assertEqual(layout['bindings'], [{'offset': root & 0xffff, 'identity': 65535}])
        # Even a class absent from all compiled queries must fit the binding
        # identity domain; an overflowing identity cannot become a valid no-match.
        result['compiled_queries'] = catalog.compile_queries(result, [])
        result['descriptors'][0]['identity'] = 65536
        with self.assertRaisesRegex(ValueError, 'identity bounds'):
            catalog.query_binding_layout(result)

    def test_binding_layout_revalidates_compiled_match_semantics(self):
        image = Image()
        root = image.descriptor(b'Type')
        result = image.build([root])
        result['compiled_queries'] = catalog.compile_queries(result, [1])
        result['compiled_queries']['plans'][0]['matches'][0]['offset'] = 4
        with self.assertRaisesRegex(ValueError, 'disagree'):
            catalog.query_binding_layout(result)

    def test_native_query_owner_does_not_depend_on_image_placement(self):
        headers = []
        for padding in (0, 128):
            image = Image()
            image.reserve(padding)
            base = image.descriptor(b'Base')
            derived = image.descriptor(b'Derived', [(base, 56, None)])
            result = image.build([derived])
            result['compiled_queries'] = catalog.compile_queries(result, [1, 2])
            headers.append(catalog.native_query_header(result))
            for descriptor in result['descriptors']:
                self.assertNotIn(f"0x{descriptor['address']:08X}", headers[-1])
            self.assertNotIn(result['dol_sha256'], headers[-1])
        self.assertEqual(*headers)

    def test_native_query_owner_rejects_stale_semantics(self):
        image = Image()
        base = image.descriptor(b'Base')
        derived = image.descriptor(b'Derived', [(base, 56, None)])
        result = image.build([derived])
        result['compiled_queries'] = catalog.compile_queries(result, [1, 2])
        result['compiled_queries']['plans'][0]['matches'][0]['offset'] = 7
        with self.assertRaisesRegex(ValueError, 'disagree'):
            catalog.native_query_header(result)



def d_form(op, rd, ra, immediate):
    return op << 26 | rd << 21 | ra << 16 | (immediate & 0xffff)


def branch(pc, target, link=False):
    return 0x48000000 | (target - pc) & 0x03fffffc | int(link)


def conditional(pc, target):
    return 0x41820000 | (target - pc) & 0xfffc


class CallerTests(unittest.TestCase):
    def solve(self, words, declarations=None):
        image = Image()
        image.words(TEXT, *[w(TEXT + 4*i) if callable(w) else w for i, w in enumerate(words)])
        declarations = declarations or [(TEXT, len(words)*4)]
        functions = 'addr_hex,name,size\n' + ''.join(f'{a:#x},test_{i},{size}\n' for i,(a,size) in enumerate(declarations))
        return catalog.recover_call_arguments(bytes(image.raw), functions.encode(), {}, CAST)

    def test_overlapping_labels_retain_unknown_independent_entry(self):
        result = self.solve([d_form(14, 5, 0, 0x2000), d_form(14, 6, 0, 0x2000+8),
                             d_form(14, 4, 0, 0), d_form(14, 7, 0, 0),
                             lambda pc: branch(pc, CAST, True), 0x4e800020], [(TEXT, 24), (TEXT+8, 16)])
        self.assertEqual(result['candidate_roots'], [0x2000, 0x2000+8])
        self.assertIsNone(result['calls'][0]['arguments']['r5'])
        self.assertEqual(len(result['calls'][0]['contexts']), 2)

    def test_loop_and_callee_saved_base_survive_call(self):
        result = self.solve([d_form(14, 31, 0, 0x2000), lambda pc: branch(pc, CAST+16, True),
                             d_form(14, 5, 31, 8), d_form(14, 6, 31, 0),
                             lambda pc: branch(pc, CAST, True), lambda pc: conditional(pc, TEXT+4), 0x4e800020])
        self.assertEqual(result['calls'][0]['arguments']['r5'], 0x2000+8)
        self.assertEqual(result['calls'][0]['arguments']['r6'], 0x2000)

    def test_volatile_base_and_conflicting_paths_are_unknown(self):
        result = self.solve([d_form(14, 3, 0, 0x2000), lambda pc: branch(pc, CAST+16, True),
                             d_form(14, 5, 3, 8), lambda pc: branch(pc, CAST, True), 0x4e800020])
        self.assertIsNone(result['calls'][0]['arguments']['r5'])
        result = self.solve([d_form(14, 5, 0, 0x2000), lambda pc: conditional(pc, TEXT+12),
                             d_form(14, 5, 0, 0x2000+8), lambda pc: branch(pc, CAST, True), 0x4e800020])
        self.assertIsNone(result['calls'][0]['arguments']['r5'])

    def test_unresolved_indirect_entry_and_unreachable_call_are_reported(self):
        result = self.solve([0x4e800420, lambda pc: branch(pc, CAST, True), 0x4e800020])
        context = result['calls'][0]['contexts'][0]
        self.assertFalse(context['reachable'])
        self.assertEqual(context['unresolved_control_sites'], [TEXT])
        self.assertEqual(result['candidate_roots'], [])

    def test_call_outside_declared_extent_has_no_invented_context(self):
        result = self.solve([0x60000000, lambda pc: branch(pc, CAST, True), 0x4e800020], [(TEXT, 4)])
        self.assertEqual(result['calls'][0]['contexts'], [])
        self.assertEqual(result['candidate_roots'], [])


if __name__ == '__main__':
    unittest.main()
