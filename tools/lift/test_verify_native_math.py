"""A pruned base entry must not conceal a surviving interior entry."""
import hashlib
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest

from verify_native_math import CONTRACTS, Dol, verify_contract, verify_matrix_index, identical_text_spans


class MathRemovalEvidenceTests(unittest.TestCase):
    def setUp(self):
        body = bytes.fromhex("6000000060000000600000004e800020")
        self.image = SimpleNamespace(raw=body, offset=lambda address: address - 0x80001000,
            contains_text=lambda address, size: address == 0x80001000 and size == len(body),
            sections=[{"size": len(body), "address": 0x80001000, "file_offset": 0, "kind": "text"}])
        self.contract = {"name": "test", "start": 0x80001000, "end": 0x80001010,
                         "hash": hashlib.sha256(body).hexdigest(), "edges": [],
                         "api": "copy_affine_matrix", "call": "native_call()"}

    def test_duplicate_inventory_excludes_data_and_unaligned_matches(self):
        body = self.image.raw
        image = SimpleNamespace(raw=body + body + b"!" + body + body,
            offset=lambda address: address - 0x80001000,
            contains_text=lambda address, size: address == 0x80001000 and size == len(body),
            sections=[{"address": 0x80001000, "file_offset": 0, "size": len(body) * 3 + 1, "kind": "text"},
                      {"address": 0x80002000, "file_offset": len(body) * 3 + 1, "size": len(body), "kind": "data"}])
        self.assertEqual(identical_text_spans(image, 0x80001000, 0x80001010), [0x80001000, 0x80001010])
        with self.assertRaisesRegex(ValueError, "invalid original"):
            identical_text_spans(image, 0x80001000, 0x8000100F)

    def test_interior_generated_entries_rejected_even_when_base_pruned(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "recomp_pruned.csv").write_text("address\n0x80001000\n")
            (root / "recomp_table.cpp").write_text("native_call()")
            for address in (0x80001000, 0x80001004, 0x8000100C):
                with self.subTest(address=address):
                    (root / "recomp_decls.h").write_text(f"void fn_{address:08X}_body();")
                    with self.assertRaisesRegex(ValueError, "generated entry"):
                        verify_contract(self.image, self.contract, generated=root)

    def test_interior_linked_entries_rejected_and_adjacent_functions_allowed(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "image.map"
            native = "?copy_affine_matrix@math@shadow@@\n"
            for address in (0x80001000, 0x80001004, 0x8000100C):
                with self.subTest(address=address):
                    path.write_text(native + f"?fn_{address:08X}_body_at@recomp@gekko@@\n")
                    with self.assertRaisesRegex(ValueError, "remains linked"):
                        verify_contract(self.image, self.contract, linker_map=path)
            path.write_text(native + "?fn_80000FFC@recomp@gekko@@\n?fn_80001010@recomp@gekko@@\n")
            verify_contract(self.image, self.contract, linker_map=path)


class MatrixIndexEvidenceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.image = Dol((Path(__file__).resolve().parents[3] / "game/sys/main.dol").read_bytes())

    def changed_word(self, address, word):
        raw = bytearray(self.image.raw)
        section = next(s for s in self.image.sections if s["address"] <= address < s["address"] + s["size"])
        offset = section["file_offset"] + address - section["address"]
        raw[offset:offset + 4] = word.to_bytes(4, "big")
        return Dol(bytes(raw))

    def test_copy_family_body_edges_and_data_references_are_pinned(self):
        contract = next(c for c in CONTRACTS if c["start"] == 0x8040D198)
        verify_contract(self.image, contract)
        self.assertEqual(identical_text_spans(self.image, 0x8040CD04, 0x8040CD24),
                         [0x8040CD04, 0x8040D198])
        for address, word, error in ((0x8040D198, 0x60000000, "implementation changed"),
                                     (0x8040CD04, 0x60000000, "not byte-identical"),
                                     (0x80008EB8, 0x60000000, "entry edges"),
                                     (0x80008D38, 0x8040D198, "data reference")):
            with self.subTest(address=address), self.assertRaisesRegex(ValueError, error):
                verify_contract(self.changed_word(address, word), contract)

    def test_range_record_and_consumer_are_both_required(self):
        verify_matrix_index(self.image)
        for address, word in ((0x800076F8, 0x1EC), (0x800076FC, 0x800057DC),
                              (0x800057D8, 1), (0x80008D14, 0x8000752C), (0x803A381C, 0x60000000)):
            with self.subTest(address=address), self.assertRaisesRegex(ValueError, "exception index"):
                verify_matrix_index(self.changed_word(address, word))

    def test_second_data_pointer_is_not_exempted(self):
        contract = next(c for c in CONTRACTS if c["api"] == "compose_affine_matrix")
        with self.assertRaisesRegex(ValueError, "data reference"):
            verify_contract(self.changed_word(0x80008D38, contract["start"]), contract)


if __name__ == "__main__":
    unittest.main()
