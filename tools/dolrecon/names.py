"""Evidence-based name recovery (docs/DOLRECON.md "Names").

    python -B -m dolrecon.names <corrected project> <new project> [jobs]       (run from source/tools)

The game binary carries no symbol table; dtk names what it recognises (SDK / runtime) and corrections.py names
virtual functions from the game's RTTI. This adds names ONLY where the binary (or the project's own validated work)
gives evidence, each with its rule and evidence in <new>/names.json. Rules, strongest first; a symbol keeps the first
name it gets, and dtk's / corrections' names are never replaced:

  M0 matched   source/decomp/matched.json: functions whose source compiles byte-identical -> their real
               (mangled) names. The only rule that may replace an existing name.
  N8 rw        .build/dolrecon/rw/rw_names.json: RenderWare functions / globals aligned with the RW 3.7 PC libraries.
  N10 driver   .build/dolrecon/rw/rw_driver.json: the GameCube RenderWare driver named by role (dolrecon.rwdriver).
  N11 typereg  .build/dolrecon/game/registrations.json: object-type table callbacks by dispatched role (gamereg).
  N12 role     .build/dolrecon/game/classroles.json: a virtual function in a base-class slot whose dispatcher is read
               from the binary -> Owner__role (classroles); may replace corrections' slot placeholder C__vfN.
  N9 rw cb     .build/dolrecon/rw/callbacks.json: a function passed as a RenderWare callback -> <caller>__<RwFunc>_cb.
  N1 owner     project-validated SDK names (extract/native-owners.txt: functions the project replaced natively).
  N6 ctor / N7 dtor  object code storing class C's vtable into this: C__dtor when it deletes this under the
               delete flag, else C__ctor (most-derived stored class only). Also sizeof(C) from operator new sites.
               A deleting destructor held only by vtables of a class D derived from C is D__dtor (D's destructor
               stores no vtable of its own). A constructor with a virtual-base flag is C__ctor when it stores C's
               vtable at the offset every constructor of C uses (dolrecon.vcalls).
  N2 construct a non-virtual function that stores class C's vtable into the object operator new returned or into
               a global object -> C__construct (the class all other stored classes are ancestors of). A function
               that builds a C only on the stack (a temporary, an exception to throw) gets no name from it.
  N3 string    a function referencing a distinctive string literal (>= 4 letters/digits, not a format string) ->
               ref_<string> (the most distinctive one when several).
  N5 self-name a function that is the ONLY user of a message naming it (CRI error code + name, SDK name(), or a
               C++ Class::Method() in debug text) -> that name. Applied before N3.
  N13 thunk    a this-adjusting thunk in a secondary vtable slot (corrections.vtable_thunk) -> <target>__thunk, or
               __vthunk through a virtual base, once its target has a name. Applied after N2.
  N14 static   a .ctors entry __init_cpp calls before main -> __sinit_<address>; a destructor passed to
               __register_global_object by one function G -> G__static_dtor (CodeWarrior's runtime, pinned words).
  N15 singleton a function that constructs one static object once, registers its destructor and returns it (or a
               global written only with it) -> C__instance (C = the destructor's class) or static_<object>__instance.
  N16 boost    Boost 1.32 shared_ptr / shared_count bodies, word for word (dolrecon.boostsp): sp_counted_base
               release, shared_count copy / assign / dtor (one each), every shared_ptr<T> copy / assign / dtor.
  N4 helper    a function whose only direct caller is a named function G -> G__sub<k> (k = its order among G's
               callees), applied up to 4 levels deep. Structural: "a helper of G", nothing more.
  D1 data      float / double constants -> kF32_<value> / kF64_<value>; strings -> str_<text>; a class's vtable ->
               C__vtable, a secondary one (dtk object) -> C__vtable_0x<subobject offset>; its RTTI record -> C__rtti;
               the record's base-class list -> C__rtti_bases.
Every new name is unique (an address suffix on collision). The new project is rebuilt and must stay byte-exact.
"""

from __future__ import annotations

import bisect
import collections
import json
import os
import re
import shutil
import struct
import subprocess
import sys

from . import boostsp
from . import corrections as corrmod
from . import dol as dolmod
from . import ppc
from . import rebuild
from . import staticstore
from . import strrefs
from . import vcalls

SYM = re.compile(r"^(\S+) = ([^:]+):0x([0-9A-Fa-f]+);(.*)$")

R_REL24 = 10


def ident(text: str, limit: int = 48) -> str:
    s = re.sub(r"[^A-Za-z0-9]+", "_", text).strip("_")[:limit].strip("_")
    return ("_" + s) if s and s[0].isdigit() else s


def fit(name: str, limit: int) -> str:
    """name shortened to limit characters, keeping its last '__' part (the role / member: a cut that drops it read
    boost__detail__sp_counted_base_impl_..._boost__check for a dispose, Astra answer 6): the prefix is cut."""
    if len(name) <= limit:
        return name
    head, sep, tail = name.rpartition("__")
    if sep and len(tail) + 3 <= limit:
        return head[:limit - len(tail) - 2].rstrip("_") + "__" + tail
    return name[:limit].rstrip("_")


def symbol(text: str, limit: int = 96) -> str:
    """A name from a tool that already made an identifier (RenderWare's _rpClumpOpen, Owner__role): kept as it is
    (fit to the limit). ident() would strip the leading underscore and collapse the class separator '__'; other
    text goes through it one '__'-separated part at a time."""
    if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", text):
        return fit(text, limit)
    s = fit("__".join(p for p in (ident(q, limit) for q in text.split("__")) if p), limit)
    return ("_" + s) if s and s[0].isdigit() else s


def top_of(classes, ancestors):
    """The class every other one is an ancestor of; None when unrelated classes are among them."""
    cs = set(classes)
    top = [c for c in cs if all(o == c or o in ancestors.get(c, {c}) for o in cs)]
    return top[0] if len(top) == 1 else None


def dtor_class(stored, holders, ancestors):
    """The class of a deleting destructor that stores `stored`'s vtable: the class whose vtables hold the body
    (the holder all other holders descend from) when that class derives from `stored`, else `stored`."""
    own = [c for c in holders if all(c in ancestors[o] for o in holders)]
    return own[0] if len(own) == 1 and own[0] != stored and stored in ancestors[own[0]] else stored


