"""Whole-image inventory of the GX write-gather pipe's store sites.

Phase 4 of the modernization directive (docs/native-time-20261005/
GX_DATA_PLANE.md section 4). Every store instruction of the main DOL whose
effective address is the write-gather pipe page (EA 0xCC008000-0xCC0080FF) is
a site; each site is classified into the family the generator lowers it to
(`extract/gx-sites.txt`, `ppcrecomp --gx-sites`):

  dead                 transport with no consumer on the PC (state, display-list
                       calls, FIFO padding, NOPs, the dummy flush strip)
  draw_sync_token      BP 0x47 payload (GXSetDrawSync)
  draw_sync_token_irq  BP 0x48 payload (GXSetDrawSync)
  draw_done            BP 0x45 payload (GXDrawDone)
  copy_trigger         BP 0x52 payload (GXCopyDisp / GXCopyTex)
  primitive_opcode     GXBegin's header byte (type | vertex format)
  primitive_count      GXBegin's vertex count
  vertex               immediate vertex data: a client store that every path
                       from its function's entry reaches through a GXBegin call
  unknown              anything the rules cannot place; blocks the list

Analysis (stated assumptions, each checked at run time by the lowered build's
unlisted-store stop): per declared function (extract/functions.csv) a forward
known-bits constant propagation over the GPRs (the canonical
gqr_format_dataflow.transfer) on the procedure's direct-branch CFG plus the
recognised jump tables (extract/jump-tables.txt), with the EABI call rule - r1,
r2, r13 and r14-r31 survive a `bl` / `bctrl` / `sc`, r0 and r3-r12 do not -
where the canonical solver invalidates everything at a call. Pipe addresses
passed in r3-r10 to a direct callee are propagated into that callee (repeated to
a fixed point). Floating-point and paired-single arithmetic the canonical
transfer does not model leave the GPRs alone; any other unmodelled word kills
every fact. Memory contents are never read: a pipe pointer loaded from memory
is not seen (the run-time stop is the backstop).

usage: gx_site_inventory.py --dol <main.dol> --functions <functions.csv>
                            --jump-tables <jump-tables.txt> --out <report.json>
                            [--census <census-v2.json>] [--write-list <gx-sites.txt>]
                            [--workers N]
"""
import argparse
import bisect
from collections import Counter, defaultdict, deque
from concurrent.futures import ProcessPoolExecutor
import hashlib
import json
from pathlib import Path
import struct
import sys

if __package__:
    from .graphics_lift_inventory import Dol, decode, hx, table
    from .gqr_format_dataflow import U32, UNKNOWN, constant, transfer
else:
    from graphics_lift_inventory import Dol, decode, hx, table
    from gqr_format_dataflow import U32, UNKNOWN, constant, transfer

PIPE_PAGE = 0xCC008000
PAGE_MASK = 0xFFFFFF00
# Pages that reach physical 0x0C008000 other than through the uncached BAT: a
# store there would be a defect of the image or of this analysis; reported.
PIPE_ALIASES = {0x8C008000: 'cached alias of the pipe page', 0x0C008000: 'untranslated physical pipe page'}
# Window of argument values worth propagating into a callee: the pipe page and
# the 0xCC010000 base the SDK forms it from (stores use -0x8000 off it).
ARGUMENT_WINDOW = (0xCC000000, 0xCC020000)

GX_SDK = (0x80393754, 0x8039AEA0)   # __GXShutdown .. GXClearGPMetric's end (TRK follows)
GX_BEGIN = 0x80396934
BP_OPCODE = 0x61
BP_FAMILIES = {0x45: 'draw_done', 0x47: 'draw_sync_token', 0x48: 'draw_sync_token_irq', 0x52: 'copy_trigger'}
LIVE_BP = set(BP_FAMILIES)
# The GX SDK functions that emit a live BP register (draw-sync token, draw
# done, copy trigger): GXSetDrawSync, GXDrawDone, GXCopyDisp, GXCopyTex.
LIVE_BP_FUNCTIONS = {0x80396428: 'GXSetDrawSync', 0x803964E8: 'GXDrawDone',
                     0x80397444: 'GXCopyDisp', 0x803975AC: 'GXCopyTex'}
# RenderWare PTank: the render callback calls the instance's vertex writer
# (+0x28) right after GXBegin; the selector stores one of the writers there.
INDIRECT_WRITER_CALL = dict(site=0x8044A23C, caller=0x80449D54, field=0x28, selector=0x804496A8)
LIVE_FAMILIES = ('draw_sync_token', 'draw_sync_token_irq', 'draw_done', 'copy_trigger',
                 'primitive_opcode', 'primitive_count', 'vertex')
FAMILIES = ('dead',) + LIVE_FAMILIES
VOLATILE_GPRS = (0, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12)
SDA_BASES = {2: 0x805FA780, 13: 0x805EC500}   # EABI: set once by __init_registers, never written again
GX_POINTER_SLOT = 0x805FA780 + 0xFD8         # __GXData pointer word (gx = *(r2 + 0xFD8))
GX_DATA = 0x805A2F60                         # its initialised value in the DOL (checked at load)
GX_DATA_SIZE = 0x600                         # __GXData ends before gx+0x600 (dirtyState at +0x5AC)
GXPTR = ('gxptr', 0)
PIPE_BASES = (0xCC008000, 0xCC010000)        # the pipe page and the base the SDK forms it from
_PIPE_POINTERS = {}                          # data word address -> pipe base it holds (set at load)
ARGUMENT_GPRS = (3, 4, 5, 6, 7, 8, 9, 10)
EMPTY_ORIGINS = tuple(frozenset() for _ in range(8))
EMPTY_DEFINITIONS = tuple(frozenset() for _ in range(32))
D_STORES = {36: ('stw', 4, False, 'gpr'), 37: ('stwu', 4, True, 'gpr'), 38: ('stb', 1, False, 'gpr'),
            39: ('stbu', 1, True, 'gpr'), 44: ('sth', 2, False, 'gpr'), 45: ('sthu', 2, True, 'gpr'),
            52: ('stfs', 4, False, 'fpr'), 53: ('stfsu', 4, True, 'fpr'), 54: ('stfd', 8, False, 'fpr'),
            55: ('stfdu', 8, True, 'fpr')}
