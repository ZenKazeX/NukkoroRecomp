"""PlayerShadow's field accesses as the binary makes them: the evidence the typed view (runtime/include/gekko/
player_view.h) is checked against.

    python -B -m dolrecon.player_layout <dolrecon root> <out csv>

For every method of Player::PlayerShadow and of its flattened RTTI bases (skeleton class headers; `this` = r3 at entry,
followed through copies by dolrecon.vcalls' must-analysis), every D-form load / store whose base register holds `this`
on all paths: offset, width, kind (int / float single / float double / paired single), direction, and the sites.
Offsets reached with more than one width or kind, or covered by two different accesses that overlap, are reported as
aliasing: the view must preserve them as the same bytes, never as two members.
"""
from __future__ import annotations

import collections
import csv
import pathlib
import re
import sys

from dolrecon import dol, game_map, ppc, vcalls

# primary opcode -> (width, kind, direction); psq_l / psq_st width depends on the GQR (recorded as 8, kind "ps")
DFORM = {
    32: (4, "int", "load"), 33: (4, "int", "load"), 34: (1, "int", "load"), 35: (1, "int", "load"),
    40: (2, "int", "load"), 41: (2, "int", "load"), 42: (2, "sint", "load"), 43: (2, "sint", "load"),
    36: (4, "int", "store"), 37: (4, "int", "store"), 38: (1, "int", "store"), 39: (1, "int", "store"),
    44: (2, "int", "store"), 45: (2, "int", "store"),
    48: (4, "f32", "load"), 49: (4, "f32", "load"), 50: (8, "f64", "load"), 51: (8, "f64", "load"),
    52: (4, "f32", "store"), 53: (4, "f32", "store"), 54: (8, "f64", "store"), 55: (8, "f64", "store"),
    46: (None, "lmw", "load"), 47: (None, "stmw", "store"),
}
PSQ = {56: "load", 57: "load", 60: "store", 61: "store"}
CLASS = "Player::PlayerShadow"


def player_methods(root: pathlib.Path):
    fn = game_map.load_functions(root)
    owners = game_map.load_owners(root / "gameplay_scope/owners.json")
    header = (root / "skeleton/classes/Player/PlayerShadow.h").read_text(encoding="utf-8")
    bases = [b.split(" +")[0].strip() for b in re.search(r"base subobjects \(RTTI, flattened\): (.*)", header)
             .group(1).split(", ")]
    classes = {CLASS, *bases}
    methods = {}
    for a, r in fn.items():
        cls = owners.get(a) or (r["class"].split(" | ")[0] if r["class"] else "")
        if cls in classes and r["layer"] in game_map.GAME_LAYERS:
            methods[a] = (r["name"], cls, int(r["size"]))
    return methods, classes


def scan(root: pathlib.Path):
    image = dol.load_file(str(root / "pal-v30/main.dol"))
    methods, classes = player_methods(root)
    access = collections.defaultdict(lambda: collections.defaultdict(list))   # offset -> (width, kind, dir) -> sites
    for a, (name, cls, size) in sorted(methods.items()):
        f = vcalls.Function(image, a, size)

        def visit(pc, w, regs, consts):
            op, d, ra, _, imm = ppc.fields(w)
            if ra not in regs or ra == 0:
                return
            if op in DFORM:
                width, kind, direction = DFORM[op]
                offset = ppc.simm(imm)
                if width is None:   # lmw / stmw: registers d..31, one word each
                    width = 4 * (32 - d)
                access[offset][(width, kind, direction)].append(pc)
            elif op in PSQ:
                offset = imm & 0xFFF
                offset = offset - 0x1000 if offset & 0x800 else offset
                access[offset][(8, "ps", PSQ[op])].append(pc)

        f.scan({3}, visit)
    return access, methods, classes


def main(argv=None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    root, out = pathlib.Path(argv[0]), pathlib.Path(argv[1])
    access, methods, classes = scan(root)
    spans = sorted((o, o + w) for o, kinds in access.items() for (w, _, _) in kinds)
    rows, aliased = [], 0
    for offset in sorted(access):
        kinds = access[offset]
        shapes = {(w, k.replace("sint", "int")) for (w, k, _) in kinds}
        widths = {w for (w, _, _) in kinds}
        overlap = any(s < offset + max(widths) and offset < e and (s, e) != (offset, offset + w)
                      for (s, e) in spans for w in widths if (s, e) != (offset, offset + w))
        alias = len(shapes) > 1 or overlap
        aliased += alias
        for (w, k, direction), sites in sorted(kinds.items()):
            rows.append({"offset": f"0x{offset:X}" if offset >= 0 else f"-0x{-offset:X}", "width": w, "kind": k,
                         "direction": direction, "sites": len(sites), "aliasing": int(alias),
                         "first_sites": " ".join(f"0x{s:08X}" for s in sorted(sites)[:4])})
    out.parent.mkdir(parents=True, exist_ok=True)
    with open(out, "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0]), lineterminator="\n")
        w.writeheader()
        w.writerows(rows)
    print(f"{len(methods)} methods of {len(classes)} classes ({', '.join(sorted(classes))}); {len(access)} offsets, "
          f"{sum(len(k) for k in access.values())} (offset, width, kind, direction) shapes, {aliased} offsets aliased; "
          f"highest offset 0x{max(access):X}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
