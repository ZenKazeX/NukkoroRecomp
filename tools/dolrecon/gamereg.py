"""Name the callbacks of the game's object-type registration table by their dispatched role.

    python -B -m dolrecon.gamereg <named project> <out dir>        (run from source/tools)

The layout factory table at 0x8052C1A0 (docs/distant-rendering/draw_paths_props.md section 9) is a null-terminated
array of pointers to type records {+0 name, +4 stage-load callback, +8 stage-release callback, +0xC create,
+0x18 u16 layout id, +0x1A kind, +0x1B flags}. Each role is the one its dispatcher gives it, and the dispatch
instructions are read from the binary before anything is named (a different binary fails here, names nothing):
  +4    0x800CA7B0  lwz r12,4(r3)  in the stage set loader's loop (setid.bin; types with +0x1B bit 0)
                    -> <TYPE>__stageLoad
  +8    0x800CA748  lwz r12,8(r3)  in the stage teardown's loop (same gate)        -> <TYPE>__stageRelease
  +0xC  0x800CA944  lwz r12,0xC(r4) after the spawn check of a layout slot, table[slot->type]
                                                                                     -> <TYPE>__create
<TYPE> is the record's name string (spaces etc. -> _). These are reconstructed ROLE names, not recovered source
identifiers. A callback that serves more than one record or role is left unnamed (a shared body names no type);
four-byte bodies (blr only) are named but counted apart, so empty callbacks do not read as reconstruction progress.
Writes <out>/registrations.json: {functions: {address: {name, evidence, size}}, records: [...]}; names.py rule N11.
"""
from __future__ import annotations

import argparse
import collections
import json
import os
import re
import struct
import sys

from . import match as mmatch

TABLE = 0x8052C1A0
DISPATCH = {          # role offset -> (instruction address, expected word, role, dispatcher)
    0x4: (0x800CA7B0, 0x81830004, "stageLoad", "stage set loader loop 0x800CA780 (called from the setid.bin loader)"),
    0x8: (0x800CA748, 0x81830008, "stageRelease", "stage teardown loop 0x800CA730"),
    0xC: (0x800CA944, 0x8184000C, "create", "spawn of a layout slot 0x800CA8D8, table[slot->type]"),
}


def cstring(proj, addr: int) -> str | None:
    out = bytearray()
    while len(out) < 64:
        b = proj.read(addr + len(out), 1)
        if not b or b == b"\0":
            break
        out += b
    try:
        text = out.decode("ascii")
    except UnicodeDecodeError:
        return None
    return text if text and text.isprintable() else None


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("project")
    ap.add_argument("out")
    args = ap.parse_args(argv)
    proj = mmatch.Project(args.project)

    def word(a):
        return struct.unpack(">I", proj.read(a, 4))[0]
    for off, (site, want, role, _) in DISPATCH.items():
        got = word(site)
        if got != want:
            print(f"dispatch for +0x{off:X} ({role}) not found: 0x{site:08X} holds {got:08X}, expected {want:08X}")
            return 1
    funcs = {a: s for a, s in proj.by_address.items() if s.section == ".text"}
    size = {}
    with open(os.path.join(args.project, "config", "symbols.txt"), encoding="utf-8") as f:
        for line in f:
            m = re.match(r"^\S+ = \.text:0x([0-9A-Fa-f]+); // type:function size:0x([0-9A-Fa-f]+)", line)
            if m:
                size[int(m[1], 16)] = int(m[2], 16)
    records, uses = [], collections.defaultdict(list)
    for i in range(1024):
        rec = word(TABLE + 4 * i)
        if rec == 0:
            break
        name = cstring(proj, word(rec)) if 0x80000000 <= word(rec) < 0x81800000 else None
        row = {"slot": i, "cell": f"0x{TABLE + 4 * i:08X}", "record": f"0x{rec:08X}", "name": name,
               "layout_id": struct.unpack(">H", proj.read(rec + 0x18, 2))[0], "kind": proj.read(rec + 0x1A, 1)[0]}
        for off, (_, _, role, _) in DISPATCH.items():
            f = word(rec + off)
            row[role] = f"0x{f:08X}" if f else None
            if f:
                if f not in size:
                    print(f"slot {i} {name}: +0x{off:X} = 0x{f:08X} is no function start: record skipped")
                    row["error"] = f"+0x{off:X} is no function"
                    continue
                uses[f].append((i, name, off, role, rec))
        records.append(row)
    out, shared = {}, 0
    for f, us in sorted(uses.items()):
        if len(us) != 1 or not us[0][1]:
            shared += 1
            continue
        i, name, off, role, rec = us[0]
        site, _, _, disp = DISPATCH[off]
        type_name = re.sub(r"[^A-Za-z0-9_]", "_", name).strip("_")
        out[f"0x{f:08X}"] = {
            "name": f"{type_name}__{role}", "size": size[f],
            "evidence": f"+0x{off:X} of type record 0x{rec:08X} \"{name}\" (table cell 0x{TABLE + 4 * i:08X}); "
                        f"dispatched by 0x{site:08X} in the {disp}"}
    os.makedirs(args.out, exist_ok=True)
    with open(os.path.join(args.out, "registrations.json"), "w", encoding="utf-8", newline="\n") as fo:
        json.dump({"table": f"0x{TABLE:08X}", "functions": out, "records": records}, fo, indent=1)
    empty = sum(1 for v in out.values() if v["size"] == 4)
    unnamed_now = sum(1 for a in out if re.fullmatch(r"fn_[0-9A-F]{8}", funcs[int(a, 16)].name))
    print(json.dumps({"records": len(records), "callbacks_named": len(out), "of_them_4_byte_bodies": empty,
                      "body_bytes": sum(v["size"] for v in out.values() if v["size"] > 4),
                      "shared_or_nameless_left_unnamed": shared, "currently_fn_": unnamed_now,
                      "by_role": dict(collections.Counter(v["name"].rsplit("__", 1)[1] for v in out.values()))},
                     indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