D_LOAD_UPDATES = {33, 35, 41, 43, 49, 51}
X_STORES = {151: ('stwx', 4, False, 'gpr'), 183: ('stwux', 4, True, 'gpr'), 215: ('stbx', 1, False, 'gpr'),
            247: ('stbux', 1, True, 'gpr'), 407: ('sthx', 2, False, 'gpr'), 439: ('sthux', 2, True, 'gpr'),
            663: ('stfsx', 4, False, 'fpr'), 695: ('stfsux', 4, True, 'fpr'), 727: ('stfdx', 8, False, 'fpr'),
            759: ('stfdux', 8, True, 'fpr'), 983: ('stfiwx', 4, False, 'fpr'), 662: ('stwbrx', 4, False, 'gpr'),
            918: ('sthbrx', 2, False, 'gpr'), 150: ('stwcx.', 4, False, 'gpr'), 438: ('ecowx', 4, False, 'gpr'),
            661: ('stswx', None, False, 'gpr'), 1014: ('dcbz', 32, False, 'none')}
X_LOAD_UPDATES = {55, 119, 311, 375, 567, 631}
# Group-31 words that write no GPR: indexed stores without update, cache
# operations, FP indexed loads without update, mtspr / mtmsr / mtsr / mtsrin /
# mtcrf, sync / eieio / tlbie / tlbsync, compares, tw.
X_NO_GPR = {151, 215, 407, 663, 727, 983, 662, 918, 150, 438, 661, 725, 1014,
            54, 86, 470, 278, 246, 982, 535, 599, 467, 146, 210, 242, 144, 854, 598, 306, 566, 0, 32, 4}
# Group-31 update forms: rA only (stores and FP loads; integer loads are modelled).
X_UPDATE_RA = {183, 247, 439, 695, 759, 567, 631}
X_WRITES_RD = {23, 87, 279, 343, 534, 790, 20, 310,              # indexed loads, lwarx, eciwx
               339, 371, 19, 83, 595, 659}                         # mfspr, mftb, mfcr, mfmsr, mfsr, mfsrin
X_WRITES_RD_RA = {55, 119, 311, 375}                               # integer indexed loads with update
XO_WRITES_RD = {266, 10, 138, 202, 234, 40, 8, 136, 200, 232, 104,  # add / sub families, neg (xo & 511)
                235, 75, 11, 491, 459}                             # mullw, mulhw, mulhwu, divw, divwu
X_WRITES_RA = {28, 60, 444, 412, 316, 476, 124, 284, 24, 536, 792, 824, 26, 954, 922}


def signed(value, bits):
    value &= (1 << bits) - 1
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


def known(value):
    return value[0] == U32


def store_form(word):
    """(mnemonic, width or None, updates base, value domain, rS, rA, rB or None, displacement)."""
    op, s, a, b = word >> 26, (word >> 21) & 31, (word >> 16) & 31, (word >> 11) & 31
    if op in D_STORES:
        name, width, update, domain = D_STORES[op]
        return name, width, update, domain, s, a, None, signed(word, 16)
    if op == 47:
        return 'stmw', 4 * (32 - s), False, 'gpr', s, a, None, signed(word, 16)
    if op in (60, 61):
        return ('psq_st' if op == 60 else 'psq_stu'), None, op == 61, 'fpr', s, a, None, signed(word, 12)
    if op == 31:
        xo = (word >> 1) & 1023
        if xo == 725:
            return 'stswi', (b or 32), False, 'gpr', s, a, None, 0
        if xo in X_STORES:
            name, width, update, domain = X_STORES[xo]
            return name, width, update, domain, s, a, b, 0
    if op == 4:
        if (word >> 1) & 63 in (7, 39):
            return ('psq_stx' if (word >> 1) & 63 == 7 else 'psq_stux'), None, (word >> 1) & 63 == 39, 'fpr', \
                s, a, b, 0
        if (word >> 1) & 1023 == 1014:
            return 'dcbz_l', 32, False, 'none', s, a, b, 0
    return None


def effective_address(form, values):
    name, _, update, _, _, a, b, displacement = form
    if a == 0 and not update:
        base = constant(0)
    else:
        base = values[a]
    if not known(base):
        return None
    if b is not None:
        index = values[b]
        if not known(index):
            return None
        return (base[1] + index[1]) & U32
    return (base[1] + displacement) & U32


def update_load_base(word, values):
    """EA of an update-form load, to keep rA known (the canonical transfer kills it)."""
    op, a = word >> 26, (word >> 16) & 31
    if op in D_LOAD_UPDATES and a != 0 and known(values[a]):
        return a, (values[a][1] + signed(word, 16)) & U32
    if op == 31 and (word >> 1) & 1023 in X_LOAD_UPDATES and a != 0:
        b = (word >> 11) & 31
        if known(values[a]) and known(values[b]):
            return a, (values[a][1] + values[b][1]) & U32
    if op == 57 and a != 0 and known(values[a]):
        return a, (values[a][1] + signed(word, 12)) & U32
    return None