def construction(stores, vptr_class, vptr_offset, ancestors):
    """("ctor", C) when a function stores C's vtable into this at C's vptr offset (C's constructor); (where, C)
    when it stores C's vtable into one new or global object at C's vptr offset; (None, None) otherwise - a stack
    temporary, a member subobject at another offset, or a vptr slot also written with something else.
    stores: vcalls.vtable_stores rows (pc, vtable or None, kind, offset, object). The class of one (object, offset)
    is the most derived of the vtables stored there, which needs them to form one base -> derived chain: it does not
    depend on the order the stores are visited in (Astra answer 5: no "last store wins")."""
    slots = collections.defaultdict(set)                # (kind, object, offset) -> classes stored (None = other)
    for _, vt, kind, off, obj in stores:
        slots[(kind, obj, off)].add(None if vt is None else vptr_class[vt])
    final = collections.defaultdict(set)                # (kind, object) -> classes proven at their vptr offset
    for (kind, obj, off), classes in slots.items():
        if classes == {None} or None in classes:
            continue
        top = top_of(classes, ancestors)
        if top and vptr_offset.get(top) == off:
            final[(kind, obj)].add(top)
    for kind, where in (("this", "ctor"), ("new", "the object operator new returned"), ("global", "a global object")):
        tops = {top_of(cs, ancestors) for (k, _), cs in final.items() if k == kind}
        if len(tops) == 1 and None not in tops:
            return where, tops.pop()
    return None, None


def this_classes(rows, vptr_class, ancestors):
    """Class stored at each vptr offset of this (vcalls rows of kind "this"): the most derived of a base -> derived
    chain, whatever the order of the stores. None (undetermined) when an offset holding a vtable also receives some
    other value, or unrelated classes."""
    slots = collections.defaultdict(set)
    for _, vt, _, off, _ in rows:
        slots[off].add(None if vt is None else vptr_class[vt])
    out = []
    for off, classes in sorted(slots.items()):
        if classes == {None}:
            continue
        if None in classes or top_of(classes, ancestors) is None:
            return None
        out.append(top_of(classes, ancestors))
    return out


def final_classes(rows, finals, vptr_class):
    """Class in each vptr slot of this when the function returns (vcalls.object_code "finals"): every reached
    return must hold the same known vtable at every offset this function stores a vtable at; else None."""
    offsets = sorted({off for _, vt, _, off, _ in rows if vt is not None})
    if not offsets:
        return []
    if not finals:
        return None
    out = []
    for off in offsets:
        values = {f.get(off) for f in finals}
        if len(values) != 1 or values.copy().pop() not in vptr_class:
            return None
        out.append(vptr_class[values.pop()])
    return out


def float_name(prefix: str, v: float) -> str:
    """kF32_1, kF32_0_5, kF32_m22_4, kF32_1em05: the value, readable (single values shown at single precision)."""
    if v != v:
        return f"{prefix}_nan"
    if v in (float("inf"), float("-inf")):
        return f"{prefix}_{'m' if v < 0 else ''}inf"
    t = (f"{v:.7g}" if prefix == "kF32" else f"{v:.15g}").replace("-", "m").replace(".", "_").replace("+", "")
    t = re.sub(r"[^A-Za-z0-9_]", "_", t)
    return f"{prefix}_{t[:32]}"


