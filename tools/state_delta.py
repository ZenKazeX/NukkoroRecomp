"""Report exact differences between v19-v23 snapshots without excluding any bytes.

Uses the comparator's maintained memory reader. Stack-pointer proximity is a
diagnostic hint, never an automatic equivalence or dead-memory decision.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent / "comparator"))
from comparator.statefile import GuestImage


def spans(before, after):
    if len(before) != len(after):
        raise ValueError("different byte lengths")
    begin = None
    for block in range(0, len(before), 4096):
        a, b = before[block:block + 4096], after[block:block + 4096]
        if a == b:
            if begin is not None:
                yield begin, block
                begin = None
            continue
        for i, (x, y) in enumerate(zip(a, b), block):
            if x != y and begin is None:
                begin = i
            elif x == y and begin is not None:
                yield begin, i
                begin = None
    if begin is not None:
        yield begin, len(before)


def describe(before, after, base=0):
    result = {"bytes": len(before), "changed_bytes": 0, "changed_ranges": 0, "ranges": []}
    for start, end in spans(before, after):
        result["changed_bytes"] += end - start
        result["changed_ranges"] += 1
        if len(result["ranges"]) < 256:
            result["ranges"].append({"start": base + start, "end": base + end,
                                     "before_hex": before[start:min(end, start + 64)].hex(),
                                     "after_hex": after[start:min(end, start + 64)].hex(),
                                     "hex_truncated": end - start > 64})
    result["range_details_truncated"] = result["changed_ranges"] > len(result["ranges"])
    return result


def describe_trailing(before, after):
    """The serialized blocks after guest memory. Versions can append fields, so lengths may differ: the common
    prefix is compared byte for byte and every byte beyond it is counted as unmatched, never skipped."""
    common = min(len(before), len(after))
    result = describe(before[:common], after[:common])
    result.update({"control_bytes": len(before), "candidate_bytes": len(after),
                   "unmatched_bytes": abs(len(before) - len(after))})
    return result


def cpu_blocks(image):
    if image.version not in (19, 20, 21, 22, 23):  # v23 changed only the trailing audio block
        raise ValueError("requires the known v19-v23 CPU layout")
    cursor, blocks = 20, []
    for _ in range(3):
        size, = struct.unpack_from("<Q", image.data, cursor)
        cursor += 8
        if cursor + size > len(image.data):
            raise ValueError("truncated CPU block")
        blocks.append(image.data[cursor:cursor + size])
        cursor += size
    if len(blocks[0]) != 4872:
        raise ValueError("unknown architected-state layout")
    return blocks


def compare(control, candidate):
    a, b = GuestImage(control), GuestImage(candidate)
    ab, bb = cpu_blocks(a), cpu_blocks(b)
    identity = lambda x: {"path": str(x.path.resolve()), "bytes": len(x.data),
                          "sha256": hashlib.sha256(x.data).hexdigest()}
    result = {"control": identity(a), "candidate": identity(b),
              "game_ticks": [a.game_ticks, b.game_ticks],
              "renderer_kind": [a.renderer_kind, b.renderer_kind],
              "stack_pointer": [struct.unpack_from("<I", block, 4)[0] for block in (ab[0], bb[0])],
              "cpu_blocks": [dict(name=name, **describe(x, y)) for name, x, y in
                             zip(("architected", "interrupt_fp", "timeline"), ab, bb)],
              "regions": [], "equivalence_proven": False}
    signature = lambda x: [(r.name, r.base, r.size, r.committed, r.kind) for r in x.regions]
    if signature(a) != signature(b):
        raise ValueError("memory-region layouts differ; address comparison would be incomplete")
    for ar, br in zip(a.regions, b.regions):
        result["regions"].append(dict(name=ar.name, base=ar.base, **describe(
            a.data[ar.offset:ar.offset + ar.committed], b.data[br.offset:br.offset + br.committed], ar.base)))
    ae = a.regions[-1].offset + a.regions[-1].committed
    be = b.regions[-1].offset + b.regions[-1].committed
    result["trailing_serialized_state"] = describe_trailing(a.data[ae:], b.data[be:])
    result["limitations"] = ["No bytes were excluded and no difference is automatically excused.",
                              "A stack pointer is context, not proof that a changed address is dead.",
                              "Range and hex details may be truncated as explicitly marked; counts cover every byte."]
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--control", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    try:
        result = compare(args.control, args.candidate)
    except (OSError, ValueError, struct.error, IndexError) as error:
        parser.exit(1, f"state delta failed: {error}\n")
    args.out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"Stack pointers: {result['stack_pointer']}")
    for item in result["cpu_blocks"] + result["regions"]:
        print(f"{item['name']}: {item['changed_bytes']} different bytes in {item['changed_ranges']} ranges")
    trailing = result["trailing_serialized_state"]
    print("Trailing serialized state:", trailing["changed_bytes"], "different bytes,",
          trailing["unmatched_bytes"], "bytes beyond the shorter file")


if __name__ == "__main__":
    main()
