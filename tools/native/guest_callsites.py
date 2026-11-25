#!/usr/bin/env python3
"""Static inventory of native -> guest call sites in the runtime sources.

A native owner that calls back into translated code keeps that guest function (and everything it reaches) in the
image and pays a native -> guest crossing per call. This lists every such site, read from the sources only:

  gekko::guest_call(c, TARGET, link, {...}, result)          native_guest_call.h
  recomp::call_indirect(c, TARGET, link)                     called from native code
  call_guest(c, TARGET, ...)                                 the per-file helpers around call_indirect
  <interrupts>.call_guest(c, frame, TARGET, r3, r4)          NativeInterrupts
  guest_continue(c, ENTRY, return_pc, return_sp)

and classifies TARGET:
  literal    a hexadecimal guest address
  constant   a named constant whose value is a guest address in the same file (or, uniquely, in another file)
  loaded     read from guest memory at the site (a vtable slot, a callback pointer): the callee set is data-dependent
  variable   any other expression (a parameter or local: usually the body of a helper that forwards its argument)

Only `literal` and `constant` sites name a guest function. The others are counted and listed so that the unknown part
is visible; nothing is inferred for them.

Measuring the sites: --addresses writes the distinct resolved targets one hex address per line, the format of
GEKKO_CALL_CENSUS_COUNTS_ADDRS (runtime/src/gekko/system.cpp, non-shipping builds). A run with
GEKKO_CALL_CENSUS_COUNTS=<file> then records calls per target and per (target, LR) pair; native owners pass a `link`
inside their own original function, so the LR tells a native caller from a guest one.

  python guest_callsites.py [--root runtime] [--functions extract/functions.csv] [--addresses out.txt] [--json out.json]

Exit code 0; 2 when the root has no sources. Definitions and declarations of the helpers are not call sites.
"""

from __future__ import annotations

import argparse
import csv
import json
import re
import sys
from pathlib import Path

SOURCE_SUFFIXES = {".cpp", ".h", ".inc"}
SKIP_DIRECTORIES = {"tests"}  # test drivers call into fixtures, not into the game image

# Callee spellings; the group names the form. Order matters: the member form must win over the plain helper.
CALL = re.compile(
    r"(?P<interrupts>\b\w*interrupts\w*(?:\(\))?\s*(?:\.|->)\s*call_guest)\s*\("
    r"|(?P<call_indirect>\brecomp::call_indirect)\s*\("
    r"|(?P<guest_call>\bguest_call)\s*\("
    r"|(?P<guest_continue>\bguest_continue)\s*\("
    r"|(?P<call_guest>\bcall_guest)\s*\(")
TARGET_INDEX = {"interrupts": 2, "call_indirect": 1, "guest_call": 1, "guest_continue": 1, "call_guest": 1}
CONSTANT = re.compile(r"\b(k[A-Z]\w*)\s*=\s*(0[xX][0-9A-Fa-f]{8})[uU]?\s*[;,}]")
LITERAL = re.compile(r"^0[xX]([0-9A-Fa-f]{8})[uU]?$")
IDENTIFIER = re.compile(r"^(?:\w+::)*(k[A-Z]\w*)$")
GUEST_TEXT = (0x80003100, 0x80600000)  # generous bounds of executable guest addresses; data constants fall outside use


def strip_comments(text: str) -> str:
    """Replaces comments and string / character literals by spaces, keeping every newline (line numbers survive)."""
    out = []
    i, n = 0, len(text)
    while i < n:
        ch = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if ch == "/" and nxt == "/":
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif ch == "/" and nxt == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join("\n" if c == "\n" else " " for c in text[i:j]))
            i = j
        elif ch == '"' or (ch == "'" and not (i > 0 and text[i - 1].isalnum())):  # 4'000'000 is a digit separator
            j = i + 1
            while j < n and text[j] != ch and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(ch + " " * max(j - i - 2, 0) + (ch if j - i >= 2 else ""))
            i = j
        else:
            out.append(ch)
            i += 1
    return "".join(out)