def destinations(word):
    """GPRs a word can write, or None for 'any' (unmodelled)."""
    op, d, a = word >> 26, (word >> 21) & 31, (word >> 16) & 31
    xo = (word >> 1) & 1023
    if op in (7, 8, 12, 13, 14, 15):
        return {d}
    if op in (20, 21, 23, 24, 25, 26, 27, 28, 29):
        return {a}
    if 32 <= op <= 45:
        if op in (36, 38, 44):
            return set()
        if op in (37, 39, 45):
            return {a}
        return {d} | ({a} if op & 1 else set())
    if op == 46:
        return set(range(d, 32))
    if op in (47, 48, 50, 52, 54, 56, 60, 2, 3, 10, 11, 16, 18, 19, 59, 63):
        return set()
    if op in (49, 51, 53, 55, 57, 61):
        return {a} if a else set()
    if op == 4:
        return {a} if (word >> 1) & 63 in (38, 39) else set()
    if op == 31:
        if xo in (533, 597):
            return None
        if xo in X_NO_GPR:
            return set()
        if xo in X_UPDATE_RA:
            return {a}
        if xo in X_WRITES_RD:
            return {d}
        if xo in X_WRITES_RD_RA:
            return {d, a}
        if xo & 511 in XO_WRITES_RD:
            return {d}
        if xo in X_WRITES_RA:
            return {a}
        return {d, a}
    return None


def step(word, state, pc, pin_sda=True):
    """Known-bits GPR / GQR facts and value tags after one word (EABI call rule).
    Returns (state, field_store or None)."""
    values, tags = state
    flow = decode(word, pc)
    op = word >> 26
    if (flow and flow.get('link')) or op == 17:
        after = list(values)
        after_tags = list(tags)
        for register in VOLATILE_GPRS:
            after[register] = UNKNOWN
            after_tags[register] = None
        for index in range(32, 40):
            after[index] = UNKNOWN
        return (tuple(after), tuple(after_tags)), None
    form = store_form(word)
    update_ea = effective_address(form, values) if form and form[2] else None
    load_update = update_load_base(word, values)
    (after, _, _), barrier = transfer(word, (values, EMPTY_ORIGINS, EMPTY_DEFINITIONS), pc)
    if barrier == 'unsupported':
        # The canonical transfer kills every fact at a word it does not model.
        # Kill only what the word can write instead (never gaining a fact):
        d, a, xo = (word >> 21) & 31, (word >> 16) & 31, (word >> 1) & 1023
        after = list(values)
        if op in (2, 3, 19, 59, 63):
            pass  # trap, CR logic / isync, floating-point arithmetic: no GPR or GQR effect
        elif op == 4:
            if (word >> 1) & 63 in (38, 39):  # psq_lux / psq_stux update rA
                after[a] = UNKNOWN
        elif op in (12, 13):
            after[d] = UNKNOWN  # addic / addic.
        elif op == 31 and xo not in (533, 597):
            for register in destinations(word):
                after[register] = UNKNOWN  # update forms: set to the EA below when it is known
        else:
            return (tuple([UNKNOWN] * 40), tuple([None] * 32)), None  # lswi / lswx and anything else
    after = list(after)
    if form and form[2]:
        after[form[5]] = constant(update_ea) if update_ea is not None else UNKNOWN
    if load_update is not None:
        after[load_update[0]] = constant(load_update[1])
    if op in (32, 33) and _PIPE_POINTERS:  # lwz from a data word holding a pipe base
        base = constant(0) if ((word >> 16) & 31) == 0 and op == 32 else values[(word >> 16) & 31]
        if known(base):
            held = _PIPE_POINTERS.get((base[1] + signed(word, 16)) & U32)
            if held is not None:
                after[(word >> 21) & 31] = constant(held)
    # value tags
    s, a = (word >> 21) & 31, (word >> 16) & 31
    written = destinations(word)
    after_tags = [None] * 32 if written is None else list(tags)
    for register in (written or ()):
        after_tags[register] = None
    field_store = None

    def gx_offset(register):
        if register == 0:
            return None
        if tags[register] and tags[register][0] == 'gxptr':
            return tags[register][1]
        value = values[register]
        if known(value) and GX_DATA <= value[1] < GX_DATA + GX_DATA_SIZE:
            return value[1] - GX_DATA
        return None
    if op in (32, 33):  # lwz / lwzu
        base = constant(0) if a == 0 and op == 32 else values[a]
        ea = (base[1] + signed(word, 16)) & U32 if known(base) else None
        if ea == GX_POINTER_SLOT:
            after_tags[s] = GXPTR
        elif gx_offset(a) is not None:
            after_tags[s] = ('gxfield', gx_offset(a) + signed(word, 16))
    elif op == 14 and gx_offset(a) is not None:  # addi rD, gx+k, imm
        after_tags[s] = ('gxptr', gx_offset(a) + signed(word, 16))
    elif op == 14 and a != 0 and word & 0xFFFF == 0:  # addi rD, rA, 0: a move
        after_tags[s] = tags[a]
    elif op == 21 and (word >> 11) & 31 == 0:  # rlwinm rA, rS, 0, MB, ME keeping bits 0-7
        mb, me = (word >> 6) & 31, (word >> 1) & 31
        if mb == 0 and me >= 7 and tags[s] and tags[s][0] == 'gxfield':
            after_tags[a] = tags[s]
    elif op == 31 and (word >> 1) & 1023 == 444 and s == (word >> 11) & 31:  # mr rA, rS
        after_tags[a] = tags[s]
    elif op == 20:  # rlwimi rA, rS, SH, MB, ME: the top byte survives when the mask spares bits 0-7
        mb, me = (word >> 6) & 31, (word >> 1) & 31
        spares_top = mb <= me and mb >= 8
        after_tags[a] = tags[a] if spares_top and tags[a] and tags[a][0] == 'gxfield' else None
    if op == 36 and gx_offset(a) is not None:  # stw rS, off(gx + k)
        field_store = (gx_offset(a) + signed(word, 16), values[s], tags[s])
    if pin_sda:
        for register, value in SDA_BASES.items():
            after[register] = constant(value)  # GX classifier assumes fixed EABI SDA bases
    return (tuple(after), tuple(after_tags)), field_store


