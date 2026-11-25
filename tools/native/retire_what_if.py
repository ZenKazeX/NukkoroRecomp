#!/usr/bin/env python3
"""What would retiring a native owner do? The generator's own answer, per build configuration.

tools/platform/what_if.py asks what OWNING a guest function frees. This asks the opposite step of the migration:
a runtime owner file (or some of its entries) is deleted, so its rows leave extract/native-owners*.txt and the
runtime no longer names their addresses. Then either nothing reaches those guest functions any more and they are
pruned (the owner existed only for callers that are gone), or translated callers still reach them and the original
guest bodies - and whatever they call - come back into the image.

  --section FILE      retire every row of the owner-list sections headed "# runtime/.../FILE" (e.g. native_trk.cpp);
                      the section's source file is taken as deleted
  --retire ADDR ...   retire these rows only (hex); with --source FILE for each runtime file the edit removes their
                      literals from
  --config NAME=DIR   a build tree whose generated/ensure_outputs_Release.cmake holds the generator command
                      (repeatable: game=...\\.build\\n1 tests=...\\.build\\tests)

Per configuration the generator runs twice with that tree's exact arguments (baseline, cached while its inputs are
unchanged; and the step), on scratch copies: the owner lists without the retired rows, the liveness roots without
the roots that only the deleted sources name. A root that another runtime file still names stays, and is listed.
For a roots file made without the test sources (its name contains "-game"), files under runtime/tests/ do not
count as naming an address.

Reports, per configuration: what became of each retired row (pruned / translated body again), rows revived (back in
the image) and freed (newly pruned) by service, the generator's refusal if any (a site list naming an instruction
of a row that changed state), and the listed sites inside retired rows. Nothing under extract/ is written.

  python retire_what_if.py --name trk --section native_trk.cpp --config game=<dir> --config tests=<dir>
                           --dolrecon-dir <dir> --out .build/platform/retire

Exit 0 when every configuration ran (a refusal is a result, not an error); 2 for bad input.
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "platform"))
sys.path.insert(0, str(HERE))
import what_if  # noqa: E402
from owner_callers import ADDRESS, SECTION  # noqa: E402

ROOT_LINE = re.compile(r"^\s*(0x[0-9A-Fa-f]{8})\b")


def split_owner_list(text: str, section_file: str | None, retire: set[int]) -> tuple[str, list[tuple[int, str, str]]]:
    """Returns (text without the retired rows, [(address, description, section path)])."""
    kept, removed = [], []
    current = ""
    for line in text.splitlines():
        section = SECTION.match(line)
        if section:
            current = section.group(1)
        address = ADDRESS.match(line)
        if address:
            value = int(address.group(1), 16)
            in_section = section_file is not None and current.rsplit("/", 1)[-1] == section_file
            if in_section or value in retire:
                removed.append((value, address.group(2).strip(), current))
                continue
        kept.append(line)
    return "\n".join(kept) + "\n", removed


def plan_roots(roots_text: str, naming: dict[int, set[str]], retired: set[int], covered: set[str], whole_file: bool,
               without_tests: bool) -> tuple[str, list[int], dict[int, list[str]]]:
    """Returns (roots text for the step, dropped roots, {retired address still rooted: [files naming it]})."""
    def namers(address: int) -> set[str]:
        files = naming.get(address, set())
        return {f for f in files if not (without_tests and f.startswith("runtime/tests/"))}

    present = set()
    for line in roots_text.splitlines():
        match = ROOT_LINE.match(line.split("#")[0])
        if match:
            present.add(int(match.group(1), 16))
    candidates = set(retired)
    if whole_file:   # the source file goes: every root only it names goes with it
        candidates |= {address for address in present if namers(address) and namers(address) <= covered}
    drop, held = set(), {}
    for address in sorted(candidates & present):
        others = namers(address) - covered
        if others:
            if address in retired:
                held[address] = sorted(others)
        else:
            drop.add(address)
    kept = []
    for line in roots_text.splitlines():
        match = ROOT_LINE.match(line.split("#")[0])
        if match and int(match.group(1), 16) in drop:
            continue
        kept.append(line)
    return "\n".join(kept) + "\n", sorted(drop), held


def sites_inside(argv: list[str], rows: set[int], source_tree: Path) -> list[dict]:
    extents = {}
    with (source_tree / "extract" / "functions.csv").open(encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            extents[int(row["addr_hex"], 16)] = int(row["size"] or 0)
    spans = sorted((a, a + max(extents.get(a, 4), 4)) for a in rows)
    found = []
    for index, argument in enumerate(argv):
        if argument not in what_if.SITE_LISTS:
            continue
        path = argv[index + 1]
        with open(path, encoding="utf-8", errors="replace") as handle:
            for number, line in enumerate(handle, 1):
                tokens = line.split("#")[0].split()
                if not tokens:
                    continue
                token = tokens[1] if tokens[0] == "site" and len(tokens) > 1 else tokens[0]
                if not re.match(r"0x[0-9A-Fa-f]{8}$", token):
                    continue
                value = int(token, 16)
                for low, high in spans:
                    if low <= value < high:
                        found.append({"list": os.path.basename(path), "line": number, "site": token,
                                      "row": f"0x{low:08X}", "text": line.strip()[:140]})
    return found


def run_config(name: str, build_dir: Path, arguments, naming, service_of, out: Path) -> dict:
    base_argv = what_if.release_argv(str(build_dir))
    generator_note = ""
    if not Path(base_argv[0]).exists():
        # A configured tree whose generator was never built (a tests tree that built two targets): the generator is
        # the same program in every tree of one commit, so another tree's binary can run this tree's arguments.
        if not arguments.generator:
            return {"config": name, "build_dir": str(build_dir),
                    "error": f"generator not built in this tree ({base_argv[0]}); pass --generator <ppcrecomp.exe>"}
        generator_note = f"{arguments.generator} (this tree has none built; its arguments are used unchanged)"
        base_argv[0] = arguments.generator
    work = out / arguments.name / name
    work.mkdir(parents=True, exist_ok=True)
    trial = list(base_argv)
    retire = {int(a, 16) for a in arguments.retire}
    removed: list[tuple[int, str, str]] = []
    for index, argument in enumerate(base_argv):
        if argument != "--native-owners":
            continue
        path = Path(base_argv[index + 1])
        text, gone = split_owner_list(path.read_text(encoding="utf-8", errors="replace"), arguments.section, retire)
        removed += gone
        scratch = work / path.name
        scratch.write_text(text, encoding="utf-8", newline="\n")
        trial[index + 1] = str(scratch)
    result: dict = {"config": name, "build_dir": str(build_dir), "generator": generator_note or base_argv[0]}
    if not removed:
        result["error"] = "no owner row matches the step in this configuration's owner lists"
        return result
    retired = {address for address, _, _ in removed}
    covered = set(arguments.source)
    if arguments.section:
        covered |= {section for _, _, section in removed if section.rsplit("/", 1)[-1] == arguments.section}
        # The file goes with its own header / inline parts: every non-test runtime file of the same stem
        # (native_trk.cpp -> runtime/include/gekko/native_trk.h).
        stem = arguments.section.rsplit(".", 1)[0]
        covered |= {f for files in naming.values() for f in files
                    if f.rsplit("/", 1)[-1].rsplit(".", 1)[0] == stem and not f.startswith("runtime/tests/")}
    result["sources_taken_as_deleted"] = sorted(covered)
    roots_index = base_argv.index("--prune-roots") + 1
    roots_path = Path(base_argv[roots_index])
    without_tests = "-game" in roots_path.name
    roots_text, dropped, held = plan_roots(roots_path.read_text(encoding="utf-8"), naming, retired, covered,
                                           arguments.section is not None, without_tests)
    scratch_roots = work / roots_path.name
    scratch_roots.write_text(roots_text, encoding="utf-8", newline="\n")
    trial[roots_index] = str(scratch_roots)

    base_dir = out / "_baseline" / name
    stamp = base_dir / "argv.json"
    key = {"argv": base_argv, "inputs": what_if.input_digests(base_argv)}
    pruned_csv = base_dir / "recomp_pruned.csv"
    if stamp.exists() and json.loads(stamp.read_text()) == key and pruned_csv.exists():
        with pruned_csv.open() as handle:
            base = {int(r["address"], 16): r["name"] for r in csv.DictReader(handle)}
    else:
        base, refusal = what_if.run(base_argv, str(base_dir))
        if refusal:
            result["error"] = "the baseline itself is refused: " + refusal
            return result
        stamp.write_text(json.dumps(key))
    after, refusal = what_if.run(trial, str(work / "generated"))
    freed = sorted(set(after) - set(base))
    revived = sorted(set(base) - set(after))

    def grouped(addresses, names):
        groups: dict[str, list[str]] = {}
        for address in addresses:
            groups.setdefault(service_of(address), []).append(f"0x{address:08X} {names[address]}")
        return {k: {"count": len(v), "rows": v} for k, v in sorted(groups.items(), key=lambda item: -len(item[1]))}

    result.update({
        "roots_file": roots_path.name, "roots_without_tests": without_tests,
        "retired": [{"address": f"0x{a:08X}", "text": text, "section": section,
                     "after": "pruned" if a in after else "translated body in the image"} for a, text, section in removed],
        "roots_dropped": [f"0x{a:08X}" for a in dropped],
        "roots_still_named": {f"0x{a:08X}": files for a, files in held.items()},
        "baseline_pruned": len(base), "after_pruned": len(after),
        "freed": len(freed), "freed_by_service": grouped(freed, after),
        "revived": len(revived), "revived_by_service": grouped(revived, base),
        "generator_refusal": refusal,
        "listed_sites_in_retired_rows": sites_inside(trial, retired, Path(what_if.SRC)),
    })
    return result


def render(step: str, results: list[dict]) -> str:
    out = [f"# Retiring {step}", ""]
    for result in results:
        out += [f"## {result['config']} ({result['build_dir']})", ""]
        if "error" in result:
            out += [f"NOT RUN: {result['error']}", ""]
            continue
        retired = result["retired"]
        pruned = sum(1 for row in retired if row["after"] == "pruned")
        out.append(f"Generator: {result['generator']}")
        out.append(f"Roots: {result['roots_file']}" + (" (tests do not root)" if result["roots_without_tests"] else "")
                   + f"; {len(result['roots_dropped'])} roots dropped with the step.")
        out.append("Sources taken as no longer naming the retired addresses: "
                   + (", ".join(result["sources_taken_as_deleted"]) or "none (pass --source)"))
        out.append(f"{len(retired)} owner rows retired: {pruned} are pruned (nothing reaches them), "
                   f"{len(retired) - pruned} come back as translated bodies.")
        out.append(f"Beyond the retired rows: {result['revived']} rows revived, {result['freed']} rows freed "
                   f"(pruned rows {result['baseline_pruned']} -> {result['after_pruned']}).")
        if result["generator_refusal"]:
            out.append(f"GENERATOR REFUSAL: {result['generator_refusal']}")
        out.append("")
        out += ["| Retired row | After the step | Owner list text |", "|---|---|---|"]
        out += [f"| {row['address']} | {row['after']} | {row['text'][:90]} |" for row in retired]
        out.append("")
        for address, files in result["roots_still_named"].items():
            out.append(f"Root kept: {address} is still named by {', '.join(files)}")
        for title, key in (("Revived (back in the image)", "revived_by_service"), ("Freed (newly pruned)", "freed_by_service")):
            if result[key]:
                out += ["", f"{title}:"]
                for service, group in result[key].items():
                    shown = ", ".join(group["rows"][:8]) + (f", ... {group['count'] - 8} more" if group["count"] > 8 else "")
                    out.append(f"- {service} ({group['count']}): {shown}")
        if result["listed_sites_in_retired_rows"]:
            out += ["", "Listed sites inside retired rows (each needs a decision when the body returns or goes):"]
            out += [f"- {site['list']}:{site['line']} {site['site']} in {site['row']}: {site['text']}"
                    for site in result["listed_sites_in_retired_rows"][:20]]
        out.append("")
    return "\n".join(out)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument("--name", required=True)
    parser.add_argument("--section", help="base name of the runtime file whose owner sections are retired")
    parser.add_argument("--retire", nargs="*", default=[], help="owner row addresses (hex)")
    parser.add_argument("--source", action="append", default=[],
                        help="runtime file (repository path) the edit removes the retired literals from")
    parser.add_argument("--config", action="append", required=True, help="NAME=BUILD_DIR")
    parser.add_argument("--generator", help="ppcrecomp.exe to use for a configuration whose own generator is not built")
    parser.add_argument("--dolrecon-dir", default=what_if.DOLRECON)
    parser.add_argument("--out", type=Path, default=Path(what_if.SRC) / ".build" / "platform" / "retire")
    arguments = parser.parse_args(argv)
    if not arguments.section and not arguments.retire:
        print("retire_what_if: give --section or --retire", file=sys.stderr)
        return 2
    try:
        configs = [(spec.split("=", 1)[0], Path(spec.split("=", 1)[1])) for spec in arguments.config]
        naming = what_if.roots_by_file()
        service_of = what_if.services(arguments.dolrecon_dir)
        results = [run_config(name, build_dir, arguments, naming, service_of, arguments.out) for name, build_dir in configs]
    except (OSError, ValueError, IndexError, StopIteration) as error:
        import traceback
        traceback.print_exc()
        print(f"retire_what_if: {error}", file=sys.stderr)
        return 2
    step = arguments.section or ", ".join(f"0x{int(a, 16):08X}" for a in arguments.retire)
    text = render(step, results)
    target = arguments.out / arguments.name
    target.mkdir(parents=True, exist_ok=True)
    (target / "retire_what_if.json").write_text(json.dumps(results, indent=1) + "\n", encoding="utf-8")
    (target / "RETIRE.md").write_text(text + "\n", encoding="utf-8")
    sys.stdout.write(text + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
