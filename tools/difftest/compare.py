"""Three-way comparator for the F1a differential harness: spec model vs reference interpreter vs
generated native code (production build and reference build).

Inputs: vectors.jsonl (generator), the driver lane JSONL files, the spec-runner JSONL, the generated
sources directory (for emitted C++ excerpts). Output: report.json plus a human summary on stdout.

Native time (docs/native-time-20261005/DESIGN.md, the difftest_driver.cpp header): native code has no
instruction count and guest code runs in zero game time. Native lanes (N NL R RL) carry no "retired"
count; the interpreter lanes' "retired" (Cpu::interpreted_instructions) is compared only between the
interpreter lanes and with the spec. TB and DEC never move inside a vector, in any lane or in the spec.
RL re-runs every vector under the differential oracle (Cpu::set_verify_recompiled); its VerifyReport
("verify") replaced the per-retire_block snapshots RL used to record.

Checks, per vector:
  host facts   every engine lane status ok (F1a has no vector that may end at a bound: "watchdog",
               "access-limit", "step-limit", "host-stack-bound", ... are all violations); native lanes
               interpreted nothing (g_non_native_ppc_executions delta 0, Cpu::interpreted_instructions 0),
               carry no retired count and kept the host stack within the driver's bound
               (host_stack_within_bound: painted depth less the lane's calibrated depth <= 16 KiB);
               interpreter lanes stepped exactly their retired count; RL's oracle run ended where the
               logged run did (verify: status ok, final_equal, mem_clean); no latched data-access fault;
               MEM1 outside the data window untouched.
               RETIRED with the instruction clock: recompiled_instructions == retired (native code counts
               nothing; zero interpretation is what is left of it)
  engines      every engine lane's final state (all GPR, FPR both halves, CR, XER, LR, CTR, MSR,
               FPSCR, pc, the full 1024-entry SPR array, SR, time base, reservation flag and address,
               both interrupt lines, fault latch; the retired count between interpreter lanes only) and
               data-window bytes must equal the reference interpreter lane I exactly; an oracle
               mismatch in RL is an engine divergence of RL (field "oracle")
  access log   logged lanes (NL, RL) vs I: the ordered MEM1 data-access sequence at byte granularity
  spec         spec final state vs I on the fields the spec State carries, masked ONLY by the
               locations the spec reports as undefined for this vector; ordered access log vs I.
               Spec verdict undefined-all / undefined-propagating / spec-gap -> engine-only (counted)

Classification: engine-divergence (lanes disagree), common-mode (all engine lanes agree, the spec
disagrees), host-fact, spec-gap-unexpected (a vector the generator expected the spec to model).

First divergent instruction:
  spec vs I      lockstep over the per-instruction traces; first differing step
  native vs I    no native per-instruction or per-block state exists (native time); the window is the
                 whole run. The RL oracle's first mismatching segment (its entry and the instructions
                 the interpreter replayed) is reported when the oracle saw one; then the vector's
                 generated prefix vectors (words[0..k) + blr) name the smallest diverging k; otherwise
                 the first instruction whose interpreter step writes a diverging field
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path

ENGINE_LANES = ("I", "IF", "R", "RL", "N", "NL")
NATIVE_LANES = ("R", "RL", "N", "NL")
INTERPRETER_LANES = ("I", "IF")
LOGGED_LANES = ("I", "RL", "NL")
# Why a driver status other than "ok" is a violation (difftest_driver.cpp header).
STATUS_NOTES = {
    "watchdog": "the native run reached no stop address and the host watchdog ended it",
    "access-limit": "the logged run outgrew the vector's step bound and the access cap ended it",
    "step-limit": "the interpreter reached the vector's step bound",
    "irq-at-retired-count": "the vector asserts a line at a retired-instruction count, which native time cannot "
                            "express (not run)",
    "host-stack-bound": "the native run's painted host stack exceeds the lane's calibrated depth by more than "
                        "--host-stack-bound",
    "native-stop-not-at-stop-address": "the native run stopped somewhere other than a stop address",
    "run-returned-without-stop": "Cpu::run returned without a stop",
}
NO_NATIVE_SNAPSHOT = ("none: native code has no instruction count, so no per-instruction native state exists "
                      "(native time); the final native state is the finding's 'generated' column")
SPR_MAP = {"srr0": 26, "srr1": 27, "dar": 19, "dsisr": 18, "hid0": 1008, "hid2": 920, "dec": 22}
# Architected state the spec State carries (compared through spec_view). Everything else an engine
# holds - the other SPRs (PVR, HID1, BATs, ...), SR, the external line, the fault latch - cannot be
# changed by a spec step that completed, so it must be unchanged by the engines too.
SPEC_CARRIED_SPR = set(SPR_MAP.values()) | set(range(272, 276)) | set(range(912, 920))


def untouched_diffs(initial: dict, final: dict) -> list:
    a, b = engine_fields(initial), engine_fields(final)
    out = []
    for k in sorted(set(a) | set(b)):
        carried = k.startswith(("gpr[", "fpr[")) or k in ("cr", "xer", "lr", "ctr", "msr", "fpscr", "pc", "tb",
                                                          "retired", "reserve", "reserve_addr", "line_dec")
        if k.startswith("spr[") and int(k[4:-1]) in SPEC_CARRIED_SPR:
            carried = True
        if not carried and a.get(k, 0) != b.get(k, 0):
            out.append(k)
    return out


def u(x):
    return int(x, 16) if isinstance(x, str) else int(x)


def engine_fields(state: dict) -> dict:
    """Flatten a driver state into comparable scalar fields."""
    out = {}
    for i, g in enumerate(state["gpr"]):
        out[f"gpr[{i}]"] = u(g)
    for i, (a, b) in enumerate(state["fpr"]):
        out[f"fpr[{i}].ps0"] = u(a)
        out[f"fpr[{i}].ps1"] = u(b)
    for key in ("cr", "xer", "lr", "ctr", "msr", "fpscr", "pc", "tb", "reserve_addr"):
        out[key] = u(state[key])
    spr = {int(n): u(v) for n, v in state["spr"]}
    for n in range(1024):
        if spr.get(n, 0):
            out[f"spr[{n}]"] = spr[n]
    for i, s in enumerate(state["sr"]):
        out[f"sr[{i}]"] = u(s)
    if "retired" in state:  # interpreter lanes and the spec only: native code has no instruction count
        out["retired"] = int(state["retired"])
    out["reserve"] = bool(state["reserve"])
    out["line_ext"] = bool(state["line_ext"])
    out["line_dec"] = bool(state["line_dec"])
    if "fault" in state:
        f = state["fault"]
        out["fault.latched"] = bool(f["latched"])
        if f["latched"]:
            out["fault.write"] = bool(f["write"])
            out["fault.effective"] = u(f["effective"])
            out["fault.pc"] = u(f["pc"])
    return out


def field_diffs(a: dict, b: dict) -> list:
    """Differing engine_fields keys of two lanes. The retired count is compared only when both lanes have one
    (two interpreter lanes); a native lane has none (native time), which the host facts check separately."""
    return [k for k in sorted(set(a) | set(b))
            if (k != "retired" or (k in a and k in b)) and a.get(k, 0) != b.get(k, 0)]


def oracle_divergence(record: dict):
    """RL's differential-oracle mismatches as an engine-divergence table entry, or None."""
    ver = record.get("verify") or {}
    if not ver.get("mismatches"):
        return None
    return {"mismatches": ver["mismatches"], "messages": list(ver.get("mismatch_messages") or [])[:8]}


