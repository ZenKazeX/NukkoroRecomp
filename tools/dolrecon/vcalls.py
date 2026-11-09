"""Typed virtual calls: which class's slot a method calls on its own object (this).

    python -B -m dolrecon.vcalls <named project> <out dir>      (run from source/tools)

The type of `this` is known only where the code proves it:
  ctor   a function that stores class C's primary vtable address at this+K gives C's vptr offset K (constructors,
         and destructors restoring it, all store at the same K). A class whose stores disagree on K gets no offset.
  method a body in a primary vtable slot receives this = the owning class (the holder all other holders descend
         from, as classroles.assign); its this is that class at offset 0.
A call of slot s on that class is proven by def-use inside one block: `lwz r12,K(rX)` with rX holding this and K the
class's vptr offset, `lwz r12,8+4s(r12)`, `mtctr r12`, `bctrl`, with no other write of r12 or CTR, no branch and no
block boundary in between, and r3 still holding this at the bctrl. The slot belongs to the topmost offset-0
ancestor whose vtable has a slot s (the class that introduced it).

Registers holding this, constants, stack / sda / allocation addresses and this->field loads are tracked by a forward
must-analysis over the function's blocks (dolrecon.ppc decodes every instruction's GPR writes; an undecoded one drops
all facts; a call clobbers r0, r3-r12). Control flow: direct branches (AA honoured), jump tables (`lwzx` of a
constant table, then mtctr / bctr: every table word inside the function is an edge), other bctr / bclr leave the
function; interior entries (direct branch targets from other functions) start with nothing known. Facts are
reported once, at the fixed point. This only finds dispatchers; it names nothing. Writes <out>/vcalls.json.
"""
from __future__ import annotations

import argparse
import bisect
import collections
import json
import os
import sys

from . import corrections as corrmod
from . import dol as dolmod
from . import ppc
from . import spine

fields, simm, is_mr, VOLATILE = ppc.fields, ppc.simm, ppc.is_mr, ppc.VOLATILE
MTCTR_R12, BCTRL = 0x7D8903A6, 0x4E800421


