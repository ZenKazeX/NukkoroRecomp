"""Attribute actual x64 PE bytes without treating linker padding or folded aliases as code.

Section sizes are exact. Function extents come from PE exception-directory entries,
not the distance to the next symbol. Leaf functions without unwind entries remain
unattributed. A matching MSVC map supplies names, never sizes. Attribution is a size
inventory, not proof of runtime cost, reachability, or native semantic ownership.
"""
from __future__ import annotations

import argparse
from collections import defaultdict
import hashlib
import json
from pathlib import Path
import re
import struct


class AuditError(ValueError):
    pass


def fingerprint(path):
    path = Path(path)
    with path.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    return {"path": str(path.resolve()), "bytes": path.stat().st_size,
            "sha256": digest}


def read_pe(data, *, include_unwind_owners=False):
    def unpack(fmt, offset):
        if offset < 0 or offset + struct.calcsize(fmt) > len(data):
            raise AuditError("truncated PE structure")
        return struct.unpack_from(fmt, data, offset)

    if data[:2] != b"MZ":
        raise AuditError("not a DOS/PE image")
    pe, = unpack("<I", 0x3c)
    if data[pe:pe + 4] != b"PE\0\0":
        raise AuditError("missing PE signature")
    machine, count, timestamp, _, _, optional_size, _ = unpack("<HHIIIHH", pe + 4)
    opt = pe + 24
    magic, = unpack("<H", opt)
    if machine != 0x8664 or magic != 0x20b:
        raise AuditError("requires an AMD64 PE32+ image")
    if optional_size < 144 or not 0 < count <= 96:
        raise AuditError("invalid optional header or section count")
    base, = unpack("<Q", opt + 24)
    image_size, headers_size = unpack("<II", opt + 56)
    directories, = unpack("<I", opt + 108)
    exception_rva, exception_size = unpack("<II", opt + 112 + 3 * 8) if directories > 3 else (0, 0)
    sections = []
    occupied = []
    for i in range(count):
        start = opt + optional_size + 40 * i
        raw_name, virtual_size, rva, raw_size, raw_offset, _, _, _, _, flags = unpack("<8sIIIIIIHHI", start)
        if raw_size and (raw_offset < headers_size or raw_offset + raw_size > len(data)):
            raise AuditError("section data outside image")
        if raw_size:
            occupied.append((raw_offset, raw_offset + raw_size))
        sections.append({"index": i + 1, "name": raw_name.rstrip(b"\0").decode("ascii", "replace"),
                         "rva": rva, "virtual_bytes": virtual_size, "file_bytes": raw_size,
                         "file_offset": raw_offset, "executable": bool(flags & 0x20000000)})
    occupied.sort()
    if any(a[1] > b[0] for a, b in zip(occupied, occupied[1:])):
        raise AuditError("overlapping file sections")

    def file_offset(rva, size):
        for section in sections:
            delta = rva - section["rva"]
            if 0 <= delta and delta + size <= section["file_bytes"]:
                return section["file_offset"] + delta
        raise AuditError("directory is not backed by section file data")

    if exception_size % 12:
        raise AuditError("malformed AMD64 exception directory size")
    functions = []
    unwind_owners = {}
    stack_frames = {}
    unwind_cache = {}

    def fixed_frame(rva, chain=()):
        # Microsoft x64 UNWIND_INFO v1. This measures the fixed prologue,
        # not dynamic alloca, callees, or the incoming return-address slot.
        if rva in chain or len(chain) >= 64:
            raise AuditError("cyclic or excessive unwind chain")
        if rva in unwind_cache:
            return unwind_cache[rva]
        at = file_offset(rva, 4)
        version_flags, _, count, frame = unpack("<BBBB", at)
        version, flags = version_flags & 7, version_flags >> 3
        result = {"fixed_stack_bytes": None, "uses_frame_pointer": bool(frame & 15),
                  "unwind_version": version, "stack_status": "unsupported unwind version"}
        if version != 1:
            return result
        if flags & ~7 or flags & 4 and flags & 3:
            raise AuditError("invalid unwind flags")
        at = file_offset(rva, 4 + count * 2)
        total = 0
        index = 0
        while index < count:
            _, operation = unpack("<BB", at + 4 + index * 2)
            op, info = operation & 15, operation >> 4
            slots = {0: 1, 1: 2 if info == 0 else 3, 2: 1, 3: 1,
                     4: 2, 5: 3, 8: 2, 9: 3, 10: 1}.get(op)
            if slots is None:
                result["stack_status"] = f"unsupported unwind operation {op}"
                return result
            if index + slots > count:
                raise AuditError("truncated unwind operation")
            extra = at + 4 + (index + 1) * 2
            if op == 0:
                total += 8
            elif op == 1:
                if info not in (0, 1):
                    raise AuditError("invalid large-stack unwind operand")
                total += unpack("<H", extra)[0] * 8 if info == 0 else unpack("<I", extra)[0]
            elif op == 2:
                total += info * 8 + 8
            elif op == 10:
                if info not in (0, 1):
                    raise AuditError("invalid machine-frame unwind operand")
                total += 40 + info * 8
            index += slots
        if flags & 4:
            chained_at = file_offset(rva + 4 + ((count + 1) & ~1) * 2, 12)
            _, _, parent = unpack("<III", chained_at)
            parent_frame = fixed_frame(parent, chain + (rva,))
            if parent_frame["fixed_stack_bytes"] is None:
                result["stack_status"] = "unsupported parent unwind information"
                return result
            total += parent_frame["fixed_stack_bytes"]
            result["uses_frame_pointer"] |= parent_frame["uses_frame_pointer"]
        result.update(fixed_stack_bytes=total, stack_status="known fixed prologue")
        unwind_cache[rva] = result
        return result

    def unwind_owner(begin, end, unwind):
        # UNW_FLAG_CHAININFO links to a previous RUNTIME_FUNCTION, including
        # noncontiguous fragments. No nearest-symbol guess or stack unwinding.
        chain, seen = [], set()
        while True:
            if unwind in seen or len(chain) >= 64:
                raise AuditError("cyclic or excessive owner unwind chain")
            seen.add(unwind)
            if begin >= end or not any(s["executable"] and s["rva"] <= begin < end <=
                                      s["rva"] + s["virtual_bytes"] for s in sections):
                raise AuditError("unwind owner outside an executable section")
            at = file_offset(unwind, 4)
            version_flags, _, count, _ = unpack("<BBBB", at)
            version, flags = version_flags & 7, version_flags >> 3
            chain.append([begin, end, unwind])
            if version != 1:
                return {"primary_start_rva": None, "chain": chain,
                        "status": "unsupported unwind version"}
            if flags & ~7 or flags & 4 and flags & 3:
                raise AuditError("invalid owner unwind flags")
            file_offset(unwind, 4 + count * 2)
            if not flags & 4:
                return {"primary_start_rva": begin, "chain": chain,
                        "status": "explicit PE unwind chain"}
            at = file_offset(unwind + 4 + ((count + 1) & ~1) * 2, 12)
            begin, end, unwind = unpack("<III", at)

    if exception_size:
        offset = file_offset(exception_rva, exception_size)
        for pos in range(offset, offset + exception_size, 12):
            begin, end, unwind = unpack("<III", pos)
            if not begin and not end and not unwind:
                continue
            if begin >= end or not any(s["executable"] and s["rva"] <= begin < end <=
                                      s["rva"] + s["virtual_bytes"] for s in sections):
                raise AuditError("unwind code extent outside an executable section")
            functions.append((begin, end))
            stack_frames[begin] = fixed_frame(unwind)
            if include_unwind_owners:
                owner = unwind_owner(begin, end, unwind)
                if len(owner["chain"]) > 1 or owner["primary_start_rva"] is None:
                    unwind_owners[begin] = owner
    functions = sorted(set(functions))
    if any(a[1] > b[0] for a, b in zip(functions, functions[1:])):
        raise AuditError("overlapping unwind code extents")
    code_bytes = sum(s["virtual_bytes"] for s in sections if s["executable"])
    raw_total = sum(s["file_bytes"] for s in sections)
    if headers_size > len(data) or raw_total + headers_size > len(data):
        raise AuditError("invalid file-byte accounting")
    return {"image_base": base, "timestamp": timestamp, "image_virtual_bytes": image_size,
            "headers_file_bytes": headers_size, "sections": sections,
            "unassigned_file_bytes": len(data) - headers_size - raw_total,
            "code_virtual_bytes": code_bytes, "unwind_extents": functions,
            "stack_frames": stack_frames,
            **({"unwind_owners": unwind_owners} if include_unwind_owners else {})}


