"""Name virtual functions by the role their dispatcher gives a base-class slot (Task, Gadget, boost's sp_counted_base,
TEnemyAIState, Player::Behavior::Behavior).

    python -B -m dolrecon.classroles <named project> <out dir>        (run from source/tools)

A slot gets a role only from code that calls it, read from the binary before anything is named (a different binary
fails here and names nothing). Slots are vtable indices of corrections.rtti_classes (pure virtuals are None).

  vptr   Task's constructor 0x8004F014 stores Task's vtable 0x8051E768 at this+0x18 (lis/addi 0x8004F01C/30, stw at
         0x8004F038): a Task's vptr is at +0x18, so are those of the classes that reach Task through offset-0 bases.
  Task slot 1   update    the task-tree walk 0x8004ECAC (first child +0x14, next sibling +0xC, recursive): a child
                          that is not suspended gets c->vtable[1](c, dt) (0x8004ED98 lwz r12,0x18(r31); lwz r12,0xC;
                          bctrl, f1 = the walk's dt) and the timed variant 0x8004EDF0 calls the same slot. A child
                          with the kill flag (+4 bit 0) is destroyed through slot 0 with r4 = 1 (0x8004ED48..58).
  Gadget slot 5 checkRemove  Gadget's update 0x8015AD28 calls vtable[5] first (0x8015AD44); a non-zero result skips
                          slot 2. Gadget's own body 0x8015AC7C sets the kill flag (ori 1 / sth 4(r31), 0x8015ACC4)
                          and returns 1 when the layout record at +0x28 says so.
  Gadget slot 2 exec      called by Gadget's update with dt when checkRemove returned 0 (0x8015AD64 .. bctrl).
  (Gadget slot 1 is Task slot 1: Gadget's update.)

  boost::detail::sp_counted_base (vptr +0; its ctor, inlined in every sp_counted_base_impl ctor such as 0x8000D500,
  stores the vtable, then use count +4 = 1 and weak count +8 = 1). Its release 0x8000945C (boost 1.31/1.32
  sp_counted_base::release without threads) decrements +4 and on zero calls
    slot 1 dispose   (0x80009484), then decrements +8 and on zero calls
    slot 2 destruct  (0x800094A8); the base body 0x800094C8 is `if (this) this->dtor(1)`, i.e. delete this.
  These are boost's own identifiers (sp_counted_base_impl exists up to boost 1.32, whose slot 2 is destruct).

  TEnemyAIState (vptr +0). EnemyBaseAI keeps the current state at +0x2C, the previous one at +0x30 and the states'
  owner at +0x28. A state change (EnemyBaseAI slot 11, 0x8019D8C0; slots 13-20 and 22 repeat it) takes the new
  state from a getter (fn_8019D970: a function-static object built by a TEnemyAIState subclass constructor) and calls
    slot 1 setOwner  with the owner +0x28 before each call below (state bodies store it at +4),
    slot 4 exit      on the old state (0x8019D910), then prev = cur, cur = new,
    slot 2 enter     on the new state (0x8019D950).
  EnemyBaseAI slot 1 (0x8019EACC) stores its f1 at +0x20 and calls
    slot 3 execute   on the current state (0x8019EB1C).

  Player::Behavior::Behavior (vptr +0). Player::Executer keeps the current behavior in a shared_ptr at +0x30 and a
  factory at +0x38 that builds one by id (ShadowBehavior slot 1 0x800AE8D0 indexes the record table 0x80522DE8,
  whose 35 factories each construct a Behavior subclass, Idle ... Super). The change 0x800A82FC calls
    slot 1 canEnter  on the new behavior with (previous id, args); zero refuses the change (0x800A8380),
    slot 3 exit      on the current one with the new behavior's id +0x10 (0x800A83A8),
    slot 2 enter     on the installed new one with (previous id, args) (0x800A83DC).
  The executer step 0x800A8160 (called by ShadowExecuter / SonicTypeExecuter update) loads the time step from the
  player (+0x28 -> +0x60) and calls
    slot 5 update    with it in f1 (0x800A81EC).
  The executer's command handler 0x800721A8 (it passes its `this` to the change 0x800A82FC and dynamic_casts its
  argument from CharCommand to DamageCommand) calls
    slot 4 onCommand on the current behavior with that CharCommand (0x8007229C).

A function f gets Owner__role only when every vtable slot holding f is one role slot (no other class or slot uses
the body), and Owner is the class among its holders that all the others descend from (the class whose body it is).
Slots without a dispatcher (Gadget 3 and 4, Task-derived slots >= 2 outside Gadget, sp_counted_base 3 = boost's
get_deleter, TEnemyAIState 5, Behavior 6) keep their vfN names. These
are reconstructed ROLE names, not recovered source identifiers; four-byte bodies (blr only) are counted apart.
Writes <out>/classroles.json: {contracts, functions: {address: {name, role, base, slot, owner, size, evidence}}};
names.py rule N12.
"""
from __future__ import annotations

