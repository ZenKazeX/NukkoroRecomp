#!/usr/bin/env python3
"""Native-runtime verification: what of the GameCube execution environment does a linked image still contain?

Reads the linker map of ShadowTheHedgehog.exe (MSVC /MAP) and measures, from what was actually linked:

  * translated guest functions (ppcrecomp output: ``gekko::recomp::fn_XXXXXXXX(Cpu&)``) and their code bytes;
  * every other linked function whose signature takes the emulated CPU context (``gekko::Cpu&``): the surface
    through which native code still speaks the guest machine's language (owners, hooks, HLE, hardware models);
  * code bytes per component class of tools/native/guest_runtime_components.json (guest CPU model, guest memory,
    console OS / hardware models, native game code, renderer, platform, third party).

The image is a native Windows game in the sense of the migration brief only when the first two are zero and no
object of a class marked ``"guest_machine": true`` is linked. The tool states that verdict; it never infers it from
source text. Ratchets (``--max-*``) turn it into a gate that can only go down.

    python -B tools/native/verify_native_runtime.py --map .build/native/bin/Release/ShadowTheHedgehog.map \
        [--json out.json] [--max-translated N] [--max-cpu-signature N] [--max-guest-machine-bytes N] [--require-native]

Exit status: 0 = within the ratchets (and native, with --require-native); 1 = a ratchet or the requirement failed;
2 = the map could not be read as expected (the tool refuses to report on a map it does not understand).
"""
from __future__ import annotations

import argparse
import fnmatch
import json
import re
import sys
from collections import defaultdict
from pathlib import Path

SYMBOL = re.compile(r"^ (\d{4}):([0-9a-fA-F]{8})\s+(\S+)\s+([0-9a-fA-F]{16})\s+(?:f\s+)?(?:i\s+)?(\S+)\s*$")
SECTION = re.compile(r"^ (\d{4}):([0-9a-fA-F]{8})\s+([0-9a-fA-F]{8})H\s+(\S+)\s+(CODE|DATA)\s*$")
TRANSLATED = re.compile(r"^\?fn_([0-9A-F]{8})(_[A-Za-z0-9_]+)?@recomp@gekko@@")
RATCHET_KEYS = ("max_translated", "max_cpu_signature", "max_guest_machine_bytes")
CPU_ARGUMENT = "AEAVCpu@"       # ...(class gekko::Cpu &...) in an MSVC decorated name, any namespace nesting


