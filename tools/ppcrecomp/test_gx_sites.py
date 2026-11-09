"""Classifier tests for the GX write-gather pipe site inventory
(tools/gx_site_inventory.py, the producer of extract/gx-sites.txt).

Synthetic procedures exercise each rule; every positive case has a mutation
that must flip it (the recognizer is not just matching everything) and the
held-out negatives must never become sites. CTest native_gx_sites_classifier.
"""
import sys
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))
import gx_site_inventory as g  # noqa: E402

BASE = 0x80010000          # synthetic client code (outside the GX SDK range)
SDK = 0x80396000           # synthetic code inside the GX SDK range


# ---- a few PowerPC encoders ---------------------------------------------------
def d_form(op, rd, ra, imm):
    return (op << 26) | (rd << 21) | (ra << 16) | (imm & 0xFFFF)


def lis(rd, imm): return d_form(15, rd, 0, imm)
def li(rd, imm): return d_form(14, rd, 0, imm)
def addi(rd, ra, imm): return d_form(14, rd, ra, imm)
def ori(ra, rs, imm): return d_form(24, rs, ra, imm)
def oris(ra, rs, imm): return d_form(25, rs, ra, imm)
def lwz(rd, d, ra): return d_form(32, rd, ra, d)
def stw(rs, d, ra): return d_form(36, rs, ra, d)
def stwu(rs, d, ra): return d_form(37, rs, ra, d)
def stb(rs, d, ra): return d_form(38, rs, ra, d)
def sth(rs, d, ra): return d_form(44, rs, ra, d)
def stfs(fs, d, ra): return d_form(52, fs, ra, d)
def mr(ra, rs): return (31 << 26) | (rs << 21) | (ra << 16) | (rs << 11) | (444 << 1)
def stwx(rs, ra, rb): return (31 << 26) | (rs << 21) | (ra << 16) | (rb << 11) | (151 << 1)
def lwzx(rd, ra, rb): return (31 << 26) | (rd << 21) | (ra << 16) | (rb << 11) | (23 << 1)
def rlwimi(ra, rs, sh, mb, me): return (20 << 26) | (rs << 21) | (ra << 16) | (sh << 11) | (mb << 6) | (me << 1)
def psq_st(fs, d, ra, w, i): return (60 << 26) | (fs << 21) | (ra << 16) | (w << 15) | (i << 12) | (d & 0xFFF)
def bl(pc, target): return (18 << 26) | ((target - pc) & 0x03FFFFFC) | 1
def b(pc, target): return (18 << 26) | ((target - pc) & 0x03FFFFFC)
def beq(pc, target): return (16 << 26) | (12 << 21) | (2 << 16) | ((target - pc) & 0xFFFC)
BLR = 0x4E800020
NOP = 0x60000000


class Code:
    """An in-memory procedure: words at consecutive addresses from `start`."""

    def __init__(self, start, words):
        self.start = start
        self.words = {start + 4 * i: w(start + 4 * i) if callable(w) else w for i, w in enumerate(words)}

    def word(self, pc):
        return self.words[pc]

    def text(self, pc):
        return pc in self.words


def solve(code, entry=None, tables=None, entry_values=None):
    entry = code.start if entry is None else entry
    words, states, begun, calls, fields = g.analyse_procedure(code.word, code.text, entry, tables or {},
                                                              entry_values)
    return g.pipe_rows(words, states, begun, entry), fields, states


def sites(code, **kw):
    rows, _, _ = solve(code, **kw)
    return {r['site']: r for r in rows}


PIPE_HI = 0xCC01 - 0x10000   # lis r, 0xCC01 (signed immediate)
PIPE_LO = -0x8000


