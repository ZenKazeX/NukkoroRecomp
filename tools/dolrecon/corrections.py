"""Evidence-based corrections to dtk's analysis, applied as a NEW project (docs/DOLRECON.md "Accuracy").

    python -B -m dolrecon.corrections <audited project dir> <new project dir> [jobs]    (run from source/tools)

Reads the audited project (config/symbols.txt, build/audit.json, main.dol), derives corrections only from evidence in
the original bytes, writes them into a new project's dtk config, re-runs dtk + the rebuild + the audit there, and
fails unless the rebuild is still byte-exact. Every correction is listed in <new>/corrections.json with its evidence.

  C1 entry labels    a `bl` into a function body (audit B2) is a second entry point. The CodeWarrior runtime's
                     register save / restore helpers are such multi-entry routines; the label is named from the
                     instruction AT the entry, decoded here: stw rN,d(r11) -> _savegpr_N, lwz -> _restgpr_N,
                     stfd fN -> _savefpr_N, lfd -> _restfpr_N; anything else -> entry_XXXXXXXX.
  C2 missed refs     audit R3 `lis`/`ori|addi|load|store` pairs whose full value is a symbol's start and that carry no
                     relocation -> dtk add_relocations (hi or ha on the lis, l on the pairing instruction).
  C3 table targets   relocations into a function body at an address dtk did not label (audit R2) -> a code label
                     there (`case_XXXXXXXX`), so the reference reads as a label instead of fn+offset.
  C4 class names     virtual functions from the game's own RTTI records ({name, vtable} pairs in .sdata, located by
                     structure, not by a fixed address), over every primary and secondary vtable (slot_holders): a
                     function whose holders are one class -> Class__vfN; several classes -> named only if one of them
                     is an ancestor (or self) of all the others; else left alone. Thunks are not named here (names.py
                     N13); names whose records or vtables conflict are left out (rtti_classes.conflicts). Only fn_XXXXXXXX names are replaced; dtk's own names win. (A signature-database name list was
                     evaluated and REJECTED: 5,579 of 6,236 names map to several addresses each.)
"""

from __future__ import annotations

import collections
import json
import os
import re
import shutil
import struct
import subprocess
import sys

from . import audit as auditmod
from . import dol as dolmod
from . import ppc
from . import rebuild

SYM_LINE = re.compile(r"^(\S+) = ([^:]+):0x([0-9A-Fa-f]+);(.*)$")


def read_symbols(path: str) -> list[list]:
    """[[name, section, address, rest-of-line]] in file order (rest keeps dtk's comment verbatim)."""
    out = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            m = SYM_LINE.match(line.rstrip("\n"))
            if m:
                out.append([m.group(1), m.group(2), int(m.group(3), 16), m.group(4)])
    return out


def sanitize(name: str) -> str:
    """C++ class name -> assembler symbol: '::' becomes '__' (after other characters collapse to single '_', so the
    two stay distinguishable)."""
    s = re.sub(r"[^A-Za-z0-9_:]", "_", name)
    s = re.sub(r"_*::_*", "::", re.sub(r"_+", "_", s)).strip("_").replace("::", "__")
    return ("_" + s) if not s or s[0].isdigit() else s


# ---- C1 ---------------------------------------------------------------------------------------------------------
def entry_name(word: int, address: int) -> tuple[str, str]:
    op, rs, ra = word >> 26, (word >> 21) & 0x1F, (word >> 16) & 0x1F
    if ra == 11:
        kind = {36: "_savegpr_", 32: "_restgpr_", 54: "_savefpr_", 50: "_restfpr_"}.get(op)
        if kind:
            return f"{kind}{rs}", f"instruction {word:08x} at entry: {'stw' if op == 36 else 'lwz' if op == 32 else 'stfd' if op == 54 else 'lfd'} r/f{rs}, d(r11)"
    return f"entry_{address:08X}", f"call target inside a function; instruction {word:08x} (no runtime pattern)"


# ---- C4 ---------------------------------------------------------------------------------------------------------
# Any printable name: the structural check below (the vtable points back at its record) is what proves a record, not
# the characters (anonymous-namespace classes read "@unnamed@PlayerEffect_cpp@::Grind").
NAME_OK = re.compile(r"^[!-~][ -~]{1,255}$")


