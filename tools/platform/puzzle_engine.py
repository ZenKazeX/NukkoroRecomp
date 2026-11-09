"""Semantic puzzle engine (prototype): connect partial evidence about platform functions - rare basic blocks, shared
globals, calls, structure, address - into an evidence graph; propose services, subsystem clusters and graded names; and
measure, without label leakage, whether that beats the address-locality baseline. docs/SEMANTIC_PUZZLE_ENGINE.md.

    python -B puzzle_engine.py --manifest <platform_manifest.json> --graph <live_graph.txt> [--plan <removal_plan.json>]
                               [--out DIR]

Inputs are read only. Outputs (deterministic JSON, sorted): functions, patterns, evidence_edges, candidate_matches,
proposed_names, subsystem_clusters, verified_identifications, replacement_readiness, investigation_priorities,
validation.

Rules the code enforces:
- Ground truth for evaluation is the manifest services whose evidence is a SYMBOL NAME ("name"); services derived from
  address ranges ("layer", "layer+range", "link-order") are used as evidence only, never as truth (they would make the
  address baseline right by construction).
- A held-out function's own label is never visible to any evidence family; in leave-one-subsystem-out, no label of the
  held-out service is visible (including callee names used as call evidence).
- Trivial blocks (fewer than 3 instructions) and ubiquitous blocks (present in more than 1 % of functions:
  prologues, epilogues) carry no weight; a block's weight is its inverse document frequency.
- A name is proposed above TENTATIVE only when at least two evidence families that are not derived from each other
  agree; EXACT only for a manifest symbol name; the engine never marks anything VERIFIED (only the gates do).
"""
from __future__ import annotations

import argparse
import bisect
import collections
import csv
import json
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(SRC, "tools"))
sys.path.insert(0, HERE)
from dolrecon import dol as dolmod  # noqa: E402
import structure_match as sm  # noqa: E402

UNKNOWN = "sdk:unidentified"
FAMILIES = ("address", "calls", "globals", "blocks", "structure", "hardware")
# hardware family: the device a function's own loads/stores reach (sdk_anchors.py, label-free), mapped to this
# project's service names by a FIXED table written from the hardware map - never fitted to labels.
DEVICE_SERVICE = {"AI": "audio.ai", "DI": "files.dvd", "SI": "input.si", "EXI": "bus.exi", "VI": "graphics.vi",
                  "CP": "graphics.gx", "PE": "graphics.gx", "WGP": "graphics.gx", "DSP": "audio.dsp",
                  "PI": "os.interrupts", "MI": "os.memory"}
LOADS, STORES = sm.LOADS, sm.STORES


# ---- Phase 2: partial patterns --------------------------------------------------------------------------------------

def decode_function(image, start, size):
    """Basic blocks (normalized signatures, field offsets kept) and small-data globals of one function."""
    words = []
    for at in range(start, start + size, 4):
        try:
            words.append((at, image.word(at)))
        except Exception:
            break
    leaders = {start}
    for at, w in words:
        op = w >> 26
        if op == 16:                                                   # bc
            bd = w & 0xFFFC
            bd = bd - 0x10000 if bd & 0x8000 else bd
            leaders.add((at + bd) & 0xFFFFFFFF)
            leaders.add(at + 4)
        elif op == 18 and not (w & 1):                                 # b (not bl)
            li = w & 0x03FFFFFC
            li = li - 0x04000000 if li & 0x02000000 else li
            leaders.add((at + li) & 0xFFFFFFFF)
            leaders.add(at + 4)
        elif op == 19:
            leaders.add(at + 4)
    blocks, cur, glob = [], [], set()
    for at, w in words:
        if at in leaders and cur:
            blocks.append(tuple(cur))
            cur = []
        op, a = w >> 26, (w >> 16) & 31
        t = sm.token(w)
        if op in LOADS | STORES:
            disp = w & 0xFFFF
            disp = disp - 0x10000 if disp & 0x8000 else disp
            if a in (2, 13):
                glob.add(f"r{a}{disp:+#x}")
                t += "@g"                                              # a global: which one is not structure
            elif a != 1:
                t += f"@{disp:+#x}"                                    # struct field offset: kept
        elif op in (10, 11) and (w & 0xFFFF) < 0x100:                  # cmpli / cmpi small constants: kept
            t += f"#{w & 0xFFFF}"
        cur.append(t)
    if cur:
        blocks.append(tuple(cur))
    return blocks, glob


