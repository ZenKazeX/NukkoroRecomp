"""Root-cause clustering of an F1b report (documentation aid; not part of the pass/fail verdict).

  rootcause_f1b.py --report report.json --vectors vectors.jsonl [--out rootcauses.json]

Every finding gets exactly one root-cause label from explicit, ordered rules over its own recorded
evidence (class, first divergent instruction, exception the spec took, the FPSCR/MSR state before the
divergent step, the diverging field set). A finding no rule matches is UNCLASSIFIED and listed in full:
the rules never hide a finding, they only name the ones they recognise. The fpmode family (one word and
state repeated with only RN, NI, the enables or MSR[FE0|FE1] changed) gives causal evidence for the FP
mode rules: a group whose mode-0 variant passes and whose other variants diverge in FPR values is
attributed to that mode bit.
"""
from __future__ import annotations

import argparse
import json
import re
from collections import Counter, defaultdict
from pathlib import Path


def u(x):
    return int(x, 16) if isinstance(x, str) else int(x)


def label(f, v):
    st = v["state"]
    cls = f["class"]
    fam = f["family"]
    shape = v.get("shape") or ""
    fd = f.get("spec_first_divergence") or f.get("first_divergence") or {}
    mn = fd.get("mnemonic")
    exc = fd.get("exception")
    word = u(fd["word"]) if fd.get("word") else None
    fields = set((f.get("spec_fields") or {}).keys()) | set((fd.get("fields") or {}).keys())
    for tab in (f.get("lanes") or {}).values():
        fields |= set(tab.keys())
    fpscr_b = u(fd["fpscr_before"]) if fd.get("fpscr_before") else st["fpscr"]
    msr_b = u(fd["msr_before"]) if fd.get("msr_before") else st["msr"]
    if f.get("facet") == "fpscr-status":
        return "R01-FPSCR-STATUS"
    if cls == "host-fact":
        return "H-HOST-FACT"
    if cls in ("imprecise-state-at-entry", "entry-registers", "manual-recognition-mismatch"):
        return "R-" + cls.upper()
    if cls == "engine-divergence":
        if fields == {"access_log"} and shape.startswith("ram-poll-idle"):
            return "E03-COALESCE-OBSERVED-PEEK"
        if fam == "irq" and "bctr-backward-switch" in shape:
            return "E01-BCTR-BACKWARD-RECOGNITION(F3)"
        if fam == "irq" and "bctr-forward-nonlabel" in shape:
            return "E02-BCTR-NONLABEL-LEAVE-RECOGNITION"
        return "E99-UNCLASSIFIED-ENGINE"
    if exc == "alignment":
        return "C02-ALIGNMENT-NOT-RAISED"
    if exc == "program" and mn in ("fsqrt", "fsqrts"):
        return "C04-FSQRT-NOT-ILLEGAL"
    if exc == "program" and msr_b & 0x4000:
        return "C05-PRIVILEGE-NOT-ENFORCED"
    if exc in ("program", "trap") and msr_b & 0x900 and word is not None and (word >> 26) in (4, 59, 63):
        return "C09-FP-ENABLED-PROGRAM-EXCEPTION"
    if exc == "program" and mn in ("mfspr", "mtspr"):
        return "C06-SPR-DECODE-PROGRAM-EXCEPTION"
    if exc == "program" and word is not None and (word >> 26) == 31 and ((word >> 1) & 0x3FF) in (339, 371, 467):
        return "C06-SPR-DECODE-PROGRAM-EXCEPTION"
    if fields == {"reserve_addr"}:
        return "C01-RESERVE-ADDR-PHYSICAL"
    if fields and all(k.startswith("spr[") for k in fields) and mn == "mtspr":
        return "C03-READ-ONLY-OR-UNIMPLEMENTED-SPR-WRITE-STORED"
    if word is not None and (word >> 26) == 31 and ((word >> 1) & 0x3FF) == 371:
        return "C07-MFTB-IGNORES-TBR"
    fpr = any(k.startswith("fpr[") for k in fields)
    if fpr or "fpscr" in fields or "cr" in fields:
        if fpscr_b & 0x3:
            return "C10-FPSCR-RN-IGNORED"
        if fpscr_b & 0x4:
            return "C11-FPSCR-NI-VALUE"
        if fpscr_b & 0xF8:
            return "C12-FPSCR-ENABLE-NO-RESULT"
        if msr_b & 0x900:
            return "C09-FP-ENABLED-PROGRAM-EXCEPTION"
    return "C99-UNCLASSIFIED"