def meet_values(left, right):
    return tuple((l[0] & r[0] & ~(l[1] ^ r[1]) & U32, l[1] & (l[0] & r[0] & ~(l[1] ^ r[1]) & U32))
                 for l, r in zip(left, right))


def meet_state(left, right):
    return (meet_values(left[0], right[0]),
            tuple(x if x == y else None for x, y in zip(left[1], right[1])))


def successors(word, pc, jump_tables, entry, entries):
    """Intra-procedural successors: fall-through, direct branches, jump tables.
    An unconditional branch to another declared function's entry is a tail
    call: no successor, reported as a call (argument propagation)."""
    flow = decode(word, pc)
    if flow is None or (word >> 26) == 17:  # sc returns to the next word
        return [pc + 4], None
    if flow.get('return_like'):
        return [], None
    result = []
    if flow.get('link'):
        result.append(pc + 4)
        return result, flow.get('target')
    target = flow.get('target')
    if target is not None and not flow.get('conditional') and target != entry and target in entries:
        return [], target
    if target is not None:
        result.append(target)
    elif flow.get('target_source') == 'CTR' and pc in jump_tables:
        result.extend(jump_tables[pc])
    if flow.get('may_fall_through'):
        result.append(pc + 4)
    return result, None


class Image:
    """The DOL as words (text) and big-endian data words (jump tables)."""

    def __init__(self, raw):
        self.dol = Dol(raw)
        self.raw = raw

    def text(self, pc):
        return self.dol.contains_text(pc, 4)

    def word(self, pc):
        return self.dol.word(pc)

    def data32(self, address):
        for section in self.dol.sections:
            if section['address'] <= address and address + 4 <= section['address'] + section['size']:
                return struct.unpack_from('>I', self.raw, section['file_offset'] + address - section['address'])[0]
        raise ValueError(f'no DOL word at {hx(address)}')


def analyse_procedure(read_word, is_text, entry, jump_tables, entry_values=None, entries=frozenset(),
                      pin_sda=True, seed_sda=True):
    """Solve one procedure. Returns its words, pre-states, GXBegin dominance, calls
    and its stores to __GXData fields."""
    initial = list(entry_values) if entry_values is not None else [UNKNOWN] * 40
    if seed_sda:
        for register, value in SDA_BASES.items():
            initial[register] = constant(value)
    states = {entry: (tuple(initial), tuple([None] * 32))}
    words = {}
    edges = defaultdict(list)
    calls = {}
    field_stores = {}
    pending = deque([entry])
    queued = {entry}
    while pending:
        pc = pending.popleft()
        queued.discard(pc)
        if pc not in words:
            words[pc] = read_word(pc)
        word = words[pc]
        after, field_store = step(word, states[pc], pc, pin_sda=pin_sda)
        if field_store is not None:
            field_stores[pc] = field_store
        targets, callee = successors(word, pc, jump_tables, entry, entries)
        if callee is not None:
            calls[pc] = callee
        for target in targets:
            if not is_text(target):
                continue
            if target not in edges[pc]:
                edges[pc].append(target)
            old = states.get(target)
            merged = after if old is None else meet_state(old, after)
            if old != merged:
                states[target] = merged
                if target not in queued:
                    pending.append(target)
                    queued.add(target)
    # Must-analysis: has every path from the entry passed a GXBegin call?
    predecessors = defaultdict(list)
    for source, targets in edges.items():
        for target in targets:
            predecessors[target].append(source)
    begun = {pc: pc != entry for pc in words}
    changed = True
    while changed:
        changed = False
        for pc in sorted(words):
            if pc == entry:
                continue
            value = all(begun[p] or calls.get(p) == GX_BEGIN for p in predecessors[pc]) if predecessors[pc] else False
            if value != begun[pc]:
                begun[pc] = value
                changed = True
    return words, states, begun, calls, field_stores


def pipe_rows(words, states, begun, root):
    rows = []
    for pc in sorted(words):
        word = words[pc]
        form = store_form(word)
        if form is None:
            continue
        ea = effective_address(form, states[pc][0])
        if ea is None:
            continue
        page = ea & PAGE_MASK
        if page != PIPE_PAGE and page not in PIPE_ALIASES:
            continue
        name, width, update, domain, s, _, _, _ = form
        value = states[pc][0][s] if domain == 'gpr' else UNKNOWN
        tag = states[pc][1][s] if domain == 'gpr' else None
        rows.append(dict(site=pc, word=word, root=root, mnemonic=name, width=width, update=update,
                         domain=domain, ea=ea, alias=PIPE_ALIASES.get(page), value_mask=value[0],
                         value_bits=value[1], value_tag=tag, after_gx_begin=begun[pc]))
    return rows