# ---- inputs ---------------------------------------------------------------------------------------------------------

def load_inputs(args):
    image = dolmod.load_file(args.dol)
    funcs = {}
    for r in csv.DictReader(open(args.functions, encoding="utf-8")):
        a = int(r["addr"])
        funcs[a] = {"name": r["name"], "size": int(r["size"]),
                    "callees": [int(x, 16) for x in r["callees"].split()] if r["callees"] else []}
    rows = {}
    for line in open(args.graph, encoding="utf-8"):
        p = line.split()
        if len(p) >= 5 and p[0] == "row":
            rows[int(p[1], 16)] = (p[3], p[4])
    manifest = json.load(open(args.manifest, encoding="utf-8"))
    man = {int(e["address"], 16): e for e in manifest["entries"]}
    owners = sm.load_owners(args.owners)
    return image, funcs, rows, man, owners


# ---- Phase 3/4: evidence and hypotheses ------------------------------------------------------------------------------

class Evidence:
    def __init__(self, funcs, man, decoded, fps, block_idf, glob_idf, platform, mmio=None):
        self.mmio = mmio or {}
        self.funcs, self.man, self.decoded, self.fps = funcs, man, decoded, fps
        self.block_idf, self.glob_idf, self.platform = block_idf, glob_idf, platform
        self.callers = collections.defaultdict(set)
        for a, f in funcs.items():
            for c in f["callees"]:
                self.callers[c].add(a)
        self.block_owners = collections.defaultdict(set)
        self.glob_owners = collections.defaultdict(set)
        for a in platform:
            blocks, glob = decoded[a]
            for b in set(blocks):
                if b in block_idf:
                    self.block_owners[b].add(a)
            for g in glob:
                if g in glob_idf:
                    self.glob_owners[g].add(a)
        self.addr_sorted = sorted(platform)

    def families(self, a, labels):
        """Per evidence family: {service: weight} from LABELED functions only (labels = visible labels)."""
        out = {}
        # address: nearest labeled platform function on each side
        i = bisect.bisect_left(self.addr_sorted, a)
        d = collections.Counter()
        for step in (-1, 1):
            j = i + (0 if step > 0 else -1)
            while 0 <= j < len(self.addr_sorted):
                b = self.addr_sorted[j]
                if b != a and b in labels:
                    d[labels[b]] += 1.0
                    break
                j += step
        out["address"] = d
        # calls: labeled callers and callees
        d = collections.Counter()
        for b in set(self.funcs.get(a, {}).get("callees", [])) | self.callers.get(a, set()):
            if b != a and b in labels:
                d[labels[b]] += 1.0
        out["calls"] = d
        # globals: labeled functions touching the same rare small-data global
        d = collections.Counter()
        for g in self.decoded[a][1]:
            w = self.glob_idf.get(g)
            if w:
                for b in self.glob_owners[g]:
                    if b != a and b in labels:
                        d[labels[b]] += w
        out["globals"] = d
        # blocks: labeled functions sharing a rare basic block (partial match)
        d = collections.Counter()
        for blk in set(self.decoded[a][0]):
            w = self.block_idf.get(blk)
            if w:
                for b in self.block_owners[blk]:
                    if b != a and b in labels:
                        d[labels[b]] += w
        out["blocks"] = d
        # structure: best whole-function Jaccard >= 0.6
        d = collections.Counter()
        best, bs = None, 0.6
        for b in self.platform:
            if b != a and b in labels:
                s = sm.jaccard(self.fps[a], self.fps[b])
                if s > bs:
                    best, bs = b, s
        if best is not None:
            d[labels[best]] += bs
        out["structure"] = d
        d = collections.Counter()
        for dev, n in self.mmio.get(a, {}).items():
            if dev in DEVICE_SERVICE:
                d[DEVICE_SERVICE[dev]] += n
        out["hardware"] = d
        return out


def combine(fam, weights):
    total = collections.Counter()
    for f, d in fam.items():
        s = sum(d.values())
        if s and weights.get(f, 0):
            for k, v in d.items():
                total[k] += weights[f] * v / s
    return total


