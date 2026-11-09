"""Name the game's RenderWare GameCube device driver by role, from the driver tables RenderWare itself defines.

    python -B -m dolrecon.rwdriver <named project> <spine dir> <out dir>        (run from source/tools)

The PC libraries cannot name the GameCube driver (it has no twin there), but every RenderWare driver registers itself
through the same structures, and the NULL driver (RW37SDK NULL libraries) shows each one with its names:
  standards  the static {RwStandardFunc index, function} list a driver copies into RWSRCGLOBAL(stdFunc)
             (NULL: NullSystem's saGStandards; index 4 -> NullRasterCreate ...). The game's list is found by shape
             (>= 20 pairs, indices unique, 1..28); slot n -> Gcn<role of n>, the roles read from the NULL list
             (indices 25-27, absent from NULL: the rwSTANDARDNATIVETEXTURE* names of rwplcore.h).
  device     the static RwDevice block (NULL: dGNullDevice inside _rwDeviceGetHandle): fpSystem is the function
             that uses the standards list; the block's other slots -> Gcn<role> after the NULL block's slot names.
             The function that only returns the block's address is _rwDeviceGetHandle (its NULL definition).
  nodes      RxNodeDefinition {name, nodeBody, nodeInit, nodeTerm, pipelineNodeInit, ...} whose name is a ".csl"
             string: body -> <X>Node, pipelineNodeInit -> <X>PipelineInit (the D3D9 libraries' convention), the
             12-byte function returning its address -> RxNodeDefinitionGet<X>; <X> = the string without ".csl"
             (kept as written: the game's "GamerCube..." typo is RenderWare's own).
A function that would get two names is left unnamed. Writes <out>/rw_driver.json: {functions, data} by address, each
with name and evidence; names.py rule N10 applies it, and rwalign takes its RenderWare API names as seeds.
"""
from __future__ import annotations

import argparse
import collections
import json
import os
import struct
import sys

from . import coff
from . import match as mmatch
from . import rwalign

NATIVE_TEXTURE = {25: "NativeTextureGetSize", 26: "NativeTextureRead", 27: "NativeTextureWrite"}


