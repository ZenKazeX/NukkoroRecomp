"""Keep the generated matching source (source/decomp/auto) on the project's current names.

    python -B -m dolrecon.decomp_names <current project> [--pin-from <project>] [--decomp ../decomp] [--dry-run]
                                                                                            (run from source/tools)

Generated files name their symbols as the project named them on the day they were written. A later, better name
(or a withdrawn wrong one) would otherwise detach a reference: dolrecon.match compares relocation targets by name.
For every file under <decomp>/auto:
  1. every external symbol (`extern ... NAME;` / `extern ... NAME[];`) without a `// @sym` line is pinned to its
     address, looked up in --pin-from (the project the files last verified under) or else the current project;
  2. every pinned symbol whose address now has another name in the current project is renamed in the file, token
     by token (`\\bNAME\\b`), pin included - unless the new name is no C identifier or is already used in the file.
Hand-written files (outside auto/) are never touched. decomp_verify proves the result: every function must still
match.
"""
from __future__ import annotations

import argparse
import os
import re
import sys

from . import match as mmatch

EXTERN = re.compile(r"^extern\s+[^;()]*?\b([A-Za-z_]\w*)\s*(?:\[\s*\])?\s*;", re.M)
PIN = re.compile(r"^// @sym (\S+) = (0x[0-9A-Fa-f]{8})\s*$", re.M)
C_IDENT = re.compile(r"[A-Za-z_]\w*")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("project")
    ap.add_argument("--pin-from")
    ap.add_argument("--decomp", default=os.path.join(os.path.dirname(__file__), "..", "..", "decomp"))
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args(argv)
    cur = mmatch.Project(args.project)
    old = mmatch.Project(args.pin_from) if args.pin_from else None
    pinned = renamed = unresolved = files = 0
    for root, _, names in os.walk(os.path.join(args.decomp, "auto")):
        for n in sorted(names):
            if not n.endswith((".c", ".cpp")):
                continue
            path = os.path.join(root, n)
            with open(path, "rb") as f:
                raw = f.read()
            text = raw.decode("utf-8")
            pins = {m[1]: int(m[2], 16) for m in PIN.finditer(text)}
            add = []
            for m in EXTERN.finditer(text):
                name = m[1]
                if name in pins:
                    continue
                sym = (old.symbols.get(name) if old else None) or cur.symbols.get(name)
                if sym is None:
                    print(f"unresolved {os.path.relpath(path, args.decomp)}: {name}")
                    unresolved += 1
                    continue
                pins[name] = sym.address
                add.append(f"// @sym {name} = 0x{sym.address:08X}")
            if add:
                head = text.index("*/") + 2 if text.startswith("/*") else 0
                nl = "\r\n" if "\r\n" in text else "\n"
                text = text[:head] + nl + nl.join(add) + text[head:]
                pinned += len(add)
            for name, addr in sorted(pins.items()):
                t = cur.by_address.get(addr)
                if t is None or t.name == name or not C_IDENT.fullmatch(t.name):
                    continue
                if re.search(rf"\b{re.escape(t.name)}\b", text):
                    print(f"kept {name} in {os.path.relpath(path, args.decomp)}: {t.name} already used there")
                    continue
                text = re.sub(rf"\b{re.escape(name)}\b", t.name, text)
                renamed += 1
            new = text.encode("utf-8")
            if new != raw:
                files += 1
                if not args.dry_run:
                    with open(path, "wb") as f:
                        f.write(new)
    print(f"{files} files changed: {pinned} external symbols pinned, {renamed} symbols renamed, "
          f"{unresolved} unresolved" + (" (dry run)" if args.dry_run else ""))
    return 1 if unresolved else 0


if __name__ == "__main__":
    sys.exit(main())