def main(argv):
    src, dst = argv[0], argv[1]
    jobs = int(argv[2]) if len(argv) > 2 else 16
    image = dolmod.load_file(os.path.join(src, "main.dol"))
    lines = open(os.path.join(src, "config", "symbols.txt"), encoding="utf-8").read().split("\n")
    syms = []  # [name, section, address, rest]
    for line in lines:
        m = SYM.match(line)
        syms.append([m.group(1), m.group(2), int(m.group(3), 16), m.group(4)] if m else None)
    by_name = {s[0]: s for s in syms if s}
    funcs = sorted((s[2], s) for s in syms if s and "type:function" in s[3])
    fstarts = [a for a, _ in funcs]
    fsize = {a: int(re.search(r"size:0x([0-9A-Fa-f]+)", s[3]).group(1), 16) for a, s in funcs}
    fsym = {a: s for a, s in funcs}

    def owner(addr):
        i = bisect.bisect_right(fstarts, addr) - 1
        return fstarts[i] if i >= 0 and addr < fstarts[i] + fsize[fstarts[i]] else None

    def unnamed(s):
        return re.fullmatch(r"(fn|lbl)_[0-9A-F]{8}", s[0]) is not None or s[0].startswith("@")

    taken = {s[0] for s in syms if s}
    result = {}  # address -> {"old", "new", "rule", "evidence"}

    def give(s, new, rule, evidence, over_slot=False, over=None, **facts):
        # facts: structured fields for other tools (e.g. class=): they read these, never the evidence prose
        # over_slot: the name may also replace corrections' slot placeholder C__vfN (N12 names the same slot's role)
        # over: a regex of another placeholder the name may replace (dtk's dtor_XXXXXXXX)
        if s[2] in result or not (unnamed(s) or over_slot and re.search(r"__vf\d+$", s[0])
                                  or over and re.fullmatch(over, s[0])):
            return False
        n = new if new not in taken else f"{new}_{s[2]:08X}"
        if n in taken:
            return False
        taken.add(n)
        result[s[2]] = {"old": s[0], "new": n, "rule": rule, "evidence": evidence, **facts}
        return True

    # ---- M0 matched source ----------------------------------------------------------------------------------
    # source/decomp/matched.json (dolrecon.decomp_verify): functions whose source compiles byte-identical with the
    # game's compiler. Proof outranks every heuristic, so M0 is the one rule that may replace an existing name.
    manifest = os.path.join(os.path.dirname(__file__), "..", "..", "decomp", "matched.json")
    if os.path.exists(manifest):
        with open(manifest, encoding="utf-8") as f:
            proof = json.load(f)
        by_addr = {s[2]: s for s in syms if s}
        for entry in proof.get("files", []):
            # generated files (decomp/auto) reuse the project's names of their day: matching proves their code, not
            # their names, so they give no M0 name (found 2026-10-31: 733 heuristic names would have read as proof)
            if entry["file"].startswith("auto/"):
                continue
            for row in entry.get("functions", []):
                if not row.get("matched"):
                    continue
                s = by_addr.get(int(row["address"], 16))
                new = row["name"]
                if s is None or s[2] in result or (new in taken and s[0] != new):
                    continue
                if s[0] != new:
                    taken.add(new)
                    result[s[2]] = {"old": s[0], "new": new, "rule": "M0 matched",
                                    "evidence": f"source/decomp/{entry['file']} compiles byte-identical "
                                                f"(cc {entry['cc']}, {entry['flags']})"}

    # ---- N8 RenderWare --------------------------------------------------------------------------------------
    # .build/dolrecon/rw/rw_names.json (dolrecon.rwalign): the game's RenderWare functions and globals aligned with
    # the RenderWare 3.7 PC static libraries (rules and held-out accuracy in rwalign.py).
    rw_path = os.path.join(os.path.dirname(os.path.abspath(src)), "rw", "rw_names.json")
    if os.path.exists(rw_path):
        with open(rw_path, encoding="utf-8") as f:
            rw = json.load(f)
        by_addr_rw = {s[2]: s for s in syms if s}
        for a_hex, row in rw["names"].items():
            s = by_addr_rw.get(int(a_hex, 16))
            if s is not None:
                give(s, symbol(row["name"]), "N8 renderware",
                     f"RenderWare 3.7 {row['module']} ({row['rule']}: {row['evidence'][:80]})")
        for guest, pcname in rw.get("globals", {}).items():
            # keyed by address (rwalign since 2026-11-01) or, in older files, by the analysed project's symbol name
            s = by_addr_rw.get(int(guest, 16)) if guest.startswith("0x") else by_name.get(guest)
            if s is not None:
                give(s, symbol(pcname), "N8 renderware", "RenderWare 3.7 global (rwalign D1: the only global left "
                     "unmapped on each side of a named pair; or T1: the same slot of a paired data table)")

    # ---- N10 RenderWare GameCube driver ---------------------------------------------------------------------
    # .build/dolrecon/rw/rw_driver.json (dolrecon.rwdriver): the GameCube device driver named by role from the
    # tables every RenderWare driver registers (RwStandardFunc list, RwDevice block, RxNodeDefinitions).
    drv_path = os.path.join(os.path.dirname(os.path.abspath(src)), "rw", "rw_driver.json")
    if os.path.exists(drv_path):
        with open(drv_path, encoding="utf-8") as f:
            drv = json.load(f)
        by_addr_drv = {s[2]: s for s in syms if s}
        for a_hex, row in list(drv["functions"].items()) + list(drv["data"].items()):
            s = by_addr_drv.get(int(a_hex, 16))
            if s is not None:
                give(s, symbol(row["name"]), "N10 rw driver", row["evidence"][:160])

    # ---- N11 game type registrations --------------------------------------------------------------------------
    # .build/dolrecon/game/registrations.json (dolrecon.gamereg): callbacks of the object-type table 0x8052C1A0,
    # named <TYPE>__create / __stageLoad / __stageRelease by the role their dispatcher gives them (role names).
    reg_path = os.path.join(os.path.dirname(os.path.abspath(src)), "game", "registrations.json")
    if os.path.exists(reg_path):
        with open(reg_path, encoding="utf-8") as f:
            reg = json.load(f)
        by_addr_reg = {s[2]: s for s in syms if s}
        for a_hex, row in reg["functions"].items():
            s = by_addr_reg.get(int(a_hex, 16))
            if s is not None:
                give(s, symbol(row["name"]), "N11 type registration", row["evidence"][:160])

    # ---- N12 class roles ---------------------------------------------------------------------------------------
    # .build/dolrecon/game/classroles.json (dolrecon.classroles): a virtual function in a base-class slot whose
    # dispatcher was read from the binary (Task slot 1 update, Gadget slot 2 exec / 5 checkRemove) -> Owner__role.
    roles_path = os.path.join(os.path.dirname(os.path.abspath(src)), "game", "classroles.json")
    if os.path.exists(roles_path):
        with open(roles_path, encoding="utf-8") as f:
            roles = json.load(f)
        by_addr_role = {s[2]: s for s in syms if s}
        for a_hex, row in roles["functions"].items():
            s = by_addr_role.get(int(a_hex, 16))
            if s is not None:
                give(s, symbol(row["name"]), "N12 class role", row["evidence"][:200], over_slot=True)

    # ---- N9 RenderWare callbacks ---------------------------------------------------------------------------
    # .build/dolrecon/rw/callbacks.json (dolrecon.rwheaders): game functions whose address a caller loads into a
    # callback parameter of a RenderWare call (types from the RW 3.7 SDK headers). Named after that role.
    cb_path = os.path.join(os.path.dirname(os.path.abspath(src)), "rw", "callbacks.json")
    if os.path.exists(cb_path):
        with open(cb_path, encoding="utf-8") as f:
            cbs = json.load(f)["callbacks"]
        by_addr_cb = {s[2]: s for s in syms if s}
        for fname, row in cbs.items():
            s = by_addr_cb.get(int(row["address"], 16)) if "address" in row else by_name.get(fname)
            if s is None:
                continue
            rw_funcs = sorted({u["rw"] for u in row["uses"]})
            callers = sorted({u["caller"] for u in row["uses"] if u["caller"] != fname})
            params = row.get("params") or sorted({u["param"] for u in row["uses"]})
            # named by role: <RenderWare function>_<parameter> (RpAtomicRegisterPlugin_constructCB), or the type
            base = (f"{rw_funcs[0]}_{params[0]}" if len(rw_funcs) == 1 and len(params) == 1 else
                    rw_funcs[0] + "_cb" if len(rw_funcs) == 1 else row["type"])
            if len(callers) == 1 and not re.fullmatch(r"fn_[0-9A-F]{8}", callers[0]):
                base = f"{callers[0][:64]}__{base}"
            u = row["uses"][0]
            give(s, re.sub(r"[^A-Za-z0-9_]", "_", base)[:110], "N9 rw callback",
                 f"passed as {u['param']} ({row['type']}) to {u['rw']} at {u['site']}" +
                 (f" (+{row['count'] - 1} more)" if row["count"] > 1 else ""))

    # ---- references from dtk's relocations (corrected project's objects) -------------------------------------
    blocks = rebuild.plan(os.path.join(src, "out", "asm"))
    refs = collections.defaultdict(list)        # function -> [(site, type, target address)]
    callers = collections.defaultdict(set)      # function -> {calling functions}
    callees_in_order = collections.defaultdict(list)
    for b in blocks:
        if b.section not in (".text", ".init"):
            continue
        path = os.path.join(src, "build", "obj", b.unit + ".o")
        if not os.path.exists(path):
            continue
        for sec, off, rtype, sym in rebuild._relocs(path):
            if sec != b.section:
                continue
            name = "@" + sym[len(rebuild.AT_PREFIX):] if sym.startswith(rebuild.AT_PREFIX) else sym
            t = by_name.get(name)
            if t is None:
                continue
            site = b.start + off
            f = owner(site)
            if f is None:
                continue
            refs[f].append((site, rtype, t[2]))
            if rtype == R_REL24:
                w = image.word(site & ~3)
                if (w >> 26) == 18 and (w & 1) and t[2] in fsym:  # bl: a call
                    callers[t[2]].add(f)
                    if t[2] not in callees_in_order[f]:
                        callees_in_order[f].append(t[2])

    # ---- N1 project-validated SDK owners -------------------------------------------------------------------
    owners_path = os.path.join(os.path.dirname(__file__), "..", "..", "extract", "native-owners.txt")
    if os.path.exists(owners_path):
        for line in open(owners_path, encoding="utf-8"):
            p = line.split()
            # "0xADDR Name" only: longer lines are prose descriptions ("0x8032D3D4 the RtGCNFS drive-error callback")
            if len(p) == 2 and not line.startswith("#") and re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", p[1]):
                a = int(p[0], 16)
                if a in fsym:
                    give(fsym[a], symbol(p[1], 64), "N1 owner", "extract/native-owners.txt (project-validated native owner)")

    # ---- N14 static initializers -----------------------------------------------------------------------------
    # dolrecon.staticstore: __init_cpp / __register_global_object / __destroy_global_chain pinned as whole bodies;
    # __init_cpp calls every .ctors word before the first 0 once, before main: each is a unit's static initializer
    # -> __sinit_<address> (CodeWarrior names it __sinit_<file>_cpp; the file name is not in the binary). 281 in PAL.
    by_name = {}
    for s in syms:
        if s:
            by_name.setdefault(s[0], s[2])
    runtime_ok = staticstore.runtime_proven(image, by_name, fsize, by_name.get("_ctors"))
    if runtime_ok:
        for slot, w in staticstore.static_initializers(image, by_name["_ctors"]):
            if w in fsym:
                give(fsym[w], f"__sinit_{w:08X}", "N14 static init",
                     f".ctors entry {slot:#010x}: __init_cpp calls it once at start-up, before main")

    # ---- N16 Boost shared_ptr / shared_count ------------------------------------------------------------------
    # dolrecon.boostsp: whole bodies pinned word for word, external calls by role. The four primitives (one copy
    # each) and every shared_ptr<T> copy / assignment / destructor instance -> boost__...; may replace dtk's
    # dtor_XXXXXXXX placeholder (dtk names exception-table destructors so). class = the Boost class (spine A3).
    sp_roles = boostsp.recognise(
        image, [(a, fsize[a]) for a in fstarts if fsym[a][1].startswith(".text")],
        {a: result[a]["new"] if a in result else fsym[a][0] for a in fstarts})
    for a, role in sorted(sp_roles.items()):
        cls, member = boostsp.NAMES[role]
        give(fsym[a], corrmod.sanitize(f"{cls}::{member}"), "N16 boost shared_ptr",
             f"Boost 1.32 {role} body, word for word (dolrecon.boostsp): {boostsp.EVIDENCE[role]}",
             over=r"dtor_[0-9A-F]{8}", **{"class": cls})

    # ---- RTTI: classes, vtables, records --------------------------------------------------------------------
    sdata = next((image.section_at_address(s[2]) for s in syms if s and s[1] == ".sdata"), None)
    objects = {s[2] for s in syms if s and "type:object" in s[3]}
    table = corrmod.rtti_classes(image, (sdata.address, sdata.address_end), objects, fsize) if sdata else {}
    vtable_class = {vt: cls for cls, info in table.items() for vt in info.get("vtables", [info["vtable"]]) if vt}
    base_list_class = {info["base_list"]: cls for cls, info in table.items() if info["base_list"]}
    # A constructor stores the primary vtable's address (the object {record, 0, methods...}, corrections.rtti_classes)
    # at the class's vptr offset, and each secondary vtable's {record, -k, ...} at the base subobject's: every one of
    # them is the class's, so a store of any of them is a store of the class.
    vptr_class = dict(vtable_class)
    secondary_at = {}                           # secondary vtable address -> (class, subobject offset)
    for cls, info in table.items():
        for s in info["secondary"]:
            vptr_class[s["vtable"]] = cls
            secondary_at[s["vtable"]] = (cls, -s["offset"])
    ancestors = {c: set(info["bases"]) | {c} for c, info in table.items()}
    slot_holders = corrmod.slot_holders(table)  # every vtable slot, secondary ones and thunks included
    in_vtable = set(slot_holders)
    holders = {}                                # function -> classes whose vtables hold it as a body of theirs
    for f, hs in slot_holders.items():
        holders[f] = corrmod.body_classes(hs)                    # None: a holder's class is unknown

    # ---- N6 constructor / N7 destructor / allocation sizes -------------------------------------------------
    # CodeWarrior's object code on dolrecon.vcalls' must-analysis over the function's blocks (vcalls.object_code;
    # until 2026-11-01 a linear scan with one register state across branches and "last store wins", Astra answer 5):
    #   N7 dtor   stores class C's vtable into `this` AND calls operator delete on `this` only where the entry flag
    #             (r4) tested by extsh. / cmpwi 0 is > 0 - the branch condition is proven, not a nearby instruction:
    #             C's deleting destructor -> C__dtor (C = the class whose vtables hold the body, names.dtor_class).
    #   N6 ctor   stores class C's vtable into `this`, never deletes it and does not test a virtual-base flag ->
    #             C__ctor. Per offset of this the stored classes must form one base -> derived chain; another
    #             value stored at a vptr slot, or unrelated classes, leave the function unnamed.
    #   size      `li r3, N` + `bl operator new`, then C's vtable stored into that object at C's vptr offset (an
    #             inlined constructor; a vtable at another offset is a member subobject), or a call on the object to
    #             an N6 constructor of C -> operator new allocates N for a C (every site must agree, else no size).
    #             An allocation size, recorded as such: it is sizeof(C) only if no site allocates more.
    # operator new / delete are taken from M0 (proven by matching), never assumed.
    op_new = next((a for a, r in result.items() if r["new"] == "__nw__FUl"), None)
    op_del = next((a for a, r in result.items() if r["new"] == "__dl__FPv"), None)
    class_info = collections.defaultdict(lambda: {"sizes": collections.Counter(), "ctors": [], "dtors": [],
                                                  "size_from": []})
    vfuncs = [(a, fsym[a][0], fsize[a], fsym[a][1]) for a in fstarts]
    entries = vcalls.interior_entries(image, vfuncs)
    vptr_offset = vcalls.survey(image, vfuncs, table, entries)[0]

    def top_class(classes):
        return top_of(classes, ancestors)

    new_calls = []
    undetermined = 0
    if op_new is not None and op_del is not None:
        vtables = set(vptr_class)
        for f in fstarts:
            if fsize.get(f, 0) < 8:
                continue
            facts = vcalls.object_code(vcalls.Function(image, f, fsize[f], entries.get(f, ())), vtables, op_new,
                                       op_del)
            for site, size in facts["allocs"].items():
                rows = [r for r in facts["stores"] if r[2] == "new" and r[4] == site]
                if any(r[1] is not None for r in rows):
                    # an inlined constructor: calls on the object are BASE constructors, only the vtable counts
                    _, cls = construction(rows, vptr_class, vptr_offset, ancestors)
                    if size and cls:
                        class_info[cls]["sizes"][size] += 1
                        class_info[cls]["size_from"].append(f"0x{site:08X} inline vtable store")
                elif size:
                    new_calls.append((size, facts["alloc_calls"].get(site, set()), site))
            this_rows = [r for r in facts["stores"] if r[2] == "this"]
            if facts["deletes"]:
                # a destructor stores the most derived vtable FIRST, then its bases': the chain's top; another
                # value at a vptr slot (an unlisted derived class) leaves it undetermined
                stores = this_classes(this_rows, vptr_class, ancestors)
            else:
                # a constructor ends with the class's own vtable in each vptr slot: the value stored LAST on every
                # path to every return (an unlisted base's vtable stored earlier does not matter)
                stores = final_classes(this_rows, facts["finals"], vptr_class)
            if stores is None:
                undetermined += 1
                continue
            if not stores or facts["plain_delete"]:
                continue
            cls = top_class(stores)
            if cls is None:
                continue
            cur = fsym[f][0]
            heuristic = unnamed(fsym[f]) or re.search(r"__vf\d+$", cur) is not None
            if facts["deletes"]:
                # A derived class whose destructor has nothing of its own to undo stores no vtable of its own: the
                # body stores only its base's (found 2026-11-01: 431 bodies, e.g. 84 EnemyAIState_* deleting
                # destructors read TEnemyAIState__dtor). The vtable slots holding the body name its class.
                stored, via = cls, ""
                if f in holders and holders[f] is None:
                    undetermined += 1                               # unknown ownership is no evidence for the base
                    continue
                cls = dtor_class(cls, holders.get(f) or set(), ancestors)
                if cls != stored:
                    via = f"; only {cls}'s vtables hold the body (it derives from the stored class)"
                class_info[cls]["dtors"].append(f)
                if heuristic and f not in result:
                    n = corrmod.sanitize(cls + "::dtor")
                    n = n if n not in taken else f"{n}_{f:08X}"
                    if n not in taken:
                        taken.add(n)
                        result[f] = {"old": cur, "new": n, "rule": "N7 dtor",
                                     "evidence": f"stores {stored}'s vtable into this and calls operator delete on "
                                                 f"this only where the delete flag is > 0 (extsh. r4 -> branch){via}",
                                     "class": cls, "stored_class": stored}
            elif facts["flag_tested"]:
                # a constructor of a class with VIRTUAL bases (found 2026-10-31: 0x802919BC stores BossCtrlUnit's
                # vtable at this+0x30, the virtual base, and the derived vtable at +4 from an unlisted table): the
                # stored classes do not name its class; the vptr offset does (N2 block below)
                continue
            elif f not in in_vtable:
                class_info[cls]["ctors"].append(f)
                give(fsym[f], corrmod.sanitize(cls + "::ctor"), "N6 ctor",
                     f"stores {cls}'s vtable into this (stored: {', '.join(dict.fromkeys(stores))}); never deletes",
                     **{"class": cls})

    ctor_class = {f: cls for cls, ci in class_info.items() for f in ci["ctors"]}
    for size, targets, site in new_calls:
        classes = {ctor_class[t] for t in targets if t in ctor_class}
        if len(classes) == 1:
            cls = classes.pop()
            tgt = next(t for t in sorted(targets) if ctor_class.get(t) == cls)
            class_info[cls]["sizes"][size] += 1
            class_info[cls]["size_from"].append(f"0x{site:08X} then constructor 0x{tgt:08X}")

    # ---- N2 construct / N6 ctor at the class's own vptr offset ------------------------------------------------
    # Where a non-virtual function N6 / N7 left unnamed stores a vtable it loads (dolrecon.vcalls must-analysis):
    #   into this, at the offset every constructor of C stores C's vtable (vcalls vptr): C's constructor. N6 leaves
    #     constructors with a virtual-base flag (extsh. r4) unnamed, because there the stored vtables need not name
    #     the class (0x802919BC); the vptr offset does (a virtual base's vtable sits at another offset).
    #   into the object operator new returned, or at a constant address: it constructs a C -> C__construct.
    #   into a stack temporary only, or nowhere: no name. It uses a C (CodeWarrior's __dynamic_cast 0x803A1AFC builds a
    #     std::bad_cast to throw; Player::Executer's command handler 0x800721A8 builds InitializeArgs to pass): until
    #     2026-11-01 N2 named 42 of these C__construct, as if they were C's.
    class_uses = {}             # function -> the class it builds a temporary of / whose vtable it loads (not named)
    for f, rs in refs.items():
        if f in in_vtable or f in result or not unnamed(fsym[f]):
            continue
        loaded = {t for _, rt, t in rs if t in vptr_class}
        if not loaded:
            continue
        # the same eligibility the N6 / N7 pass applies: a function deleting this is no constructor (Astra 6)
        facts = vcalls.object_code(vcalls.Function(image, f, fsize[f], entries.get(f, ())), loaded, op_new, op_del)
        stores = facts["stores"]
        kind, top = construction(stores, vptr_class, vptr_offset, ancestors)
        if kind and (facts["deletes"] or facts["plain_delete"]):
            continue
        if kind == "ctor":
            class_info[top]["ctors"].append(f)
            give(fsym[f], corrmod.sanitize(top + "::ctor"), "N6 ctor",
                 f"stores {top}'s vtable at this+{vptr_offset[top]:#x}, the offset every constructor of {top} "
                 f"stores it at (a constructor with a virtual-base flag); never deletes", **{"class": top})
        elif kind:
            give(fsym[f], corrmod.sanitize(top + "::construct"), "N2 construct",
                 f"stores {top}'s vtable into {kind}; not itself a virtual method", **{"class": top})
        else:
            # no name, but the fact stays: it builds (or loads the vtable of) a C - library evidence for the spine
            used = top_of({vptr_class[t] for t in loaded}, ancestors)
            if used:
                class_uses[f] = {"class": used, "evidence": (
                    f"builds a {used} on the stack" if any(r[2] == "stack" and r[1] for r in stores) else
                    f"loads {used}'s vtable address")}

    # ---- N13 thunk -----------------------------------------------------------------------------------------
    # A secondary vtable slot holding a this-adjusting thunk (corrections.vtable_thunk: `addi r3,r3,<the table's
    # offset>; b T`, or the same through a virtual base's offset field) is compiler-made glue for T, the class's
    # implementation of that base subobject's slot -> <T's name>__thunk / __vthunk, once T has a name.
    for f, hs in sorted(slot_holders.items()):
        if f not in fsym or f in result or not unnamed(fsym[f]) or any(h[0] != "thunk" for h in hs):
            continue
        facts = {(h[1], h[2], h[3]) for h in hs}
        info = next(s["thunks"][h[3]] for h in hs for s in table[h[1]]["secondary"]
                    if s["methods"][h[3]:h[3] + 1] == [f] and h[3] in s["thunks"])
        target = info["target"]
        tname = result[target]["new"] if target in result else fsym[target][0] if target in fsym else None
        if tname is None or re.fullmatch(r"fn_[0-9A-F]{8}", tname):
            continue
        cls, view, slot = sorted(facts)[0]
        via = (f"this += *(this+{info['vbase_field']:#x}) {info['adjust']:+#x} (a virtual base)"
               if info["vbase_field"] is not None else f"this {info['adjust']:+#x}")
        where = "; ".join(f"slot {k} of {c}'s secondary vtable for its {v} subobject" for c, v, k in sorted(facts))
        give(fsym[f], tname[:110] + ("__vthunk" if info["vbase_field"] is not None else "__thunk"), "N13 thunk",
             f"{where}: {via}, then b {tname}", **{"class": cls},
             holders=[{"class": c, "subobject": v, "slot": k} for c, v, k in sorted(facts)])

    # ---- strings -------------------------------------------------------------------------------------------
    def cstring(addr):
        sec = image.section_at_address(addr)
        if sec is None:
            return None
        off = sec.file_offset + addr - sec.address
        end = image.raw.find(b"\0", off, min(off + 200, sec.file_end))
        if end < 0:
            return None
        try:
            return image.raw[off:end].decode("ascii")
        except UnicodeDecodeError:
            return None

    string_at = {s[2]: cstring(s[2]) for s in syms if s and "data:string" in s[3]}
    # strings reached through a string pool (dolrecon.strrefs: register values, not relocations, which only name the
    # pool). Keyed POOL | address so a relocation to a pool never counts as a use of the pool's first string.
    POOL = 1 << 32
    pool_text = strrefs.pools(image, syms)
    # A relocation to a string is no use of it: dtk splits a unit's literals into one object each, and code reaches
    # the others as the FIRST one's address plus an offset (adxt_GetTime: lis/addi r31 = adxt_SetKeyString's
    # message, then addi r3,r31,off = its own; found 2026-11-01, it lost its N5 name). A function uses a string
    # strrefs sees only where the value is used (POOL | address below); relocation keys count for the others.

    def used_text(t):
        """The text a function's reference t uses as a string, or None."""
        if t not in string_at or not string_at[t] or t < POOL and t in pool_text:
            return None
        return string_at[t]
    text0 = next(sec for sec in image.sections if sec.kind == "text")
    sda = strrefs.sda_bases(image, text0.address, text0.address + text0.size)
    for f, rs in strrefs.pooled_refs(image, [(a, fsize[a]) for a in fstarts], pool_text, sda).items():
        for site, a in rs:
            refs[f].append((site, None, POOL | a))
            string_at[POOL | a] = pool_text[a]

    def distinctive(text):
        if not text or "%" in text or len(re.sub(r"[^A-Za-z0-9]", "", text)) < 4:
            return 0
        score = len(text)
        if re.search(r"\.(bin|dff|txd|one|adx|sfd|mtn|bon|prs|tex|txt|csv|thp|arc)$", text, re.I):
            score += 100
        if "::" in text or re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]{3,}", text):
            score += 50
        return score

    # ---- N5 self-name -------------------------------------------------------------------------------------
    # Library code reports errors under its own name: CRI "E02080849 adxt_ResetEntry: parameter error", the SDK
    # "DVDReadAsync(): specified area ...", game debug text "SetCameraDataManager::RequestFileLoad() %s ...".
    # The name is taken only when exactly ONE function uses that text (a message in several functions is an inlined
    # copy or a shared reporter, which names no single function), and only in the forms below.
    text_users = collections.defaultdict(set)
    for f, rs in refs.items():
        for _, _, t in rs:
            if used_text(t):
                text_users[used_text(t)].add(f)
    c_self = re.compile(r"^\s*(?:(?:Warning|WARNING|Error|ERROR)\s*:\s*)?(?:([EW]\d{4,}[A-Z]?)\s*:?\s*)?"
                        r"([A-Za-z_][A-Za-z0-9_]{3,})\s*(\(\s*\))?\s*:")
    cpp_self = re.compile(r"^\s*([A-Za-z_]\w*(?:::~?[A-Za-z_]\w*)+)\s*\(\s*\)")
    not_names = {"ERROR", "Error", "Warning", "WARNING", "WORNING", "NOTE", "Note", "Usage", "usage", "assert"}
    # CRI's other forms: "E9040822:'adxf' is NULL.(adxf_Stop)", "cvFsGetStat #1:handle error",
    # "2103102:SVM:svm_unlock:lock type miss match". Only for names shaped like library functions (an underscore,
    # or CRI's camel-case prefixes), so ordinary words in messages never become names.
    tail_self = re.compile(r"^\s*[EW]?\d{5,}\s*:.*\(([A-Za-z_][A-Za-z0-9_]{3,})\)\s*\.?\s*$")
    hash_self = re.compile(r"^\s*([A-Za-z_][A-Za-z0-9_]{3,})\s+#\d+\s*:")
    lib_self = re.compile(r"^\s*\d{5,}\s*:\s*[A-Z]{2,8}\s*:\s*([A-Za-z_][A-Za-z0-9_]{3,})\s*:")
    lib_shaped = re.compile(r"^(?:[A-Za-z0-9]+_[A-Za-z0-9_]+|(?:cvFs|ADX|SJ|Sfd|sfd|MW|mw)[A-Za-z0-9]+)$")

    def self_name(text):
        m = cpp_self.match(text)
        if m:
            return m[1].replace("::", "__"), "its own method name"
        m = c_self.match(text)
        if m and m[2] not in not_names and (m[1] or m[3]):
            return m[2], ("error code " + m[1] if m[1] else "name()")
        for pat, why in ((tail_self, "name in its error text"), (hash_self, "name #n:"), (lib_self, "code:LIB:name:")):
            m = pat.match(text)
            if m and lib_shaped.match(m[1]):
                return m[1], why
        return None
    # Owners by elimination. Inlining copies a callee's check, message included, into its callers (adxt_Stop's
    # message is also in adxt_StartSj and adxt_Destroy), so a message can have several users. A function takes name
    # F when F is the only self-name of its messages not yet given to another function, and no other function is in
    # that position for F; repeated until nothing changes. Anything ambiguous stays unnamed.
    selfs = collections.defaultdict(dict)        # function -> {self-name: (text, why)}
    for text, fs in text_users.items():
        got = self_name(text)
        if got:
            for f in fs:
                selfs[f].setdefault(got[0], (text, got[1], len(fs)))
    owner = {}
    while True:
        claim = collections.defaultdict(list)
        for f, ns in selfs.items():
            if f in owner.values():
                continue
            left = [n for n in ns if n not in owner]
            if len(left) == 1:
                claim[left[0]].append(f)
        new = {n: fs[0] for n, fs in claim.items() if len(fs) == 1}
        if not new:
            break
        owner.update(new)
    for n, f in sorted(owner.items(), key=lambda kv: kv[1]):
        text, why, users = selfs[f][n]
        others = sorted(set(selfs[f]) - {n})
        give(fsym[f], symbol(n, 96), "N5 self-name",
             f"{text[:80]!r} ({why}); " + ("its only user" if users == 1 else
                                            f"{users} users, the others inlined it") +
             (f"; its other messages name {', '.join(others[:3])} (inlined callees)" if others else ""))

    # ---- N3 string ------------------------------------------------------------------------------------------
    string_users = collections.defaultdict(set)  # by TEXT: the compiler keeps a separate copy of a literal per unit
    for f, rs in refs.items():
        for _, _, t in rs:
            if used_text(t):
                string_users[used_text(t)].add(f)
    for f, rs in refs.items():
        # a string names a function only when at most 2 functions use that text (a shared message is no identity)
        texts = [(distinctive(used_text(t)), used_text(t)) for _, _, t in rs
                 if used_text(t) and len(string_users[used_text(t)]) <= 2]
        texts = [x for x in texts if x[0] > 0]
        if texts:
            score, text = max(texts)
            give(fsym[f], "ref_" + ident(text), "N3 string", f"references the string {text!r}")

    # ---- N4 helper (up to 4 levels), with N14 exit-time destructors -------------------------------------------
    def name_of(a):
        return result[a]["new"] if a in result else fsym[a][0]

    # N14: __register_global_object links {object r3, destructor r4} into the chain __destroy_global_chain walks at
    # exit, calling destructor(object, -1) (staticstore.RUNTIME_BODIES): the constant r4 at a call is that object's
    # destructor.
    # A static-storage object of a class without a vtable has no N7 name; it is named after the one function that
    # registers it (a unit's __sinit_, or the function owning a function-local static) -> <that>__static_dtor[k],
    # k = its order among that function's registrations. Applied with N4, as the registrants get names.
    register = by_name.get("__register_global_object")
    text_sec = next(x for x in image.sections if x.kind == "text")
    sda = dict(zip((2, 13), strrefs.sda_bases(image, text_sec.address, text_sec.address + text_sec.size)))
    static_dtors, unresolved_registrations = (
        staticstore.exit_destructors(image, register, callers.get(register, set()), fsize, entries, sda)
        if runtime_ok and register in fsize else ({}, []))

    def name_static_dtors():
        n = 0
        for d, regs in sorted(static_dtors.items()):
            owners = {r[0] for r in regs}
            if d not in fsym or d in result or not unnamed(fsym[d]) or len(owners) != 1:
                continue
            g = owners.pop()
            gname = name_of(g)
            if re.fullmatch(r"fn_[0-9A-F]{8}", gname):
                continue
            mine = list(dict.fromkeys(dd for _, dd in sorted((r[1], dd) for dd, rr in static_dtors.items()
                                                                for r in rr if r[0] == g)))
            k = "" if len(mine) == 1 else str(mine.index(d))
            pcs = ", ".join(f"{r[1]:#010x}" + (f" object {r[2]:#010x}" if r[2] is not None else "") for r in sorted(regs))
            if give(fsym[d], gname[:96] + f"__static_dtor{k}", "N14 static dtor",
                    f"the destructor {gname} passes to __register_global_object ({pcs}): __destroy_global_chain "
                    f"calls it on that object at exit"):
                n += 1
        return n

    # N15: a function-local static's getter (Astra answers 5 item 16 and 6 item 1): staticstore.singleton proves
    # the protocol - one registration of a constant object O and destructor D, a constructor returning this run on
    # O on every path, the registration only past a guard byte tested 0, the guard set right after it, every return
    # giving O or a load of a cache C known non-zero there; staticstore.cache_holds_only: C starts as 0 or O and
    # every direct store into it, in any code section, writes O or 0. g -> C__instance when D carries a class (an
    # N7 destructor), else static_<O>__instance. Its destructor then reads <getter>__static_dtor (N14).
    code_funcs = [f for f in fstarts if fsize.get(f, 0) >= 4 and (image.section_at_address(f) is not None and
                                                                     image.section_at_address(f).kind == "text")]
    stores_at = staticstore.store_index(image, code_funcs, fsize) if static_dtors else {}
    bss_objects = sorted((s[2], int(re.search(r"size:0x([0-9A-Fa-f]+)", s[3]).group(1), 16))
                         for s in syms if s and s[1] in (".bss", ".sbss") and re.search(r"size:0x", s[3]))

    def in_bss(a):
        i = bisect.bisect_right(bss_objects, (a, 1 << 32)) - 1
        return i >= 0 and bss_objects[i][0] <= a < bss_objects[i][0] + bss_objects[i][1]
    this_returner = {}

    def returns_this(c):
        if c not in this_returner:
            this_returner[c] = vcalls.returns_this(image, c, fsize[c], entries.get(c, ()))
        return this_returner[c]

    for g in sorted({r[0] for regs in static_dtors.values() for r in regs}):
        if g in result or not unnamed(fsym[g]):
            continue
        receivers = {c for c in callees_in_order[g] if c in fsize and returns_this(c)}
        found = staticstore.singleton(image, g, fsize[g], entries.get(g, ()), register, sda, receivers)
        if found is None:
            continue
        obj, dtor, cache, _ = found
        if cache is not None and not staticstore.cache_holds_only(image, stores_at, fsize, entries, cache, obj, sda,
                                                                  in_bss(cache)):
            continue
        cls = (result.get(dtor) or {}).get("class")
        give(fsym[g], corrmod.sanitize(cls + "::instance") if cls else f"static_{obj:08X}__instance", "N15 singleton",
             f"constructs the static object {obj:#010x} once (guarded), registers its destructor {name_of(dtor)} for "
             f"exit and returns it" + (f" (through {cache:#010x}, written only with it)" if cache is not None else ""),
             **({"class": cls} if cls else {}))

    name_static_dtors()
    for _ in range(4):
        new_count = name_static_dtors()
        for f, cs in sorted(callers.items()):
            if len(cs) != 1 or f in result or not unnamed(fsym[f]):
                continue
            g = next(iter(cs))
            gname = name_of(g)
            if re.fullmatch(r"fn_[0-9A-F]{8}", gname):
                continue
            k = callees_in_order[g].index(f)
            if give(fsym[f], gname[:96] + f"__sub{k}", "N4 helper", f"only direct caller: {gname}"):
                new_count += 1
        if not new_count:
            break

    # ---- D1 data ---------------------------------------------------------------------------------------------
    rtti_records = {info["record"]: cls for cls, info in table.items()}
    for s in syms:
        if not s or "type:object" not in s[3]:
            continue
        a = s[2]
        if image.section_at_address(a) is None:  # .bss / .sbss: no initial value in the file, no value-based name
            continue
        if a in vtable_class:
            give(s, corrmod.sanitize(vtable_class[a] + "::vtable"), "D1 vtable",
                 "data object {RTTI record, 0, methods}: the primary vtable")
        elif a in secondary_at:
            cls, k = secondary_at[a]
            give(s, corrmod.sanitize(cls + "::vtable") + f"_0x{k:X}", "D1 vtable",
                 f"data object {{RTTI record, -{k:#x}, methods}}: the secondary vtable of the base subobject at "
                 f"this+{k:#x}")
        elif a in base_list_class:
            give(s, corrmod.sanitize(base_list_class[a] + "::rtti_bases"), "D1 rtti bases",
                 "the RTTI record's base-class list {record, offset}..., 0")
        elif a in rtti_records:
            give(s, corrmod.sanitize(rtti_records[a] + "::rtti"), "D1 rtti", "RTTI record {name, base list}")
        elif "data:float" in s[3]:
            v = struct.unpack(">f", image.read(a, 4))[0]
            give(s, float_name("kF32", v), "D1 float", f"value {v!r}")
        elif "data:double" in s[3]:
            v = struct.unpack(">d", image.read(a, 8))[0]
            give(s, float_name("kF64", v), "D1 double", f"value {v!r}")
        elif a in string_at and string_at[a] and len(re.sub(r"[^A-Za-z0-9]", "", string_at[a])) >= 3:
            give(s, "str_" + ident(string_at[a], 40), "D1 string", f"text {string_at[a]!r}")

    # ---- apply to a new project -------------------------------------------------------------------------------
    if os.path.exists(dst):
        raise FileExistsError(dst)
    os.makedirs(os.path.join(dst, "config"))
    for f in ("main.dol", "config.yml"):
        shutil.copyfile(os.path.join(src, f), os.path.join(dst, f))
    shutil.copyfile(os.path.join(src, "config", "splits.txt"), os.path.join(dst, "config", "splits.txt"))
    out = []
    for line, s in zip(lines, syms):
        if s and s[2] in result and result[s[2]]["old"] == s[0]:
            line = result[s[2]]["new"] + line[len(s[0]):]
        out.append(line)
    with open(os.path.join(dst, "config", "symbols.txt"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(out))
    counts = collections.Counter(v["rule"] for v in result.values())
    with open(os.path.join(dst, "names.json"), "w") as fh:
        classes_out = {}
        for cls, info in sorted(table.items()):
            ci = class_info.get(cls) if cls in class_info else None
            sizes = ci["sizes"] if ci else {}
            classes_out[cls] = {
                "record": f"0x{info['record']:08X}",
                "vtable": f"0x{info['vtable']:08X}" if info["vtable"] else None,
                "direct_bases": info["direct_bases"],
                "methods": [None if m is None else f"0x{m:08X}" for m in info["methods"]],
                "secondary": [{"vtable": f"0x{s['vtable']:08X}", "offset": s["offset"], "subobject": s["subobject"],
                               "methods": [None if m is None else f"0x{m:08X}" for m in s["methods"]],
                               "thunks": sorted(s["thunks"])} for s in info["secondary"]],
                "size": next(iter(sizes)) if len(sizes) == 1 else None,
                "size_sites": dict(sizes) if sizes else {},
                "size_from": ci["size_from"] if ci else [],
                "ctors": [f"0x{a:08X}" for a in (ci["ctors"] if ci else [])],
                "dtors": [f"0x{a:08X}" for a in (ci["dtors"] if ci else [])],
            }
        signatures = {}
        proto_path = os.path.join(os.path.dirname(os.path.abspath(src)), "rw", "prototypes.json")
        if os.path.exists(proto_path):
            with open(proto_path, encoding="utf-8") as f:
                protos = json.load(f)
            final_name = {a: (result[a]["new"] if a in result else fsym[a][0]) for a in fsym}
            for a, n in final_name.items():
                if n in protos["functions"]:
                    pr = protos["functions"][n]
                    signatures[f"{a:08X}"] = {"source": "RW 3.7 SDK " + pr["header"], "returns": pr["returns"],
                                              "params": pr["params"]}
            if os.path.exists(cb_path):
                for fname, row in cbs.items():
                    # by address, as N9 itself (a callback file from a project with other names still applies)
                    s = by_addr_cb.get(int(row["address"], 16)) if "address" in row else by_name.get(fname)
                    if s is not None:
                        sig = row["signature"]
                        signatures[f"{s[2]:08X}"] = {"source": f"RW 3.7 SDK callback type {row['type']}",
                                                     "returns": sig["returns"], "params": sig["params"]}
        json.dump({"counts": dict(counts), "names": {f"{a:08X}": v for a, v in sorted(result.items())},
                   "classes": classes_out, "signatures": signatures,
                   "class_uses": {f"{a:08X}": v for a, v in sorted(class_uses.items())}}, fh, indent=1)
    r = subprocess.run([rebuild.TOOLS["dtk"], "--no-color", "dol", "split", "-j", str(jobs), "config.yml", "out"],
                       cwd=dst, capture_output=True, text=True)
    open(os.path.join(dst, "split.log"), "w").write(r.stdout + r.stderr)
    if r.returncode != 0:
        raise RuntimeError("dtk split failed: " + r.stderr[-1500:])
    rep = rebuild.run(dst, jobs)
    fn_total = len(funcs)
    fn_named = sum(1 for a, s in funcs if not re.fullmatch(r"fn_[0-9A-F]{8}", result[a]["new"] if a in result else s[0]))
    print(json.dumps({"new_names_by_rule": dict(counts), "functions": fn_total, "functions_named_now": fn_named,
                      "byte_exact": rep.get("byte_exact")}, indent=1))
    if not rep.get("byte_exact"):
        raise RuntimeError("names broke the rebuild")


if __name__ == "__main__":
    main(sys.argv[1:])
