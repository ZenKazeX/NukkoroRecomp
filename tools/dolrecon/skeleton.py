"""The detached source skeleton: every recovered class, every function, and the platform boundary, as files.

    python -B -m dolrecon.skeleton <named project> <spine dir> <out dir>        (run from source/tools)

Reads the named project (names.json with its "classes" table), the spine (layer per function) and
source/decomp/matched.json, and writes:

  classes/<namespace path>/<Class>.h   one header per RTTI class: bases with their offsets, sizeof when proven
                                       (operator new sites), constructors / destructor, every vtable slot with its
                                       address. Members are NOT recovered yet: the layout is an opaque byte array.
  functions.csv                        every function: address, size, name, naming rule, layer, class, vtable slot,
                                       decompilation status (matched in source/decomp, or "asm" = not yet source)
  platform/boundary.h                  every SDK / RenderWare / CRI / runtime function the game calls directly,
                                       grouped by library, with call counts: what a PC platform layer must provide
  README.md                            coverage numbers and how to read the rest

Everything is generated from evidence already recorded elsewhere; nothing here is guessed. The headers are a scaffold
for decompilation (names, hierarchy, sizes, virtual order); a class is only real source once its functions are in
source/decomp and match.
"""
from __future__ import annotations

import argparse
import collections
import csv
import json
import os
import re
import sys

CPP_IDENT = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


def split_scope(cls: str) -> list[str] | None:
    """'A::B::C' -> ['A', 'B', 'C'] when every part is a plain identifier; None for templates / anonymous names."""
    parts = cls.split("::")
    return parts if all(CPP_IDENT.match(p) for p in parts) else None


