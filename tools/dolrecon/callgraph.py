"""The program's call graph with every kind of edge the binary shows, not only direct calls.

    python -B -m dolrecon.callgraph <dtk project dir> [--vcalls <vcalls.json>]   (writes <project>/callgraph.json)

Read from the project's dtk assembly (out/asm/*.s), whose operands and data words are symbolised by dtk's relocation
analysis (the byte-exact rebuild places every symbol where the original has it):
  call     `bl F`
  tail     `b F` to the start of another function (a tail call; branches inside the function are labels)
  addr     an instruction takes F's address (`F@ha` / `F@l` / `F@h` / `F@sda21`): F is handed on - a callback, a table
           entry built at run time, a thread or task entry
  data     the function references a data object (`O@ha` ...) and O holds F's address (a word `.4byte F`): F is reached
           through a table, record or descriptor; via = O
  vtable   as data, O being a class's virtual table: the function constructs or destroys objects of the class
  virtual  a virtual call resolved by dolrecon.vcalls (receiver `this` of a known class, slot s): F = slot s of that
           class's table and of every class deriving from it at offset 0 (from the RTTI bases), each a possible target
Also: `data_holders` (data object -> functions it holds, for objects no code references: reached only from data, e.g.
static initialiser lists or tables reached through other tables) and `taken_into` (functions whose address is taken
by a function that also calls a library layer - callback candidates handed to the SDK / CRI / RenderWare).
A data word that equals a function's address only by coincidence would make a false `data` edge; such words are
counted where they can be told apart (words inside float-only objects are not looked at: only .4byte words).
"""
from __future__ import annotations

import collections
import json
import os
import re
import sys

LINE = re.compile(r"^/\* ([0-9A-F]{8}) [0-9A-F]{8}  (?:[0-9A-F]{2} ){4}\*/\t(\S+)\s*(.*)$")
OPERAND_SYM = re.compile(r"([^\s,()]+)@(ha|l|h|sda21)\b")
OBJ = re.compile(r"^\.obj (\S+?),")
WORD = re.compile(r"^\s*\.4byte ([A-Za-z_.$@][^\s+]*)\s*$")
SYM = re.compile(r"^(\S+) = ([^:]+):0x([0-9A-Fa-f]+);(.*)$")
SIZE = re.compile(r"size:0x([0-9A-Fa-f]+)")
# dtk's code files: one per unit and section, a duplicate unit numbered (`..._text_1.s`), and the C++ exception
# table's initialiser unit `__init_cpp_exceptions.s` (its .text holds __init_cpp_exceptions / __fini_cpp_exceptions).
TEXT_FILE = re.compile(r"(?:_text(?:_\d+)?|_init)\.s$|^__init_cpp_exceptions\.s$")


DATA_DEPTH = 3   # data objects followed through other data objects, at most this many levels


def load_symbols(project_dir: str):
    funcs, objects = {}, {}
    with open(os.path.join(project_dir, "config", "symbols.txt"), encoding="utf-8") as f:
        for line in f:
            m = SYM.match(line.rstrip("\r\n"))
            if not m:
                continue
            size = SIZE.search(m.group(4))
            entry = (int(m.group(3), 16), int(size.group(1), 16) if size else 0, m.group(2))
            (funcs if "type:function" in m.group(4) else objects)[m.group(1)] = entry
    return funcs, objects


