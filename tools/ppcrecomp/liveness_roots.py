"""Extra liveness roots for the generator's unreachable-translation pruning.

ppcrecomp --prune-roots <file> keeps a translation only if it can execute
(see the pruning pass in tools/ppcrecomp/main.cpp). The DOL itself supplies the
entry point, the vector sections and every data word; what it cannot supply is
the guest code the NATIVE runtime enters: HLE hooks, native callers and
continuations named by address in the runtime sources. This tool lists those,
plus the address census's evidence-backed augmented roots, one per line:

  0x80051BBC                       a root (a runtime literal)
  0x80379C18 census 0x8037A0EC     a census augmented root and the sites that
                                   construct it (none: nothing records one)

The generator reaches a census root through the rows that construct it (those
holding a listed site, and those whose hi/lo halves form it) and seeds it
unconditionally only when nothing forms it. A runtime literal is always a root.

  liveness_roots.py --runtime runtime --census <census-v2.json> --out <file>
  liveness_roots.py --runtime runtime --census <census-v2.json> --check <file>

A build tree that does not compile some runtime sources (the lifted renderer's tree drops the legacy RenderWare
capture, GPU and bloom: SHADOW_RENDER_LIFTED in CMakeLists.txt) must not be rooted by their literals, or the
generator keeps guest bodies only code that tree never builds can enter. --without <path> (repeatable; a path from
the directory holding the runtime directory, e.g. runtime/src/gekko/rw_capture.cpp) leaves that file out, and
the output names the omitted files in its header, so --check also refuses a list that no longer matches.
--base <file> lists only roots that file (the full roots file) also lists: the lifted file is then exactly the
full file minus the roots only the omitted sources name, however stale the full file is (native_liveness_roots
reports that on its own). A named
file that does not exist is an error (a renamed source must not silently root again).

--check fails (exit 1) unless the file is exactly what these inputs produce, so
a new hook can never be pruned silently. Any 0x8xxxxxxx hex literal in the CODE
of runtime/**/*.{cpp,h} counts (string literals included, comments not - a
comment cannot enter guest code): a data address roots nothing, so over-listing
is harmless and under-listing is impossible.

One exception, declared and verified: a TEST under runtime/tests/ that only
runs original words in the reference interpreter names guest code it never
enters as translated code (the original SDK it compares a native owner with).
Its literals would keep exactly the bodies a native owner replaced in the
shipping image. Such a file carries the line

  // liveness-roots: reference-interpreter-only

and contributes no roots. The declaration is refused outside runtime/tests/
and in any file that can reach the generated code (it includes gekko/recomp.h
or names the recomp:: tables).

A second, per line: a guest code address the runtime only uses as a VALUE and
never enters - a return address it hands a callee as its LR, so the callee
records the original call site - roots nothing, and rooting it would keep the
fragment of a replaced body that contains it. Such a declaration is one line
holding exactly one literal and the comment

  // liveness-roots: value-only

Anything else on a marked line is refused.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

LITERAL = re.compile(r"0[xX](8[0-9A-Fa-f]{7})(?![0-9A-Fa-f])")
SUFFIXES = (".cpp", ".h", ".hpp", ".inl", ".cc")
# One token at a time: a string or character literal (kept, it is code), a
# line or block comment (dropped), anything else (kept).
TOKENS = re.compile(r'"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'|//[^\n]*|/\*.*?\*/', re.S)


def strip_comments(text: str) -> str:
    return TOKENS.sub(lambda m: " " if m.group(0)[:2] in ("//", "/*") else m.group(0), text)


INTERPRETER_ONLY = "// liveness-roots: reference-interpreter-only"


def interpreter_only(path: Path, root: Path, text: str) -> bool:
    """True for a test that declares (verifiably) it never enters generated code."""
    if not any(line.strip() == INTERPRETER_ONLY for line in text.splitlines()):
        return False
    relative = path.relative_to(root).as_posix()
    if not relative.startswith("tests/"):
        raise SystemExit(f"{relative}: only a test under tests/ may declare '{INTERPRETER_ONLY}'")
    code = strip_comments(text)
    if "gekko/recomp.h" in code or "recomp::" in code:
        raise SystemExit(f"{relative}: declares '{INTERPRETER_ONLY}' but can reach the generated code")
    return True


VALUE_ONLY = "// liveness-roots: value-only"


def drop_value_only(path: Path, root: Path, text: str) -> str:
    """The text without its value-only declaration lines (each holds exactly one literal)."""
    kept = []
    for number, line in enumerate(text.splitlines(keepends=True), 1):
        if VALUE_ONLY in line:
            if len(LITERAL.findall(strip_comments(line))) != 1:
                relative = path.relative_to(root).as_posix()
                raise SystemExit(f"{relative}:{number}: '{VALUE_ONLY}' must mark a line with exactly one literal")
            kept.append("\n")
            continue
        kept.append(line)
    return "".join(kept)


def runtime_literals(root: Path, without: tuple = ()) -> set:
    found = set()
    omitted = set(without)
    for name in omitted:
        if not (root.parent / name).is_file():
            raise SystemExit(f"--without {name}: no such file under {root.parent}")
    for path in sorted(root.rglob("*")):
        if path.is_file() and path.suffix.lower() in SUFFIXES:
            if path.relative_to(root.parent).as_posix() in omitted:
                continue
            text = path.read_text(encoding="utf-8", errors="replace")
            if interpreter_only(path, root, text):
                continue
            code = strip_comments(drop_value_only(path, root, text))
            for match in LITERAL.finditer(code):
                found.add(int(match.group(1), 16))
    return found


def census_roots(path: Path) -> dict:
    """Augmented root -> the sites the census records constructing it."""
    census = json.loads(path.read_text(encoding="utf-8"))
    if census.get("schema") != "platform_address_coordinate_census_v2":
        raise SystemExit("unsupported census schema")
    result = {}
    for entry in census["augmented_roots"]:
        sites = entry.get("evidence", {}).get("adjacent_code_pointer_construction", [])
        result[int(entry["root"], 16)] = sorted({int(site, 16) for site in sites})
    return result


def render(literals: set, census: dict, without: tuple = (), base: set = None) -> str:
    lines = ["# Extra liveness roots for ppcrecomp --prune-roots.",
             "# Generated by tools/ppcrecomp/liveness_roots.py: every 0x8xxxxxxx literal in the",
             "# runtime sources' code, and every augmented root of the address census with the",
             "# sites that construct it. Do not edit; regenerate (the native_liveness_roots test",
             "# refuses a stale file)."]
    if without:
        lines += ["# This build does not compile these runtime sources, so their literals root nothing here:"]
        lines += ["#   " + name for name in sorted(without)]
    for address in sorted(literals | set(census)):
        if base is not None and address not in base:
            continue
        if address in literals:
            lines.append("0x%08X" % address)
        else:
            lines.append(" ".join(["0x%08X census" % address] + ["0x%08X" % site for site in census[address]]))
    return "\n".join(lines) + "\n"


def parse(text: str) -> set:
    """Every address the file lists, root or census."""
    return {int(line.split()[0], 16) for line in text.splitlines() if line.strip() and not line.startswith("#")}


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--census", type=Path, required=True)
    parser.add_argument("--without", action="append", default=[], metavar="PATH",
                        help="a runtime source the build does not compile (repeatable)")
    parser.add_argument("--base", type=Path, help="list only roots this roots file also lists")
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--out", type=Path)
    group.add_argument("--check", type=Path)
    args = parser.parse_args(argv)
    without = tuple(sorted(set(args.without)))
    literals = runtime_literals(args.runtime, without)
    census = census_roots(args.census)
    base = parse(args.base.read_text(encoding="utf-8")) if args.base else None
    text = render(literals, census, without, base)
    if args.out is not None:
        with args.out.open("x", encoding="utf-8", newline="\n") as stream:
            stream.write(text)
        print(f"{len(parse(text))} liveness roots written to {args.out}")
        return 0
    listed = args.check.read_text(encoding="utf-8")
    if listed != text:
        missing = sorted(((literals | set(census)) & (base if base is not None else (literals | set(census)))) - parse(listed))
        print("liveness roots file is stale" + (": unlisted addresses " + ", ".join("0x%08X" % a for a in missing[:20])
                                                + (" ..." if len(missing) > 20 else "") if missing else "; regenerate it"))
        return 1
    print(f"liveness roots cover the runtime: {len(literals)} runtime roots, {len(census)} census roots")
    return 0


if __name__ == "__main__":
    sys.exit(main())