class PipeAddressing(unittest.TestCase):
    def test_constant_base_in_volatile_register(self):
        code = Code(BASE, [lis(3, PIPE_HI), li(0, 0x61), stb(0, PIPE_LO, 3), BLR])
        found = sites(code)
        self.assertEqual(set(found), {BASE + 8})
        self.assertEqual(found[BASE + 8]['ea'], 0xCC008000)

    def test_base_in_nonvolatile_register_survives_a_call(self):
        callee = BASE + 0x100
        code = Code(BASE, [lis(31, PIPE_HI), lambda pc: bl(pc, callee), stw(0, PIPE_LO, 31), BLR])
        self.assertEqual(set(sites(code)), {BASE + 8})

    def test_mutation_base_in_volatile_register_does_not_survive_a_call(self):
        callee = BASE + 0x100
        code = Code(BASE, [lis(3, PIPE_HI), lambda pc: bl(pc, callee), stw(0, PIPE_LO, 3), BLR])
        self.assertEqual(sites(code), {})

    def test_update_form_keeps_the_new_base(self):
        # GXLoadPosMtxImm style: stwu leaves r4 = 0xCC008000 for the psq_st after it
        code = Code(BASE, [lis(4, PIPE_HI), stwu(0, PIPE_LO, 4), psq_st(1, 0, 4, 0, 0), BLR])
        found = sites(code)
        self.assertEqual(set(found), {BASE + 4, BASE + 8})
        self.assertEqual(found[BASE + 8]['ea'], 0xCC008000)

    def test_indexed_store_does_not_kill_its_base(self):
        code = Code(BASE, [lis(8, PIPE_HI), stwx(0, 9, 10), stb(0, PIPE_LO, 8), BLR])
        self.assertEqual(set(sites(code)), {BASE + 8})

    def test_indexed_load_does_not_kill_its_base(self):
        code = Code(BASE, [lis(8, PIPE_HI), lwzx(0, 8, 4), stb(0, PIPE_LO, 8), BLR])
        self.assertEqual(set(sites(code)), {BASE + 8})

    def test_merge_of_two_paths_with_the_same_base(self):
        code = Code(BASE, [lambda pc: beq(pc, BASE + 12), lis(5, PIPE_HI), lambda pc: b(pc, BASE + 16),
                           lis(5, PIPE_HI), stb(0, PIPE_LO, 5), BLR])
        self.assertEqual(set(sites(code)), {BASE + 16})

    def test_mutation_merge_of_two_different_bases_is_not_a_site(self):
        code = Code(BASE, [lambda pc: beq(pc, BASE + 12), lis(5, PIPE_HI), lambda pc: b(pc, BASE + 16),
                           lis(5, 0x8001 - 0x10000), stb(0, PIPE_LO, 5), BLR])
        self.assertEqual(sites(code), {})

    def test_jump_table_case_is_reached_only_through_its_table(self):
        bctr = 0x4E800420
        code = Code(BASE, [bctr, BLR, lis(6, PIPE_HI), stw(0, PIPE_LO, 6), BLR])
        self.assertEqual(sites(code), {})
        self.assertEqual(set(sites(code, tables={BASE: [BASE + 8]})), {BASE + 12})

    def test_entry_argument_propagation(self):
        code = Code(BASE, [stw(0, 0, 3), BLR])
        values = [g.UNKNOWN] * 40
        values[3] = g.constant(0xCC008000)
        self.assertEqual(set(sites(code, entry_values=values)), {BASE})
        self.assertEqual(sites(code), {})

    def test_pipe_pointer_loaded_from_constant_data(self):
        # RenderWare PTank: lwz r27, 0x1998(r2) = *(0x805FC118) = 0xCC008000
        code = Code(BASE, [lwz(27, 0x1998, 2), stfs(1, 0, 27), BLR])
        try:
            g._PIPE_POINTERS.update({0x805FA780 + 0x1998: 0xCC008000})
            self.assertEqual(set(sites(code)), {BASE + 4})
        finally:
            g._PIPE_POINTERS.clear()
        self.assertEqual(sites(code), {})  # mutation: without the data word it is not a site


class HeldOutNegatives(unittest.TestCase):
    def test_stack_sda_and_ram_stores_are_never_sites(self):
        code = Code(BASE, [stw(0, 8, 1), stw(0, 0x10, 13), stw(0, 0x20, 2), lis(4, 0x8000 - 0x10000),
                           stw(0, 0x100, 4), lis(5, 0xCC00 - 0x10000), stw(0, 0x2000, 5), BLR])
        self.assertEqual(sites(code), {})

    def test_overwritten_base_is_not_a_site(self):
        code = Code(BASE, [lis(31, PIPE_HI), li(31, 0), stw(0, PIPE_LO, 31), BLR])
        self.assertEqual(sites(code), {})

    def test_other_mmio_page_is_not_the_pipe(self):
        code = Code(BASE, [lis(3, 0xCC00 - 0x10000), sth(0, 0x2002, 3), BLR])  # VI
        self.assertEqual(sites(code), {})


def classify_code(code, function):
    rows, field_rows, _ = solve(code)
    for r in rows:
        r['entry_registers'] = []
    merged = g.merge_sites(rows, [function])
    fields = g.field_registers([(pc, off, v[0] >> 24 == 0xFF, v[1] >> 24, tag)
                                for pc, (off, v, tag) in field_rows.items()])
    payloads = g.bp_payloads(merged, code, fields)
    return {s: f for s, (f, _) in g.classify(merged, payloads).items()}