def load_aliases(project_dir: str, funcs, owner) -> tuple[dict[str, str], dict[str, str]]:
    """Names dtk writes in operands for a function that are not its symbol name, as (names, branch_labels):
    - names: a scope:local function written with its address appended (`bl stateBusy_8037C5A8` for `stateBusy`);
    - branch_labels: a label (`type:label`) used as a branch target inside a function (`bl _savegpr_14` at the start
      of __save_gpr, `bl _savegpr_19` inside it): a call to it executes the containing function. Only for branches:
      other label uses are address markers (__OSDBINTSTART / END bracket code the OS copies), not function pointers."""
    names = {f"{n}_{a:08X}": n for n, (a, _, _) in funcs.items()}
    labels = {}
    with open(os.path.join(project_dir, "config", "symbols.txt"), encoding="utf-8") as f:
        for line in f:
            m = SYM.match(line.rstrip("\r\n"))
            if m and "type:label" in m.group(4):
                o = owner(int(m.group(3), 16))
                if o is not None:
                    labels[m.group(1)] = o
                    labels[f"{m.group(1)}_{int(m.group(3), 16):08X}"] = o
    return names, labels


def scan(project_dir: str):
    """(code facts per function, data objects -> function words) from the project's assembly."""
    funcs, objects = load_symbols(project_dir)
    by_addr = sorted((a, s, n) for n, (a, s, _) in funcs.items())
    starts = [a for a, _, _ in by_addr]
    import bisect

    def owner(pc):
        i = bisect.bisect_right(starts, pc) - 1
        if i >= 0 and pc < by_addr[i][0] + by_addr[i][1]:
            return by_addr[i][2]
        return None

    alias, branch_labels = load_aliases(project_dir, funcs, owner)

    def resolve(name):  # a function's address (operands, data words)
        return name if name in funcs else alias.get(name)

    def resolve_branch(name):  # a branch target: also a label inside a function
        return resolve(name) or branch_labels.get(name)

    code = collections.defaultdict(lambda: {"call": set(), "tail": set(), "addr": set(), "refs": set()})
    holders = collections.defaultdict(list)
    links = collections.defaultdict(set)   # data object -> data objects it points at
    asm = os.path.join(project_dir, "out", "asm")
    for name in sorted(os.listdir(asm)):
        path = os.path.join(asm, name)
        text = bool(TEXT_FILE.search(name))
        obj = None
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                if text:
                    m = LINE.match(line)
                    if not m:
                        continue
                    pc, op, args = int(m.group(1), 16), m.group(2), m.group(3)
                    me = owner(pc)
                    if me is None:
                        continue
                    target = resolve_branch(args)
                    if op == "bl" and target:
                        code[me]["call"].add(target)
                    elif op == "b" and target and target != me:
                        code[me]["tail"].add(target)
                    for s, _ in OPERAND_SYM.findall(args):
                        s = s.strip()
                        if resolve(s):
                            code[me]["addr"].add(resolve(s))
                        elif s in objects:
                            code[me]["refs"].add(s)
                else:
                    m = OBJ.match(line)
                    if m:
                        obj = m.group(1)
                        continue
                    if line.startswith(".endobj"):
                        obj = None
                        continue
                    w = WORD.match(line)
                    if w and obj and resolve(w.group(1)):
                        holders[obj].append(resolve(w.group(1)))
                    elif w and obj and w.group(1) in objects and w.group(1) != obj:
                        links[obj].add(w.group(1))
    return funcs, objects, code, holders, links


def class_tables(skeleton_dir: str):
    """{class: {slot: function name}} and {class: [(base, offset)]} from the skeleton's class headers."""
    slots, bases = {}, {}
    slot_re = re.compile(r"// slot (\d+): 0x[0-9A-F]+ (\S+)")
    base_re = re.compile(r"base subobjects \(RTTI, flattened\): (.*)")
    for root, _, files in os.walk(os.path.join(skeleton_dir, "classes")):
        for fn in files:
            if not fn.endswith(".h"):
                continue
            with open(os.path.join(root, fn), encoding="utf-8", errors="replace") as f:
                t = f.read()
            name = t.splitlines()[0][3:].strip()
            slots[name] = {int(s): n for s, n in slot_re.findall(t)}
            m = base_re.search(t)
            bl = []
            if m:
                for part in m.group(1).split(", "):
                    b, _, off = part.rpartition(" +")
                    try:
                        bl.append((b.strip(), int(off, 16)))
                    except ValueError:
                        pass
            bases[name] = bl
    return slots, bases