def native_host_facts(name: str, record: dict) -> list:
    """Host facts of a native lane: nothing interpreted, no instruction count, host stack within the bound."""
    h = record["host"]
    out = []
    if h["non_native"] != 0:
        out.append(f"{name}: {h['non_native']} instructions interpreted in a native lane")
    if h.get("interpreted", 0) != 0:
        out.append(f"{name}: Cpu::interpreted_instructions {h['interpreted']} in a native lane")
    if "retired" in record["final"] or "retired" in record["initial"]:
        out.append(f"{name}: a native lane reports a retired count (native code has no instruction count)")
    within = h.get("host_stack_within_bound")
    if within is not True:
        out.append(f"{name}: host stack {'not measured' if within is None else 'beyond the bound'} "
                   f"(host_stack_within_bound {within}, spread {h.get('host_stack_spread')})")
    return out


def oracle_host_facts(record: dict, expected: str = "ok") -> list:
    """RL's oracle run must end where its logged run ended (the driver's "verify" object)."""
    ver = record.get("verify")
    if ver is None:
        return ["RL: no differential-oracle report (verify)"]
    out = []
    if ver["status"] != expected and ver["status"] != "final-differs":
        out.append(f"RL: the oracle run ended {ver['status']}, expected {expected}")
    if not ver["final_equal"]:
        out.append("RL: the oracle run's final state or data window differs from the logged run's")
    if not ver["mem_clean"]:
        out.append("RL: the oracle run modified MEM1 outside the data window")
    return out


def engine_spec_view(state: dict) -> dict:
    """The engine state on the spec State's vocabulary."""
    e = engine_fields(state)
    out = {k: v for k, v in e.items() if k.startswith("gpr[") or k.startswith("fpr[")}
    for key in ("cr", "xer", "lr", "ctr", "msr", "fpscr", "pc", "tb", "retired", "reserve", "reserve_addr",
                "line_dec"):
        if key in e:  # "retired" is absent from native lanes
            out[key] = e[key]
    spr = {int(n): u(v) for n, v in state["spr"]}
    for name, n in SPR_MAP.items():
        out[name] = spr.get(n, 0)
    for i in range(4):
        out[f"sprg[{i}]"] = spr.get(272 + i, 0)
    for i in range(8):
        out[f"gqr[{i}]"] = spr.get(912 + i, 0)
    out["exception"] = None
    return out


