"""Typed receivers for the virtual calls dolrecon.vcalls leaves open, and RenderWare engine-table calls.

    python -B -m dolrecon.receivers <dtk project dir>      (run from source/tools; writes <project>/receivers.json)

vcalls types a virtual call only when its receiver is `this`. Here a receiver's class comes from proofs in the code:
  exact     the receiver is an object this function allocated (operator new) and then built: a constructor of C (an
            N6-named function that stores C's vtable into this, returns this and never deletes) ran on it on every
            path (vcalls' "built" path fact; the most derived of the classes built), or C's vtable was stored into it
            at C's vptr offset (an inlined constructor). Its dynamic type is C: one target, slot s of C.
  returned  the receiver is the result of a call to a function proven to return a C: vcalls.returned_construction
            (every return gives one object - new, a global or sda address - built as a C), or, to a fixed point, a
            function every return of which gives an object already typed C. Static type C: the targets are slot s
            of C and of every class deriving from C at offset 0.
  field     the receiver is this->F (`lwz rX, F(this)`), F a member of the method's class O (or of an offset-0
            ancestor) whose every store in a method of that class is a typed object of one class C, or zero.
            Static type C, targets as for returned. Stores outside the class's own methods are not seen: a member
            another class rewrites with an unrelated object would be missed (the stated limit).
A typed call is kept only if the vtable offset it loads (`lwz r12,K(rX)`) is C's vptr offset (vcalls.survey) and C's
table has slot s; otherwise it is counted as rejected. The receiver must still be in r3 at the bctrl.
  engine    `lwz r12,K(rA)` with rA = the word at RwEngineInstance, then mtctr r12 / bctrl: a call through RenderWare's
            engine structure (RwGlobals) at offset K; named where the offset is known (ENGINE_SLOTS).
"""
from __future__ import annotations

import argparse
import collections
import json
import os
import sys

from . import corrections as corrmod
from . import dol as dolmod
from . import ppc
from . import spine
from . import strrefs
from . import vcalls

fields, simm = ppc.fields, ppc.simm
ROUNDS = 6   # inference rounds at most (each one full pass)
BCTRL = vcalls.BCTRL
# RwGlobals of the GameCube build (RenderWare 3.7 rwplcore.h order; offsets confirmed by calls with matching
# arguments: 0xD0 (dst, src, 256) strncpy, 0xD4 strcat, 0xF4 strlen; 0x108.. the memory functions the reference
# interpreter's harness sets, runtime/tests/rwcore_oracle.h)
ENGINE_SLOTS = {0xC4: "stringFuncs.rwsprintf", 0xC8: "stringFuncs.rwvsprintf", 0xCC: "stringFuncs.rwstrcpy",
                0xD0: "stringFuncs.rwstrncpy", 0xD4: "stringFuncs.rwstrcat", 0xD8: "stringFuncs.rwstrncat",
                0xDC: "stringFuncs.rwstrrchr", 0xE0: "stringFuncs.rwstrchr", 0xE4: "stringFuncs.rwstrstr",
                0xE8: "stringFuncs.rwstrcmp", 0xEC: "stringFuncs.rwstrncmp", 0xF0: "stringFuncs.rwstricmp",
                0xF4: "stringFuncs.rwstrlen", 0x108: "memoryFuncs.rwmalloc", 0x10C: "memoryFuncs.rwfree",
                0x110: "memoryFuncs.rwrealloc", 0x114: "memoryFuncs.rwcalloc", 0x118: "memoryAlloc",
                0x11C: "memoryFree"}


