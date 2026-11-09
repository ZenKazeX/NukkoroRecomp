"""Original-word platform census. Evidence only; never authorizes deletion."""

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import struct
import sys

if __package__:
    from .graphics_lift_inventory import Dol, Extents, decode, hx, table
else:
    from graphics_lift_inventory import Dol, Extents, decode, hx, table


# Labels describe architecture, not recovered game API identity.
SPR_NAMES = {1: "XER", 8: "LR", 9: "CTR", 18: "DSISR", 19: "DAR",
             22: "DEC", 25: "SDR1", 26: "SRR0", 27: "SRR1",
             268: "TBL(read)", 269: "TBU(read)", 284: "TBL(write)",
             285: "TBU(write)", 287: "PVR", 920: "HID2", 921: "WPAR",
             1008: "HID0", 1009: "HID1", 1010: "IABR", 1013: "DABR",
             1017: "L2CR"}
for _n in range(272, 276):
    SPR_NAMES[_n] = f"SPRG{_n - 272}"
for _n in range(528, 544):
    SPR_NAMES[_n] = f"{'I' if _n < 536 else 'D'}BAT{(_n % 8) // 2}{'U' if _n % 2 == 0 else 'L'}"
for _n in range(912, 920):
    SPR_NAMES[_n] = f"GQR{_n - 912}"

OPERATIONS = {
    83: ("mfmsr", "context", "read"), 146: ("mtmsr", "context", "write"),
    595: ("mfsr", "address_translation", "read"),
    659: ("mfsrin", "address_translation", "read"),
    210: ("mtsr", "address_translation", "write"),
    242: ("mtsrin", "address_translation", "write"),
    306: ("tlbie", "address_translation", "invalidate"),
    370: ("tlbia", "address_translation", "invalidate"),
    566: ("tlbsync", "address_translation", "order"),
    54: ("dcbst", "cache_and_visibility", "flush"),
    86: ("dcbf", "cache_and_visibility", "flush"),
    470: ("dcbi", "cache_and_visibility", "invalidate"),
    278: ("dcbt", "cache_and_visibility", "prefetch"),
    246: ("dcbtst", "cache_and_visibility", "prefetch"),
    982: ("icbi", "cache_and_visibility", "invalidate"),
    # dcbz writes memory; it cannot be classified as disposable cache setup.
    1014: ("dcbz", "memory_zero", "write"),
    598: ("sync", "ordering", "order"),
    854: ("eieio", "ordering", "order"),
    310: ("eciwx", "external_memory", "read"),
    438: ("ecowx", "external_memory", "write"),
    20: ("lwarx", "atomic_memory", "read"),
    150: ("stwcx.", "atomic_memory", "conditional_write"),
}


def spr_identity(number):
    if number in (268, 284):
        return "timebase.low", "logical_clock"
    if number in (269, 285):
        return "timebase.high", "logical_clock"
    if number == 22:
        return "decrementer", "logical_clock"
    if number in (1, 8, 9):
        return f"spr.{number}", "execution_semantics"
    if 912 <= number <= 919:
        return f"spr.{number}", "quantized_numeric_semantics"
    if 528 <= number <= 543 or number == 25:
        return f"spr.{number}", "address_translation"
    if number in (1008, 1009, 1017, 920, 921):
        return f"spr.{number}", "cache_and_processor_configuration"
    return f"spr.{number}", "context_or_unclassified_processor_state"


def platform_effect(word):
    """Decode an encoding, not a claim that the word is executable/reachable."""
    op, xo = word >> 26, (word >> 1) & 1023
    if op == 31 and xo in (339, 467, 371):
        number = ((word >> 16) & 31) | (((word >> 11) & 31) << 5)
        key, domain = spr_identity(number)
        return {"operation": {339: "mfspr", 467: "mtspr", 371: "mftb"}[xo],
                "access": "write" if xo == 467 else "read", "spr": number,
                "spr_label": SPR_NAMES.get(number, f"SPR{number}"),
                "identity": key, "domain": domain, "gpr": (word >> 21) & 31}
    if op == 31 and xo in OPERATIONS:
        name, domain, access = OPERATIONS[xo]
    elif op == 19 and xo == 150:
        name, domain, access = "isync", "ordering", "order"
    elif op == 19 and xo == 50:
        name, domain, access = "rfi", "context", "return"
    elif op == 17:
        name, domain, access = "sc", "context", "call"
    elif op == 4 and xo == 1014:
        name, domain, access = "dcbz_l", "memory_zero", "write"
    elif op == 63 and xo in (583, 711, 38, 70, 134, 64):
        name = {583: "mffs", 711: "mtfsf", 38: "mtfsb1", 70: "mtfsb0", 134: "mtfsfi", 64: "mcrfs"}[xo]
        domain, access = "numeric_context", "read" if xo == 583 else "read_modify_write"
    else:
        return None
    identity = ("fpscr" if domain == "numeric_context" else
                "msr" if name in ("mfmsr", "mtmsr") else name)
    return {"operation": name, "domain": domain, "access": access, "identity": identity}