def spec_view(state: dict) -> dict:
    out = {}
    for i, g in enumerate(state["gpr"]):
        out[f"gpr[{i}]"] = u(g)
    for i, (a, b) in enumerate(state["fpr"]):
        out[f"fpr[{i}].ps0"] = u(a)
        out[f"fpr[{i}].ps1"] = u(b)
    for key in ("cr", "xer", "lr", "ctr", "msr", "fpscr", "pc", "tb", "reserve_addr", "srr0", "srr1", "dar",
                "dsisr", "hid0", "hid2", "dec"):
        out[key] = u(state[key])
    for i, x in enumerate(state["sprg"]):
        out[f"sprg[{i}]"] = u(x)
    for i, x in enumerate(state["gqr"]):
        out[f"gqr[{i}]"] = u(x)
    out["retired"] = int(state["retired"])
    out["reserve"] = bool(state["reserve"])
    out["line_dec"] = bool(state["line_dec"])
    out["exception"] = state.get("exception")
    return out


def apply_masks(a: dict, b: dict, masks: dict) -> list:
    diffs = []
    for key in sorted(set(a) | set(b)):
        va, vb = a.get(key, 0), b.get(key, 0)
        mask = masks.get(key)
        if mask is not None and isinstance(va, int) and not isinstance(va, bool) and isinstance(vb, int):
            if (va & ~mask) == (vb & ~mask):
                continue
        elif mask is not None:
            continue
        if va != vb:
            diffs.append(key)
    return diffs


def mem_bytes(runs) -> dict:
    out = {}
    for addr, hexbytes in runs:
        base = u(addr)
        for i, byte in enumerate(bytes.fromhex(hexbytes)):
            out[base + i] = byte
    return out


def canonical_log(log, effective=False):
    """[(kind, byte address, byte)] in order. Engines and the spec both name window addresses
    (`effective` is accepted for the callers that still pass it; it changes nothing)."""
    seq = []
    for kind, addr, width, hexbytes in log:
        base = u(addr)
        data = bytes.fromhex(hexbytes)
        assert len(data) == width, (addr, width, hexbytes)
        for i, byte in enumerate(data):
            seq.append((kind, base + i, byte))
    return seq


def fmt(v):
    if isinstance(v, bool) or v is None:
        return v
    if isinstance(v, int):
        return f"0x{v:08X}" if v <= 0xFFFFFFFF else f"0x{v:016X}"
    return v


def excerpt(generated: Path, pc: int, cache: dict) -> str:
    """The emitted C++ record that translates pc: from its `// 0xXXXXXXXX` header to the next."""
    if "records" not in cache:
        records = {}
        for shard in sorted(generated.glob("recomp_[0-9][0-9][0-9].cpp")):
            text = shard.read_text(encoding="utf-8")
            for m in re.finditer(r"^// 0x([0-9A-F]{8})  .*?(?=^// 0x[0-9A-F]{8}  |\Z)", text, re.M | re.S):
                records[int(m.group(1), 16)] = (shard.name, m.group(0))
        cache["records"] = records
    records = cache["records"]
    owners = [a for a in records if a <= pc]
    if not owners:
        return ""
    start = max(owners)
    shard, body = records[start]
    lines = body.splitlines()
    return f"{shard}:\n" + "\n".join(lines[:120]) + ("\n..." if len(lines) > 120 else "")


