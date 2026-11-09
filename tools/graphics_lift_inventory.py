import argparse
from bisect import bisect_right
from collections import defaultdict
from contextlib import ExitStack
import csv
import hashlib
import io
import json
from pathlib import Path
import re
import struct
import sys


LIMIT = 1 << 32
CANDIDATE = re.compile(r"^_*(?:GX|Rw|rw|Rp|rp|Render|Draw|PostGlare)")


def hx(value):
    return f"0x{value:08x}"


def disjoint(spans, source):
    spans = sorted(spans)
    for left, right in zip(spans, spans[1:]):
        if left[1] > right[0]:
            raise ValueError(f"{source}: overlapping ranges {left[2]} and {right[2]}")


class Dol:
    def __init__(self, raw):
        if len(raw) < 0x100:
            raise ValueError("DOL: truncated 0x100-byte header")
        self.raw, self.sections = raw, []
        for index in range(18):
            offset, address, size = (struct.unpack_from(">I", raw, base + index * 4)[0]
                                     for base in (0, 0x48, 0x90))
            if not size:
                continue
            if offset < 0x100 or offset + size > len(raw) or address + size > LIMIT:
                raise ValueError(f"DOL section {index}: invalid file or address bounds")
            if index < 7 and (offset % 4 or address % 4 or size % 4):
                raise ValueError(f"DOL text section {index}: unaligned offset/address/size")
            self.sections.append({"index": index, "address": address, "size": size,
                                  "file_offset": offset, "kind": "text" if index < 7 else "data"})
        for field in ("file_offset", "address"):
            disjoint([(s[field], s[field] + s["size"], s["index"]) for s in self.sections],
                     f"DOL section {field}")
        self.text = sorted((s for s in self.sections if s["kind"] == "text"), key=lambda s: s["address"])
        self.bss, self.bss_size, self.entry = struct.unpack_from(">III", raw, 0xd8)
        if self.bss + self.bss_size > LIMIT:
            raise ValueError("DOL: BSS address range wraps")
        if self.entry % 4 or not self.contains_text(self.entry, 4):
            raise ValueError("DOL: entry is not an aligned file-backed text word")

    def contains_text(self, address, size):
        end = address + size
        for section in self.text:
            start, stop = section["address"], section["address"] + section["size"]
            if stop <= address:
                continue
            if start > address:
                break
            address = min(end, stop)
            if address == end:
                return True
        return False

    def offset(self, address):
        for section in self.text:
            if section["address"] <= address and address + 4 <= section["address"] + section["size"]:
                return section["file_offset"] + address - section["address"]
        raise ValueError(f"DOL: no text word at {hx(address)}")

    def word(self, address):
        return struct.unpack_from(">I", self.raw, self.offset(address))[0]


def csv_rows(raw, source):
    try:
        reader = csv.reader(io.StringIO(raw.decode("utf-8-sig"), newline=""), strict=True)
        for values in reader:
            yield reader.line_num, values
    except (UnicodeError, csv.Error) as error:
        raise ValueError(f"{source} CSV: {error}") from error


def table(raw, source, image):
    records = csv_rows(raw, source)
    _, header = next(records, (0, []))
    if (not {"addr_hex", "name", "size"}.issubset(header) or
            len(header) != len(set(header)) or any(not key for key in header)):
        raise ValueError(f"{source} CSV: missing/empty/duplicate header columns")
    rows = []
    for line, values in records:
        where = f"{source} CSV line {line}"
        if len(values) != len(header):
            raise ValueError(f"{where}: wrong column count")
        row = dict(zip(header, values))
        address, name, size = (row[key].strip() for key in ("addr_hex", "name", "size"))
        if not re.fullmatch(r"(?:0[xX])?[0-9a-fA-F]{1,8}", address):
            raise ValueError(f"{where}: invalid hexadecimal addr_hex")
        if not re.fullmatch(r"[0-9]{1,10}", size) or not name or any(ord(c) < 32 for c in name):
            raise ValueError(f"{where}: invalid name or decimal byte size")
        address, size = int(address, 16), int(size, 10)
        if (not size or address % 4 or size % 4 or address + size > LIMIT or
                not image.contains_text(address, size)):
            raise ValueError(f"{where}: range must be aligned, nonempty, and fully in text")
        rows.append({"id": f"{source}:{line}", "source": source, "csv_line": line,
                     "address": address, "name": name, "size": size})
    if source == "functions" and not rows:
        raise ValueError("functions CSV: no declared entries")
    return sorted(rows, key=lambda r: (r["address"], r["csv_line"]))