def argument_facts(words, states, calls):
    """(callee, register, value) for pipe-window values passed in r3-r10."""
    facts = set()
    for pc, callee in calls.items():
        for register in ARGUMENT_GPRS:
            value = states[pc][0][register]
            if known(value) and ARGUMENT_WINDOW[0] <= value[1] < ARGUMENT_WINDOW[1]:
                facts.add((callee, register, value[1]))
    return facts


_IMAGE = None
_JUMP_TABLES = None
_ENTRIES = frozenset()


def _init_worker(raw, jump_tables, entries, pipe_pointers=None):
    global _IMAGE, _JUMP_TABLES, _ENTRIES
    _IMAGE = Image(raw)
    _JUMP_TABLES = jump_tables
    _ENTRIES = frozenset(entries)
    _PIPE_POINTERS.clear()
    _PIPE_POINTERS.update(pipe_pointers or {})


def pipe_pointer_words(image):
    """Initialised data words holding a pipe base."""
    found = {}
    for section in image.dol.sections:
        if section['kind'] != 'data':
            continue
        for offset in range(0, section['size'] - 3, 4):
            value = struct.unpack_from('>I', image.raw, section['file_offset'] + offset)[0]
            if value in PIPE_BASES:
                found[section['address'] + offset] = value
    return found


def store_eas(words, states):
    """Every exact store effective address of a procedure (for the pipe-pointer proof)."""
    result = set()
    for pc, word in words.items():
        form = store_form(word)
        if form is None:
            continue
        ea = effective_address(form, states[pc][0])
        if ea is not None:
            result.add((pc, ea, form[1] or 4))
    return result


def _solve(task):
    entry, entry_registers = task
    entry_values = None
    if entry_registers:
        entry_values = [UNKNOWN] * 40
        for register, value in entry_registers:
            entry_values[register] = constant(value)
    words, states, begun, calls, field_stores = analyse_procedure(_IMAGE.word, _IMAGE.text, entry, _JUMP_TABLES,
                                                                  entry_values, _ENTRIES)
    fields = sorted((pc, off, value[0] >> 24 == 0xFF, value[1] >> 24, tag)
                    for pc, (off, value, tag) in field_stores.items())
    pointer_writes = sorted((pc, ea) for pc, ea, width in store_eas(words, states)
                            if any(ea < pointer + 4 and pointer < ea + width for pointer in _PIPE_POINTERS))
    return (entry, entry_registers, pipe_rows(words, states, begun, entry),
            sorted(argument_facts(words, states, calls)), fields, pointer_writes)


def classify(sites, bp_payload_of, indirect_writers=frozenset()):
    """Family per site; returns {site: (family, reason)}."""
    families = {}
    for site, row in sites.items():
        sdk = GX_SDK[0] <= row['function'] < GX_SDK[1]
        if row['alias']:
            families[site] = ('unknown', row['alias'])
            continue
        if row['function'] == GX_BEGIN and row['mnemonic'] == 'stb':
            families[site] = ('primitive_opcode', "GXBegin's header byte")
            continue
        if row['function'] == GX_BEGIN and row['mnemonic'] == 'sth':
            families[site] = ('primitive_count', "GXBegin's vertex count")
            continue
        if sdk:
            payload = bp_payload_of.get(site)
            if payload is not None:
                register, how = payload
                if register == 'unknown':
                    if row['function'] in LIVE_BP_FUNCTIONS:
                        families[site] = ('unknown', f'SDK BP payload in {LIVE_BP_FUNCTIONS[row["function"]]}: {how}')
                    else:
                        families[site] = ('dead', f'dead_by_api_purpose: BP payload of a state setter; {how}')
                elif isinstance(register, frozenset):
                    families[site] = ('dead', f'BP payload, state register in {sorted(register)} ({how})')
                elif register in BP_FAMILIES:
                    families[site] = (BP_FAMILIES[register], f'BP 0x{register:02X} payload ({how})')
                else:
                    families[site] = ('dead', f'BP 0x{register:02X} payload, state ({how})')
                continue
            families[site] = ('dead', 'GX SDK transport')
            continue
        if row['after_gx_begin'] is True:
            families[site] = ('vertex', 'client store after GXBegin on every path')
        elif row['function'] in indirect_writers:
            families[site] = ('vertex', 'PTank vertex writer, called through +0x28 after GXBegin '
                                        f"(bctrl at {hx(INDIRECT_WRITER_CALL['site'])})")
        elif row['after_gx_begin'] is False:
            families[site] = ('unknown', 'client pipe store not preceded by GXBegin on every path')
        else:
            families[site] = ('unknown', 'GXBegin dominance differs between roots')
    return families


def field_registers(field_rows):
    """__GXData field offset -> set of BP register bytes stored to it (known top
    bytes), resolving fields copied from another field."""
    direct, copies = defaultdict(set), defaultdict(set)
    for _, off, top_known, top, tag in field_rows:
        if top_known:
            direct[off].add(top)
        elif tag and tag[0] == 'gxfield':
            copies[off].add(tag[1])
        else:
            direct[off].add('unknown')
    resolved = {}

    def resolve(off, seen=()):
        if off in resolved:
            return resolved[off]
        result = set(direct.get(off, ()))
        for source in copies.get(off, ()):
            if source not in seen:
                result |= resolve(source, seen + (off,))
        resolved[off] = result
        return result
    for off in set(direct) | set(copies):
        resolve(off)
    return resolved