def split_arguments(text: str, open_at: int) -> tuple[list[str], int] | None:
    """Arguments of the call whose '(' is at open_at, split at top-level commas; None when it is not closed."""
    depth = 0
    start = open_at + 1
    arguments = []
    for i in range(open_at, len(text)):
        ch = text[i]
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
            if depth == 0:
                last = text[start:i].strip()
                if last or arguments:
                    arguments.append(last)
                return arguments, i
        elif ch == "," and depth == 1:
            arguments.append(text[start:i].strip())
            start = i + 1
    return None


def is_definition(text: str, match_start: int, arguments: list[str], close_at: int) -> bool:
    """A declaration or definition of the helper itself: typed parameters, or a body / qualifier after the ')'."""
    if any(re.match(r"^(?:const\s+)?(?:gekko::)?(?:Cpu|u32|u64|std::\w+)\s*&?\s+\w+", a) or a.startswith("Cpu&")
           for a in arguments[:2]):
        return True
    line_start = text.rfind("\n", 0, match_start) + 1
    before = text[line_start:match_start]
    return bool(re.search(r"\b(?:bool|int|void|u32)\s+(?:\w+::)*$", before))


def classify(expression: str, local: dict[str, int], everywhere: dict[str, set[int]]) -> tuple[str, int | None, str]:
    """(kind, address or None, name)."""
    expression = " ".join(expression.split())
    literal = LITERAL.match(expression)
    if literal:
        return "literal", int(literal.group(1), 16), ""
    name = IDENTIFIER.match(expression)
    if name:
        key = name.group(1)
        if key in local:
            return "constant", local[key], key
        values = everywhere.get(key, set())
        if len(values) == 1:
            return "constant", next(iter(values)), key
        return "variable", None, key  # a constant we cannot resolve uniquely is not guessed
    if "mem_read32" in expression or "read32" in expression or "vtable" in expression.lower():
        return "loaded", None, expression
    return "variable", None, expression


def scan(root: Path) -> dict:
    files = sorted(p for p in root.rglob("*") if p.suffix in SOURCE_SUFFIXES and p.is_file()
                   and not (SKIP_DIRECTORIES & set(part.lower() for part in p.relative_to(root).parts[:-1])))
    if not files:
        raise FileNotFoundError(f"no sources under {root}")
    texts = {}
    constants: dict[Path, dict[str, int]] = {}
    everywhere: dict[str, set[int]] = {}
    for path in files:
        text = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
        texts[path] = text
        table = {}
        for found in CONSTANT.finditer(text):
            table[found.group(1)] = int(found.group(2), 16)
            everywhere.setdefault(found.group(1), set()).add(int(found.group(2), 16))
        constants[path] = table
    sites = []
    for path in files:
        text = texts[path]
        for found in CALL.finditer(text):
            form = found.lastgroup
            parsed = split_arguments(text, found.end() - 1)
            if parsed is None:
                continue
            arguments, close_at = parsed
            if is_definition(text, found.start(), arguments, close_at):
                continue
            index = TARGET_INDEX[form]
            if len(arguments) <= index:
                continue
            kind, address, name = classify(arguments[index], constants[path], everywhere)
            if address is not None and not (GUEST_TEXT[0] <= address < GUEST_TEXT[1]):
                kind, address = "variable", None
            sites.append({
                "file": path.relative_to(root).as_posix(), "line": text.count("\n", 0, found.start()) + 1,
                "form": form, "kind": kind, "target": address, "name": name})
    return {"root": str(root), "files_scanned": len(files), "sites": sites}