def read_map(path: Path):
    """Returns (code_sections {index: end offset}, symbols [(section, offset, name, object)])."""
    code_end: dict[str, int] = {}
    symbols = []
    with path.open("r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            match = SYMBOL.match(line)
            if match:
                section, offset, name, _rva, obj = match.groups()
                symbols.append((section, int(offset, 16), name, obj))
                continue
            match = SECTION.match(line)
            if match and match.group(5) == "CODE":
                section, start, length = match.group(1), int(match.group(2), 16), int(match.group(3), 16)
                code_end[section] = max(code_end.get(section, 0), start + length)
    return code_end, symbols


def classify(obj: str, classes) -> str:
    low = obj.lower()
    for entry in classes:
        for pattern in entry["objects"]:
            if fnmatch.fnmatchcase(low, pattern.lower()):
                return entry["name"]
    return "unclassified"


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--map", required=True, type=Path)
    parser.add_argument("--components", type=Path, default=Path(__file__).with_name("guest_runtime_components.json"))
    parser.add_argument("--json", type=Path)
    parser.add_argument("--top", type=int, default=12, help="objects listed per guest-machine class")
    parser.add_argument("--max-translated", type=int, help="fail above this many translated guest functions")
    parser.add_argument("--max-cpu-signature", type=int, help="fail above this many native functions taking Cpu&")
    parser.add_argument("--max-guest-machine-bytes", type=int, help="fail above this many code bytes in guest-machine classes")
    parser.add_argument("--max-unclassified-objects", type=int, default=0,
                        help="fail when more objects than this have no class (default 0: every object is classified)")
    parser.add_argument("--require-native", action="store_true", help="fail unless the verdict is NATIVE")
    parser.add_argument("--ratchet", type=Path,
                        help="JSON file with max_translated / max_cpu_signature / max_guest_machine_bytes; a value "
                             "given on the command line wins over the file")
    parser.add_argument("--write-ratchet", action="store_true",
                        help="after a run within the ratchets, lower the --ratchet file to the measured values "
                             "(a value is never raised; a missing file is created)")
    parser.add_argument("--min-symbols", type=int, default=1000,
                        help="refuse a map with fewer symbols than this (default 1000: a real image)")
    args = parser.parse_args(argv)
    ratchet = {}
    if args.ratchet is not None and args.ratchet.exists():
        try:
            ratchet = json.loads(args.ratchet.read_text(encoding="utf-8"))
        except ValueError as error:
            print(f"error: {args.ratchet} is not JSON: {error}")
            return 2
    if args.ratchet is not None and not args.ratchet.exists() and not args.write_ratchet:
        print(f"error: ratchet file {args.ratchet} does not exist (create it with --write-ratchet on the baseline image); "
              "a missing ratchet is not a passing one")
        return 2
    if args.write_ratchet and args.ratchet is None:
        print("error: --write-ratchet needs --ratchet FILE")
        return 2
    for key in RATCHET_KEYS:
        if getattr(args, key) is None and isinstance(ratchet.get(key), int):
            setattr(args, key, ratchet[key])

    # A missing map is never a pass and never a traceback: say what is missing and how the designated run gets it.
    if str(args.map) in ("", ".") or not args.map.is_file():
        print(f"error: game image map not found: '{args.map}'. Build the game image (lifted renderer, no tests) and "
              "give its bin/Release/ShadowTheHedgehog.map; for the CTest native_runtime_ratchet, configure the tests "
              "tree with -DSHADOW_NATIVE_RATCHET_MAP=<that file>.")
        return 2
    classes = json.loads(args.components.read_text(encoding="utf-8"))["classes"]
    code_end, symbols = read_map(args.map)
    if not code_end or len(symbols) < args.min_symbols:
        print(f"error: {args.map} is not a readable /MAP file ({len(code_end)} code sections, {len(symbols)} symbols)")
        return 2

    # Size of a code symbol = distance to the next distinct address in its section (the map gives no lengths).
    code = sorted({(s, o, n, obj) for (s, o, n, obj) in symbols if s in code_end})
    sizes = []
    for index, (section, offset, name, obj) in enumerate(code):
        end = code_end[section]
        for later in range(index + 1, len(code)):
            if code[later][0] != section:
                break
            if code[later][1] != offset:
                end = code[later][1]
                break
        sizes.append((name, obj, max(0, end - offset)))

    per_class = defaultdict(lambda: {"bytes": 0, "symbols": 0, "objects": defaultdict(int)})
    translated_functions: set[str] = set()
    translated_symbols = translated_bytes = 0
    cpu_signature = defaultdict(int)          # object -> native functions taking Cpu&
    cpu_signature_bytes = 0
    for name, obj, size in sizes:
        bucket = per_class[classify(obj, classes)]
        bucket["bytes"] += size
        bucket["symbols"] += 1
        bucket["objects"][obj] += size
        match = TRANSLATED.match(name)
        if match:
            translated_functions.add(match.group(1))
            translated_symbols += 1
            translated_bytes += size
        elif CPU_ARGUMENT in name:
            cpu_signature[obj] += 1
            cpu_signature_bytes += size

    guest_classes = {entry["name"] for entry in classes if entry.get("guest_machine")}
    guest_machine_bytes = sum(per_class[name]["bytes"] for name in guest_classes if name in per_class)
    total_bytes = sum(bucket["bytes"] for bucket in per_class.values())
    unclassified = sorted(per_class["unclassified"]["objects"]) if "unclassified" in per_class else []
    cpu_signature_total = sum(cpu_signature.values())
    native = not translated_functions and cpu_signature_total == 0 and guest_machine_bytes == 0 and not unclassified

    print(f"map: {args.map}")
    print(f"code: {total_bytes:,} bytes in {len(sizes):,} symbols")
    print(f"translated guest functions: {len(translated_functions):,} "
          f"({translated_symbols:,} symbols incl. extra entries, {translated_bytes:,} bytes = "
          f"{100.0 * translated_bytes / max(1, total_bytes):.1f} % of the code)")
    print(f"native functions taking the guest CPU context (Cpu&): {cpu_signature_total:,} in {len(cpu_signature):,} "
          f"objects, {cpu_signature_bytes:,} bytes")
    print("code by component class (guest-machine classes marked *):")
    order = [entry["name"] for entry in classes] + ["unclassified"]
    for name in order:
        if name not in per_class:
            continue
        bucket = per_class[name]
        mark = "*" if name in guest_classes else " "
        print(f"  {mark} {name:<28} {bucket['bytes']:>12,} bytes  {len(bucket['objects']):>4} objects  "
              f"{100.0 * bucket['bytes'] / max(1, total_bytes):5.1f} %")
        if name in guest_classes or name == "unclassified":
            ranked = sorted(bucket["objects"].items(), key=lambda item: -item[1])
            for obj, size in ranked[: args.top]:
                print(f"        {size:>12,}  {obj}")
            if len(ranked) > args.top:
                print(f"        ... {len(ranked) - args.top} more")
    print(f"guest-machine code: {guest_machine_bytes:,} bytes = {100.0 * guest_machine_bytes / max(1, total_bytes):.1f} %")
    print("VERDICT: " + ("NATIVE (no translated function, no Cpu& signature, no guest-machine object)" if native else
                         "NOT NATIVE (the image contains the GameCube execution environment)"))

    failures = []
    if args.max_translated is not None and len(translated_functions) > args.max_translated:
        failures.append(f"translated guest functions {len(translated_functions):,} > ratchet {args.max_translated:,}")
    if args.max_cpu_signature is not None and cpu_signature_total > args.max_cpu_signature:
        failures.append(f"Cpu& functions {cpu_signature_total:,} > ratchet {args.max_cpu_signature:,}")
    if args.max_guest_machine_bytes is not None and guest_machine_bytes > args.max_guest_machine_bytes:
        failures.append(f"guest-machine bytes {guest_machine_bytes:,} > ratchet {args.max_guest_machine_bytes:,}")
    if len(unclassified) > args.max_unclassified_objects:
        failures.append(f"{len(unclassified)} unclassified objects (add them to {args.components.name}): "
                        + ", ".join(unclassified[:8]))
    if args.require_native and not native:
        failures.append("--require-native: the image is not native")

    if args.json:
        report = {
            "map": str(args.map), "code_bytes": total_bytes, "native": native,
            "translated_functions": len(translated_functions), "translated_symbols": translated_symbols,
            "translated_bytes": translated_bytes, "cpu_signature_functions": cpu_signature_total,
            "cpu_signature_bytes": cpu_signature_bytes, "guest_machine_bytes": guest_machine_bytes,
            "cpu_signature_by_object": dict(sorted(cpu_signature.items(), key=lambda item: -item[1])),
            "classes": {name: {"bytes": bucket["bytes"], "symbols": bucket["symbols"],
                               "guest_machine": name in guest_classes,
                               "objects": dict(sorted(bucket["objects"].items(), key=lambda item: -item[1]))}
                        for name, bucket in per_class.items()},
            "failures": failures,
        }
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(report, indent=1) + "\n", encoding="utf-8")
    for failure in failures:
        print("FAIL: " + failure)
    if args.write_ratchet and not failures:
        measured = {"max_translated": len(translated_functions), "max_cpu_signature": cpu_signature_total,
                    "max_guest_machine_bytes": guest_machine_bytes}
        lowered = {key: min(measured[key], ratchet[key]) if isinstance(ratchet.get(key), int) else measured[key]
                   for key in RATCHET_KEYS}
        if any(ratchet.get(key) != lowered[key] for key in RATCHET_KEYS):
            ratchet.update(lowered)
            ratchet.setdefault("comment", "Ratchets of tools/native/verify_native_runtime.py: the linked image may not "
                                          "exceed these. Lowered with --write-ratchet after a migration step; never "
                                          "raised to make a build pass.")
            args.ratchet.write_text(json.dumps(ratchet, indent=1) + "\n", encoding="utf-8")
            print("ratchet lowered: " + ", ".join(f"{key} {lowered[key]:,}" for key in RATCHET_KEYS))
        else:
            print("ratchet unchanged")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
