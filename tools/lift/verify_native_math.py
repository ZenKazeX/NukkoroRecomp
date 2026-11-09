"""Verify recovered native math contract inputs and software-body removal.

Each DOL body hash pins the complete original implementation, not just its signature.
Direct entry edges and data pointers are checked; conservative computed-address
reachability is the generator's responsibility, checked through its pruning output.
This proves the stated static boundary, not every possible runtime pointer value.
"""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import re
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from graphics_lift_inventory import Dol

CONTRACTS = (
    {"name": "scalar square root", "start": 0x803B2D2C, "end": 0x803B2F50,
     "hash": "fd6cf7f6738e84f9b270ff49d03306802527bf3e1414b720c1c24c0235d0b005",
     "edges": [(0x803B2FA4, 0x803B2D2C, 0x4BFFFD89)],
     "api": "square_root", "call": "shadow::math::square_root(bits_to_double(c.fpr[1].bits[0]))"},
    {"name": "affine matrix copy", "start": 0x8040CD04, "end": 0x8040CD24,
     "hash": "949be76368af4f56469cd79b3c4ca0f058309b6098a4bc4649b7d31b069a4faa",
     "edges": [(0x8040CB44, 0x8040CD04, 0x480001C1), (0x8040D004, 0x8040CD04, 0x4BFFFD01)],
     "api": "copy_affine_matrix", "call": "shadow::math::copy_affine_matrix(destination, source)"},
    {"name": "affine matrix copy duplicate", "start": 0x8040D198, "end": 0x8040D1B8,
     "hash": "949be76368af4f56469cd79b3c4ca0f058309b6098a4bc4649b7d31b069a4faa",
     "edges_hash": "7a40bd50b71c34d1d3963dd7b65af59e35b30ed27fc97146db5a7058ae1b0c75",
     "edge_count": 683, "identical_to": 0x8040CD04,
     "api": "copy_affine_matrix", "call": "shadow::math::copy_affine_matrix(destination, source)"},
    {"name": "affine matrix composition", "start": 0x8040CB14, "end": 0x8040CD04,
     "hash": "c18e10dde4c1a98a61a1da12e39ae0747581bd77e3c3d8cc1902b5de355d837e",
     "edges_hash": "0cd771fe2be20cf6fd6d5f047ec5c2a6583dc5a07e754d0e6f315152d7afa6f2",
     "edge_count": 68, "range_key": 0x800076F4,
     "api": "compose_affine_matrix", "call": "shadow::math::compose_affine_matrix(values + li, values + ri,"},
    # The game's MSL math, every body retired by the msl_* lowered sites (docs/NATIVE_MSL_MATH.md).
    {"name": 'MSL sin', "start": 0x803B2AFC, "end": 0x803B2BD4,
     "hash": "710ee62179e718c3a92975f639bd828986aab5b55017f89961512cb2804a7977",
     "edges_hash": "6cfb68d65ab56f2c65e8ae8e16836fae56052d9adb5103f1ab8507b08a78bfee", "edge_count": 6,
     "api": 'sin@msl', "call": 'shadow::math::msl::sin(x)'},
    {"name": 'MSL cos', "start": 0x803B2594, "end": 0x803B2668,
     "hash": "94eca31d9de42bc6361e690420ae8d2807ba910a4230842e20ce1d9c04cb6f93",
     "edges_hash": "0b99b17db39c3ff0966979acf26d90baa370a3baa556b04f1c7a2eba215efc2e", "edge_count": 10,
     "api": 'cos@msl', "call": 'shadow::math::msl::cos(x)'},
    {"name": 'MSL tan', "start": 0x803B2BD4, "end": 0x803B2C4C,
     "hash": "d9bfe7043d0621888cc0694350a376da7c4bb82d11e9aa4bfd786904ae48fef0",
     "edges_hash": "9539c705f5cdf96554b9d4ac596a98bf8e45e1240a6dc4508ecd7604b9d7e76f", "edge_count": 1,
     "api": 'tan@msl', "call": 'shadow::math::msl::tan(x)'},
    {"name": 'MSL atan', "start": 0x803B2210, "end": 0x803B2428,
     "hash": "70137057ffca4bbe53c05d6aa82f22fb8a3a76bc223268b1d17c82734b02218d",
     "edges_hash": "64ff1b8ba96fa5c37ce5972d6c995f72edf18837dc6d65ed599083f4a9260840", "edge_count": 3,
     "api": 'atan@msl', "call": 'shadow::math::msl::atan(x)'},
    {"name": 'MSL __ieee754_atan2', "start": 0x803AFAEC, "end": 0x803AFD7C,
     "hash": "3fec6e704292101f356a0590182d87f4022010b7c94744c062935397b8cb3024",
     "edges_hash": "ef7923be382725d1b237b74156208a68c9dc5ed09b6dc1f1cc5523e06fed3a3f", "edge_count": 1,
     "api": 'atan2@msl', "call": 'shadow::math::msl::atan2(x, '},
    {"name": 'MSL atan2', "start": 0x803B2C8C, "end": 0x803B2CAC,
     "hash": "91439d22338127f479a90383fa03f4646f7eaa26c3be50523331ca2ac631c528",
     "edges_hash": "e11f72e279f2994637c11072dbbf5e3cba9fc4a4dfc46d3b0c459df2e0a503b7", "edge_count": 2,
     "api": 'atan2@msl', "call": 'shadow::math::msl::atan2(x, '},
    {"name": 'MSL __ieee754_acos', "start": 0x803AF678, "end": 0x803AF8B4,
     "hash": "b1d078aa5bf648ba3fae1cafaf814b2a064524abdd7c1f49fbf5f1837151cf14",
     "edges_hash": "abd65e1ea0154c2b567e4ea70124d8d55fc675a8f24d4ec69d755a11b6e02e94", "edge_count": 1,
     "api": 'acos@msl', "call": 'shadow::math::msl::acos(x)'},
    {"name": 'MSL acos', "start": 0x803B2C4C, "end": 0x803B2C6C,
     "hash": "eca8b35904ca1b70808912fb8aa540e2e903e4ea5ef86977374957967f030b30",
     "edges_hash": "27153cb994b204348cc3b334902f8c46461249bc2d71736036fbe23f5af297e8", "edge_count": 2,
     "api": 'acos@msl', "call": 'shadow::math::msl::acos(x)'},
    {"name": 'MSL __ieee754_rem_pio2', "start": 0x803B0C74, "end": 0x803B1014,
     "hash": "70f0dabdf8c53a0f9dc5b88b4ec3e09991eade9c887da72d66591086b871b2c1",
     "edges_hash": "56f3f8d39da205a900f032871521799aaf4163419645cc2afdee995f139f1be2", "edge_count": 3,
     "api": 'sin@msl', "call": 'shadow::math::msl::sin(x)'},
    {"name": 'MSL __kernel_cos', "start": 0x803B1014, "end": 0x803B1108,
     "hash": "6d8375bb500f13335c20fe43ec6498bc96144229784e5c5b8bee7eeb2fe2bbc6",
     "edges_hash": "fc0921c90943b95ebdc643767f64c95de9469d72f25e095b3a73f5136d458fd8", "edge_count": 5,
     "api": 'cos@msl', "call": 'shadow::math::msl::cos(x)'},
    {"name": 'MSL __kernel_rem_pio2', "start": 0x803B1108, "end": 0x803B1F5C,
     "hash": "20b5878b8bf48369ec1055062d9bf96856a3578a8aec62e90368eb0bb95b6432",
     "edges_hash": "6334d7d29bf6235dce7091656d7f21cc8072b34e4e4a9779edab999149ce87fb", "edge_count": 1,
     "api": 'sin@msl', "call": 'shadow::math::msl::sin(x)'},
    {"name": 'MSL __kernel_sin', "start": 0x803B1F5C, "end": 0x803B1FFC,
     "hash": "a1e452eea68ca205d3f41fd97a592059e7f7ae78513cc7cbbae4c4208892aaa4",
     "edges_hash": "506fd86d8333bb8663d0dbae7643b84f6aaf0a5d5310afbe4f9b23b6ba83ba88", "edge_count": 5,
     "api": 'sin@msl', "call": 'shadow::math::msl::sin(x)'},
    {"name": 'MSL __kernel_tan', "start": 0x803B1FFC, "end": 0x803B2210,
     "hash": "e81636b24498d1fbd42aa944cb3dc8944ad3abdebffa6720b363faadf1ae1a94",
     "edges_hash": "2f64efd60e5de5322f856a3a796fbef350253b37b4ce4ac9b22e833a514c854b", "edge_count": 2,
     "api": 'tan@msl', "call": 'shadow::math::msl::tan(x)'},
    {"name": 'MSL sinf', "start": 0x8000FFE8, "end": 0x8001000C,
     "hash": "c6fa8ad82386366786c5b770284b168ecbe3ccc51572cc8c51cd530f82093015",
     "edges_hash": "96c526a1bcc5e556de075cee61a1c69edb20dd548c94bad9fcd140d2cf36af1a", "edge_count": 57,
     "api": 'sin@msl', "call": 'shadow::math::msl::sin(x)'},
    {"name": 'MSL cosf', "start": 0x8000F3B0, "end": 0x8000F3D4,
     "hash": "1bac954eaa615843a166658e4977404972c89a82656da7c0d13ce980152837bb",
     "edges_hash": "eaeaf9b2a6ce802bd19fe0a5dc9d633ef9f2fd4b65f3e708781795e11246e4d2", "edge_count": 75,
     "api": 'cos@msl', "call": 'shadow::math::msl::cos(x)'},
    {"name": 'MSL cosf (second)', "start": 0x803B2F74, "end": 0x803B2F98,
     "hash": "834f7ed67fa2ee2db38a1f5982754e11ff51ede8ea2f577c0a13f03d9af41313",
     "edges_hash": "cb9f364de48732028f89551499c30602e1979c1b7c758fa61a400ebc81a3352d", "edge_count": 1,
     "api": 'cos@msl', "call": 'shadow::math::msl::cos(x)'},
    {"name": 'MSL tanf', "start": 0x8000F3D4, "end": 0x8000F3F8,
     "hash": "abb98eee4981c3d9e8190734ee228810ec8135548f51d4b83dd9fa0b0fadba11",
     "edges_hash": "7c5c635f490b890fc576fa3b98c6e7eeac9f9d0fafe5efa5f322e85ff4d18766", "edge_count": 14,
     "api": 'tan@msl', "call": 'shadow::math::msl::tan(x)'},
    {"name": 'MSL atanf', "start": 0x8000F3F8, "end": 0x8000F41C,
     "hash": "7719466a0554f81bc23f064dc36ac0e2faa8dd4eac545d1db737258364fc00fc",
     "edges_hash": "937a6ce1cf262ad1cd7dbaa99d2a745e2d920f32326491436c775eea2724aa4d", "edge_count": 14,
     "api": 'atan@msl', "call": 'shadow::math::msl::atan(x)'},
    {"name": 'MSL atan2f', "start": 0x80016260, "end": 0x80016284,
     "hash": "aa22108e9a412cd16d0e59d83fbb43019632b996d8b8f7d154fd8ae10f3909aa",
     "edges_hash": "58107a07410581d3e893db4c9a36fb4b8a4fb806446d2b955f3da56cdf6f09bc", "edge_count": 81,
     "api": 'atan2@msl', "call": 'shadow::math::msl::atan2(x, '},
    {"name": 'MSL acosf', "start": 0x801B2150, "end": 0x801B2174,
     "hash": "59585a649e5b13a1277e4dcff5916529113188776b5a7be4eb015e8a6772d8d3",
     "edges_hash": "acb0971ca86b410e49daa526df7ab5a27f913f0dd886a1f72b99fddd143ffed6", "edge_count": 6,
     "api": 'acos@msl', "call": 'shadow::math::msl::acos(x)'},
    {"name": 'MSL sincos', "start": 0x80031568, "end": 0x800315C4,
     "hash": "73b5efb4aea0e4d8912504dc2b7c0201f217e68b70f63a4d0760aa94c0f5a453",
     "edges_hash": "23c1bfff91775ebbfb98da681676610f306220ca42e9170e3804f50dce9b24f0", "edge_count": 12,
     "api": 'sin@msl', "call": 'shadow::math::msl::sin(x)'},
)