def safe_path(cls: str) -> str:
    return "/".join(re.sub(r"[^A-Za-z0-9_.-]+", "_", p).strip("_") or "_" for p in cls.split("::"))


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("project")
    ap.add_argument("spine")
    ap.add_argument("out")
    ap.add_argument("--decomp", default=os.path.join(os.path.dirname(__file__), "..", "..", "decomp"))
    args = ap.parse_args(argv)

    with open(os.path.join(args.project, "names.json"), encoding="utf-8") as f:
        nj = json.load(f)
    names, classes = nj["names"], nj.get("classes", {})
    signatures = nj.get("signatures", {})

    def prototype(addr: int, name: str) -> str:
        sig = signatures.get(f"{addr:08X}")
        if not sig:
            return ""
        params = ", ".join(f"{t} {n}".strip() for t, n in sig["params"]) or "void"
        return f"{sig['returns']} {name}({params})"
    with open(os.path.join(args.spine, "functions.csv"), encoding="utf-8") as f:
        rows = list(csv.DictReader(f))
    by_addr = {int(r["address"], 16): r for r in rows}
    matched = {}
    mpath = os.path.join(args.decomp, "matched.json")
    if os.path.exists(mpath):
        with open(mpath, encoding="utf-8") as f:
            for e in json.load(f).get("files", []):
                for fn in e.get("functions", []):
                    if fn.get("matched"):
                        matched[int(fn["address"], 16)] = f"source/decomp/{e['file']}"

    # slot / class ownership
    slot_of = collections.defaultdict(list)      # function -> [(class, slot)]
    for cls, info in classes.items():
        for i, m in enumerate(info["methods"]):
            if m is not None:
                slot_of[int(m, 16)].append((cls, i))
    ctor_of = {int(a, 16): cls for cls, info in classes.items() for a in info["ctors"]}
    dtor_of = {int(a, 16): cls for cls, info in classes.items() for a in info["dtors"]}

    os.makedirs(args.out, exist_ok=True)
    # ---- functions.csv ------------------------------------------------------------------------------------------
    status = collections.Counter()
    with open(os.path.join(args.out, "functions.csv"), "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["address", "size", "name", "naming_rule", "layer", "class", "role", "vtable_slots", "status",
                    "signature"])
        for a, r in sorted(by_addr.items()):
            rule = names.get(f"{a:08X}", {}).get("rule", "") or ("dtk/corrections" if not re.match(
                r"(fn|lbl)_[0-9A-F]{8}$", r["name"]) else "")
            cls, role = "", ""
            if a in dtor_of:
                cls, role = dtor_of[a], "destructor"
            elif a in ctor_of:
                cls, role = ctor_of[a], "constructor"
            elif a in slot_of:
                owners = sorted({c for c, _ in slot_of[a]})
                cls, role = (owners[0], "virtual") if len(owners) == 1 else (" | ".join(owners[:4]), "virtual (shared)")
            slots = " ".join(f"{c}#{i}" for c, i in slot_of.get(a, [])[:6])
            st = "matched" if a in matched else "asm"
            status[(r["layer"], st)] += 1
            w.writerow([f"0x{a:08X}", r["size"], r["name"], rule, r["layer"], cls, role, slots, st,
                        prototype(a, r["name"])])

    # ---- class headers --------------------------------------------------------------------------------------------
    fname = {a: r["name"] for a, r in by_addr.items()}
    written = 0
    for cls, info in sorted(classes.items()):
        path = os.path.join(args.out, "classes", safe_path(cls) + ".h")
        os.makedirs(os.path.dirname(path), exist_ok=True)
        scope = split_scope(cls)
        lines = [f"// {cls}", f"// generated by tools/dolrecon/skeleton.py from {os.path.basename(args.project)}; "
                 "evidence in names.json \"classes\"",
                 f"// RTTI record {info['record']}, vtable {info['vtable'] or '(none found)'}"]
        if info.get("size"):
            sites = ", ".join(info.get("size_from", [])[:3])
            lines.append(f"// operator new allocates 0x{info['size']:X} ({info['size']}) bytes for it at every site "
                         f"found: {sites} (= sizeof unless a site allocates more)")
        else:
            lines.append("// size: no operator new site with a known constructor")
        lines.append("#pragma once")
        lines.append("")
        # CodeWarrior's RTTI base list is FLATTENED (every base subobject, virtual ones too, at its offset in this
        # class): it does not say which bases are direct, so no inheritance list is written (Astra answer 6)
        subs = info["direct_bases"]
        if subs:
            listed = {b for b, _ in subs}
            outer = [b for b in dict.fromkeys(b for b, _ in subs)
                     if not any(b in {x for x, _ in classes.get(o, {}).get("direct_bases", [])} for o in listed - {b})]
            lines.append("// base subobjects (RTTI, flattened): " + ", ".join(f"{b} +0x{off:X}" for b, off in subs))
            lines.append("// not derived through another listed base: " + ", ".join(outer))
        body_indent = "    "
        if scope:
            for p in scope[:-1]:
                lines.append(f"namespace {p} {{")
            head = f"class {scope[-1]}" + " {"
        else:
            lines.append(f"// '{cls}' is a template or unnamed-namespace class: declared by name only")
            head = f"class /* {cls} */ __skeleton_{re.sub(r'[^A-Za-z0-9_]', '_', cls)[:80]}" + " {"
        lines.append(head)
        lines.append("public:")
        short = scope[-1] if scope else "Class"
        for a in info["ctors"]:
            lines.append(f"{body_indent}{short}(/* parameters not recovered */);  // {a} {fname.get(int(a, 16), '')}")
        for i, m in enumerate(info["methods"]):
            if m is None:
                lines.append(f"{body_indent}virtual void vf{i}(/* signature not recovered */) = 0;  // slot {i}: pure")
                continue
            ma = int(m, 16)
            n = fname.get(ma, f"fn_{ma:08X}")
            size = by_addr.get(ma, {}).get("size", "?")
            tag = "matched" if ma in matched else "asm"
            if m in info["dtors"]:
                lines.append(f"{body_indent}virtual ~{short}();  // slot {i}: {m} {n} ({size} B, {tag})")
            else:
                lines.append(f"{body_indent}virtual void vf{i}(/* signature not recovered */);  // slot {i}: {m} "
                             f"{n} ({size} B, {tag})")
        lines.append("private:")
        lines.append(f"{body_indent}// members not recovered yet")
        lines.append("};")
        if scope:
            for p in reversed(scope[:-1]):
                lines.append(f"}}  // namespace {p}")
        if info.get("size") and scope:
            lines.append(f"// once members are in: static_assert(sizeof({cls}) == 0x{info['size']:X});")
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(lines) + "\n")
        written += 1

    # ---- platform boundary ----------------------------------------------------------------------------------------
    with open(os.path.join(args.spine, "boundary.csv"), encoding="utf-8") as f:
        brows = list(csv.DictReader(f))
    groups = collections.defaultdict(list)
    for b in brows:
        groups[(b["layer"], b["sdk_lib"])].append(b)
    os.makedirs(os.path.join(args.out, "platform"), exist_ok=True)
    out = ["// Every non-game function that game code calls directly (tools/dolrecon/spine.py boundary.csv).",
           "// A native PC build implements these (SDK -> a PC platform layer; RenderWare -> RW 3.7 for PC;",
           "// CRI -> ADX/Sofdec decoders; msl/runtime -> the host C/C++ runtime). Signatures are not recovered",
           "// yet: each entry is the original symbol, its address, and how often the game calls it.",
           "#pragma once", ""]
    for (layer, lib), bs in sorted(groups.items()):
        sites = sum(int(b["game_call_sites"]) for b in bs)
        out.append(f"// ---- {layer}{(' / ' + lib) if lib else ''}: {len(bs)} functions, {sites} game call sites")
        for b in sorted(bs, key=lambda x: -int(x["game_call_sites"])):
            proto = prototype(int(b["address"], 16), b["callee"])
            if proto:
                out.append(f"{proto};  // {b['address']}, {b['game_call_sites']} game calls from "
                           f"{b['game_callers']} functions (RW 3.7 SDK prototype)")
            else:
                out.append(f"//   {b['callee']:<60} {b['address']}  calls {b['game_call_sites']:>5}  "
                           f"from {b['game_callers']} functions")
        out.append("")
    with open(os.path.join(args.out, "platform", "boundary.h"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out))

    # ---- README ---------------------------------------------------------------------------------------------------
    named = sum(1 for r in rows if not re.match(r"(fn|lbl)_[0-9A-F]{8}$", r["name"]))
    by_rule = collections.Counter(v["rule"] for v in names.values())
    layer_tot = collections.Counter(r["layer"] for r in rows)
    layer_named = collections.Counter(r["layer"] for r in rows if not re.match(r"(fn|lbl)_[0-9A-F]{8}$", r["name"]))
    sized = sum(1 for v in classes.values() if v.get("size"))
    md = ["# Detached source skeleton (generated)", "",
          f"Project `{os.path.basename(args.project)}`, spine `{os.path.basename(args.spine)}`.",
          "Generated by `tools/dolrecon/skeleton.py`; regenerate, do not edit.", "",
          "## Coverage", "",
          f"- functions: {len(rows)}, named {named} ({100 * named / len(rows):.1f} %)",
          f"- classes: {len(classes)} RTTI classes, {sum(1 for v in classes.values() if v['vtable'])} with a vtable, "
          f"{sized} with an operator new allocation size, {written} headers",
          f"- constructors named {by_rule.get('N6 ctor', 0)}, destructors {by_rule.get('N7 dtor', 0)}, "
          f"self-named library functions {by_rule.get('N5 self-name', 0)}",
          f"- decompiled to matching source: {len(matched)} functions (source/decomp)", "",
          "| layer | functions | named | matched source |", "|---|---:|---:|---:|"]
    for lay in ["game", "game-unit", "boost", "rw", "cri", "msl", "runtime", "sdk", "unresolved"]:
        md.append(f"| {lay} | {layer_tot[lay]} | {layer_named[lay]} | {status[(lay, 'matched')]} |")
    md += ["", "## Files", "",
           "- `functions.csv`: every function with name, naming rule, layer, class, role, vtable slots, status.",
           "- `classes/`: one header per class (hierarchy, sizeof when proven, ctor/dtor, virtual slots).",
           "- `platform/boundary.h`: the SDK / RenderWare / CRI / runtime surface the game calls.", "",
           "## What is not recovered yet", "",
           "- function signatures and member layouts (headers carry opaque layouts);",
           "- names of most SDK, RenderWare and CRI internals (dtk names, self-names and RW anchors only);",
           "- source for everything not listed as matched: those functions exist as byte-exact assembly "
           "(`.build/dolrecon/<project>/build/asm`)."]
    with open(os.path.join(args.out, "README.md"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(md) + "\n")
    print("\n".join(md[:20]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