def rtti_instances(image: dolmod.DolImage, sdata, object_starts: set | None = None) -> dict:
    """Every RTTI record and vtable instance in the image, from CodeWarrior's RTTI (layout established 2026-10-31 from
    CameraModeMtnViewer, whose constructor stores the vtable address); rtti_classes builds the class view on it:

      record     8 bytes in .sdata: {const char* name, base list or 0}. 0 = a class without bases.
      base list  {record, offset} pairs ending in a 0 word (CameraModeMtnViewer: {&CameraMode record, 0}, 0).
      vtable     {record, offset, method...}: offset 0 = the primary vtable (what a constructor stores), negative = a
                 secondary vtable of a multiply inherited class; a dtk data object, or a {record, offset} header
                 inside one right after another vtable (BkChaos holds five secondaries in one object).

    A record counts only with structural proof: a vtable whose first word points at it, or a base list of a proven
    class naming it. A {name, x} lookalike (parameter records, string tables) has neither. Returns {"name": {record:
    name}, "base_list": {record: word}, "bases": {record: [(record, offset)]}, "vtables": {record: [(address,
    offset, methods, trailing zero words dropped)]}, "proven": records}."""
    text = [s for s in image.sections if s.kind == "text"]
    data = [s for s in image.sections if s.kind != "text"]

    def is_text(v):
        return any(s.address <= v < s.address_end for s in text)

    def cstr(addr):
        sec = image.section_at_address(addr)
        if sec is None:
            return None
        off = sec.file_offset + addr - sec.address
        end = image.raw.find(b"\0", off, min(off + 256, sec.file_end))
        if end < 0:
            return None
        try:
            s = image.raw[off:end].decode("ascii")
        except UnicodeDecodeError:
            return None
        return s if NAME_OK.match(s) else None

    def signed(w):
        return w - (1 << 32) if w & 0x80000000 else w

    # candidate records: a printable name and a second word that is 0 or a well-formed base list
    cand = {}
    for rec in range(sdata[0], sdata[1] - 7, 4):
        name_ptr = image.word(rec)
        if not name_ptr or image.section_at_address(name_ptr) is None:
            continue
        name = cstr(name_ptr)
        if name:
            cand[rec] = (name, image.word(rec + 4))

    def base_list(addr):
        out = []
        for k in range(64):
            if image.section_at_address(addr + 8 * k) is None:
                return None
            r = image.word(addr + 8 * k)
            if r == 0:
                return out
            if r not in cand:
                return None
            out.append((r, signed(image.word(addr + 8 * k + 4))))
        return None

    bases_of = {}
    for rec, (name, bl) in cand.items():
        if bl == 0:
            bases_of[rec] = []
        elif not is_text(bl):
            lst = base_list(bl)
            if lst:
                bases_of[rec] = lst

    # vtables: data objects {record, offset (0 or negative), code pointer...}
    starts = sorted(object_starts) if object_starts is not None else None
    start_set = set(starts) if starts is not None else None
    positions = starts if starts is not None else [a for s in data for a in range(s.address, s.address_end - 7, 4)]

    def header(pos):
        """(record, offset) when pos holds a {record, offset} vtable header."""
        sec = image.section_at_address(pos)
        if sec is None or sec.kind == "text" or pos + 8 > sec.address_end:
            return None
        rec, off = image.word(pos), signed(image.word(pos + 4))
        return (rec, off) if rec in bases_of and -0x100000 < off <= 0 else None

    vtables = collections.defaultdict(list)
    todo, seen = list(positions), set()
    while todo:
        pos = todo.pop()
        if pos in seen or (h := header(pos)) is None:
            continue
        seen.add(pos)
        rec, off = h
        end = image.section_at_address(pos).address_end
        methods, slot = [], pos + 8
        while slot + 4 <= end and len(methods) < 512:
            if start_set is not None and slot in start_set:
                break                       # dtk's objects bound the array: a jump table often follows a vtable
            w = image.word(slot)
            if w == 0 and start_set is not None:
                # a pure virtual: CodeWarrior leaves its slot 0 (Gadget {dtor, update, 0, blr, 0, checkRemove};
                # boost::detail::sp_counted_base {dtor, 0, destruct, 0} = its dispose / get_deleter = 0). Kept
                # as None only inside dtk's object; trailing zeros (pure or alignment) are dropped below.
                methods.append(None)
                slot += 4
                continue
            if not is_text(w):
                if start_set is not None and header(slot) is not None:
                    todo.append(slot)       # a secondary vtable's {record, offset} header in the same data object
                break
            methods.append(w)
            slot += 4
        trailing = 0
        while methods and methods[-1] is None:
            methods.pop()
            trailing += 1
        if methods:
            vtables[rec].append((pos, off, methods, trailing))
    for rec in vtables:
        vtables[rec].sort()

    proven = {rec for rec in vtables if any(v[1] == 0 for v in vtables[rec])}
    frontier = list(proven)
    while frontier:
        rec = frontier.pop()
        for b, _ in bases_of.get(rec, []):
            if b not in proven and b in bases_of:
                proven.add(b)
                frontier.append(b)
    return {"name": {r: cand[r][0] for r in cand}, "base_list": {r: cand[r][1] for r in cand}, "bases": bases_of,
            "vtables": dict(vtables), "proven": proven}