def verify_matrix_index(image):
    """Pin the recovered consumer, registration, index bounds and empty cleanup.

    The independent C++ oracle executes registration and lookup. This exemption
    concerns executable roots only; neither the DOL nor the index is rewritten.
    """
    for start, end, digest in (
        (0x803A2520, 0x803A255C, "60024deb4c93ad487be5349858cff7c401e1f958ecccbca6a6cc2b6ad75437e2"),
        (0x803A3954, 0x803A3988, "009a215c0fc2d003891c0577c3c715ef857898ffc2645aaad589b682b15eda1e"),
        (0x803A371C, 0x803A3920, "6fe3808c941abbe735fdcc761d9280b2fc7c3fd9cbf711352d30dd0c74276c0d"),
    ):
        offset = image.offset(start)
        if hashlib.sha256(image.raw[offset:offset + end - start]).hexdigest() != digest:
            raise ValueError("exception index consumer changed")
    def words(address, count):
        for section in image.sections:
            if section["address"] <= address and address + count * 4 <= section["address"] + section["size"]:
                return struct.unpack_from(f">{count}I", image.raw,
                                          section["file_offset"] + address - section["address"])
        raise ValueError("exception index data is missing")
    for address, expected in (
        (0x80008D14, (0x80007520, 0x80008D14, 0x803A1258, 0x9A874, 0, 0, 0, 0)),
        (0x800076F4, (0x8040CB14, 0x1F0, 0x800057D4)),
        (0x800057D4, (0x108A0000, 0)),
    ):
        if words(address, len(expected)) != expected:
            raise ValueError("exception index range/descriptor changed")
    records = [words(address, 3) for address in range(0x80007520, 0x80008D14, 12)]
    if any(a[0] >= b[0] for a, b in zip(records, records[1:])):
        raise ValueError("exception index is no longer sorted")