def implicit_consumers(word):
    """Known consumer encodings; not an exhaustive helper/memory-effect model.

    Source boundaries: ppc_instruction_traits.h, generator emit_group4 and
    emit_primary, ppc_ops.h psq_load/psq_store. Masks preserve raw-bit evidence;
    this function executes no guest numerical semantics.
    """
    op, xo, xo6 = word >> 26, (word >> 1) & 1023, (word >> 1) & 63
    result = []
    if op in (56, 57, 60, 61) or (op == 4 and xo6 in (6, 7, 38, 39)):
        store = op in (60, 61) or (op == 4 and xo6 in (7, 39))
        index = (word >> (7 if op == 4 else 12)) & 7
        number = 912 + index
        result.append({"identity": f"spr.{number}", "spr": number,
                       "domain": "quantized_numeric_semantics", "consumer": "psq_store" if store else "psq_load",
                       "required_mask": hx(0x3f07 if store else 0x3f070000)})
    hid2_mask = (0x10000000 if xo == 1014 else 0x20000000) if op == 4 else (
        0xa0000000 if op in (56, 57, 60, 61) else 0)
    if hid2_mask:
        result.append({"identity": "spr.920", "spr": 920,
                       "domain": "cache_and_processor_configuration", "consumer": "extension_enable_guard",
                       "required_mask": hx(hid2_mask)})
    fp_guard = ((op == 4 and xo != 1014) or op in (48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 59, 60, 61, 63)
                or (op == 31 and xo in (535, 567, 599, 631, 663, 695, 727, 759, 983)))
    if fp_guard:
        result.append({"identity": "msr", "domain": "context", "consumer": "fp_available_guard",
                       "required_mask": hx(0x2000)})
    if op == 19 and xo == 50:
        for number in (26, 27):
            result.append({"identity": f"spr.{number}", "spr": number,
                           "domain": "context_or_unclassified_processor_state", "consumer": "exception_return"})
    return result


def structural_walk(image, roots):
    """Conservative direct paths. No ABI/indirect/exception closure inference."""
    seen, pending, unresolved, outside = set(), list(roots), [], []
    while pending:
        pc = pending.pop()
        if pc in seen:
            continue
        seen.add(pc)
        word = image.word(pc)
        flow = decode(word, pc)
        successors = []
        if flow is not None:
            if flow["target"] is None:
                unresolved.append({"site": hx(pc), "word": hx(word), **flow})
            else:
                successors.append(flow["target"])
        if flow is None or flow["may_fall_through"]:
            successors.append((pc + 4) & 0xffffffff)
        for target in successors:
            if image.contains_text(target, 4):
                if target not in seen:
                    pending.append(target)
            else:
                outside.append({"site": hx(pc), "target": hx(target)})
    return seen, sorted(unresolved, key=lambda r: r["site"]), outside