def bp_payloads(sites, image, fields):
    """BP payload stores: a 4-byte GPR pipe store whose closest preceding pipe
    store of the same function is `stb` of the constant 0x61 with no branch
    word between them. The register byte: the payload's top 8 bits when known
    on every path; else, for a payload loaded from a __GXData field with its
    top byte intact, the one register byte the SDK stores to that field; else
    'unknown'. Returns {site: (register or 'unknown', how)}."""
    result = {}
    by_function = defaultdict(list)
    for site, row in sites.items():
        by_function[row['function']].append(site)
    for members in by_function.values():
        members.sort()
        for left, site in zip(members, members[1:]):
            row, previous = sites[site], sites[left]
            if row['mnemonic'] != 'stw' or previous['mnemonic'] != 'stb':
                continue
            if previous['value_mask'] & 0xFF != 0xFF or previous['value_bits'] & 0xFF != BP_OPCODE:
                continue
            if any(decode(image.word(pc), pc) is not None for pc in range(left + 4, site, 4)):
                continue
            if row['value_mask'] >> 24 == 0xFF:
                result[site] = (row['value_bits'] >> 24, 'constant top byte')
                continue
            tag = row['value_tag']
            if tag and tag[0] == 'gxfield':
                registers = fields.get(tag[1], set())
                if len(registers) == 1 and 'unknown' not in registers:
                    result[site] = (next(iter(registers)), f'__GXData field +0x{tag[1]:X}')
                    continue
                if registers and 'unknown' not in registers and not (registers & LIVE_BP):
                    result[site] = (frozenset(registers), f'__GXData field +0x{tag[1]:X}')
                    continue
                result[site] = ('unknown', f'__GXData field +0x{tag[1]:X} holds {sorted(map(str, registers))}')
                continue
            result[site] = ('unknown', 'register byte neither constant nor a __GXData field')
    return result


def address_references(image, targets):
    """Every lis + addi / ori pair and every data word in the image that forms
    one of `targets`: {target: [(kind, site, result register or None)]}."""
    refs = defaultdict(list)
    for section in image.dol.text:
        high = {}
        for pc in range(section['address'], section['address'] + section['size'], 4):
            word = image.word(pc)
            op, d, a = word >> 26, (word >> 21) & 31, (word >> 16) & 31
            if op == 15 and a == 0:
                high[d] = (word & 0xFFFF) << 16
                continue
            if op in (14, 24) and a in high:
                low = word & 0xFFFF
                value = (high[a] + (signed(low, 16) if op == 14 else low)) & U32
                if value in targets:
                    refs[value].append(('code', pc, d if op == 14 else a))
            if decode(word, pc) is not None:
                high = {}
    for section in image.dol.sections:
        if section['kind'] != 'data':
            continue
        for offset in range(0, section['size'] - 3, 4):
            value = struct.unpack_from('>I', image.raw, section['file_offset'] + offset)[0]
            if value in targets:
                refs[value].append(('data', section['address'] + offset, None))
    return refs


def indirect_vertex_writers(image, sites, jump_tables, entries):
    """Client functions with pipe stores and no GXBegin of their own that the
    PTank render callback calls through +0x28 after GXBegin. Returns
    (writers, evidence)."""
    spec = INDIRECT_WRITER_CALL
    evidence = dict(call_site=hx(spec['site']), caller=hx(spec['caller']), field=hx(spec['field']),
                    selector=hx(spec['selector']), checks={})
    candidates = sorted({r['function'] for r in sites.values()
                         if not (GX_SDK[0] <= r['function'] < GX_SDK[1]) and r['after_gx_begin'] is False})
    # the call site: bctrl, reached through GXBegin, CTR from lwz _, field(_)
    words, states, begun, calls, _ = analyse_procedure(image.word, image.text, spec['caller'], jump_tables,
                                                       entries=entries)
    call_ok = image.word(spec['site']) == 0x4E800421 and begun.get(spec['site']) is True
    source, pc = None, spec['site'] - 4
    while pc >= spec['caller'] and source is None:
        word = image.word(pc)
        if word >> 26 == 31 and (word >> 1) & 1023 == 467 and ((word >> 16) & 31 | ((word >> 11) & 31) << 5) == 9:
            source = (word >> 21) & 31  # mtctr rS
        pc -= 4
    chain_ok = False
    while source is not None and pc >= spec['caller']:
        word = image.word(pc)
        op, d, a = word >> 26, (word >> 21) & 31, (word >> 16) & 31
        if op == 31 and (word >> 1) & 1023 == 444 and a == source and d == (word >> 11) & 31:
            source = d  # mr source, d
        elif op == 32 and d == source:
            chain_ok = signed(word, 16) == spec['field']
            break
        elif d == source and op not in (36, 37, 38, 39, 44, 45, 52, 53, 54, 55, 47, 60, 61):
            break  # redefined by something else
        pc -= 4
    evidence['checks']['call_site_bctrl_after_gx_begin'] = call_ok
    evidence['checks']['ctr_loaded_from_field'] = chain_ok
    writers = set()
    if not (call_ok and chain_ok):
        return writers, evidence
    refs = address_references(image, set(candidates))
    per_writer = {}
    for function in candidates:
        rows = refs.get(function, [])
        ok = bool(rows)
        for kind, site, register in rows:
            if kind != 'code' or not (spec['selector'] <= site < spec['selector'] + 0x400):
                ok = False
                break
            store = image.word(site + 4)
            if not (store >> 26 == 36 and (store >> 21) & 31 == register and signed(store, 16) == spec['field']):
                ok = False
                break
        has_begin = any(image.word(pc) & 0xFC000003 == 0x48000001 and
                        (pc + signed(image.word(pc) & 0x03FFFFFC, 26)) & U32 == GX_BEGIN
                        for pc in range(function, function + 0x1000, 4)
                        if image.text(pc) and pc < next((e for e in sorted(entries) if e > function), function + 0x1000))
        per_writer[hx(function)] = dict(references=[dict(kind=k, site=hx(s)) for k, s, _ in rows],
                                        all_references_store_the_field=ok, calls_gx_begin=has_begin)
        if ok and not has_begin:
            writers.add(function)
    evidence['writers'] = per_writer
    return writers, evidence