import argparse
import collections
import json
import os
import re
import struct
import sys

from . import corrections as corrmod
from . import dol as dolmod
from . import match as mmatch
from . import spine
from . import staticstore
from . import strrefs
from . import vcalls

# instruction address -> expected word: the vptr store and each dispatch, as read from PAL main.dol
CHECKS = {
    0x8004F01C: 0x3CA08052, 0x8004F030: 0x3865E768, 0x8004F038: 0x907E0018,             # Task ctor: vptr at +0x18
    0x8004ED44: 0x7FC3F378, 0x8004ED48: 0x38800001, 0x8004ED4C: 0x819E0018,             # walk: slot 0 (this, 1)
    0x8004ED50: 0x818C0008, 0x8004ED58: 0x4E800421,
    0x8004ED90: 0x7FE3FB78, 0x8004ED94: 0xFC20F890, 0x8004ED98: 0x819F0018,             # walk: slot 1 (this, dt)
    0x8004ED9C: 0x818C000C, 0x8004EDA4: 0x4E800421,
    0x8004EE44: 0x819F0018, 0x8004EE48: 0x818C000C, 0x8004EE50: 0x4E800421,             # timed walk: slot 1
    0x8015AD44: 0x81830018, 0x8015AD48: 0x818C001C, 0x8015AD54: 0x5460063F,             # Gadget update: slot 5
    0x8015AD5C: 0x7FE3FB78, 0x8015AD64: 0x819F0018, 0x8015AD68: 0x818C0010,             # then slot 2 (this, dt)
    0x8015AD70: 0x4E800421,
    0x8015ACC0: 0x38600001, 0x8015ACC4: 0x60000001, 0x8015ACC8: 0xB01F0004,             # checkRemove sets kill
    # sp_counted_base: the impl ctor sets both counts to 1; release: --use (+4) -> slot 1, --weak (+8) -> slot 2
    0x8000D50C: 0x38E00001, 0x8000D51C: 0x90E30004, 0x8000D520: 0x90E30008,
    0x8000946C: 0x7C7F1B78, 0x80009470: 0x80830004, 0x80009474: 0x3404FFFF, 0x80009478: 0x90030004,
    0x8000947C: 0x40820038, 0x80009480: 0x81830000, 0x80009484: 0x818C000C, 0x8000948C: 0x4E800421,
    0x80009490: 0x807F0008, 0x80009494: 0x3403FFFF, 0x80009498: 0x901F0008, 0x8000949C: 0x40820018,
    0x800094A4: 0x819F0000, 0x800094A8: 0x818C0010, 0x800094B0: 0x4E800421,
    0x800094D0: 0x28030000, 0x800094DC: 0x81830000, 0x800094E0: 0x38800001, 0x800094E4: 0x818C0008,  # delete this
    0x800094EC: 0x4E800421,
    # EnemyBaseAI state change: new = getter(); cur->setOwner; cur->exit; prev = cur; cur = new; setOwner; enter
    0x8019D8D4: 0x4800009D, 0x8019D8DC: 0x7C7F1B78, 0x8019D8E8: 0x807E002C, 0x8019D8F4: 0x81830000,
    0x8019D8F8: 0x809E0028, 0x8019D8FC: 0x818C000C, 0x8019D904: 0x4E800421, 0x8019D908: 0x807E002C,
    0x8019D90C: 0x81830000, 0x8019D910: 0x818C0018, 0x8019D918: 0x4E800421, 0x8019D924: 0x801E002C,
    0x8019D928: 0x901E0030, 0x8019D92C: 0x93FE002C, 0x8019D930: 0x807E002C, 0x8019D934: 0x809E0028,
    0x8019D938: 0x81830000, 0x8019D93C: 0x818C000C, 0x8019D944: 0x4E800421, 0x8019D948: 0x807E002C,
    0x8019D94C: 0x81830000, 0x8019D950: 0x818C0010, 0x8019D958: 0x4E800421,
    0x8019D988: 0x386D3F00, 0x8019D9B0: 0x386D3F00,                         # the getter returns the object it built
    # EnemyBaseAI slot 1: store f1; cur->setOwner; cur->execute
    0x8019EAE0: 0xD0230020, 0x8019EAF4: 0x807F002C, 0x8019EB00: 0x81830000, 0x8019EB04: 0x809F0028,
    0x8019EB08: 0x818C000C, 0x8019EB10: 0x4E800421, 0x8019EB14: 0x807F002C, 0x8019EB18: 0x81830000,
    0x8019EB1C: 0x818C0014, 0x8019EB24: 0x4E800421,
    # Executer behavior change: new = (+0x38)->slot1(id); new->canEnter; cur->exit(new id); cur = new; new->enter
    0x800A8314: 0x80630030, 0x800A8320: 0x83E30010, 0x800A832C: 0x809D0038, 0x800A8334: 0x80BE0004,
    0x800A8338: 0x81840000, 0x800A833C: 0x818C000C, 0x800A8344: 0x4E800421, 0x800A8368: 0x80610010,
    0x800A8374: 0x81830000, 0x800A8378: 0x7FE4FB78, 0x800A837C: 0x7FC5F378, 0x800A8380: 0x818C000C,
    0x800A8388: 0x4E800421, 0x800A838C: 0x5460063F, 0x800A8390: 0x4182006C, 0x800A8394: 0x807D0030,
    0x800A83A0: 0x81830000, 0x800A83A4: 0x80810010, 0x800A83A8: 0x818C0014, 0x800A83AC: 0x80840010,
    0x800A83B4: 0x4E800421, 0x800A83B8: 0x387D0030, 0x800A83C0: 0x48000061, 0x800A83C4: 0x807D0030,
    0x800A83D0: 0x81830000, 0x800A83D4: 0x7FE4FB78, 0x800A83D8: 0x7FC5F378, 0x800A83DC: 0x818C0010,
    0x800A83E4: 0x4E800421,
    # ShadowBehavior slot 1: record = table[id]; record->factory(player)
    0x800AE928: 0x3C608052, 0x800AE92C: 0x57E0103A, 0x800AE930: 0x38632DE8, 0x800AE934: 0x7C63002E,
    0x800AE940: 0x81830000, 0x800AE94C: 0x387E0004, 0x800AE954: 0x4E800421,
    # Executer step: f1 = step at player +0x60; cur->update(f1); bl from SonicTypeExecuter / ShadowExecuter update
    0x800A817C: 0x80830028, 0x800A8180: 0x80840060, 0x800A8184: 0xC3E40000, 0x800A81D8: 0x389F0030,
    0x800A81DC: 0x4BFCA3C1, 0x800A81E0: 0x80610008, 0x800A81E4: 0xFC20F890, 0x800A81E8: 0x81830000,
    0x800A81EC: 0x818C001C, 0x800A81F4: 0x4E800421, 0x80072658: 0x48035B09, 0x800AFD5C: 0x4BFF8405,
    # Executer command handler: cur->onCommand(command); passes its this to the change (bl 0x800A82FC)
    0x80072284: 0x38610030, 0x80072288: 0x389E0030, 0x8007228C: 0x48000311, 0x80072290: 0x80610030,
    0x80072294: 0x7FE4FB78, 0x80072298: 0x81830000, 0x8007229C: 0x818C0018, 0x800722A4: 0x4E800421,
    0x80072380: 0x7FC3F378, 0x80072388: 0x48035F75,
}
TASK_VTABLE = 0x8051E768
SPC, BEH = "boost::detail::sp_counted_base", "Player::Behavior::Behavior"
CONTRACTS = [  # (base, slot, role, evidence)
    ("Task", 1, "update", "Task slot 1: called by the task-tree walk 0x8004ECAC (0x8004ED98) and its timed variant "
                          "0x8004EDF0 with (this, dt) for every live task"),
    ("Gadget", 5, "checkRemove", "Gadget slot 5: called first by Gadget's update 0x8015AD28 (0x8015AD44); non-zero "
                                 "skips slot 2; Gadget's body 0x8015AC7C sets the kill flag the walk destroys on"),
    ("Gadget", 2, "exec", "Gadget slot 2: called by Gadget's update 0x8015AD28 (0x8015AD64) with dt when "
                          "checkRemove returned 0"),
    (SPC, 1, "dispose", "sp_counted_base slot 1: called by release 0x8000945C (0x80009484) when the use count +4 "
                        "reaches 0 (boost sp_counted_base::dispose)"),
    (SPC, 2, "destruct", "sp_counted_base slot 2: called by release 0x8000945C (0x800094A8) when the weak count +8 "
                         "reaches 0; the base body 0x800094C8 deletes this (boost sp_counted_base::destruct)"),
    ("TEnemyAIState", 1, "setOwner", "TEnemyAIState slot 1: EnemyBaseAI passes its +0x28 before every state call "
                                     "(0x8019D8FC, 0x8019EB08)"),
    ("TEnemyAIState", 2, "enter", "TEnemyAIState slot 2: called on the new state once EnemyBaseAI's change makes it "
                                  "current (0x8019D950)"),
    ("TEnemyAIState", 3, "execute", "TEnemyAIState slot 3: called on the current state by EnemyBaseAI slot 1 "
                                    "0x8019EACC after it stores its f1 (0x8019EB1C)"),
    ("TEnemyAIState", 4, "exit", "TEnemyAIState slot 4: called on the current state before EnemyBaseAI's change "
                                 "replaces it (0x8019D910)"),
    (BEH, 1, "canEnter", "Behavior slot 1: called by Executer's change 0x800A82FC on the new behavior with "
                         "(previous id, args); zero refuses the change (0x800A8380)"),
    (BEH, 2, "enter", "Behavior slot 2: called by Executer's change 0x800A82FC on the installed behavior with "
                      "(previous id, args) (0x800A83DC)"),
    (BEH, 3, "exit", "Behavior slot 3: called by Executer's change 0x800A82FC on the replaced behavior with the new "
                     "behavior's id (0x800A83A8)"),
    (BEH, 4, "onCommand", "Behavior slot 4: Executer's command handler 0x800721A8 passes the CharCommand it got to "
                          "the current behavior (0x8007229C)"),
    (BEH, 5, "update", "Behavior slot 5: Executer's step 0x800A8160 calls the current behavior with the player's "
                       "time step in f1 (0x800A81EC)"),
]
SLOT1 = {"Task": 0x8004EA80, "Gadget": 0x8015AD28}     # the slot-1 bodies the checks above read
VPTR = {"Task": 0x18, SPC: 0, "TEnemyAIState": 0, BEH: 0}       # where the dispatches above load the vtable from
SPC_IMPL_CTOR = 0x8000D500       # sp_counted_base_impl<CameraManagerUnit, ...> ctor: stores sp_counted_base's vtable
STATE_GETTER = 0x8019D970        # a function-local static's getter: its object must be a TEnemyAIState
BEHAVIOR_FACTORY = ("Player::Shadow::ShadowBehavior", 1, 0x800AE8D0)
BEHAVIOR_RECORDS = (0x80522DE8, 37)      # the record table that factory indexes (word 0 = factory; 0 = no behavior)