def trace_step_state(snap: dict, spec: bool) -> dict:
    return spec_view(snap) if spec else engine_spec_view(snap["s"] if "s" in snap else snap)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--vectors", type=Path, required=True)
    p.add_argument("--lanes", type=Path, nargs="+", required=True)
    p.add_argument("--spec-results", type=Path, required=True)
    p.add_argument("--generated", type=Path, required=True)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--known", type=Path, default=None,
                   help="exact-fingerprint ledger of recorded candidate divergences (see README)")
    p.add_argument("--max-detail", type=int, default=400)
    a = p.parse_args()

    vectors = {}
    order = []
    for line in a.vectors.open(encoding="utf-8"):
        v = json.loads(line)
        vectors[v["id"]] = v
        order.append(v["id"])
    lanes = defaultdict(dict)
    lane_summaries = []
    for path in a.lanes:
        for line in path.open(encoding="utf-8"):
            r = json.loads(line)
            if "lane_summary" in r:
                lane_summaries.append(r)
                continue
            lanes[r["id"]][r["lane"]] = r
    for line in a.spec_results.open(encoding="utf-8"):
        r = json.loads(line)
        lanes[r["id"]]["S"] = r

    children = defaultdict(list)
    for vid in order:
        v = vectors[vid]
        if v.get("prefix_k") is not None:
            children[v["parent"]].append(vid)

    findings = []
    counts = Counter()
    engine_only = Counter()
    harness_errors = []
    excerpt_cache = {}
    class_coverage = Counter()
    spec_compared_classes = Counter()
    oracle_totals = Counter()   # RL's differential-oracle reports, summed (evidence, not a verdict by itself)

    for summary in lane_summaries:
        if not summary["locked_cache_clean"]:
            harness_errors.append(f"lane {summary['lane_summary']}: locked cache modified")

    def engine_diff(vid, x):
        ref = lanes[vid]["I"]
        other = lanes[vid][x]
        fi, fx = engine_fields(ref["final"]), engine_fields(other["final"])
        diffs = field_diffs(fi, fx)
        mi, mx = mem_bytes(ref["mem"]), mem_bytes(other["mem"])
        for addr in sorted(set(mi) | set(mx)):
            if mi.get(addr) != mx.get(addr):
                diffs.append(f"mem[0x{addr:08X}]")
        if ref["mem_clean"] != other["mem_clean"]:
            diffs.append("mem_clean")
        return diffs, fi, fx, mi, mx

    def lane_diverges(vid, x):
        return bool(engine_diff(vid, x)[0])

    for vid in order:
        v = vectors[vid]
        rec = lanes.get(vid, {})
        missing = [ln for ln in ENGINE_LANES + ("S",) if ln not in rec]
        if missing:
            harness_errors.append(f"{vid}: missing lanes {missing}")
            continue
        counts["vectors"] += 1
        ver = rec["RL"].get("verify") or {}
        for key in ("verified", "verified_instructions", "skipped_device", "skipped_exit", "skipped_hle",
                    "mismatches"):
            oracle_totals[key] += int(ver.get(key, 0))
        oracle_totals["vectors_with_no_verified_segment"] += int(not ver.get("verified"))
        # ---------------- host facts
        host_problems = []
        for ln in ENGINE_LANES:
            r = rec[ln]
            # An oracle mismatch is not a host fact: RL's native code disagreed with the interpreter, an engine
            # divergence reported below (field "oracle").
            if r["status"] != "ok" and not (ln == "RL" and r["status"] == "oracle-mismatch"):
                note = STATUS_NOTES.get(r["status"])
                host_problems.append(f"{ln}: status {r['status']} ({r['host']['stop_detail']})" +
                                     (f": {note}" if note else ""))
            if not r["mem_clean"]:
                host_problems.append(f"{ln}: MEM1 outside the data window modified")
            if r["final"]["fault"]["latched"]:
                host_problems.append(f"{ln}: data-access fault latched at EA {r['final']['fault']['effective']}")
        for ln in NATIVE_LANES:
            host_problems += native_host_facts(ln, rec[ln])
        for ln in INTERPRETER_LANES:
            if "retired" not in rec[ln]["final"]:
                host_problems.append(f"{ln}: an interpreter lane without a retired count")
            elif rec[ln]["host"]["steps"] != rec[ln]["final"]["retired"]:
                host_problems.append(f"{ln}: steps {rec[ln]['host']['steps']} != retired")
        host_problems += oracle_host_facts(rec["RL"])
        if host_problems:
            findings.append({"id": vid, "class": "host-fact", "detail": host_problems})
            counts["host-fact"] += 1

        # ---------------- engines
        engine_divergent = {}
        for ln in ENGINE_LANES[1:]:
            diffs, fi, fx, mi, mx = engine_diff(vid, ln)
            if diffs:
                engine_divergent[ln] = {
                    k: {"I": fmt(fi.get(k, 0)) if not k.startswith("mem[") else mi.get(int(k[4:-1], 16)),
                        ln: fmt(fx.get(k, 0)) if not k.startswith("mem[") else mx.get(int(k[4:-1], 16))}
                    for k in diffs}
        oracle = oracle_divergence(rec["RL"])
        if oracle is not None:
            engine_divergent.setdefault("RL", {})["oracle"] = oracle
        # access logs
        log_i = canonical_log(rec["I"]["log"])
        for ln in ("RL", "NL"):
            log_x = canonical_log(rec[ln]["log"])
            if log_x != log_i:
                first = next((i for i, (p_, q) in enumerate(zip(log_i, log_x)) if p_ != q), min(len(log_i), len(log_x)))
                engine_divergent.setdefault(ln, {})["access_log"] = {
                    "first_index": first, "I": log_i[first] if first < len(log_i) else None,
                    ln: log_x[first] if first < len(log_x) else None, "I_len": len(log_i), f"{ln}_len": len(log_x)}

        # ---------------- spec
        s = rec["S"]
        spec_diffs = []
        spec_reason = None
        if s["status"] in ("spec-gap", "spec-ambiguous"):
            spec_reason = "spec-gap: " + "; ".join(s["notes"])
            if v["spec"] == "compare":
                harness_errors.append(f"{vid}: generator expected a spec comparison but {spec_reason}")
        elif s["verdict"] in ("undefined-all", "undefined-propagating"):
            spec_reason = s["verdict"] + ": " + "; ".join(s["notes"])[:300]
        elif s["status"] == "step-limit":
            harness_errors.append(f"{vid}: spec step limit")
        if spec_reason:
            engine_only[spec_reason.split(":")[0] + ": " + (v["reason"] or s["notes"][0][:80] if s["notes"] else "")] += 1
            counts["engine-only"] += 1
        else:
            counts["spec-compared"] += 1
            sv = spec_view(s["final"])
            iv = engine_spec_view(rec["I"]["final"])
            masks = s["masks"]
            spec_diffs = apply_masks(sv, iv, masks)
            ms, mi = mem_bytes(s["mem"]), mem_bytes(rec["I"]["mem"])
            for addr in sorted(set(ms) | set(mi)):
                if ms.get(addr) != mi.get(addr):
                    spec_diffs.append(f"mem[0x{addr:08X}]")
            log_s = canonical_log(s["log"], effective=True)
            if log_s != log_i and "pc" not in masks:
                spec_diffs.append("access_log")
            spec_diffs += untouched_diffs(rec["I"]["initial"], rec["I"]["final"])
            for snap in s["trace"]:
                spec_compared_classes[snap.get("variant") or snap["mnemonic"]] += 1

        # ---------------- classify + first divergence
        if not engine_divergent and not spec_diffs:
            counts["pass"] += 1
            continue
        finding = {"id": vid, "family": v["family"], "note": v["note"], "entry": f"0x{v['entry']:08X}",
                   "rows": {k: [f"0x{w:08X}" for w in ws] for k, ws in v["row_words"].items()}}
        if engine_divergent:
            finding["class"] = "engine-divergence"
            finding["lanes"] = engine_divergent
            finding["first_divergence"] = locate_native(vid, v, rec, vectors, lanes, children, lane_diverges,
                                                        engine_divergent)
            counts["engine-divergence"] += 1
        if spec_diffs:
            sv = spec_view(s["final"])
            iv = engine_spec_view(rec["I"]["final"])
            nv = engine_spec_view(rec["N"]["final"])
            ms, mi, mn = mem_bytes(s["mem"]), mem_bytes(rec["I"]["mem"]), mem_bytes(rec["N"]["mem"])
            table = {}
            for k in spec_diffs:
                if k.startswith("mem["):
                    addr = int(k[4:-1], 16)
                    table[k] = {"spec": ms.get(addr), "interpreter": mi.get(addr), "generated": mn.get(addr)}
                elif k == "access_log":
                    ls_ = canonical_log(s["log"], effective=True)
                    first = next((i for i, (p_, q) in enumerate(zip(ls_, log_i)) if p_ != q), min(len(ls_), len(log_i)))
                    table[k] = {"first_index": first, "spec": ls_[first] if first < len(ls_) else None,
                                "interpreter": log_i[first] if first < len(log_i) else None,
                                "spec_len": len(ls_), "interpreter_len": len(log_i)}
                elif k not in sv:
                    ini, fi, fn = (engine_fields(rec["I"]["initial"]), engine_fields(rec["I"]["final"]),
                                   engine_fields(rec["N"]["final"]))
                    table[k] = {"spec": fmt(ini.get(k, 0)), "interpreter": fmt(fi.get(k, 0)),
                                "generated": fmt(fn.get(k, 0)),
                                "note": "not carried by the spec State: a completed spec step leaves it unchanged"}
                else:
                    table[k] = {"spec": fmt(sv.get(k)), "interpreter": fmt(iv.get(k)), "generated": fmt(nv.get(k)),
                                "mask": fmt(s["masks"].get(k)) if k in s["masks"] else None}
            finding.setdefault("class", "common-mode" if not engine_divergent else "engine-divergence")
            finding["spec_fields"] = table
            finding["spec_status"] = s["status"]
            finding["spec_notes"] = s["notes"]
            finding["spec_first_divergence"] = locate_spec(v, rec)
            if not engine_divergent:
                counts["common-mode"] += 1
        pc = finding.get("spec_first_divergence", {}).get("pc") or \
            finding.get("first_divergence", {}).get("pc")
        if pc:
            finding["excerpt"] = excerpt(a.generated, u(pc), excerpt_cache)
        findings.append(finding)

    # ---------------- group into candidate signatures
    groups = defaultdict(list)
    for f in findings:
        if f["class"] == "host-fact":
            key = ("host-fact", tuple(sorted(set(d.split(":")[0] + ":" + d.split(":")[1].split("(")[0]
                                                 for d in f["detail"]))))
        else:
            fd = f.get("spec_first_divergence") if f["class"] == "common-mode" else f.get("first_divergence")
            mnemonic = (fd or {}).get("mnemonic")
            fields = sorted(set(re.sub(r"\[\d+\]|\[0x[0-9A-F]+\]", "[]", k)
                                for k in (f.get("spec_fields") or {}).keys()) |
                            set(re.sub(r"\[\d+\]|\[0x[0-9A-F]+\]", "[]", k)
                                for ln in (f.get("lanes") or {}).values() for k in ln))
            lanes_key = tuple(sorted(f.get("lanes", {}).keys()))
            key = (f["class"], mnemonic, lanes_key, tuple(fields))
        groups[key].append(f)

    candidates = []
    for key, members in sorted(groups.items(), key=lambda kv: (-len(kv[1]), str(kv[0]))):
        fingerprint = hashlib.sha1(json.dumps(sorted(
            json.dumps({k: m.get(k) for k in ("id", "class", "lanes", "spec_fields")}, sort_keys=True, default=str)
            for m in members)).encode()).hexdigest()
        candidates.append({"signature": list(map(lambda x: list(x) if isinstance(x, tuple) else x, key)),
                           "count": len(members), "vectors": [m["id"] for m in members],
                           "fingerprint": fingerprint, "example": members[0]})

    known = json.loads(a.known.read_text(encoding="utf-8")) if a.known and a.known.exists() else []
    known_fps = {k["fingerprint"]: k for k in known}
    unexplained = [c for c in candidates if c["fingerprint"] not in known_fps]
    stale = [k for k in known if k["fingerprint"] not in {c["fingerprint"] for c in candidates}]

    # emitter feature coverage (the generated C++ this run executed)
    features = Counter()
    patterns = {
        "cr_fusion": r"cnd = !?\((static_cast<s32>\(c\.gpr|c\.gpr\[\d+\] [<>=])",
        "unfused_cr_bit": r"ppc::cr_bit\(c, ",
        "ctr_decrement": r"--c\.ctr;",
        # Native time: a cold (mid-record) entry is a slot switch; the kEntrySkip/unretire retirement literal it
        # carried, and the conditional fall-through retire_block batch (conditional_fallthrough_batch), went
        # with the instruction clock.
        "cold_entry_slot_switch": r"const unsigned gekko_slot = \(entry - 0x[0-9A-F]+u\) >> 2;",
        "back_edge_poll": r"if \(c\.needs_dispatch\(\)\) \{ c\.leave_to_dispatcher\(0x",
        "bctr_switch_backward_poll": r"c\.pc = tmp;\n\s+if \(c\.needs_dispatch\(\)\)",
        "bctr_switch_forward_case": r"case 0x[0-9A-F]+u: (if \(c\.try_hle\(0x[0-9A-F]+u\)\) return; )?goto L_",
        "jump_indirect": r"recomp::jump_indirect\(c, tmp\)",
        "call_indirect": r"recomp::call_indirect\(c, tmp, ",
        # R12 (2026-09-28) forms: a linked call checks for published transfers, an LK=0 transfer is published.
        "direct_call_exiting_check": r"if \(c\.exiting\(\) && c\.finish_transfers\(\)\) return;",
        "resume_elsewhere": r"recomp::resume_elsewhere\(",
        "cross_record_transfer": r"^\s+c\.transfer_to\(&fn_[0-9A-F]{8}(_e[0-9A-F]{8})?\);\n\s+return;",
        "mem_read_write": r"c\.mem_(read|write)(8|16|32)\(",
        "lwarx_stwcx": r"ppc::(lwarx|stwcx)\(",
        "dcbz_loop": r"for \(u32 i = 0; i < 32u; i \+= 4u\)",
        # repair L0r0-03: lswi/stswi unroll into 1-byte accesses; lswx/stswx loop over XER[25-31]
        "string_counted_loop": r"tmp = c\.xer & 0x7Fu;\n\s+for \(u32 i = 0u; i < tmp; \+\+i\)",
        "string_unrolled_load_byte": r"tmp \|?= static_cast<u32>\(c\.mem_read8\(ea( \+ 0x[0-9A-F]+u)?\)\)",
        "string_unrolled_store_byte": r"c\.mem_write8\(ea( \+ 0x[0-9A-F]+u)?, static_cast<u8>\(tmp",
    }
    for shard in sorted(a.generated.glob("recomp_[0-9][0-9][0-9].cpp")):
        text = shard.read_text(encoding="utf-8")
        for name, pattern in patterns.items():
            features[name] += len(re.findall(pattern, text, re.M))
    report_text = (a.generated / "recomp_report.txt").read_text(encoding="utf-8") \
        if (a.generated / "recomp_report.txt").exists() else ""
    m = re.search(r"recovered entries\s*:\s*(\d+)", (a.generated / "ppcrecomp.log").read_text(encoding="utf-8")) \
        if (a.generated / "ppcrecomp.log").exists() else None
    gap = re.search(r"entry points: (\d+) functions, (\d+) loop heads, (\d+) recovered routines, (\d+) interior",
                    (a.generated / "ppcrecomp.log").read_text(encoding="utf-8")) \
        if (a.generated / "ppcrecomp.log").exists() else None
    if gap:
        features["functions"] = int(gap.group(1))
        features["loop_head_entries"] = int(gap.group(2))
        features["gap_sweep_recovered_routines"] = int(gap.group(3))
        features["interior_branch_call_entries"] = int(gap.group(4))

    report = {
        "summary": dict(counts),
        "harness_errors": harness_errors,
        "engine_only": dict(engine_only),
        "candidates": [{k: c[k] for k in ("signature", "count", "vectors", "fingerprint")} for c in candidates],
        "unexplained_candidates": [c["fingerprint"] for c in unexplained],
        "stale_known": stale,
        "findings": findings[:a.max_detail],
        "findings_total": len(findings),
        "spec_compared_instruction_classes": dict(spec_compared_classes),
        "emitter_features_in_generated_code": dict(features),
        "rl_oracle": dict(oracle_totals),
        "lane_summaries": lane_summaries,
    }
    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_text(json.dumps(report, indent=1, default=str), encoding="utf-8")

    print(f"difftest compare: {counts['vectors']} vectors, {counts['pass']} pass, "
          f"{counts['spec-compared']} spec-compared, {counts['engine-only']} engine-only, "
          f"{counts['engine-divergence']} engine-divergence, {counts['common-mode']} common-mode, "
          f"{counts['host-fact']} host-fact, {len(harness_errors)} harness errors")
    print(f"  RL differential oracle: {oracle_totals['verified']} segments / {oracle_totals['verified_instructions']} "
          f"instructions verified, {oracle_totals['mismatches']} mismatches, skipped {oracle_totals['skipped_exit']} "
          f"(no recognisable end) {oracle_totals['skipped_device']} (device) {oracle_totals['skipped_hle']} (HLE); "
          f"{oracle_totals['vectors_with_no_verified_segment']} vectors with no verified segment")
    for reason, n in sorted(engine_only.items(), key=lambda kv: -kv[1]):
        print(f"  engine-only {n:4d}  {reason}")
    for c in candidates:
        tag = "KNOWN " + known_fps[c["fingerprint"]]["candidate"] if c["fingerprint"] in known_fps else "NEW"
        ex = c["example"]
        fd = ex.get("spec_first_divergence") if ex["class"] == "common-mode" else ex.get("first_divergence")
        print(f"  [{tag}] {c['count']:4d} x {c['signature']}  e.g. {ex['id']} {ex.get('note', '')}")
        if fd:
            print(f"         first divergence: {json.dumps(fd, default=str)[:600]}")
    for e in harness_errors[:20]:
        print("  HARNESS ERROR " + e)
    for k in stale:
        print(f"  STALE known candidate {k['candidate']} ({k['fingerprint']}) no longer reproduces")
    print(f"report: {a.out}")
    failed = bool(harness_errors or unexplained or stale)
    return 1 if failed else 0


