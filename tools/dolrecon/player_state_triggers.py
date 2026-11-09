"""What original code enters each player behaviour state (docs/SHADOW_PLAYER_COVERAGE_AUDIT.md §2; route planning).

    python -B -m dolrecon.player_state_triggers <dolrecon root> <callgraph.json> <player_states.csv> <out.csv>

Two kinds of evidence, both read from main.dol:
  change   a `bl 0x800A82FC` (behaviour change: factory -> CanEnter -> Leave -> Enter) whose state id argument (r4) is a
           constant `li r4, id` reaching the call inside the calling function
  command  the classes `Player::<Name>Command` and who constructs them: a function that calls a command class's
           constructor is a sender (an object, a gimmick, the controller) - the stage content that can request it
"""
from __future__ import annotations

import collections
import csv
import json
import pathlib
import sys

from dolrecon import dol, game_map

CHANGE = 0x800A82FC


def args_state(image, fn, c: int, k: int, states: dict) -> tuple[int | None, str]:
    """The state id behind `r4` at the call c+k: r4 = r1 + off points at an InitializeArgs {vtable, id}. The id is a
    `li rX, imm` stored to off+4(r1), or - when a constructor fills the slot (`addi r3, r1, off; bl ctor`) - the
    state whose InitializeArgs<Name> class that constructor belongs to."""
    off = None
    for j in range(k - 4, max(-4, k - 160), -4):
        x = image.word(c + j)
        if (x >> 26) == 14 and ((x >> 21) & 31) == 4 and ((x >> 16) & 31) == 1:
            off = ((x & 0xFFFF) ^ 0x8000) - 0x8000
            break
        if (x >> 26) == 31 and ((x >> 1) & 1023) == 444 and ((x >> 16) & 31) == 4:  # mr r4, rS
            return None, "args-register"
    if off is None:
        return None, "no-args-slot"
    for j in range(k - 4, max(-4, k - 160), -4):
        x = image.word(c + j)
        if (x >> 26) == 36 and ((x >> 16) & 31) == 1 and (((x & 0xFFFF) ^ 0x8000) - 0x8000) == off + 4:  # stw rX
            rx = (x >> 21) & 31
            for i in range(j - 4, max(-4, j - 64), -4):
                y = image.word(c + i)
                if (y >> 26) == 14 and ((y >> 21) & 31) == rx and ((y >> 16) & 31) == 0:
                    return ((y & 0xFFFF) ^ 0x8000) - 0x8000, "args-constant"
                if ((y >> 21) & 31) == rx and (y >> 26) not in (36, 37, 38, 44, 47):
                    return None, "args-computed"
            return None, "args-computed"
        if (x >> 26) == 18 and (x & 1):  # bl: a constructor on the slot?
            prevs = [image.word(c + j - 4 * n) for n in range(1, 5)]
            if any((p >> 26) == 14 and ((p >> 21) & 31) == 3 and ((p >> 16) & 31) == 1 and
                   (((p & 0xFFFF) ^ 0x8000) - 0x8000) == off for p in prevs):
                ctor = c + j + (((x & 0x03FFFFFC) ^ 0x02000000) - 0x02000000)
                cls = fn.get(ctor, {}).get("class", "")
                for sid, name in states.items():
                    if cls.endswith("InitializeArgs" + name.rsplit("::", 1)[-1]):
                        return sid, "args-constructor"
                return None, "args-constructor:" + (cls or fn.get(ctor, {}).get("name", "?"))
    return None, "args-unknown"


def main(argv=None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    root, cg_path, states_path, out = pathlib.Path(argv[0]), argv[1], argv[2], argv[3]
    image = dol.load_file(str(root / "pal-v30/main.dol"))
    fn = game_map.load_functions(root)
    n2a = {r["name"]: a for a, r in fn.items()}
    states = {int(r["id"]): r["class"] for r in csv.DictReader(open(states_path, encoding="utf-8"))}
    callers = collections.defaultdict(set)
    for e in json.load(open(cg_path, encoding="utf-8"))["edges"]:
        s, t = n2a.get(e["from"]), n2a.get(e["to"])
        if s is not None and t is not None and e["kind"] in ("call", "tail"):
            callers[t].add(s)

    rows = []
    # 1. behaviour change call sites with a constant id
    unknown_id_sites = 0
    for c in sorted(callers[CHANGE]):
        size = int(fn[c]["size"])
        for k in range(0, size, 4):
            w = image.word(c + k)
            if (w >> 26) != 18 or not (w & 1) or (w & 2):
                continue
            if c + k + (((w & 0x03FFFFFC) ^ 0x02000000) - 0x02000000) != CHANGE:
                continue
            sid, how = args_state(image, fn, c, k, states)
            if sid is None:
                unknown_id_sites += 1
            rows.append({"state": sid if sid is not None else "", "state_class": states.get(sid, ""),
                         "kind": "change:" + how, "site": f"0x{c + k:08X}", "from_function": f"0x{c:08X}",
                         "from_name": fn[c]["name"], "from_class": fn[c]["class"]})
    # 2. command classes and their constructors' callers
    for a, r in fn.items():
        cls = r["class"].split(" | ")[0]
        if not cls.endswith("Command") or r.get("role") != "constructor":
            continue
        for c in sorted(callers[a]):
            rows.append({"state": "", "state_class": cls, "kind": "command", "site": f"0x{a:08X}",
                         "from_function": f"0x{c:08X}", "from_name": fn[c]["name"],
                         "from_class": fn[c]["class"]})
    with open(out, "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0]), lineterminator="\n")
        w.writeheader()
        w.writerows(rows)
    by_state = collections.Counter(r["state"] for r in rows if r["kind"].startswith("change"))
    print(f"change sites {sum(by_state.values())} ({unknown_id_sites} with a non-constant id); command senders "
          f"{sum(1 for r in rows if r['kind'] == 'command')}")
    for sid in sorted(states):
        print(f"  {sid:2d} {states[sid]:40s} change sites {by_state.get(sid, 0)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