def engine_call_at(fn: vcalls.Function, i: int):
    """(base register, offset, bctrl address) when word i is `lwz r12,K(rA)` (rA not r12) followed, inside its block,
    by `mtctr r12` and `bctrl` with no other write of r12 / CTR and no branch, and NOT by a second `lwz r12,..(r12)`
    (that is a virtual call: vcall_at)."""
    if i + 2 >= len(fn.words):
        return None
    op0, d0, a0, _, k = fields(fn.words[i])
    if op0 != 32 or d0 != 12 or a0 in (0, 12):
        return None
    op1, d1, a1, _, _ = fields(fn.words[i + 1])
    if op1 == 32 and d1 == 12 and a1 == 12:
        return None
    ctr = False
    for j in range(i + 1, min(i + 10, len(fn.words))):
        pc, w = fn.start + 4 * j, fn.words[j]
        if pc in fn.leaders:
            return None
        if w == BCTRL:
            return (a0, simm(k), pc) if ctr else None
        if ppc.branch(w, pc) is not None:
            return None
        if w & 0xFC1FFFFF == 0x7C0903A6:
            ctr = (w >> 21) & 31 == 12
            continue
        written = ppc.gpr_writes(w)
        if written is None or 12 in written:
            return None
    return None


class Analysis:
    def __init__(self, project_dir: str):
        self.image = image = dolmod.load_file(os.path.join(project_dir, "main.dol"))
        self.funcs, objects = spine.load_symbols(os.path.join(project_dir, "config", "symbols.txt"))
        sdata = image.section_at_address(next(v[0] for v in objects.values() if v[2] == ".sdata"))
        self.table = corrmod.rtti_classes(image, (sdata.address, sdata.address_end), {v[0] for v in objects.values()},
                                          {a: s for a, _, s, _ in self.funcs})
        self.size = {a: s for a, _, s, _ in self.funcs}
        self.name = {a: n for a, n, _, _ in self.funcs}
        by_name = {n: a for a, n, _, _ in self.funcs}
        self.op_new, self.op_del = by_name.get("__nw__FUl"), by_name.get("__dl__FPv")
        self.engine = next((v[0] for n, v in objects.items() if n == "RwEngineInstance"), None)
        text = next(x for x in image.sections if x.kind == "text")
        r2, r13 = strrefs.sda_bases(image, text.address, text.address + text.size)
        self.sda = {r: v for r, v in ((2, r2), (13, r13)) if v is not None}
        self.entries = vcalls.interior_entries(image, self.funcs)
        self.vptr, ctors, self.this_calls, self.field_calls = vcalls.survey(image, self.funcs, self.table, self.entries)
        self.vt_class = {i["vtable"]: c for c, i in self.table.items() if i.get("vtable")}
        # N6 constructors (named C__ctor by names.py) that store their class's vtable into this, return this and
        # never call operator delete
        self.ctor_class = {}
        for c, fs in ctors.items():
            for f in fs:
                if self.name.get(f, "").endswith("__ctor") and not self.calls_delete(f) and \
                        vcalls.returns_this(image, f, self.size[f], self.entries.get(f, ())):
                    self.ctor_class[f] = c
        self.this_type = self.method_classes(ctors)
        self.derived = collections.defaultdict(set)
        for c, info in self.table.items():
            for b, off in info["direct_bases"]:
                if off == 0:
                    self.derived[b].add(c)
        self.returns = {}            # function -> class every return gives (or derived)
        self.field_types = {}        # (class, offset) -> class
        self.global_types = {}       # static address -> class of the object every typed store there writes
        self.params = {}             # function -> {argument register: class every caller passes (or derived)}
        self.last_calls, self.last_gstores = [], (None, [])

    def calls_delete(self, f):
        return self.op_del is not None and any(
            (br := ppc.branch(self.image.word(f + 4 * k), f + 4 * k)) is not None and br.link and br.target == self.op_del
            for k in range(self.size.get(f, 0) // 4))

    def method_classes(self, ctors):
        """{function: class of this} - constructors and destructors (vtable stores) and the bodies of vtable slots
        owned by one class (vcalls.survey's rule)."""
        anc = {c: set(i["bases"]) | {c} for c, i in self.table.items()}
        out = collections.defaultdict(set)
        for f, hs in corrmod.slot_holders(self.table).items():
            cs = corrmod.body_classes(hs)
            if cs:
                own = [c for c in cs if all(c in anc[o] for o in cs)]
                if len(own) == 1:
                    out[f].add(own[0])
        for c, fs in ctors.items():
            for f in fs:
                out[f].add(c)
        return {f: next(iter(cs)) for f, cs in out.items() if len(cs) == 1}

    def descends(self, c):
        """c and every class deriving from it at offset 0."""
        out, todo = {c}, [c]
        while todo:
            for d in self.derived.get(todo.pop(), ()):
                if d not in out:
                    out.add(d)
                    todo.append(d)
        return out

    def chain(self, c):
        out, todo = [c], [c]
        while todo:
            x = todo.pop()
            for b, off in self.table.get(x, {}).get("direct_bases", []):
                if off == 0 and b not in out:
                    out.append(b)
                    todo.append(b)
        return out

    def most_derived(self, classes):
        classes = {c for c in classes if c in self.table}
        top = [c for c in classes if all(o in self.chain(c) for o in classes)]
        return top[0] if len(top) == 1 else None

    def function(self, f):
        fn = vcalls.Function(self.image, f, self.size[f], self.entries.get(f, ()))
        fn.loads, fn.sda = True, self.sda
        if self.op_new is not None:
            fn.returns[self.op_new] = "new"
        for t in self.ctor_class:
            fn.returns[t] = "receiver"
        for t, c in self.returns.items():
            fn.returns[t] = ("class", c)
        return fn

    def value_class(self, v, consts, sites, this_class):
        """(class, exact) of value v at a point, or None."""
        if isinstance(v, tuple) and v[0] == "new" and len(v) == 3 and v[2] == 0:
            built = consts.get(("built", v)) or ()
            cs = {self.ctor_class[t] for t in built if t in self.ctor_class} | sites.get(v[1], set())
            c = self.most_derived(cs) if cs else None
            return (c, True) if c else None
        if isinstance(v, tuple) and v[0] == "obj":
            return (v[1], False)
        if isinstance(v, tuple) and v[0] in ("field", "ofield"):
            owner, off = (this_class, v[1]) if v[0] == "field" else (v[1], v[2])
            for x in self.chain(owner) if owner else ():
                c = self.field_types.get((x, off))
                if c:
                    return (c, False)
        if isinstance(v, tuple) and v[0] == "load" and isinstance(v[1], int):
            c = self.global_types.get(v[1])
            if c:
                return (c, False)
        return None

    def scan_function(self, f, want_calls=True):
        """(typed calls, field stores, return values) of f."""
        fn = self.function(f)
        this_class = self.this_type.get(f)
        sites = collections.defaultdict(set)      # allocation site -> classes whose vtable was stored into it
        pending, calls, stores, rets, engine, gstores, argcalls = {}, [], [], [], [], [], []

        def visit(pc, w, regs, consts):
            i = (pc - f) // 4
            row = vcalls.store_row(w, regs, consts, self.vt_class)
            if row and row[0] is not None and row[1] == "new":
                c = self.vt_class[row[0]]
                if self.vptr.get(c) == row[2]:
                    sites[row[3]].add(c)
            op, src, base, _, imm = fields(w)
            if op == 36 and base in regs and this_class:                    # stw rS, F(this)
                stores.append((this_class, simm(imm), consts.get(src), dict(consts)))
            elif op == 36 and base != 0:
                bv = consts.get(base)
                owner = self.value_class(bv, consts, sites, this_class) if isinstance(bv, tuple) else None
                if owner:                                                    # stw rS, F(typed object)
                    stores.append((owner[0], simm(imm), consts.get(src), dict(consts)))
            if op == 36:                                                     # stw rS, d(constant / r13 / r2)
                mbase = 0 if base == 0 else self.sda.get(base, consts.get(base))
                if isinstance(mbase, int):
                    gstores.append(((mbase + simm(imm)) & 0xFFFFFFFF, consts.get(src), dict(consts)))
            if vcalls.is_return(w, pc):
                rets.append((consts.get(3), dict(consts)))
            br = ppc.branch(w, pc)
            if br is not None and br.target is not None and (br.link or br.kind == "b" and not f <= br.target < f + self.size[f]):
                argcalls.append((br.target, {r: consts.get(r) for r in (3, 4, 5, 6)}, dict(consts)))
            if not want_calls:
                return
            v = vcalls.vcall_at(fn, i)
            if v and v[1] not in regs:
                pending[v[3]] = ("virtual", consts.get(v[1]), v[0], v[2], dict(consts))
            e = engine_call_at(fn, i)
            if e and self.engine is not None and consts.get(e[0]) == ("load", self.engine):
                pending[e[2]] = ("engine", None, e[1], None, None)
            want = pending.pop(pc, None)
            if want is None:
                return
            kind, value, k, slot, at_consts = want
            if kind == "engine":
                engine.append((pc, k))
            elif value is not None and consts.get(3) == value:
                calls.append((pc, value, k, slot, at_consts))
        entry = dict(self.sda)
        for r, c in self.params.get(f, {}).items():
            if not (r == 3 and this_class):
                entry[r] = ("obj", c)
        fn.scan({3} if this_class else (), visit, entry)
        self.last_gstores = (sites, gstores)
        self.last_calls = argcalls
        return fn, sites, calls, stores, rets, engine

    def initial_returns(self):
        """vcalls.returned_construction for every function that calls operator new (not a constructor)."""
        ctors_of = collections.defaultdict(set)
        for t, c in self.ctor_class.items():
            ctors_of[t].add(c)
        for f, _, s, sec in self.funcs:
            if not s or not sec.startswith(".text") or f in self.ctor_class or self.op_new is None:
                continue
            if not any((br := ppc.branch(self.image.word(f + 4 * k), f + 4 * k)) is not None and br.link and
                       br.target == self.op_new for k in range(s // 4)):
                continue
            cs = vcalls.returned_construction(self.image, f, self.size, ctors_of, self.op_new, self.op_del, self.entries)
            c = self.most_derived(cs) if cs else None
            if c:
                self.returns[f] = c

    def common_base(self, classes):
        """The most derived class every one of classes is (or derives from at offset 0), or None."""
        classes = [c for c in classes if c in self.table]
        if not classes:
            return None
        common = [x for x in self.chain(classes[0]) if all(x in self.chain(c) for c in classes[1:])]
        return common[0] if common else None

    def infer_round(self, direct_only):
        """One pass over every function, then the types it proves: returns (every return gives an object of one
        class), members and globals (every store a typed object of one class, or 0), parameters r3..r6 (every call
        and tail call of a function reached by no other edge passes a typed object; several classes -> their common
        base). Returns the number of new facts."""
        rets_seen = collections.defaultdict(set)
        fseen, gseen = collections.defaultdict(set), collections.defaultdict(set)
        pseen = collections.defaultdict(lambda: collections.defaultdict(set))
        for f, _, s, sec in self.funcs:
            if not s or not sec.startswith(".text"):
                continue
            _, sites, _, stores, rets, _ = self.scan_function(f, want_calls=False)
            this_class = self.this_type.get(f)
            for owner, off, v, consts in stores:
                if v != 0:
                    vc = self.value_class(v, consts, sites, this_class)
                    fseen[(owner, off)].add(vc[0] if vc else "untyped")
            for at, v, consts in self.last_gstores[1]:
                if v != 0:
                    vc = self.value_class(v, consts, sites, this_class)
                    gseen[at].add(vc[0] if vc else "untyped")
            if f not in self.ctor_class and f not in self.returns:
                for v, consts in rets:
                    vc = self.value_class(v, consts, sites, this_class)
                    rets_seen[f].add(vc[0] if vc else "untyped")
            for g, args, consts in self.last_calls:
                if g not in direct_only:
                    continue
                for r in (3, 4, 5, 6):
                    vc = self.value_class(args.get(r), consts, sites, this_class)
                    pseen[g][r].add(vc[0] if vc else "untyped")
        added = 0
        for f, cs in rets_seen.items():
            if "untyped" not in cs and len(cs) == 1 and f not in self.returns:
                self.returns[f] = next(iter(cs))
                added += 1
        for key, cs in fseen.items():
            if "untyped" not in cs and len(cs) == 1 and key not in self.field_types:
                self.field_types[key] = next(iter(cs))
                added += 1
        for at, cs in gseen.items():
            if "untyped" not in cs and len(cs) == 1 and at not in self.global_types:
                self.global_types[at] = next(iter(cs))
                added += 1
        for g, regs in pseen.items():
            for r, cs in regs.items():
                if "untyped" in cs or (r == 3 and g in self.this_type):
                    continue
                c = self.common_base(cs)
                if c and self.params.get(g, {}).get(r) != c:
                    self.params.setdefault(g, {})[r] = c
                    added += 1
        return added

    def typed_calls(self):
        out, rejected, engine = [], collections.Counter(), []
        for f, _, s, sec in self.funcs:
            if not s or not sec.startswith(".text"):
                continue
            _, sites, calls, _, _, eng = self.scan_function(f)
            for pc, k in eng:
                engine.append({"caller": self.name[f], "at": f"0x{pc:08X}", "offset": k,
                               "slot": ENGINE_SLOTS.get(k, f"RwGlobals+0x{k:X}")})
            for pc, value, k, slot, consts in calls:
                vc = self.value_class(value, consts, sites, self.this_type.get(f))
                if vc is None:
                    rejected["untyped receiver"] += 1
                    continue
                c, exact = vc
                if self.vptr.get(c) != k:
                    rejected["vptr offset differs from the class's"] += 1
                    continue
                if slot >= len(self.table[c]["methods"]):
                    rejected["slot outside the class's table"] += 1
                    continue
                classes = {c} if exact else self.descends(c)
                targets = sorted({self.name.get(self.table[x]["methods"][slot]) for x in classes
                                  if x in self.table and slot < len(self.table[x]["methods"])} - {None})
                kind = "exact" if exact else {"field": "field", "ofield": "field", "load": "global"}.get(
                    value[0] if isinstance(value, tuple) else None, "returned")
                out.append({"caller": self.name[f], "at": f"0x{pc:08X}", "class": c, "slot": slot, "evidence": kind,
                            "targets": targets})
        return out, rejected, engine


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("project")
    args = ap.parse_args(argv)
    a = Analysis(args.project)
    a.initial_returns()
    # functions reached only by direct and tail calls (dolrecon.callgraph): their callers are all seen
    with open(os.path.join(args.project, "callgraph.json"), encoding="utf-8") as f:
        cg = json.load(f)
    by_name = {n: x for x, n in a.name.items()}
    other = {by_name[e["to"]] for e in cg["edges"] if e["kind"] not in ("call", "tail") and e["to"] in by_name}
    direct_only = {x for x in a.size if x not in other}
    for r in range(ROUNDS):
        added = a.infer_round(direct_only)
        print(f"round {r + 1}: {added} new facts", file=sys.stderr)
        if not added:
            break
    calls, rejected, engine = a.typed_calls()
    out = {"schema": "dolrecon.receivers/1", "typed_calls": calls, "engine_calls": engine,
           "returns": {a.name[f]: c for f, c in a.returns.items()},
           "field_types": {f"{c}+0x{off:X}": t for (c, off), t in a.field_types.items()},
           "global_types": {f"0x{g:08X}": t for g, t in a.global_types.items()},
           "param_types": {a.name[g]: {f"r{r}": c for r, c in rs.items()} for g, rs in a.params.items()},
           "rejected": dict(rejected)}
    with open(os.path.join(args.project, "receivers.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(out, f, indent=1)
    by = collections.Counter(c["evidence"] for c in calls)
    print(json.dumps({"typed_calls": len(calls), "by_evidence": by, "engine_calls": len(engine),
                      "typed_returns": len(a.returns), "typed_fields": len(a.field_types),
                      "typed_globals": len(a.global_types),
                      "typed_params": sum(len(r) for r in a.params.values()), "rejected": rejected},
                     indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