class Families(unittest.TestCase):
    def test_vertex_after_gx_begin_on_every_path(self):
        code = Code(BASE, [lambda pc: bl(pc, g.GX_BEGIN), lis(3, PIPE_HI), stfs(1, PIPE_LO, 3),
                           stfs(2, PIPE_LO, 3), BLR])
        self.assertEqual(set(classify_code(code, BASE).values()), {'vertex'})

    def test_mutation_store_before_gx_begin_is_not_vertex(self):
        code = Code(BASE, [lis(3, PIPE_HI), stfs(1, PIPE_LO, 3), lambda pc: bl(pc, g.GX_BEGIN),
                           lis(3, PIPE_HI), stfs(2, PIPE_LO, 3), BLR])
        families = classify_code(code, BASE)
        self.assertEqual(families[BASE + 4], 'unknown')
        self.assertEqual(families[BASE + 16], 'vertex')

    def test_mutation_gx_begin_on_one_path_only_is_not_vertex(self):
        code = Code(BASE, [lambda pc: beq(pc, BASE + 8), lambda pc: bl(pc, g.GX_BEGIN),
                           lis(3, PIPE_HI), stfs(1, PIPE_LO, 3), BLR])
        self.assertEqual(classify_code(code, BASE)[BASE + 12], 'unknown')

    def test_draw_done_payload(self):
        code = Code(SDK, [li(0, 0x61), lis(5, PIPE_HI), lis(4, 0x4500), stb(0, PIPE_LO, 5), addi(0, 4, 2),
                          stw(0, PIPE_LO, 5), BLR])
        families = classify_code(code, SDK)
        self.assertEqual(families[SDK + 12], 'dead')       # the BP opcode byte
        self.assertEqual(families[SDK + 20], 'draw_done')

    def test_mutation_other_bp_register_is_state(self):
        code = Code(SDK, [li(0, 0x61), lis(5, PIPE_HI), lis(4, 0x4600), stb(0, PIPE_LO, 5), addi(0, 4, 2),
                          stw(0, PIPE_LO, 5), BLR])
        self.assertEqual(classify_code(code, SDK)[SDK + 20], 'dead')

    def test_token_payloads(self):
        code = Code(SDK, [li(6, 0x61), lis(5, PIPE_HI), oris(7, 3, 0x4800), stb(6, PIPE_LO, 5), stw(7, PIPE_LO, 5),
                          li(0, 0x47), rlwimi(7, 0, 24, 0, 7), stb(6, PIPE_LO, 5), stw(7, PIPE_LO, 5), BLR])
        families = classify_code(code, SDK)
        # r3's high bits are unknown, so oris alone does not give the byte...
        self.assertEqual(families[SDK + 16], 'dead')
        # ...while rlwimi of the constant 0x47 does
        self.assertEqual(families[SDK + 32], 'draw_sync_token')

    def test_copy_trigger_through_a_gx_field(self):
        gx = g.GX_DATA
        hi = ((gx + 0x8000) >> 16) - 0x10000 if ((gx + 0x8000) >> 16) & 0x8000 else (gx + 0x8000) >> 16
        lo = gx & 0xFFFF
        code = Code(SDK, [lis(6, hi), addi(6, 6, lo - 0x10000 if lo & 0x8000 else lo),
                          lwz(3, 0x1EC, 6), li(0, 0x52), rlwimi(3, 0, 24, 0, 7), stw(3, 0x1EC, 6),
                          li(8, 0x61), lis(7, PIPE_HI), stb(8, PIPE_LO, 7), lwz(0, 0x1EC, 6), stw(0, PIPE_LO, 7),
                          BLR])
        families = classify_code(code, SDK)
        self.assertEqual(families[SDK + 40], 'copy_trigger')

    def test_mutation_field_holding_a_state_register_is_dead(self):
        gx = g.GX_DATA
        hi = ((gx + 0x8000) >> 16) - 0x10000 if ((gx + 0x8000) >> 16) & 0x8000 else (gx + 0x8000) >> 16
        lo = gx & 0xFFFF
        code = Code(SDK, [lis(6, hi), addi(6, 6, lo - 0x10000 if lo & 0x8000 else lo),
                          lwz(3, 0x1EC, 6), li(0, 0x49), rlwimi(3, 0, 24, 0, 7), stw(3, 0x1EC, 6),
                          li(8, 0x61), lis(7, PIPE_HI), stb(8, PIPE_LO, 7), lwz(0, 0x1EC, 6), stw(0, PIPE_LO, 7),
                          BLR])
        self.assertEqual(classify_code(code, SDK)[SDK + 40], 'dead')

    def test_unresolved_payload_in_a_live_bp_function_stays_unknown(self):
        function = min(g.LIVE_BP_FUNCTIONS)
        code = Code(function, [li(8, 0x61), lis(7, PIPE_HI), stb(8, PIPE_LO, 7), stw(9, PIPE_LO, 7), BLR])
        self.assertEqual(classify_code(code, function)[function + 12], 'unknown')

    def test_unresolved_payload_of_a_state_setter_is_dead_by_api_purpose(self):
        code = Code(SDK + 0x2000, [li(8, 0x61), lis(7, PIPE_HI), stb(8, PIPE_LO, 7), stw(9, PIPE_LO, 7), BLR])
        rows, _, _ = solve(code)
        merged = g.merge_sites([dict(r, entry_registers=[]) for r in rows], [SDK + 0x2000])
        payloads = g.bp_payloads(merged, code, {})
        family, reason = g.classify(merged, payloads)[SDK + 0x200C]
        self.assertEqual(family, 'dead')
        self.assertTrue(reason.startswith('dead_by_api_purpose'))

    def test_gx_begin_header(self):
        code = Code(g.GX_BEGIN, [lis(3, PIPE_HI), stb(0, PIPE_LO, 3), sth(30, PIPE_LO, 3), BLR])
        families = classify_code(code, g.GX_BEGIN)
        self.assertEqual(families[g.GX_BEGIN + 4], 'primitive_opcode')
        self.assertEqual(families[g.GX_BEGIN + 8], 'primitive_count')


if __name__ == '__main__':
    unittest.main()