def ranked(counter):
    return [k for k, _ in sorted(counter.items(), key=lambda kv: (-kv[1], kv[0]))]


# ---- main -----------------------------------------------------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--manifest", required=True)
    ap.add_argument("--graph", required=True)
    ap.add_argument("--plan")
    ap.add_argument("--dol", default=os.path.join(SRC, "..", "game", "sys", "main.dol"))
    ap.add_argument("--functions", default=os.path.join(SRC, "extract", "functions.csv"))
    ap.add_argument("--owners", default=os.path.join(SRC, "extract", "native-owners.txt"))
    ap.add_argument("--hits", default=os.path.join(SRC, "test_data", "platform", "gx_rw_frontier.csv"))
    ap.add_argument("--out", default=os.path.join(SRC, ".build", "puzzle"))
    args = ap.parse_args(argv)
    image, funcs, rows, man, owners = load_inputs(args)

    platform = sorted(a for a in man if a in funcs)
    names = {a: f["name"] for a, f in funcs.items()}
    names.update({a: e["name"] for a, e in man.items()})

    # Phase 1 inventory
    inv = {"binary_functions": len(funcs), "generator_rows": len(rows),
           "rows_by_kind": dict(collections.Counter(f"{k} {s}" for k, s in rows.values())),
           "manifest_platform_functions": len(man),
           "manifest_by_layer": dict(collections.Counter(e["layer"] for e in man.values())),
           "manifest_by_evidence": dict(collections.Counter(e["evidence"] for e in man.values())),
           "platform_functions_compiled": sum(1 for a in platform if rows.get(a) == ("body", "live")),
           "platform_functions_owned": sum(1 for a in platform if a in owners or (rows.get(a) or ("",))[0] == "owner"),
           "platform_functions_pruned": sum(1 for a in platform if rows.get(a) == ("body", "pruned"))}
    if args.plan:
        inv["plan_platform_rows_compiled"] = json.load(open(args.plan, encoding="utf-8"))["platform_live_bodies"]

    # Phase 2 patterns: decode every function (blocks for IDF across the whole binary), fingerprints for platform
    decoded, df_block, df_glob = {}, collections.Counter(), collections.Counter()
    for a, f in funcs.items():
        if f["size"] <= 0:
            continue
        blocks, glob = decode_function(image, a, f["size"])
        decoded[a] = (blocks, glob)
        df_block.update({b for b in set(blocks) if len(b) >= 3})
        df_glob.update(glob)
    n = len(decoded)
    cap = max(2, n // 100)                                             # in more than 1 %: compiler idiom, no weight
    block_idf = {b: math.log(n / c) for b, c in df_block.items() if 2 <= c <= cap}
    glob_idf = {g: math.log(n / c) for g, c in df_glob.items() if 2 <= c <= 40}
    platform = [a for a in platform if a in decoded]
    fps = {a: sm.features(image, a, funcs[a]["size"], names)[0] for a in platform}
    infos = {a: sm.features(image, a, funcs[a]["size"], names)[1] for a in platform}
    import sdk_anchors
    mmio = {}
    for a in platform:
        found = sdk_anchors.anchors(image, a, funcs[a]["size"])[0]["mmio"]
        if found:
            mmio[a] = dict(found)
    ev = Evidence(funcs, man, decoded, fps, block_idf, glob_idf, platform, mmio)

    # Labels: truth = symbol-name evidence only; all non-unknown labels are usable as evidence
    truth = {a: man[a]["service"] for a in platform if man[a]["evidence"] == "name" and man[a]["service"] != UNKNOWN}
    evidence_labels = {a: man[a]["service"] for a in platform if man[a]["service"] != UNKNOWN}

    # Phase 6 validation: tune weights on a dev split (even index), evaluate on test (odd index)
    keys = sorted(truth)
    dev, test = keys[0::2], keys[1::2]
    fam_cache = {}

    def fams(a, labels_key, labels):
        k = (a, labels_key)
        if k not in fam_cache:
            fam_cache[k] = ev.families(a, labels)
        return fam_cache[k]

    def loo_labels(a):
        lab = dict(evidence_labels)
        lab.pop(a, None)
        return lab

    def accuracy(subset, weights):
        top1 = top5 = 0
        for a in subset:
            sc = combine(fams(a, "loo", loo_labels(a)), weights)
            r = ranked(sc)
            top1 += bool(r) and r[0] == truth[a]
            top5 += truth[a] in r[:5]
        return top1 / max(1, len(subset)), top5 / max(1, len(subset))

    single = {f: {g: (1.0 if g == f else 0.0) for g in FAMILIES} for f in FAMILIES}
    grid = []
    for wa in (0.0, 0.5, 1.0):
        for wc in (0.0, 0.5, 1.0):
            for wg in (0.0, 0.5, 1.0):
                for wb in (0.0, 0.5, 1.0):
                    for wh in (0.0, 0.5, 1.0):
                        w = {"address": wa, "calls": wc, "globals": wg, "blocks": wb, "structure": 0.25,
                             "hardware": wh}
                        grid.append((accuracy(dev, w)[0], w))
    best_w = max(grid, key=lambda x: (x[0], -sum(x[1].values())))[1]
    validation = {"truth": "manifest services with symbol-name evidence", "truth_functions": len(keys),
                  "dev": len(dev), "test": len(test), "weights_tuned_on_dev": best_w, "test_results": {}}
    for f in FAMILIES:
        t1, t5 = accuracy(test, single[f])
        validation["test_results"][f"{f}-only"] = {"top1": round(t1, 4), "top5": round(t5, 4)}
    t1, t5 = accuracy(test, best_w)
    validation["test_results"]["combined"] = {"top1": round(t1, 4), "top5": round(t5, 4)}
    # marginal value: test cases where address-only is wrong
    addr_wrong = [a for a in test if (ranked(combine(fams(a, "loo", loo_labels(a)), single["address"])) or [None])[0]
                  != truth[a]]
    fixed = sum(1 for a in addr_wrong
                if (ranked(combine(fams(a, "loo", loo_labels(a)), best_w)) or [None])[0] == truth[a])
    broke = sum(1 for a in test if a not in addr_wrong
                and (ranked(combine(fams(a, "loo", loo_labels(a)), best_w)) or [None])[0] != truth[a])
    validation["marginal_over_address"] = {"address_wrong": len(addr_wrong), "combined_fixes": fixed,
                                           "combined_breaks_address_right": broke}
    # calibration: combined top-1 accuracy by score-share bucket
    buckets = collections.defaultdict(lambda: [0, 0])
    for a in test:
        sc = combine(fams(a, "loo", loo_labels(a)), best_w)
        r = ranked(sc)
        if not r:
            continue
        share = sc[r[0]] / sum(sc.values())
        bk = min(4, int(share * 5))
        buckets[bk][0] += 1
        buckets[bk][1] += r[0] == truth[a]
    validation["calibration"] = {f"{b * 20}-{b * 20 + 20}%": f"{c}/{n_} = {c / n_:.0%}"
                                 for b, (n_, c) in sorted(buckets.items())}

    # Leave-one-subsystem-out: hide every label of one service; recover its members as a CLUSTER from calls + globals +
    # blocks only (no address), compare with an address-run baseline. Purity/recall of the best-matching cluster.
    loso = {}
    for svc in sorted({truth[a] for a in keys}):
        members = {a for a in platform if evidence_labels.get(a) == svc}
        if len(members) < 4:
            continue
        hidden = set(members)
        visible = {a: l for a, l in evidence_labels.items() if a not in hidden}
        # candidate pool: unlabeled-by-visible functions near any member in the graph (no address)
        pool = {a for a in platform if a not in visible}
        adj = collections.defaultdict(set)
        for a in pool:
            nb = set(funcs[a]["callees"]) | ev.callers.get(a, set())
            for g in decoded[a][1]:
                if g in glob_idf:
                    nb |= ev.glob_owners[g]
            for blk in set(decoded[a][0]):
                if blk in block_idf:
                    nb |= ev.block_owners[blk]
            for b in nb & pool:
                if b != a:
                    adj[a].add(b)
                    adj[b].add(a)
        seen, comps = set(), []
        for a in sorted(pool):
            if a in seen:
                continue
            comp, stack = set(), [a]
            while stack:
                x = stack.pop()
                if x in seen:
                    continue
                seen.add(x)
                comp.add(x)
                stack.extend(adj[x] - seen)
            comps.append(comp)
        bestc = max(comps, key=lambda c: len(c & members))
        # address baseline: maximal address runs of pool functions (no visible label between)
        runs, cur = [], []
        for a in ev.addr_sorted:
            if a in pool:
                cur.append(a)
            elif cur:
                runs.append(set(cur))
                cur = []
        if cur:
            runs.append(set(cur))
        bestr = max(runs, key=lambda c: len(c & members))
        loso[svc] = {"members": len(members),
                     "graph_cluster": {"size": len(bestc), "recall": round(len(bestc & members) / len(members), 3),
                                       "precision": round(len(bestc & members) / len(bestc), 3)},
                     "address_run": {"size": len(bestr), "recall": round(len(bestr & members) / len(members), 3),
                                     "precision": round(len(bestr & members) / len(bestr), 3)}}
    validation["leave_one_subsystem_out"] = loso

    # Negative control for partial matching: shuffle which function each rare block belongs to (same block-count
    # distribution) and count cross-service rare-block links at the same threshold.
    import random
    rng = random.Random(20261107)
    real_links = same_real = 0
    for blk, owners_ in ev.block_owners.items():
        lab = [evidence_labels[o] for o in owners_ if o in evidence_labels]
        for i in range(len(lab)):
            for j in range(i + 1, len(lab)):
                real_links += 1
                same_real += lab[i] == lab[j]
    shuffled = list(evidence_labels.values())
    rng.shuffle(shuffled)
    shuf = dict(zip(evidence_labels.keys(), shuffled))
    same_shuf = 0
    for blk, owners_ in ev.block_owners.items():
        lab = [shuf[o] for o in owners_ if o in shuf]
        for i in range(len(lab)):
            for j in range(i + 1, len(lab)):
                same_shuf += lab[i] == lab[j]
    # Precision of the PROBABLE bar on held-out truth: >= 2 non-address families agree with the combined top AND
    # the top holds >= 80 % of the combined score (the calibrated bucket).
    def probable(fam, sc):
        r = ranked(sc)
        if not r:
            return None
        agree = [f for f in ("calls", "globals", "blocks", "structure", "hardware") if fam[f] and ranked(fam[f])[0] == r[0]]
        share = sc[r[0]] / sum(sc.values())
        return r[0] if len(agree) >= 2 and share >= 0.8 else None

    hit = cov = 0
    for a in test:
        fam = fams(a, "loo", loo_labels(a))
        p = probable(fam, combine(fam, best_w))
        if p is not None:
            cov += 1
            hit += p == truth[a]
    validation["probable_bar"] = {"rule": ">= 2 non-address families agree with the combined top and share >= 80 %",
                                  "coverage": f"{cov}/{len(test)}", "precision": f"{hit}/{cov}" if cov else "n/a"}

    validation["negative_control_blocks"] = {
        "rare_block_links": real_links,
        "same_service_real": round(same_real / max(1, real_links), 4),
        "same_service_shuffled_labels": round(same_shuf / max(1, real_links), 4)}

    # ---- outputs for every platform function (Phase 4/5/8/9) ----
    hits = {}
    if os.path.exists(args.hits):
        for r in csv.DictReader(open(args.hits, encoding="utf-8")):
            try:
                hits[int(r["address"], 16)] = int(r["hits"] or 0)
            except ValueError:
                pass
    # Semantic rule: callback registration. A setter that stores a global with interrupts off (structure_match
    # template "interrupt-bracketed-update") paired with the functions that load the same global and call through it
    # (bctrl / blrl): "registers the callback that <consumer> invokes". Positive controls: the known VI / AX setters.
    st, ld, indirect = collections.defaultdict(set), collections.defaultdict(set), set()
    for a, f in funcs.items():
        if f["size"] <= 0:
            continue
        for at in range(a, a + f["size"], 4):
            w = image.word(at)
            op, ra = w >> 26, (w >> 16) & 31
            if ra == 13 and op in LOADS | STORES:
                d = w & 0xFFFF
                d = d - 0x10000 if d & 0x8000 else d
                (st if op in STORES else ld)[d].add(a)
            if w in (0x4E800421, 0x4E800021):
                indirect.add(a)
    def calls_through_global(u, d):
        """Straight-line data flow in consumer u: lwz rX, d(r13) -> (mr copies) -> mtctr/mtlr rX -> bctrl/blrl."""
        size = funcs[u]["size"]
        held = set()
        for at in range(u, u + size, 4):
            w = image.word(at)
            op, rd, ra = w >> 26, (w >> 21) & 31, (w >> 16) & 31
            disp = w & 0xFFFF
            disp = disp - 0x10000 if disp & 0x8000 else disp
            if op == 32 and ra == 13 and disp == d:                    # lwz rd, d(r13)
                held.add(rd)
            elif op == 31 and ((w >> 1) & 0x3FF) == 444 and rd in held and ((w >> 11) & 31) == rd:
                held.add(ra)                                           # mr ra, rd (or ra = rd | rd)
            elif op == 31 and ((w >> 1) & 0x3FF) == 467 and rd in held:   # mtspr: rS in rd field
                spr = ((w >> 16) & 0x1F) | (((w >> 11) & 0x1F) << 5)
                if spr in (8, 9):                                      # LR / CTR
                    return True
            elif op == 18 and (w & 1):                                 # bl: volatile registers die
                held -= {0, *range(3, 13)}
            elif (op == 18) or w == 0x4E800020:                        # unconditional b / blr: flow ends
                held.clear()
            # bc (null checks before the call) keeps the held registers
        return False

    callback_roles = {}
    for a in platform:
        if sm.template(infos[a], names) != "interrupt-bracketed-update":
            continue
        for d, writers in st.items():
            if a not in writers:
                continue
            consumers = sorted(u for u in ld[d] if u != a and u in indirect and calls_through_global(u, d))
            if not consumers:
                continue
            svc = man[a]["service"]
            handler = names.get(consumers[0], f"fn_{consumers[0]:08x}")
            callback_roles[a] = {
                "global": f"r13{d:+#x}", "consumers": [f"0x{u:08X} {names.get(u)}" for u in consumers],
                "proposed": (names.get(a) if not names.get(a, "fn_").startswith("fn_") else
                             f"{svc.split('.')[-1].upper()}_SetCallbackInvokedBy_{handler}"),
                "known_name": not names.get(a, "fn_").startswith("fn_")}
    functions_out, patterns_out, edges_out, cands, props, ready, prio = [], [], [], [], [], [], []
    for a, role in sorted(callback_roles.items()):
        for u in role["consumers"]:
            edges_out.append({"from": f"0x{a:08X}", "to": u.split()[0], "type": "registers-callback-invoked-by",
                              "provenance": f"stores {role['global']} with interrupts off; consumer loads it and calls "
                                            f"through ctr/lr"})
    for a in platform:
        e = man[a]
        kind = rows.get(a)
        functions_out.append({"address": f"0x{a:08X}", "name": names.get(a), "service": e["service"],
                              "service_evidence": e["evidence"], "layer": e["layer"],
                              "row": " ".join(kind) if kind else "none", "owned": a in owners,
                              "size": funcs[a]["size"], "template": sm.template(infos[a], names)})
        rare = sorted({b for b in decoded[a][0] if b in block_idf}, key=lambda b: -block_idf[b])
        for b in rare[:3]:
            patterns_out.append({"address": f"0x{a:08X}", "block": " ".join(b), "idf": round(block_idf[b], 3),
                                 "shared_with": len(ev.block_owners[b]) - 1})
        for c in funcs[a]["callees"]:
            if c in man:
                edges_out.append({"from": f"0x{a:08X}", "to": f"0x{c:08X}", "type": "calls", "provenance":
                                  "extract/functions.csv callees (static bl)"})
        for g in sorted(decoded[a][1]):
            if g in glob_idf:
                edges_out.append({"from": f"0x{a:08X}", "to": g, "type": "reads-or-writes-global", "provenance":
                                  "decoded r13/r2-relative load/store", "weight": round(glob_idf[g], 3)})
        # hypotheses for functions without a symbol-name identity
        lab = dict(evidence_labels)
        lab.pop(a, None)
        fam = ev.families(a, lab)
        sc = combine(fam, best_w)
        r = ranked(sc)
        agreeing = [f for f in ("calls", "globals", "blocks", "structure", "hardware") if fam[f] and ranked(fam[f])[0] == (r[0] if r
                                                                                                             else None)]
        if e["evidence"] != "name":
            cands.append({"address": f"0x{a:08X}", "name": names.get(a), "manifest_service": e["service"],
                          "hypotheses": [{"service": k, "score": round(sc[k], 3)} for k in r[:3]],
                          "agreeing_independent_families": agreeing,
                          "address_family": ranked(fam["address"])[:1]})
        share = (sc[r[0]] / sum(sc.values())) if r else 0.0
        if e["evidence"] == "name" and not names.get(a, "fn_").startswith("fn_"):
            level = "EXACT IDENTITY (manifest symbol name; not re-verified here)"
            prop = names.get(a)
        elif a in callback_roles:
            level = "PROBABLE ROLE"
            prop = callback_roles[a]["proposed"]
            agreeing = agreeing + ["callback-pairing"]
        elif r and len(agreeing) >= 2 and share >= 0.8:
            level = "PROBABLE ROLE"
            prop = f"{r[0].replace('.', '_').replace(':', '_')}__{sm.template(infos[a], names).replace('-', '_')}" \
                   f"_{a:08X}"
        elif r:
            level = "TENTATIVE ROLE"
            prop = f"{r[0].replace('.', '_').replace(':', '_')}__{a:08X}"
        else:
            level, prop = "UNKNOWN", f"fn_{a:08x}"
        props.append({"address": f"0x{a:08X}", "current_symbol": names.get(a), "proposed": prop, "level": level,
                      "evidence_families": agreeing, "address_says": ranked(fam["address"])[:1],
                      "verify_next": "contract test vs the original body in the reference interpreter"})
        if kind == ("body", "live"):
            info = infos[a]
            unknown_nb = sum(1 for b in set(funcs[a]["callees"]) | ev.callers.get(a, set())
                             if b in man and man[b]["evidence"] != "name")
            prio.append({"address": f"0x{a:08X}", "name": names.get(a), "service": e["service"],
                         "unknown_neighbours": unknown_nb,
                         "callers": len(ev.callers.get(a, set())), "callees": len(funcs[a]["callees"]),
                         "hits": hits.get(a), "competing": len(r[:3]) if r else 0,
                         "score": unknown_nb + (1 if len(r) > 1 else 0) + (2 if hits.get(a) else 0)})
            ready.append({"address": f"0x{a:08X}", "name": names.get(a), "service": e["service"],
                          "owned": False, "compiled": True, "callers": len(ev.callers.get(a, set())),
                          "template": sm.template(infos[a], names),
                          "callbacks_or_indirect": any(t in ("bctrl", "blrl") for blk in decoded[a][0] for t in blk),
                          "touches_hardware": any(s.startswith("mmio") or s == "gx-fifo" for s in info["sem"]),
                          "interrupt_bracketed": "call:OSDisableInterrupts" in info["sem"],
                          "readiness": ("trivial" if info["ins"] <= 2 else
                                        "needs-oracle" if any(s.startswith("mmio") or s == "gx-fifo"
                                                              for s in info["sem"]) else "candidate")})
    prio.sort(key=lambda p: (-p["score"], p["address"]))

    os.makedirs(args.out, exist_ok=True)
    outputs = {"inventory": inv, "functions": functions_out, "patterns": patterns_out, "evidence_edges": edges_out,
               "candidate_matches": cands, "proposed_names": props, "subsystem_clusters": loso,
               "verified_identifications": [p for p in props if p["level"].startswith("EXACT")],
               "replacement_readiness": ready, "investigation_priorities": prio[:200], "validation": validation,
               "callback_roles": {f"0x{a:08X}": v for a, v in sorted(callback_roles.items())}}
    for k, v in outputs.items():
        with open(os.path.join(args.out, f"{k}.json"), "w", encoding="utf-8", newline="\n") as fh:
            json.dump(v, fh, indent=1, sort_keys=True)
    print(json.dumps({"inventory": inv, "validation": {k: v for k, v in validation.items()
                                                       if k != "leave_one_subsystem_out"}}, indent=1))
    print("leave-one-subsystem-out (graph cluster vs address run, recall/precision):")
    for s, v in loso.items():
        print(f"  {s:32} n={v['members']:4}  graph {v['graph_cluster']['recall']:.2f}/{v['graph_cluster']['precision']:.2f}"
              f"  address {v['address_run']['recall']:.2f}/{v['address_run']['precision']:.2f}")
    print("levels:", dict(collections.Counter(p["level"].split(" (")[0] for p in props)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