def load_functions(path: Path) -> list[tuple[int, int, str]]:
    rows = []
    with path.open(newline="", encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            start = int(row["addr_hex"], 16)
            rows.append((start, start + int(row["size"]), row["name"]))
    rows.sort()
    return rows


def function_at(functions: list[tuple[int, int, str]], address: int) -> tuple[str, bool] | None:
    """(name, is the function's entry) of the function containing address."""
    low, high = 0, len(functions)
    while low < high:
        middle = (low + high) // 2
        if functions[middle][0] <= address:
            low = middle + 1
        else:
            high = middle
    if low and functions[low - 1][0] <= address < functions[low - 1][1]:
        return functions[low - 1][2], functions[low - 1][0] == address
    return None


def summarize(result: dict, functions: list[tuple[int, int, str]] | None) -> dict:
    per_file: dict[str, dict[str, int]] = {}
    targets: dict[int, dict] = {}
    for site in result["sites"]:
        counts = per_file.setdefault(site["file"], {"literal": 0, "constant": 0, "loaded": 0, "variable": 0})
        counts[site["kind"]] += 1
        if site["target"] is not None:
            entry = targets.setdefault(site["target"], {"address": site["target"], "names": set(), "files": set(), "sites": 0})
            entry["sites"] += 1
            entry["files"].add(site["file"])
            if site["name"]:
                entry["names"].add(site["name"])
    rows = []
    for address in sorted(targets):
        entry = targets[address]
        row = {"address": address, "sites": entry["sites"], "names": sorted(entry["names"]), "files": sorted(entry["files"])}
        if functions is not None:
            found = function_at(functions, address)
            row["function"] = found[0] if found else None
            row["is_entry"] = bool(found and found[1])
        rows.append(row)
    return {"per_file": per_file, "targets": rows}


def render(result: dict, summary: dict) -> str:
    sites = result["sites"]
    kinds = {kind: sum(1 for s in sites if s["kind"] == kind) for kind in ("literal", "constant", "loaded", "variable")}
    out = [f"# Native -> guest call sites under {result['root']}", ""]
    out.append(f"{len(sites)} call sites in {len(summary['per_file'])} of {result['files_scanned']} source files: "
               f"{kinds['literal']} literal, {kinds['constant']} named constant, {kinds['loaded']} loaded from guest "
               f"memory, {kinds['variable']} other expression. {len(summary['targets'])} distinct guest targets are named; "
               f"the callees of the {kinds['loaded'] + kinds['variable']} other sites are not known statically.")
    out += ["", "## By file", "", "| File | Sites | Literal | Constant | Loaded | Variable |", "|---|---:|---:|---:|---:|---:|"]
    for name, counts in sorted(summary["per_file"].items(), key=lambda item: (-sum(item[1].values()), item[0])):
        out.append(f"| {name} | {sum(counts.values())} | {counts['literal']} | {counts['constant']} | "
                   f"{counts['loaded']} | {counts['variable']} |")
    out += ["", "## Named guest targets", ""]
    with_functions = any("function" in row for row in summary["targets"])
    out.append("| Target | Sites | Constant | " + ("Guest function | " if with_functions else "") + "Called from |")
    out.append("|---|---:|---|" + ("---|" if with_functions else "") + "---|")
    for row in summary["targets"]:
        function = ""
        if with_functions:
            if row["function"] is None:
                function = "NOT IN functions.csv | "
            else:
                function = row["function"] + ("" if row["is_entry"] else " (mid-function)") + " | "
        out.append(f"| 0x{row['address']:08X} | {row['sites']} | {', '.join(row['names'])} | {function}"
                   f"{', '.join(row['files'])} |")
    out += ["", "## Sites whose target is not a named address", "", "| Site | Form | Kind | Target expression |", "|---|---|---|---|"]
    for site in sites:
        if site["target"] is None:
            expression = site["name"].replace("|", "\\|")
            out.append(f"| {site['file']}:{site['line']} | {site['form']} | {site['kind']} | `{expression}` |")
    out.append("")
    return "\n".join(out)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument("--root", default="runtime", help="runtime source root (default: runtime)")
    parser.add_argument("--functions", help="extract/functions.csv: name the guest function of each target")
    parser.add_argument("--addresses", help="write the distinct targets, one hex address per line")
    parser.add_argument("--json", dest="json_out", help="write sites and summary as JSON")
    arguments = parser.parse_args(argv)
    try:
        result = scan(Path(arguments.root))
        functions = load_functions(Path(arguments.functions)) if arguments.functions else None
    except (OSError, KeyError, ValueError) as error:
        print(f"guest_callsites: {error}", file=sys.stderr)
        return 2
    summary = summarize(result, functions)
    sys.stdout.write(render(result, summary) + "\n")
    if arguments.addresses:
        Path(arguments.addresses).write_text(
            "".join(f"{row['address']:08X}\n" for row in summary["targets"]), encoding="ascii")
    if arguments.json_out:
        Path(arguments.json_out).write_text(
            json.dumps({"root": result["root"], "files_scanned": result["files_scanned"], "sites": result["sites"],
                        "per_file": summary["per_file"], "targets": summary["targets"]}, indent=1) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