MAP_BASE = re.compile(r"\s*Preferred load address is ([0-9a-fA-F]+)\s*")
MAP_TIME = re.compile(r"\s*Timestamp is ([0-9a-fA-F]+)\b")
MAP_SYMBOL = re.compile(r"\s*([0-9a-fA-F]{4}):([0-9a-fA-F]{8,16})\s+(\S+)\s+"
                        r"([0-9a-fA-F]{8,16})\s+(.+?)\s*$")
GUEST = re.compile(r"\?fn_([0-9a-fA-F]{8})(?:_|@)")


def read_map(lines, pe):
    symbols = defaultdict(set)
    base = timestamp = None
    for line in lines:
        match = MAP_BASE.fullmatch(line.rstrip())
        if match:
            if base is not None:
                raise AuditError("duplicate map base")
            base = int(match[1], 16)
        match = MAP_TIME.match(line)
        if match:
            timestamp = int(match[1], 16)
        match = MAP_SYMBOL.fullmatch(line.rstrip())
        if not match:
            continue
        fields = match[5].split()
        flags = set()
        while fields and fields[0] in ("f", "i"):
            flags.add(fields.pop(0))
        if "f" not in flags or not fields or int(match[1], 16) == 0:
            continue
        rva = int(match[4], 16) - pe["image_base"]
        section = next((s for s in pe["sections"] if s["index"] == int(match[1], 16)), None)
        if not section or not section["executable"]:
            continue
        if rva != section["rva"] + int(match[2], 16):
            raise AuditError("map symbol section/address does not match PE")
        if not section["rva"] <= rva < section["rva"] + section["virtual_bytes"]:
            raise AuditError("map code symbol outside its section")
        symbols[rva].add((match[3], " ".join(fields)))
    if base != pe["image_base"] or timestamp != pe["timestamp"]:
        raise AuditError("map base/timestamp does not match executable")
    if not symbols:
        raise AuditError("map has no matched code symbols")
    return symbols