def rn_counterfactual(f, v):
    """Re-run the spec model with FPSCR[RN] = 0 in the initial state. True when every diverging FPR field
    then equals the engines' value (RN is the whole cause), False when some still differ, None when the
    vector changes RN itself (mtfsfi/mtfsf/mtfsb) or a field is not an FPR image."""
    import copy
    import spec_runner_f1b as SR
    words = [w for ws in v["row_words"].values() for w in ws]
    if any((w >> 26) == 63 and ((w >> 1) & 0x3FF) in (134, 711, 38, 70) for w in words):
        return None
    table = f.get("spec_fields") or {}
    fields = {k: r_ for k, r_ in table.items() if k.startswith("fpr[")}
    if not fields:
        return None
    v2 = copy.deepcopy(v)
    v2["state"]["fpscr"] &= ~0x3
    s = SR.run_vector(v2, None)
    for k, r_ in fields.items():
        i = int(k[4:k.index("]")])
        slot = int(k[-1])
        if u(s["final"]["fpr"][i][slot]) & ~s["masks"].get(k, 0) != u(r_["interpreter"]) & ~s["masks"].get(k, 0):
            return False
    return True


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--report", type=Path, required=True)
    p.add_argument("--vectors", type=Path, required=True)
    p.add_argument("--spec", type=Path, required=True)
    p.add_argument("--out", type=Path, default=None)
    p.add_argument("--ledger-out", type=Path, default=None,
                   help="write the exact-fingerprint ledger (known_candidates_f1b.json) for this report")
    p.add_argument("--manifest", type=Path, default=None, help="manifest.json of the generated set (seed/scale)")
    a = p.parse_args()
    import sys
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import spec_runner_f1b as SR
    SR.load(a.spec)
    vec = {}
    for line in a.vectors.open(encoding="utf-8"):
        v = json.loads(line)
        vec[v["id"]] = v
    rep = json.loads(a.report.read_text(encoding="utf-8"))
    if rep.get("findings_total", 0) > len(rep["findings"]):
        raise SystemExit("report was written with a --max-detail below the finding count; rerun with a larger one")
    groups = defaultdict(list)
    for f in rep["findings"]:
        name = label(f, vec[f["id"]])
        if name == "C10-FPSCR-RN-IGNORED":
            cf = rn_counterfactual(f, vec[f["id"]])
            f["rn_counterfactual"] = cf
            if cf is False:
                st = vec[f["id"]]["state"]
                name = "C12-FPSCR-ENABLE-NO-RESULT" if st["fpscr"] & 0xF8 else "C10b-RN-SET-BUT-NOT-THE-WHOLE-CAUSE"
        groups[name].append(f)
    out = {}
    for name, members in sorted(groups.items()):
        mnems = Counter()
        fams = Counter()
        for f in members:
            fd = f.get("spec_first_divergence") or f.get("first_divergence") or {}
            mnems[str(fd.get("mnemonic"))] += 1
            fams[f["family"]] += 1
        cf = Counter(str(f.get("rn_counterfactual")) for f in members if "rn_counterfactual" in f)
        out[name] = {"findings": len(members), "vectors": sorted({f["id"] for f in members}),
                     "rn_counterfactual": dict(cf),
                     "first_divergent_mnemonics": dict(mnems.most_common()), "families": dict(fams),
                     "example": members[0]}
    # fpmode causal table
    mode = defaultdict(dict)
    failing = defaultdict(set)
    for f in rep["findings"]:
        v = vec[f["id"]]
        if v["family"] == "fpmode" and not f.get("facet"):
            failing[f["id"]].add(label(f, v))
    for v in vec.values():
        if v["family"] != "fpmode":
            continue
        group, variant = v["shape"].split("/")
        mode[group][variant] = sorted(failing.get(v["id"], [])) or ["pass"]
    causal = Counter()
    for group, variants in mode.items():
        for axis in ("RN", "NI", "EN", "FE"):
            keys = sorted(k for k in variants if k.startswith(axis + "="))
            if not keys:
                continue
            base = variants[keys[0]]
            others = [variants[k] for k in keys[1:]]
            if base == ["pass"] and any(o != ["pass"] for o in others):
                causal[f"{axis}: mode-0 passes, another value diverges"] += 1
            elif base != ["pass"]:
                causal[f"{axis}: mode-0 already diverges"] += 1
            else:
                causal[f"{axis}: all values pass"] += 1
    out["_fpmode_causal"] = dict(causal)
    out["_fpmode_groups"] = {g: mode[g] for g in sorted(mode)[:60]}
    if a.ledger_out:
        manifest = json.loads(a.manifest.read_text(encoding="utf-8"))
        by_id = defaultdict(set)
        for name, members in groups.items():
            for f in members:
                by_id[(f["id"], f["class"], f.get("facet"))].add(name)
        entries = []
        for c in rep["candidates"]:
            if c["signature"][0] == "host-fact":
                continue
            names = set()
            for f in rep["findings"]:
                if f["id"] in set(c["vectors"]):
                    names |= by_id.get((f["id"], f["class"], f.get("facet")), set())
            entries.append({"candidate": "+".join(sorted(names)) or "unlabelled", "fingerprint": c["fingerprint"],
                            "signature": c["signature"], "count": c["count"]})
        ledger = {"config": {"seed": manifest["seed"], "scale": manifest["scale"],
                             "generator_sha256": manifest["generator_sha256"]},
                  "note": "Exact fingerprints of the F1b candidate groups recorded in "
                          "docs/recompiler-core-audit-2026-09-13/infrastructure/DIFFTEST_F1B.md. Regenerate only "
                          "after reviewing every changed group: tools/difftest/rootcause_f1b.py --ledger-out.",
                  "candidates": entries}
        a.ledger_out.write_text(json.dumps(ledger, indent=1), encoding="utf-8")
        print(f"ledger: {len(entries)} entries -> {a.ledger_out}")
    text = json.dumps(out, indent=1, default=str)
    if a.out:
        a.out.write_text(text, encoding="utf-8")
    for name, g in out.items():
        if name.startswith("_"):
            print(name, json.dumps(g)[:600])
            continue
        print(f"{g['findings']:5d}  {name:45s} {json.dumps(g['first_divergent_mnemonics'])[:160]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