def locate_spec(v, rec):
    """Lockstep the spec trace against the interpreter trace; first differing step."""
    s_trace = rec["S"]["trace"]
    i_trace = rec["I"].get("trace", [])
    masks = rec["S"]["masks"]
    for k, snap in enumerate(s_trace):
        if k >= len(i_trace):
            return {"step": k, "pc": snap["from_pc"], "word": snap["word"], "mnemonic": snap["mnemonic"],
                    "reason": "interpreter trace ended"}
        sv = spec_view(snap)
        iv = engine_spec_view(i_trace[k]["s"])
        diffs = apply_masks(sv, iv, masks)
        ms, mi = mem_bytes(snap["mem"]), mem_bytes(i_trace[k]["mem"])
        diffs += [f"mem[0x{x:08X}]" for x in sorted(set(ms) | set(mi)) if ms.get(x) != mi.get(x)]
        untouched = untouched_diffs(rec["I"]["initial"], i_trace[k]["s"])
        if untouched and not diffs:
            ini, now = engine_fields(rec["I"]["initial"]), engine_fields(i_trace[k]["s"])
            return {"step": k, "pc": snap["from_pc"], "word": snap["word"], "mnemonic": snap["mnemonic"],
                    "fields": {d: {"spec": fmt(ini.get(d, 0)), "interpreter": fmt(now.get(d, 0)),
                                   "note": "state the spec State does not carry; unchanged by a completed spec step"}
                               for d in untouched},
                    "generated_snapshot": NO_NATIVE_SNAPSHOT}
        if diffs:
            return {"step": k, "pc": snap["from_pc"], "word": snap["word"], "mnemonic": snap["mnemonic"],
                    "fields": {d: {"spec": fmt(sv.get(d)) if not d.startswith("mem[") else ms.get(int(d[4:-1], 16)),
                                   "interpreter": fmt(iv.get(d)) if not d.startswith("mem[") else mi.get(int(d[4:-1], 16)),
                                   } for d in diffs},
                    "generated_snapshot": NO_NATIVE_SNAPSHOT}
    if len(i_trace) > len(s_trace):
        return {"step": len(s_trace), "reason": "spec trace ended (status %s)" % rec["S"]["status"],
                "pc": s_trace[-1]["pc"] if s_trace else f"0x{v['entry']:08X}"}
    return {"reason": "per-step traces agree; divergence only in the final comparison",
            "pc": f"0x{v['entry']:08X}"}