def read_jump_tables(path, image):
    tables = {}
    for line in path.read_text(encoding='utf-8').splitlines():
        fields = line.split()
        if not fields or fields[0].startswith('#'):
            continue
        site, base, count = int(fields[0], 16), int(fields[1], 16), int(fields[2], 10)
        tables[site] = [image.data32(base + 4 * i) for i in range(count)]
    return tables


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def run(dol, functions, jump_tables_path, workers=1):
    raw = Path(dol).read_bytes()
    image = Image(raw)
    if image.data32(GX_POINTER_SLOT) != GX_DATA:
        raise SystemExit(f'the __GXData pointer slot {hx(GX_POINTER_SLOT)} does not hold {hx(GX_DATA)} in this DOL')
    roots = sorted({row['address'] for row in table(Path(functions).read_bytes(), 'functions', image.dol)})
    jump_tables = read_jump_tables(Path(jump_tables_path), image)
    pointers = pipe_pointer_words(image)
    rows, facts_done, tasks, field_rows = [], set(), [(entry, ()) for entry in roots], []
    pointer_writes = set()
    rounds = 0
    executor = ProcessPoolExecutor(max_workers=workers, initializer=_init_worker,
                                   initargs=(raw, jump_tables, roots, pointers)) if workers > 1 else None
    if executor is None:
        _init_worker(raw, jump_tables, roots, pointers)
    try:
        while tasks:
            rounds += 1
            results = executor.map(_solve, tasks, chunksize=64) if executor else map(_solve, tasks)
            new_facts = defaultdict(set)
            for entry, registers, found, facts, fields, writes in results:
                field_rows.extend(fields)
                pointer_writes.update(writes)
                rows.extend(dict(r, entry_registers=[[g, hx(v)] for g, v in registers]) for r in found)
                for callee, register, value in facts:
                    key = (callee, register, value)
                    if key not in facts_done and image.text(callee):
                        facts_done.add(key)
                        new_facts[callee].add((register, value))
            tasks = [(callee, tuple(sorted(registers))) for callee, registers in sorted(new_facts.items())]
            if rounds > 8:
                raise RuntimeError('argument propagation did not reach a fixed point in 8 rounds')
    finally:
        if executor:
            executor.shutdown()
    return image, roots, jump_tables, rows, sorted(facts_done), rounds, field_rows, pointers, sorted(pointer_writes)


def function_of(site, roots):
    index = bisect.bisect_right(roots, site) - 1
    return roots[index] if index >= 0 else None


def merge_sites(rows, roots):
    sites = {}
    for row in rows:
        site = row['site']
        merged = sites.get(site)
        if merged is None:
            merged = dict(site=site, word=row['word'], function=function_of(site, roots), mnemonic=row['mnemonic'],
                          width=row['width'], update=row['update'], domain=row['domain'], ea=set(), alias=row['alias'],
                          value_mask=row['value_mask'], value_bits=row['value_bits'], value_tag=row['value_tag'],
                          after_gx_begin=row['after_gx_begin'], roots=set())
            sites[site] = merged
        merged['ea'].add(row['ea'])
        merged['roots'].add(row['root'])
        if merged['after_gx_begin'] != row['after_gx_begin']:
            merged['after_gx_begin'] = None
        if merged['value_tag'] != row['value_tag']:
            merged['value_tag'] = None
        mask = merged['value_mask'] & row['value_mask'] & ~(merged['value_bits'] ^ row['value_bits']) & U32
        merged['value_mask'], merged['value_bits'] = mask, merged['value_bits'] & mask
    return sites


