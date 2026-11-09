"""Learn from the functions already identified and owned: fingerprint every SDK function's instruction structure, then
rank the SDK bodies still compiled by the template they share with owned functions.

    python -B structure_match.py --manifest <platform_manifest.json> --graph <live_graph.txt>
                                 [--dol ../game/sys/main.dol] [--functions extract/functions.csv]
                                 [--owners extract/native-owners.txt] [--out DIR] [--validate]

Fingerprint (per function, from the DOL words, no execution): the set of 3-grams of normalized instruction tokens
(opcode + extended opcode; registers and immediates dropped, branches folded to b / bl / bc / blr / bctr), plus
semantic tokens: calls to named functions (e.g. call:OSDisableInterrupts), small-data accesses (r13 / r2 based loads
and stores), MMIO bases (lis 0xCC00 / 0xCC01 / 0xCD00), the GX FIFO port (0xCC008000), leaf / size bucket.
Similarity = Jaccard of the token sets.

Templates are rules over those features (a getter that only reads a global, a setter swapping a global with
interrupts off, a GX FIFO writer, a thin wrapper, ...): they say what kind of owner a body needs, not that it is safe
to own (every owner still goes through the gates of docs/GC_REMOVAL_PLAN.md).

--validate: leave-one-out over SDK functions with a known service. The structural nearest neighbour predicts the
service; it is reported next to a baseline that predicts the service of the nearest function by ADDRESS (SDK
libraries are contiguous, so that baseline is strong). The structural model is only worth using where it beats it.

Writes <out>/structure_match.json and <out>/STRUCTURE_MATCH.md.
"""
from __future__ import annotations

import argparse
import bisect
import collections
import csv
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(SRC, "tools"))
from dolrecon import dol as dolmod  # noqa: E402

UNKNOWN = "sdk:unidentified"


# ---- decoding -------------------------------------------------------------------------------------------------------

def token(w: int) -> str:
    op = w >> 26
    if op == 18:
        return "bl" if w & 1 else "b"
    if op == 16:
        return "bcl" if w & 1 else "bc"
    if op == 19:
        xo = (w >> 1) & 0x3FF
        if xo == 16:
            return "blrl" if w & 1 else ("blr" if ((w >> 21) & 0x14) == 0x14 else "bclr")
        if xo == 528:
            return "bctrl" if w & 1 else "bctr"
        return f"19.{xo}"
    if op == 31:
        return f"31.{(w >> 1) & 0x3FF}"
    if op in (59, 63):
        xo5 = (w >> 1) & 0x1F
        return f"{op}.{xo5}" if xo5 >= 18 else f"{op}.{(w >> 1) & 0x3FF}"
    if op == 4:
        xo6 = (w >> 1) & 0x3F
        return f"4.{xo6}" if xo6 in (6, 7, 38, 39) else f"4.{(w >> 1) & 0x1F}"
    return str(op)


LOADS = {32, 33, 34, 35, 40, 41, 42, 43, 46, 48, 49, 50, 51, 56, 57}
STORES = {36, 37, 38, 39, 44, 45, 47, 52, 53, 54, 55, 60, 61}