def interior_entries(image, funcs):
    """{function: {addresses inside it that a direct branch from ANOTHER function targets}}."""
    starts = sorted(a for a, _, s, sec in funcs if s and sec.startswith(".text"))
    size = {a: s for a, _, s, _ in funcs}
    out = collections.defaultdict(set)
    for a in starts:
        for i in range(size[a] // 4):
            pc = a + 4 * i
            br = ppc.branch(image.word(pc), pc)
            if br is None or br.target is None or a <= br.target < a + size[a]:
                continue
            k = bisect.bisect_right(starts, br.target) - 1
            if k >= 0 and starts[k] < br.target < starts[k] + size[starts[k]]:
                out[starts[k]].add(br.target)
    return out


class Function:
    def __init__(self, image, start, size, entries=()):
        self.image, self.start, self.size = image, start, size
        self.words = [image.word(start + 4 * i) for i in range(size // 4)]
        self.entries = {e for e in entries if start < e < start + size and e % 4 == 0}
        self.returns = {}           # call target -> "new" (r3 = a fresh object), "receiver" (r3 = the r3 passed) or
                                    # ("class", C): r3 = an object of class C or one deriving from it (dolrecon.receivers)
        self.loads = False          # True: lwz rD, d(constant or r2 / r13) gives ("load", address or (sdaN, d))
        self.statics = False        # True: also global facts for static storage (see GLOBAL_FACTS)
        self.sda = {}               # {2: r2, 13: r13} when known: memory addresses through them become ints
        self.leaders = set()

    def jump_table(self, i):
        """Targets inside this function of the jump table a bctr at word i dispatches through ([] = it leaves)."""
        src, j = None, i - 1
        while j >= 0 and ppc.branch(self.words[j], self.start + 4 * j) is None and i - j <= 16:
            w = self.words[j]
            if src is None and w & 0xFC1FFFFF == 0x7C0903A6:          # mtctr rS
                src = (w >> 21) & 31
            elif src is not None and w >> 26 == 31 and (w >> 1) & 0x3FF == 23 and (w >> 21) & 31 == src:
                bases = {(w >> 16) & 31, (w >> 11) & 31}                # lwzx rS, rA, rB: one of them the table
                table = self.constant_before(j, bases)
                if table is None:
                    return []
                end, out = self.start + self.size, []
                for k in range(512):
                    t = self.image.word(table + 4 * k) if self.image.section_at_address(table + 4 * k) else None
                    if t is None or not self.start <= t < end or t % 4:
                        break
                    out.append(t)
                return out
            j -= 1
        return []

    def constant_before(self, j, regs):
        """The constant (lis / addi / ori) one of regs holds at word j, scanning the straight-line run before it."""
        val, k = {}, j - 1
        while k > 0 and ppc.branch(self.words[k - 1], self.start + 4 * (k - 1)) is None and j - k < 16:
            k -= 1
        for m in range(max(k, 0), j):
            w = self.words[m]
            op, d, a, _, imm = fields(w)
            new = None
            if op == 15 and a == 0:
                new = (d, imm << 16)
            elif op == 14 and a in val:
                new = (d, (val[a] + simm(imm)) & 0xFFFFFFFF)
            elif op == 24 and d in val:
                new = (a, val[d] | imm)
            written = ppc.gpr_writes(w)
            for r in range(32) if written is None else written:
                val.pop(r, None)
            if new:
                val[new[0]] = new[1]
        return next((val[r] for r in regs if r in val), None)

    def blocks(self):
        """{block start: (end, successors)}."""
        end = self.start + self.size
        leaders, tables = {self.start} | self.entries, {}
        for i, w in enumerate(self.words):
            pc = self.start + 4 * i
            br = ppc.branch(w, pc)
            if br is None:
                continue
            leaders.add(pc + 4)
            if br.target is not None and self.start <= br.target < end:
                leaders.add(br.target)
            if br.kind == "bcctr" and not br.link:
                tables[pc] = self.jump_table(i)
                leaders.update(tables[pc])
        out, order = {}, sorted(a for a in leaders if a < end)
        for k, s in enumerate(order):
            e = order[k + 1] if k + 1 < len(order) else end
            pc = e - 4
            br = ppc.branch(self.words[(pc - self.start) // 4], pc)
            nxt = [e] if e < end else []
            if br is None or br.link:
                succ = nxt
            elif br.kind in ("b", "bc"):
                succ = ([br.target] if self.start <= br.target < end else []) + ([] if br.always else nxt)
            elif br.kind == "bcctr":
                succ = tables.get(pc, []) + ([] if br.always else nxt)
            else:                                                     # bclr: return
                succ = [] if br.always else nxt
            out[s] = (e, list(dict.fromkeys(succ)))
        self.leaders = set(out)
        return out

    def scan_values(self, entry_values, visit):
        """scan() with no this register and these registers known at entry (r2 / r13: the small-data bases, which
        no call changes)."""
        self.scan((), visit, entry_values)

    def scan(self, entry_this, visit, entry_values=None):
        """Run the must-analysis to its fixed point, then call visit(pc, word, this_regs, values) once for every
        instruction of every reached block with the state that holds there on all paths."""
        blocks = self.blocks()
        state = {self.start: (frozenset(entry_this), dict(entry_values or {}))}
        for e in self.entries:
            state[e] = (frozenset(), {r: v for r, v in (entry_values or {}).items() if r in (2, 13)})
        work = list(state)
        done = {}
        while work:
            s = work.pop()
            regs, values = state[s]
            key = (regs, tuple(sorted(values.items(), key=repr)))
            if done.get(s) == key:
                continue
            done[s] = key
            regs_out, values_out = self.run_block(s, blocks[s][0], regs, values, None)
            for t in blocks[s][1]:
                values_t = edge_values(self, blocks[s][0], t, values_out)
                if t not in state:
                    state[t] = (frozenset(regs_out), values_t)
                else:
                    pr, pv = state[t]
                    state[t] = (pr & frozenset(regs_out),
                                {r: m for r in pv.keys() & values_t.keys()
                                 if (m := merge_value(r, pv[r], values_t[r])) is not None})
                work.append(t)
        if visit:
            for s in sorted(state):
                self.run_block(s, blocks[s][0], *state[s], visit)

    def run_block(self, s, e, regs, consts, visit):
        """The registers holding this and the known values after the block [s, e)."""
        regs, consts = set(regs), dict(consts)
        for pc in range(s, e, 4):
            w = self.words[(pc - self.start) // 4]
            if visit:
                visit(pc, w, regs, consts)
            ok, a, src = is_mr(w)
            op, d, ra, _, imm = fields(w)
            if ok:
                (regs.add if src in regs else regs.discard)(a)
                if src in consts:
                    consts[a] = consts[src]
                else:
                    consts.pop(a, None)
                if w & 1:                                           # mr.: cr0 = the copy compared with 0
                    drop_cr0(consts)
                    v = consts.get(a)
                    if isinstance(v, tuple) and v[0] == "new":      # operator new's result tested against null
                        consts["cr0"] = ("zero", ("v", v))
                continue
            cr0 = writes_cr0(w)
            flag = consts.get(d) if op == 31 else consts.get(ra)
            flag = flag_source(flag)
            ztag = zero_test(w, op, d, ra, imm, consts) if self.statics else None     # before the write
            newc = None
            base = consts.get(ra)
            # a D-form memory access: rA = 0 is the literal 0, not r0 (Astra answer 6); r2 / r13 as given
            at = None
            if op in MEMORY_D:
                mbase = 0 if ra == 0 else self.sda.get(ra, consts.get(ra))
                if isinstance(mbase, int):
                    at = (mbase + simm(imm)) & 0xFFFFFFFF
                elif ra in (2, 13):
                    at = (f"sda{ra}", simm(imm))
            if op == 15 and ra == 0:
                newc = (d, imm << 16)
            elif op == 14:                                          # addi / li
                if ra == 0:
                    newc = (d, simm(imm) & 0xFFFFFFFF)
                elif isinstance(base, int):
                    newc = (d, (base + simm(imm)) & 0xFFFFFFFF)
                elif ra == 1:
                    newc = (d, ("stack", simm(imm)))
                elif ra in (2, 13):
                    newc = (d, (f"sda{ra}", simm(imm)))
                elif isinstance(base, tuple) and base[0] in ("stack", "new"):
                    newc = (d, base[:-1] + (base[-1] + simm(imm),))
            elif op == 24 and isinstance(consts.get(d), int):       # ori rA,rS,imm (in place too)
                newc = (ra, consts[d] | imm)
            elif op == 32 and ra in regs and ra != 0:               # lwz rD,F(this): the field this->F
                newc = (d, ("field", simm(imm)))
            elif op == 32 and isinstance(base, tuple) and base[0] == "obj" and ra != 0:
                newc = (d, ("ofield", base[1], simm(imm)))          # lwz rD,F(obj): a field of a typed object
            elif op == 32 and self.loads and at is not None:
                if self.statics and isinstance(consts.get(("gval", at)), int):
                    newc = (d, consts[("gval", at)])                # the value stored there on every path
                elif self.statics and ("nz", ("w", at)) in consts:
                    newc = (d, ("load_nz", at))                     # read where it is known not 0
                else:
                    newc = (d, ("load", at))
            elif op == 34 and self.statics and at is not None:      # lbz: a guard byte
                newc = (d, ("loadb", at))
            elif op == 31 and (w >> 1) & 0x3FF == 922 and flag is not None:     # extsh rA,rS of the entry flag
                newc = (ra, ("sext16", flag))
            if op == 36 and ra in regs and ra != 0:                 # stw into this+F: the slot's last value
                v = consts.get(d)
                consts[("mem", simm(imm))] = v if isinstance(v, int) else ("unknown",)
            if self.statics and (op in STORE_BYTES or (op == 31 and (w >> 1) & 0x3FF in X_STORES)):
                store_facts(consts, w, op, d, at if op in STORE_BYTES and ra not in regs else None)
            written = ppc.gpr_writes(w)
            if written is None:                                     # not decoded: nothing is known any more
                regs.clear()
                consts.clear()
            else:
                for r in written:
                    regs.discard(r)
                    consts.pop(r, None)
            if newc:
                consts[newc[0]] = newc[1]
            if cr0 is None:                                         # cr0 / other CR fields: unknown
                drop_cr0(consts, everything=True)
            elif cr0:
                drop_cr0(consts)
                tag = None
                if op == 31 and (w >> 1) & 0x3FF == 922 and flag is not None:  # extsh. of the flag
                    tag = ("sign", flag)
                elif op == 11 and imm == 0 and isinstance(consts.get(ra), tuple) and consts[ra][0] == "sext16":
                    tag = ("sign", consts[ra][1])                   # cmpwi (sign-extended flag), 0
                elif ztag:
                    tag = ztag
                if tag:
                    consts["cr0"] = tag
            br = ppc.branch(w, pc)
            if br is not None and br.link:
                drop_cr0(consts, everything=True)                   # cr0, cr1, cr5-7 are volatile
                for k in [k for k in consts if isinstance(k, tuple) and k[0] in GLOBAL_FACTS]:
                    del consts[k]                                   # a callee may write any global
                r3_this, r3_val = 3 in regs, consts.get(3)
                regs -= VOLATILE
                for r in list(consts):
                    if r in VOLATILE:
                        del consts[r]
                how = self.returns.get(br.target) if br.target is not None else None
                if how == "new":
                    consts[3] = ("new", pc, 0)
                elif isinstance(how, tuple) and how[0] == "class":   # r3 = an object of a proven class (or derived)
                    consts[3] = ("obj", how[1])
                elif how == "receiver":
                    if r3_this:
                        regs.add(3)
                    if r3_val is not None:
                        consts[3] = r3_val
                        # a path fact (Astra answer 6): which receiver-returning functions ran on this object
                        consts[("built", r3_val)] = consts.get(("built", r3_val), frozenset()) | {br.target}
        return regs, consts


MEMORY_D = {32, 33, 34, 35, 40, 41, 42, 43, 46, 47, 36, 37, 38, 39, 44, 45, 48, 49, 50, 51, 52, 53, 54, 55}
STORE_BYTES = {36: 4, 37: 4, 38: 1, 39: 1, 44: 2, 45: 2, 47: 4, 52: 4, 53: 4, 54: 8, 55: 8}
X_STORES = {151, 183, 215, 247, 407, 439, 663, 695, 727, 759, 662, 918, 150, 983}     # indexed / reversed stores
# Static-storage facts (Function.statics), keyed by address A: ("gval", A) the word last stored there on every path;
# ("nz", ("w" | "b", A)) / ("isz", ...) the word / byte is known non-zero / zero (a store of a non-zero constant,
# or the edge of a test of a loaded value against 0); ("bset", A) a byte store of a non-zero constant. A call, or a
# store through an address the analysis does not know, drops them all.
GLOBAL_FACTS = {"gval", "nz", "isz", "bset"}


def store_facts(consts, w, op, src, at):
    """Update the static-storage facts for a store; at = its address (None: not known)."""
    if at is None or not isinstance(at, int):
        for k in [k for k in consts if isinstance(k, tuple) and k[0] in GLOBAL_FACTS]:
            del consts[k]
        return
    width = 4 * (32 - src) if op == 47 else STORE_BYTES[op]
    for k in [k for k in consts if isinstance(k, tuple) and k[0] in GLOBAL_FACTS]:
        a = k[1][1] if k[0] in ("nz", "isz") else k[1]
        size = 1 if k[0] in ("nz", "isz") and k[1][0] == "b" or k[0] == "bset" else 4
        if isinstance(a, int) and a < at + width and at < a + size:
            del consts[k]
    v = consts.get(src)
    if op == 36 and isinstance(v, int):
        consts[("gval", at)] = v
        if v:
            consts[("nz", ("w", at))] = True
    elif op == 38 and isinstance(v, int) and v & 0xFF:
        consts[("bset", at)] = v & 0xFF
        consts[("nz", ("b", at))] = True


def zero_test(w, op, d, ra, imm, consts):
    """cr0 tag ("zero", ("w" | "b", A)) for `cmplwi / cmpwi rA, 0` of a loaded word, or `extsb.` of a loaded byte."""
    if op in (10, 11) and imm == 0 and (w >> 23) & 7 == 0:
        v = consts.get(ra)
        if isinstance(v, tuple) and v[0] in ("load", "load_nz"):
            return ("zero", ("w", v[1]))
    if op == 31 and (w >> 1) & 0x3FF == 954 and w & 1:
        v = consts.get(d)
        if isinstance(v, tuple) and v[0] == "loadb":
            return ("zero", ("b", v[1]))
    return None


def is_return(w, pc) -> bool:
    """blr, or a conditional bclr (beqlr ...): a reached return when taken (Astra answer 6)."""
    br = ppc.branch(w, pc)
    return br is not None and br.kind == "bclr" and not br.link


FLAG = ("arg", 4)              # the second argument at entry: a destructor's delete flag / a constructor's vbase flag


def flag_source(v):
    """The entry flag a value carries (the flag itself or its sign extension), else None."""
    if v == FLAG:
        return FLAG
    return v[1] if isinstance(v, tuple) and v[0] == "sext16" else None


def writes_cr0(w):
    """True when the instruction sets cr0, False when it leaves the condition register alone, None when it may
    change other CR fields (then nothing about the CR is kept)."""
    op, d = w >> 26, (w >> 21) & 31
    if op in (13, 28, 29):                                         # addic. andi. andis.
        return True
    if op in (10, 11):                                             # cmpli / cmpi
        return d >> 2 == 0 or None
    if op in (20, 21, 23):                                         # rlwimi. rlwinm. rlwnm.
        return bool(w & 1)
    if op == 31:
        xo = (w >> 1) & 0x3FF
        if xo in (0, 32):                                          # cmp / cmpl
            return d >> 2 == 0 or None
        if xo in (144, 512):                                       # mtcrf, mcrxr
            return None
        return bool(w & 1)                                         # record forms (stwcx. too)
    if op in (59, 63):
        if op == 63 and (w >> 1) & 0x3FF in (0, 32, 64, 583, 711, 70, 38):    # fcmpu fcmpo mcrfs mffs. mtfsf...
            return None
        return None if w & 1 else False                            # Rc on a float op sets cr1
    if op == 4:
        return None if (w >> 1) & 0x3FF in (0, 32, 64, 96) or w & 1 else False   # ps_cmp* / record forms
    if op == 19 and (w >> 1) & 0x3FF not in (16, 528, 150):        # CR logic, mcrf (not bclr / bcctr / isync)
        return None
    return False


def drop_cr0(consts, everything=False):
    for k in [k for k in consts if k == "cr0" or isinstance(k, tuple) and k[0] == "crbit" and
              (everything or k[1] < 4)]:
        del consts[k]


def edge_values(fn, block_end, target, values):
    """Values on the edge to target: a conditional branch on a cr0 bit whose cr0 is a known comparison adds
    ("crbit", bit) = (comparison, bit value on that edge)."""
    pc = block_end - 4
    w = fn.words[(pc - fn.start) // 4]
    br = ppc.branch(w, pc)
    if br is None or br.kind != "bc" or br.always or "cr0" not in values:
        return dict(values)
    bo, bi = (w >> 21) & 31, (w >> 16) & 31
    if not bo & 0x04 or bi > 3 or br.target == block_end:          # decrements CTR / other field / both edges same
        return dict(values)
    taken = target == br.target
    out = dict(values)
    out[("crbit", bi)] = (values["cr0"], bool(bo & 0x08) == taken)
    tag = values["cr0"]
    if isinstance(tag, tuple) and tag[0] == "zero" and bi == 2:      # EQ of a value against 0, on this edge
        if tag[1][0] == "v":
            if out[("crbit", 2)][1]:                                 # the object is null here: nothing to build
                out[("built", tag[1][1])] = ANY
        else:
            out[("isz" if out[("crbit", 2)][1] else "nz", tag[1])] = True
    return out


ANY = frozenset({"*"})          # ("built", V) on a path where V is null: any construction claim holds vacuously


def merge_value(key, a, b):
    """The value of key where two paths join, None when it is not known on both: equal values stay; ("built", V)
    keeps the functions run on V on both paths, a null path (ANY) taking the other path's."""
    if isinstance(key, tuple) and key[0] == "built":
        if a == ANY or b == ANY:
            return b if a == ANY else a
        return (a & b) or None
    return a if a == b else None


def object_code(fn: Function, vtables, op_new=None, op_delete=None):
    """Facts N6 / N7 / sizes need, from one run of the must-analysis (Astra answer 5, item 1: the linear scan of
    names.py kept one register state across branches and took "last store" in address order):
      stores        vtable_stores rows (pc, vtable or None, kind, offset, object)
      deletes       operator delete is called on this, and only on paths where the entry flag (r4, a copy, or its
                    sign extension) tested by extsh. / cmpwi 0 is > 0 (cr0 GT set): CodeWarrior's deleting
                    destructor. EQ clear only proves "not 0", and the exit chain passes -1 (Astra answer 6)
      plain_delete  operator delete called on this on some path without that test (no deleting destructor)
      flag_tested   the entry flag's sign is tested (a deleting destructor or a constructor with virtual bases)
      allocs        {operator new call site: size in r3 (int) or None}
      alloc_calls   {allocation site: {targets of linked calls whose r3 is that new object}}
      finals        per reached return (blr, or a conditional bclr taken): {offset of this: the value last
                    stored there on every path to it}
                    - an int, ("unknown",) for another value; an offset missing = the paths disagree. Calls are
                    assumed not to rewrite this's vtable slots once the function has stored them."""
    if op_new is not None:
        fn.returns[op_new] = "new"
    out = {"stores": [], "deletes": False, "plain_delete": False, "flag_tested": False, "allocs": {},
           "alloc_calls": collections.defaultdict(set), "finals": []}

    def visit(pc, w, regs, consts):
        row = store_row(w, regs, consts, vtables)
        if row:
            out["stores"].append((pc,) + row)
        op = w >> 26
        if op == 31 and (w >> 1) & 0x3FF == 922 and w & 1 and flag_source(consts.get((w >> 21) & 31)):
            out["flag_tested"] = True
        elif op == 11 and w & 0xFFFF == 0 and isinstance(consts.get((w >> 16) & 31), tuple) and \
                consts[(w >> 16) & 31][0] == "sext16":
            out["flag_tested"] = True
        br = ppc.branch(w, pc)
        if br is not None and br.kind == "bclr" and not br.link:
            out["finals"].append({k[1]: v for k, v in consts.items() if isinstance(k, tuple) and k[0] == "mem"})
        if br is None or not br.link or br.target is None:
            return
        if br.target == op_new:
            out["allocs"][pc] = consts.get(3) if isinstance(consts.get(3), int) else None
        elif br.target == op_delete and 3 in regs:
            sign = ("sign", FLAG)
            if consts.get(("crbit", 1)) == (sign, True):
                out["deletes"] = True
            else:
                out["plain_delete"] = True
        r3 = consts.get(3)
        if isinstance(r3, tuple) and r3[0] == "new" and r3[2] == 0:
            out["alloc_calls"][r3[1]].add(br.target)
    fn.scan({3}, visit, {4: FLAG})
    return out


def store_row(w, regs, consts, vtables):
    """(vtable or None, kind, offset, object) for a `stw` vtable_stores reports, else None."""
    op, src, base, _, imm = fields(w)
    if op != 36:
        return None
    vt = consts.get(src) if consts.get(src) in vtables else None
    v = consts.get(base)
    if base in regs:
        return vt, "this", simm(imm), "this"
    if isinstance(v, tuple) and v[0] == "new":
        return vt, "new", v[2] + simm(imm), v[1]
    if base == 1 or isinstance(v, tuple) and v[0] == "stack":
        return vt, "stack", (v[1] if base != 1 else 0) + simm(imm), "stack"
    if isinstance(v, int) or isinstance(v, tuple) and v[0] in ("sda2", "sda13"):
        return vt, "global", 0, (v + simm(imm)) if isinstance(v, int) else (v[0], v[1] + simm(imm))
    return (vt, "other", simm(imm), None) if vt is not None else None


def vtable_stores(fn: Function, vtables, op_new=None):
    """[(pc, vtable or None, kind, offset, object)] for every `stw` into this (r3 at entry or a copy), the object
    operator new returned ("new", object = its call site), the stack or a constant / sda address, and every `stw` of
    an address in vtables elsewhere ("other"). vtable None = some other value stored there (it may overwrite one)."""
    if op_new is not None:
        fn.returns[op_new] = "new"
    out = []

    def visit(pc, w, regs, consts):
        row = store_row(w, regs, consts, vtables)
        if row:
            out.append((pc,) + row)
    fn.scan({3}, visit)
    return out


BLR = 0x4E800020


def returns_this(image, f, size, entries=()):
    """True when every reached return of f (blr) has r3 = the r3 f was called with (CodeWarrior constructors and
    destructors return this): a call to f may then be followed with r3 = its receiver."""
    fn, seen = Function(image, f, size, entries), []

    def visit(pc, w, regs, consts):
        if is_return(w, pc):
            seen.append(3 in regs)
    fn.scan({3}, visit)
    return bool(seen) and all(seen)


def returned_construction(image, f, size, ctors_of, op_new, op_delete=None, entries=None):
    """Classes of the object f returns, proven by provenance (Astra answer 5, item 7): every reached return of f has
    the same r3 value V (one object: operator new's result, a global or sda address), and f calls a constructor of
    the class with receiver V - a function in ctors_of ({function: classes whose vtable it stores into this}) that
    returns this and never calls operator delete. Empty when that is not proven."""
    entries = entries or {}
    fn = Function(image, f, size.get(f, 0), entries.get(f, ()))
    if op_new is not None:
        fn.returns[op_new] = "new"
    for w_i, w in enumerate(fn.words):
        br = ppc.branch(w, f + 4 * w_i)
        t = br.target if br is not None and br.link else None
        if t in ctors_of and t not in fn.returns:
            callee = [image.word(t + 4 * k) for k in range(size.get(t, 0) // 4)]
            deletes = op_delete is not None and any(
                ppc.branch(x, t + 4 * k) is not None and ppc.branch(x, t + 4 * k).link and
                ppc.branch(x, t + 4 * k).target == op_delete for k, x in enumerate(callee))
            if not deletes and returns_this(image, t, size.get(t, 0), entries.get(t, ())):
                fn.returns[t] = "receiver"
    rets = []

    def visit(pc, w, regs, consts):
        if is_return(w, pc):
            v = consts.get(3)
            rets.append((v, consts.get(("built", v), frozenset()) if v is not None else frozenset()))
    fn.scan({3}, visit)
    if not rets or any(v is None for v, _ in rets) or len({v for v, _ in rets}) != 1:
        return set()
    # the constructor ran on the returned object on every path where it is not null (a path fact, Astra answer 6)
    per_return = [set().union(*(ctors_of[t] for t in built if t in ctors_of)) for _, built in rets if built != ANY]
    return set.intersection(*per_return) if per_return else set()


def vcall_at(fn: Function, i: int):
    """(vptr offset, base register, slot, bctrl address) when words i.. are one virtual call in one block, else None:
    lwz r12,K(rX); lwz r12,8+4s(r12); then only instructions that write neither r12 nor CTR and do not branch until
    mtctr r12 ... bctrl. The receiver (r3 at the bctrl) is checked by the caller against the analysis state."""
    if i + 3 >= len(fn.words):
        return None
    op0, d0, a0, _, k = fields(fn.words[i])
    op1, d1, a1, _, e = fields(fn.words[i + 1])
    if op0 != 32 or d0 != 12 or a0 == 12 or op1 != 32 or d1 != 12 or a1 != 12 or not 8 <= e < 0x8000 or e % 4:
        return None
    ctr = False
    for j in range(i + 2, min(i + 10, len(fn.words))):
        pc, w = fn.start + 4 * j, fn.words[j]
        if pc in fn.leaders:
            return None
        if w == BCTRL:
            return (simm(k), a0, (e - 8) // 4, pc) if ctr else None
        if ppc.branch(w, pc) is not None:
            return None
        if w & 0xFC1FFFFF == 0x7C0903A6:                            # mtctr rS
            ctr = (w >> 21) & 31 == 12
            continue
        written = ppc.gpr_writes(w)
        if written is None or 12 in written:
            return None
    return None


def survey(image, funcs, table, entries=None):
    """(vptr offsets, {class: functions storing its vtable into this}, typed calls on this, calls on this->field)."""
    size = {a: s for a, _, s, _ in funcs}
    name_of = {a: n for a, n, _, _ in funcs}
    entries = interior_entries(image, funcs) if entries is None else entries
    vt_class = {i["vtable"]: c for c, i in table.items() if i.get("vtable")}
    offsets = collections.defaultdict(set)
    ctors = collections.defaultdict(set)
    fcache = {}

    def fn(a):
        if a not in fcache:
            fcache[a] = Function(image, a, size[a], entries.get(a, ()))
        return fcache[a]

    for a, _, s, sec in funcs:                     # 1. stores of a primary vtable address at this+K
        if not s or not sec.startswith(".text"):
            continue
        for _, vt, kind, off, _ in vtable_stores(fn(a), vt_class):
            if vt is not None and kind == "this":
                offsets[vt_class[vt]].add(off)
                ctors[vt_class[vt]].add(a)
    vptr = {c: next(iter(k)) for c, k in offsets.items() if len(k) == 1}
    anc = {c: set(i["bases"]) | {c} for c, i in table.items()}
    this_type = collections.defaultdict(set)       # 2. methods: the class `this` has in each vtable body
    for f_, hs in corrmod.slot_holders(table).items():
        cs = corrmod.body_classes(hs)
        if not cs:
            continue
        own = [c for c in cs if all(c in anc[o] for o in cs)]
        if len(own) == 1:
            this_type[f_].add(own[0])
    for c, fs in ctors.items():
        for f_ in fs:
            this_type[f_].add(c)
    zero_chain = {}

    def chain(c):
        """c and its offset-0 ancestors, nearest first."""
        if c not in zero_chain:
            out = [c]
            for b, off in table[c]["direct_bases"]:
                if off == 0 and b in table:
                    out += [x for x in chain(b) if x not in out]
            zero_chain[c] = out
        return zero_chain[c]

    calls, field_calls = [], []
    for f_, types in this_type.items():
        if len(types) != 1 or f_ not in size:
            continue
        c = next(iter(types))
        if c not in vptr:
            continue
        f = fn(f_)
        pending, hits = {}, []

        def visit(pc, w, regs, consts):
            v = vcall_at(f, (pc - f.start) // 4)
            if v and v[1] in regs and v[0] == vptr[c]:
                pending[v[3]] = ("this", pc, v[2], None)
            elif v and isinstance(consts.get(v[1]), tuple) and consts[v[1]][0] == "field":
                pending[v[3]] = ("field", pc, v[2], (consts[v[1]], v[0]))
            want = pending.pop(pc, None)
            if want is None:
                return
            kind, at, slot, extra = want
            if kind == "this" and 3 in regs:
                hits.append((at, slot, pc))
            elif kind == "field" and consts.get(3) == extra[0]:
                field_calls.append({"caller": f_, "caller_name": name_of.get(f_), "this": c,
                                    "field": extra[0][1], "vptr": extra[1], "slot": slot, "at": at})
        f.scan({3}, visit)
        for pc, slot, bctrl in hits:
            # the farthest class whose table has the slot introduced it - unless a farther one may have it too:
            # zero words dropped after its last method (pure virtuals or alignment, corrections.rtti_classes)
            intro = [x for x in chain(c) if len(table[x]["methods"]) > slot]
            if any(len(table[x]["methods"]) <= slot < len(table[x]["methods"]) + table[x].get("trailing_zero_words", 0)
                   for x in chain(c)):
                intro = []
            calls.append({"caller": f_, "caller_name": name_of.get(f_), "this": c, "slot": slot,
                          "base": intro[-1] if intro else None, "at": pc, "bctrl": bctrl})
    return vptr, ctors, calls, field_calls


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("project")
    ap.add_argument("out")
    args = ap.parse_args(argv)
    image = dolmod.load_file(os.path.join(args.project, "main.dol"))
    funcs, objects = spine.load_symbols(os.path.join(args.project, "config", "symbols.txt"))
    sdata = image.section_at_address(next(v[0] for v in objects.values() if v[2] == ".sdata"))
    table = corrmod.rtti_classes(image, (sdata.address, sdata.address_end), {v[0] for v in objects.values()},
                                 {a: s for a, _, s, _ in funcs})
    vptr, ctors, calls, field_calls = survey(image, funcs, table)
    by_slot = collections.defaultdict(list)
    for r in calls:
        by_slot[f"{r['base']}[{r['slot']}]"].append(r)
    os.makedirs(args.out, exist_ok=True)
    with open(os.path.join(args.out, "vcalls.json"), "w", encoding="utf-8", newline="\n") as fo:
        json.dump({"vptr": vptr, "calls": [{**r, "caller": f"0x{r['caller']:08X}", "at": f"0x{r['at']:08X}",
                                            "bctrl": f"0x{r['bctrl']:08X}"} for r in calls],
                   "field_calls": [{**r, "caller": f"0x{r['caller']:08X}", "at": f"0x{r['at']:08X}"}
                                   for r in field_calls]}, fo, indent=1)
    print(json.dumps({"classes_with_vptr_offset": len(vptr), "conflicting": sum(1 for c in ctors if c not in vptr),
                      "calls": len(calls), "slots_called": len(by_slot),
                      "field_calls": len(field_calls)}, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
