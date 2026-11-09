"""Ownership-census regressions: incomplete evidence must stay incomplete."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import type_metadata_uses as uses
import gx_site_inventory as flow
from test_type_catalog import Image, TEXT, DATA, branch, conditional, d_form


def address_words(register, address):
    return [d_form(15, register, 0, (address + 0x8000) >> 16),
            d_form(14, register, register, address)]


class OwnershipTests(unittest.TestCase):
    def solve(self, image, words, roots, declarations=None, jumps=None):
        image.words(TEXT, *[word(TEXT + 4*i) if callable(word) else word for i, word in enumerate(words)])
        declarations = declarations or [(TEXT, len(words)*4)]
        functions = 'addr_hex,name,size\n' + ''.join(f'{a:#x},f{i},{size}\n' for i,(a,size) in enumerate(declarations))
        return uses.audit(bytes(image.raw), functions.encode(), jumps or {}, image.build(roots, True))

    def test_literal_metadata_mutation_is_reported(self):
        image = Image()
        descriptor = image.descriptor(b'Type')
        words = address_words(3, descriptor) + [d_form(14, 4, 0, 0), d_form(36, 4, 3, 4), 0x4e800020]
        result = self.solve(image, words, [descriptor])
        self.assertEqual(result['literal_input_stores'][0]['metadata_overlap'], [[descriptor+4, descriptor+8]])
        self.assertEqual(result['different_known_word_stores'], [])  # already zero
        words[2] = d_form(14, 4, 0, 12)
        result = self.solve(image, words, [descriptor])
        self.assertEqual(result['different_known_word_stores'][0]['stored_word'], 12)
        self.assertFalse(result['immutability_proven'])

    def test_stored_null_first_method_discovers_missing_type(self):
        image = Image()
        seed = image.descriptor(b'Seed')
        missing = image.descriptor(b'Missing')
        header = image.vtable(missing, -32)
        image.words(header + 8, 0)
        words = address_words(4, header) + [d_form(36, 4, 3, 12), 0x4e800020]
        result = self.solve(image, words, [seed])
        found = result['stored_header_candidates'][0]
        self.assertEqual(found['descriptor'], missing)
        self.assertEqual(found['adjustment'], -32)
        self.assertFalse(found['in_prior_vtable_census'])
        self.assertEqual(result['new_descriptor_roots'], [missing])
        self.assertEqual(result['unknown_destination_sites']['count'], 1)
        self.assertFalse(result['complete_dynamic_domain_proven'])

    def test_overlapping_entry_keeps_unknown_destination(self):
        image = Image()
        descriptor = image.descriptor(b'Type')
        words = address_words(3, descriptor) + [d_form(36, 4, 3, 0), 0x4e800020]
        result = self.solve(image, words, [descriptor], [(TEXT, 16), (TEXT+8, 8)])
        self.assertEqual(len(result['literal_input_stores']), 1)
        self.assertEqual(result['unknown_destination_sites']['first_sites'], [TEXT+8])
        self.assertEqual(result['store_contexts']['stores'], 2)

    def test_calls_and_merges_never_invent_known_destination(self):
        image = Image()
        descriptor = image.descriptor(b'Type')
        words = address_words(3, descriptor) + [lambda pc: branch(pc, TEXT+0x800, True),
                d_form(36, 4, 3, 0), 0x4e800020]
        result = self.solve(image, words, [descriptor])
        self.assertEqual(result['literal_input_stores'], [])
        self.assertEqual(result['unknown_destination_sites']['count'], 1)
        words = address_words(3, descriptor) + [lambda pc: conditional(pc, TEXT+16),
                d_form(14, 3, 0, 0), d_form(36, 4, 3, 0), 0x4e800020]
        self.assertEqual(self.solve(image, words, [descriptor])['unknown_destination_sites']['count'], 1)

    def test_unresolved_control_and_unreached_stores_remain_visible(self):
        image = Image()
        descriptor = image.descriptor(b'Type')
        result = self.solve(image, [0x4e800420, d_form(36, 4, 3, 0), 0x4e800020], [descriptor])
        self.assertEqual(result['unresolved_control'], [{'entry': TEXT, 'site': TEXT, 'source': 'CTR'}])
        self.assertEqual(result['raw_store_words_not_reached']['first_sites'], [TEXT+4])
        known = self.solve(image, [0x4e800420, d_form(36, 4, 3, 0), 0x4e800020], [descriptor], jumps={TEXT: [TEXT+4]})
        self.assertEqual(known['unresolved_control'], [])
        self.assertEqual(known['analyzed_store_sites']['count'], 1)

    def test_cache_line_alignment_wrap_and_partial_overlap(self):
        spans = uses.SpanIndex([(0, 8), (0x1000, 0x1008), (0xfffffffc, 0x100000000)])
        self.assertEqual(spans.overlaps(0xfffffffe, 4), [[0xfffffffe, 0x100000000], [0, 2]])
        self.assertEqual(spans.overlaps(0xffe, 4), [[0x1000, 0x1002]])
        self.assertEqual(spans.overlaps(8, 4), [])
        values = [flow.UNKNOWN] * 40
        values[3] = flow.constant(0x101f)
        values[0] = flow.constant(0)  # Indexed rB=0 reads r0; only rA has a zero-base rule.
        word = (31 << 26) | (3 << 16) | (1014 << 1)
        self.assertEqual(uses.store_extent(flow.store_form(word), word, values), (0x1000, 32, True))

    def test_quantized_and_string_stores_use_conservative_widths(self):
        values = [flow.UNKNOWN] * 40
        values[3] = flow.constant(DATA)
        values[4] = flow.constant(0)
        for indexed in (False, True):
            for one in (0, 1):
                word = ((4 << 26) | (3 << 16) | (4 << 11) | (one << 10) | (7 << 1)) if indexed else \
                       ((60 << 26) | (3 << 16) | (one << 15))
                self.assertEqual(uses.store_extent(flow.store_form(word), word, values), (DATA, 4 if one else 8, False))
        word = (31 << 26) | (3 << 16) | (4 << 11) | (661 << 1)
        self.assertEqual(uses.store_extent(flow.store_form(word), word, values), (DATA, 127, False))

    def test_multiword_and_reversed_stores_preserve_word_offsets(self):
        values = [flow.UNKNOWN] * 40
        values[30], values[31], values[0] = map(flow.constant, (0x12345678, 0x87654321, 42))
        word = d_form(47, 30, 3, 0)
        self.assertEqual(uses.stored_words(flow.store_form(word), values), [(0, 0x12345678), (4, 0x87654321)])
        word = (31 << 26) | (31 << 21) | (3 << 16) | (9 << 11) | (725 << 1)
        self.assertEqual(uses.stored_words(flow.store_form(word), values), [(0, 0x87654321), (4, 42)])
        word = (31 << 26) | (30 << 21) | (3 << 16) | (662 << 1)
        self.assertEqual(uses.stored_words(flow.store_form(word), values), [(0, 0x78563412)])

    def test_bad_catalog_identity_is_rejected(self):
        image = Image()
        descriptor = image.descriptor(b'Type')
        catalog = image.build([descriptor])
        catalog['dol_sha256'] = '0' * 64
        with self.assertRaisesRegex(ValueError, 'different DOL'):
            uses.audit(bytes(image.raw), b'', {}, catalog)

    def test_explicit_sda_writes_and_unknown_entry_are_respected(self):
        image = Image()
        descriptor = image.descriptor(b'Type')
        words = address_words(2, descriptor) + [d_form(36, 4, 2, 0), 0x4e800020]
        result = self.solve(image, words, [descriptor])
        hit = result['literal_input_stores'][0]
        self.assertEqual(hit['effective_address'], descriptor)
        self.assertFalse(hit['requires_entry_sda'])
        self.assertEqual(len(result['explicit_sda_register_writes']), 2)
        words = address_words(13, descriptor) + [d_form(32, 13, 3, 0), d_form(36, 4, 13, 0), 0x4e800020]
        result = self.solve(image, words, [descriptor])
        self.assertEqual(result['literal_input_stores'], [])
        self.assertEqual(result['unknown_destination_sites']['count'], 1)
        pc = TEXT
        word = d_form(36, 4, 2, 0)
        read = lambda at: word if at == pc else 0x4e800020
        _, states, _, _, _ = flow.analyse_procedure(read, lambda at: pc <= at < pc+8, pc, {},
                                                    pin_sda=False, seed_sda=False)
        self.assertFalse(flow.known(states[pc][0][2]))

    def test_entry_sda_dependency_is_reported_instead_of_certified(self):
        image = Image()
        descriptor = image.descriptor(b'Type')
        with patch.dict(flow.SDA_BASES, {2: descriptor}, clear=True):
            result = self.solve(image, [d_form(36, 4, 2, 0), 0x4e800020], [descriptor])
        self.assertTrue(result['literal_input_stores'][0]['requires_entry_sda'])
        self.assertFalse(result['immutability_proven'])

    def test_malformed_candidate_does_not_become_type(self):
        image = Image()
        descriptor = image.descriptor(b'Type')
        invalid = image.reserve(12)
        image.words(invalid, DATA+0x5000, 0, TEXT)
        words = address_words(4, invalid) + [d_form(36, 4, 3, 0), 0x4e800020]
        result = self.solve(image, words, [descriptor])
        self.assertEqual(result['stored_header_candidates'], [])
        self.assertEqual(result['new_descriptor_roots'], [])

    def test_invalid_cli_keeps_previous_report(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            bad = root / 'bad.dol'
            bad.write_bytes(b'bad')
            catalog = root / 'catalog.json'
            catalog.write_text(json.dumps({'dol_sha256': '0' * 64}))
            functions, jumps, report = root / 'functions.csv', root / 'jumps.txt', root / 'report.json'
            functions.write_text(''); jumps.write_text(''); report.write_text('existing')
            command = [sys.executable, '-B', str(Path(uses.__file__)), '--dol', str(bad),
                       '--functions', str(functions), '--jump-tables', str(jumps),
                       '--catalog', str(catalog), '--out', str(report)]
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(report.read_text(), 'existing')

    def test_metadata_reads_and_call_register_candidates(self):
        image = Image()
        descriptor = image.descriptor(b'Type')
        words = address_words(5, descriptor) + [d_form(32, 4, 5, 0),
                lambda pc: branch(pc, TEXT+0x800, True), 0x4e800020]
        result = self.solve(image, words, [descriptor])
        read = result['literal_input_reads'][0]
        self.assertEqual(read['effective_address'], descriptor)
        self.assertFalse(read['requires_entry_sda'])
        self.assertEqual([(v['register'],v['value']) for v in result['known_metadata_call_values']], [(5,descriptor)])
        self.assertEqual(result['known_metadata_call_values'][0]['callee'], TEXT+0x800)

    def test_loaded_values_and_clobbered_arguments_remain_unknown(self):
        image = Image()
        descriptor = image.descriptor(b'Type')
        words = address_words(5, descriptor) + [d_form(32, 3, 5, 0),
                lambda pc: branch(pc, TEXT+0x800, True), lambda pc: branch(pc, TEXT+0x804, True), 0x4e800020]
        result = self.solve(image, words, [descriptor])
        self.assertEqual([v['register'] for v in result['known_metadata_call_values']], [5])
        self.assertEqual(len(result['known_metadata_call_values']), 1)

    def test_external_data_references_are_not_hidden_by_descriptor_roots(self):
        image = Image()
        base = image.descriptor(b'Base')
        derived = image.descriptor(b'Derived', [(base, 8, [(base, 8)])])
        image.vtable(derived)
        external = image.reserve(8)
        image.words(external, derived, derived+1)
        refs = uses.metadata_data_references(bytes(image.raw), image.build([derived], True))
        self.assertEqual(refs['owned_reference_roles']['descriptor_name'], 2)
        self.assertEqual(refs['owned_reference_roles']['descriptor_bases'], 1)
        self.assertEqual(refs['owned_reference_roles']['base_type'], 1)
        self.assertEqual(refs['owned_reference_roles']['restricted_source_type'], 1)
        self.assertEqual(refs['owned_reference_roles']['vtable_type'], 1)
        self.assertEqual(refs['external_references'], [{'address':external,'value':derived},
                                                     {'address':external+4,'value':derived+1}])

    def test_read_bounds_and_unknown_pointer_are_retained(self):
        values = [flow.UNKNOWN] * 40
        values[3] = flow.constant(DATA)
        self.assertEqual(uses.read_extent(d_form(46,30,3,4), values, TEXT), (DATA+4,8,True))
        self.assertEqual(uses.read_extent(d_form(32,4,5,0), values, TEXT), (None,4,True))
        word = (56 << 26) | (3 << 16)
        self.assertEqual(uses.read_extent(word, values, TEXT), (DATA,8,False))


if __name__ == '__main__':
    unittest.main()