def report(dol, functions, jump_tables_path, census=None, workers=1):
    image, roots, jump_tables, rows, facts, rounds, field_rows, pointers, pointer_writes = run(
        dol, functions, jump_tables_path, workers)
    sites = merge_sites(rows, roots)
    fields = field_registers(field_rows)
    payloads = bp_payloads(sites, image, fields)
    writers, writer_evidence = indirect_vertex_writers(image, sites, jump_tables, frozenset(roots))
    families = classify(sites, payloads, writers)
    for site, (family, reason) in families.items():
        sites[site]['family'], sites[site]['reason'] = family, reason
    counts = Counter(r['family'] for r in sites.values())
    by_function = defaultdict(Counter)
    for r in sites.values():
        by_function[r['function']][r['family']] += 1
    census_check = None
    if census is not None:
        data = json.loads(Path(census).read_text(encoding='utf-8'))
        exact = {int(s['site'], 16) for s in data['sites']
                 if any(o.get('subregion') == 'GX_FIFO' for o in s['outcomes'])}
        census_check = dict(census_exact_pipe_sites=len(exact), found_again=len(exact & set(sites)),
                            census_sites_missed=[hx(s) for s in sorted(exact - set(sites))],
                            new_sites_beyond_census=len(set(sites) - exact),
                            new_site_list=[hx(s) for s in sorted(set(sites) - exact)])
    serial = []
    for site in sorted(sites):
        r = sites[site]
        serial.append(dict(site=hx(site), word=hx(r['word']), function=hx(r['function']), mnemonic=r['mnemonic'],
                           width=r['width'], update=r['update'], domain=r['domain'],
                           ea=[hx(e) for e in sorted(r['ea'])], alias=r['alias'],
                           value_known_mask=hx(r['value_mask']), value_known_bits=hx(r['value_bits']),
                           value_source=None if not r['value_tag'] else
                           (f"gx pointer +0x{r['value_tag'][1]:X}" if r['value_tag'][0] == 'gxptr'
                            else f"__GXData+0x{r['value_tag'][1]:X}"),
                           after_gx_begin=r['after_gx_begin'], roots=[hx(x) for x in sorted(r['roots'])],
                           family=r['family'], reason=r['reason']))
    return dict(
        schema='gx_site_inventory_v1',
        inputs=dict(dol=dict(path=str(dol), sha256=sha(dol)), functions=dict(path=str(functions), sha256=sha(functions)),
                    jump_tables=dict(path=str(jump_tables_path), sha256=sha(jump_tables_path)),
                    tool=dict(path=str(Path(__file__).resolve()), sha256=sha(__file__))),
        assumptions=dict(call_rule='EABI: r1, r2, r13, r14-r31 survive bl / bctrl / sc; r0, r3-r12 and GQRs do not',
                         sda_bases='r2 = 0x805FA780 and r13 = 0x805EC500 at every word (set once, never written)',
                         gx_fields='a BP payload loaded from a __GXData field carries the register byte the SDK '
                                   'stores to that field (known top bytes of every stw to gx+off, image-wide)',
                         memory='memory contents never read; a pipe pointer loaded from memory is not seen',
                         pipe_pointers='a lwz from an initialised data word holding a pipe base yields it; '
                                       'no exact store EA in the image may touch such a word (reported)',
                         argument_propagation=f'pipe-window values in r3-r10 propagated into direct callees '
                                              f'({len(facts)} facts, {rounds} rounds)',
                         backstop='the lowered build stops on any store to the pipe page not in the list'),
        roots=len(roots), jump_tables=len(jump_tables),
        summary=dict(sites=len(sites), families=dict(sorted(counts.items())),
                     functions=len(by_function),
                     forms=dict(sorted(Counter(r['mnemonic'] for r in sites.values()).items()))),
        census=census_check,
        indirect_vertex_writers=writer_evidence,
        dead_by_api_purpose=sorted(s for s, (f, why) in families.items() if why.startswith('dead_by_api_purpose')
                                   and False) or [hx(s) for s, (f, why) in sorted(families.items())
                                                  if why.startswith('dead_by_api_purpose')],
        argument_facts=[dict(callee=hx(c), register=g, value=hx(v)) for c, g, v in facts],
        gx_field_registers={f'0x{off:X}': sorted(map(str, regs)) for off, regs in sorted(fields.items())},
        pipe_pointer_words=dict(words={hx(a): hx(v) for a, v in sorted(pointers.items())},
                                exact_stores_to_them=[dict(site=hx(pc), ea=hx(ea)) for pc, ea in pointer_writes]),
        functions=[dict(function=hx(f), families=dict(sorted(c.items()))) for f, c in sorted(by_function.items())],
        sites=serial)


def write_list(data, path):
    unknown = [s for s in data['sites'] if s['family'] == 'unknown']
    if unknown:
        raise SystemExit(f'{len(unknown)} sites are unknown; the list is not written (first {unknown[0]["site"]})')
    lines = ['# GX write-gather pipe sites for ppcrecomp --gx-sites: site word family [note].',
             '# docs/native-time-20261005/GX_DATA_PLANE.md: every store of the main DOL to the pipe page',
             '# (EA 0xCC008000) and the family the generator lowers it to. Generated by',
             '# tools/gx_site_inventory.py; do not edit (regenerate: the list pins DOL words).']
    for s in data['sites']:
        lines.append(f"0x{int(s['site'], 16):08X} 0x{int(s['word'], 16):08X} {s['family']}  "
                     f"fn 0x{int(s['function'], 16):08X} {s['mnemonic']}: {s['reason']}")
    Path(path).write_text('\n'.join(lines) + '\n', encoding='utf-8')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--dol', type=Path, required=True)
    parser.add_argument('--functions', type=Path, required=True)
    parser.add_argument('--jump-tables', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--census', type=Path)
    parser.add_argument('--write-list', type=Path)
    parser.add_argument('--workers', type=int, default=1)
    args = parser.parse_args(argv)
    data = report(args.dol, args.functions, args.jump_tables, args.census, args.workers)
    args.out.write_text(json.dumps(data, indent=1) + '\n', encoding='utf-8')
    print(json.dumps(dict(summary=data['summary'], census={k: v for k, v in (data['census'] or {}).items()
                                                            if not k.endswith('_list')}), indent=1))
    if args.write_list:
        write_list(data, args.write_list)
        print('wrote', args.write_list)
    return 0


if __name__ == '__main__':
    sys.exit(main())