class Extents:
    def __init__(self, rows):
        events = defaultdict(list)
        for row in rows:
            events[row["address"]].append((True, row["id"]))
            events[row["address"] + row["size"]].append((False, row["id"]))
        self.spans, active, previous = [], set(), None
        for address in sorted(events):
            if active and previous is not None:
                self.spans.append((previous, address, tuple(sorted(active))))
            for add, identifier in events[address]:
                active.add(identifier) if add else active.remove(identifier)
            previous = address
        self.starts = [span[0] for span in self.spans]

    def at(self, address):
        if address is None:
            return None
        index = bisect_right(self.starts, address) - 1
        return self.spans[index][2] if index >= 0 and address < self.spans[index][1] else ()

    def overlaps(self):
        return [{"address": hx(a), "end_exclusive": hx(b), "label_ids": ids}
                for a, b, ids in self.spans if len(ids) > 1]


def decode(word, pc):
    op, xo = word >> 26, (word >> 1) & 1023
    if op in (16, 18):
        bits, mask = (16, 0xfffc) if op == 16 else (26, 0x03fffffc)
        offset = word & mask
        offset = (offset ^ (1 << (bits - 1))) - (1 << (bits - 1))
        flow = {"mnemonic": "bc" if op == 16 else "b", "absolute": bool(word & 2),
                "link": bool(word & 1), "target": (offset + (0 if word & 2 else pc)) & (LIMIT - 1)}
    elif op == 19 and xo in (16, 528):
        flow = {"mnemonic": "bclr" if xo == 16 else "bcctr", "link": bool(word & 1),
                "target": None, "target_source": "LR" if xo == 16 else "CTR",
                "return_like": word == 0x4e800020}
    elif op == 17 or (op == 19 and xo == 50):
        return {"mnemonic": "sc" if op == 17 else "rfi", "target": None,
                "target_source": "exception_handler" if op == 17 else "exception_state",
                "conditional": False, "link": False, "may_fall_through": False}
    else:
        return None
    flow["conditional"] = False
    if op == 16 or op == 19:
        flow["bo"], flow["bi"] = (word >> 21) & 31, (word >> 16) & 31
        flow["conditional"] = (flow["bo"] & 0x14) != 0x14
    flow["may_fall_through"] = flow["link"] or flow["conditional"]
    return flow


