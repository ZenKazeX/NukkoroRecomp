"""The scratch-state audit must expose reads and never invent call clobbers."""
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import type_call_liveness as liveness
from test_type_catalog import Image, TEXT, branch, conditional, d_form


class LivenessTests(unittest.TestCase):
    def run_words(self, words, register='r4', **kwargs):
        encoded = {TEXT + 4*i: word(TEXT + 4*i) if callable(word) else word
                   for i, word in enumerate(words)}
        return liveness.Tracer(encoded.__getitem__, encoded.__contains__, **kwargs).trace(TEXT, register)

    def test_read_precedes_overwrite(self):
        self.assertEqual(self.run_words([d_form(14, 4, 4, 1), 0x4e800020]),
                         (frozenset({TEXT}), frozenset(), False))
        self.assertEqual(self.run_words([d_form(14, 4, 0, 1), d_form(36, 4, 3, 0), 0x4e800020]),
                         (frozenset(), frozenset(), False))

    def test_branch_join_keeps_an_unwritten_path(self):
        words = [lambda pc: conditional(pc, TEXT+8), d_form(14, 4, 0, 0),
                 d_form(36, 4, 3, 0), 0x4e800020]
        reads, barriers, escapes = self.run_words(words)
        self.assertEqual(reads, {TEXT+8})
        self.assertFalse(barriers or escapes)

    def test_direct_callee_input_is_not_assumed_dead(self):
        words = [lambda pc: branch(pc, TEXT+12, True), d_form(14, 4, 0, 0),
                 0x4e800020, d_form(36, 4, 3, 0), 0x4e800020]
        self.assertEqual(self.run_words(words)[0], {TEXT+12})

    def test_callee_preservation_reaches_caller_continuation(self):
        words = [lambda pc: branch(pc, TEXT+12, True), d_form(36, 4, 3, 0),
                 0x4e800020, d_form(14, 5, 0, 0), 0x4e800020]
        self.assertEqual(self.run_words(words)[0], {TEXT+4})
        words[3] = d_form(14, 4, 0, 0)
        self.assertEqual(self.run_words(words), (frozenset(), frozenset(), False))

    def test_conditional_call_skip_path_survives(self):
        words = [lambda pc: conditional(pc, TEXT+12) | 1, d_form(36, 4, 3, 0),
                 0x4e800020, d_form(14, 4, 0, 0), 0x4e800020]
        self.assertEqual(self.run_words(words)[0], {TEXT+4})

    def test_return_escape_is_not_called_an_overwrite(self):
        self.assertEqual(self.run_words([0x4e800020]), (frozenset(), frozenset(), True))

    def test_cr_and_count_branches_consume_state(self):
        self.assertEqual(self.run_words([lambda pc: conditional(pc, TEXT+4), 0x4e800020], 'cr0')[0], {TEXT})
        self.assertEqual(self.run_words([0x42000004, 0x4e800020], 'ctr')[0], {TEXT})

    def test_unknown_indirect_and_recursive_paths_remain_barriers(self):
        for words, expected in [([0], ('unknown_instruction', TEXT)),
                                ([0x4e800421], ('indirect_call', TEXT)),
                                ([lambda pc: branch(pc, pc, True)], ('recursive_call', TEXT))]:
            self.assertIn(expected, self.run_words(words)[1])

    def test_explicit_bounds_and_limits_remain_barriers(self):
        words = {TEXT: 0x60000000, TEXT+4: 0x4e800020}
        tracer = liveness.Tracer(words.__getitem__, words.__contains__)
        self.assertIn(('outside_caller_context', TEXT+4), tracer.trace(TEXT, 'r4', (TEXT, TEXT+4))[1])
        self.assertIn(('site_bound', TEXT+4), self.run_words(list(words.values()), max_sites=1)[1])
        self.assertIn(('call_depth_bound', TEXT+8),
                      self.run_words([lambda pc: branch(pc, TEXT+8, True), 0x4e800020, 0x4e800020], max_depth=1)[1])

    def test_jump_table_targets_are_followed_and_unknown_tables_refused(self):
        words = [0x4e800420, d_form(36, 4, 3, 0), 0x4e800020]
        self.assertIn(('indirect_branch', TEXT), self.run_words(words)[1])
        self.assertEqual(self.run_words(words, jump_tables={TEXT: [TEXT+4]})[0], {TEXT+4})
        self.assertEqual(self.run_words(words, 'ctr', jump_tables={TEXT: [TEXT+4]})[0], {TEXT})

    def test_closed_loop_finishes_without_inventing_a_return(self):
        self.assertEqual(self.run_words([lambda pc: branch(pc, pc)]),
                         (frozenset(), frozenset(), False))

    def test_changed_dol_and_cast_contract_are_refused(self):
        raw = bytes(Image().raw)
        with self.assertRaisesRegex(ValueError, 'different DOL'):
            liveness.audit(raw, {'dol_sha256': '0'*64})
        with self.assertRaises(ValueError):
            liveness.audit(raw, {'dol_sha256': hashlib.sha256(raw).hexdigest()})

    def test_audit_keeps_every_context_and_pins_call_words(self):
        image = Image()
        image.words(TEXT, 0x4e800020)
        site = TEXT+16
        call = branch(site, TEXT, True)
        image.words(site, call, d_form(36, 4, 1, 8), 0x4e800020)
        raw = bytes(image.raw)
        catalog = {'dol_sha256': hashlib.sha256(raw).hexdigest(),
                   'call_arguments': {'calls': [{'site': site, 'word': call, 'contexts': [
                       {'entry': site, 'end_exclusive': site+12, 'reachable': True},
                       {'entry': site, 'end_exclusive': site+8, 'reachable': False}]}]}}
        body = (0x4e800020).to_bytes(4, 'big')
        with patch.multiple(liveness, CAST=TEXT, CAST_END=TEXT+4,
                            CAST_SHA256=hashlib.sha256(body).hexdigest()):
            result = liveness.audit(raw, catalog)
            self.assertEqual(result['counts']['register_contexts'], 20)
            self.assertEqual(result['read_sites'], [{'site': site+4, 'word': d_form(36,4,1,8),
                                                    'registers':['r4'], 'kind':'store', 'r1_relative_store':True}])
            self.assertFalse(result['abi_clobbers_assumed'])
            self.assertFalse(result['complete_caller_contract_proven'])
            catalog['call_arguments']['calls'][0]['word'] ^= 4
            with self.assertRaisesRegex(ValueError, 'stale or non-cast'):
                liveness.audit(raw, catalog)
        with patch.multiple(liveness, CAST=TEXT, CAST_END=TEXT+4):
            with self.assertRaisesRegex(ValueError, 'scratch-state contract changed'):
                liveness.audit(raw, catalog)

    def test_invalid_cli_preserves_existing_report(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root/'bad.dol').write_bytes(b'bad')
            (root/'catalog.json').write_text('{}')
            (root/'tables.txt').write_text('')
            (root/'report.json').write_text('existing')
            command = [sys.executable, '-B', liveness.__file__, '--dol', str(root/'bad.dol'),
                       '--catalog', str(root/'catalog.json'), '--jump-tables', str(root/'tables.txt'),
                       '--out', str(root/'report.json')]
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual((root/'report.json').read_text(), 'existing')

    def guard(self, first, middle=0x60000000):
        words = [first, middle, 0x7d200775, 0x40800008, 0x4e800020,
                 0x5563043e, 0x4e800020]
        encoded = {TEXT+4*i: word for i,word in enumerate(words)}
        return liveness.negative_byte_guard(encoded.__getitem__, TEXT, TEXT+28,
                                            TEXT+12, 9, TEXT+20)

    def test_negative_byte_guard_proves_first_edge_unreachable(self):
        proof = self.guard(d_form(14,9,0,-1))
        self.assertTrue(proof['proven_for_direct_entry'])
        self.assertEqual(proof['initializers'], [TEXT])

    def test_nonnegative_or_unknown_guard_input_remains_unproved(self):
        for instruction in [d_form(14,9,0,0), d_form(14,9,3,-1), d_form(32,9,3,0)]:
            with self.subTest(instruction=instruction):
                self.assertFalse(self.guard(instruction)['proven_for_direct_entry'])

    def test_guard_refuses_calls_and_bypassed_predicate(self):
        self.assertFalse(self.guard(d_form(14,9,0,-1), branch(TEXT+4,TEXT+24,True))['proven_for_direct_entry'])
        self.assertFalse(self.guard(branch(TEXT,TEXT+12))['proven_for_direct_entry'])

    def test_later_write_invalidates_negative_guard(self):
        self.assertFalse(self.guard(d_form(14,9,0,-1), d_form(14,9,0,1))['proven_for_direct_entry'])


if __name__ == '__main__':
    unittest.main()
