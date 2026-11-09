"""RenderWare prototypes and callback types from the RW 3.7 SDK headers, and the game callbacks they reveal.

    python -B -m dolrecon.rwheaders <named project> <out dir>        (run from source/tools)

1. Parses every `extern <type> Name(<params>);` and `typedef <type> (*Name)(<params>);` in
   RW37SDK/Library NULL Hdr Files/*.h (comments and preprocessor lines removed) -> prototypes.json.
2. Walks every call in the game (instruction words of the named project's main.dol) to a function whose name is a
   RenderWare prototype (bl, or a tail call b). Per callback-typed parameter it maps the parameter to its EABI
   register (pointer / integer arguments r3.., floating arguments f1.., counted separately) and reads which function
   address that register holds at the call on EVERY path (dolrecon.vcalls must-analysis over the caller's blocks:
   lis + addi / ori, copies incl. mr., every GPR write decoded by dolrecon.ppc, calls clobber r0, r3-r12).
   Until 2026-11-01 this read dtk's assembly text per label-delimited block (Astra answer 5: lmw, update loads,
   mr., lis + ori and bla were mishandled, and only 8 uses per callback reached names.py N9).
   -> callbacks.json: function -> [(RenderWare function, parameter, callback type, call site)], every use.
A callback seen with two different callback types is reported as a conflict and named by neither.
"""
from __future__ import annotations

import argparse
import collections
import json
import os
import re
import sys

from . import dol as dolmod
from . import ppc
from . import spine
from . import vcalls

HDR = r"H:\ShadowTheHedgehogDiskGC_Extract\RW37SDK\Library NULL Hdr Files"
FLOAT_TYPES = {"RwReal", "float", "double", "RwReal const", "const RwReal"}