def assign(table: dict, contracts=CONTRACTS):
    """({function: row}, {(class, slot): (role, base, evidence)}, skipped) for an rtti_classes table."""
    def at_zero(cls, base, seen=()):
        """cls reaches base through offset-0 bases only: base's slots are cls's primary vtable slots."""
        return cls == base or any(off == 0 and b in table and b not in seen and at_zero(b, base, seen + (cls,))
                                  for b, off in table[cls]["direct_bases"])
    role_of = {}                                       # (class, slot) -> the contract role of that slot
    for base, slot, role, ev in contracts:
        for cls in table:
            if at_zero(cls, base):
                role_of[(cls, slot)] = (role, base, ev)
    # every vtable, secondary ones included (corrections.slot_holders): a body also in another class's secondary
    # vtable at another role, or in no role there, is not this role's body alone. A thunk is no body of a role.
    ancestors = {c: set(info["bases"]) | {c} for c, info in table.items()}
    out, skipped = {}, collections.Counter()
    for f, hs in sorted(corrmod.slot_holders(table).items()):
        if any(h[0] == "thunk" for h in hs):
            continue
        roles = {role_of.get((view, slot)) for _, _, view, slot in hs}
        if roles == {None}:
            continue
        if len(roles) != 1 or None in roles:
            skipped["body also in another slot or role"] += 1
            continue
        role, base, ev = roles.pop()
        classes = corrmod.body_classes(hs)
        owner = [c for c in classes if all(c in ancestors[o] for o in classes)] if classes else []
        if len(owner) != 1:
            skipped["no single owning class"] += 1
            continue
        slot = hs[0][3]
        out[f] = {"name": corrmod.sanitize(f"{owner[0]}::{role}"), "role": role, "base": base, "slot": slot,
                  "owner": owner[0], "holders": len(classes),
                  "evidence": f"{ev}; slot {slot} of {owner[0]}" + (
                      f" (inherited by {len(classes) - 1} classes)" if len(classes) > 1 else "") + (
                      "; through a secondary vtable" if any(h[0] != "primary" for h in hs) else "")}
    return out, role_of, skipped