def null_tables():
    """(index -> role) from NullSystem's standards list and (offset -> role) from dGNullDevice."""
    roles, device = {}, {}
    for name, body in coff.members(os.path.join(rwalign.LIBDIR, "rwcore.lib")):
        if not name.endswith("banull.obj"):
            continue
        secs, syms = coff._read(body)
        objs = {d.name: d for d in coff.parse_data(name, body)}
        for _, (raw_name, value, sec, _t, _c) in syms.items():
            n = coff.clean(raw_name)              # as parse_data names it (MSVC's trailing @9 dropped)
            if "saGStandards" in n and n in objs:
                raw = secs[sec - 1][4] + value
                for off, e in objs[n].entries.items():
                    if e[0] == "fn" and off >= 4:
                        roles[struct.unpack_from("<I", body, raw + off - 4)[0]] = e[1].removeprefix("Null")
            if "dGNullDevice" in n and n in objs:
                device = {off: e[1].removeprefix("Null") for off, e in objs[n].entries.items() if e[0] == "fn"}
    roles.update(NATIVE_TEXTURE)
    return roles, device


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("project")
    ap.add_argument("spine")
    ap.add_argument("out")
    args = ap.parse_args(argv)
    proj = mmatch.Project(args.project)
    info, _gc, _rw, _gs, _gp, gdata, _gk, gobjs = rwalign.load_guest(args.project, args.spine)
    roles, device_roles = null_tables()
    users = collections.defaultdict(list)
    for a, ds in gdata.items():
        for d in ds:
            users[d].append(a)
    fnames = collections.defaultdict(set)          # guest function -> {(name, evidence)}
    data = {}

    def name_fn(addr, name, why):
        fnames[addr].add((name, why))

    def getter(obj):
        """The function that only returns obj's address: exactly lis r3, hi / addi r3, r3, lo / blr, the value
        being obj's address (the shape itself is checked, not only a size and a reference)."""
        want = proj.symbols[obj].address
        found = []
        for a in users.get(obj, ()):
            if int(info[a]["size"]) != 12:
                continue
            w0, w1, w2 = struct.unpack(">3I", proj.read(a, 12))
            lo = (w1 & 0xFFFF) - (0x10000 if w1 & 0x8000 else 0)
            if (w0 & 0xFFFF0000 == 0x3C600000 and w1 & 0xFFFF0000 == 0x38630000 and w2 == 0x4E800020
                    and (((w0 & 0xFFFF) << 16) + lo) & 0xFFFFFFFF == want):
                found.append(a)
        return found[0] if len(found) == 1 else None

    # standards list
    std = None
    for k, (size, e) in gobjs.items():
        if k not in proj.symbols:
            continue
        base = proj.symbols[k].address
        pairs = [(struct.unpack(">I", proj.read(base + off - 4, 4))[0], e[off][1])
                 for off in sorted(e) if e[off][0] == "fn" and off >= 4 and off - 4 not in e]
        idx = [i for i, _ in pairs]
        if len(pairs) >= 20 and len(set(idx)) == len(idx) and all(1 <= i <= 28 for i in idx) and size == 8 * len(pairs):
            if std is not None:
                print(f"two standards lists ({std} and {k}): none named")
                std = None
                fnames.clear()                   # the first list's names are not evidence either
                break
            std = k
            for i, f in pairs:
                if i in roles:
                    name_fn(f, f"Gcn{roles[i]}", f"slot {i} of the GameCube driver's RwStandardFunc list {k} "
                                                 f"(NULL: Null{roles[i]})")
    system = None
    if std:
        sysf = users.get(std, [])
        if len(sysf) == 1:
            system = sysf[0]
            data[proj.symbols[std].address] = ("GcnSystem__saGStandards",
                                               "the {RwStandardFunc index, function} list GcnSystem installs "
                                               "(NULL: NullSystem's saGStandards)")
    # device block
    for k, (size, e) in gobjs.items():
        if system is None or size != 0x38 or e.get(4) != ("fn", system):
            continue
        for off, role in device_roles.items():
            if e.get(off, ("",))[0] == "fn":
                name_fn(e[off][1], f"Gcn{role}", f"RwDevice slot +0x{off:X} of the GameCube device block {k} "
                                                f"(NULL: Null{role})")
        data[proj.symbols[k].address] = ("_rwDeviceGetHandle__dGGcnDevice",
                                         "the GameCube RwDevice block (fpSystem = GcnSystem; NULL: dGNullDevice)")
        g = getter(k)
        if g:
            name_fn(g, "_rwDeviceGetHandle", f"returns the address of the device block {k} (the NULL driver "
                                             "defines _rwDeviceGetHandle the same way)")
    # pipeline node definitions
    for k, (size, e) in gobjs.items():
        head = e.get(0)
        if not head or head[0] != "str" or not head[1].endswith(".csl") or k not in proj.symbols:
            continue
        x = head[1][:-4].removeprefix("node")
        if e.get(4, ("",))[0] == "fn":
            name_fn(e[4][1], f"{x}Node", f"nodeBody of RxNodeDefinition {k} \"{head[1]}\"")
        if e.get(0x10, ("",))[0] == "fn":
            name_fn(e[0x10][1], f"{x}PipelineInit", f"pipelineNodeInit of RxNodeDefinition {k} \"{head[1]}\"")
        g = getter(k)
        if g:
            name_fn(g, f"RxNodeDefinitionGet{x}", f"returns the address of RxNodeDefinition {k} \"{head[1]}\"")
        data[proj.symbols[k].address] = (f"RxNodeDefinitionGet{x}__node{x}CSL", f"RxNodeDefinition \"{head[1]}\"")
    functions = {}
    for a, cand in sorted(fnames.items()):
        names = {n for n, _ in cand}
        if len(names) == 1:
            n, why = next(iter(cand))
            functions[f"0x{a:08X}"] = {"name": n, "evidence": why, "layer": info[a]["layer"]}
        else:
            print(f"0x{a:08X}: two roles {sorted(names)}: unnamed")
    out = {"functions": functions,
           "data": {f"0x{a:08X}": {"name": n, "evidence": w} for a, (n, w) in sorted(data.items())}}
    os.makedirs(args.out, exist_ok=True)
    with open(os.path.join(args.out, "rw_driver.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(out, f, indent=1)
    print(json.dumps({"functions": len(functions), "data": len(data),
                      "names": sorted(v["name"] for v in functions.values())}, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