def features(image, start: int, size: int, names: dict) -> tuple[set, dict]:
    toks, sem = [], set()
    calls, n = [], 0
    lis_hi = {}
    for at in range(start, start + size, 4):
        try:
            w = image.word(at)
        except Exception:
            break
        n += 1
        t = token(w)
        toks.append(t)
        op, d, a = w >> 26, (w >> 21) & 31, (w >> 16) & 31
        imm = w & 0xFFFF
        if op == 18 and w & 1:
            li = w & 0x03FFFFFC
            if li & 0x02000000:
                li -= 0x04000000
            target = (at + li) & 0xFFFFFFFF
            calls.append(target)
            nm = names.get(target)
            if nm and not nm.startswith("fn_"):
                sem.add(f"call:{nm}")
        if op == 15 and a == 0:                                    # lis rD, imm
            lis_hi[d] = imm
            if imm in (0xCC00, 0xCC01, 0xCD00, 0xCD01, 0xCD80):
                sem.add(f"mmio:{imm:04X}")
        if op in (14, 24) and a in lis_hi:                         # addi / ori on a lis base
            full = ((lis_hi[a] << 16) + (imm - 0x10000 if (op == 14 and imm & 0x8000) else imm)) & 0xFFFFFFFF
            if full == 0xCC008000:
                sem.add("gx-fifo")
        if op in LOADS | STORES and a in (2, 13):
            sem.add(("sda-load" if op in LOADS else "sda-store") + ("" if a == 13 else "-r2"))
        if op in STORES and a in lis_hi and lis_hi[a] in (0xCC00, 0xCC01, 0xCD00):
            sem.add("mmio-store")
        if op in (16, 18) or t in ("blr", "bctr", "bctrl", "blrl"):
            lis_hi.clear()                                         # straight-line constant tracking only
    grams = {"|".join(toks[i:i + 3]) for i in range(max(0, len(toks) - 2))} or set(toks)
    sem.add("leaf" if not calls else f"calls:{min(len(calls), 4)}")
    sem.add(f"size:{min(n // 8, 8)}")
    fp = grams | sem
    info = {"ins": n, "calls": calls, "sem": sorted(sem)}
    return fp, info


def template(info: dict, names: dict) -> str:
    sem, n, calls = set(info["sem"]), info["ins"], info["calls"]
    callnames = {names.get(c, "") for c in calls}
    if "gx-fifo" in sem:
        return "gx-fifo-writer"
    if "mmio-store" in sem or any(s.startswith("mmio:") for s in sem):
        return "mmio-register-access"
    if {"OSDisableInterrupts", "OSRestoreInterrupts"} <= callnames and len(calls) <= 3:
        return "interrupt-bracketed-update"
    if not calls and n <= 8 and "sda-store" not in sem and ("sda-load" in sem or "sda-load-r2" in sem):
        return "leaf-getter"
    if not calls and n <= 12 and "sda-store" in sem:
        return "leaf-setter"
    if len(calls) == 1 and n <= 12:
        return "thin-wrapper"
    if not calls:
        return "leaf-compute"
    return "general"


# ---- inputs ---------------------------------------------------------------------------------------------------------

def load_graph(path):
    rows = {}
    for line in open(path, encoding="utf-8"):
        p = line.split()
        if len(p) >= 5 and p[0] == "row":
            rows[int(p[1], 16)] = (int(p[2]), p[3], p[4])
    return rows


def load_owners(path):
    owned = {}
    for line in open(path, encoding="utf-8"):
        s = line.strip()
        if s and not s.startswith("#"):
            parts = s.split(None, 1)
            owned[int(parts[0], 16)] = parts[1] if len(parts) > 1 else ""
    return owned


def jaccard(a: set, b: set) -> float:
    if not a and not b:
        return 0.0
    return len(a & b) / len(a | b)