def strip_c(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    lines = []
    cont = False
    for line in text.split("\n"):
        if cont or line.lstrip().startswith("#"):
            cont = line.rstrip().endswith("\\")
            continue
        lines.append(line)
    return " ".join(lines)


def split_params(s: str) -> list[str]:
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return [] if out == ["void"] else out


def param_type(p: str) -> tuple[str, str]:
    """('RpAtomicCallBack', 'callback') from 'RpAtomicCallBack callback'; handles pointers and unnamed params."""
    p = re.sub(r"\s+", " ", p).strip()
    m = re.match(r"^(.*?)([A-Za-z_]\w*)\s*(\[\s*\w*\s*\])?$", p)
    if m and m[1].strip() and not m[1].strip().endswith(("struct", "enum", "const")):
        return m[1].strip().replace(" *", "*"), m[2]
    return p.replace(" *", "*"), ""


def parse_headers():
    protos, callbacks = {}, {}
    proto_re = re.compile(r"^extern\s+(?:\"C\"\s*)?(.+?)\b([A-Za-z_]\w*)\s*\((.*)\)\s*$", re.S)
    cb_re = re.compile(r"^typedef\s+(.+?)\(\s*\*\s*([A-Za-z_]\w*)\s*\)\s*\((.*)\)\s*$", re.S)
    for name in sorted(os.listdir(HDR)):
        if not name.endswith(".h"):
            continue
        with open(os.path.join(HDR, name), encoding="latin-1") as f:
            text = strip_c(f.read())
        for stmt in text.split(";"):
            stmt = re.sub(r"\s+", " ", stmt).strip()
            stmt = re.sub(r"^.*?\b(extern|typedef)\b", r"\1", stmt) if ("extern" in stmt or "typedef" in stmt) else stmt
            m = cb_re.match(stmt)
            if m:
                callbacks[m[2]] = {"returns": m[1].strip(), "params": [param_type(p) for p in split_params(m[3])],
                                   "header": name}
                continue
            m = proto_re.match(stmt)
            if m and "(" not in m[1]:
                protos[m[2]] = {"returns": m[1].strip(), "params": [param_type(p) for p in split_params(m[3])],
                                "header": name}
    return protos, callbacks


def registers(params) -> list[str]:
    """EABI argument register of each parameter."""
    out, gi, fi = [], 3, 1
    for t, _ in params:
        if t.replace("const ", "") in FLOAT_TYPES:
            out.append(f"f{fi}")
            fi += 1
        else:
            out.append(f"r{gi}")
            gi += 1
    return out


def scan_calls(image, funcs, protos, callbacks, entries=None):
    """{callback function name: [use]} for every call of a RenderWare prototype that receives a function address in
    a callback-typed parameter. funcs: [(address, name, size, section)]."""
    name_of = {a: n for a, n, _, _ in funcs}
    starts = {a for a, _, s, sec in funcs if s and sec.startswith(".text")}
    entries = vcalls.interior_entries(image, funcs) if entries is None else entries
    wanted = {a: n for a, n in name_of.items() if n in protos}
    found = collections.defaultdict(list)
    for a, n, size, sec in funcs:
        if not size or not sec.startswith(".text"):
            continue
        fn = vcalls.Function(image, a, size, entries.get(a, ()))
        if not any((br := ppc.branch(w, a + 4 * i)) is not None and br.target in wanted
                   for i, w in enumerate(fn.words)):
            continue

        def visit(pc, w, regs, consts, caller=n, lo=a, hi=a + size):
            br = ppc.branch(w, pc)
            if br is None or br.target not in wanted or not (br.link or not lo <= br.target < hi):
                return
            target = wanted[br.target]
            params = protos[target]["params"]
            for (t, pname), reg in zip(params, registers(params)):
                v = consts.get(int(reg[1:])) if reg.startswith("r") else None
                if t in callbacks and isinstance(v, int) and v in starts:
                    found[name_of[v]].append({"rw": target, "param": pname, "type": t, "caller": caller,
                                              "site": f"0x{pc:08X}"})
        fn.scan({3}, visit)
    return found


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("project")
    ap.add_argument("out")
    args = ap.parse_args(argv)
    protos, callbacks = parse_headers()
    functions, address = set(), {}
    pat = re.compile(r"^(\S+) = \.\w+:0x([0-9A-Fa-f]+); // type:function")
    with open(os.path.join(args.project, "config", "symbols.txt"), encoding="utf-8") as f:
        for line in f:
            m = pat.match(line)
            if m:
                functions.add(m[1])
                address[m[1]] = int(m[2], 16)
    image = dolmod.load_file(os.path.join(args.project, "main.dol"))
    found = scan_calls(image, spine.load_symbols(os.path.join(args.project, "config", "symbols.txt"))[0], protos,
                       callbacks)
    os.makedirs(args.out, exist_ok=True)
    named_rw = sorted(f for f in functions if f in protos)
    result, conflicts = {}, {}
    for fn, uses in sorted(found.items()):
        types = sorted({u["type"] for u in uses})
        if len(types) == 1:
            # "address": names.py keys on it (the analysed project's names differ from the corrected project's)
            result[fn] = {"address": f"0x{address[fn]:08X}", "type": types[0], "signature": callbacks[types[0]],
                          "params": sorted({u["param"] for u in uses}), "uses": uses, "count": len(uses)}
        else:
            conflicts[fn] = types
    with open(os.path.join(args.out, "prototypes.json"), "w", encoding="utf-8") as f:
        json.dump({"functions": protos, "callbacks": callbacks}, f, indent=1)
    with open(os.path.join(args.out, "callbacks.json"), "w", encoding="utf-8") as f:
        json.dump({"callbacks": result, "conflicts": conflicts}, f, indent=1)
    by_type = collections.Counter(v["type"] for v in result.values())
    print(json.dumps({"prototypes": len(protos), "callback_types": len(callbacks),
                      "game_symbols_with_rw_prototype": len(named_rw), "callbacks_found": len(result),
                      "conflicts": len(conflicts), "by_type": dict(by_type.most_common(20))}, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