def matched_address(name):
    """Address of a function proven by byte-identical source (decomp/matched.json, rule M0's proof), else None."""
    path = os.path.join(os.path.dirname(__file__), "..", "..", "decomp", "matched.json")
    if not os.path.exists(path):
        return None
    with open(path, encoding="utf-8") as f:
        for entry in json.load(f).get("files", []):
            for row in entry.get("functions", []):
                if row.get("matched") and row.get("original") == name:
                    return int(row["address"], 16)
    return None


def type_errors(image, funcs, table) -> list[str]:
    """What the dispatched objects are, proven from constructors (vcalls): each base's vptr offset is the one its
    dispatch loads; the sp_counted_base fields release counts are the ones its constructor initialises; the state
    getter is a function-local static's getter (staticstore.singleton: guard, construction, registration,
    publication, return) whose object a TEnemyAIState constructor built, and every behavior factory returns the
    object a Behavior constructor built on every path where operator new did not return null
    (vcalls.returned_construction: the receiver of a constructor that returns this and never deletes - not merely
    "calls some constructor", Astra answers 5 and 6)."""
    entries = vcalls.interior_entries(image, funcs)
    vptr, ctors, _, _ = vcalls.survey(image, funcs, table, entries)
    ctor_of = collections.defaultdict(set)
    for c, fs in ctors.items():
        for f in fs:
            ctor_of[f].add(c)
    size = {a: s for a, _, s, _ in funcs}
    op_new, op_delete = matched_address("__nw__FUl"), matched_address("__dl__FPv")

    def derives(c, base):
        return c == base or base in table[c]["bases"]

    def returns_a(f, base):
        return any(derives(c, base) for c in vcalls.returned_construction(image, f, size, ctor_of, op_new, op_delete,
                                                                          entries))

    def static_of(f, base):
        """The getter's object was built by a constructor of base (staticstore.singleton's protocol)."""
        register = next((a for a, n, _, _ in funcs if n == "__register_global_object"), None)
        text = next(x for x in image.sections if x.kind == "text")
        sda = dict(zip((2, 13), strrefs.sda_bases(image, text.address, text.address + text.size)))
        receivers = {c for c in ctor_of if vcalls.returns_this(image, c, size.get(c, 0), entries.get(c, ()))}
        found = staticstore.singleton(image, f, size.get(f, 0), entries.get(f, ()), register, sda, receivers)
        return found is not None and any(derives(c, base) for t in found[3] for c in ctor_of.get(t, ()))

    errors = [f"{c}: vptr at {vptr.get(c)}, the dispatch loads +{k:#x}" for c, k in VPTR.items() if vptr.get(c) != k]
    if op_new is None or op_delete is None:
        errors.append("operator new / delete are not proven by decomp/matched.json")
    if SPC not in ctor_of.get(SPC_IMPL_CTOR, ()):
        errors.append(f"0x{SPC_IMPL_CTOR:08X} does not store {SPC}'s vtable")
    if not static_of(STATE_GETTER, "TEnemyAIState"):
        errors.append(f"0x{STATE_GETTER:08X} is not the getter of a static TEnemyAIState")
    cls, slot, body = BEHAVIOR_FACTORY
    if table.get(cls, {}).get("methods", [])[slot:slot + 1] != [body]:
        errors.append(f"{cls} slot {slot} is not 0x{body:08X}")
    start, count = BEHAVIOR_RECORDS
    records = [image.word(start + 4 * i) for i in range(count)]
    for i, rec in enumerate(records):
        if rec and not returns_a(image.word(rec), BEH):
            errors.append(f"behavior record {i} (0x{rec:08X}): its factory does not return a Behavior it constructed")
    if sum(1 for r in records if r) != 35:
        errors.append("the behavior record table changed")
    return errors


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("project")
    ap.add_argument("out")
    args = ap.parse_args(argv)
    proj = mmatch.Project(args.project)
    for a, want in CHECKS.items():
        got = struct.unpack(">I", proj.read(a, 4))[0]
        if got != want:
            print(f"dispatch evidence not found: 0x{a:08X} holds {got:08X}, expected {want:08X}")
            return 1
    image = dolmod.load_file(os.path.join(args.project, "main.dol"))
    funcs, objects = spine.load_symbols(os.path.join(args.project, "config", "symbols.txt"))
    size = {a: s for a, _, s, _ in funcs}
    name_of = {a: n for a, n, _, _ in funcs}
    sdata = image.section_at_address(next(v[0] for v in objects.values() if v[2] == ".sdata"))
    table = corrmod.rtti_classes(image, (sdata.address, sdata.address_end), {v[0] for v in objects.values()}, size)
    if table["Task"]["vtable"] != TASK_VTABLE or table["Task"]["methods"][1] != SLOT1["Task"]:
        print("Task's vtable is not the one its constructor stores")
        return 1
    if table["Gadget"]["methods"][1] != SLOT1["Gadget"] or ("Task", 0) not in table["Gadget"]["direct_bases"]:
        print("Gadget is not a Task at offset 0 whose slot 1 is the dispatcher read above")
        return 1
    errors = type_errors(image, funcs, table)
    if errors:
        print("dispatched object types not proven:\n  " + "\n  ".join(errors))
        return 1
    found, role_of, skipped = assign(table)
    out = {f"0x{f:08X}": {**row, "size": size.get(f), "was": name_of.get(f)} for f, row in found.items()}
    os.makedirs(args.out, exist_ok=True)
    with open(os.path.join(args.out, "classroles.json"), "w", encoding="utf-8", newline="\n") as fo:
        json.dump({"contracts": [{"base": b, "slot": s, "role": r, "evidence": e} for b, s, r, e in CONTRACTS],
                   "functions": out}, fo, indent=1)
    rows = list(out.values())
    print(json.dumps({
        "named": len(rows), "by_role": dict(collections.Counter(r["role"] for r in rows)),
        "of_them_4_byte_bodies": sum(1 for r in rows if r["size"] == 4),
        "were_fn_": sum(1 for r in rows if re.fullmatch(r"fn_[0-9A-F]{8}", r["was"] or "")),
        "were_vfN": sum(1 for r in rows if re.search(r"__vf\d+$", r["was"] or "")),
        "classes_with_a_role_slot": len({c for c, _ in role_of}), "skipped": dict(skipped)}, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