# ---- main -----------------------------------------------------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--manifest", required=True)
    ap.add_argument("--graph", required=True)
    ap.add_argument("--dol", default=os.path.join(SRC, "..", "game", "sys", "main.dol"))
    ap.add_argument("--functions", default=os.path.join(SRC, "extract", "functions.csv"))
    ap.add_argument("--owners", default=os.path.join(SRC, "extract", "native-owners.txt"))
    ap.add_argument("--out", default=os.path.join(SRC, ".build", "platform", "structure_match"))
    ap.add_argument("--validate", action="store_true")
    ap.add_argument("--plan", help="removal_plan.json: its services decide what counts as platform code (all layers)")
    args = ap.parse_args(argv)

    image = dolmod.load_file(args.dol)
    sizes = {}
    for r in csv.DictReader(open(args.functions, encoding="utf-8")):
        sizes[int(r["addr"])] = int(r["size"])
    manifest = json.load(open(args.manifest, encoding="utf-8"))
    platform_services = None
    if args.plan:
        platform_services = set(json.load(open(args.plan, encoding="utf-8"))["services"])
    # Platform code = the removal plan's services (any layer: the RW GameCube driver is not layer "sdk"); without a
    # plan, the manifest's sdk layer.
    # Units: manifest FUNCTIONS (the plan counts generator ROWS; a split function is several rows, so the plan's body
    # counts are larger). The sdk layer is always included: a newer manifest renames services the plan predates.
    entries = [e for e in manifest["entries"]
               if e.get("layer") == "sdk" or (platform_services is not None and e.get("service") in platform_services)]
    names = {int(e["address"], 16): e["name"] for e in manifest["entries"]}
    graph = load_graph(args.graph)
    owned = load_owners(args.owners)

    fps, infos, svc = {}, {}, {}
    for e in entries:
        a = int(e["address"], 16)
        size = sizes.get(a) or (graph.get(a, (0,))[0])
        if not size:
            continue
        fps[a], infos[a] = features(image, a, size, names)
        svc[a] = e["service"]

    # Owned = in the owner list (or an owner row of the live graph); remaining = SDK bodies the build still compiles.
    owned_set = {a for a in fps if a in owned or graph.get(a, (0, "", ""))[1] == "owner"}
    remaining = sorted(a for a in fps if graph.get(a, (0, "", ""))[1:] == ("body", "live"))

    out = {"inputs": {k: os.path.abspath(v) for k, v in vars(args).items() if isinstance(v, str)},
           "sdk_functions": len(fps), "owned": len(owned_set), "remaining": len(remaining)}

    if args.validate:
        known = [a for a in fps if svc[a] != UNKNOWN]
        addr_sorted = sorted(known)
        hit_struct = hit_addr = 0
        per = collections.Counter()
        per_ok = collections.Counter()
        for a in known:
            best, bs = None, -1.0
            for b in known:
                if b == a:
                    continue
                s = jaccard(fps[a], fps[b])
                if s > bs:
                    best, bs = b, s
            i = bisect.bisect_left(addr_sorted, a)
            nb = [x for x in (addr_sorted[i - 1] if i > 0 else None,
                              addr_sorted[i + 1] if i + 1 < len(addr_sorted) else None) if x is not None]
            near = min(nb, key=lambda x: abs(x - a))
            per[svc[a]] += 1
            if svc[best] == svc[a]:
                hit_struct += 1
                per_ok[svc[a]] += 1
            if svc[near] == svc[a]:
                hit_addr += 1
        # Template validation: how the already-owned functions distribute over the templates, per owner section of
        # the owner list (a template is useful when owners of one kind share it).
        owned_templates = collections.Counter(template(infos[a], names) for a in owned_set)
        out["owned_templates"] = dict(owned_templates.most_common())
        # Owner grouping: among owned functions, does the structural neighbour belong to the same runtime owner file?
        owner_file, cur = {}, None
        for line in open(args.owners, encoding="utf-8"):
            s = line.strip()
            if s.startswith("#") and "runtime/" in s and (".cpp" in s or ".h" in s):
                cur = s.split("runtime/", 1)[1].split()[0]
            elif s and not s.startswith("#"):
                owner_file[int(s.split()[0], 16)] = cur
        og = sorted(a for a in owner_file if owner_file[a] and a in fps)
        same_s = same_a = 0
        for a in og:
            b = max((x for x in og if x != a), key=lambda x: jaccard(fps[a], fps[x]))
            i = bisect.bisect_left(og, a)
            n = min((og[j] for j in (i - 1, i + 1) if 0 <= j < len(og)), key=lambda x: abs(x - a))
            same_s += owner_file[b] == owner_file[a]
            same_a += owner_file[n] == owner_file[a]
        out["owner_grouping"] = {"owned": len(og), "structural_nn_same_owner": round(same_s / max(1, len(og)), 4),
                                 "address_nn_same_owner": round(same_a / max(1, len(og)), 4)}
        out["validation"] = {
            "known": len(known),
            "structural_nn_accuracy": round(hit_struct / len(known), 4),
            "address_nn_baseline": round(hit_addr / len(known), 4),
            "per_service_structural": {s: f"{per_ok[s]}/{per[s]}" for s in sorted(per)},
        }

    # Rank the remaining bodies: template, nearest OWNED function by structure.
    owned_list = sorted(owned_set)
    ranked = []
    for a in remaining:
        best, bs = None, 0.0
        for b in owned_list:
            s = jaccard(fps[a], fps[b])
            if s > bs:
                best, bs = b, s
        ranked.append({
            "address": f"0x{a:08X}", "name": names.get(a, ""), "service": svc[a],
            "template": template(infos[a], names), "ins": infos[a]["ins"],
            "nearest_owned": (f"0x{best:08X} {names.get(best, '')}" if best is not None else None),
            "similarity": round(bs, 3),
        })
    ranked.sort(key=lambda r: (-r["similarity"], r["ins"]))
    out["templates"] = dict(collections.Counter(r["template"] for r in ranked).most_common())
    # The defensible use (validation: structure does NOT beat address locality at naming or grouping): bodies that
    # are an exact structural twin of an owned function, or trivial (an empty body). Each still needs its gates.
    for r in ranked:
        if r["ins"] == 1:
            r["mechanical"] = "empty body (blr only)"
        elif r["similarity"] >= 0.9 and r["template"] in ("leaf-getter", "leaf-setter", "thin-wrapper",
                                                          "interrupt-bracketed-update"):
            r["mechanical"] = f"structural twin of owned {r['nearest_owned']}"
    out["mechanical"] = [r for r in ranked if "mechanical" in r]
    out["ranked"] = ranked

    os.makedirs(args.out, exist_ok=True)
    with open(os.path.join(args.out, "structure_match.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(out, f, indent=1)
    lines = ["# Structure match: remaining SDK bodies vs owned functions", "",
             f"Platform functions fingerprinted: {out['sdk_functions']}; owned: {out['owned']}; still compiled: "
             f"{out['remaining']} (functions; the removal plan counts generator rows, so its numbers are larger).", "",
             "Use: the template says what KIND of owner a body needs and the nearest owned function shows an "
             "existing owner of that kind to follow. Do not use the structural neighbour to name a function or pick "
             "its service: the validation below shows address locality is far better at that.", ""]
    if "validation" in out:
        v = out["validation"]
        lines += [f"Leave-one-out service prediction over {v['known']} known SDK functions: structural nearest neighbour "
                  f"{v['structural_nn_accuracy']:.1%}, address-neighbour baseline {v['address_nn_baseline']:.1%}.", ""]
    lines += [f"## Mechanical candidates ({len(out['mechanical'])})", "",
              "| address | name | service | why |", "|---|---|---|---|"]
    lines += [f"| {r['address']} | {r['name']} | {r['service']} | {r['mechanical']} |" for r in out["mechanical"]]
    lines += ["", "## All remaining bodies by template", ""]
    lines += ["| template | bodies |", "|---|---:|"] + [f"| {k} | {v} |" for k, v in out["templates"].items()] + [""]
    lines += ["| address | name | service | template | ins | nearest owned | sim |", "|---|---|---|---|---:|---|---:|"]
    lines += [f"| {r['address']} | {r['name']} | {r['service']} | {r['template']} | {r['ins']} | {r['nearest_owned']} "
              f"| {r['similarity']} |" for r in ranked[:200]]
    with open(os.path.join(args.out, "STRUCTURE_MATCH.md"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print(f"structure match: {out['remaining']} remaining SDK bodies, {out['owned']} owned; templates "
          f"{out['templates']}")
    if "validation" in out:
        v = out["validation"]
        print(f"validation: structural {v['structural_nn_accuracy']:.1%} vs address baseline "
              f"{v['address_nn_baseline']:.1%} over {v['known']} known")
    return 0


if __name__ == "__main__":
    sys.exit(main())