ORACLE_SEGMENT = re.compile(r"0x([0-9A-Fa-f]{8}) after (\d+) interpreted instructions")


def oracle_first_segment(rec) -> dict:
    """The RL oracle's first mismatching native segment: its entry and how many instructions the interpreter
    replayed from there before the states were compared ({} when the oracle saw no mismatch)."""
    messages = (rec["RL"].get("verify") or {}).get("mismatch_messages") or []
    if not messages:
        return {}
    out = {"messages": messages[:4]}
    m = ORACLE_SEGMENT.match(messages[0])
    if m:
        out.update({"entry": f"0x{int(m.group(1), 16):08X}", "replayed": int(m.group(2))})
    return out


def locate_native(vid, v, rec, vectors, lanes, children, lane_diverges, divergent_tables):
    # Native time: native code publishes no per-instruction or per-block state (the RL retire_block snapshots,
    # aligned by retired count, went with the instruction clock), so the window is the whole interpreter run.
    i_trace = rec["I"].get("trace", [])
    divergent_lanes = list(divergent_tables)
    result = {"divergent_lanes": divergent_lanes}
    segment = oracle_first_segment(rec)
    if segment:
        result["oracle_segment"] = segment
    window = (0, int(rec["I"]["final"]["retired"]))
    result["window_retired"] = list(window)
    s_trace = rec["S"]["trace"]
    lo, hi = window
    # Every instruction of the window, in execution order (spec trace), so a control-flow defect
    # (a branch taken the wrong way writes no field itself) is visible next to the heuristic pick.
    result["window_instructions"] = [f"{s['from_pc']} {s['word']} {s['variant'] or s['mnemonic']}"
                                     for s in s_trace[lo:hi]][:64]
    if hi - lo == 1 and hi - 1 < len(s_trace):
        snap = s_trace[hi - 1]
        result.update({"pc": snap["from_pc"], "word": snap["word"], "mnemonic": snap["mnemonic"],
                       "method": "a run of one instruction"})
        return result
    kids = sorted(children.get(vid, []), key=lambda c: vectors[c]["prefix_k"])
    for kid in kids:
        if any(lane_diverges(kid, ln) for ln in divergent_lanes if ln in lanes[kid]):
            k = vectors[kid]["prefix_k"]
            # Prefix vectors exist only for straight-line rows entered at their start: instruction k-1
            # of the parent row is the one the smallest diverging prefix adds.
            row = v["row_words"][f"{v['entry']:08X}"]
            prefix_words = next(iter(vectors[kid]["row_words"].values()))
            name = (vectors[kid].get("mnemonic") or "").split(",")[-1] or None
            result.update({"method": f"smallest diverging prefix {kid} (words[0..{k}) + blr)",
                           "pc": f"0x{v['entry'] + 4 * (k - 1):08X}", "word": f"0x{row[k - 1]:08X}",
                           "mnemonic": name})
            assert prefix_words[k - 1] == row[k - 1]
            return result
    if kids:
        result["prefixes"] = "no prefix diverges: context-sensitive (needs the successor instructions)"
    # Inside the window: the first instruction whose interpreter step writes a field that diverges in the final
    # states (the union over the divergent lanes' tables).
    # (pc changes at every step and names no instruction; the other non-state keys are not written by a step)
    divergent = {k for table in divergent_tables.values() for k in table
                 if k not in ("pc", "retired", "access_log", "oracle", "mem_clean")}
    if divergent and s_trace:
        for k in range(lo, min(hi, len(i_trace))):
            after = engine_fields(i_trace[k]["s"])
            if k > 0:
                before = engine_fields(i_trace[k - 1]["s"])
                mb = mem_bytes(i_trace[k - 1]["mem"])
            else:
                initial = v["state"]
                before = {f"gpr[{i}]": g for i, g in enumerate(initial["gpr"])}
                before.update({"cr": initial["cr"], "xer": initial["xer"], "lr": initial["lr"],
                               "ctr": initial["ctr"], "reserve": initial["reserve"],
                               "reserve_addr": initial["reserve_addr"]})
                mb = {}
            ma = mem_bytes(i_trace[k]["mem"])
            written = {f for f in divergent if not f.startswith("mem[") and before.get(f) != after.get(f)}
            written |= {f for f in divergent if f.startswith("mem[") and
                        mb.get(int(f[4:-1], 16)) != ma.get(int(f[4:-1], 16))}
            if written and k < len(s_trace):
                snap = s_trace[k]
                result.update({"pc": snap["from_pc"], "word": snap["word"], "mnemonic": snap["mnemonic"],
                               "method": f"first instruction of the run {window} whose interpreter step "
                                         f"writes a divergent field ({sorted(written)})"})
                return result
    if s_trace and lo < len(s_trace):
        snap = s_trace[lo]
        result.update({"pc": snap["from_pc"], "word": snap["word"], "mnemonic": snap["mnemonic"],
                       "method": f"first instruction of the run {window}"})
    return result


if __name__ == "__main__":
    sys.exit(main())