def build(project_dir: str, vcalls_path: str | None, skeleton_dir: str | None):
    funcs, objects, code, holders, links = scan(project_dir)

    def reach(o, depth=DATA_DEPTH):
        """Functions held by o or by the objects it points at (to DATA_DEPTH levels), with the object holding each."""
        out, seen, frontier = {}, {o}, [o]
        for _ in range(depth + 1):
            nxt = []
            for x in frontier:
                for t in holders.get(x, ()):
                    out.setdefault(t, x)
                for y in links.get(x, ()):
                    if y not in seen:
                        seen.add(y)
                        nxt.append(y)
            frontier = nxt
        return out
    vtable_objs = {o for o in holders if o.endswith("__vtable")}
    edges = collections.Counter()
    for f, k in code.items():
        for kind in ("call", "tail", "addr"):
            for t in k[kind]:
                edges[(f, t, kind, "")] += 1
        for o in k["refs"]:
            for t, held_by in reach(o).items():
                edges[(f, t, "vtable" if held_by in vtable_objs else "data", o if held_by == o else o + " -> " + held_by)] += 1
    unresolved_virtual = 0
    if vcalls_path and skeleton_dir and os.path.exists(vcalls_path):
        slots, bases = class_tables(skeleton_dir)
        derived = collections.defaultdict(set)
        for c, bl in bases.items():
            for b, off in bl:
                if off == 0:
                    derived[b].add(c)
        with open(vcalls_path, encoding="utf-8") as f:
            vc = json.load(f)
        addr_name = {a: n for n, (a, _, _) in funcs.items()}
        for call in vc.get("calls", []):
            caller = addr_name.get(int(call["caller"], 16))
            cls, slot = call["this"], call["slot"]
            targets = {slots.get(c, {}).get(slot) for c in {cls} | derived.get(cls, set())}
            targets.discard(None)
            targets = {t for t in targets if t in funcs}
            if not targets or caller is None:
                unresolved_virtual += 1
            for t in targets:
                edges[(caller, t, "virtual", f"{cls} slot {slot}")] += 1
    referenced = set()
    for k in code.values():
        for o in k["refs"]:
            referenced.add(o)
            stack = [o]
            while stack:
                x = stack.pop()
                for y in links.get(x, ()):
                    if y not in referenced:
                        referenced.add(y)
                        stack.append(y)
    return {
        "schema": "dolrecon.callgraph/1",
        "project": project_dir,
        "edges": [{"from": a, "to": b, "kind": k, "via": v} for (a, b, k, v) in sorted(edges)],
        "kinds": dict(collections.Counter(k for (_, _, k, _) in edges)),
        "data_only_holders": {o: sorted(set(fs)) for o, fs in holders.items() if o not in referenced},
        "unresolved_virtual_calls": unresolved_virtual,
        "functions": len(funcs),
    }


def main(argv=None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    vc = None
    if "--vcalls" in argv:
        i = argv.index("--vcalls")
        vc = argv[i + 1]
        del argv[i:i + 2]
    project = argv[0]
    skeleton = os.path.join(os.path.dirname(os.path.abspath(project)), "skeleton")
    out = build(project, vc, skeleton)
    with open(os.path.join(project, "callgraph.json"), "w", encoding="utf-8") as f:
        json.dump(out, f, indent=0)
    reached = collections.defaultdict(set)
    for e in out["edges"]:
        reached[e["to"]].add(e["kind"])
    only_data = sum(1 for k in reached.values() if k <= {"data", "vtable", "addr"})
    print(f"callgraph: {len(out['edges'])} edges {out['kinds']}; {len(reached)} of {out['functions']} functions have a "
          f"caller of some kind, {only_data} only through data / addresses; {len(out['data_only_holders'])} data objects "
          f"holding functions that no code references; {out['unresolved_virtual_calls']} virtual calls without a target")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