def gaps(start, end, spans):
    result, cursor = [], start
    for left, right in spans:
        if right <= cursor:
            continue
        if left >= end:
            break
        if left > cursor:
            result.append((cursor, left))
        cursor = min(end, max(cursor, right))
    if cursor < end:
        result.append((cursor, end))
    return [{"address": hx(a), "end_exclusive": hx(b), "word_count": (b - a) // 4} for a, b in result]


def analyse(image, roots, labels, extents):
    seen, work, transfers, entry_words = set(), sorted(roots, reverse=True), {}, {}
    leaders = set(roots) | set(labels) | {section["address"] for section in image.text}
    direct, unresolved, boundary = [], [], []

    def record(pc, word, target):
        return {"site": hx(pc), "word": hx(word), "file_offset": image.offset(pc),
                "site_declarations": extents.at(pc), "target": hx(target) if target is not None else None,
                "target_declarations": extents.at(target)}

    while work:
        pc = work.pop()
        if pc in seen:
            continue
        seen.add(pc)
        word, successors = image.word(pc), []
        if pc in labels:
            entry_words[pc] = word
        flow = decode(word, pc)
        if flow is not None:
            target = flow["target"]
            edge = {**flow, **record(pc, word, target)}
            if target is None:
                unresolved.append(edge)
            else:
                edge.update(kind="call" if flow["link"] else "branch", target_in_text=image.contains_text(target, 4))
                direct.append(edge)
                successors.append(target)
                leaders.add(target)
        if flow is None or flow["may_fall_through"]:
            target = (pc + 4) & (LIMIT - 1)
            successors.append(target)
            if flow is not None:
                leaders.add(target)
            if not image.contains_text(target, 4):
                basis = "conditional_fallthrough" if flow and flow["conditional"] else "fallthrough"
                if flow and flow["link"]:
                    basis = "conditional_or_call_may_return" if flow["conditional"] else "call_may_return"
                boundary.append({**record(pc, word, target), "basis": basis})
        mapped = tuple(sorted({target for target in successors if image.contains_text(target, 4)}))
        if flow is not None:
            transfers[pc] = mapped
        work.extend(target for target in mapped if target not in seen)
    blocks = []
    for pc in sorted(seen):
        if not blocks or pc in leaders or blocks[-1][1] != pc:
            blocks.append([pc, pc + 4])
        else:
            blocks[-1][1] = pc + 4
    nodes = [{"address": hx(a), "end_exclusive": hx(b), "instruction_count": (b - a) // 4,
              "root_labels": roots.get(a), "enclosing_declarations_at_start": extents.at(a),
              "successors": [hx(target) for target in transfers.get(b - 4, (b & (LIMIT - 1),)) if target in seen]}
             for a, b in blocks]
    return seen, blocks, entry_words, {"cfg_nodes": nodes, "direct_edges": sorted(direct, key=lambda e: e["site"]),
        "unresolved_sites": sorted(unresolved, key=lambda e: e["site"]),
        "unmapped_fallthroughs": sorted(boundary, key=lambda e: e["site"])}


def label(row):
    return {"id": row["id"], "address": hx(row["address"]), "name": row["name"], "source": row["source"],
            "csv_line": row["csv_line"], "size_bytes": row["size"], "end_exclusive": hx(row["address"] + row["size"]),
            "verification": "unverified", "graphics_candidate": bool(CANDIDATE.match(row["name"]))}


def build_inventory(dol_raw, functions_raw, names_raw=None):
    image = Dol(dol_raw)
    functions = table(functions_raw, "functions", image)
    hints = table(names_raw, "names", image) if names_raw is not None else []
    extents, hint_extents = Extents(functions), Extents(hints)
    by_entry, hint_entries, labels = defaultdict(list), defaultdict(list), defaultdict(list)
    for row in functions + hints:
        (by_entry if row["source"] == "functions" else hint_entries)[row["address"]].append(row["id"])
        labels[row["address"]].append(label(row))
    roots = {address: {"address": hx(address), "dol_entry": address == image.entry,
                      "declarations": by_entry.get(address, []), "name_hints": hint_entries.get(address, [])}
             for address in sorted(set(by_entry) | {image.entry})}
    seen, blocks, entry_words, graph = analyse(image, roots, labels, extents)
    nodes = {int(node["address"], 16): node for node in graph["cfg_nodes"]}
    inbound = defaultdict(list)
    for edge in graph["direct_edges"]:
        inbound[edge["target"]].append(edge)
    candidates = [{"entry": hx(address), "is_root": address in roots, "visited": address in seen,
                   "entry_word": hx(entry_words[address]) if address in entry_words else None,
                   "entry_file_offset": image.offset(address), "labels": items,
                   "enclosing_declarations": extents.at(address),
                   "entry_block_instruction_count": nodes[address]["instruction_count"] if address in nodes else None,
                   "inbound_direct_edges": inbound.get(hx(address), [])}
                  for address, items in sorted(labels.items()) if any(item["graphics_candidate"] for item in items)]
    ambiguities = {"declaration_extent_overlaps": extents.overlaps(), "name_hint_extent_overlaps": hint_extents.overlaps(),
        "duplicate_function_entries": [{"address": hx(a), "label_ids": ids} for a, ids in sorted(by_entry.items()) if len(ids) > 1],
        "duplicate_name_hint_entries": [{"address": hx(a), "label_ids": ids} for a, ids in sorted(hint_entries.items()) if len(ids) > 1]}
    uncovered = [gap for section in image.text for gap in gaps(section["address"], section["address"] + section["size"], blocks)]
    summary = {"declarations": len(functions), "name_hints": len(hints), "roots": len(roots),
        "text_words": sum(s["size"] // 4 for s in image.text), "instruction_count": len(seen), "cfg_nodes": len(nodes),
        "direct_edges": len(graph["direct_edges"]), "direct_calls": sum(e["link"] for e in graph["direct_edges"]),
        "unresolved_sites": len(graph["unresolved_sites"]), "unmapped_fallthroughs": len(graph["unmapped_fallthroughs"]),
        "direct_edges_outside_text": sum(not e["target_in_text"] for e in graph["direct_edges"]),
        "unvisited_text_words": sum(gap["word_count"] for gap in uncovered), "graphics_candidate_entries": len(candidates),
        "candidate_inbound_direct_edges": sum(len(c["inbound_direct_edges"]) for c in candidates),
        **{key: len(value) for key, value in ambiguities.items()}}
    return {"schema": "graphics_lift_inventory_v2", "view": "full_graph",
        "inputs": {name: {"sha256": hashlib.sha256(raw).hexdigest(), "size_bytes": len(raw)} if raw is not None else None
                   for name, raw in (("dol", dol_raw), ("functions", functions_raw), ("names", names_raw))},
        "scope": {
            "mode": "offline_partial_global_raw_address_cfg_evidence_only",
            "roots": "DOL entry plus every declared function entry; CSV roots are hypothetical, not proof of executable reachability.",
            "traversal": "Follow mapped-text direct targets and conditional/call continuations globally, irrespective of declared extents.",
            "ownership": "Address containment only: all enclosing function declaration IDs are listed (0/1/many); no exclusive owner.",
            "extents": "CSV sizes are advisory hypotheses, never traversal boundaries. Overlaps and duplicate aliases are retained.",
            "blocks": "One global address partition, split at roots, labels, text sections, direct targets and control continuations.",
            "block_annotations": "Enclosing declarations at a block start need not enclose the whole block.",
            "names": "Every name is unverified, including tiny signature collisions; name hints do not create roots.",
            "candidate_name_pattern": CANDIDATE.pattern,
            "candidate_inbound": "Exact entry-address targets only, including non-link branches; these are views of global edges, not new edges.",
            "ignored_csv_columns": "All except addr_hex, name, size; cached ins/callees are never evidence.",
            "calls": "LK-set encodings only; continuations assume calls may return, not a proven ABI or callee identity.",
            "nonbranches": "Conservative fallthrough, no condition evaluation, instruction validity or exception closure proof.",
            "instruction_count": "Unique visited DOL words globally, not proven valid or executed instructions or per-function sizes.",
            "native_lifting_proven": False, "whole_program_reachability_proven": False,
            "instruction_validity_proven": False, "exception_closure_proven": False, "indirect_targets_resolved": False,
            "state_effect_liveness_proven": False, "interprocedural_constant_propagation": False, "runtime_generated_code_covered": False},
        "summary": summary,
        "dol": {"entry": hx(image.entry), "entry_declarations": extents.at(image.entry),
                "sections": [{**s, "address": hx(s["address"])} for s in image.sections],
                "bss": {"address": hx(image.bss), "size_bytes": image.bss_size,
                        "overlapping_loaded_sections": [s["index"] for s in image.sections if image.bss_size
                            and image.bss < s["address"] + s["size"] and s["address"] < image.bss + image.bss_size]}},
        "declarations": [label(row) for row in functions], "name_hints": [label(row) for row in hints],
        "roots": list(roots.values()), "ambiguities": ambiguities, **graph,
        "coverage_gaps": {"unvisited_text": uncovered,
            "unfollowed_direct_sites": [e["site"] for e in graph["direct_edges"] if not e["target_in_text"]],
            "limitations": ["Unvisited text is not proven unreachable or non-code; there is no linear gap sweep.",
                "LR/CTR targets, including return-like blr, remain unproven; no ABI target inference.",
                "sc/rfi exception handlers and resumption are unresolved and not followed.",
                "Data/BSS execution, initialization effects, runtime-generated and modified code are not covered.",
                "BSS is header metadata, not file-backed text; overlap with loaded sections is reported, not a mapping.",
                "No semantic lifting, native replacement, or guest-state mirror/flag liveness is proven.",
                "Native lowering must prove whole-cluster side effects and liveness before dropping mirror or flag writes."]},
        "graphics_candidates": candidates}


def candidates_view(report):
    required = set()
    for candidate in report["graphics_candidates"]:
        required.update(candidate["enclosing_declarations"])
        for edge in candidate["inbound_direct_edges"]:
            required.update(edge["site_declarations"])
            required.update(edge["target_declarations"])
    return {"schema": report["schema"], "view": "graphics_candidates", "inputs": report["inputs"],
            "scope": report["scope"], "summary": report["summary"], "graphics_candidates": report["graphics_candidates"],
            "referenced_declarations": [row for row in report["declarations"] if row["id"] in required]}


def validate_outputs(inputs, outputs):
    resolved = [path.resolve() for path in outputs]
    if len(set(resolved)) != len(resolved):
        raise ValueError("output paths alias each other")
    if set(resolved) & {path.resolve() for path in inputs}:
        raise ValueError("output path aliases an input")
    for path in outputs:
        if path.exists() or path.is_symlink():
            raise FileExistsError(f"refusing existing output: {path}")


def main(argv=None):
    parser = argparse.ArgumentParser(description="Offline partial global PPC CFG evidence; no translation or execution.")
    for flag in ("--dol", "--functions", "--output"):
        parser.add_argument(flag, required=True, type=Path)
    parser.add_argument("--names", type=Path)
    parser.add_argument("--candidates-output", type=Path)
    args = parser.parse_args(argv)
    inputs = [path for path in (args.dol, args.functions, args.names) if path is not None]
    outputs = [path for path in (args.output, args.candidates_output) if path is not None]
    try:
        validate_outputs(inputs, outputs)
        report = build_inventory(args.dol.read_bytes(), args.functions.read_bytes(),
                                 args.names.read_bytes() if args.names is not None else None)
        documents = [report] + ([candidates_view(report)] if args.candidates_output is not None else [])
        validate_outputs(inputs, outputs)
        with ExitStack() as stack:
            streams = [stack.enter_context(path.open("x", encoding="utf-8", newline="\n")) for path in outputs]
            for stream, document in zip(streams, documents):
                json.dump(document, stream, indent=2, ensure_ascii=True)
                stream.write("\n")
    except (OSError, ValueError, csv.Error) as error:
        print(f"graphics_lift_inventory: {error}", file=sys.stderr)
        return 2
    print(json.dumps({"status": "ok", **report["summary"]}, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