def build_inventory(dol_raw, functions_raw=None, roots=()):
    image = Dol(dol_raw)
    root_set = sorted(set((image.entry, *roots)))
    if any(pc % 4 or not image.contains_text(pc, 4) for pc in root_set):
        raise ValueError("every root must be an aligned file-backed text word")
    rows = table(functions_raw, "functions", image) if functions_raw is not None else []
    extents = Extents(rows)
    reached, unresolved, outside = structural_walk(image, root_set)
    sites, groups, operations, domains = [], {}, Counter(), Counter()
    execution_counts, words, implicit_counts = Counter(), 0, Counter()

    def group_for(identity, domain):
        return groups.setdefault(identity, {"identity": identity, "domain": domain,
            "register_encodings": set(), "reads": [], "writes": [], "other": [],
            "implicit_reads": 0, "implicit_examples": [],
            "deletion_authorized": False, "consumer_closure_proven": False})

    for section in image.text:
        data = memoryview(image.raw)[section["file_offset"]:section["file_offset"] + section["size"]]
        for index, (word,) in enumerate(struct.iter_unpack(">I", data)):
            words += 1
            pc = section["address"] + index * 4
            for consumer in implicit_consumers(word):
                implicit_counts[consumer["consumer"]] += 1
                group = group_for(consumer["identity"], consumer["domain"])
                if "spr" in consumer:
                    group["register_encodings"].add(consumer["spr"])
                group["implicit_reads"] += 1
                # Presentation examples only. Every consumer is counted; a
                # truncated example list is explicitly never a complete trace.
                if len(group["implicit_examples"]) < 32:
                    group["implicit_examples"].append({"site": hx(pc), "word": hx(word), **consumer})
            effect = platform_effect(word)
            if effect is None:
                continue
            operations[effect["operation"]] += 1
            domains[effect["domain"]] += 1
            if effect["domain"] == "execution_semantics":
                execution_counts[effect["spr_label"] + "." + effect["access"]] += 1
                continue
            item = {"site": hx(pc), "word": hx(word), "file_offset": image.offset(pc),
                    **effect, "direct_structural_path_from_roots": pc in reached,
                    "declaration_annotations": list(extents.at(pc)), "executed_proven": False}
            sites.append(item)
            group = group_for(effect["identity"], effect["domain"])
            if "spr" in effect:
                group["register_encodings"].add(effect["spr"])
            group[{"read": "reads", "write": "writes"}.get(effect["access"], "other")].append(hx(pc))
    for group in groups.values():
        group["register_encodings"] = sorted(group["register_encodings"])
        group["encoded_reads_absent"] = not group["reads"] and group["implicit_reads"] == 0
        group["implicit_examples_complete"] = len(group["implicit_examples"]) == group["implicit_reads"]
    return {"schema": "platform_effect_inventory_v1", "whole_game_native": False,
            "inputs": {name: {"sha256": hashlib.sha256(raw).hexdigest(), "size_bytes": len(raw)}
                       for name, raw in (("dol", dol_raw), ("functions", functions_raw)) if raw is not None},
            "scope": {"word_census": "Every aligned file-backed text word; data encodings may match.",
                      "roots": [hx(pc) for pc in root_set],
                      "direct_paths": "Conditional branches and calls conservatively include fallthrough; no conditions evaluated.",
                      "declarations": "CSV containment annotations only; overlaps retained; not procedure ownership.",
                      "indirect_and_exception_closure_proven": False, "runtime_liveness_proven": False,
                      "mmio_memory_accesses_covered": False, "runtime_generated_code_covered": False,
                      "implicit_reads": "All known PSQ GQR, extension/FP guard and rfi SRR encodings counted; examples capped at32, not a trace. Other implicit/helper effects remain unknown.",
                      "native_replacement_proven": False, "deletion_authorized": False,
                      "warning": "Absence of encoded reads or a direct path never proves dead state. Aliases, memory, native helpers, callbacks, saves and exceptions need independent proofs."},
            "summary": {"text_words_scanned": words, "direct_structural_words": len(reached),
                        "platform_sites": len(sites), "unresolved_control_sites": len(unresolved),
                        "outside_text_edges": len(outside), "operations": dict(sorted(operations.items())),
                        "domains": dict(sorted(domains.items())),
                        "implicit_consumers": dict(sorted(implicit_counts.items())),
                        "execution_register_encodings_omitted_from_site_list": dict(sorted(execution_counts.items()))},
            "groups": sorted(groups.values(), key=lambda r: r["identity"]),
            "sites": sites, "unresolved_control_sites": unresolved,
            "outside_text_edges": outside,
            "declaration_annotations": rows, "declaration_extent_overlaps": extents.overlaps()}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dol", required=True, type=Path)
    parser.add_argument("--functions", type=Path)
    parser.add_argument("--root", action="append", default=[], type=lambda v: int(v, 0))
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        if args.out.resolve() in {p.resolve() for p in (args.dol, args.functions) if p}:
            raise ValueError("output may not overwrite an input")
        if args.dol.stat().st_size > 64 * 1024 * 1024:
            raise ValueError("DOL exceeds supported original-image format bound (64 MiB)")
        report = build_inventory(args.dol.read_bytes(), args.functions.read_bytes() if args.functions else None,
                                 args.root)
        with args.out.open("x", encoding="utf-8", newline="\n") as output:
            json.dump(report, output, indent=2)
            output.write("\n")
    except (OSError, ValueError) as error:
        print(f"platform inventory refused: {error}", file=sys.stderr)
        return 1
    print(json.dumps(report["summary"], sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