def classify(origin):
    if origin.lower().startswith("shadow_recomp:"):
        return "generated_game_code"
    return origin.split(":", 1)[0]


def attribute(pe, symbols):
    groups = defaultdict(int)
    guests = defaultdict(lambda: {"exclusive_unwind_bytes": 0, "extents": 0})
    largest = []
    named_bytes = shared_bytes = 0
    for begin, end in pe["unwind_extents"]:
        size = end - begin
        aliases = sorted(symbols.get(begin, ()))
        owners = sorted({classify(origin) for _, origin in aliases})
        group = owners[0] if len(owners) == 1 else "shared_across_owners" if owners else "no_exact_symbol"
        groups[group] += size
        if aliases:
            named_bytes += size
        guest_ids = {match[1].upper() for name, _ in aliases if (match := GUEST.search(name))}
        # Folded aliases share one extent: never multiply its bytes by alias count.
        if len(guest_ids) == 1 and all(GUEST.search(name) for name, _ in aliases):
            entry = guests[next(iter(guest_ids))]
            entry["exclusive_unwind_bytes"] += size
            entry["extents"] += 1
        elif guest_ids:
            shared_bytes += size
        largest.append({"rva": f"0x{begin:08X}", "unwind_code_bytes": size, "group": group,
                        "alias_count": len(aliases), "aliases": [{"symbol": n, "object": o}
                                                                  for n, o in aliases[:8]],
                        "guest_addresses": sorted(guest_ids)[:16], **pe["stack_frames"][begin]})
    covered = sum(end - start for start, end in pe["unwind_extents"])
    return {"unwind_covered_code_bytes": covered, "exact_named_unwind_bytes": named_bytes,
            "code_bytes_without_unwind_extent": pe["code_virtual_bytes"] - covered,
            "folded_or_mixed_guest_unwind_bytes": shared_bytes,
            "groups": dict(sorted(groups.items(), key=lambda item: (-item[1], item[0]))),
            "largest_extents": sorted(largest, key=lambda entry: -entry["unwind_code_bytes"])[:50],
            "largest_fixed_stack_frames": sorted(
                (entry for entry in largest if entry["fixed_stack_bytes"] is not None),
                key=lambda entry: -entry["fixed_stack_bytes"])[:50],
            "unknown_stack_extent_count": sum(entry["fixed_stack_bytes"] is None for entry in largest),
            "largest_generated_functions": [dict(address="0x" + address, **values) for address, values in
                                            sorted(guests.items(), key=lambda item: -item[1]["exclusive_unwind_bytes"])[:100]]}


def audit(executable, linker_map):
    executable, linker_map = Path(executable), Path(linker_map)
    identity = fingerprint(executable)
    map_identity = fingerprint(linker_map)
    pe = read_pe(executable.read_bytes())
    with linker_map.open(encoding="latin-1") as stream:
        symbols = read_map(stream, pe)
    result = {"schema": "shadow-executable-size-v1", "executable": identity, "linker_map": map_identity,
              "pe": {key: value for key, value in pe.items() if key not in ("unwind_extents", "stack_frames")},
              "attribution": attribute(pe, symbols),
              "limitations": ["Unwind extents measure code ranges, not complete symbol sizes; leaf code and padding are not guessed.",
                              "Folded aliases share bytes; shared code is never charged in full to every alias.",
                              "Fixed stack bytes describe unwind prologues, excluding the incoming return address, dynamic allocations and callees; they are not peak thread stack usage.",
                              "A map timestamp/base match is a consistency check, not cryptographic build provenance.",
                              "Object origin describes linkage, not native semantics, reachability, or runtime cost."]}
    if fingerprint(executable) != identity or fingerprint(linker_map) != map_identity:
        raise AuditError("input changed while auditing")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True, type=Path)
    parser.add_argument("--map", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    try:
        result = audit(args.exe, args.map)
    except (OSError, ValueError, struct.error) as error:
        parser.exit(1, f"size audit failed: {error}\n")
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"Executable: {result['executable']['bytes']:,} bytes")
    for section in result["pe"]["sections"]:
        print(f"  {section['name']}: file {section['file_bytes']:,}; virtual {section['virtual_bytes']:,}")
    print("Exact unwind-code attribution:")
    for name, size in list(result["attribution"]["groups"].items())[:12]:
        print(f"  {name}: {size:,} bytes")
    print(f"Report: {args.out}")


if __name__ == "__main__":
    main()