class RttiTable(dict):
    """rtti_classes' result, {class: info}; .conflicts = {class: reason} for the names it abstained on."""

    def __init__(self):
        super().__init__()
        self.conflicts = {}


def vtable_thunk(image: dolmod.DolImage, f: int, table_offset: int) -> dict | None:
    """{"adjust", "vbase_field", "target"} when the body f in a secondary vtable at table_offset is a this-adjusting
    thunk as CodeWarrior emits them (census 2026-11-01: 1,647 such slots, each exactly its dtk function's size):
      addi r3,r3,A ; b T                       this += A, A = the table's offset (1,191 slots)
      li r11,F ; lwzx r11,r3,r11 ; add r3,r3,r11 ; [addi r3,r3,A] ; b T
                                               this += *(this + F) + A: through a virtual base's offset field (456)
    A method of the subobject that forwards to a member is the first form with A > 0 (members lie after this); a
    thunk's A is the table offset, which is negative. None for anything else."""
    sec = image.section_at_address(f)
    if sec is None or sec.kind != "text":
        return None
    w = [image.word(f + 4 * k) for k in range(min(5, (sec.address_end - f) // 4))]

    def addi_r3(x):
        return x >> 26 == 14 and (x >> 16) & 0x3FF == 0x63          # addi r3, r3, d

    def jump(k):
        b = ppc.branch(w[k], f + 4 * k) if k < len(w) else None
        return b.target if b is not None and b.kind == "b" and not b.link else None

    if w and addi_r3(w[0]) and ppc.simm(w[0] & 0xFFFF) == table_offset and jump(1) is not None:
        return {"adjust": table_offset, "vbase_field": None, "target": jump(1)}
    if len(w) >= 4 and w[0] >> 16 == 0x3960 and w[1:3] == [0x7D63582E, 0x7C635A14]:     # li r11; lwzx; add
        k = 4 if addi_r3(w[3]) else 3
        if jump(k) is not None:
            return {"adjust": ppc.simm(w[3] & 0xFFFF) if k == 4 else 0, "vbase_field": ppc.simm(w[0] & 0xFFFF),
                    "target": jump(k)}
    return None


def thunk_length(t) -> int:
    """Bytes of a vtable_thunk: 8 (addi; b), 16 or 20 (li; lwzx; add; [addi;] b)."""
    if t["vbase_field"] is None:
        return 8
    return 20 if t["adjust"] else 16


def rtti_classes(image: dolmod.DolImage, sdata, object_starts: set | None = None,
                 functions: dict | None = None) -> RttiTable:
    """{class: {"vtable", "vtables", "bases", "direct_bases", "base_list", "methods", "record", "records",
    "secondary", "trailing_zero_words"}} for every proven class name whose instances agree.

    "direct_bases" is the record's base list as CodeWarrior writes it: EVERY base subobject, indirect and virtual
    ones included, at its offset in this class (EggPawnBase lists EnemyReferer, a virtual base, at 756;
    EnemyWalkerCommon at 624). "secondary": one entry per secondary vtable {"vtable", "offset", "methods",
    "subobject", "thunks"}: "subobject" is the class of the base subobject at -offset (the most derived class the
    base list puts there; None when there is no single one), "thunks" {slot: vtable_thunk} the slots holding a
    this-adjusting thunk rather than a body of the subobject. functions ({start: size}, when given): a thunk must be
    exactly its function (its size the thunk's length) and its target a function start (Astra answer 6).

    CodeWarrior emits a class's record in every unit that needs it (Task has one beside ResourceManagerTask's base
    list and one beside its own vtable 0x8051E768), and an inline class's vtable in every unit that constructs it. A
    name stands only when all its records have the same direct bases (by name and offset) and all its primary vtables
    the same slots; otherwise it is left out and listed in .conflicts. "vtable" is the lowest primary vtable address,
    "vtables" all of them (each is what some constructor stores). "trailing_zero_words" > 0 means zero words after
    the last method were dropped (pure virtuals or alignment): the method list is then a lower bound on the table."""
    inst = rtti_instances(image, sdata, object_starts)
    name, bases_of, vtables = inst["name"], inst["bases"], inst["vtables"]

    def ancestors(rec, seen=None):
        seen = set() if seen is None else seen
        for b, _ in bases_of.get(rec, []):
            if b not in seen:
                seen.add(b)
                ancestors(b, seen)
        return seen

    by_name = collections.defaultdict(list)
    for rec in sorted(inst["proven"]):
        by_name[name[rec]].append(rec)
    table = RttiTable()
    for cls, recs in by_name.items():
        shapes = {tuple((name[b], off) for b, off in bases_of.get(r, [])) for r in recs}
        if len(shapes) > 1:
            table.conflicts[cls] = f"{len(recs)} records with {len(shapes)} different base lists"
            continue
        tabs = [(r, v) for r in recs for v in vtables.get(r, [])]
        prim = [(r, v) for r, v in tabs if v[1] == 0]
        if len({tuple(v[2]) for _, v in prim}) > 1:
            table.conflicts[cls] = f"{len(prim)} primary vtables with different slots"
            continue
        sec = {}
        for _, (a, off, m, tr) in tabs:
            if off != 0:
                sec.setdefault(off, set()).add(tuple(m))
        if any(len(s) > 1 for s in sec.values()):
            table.conflicts[cls] = "secondary vtables at one offset with different slots"
            continue
        rec = min(r for r, _ in prim) if prim else recs[0]
        table[cls] = {
            "records": recs,
            "record": rec,
            "base_list": inst["base_list"][rec] or None,
            "direct_bases": [(name[b], off) for b, off in bases_of.get(rec, [])],
            "bases": sorted({name[b] for b in ancestors(rec)}),
            "vtable": min(v[0] for _, v in prim) if prim else None,
            "vtables": sorted(v[0] for _, v in prim),
            "methods": list(prim[0][1][2]) if prim else [],
            "trailing_zero_words": max((v[3] for _, v in prim), default=0),
            "secondary": [{"vtable": a, "offset": off, "methods": m} for _, (a, off, m, _) in tabs if off != 0],
        }
    anc = {c: set(info["bases"]) for c, info in table.items()}
    for info in table.values():
        for s in info["secondary"]:
            at = {b for b, off in info["direct_bases"] if off == -s["offset"]}
            top = [c for c in at if c in table and all(o == c or o in anc[c] for o in at)]
            s["subobject"] = top[0] if len(top) == 1 else None
            s["thunks"] = {k: t for k, f in enumerate(s["methods"])
                           if f is not None and (t := vtable_thunk(image, f, s["offset"])) is not None
                           and (functions is None or functions.get(f) == thunk_length(t) and t["target"] in functions)}
    return table


def slot_holders(table: dict) -> dict:
    """{function: [(kind, table_class, view, slot)]} over every non-pure slot of every vtable in an rtti_classes
    table (Astra answer 5: ownership counts secondary vtables, and a thunk is not its target):
      "primary"   slot `slot` of table_class's primary vtable; view = table_class
      "direct"    a body in a secondary vtable of table_class: it takes the subobject's this, so it is a method of
                  `view`, the subobject's class (None: no single class at that offset), at view's slot `slot`
      "thunk"     a this-adjusting thunk there (vtable_thunk), compiler-made for table_class
      "override"  a plain thunk's target (this += the table offset lands on table_class's this): table_class's
                  implementation of view's slot `slot`
      "vtarget"   a virtual-base thunk's target: an implementation of view's slot `slot`, but the thunk adds an
                  offset read at run time, so its this may be any subobject (0x8018FF8C, reached so from
                  BkWingLarge's EnemyParamCommon table, is a body of its TEnemySetBase subobject): no class claim"""
    out = collections.defaultdict(list)
    for cls, info in table.items():
        for i, f in enumerate(info["methods"]):
            if f is not None:
                out[f].append(("primary", cls, cls, i))
        for s in info.get("secondary", []):
            view, thunks = s.get("subobject"), s.get("thunks", {})
            for i, f in enumerate(s["methods"]):
                if f is None:
                    continue
                if i in thunks:
                    out[f].append(("thunk", cls, view, i))
                    out[thunks[i]["target"]].append(
                        ("override" if thunks[i]["vbase_field"] is None else "vtarget", cls, view, i))
                else:
                    out[f].append(("direct", cls, view, i))
    return out


def holder_class(holder) -> str | None:
    """The class a slot_holders entry makes `this` of the function: the subobject's for a direct secondary slot,
    the table's for a primary slot or a plain thunk's target. Thunks and virtual-base thunk targets make no claim
    (body_classes skips them)."""
    kind, cls, view, _ = holder
    return view if kind == "direct" else cls


def body_classes(holders) -> set | None:
    """The classes whose `this` a function takes, over its slot_holders entries; None when one of them is unknown
    (a secondary slot without a single subobject class)."""
    classes = {holder_class(h) for h in holders if h[0] not in ("thunk", "vtarget")}
    return None if None in classes else classes


def class_names(table: dict) -> tuple[dict[int, tuple[str, str]], collections.Counter]:
    """C4 {function: (name, evidence)} over every vtable slot (slot_holders): a body is named after the class all its
    holders' classes descend from, Class::vfN at that class's slot N, or Class::Sub_vfN when only thunks reach it
    (Class's implementation of slot N of its base subobject Sub). Thunks are left to names.py N13, which names them
    after their targets. A body whose holders have no common class, or a secondary slot whose subobject has no single
    class, stays unnamed."""
    ancestors = {c: set(info["bases"]) | {c} for c, info in table.items()}
    stats = collections.Counter()
    names = {}
    for f, hs in slot_holders(table).items():
        kinds = {h[0] for h in hs}
        if "thunk" in kinds:
            stats["thunk" if kinds == {"thunk"} else "thunk_also_a_slot_body_left_unnamed"] += 1
            continue
        classes = body_classes(hs)
        if classes is None:
            stats["secondary_slot_without_subobject_class"] += 1
            continue
        if not classes:
            stats["virtual_base_thunk_target_only_left_unnamed"] += 1
            continue
        common = [c for c in classes if all(c in ancestors.get(o, {o}) for o in classes)]
        if len(common) != 1:
            stats["ambiguous_left_unnamed"] += 1
            continue
        cls = common[0]
        own = [h[3] for h in hs if h[0] in ("primary", "direct") and holder_class(h) == cls]
        secondary = sum(h[0] != "primary" for h in hs)
        if own:
            slot = min(own)
            why = (f"vtable slot {slot} of {cls} only" if len(classes) == 1
                   else f"in {len(classes)} vtables; {cls} is an ancestor of all")
            names[f] = (f"{cls}::vf{slot}", why + (f" ({secondary} of the slots in secondary vtables)" if secondary
                                                    else ""))
            stats["single_class" if len(classes) == 1 else "common_ancestor"] += 1
            continue
        views = {(h[2], h[3]) for h in hs if h[0] == "override" and h[1] == cls}
        if len(views) != 1 or None in {v for v, _ in views}:
            stats["thunk_target_of_several_slots_left_unnamed"] += 1
            continue
        (view, slot), = views
        names[f] = (f"{cls}::{view}_vf{slot}", f"target of this-adjusting thunks in {cls}'s secondary vtable for its "
                    f"{view} subobject, slot {slot}" + (f" ({len(classes)} classes; {cls} is an ancestor of all)"
                                                        if len(classes) > 1 else ""))
        stats["thunk_target"] += 1
    return names, stats


def derive(project: str) -> dict:
    image = dolmod.load_file(os.path.join(project, "main.dol"))
    syms = read_symbols(os.path.join(project, "config", "symbols.txt"))
    with open(os.path.join(project, "build", "audit.json")) as f:
        aud = json.load(f)
    funcs = sorted((a, n, sec) for n, sec, a, rest in syms if "type:function" in rest)
    sec_of = {}
    for n, sec, a, rest in syms:
        sec_of.setdefault(a, sec)
    by_name = {n for n, _, _, _ in syms}
    starts = [a for a, _, _ in funcs]

    def section_for(addr):
        i = max(0, __import__("bisect").bisect_right(starts, addr) - 1)
        return funcs[i][2]

    corr = {"labels": [], "relocations": [], "renames": [], "stats": {}}
    taken = set(by_name)

    def add_label(addr, name, why):
        if any(a == addr for a in sym_addrs):
            return False
        n = name if name not in taken else f"{name}_{addr:08X}"
        taken.add(n)
        corr["labels"].append({"address": f"{addr:#010x}", "section": section_for(addr), "name": n, "evidence": why})
        return True

    sym_addrs = {a for _, _, a, _ in syms}
    # C1
    for e in aud["B2_calls_into_bodies"]["examples"]:
        t = int(e["target"], 16)
        name, why = entry_name(image.word(t), t)
        add_label(t, name, f"{len(e['sites'])}+ calls from e.g. {e['sites'][0]}; {why}")
    # C2
    for e in aud["R3_raw_lis"]["strong"]:
        site, value = int(e["site"], 16), int(e["value"], 16)
        target = next((n for n, _, a, _ in syms if a == value), None)
        if target is None:
            continue
        # find the pairing instruction again (same rule as the audit)
        rd = e["rd"]
        for nxt in range(site + 4, site + 24, 4):
            w2 = image.word(nxt)
            if (w2 >> 26) in (14, 24, 32, 34, 40, 42, 48, 50, 36, 38, 44, 52, 54) and ((w2 >> 16) & 0x1F) == rd:
                lo_is_ori = (w2 >> 26) == 24
                corr["relocations"] += [
                    {"source": f"{section_for(site)}:{site:#010x}", "type": "hi" if lo_is_ori else "ha",
                     "target": target, "evidence": f"lis r{rd} + {'ori' if lo_is_ori else 'low half'} at {nxt:#010x} builds {value:#010x} = {target}"},
                    {"source": f"{section_for(nxt)}:{nxt:#010x}", "type": "l", "target": target,
                     "evidence": f"pairs with lis at {site:#010x}"}]
                break
    # C3
    for e in aud["R2_relocation_targets"]["suspicious_examples"]:
        v = int(e["value"], 16)
        add_label(v, f"case_{v:08X}", f"relocation at {e['site']} points into {e['inside']} (jump table target)")
    # C4
    sdata = next(((a, a) for n, sec, a, r in syms if sec == ".sdata"), None)
    sdata_lo = min((a for n, sec, a, r in syms if sec == ".sdata"), default=None)
    sdata_sec = image.section_at_address(sdata_lo) if sdata_lo else None
    rtti = {}
    if sdata_sec:
        rtti = rtti_classes(image, (sdata_sec.address, sdata_sec.address_end),
                            {a for n, sec, a, r in syms if "type:object" in r},
                            {a: int(m.group(1), 16) for n, sec, a, r in syms if "type:function" in r
                             and (m := re.search(r"size:0x([0-9A-Fa-f]+)", r))})
        names, stats = class_names(rtti)
        corr["stats"]["rtti"] = {"classes_with_vtables": len(rtti),
                                 "slots": sum(1 for v in rtti.values() for m in v["methods"] if m is not None),
                                 "secondary_slots": sum(1 for v in rtti.values() for s in v["secondary"]
                                                        for m in s["methods"] if m is not None),
                                 "conflicting_names_left_out": len(rtti.conflicts), **stats}
        fn_names = {a: n for a, n, _ in funcs}
        for f, (raw, why) in sorted(names.items()):
            cur = fn_names.get(f)
            if cur is None:
                corr["stats"]["rtti"]["target_not_a_function"] = corr["stats"]["rtti"].get("target_not_a_function", 0) + 1
                continue
            if not re.fullmatch(r"fn_[0-9A-F]{8}", cur):
                continue  # dtk's own name wins
            new = sanitize(raw)
            if new in taken:
                new = f"{new}_{f:08X}"
            taken.add(new)
            corr["renames"].append({"address": f"{f:#010x}", "from": cur, "to": new, "class_name": raw, "evidence": why})
    corr["stats"].update({"labels": len(corr["labels"]), "relocations": len(corr["relocations"]),
                          "renames": len(corr["renames"])})
    return corr


def apply(src: str, dst: str, corr: dict, jobs: int) -> dict:
    if os.path.exists(dst):
        raise FileExistsError(dst)
    os.makedirs(os.path.join(dst, "config"))
    shutil.copyfile(os.path.join(src, "main.dol"), os.path.join(dst, "main.dol"))
    shutil.copyfile(os.path.join(src, "config", "splits.txt"), os.path.join(dst, "config", "splits.txt"))
    renames = {r["from"]: r["to"] for r in corr["renames"]}
    with open(os.path.join(src, "config", "symbols.txt"), encoding="utf-8") as f:
        lines = f.read().split("\n")
    out = []
    for line in lines:
        m = SYM_LINE.match(line)
        if m and m.group(1) in renames:
            line = renames[m.group(1)] + line[len(m.group(1)):]
        out.append(line)
    while out and out[-1] == "":
        out.pop()
    for lab in corr["labels"]:
        out.append(f"{lab['name']} = {lab['section']}:0x{int(lab['address'], 16):08X}; // type:label scope:global")
    with open(os.path.join(dst, "config", "symbols.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out) + "\n")
    with open(os.path.join(src, "config.yml"), encoding="utf-8") as f:
        cfg = f.read().rstrip("\n") + "\n"
    if corr["relocations"]:
        cfg += "add_relocations:\n"
        for r in corr["relocations"]:
            sec, addr = r["source"].split(":")
            cfg += f"  - source: {sec}:0x{int(addr, 16):08X}\n    type: {r['type']}\n    target: {r['target']}\n"
    with open(os.path.join(dst, "config.yml"), "w", newline="\n") as f:
        f.write(cfg)
    with open(os.path.join(dst, "corrections.json"), "w") as f:
        json.dump(corr, f, indent=1)
    r = subprocess.run([rebuild.TOOLS["dtk"], "--no-color", "dol", "split", "-j", str(jobs), "config.yml", "out"],
                       cwd=dst, capture_output=True, text=True)
    with open(os.path.join(dst, "split.log"), "w") as f:
        f.write(r.stdout + r.stderr)
    if r.returncode != 0:
        raise RuntimeError(f"dtk split failed: {r.stderr[-2000:]}")
    rep = rebuild.run(dst, jobs)
    if not rep.get("byte_exact"):
        raise RuntimeError(f"corrections broke the rebuild: {json.dumps(rep)[:2000]}")
    return rep


if __name__ == "__main__":
    src, dst = sys.argv[1], sys.argv[2]
    jobs = int(sys.argv[3]) if len(sys.argv) > 3 else 16
    corr = derive(src)
    print(json.dumps(corr["stats"], indent=1))
    rep = apply(src, dst, corr, jobs)
    print("byte_exact", rep["byte_exact"], rep["bytes_by_owner"])
    print(auditmod.summary(auditmod.run(dst)))