def identical_text_spans(image, start, end):
    """Inventory exact aligned text matches, not automatic semantic replacements.

    PC-relative external targets or link-address materialization can make equal
    bytes behave differently. Only explicitly recovered contracts are promoted.
    """
    if end <= start or (start | end) & 3 or not image.contains_text(start, end - start):
        raise ValueError("invalid original text span")
    offset = image.offset(start)
    body = image.raw[offset:offset + end - start]
    matches = []
    for section in image.sections:
        if section["kind"] != "text": continue
        first = section["file_offset"]
        stop = first + section["size"]
        at = image.raw.find(body, first, stop)
        while at != -1:
            address = section["address"] + at - first
            if address % 4 == 0: matches.append(address)
            at = image.raw.find(body, at + 1, stop)
    return sorted(matches)


def verify_contract(image, contract, generated=None, linker_map=None):
    start, end = contract["start"], contract["end"]
    def has_retired_entry(text):
        return any(start <= int(match[1], 16) < end for match in
                   re.finditer(r"\bfn_([0-9a-fA-F]{8})(?:_|@|\b)", text))
    offset = image.offset(start)
    if hashlib.sha256(image.raw[offset:offset + end - start]).hexdigest() != contract["hash"]:
        raise ValueError(f"original {contract['name']} implementation changed")
    if "identical_to" in contract:
        other = image.offset(contract["identical_to"])
        if image.raw[offset:offset + end - start] != image.raw[other:other + end - start]:
            raise ValueError("pinned copy-family bodies are not byte-identical")
    incoming = []
    range_keys = []
    for section in image.sections:
        for at in range(0, section["size"] - 3, 4):
            address = section["address"] + at
            word, = struct.unpack_from(">I", image.raw, section["file_offset"] + at)
            if section["kind"] == "data":
                if start <= word < end:
                    if address != contract.get("range_key") or word != start:
                        raise ValueError(f"data reference to retired math code at {address:08X}")
                    verify_matrix_index(image)
                    range_keys.append(address)
                continue
            primary = word >> 26
            if primary not in (16, 18):
                continue
            mask, sign = (0xfffc, 0x8000) if primary == 16 else (0x3fffffc, 0x2000000)
            delta = word & mask
            if delta & sign:
                delta -= sign * 2
            target = (delta if word & 2 else address + delta) & 0xffffffff
            if start <= target < end and not start <= address < end:
                incoming.append((address, target, word))
    if "edges" in contract:
        edges_match = incoming == contract["edges"]
    else:
        encoded = b"".join(struct.pack(">III", *edge) for edge in incoming)
        edges_match = len(incoming) == contract["edge_count"] and hashlib.sha256(encoded).hexdigest() == contract["edges_hash"]
    if not edges_match:
        raise ValueError(f"unexpected {contract['name']} entry edges: {incoming}")
    if "range_key" in contract and range_keys != [contract["range_key"]]:
        raise ValueError("pinned exception range key is missing")
    if generated is not None:
        with (generated / "recomp_pruned.csv").open(newline="") as stream:
            removed = {int(row["address"], 16) for row in csv.DictReader(stream)}
        if start not in removed:
            raise ValueError(f"software {contract['name']} was not pruned")
        decls = (generated / "recomp_decls.h").read_text()
        table = (generated / "recomp_table.cpp").read_text()
        if has_retired_entry(decls) or has_retired_entry(table):
            raise ValueError(f"retired {contract['name']} still has a generated entry")
        if contract["call"] not in table:
            raise ValueError("native value API not called by generated code")
    if linker_map is not None:
        native = False
        with linker_map.open(encoding="latin-1") as stream:
            for line in stream:
                if has_retired_entry(line):
                    raise ValueError(f"retired {contract['name']} remains linked")
                native |= "?" + contract["api"] + "@math@shadow@@" in line
        if not native:
            raise ValueError(f"native {contract['name']} implementation is absent from linker map")
    print(f"PASS {contract['name']}: pinned original body, all direct callers, " +
          ("only the verified exception range key" if range_keys else "no data pointers") +
          (", original body pruned" if generated else "") +
          (", native API linked and original symbol absent" if linker_map else ""))

    return {"name": contract["name"], "start": start, "end": end,
            "body_sha256": contract["hash"], "entry_edges": incoming,
            "exact_text_matches": identical_text_spans(image, start, end),
            "scope": "Pinned contract and exact byte matches; byte identity alone does not establish relocation equivalence."}


def verify(dol_path, generated=None, linker_map=None):
    image = Dol(dol_path.read_bytes())
    return [verify_contract(image, contract, generated, linker_map) for contract in CONTRACTS]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dol", type=Path, required=True)
    parser.add_argument("--generated", type=Path)
    parser.add_argument("--map", type=Path)
    parser.add_argument("--inventory", type=Path, help="write pinned bodies, entry edges and exact-text duplicate candidates")
    args = parser.parse_args()
    try:
        inventory = verify(args.dol, args.generated, args.map)
        if args.inventory:
            args.inventory.write_text(json.dumps({"dol_sha256": hashlib.sha256(args.dol.read_bytes()).hexdigest(),
                "tool_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(), "contracts": inventory,
                "generated": str(args.generated) if args.generated else None,
                "map_sha256": hashlib.sha256(args.map.read_bytes()).hexdigest() if args.map else None}, indent=2) + "\n")
    except (OSError, ValueError, struct.error) as error:
        parser.exit(1, f"native math evidence failed: {error}\n")


if __name__ == "__main__":
    main()
